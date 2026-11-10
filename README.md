<p align="center"><img src="docs/img/banner.svg" alt="Corundum HFT by Euclase" width="100%"></p>

<p align="center">
  <a href="https://github.com/SemyonZamiakin/CorundumHFT/actions/workflows/ci.yml"><img src="https://github.com/SemyonZamiakin/CorundumHFT/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/C11%20%2F%20C%2B%2B17-2DD4BF?labelColor=0F2933" alt="C11 / C++17">
  <img src="https://img.shields.io/badge/Linux-x86--64%20%7C%20arm64-2DD4BF?labelColor=0F2933" alt="Linux x86-64 | arm64">
  <img src="https://img.shields.io/badge/license-MIT-2DD4BF?labelColor=0F2933" alt="MIT">
</p>

**English** · [Українська](README.uk.md)

# Corundum HFT

A zero-copy, lock-free ring buffer in POSIX shared memory for passing high-frequency telemetry between Linux processes, for example from a background monitoring daemon to a GUI.

Pipes and sockets copy every message into the kernel and back out, and each transfer is a system call. Corundum places a single-producer / single-consumer ring in a `shm_open` segment that both processes map. The writer builds a message directly in shared memory, the reader uses it in place, and the two coordinate through one acquire load and one release store per side. Sending and receiving makes no system calls and allocates no memory.

## Features

- **Zero-copy in both directions.** `crd_reserve` returns a pointer into the ring, you fill it, `crd_commit` publishes it. `crd_read` returns a pointer to the record in place.
- **No syscalls, locks or heap on the hot path.** The hot path is a handful of inline functions in the header. The library never calls `malloc`, not even during setup: handles are plain structs you place anywhere.
- **Variable-size records** up to half the ring capacity, 8-byte aligned, each with a 32-bit user type. A record never wraps around the end of the buffer, so it is always one contiguous block.
- **Batching on both sides:** stage several records and publish once, read several records and release once.
- **Optional blocking.** `crd_wait` sleeps on a futex while the ring is empty. `crd_notify` costs one memory fence and enters the kernel only if the reader is actually asleep.
- **Robust against the peer.** The reader validates every record header, so a buggy or hostile writer cannot make it read outside the mapping. A crashed writer's slot is reclaimed automatically, and data it published stays readable.
- **C and C++.** A C11 core, plus a C++17 header with RAII endpoints, typed writes, in-place construction (`try_emplace<T>`) and typed zero-copy reads (`msg.as<T>()`).
- **Tooling:** `corundum-inspect` shows a ring's fill level and who is attached; `corundum-bench` compares against a pipe and a Unix socket on your machine.

## Quick start

```c
#include <corundum/corundum.h>

/* daemon: create the ring once, then write */
crd_create("/telemetry", 1 << 20, 0600, CRD_CREATE_REPLACE);
crd_writer w;
crd_writer_open(&w, "/telemetry", CRD_OPEN_PREFAULT);

struct sample *s;
if (crd_reserve(&w, sizeof *s, (void **)&s) == CRD_OK) {   /* CRD_FULL: reader is behind */
    s->cpu = read_cpu();                                  /* written straight into shared memory */
    crd_commit(&w, sizeof *s, MSG_SAMPLE);
    crd_notify(&w);                                       /* only needed if the reader may sleep */
}
```

```c
/* GUI: read in place */
crd_reader r;
crd_reader_open(&r, "/telemetry", 0);
while (crd_wait(&r, -1) == CRD_OK) {          /* sleeps on a futex while empty */
    crd_msg m;
    while (crd_read(&r, &m) == CRD_OK)
        draw((const struct sample *)m.data);  /* no copy */
    crd_release(&r);                          /* hand the space back to the writer */
}
```

The same in C++:

```cpp
#include <corundum/corundum.hpp>

corundum::Writer w("/telemetry");
w.try_emplace<Sample>(MSG_SAMPLE, read_cpu(), now());   // constructed inside the ring

corundum::Reader r("/telemetry");
corundum::Message m;
while (r.try_read(m))
    if (const Sample *s = m.as<Sample>()) draw(*s);
r.release();
```

A complete daemon / viewer pair that streams per-core CPU load at 1 kHz is in [examples/](examples/).

## API at a glance

| Call | What it does | Syscalls |
|---|---|---|
| `crd_create(name, capacity, mode, flags)` | Creates a segment with an empty ring (capacity: power of two, 4 KiB to 4 GiB) | yes |
| `crd_writer_open` / `crd_reader_open` | Attach as the single producer / consumer | yes |
| `crd_reserve(w, size, &ptr)` | Room for `size` bytes, pointer into shared memory | no |
| `crd_commit(w, size, type)` | Publish the reserved record | no |
| `crd_stage` + `crd_publish` | Publish several records with one store | no |
| `crd_write(w, type, src, size)` | Copying convenience | no |
| `crd_read(r, &msg)` | Next record, in place | no |
| `crd_release(r)` | Free everything read so far | no |
| `crd_notify(w)` / `crd_wait(r, timeout)` | Optional futex wake-up / sleep | only when sleeping |
| `crd_writer_close` / `crd_reader_close` / `crd_unlink` | Detach / remove the name | yes |
| `crd_inspect(name, &info)` | Read-only snapshot for monitoring | yes |

