/* Single-process unit tests: API contract, edge cases, corruption handling. */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "corundum/corundum.h"
#include "check.h"

#include <string.h>

static char NAME[64];

static void fresh(uint64_t capacity) {
    CHECK_EQ(crd_create(NAME, capacity, 0600, CRD_CREATE_REPLACE), CRD_OK);
}

static void test_validation(void) {
    CHECK_EQ(crd_create(NULL, 4096, 0600, 0), CRD_EINVAL);
    CHECK_EQ(crd_create("noslash", 4096, 0600, 0), CRD_EINVAL);
    CHECK_EQ(crd_create("/a/b", 4096, 0600, 0), CRD_EINVAL);
    CHECK_EQ(crd_create("/", 4096, 0600, 0), CRD_EINVAL);
    CHECK_EQ(crd_create(NAME, 0, 0600, 0), CRD_EINVAL);
    CHECK_EQ(crd_create(NAME, 2048, 0600, 0), CRD_EINVAL);     /* below minimum */
    CHECK_EQ(crd_create(NAME, 4097, 0600, 0), CRD_EINVAL);     /* not a power of two */
    CHECK_EQ(crd_create(NAME, 1ull << 33, 0600, 0), CRD_EINVAL); /* above maximum */
    CHECK_EQ(crd_create(NAME, 4096, 0600, 0x80), CRD_EINVAL);  /* unknown flag */

    crd_writer w;
    crd_reader r;
    crd_unlink(NAME);
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_ENOENT);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_ENOENT);
    CHECK_EQ(crd_unlink(NAME), CRD_ENOENT);

    CHECK_EQ(crd_create(NAME, 4096, 0600, 0), CRD_OK);
    CHECK_EQ(crd_create(NAME, 4096, 0600, 0), CRD_EEXIST);
    CHECK_EQ(crd_create(NAME, 8192, 0600, CRD_CREATE_REPLACE), CRD_OK);
    CHECK_EQ(crd_writer_open(&w, NAME, 0x100), CRD_EINVAL);
    CHECK_EQ(crd_writer_open(&w, NAME, CRD_OPEN_PREFAULT), CRD_OK);
    CHECK_EQ(w.capacity, 8192u);
    CHECK_EQ(crd_max_payload(&w), 4096u - 8u);
    crd_writer_close(&w);
    crd_writer_close(&w); /* idempotent on a closed handle */

    for (int s = -9; s <= 4; ++s) CHECK(crd_strerror((crd_status)s) != NULL);
}

/* A segment that exists but is not (yet) a valid ring. */
static void test_foreign_segment(void) {
    crd_unlink(NAME);
    crd_test_seg seg;
    crd_reader r;
#if !defined(_WIN32)
    /* Linux only: a POSIX segment can exist with size 0 while its creator is mid-way */
    int fd = shm_open(NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_ENOTREADY);
    close(fd);
    shm_unlink(NAME);
#endif
    CHECK(crd_test_seg_create(&seg, NAME, CRD_HEADER_SIZE + 4096));
    crd_shm *shm = (crd_shm *)seg.p;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_ENOTREADY); /* magic not yet published */
    shm->magic = 0x1234;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_EVERSION);
    shm->magic = CRD_MAGIC;
    shm->version = 99;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_EVERSION);
    shm->version = CRD_LAYOUT_VERSION;
    shm->header_size = CRD_HEADER_SIZE;
    shm->capacity = 8192; /* larger than the segment */
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_ECORRUPT);
    shm->capacity = 4096;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    crd_reader_close(&r);
    crd_test_seg_destroy(&seg, NAME);
}

