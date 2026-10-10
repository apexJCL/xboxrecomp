/* The render-scale and present helpers (nv2a_backend_common.h), and the
 * guarantee they give at stock: every one is the identity at scale 1. */
#include <stdio.h>
#include <stdlib.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int rect_is(const struct nv2a_rect *r, int x, int y, int w, int h)
{
    return r->x == x && r->y == y && r->w == w && r->h == h;
}

/* A fake query pool for the tracker: every query reports s_samples. */
static uint64_t s_samples;
static int s_q;
static void *q_get(void *c) { (void)c; return &s_q; }
static void q_put(void *c, void *q) { (void)c; (void)q; }
static void q_begin(void *c, void *q) { (void)c; (void)q; }
static void q_end(void *c, void *q) { (void)c; (void)q; }
static int q_result(void *c, void *q, int f, uint64_t *n) { (void)c; (void)q; (void)f; *n = s_samples; return 1; }
static uint64_t q_now(void *c) { (void)c; return 0; }

static uint32_t occ_run(unsigned scale, uint64_t samples)
{
    static const struct nv2a_occ_ops ops = { q_get, q_put, q_begin, q_end, q_result, q_now, NULL };
    static struct nv2a_occ o;
    static uint32_t mem[64];
    nv2a_occ_init(&o, &ops, (uint8_t *)mem);
    nv2a_occ_set_scale(&o, scale);
    s_samples = samples;
    nv2a_occ_zpass(&o, 1);
    nv2a_occ_zpass(&o, 0);
    nv2a_occ_report(&o, 16, 1);
    return mem[4 + 2];       /* the report's count word */
}

