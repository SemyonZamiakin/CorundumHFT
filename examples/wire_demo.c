/*
 * wire_demo: writes checked binary records (examples/market_data.h) into a
 * ring and reads them back in place, then prints the bytes of one record
 * exactly as they sit in shared memory. Linux and Windows.
 *
 * In a real deployment the writer and the reader are separate processes
 * (a feed handler and a strategy, a daemon and a dashboard); here both ends
 * live in one process to keep the example short.
 */
#include "market_data.h"

#include <corundum/corundum.h>
#include <stdio.h>
#include <string.h>

#define RING "/crd-wire-demo"

static int publish_quote(crd_writer *w, uint32_t seq) {
    md_quote *q;
    if (crd_reserve(w, sizeof *q, (void **)&q) != CRD_OK) return 0;
    /* fill every field in place, including the reserved bytes */
    q->ts_ns = 1700000000000000000ull + seq;
    q->bid_px = 4210050000000; /* 42100.50 */
    q->ask_px = 4210075000000; /* 42100.75 */
    q->bid_qty = 3;
    q->ask_qty = 5;
    q->instrument = 7;
    q->seq = seq;
    q->venue = 2;
    q->flags = 0;
    memset(q->reserved, 0, sizeof q->reserved);
    return crd_commit(w, sizeof *q, MD_QUOTE) == CRD_OK;
}

static int publish_trade(crd_writer *w, uint64_t id) {
    md_trade t;
    memset(&t, 0, sizeof t);
    t.ts_ns = 1700000000000000000ull + id;
    t.px = 4210060000000;
    t.trade_id = id;
    t.qty = 1;
    t.instrument = 7;
    t.venue = 2;
    t.side = 'B';
    return crd_write(w, MD_TRADE, &t, sizeof t) == CRD_OK; /* copying variant */
}

int main(void) {
    crd_status s = crd_create(RING, 1 << 16, 0600, CRD_CREATE_REPLACE);
    crd_writer w;
    crd_reader r;
    if (s != CRD_OK || (s = crd_writer_open(&w, RING, 0)) != CRD_OK || (s = crd_reader_open(&r, RING, 0)) != CRD_OK) {
        fprintf(stderr, "setup: %s\n", crd_strerror(s));
        return 1;
    }
    int ok = 1;
    for (uint32_t i = 0; i < 4; ++i) ok &= publish_quote(&w, i) && publish_trade(&w, 100 + i);

    crd_msg m = {0};
    unsigned quotes = 0, trades = 0;
    while (crd_read(&r, &m) == CRD_OK) {
        if (m.type == MD_QUOTE) {
            const md_quote *q = CRD_MSG_AS(md_quote, &m); /* NULL if the size is wrong */
            if (!q || q->seq != quotes) ok = 0;
            if (quotes == 0) {
                printf("first md_quote, %u bytes straight from shared memory:\n", m.size);
                const unsigned char *b = m.data;
                for (uint32_t i = 0; i < m.size; ++i) printf("%02x%s", b[i], (i % 16 == 15) ? "\n" : " ");
                printf("bid %lld.%08lld  ask %lld.%08lld  seq %u\n", (long long)(q->bid_px / MD_PRICE_SCALE),
                       (long long)(q->bid_px % MD_PRICE_SCALE), (long long)(q->ask_px / MD_PRICE_SCALE),
                       (long long)(q->ask_px % MD_PRICE_SCALE), q->seq);
            }
            ++quotes;
        } else if (m.type == MD_TRADE) {
            const md_trade *t = CRD_MSG_AS(md_trade, &m);
            if (!t || t->trade_id != 100 + trades) ok = 0;
            ++trades;
        }
    }
    crd_release(&r);
    printf("read %u quotes and %u trades in place: %s\n", quotes, trades, ok && quotes == 4 && trades == 4 ? "ok" : "MISMATCH");
    crd_writer_close(&w);
    crd_reader_close(&r);
    crd_unlink(RING);
    return ok && quotes == 4 && trades == 4 ? 0 : 1;
}
