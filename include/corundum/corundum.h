/*
 * Corundum HFT by Euclase
 * Zero-copy, lock-free single-producer / single-consumer ring buffer
 * in shared memory, for passing high-frequency telemetry between
 * processes on Linux (POSIX shm + futex) and Windows (named file mapping
 * + event).
 *
 * The hot path (reserve / commit / read / release) is header-only and inline:
 * no system calls, no locks and no heap allocation. Synchronisation is one
 * acquire load and one release store per side, on separate cache lines.
 *
 * Requires GCC or Clang (uses the __atomic builtins; on Windows: MinGW-w64
 * or clang) and a 64-bit target with lock-free 64-bit atomics. Usable from
 * C99 and C++11 and later.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#ifndef CORUNDUM_H
#define CORUNDUM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRD_VERSION_MAJOR 1
#define CRD_VERSION_MINOR 1
#define CRD_VERSION_PATCH 0
#define CRD_VERSION_STRING "1.1.0"

#if !defined(__GNUC__) && !defined(__clang__)
#error "Corundum requires GCC or Clang (__atomic builtins)"
#endif
#if !defined(__SIZEOF_POINTER__) || __SIZEOF_POINTER__ != 8
#error "Corundum requires a 64-bit target"
#endif

/* ------------------------------------------------------------------------ */
/* Status codes                                                              */
/* ------------------------------------------------------------------------ */

/* Non-negative values are normal outcomes, negative values are errors. */
typedef enum crd_status {
    CRD_OK = 0,
    CRD_EMPTY = 1,      /* nothing to read right now */
    CRD_FULL = 2,       /* not enough free space right now */
    CRD_TIMEOUT = 3,    /* crd_wait: the timeout expired */
    CRD_CLOSED = 4,     /* crd_wait: ring is empty and the writer has closed */

    CRD_EINVAL = -1,    /* invalid argument or name */
    CRD_ESYS = -2,      /* a system call failed; errno (Windows: GetLastError()) holds the reason */
    CRD_EEXIST = -3,    /* crd_create: a segment with this name already exists */
    CRD_ENOENT = -4,    /* no segment with this name */
    CRD_EBUSY = -5,     /* another live process holds this endpoint */
    CRD_EVERSION = -6,  /* segment is not a Corundum ring or has another layout version */
    CRD_ENOTREADY = -7, /* segment exists but its creator has not finished initialising it */
    CRD_ECORRUPT = -8,  /* shared state is inconsistent (bug or misbehaving peer) */
    CRD_ETOOBIG = -9    /* message is larger than crd_max_payload() */
} crd_status;

/* Human-readable description of a status code (static string, never NULL). */
const char *crd_strerror(crd_status status);

/* ------------------------------------------------------------------------ */
/* Shared-memory layout (identical in every process that maps the ring)     */
/* ------------------------------------------------------------------------ */

#define CRD_CACHELINE 128u       /* two 64-byte lines: also defeats the adjacent-line prefetcher */
#define CRD_HEADER_SIZE 512u     /* data region starts at this offset */
#define CRD_MAGIC 0x4D55444E55524F43ull /* "CORUNDUM" in little-endian ASCII */
#define CRD_LAYOUT_VERSION 1u
#define CRD_RECORD_ALIGN 8u
#define CRD_RECORD_HEADER 8u     /* uint32 size + uint32 type in front of every payload */
#define CRD_PAD_TYPE 0xFFFFFFFFu /* reserved type: filler up to the end of the buffer */
#define CRD_MIN_CAPACITY 4096ull
#define CRD_MAX_CAPACITY 4294967296ull /* 4 GiB */

/*
 * Do not access these fields directly; they are public only so the hot path
 * can be inlined. Positions are byte counts that only ever grow (64-bit, so
 * they do not wrap in practice); the offset in the buffer is pos & (capacity-1).
 */
