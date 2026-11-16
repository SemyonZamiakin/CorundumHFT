/* Zero-copy integrity: cache-line separation of the shared header, which bytes
 * each side's hot path writes, buffer overflow at every boundary, and 64-bit
 * position arithmetic across the 2^64 wrap. */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "corundum/corundum.h"
#include "check.h"

#include <stddef.h>
#include <string.h>

static char NAME[64];

/* ---- static layout ------------------------------------------------------ */

/* Fields grouped by who writes them. 64 is the cache line on x86-64 and most
 * arm64 cores; CRD_CACHELINE (128) also keeps the two sides off the pair of
 * lines Intel's adjacent-line prefetcher fetches together. */
#define BLOCK_OF(f) (offsetof(crd_shm, f) / CRD_CACHELINE)
_Static_assert(CRD_CACHELINE == 128 && CRD_CACHELINE % 64 == 0, "block is a whole number of 64-byte lines");
_Static_assert(CRD_HEADER_SIZE % CRD_CACHELINE == 0, "data region starts on a block boundary");
_Static_assert(BLOCK_OF(magic) == 0 && BLOCK_OF(capacity) == 0 && BLOCK_OF(created_unix_ns) == 0, "metadata: block 0");
_Static_assert(BLOCK_OF(head) == 1 && BLOCK_OF(notify_seq) == 1 && BLOCK_OF(writer_pid) == 1 &&
                   BLOCK_OF(writer_closed) == 1,
               "producer-written fields: block 1");
_Static_assert(BLOCK_OF(tail) == 2 && BLOCK_OF(reader_waiting) == 2 && BLOCK_OF(reader_pid) == 2,
               "consumer-written fields: block 2");
_Static_assert(offsetof(crd_shm, head) % 8 == 0 && offsetof(crd_shm, tail) % 8 == 0, "64-bit atomics are aligned");
_Static_assert(offsetof(crd_shm, notify_seq) % 4 == 0 && offsetof(crd_shm, reader_waiting) % 4 == 0,
               "32-bit atomics are aligned");

static void test_static_layout(void) {
    /* the same facts at run time, so a failure prints numbers */
    CHECK_EQ(sizeof(crd_shm), (size_t)CRD_HEADER_SIZE);
    CHECK_EQ(offsetof(crd_shm, head), 128u);
    CHECK_EQ(offsetof(crd_shm, tail), 256u);
    CHECK(offsetof(crd_shm, tail) - offsetof(crd_shm, head) >= 2 * 64); /* not on adjacent 64-byte lines */
    CHECK(offsetof(crd_shm, head) / 64 != offsetof(crd_shm, created_unix_ns) / 64);
}

