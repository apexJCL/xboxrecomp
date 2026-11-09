/* The __atomic/__sync builtins as the runtime uses them, checked value by
 * value. Under MSVC they come from src/platform/msvc_atomic.h (force-included
 * by CMakeLists.txt); under GCC and Clang they are the compiler's own. */
#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* One width: load/store/exchange/CAS and every read-modify-write, old and
 * new values, with a neighbour on each side to catch a too-wide write. */
#define WIDTH_TEST(T, name)                                                       \
    static void test_##name(void)                                                 \
    {                                                                             \
        struct { T lo; volatile T v; T hi; } s = { (T)0x5A, 0, (T)0xA5 };         \
        T old, exp;                                                               \
        __atomic_store_n(&s.v, (T)7, __ATOMIC_RELAXED);                           \
        CHECK(__atomic_load_n(&s.v, __ATOMIC_ACQUIRE) == (T)7);                   \
        __atomic_store_n(&s.v, (T)9, __ATOMIC_SEQ_CST);                           \
        CHECK(s.v == (T)9);                                                       \
        old = __atomic_exchange_n(&s.v, (T)3, __ATOMIC_ACQ_REL);                  \
        CHECK(old == (T)9 && s.v == (T)3);                                        \
        exp = 4;                                                                  \
        CHECK(!__atomic_compare_exchange_n(&s.v, &exp, (T)5, 0,                   \
                                           __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));  \
        CHECK(exp == (T)3 && s.v == (T)3);                                        \
        CHECK(__atomic_compare_exchange_n(&s.v, &exp, (T)5, 1,                    \
                                          __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));   \
        CHECK(s.v == (T)5);                                                       \
        CHECK(__atomic_fetch_add(&s.v, 2, __ATOMIC_RELAXED) == (T)5);             \
        CHECK(__atomic_add_fetch(&s.v, 2, __ATOMIC_RELAXED) == (T)9);             \
        CHECK(__atomic_fetch_sub(&s.v, 1, __ATOMIC_RELAXED) == (T)9);             \
        CHECK(__atomic_sub_fetch(&s.v, 10, __ATOMIC_RELAXED) == (T)-2);           \
        __atomic_store_n(&s.v, (T)0x0C, __ATOMIC_RELEASE);                        \
        CHECK(__atomic_fetch_and(&s.v, 0x0A, __ATOMIC_RELAXED) == (T)0x0C);       \
        CHECK(__atomic_and_fetch(&s.v, 0x08, __ATOMIC_RELAXED) == (T)0x08);       \
        CHECK(__atomic_fetch_or(&s.v, 0x01, __ATOMIC_RELAXED) == (T)0x08);        \
        CHECK(__atomic_or_fetch(&s.v, 0x10, __ATOMIC_RELAXED) == (T)0x19);        \
        CHECK(__atomic_fetch_xor(&s.v, 0x01, __ATOMIC_RELAXED) == (T)0x19);       \
        CHECK(s.v == (T)0x18);                                                    \
        CHECK(__sync_fetch_and_add(&s.v, 1) == (T)0x18);                          \
        CHECK(__sync_add_and_fetch(&s.v, 1) == (T)0x1A);                          \
        CHECK(__sync_fetch_and_or(&s.v, 0x20) == (T)0x1A);                        \
        CHECK(__sync_fetch_and_and(&s.v, 0x3F) == (T)0x3A);                       \
        CHECK(__sync_val_compare_and_swap(&s.v, (T)0x3A, (T)1) == (T)0x3A);       \
        CHECK(__sync_val_compare_and_swap(&s.v, (T)0x3A, (T)2) == (T)1);          \
        CHECK(s.v == (T)1);                                                       \
        CHECK(s.lo == (T)0x5A && s.hi == (T)0xA5);                                \
    }

WIDTH_TEST(uint8_t, u8)
WIDTH_TEST(int16_t, s16)
WIDTH_TEST(uint32_t, u32)
WIDTH_TEST(int64_t, s64)

static void test_wrap(void)
{
    /* Arithmetic wraps at the object's width; the new value is that width. */
    uint8_t b = 250;
    uint16_t h = 0xFFFF;
    CHECK(__atomic_add_fetch(&b, 10, __ATOMIC_SEQ_CST) == 4);
    CHECK(__atomic_add_fetch(&h, 1, __ATOMIC_SEQ_CST) == 0);
    CHECK(__atomic_sub_fetch(&h, 1, __ATOMIC_SEQ_CST) == 0xFFFF);
}

static void test_pointer(void)
{
    static int a, b;
    int *p = &a, *q;
    void *vp = NULL;
    void *exp = NULL;
    CHECK(__atomic_load_n(&p, __ATOMIC_ACQUIRE) == &a);
    __atomic_store_n(&p, &b, __ATOMIC_RELEASE);
    CHECK(p == &b);
    q = __atomic_exchange_n(&p, &a, __ATOMIC_SEQ_CST);
    CHECK(q == &b && p == &a);
    CHECK(__atomic_compare_exchange_n(&vp, &exp, (void *)&b, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    CHECK(vp == &b);
}

static void test_float(void)
{
    /* Bits, not values, go through: a float or double is stored and loaded
     * whole, not converted. */
    float f = 0.0f;
    double d = 0.0, old;
    __atomic_store_n(&f, 1.5f, __ATOMIC_RELAXED);
    CHECK(__atomic_load_n(&f, __ATOMIC_RELAXED) == 1.5f);
    __atomic_store_n(&d, -2.25, __ATOMIC_SEQ_CST);
    CHECK(__atomic_load_n(&d, __ATOMIC_SEQ_CST) == -2.25);
    old = __atomic_exchange_n(&d, 3.0, __ATOMIC_SEQ_CST);
    CHECK(old == -2.25 && d == 3.0);
}

static void test_generic_and_flags(void)
{
    uint32_t v = 0, out = 0, in = 77;
    unsigned char flag = 0;
    __atomic_store(&v, &in, __ATOMIC_SEQ_CST);
    __atomic_load(&v, &out, __ATOMIC_SEQ_CST);
    CHECK(out == 77);
    CHECK(!__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE));
    CHECK(__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE));
    __atomic_clear(&flag, __ATOMIC_RELEASE);
    CHECK(flag == 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

int main(void)
{
    test_u8();
    test_s16();
    test_u32();
    test_s64();
    test_wrap();
    test_pointer();
    test_float();
    test_generic_and_flags();
    if (failures) {
        fprintf(stderr, "msvc_atomic: %d failure(s)\n", failures);
        return 1;
    }
    printf("msvc_atomic: all checks passed\n");
    return 0;
}
