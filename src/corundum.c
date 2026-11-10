/*
 * Corundum HFT by Euclase: setup, teardown and blocking helpers.
 * Everything in this file is cold path; the hot path is inline in corundum.h.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#define _GNU_SOURCE
#include "corundum/corundum.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

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
    case CRD_ESYS: return "system call failed (see errno)";
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

/* POSIX shm names: leading '/', no other '/', 2..255 characters. */
static int valid_name(const char *name) {
    if (!name || name[0] != '/') return 0;
    size_t n = strlen(name);
    if (n < 2 || n > NAME_MAX) return 0;
    return strchr(name + 1, '/') == NULL;
}

static int valid_capacity(uint64_t c) {
    return c >= CRD_MIN_CAPACITY && c <= CRD_MAX_CAPACITY && (c & (c - 1)) == 0;
}

static crd_status errno_status(void) {
    switch (errno) {
    case ENOENT: return CRD_ENOENT;
    case EEXIST: return CRD_EEXIST;
    case EINVAL:
    case ENAMETOOLONG: return CRD_EINVAL;
    default: return CRD_ESYS;
    }
}

static uint64_t now_unix_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

    crd_shm *shm = (crd_shm *)p; /* ftruncate zero-filled everything */
    shm->version = CRD_LAYOUT_VERSION;
    shm->header_size = CRD_HEADER_SIZE;
    shm->capacity = capacity;
    shm->created_unix_ns = now_unix_ns();
    shm->create_flags = flags;
    /* openers check magic with acquire: they see a fully initialised header */
    __atomic_store_n(&shm->magic, CRD_MAGIC, __ATOMIC_RELEASE);
    munmap(p, size);
    return CRD_OK;
}

crd_status crd_unlink(const char *name) {
    if (!valid_name(name)) return CRD_EINVAL;
    return shm_unlink(name) == 0 ? CRD_OK : errno_status();
}

/* Opens and validates a segment. On success *out is the mapping, *out_size its length. */
static crd_status map_ring(const char *name, unsigned flags, int writable, crd_shm **out, size_t *out_size) {
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
    crd_status st = CRD_OK;
    uint64_t magic = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);
    if (magic == 0) st = CRD_ENOTREADY;
    else if (magic != CRD_MAGIC || shm->version != CRD_LAYOUT_VERSION || shm->header_size != CRD_HEADER_SIZE) st = CRD_EVERSION;
    else if (!valid_capacity(shm->capacity) || CRD_HEADER_SIZE + shm->capacity != (uint64_t)size) st = CRD_ECORRUPT;
    if (st == CRD_OK && (flags & CRD_OPEN_MLOCK) && mlock(p, size) != 0) st = CRD_ESYS;
    if (st != CRD_OK) {
        saved = errno;
        munmap(p, size);
        errno = saved;
        return st;
    }
    *out = shm;
    *out_size = size;
    return CRD_OK;
}

static int process_alive(int32_t pid) {
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno != ESRCH; /* EPERM: exists but belongs to someone else */
}

