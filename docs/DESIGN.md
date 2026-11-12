**English** · [Українська](DESIGN.uk.md)

# Corundum HFT: design

This document describes the shared-memory layout, the protocol between writer and reader, and why it is correct on weakly ordered CPUs. Source of truth: [corundum.h](../include/corundum/corundum.h) and [corundum.c](../src/corundum.c).

## 1. Segment layout

```
offset   size   contents                                  written by
0        128    magic, version, header size, capacity,    crd_create (once)
                creation time, flags
128      128    head, notify_seq (futex word),            producer
                writer_pid, writer_closed
256      128    tail, reader_waiting, reader_pid          consumer
384      128    reserved
512      C      data region, C = capacity (power of two)  producer (records)
```

Each block is 128 bytes, not 64: Intel cores prefetch cache lines in adjacent pairs, so two hot fields 64 bytes apart can still interfere. The producer and consumer never write to the same 128-byte block.

`head` and `tail` are 64-bit byte counters that only grow. The byte offset in the data region is `pos & (C - 1)`. At 10 GB/s a 64-bit counter wraps after about 58 years, so wrap-around of the counters is not handled.

- `head - tail` = bytes in use, always `<= C`.
- empty: `head == tail`. full: not enough room between `head` and `tail + C` for the next record.

## 2. Records

```
+----------------+----------------+---------------------------+---------+
| size: uint32   | type: uint32   | payload (size bytes)      | padding |
+----------------+----------------+---------------------------+---------+
 <------------- 8 + align8(size) bytes, starts 8-byte aligned ---------->
```

- Payload is 8-byte aligned, so any type with `alignof <= 8` can be read in place.
- `type` is user-defined; `0xFFFFFFFF` is reserved for filler.
- **Records never wrap.** If a record does not fit between the current offset and the end of the buffer, the writer fills the rest with a filler record (`type = 0xFFFFFFFF`, `size = remaining - 8`) and places the record at offset 0. The remainder is always a multiple of 8 and at least 8, so the filler header always fits.
- Maximum payload is `C/2 - 8`. A record of total size `T <= C/2` that needs a filler wastes less than `T` bytes, so filler plus record never exceed `C`, and the largest record can always be written once the ring drains.

## 3. Protocol

Writer, process-local state: `head` (own copy, includes staged records), `tail_cache`.

```
reserve(n):
    T     = 8 + align8(n)
    start = head, or the next multiple of C if the record would cross the end
    if start + T - tail_cache > C:
        tail_cache = load_acquire(shm.tail)            // (A)
        if start + T - tail_cache > C: return FULL
    write filler header if start != head
    return data + (start mod C) + 8
commit(n, type):
    write size and type into the record header
    head = start + 8 + align8(n)
    store_release(shm.head, head)                     // (B)
```

Reader, process-local state: `cursor`, `head_cache`.

```
read():
    if cursor == head_cache:
        head_cache = load_acquire(shm.head)           // (C)
        if cursor == head_cache: return EMPTY
    validate and read the record header at cursor; skip a filler
    cursor += 8 + align8(size)
    return pointer to payload
release():
    store_release(shm.tail, cursor)                   // (D)
```

`crd_stage` is `commit` without (B), and `crd_publish` is (B) alone, so N records cost one release store. Likewise N `crd_read` calls followed by one `crd_release` cost one store.

## 4. Why it is correct

Two properties are needed, and each is one acquire/release pair.

**The reader never sees a half-written record.** The writer fills the payload and header with ordinary stores, then performs the release store (B). The reader's acquire load (C) that observes the new `head` synchronises with (B): every store sequenced before (B) is visible to everything sequenced after (C). So the reader reads complete headers and payloads for all records below `head`.

**The writer never overwrites a record that is still being read.** The reader's loads of a record are sequenced before its release store (D). The writer's acquire load (A) that observes the new `tail` synchronises with (D), so the writer's later stores into that space happen after the reader's loads. A release store orders earlier *loads* as well as stores, which is exactly what is needed here.

No other shared variable is involved in data transfer, so there are no read-modify-write instructions, no locks and no compare-and-swap on the hot path. On x86-64 acquire loads and release stores compile to plain `mov` instructions; on arm64 to `ldar` and `stlr`. The arm64 CI job exists because a missing barrier is invisible on x86 (which only reorders stores after loads) and shows up quickly on arm64.

**Cached positions** (`tail_cache`, `head_cache`) are only ever behind the true value. A stale `tail_cache` makes the writer think the ring is fuller than it is, so it rereads; a stale `head_cache` makes the reader think there is less data, so it rereads. Neither can cause an overrun. In steady state this means the writer touches the consumer's cache line about once per ring lap instead of once per message.

