/*
 * Parts of corundum-bench shared by the Linux and Windows drivers: the cycle
 * counter, percentile reports, kernel-entry counters, the in-process "ops"
 * benchmark and both ends of the cross-process one-way benchmark.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#ifndef CRD_BENCH_COMMON_H
#define CRD_BENCH_COMMON_H

#include "corundum/corundum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

/* ---- clocks --------------------------------------------------------------- */

static inline uint64_t bench_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* CPU cycle counter, read without a system call. On x86-64 this is the
 * invariant TSC (constant rate, synchronised across the cores of one socket);
 * on arm64 the generic timer, which is synchronised by design. The fences
 * keep the read from moving across the loads and stores around it. */
static inline uint64_t ticks(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0; isb" : "=r"(v)::"memory");
    return v;
#else
    return bench_now_ns();
#endif
}

typedef struct {
    uint64_t t0, n0;
} calib;
static inline calib calib_start(void) {
    calib c;
    c.t0 = ticks();
    c.n0 = bench_now_ns();
    return c;
}
static inline double calib_ns_per_tick(calib c) {
    uint64_t t1 = ticks(), n1 = bench_now_ns();
    return (double)(n1 - c.n0) / (double)(t1 - c.t0);
}

/* ---- reports -------------------------------------------------------------- */

static int bench_cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Sorts samples (in ticks) and prints min / p50 / p99 / p99.9 / max / mean,
 * each multiplied by `scale` ns per tick. */
static void report_ticks(const char *what, uint32_t *v, size_t n, double scale) {
    qsort(v, n, sizeof *v, bench_cmp_u32);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += v[i];
#define P(q) (v[(size_t)((double)(n - 1) * (q))] * scale)
    printf("%-22s min %6.0f  p50 %6.0f  p99 %6.0f  p99.9 %7.0f  max %8.0f  mean %6.1f  ns\n", what, v[0] * scale,
           P(0.50), P(0.99), P(0.999), v[n - 1] * scale, sum / (double)n * scale);
#undef P
}

static void bench_die(const char *what, crd_status s) {
    fprintf(stderr, "%s: %s\n", what, crd_strerror(s));
    exit(1);
}

/* ---- kernel entries during a hot loop ------------------------------------- */

/* What the OS can tell about a thread entering the kernel during a hot loop.
 *
 * Linux: getrusage(RUSAGE_THREAD): context switches (a voluntary one means
 * the thread blocked in a system call; an involuntary one means the scheduler
 * preempted it) and system time.
 *
 * Windows: the thread's context-switch count from NtQuerySystemInformation
 * (SystemProcessInformation, the same counter Process Explorer shows), and
 * kernel time from GetThreadTimes. Windows charges kernel and user time per
 * scheduler tick (~15.6 ms), so a few ms of "kernel time" over a short loop
 * is sampling noise from interrupts, not system calls.
 *
 * Both are read only before and after the loop, never inside it. */
typedef struct {
    int64_t switches;  /* context switches; -1 if unavailable */
    int64_t voluntary; /* of which voluntary (Linux only); -1 if unavailable */
    int64_t kernel_ns, user_ns;
} kstats;

#if defined(_WIN32)
/* x64 layouts of SYSTEM_PROCESS_INFORMATION / SYSTEM_THREAD_INFORMATION:
 * stable since Windows XP x64 and used by every process-inspection tool. */
typedef LONG(WINAPI *crd_ntqsi_fn)(ULONG, PVOID, ULONG, PULONG);
static int64_t bench_thread_switches(void) {
    static crd_ntqsi_fn ntqsi;
    static unsigned char *buf;
    static ULONG cap;
    if (!ntqsi) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        FARPROC f = nt ? GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
        if (!f) return -1;
        memcpy(&ntqsi, &f, sizeof ntqsi);
    }
    for (int tries = 0; tries < 8; ++tries) {
        ULONG need = 0;
        if (buf) {
            LONG st = ntqsi(5 /* SystemProcessInformation */, buf, cap, &need);
            if (st == 0) break;
            if (st != (LONG)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */) return -1;
        }
        free(buf);
        cap = (need > cap ? need : cap) * 2 + (1u << 20);
        buf = (unsigned char *)malloc(cap);
        if (!buf) return -1;
        if (tries == 7) return -1;
    }
    const uintptr_t pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    for (unsigned char *p = buf;;) {
        ULONG next, nthreads;
        uintptr_t upid;
        memcpy(&next, p + 0x00, 4);
        memcpy(&nthreads, p + 0x04, 4);
        memcpy(&upid, p + 0x50, 8);
        if (upid == pid) {
            for (ULONG i = 0; i < nthreads; ++i) {
                const unsigned char *t = p + 0x100 + (size_t)i * 0x50;
                uintptr_t utid;
                ULONG sw;
                memcpy(&utid, t + 0x30, 8);
                memcpy(&sw, t + 0x40, 4);
                if (utid == tid) return (int64_t)sw;
            }
            return -1;
        }
        if (!next) return -1;
        p += next;
    }
}
#endif

