/*
 * Corundum HFT by Euclase: setup, teardown and blocking helpers.
 * Everything in this file is cold path; the hot path is inline in corundum.h.
 *
 * Platform layer:
 *   Linux   - POSIX shm_open + mmap, futex for crd_wait / crd_notify.
 *   Windows - named pagefile-backed file mapping ("Local\corundum.<name>"),
 *             named auto-reset event ("Local\corundum.<name>.wake").
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "corundum/corundum.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if !defined(__GCC_ATOMIC_LLONG_LOCK_FREE) || __GCC_ATOMIC_LLONG_LOCK_FREE != 2
#error "Corundum needs lock-free 64-bit atomics (they must work across processes)"
#endif
_Static_assert(offsetof(crd_shm, head) == 1 * CRD_CACHELINE, "head on its own cache line");
_Static_assert(offsetof(crd_shm, tail) == 2 * CRD_CACHELINE, "tail on its own cache line");

const char *crd_strerror(crd_status s) {
    switch (s) {
    case CRD_OK: return "ok";
    case CRD_EMPTY: return "ring is empty";
    case CRD_FULL: return "ring is full";
    case CRD_TIMEOUT: return "timed out";
    case CRD_CLOSED: return "writer closed the ring";
    case CRD_EINVAL: return "invalid argument";
    case CRD_ESYS: return "system call failed (see errno / GetLastError)";
    case CRD_EEXIST: return "segment already exists";
    case CRD_ENOENT: return "segment does not exist";
    case CRD_EBUSY: return "endpoint is held by another process";
    case CRD_EVERSION: return "not a Corundum ring, or incompatible layout version";
    case CRD_ENOTREADY: return "segment is not initialised yet";
    case CRD_ECORRUPT: return "shared state is corrupt";
    case CRD_ETOOBIG: return "message larger than the maximum payload";
    }
    return "unknown status";
}

/* Ring names: leading '/', no other '/', 2..200 characters. */
#define CRD_NAME_MAX 200
static int valid_name(const char *name) {
    if (!name || name[0] != '/') return 0;
    size_t n = strlen(name);
    if (n < 2 || n > CRD_NAME_MAX) return 0;
    if (strchr(name + 1, '/') != NULL) return 0;
#if defined(_WIN32)
    if (strchr(name + 1, '\\') != NULL) return 0;
#endif
    return 1;
}

static int valid_capacity(uint64_t c) {
    return c >= CRD_MIN_CAPACITY && c <= CRD_MAX_CAPACITY && (c & (c - 1)) == 0;
}

static uint64_t now_unix_ns(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft); /* 100 ns ticks since 1601 */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* Fills a freshly created, zeroed segment and publishes it. */
static void init_header(crd_shm *shm, uint64_t capacity, unsigned flags) {
    shm->version = CRD_LAYOUT_VERSION;
    shm->header_size = CRD_HEADER_SIZE;
    shm->capacity = capacity;
    shm->created_unix_ns = now_unix_ns();
    shm->create_flags = flags;
    /* openers check magic with acquire: they see a fully initialised header */
    __atomic_store_n(&shm->magic, CRD_MAGIC, __ATOMIC_RELEASE);
}

/* Checks a mapped segment of `size` bytes. */
static crd_status validate(const crd_shm *shm, uint64_t size) {
    uint64_t magic = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);
    if (magic == 0) return CRD_ENOTREADY;
    if (magic != CRD_MAGIC || shm->version != CRD_LAYOUT_VERSION || shm->header_size != CRD_HEADER_SIZE) return CRD_EVERSION;
    if (!valid_capacity(shm->capacity) || CRD_HEADER_SIZE + shm->capacity > size) return CRD_ECORRUPT;
    return CRD_OK;
}

/* ======================================================================== */
/* Platform layer                                                            */
/* ======================================================================== */

typedef struct {
    crd_shm *shm;
    size_t size;
    void *event; /* Windows only */
} mapping;