## 5. Blocking: futex wait and notify

Busy-polling gives the lowest latency but burns a core. `crd_wait` lets the reader sleep instead, with no cost to a writer whose reader is awake.

```
reader (crd_wait)                      writer (crd_notify, after publishing)
  s = load(notify_seq)                   (head already stored with release)
  store(reader_waiting, 1)               fence(seq_cst)
  fence(seq_cst)                         if load(reader_waiting):
  if head != cursor: return OK               fetch_add(notify_seq, 1)
  futex_wait(&notify_seq, s, timeout)        futex_wake(&notify_seq)
```

This is the Dekker pattern: each side stores its own flag, issues a full fence, then loads the other side's flag. With sequentially consistent fences, at least one side sees the other's store. Either the reader sees the new `head` and does not sleep, or the writer sees `reader_waiting = 1` and wakes it. If the writer bumps `notify_seq` between the reader's load of `s` and its `futex_wait`, the kernel sees `notify_seq != s` and returns immediately. No wake-up is lost; the 20 000-record sleep test in `test_ipc` checks exactly this.

When the reader is awake, `crd_notify` costs one fence (`mfence` / `lock`-prefixed instruction on x86, `dmb ish` on arm64) and one load, with no system call. The futex is a shared futex (no `FUTEX_PRIVATE_FLAG`), which the kernel keys by physical page, so it works across processes.

### Windows

The Windows backend keeps the same shared layout and the same hot path; only setup and sleeping change:

- The segment is a pagefile-backed file mapping named `Local\corundum.<name>` (the current logon session's namespace, no special privileges). Windows deletes a mapping when its last handle closes, so the creating process keeps one handle in a small fixed table (no heap) until `crd_unlink` or process exit.
- `crd_wait` / `crd_notify` use a named auto-reset event `Local\corundum.<name>.wake` instead of a futex. The Dekker handshake above is unchanged. An auto-reset event stays signalled until a wait consumes it, so a `SetEvent` that happens between the reader's check and its `WaitForSingleObject` is not lost. A stale signal only causes one extra wake-up, after which the reader re-checks and sleeps again.
- Liveness of a writer or reader slot is checked with `OpenProcess` + `WaitForSingleObject(handle, 0)`.

## 6. Lifecycle and failure handling

- **Creation is atomic for openers.** `crd_create` uses `O_CREAT | O_EXCL`, sizes the segment, fills in the header and stores `magic` last with release. An opener that sees a size below the header, or `magic == 0`, gets `CRD_ENOTREADY` and can retry. A wrong magic or version gives `CRD_EVERSION`; a capacity that does not match the segment size gives `CRD_ECORRUPT`.
- **Exclusive roles.** `writer_pid` and `reader_pid` are claimed with compare-and-swap. If the slot names a process that no longer exists (`kill(pid, 0)` fails with `ESRCH`), the slot is taken over. `CRD_OPEN_TAKEOVER` forces it, for the case of a reused pid or processes in different PID namespaces.
- **Writer crash.** Published records stay readable. Staged but unpublished records are lost. A new writer resumes from the published `head`.
- **Reader crash.** Records read but not released are delivered again to the next reader, which starts at `tail`: delivery is at-least-once across a reader crash and exactly-once otherwise.
- **Orderly close.** `crd_writer_close` publishes, sets `writer_closed` and always wakes the reader. `crd_wait` returns `CRD_CLOSED` once the ring is empty and closed; records published before the close can still be read.

## 7. Untrusted peers

The reader assumes the writer may be buggy or hostile:

- `head - cursor` must be at most `C` and a multiple of 8.
- Each header word is read exactly once with an atomic load, then validated: the record must lie inside the published range and must not cross the end of the buffer. A filler must end exactly at the end of the buffer.
- Any violation returns `CRD_ECORRUPT` without touching memory outside the mapping.

Payload bytes are not protected: the writer can change them while the reader looks at them. A reader that does not trust its writer should copy the payload out and validate the copy. Segment permissions (`mode` in `crd_create`, `0600` in the examples) decide who can attach at all.

## 8. Non-goals

- Multiple producers or multiple consumers on one ring. That requires read-modify-write operations on the hot path; use one ring per producer–consumer pair.
- Communication between machines or between 32-bit and 64-bit processes.
- Durability: the ring lives in RAM.
- Huge pages: POSIX `shm_open` segments use normal pages. For very large rings, `CRD_OPEN_PREFAULT` and `CRD_OPEN_MLOCK` remove page faults and swapping from the hot path.
