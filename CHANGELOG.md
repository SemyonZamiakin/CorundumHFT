# Changelog

## 1.2.0 (2026-11-16)

Additions only: the API, the hot path and the shared layout are unchanged (`CRD_LAYOUT_VERSION` stays 1), so 1.0, 1.1 and 1.2 processes interoperate.

- **Benchmarks.** `corundum-bench` has two new modes. `oneway` measures one-way latency between processes directly from cycle-counter timestamps (and reports samples that would reveal unsynchronised counters). `ops` measures the cost of the hot-path calls in one thread. The ping-pong prints the round trip as well as round trip / 2, and every hot loop reports the thread's context switches (Linux: voluntary and involuntary from `getrusage`; Windows: the thread's context-switch counter) plus kernel and user time. The shared parts of the Linux and Windows drivers moved to `bench/bench_common.h`. On an i9-9900K under Windows 11: round trip / 2 76 ns p50, one-way 93–97 ns p50, 8 ns to write and 4.5 ns to read a 64-byte packet. Raw output in `docs/bench/`.
- **New header `corundum/wire.h`** for binary message layouts read in place: `CRD_WIRE_PACKED` (no padding, natural alignment, alignment <= 8, and in C++ trivially copyable + standard-layout), `CRD_WIRE_SIZE`, `CRD_WIRE_OFFSET`, `CRD_MSG_AS`. C11 and C++11+, compile-time only. Guide in `docs/WIRE.md` (and `docs/WIRE.uk.md`); example layouts in `examples/market_data.h`, demo in `examples/wire_demo.c` (Linux and Windows).
- `corundum::Message::native()` returns the underlying `crd_msg`.
- **New tests:** `test_layout` (cache-line separation of the header at compile and run time, the header bytes each side's hot path writes, overflow at every boundary for 133 record sizes, 64-bit positions across the 2^64 wrap), `test_concurrency` (producer and consumer threads on one mapping, so ThreadSanitizer can verify the acquire/release pairing; checked by hand to fail when any of the four orderings is weakened to relaxed), `test_wire` and `test_wire_cpp` (byte-exact layouts through the ring), and `wire_compile_fail_*` (seven layouts that must be rejected at compile time, one control that must compile).
- `scripts/check.sh` runs the whole CTest suite in both builds and adds a ThreadSanitizer build for the threaded tests (run with ASLR disabled through `setarch -R`, which newer kernels need for TSan).
- The examples' `telemetry_sample` is checked with the new macros (152 bytes, no padding).

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