static void test_exclusive_roles(void) {
    fresh(4096);
    crd_writer w1, w2;
    crd_reader r1, r2;
    CHECK_EQ(crd_writer_open(&w1, NAME, 0), CRD_OK);
    CHECK_EQ(crd_writer_open(&w2, NAME, 0), CRD_EBUSY);
    CHECK_EQ(crd_reader_open(&r1, NAME, 0), CRD_OK);
    CHECK_EQ(crd_reader_open(&r2, NAME, 0), CRD_EBUSY);
    crd_writer_close(&w1);
    CHECK_EQ(crd_writer_open(&w2, NAME, 0), CRD_OK);
    crd_writer_close(&w2);
    crd_reader_close(&r1);

    /* a writer slot held by a process that no longer exists is reclaimed */
    int dead = crd_test_dead_pid();
    CHECK(dead > 0);
    crd_writer_open(&w1, NAME, 0);
    w1.shm->writer_pid = (int32_t)dead;
    crd_shm *shm = w1.shm;
    CHECK_EQ(crd_writer_open(&w2, NAME, 0), CRD_OK); /* stale pid */
    crd_writer_close(&w2);

    /* a live foreign holder blocks, unless TAKEOVER is given */
    shm->writer_pid = (int32_t)crd_test_live_foreign_pid();
    CHECK_EQ(crd_writer_open(&w2, NAME, 0), CRD_EBUSY);
    CHECK_EQ(crd_writer_open(&w2, NAME, CRD_OPEN_TAKEOVER), CRD_OK);
    crd_writer_close(&w2);
    w1.shm->writer_pid = (int32_t)crd_test_pid();
    crd_writer_close(&w1);
}

static void fill_pattern(unsigned char *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i) p[i] = (unsigned char)(seed * 31u + i * 7u);
}
static int check_pattern(const unsigned char *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] != (unsigned char)(seed * 31u + i * 7u)) return 0;
    return 1;
}

static void test_roundtrip(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    CHECK_EQ(crd_read(&r, &m), CRD_EMPTY);

    unsigned char buf[200];
    for (uint32_t n = 0; n <= 100; ++n) {
        fill_pattern(buf, n, n);
        CHECK_EQ(crd_write(&w, n + 1000, buf, n), CRD_OK);
        CHECK_EQ(crd_read(&r, &m), CRD_OK);
        CHECK_EQ(m.size, n);
        CHECK_EQ(m.type, n + 1000);
        CHECK(((uintptr_t)m.data & 7u) == 0);
        CHECK(check_pattern(m.data, n, n));
        CHECK_EQ(crd_read(&r, &m), CRD_EMPTY);
        crd_release(&r);
    }
    crd_writer_close(&w);
    crd_reader_close(&r);
}

static void test_full_and_release(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    crd_writer_open(&w, NAME, 0);
    crd_reader_open(&r, NAME, 0);
    unsigned char buf[100] = {0};
    int n = 0;
    while (crd_write(&w, 1, buf, sizeof buf) == CRD_OK) ++n;
    CHECK_EQ(n, 4096 / (8 + 104)); /* 36 records of 112 bytes */
    CHECK_EQ(crd_write(&w, 1, buf, sizeof buf), CRD_FULL);
    CHECK_EQ(crd_read(&r, &m), CRD_OK);
    CHECK_EQ(crd_write(&w, 1, buf, sizeof buf), CRD_FULL); /* read but not released yet */
    crd_release(&r);
    CHECK_EQ(crd_write(&w, 1, buf, sizeof buf), CRD_OK);
    CHECK(crd_writer_free(&w) < 112);
    int got = 0;
    while (crd_read(&r, &m) == CRD_OK) ++got;
    CHECK_EQ(got, n);
    crd_release(&r);
    CHECK_EQ(crd_writer_free(&w), 4096u);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

static void test_too_big(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    void *p = NULL;
    crd_writer_open(&w, NAME, 0);
    crd_reader_open(&r, NAME, 0);
    CHECK_EQ(crd_reserve(&w, 2041, &p), CRD_ETOOBIG);
    CHECK_EQ(crd_reserve(&w, UINT32_MAX, &p), CRD_ETOOBIG);
    /* the maximum fits wherever head is, even when it forces a wrap */
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(crd_reserve(&w, 2040, &p), CRD_OK);
        memset(p, i, 2040);
        CHECK_EQ(crd_commit(&w, 2040, (uint32_t)i), CRD_OK);
        CHECK_EQ(crd_read(&r, &m), CRD_OK);
        CHECK_EQ(m.size, 2040u);
        CHECK_EQ(((const unsigned char *)m.data)[2039], (unsigned char)i);
        crd_release(&r);
        CHECK_EQ(crd_write(&w, 9, "abc", 3), CRD_OK); /* shift head off alignment with the end */
        CHECK_EQ(crd_read(&r, &m), CRD_OK);
        crd_release(&r);
    }
    crd_writer_close(&w);
    crd_reader_close(&r);
}

