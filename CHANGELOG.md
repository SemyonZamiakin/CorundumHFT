# Changelog

## 1.1.0 (2026-11-12)

- **Windows support.** Same API and the same inline hot path; the platform layer uses a named pagefile-backed file mapping (`Local\corundum.<name>`) and a named auto-reset event for `crd_wait` / `crd_notify`. Builds with MinGW-w64 GCC (and Clang).
- `corundum-bench` for Windows, comparing against an anonymous pipe. Median one-way latency 74 ns vs 6.7 µs on an i9-9900K.
- The test suite runs on Windows too (child processes are started with `CreateProcess` instead of `fork`); CI gained a Windows job.
- `crd_writer` / `crd_reader` have a new process-local field (`os_event`). The shared layout is unchanged (`CRD_LAYOUT_VERSION` stays 1), so 1.0 and 1.1 processes on Linux interoperate.
- Used by Euclase Monitor 1.1 (publishes metrics) and Euclase Light 1.1 (reads them).

## 1.0.0 (2026-11-10)

First release.

- Single-producer / single-consumer ring buffer in a POSIX shared-memory segment, with variable-size 8-byte-aligned records that never wrap.
- Zero-copy write (`crd_reserve` / `crd_commit`) and read (`crd_read` / `crd_release`), batched publishing (`crd_stage` / `crd_publish`).
- Optional futex-based blocking (`crd_wait` / `crd_notify`) without cost to an awake reader.
- Validation of all shared state on the reader side; exclusive writer and reader roles with stale-owner recovery; orderly close.
- C++17 wrapper with RAII endpoints, typed writes, in-place construction and typed reads.
- `corundum-inspect` tool, `corundum-bench` benchmark against pipe and Unix socket, telemetry daemon and viewer example.
- Tests: unit, cross-process stress, futex wake-up, writer crash, C++ wrapper; CI on x86-64 and arm64 with sanitizers.