/* Claims a role slot holding a pid. Returns CRD_OK or CRD_EBUSY. */
static crd_status claim(int32_t *slot, unsigned flags) {
    const int32_t self = (int32_t)getpid();
    for (;;) {
        int32_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (cur != 0 && cur != self && process_alive(cur) && !(flags & CRD_OPEN_TAKEOVER)) return CRD_EBUSY;
        if (cur == self) return CRD_EBUSY; /* already attached in this process */
        if (__atomic_compare_exchange_n(slot, &cur, self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return CRD_OK;
    }
}

static void unclaim(int32_t *slot) {
    int32_t self = (int32_t)getpid();
    __atomic_compare_exchange_n(slot, &self, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

crd_status crd_writer_open(crd_writer *w, const char *name, unsigned flags) {
    if (!w) return CRD_EINVAL;
    memset(w, 0, sizeof *w);
    if (flags & ~(CRD_OPEN_PREFAULT | CRD_OPEN_MLOCK | CRD_OPEN_TAKEOVER)) return CRD_EINVAL;
    crd_shm *shm;
    size_t size;
    crd_status st = map_ring(name, flags, 1, &shm, &size);
    if (st != CRD_OK) return st;
    if ((st = claim(&shm->writer_pid, flags)) != CRD_OK) {
        munmap(shm, size);
        return st;
    }
    w->shm = shm;
    w->data = (unsigned char *)shm + CRD_HEADER_SIZE;
    w->capacity = shm->capacity;
    w->mask = shm->capacity - 1;
    w->head = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
    w->tail_cache = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
    w->map_size = size;
    w->flags = flags;
    if (w->head - w->tail_cache > w->capacity || (w->head & (CRD_RECORD_ALIGN - 1u))) {
        unclaim(&shm->writer_pid);
        munmap(shm, size);
        memset(w, 0, sizeof *w);
        return CRD_ECORRUPT;
    }
    __atomic_store_n(&shm->writer_closed, 0u, __ATOMIC_RELEASE);
    return CRD_OK;
}

crd_status crd_reader_open(crd_reader *r, const char *name, unsigned flags) {
    if (!r) return CRD_EINVAL;
    memset(r, 0, sizeof *r);
    if (flags & ~(CRD_OPEN_PREFAULT | CRD_OPEN_MLOCK | CRD_OPEN_TAKEOVER)) return CRD_EINVAL;
    crd_shm *shm;
    size_t size;
    crd_status st = map_ring(name, flags, 1, &shm, &size);
    if (st != CRD_OK) return st;
    if ((st = claim(&shm->reader_pid, flags)) != CRD_OK) {
        munmap(shm, size);
        return st;
    }
    r->shm = shm;
    r->data = (const unsigned char *)shm + CRD_HEADER_SIZE;
    r->capacity = shm->capacity;
    r->mask = shm->capacity - 1;
    r->cursor = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
    r->head_cache = r->cursor;
    r->map_size = size;
    r->flags = flags;
    return CRD_OK;
}

void crd_writer_close(crd_writer *w) {
    if (!w || !w->shm) return;
    crd_publish(w);
    __atomic_store_n(&w->shm->writer_closed, 1u, __ATOMIC_RELEASE);
    crd__wake(w->shm); /* unconditionally: the reader must learn about the close */
    unclaim(&w->shm->writer_pid);
    munmap(w->shm, w->map_size);
    memset(w, 0, sizeof *w);
}

void crd_reader_close(crd_reader *r) {
    if (!r || !r->shm) return;
    __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELEASE);
    unclaim(&r->shm->reader_pid);
    munmap(r->shm, r->map_size);
    memset(r, 0, sizeof *r);
}

crd_status crd_inspect(const char *name, crd_info *info) {
    if (!info) return CRD_EINVAL;
    crd_shm *shm;
    size_t size;
    crd_status st = map_ring(name, 0, 0, &shm, &size);
    if (st != CRD_OK) return st;
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
    munmap(shm, size);
    return CRD_OK;
}

/* ------------------------------------------------------------------------ */
/* futex-based wait / notify                                                 */
/* ------------------------------------------------------------------------ */

/* Shared (not FUTEX_PRIVATE_FLAG) futex: the word lives in memory mapped by
 * several processes, the kernel keys it by the underlying page. */
static long futex(uint32_t *addr, int op, uint32_t val, const struct timespec *ts) {
    return syscall(SYS_futex, addr, op, val, ts, NULL, 0);
}

void crd__wake(crd_shm *shm) {
    __atomic_fetch_add(&shm->notify_seq, 1u, __ATOMIC_RELEASE);
    futex(&shm->notify_seq, FUTEX_WAKE, INT_MAX, NULL);
}

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

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
         * other's store, so a wake-up cannot be lost. Reading the futex word
         * before re-checking makes FUTEX_WAIT return at once if the writer
         * bumped it in between.
         */
        const uint32_t seq = __atomic_load_n(&r->shm->notify_seq, __ATOMIC_ACQUIRE);
        __atomic_store_n(&r->shm->reader_waiting, 1u, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (readable(r)) break;
        if (crd_writer_closed(r)) {
            __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
            return readable(r) ? CRD_OK : CRD_CLOSED;
        }
        struct timespec ts, *pts = NULL;
        if (deadline) {
            int64_t left = deadline - mono_ns();
            if (left <= 0) {
                __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
                return CRD_TIMEOUT;
            }
            ts.tv_sec = (time_t)(left / 1000000000ll);
            ts.tv_nsec = (long)(left % 1000000000ll);
            pts = &ts;
        }
        if (futex(&r->shm->notify_seq, FUTEX_WAIT, seq, pts) != 0 && errno != EAGAIN && errno != EINTR &&
            errno != ETIMEDOUT) {
            __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
            return CRD_ESYS;
        }
        if (readable(r)) break;
    }
    __atomic_store_n(&r->shm->reader_waiting, 0u, __ATOMIC_RELAXED);
    return CRD_OK;
}
