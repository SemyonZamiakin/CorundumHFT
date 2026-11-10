/*
 * Background daemon side: samples per-core CPU load and memory use from /proc
 * and publishes one record per sample, built in place inside shared memory.
 *
 *   telemetry_daemon [rate_hz]     (default 1000; Ctrl+C to stop)
 */
#define _GNU_SOURCE
#include "corundum/corundum.h"
#include "telemetry.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop = 0;
static void on_signal(int sig) { (void)sig; stop = 1; }

typedef struct { unsigned long long busy, total; } cpu_times;

static int read_cpus(cpu_times *t, int max) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        if (strncmp(line, "cpu", 3) || line[3] == ' ') continue; /* skip the aggregate line */
        unsigned long long v[8] = {0};
        if (sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                   &v[6], &v[7]) < 4)
            continue;
        unsigned long long idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; ++i) total += v[i];
        t[n].busy = total - idle;
        t[n].total = total;
        ++n;
    }
    fclose(f);
    return n;
}

static uint32_t mem_used_permille(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    unsigned long long total = 0, avail = 0, v;
    char key[64];
    while (fscanf(f, "%63s %llu kB\n", key, &v) == 2) {
        if (!strcmp(key, "MemTotal:")) total = v;
        else if (!strcmp(key, "MemAvailable:")) avail = v;
    }
    fclose(f);
    return total ? (uint32_t)((total - avail) * 1000 / total) : 0;
}

int main(int argc, char **argv) {
    const long rate = argc > 1 ? atol(argv[1]) : 1000;
    if (rate <= 0) return 2;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    crd_status s = crd_create(TELEMETRY_RING, 1u << 20, 0600, CRD_CREATE_REPLACE);
    crd_writer w;
    if (s == CRD_OK) s = crd_writer_open(&w, TELEMETRY_RING, CRD_OPEN_PREFAULT);
    if (s != CRD_OK) {
        fprintf(stderr, "telemetry_daemon: %s\n", crd_strerror(s));
        return 1;
    }
    printf("publishing to %s at %ld Hz, start telemetry_viewer in another terminal\n", TELEMETRY_RING, rate);

    cpu_times prev[TELEMETRY_MAX_CPUS], cur[TELEMETRY_MAX_CPUS];
    int ncpu = read_cpus(prev, TELEMETRY_MAX_CPUS);
    uint64_t seq = 0, dropped = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!stop) {
        next.tv_nsec += 1000000000L / rate;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        int n = read_cpus(cur, TELEMETRY_MAX_CPUS);
        telemetry_sample *smp;
        /* Telemetry policy: if the viewer falls behind, drop the sample instead of blocking. */
        if (crd_reserve(&w, sizeof *smp, (void **)&smp) != CRD_OK) {
            ++dropped;
            continue;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        smp->seq = seq++;
        smp->mono_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
        smp->cpu_count = (uint32_t)(n < ncpu ? n : ncpu);
        smp->mem_used_permille = mem_used_permille();
        for (uint32_t i = 0; i < smp->cpu_count; ++i) {
            unsigned long long dt = cur[i].total - prev[i].total, db = cur[i].busy - prev[i].busy;
            smp->cpu_busy_permille[i] = (uint16_t)(dt ? db * 1000 / dt : 0);
            prev[i] = cur[i];
        }
        crd_commit(&w, sizeof *smp, MSG_SAMPLE);
        crd_notify(&w);
    }
    printf("\nstopped after %llu samples, %llu dropped\n", (unsigned long long)seq, (unsigned long long)dropped);
    crd_writer_close(&w);
    crd_unlink(TELEMETRY_RING);
    return 0;
}
