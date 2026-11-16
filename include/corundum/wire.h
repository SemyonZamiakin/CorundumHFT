/*
 * Corundum HFT by Euclase: compile-time checks for binary message layouts.
 *
 * A record in a Corundum ring is read in place: the consumer casts the
 * payload pointer to a struct and uses it. That is only correct if both
 * sides agree on the struct's layout byte for byte. These macros turn that
 * agreement into compile errors instead of silent misreads:
 *
 *   typedef struct quote {           // 32 bytes, no padding
 *       uint64_t ts_ns;              //  0
 *       int64_t  price;              //  8  fixed point, 1e-8
 *       uint32_t qty;                // 16
 *       uint32_t instrument;         // 20
 *       uint16_t venue;              // 24
 *       uint8_t  side, flags;        // 26, 27
 *       uint32_t seq;                // 28
 *   } quote;
 *   CRD_WIRE_SIZE(quote, 32);
 *   CRD_WIRE_PACKED(quote, ts_ns, price, qty, instrument, venue, side, flags, seq);
 *   CRD_WIRE_OFFSET(quote, price, 8);
 *
 * CRD_WIRE_PACKED lists every field; it fails if the sizes of the listed
 * fields do not add up to sizeof(T) (so there is a padding hole, or a field
 * is missing from the list), if a field is not naturally aligned, or if T
 * needs more alignment than a ring record provides (8 bytes). Ordering the
 * fields from largest to smallest alignment avoids padding without
 * __attribute__((packed)), so every field stays naturally aligned and is
 * read with an ordinary load.
 *
 * Layouts are native little-endian: every target Corundum supports (x86-64,
 * arm64) is little-endian, and this header refuses to compile elsewhere, so
 * a byte dump of a record means the same thing on every supported machine.
 *
 * Works in C11 and C++11 and later, with GCC or Clang. Only macros and one
 * inline helper; no code is generated for the checks.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Euclase
 */
#ifndef CORUNDUM_WIRE_H
#define CORUNDUM_WIRE_H

#include "corundum.h"

#include <stddef.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "corundum/wire.h: wire layouts are defined as little-endian"
#endif