typedef struct crd_shm {
    /* cache line 0: written once by crd_create, read-only afterwards */
    uint64_t magic; /* stored last, with release, when initialisation is complete */
    uint32_t version;
    uint32_t header_size;
    uint64_t capacity;
    uint64_t created_unix_ns;
    uint32_t create_flags;
    uint32_t reserved0;
    unsigned char pad0[CRD_CACHELINE - 40];

    /* cache line 1: written only by the producer */
    uint64_t head;          /* end of the last published record */
    uint32_t notify_seq;    /* futex word for crd_wait / crd_notify (Linux) */
    int32_t writer_pid;     /* 0 when no writer is attached */
    uint32_t writer_closed; /* 1 after crd_writer_close, until a writer attaches again */
    uint32_t reserved1;
    unsigned char pad1[CRD_CACHELINE - 24];

    /* cache line 2: written only by the consumer */
    uint64_t tail;          /* everything before tail may be overwritten */
    uint32_t reader_waiting;/* 1 while the reader sleeps in crd_wait */
    int32_t reader_pid;     /* 0 when no reader is attached */
    unsigned char pad2[CRD_CACHELINE - 16];

    /* cache line 3: reserved for future use */
    unsigned char pad3[CRD_CACHELINE];
} crd_shm;

#ifdef __cplusplus
static_assert(sizeof(crd_shm) == CRD_HEADER_SIZE, "crd_shm layout");
#else
_Static_assert(sizeof(crd_shm) == CRD_HEADER_SIZE, "crd_shm layout");
#endif

/* ------------------------------------------------------------------------ */
/* Process-local endpoint handles. Place them anywhere (stack, static,      */
/* inside your own structs); the library never allocates.                   */
/* ------------------------------------------------------------------------ */

typedef struct crd_writer {
    crd_shm *shm;
    unsigned char *data;
    uint64_t capacity;
    uint64_t mask;
    uint64_t head;       /* local head: includes staged, not yet published records */
    uint64_t tail_cache; /* last tail seen; refreshed only when the ring looks full */
    uint64_t res_pos;    /* stream position of the reserved record */
    uint32_t res_size;   /* payload bytes reserved */
    uint32_t has_res;
    size_t map_size;
    unsigned flags;
    void *os_event;      /* Windows: wake-up event handle; unused on Linux */
} crd_writer;

typedef struct crd_reader {
    crd_shm *shm;
    const unsigned char *data;
    uint64_t capacity;
    uint64_t mask;
    uint64_t cursor;     /* position of the next record to read */
    uint64_t head_cache; /* last head seen; refreshed only when the ring looks empty */
    size_t map_size;
    unsigned flags;
    void *os_event;      /* Windows: wake-up event handle; unused on Linux */
} crd_reader;

/* A message returned by crd_read. data points straight into shared memory and
 * stays valid until the next crd_release() on the same reader. data is 8-byte
 * aligned. */
typedef struct crd_msg {
    const void *data;
    uint32_t size;
    uint32_t type;
} crd_msg;

/* ------------------------------------------------------------------------ */
/* Setup (cold path: these make system calls)                               */
/* ------------------------------------------------------------------------ */

/* crd_create flags */
#define CRD_CREATE_REPLACE 0x1u /* unlink an existing segment with this name first */

/* crd_writer_open / crd_reader_open flags */
#define CRD_OPEN_PREFAULT 0x1u  /* map with MAP_POPULATE: no page faults on the hot path */
#define CRD_OPEN_MLOCK 0x2u     /* mlock the mapping: it is never swapped out */
#define CRD_OPEN_TAKEOVER 0x4u  /* attach even if another process seems to hold the endpoint */

/*
 * Creates a shared-memory segment named `name` ("/something", no other '/')
 * holding an empty ring of `capacity` data bytes (a power of two between
 * CRD_MIN_CAPACITY and CRD_MAX_CAPACITY).
 *
 * Linux: a POSIX shm object; `mode` is the permission mask (e.g. 0600). It
 * lives until crd_unlink or reboot.
 * Windows: a pagefile-backed file mapping named "Local\corundum.<name>"
 * (visible to the current logon session; `mode` is ignored, the default
 * security of the creating process applies). Windows deletes a mapping when
 * its last handle closes, so the creating process keeps one handle until it
 * calls crd_unlink or exits; the ring stays alive while any process has it open.
 */
crd_status crd_create(const char *name, uint64_t capacity, unsigned mode, unsigned flags);

/* Removes the name (Windows: releases the creator's handle). Processes that
 * have the ring open keep working. */
crd_status crd_unlink(const char *name);

/* Attaches as the only producer / the only consumer. Fails with CRD_EBUSY if
 * another live process is attached in the same role. */