All functions return a `crd_status`; negative values are errors and `crd_strerror` describes them. The full contract is in [corundum.h](include/corundum/corundum.h).

## Benchmarks

`corundum-bench` runs every test between two processes pinned to separate cores. Latency is a 64-byte ping-pong (one-way = round trip / 2). Throughput is a one-way stream in which the producer writes every byte and the consumer touches every cache line.

Measured on an Intel Core i9-9900K under **WSL2** (Ubuntu, kernel 6.18, GCC 15, Release build `-O3`), `corundum-bench all -n 1000000 -c 2,4`:

| One-way latency, 64 B | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| Corundum | 25–75 ns | 105 ns | 120–150 ns |
| pipe | 21.9 µs | 37.0 µs | 63.7 µs |
| Unix socket | 20.5 µs | 40.7 µs | 66.8 µs |

| Throughput | 16 B | 64 B | 256 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|---:|---:|
| Corundum, M msg/s | 188 | 170 | 84 | 21.3 | 5.7 |
| Corundum, GB/s | 3.0 | 10.9 | 21.5 | 21.9 | 23.5 |
| pipe, GB/s | 0.07 | 0.28 | 0.97 | 3.0 | 6.1 |
| Unix socket, GB/s | 0.03 | 0.11 | 0.43 | 1.6 | 3.8 |

How to read these numbers:

- WSL2 is a virtual machine. Its vCPUs do not map to fixed physical cores, so the Corundum p50 moved between 25 and 75 ns from run to run. The pipe and socket figures include the hypervisor's wake-up cost and are several times worse than on bare metal, where a pipe round trip is typically a few microseconds. Run the benchmark on your own hardware before relying on any number.
- Corundum's latency is essentially the time to move two cache lines between cores; the pipe and socket latency is dominated by waking the blocked process. Corundum's readers in this test busy-poll; a reader using `crd_wait` pays a futex wake-up when it sleeps, which is comparable to a pipe.
- CI runs the benchmark on GitHub's x86-64 and arm64 runners on every push (shared VMs, so treat their numbers as a smoke test).

## Guarantees and limits

- **One writer and one reader per ring.** Attaching a second one fails with `CRD_EBUSY`. For several consumers, create one ring per consumer.
- **Records are delivered in order, complete and exactly once** while both processes are alive. A record is visible to the reader only after it is committed in full. If the reader crashes, records it read but did not release are delivered again to the next reader.
- **The writer never blocks.** If the reader falls behind, `crd_reserve` returns `CRD_FULL` and the writer decides whether to drop, retry or buffer. This suits telemetry, where fresh data matters more than old data.
- **Shared memory is shared.** The reader validates record headers, but payload bytes stay writable by the writer. If you do not trust the writer, copy the payload before validating it. See [SECURITY.md](SECURITY.md).
- **Both processes must use the same ABI:** 64-bit Linux, GCC or Clang, and the same Corundum layout version (checked when attaching).
- **Not a persistence layer.** A segment lives in RAM (`/dev/shm`) until `crd_unlink` or reboot.

## Building

Requirements: Linux, CMake 3.16+, GCC or Clang (tested in CI with GCC 11, 13 and Clang 18; locally with GCC 15 and Clang 21).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build      # optional: headers, libcorundum.a, CMake package, corundum-inspect
```

In your project: `find_package(Corundum REQUIRED)` and `target_link_libraries(app PRIVATE Corundum::corundum)`, or add this repository with `add_subdirectory`.

## Testing

| Suite | What it covers |
|---|---|
| `test_ring` (907 checks) | Argument validation, foreign or half-initialised segments, exclusive roles and stale-owner recovery, full/empty boundaries, maximum-size records at every wrap position, staging, 400 000 random interleaved operations, corruption detection, close and timeout semantics |
| `test_ipc` | 3 million variable-size records between two processes, every byte verified; 20 000 records to a futex-sleeping reader with no lost wake-ups; a writer killed with `SIGKILL` mid-stream |
| `test_cpp` | The C++ wrapper: typed and in-place writes, typed reads, move semantics, exceptions, a producer thread with `wait` |

`scripts/check.sh` runs everything with GCC (`-Werror`) and again with Clang under AddressSanitizer and UndefinedBehaviorSanitizer. CI repeats this on x86-64 and on **arm64**, whose weaker memory model is where a missing acquire or release would show up.

## How it works

The segment is a 512-byte header followed by the data region. The producer's position, the consumer's position and the immutable metadata each sit on their own 128-byte block, so the two processes never write to the same cache line. Each side keeps a private copy of the other's position and rereads the shared one only when the ring looks full or empty, so in steady state most operations touch no shared line at all.

The full protocol, the memory-ordering argument and the futex handshake are in [docs/DESIGN.md](docs/DESIGN.md).

## Project

Corundum HFT is developed by **Euclase**. Issues and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md). Licensed under the [MIT License](LICENSE).
