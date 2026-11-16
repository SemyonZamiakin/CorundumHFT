/*
 * corundum-bench (Windows): latency and throughput of Corundum against an
 * anonymous pipe, between two processes.
 *
 *   corundum-bench [latency|oneway|ops|throughput|all] [-n COUNT] [-c CPU_A,CPU_B]
 *
 * latency:    ping-pong of 64-byte messages between two processes (round trip,
 *             and round trip / 2), against an anonymous pipe.
 * oneway:     one-way latency measured directly: the writer timestamps each
 *             packet, the reader in another process timestamps its arrival.
 * ops:        cost of the hot-path calls themselves, in one thread.
 * throughput: one process streams COUNT messages, the other consumes them.
 * The child side is this same executable started with --child.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#include "corundum/corundum.h"
#include "bench_common.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cpu_a = -1, cpu_b = -1;
static char self_path[MAX_PATH];

static void pin(int cpu) {
    if (cpu >= 0 && cpu < 64) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

#define qpc_ns bench_now_ns

/* Ping-pong round trips in ticks: prints the round trip and half of it. */
static void report_latency(const char *what, uint32_t *rtt, size_t n, double ns_per_tick) {
    char label[64];
    snprintf(label, sizeof label, "%s round trip", what);
    report_ticks(label, rtt, n, ns_per_tick);
    snprintf(label, sizeof label, "%s rtt / 2", what);
    report_ticks(label, rtt, n, ns_per_tick / 2.0); /* already sorted */
}
static void report_tp(const char *what, uint32_t size, size_t n, uint64_t ns) {
    double sec = (double)ns * 1e-9;
    printf("%-12s %5u B: %8.2f M msg/s  %7.2f GB/s\n", what, size, (double)n / sec / 1e6, (double)n * size / sec / 1e9);
}
static void die(const char *what, crd_status s) {
    fprintf(stderr, "%s: %s (GetLastError %lu)\n", what, crd_strerror(s), GetLastError());
    exit(1);
}

static HANDLE spawn(const char *args, BOOL inherit) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "\"%s\" --child %s", self_path, args);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, inherit, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        exit(1);
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

typedef struct {
    uint64_t seq;
    unsigned char pad[56];
} ping_t;

static void rw(HANDLE h, void *buf, DWORD n, int writing) {
    DWORD done = 0, k;
    while (done < n) {
        BOOL ok = writing ? WriteFile(h, (char *)buf + done, n - done, &k, NULL) : ReadFile(h, (char *)buf + done, n - done, &k, NULL);
        if (!ok || !k) { fprintf(stderr, "pipe I/O failed: %lu\n", GetLastError()); exit(1); }
        done += k;
    }
}

/* ------------------------------------------------------------------ children */

static int child_main(int argc, char **argv) {
    const char *mode = argv[2];
    pin(cpu_b);
    if (!strcmp(mode, "echo-crd")) { /* argv: echo-crd in out n */
        crd_reader in;
        crd_writer out;
        if (crd_reader_open(&in, argv[3], CRD_OPEN_PREFAULT) || crd_writer_open(&out, argv[4], CRD_OPEN_PREFAULT)) return 3;
        size_t n = strtoull(argv[5], NULL, 10);
        for (size_t i = 0; i < n; ++i) {
            crd_msg m = {0};
            while (crd_read(&in, &m) != CRD_OK) crd_cpu_relax();
            ping_t *p = NULL;
            while (crd_reserve(&out, sizeof(ping_t), (void **)&p) != CRD_OK) crd_cpu_relax();
            memcpy(p, m.data, sizeof(ping_t));
            crd_release(&in);
            crd_commit(&out, sizeof(ping_t), 1);
        }
        crd_reader_close(&in);
        crd_writer_close(&out);
        return 0;
    }
    if (!strcmp(mode, "echo-pipe")) { /* argv: echo-pipe hin hout n */
        HANDLE hin = (HANDLE)(uintptr_t)strtoull(argv[3], NULL, 10), hout = (HANDLE)(uintptr_t)strtoull(argv[4], NULL, 10);
        size_t n = strtoull(argv[5], NULL, 10);
        ping_t p;
        for (size_t i = 0; i < n; ++i) {
            rw(hin, &p, sizeof p, 0);
            rw(hout, &p, sizeof p, 1);
        }
        return 0;
    }
    if (!strcmp(mode, "oneway-src")) { /* argv: oneway-src ring total */
        crd_writer w;
        if (crd_writer_open(&w, argv[3], CRD_OPEN_PREFAULT) != CRD_OK) return 3;
        oneway_writer(&w, strtoull(argv[4], NULL, 10));
        crd_writer_close(&w);
        return 0;
    }
    if (!strcmp(mode, "src-crd")) { /* argv: src-crd ring n size */
        crd_writer w;
        if (crd_writer_open(&w, argv[3], CRD_OPEN_PREFAULT) != CRD_OK) return 3;
        size_t n = strtoull(argv[4], NULL, 10);
        uint32_t size = (uint32_t)atoi(argv[5]);
        for (size_t i = 0; i < n; ++i) {
            void *p = NULL;
            while (crd_reserve(&w, size, &p) != CRD_OK) {
                crd_publish(&w);
                crd_cpu_relax();
            }
            memset(p, (int)i, size);
            crd_stage(&w, size, 1);
            if ((i & 15) == 15) crd_publish(&w);
        }
        crd_writer_close(&w);
        return 0;
    }
    if (!strcmp(mode, "src-pipe")) { /* argv: src-pipe hout n size */
        HANDLE hout = (HANDLE)(uintptr_t)strtoull(argv[3], NULL, 10);
        size_t n = strtoull(argv[4], NULL, 10);
        uint32_t size = (uint32_t)atoi(argv[5]);
        unsigned char *buf = malloc(size);
        for (size_t i = 0; i < n; ++i) {
            memset(buf, (int)i, size);
            rw(hout, buf, size, 1);
        }
        return 0;
    }
    (void)argc;
    return 2;
}

