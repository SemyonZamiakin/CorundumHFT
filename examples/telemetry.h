/* Wire format shared by telemetry_daemon (C) and telemetry_viewer (C++). */
#ifndef CRD_EXAMPLE_TELEMETRY_H
#define CRD_EXAMPLE_TELEMETRY_H

#include <stdint.h>

#define TELEMETRY_RING "/euclase-telemetry"
#define TELEMETRY_MAX_CPUS 64

enum { MSG_SAMPLE = 1 };

typedef struct telemetry_sample {
    uint64_t seq;
    uint64_t mono_ns;            /* CLOCK_MONOTONIC when sampled */
    uint32_t cpu_count;
    uint32_t mem_used_permille;  /* of MemTotal */
    uint16_t cpu_busy_permille[TELEMETRY_MAX_CPUS];
} telemetry_sample;

#endif