static int32_t self_pid(void);
static int process_alive(int32_t pid);
static crd_status os_map(const char *name, unsigned flags, int writable, mapping *out);
static void os_unmap(mapping *m);
static void os_wake(crd_shm *shm, void *event);
/* Sleeps until woken or until timeout_ns passes (<0: forever). The caller re-checks. */
static crd_status os_sleep(crd_reader *r, uint32_t seq, int64_t timeout_ns);

#if defined(_WIN32)
/* ------------------------------------------------------------------ Windows */

/* "/telemetry" -> L"Local\\corundum.telemetry<suffix>" */
static int win_name(const char *name, const char *suffix, wchar_t *out, size_t cap) {
    char buf[CRD_NAME_MAX + 64];
    int n = snprintf(buf, sizeof buf, "Local\\corundum.%s%s", name + 1, suffix);
    if (n <= 0 || (size_t)n >= sizeof buf) return 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, -1, out, (int)cap) > 0;
}

static crd_status win_status(DWORD err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND: return CRD_ENOENT;
    case ERROR_ALREADY_EXISTS: return CRD_EEXIST;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER: return CRD_EINVAL;
    default: SetLastError(err); return CRD_ESYS;
    }
}

/* Windows deletes a mapping when its last handle closes. The creator keeps one
 * handle per ring here until crd_unlink or process exit. Fixed table: no heap. */
#define CRD_MAX_OWNED 64
static struct {
    char name[CRD_NAME_MAX + 1];
    HANDLE map;
    HANDLE event;
} g_owned[CRD_MAX_OWNED];
static volatile LONG g_owned_lock = 0;
static void owned_lock(void) { while (InterlockedExchange(&g_owned_lock, 1)) Sleep(0); }
static void owned_unlock(void) { InterlockedExchange(&g_owned_lock, 0); }

static int32_t self_pid(void) { return (int32_t)GetCurrentProcessId(); }

static int process_alive(int32_t pid) {
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED; /* exists, but not ours */
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

crd_status crd_create(const char *name, uint64_t capacity, unsigned mode, unsigned flags) {
    (void)mode;
    if (!valid_name(name) || !valid_capacity(capacity) || (flags & ~CRD_CREATE_REPLACE)) return CRD_EINVAL;
    wchar_t wmap[CRD_NAME_MAX + 64], wev[CRD_NAME_MAX + 64];
    if (!win_name(name, "", wmap, CRD_NAME_MAX + 64) || !win_name(name, ".wake", wev, CRD_NAME_MAX + 64)) return CRD_EINVAL;
    if (flags & CRD_CREATE_REPLACE) (void)crd_unlink(name);

    const uint64_t size = CRD_HEADER_SIZE + capacity;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, wmap);
    if (!map) return win_status(GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(map);
        return CRD_EEXIST;
    }
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, wev);
    if (!ev) {
        DWORD e = GetLastError();
        CloseHandle(map);
        return win_status(e);
    }
    crd_shm *shm = (crd_shm *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
    if (!shm) {
        DWORD e = GetLastError();
        CloseHandle(ev);
        CloseHandle(map);
        return win_status(e);
    }
    init_header(shm, capacity, flags); /* pagefile-backed sections start zero-filled */
    UnmapViewOfFile(shm);

    owned_lock();
    int slot = -1;
    for (int i = 0; i < CRD_MAX_OWNED; ++i)
        if (!g_owned[i].map) { slot = i; break; }
    if (slot >= 0) {
        strcpy(g_owned[slot].name, name);
        g_owned[slot].map = map;
        g_owned[slot].event = ev;
    }
    owned_unlock();
    if (slot < 0) { /* table full: the ring would vanish at once, so refuse */
        CloseHandle(ev);
        CloseHandle(map);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return CRD_ESYS;
    }
    return CRD_OK;
}

