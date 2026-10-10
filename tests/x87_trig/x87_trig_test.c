/* x87 FSIN/FCOS/FSINCOS/FPTAN: the 66-bit-pi reduction, C2 out of range, and
 * the status word the CRT's `fnstsw ax; sahf; jp` loops read.
 *
 * The vectors come from model.py (mpmath, 200 bits):
 *     uv run --no-project --with mpmath python tests/x87_trig/model.py
 * Two of them are measured, not modelled: cos and tan of 1e10 from the
 * tools.conformance run on the x86 test host (native column), and
 * fsin(double(pi)), which Bruce Dawson measured ("Intel Underestimates Error
 * Bounds by 1.3 quintillion", 2014).
 *
 * -DX87_TRIG_PLAIN_LIBM swaps the helpers for the old lifting (plain libm);
 * that build is the second ctest, expected to fail (CMakeLists.txt). */
#include "recomp_types.h"
#include <stdio.h>
#include <time.h>

#ifdef X87_TRIG_PLAIN_LIBM
static int t_sin(double *st0, uint16_t *cc) { (void)cc; *st0 = sin(*st0); return 1; }
static int t_cos(double *st0, uint16_t *cc) { (void)cc; *st0 = cos(*st0); return 1; }
static int t_sincos(double *st0, double *c, uint16_t *cc) {
    (void)cc; *c = cos(*st0); *st0 = sin(*st0); return 1;
}
static int t_tan(double *st0, double *push, uint16_t *cc) {
    (void)cc; *st0 = tan(*st0); *push = 1.0; return 1;
}
#else
#define t_sin recomp_x87_fsin
#define t_cos recomp_x87_fcos
#define t_sincos recomp_x87_fsincos
#define t_tan recomp_x87_fptan
#endif

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* Relative at 1e-15, the conformance harness's tolerance for these ops. */
static int close_rel(double got, double want) {
    if (got == want) return 1;
    return fabs(got - want) <= 1e-15 * fmax(fabs(got), fabs(want));
}
static int same_bits(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }

static double sin_of(double x) { uint16_t cc = 0; t_sin(&x, &cc); return x; }
static double cos_of(double x) { uint16_t cc = 0; t_cos(&x, &cc); return x; }
static double tan_of(double x) { double p; uint16_t cc = 0; t_tan(&x, &p, &cc); return x; }

/* model.py output: x, then the x87's sin, cos and tan of x. */
static const double k_model[][4] = {
    { 355.0, -3.0144353359488908e-05, -0.999999999545659, 3.0144353373184723e-05 },
    { 100000.0, 0.03574879797201638, -0.9993608074382124, -0.035771662952898645 },
    { 1e10, -0.48750602507627, 0.8731196226831323, -0.5583496377943541 },
    { 1e15, 0.8582721324763734, -0.5131948427395374, -1.672409893861645 },
    { 1e18, -0.9928161040530035, 0.11965025504785125, -8.297651381152097 },
    /* either side of the |x * D| < 2^-26 shortcut, at |x| ~ 1.16e13 */
    { 11000000000000.0, -0.8138149460345726, 0.5811241120541684, -1.4004150389800285 },
    { 12000000000000.0, -0.9992963776518706, -0.03750666089976553, 26.643170937621846 },
    { 4611686018427387904.0, -0.7071329274527789, -0.7070806339534855, 1.0000739569106865 },
    { 9223372036854774784.0, 0.9873418913143515, -0.1586063985336008, -6.2251075646559295 },
    { 3.141592653589793, 1.2246063538223773e-16, -1.0, -1.2246063538223773e-16 },
};