#ifdef __cplusplus
#include <type_traits>
#define CRD_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define CRD__WIRE_CXX_TRAITS(T)                                                              \
    ;                                                                                        \
    static_assert(std::is_trivially_copyable<T>::value && std::is_standard_layout<T>::value, \
                  #T " must be trivially copyable and standard-layout to be read in place")
#define CRD__FIELD_SIZE(T, f) sizeof(decltype(T::f))
#define CRD__FIELD_ALIGN(T, f) alignof(decltype(T::f))
#else
#define CRD_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define CRD__FIELD_SIZE(T, f) sizeof(((T *)0)->f)
#define CRD__FIELD_ALIGN(T, f) __alignof__(__typeof__(((T *)0)->f)) /* the type, not the (packed) member */
#define CRD__WIRE_CXX_TRAITS(T)
#endif

/* sizeof(T) must be exactly `bytes`. */
#define CRD_WIRE_SIZE(T, bytes) CRD_STATIC_ASSERT(sizeof(T) == (bytes), "wire size of " #T " changed")

/* Field `f` of T must start at byte `off`. */
#define CRD_WIRE_OFFSET(T, f, off) \
    CRD_STATIC_ASSERT(offsetof(T, f) == (off), "wire offset of " #T "." #f " changed")

/* ---- CRD_WIRE_PACKED(T, f1, ..., fN), 1 <= N <= 24 ---------------------- */

#define CRD__CAT(a, b) CRD__CAT_(a, b)
#define CRD__CAT_(a, b) a##b
#define CRD__NARG(...) CRD__NARG_(__VA_ARGS__, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define CRD__NARG_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, \
                   _23, _24, N, ...)                                                                                     \
    N

/* CRD__SUM(T, f...) -> (sizeof(T::f1) + ... + sizeof(T::fN)) */
#define CRD__S(T, f) CRD__FIELD_SIZE(T, f)
#define CRD__SUM_1(T, a) CRD__S(T, a)
#define CRD__SUM_2(T, a, ...) CRD__S(T, a) + CRD__SUM_1(T, __VA_ARGS__)
#define CRD__SUM_3(T, a, ...) CRD__S(T, a) + CRD__SUM_2(T, __VA_ARGS__)
#define CRD__SUM_4(T, a, ...) CRD__S(T, a) + CRD__SUM_3(T, __VA_ARGS__)
#define CRD__SUM_5(T, a, ...) CRD__S(T, a) + CRD__SUM_4(T, __VA_ARGS__)
#define CRD__SUM_6(T, a, ...) CRD__S(T, a) + CRD__SUM_5(T, __VA_ARGS__)
#define CRD__SUM_7(T, a, ...) CRD__S(T, a) + CRD__SUM_6(T, __VA_ARGS__)
#define CRD__SUM_8(T, a, ...) CRD__S(T, a) + CRD__SUM_7(T, __VA_ARGS__)
#define CRD__SUM_9(T, a, ...) CRD__S(T, a) + CRD__SUM_8(T, __VA_ARGS__)
#define CRD__SUM_10(T, a, ...) CRD__S(T, a) + CRD__SUM_9(T, __VA_ARGS__)
#define CRD__SUM_11(T, a, ...) CRD__S(T, a) + CRD__SUM_10(T, __VA_ARGS__)
#define CRD__SUM_12(T, a, ...) CRD__S(T, a) + CRD__SUM_11(T, __VA_ARGS__)
#define CRD__SUM_13(T, a, ...) CRD__S(T, a) + CRD__SUM_12(T, __VA_ARGS__)
#define CRD__SUM_14(T, a, ...) CRD__S(T, a) + CRD__SUM_13(T, __VA_ARGS__)
#define CRD__SUM_15(T, a, ...) CRD__S(T, a) + CRD__SUM_14(T, __VA_ARGS__)
#define CRD__SUM_16(T, a, ...) CRD__S(T, a) + CRD__SUM_15(T, __VA_ARGS__)
#define CRD__SUM_17(T, a, ...) CRD__S(T, a) + CRD__SUM_16(T, __VA_ARGS__)
#define CRD__SUM_18(T, a, ...) CRD__S(T, a) + CRD__SUM_17(T, __VA_ARGS__)
#define CRD__SUM_19(T, a, ...) CRD__S(T, a) + CRD__SUM_18(T, __VA_ARGS__)
#define CRD__SUM_20(T, a, ...) CRD__S(T, a) + CRD__SUM_19(T, __VA_ARGS__)
#define CRD__SUM_21(T, a, ...) CRD__S(T, a) + CRD__SUM_20(T, __VA_ARGS__)
#define CRD__SUM_22(T, a, ...) CRD__S(T, a) + CRD__SUM_21(T, __VA_ARGS__)
#define CRD__SUM_23(T, a, ...) CRD__S(T, a) + CRD__SUM_22(T, __VA_ARGS__)
#define CRD__SUM_24(T, a, ...) CRD__S(T, a) + CRD__SUM_23(T, __VA_ARGS__)
#define CRD__SUM(T, ...) (CRD__CAT(CRD__SUM_, CRD__NARG(__VA_ARGS__))(T, __VA_ARGS__))

/* CRD__ALIGNED(T, f...) -> 1 if every field's offset is a multiple of its alignment */
#define CRD__A(T, f) (offsetof(T, f) % CRD__FIELD_ALIGN(T, f) == 0)
#define CRD__AL_1(T, a) CRD__A(T, a)
#define CRD__AL_2(T, a, ...) CRD__A(T, a) && CRD__AL_1(T, __VA_ARGS__)
#define CRD__AL_3(T, a, ...) CRD__A(T, a) && CRD__AL_2(T, __VA_ARGS__)
#define CRD__AL_4(T, a, ...) CRD__A(T, a) && CRD__AL_3(T, __VA_ARGS__)
#define CRD__AL_5(T, a, ...) CRD__A(T, a) && CRD__AL_4(T, __VA_ARGS__)
#define CRD__AL_6(T, a, ...) CRD__A(T, a) && CRD__AL_5(T, __VA_ARGS__)
#define CRD__AL_7(T, a, ...) CRD__A(T, a) && CRD__AL_6(T, __VA_ARGS__)
#define CRD__AL_8(T, a, ...) CRD__A(T, a) && CRD__AL_7(T, __VA_ARGS__)
#define CRD__AL_9(T, a, ...) CRD__A(T, a) && CRD__AL_8(T, __VA_ARGS__)
#define CRD__AL_10(T, a, ...) CRD__A(T, a) && CRD__AL_9(T, __VA_ARGS__)
#define CRD__AL_11(T, a, ...) CRD__A(T, a) && CRD__AL_10(T, __VA_ARGS__)
#define CRD__AL_12(T, a, ...) CRD__A(T, a) && CRD__AL_11(T, __VA_ARGS__)
#define CRD__AL_13(T, a, ...) CRD__A(T, a) && CRD__AL_12(T, __VA_ARGS__)
#define CRD__AL_14(T, a, ...) CRD__A(T, a) && CRD__AL_13(T, __VA_ARGS__)
#define CRD__AL_15(T, a, ...) CRD__A(T, a) && CRD__AL_14(T, __VA_ARGS__)
#define CRD__AL_16(T, a, ...) CRD__A(T, a) && CRD__AL_15(T, __VA_ARGS__)
#define CRD__AL_17(T, a, ...) CRD__A(T, a) && CRD__AL_16(T, __VA_ARGS__)
#define CRD__AL_18(T, a, ...) CRD__A(T, a) && CRD__AL_17(T, __VA_ARGS__)
#define CRD__AL_19(T, a, ...) CRD__A(T, a) && CRD__AL_18(T, __VA_ARGS__)
#define CRD__AL_20(T, a, ...) CRD__A(T, a) && CRD__AL_19(T, __VA_ARGS__)
#define CRD__AL_21(T, a, ...) CRD__A(T, a) && CRD__AL_20(T, __VA_ARGS__)
#define CRD__AL_22(T, a, ...) CRD__A(T, a) && CRD__AL_21(T, __VA_ARGS__)
#define CRD__AL_23(T, a, ...) CRD__A(T, a) && CRD__AL_22(T, __VA_ARGS__)
#define CRD__AL_24(T, a, ...) CRD__A(T, a) && CRD__AL_23(T, __VA_ARGS__)
#define CRD__ALIGNED(T, ...) (CRD__CAT(CRD__AL_, CRD__NARG(__VA_ARGS__))(T, __VA_ARGS__))

/*
 * Every field of T, listed in any order. Fails to compile if T has padding
 * (or a field is missing from the list), if a field is misaligned (only
 * possible with packed attributes or pragmas), or if alignof(T) > 8. In C++
 * it also requires T to be trivially copyable and standard-layout.
 * Array fields count with their full size; nested structs count as one field
 * and need their own CRD_WIRE_PACKED.
 */
#define CRD_WIRE_PACKED(T, ...)                                                                             \
    CRD_STATIC_ASSERT(sizeof(T) == CRD__SUM(T, __VA_ARGS__), #T " has padding or an unlisted field");       \
    CRD_STATIC_ASSERT(CRD__ALIGNED(T, __VA_ARGS__), #T " has a field that is not naturally aligned");       \
    CRD_STATIC_ASSERT(__alignof__(T) <= CRD_RECORD_ALIGN, #T " needs more alignment than a ring record has") \
    CRD__WIRE_CXX_TRAITS(T)

/*
 * Typed zero-copy view of a record: a `const T *` into shared memory if the
 * record's size is exactly sizeof(T), otherwise NULL. Pair it with a type
 * check on m->type. Records are 8-byte aligned, so any T accepted by
 * CRD_WIRE_PACKED can be read through the pointer directly.
 */
#define CRD_MSG_AS(T, m) ((const T *)crd__msg_view((m), sizeof(T)))

static inline const void *crd__msg_view(const crd_msg *m, size_t size) {
    return m->size == size ? m->data : NULL;
}

#endif /* CORUNDUM_WIRE_H */