crd_status crd_unlink(const char *name) {
    if (!valid_name(name)) return CRD_EINVAL;
    crd_status st = CRD_ENOENT;
    owned_lock();
    for (int i = 0; i < CRD_MAX_OWNED; ++i) {
        if (g_owned[i].map && strcmp(g_owned[i].name, name) == 0) {
            CloseHandle(g_owned[i].map);
            CloseHandle(g_owned[i].event);
            g_owned[i].map = g_owned[i].event = NULL;
            g_owned[i].name[0] = 0;
            st = CRD_OK;
        }
    }
    owned_unlock();
    if (st == CRD_OK) return st;
    /* not created by this process: report whether it exists at all */
    wchar_t wmap[CRD_NAME_MAX + 64];
    if (!win_name(name, "", wmap, CRD_NAME_MAX + 64)) return CRD_EINVAL;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, wmap);
    if (!h) return win_status(GetLastError());
    CloseHandle(h);
    return CRD_OK; /* exists; it disappears when its other users close it */
}

static crd_status os_map(const char *name, unsigned flags, int writable, mapping *out) {
    if (!valid_name(name)) return CRD_EINVAL;
    wchar_t wmap[CRD_NAME_MAX + 64], wev[CRD_NAME_MAX + 64];
    if (!win_name(name, "", wmap, CRD_NAME_MAX + 64) || !win_name(name, ".wake", wev, CRD_NAME_MAX + 64)) return CRD_EINVAL;
    HANDLE map = OpenFileMappingW(writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, FALSE, wmap);
    if (!map) return win_status(GetLastError());
    void *p = MapViewOfFile(map, writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, 0);
    DWORD err = GetLastError();
    CloseHandle(map); /* the view keeps the section alive */
    if (!p) return win_status(err);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.RegionSize < CRD_HEADER_SIZE) {
        UnmapViewOfFile(p);
        return CRD_ENOTREADY;
    }
    crd_shm *shm = (crd_shm *)p;
    crd_status st = validate(shm, (uint64_t)mbi.RegionSize);
    size_t size = st == CRD_OK ? (size_t)(CRD_HEADER_SIZE + shm->capacity) : 0;
    HANDLE ev = NULL;
    if (st == CRD_OK && writable) {
        ev = CreateEventW(NULL, FALSE, FALSE, wev); /* opens the creator's event, or makes it if missing */
        if (!ev) st = win_status(GetLastError());
    }
    if (st == CRD_OK && (flags & CRD_OPEN_PREFAULT)) {
        volatile const unsigned char *b = (const unsigned char *)p;
        unsigned char sink = 0;
        for (size_t off = 0; off < size; off += 4096) sink ^= b[off]; /* touch every page once */
        (void)sink;
    }
    if (st == CRD_OK && (flags & CRD_OPEN_MLOCK) && !VirtualLock(p, size)) st = CRD_ESYS;
    if (st != CRD_OK) {
        err = GetLastError();
        if (ev) CloseHandle(ev);
        UnmapViewOfFile(p);
        SetLastError(err);
        return st;
    }
    out->shm = shm;
    out->size = size;
    out->event = ev;
    return CRD_OK;
}

static void os_unmap(mapping *m) {
    if (m->event) CloseHandle((HANDLE)m->event);
    if (m->shm) UnmapViewOfFile(m->shm);
    m->shm = NULL;
    m->event = NULL;
}

static void os_wake(crd_shm *shm, void *event) {
    __atomic_fetch_add(&shm->notify_seq, 1u, __ATOMIC_RELEASE);
    if (event) SetEvent((HANDLE)event);
}

static crd_status os_sleep(crd_reader *r, uint32_t seq, int64_t timeout_ns) {
    (void)seq; /* the auto-reset event stays signalled until consumed: no lost wake-ups */
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) {
        int64_t m = (timeout_ns + 999999) / 1000000;
        ms = m > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)m;
    }
    DWORD w = WaitForSingleObject((HANDLE)r->os_event, ms);
    return w == WAIT_OBJECT_0 || w == WAIT_TIMEOUT ? CRD_OK : CRD_ESYS;
}