static inline kstats kstats_now(void) {
    kstats k;
#if defined(_WIN32)
    k.switches = bench_thread_switches();
    k.voluntary = -1;
    FILETIME c, e, kt, ut;
    GetThreadTimes(GetCurrentThread(), &c, &e, &kt, &ut);
    k.kernel_ns = (int64_t)((((uint64_t)kt.dwHighDateTime << 32) | kt.dwLowDateTime) * 100u);
    k.user_ns = (int64_t)((((uint64_t)ut.dwHighDateTime << 32) | ut.dwLowDateTime) * 100u);
#else
    struct rusage ru;
#ifdef RUSAGE_THREAD
    getrusage(RUSAGE_THREAD, &ru);
#else
    getrusage(RUSAGE_SELF, &ru);
#endif
    k.voluntary = ru.ru_nvcsw;
    k.switches = ru.ru_nvcsw + ru.ru_nivcsw;
    k.kernel_ns = (int64_t)ru.ru_stime.tv_sec * 1000000000 + (int64_t)ru.ru_stime.tv_usec * 1000;
    k.user_ns = (int64_t)ru.ru_utime.tv_sec * 1000000000 + (int64_t)ru.ru_utime.tv_usec * 1000;
#endif
    return k;
}

static inline kstats kstats_diff(kstats a, kstats b) {
    kstats d;
    d.switches = (a.switches < 0 || b.switches < 0) ? -1 : b.switches - a.switches;
    d.voluntary = (a.voluntary < 0 || b.voluntary < 0) ? -1 : b.voluntary - a.voluntary;
    d.kernel_ns = b.kernel_ns - a.kernel_ns;
    d.user_ns = b.user_ns - a.user_ns;
    return d;
}

static void report_kstats(const char *who, kstats d) {
    char sw[96] = "context switches: n/a";
    if (d.switches >= 0 && d.voluntary >= 0)
        snprintf(sw, sizeof sw, "%lld context switches (%lld voluntary)", (long long)d.switches, (long long)d.voluntary);
    else if (d.switches >= 0)
        snprintf(sw, sizeof sw, "%lld context switches", (long long)d.switches);
    printf("  %-7s hot loop: %s, kernel %.1f ms, user %.1f ms\n", who, sw, (double)d.kernel_ns * 1e-6,
           (double)d.user_ns * 1e-6);
}

/* ---- ops: cost of the calls themselves, one thread ------------------------- */

typedef struct {
    uint64_t seq;
    uint64_t t_send; /* ticks() just before the record was committed */
    unsigned char pad[48];
} pkt64; /* 64-byte payload, 72 bytes in the ring */

/* Writes and reads 64-byte packets in one thread on a warm ring: no cache
 * line moves between cores, so this is the instruction cost of the hot path
 * (reserve + fill + commit, read + use + release), the floor under any
 * cross-process number. */
static void bench_ops(const char *ring, size_t n) {
    crd_status s;
    if ((s = crd_create(ring, 1u << 20, 0600, CRD_CREATE_REPLACE)) != CRD_OK) bench_die("create", s);
    crd_writer w;
    crd_reader r;
    if ((s = crd_writer_open(&w, ring, CRD_OPEN_PREFAULT)) != CRD_OK) bench_die("writer_open", s);
    if ((s = crd_reader_open(&r, ring, CRD_OPEN_PREFAULT)) != CRD_OK) bench_die("reader_open", s);
    const size_t burst = (1u << 20) / 2 / sizeof(pkt64) / 2 * 2; /* records that fit in half the ring */
    uint64_t sink = 0, write_t = 0, read_t = 0, rt_t = 0;
    size_t done = 0;
    calib cal = calib_start();
    kstats k0 = kstats_now();
    for (int warm = 1; warm >= 0; --warm) { /* first pass warms caches and TLB, not counted */
        done = 0;
        write_t = read_t = rt_t = 0;
        while (done < n) {
            size_t b = n - done < burst ? n - done : burst;
            uint64_t t0 = ticks();
            for (size_t i = 0; i < b; ++i) {
                pkt64 *p;
                if (crd_reserve(&w, sizeof *p, (void **)&p) != CRD_OK) bench_die("ops reserve", CRD_FULL);
                p->seq = i;
                p->t_send = t0;
                crd_commit(&w, sizeof *p, 1);
            }
            uint64_t t1 = ticks();
            for (size_t i = 0; i < b; ++i) {
                crd_msg m;
                if (crd_read(&r, &m) != CRD_OK) bench_die("ops read", CRD_ECORRUPT);
                sink += ((const pkt64 *)m.data)->seq;
                crd_release(&r);
            }
            uint64_t t2 = ticks();
            /* write immediately followed by read of the same packet */
            for (size_t i = 0; i < b; ++i) {
                pkt64 *p;
                if (crd_reserve(&w, sizeof *p, (void **)&p) != CRD_OK) bench_die("ops reserve", CRD_FULL);
                p->seq = i;
                crd_commit(&w, sizeof *p, 1);
                crd_msg m;
                if (crd_read(&r, &m) != CRD_OK) bench_die("ops read", CRD_ECORRUPT);
                sink += ((const pkt64 *)m.data)->seq;
                crd_release(&r);
            }
            uint64_t t3 = ticks();
            write_t += t1 - t0;
            read_t += t2 - t1;
            rt_t += t3 - t2;
            done += b;
        }
    }
    kstats kd = kstats_diff(k0, kstats_now());
    const double npt = calib_ns_per_tick(cal);
    if (sink == 42) puts("");
    printf("write  (reserve + fill + commit)  %6.2f ns/packet\n", (double)write_t * npt / (double)n);
    printf("read   (read + use + release)     %6.2f ns/packet\n", (double)read_t * npt / (double)n);
    printf("write + read of the same packet   %6.2f ns/packet\n", (double)rt_t * npt / (double)n);
    report_kstats("ops", kd);
    crd_writer_close(&w);
    crd_reader_close(&r);
    crd_unlink(ring);
}