static void test_reserve_commit_rules(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    void *p = NULL;
    crd_writer_open(&w, NAME, 0);
    crd_reader_open(&r, NAME, 0);
    CHECK_EQ(crd_commit(&w, 0, 1), CRD_EINVAL); /* nothing reserved */
    CHECK_EQ(crd_reserve(&w, 64, &p), CRD_OK);
    CHECK_EQ(crd_commit(&w, 65, 1), CRD_EINVAL); /* larger than reserved */
    CHECK_EQ(crd_commit(&w, 10, CRD_PAD_TYPE), CRD_EINVAL);
    memcpy(p, "hello", 5);
    CHECK_EQ(crd_commit(&w, 5, 7), CRD_OK); /* smaller than reserved is fine */
    CHECK_EQ(crd_commit(&w, 5, 7), CRD_EINVAL); /* reservation consumed */
    CHECK_EQ(crd_read(&r, &m), CRD_OK);
    CHECK_EQ(m.size, 5u);
    CHECK(memcmp(m.data, "hello", 5) == 0);
    CHECK_EQ(crd_read(&r, &m), CRD_EMPTY);
    crd_release(&r);
    CHECK_EQ(crd_writer_free(&w), 4096u);

    /* staging: invisible until publish */
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK_EQ(crd_reserve(&w, 4, &p), CRD_OK);
        memcpy(p, &i, 4);
        CHECK_EQ(crd_stage(&w, 4, i), CRD_OK);
    }
    CHECK_EQ(crd_read(&r, &m), CRD_EMPTY);
    CHECK_EQ(crd_reader_pending(&r), 0u);
    crd_publish(&w);
    CHECK_EQ(crd_reader_pending(&r), 3u * 16u);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK_EQ(crd_read(&r, &m), CRD_OK);
        CHECK_EQ(m.type, i);
        CHECK_EQ(*(const uint32_t *)m.data, i);
    }
    /* batched reads: pointers stay valid until release */
    crd_release(&r);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

