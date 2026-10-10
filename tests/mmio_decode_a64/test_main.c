/*
 * mmio_decode_a64 - does the AArch64 trapped-MMIO decoder service what clang
 * emits for a volatile MEM32()/MEM16()/MEM8() in lifted code, through a real
 * fault: a PROT_NONE page, SIGBUS/SIGSEGV, the ucontext, and a resume past
 * the instruction. The cases that matter are the ones where being wrong is
 * silent: a W load that does not clear bits 63:32, a sign-extending load that
 * does not, a post-index store whose base is not advanced, and an opcode the
 * decoder does not know reported as handled.
 *
 * Skips (exit 77) on hosts that are not POSIX arm64.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if defined(__aarch64__) && !defined(_WIN32)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "mmio_decode_a64.h"

static int failures;
#define CHECK_U64(name, got, want) \
    do { uint64_t g_=(uint64_t)(got), w_=(uint64_t)(want); \
         if (g_ != w_) { printf("FAIL: %s (got 0x%llX, want 0x%llX)\n", name, \
                   (unsigned long long)g_, (unsigned long long)w_); failures++; } } while (0)

#define WIN 0x8000u
static uint8_t *win;                 /* PROT_NONE */
static uint8_t model[WIN];           /* the device behind it */
static int traps, unknown;

static uint64_t rd(void *d, uint32_t off, int size)
{ uint64_t v = 0; memcpy(&v, (uint8_t *)d + off, size); return v; }
static void wr(void *d, uint32_t off, uint64_t v, int size)
{ memcpy((uint8_t *)d + off, &v, size); }

static void handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t f = (uintptr_t)si->si_addr;
    if (f >= (uintptr_t)win && f < (uintptr_t)win + WIN) {
        traps++;
        if (mmio_emulate_a64((ucontext_t *)ucv, (uint32_t)(f - (uintptr_t)win),
                             model, rd, wr))
            return;
        unknown++;
        /* Step over it anyway so the test can report instead of dying. */
        A64_CTX_PC((ucontext_t *)ucv) += 4;
        return;
    }
    signal(sig, SIG_DFL);
}

int main(void)
{
    struct sigaction sa;
    uint64_t x;
    volatile uint32_t *p32;

    win = mmap(NULL, WIN, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    /* MOV r/m32 both ways: str w / ldr w, unsigned immediate */
    p32 = (volatile uint32_t *)(win + 0x40);
    *p32 = 0xDEADBEEFu;
    CHECK_U64("str w lands in model", *(uint32_t *)(model + 0x40), 0xDEADBEEFu);
    x = ~0ULL;
    __asm__ volatile("ldr %w0, [%1]" : "+r"(x) : "r"(p32) : "memory");
    CHECK_U64("ldr w clears 63:32", x, 0xDEADBEEFu);

    /* Narrow forms, zero- and sign-extending */
    *(volatile uint16_t *)(win + 0x50) = 0xBEEF;
    *(volatile uint8_t *)(win + 0x60) = 0x80;
    CHECK_U64("ldrh", *(volatile uint16_t *)(win + 0x50), 0xBEEF);
    CHECK_U64("ldrsh -> int", (int64_t)*(volatile int16_t *)(win + 0x50), (int64_t)(int16_t)0xBEEF);
    CHECK_U64("ldrb", *(volatile uint8_t *)(win + 0x60), 0x80);
    CHECK_U64("ldrsb -> int", (int64_t)*(volatile int8_t *)(win + 0x60), -128);
    x = 0;
    __asm__ volatile("ldrsw %0, [%1]" : "=r"(x) : "r"(win + 0x40) : "memory");
    CHECK_U64("ldrsw sign-extends", x, 0xFFFFFFFFDEADBEEFull);

    /* Register offset, with the scaled-index form the lifted code produces
     * for MEM32(base + index*4). */
    {
        volatile uint32_t *base = (volatile uint32_t *)win;
        uint64_t idx = 0x40 / 4;
        __asm__ volatile("" : "+r"(idx));
        CHECK_U64("ldr w,[x,x,lsl 2]", base[idx], 0xDEADBEEFu);
    }

    /* Post-index store: a `rep stosd`-shaped loop. The base must advance. */
    {
        uint8_t *q = win + 0x100;
        __asm__ volatile("str %w1, [%0], #4\n\tstr %w2, [%0], #4"
                         : "+r"(q) : "r"(0x11111111u), "r"(0x22222222u) : "memory");
        CHECK_U64("post-index base advanced", (uintptr_t)q, (uintptr_t)(win + 0x108));
        CHECK_U64("post-index 1st", *(uint32_t *)(model + 0x100), 0x11111111u);
        CHECK_U64("post-index 2nd", *(uint32_t *)(model + 0x104), 0x22222222u);
    }
    /* Pre-index load */
    {
        uint8_t *q = win + 0x100; uint32_t v = 0;
        __asm__ volatile("ldr %w0, [%1, #4]!" : "=r"(v), "+r"(q) : : "memory");
        CHECK_U64("pre-index value", v, 0x22222222u);
        CHECK_U64("pre-index base", (uintptr_t)q, (uintptr_t)(win + 0x104));
    }
    /* Unscaled signed offset (ldur/stur) */
    {
        uint32_t v = 0;
        __asm__ volatile("stur %w0, [%1, #-4]" : : "r"(0x33333333u), "r"(win + 0x104) : "memory");
        __asm__ volatile("ldur %w0, [%1, #-4]" : "=r"(v) : "r"(win + 0x104) : "memory");
        CHECK_U64("stur/ldur", v, 0x33333333u);
    }
    /* 64-bit and FP */
    *(volatile uint64_t *)(win + 0x200) = 0x0102030405060708ull;
    CHECK_U64("ldr x", *(volatile uint64_t *)(win + 0x200), 0x0102030405060708ull);
    *(volatile float *)(win + 0x210) = 1.5f;
    CHECK_U64("ldr s", *(volatile uint32_t *)(win + 0x210), 0x3FC00000u);
    /* LDP/STP */
    {
        uint64_t a = 0, b = 0;
        __asm__ volatile("stp %w0, %w1, [%2]" : : "r"(5u), "r"(6u), "r"(win + 0x300) : "memory");
        __asm__ volatile("ldp %w0, %w1, [%2]" : "=r"(a), "=r"(b) : "r"(win + 0x300) : "memory");
        CHECK_U64("stp/ldp a", a, 5); CHECK_U64("stp/ldp b", b, 6);
    }
    /* wzr store */
    __asm__ volatile("str wzr, [%0]" : : "r"(win + 0x40) : "memory");
    CHECK_U64("str wzr", *(uint32_t *)(model + 0x40), 0);

    /* Refused: an exclusive load must not be reported as handled. */
    {
        int before = unknown;
        uint32_t v;
        __asm__ volatile("ldxr %w0, [%1]" : "=r"(v) : "r"(win + 0x40) : "memory");
        CHECK_U64("ldxr refused", unknown - before, 1);
    }

    printf("%d traps, %d unknown, %d failures\n", traps, unknown, failures);
    return failures ? 1 : 0;
}
#else
int main(void) { printf("mmio_decode_a64: not a POSIX arm64 host, skipped\n"); return 77; }
#endif