/* Mapping addresses: the header and every record start where the layout says. */
static void test_runtime_alignment(void) {
    CHECK_EQ(crd_create(NAME, 1u << 16, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_writer w;
    crd_reader r;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    CHECK(((uintptr_t)w.shm % 4096) == 0); /* mappings are page-aligned */
    CHECK(((uintptr_t)r.shm % 4096) == 0);
    CHECK(((uintptr_t)&w.shm->head % CRD_CACHELINE) == 0);
    CHECK(((uintptr_t)&r.shm->tail % CRD_CACHELINE) == 0);
    CHECK(((uintptr_t)w.data % CRD_CACHELINE) == 0);
    int misaligned = 0;
    for (uint32_t i = 0; i < 5000; ++i) {
        void *p;
        uint32_t n = (i * 37u) % 1000u;
        if (crd_reserve(&w, n, &p) != CRD_OK) break;
        if ((uintptr_t)p & 7u) ++misaligned;
        crd_commit(&w, n, 1);
        crd_msg m = {0};
        /* each side has its own mapping: compare offsets into the data region */
        if (crd_read(&r, &m) != CRD_OK || ((uintptr_t)m.data & 7u) ||
            (const unsigned char *)m.data - r.data != (unsigned char *)p - w.data)
            ++misaligned;
        crd_release(&r);
    }
    CHECK_EQ(misaligned, 0);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

/* ---- write sets: which header bytes each hot-path call modifies ---------- */

/* Byte ranges of the header that changed between two snapshots. */
static int changed_outside(const unsigned char *before, const unsigned char *after, size_t lo, size_t hi) {
    int n = 0;
    for (size_t i = 0; i < CRD_HEADER_SIZE; ++i)
        if (before[i] != after[i] && (i < lo || i >= hi)) {
            fprintf(stderr, "  header byte %zu changed outside [%zu,%zu)\n", i, lo, hi);
            ++n;
        }
    return n;
}

static int changed_inside(const unsigned char *before, const unsigned char *after, size_t lo, size_t hi) {
    for (size_t i = lo; i < hi; ++i)
        if (before[i] != after[i]) return 1;
    return 0;
}

/* The producer's hot path writes only its own block (plus the data region);
 * the consumer's hot path writes only its block and nothing at all in the data
 * region. So the two processes never write the same cache line: no false
 * sharing, and no write by the reader can disturb a record. */
static void test_hot_path_write_sets(void) {
    CHECK_EQ(crd_create(NAME, 4096, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_writer w;
    crd_reader r;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    const size_t P0 = 1 * CRD_CACHELINE, P1 = 2 * CRD_CACHELINE; /* producer block */
    const size_t C0 = 2 * CRD_CACHELINE, C1 = 3 * CRD_CACHELINE; /* consumer block */
    static unsigned char hdr0[CRD_HEADER_SIZE], hdr1[CRD_HEADER_SIZE], data0[4096];
    int bad = 0, producer_wrote = 0, consumer_wrote = 0;

    for (uint32_t round = 0; round < 300; ++round) {
        uint32_t n = 1 + (round * 53u) % 900u;
        /* producer: reserve + fill + commit + publish + notify (nobody waiting) */
        memcpy(hdr0, w.shm, CRD_HEADER_SIZE);
        void *p;
        if (crd_reserve(&w, n, &p) == CRD_OK) {
            memset(p, (int)round, n);
            crd_commit(&w, n, 2);
            crd_notify(&w);
        }
        memcpy(hdr1, w.shm, CRD_HEADER_SIZE);
        bad += changed_outside(hdr0, hdr1, P0, P1);
        producer_wrote |= changed_inside(hdr0, hdr1, P0, P1);

        /* consumer: read + release; also snapshot the data region */
        memcpy(hdr0, w.shm, CRD_HEADER_SIZE);
        memcpy(data0, w.data, sizeof data0);
        crd_msg m = {0};
        while (crd_read(&r, &m) == CRD_OK) {
        }
        crd_release(&r);
        (void)crd_reader_pending(&r);
        memcpy(hdr1, w.shm, CRD_HEADER_SIZE);
        bad += changed_outside(hdr0, hdr1, C0, C1);
        consumer_wrote |= changed_inside(hdr0, hdr1, C0, C1);
        if (memcmp(data0, w.data, sizeof data0) != 0) ++bad; /* the reader never writes records */
    }
    CHECK_EQ(bad, 0);
    CHECK(producer_wrote); /* the snapshots do observe the stores */
    CHECK(consumer_wrote);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

/* ---- overflow ------------------------------------------------------------ */

static void fill(unsigned char *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i) p[i] = (unsigned char)(seed * 131u + i);
}
static int same(const unsigned char *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] != (unsigned char)(seed * 131u + i)) return 0;
    return 1;
}

/* For every record size class: fill the ring until CRD_FULL, then check that
 * the writer refused exactly when the next record would overwrite unread
 * data, that the refusal changed nothing, and that every record reads back
 * intact. Repeated from many start offsets so the boundary falls everywhere. */
static void test_overflow_boundaries(void) {
    const uint64_t cap = 4096;
    CHECK_EQ(crd_create(NAME, cap, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_writer w;
    crd_reader r;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    unsigned char buf[2048];
    static unsigned char snap[4096];
    int bad = 0;
    uint32_t seq = 0;
    for (uint32_t size = 0; size <= 2040; size += (size < 64 ? 1 : 29)) {
        for (uint32_t shift = 0; shift < 3; ++shift) {
            /* move the start offset */
            for (uint32_t k = 0; k <= shift; ++k) {
                if (crd_write(&w, 1, buf, 8 * (k + 1)) != CRD_OK) ++bad;
                crd_msg m = {0};
                if (crd_read(&r, &m) != CRD_OK) ++bad;
                crd_release(&r);
            }
            uint32_t first = seq, written = 0;
            for (;;) {
                uint64_t used = w.head - __atomic_load_n(&w.shm->tail, __ATOMIC_ACQUIRE);
                if (used > cap) ++bad; /* never more than capacity in flight */
                fill(buf, size, seq);
                uint64_t head_before = w.head;
                memcpy(snap, w.data, cap);
                crd_status s = crd_write(&w, seq, buf, size);
                if (s == CRD_FULL) {
                    /* refusal: no state change, no byte of the data region touched */
                    if (w.head != head_before || memcmp(snap, w.data, cap) != 0) ++bad;
                    /* and it was a real full condition: the record (plus a possible
                     * filler to the end of the buffer) would not fit */
                    uint64_t total = CRD_RECORD_HEADER + ((size + 7u) & ~7u);
                    uint64_t pos = w.head & (cap - 1), start = w.head;
                    if (pos + total > cap) start += cap - pos;
                    if (start + total - __atomic_load_n(&w.shm->tail, __ATOMIC_ACQUIRE) <= cap) ++bad;
                    break;
                }
                if (s != CRD_OK) {
                    ++bad;
                    break;
                }
                ++seq;
                ++written;
            }
            if (written == 0) ++bad; /* an empty ring always takes one record */
            crd_msg m = {0};
            for (uint32_t i = 0; i < written; ++i) {
                if (crd_read(&r, &m) != CRD_OK || m.type != first + i || m.size != size ||
                    !same(m.data, size, first + i))
                    ++bad;
            }
            if (crd_read(&r, &m) != CRD_EMPTY) ++bad;
            crd_release(&r);
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(seq > 1000);
    void *p;
    CHECK_EQ(crd_reserve(&w, crd_max_payload(&w) + 1, &p), CRD_ETOOBIG);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

/* ---- 64-bit positions across the 2^64 wrap -------------------------------- */

/* Positions are free-running byte counters. They do not wrap in practice
 * (2^64 bytes is 584 years at 1 TB/s), but the arithmetic is written modulo
 * 2^64, and this test proves it by starting both sides just below 2^64. */
static void test_position_wrap(void) {
    const uint64_t cap = 4096;
    CHECK_EQ(crd_create(NAME, cap, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_writer w;
    crd_reader r;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    const uint64_t start = 0ull - 3 * cap - 24; /* 8-aligned, 3 laps before the wrap */
    __atomic_store_n(&w.shm->head, start, __ATOMIC_RELEASE);
    __atomic_store_n(&w.shm->tail, start, __ATOMIC_RELEASE);
    w.head = w.tail_cache = start;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    CHECK_EQ(r.cursor, start);

    unsigned char buf[600];
    int bad = 0, crossed = 0;
    uint32_t wseq = 0, rseq = 0;
    for (int step = 0; step < 4000; ++step) {
        uint32_t n = 4 + (uint32_t)(step * 97) % 596u;
        memcpy(buf, &wseq, 4);
        fill(buf + 4, n - 4, wseq);
        uint64_t before = w.head;
        if (crd_write(&w, 3, buf, n) == CRD_OK) {
            ++wseq;
            if (w.head < before) crossed = 1; /* went through 2^64 */
        }
        if (step % 3 == 0) {
            crd_msg m = {0};
            while (crd_read(&r, &m) == CRD_OK) {
                uint32_t s;
                memcpy(&s, m.data, 4);
                if (s != rseq || !same((const unsigned char *)m.data + 4, m.size - 4, rseq)) ++bad;
                ++rseq;
            }
            crd_release(&r);
            if (crd_writer_free(&w) > cap) ++bad;
        }
    }
    crd_msg m = {0};
    while (crd_read(&r, &m) == CRD_OK) ++rseq;
    crd_release(&r);
    CHECK(crossed);
    CHECK_EQ(bad, 0);
    CHECK_EQ(rseq, wseq);
    CHECK_EQ(crd_writer_free(&w), cap);
    crd_info info;
    CHECK_EQ(crd_inspect(NAME, &info), CRD_OK);
    CHECK_EQ(info.used_bytes, 0u);
    CHECK(info.head < start); /* small number after the wrap */
    crd_writer_close(&w);
    crd_reader_close(&r);
    /* a writer re-attaching after the wrap accepts the state */
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    CHECK_EQ(crd_write(&w, 1, "z", 1), CRD_OK);
    crd_writer_close(&w);
}

int main(void) {
    crd_test_name(NAME, sizeof NAME, "layout");
    test_static_layout();
    test_runtime_alignment();
    test_hot_path_write_sets();
    test_overflow_boundaries();
    test_position_wrap();
    crd_unlink(NAME);
    return crd_test_report("test_layout");
}