static int64_t mono_ns(void) {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

#else
/* -------------------------------------------------------------------- Linux */

static crd_status errno_status(void) {
    switch (errno) {
    case ENOENT: return CRD_ENOENT;
    case EEXIST: return CRD_EEXIST;
    case EINVAL:
    case ENAMETOOLONG: return CRD_EINVAL;
    default: return CRD_ESYS;
    }
}

static int32_t self_pid(void) { return (int32_t)getpid(); }

static int process_alive(int32_t pid) {
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno != ESRCH; /* EPERM: exists but belongs to someone else */
}

crd_status crd_create(const char *name, uint64_t capacity, unsigned mode, unsigned flags) {
    if (!valid_name(name) || !valid_capacity(capacity) || (flags & ~CRD_CREATE_REPLACE)) return CRD_EINVAL;
    if (flags & CRD_CREATE_REPLACE) (void)shm_unlink(name);

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)mode);
    if (fd < 0) return errno_status();
    const size_t size = CRD_HEADER_SIZE + (size_t)capacity;
    crd_status st = CRD_OK;
    void *p = MAP_FAILED;
    if (ftruncate(fd, (off_t)size) != 0) {
        st = CRD_ESYS;
    } else {
        p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) st = CRD_ESYS;
    }
    if (st != CRD_OK) {
        int saved = errno;
        close(fd);
        shm_unlink(name);
        errno = saved;
        return st;
    }
    close(fd);
    init_header((crd_shm *)p, capacity, flags); /* ftruncate zero-filled everything */
    munmap(p, size);
    return CRD_OK;
}

crd_status crd_unlink(const char *name) {
    if (!valid_name(name)) return CRD_EINVAL;
    return shm_unlink(name) == 0 ? CRD_OK : errno_status();
}

static crd_status os_map(const char *name, unsigned flags, int writable, mapping *out) {
    if (!valid_name(name)) return CRD_EINVAL;
    int fd = shm_open(name, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC, 0);
    if (fd < 0) return errno_status();
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return CRD_ESYS;
    }
    if ((uint64_t)sb.st_size < CRD_HEADER_SIZE) {
        close(fd);
        return CRD_ENOTREADY; /* creator has not called ftruncate yet */
    }
    const size_t size = (size_t)sb.st_size;
    int mflags = MAP_SHARED;
    if (flags & CRD_OPEN_PREFAULT) mflags |= MAP_POPULATE;
    void *p = mmap(NULL, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, mflags, fd, 0);
    int saved = errno;
    close(fd);
    if (p == MAP_FAILED) {
        errno = saved;
        return CRD_ESYS;
    }
    crd_shm *shm = (crd_shm *)p;
    crd_status st = validate(shm, (uint64_t)size);
    if (st == CRD_OK && CRD_HEADER_SIZE + shm->capacity != (uint64_t)size) st = CRD_ECORRUPT;
    if (st == CRD_OK && (flags & CRD_OPEN_MLOCK) && mlock(p, size) != 0) st = CRD_ESYS;
    if (st != CRD_OK) {
        saved = errno;
        munmap(p, size);
        errno = saved;
        return st;
    }
    out->shm = shm;
    out->size = size;
    out->event = NULL;
    return CRD_OK;
}

static void os_unmap(mapping *m) {
    if (m->shm) munmap(m->shm, m->size);
    m->shm = NULL;
}

/* Shared (not FUTEX_PRIVATE_FLAG) futex: the word lives in memory mapped by
 * several processes, the kernel keys it by the underlying page. */
static long futex(uint32_t *addr, int op, uint32_t val, const struct timespec *ts) {
    return syscall(SYS_futex, addr, op, val, ts, NULL, 0);
}