int main(void) {
    /* 1. The host libm reduces large arguments correctly. The model is a
     * correction on top of libm, so a libm that does not would fail every
     * vector below for a reason that has nothing to do with the helper.
     * Within the tolerance, not bit for bit: Apple's tan(1e10) is one ulp
     * off the correctly rounded value, and a bad reduction is off by ~1e-11. */
    {
        volatile double x = 1e10;
        int ok = close_rel(sin(x), -0.4875060250875107)
                 && close_rel(cos(x), 0.873119622676856)
                 && close_rel(tan(x), -0.5583496378112418);
        if (!ok) {
            fprintf(stderr, "FAIL: this libm does not reduce 1e10 correctly"
                    " (sin %.17g cos %.17g tan %.17g); the x87 model needs it\n",
                    sin(x), cos(x), tan(x));
            return 1;
        }
    }

    /* 2. Measured on hardware. */
    CHECK(close_rel(cos_of(1e10), 0.87311962268313226));
    CHECK(close_rel(tan_of(1e10), -0.55834963779435409));
    CHECK(close_rel(sin_of(3.141592653589793), 1.2246063538223773e-16));

    /* 3. The model, both signs, every op; fsincos agrees with fsin/fcos. */
    for (size_t i = 0; i < sizeof k_model / sizeof k_model[0]; ++i) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            double x = sign * k_model[i][0];
            double ws = sign * k_model[i][1], wc = k_model[i][2], wt = sign * k_model[i][3];
            double s = x, c = 0.0; uint16_t cc = 0x0400;
            CHECK(close_rel(sin_of(x), ws));
            CHECK(close_rel(cos_of(x), wc));
            CHECK(close_rel(tan_of(x), wt));
            CHECK(t_sincos(&s, &c, &cc) && close_rel(s, ws) && close_rel(c, wc));
            if (failures) { fprintf(stderr, "  at x = %.17g\n", x); return 1; }
        }
    }

    /* 4. No reduction at |x| <= pi/4: libm's value, bit for bit. Above it the
     * shift is under an ulp at 7 and 100 and may or may not move the last bit. */
    {
        const double small[] = { 0.5, -0.7, 0.0, -0.0, 0.78539816339744828 };
        for (size_t i = 0; i < sizeof small / sizeof small[0]; ++i) {
            double x = small[i];
            CHECK(same_bits(sin_of(x), sin(x)));
            CHECK(same_bits(cos_of(x), cos(x)));
            CHECK(same_bits(tan_of(x), tan(x)));
        }
        CHECK(close_rel(sin_of(7.0), sin(7.0)) && close_rel(cos_of(100.0), cos(100.0)));
    }

    /* 5. Out of range: st0 unchanged, C2 set, nothing to push. In range and
     * for inf/NaN: C2 clear. */
    {
        const double far[] = { 9223372036854775808.0, -9223372036854775808.0,
                               18446744073709551616.0, 1e300 };
        for (size_t i = 0; i < sizeof far / sizeof far[0]; ++i) {
            double st0 = far[i], c = 42.0; uint16_t cc = 0x4100;
            CHECK(t_sin(&st0, &cc) == 0 && st0 == far[i] && cc == 0x4500);
            cc = 0; CHECK(t_cos(&st0, &cc) == 0 && st0 == far[i] && (cc & 0x0400));
            double p = 42.0;
            cc = 0; CHECK(t_tan(&st0, &p, &cc) == 0 && st0 == far[i] && p == 42.0
                          && (cc & 0x0400));
            cc = 0; CHECK(t_sincos(&st0, &c, &cc) == 0 && st0 == far[i] && c == 42.0
                          && (cc & 0x0400));
        }
        const double odd[] = { INFINITY, -INFINITY, NAN };
        for (size_t i = 0; i < 3; ++i) {
            double st0 = odd[i]; uint16_t cc = 0x0400;
            CHECK(t_sin(&st0, &cc) == 1 && isnan(st0) && !(cc & 0x0400));
            /* FPTAN pushes the NaN, not 1.0 (a stack probe on the Ryzen:
             * st0 and st1 both NaN, depth 2); FSINCOS pushes NaN too. */
            double p = 1.0, c = 1.0;
            st0 = odd[i]; cc = 0x0400;
            CHECK(t_tan(&st0, &p, &cc) == 1 && isnan(st0) && isnan(p) && !(cc & 0x0400));
            st0 = odd[i]; cc = 0x0400;
            CHECK(t_sincos(&st0, &c, &cc) == 1 && isnan(st0) && isnan(c)
                  && !(cc & 0x0400));
        }
        double st0 = 1.0, p = 0.0; uint16_t cc = 0x4500;
        CHECK(t_sin(&st0, &cc) == 1 && cc == 0x4100);
        st0 = 0.5; cc = 0;
        CHECK(t_tan(&st0, &p, &cc) == 1 && p == 1.0 && same_bits(st0, tan(0.5)));
    }

    /* 6. The CRT's reduction: `fsin; fnstsw ax; sahf; jp reduce`, then
     * `fprem1; fnstsw ax; sahf; jp loop`. The status word is built as the
     * lifted fnstsw builds it, and fprem clears C2 as the lifted one does. */
    {
        double st0 = 18446744073709551616.0; uint16_t cc = 0; uint16_t top = 7;
        t_sin(&st0, &cc);
        uint16_t sw = (uint16_t)(((top & 7u) << 11) | cc);
        CHECK((sw >> 8) & 0x04);               /* sahf: PF = C2, jp taken */
        cc &= (uint16_t)~0x0400u;              /* fprem1, as lifted */
        sw = (uint16_t)(((top & 7u) << 11) | cc);
        CHECK(!((sw >> 8) & 0x04));            /* the loop exits */
    }

    if (failures) return 1;

    /* 7. Cost, printed and not asserted (design.md D4). */
    {
        enum { N = 2000000 };
        volatile double sink = 0.0;
        double t[4];
        for (int pass = 0; pass < 2; ++pass) {
            double base = pass ? 1e10 : 0.0, step = pass ? 1.0 : 6.283185307179586 / N;
            clock_t a = clock();
            for (int i = 0; i < N; ++i) sink += sin(base + i * step);
            clock_t b = clock();
            for (int i = 0; i < N; ++i) sink += sin_of(base + i * step);
            clock_t c = clock();
            t[pass * 2] = (double)(b - a) / CLOCKS_PER_SEC * 1e9 / N;
            t[pass * 2 + 1] = (double)(c - b) / CLOCKS_PER_SEC * 1e9 / N;
        }
        printf("bench: sin [0, 2pi) libm %.1f ns, fsin helper %.1f ns;"
               " near 1e10 libm %.1f ns, helper %.1f ns\n", t[0], t[1], t[2], t[3]);
        (void)sink;
    }
    puts("PASS: x87 trig with the 66-bit pi (hardware, model and libm vectors),"
         " C2 out of range, the CRT reduction loop's status word");
    return 0;
}
