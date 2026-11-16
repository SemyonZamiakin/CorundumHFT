# Binary message layouts

Corundum hands the reader a pointer to the record in shared memory. The fastest way to use it is to treat that pointer as a struct: no parsing, no copying, no allocation. That only works if the writer and every reader (a trading strategy, a telemetry dashboard, a recorder) agree on the struct's layout byte for byte. [`corundum/wire.h`](../include/corundum/wire.h) turns that agreement into compile-time checks.

[Українська](WIRE.uk.md)

## The checks

```c
#include <corundum/wire.h>

typedef struct md_quote {
    uint64_t ts_ns;      /*  0  exchange timestamp */
    int64_t  bid_px;     /*  8  fixed point, 1e-8 */
    int64_t  ask_px;     /* 16 */
    uint32_t bid_qty;    /* 24 */
    uint32_t ask_qty;    /* 28 */
    uint32_t instrument; /* 32 */
    uint32_t seq;        /* 36 */
    uint16_t venue;      /* 40 */
    uint8_t  flags;      /* 42 */
    uint8_t  reserved[5];/* 43  zero */
} md_quote;

CRD_WIRE_SIZE(md_quote, 48);
CRD_WIRE_PACKED(md_quote, ts_ns, bid_px, ask_px, bid_qty, ask_qty,
                instrument, seq, venue, flags, reserved);
CRD_WIRE_OFFSET(md_quote, seq, 36);
```

| Macro | Fails to compile when |
|---|---|
| `CRD_WIRE_SIZE(T, n)` | `sizeof(T) != n` |
| `CRD_WIRE_OFFSET(T, f, off)` | field `f` does not start at byte `off` |
| `CRD_WIRE_PACKED(T, f1, ..., fN)` | the listed fields' sizes do not add up to `sizeof(T)` (a padding hole, tail padding, or a field missing from the list); a field is not naturally aligned (only possible with `packed` attributes or pragmas); `alignof(T) > 8`; in C++ also when `T` is not trivially copyable and standard-layout |
| `CRD_MSG_AS(T, &msg)` | (run time) returns `const T *` into shared memory if `msg.size == sizeof(T)`, otherwise `NULL` |

`CRD_WIRE_PACKED` takes up to 24 fields. An array counts as one field with its full size; a nested struct counts as one field and should have its own `CRD_WIRE_PACKED`.

The header compiles only for little-endian targets (all supported ones: x86-64 and arm64 on Linux and Windows), so a byte dump of a record means the same on every supported machine.

All checks are `_Static_assert` / `static_assert`: they generate no code. The test suite builds eight small programs that each break one rule and verifies that seven of them are rejected and the control case is accepted (`wire_compile_fail_*` in CTest).

## Rules for a layout that never needs padding

1. **Fixed-width types only:** `uint64_t`, `int32_t`, `uint8_t`... Never `int`, `long`, `size_t`, `bool`, enums or pointers: their size or meaning differs between compilers and processes.
2. **Order fields from 8-byte down to 1-byte.** Then every field is naturally aligned and the compiler inserts no padding, without `__attribute__((packed))` (which would force slower unaligned access and is rejected by `CRD_WIRE_PACKED` if it leaves a field misaligned).
3. **Make the size a multiple of 8** with explicit `reserved` bytes, and have the writer zero them. Ring records are 8-byte aligned, so this wastes nothing, and the reserved bytes are room for later fields.
4. **Prices as fixed-point integers** (`int64_t` scaled by 1e8, for example), not `double`: exact, comparable with `==`, and identical bit patterns on every machine.
5. **Put the record kind in the ring's 32-bit `type`**, not in the payload: the reader can dispatch before it touches the payload.
6. **Never change a published layout.** Add a new record type (or use reserved bytes, with a zero meaning "absent") and keep the old one until every reader is updated.

## Reading in place

```c
crd_msg m;
while (crd_read(&r, &m) == CRD_OK) {
    if (m.type == MD_QUOTE) {
        const md_quote *q = CRD_MSG_AS(md_quote, &m);   /* NULL if the size is wrong */
        if (q) on_quote(q);                            /* reads straight from shared memory */
    }
}
crd_release(&r);
```

In C++, `corundum::Message::as<T>()` does the same, and `Writer::try_emplace<T>(type, args...)` constructs `T` directly inside the ring.

The pointer stays valid until `crd_release`. The payload lives in memory the writer can also write. If the writer is not trusted, copy the record out before you validate it (see [SECURITY.md](../SECURITY.md)).

## Example

[`examples/market_data.h`](../examples/market_data.h) defines quote, trade and heartbeat records with all checks. [`examples/wire_demo.c`](../examples/wire_demo.c) writes them into a ring, reads them back in place, and prints one record's bytes as they sit in shared memory:

```
first md_quote, 48 bytes straight from shared memory:
00 00 2a 36 fe 9c 97 17 80 e4 88 3a d4 03 00 00
c0 5c 06 3c d4 03 00 00 03 00 00 00 05 00 00 00
07 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
bid 42100.50000000  ask 42100.75000000  seq 0
read 4 quotes and 4 trades in place: ok
```

`tests/test_wire.c` builds the expected 48 bytes of a quote by hand, independently of the struct definition, and compares them with what the reader sees in the ring at every offset of the buffer, including the wrap point.