static void os_wake(crd_shm *shm, void *event) {
    (void)event;
    __atomic_fetch_add(&shm->notify_seq, 1u, __ATOMIC_RELEASE);
    futex(&shm->notify_seq, FUTEX_WAKE, INT_MAX, NULL);
}

static crd_status os_sleep(crd_reader *r, uint32_t seq, int64_t timeout_ns) {
    struct timespec ts, *pts = NULL;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000ll);
        ts.tv_nsec = (long)(timeout_ns % 1000000000ll);
        pts = &ts;
    }
    if (futex(&r->shm->notify_seq, FUTEX_WAIT, seq, pts) != 0 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT)
        return CRD_ESYS;
    return CRD_OK;
}

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
#endif

/* ======================================================================== */
/* Portable part                                                             */
/* ======================================================================== */

/* Claims a role slot holding a pid. Returns CRD_OK or CRD_EBUSY. */
static crd_status claim(int32_t *slot, unsigned flags) {
    const int32_t self = self_pid();
    for (;;) {
        int32_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (cur == self) return CRD_EBUSY; /* already attached in this process */
        if (cur != 0 && process_alive(cur) && !(flags & CRD_OPEN_TAKEOVER)) return CRD_EBUSY;
        if (__atomic_compare_exchange_n(slot, &cur, self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return CRD_OK;
    }
}

static void unclaim(int32_t *slot) {
    int32_t self = self_pid();
    __atomic_compare_exchange_n(slot, &self, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

#define CRD_OPEN_ALL (CRD_OPEN_PREFAULT | CRD_OPEN_MLOCK | CRD_OPEN_TAKEOVER)

crd_status crd_writer_open(crd_writer *w, const char *name, unsigned flags) {
    if (!w) return CRD_EINVAL;
    memset(w, 0, sizeof *w);
    if (flags & ~CRD_OPEN_ALL) return CRD_EINVAL;
    mapping m = {0};
    crd_status st = os_map(name, flags, 1, &m);
    if (st != CRD_OK) return st;
    if ((st = claim(&m.shm->writer_pid, flags)) != CRD_OK) {
        os_unmap(&m);
        return st;
    }
    crd_shm *shm = m.shm;
    w->shm = shm;
    w->data = (unsigned char *)shm + CRD_HEADER_SIZE;
    w->capacity = shm->capacity;
    w->mask = shm->capacity - 1;
    w->head = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
    w->tail_cache = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
    w->map_size = m.size;
    w->flags = flags;
    w->os_event = m.event;
    if (w->head - w->tail_cache > w->capacity || (w->head & (CRD_RECORD_ALIGN - 1u))) {
        unclaim(&shm->writer_pid);
        os_unmap(&m);
        memset(w, 0, sizeof *w);
        return CRD_ECORRUPT;
    }
    __atomic_store_n(&shm->writer_closed, 0u, __ATOMIC_RELEASE);
    return CRD_OK;
}

crd_status crd_reader_open(crd_reader *r, const char *name, unsigned flags) {
    if (!r) return CRD_EINVAL;
    memset(r, 0, sizeof *r);
    if (flags & ~CRD_OPEN_ALL) return CRD_EINVAL;
    mapping m = {0};
    crd_status st = os_map(name, flags, 1, &m);
    if (st != CRD_OK) return st;
    if ((st = claim(&m.shm->reader_pid, flags)) != CRD_OK) {
        os_unmap(&m);
        return st;
    }
    r->shm = m.shm;
    r->data = (const unsigned char *)m.shm + CRD_HEADER_SIZE;
    r->capacity = m.shm->capacity;
    r->mask = m.shm->capacity - 1;
    r->cursor = __atomic_load_n(&m.shm->tail, __ATOMIC_ACQUIRE);
    r->head_cache = r->cursor;
    r->map_size = m.size;
    r->flags = flags;
    r->os_event = m.event;
    return CRD_OK;
}

void crd_writer_close(crd_writer *w) {
    if (!w || !w->shm) return;
    crd_publish(w);
    __atomic_store_n(&w->shm->writer_closed, 1u, __ATOMIC_RELEASE);
    crd__wake(w->shm, w->os_event); /* unconditionally: the reader must learn about the close */
    unclaim(&w->shm->writer_pid);
    mapping m = {w->shm, w->map_size, w->os_event};
    os_unmap(&m);
    memset(w, 0, sizeof *w);
}

void crd_reader_close(crd_reader *r) {
    if (!r || !r->shm) return;
    __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELEASE);
    unclaim(&r->shm->reader_pid);
    mapping m = {r->shm, r->map_size, r->os_event};
    os_unmap(&m);
    memset(r, 0, sizeof *r);
}

crd_status crd_inspect(const char *name, crd_info *info) {
    if (!info) return CRD_EINVAL;
    mapping m = {0};
    crd_status st = os_map(name, 0, 0, &m);
    if (st != CRD_OK) return st;
    crd_shm *shm = m.shm;
    memset(info, 0, sizeof *info);
    info->capacity = shm->capacity;
    info->tail = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
    info->head = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
    info->used_bytes = info->head - info->tail;
    info->created_unix_ns = shm->created_unix_ns;
    info->writer_pid = __atomic_load_n(&shm->writer_pid, __ATOMIC_ACQUIRE);
    info->reader_pid = __atomic_load_n(&shm->reader_pid, __ATOMIC_ACQUIRE);
    info->writer_closed = (int)__atomic_load_n(&shm->writer_closed, __ATOMIC_ACQUIRE);
    info->reader_waiting = (int)__atomic_load_n(&shm->reader_waiting, __ATOMIC_ACQUIRE);
    os_unmap(&m);
    return CRD_OK;
}

/* ------------------------------------------------------------------------ */
/* wait / notify                                                             */
/* ------------------------------------------------------------------------ */

void crd__wake(crd_shm *shm, void *os_event) { os_wake(shm, os_event); }

static int readable(crd_reader *r) {
    r->head_cache = __atomic_load_n(&r->shm->head, __ATOMIC_ACQUIRE);
    return r->head_cache != r->cursor;
}

crd_status crd_wait(crd_reader *r, int64_t timeout_ns) {
    if (!r || !r->shm) return CRD_EINVAL;
    if (readable(r)) return CRD_OK;
    if (crd_writer_closed(r)) return readable(r) ? CRD_OK : CRD_CLOSED;
    if (timeout_ns == 0) return CRD_TIMEOUT;
    const int64_t deadline = timeout_ns > 0 ? mono_ns() + timeout_ns : 0;

    for (;;) {
        /*
         * Dekker-style handshake with crd_notify:
         *   reader: waiting = 1; fence; load head
         *   writer: store head;  fence; load waiting
         * With both fences sequentially consistent, at least one side sees the
         * other's store, so a wake-up cannot be lost. Linux: reading the futex
         * word before re-checking makes FUTEX_WAIT return at once if the writer
         * bumped it in between. Windows: the auto-reset event stays signalled
         * until a wait consumes it, which has the same effect.
         */
        const uint32_t seq = __atomic_load_n(&r->shm->notify_seq, __ATOMIC_ACQUIRE);
        __atomic_store_n(&r->shm->reader_waiting, 1u, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (readable(r)) break;
        if (crd_writer_closed(r)) {
            __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
            return readable(r) ? CRD_OK : CRD_CLOSED;
        }
        int64_t left = -1;
        if (deadline) {
            left = deadline - mono_ns();
            if (left <= 0) {
                __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
                return CRD_TIMEOUT;
            }
        }
        if (os_sleep(r, seq, left) != CRD_OK) {
            __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
            return CRD_ESYS;
        }
        if (readable(r)) break;
    }
    __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
    return CRD_OK;
}
