/*
 * Example wire format for market data and telemetry records, checked with
 * corundum/wire.h. Shared by examples/wire_demo.c and the tests.
 *
 * Rules these layouts follow (see docs/WIRE.md):
 *   - fixed-width integer types only; prices as fixed-point integers
 *   - fields ordered from 8-byte down to 1-byte, so there is no padding and
 *     no packed attribute is needed
 *   - explicit reserved bytes instead of implicit padding, zeroed by the writer
 *   - sizes a multiple of 8, so consecutive records waste no space in the ring
 */
#ifndef CRD_EXAMPLE_MARKET_DATA_H
#define CRD_EXAMPLE_MARKET_DATA_H

#include <corundum/wire.h>
#include <stdint.h>

/* record types (the 32-bit `type` of each ring record) */
enum { MD_QUOTE = 1, MD_TRADE = 2, MD_HEARTBEAT = 3 };

#define MD_PRICE_SCALE 100000000 /* price = value / 1e8 */

typedef struct md_quote {
    uint64_t ts_ns;      /*  0  exchange timestamp */
    int64_t bid_px;      /*  8  fixed point, MD_PRICE_SCALE */
    int64_t ask_px;      /* 16 */
    uint32_t bid_qty;    /* 24 */
    uint32_t ask_qty;    /* 28 */
    uint32_t instrument; /* 32 */
    uint32_t seq;        /* 36 */
    uint16_t venue;      /* 40 */
    uint8_t flags;       /* 42 */
    uint8_t reserved[5]; /* 43  zero */
} md_quote;
CRD_WIRE_SIZE(md_quote, 48);
CRD_WIRE_PACKED(md_quote, ts_ns, bid_px, ask_px, bid_qty, ask_qty, instrument, seq, venue, flags, reserved);
CRD_WIRE_OFFSET(md_quote, bid_px, 8);
CRD_WIRE_OFFSET(md_quote, ask_px, 16);
CRD_WIRE_OFFSET(md_quote, bid_qty, 24);
CRD_WIRE_OFFSET(md_quote, instrument, 32);
CRD_WIRE_OFFSET(md_quote, seq, 36);
CRD_WIRE_OFFSET(md_quote, venue, 40);
CRD_WIRE_OFFSET(md_quote, flags, 42);

typedef struct md_trade {
    uint64_t ts_ns;      /*  0 */
    int64_t px;          /*  8  fixed point, MD_PRICE_SCALE */
    uint64_t trade_id;   /* 16 */
    uint32_t qty;        /* 24 */
    uint32_t instrument; /* 28 */
    uint16_t venue;      /* 32 */
    uint8_t side;        /* 34  'B' or 'S' */
    uint8_t reserved[5]; /* 35  zero */
} md_trade;
CRD_WIRE_SIZE(md_trade, 40);
CRD_WIRE_PACKED(md_trade, ts_ns, px, trade_id, qty, instrument, venue, side, reserved);
CRD_WIRE_OFFSET(md_trade, trade_id, 16);
CRD_WIRE_OFFSET(md_trade, qty, 24);
CRD_WIRE_OFFSET(md_trade, side, 34);

typedef struct md_heartbeat {
    uint64_t ts_ns;     /* 0 */
    uint32_t seq;       /* 8 */
    uint32_t producer;  /* 12 */
} md_heartbeat;
CRD_WIRE_SIZE(md_heartbeat, 16);
CRD_WIRE_PACKED(md_heartbeat, ts_ns, seq, producer);

#endif