/* ------------------------------------------------------------------ latency */

static void latency_corundum(size_t n) {
    const char *ab = "/crd-bench-ab", *ba = "/crd-bench-ba";
    crd_status s;
    if ((s = crd_create(ab, 1u << 16, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    if ((s = crd_create(ba, 1u << 16, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    const size_t warm = n / 10;
    crd_writer out;
    crd_reader in;
    if ((s = crd_writer_open(&out, ab, CRD_OPEN_PREFAULT)) != CRD_OK) die("writer_open", s);
    if ((s = crd_reader_open(&in, ba, CRD_OPEN_PREFAULT)) != CRD_OK) die("reader_open", s);
    char args[256];
    snprintf(args, sizeof args, "echo-crd %s %s %zu", ab, ba, n + warm);
    HANDLE child = spawn(args, FALSE);
    pin(cpu_a);
    uint32_t *rtt = malloc(n * sizeof *rtt);
    uint64_t t0q = qpc_ns(), t0t = ticks();
    kstats k0 = kstats_now();
    for (size_t i = 0; i < n + warm; ++i) {
        uint64_t t0 = ticks();
        ping_t *p = NULL;
        while (crd_reserve(&out, sizeof(ping_t), (void **)&p) != CRD_OK) crd_cpu_relax();
        p->seq = i;
        crd_commit(&out, sizeof(ping_t), 1);
        crd_msg m = {0};
        while (crd_read(&in, &m) != CRD_OK) crd_cpu_relax();
        if (((const ping_t *)m.data)->seq != i) die("latency", CRD_ECORRUPT);
        crd_release(&in);
        uint64_t t1 = ticks();
        if (i >= warm) rtt[i - warm] = (uint32_t)(t1 - t0);
    }
    double npt = (double)(qpc_ns() - t0q) / (double)(ticks() - t0t);
    kstats kd = kstats_diff(k0, kstats_now());
    report_latency("corundum", rtt, n, npt);
    report_kstats("pinger", kd);
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
    free(rtt);
    crd_writer_close(&out);
    crd_reader_close(&in);
    crd_unlink(ab);
    crd_unlink(ba);
}

static void latency_pipe(size_t n) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE a_r, a_w, b_r, b_w; /* parent writes a, child reads a; child writes b, parent reads b */
    if (!CreatePipe(&a_r, &a_w, &sa, 0) || !CreatePipe(&b_r, &b_w, &sa, 0)) exit(1);
    SetHandleInformation(a_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(b_r, HANDLE_FLAG_INHERIT, 0);
    const size_t warm = n / 10;
    char args[256];
    snprintf(args, sizeof args, "echo-pipe %llu %llu %zu", (unsigned long long)(uintptr_t)a_r, (unsigned long long)(uintptr_t)b_w, n + warm);
    HANDLE child = spawn(args, TRUE);
    CloseHandle(a_r);
    CloseHandle(b_w);
    pin(cpu_a);
    uint32_t *rtt = malloc(n * sizeof *rtt);
    ping_t p;
    memset(&p, 0, sizeof p);
    uint64_t t0q = qpc_ns(), t0t = ticks();
    for (size_t i = 0; i < n + warm; ++i) {
        uint64_t t0 = ticks();
        p.seq = i;
        rw(a_w, &p, sizeof p, 1);
        rw(b_r, &p, sizeof p, 0);
        uint64_t t1 = ticks();
        if (i >= warm) rtt[i - warm] = (uint32_t)(t1 - t0);
    }
    double npt = (double)(qpc_ns() - t0q) / (double)(ticks() - t0t);
    report_latency("pipe", rtt, n, npt);
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
    CloseHandle(a_w);
    CloseHandle(b_r);
    free(rtt);
}

/* ------------------------------------------------------------------- oneway */

static void oneway_corundum(size_t n) {
    const char *name = "/crd-bench-ow";
    crd_status s;
    if ((s = crd_create(name, 1u << 16, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    crd_reader r;
    if ((s = crd_reader_open(&r, name, CRD_OPEN_PREFAULT)) != CRD_OK) die("reader_open", s);
    const size_t warm = n / 10;
    char args[256];
    snprintf(args, sizeof args, "oneway-src %s %zu", name, n + warm);
    HANDLE child = spawn(args, FALSE);
    pin(cpu_a);
    oneway_reader(&r, n, warm);
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
    crd_reader_close(&r);
    crd_unlink(name);
}

/* --------------------------------------------------------------- throughput */

static void throughput_corundum(size_t n, uint32_t size) {
    const char *name = "/crd-bench-tp";
    crd_status s;
    if ((s = crd_create(name, 1u << 22, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    crd_reader r;
    if ((s = crd_reader_open(&r, name, CRD_OPEN_PREFAULT)) != CRD_OK) die("reader_open", s);
    char args[256];
    snprintf(args, sizeof args, "src-crd %s %zu %u", name, n, size);
    HANDLE child = spawn(args, FALSE);
    pin(cpu_a);
    size_t got = 0, since = 0;
    uint64_t sink = 0, t0 = 0;
    while (got < n) {
        crd_msg m = {0};
        if (crd_read(&r, &m) == CRD_OK) {
            if (got == 0) t0 = qpc_ns();
            const unsigned char *d = m.data;
            for (uint32_t off = 0; off < m.size; off += 64) sink += d[off]; /* touch every cache line */
            sink += d[m.size - 1];
            ++got;
            if (++since == 16) {
                crd_release(&r);
                since = 0;
            }
        } else {
            crd_release(&r);
            since = 0;
            crd_cpu_relax();
        }
    }
    uint64_t t1 = qpc_ns();
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
    if (sink == 42) puts("");
    report_tp("corundum", size, n, t1 - t0);
    crd_reader_close(&r);
    crd_unlink(name);
}

static void throughput_pipe(size_t n, uint32_t size) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) exit(1);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    char args[256];
    snprintf(args, sizeof args, "src-pipe %llu %zu %u", (unsigned long long)(uintptr_t)wr, n, size);
    HANDLE child = spawn(args, TRUE);
    CloseHandle(wr);
    pin(cpu_a);
    unsigned char *buf = malloc(size);
    uint64_t t0 = qpc_ns();
    for (size_t i = 0; i < n; ++i) rw(rd, buf, size, 0);
    uint64_t t1 = qpc_ns();
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
    CloseHandle(rd);
    free(buf);
    report_tp("pipe", size, n, t1 - t0);
}

int main(int argc, char **argv) {
    GetModuleFileNameA(NULL, self_path, sizeof self_path);
    const char *mode = "all";
    size_t n = 1000000;
    int child = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--child")) { child = i; break; }
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &cpu_a, &cpu_b);
        else if (argv[i][0] != '-') mode = argv[i];
        else {
            fprintf(stderr, "usage: %s [latency|oneway|ops|throughput|all] [-n COUNT] [-c CPU_A,CPU_B]\n", argv[0]);
            return 2;
        }
    }
    if (child) { /* the CPU for the child is passed through the environment */
        const char *c = getenv("CRD_BENCH_CPU");
        if (c) cpu_b = atoi(c);
        return child_main(argc - child + 1, argv + child - 1);
    }
    if (cpu_b >= 0) {
        char b[16];
        snprintf(b, sizeof b, "%d", cpu_b);
        SetEnvironmentVariableA("CRD_BENCH_CPU", b);
    }
    printf("Corundum %s benchmark (Windows), %zu messages%s\n", CRD_VERSION_STRING, n, cpu_a >= 0 ? ", pinned" : "");
    if (!strcmp(mode, "latency") || !strcmp(mode, "all")) {
        printf("\n-- latency, 64-byte ping-pong between two processes --\n");
        latency_corundum(n);
        latency_pipe(n / 4);
    }
    if (!strcmp(mode, "oneway") || !strcmp(mode, "all")) {
        printf("\n-- one-way latency, 64-byte packet, writer and reader in separate processes --\n");
        oneway_corundum(n);
    }
    if (!strcmp(mode, "ops") || !strcmp(mode, "all")) {
        printf("\n-- hot-path cost, 64-byte packet, one thread --\n");
        pin(cpu_a);
        bench_ops("/crd-bench-ops", n * 10);
    }
    if (!strcmp(mode, "throughput") || !strcmp(mode, "all")) {
        printf("\n-- throughput, one-way stream --\n");
        const uint32_t sizes[] = {16, 64, 256, 1024, 4096};
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
            size_t count = sizes[i] >= 1024 ? n * 2 : n * 10;
            throughput_corundum(count, sizes[i]);
            throughput_pipe(count / 4, sizes[i]);
        }
    }
    return 0;
}
