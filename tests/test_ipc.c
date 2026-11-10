/* Cross-process tests: a forked writer and the parent as reader share one ring. */
#define _GNU_SOURCE
#include "corundum/corundum.h"
#include "check.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

static char NAME[64];

static uint64_t mix(uint64_t x) { /* splitmix64 */
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* Payload of message `seq`: size depends on seq, bytes derived from seq. */
static uint32_t msg_size(uint64_t seq) { return 8u + (uint32_t)(mix(seq) % 377u); }
static void msg_fill(unsigned char *p, uint32_t n, uint64_t seq) {
    memcpy(p, &seq, 8);
    uint64_t s = mix(seq ^ 0xABCDEFull);
    for (uint32_t i = 8; i < n; ++i) {
        p[i] = (unsigned char)(s >> ((i & 7u) * 8u));
        if ((i & 7u) == 7u) s = mix(s);
    }
}
static int msg_ok(const crd_msg *m, uint64_t seq) {
    unsigned char want[400];
    uint32_t n = msg_size(seq);
    if (m->size != n || m->type != (uint32_t)seq) return 0;
    msg_fill(want, n, seq);
    return memcmp(m->data, want, n) == 0;
}

static pid_t spawn_writer(void (*body)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(crd_test_failures ? 1 : 0);
    }
    return pid;
}

static int child_status(pid_t pid) {
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* ---- 1. busy-poll stress: millions of variable-size records, verified byte by byte ---- */

static const uint64_t STRESS_N = 3000000;

static void stress_writer(void) {
    crd_writer w;
    if (crd_writer_open(&w, NAME, CRD_OPEN_PREFAULT) != CRD_OK) _exit(2);
    for (uint64_t seq = 0; seq < STRESS_N; ++seq) {
        const uint32_t n = msg_size(seq);
        void *p = NULL;
        while (crd_reserve(&w, n, &p) == CRD_FULL) {
            crd_publish(&w); /* staged records must become visible, or the reader never frees space */
            crd_cpu_relax();
        }
        msg_fill(p, n, seq);
        /* alternate between immediate commits and small staged batches */
        if (seq % 3 == 0) {
            crd_stage(&w, n, (uint32_t)seq);
            if (seq % 9 == 0) crd_publish(&w);
        } else {
            crd_commit(&w, n, (uint32_t)seq);
        }
    }
    crd_writer_close(&w); /* publishes the tail end */
}

static void test_stress(void) {
    CHECK_EQ(crd_create(NAME, 1u << 16, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_reader r;
    CHECK_EQ(crd_reader_open(&r, NAME, CRD_OPEN_PREFAULT), CRD_OK);
    pid_t child = spawn_writer(stress_writer);

    uint64_t seq = 0, bad = 0, since_release = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (seq < STRESS_N) {
        crd_msg m = {0};
        crd_status s = crd_read(&r, &m);
        if (s == CRD_OK) {
            if (!msg_ok(&m, seq)) ++bad;
            ++seq;
            if (++since_release == 32) { /* batched release */
                crd_release(&r);
                since_release = 0;
            }
        } else if (s == CRD_EMPTY) {
            crd_release(&r);
            since_release = 0;
            if (crd_writer_closed(&r) && crd_reader_pending(&r) == 0) break;
            crd_cpu_relax();
        } else {
            ++bad;
            break;
        }
    }
    crd_release(&r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("stress: %llu records verified in %.2f s (%.1f M/s)\n", (unsigned long long)seq, sec, (double)seq / sec / 1e6);
    CHECK_EQ(bad, 0u);
    CHECK_EQ(seq, STRESS_N);
    CHECK_EQ(child_status(child), 0);
    crd_reader_close(&r);
}

/* ---- 2. futex wait/notify: a sleeping reader never misses a record ---- */

static const uint64_t WAIT_N = 20000;

static void wait_writer(void) {
    crd_writer w;
    if (crd_writer_open(&w, NAME, 0) != CRD_OK) _exit(2);
    uint64_t rng = 1;
    for (uint64_t seq = 0; seq < WAIT_N; ++seq) {
        const uint32_t n = msg_size(seq);
        void *p = NULL;
        while (crd_reserve(&w, n, &p) == CRD_FULL) crd_cpu_relax();
        msg_fill(p, n, seq);
        crd_commit(&w, n, (uint32_t)seq);
        crd_notify(&w);
        rng = mix(rng);
        if (rng % 64 == 0) { /* pause now and then so the reader really goes to sleep */
            struct timespec ts = {0, (long)(rng % 200000)};
            nanosleep(&ts, NULL);
        }
    }
    crd_writer_close(&w);
}

static void test_wait_notify(void) {
    CHECK_EQ(crd_create(NAME, 1u << 14, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_reader r;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    pid_t child = spawn_writer(wait_writer);
    uint64_t seq = 0, bad = 0, timeouts = 0;
    crd_status ws;
    for (;;) {
        ws = crd_wait(&r, 2000000000ll); /* 2 s: a lost wake-up shows up as a timeout */
        if (ws == CRD_TIMEOUT) {
            ++timeouts;
            break;
        }
        if (ws != CRD_OK) break;
        crd_msg m = {0};
        while (crd_read(&r, &m) == CRD_OK) {
            if (!msg_ok(&m, seq)) ++bad;
            ++seq;
        }
        crd_release(&r);
    }
    printf("wait/notify: %llu records, final status %s\n", (unsigned long long)seq, crd_strerror(ws));
    CHECK_EQ(ws, CRD_CLOSED);
    CHECK_EQ(timeouts, 0u);
    CHECK_EQ(bad, 0u);
    CHECK_EQ(seq, WAIT_N);
    CHECK_EQ(child_status(child), 0);
    crd_reader_close(&r);
}

/* ---- 3. writer crash: data survives and a new writer can attach ---- */

static void crash_writer(void) {
    crd_writer w;
    if (crd_writer_open(&w, NAME, 0) != CRD_OK) _exit(2);
    for (uint64_t seq = 0; seq < 10; ++seq) {
        unsigned char buf[400];
        msg_fill(buf, msg_size(seq), seq);
        crd_write(&w, (uint32_t)seq, buf, msg_size(seq));
    }
    raise(SIGKILL); /* no crd_writer_close */
}

static void test_writer_crash(void) {
    CHECK_EQ(crd_create(NAME, 1u << 14, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_reader r;
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    pid_t child = spawn_writer(crash_writer);
    CHECK_EQ(child_status(child), 100 + SIGKILL);

    crd_info info;
    CHECK_EQ(crd_inspect(NAME, &info), CRD_OK);
    CHECK_EQ(info.writer_pid, (int32_t)child); /* slot still names the dead process */

    crd_msg m = {0};
    uint64_t seq = 0;
    while (crd_read(&r, &m) == CRD_OK) {
        CHECK(msg_ok(&m, seq));
        ++seq;
    }
    CHECK_EQ(seq, 10u);
    crd_release(&r);

    crd_writer w;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK); /* stale owner is detected */
    CHECK_EQ(crd_write(&w, 77, "again", 5), CRD_OK);
    CHECK_EQ(crd_read(&r, &m), CRD_OK);
    CHECK_EQ(m.type, 77u);
    crd_writer_close(&w);
    crd_reader_close(&r);
}

int main(void) {
    crd_test_name(NAME, sizeof NAME, "ipc");
    test_stress();
    test_wait_notify();
    test_writer_crash();
    crd_unlink(NAME);
    return crd_test_report("test_ipc");
}