int main(void)
{
    struct nv2a_rect r;
    uint32_t hw, hh, x, y;

    /* ---- host options: zero is stock ---- */
    CHECK(nv2a_host_opts()->render_scale == 0);
    CHECK(nv2a_host_opts()->present_filter == NV2A_PRESENT_NEAREST);
    CHECK(nv2a_host_opts()->fullscreen == 0);
    CHECK(nv2a_host_render_scale() == 1);

    /* ---- host size ---- */
    CHECK(nv2a_host_size(640, 480, 0, 16384, &hw, &hh) == 1 && hw == 640 && hh == 480);
    CHECK(nv2a_host_size(640, 480, 1, 16384, &hw, &hh) == 1 && hw == 640 && hh == 480);
    CHECK(nv2a_host_size(640, 480, 2, 16384, &hw, &hh) == 2 && hw == 1280 && hh == 960);
    CHECK(nv2a_host_size(256, 256, 3, 16384, &hw, &hh) == 3 && hw == 768 && hh == 768);
    CHECK(nv2a_host_size(640, 480, 4, 16384, &hw, &hh) == 4 && hw == 2560 && hh == 1920);
    /* Clamp: a 4096 surface at 4x does not fit 8192, 2x does. */
    CHECK(nv2a_host_size(4096, 64, 4, 8192, &hw, &hh) == 2 && hw == 8192 && hh == 128);
    CHECK(nv2a_host_size(4096, 4096, 4, 4096, &hw, &hh) == 1 && hw == 4096);

    /* ---- present rectangle ---- */
    /* Exact fit is the whole output, every filter. */
    CHECK(nv2a_present_rect(640, 480, 640, 480, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 0, 640, 480));
    CHECK(nv2a_present_rect(640, 480, 640, 480, NV2A_PRESENT_INTEGER, &r) == 1 && rect_is(&r, 0, 0, 640, 480));
    /* Fit: pillarbox in 16:9, letterbox in a tall output. */
    CHECK(nv2a_present_rect(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &r) == 0 && rect_is(&r, 240, 0, 1440, 1080));
    CHECK(nv2a_present_rect(640, 480, 800, 1000, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 200, 800, 600));
    /* Integer: 640x480 in 1920x1080 is 2x, centred. */
    CHECK(nv2a_present_rect(640, 480, 1920, 1080, NV2A_PRESENT_INTEGER, &r) == 1 && rect_is(&r, 320, 60, 1280, 960));
    /* Integer fallback: a 2x frame in a smaller output is the fit. */
    CHECK(nv2a_present_rect(1280, 960, 1024, 768, NV2A_PRESENT_INTEGER, &r) == 0 && rect_is(&r, 0, 0, 1024, 768));
    CHECK(nv2a_present_rect(1280, 960, 1000, 700, NV2A_PRESENT_INTEGER, &r) == 0 && rect_is(&r, 33, 0, 933, 700));
    /* Odd sizes round to the nearest pixel and stay inside. */
    CHECK(nv2a_present_rect(640, 480, 1001, 333, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 278, 0, 444, 333));
    /* Empty in, empty out. */
    CHECK(nv2a_present_rect(0, 480, 640, 480, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 0, 0, 0));
    CHECK(nv2a_present_rect(640, 480, 0, 0, NV2A_PRESENT_INTEGER, &r) == 0 && rect_is(&r, 0, 0, 0, 0));

    /* ---- upscale / downscale ---- */
    {
        enum { W = 5, H = 3 };
        uint32_t src[W * H], up[W * 3 * H * 3], back[W * H];
        unsigned n;
        for (x = 0; x < W * H; x++)
            src[x] = 0x01020304u * (x + 1) ^ 0xA5C3F00Fu;
        for (n = 1; n <= 3; n++) {
            nv2a_upscale_nearest32((const uint8_t *)src, W * 4, W, H, n, (uint8_t *)up, W * n * 4);
            /* Every n x n block is the source pixel. */
            for (y = 0; y < H * n; y++)
                for (x = 0; x < W * n; x++)
                    CHECK(up[y * W * n + x] == src[(y / n) * W + x / n]);
            /* Round trip: box of a nearest upscale is the input. */
            nv2a_downscale_box32((const uint8_t *)up, W * n * 4, W, H, n, (uint8_t *)back, W * 4);
            for (x = 0; x < W * H; x++)
                CHECK(back[x] == src[x]);
        }
    }
    {
        /* Box average per channel, rounded: 2x2 of 0, 255, 255, 255 -> 191;
         * 3x3 of eight 0 and one 10 -> 1 (10/9 = 1.1). Bytes, so BGRA order
         * is irrelevant; a pitch wider than the row is honoured. */
        uint8_t s2[2 * 12] = {0}, d2[8] = {0};
        uint8_t s3[3 * 12] = {0}, d3[4] = {0};
        int c;
        for (c = 0; c < 4; c++) {
            s2[4 + c] = 255;          /* (1,0) */
            s2[12 + c] = 255;         /* (0,1), pitch 12 */
            s2[16 + c] = 255;         /* (1,1) */
        }
        nv2a_downscale_box32(s2, 12, 1, 1, 2, d2, 8);
        for (c = 0; c < 4; c++)
            CHECK(d2[c] == 191);
        s3[12 + 4] = 10;              /* centre pixel's first byte */
        nv2a_downscale_box32(s3, 12, 1, 1, 3, d3, 4);
        CHECK(d3[0] == 1 && d3[1] == 0);
    }

    /* ---- visibility counts ---- */
    CHECK(nv2a_occ_scale_count(0, 1) == 0);
    CHECK(nv2a_occ_scale_count(12345, 1) == 12345);
    CHECK(nv2a_occ_scale_count(12345, 0) == 12345);
    CHECK(nv2a_occ_scale_count(0, 2) == 0);
    CHECK(nv2a_occ_scale_count(1, 2) == 1);          /* visible stays visible */
    CHECK(nv2a_occ_scale_count(3, 2) == 1);
    CHECK(nv2a_occ_scale_count(4, 2) == 1);
    CHECK(nv2a_occ_scale_count(6, 2) == 2);          /* 1.5 rounds up */
    CHECK(nv2a_occ_scale_count(4000, 2) == 1000);
    CHECK(nv2a_occ_scale_count(9 * 307200ull, 3) == 307200);
    CHECK(nv2a_occ_scale_count(4, 3) == 1);          /* (4+4)/9 rounds to 0 */
    for (uint64_t n = 1; n <= 8; n++)
        CHECK(nv2a_occ_scale_count(n, 3) != 0);
    for (uint64_t n = 1; n <= 15; n++)
        CHECK(nv2a_occ_scale_count(n, 4) != 0);
    CHECK(nv2a_occ_scale_count(0x400000000ull, 1) == 0xFFFFFFFFu);
    CHECK(nv2a_occ_scale_count(0x400000000ull, 2) == 0xFFFFFFFFu);
    CHECK(nv2a_occ_scale_count(0x400000000ull, 4) == 0x40000000u);
    /* Through the tracker: the report's word is the scaled count. */
    CHECK(occ_run(1, 4000) == 4000);
    CHECK(occ_run(2, 4000) == 1000);
    CHECK(occ_run(2, 1) == 1);
    CHECK(occ_run(2, 0) == 0);

    /* ---- the options round-trip; init picks the scale up ---- */
    {
        struct nv2a_host_opts o = { 3, NV2A_PRESENT_INTEGER, 1 };
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_render_scale() == 3);
        CHECK(nv2a_host_opts()->present_filter == NV2A_PRESENT_INTEGER && nv2a_host_opts()->fullscreen == 1);
        {
            static const struct nv2a_occ_ops ops = { q_get, q_put, q_begin, q_end, q_result, q_now, NULL };
            struct nv2a_occ oc;
            static uint32_t mem[8];
            nv2a_occ_init(&oc, &ops, (uint8_t *)mem);
            CHECK(oc.scale == 3);
        }
    }

    printf("render_scale: all passed\n");
    return 0;
}
