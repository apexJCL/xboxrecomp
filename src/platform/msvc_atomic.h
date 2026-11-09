/*
 * msvc_atomic.h -- the GCC/Clang __atomic and __sync builtins for MSVC.
 *
 * The runtime uses the compiler builtins (__atomic_load_n, __atomic_store_n,
 * __atomic_exchange_n, ...) on plain objects, which GCC and Clang (clang-cl
 * and llvm-mingw included) provide and MSVC does not: under cl.exe every one
 * of them is an undeclared function and every __ATOMIC_* an undeclared
 * identifier. This header supplies them, built on the Interlocked
 * intrinsics, and is force-included into every C translation unit by the
 * top-level CMakeLists.txt when the compiler is MSVC proper. Under any other
 * compiler it is empty, so nothing changes there.
 *
 * Semantics, on x64 (the only MSVC target the runtime builds for):
 *   - loads are volatile reads, which x64 orders as acquire;
 *   - relaxed and release stores are volatile writes (x64 stores are
 *     release); every other store is an Interlocked exchange (a full
 *     barrier), which is what sequential consistency needs on x64;
 *   - read-modify-write operations are Interlocked (full barriers), which is
 *     at least as strong as any memory order asked for;
 *   - __atomic_thread_fence is a compiler barrier, or a full fence for
 *     __ATOMIC_SEQ_CST.
 * A value goes through a uint64_t and is reinterpreted at the object's own
 * type (__typeof__, MSVC 19.39+), so pointers, integers of every width and
 * floating-point objects keep their bits.
 */
#ifndef XBOXRECOMP_MSVC_ATOMIC_H
#define XBOXRECOMP_MSVC_ATOMIC_H

#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)

#if !defined(_M_X64)
#error "msvc_atomic.h: MSVC builds of the runtime are x64 only (or use clang-cl)"
#endif

#include <stdint.h>
#include <string.h>
#include <intrin.h>

#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

/* The bits of the n-byte object at p, zero-extended. */
static __forceinline uint64_t msvc_atomic_bits_(const volatile void *p, size_t n)
{
    switch (n) {
    case 1: return *(const volatile uint8_t *)p;
    case 2: return *(const volatile uint16_t *)p;
    case 4: return *(const volatile uint32_t *)p;
    default: return *(const volatile uint64_t *)p;
    }
}

static __forceinline uint64_t msvc_atomic_load_(const volatile void *p, size_t n)
{
    uint64_t v = msvc_atomic_bits_(p, n);
    _ReadWriteBarrier();
    return v;
}

static __forceinline void msvc_atomic_store_(volatile void *p, size_t n, uint64_t v, int mo)
{
    if (mo == __ATOMIC_RELAXED || mo == __ATOMIC_RELEASE) {
        _ReadWriteBarrier();
        switch (n) {
        case 1: *(volatile uint8_t *)p = (uint8_t)v; break;
        case 2: *(volatile uint16_t *)p = (uint16_t)v; break;
        case 4: *(volatile uint32_t *)p = (uint32_t)v; break;
        default: *(volatile uint64_t *)p = v; break;
        }
        return;
    }
    switch (n) {
    case 1: _InterlockedExchange8((volatile char *)p, (char)v); break;
    case 2: _InterlockedExchange16((volatile short *)p, (short)v); break;
    case 4: _InterlockedExchange((volatile long *)p, (long)v); break;
    default: _InterlockedExchange64((volatile __int64 *)p, (__int64)v); break;
    }
}

static __forceinline uint64_t msvc_atomic_xchg_(volatile void *p, size_t n, uint64_t v)
{
    switch (n) {
    case 1: return (uint8_t)_InterlockedExchange8((volatile char *)p, (char)v);
    case 2: return (uint16_t)_InterlockedExchange16((volatile short *)p, (short)v);
    case 4: return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v);
    default: return (uint64_t)_InterlockedExchange64((volatile __int64 *)p, (__int64)v);
    }
}