/* Interleaved writes and reads with varying sizes across thousands of wraps. */
static void test_wrap_sequence(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    crd_writer_open(&w, NAME, 0);
    crd_reader_open(&r, NAME, 0);
    uint32_t rng = 12345, next_w = 0, next_r = 0, errors = 0;
    unsigned char buf[2048];
    for (int step = 0; step < 400000; ++step) {
        rng = rng * 1103515245u + 12345u;
        if ((rng >> 16) & 1) {
            uint32_t n = 4 + (rng >> 20) % 700;
            memcpy(buf, &next_w, 4);
            fill_pattern(buf + 4, n - 4, next_w);
            if (crd_write(&w, next_w, buf, n) == CRD_OK) ++next_w;
        } else {
            int batch = 1 + (int)((rng >> 8) % 4);
            for (int b = 0; b < batch && crd_read(&r, &m) == CRD_OK; ++b) {
                uint32_t seq;
                memcpy(&seq, m.data, 4);
                /* bytes 0..3 hold the sequence number, the rest the pattern */
                if (seq != next_r || m.type != next_r ||
                    !check_pattern((const unsigned char *)m.data + 4, m.size - 4, next_r))
                    ++errors;
                ++next_r;
            }
            crd_release(&r);
        }
    }
    while (crd_read(&r, &m) == CRD_OK) ++next_r;
    CHECK_EQ(errors, 0u);
    CHECK_EQ(next_r, next_w);
    CHECK(next_w > 100000u);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

static void test_corruption_detected(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    crd_writer_open(&w, NAME, 0);
    crd_reader_open(&r, NAME, 0);
    crd_shm *shm = w.shm;

    shm->head = 8192; /* more than capacity ahead of tail */
    CHECK_EQ(crd_read(&r, &m), CRD_ECORRUPT);
    shm->head = 12; /* not record-aligned */
    r.head_cache = r.cursor;
    CHECK_EQ(crd_read(&r, &m), CRD_ECORRUPT);

    uint32_t *rec = (uint32_t *)w.data;
    rec[0] = 4000; /* claims more bytes than were published */
    rec[1] = 1;
    shm->head = 16;
    r.head_cache = r.cursor;
    CHECK_EQ(crd_read(&r, &m), CRD_ECORRUPT);
    rec[0] = 100; /* bogus filler not reaching the end of the buffer */
    rec[1] = CRD_PAD_TYPE;
    CHECK_EQ(crd_read(&r, &m), CRD_ECORRUPT);
    rec[0] = 8;
    rec[1] = 3;
    CHECK_EQ(crd_read(&r, &m), CRD_OK); /* valid again */

    crd_writer_close(&w);
    crd_reader_close(&r);
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK); /* close restored a sane head */
    crd_writer_close(&w);
}

static void test_inspect_close_wait(void) {
    fresh(4096);
    crd_writer w;
    crd_reader r;
    crd_msg m = {0};
    crd_info info;
    crd_writer_open(&w, NAME, 0);
    crd_reader_open(&r, NAME, 0);
    crd_write(&w, 1, "xy", 2);
    CHECK_EQ(crd_inspect(NAME, &info), CRD_OK);
    CHECK_EQ(info.capacity, 4096u);
    CHECK_EQ(info.used_bytes, 16u);
    CHECK_EQ(info.writer_pid, (int32_t)crd_test_pid());
    CHECK_EQ(info.reader_pid, (int32_t)crd_test_pid());
    CHECK_EQ(info.writer_closed, 0);
    CHECK(info.created_unix_ns > 0);

    CHECK_EQ(crd_wait(&r, 0), CRD_OK); /* data pending */
    crd_read(&r, &m);
    crd_release(&r);

    int64_t t0 = crd_test_ms();
    CHECK_EQ(crd_wait(&r, 20 * 1000 * 1000), CRD_TIMEOUT);
    int64_t ms = crd_test_ms() - t0;
    CHECK(ms >= 15); /* GetTickCount64 has ~16 ms granularity on Windows */
    CHECK_EQ(crd_wait(&r, 0), CRD_TIMEOUT);

    crd_write(&w, 2, "last", 4);
    crd_writer_close(&w);
    CHECK(crd_writer_closed(&r));
    CHECK_EQ(crd_wait(&r, -1), CRD_OK); /* the last record is still readable */
    CHECK_EQ(crd_read(&r, &m), CRD_OK);
    CHECK_EQ(m.type, 2u);
    crd_release(&r);
    CHECK_EQ(crd_wait(&r, -1), CRD_CLOSED);
    CHECK_EQ(crd_inspect(NAME, &info), CRD_OK);
    CHECK_EQ(info.writer_pid, 0);
    CHECK_EQ(info.writer_closed, 1);

    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK); /* a new writer reopens the stream */
    CHECK(!crd_writer_closed(&r));
    crd_writer_close(&w);
    crd_reader_close(&r);
}

int main(void) {
    crd_test_name(NAME, sizeof NAME, "ring");
    test_validation();
    test_foreign_segment();
    test_exclusive_roles();
    test_roundtrip();
    test_full_and_release();
    test_too_big();
    test_reserve_commit_rules();
    test_wrap_sequence();
    test_corruption_detected();
    test_inspect_close_wait();
    crd_unlink(NAME);
    return crd_test_report("test_ring");
}