crd_status crd_writer_open(crd_writer *w, const char *name, unsigned flags);
crd_status crd_reader_open(crd_reader *r, const char *name, unsigned flags);

/* Detaches. crd_writer_close publishes staged records, marks the stream closed
 * and wakes a sleeping reader. Safe to call on a zeroed handle. */
void crd_writer_close(crd_writer *w);
void crd_reader_close(crd_reader *r);

/* Snapshot of a ring's state, for monitoring tools. Maps the segment
 * read-only and does not attach. */
typedef struct crd_info {
    uint64_t capacity;
    uint64_t head;
    uint64_t tail;
    uint64_t used_bytes;
    uint64_t created_unix_ns;
    int32_t writer_pid;
    int32_t reader_pid;
    int writer_closed;
    int reader_waiting;
} crd_info;
crd_status crd_inspect(const char *name, crd_info *info);

/* ------------------------------------------------------------------------ */
/* Blocking helpers (system calls only when there is nothing to do)         */
/* ------------------------------------------------------------------------ */

/*
 * Waits until a record is readable, the writer closes, or timeout_ns passes
 * (negative: wait forever, 0: just check). Sleeps on a futex, so the reader
 * uses no CPU while idle. Returns CRD_OK, CRD_CLOSED or CRD_TIMEOUT.
 * The writer must call crd_notify() for a sleeping reader to wake up.
 */
crd_status crd_wait(crd_reader *r, int64_t timeout_ns);

/* Slow part of crd_notify; do not call directly. */
void crd__wake(crd_shm *shm, void *os_event);

/* ------------------------------------------------------------------------ */
/* Hot path: inline, wait-free, no system calls                             */
/* ------------------------------------------------------------------------ */

static inline uint64_t crd__align(uint64_t n) {
    return (n + (CRD_RECORD_ALIGN - 1u)) & ~(uint64_t)(CRD_RECORD_ALIGN - 1u);
}

/* Spin-wait hint for busy-polling loops. */
static inline void crd_cpu_relax(void) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Largest payload that can ever be written to this ring: capacity/2 - 8. */
static inline uint32_t crd_max_payload(const crd_writer *w) {
    return (uint32_t)(w->capacity / 2u - CRD_RECORD_HEADER);
}

/*
 * Reserves room for a payload of `size` bytes and stores a pointer to it
 * (8-byte aligned, inside shared memory) in *out. Fill it in place, then call
 * crd_commit or crd_stage. A new reservation replaces an uncommitted one.
 * Returns CRD_OK, CRD_FULL (reader is behind; try again later) or CRD_ETOOBIG.
 */
static inline crd_status crd_reserve(crd_writer *w, uint32_t size, void **out) {
    if (size > crd_max_payload(w)) return CRD_ETOOBIG;
    const uint64_t total = CRD_RECORD_HEADER + crd__align(size);
    const uint64_t pos = w->head & w->mask;
    uint64_t start = w->head;
    if (pos + total > w->capacity) start += w->capacity - pos; /* skip to the beginning */
    const uint64_t end = start + total;
    if (end - w->tail_cache > w->capacity) {
        w->tail_cache = __atomic_load_n(&w->shm->tail, __ATOMIC_ACQUIRE);
        if (end - w->tail_cache > w->capacity) return CRD_FULL;
    }
    if (start != w->head) { /* filler record up to the end of the buffer */
        uint32_t *pad = (uint32_t *)(w->data + pos);
        pad[0] = (uint32_t)(w->capacity - pos - CRD_RECORD_HEADER);
        pad[1] = CRD_PAD_TYPE;
    }
    w->res_pos = start;
    w->res_size = size;
    w->has_res = 1;
    *out = w->data + (start & w->mask) + CRD_RECORD_HEADER;
    return CRD_OK;
}

/* Finishes the reservation with the actual payload size (<= reserved size)
 * and a user-defined type, but does not make it visible yet. */
static inline crd_status crd_stage(crd_writer *w, uint32_t size, uint32_t type) {
    if (!w->has_res || size > w->res_size || type == CRD_PAD_TYPE) return CRD_EINVAL;
    uint32_t *hdr = (uint32_t *)(w->data + (w->res_pos & w->mask));
    hdr[0] = size;
    hdr[1] = type;
    w->head = w->res_pos + CRD_RECORD_HEADER + crd__align(size);
    w->has_res = 0;
    return CRD_OK;
}