/* Compare-and-swap; the value seen is returned. */
static __forceinline uint64_t msvc_atomic_cas_(volatile void *p, size_t n, uint64_t expected,
                                               uint64_t desired)
{
    switch (n) {
    case 1: return (uint8_t)_InterlockedCompareExchange8((volatile char *)p, (char)desired,
                                                         (char)expected);
    case 2: return (uint16_t)_InterlockedCompareExchange16((volatile short *)p, (short)desired,
                                                           (short)expected);
    case 4: return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired,
                                                         (long)expected);
    default: return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p,
                                                            (__int64)desired, (__int64)expected);
    }
}

/* __atomic_compare_exchange_n: *expected is updated with the value seen when
 * the exchange fails. */
static __forceinline int msvc_atomic_cmpxchg_(volatile void *p, size_t n, void *expected,
                                              uint64_t desired)
{
    uint64_t want = msvc_atomic_bits_(expected, n);
    uint64_t seen = msvc_atomic_cas_(p, n, want, desired);
    if (seen == want)
        return 1;
    memcpy(expected, &seen, n);
    return 0;
}

/* Read-modify-write. op: 0 add, 1 and, 2 or, 3 xor. Returns the old value;
 * *after gets the new one. */
static __forceinline uint64_t msvc_atomic_rmw_(volatile void *p, size_t n, int op, uint64_t v,
                                               uint64_t *after)
{
    uint64_t old, mask = n >= 8 ? ~0ull : ((1ull << (n * 8)) - 1);
    switch (op) {
    case 0:
        switch (n) {
        case 1: old = (uint8_t)_InterlockedExchangeAdd8((volatile char *)p, (char)v); break;
        case 2: old = (uint16_t)_InterlockedExchangeAdd16((volatile short *)p, (short)v); break;
        case 4: old = (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v); break;
        default: old = (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v); break;
        }
        *after = (old + v) & mask;
        return old;
    case 1:
        switch (n) {
        case 1: old = (uint8_t)_InterlockedAnd8((volatile char *)p, (char)v); break;
        case 2: old = (uint16_t)_InterlockedAnd16((volatile short *)p, (short)v); break;
        case 4: old = (uint32_t)_InterlockedAnd((volatile long *)p, (long)v); break;
        default: old = (uint64_t)_InterlockedAnd64((volatile __int64 *)p, (__int64)v); break;
        }
        *after = (old & v) & mask;
        return old;
    case 2:
        switch (n) {
        case 1: old = (uint8_t)_InterlockedOr8((volatile char *)p, (char)v); break;
        case 2: old = (uint16_t)_InterlockedOr16((volatile short *)p, (short)v); break;
        case 4: old = (uint32_t)_InterlockedOr((volatile long *)p, (long)v); break;
        default: old = (uint64_t)_InterlockedOr64((volatile __int64 *)p, (__int64)v); break;
        }
        *after = (old | v) & mask;
        return old;
    default:
        switch (n) {
        case 1: old = (uint8_t)_InterlockedXor8((volatile char *)p, (char)v); break;
        case 2: old = (uint16_t)_InterlockedXor16((volatile short *)p, (short)v); break;
        case 4: old = (uint32_t)_InterlockedXor((volatile long *)p, (long)v); break;
        default: old = (uint64_t)_InterlockedXor64((volatile __int64 *)p, (__int64)v); break;
        }
        *after = (old ^ v) & mask;
        return old;
    }
}

static __forceinline void msvc_atomic_fence_(int mo)
{
    if (mo == __ATOMIC_SEQ_CST)
        _mm_mfence();
    else
        _ReadWriteBarrier();
}

/* uint64_t bits -> a value of p's object type (little-endian: the low bytes
 * come first). */
#define MSVC_ATOMIC_AS_(p, bits) (*(__typeof__(*(p)) *)&(uint64_t){ (bits) })
/* A value converted to p's object type -> its bits. */
#define MSVC_ATOMIC_BITS_(p, v) msvc_atomic_bits_(&(__typeof__(*(p))){ (v) }, sizeof *(p))
/* An operand of an integer RMW: converted as an integer (pointers add bytes,
 * as GCC's builtins do on pointer objects). */
#define MSVC_ATOMIC_OPND_(v) ((uint64_t)(int64_t)(v))
#define MSVC_ATOMIC_RMW_OLD_(p, op, v) \
    MSVC_ATOMIC_AS_(p, msvc_atomic_rmw_((p), sizeof *(p), (op), MSVC_ATOMIC_OPND_(v), &(uint64_t){ 0 }))
