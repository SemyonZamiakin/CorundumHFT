/* Binary layouts (corundum/wire.h, examples/market_data.h): the bytes a reader
 * sees in shared memory are exactly the documented layout. */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "corundum/corundum.h"
#include "corundum/wire.h"
#include "../examples/market_data.h"
#include "../examples/telemetry.h"
#include "check.h"

#include <string.h>

static char NAME[64];

/* telemetry.h is a wire format too: check it with the same macros */
CRD_WIRE_SIZE(telemetry_sample, 152);
CRD_WIRE_PACKED(telemetry_sample, seq, mono_ns, cpu_count, mem_used_permille, cpu_busy_permille);

/* little-endian helpers that do not depend on the struct definitions */
static void put(unsigned char *b, size_t off, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) b[off + i] = (unsigned char)(v >> (8 * i));
}

static md_quote sample_quote(void) {
    md_quote q;
    memset(&q, 0, sizeof q);
    q.ts_ns = 0x1122334455667788ull;
    q.bid_px = -2; /* negative: two's complement on the wire */
    q.ask_px = 4210075000000;
    q.bid_qty = 0xA1A2A3A4u;
    q.ask_qty = 5;
    q.instrument = 0x01020304u;
    q.seq = 0xDEADBEEFu;
    q.venue = 0xCAFE;
    q.flags = 0x5A;
    return q;
}

/* the layout spelled out byte by byte, independently of md_quote */
static void expected_quote(unsigned char b[48]) {
    memset(b, 0, 48);
    put(b, 0, 0x1122334455667788ull, 8);
    put(b, 8, (uint64_t)-2, 8);
    put(b, 16, 4210075000000ull, 8);
    put(b, 24, 0xA1A2A3A4u, 4);
    put(b, 28, 5, 4);
    put(b, 32, 0x01020304u, 4);
    put(b, 36, 0xDEADBEEFu, 4);
    put(b, 40, 0xCAFE, 2);
    put(b, 42, 0x5A, 1);
}

static void test_byte_image(void) {
    unsigned char want[48];
    expected_quote(want);
    md_quote q = sample_quote();
    CHECK_EQ(sizeof q, 48u);
    CHECK(memcmp(&q, want, 48) == 0);
    CHECK_EQ(want[0], 0x88); /* least significant byte first */
    CHECK_EQ(want[36], 0xEF);

    md_trade t;
    memset(&t, 0xFF, sizeof t);
    t.side = 'S';
    CHECK_EQ(((const unsigned char *)&t)[34], 'S');
    CHECK_EQ(sizeof(md_trade), 40u);
    CHECK_EQ(sizeof(md_heartbeat), 16u);
}

/* Through the ring: written in place, read in place, compared with the image. */
static void test_through_ring(void) {
    CHECK_EQ(crd_create(NAME, 4096, 0600, CRD_CREATE_REPLACE), CRD_OK);
    crd_writer w;
    crd_reader r;
    CHECK_EQ(crd_writer_open(&w, NAME, 0), CRD_OK);
    CHECK_EQ(crd_reader_open(&r, NAME, 0), CRD_OK);
    unsigned char want[48];
    expected_quote(want);

    /* many rounds, so records land at every offset including the wrap point */
    int bad = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        md_quote *q;
        CHECK_EQ(crd_reserve(&w, sizeof *q, (void **)&q), CRD_OK);
        *q = sample_quote();
        CHECK_EQ(crd_commit(&w, sizeof *q, MD_QUOTE), CRD_OK);
        md_heartbeat hb = {i, i, 9};
        CHECK_EQ(crd_write(&w, MD_HEARTBEAT, &hb, sizeof hb), CRD_OK);

        crd_msg m = {0};
        CHECK_EQ(crd_read(&r, &m), CRD_OK);
        const md_quote *in = CRD_MSG_AS(md_quote, &m);
        if (!in || m.type != MD_QUOTE || memcmp(m.data, want, 48) != 0 || in->seq != 0xDEADBEEFu) ++bad;
        if (CRD_MSG_AS(md_trade, &m) != NULL) ++bad; /* wrong size: refused */
        CHECK_EQ(crd_read(&r, &m), CRD_OK);
        const md_heartbeat *h = CRD_MSG_AS(md_heartbeat, &m);
        if (!h || h->seq != i || h->producer != 9 || ((uintptr_t)h & 7u)) ++bad;
        crd_release(&r);
    }
    CHECK_EQ(bad, 0);
    crd_writer_close(&w);
    crd_reader_close(&r);
    crd_unlink(NAME);
}

int main(void) {
    crd_test_name(NAME, sizeof NAME, "wire");
    test_byte_image();
    test_through_ring();
    return crd_test_report("test_wire");
}
