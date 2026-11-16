/* Each WIRE_CASE must fail to compile because of a corundum/wire.h check;
 * WIRE_CASE 0 is the control and must compile. Driven by CTest (WILL_FAIL). */
#include "corundum/wire.h"
#include <stdint.h>

#if WIRE_CASE == 0 /* control: a correct layout */
typedef struct { uint64_t a; uint32_t b; uint16_t c; uint8_t d, e; } t;
CRD_WIRE_SIZE(t, 16);
CRD_WIRE_PACKED(t, a, b, c, d, e);
CRD_WIRE_OFFSET(t, c, 12);
#elif WIRE_CASE == 1 /* padding hole between b and a */
typedef struct { uint32_t b; uint64_t a; } t;
CRD_WIRE_PACKED(t, b, a);
#elif WIRE_CASE == 2 /* tail padding */
typedef struct { uint64_t a; uint8_t b; } t;
CRD_WIRE_PACKED(t, a, b);
#elif WIRE_CASE == 3 /* a field missing from the list */
typedef struct { uint64_t a; uint32_t b, c; } t;
CRD_WIRE_PACKED(t, a, b);
#elif WIRE_CASE == 4 /* packed attribute: no padding, but a misaligned field */
typedef struct __attribute__((packed)) { uint8_t a; uint64_t b; uint8_t c[7]; } t;
CRD_WIRE_PACKED(t, a, b, c);
#elif WIRE_CASE == 5 /* over-aligned: records are only 8-byte aligned */
typedef struct { _Alignas(16) uint64_t a; uint64_t b; } t;
CRD_WIRE_PACKED(t, a, b);
#elif WIRE_CASE == 6 /* size changed */
typedef struct { uint64_t a; uint64_t b; } t;
CRD_WIRE_SIZE(t, 24);
#elif WIRE_CASE == 7 /* offset changed */
typedef struct { uint64_t a; uint32_t b; uint32_t c; } t;
CRD_WIRE_OFFSET(t, c, 8);
#endif

int main(void) { return 0; }
