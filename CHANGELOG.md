# Changelog

## 1.0.0 (2026-11-10)

First release.

- Single-producer / single-consumer ring buffer in a POSIX shared-memory segment, with variable-size 8-byte-aligned records that never wrap.
- Zero-copy write (`crd_reserve` / `crd_commit`) and read (`crd_read` / `crd_release`), batched publishing (`crd_stage` / `crd_publish`).
- Optional futex-based blocking (`crd_wait` / `crd_notify`) without cost to an awake reader.
- Validation of all shared state on the reader side; exclusive writer and reader roles with stale-owner recovery; orderly close.
- C++17 wrapper with RAII endpoints, typed writes, in-place construction and typed reads.
- `corundum-inspect` tool, `corundum-bench` benchmark against pipe and Unix socket, telemetry daemon and viewer example.
- Tests: unit, cross-process stress, futex wake-up, writer crash, C++ wrapper; CI on x86-64 and arm64 with sanitizers.
