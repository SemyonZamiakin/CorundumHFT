/*
 * corundum-bench: latency and throughput of Corundum against a pipe and a
 * Unix-domain socketpair, between two processes.
 *
 *   corundum-bench [latency|oneway|ops|throughput|all] [-n COUNT] [-c CPU_A,CPU_B]
 *
 * latency:    ping-pong of 64-byte messages between two processes (round trip,
 *             and round trip / 2), against a pipe and a Unix socket.
 * oneway:     one-way latency measured directly: the writer timestamps each
 *             packet, the reader in another process timestamps its arrival.
 * ops:        cost of the hot-path calls themselves, in one thread.
 * throughput: one process streams COUNT messages, the other consumes them.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#define _GNU_SOURCE
#include "corundum/corundum.h"
#include "bench_common.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int cpu_a = -1, cpu_b = -1;

static void pin(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) perror("sched_setaffinity");
}

#define now_ns bench_now_ns
#define die bench_die

/* Ping-pong round trips in ticks: prints the round trip and half of it. */
static void report_latency(const char *what, uint32_t *rtt, size_t n, double ns_per_tick) {
    char label[64];
    snprintf(label, sizeof label, "%s round trip", what);
    report_ticks(label, rtt, n, ns_per_tick);
    snprintf(label, sizeof label, "%s rtt / 2", what);
    report_ticks(label, rtt, n, ns_per_tick / 2.0); /* already sorted */
}

/* ------------------------------------------------------------------ latency */

typedef struct {
    uint64_t seq;
    unsigned char pad[56];
} ping_t; /* 64 bytes */