#define MSVC_ATOMIC_RMW_NEW_(p, op, v) \
    MSVC_ATOMIC_AS_(p, msvc_atomic_rmw_new_((p), sizeof *(p), (op), MSVC_ATOMIC_OPND_(v)))

static __forceinline uint64_t msvc_atomic_rmw_new_(volatile void *p, size_t n, int op, uint64_t v)
{
    uint64_t after;
    msvc_atomic_rmw_(p, n, op, v, &after);
    return after;
}

#define __atomic_load_n(p, mo)          MSVC_ATOMIC_AS_(p, msvc_atomic_load_((p), sizeof *(p)))
#define __atomic_store_n(p, v, mo)      msvc_atomic_store_((p), sizeof *(p), MSVC_ATOMIC_BITS_(p, v), (mo))
#define __atomic_exchange_n(p, v, mo)   MSVC_ATOMIC_AS_(p, msvc_atomic_xchg_((p), sizeof *(p), MSVC_ATOMIC_BITS_(p, v)))
#define __atomic_compare_exchange_n(p, expected, desired, weak, so, fo) \
    msvc_atomic_cmpxchg_((p), sizeof *(p), (expected), MSVC_ATOMIC_BITS_(p, desired))
#define __atomic_load(p, ret, mo)       ((void)(*(ret) = __atomic_load_n((p), (mo))))
#define __atomic_store(p, val, mo)      __atomic_store_n((p), *(val), (mo))

#define __atomic_fetch_add(p, v, mo)    MSVC_ATOMIC_RMW_OLD_(p, 0, v)
#define __atomic_fetch_sub(p, v, mo)    MSVC_ATOMIC_RMW_OLD_(p, 0, -(int64_t)(v))
#define __atomic_fetch_and(p, v, mo)    MSVC_ATOMIC_RMW_OLD_(p, 1, v)
#define __atomic_fetch_or(p, v, mo)     MSVC_ATOMIC_RMW_OLD_(p, 2, v)
#define __atomic_fetch_xor(p, v, mo)    MSVC_ATOMIC_RMW_OLD_(p, 3, v)
#define __atomic_add_fetch(p, v, mo)    MSVC_ATOMIC_RMW_NEW_(p, 0, v)
#define __atomic_sub_fetch(p, v, mo)    MSVC_ATOMIC_RMW_NEW_(p, 0, -(int64_t)(v))
#define __atomic_and_fetch(p, v, mo)    MSVC_ATOMIC_RMW_NEW_(p, 1, v)
#define __atomic_or_fetch(p, v, mo)     MSVC_ATOMIC_RMW_NEW_(p, 2, v)
#define __atomic_xor_fetch(p, v, mo)    MSVC_ATOMIC_RMW_NEW_(p, 3, v)

#define __atomic_test_and_set(p, mo)    (_InterlockedExchange8((volatile char *)(p), 1) != 0)
#define __atomic_clear(p, mo)           msvc_atomic_store_((p), 1, 0, (mo))
#define __atomic_thread_fence(mo)       msvc_atomic_fence_(mo)
#define __atomic_signal_fence(mo)       _ReadWriteBarrier()

#define __sync_fetch_and_add(p, v)      __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define __sync_fetch_and_sub(p, v)      __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST)
#define __sync_fetch_and_and(p, v)      __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define __sync_fetch_and_or(p, v)       __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define __sync_add_and_fetch(p, v)      __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define __sync_sub_and_fetch(p, v)      __atomic_sub_fetch((p), (v), __ATOMIC_SEQ_CST)
#define __sync_val_compare_and_swap(p, old, new) \
    MSVC_ATOMIC_AS_(p, msvc_atomic_cas_((p), sizeof *(p), MSVC_ATOMIC_BITS_(p, old), MSVC_ATOMIC_BITS_(p, new)))
#define __sync_bool_compare_and_swap(p, old, new) \
    (msvc_atomic_cas_((p), sizeof *(p), MSVC_ATOMIC_BITS_(p, old), MSVC_ATOMIC_BITS_(p, new)) \
     == MSVC_ATOMIC_BITS_(p, old))
#define __sync_synchronize()            _mm_mfence()

#endif /* MSVC, C */
#endif /* XBOXRECOMP_MSVC_ATOMIC_H */