/* Makes every staged record visible to the reader (one release store). */
static inline void crd_publish(crd_writer *w) {
    __atomic_store_n(&w->shm->head, w->head, __ATOMIC_RELEASE);
}

/* crd_stage + crd_publish. */
static inline crd_status crd_commit(crd_writer *w, uint32_t size, uint32_t type) {
    crd_status s = crd_stage(w, size, type);
    if (s == CRD_OK) crd_publish(w);
    return s;
}

/* Copying convenience: reserve, memcpy, commit. */
static inline crd_status crd_write(crd_writer *w, uint32_t type, const void *src, uint32_t size) {
    void *dst;
    crd_status s = crd_reserve(w, size, &dst);
    if (s != CRD_OK) return s;
    if (size) memcpy(dst, src, size);
    return crd_commit(w, size, type);
}

/*
 * Wakes the reader if it sleeps in crd_wait. Costs one full memory fence when
 * nobody sleeps, and a futex syscall only when someone does. Call it after
 * publishing; busy-polling readers do not need it.
 */
static inline void crd_notify(crd_writer *w) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&w->shm->reader_waiting, __ATOMIC_RELAXED)) crd__wake(w->shm, w->os_event);
}

/* Bytes currently free for the writer (may be stale by the time you use it). */
static inline uint64_t crd_writer_free(crd_writer *w) {
    w->tail_cache = __atomic_load_n(&w->shm->tail, __ATOMIC_ACQUIRE);
    return w->capacity - (w->head - w->tail_cache);
}

/*
 * Reads the next record without copying. The message stays valid, and its
 * space stays reserved, until crd_release. Several records can be read before
 * one release. Returns CRD_OK, CRD_EMPTY or CRD_ECORRUPT.
 *
 * Record headers are validated, so a misbehaving writer cannot make the reader
 * access memory outside the ring. The payload itself is shared: if the writer
 * is not trusted, copy the payload out before validating its contents.
 */
static inline crd_status crd_read(crd_reader *r, crd_msg *m) {
    for (;;) {
        if (r->cursor == r->head_cache) {
            r->head_cache = __atomic_load_n(&r->shm->head, __ATOMIC_ACQUIRE);
            if (r->cursor == r->head_cache) return CRD_EMPTY;
        }
        const uint64_t avail = r->head_cache - r->cursor;
        if (avail > r->capacity || (avail & (CRD_RECORD_ALIGN - 1u))) return CRD_ECORRUPT;
        const uint64_t pos = r->cursor & r->mask;
        const uint32_t *hdr = (const uint32_t *)(r->data + pos);
        /* atomic loads: each header word is read exactly once, then validated */
        const uint32_t size = __atomic_load_n(&hdr[0], __ATOMIC_RELAXED);
        const uint32_t type = __atomic_load_n(&hdr[1], __ATOMIC_RELAXED);
        if (type == CRD_PAD_TYPE) {
            const uint64_t skip = r->capacity - pos;
            if ((uint64_t)size + CRD_RECORD_HEADER != skip || skip >= avail) return CRD_ECORRUPT;
            r->cursor += skip;
            continue;
        }
        const uint64_t total = CRD_RECORD_HEADER + crd__align(size);
        if (total > avail || pos + total > r->capacity) return CRD_ECORRUPT;
        m->data = hdr + 2;
        m->size = size;
        m->type = type;
        r->cursor += total;
        return CRD_OK;
    }
}

/* Gives the space of every record read so far back to the writer
 * (one release store). Pointers from earlier crd_read calls become invalid. */
static inline void crd_release(crd_reader *r) {
    __atomic_store_n(&r->shm->tail, r->cursor, __ATOMIC_RELEASE);
}

/* Bytes published but not read yet by this reader. */
static inline uint64_t crd_reader_pending(crd_reader *r) {
    r->head_cache = __atomic_load_n(&r->shm->head, __ATOMIC_ACQUIRE);
    return r->head_cache - r->cursor;
}

/* Non-zero once the writer has called crd_writer_close (and no writer has
 * attached since). Records published before closing can still be read. */
static inline int crd_writer_closed(const crd_reader *r) {
    return (int)__atomic_load_n(&r->shm->writer_closed, __ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORUNDUM_H */