static void latency_corundum(size_t n) {
    const char *ab = "/crd-bench-ab", *ba = "/crd-bench-ba";
    crd_status s;
    if ((s = crd_create(ab, 1u << 16, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    if ((s = crd_create(ba, 1u << 16, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    const size_t warm = n / 10;
    pid_t child = fork();
    if (child == 0) { /* echo side */
        pin(cpu_b);
        crd_reader in;
        crd_writer out;
        if (crd_reader_open(&in, ab, CRD_OPEN_PREFAULT) || crd_writer_open(&out, ba, CRD_OPEN_PREFAULT)) _exit(1);
        for (size_t i = 0; i < n + warm; ++i) {
            crd_msg m = {0};
            while (crd_read(&in, &m) != CRD_OK) crd_cpu_relax();
            ping_t *p;
            while (crd_reserve(&out, sizeof(ping_t), (void **)&p) != CRD_OK) crd_cpu_relax();
            memcpy(p, m.data, sizeof(ping_t));
            crd_release(&in);
            crd_commit(&out, sizeof(ping_t), 1);
        }
        crd_reader_close(&in);
        crd_writer_close(&out);
        _exit(0);
    }
    pin(cpu_a);
    crd_writer out;
    crd_reader in;
    if ((s = crd_writer_open(&out, ab, CRD_OPEN_PREFAULT)) != CRD_OK) die("writer_open", s);
    while ((s = crd_reader_open(&in, ba, CRD_OPEN_PREFAULT)) == CRD_EBUSY) sched_yield();
    if (s != CRD_OK) die("reader_open", s);
    uint32_t *rtt = malloc(n * sizeof *rtt);
    calib cal = calib_start();
    kstats k0 = kstats_now();
    for (size_t i = 0; i < n + warm; ++i) {
        uint64_t t0 = ticks();
        ping_t *p;
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
    kstats kd = kstats_diff(k0, kstats_now());
    report_latency("corundum", rtt, n, calib_ns_per_tick(cal));
    report_kstats("pinger", kd);
    waitpid(child, NULL, 0);
    free(rtt);
    crd_writer_close(&out);
    crd_reader_close(&in);
    crd_unlink(ab);
    crd_unlink(ba);
}

static void xfer(int fd, void *buf, size_t n, int writing) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = writing ? write(fd, (char *)buf + done, n - done) : read(fd, (char *)buf + done, n - done);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            perror("xfer");
            exit(1);
        }
        done += (size_t)r;
    }
}

/* kind 0: two pipes, kind 1: socketpair */
static void latency_fd(size_t n, int kind) {
    int a[2], b[2];
    if (kind == 0) {
        if (pipe(a) || pipe(b)) exit(1);
    } else {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, a)) exit(1);
        b[0] = a[1];
        b[1] = a[0];
    }
    /* parent writes a[1], reads b[0]; child reads a[0], writes b[1] */
    const size_t warm = n / 10;
    pid_t child = fork();
    if (child == 0) {
        pin(cpu_b);
        ping_t p;
        for (size_t i = 0; i < n + warm; ++i) {
            xfer(a[0], &p, sizeof p, 0);
            xfer(b[1], &p, sizeof p, 1);
        }
        _exit(0);
    }
    pin(cpu_a);
    uint32_t *rtt = malloc(n * sizeof *rtt);
    ping_t p;
    memset(&p, 0, sizeof p);
    calib cal = calib_start();
    for (size_t i = 0; i < n + warm; ++i) {
        uint64_t t0 = ticks();
        p.seq = i;
        xfer(a[1], &p, sizeof p, 1);
        xfer(b[0], &p, sizeof p, 0);
        uint64_t t1 = ticks();
        if (i >= warm) rtt[i - warm] = (uint32_t)(t1 - t0);
    }
    report_latency(kind == 0 ? "pipe" : "unix socket", rtt, n, calib_ns_per_tick(cal));
    waitpid(child, NULL, 0);
    free(rtt);
    close(a[0]);
    close(a[1]);
    if (kind == 0) {
        close(b[0]);
        close(b[1]);
    }
}

/* ------------------------------------------------------------------- oneway */

static void oneway_corundum(size_t n) {
    const char *name = "/crd-bench-ow";
    crd_status s;
    if ((s = crd_create(name, 1u << 16, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    crd_reader r;
    if ((s = crd_reader_open(&r, name, CRD_OPEN_PREFAULT)) != CRD_OK) die("reader_open", s);
    const size_t warm = n / 10;
    pid_t child = fork();
    if (child == 0) {
        pin(cpu_b);
        crd_writer w;
        if (crd_writer_open(&w, name, CRD_OPEN_PREFAULT) != CRD_OK) _exit(1);
        oneway_writer(&w, n + warm);
        crd_writer_close(&w);
        _exit(0);
    }
    pin(cpu_a);
    oneway_reader(&r, n, warm);
    waitpid(child, NULL, 0);
    crd_reader_close(&r);
    crd_unlink(name);
}

/* --------------------------------------------------------------- throughput */

static void report_tp(const char *what, uint32_t size, size_t n, uint64_t ns) {
    double sec = (double)ns * 1e-9;
    printf("%-12s %5u B: %8.2f M msg/s  %7.2f GB/s\n", what, size, (double)n / sec / 1e6,
           (double)n * size / sec / 1e9);
}

static void throughput_corundum(size_t n, uint32_t size) {
    const char *name = "/crd-bench-tp";
    crd_status s;
    if ((s = crd_create(name, 1u << 22, 0600, CRD_CREATE_REPLACE)) != CRD_OK) die("create", s);
    crd_reader r;
    if ((s = crd_reader_open(&r, name, CRD_OPEN_PREFAULT)) != CRD_OK) die("reader_open", s);
    pid_t child = fork();
    if (child == 0) {
        pin(cpu_b);
        crd_writer w;
        if (crd_writer_open(&w, name, CRD_OPEN_PREFAULT) != CRD_OK) _exit(1);
        for (size_t i = 0; i < n; ++i) {
            void *p = NULL;
            while (crd_reserve(&w, size, &p) != CRD_OK) {
                crd_publish(&w);
                crd_cpu_relax();
            }
            memset(p, (int)i, size); /* touch every byte, as a real producer would */
            crd_stage(&w, size, 1);
            if ((i & 15) == 15) crd_publish(&w);
        }
        crd_writer_close(&w);
        _exit(0);
    }
    pin(cpu_a);
    size_t got = 0, since = 0;
    uint64_t sink = 0, t0 = 0;
    while (got < n) {
        crd_msg m = {0};
        if (crd_read(&r, &m) == CRD_OK) {
            if (got == 0) t0 = now_ns();
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
    uint64_t t1 = now_ns();
    waitpid(child, NULL, 0);
    if (sink == 42) puts("");
    report_tp("corundum", size, n, t1 - t0);
    crd_reader_close(&r);
    crd_unlink(name);
}

static void throughput_fd(size_t n, uint32_t size, int kind) {
    int fds[2];
    if (kind == 0 ? pipe(fds) : socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) exit(1);
    pid_t child = fork();
    if (child == 0) {
        pin(cpu_b);
        unsigned char *buf = malloc(size);
        for (size_t i = 0; i < n; ++i) {
            memset(buf, (int)i, size);
            xfer(fds[1], buf, size, 1);
        }
        _exit(0);
    }
    pin(cpu_a);
    unsigned char *buf = malloc(size);
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n; ++i) xfer(fds[0], buf, size, 0);
    uint64_t t1 = now_ns();
    waitpid(child, NULL, 0);
    report_tp(kind == 0 ? "pipe" : "unix socket", size, n, t1 - t0);
    free(buf);
    close(fds[0]);
    close(fds[1]);
}

int main(int argc, char **argv) {
    const char *mode = "all";
    size_t n = 1000000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &cpu_a, &cpu_b);
        else if (argv[i][0] != '-') mode = argv[i];
        else {
            fprintf(stderr, "usage: %s [latency|oneway|ops|throughput|all] [-n COUNT] [-c CPU_A,CPU_B]\n", argv[0]);
            return 2;
        }
    }
    printf("Corundum %s benchmark, %zu messages%s\n", CRD_VERSION_STRING, n, cpu_a >= 0 ? ", pinned" : "");
    if (!strcmp(mode, "latency") || !strcmp(mode, "all")) {
        printf("\n-- latency, 64-byte ping-pong between two processes --\n");
        latency_corundum(n);
        latency_fd(n / 4, 0);
        latency_fd(n / 4, 1);
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
            throughput_fd(count / 4, sizes[i], 0);
            throughput_fd(count / 4, sizes[i], 1);
        }
    }
    return 0;
}