/* ---- one-way: writer in one process, reader in another --------------------- */

/*
 * The writer stamps each packet with ticks() right before committing it; the
 * reader reads ticks() right after crd_read returns it. The difference is the
 * one-way latency: publish store, cache-line transfer to the other core,
 * header and payload loads. The writer sends the next packet only after the
 * reader has released the previous one, so the ring is empty every time and
 * there is no queueing. Requires a counter that is synchronised across cores
 * (true for the invariant TSC on a single-socket x86-64 machine and for the
 * arm64 generic timer); a negative difference would show it is not, and the
 * reader counts those.
 *
 * Type 1 records are packets; one type 2 record at the end carries the
 * writer's kernel-entry counters.
 */
#define ONEWAY_GAP_TICKS 2000 /* idle time before each packet, so the reader is polling */

static void oneway_writer(crd_writer *w, size_t total) {
    kstats k0 = kstats_now();
    for (size_t i = 0; i < total; ++i) {
        while (crd_writer_free(w) != w->capacity) crd_cpu_relax(); /* previous packet released */
        uint64_t until = ticks() + ONEWAY_GAP_TICKS;
        while (ticks() < until) crd_cpu_relax();
        pkt64 *p;
        while (crd_reserve(w, sizeof *p, (void **)&p) != CRD_OK) crd_cpu_relax();
        p->seq = i;
        p->t_send = ticks();
        crd_commit(w, sizeof *p, 1);
    }
    kstats kd = kstats_diff(k0, kstats_now());
    while (crd_write(w, 2, &kd, sizeof kd) != CRD_OK) crd_cpu_relax();
}

/* Returns 0 on success. Prints the distribution and both sides' counters. */
static int oneway_reader(crd_reader *r, size_t n, size_t warm) {
    uint32_t *lat = (uint32_t *)malloc(n * sizeof *lat);
    size_t negative = 0;
    calib cal = calib_start();
    kstats k0 = kstats_now();
    for (size_t i = 0; i < n + warm; ++i) {
        crd_msg m;
        while (crd_read(r, &m) != CRD_OK) crd_cpu_relax();
        uint64_t t_recv = ticks();
        const pkt64 *p = (const pkt64 *)m.data;
        if (m.type != 1 || p->seq != i) bench_die("oneway sequence", CRD_ECORRUPT);
        int64_t d = (int64_t)(t_recv - p->t_send);
        crd_release(r);
        if (i < warm) continue;
        if (d < 0) {
            ++negative;
            d = 0;
        }
        lat[i - warm] = d > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)d;
    }
    kstats kd = kstats_diff(k0, kstats_now());
    const double npt = calib_ns_per_tick(cal);
    crd_msg m;
    while (crd_read(r, &m) != CRD_OK) crd_cpu_relax();
    kstats wk;
    memset(&wk, 0, sizeof wk);
    if (m.type == 2 && m.size == sizeof wk) memcpy(&wk, m.data, sizeof wk);
    crd_release(r);
    report_ticks("corundum one-way", lat, n, npt);
    if (negative) printf("  %zu samples had receive time < send time: counters are not synchronised\n", negative);
    report_kstats("writer", wk);
    report_kstats("reader", kd);
    free(lat);
    return 0;
}

#endif
