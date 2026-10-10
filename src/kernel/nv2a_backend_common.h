/**
 * NV2A state logic shared by the pushbuffer backends.
 *
 * Everything here is API-independent: it turns NV2A register values and
 * guest memory into plain numbers and texels, and leaves the graphics API to
 * the backend. The D3D11 backend (src/d3d/nv2a_pb_d3d11.c) and the CPU
 * rasteriser (nv2a_pb_exec.c) both use it, so a fix to a texel format, a
 * blend factor or a stage decode lands in both at once, and a new backend
 * (Metal) starts from the same tables.
 *
 * Builds on every platform: no OS or graphics headers. The per-texel and
 * per-vertex helpers are static inline because the CPU path calls them per
 * pixel.
 */
#ifndef NV2A_BACKEND_COMMON_H
#define NV2A_BACKEND_COMMON_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../d3d/d3d8_swizzle.h"
#include "../d3d/d3d8_vsh_parse.h"
#include "nv2a_pb_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */
/* ------------------------------------------------------------------------ */

#define NV2A_TEX_P8 0x0Bu               /* SZ_I8_A8R8G8B8, through a palette */
#define NV2A_TEX_MAGENTA 0xFFFF00FFu    /* what an undecodable texel reads as */

/* The linear format with the same texel encoding as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06)
 * and LIN_A8R8G8B8 (0x12) are the same four bytes in the same order, so the
 * whole difference is the address calculation, and one decoder serves both.
 * Pairs read off the table in d3d8_xbox.h. */
static inline uint32_t nv2a_tex_linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;             /* L8        -> LIN_L8        */
    case 0x02: return 0x10;             /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;             /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;             /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;             /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;             /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;             /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;             /* A8        -> LIN_A8        */
    case 0x01: return 0x1B;             /* AY8       -> LIN_AY8       */
    case 0x1A: return 0x20;             /* A8Y8      -> LIN_A8Y8      */
    case 0x27: return 0x37;             /* R6G5B5    -> LIN_R6G5B5    */
    case 0x28: return 0x17;             /* G8B8      -> LIN_G8B8      */
    case 0x32: return 0x35;             /* Y16       -> LIN_Y16       */
    case 0x38: return 0x3D;             /* R5G5B5A1  -> LIN_R5G5B5A1  */
    case 0x39: return 0x3E;             /* R4G4B4A4  -> LIN_R4G4B4A4  */
    case 0x3A: return 0x3F;             /* A8B8G8R8  -> LIN_A8B8G8R8  */
    case 0x3B: return 0x40;             /* B8G8R8A8  -> LIN_B8G8R8A8  */
    case 0x3C: return 0x41;             /* R8G8B8A8  -> LIN_R8G8B8A8  */
    default:   return fmt;              /* already linear, P8, or unhandled */
    }
}

/* Bytes per texel of a linear format nv2a_tex_decode_texel reads (P8
 * included), 0 if it has no decoder. */
static inline uint32_t nv2a_tex_texel_bytes(uint32_t lin)
{
    switch (lin) {
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x10: case 0x1C: case 0x11: case 0x1D: case 0x24: case 0x25:
    case 0x20: case 0x37: case 0x35: case 0x3D: case 0x3E: case 0x17: return 2;
    case 0x13: case 0x1F: case 0x1B: case 0x0B: return 1;
    default: return 0;
    }
}

/* Whether the size comes from the format register (swizzled and DXT, power
 * of two, mipmapped) rather than from the image-rect register (linear). */
static inline int nv2a_tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

/* Level l's size: halved per level, never below 1. */
static inline uint32_t nv2a_tex_level_dim(uint32_t d, uint32_t l)
{
    d >>= l;
    return d ? d : 1u;
}

/* Bytes of guest memory a w x h texture of `levels` levels occupies, 0 if it
 * cannot be decoded. Swizzled and DXT levels are packed one after another;
 * a linear texture is one level at `pitch` bytes per row. The caller checks
 * validity and its own size limit first. */
/* SET_TEXTURE_FILTER, as every path reads it: MIN in bits 16-21, MAG in
 * 24-27, the LOD bias in 0-12. MIN: 1 nearest and 2 linear on level 0, 3/4
 * nearest/linear within the nearest level, 5/6 nearest/linear blended
 * between two levels, 7 convolution on level 0 (taken as linear). MAG: 1
 * nearest, 2 linear, 4 convolution (linear). */
static inline int nv2a_tex_filter_linear(uint32_t f, int mag)
{
    uint32_t m = mag ? (f >> 24) & 0xF : (f >> 16) & 0x3F;
    if (mag)
        return m == 2 || m == 4;
    return m == 2 || m == 4 || m == 6 || m == 7;
}

/* 0: level 0 only, 1: nearest level, 2: blend two levels. */
static inline int nv2a_tex_filter_mip(uint32_t f)
{
    uint32_t m = (f >> 16) & 0x3F;
    return m == 3 || m == 4 ? 1 : m == 5 || m == 6 ? 2 : 0;
}

/* The mip LOD bias, signed 5.8 fixed point. */
static inline float nv2a_tex_lod_bias(uint32_t f)
{
    int b = (int)(f & 0x1FFFu);
    if (b & 0x1000)
        b -= 0x2000;
    return (float)b / 256.0f;
}

/* Cube-map face and its 2D coordinates for direction `c` (xemu, and the
 * D3D face order +X -X +Y -Y +Z -Z); s, t in [0,1] on the face. */
static inline uint32_t nv2a_cube_face(const float c[3], float *s, float *t)
{
    float x = c[0], y = c[1], z = c[2];
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
    uint32_t face;
    if (ax >= ay && ax >= az) {
        face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
    } else if (ay >= az) {
        face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
    } else {
        face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
    }
    if (ma == 0.0f)
        ma = 1.0f;
    *s = (sc / ma + 1.0f) * 0.5f;
    *t = (tc / ma + 1.0f) * 0.5f;
    return face;
}

/* Bytes from one cube-map face to the next: a face with all `levels` of
 * its levels, rounded up to the NV2A's 128-byte face alignment (xemu
 * texture.c). A linear face is `pitch` * h. */
uint32_t nv2a_tex_face_stride(uint32_t fmt, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t levels);

uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels);

/* Level l's byte offset from level 0 in a swizzled or DXT mip chain (the
 * levels before it, packed). */
static inline uint32_t nv2a_tex_level_offset(uint32_t fmt, uint32_t w,
                                             uint32_t h, uint32_t l)
{
    return l ? nv2a_tex_extent(fmt, w, h, 0, l) : 0u;
}

/* A sampler's level of detail, for a sampler that computes its own (the
 * GPU paths get it from their sampler state: MipLODBias, MinLOD, MaxLOD):
 * log2 of rho, the larger of the pixel's x and y footprints in level-0
 * texels (rho2x, rho2y are their squares), plus the filter's LOD bias,
 * clamped to CONTROL0's [lod_min, lod_max]. No footprint (0 or NaN) gives
 * lod_min. */
static inline float nv2a_tex_lod(float rho2x, float rho2y, float bias,
                                 float lod_min, float lod_max)
{
    float r2 = rho2x > rho2y ? rho2x : rho2y, l;
    if (!(r2 > 0.0f))
        return lod_min;
    l = 0.5f * log2f(r2) + bias;
    if (!(l > lod_min)) return lod_min;     /* NaN too */
    return l < lod_max ? l : lod_max;
}

/* The level(s) MIN mip mode `mip` (nv2a_tex_filter_mip) reads at a clamped
 * LOD (>= 0) of a chain of `levels`: mode 1 the nearest level (*l0), mode 2
 * the two around it, *l1 weighted *f. Level l1 == l0 with *f 0 when one
 * level is all there is. */
static inline void nv2a_tex_mip_levels(int mip, float lod, uint32_t levels,
                                       uint32_t *l0, uint32_t *l1, float *f)
{
    uint32_t top = levels ? levels - 1u : 0u, l;
    if (mip == 2) {
        l = (uint32_t)lod;                  /* lod >= 0: floor */
        if (l >= top) { *l0 = *l1 = top; *f = 0.0f; return; }
        *l0 = l; *l1 = l + 1u; *f = lod - (float)l;
        return;
    }
    l = mip == 1 ? (uint32_t)(lod + 0.5f) : 0u;
    *l0 = *l1 = l < top ? l : top;
    *f = 0.0f;
}

/* FNV-style over n bytes, four independent lanes so it runs at memory
 * speed. h is the seed (NV2A_TEX_HASH_SEED for a fresh hash). */
#define NV2A_TEX_HASH_SEED 0xCBF29CE484222325ull
uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h);

/* Render-target ownership. A GPU backend keeps a target per surface the
 * title draws into, but the title owns that memory: it can free it and load
 * a stage's textures, vertices or tables there, and the old image must then
 * neither be sampled nor written back over them. Recompiled stores go
 * straight to guest memory, so there is no write tracking to ask; a target
 * keeps instead the hash of its guest bytes (pitch x h at its resolved
 * address, the bytes the seed reads and a write-back writes, clamped to the
 * window it lies in) as of the last time the backend knew them: its
 * creation and each write-back. A target not drawn in the current flip whose
 * bytes no longer hash the same was overwritten by the title and is stale.
 * One check per target per flip at most (checked_flip caches a good
 * result), plus one after each reset. */
#define NV2A_RT_WINDOW 0x08000000u   /* address compares use the low 27 bits */
/* The windows a resolved address lies in, as kernel.h's XBOX_CONTIG_BASE and
 * XBOX_CONTIG_SIZE and the 128 MB low mapping (this header takes no kernel
 * headers). */
#define NV2A_RT_CONTIG_BASE 0x80000000u
#define NV2A_RT_CONTIG_SIZE 0x04000000u
struct nv2a_rt_own {
    uint64_t mem_hash;
    uint32_t draw_flip;      /* the flip of the last draw or clear into it */
    uint32_t checked_flip;   /* the flip of the last hash */
};

/* Hash now: the bytes are the target's (creation, write-back). */
void nv2a_rt_own_reset(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                       uint32_t pitch, uint32_t h, uint32_t flip);

static inline void nv2a_rt_own_drawn(struct nv2a_rt_own *o, uint32_t flip)
{
    o->draw_flip = flip;
}

/* 1 when the title has rewritten the target's bytes: never for a target
 * drawn this flip, nor rehashed twice in one flip. */
int nv2a_rt_own_stale(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                      uint32_t pitch, uint32_t h, uint32_t flip);

/* Hashes taken so far by the two above (tests and the profile). */
uint64_t nv2a_rt_own_hashes(void);

/* Whether [a, a + an) and [b, b + bn) share a byte, through the low 27 bits
 * (the backends' rule for "the same memory through either window"). */
static inline int nv2a_rt_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    a &= NV2A_RT_WINDOW - 1u;
    b &= NV2A_RT_WINDOW - 1u;
    return an && bn && a < b + bn && b < a + an;
}

/* What a colour clear covers, in guest pixels, box = {x0, y0, x1, y1} with
 * x1/y1 exclusive: the surface clip, bounded by the clear rect once the title
 * has set one (inclusive max, as xemu reads it), clamped to a w x h target.
 * 0 when nothing is left. One box for every path: since a GPU target serves
 * every clip of its surface (Burnout 3 clears 159x344 regions of its 640x480
 * back buffer), a whole-target clear would wipe what another clip drew. */
static inline int nv2a_clear_box(const struct nv2a_pb_gpu *g, uint32_t w, uint32_t h,
                                 uint32_t box[4])
{
    uint32_t ax = g->clip_x, ay = g->clip_y;
    uint32_t bx = g->clip_x + g->clip_w, by = g->clip_y + g->clip_h;
    if (g->clear_rect_set) {
        uint32_t rx0 = g->clear_rect_h & 0xFFFFu, rx1 = (g->clear_rect_h >> 16) + 1u;
        uint32_t ry0 = g->clear_rect_v & 0xFFFFu, ry1 = (g->clear_rect_v >> 16) + 1u;
        if (rx0 > ax) ax = rx0;
        if (ry0 > ay) ay = ry0;
        if (rx1 < bx) bx = rx1;
        if (ry1 < by) by = ry1;
    }
    if (bx > w) bx = w;
    if (by > h) by = h;
    if (ax >= bx || ay >= by)
        return 0;
    box[0] = ax;
    box[1] = ay;
    box[2] = bx;
    box[3] = by;
    return 1;
}

static inline uint8_t nv2a_clamp255(int x)
{
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* One texel as A8R8G8B8 into *argb, from texel index u of the row at p
 * (linear formats; for a swizzled one p is the image base and u the Morton
 * index). P8 reads pal[0..pal_len), magenta for a missing palette or an
 * index past it. Returns 0, *argb untouched, for a format with no decoder. */
static inline int nv2a_tex_decode_texel(uint32_t fmt, const uint8_t *p,
                                        uint32_t u, const uint32_t *pal,
                                        uint32_t pal_len, uint32_t *argb)
{
    uint32_t t;
    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F:                                      /* LIN_A8B8G8R8 */
        t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    case 0x40:                                      /* LIN_B8G8R8A8 */
        t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (((t >> 8) & 0xFFu) << 16)
              | (((t >> 16) & 0xFFu) << 8) | ((t >> 24) & 0xFFu);
        return 1;
    case 0x41:                                      /* LIN_R8G8B8A8 */
        t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;

    /* 16-bit. */
    case 0x10:                                      /* LIN_A1R5G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x1C:                                      /* LIN_X1R5G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x11:                                      /* LIN_R5G6B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x3F, 6) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x1D:                                      /* LIN_A4R4G4B4 */
        t = ((const uint16_t *)p)[u];
        *argb = (d3d8_expand_channel((t >> 12) & 0xF, 4) << 24)
              | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 16)
              | (d3d8_expand_channel((t >> 4) & 0xF, 4) << 8)
              |  d3d8_expand_channel(t & 0xF, 4);
        return 1;

    /* 8-bit. */
    case 0x13:                                      /* LIN_L8 */
        t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;
    case 0x1B:                                      /* LIN_AY8: Y in all four */
        *argb = (uint32_t)p[u] * 0x01010101u;
        return 1;
    case 0x0B:                                      /* P8, through the palette */
        t = p[u];
        *argb = (pal && t < pal_len) ? pal[t] : NV2A_TEX_MAGENTA;
        return 1;

    /* 16-bit, the rest of the D3D8 set. */
    case 0x20:                                      /* LIN_A8Y8: Y low, A high */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | (t & 0xFFu) * 0x010101u;
        return 1;
    case 0x17:                                      /* LIN_G8B8 (and V8U8) */
        /* xemu's table loads it as GL_RG8 (low byte red, high byte green)
         * and swizzles to (R, G, R, G): so B and R are the low byte, G and
         * A the high one. A bump-map stage reads it back from b and g. */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | ((t & 0xFFu) << 16) | ((t >> 8) << 8)
              | (t & 0xFFu);
        return 1;
    case 0x35:                                      /* LIN_Y16: top byte */
        t = ((const uint16_t *)p)[u] >> 8;
        *argb = 0xFF000000u | t * 0x010101u;
        return 1;
    case 0x37:                                      /* LIN_R6G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 10) & 0x3F, 6) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x3D:                                      /* LIN_R5G5B5A1 */
        t = ((const uint16_t *)p)[u];
        *argb = ((t & 1u) ? 0xFF000000u : 0u)
              | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 6) & 0x1F, 5) << 8)
              |  d3d8_expand_channel((t >> 1) & 0x1F, 5);
        return 1;
    case 0x3E:                                      /* LIN_R4G4B4A4 */
        t = ((const uint16_t *)p)[u];
        *argb = (d3d8_expand_channel(t & 0xF, 4) << 24)
              | (d3d8_expand_channel((t >> 12) & 0xF, 4) << 16)
              | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 8)
              |  d3d8_expand_channel((t >> 4) & 0xF, 4);
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes: how a title hands over a
     * decoded video frame. The chroma pair is shared between an even texel
     * and the one after it, so the group is found by masking the bottom bit
     * of the index. BT.601, the coefficients the D3D8 upload path uses. */
    case 0x24:                                      /* YUY2 */
    case 0x25: {                                    /* UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        *argb = 0xFF000000u
              | ((uint32_t)nv2a_clamp255((298 * c + 409 * cv + 128) >> 8) << 16)
              | ((uint32_t)nv2a_clamp255((298 * c - 100 * cu - 208 * cv + 128) >> 8) << 8)
              |  (uint32_t)nv2a_clamp255((298 * c + 516 * cu + 128) >> 8);
        return 1;
    }

    default:
        return 0;
    }
}

/* Texels in levels 0..levels-1 of a w x h texture. */
size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels);

/* One swizzled or DXT level of w x h at mem into out (A8R8G8B8, row-major,
 * 0 for a texel with no decoder); returns the guest bytes it took, which is
 * where the next level starts. */
uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out);

/* A whole texture into out (nv2a_tex_texels() entries, A8R8G8B8): level 0
 * (w x h), then each further level packed after it. Swizzled and DXT
 * formats read `levels` levels from mem; a linear one reads one level at
 * `pitch` bytes per row. */
void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out);

/* ------------------------------------------------------------------------ */
/* Texture stages                                                            */
/* ------------------------------------------------------------------------ */

/* One texture stage as the registers program it. */
struct nv2a_stage {
    uint32_t offset, color, width, height, pitch, addr_u, addr_v;
    uint32_t palette, pal_len;  /* P8: resolved palette address, entries */
    uint32_t filter;            /* SET_TEXTURE_FILTER as written */
    uint32_t levels;            /* mip levels (1 for linear, or without
                                 * NV2A_STAGE_MIPS) */
    uint32_t lod_min, lod_max;  /* CONTROL0 LOD clamps, within levels    */
    uint32_t fmt_levels;        /* FORMAT's MIPMAP_LEVELS as stored (>= 1):
                                 * the memory layout, whatever is sampled */
    int      cube;              /* FORMAT CUBEMAP_ENABLE: six faces      */
    uint32_t dims;              /* FORMAT DIMENSIONALITY: 1, 2 or 3      */
    int      valid;
    int      on;        /* CONTROL0 enable as used (never-set stage 0 counts on) */
    int      mode;      /* 0 none, 1 2D, 4 pass-through, other: sampled as 2D */
    int      raw_mode;  /* the shader program's mode before the gating */
};

#define NV2A_STAGE_MIPS        1u   /* count mip levels as xemu does */
#define NV2A_STAGE_P8_PALETTE  2u   /* P8 is valid only with a palette */
#define NV2A_STAGE_PITCH_LINEAR 4u  /* pitch 0 for a swizzled or DXT texture */

typedef uint32_t (*nv2a_dma_resolve_fn)(uint32_t offset);

/* SET_TEXTURE_PALETTE (0x1B20 + 0x40 * stage): DMA offset in bits 6-31,
 * length 256 >> bits 2-3 entries of A8R8G8B8. *pal is the resolved guest
 * address, 0 for none. */
static inline void nv2a_tex_palette_decode(uint32_t reg,
                                           nv2a_dma_resolve_fn resolve,
                                           uint32_t *pal, uint32_t *len)
{
    *pal = (reg & ~0x3Fu) ? resolve(reg & ~0x3Fu) : 0;
    *len = 256u >> ((reg >> 2) & 3u);
}

/* Stage n from its 16 registers r (SET_TEXTURE_OFFSET 0x1B00 + 0x40 * n
 * onwards) and their written flags, with the shader-stage program
 * (shader_set false: stage 0 2D, the rest off). mode is the program's mode,
 * 0 where it asks for a texture the stage cannot give (disabled or
 * invalid; pass-through, 4, needs none). */
void nv2a_stage_decode(int n, const uint32_t *r, const uint8_t *set,
                       int shader_set, uint32_t shader_prog,
                       nv2a_dma_resolve_fn resolve, unsigned flags,
                       struct nv2a_stage *t);

/* ------------------------------------------------------------------------ */
/* Vertex attributes                                                         */
/* ------------------------------------------------------------------------ */

/* SET_VERTEX_DATA_ARRAY_FORMAT type codes: NV2A_VTX_* (d3d8_vsh_parse.h). */

/* One attribute of `size` components of `type` at p as float4, missing
 * components (0,0,0,1). Returns 0 for a type with no decoder (out is then
 * (0,0,0,1)). Decoded the way xemu hands them to GL. */
static inline int nv2a_vtx_decode(uint32_t type, uint32_t size,
                                  const uint8_t *p, float out[4])
{
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case NV2A_VTX_D3DCOLOR:
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV2A_VTX_FLOAT:
        for (i = 0; i < size && i < 4; i++)
            memcpy(&out[i], p + 4 * i, 4);
        return 1;
    case NV2A_VTX_UBYTE:
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case NV2A_VTX_S1:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            float f;
            memcpy(&s, p + 2 * i, 2);
            f = (float)s / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case NV2A_VTX_S32K:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = (float)s;
        }
        return 1;
    case NV2A_VTX_CMP: {
        uint32_t w;
        int32_t x, y, z;
        memcpy(&w, p, 4);
        x = (int32_t)(w << 21) >> 21;
        y = (int32_t)(w << 10) >> 21;
        z = (int32_t)w >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Primitives and programs                                                   */
/* ------------------------------------------------------------------------ */

enum nv2a_topology {
    NV2A_TOPO_NONE = 0,
    NV2A_TOPO_POINTS,
    NV2A_TOPO_LINES,
    NV2A_TOPO_TRIANGLES
};

/* Point, line and triangle lists from any NV097 primitive (NV_PRIM_*):
 * the n indices at v, each rebased by -base, into out (room for 3 * n).
 * Returns the index count and sets *topo; 0 and NV2A_TOPO_NONE for an
 * unknown primitive. Strips keep their winding, loops close. */
uint32_t nv2a_prim_to_list(uint32_t prim, const uint16_t *v, uint32_t n,
                           uint32_t base, uint32_t *out,
                           enum nv2a_topology *topo);

/* FNV-1a over the vertex program's slots, from v->start through the slot
 * with the FINAL bit (or the end). *len, if given, gets the slot count. */
uint32_t nv2a_vsh_program_hash(const struct nv2a_pb_vsh *v, uint32_t *len);

/* ------------------------------------------------------------------------ */
/* Depth, stencil, alpha test and blending                                   */
/* ------------------------------------------------------------------------ */

/* Compare functions in GL's order (SET_DEPTH_FUNC / SET_ALPHA_FUNC take GL's
 * 0x200 NEVER .. 0x207 ALWAYS, SET_STENCIL_FUNC its low bits), which is also
 * D3D11_COMPARISON_* - 1 and MTLCompareFunction's order. */
enum nv2a_cmp {
    NV2A_CMP_NEVER = 0, NV2A_CMP_LESS, NV2A_CMP_EQUAL, NV2A_CMP_LEQUAL,
    NV2A_CMP_GREATER, NV2A_CMP_NOTEQUAL, NV2A_CMP_GEQUAL, NV2A_CMP_ALWAYS
};

/* A GL compare code as nv2a_cmp; anything else tests as ALWAYS. */
static inline uint32_t nv2a_cmp_from_gl(uint32_t gl)
{
    return (gl >= 0x200 && gl <= 0x207) ? gl - 0x200 : NV2A_CMP_ALWAYS;
}

/* Whether `a cmp b` passes (a the incoming value, b the stored or ref). */
static inline int nv2a_cmp_test_u(uint32_t cmp, uint32_t a, uint32_t b)
{
    switch (cmp) {
    case NV2A_CMP_NEVER:    return 0;
    case NV2A_CMP_LESS:     return a <  b;
    case NV2A_CMP_EQUAL:    return a == b;
    case NV2A_CMP_LEQUAL:   return a <= b;
    case NV2A_CMP_GREATER:  return a >  b;
    case NV2A_CMP_NOTEQUAL: return a != b;
    case NV2A_CMP_GEQUAL:   return a >= b;
    default:                return 1;
    }
}

static inline int nv2a_cmp_test_f(uint32_t cmp, float a, float b)
{
    switch (cmp) {
    case NV2A_CMP_NEVER:    return 0;
    case NV2A_CMP_LESS:     return a <  b;
    case NV2A_CMP_EQUAL:    return a == b;
    case NV2A_CMP_LEQUAL:   return a <= b;
    case NV2A_CMP_GREATER:  return a >  b;
    case NV2A_CMP_NOTEQUAL: return a != b;
    case NV2A_CMP_GEQUAL:   return a >= b;
    default:                return 1;
    }
}

/* Stencil ops, in D3D11_STENCIL_OP_* - 1 (and MTLStencilOperation) order. */
enum nv2a_stencil_op {
    NV2A_SOP_KEEP = 0, NV2A_SOP_ZERO, NV2A_SOP_REPLACE, NV2A_SOP_INCR_SAT,
    NV2A_SOP_DECR_SAT, NV2A_SOP_INVERT, NV2A_SOP_INCR, NV2A_SOP_DECR
};

/* GL's stencil op codes; anything else keeps. */
static inline uint32_t nv2a_stencil_op_from_gl(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return NV2A_SOP_ZERO;
    case 0x1E01: return NV2A_SOP_REPLACE;
    case 0x1E02: return NV2A_SOP_INCR_SAT;
    case 0x1E03: return NV2A_SOP_DECR_SAT;
    case 0x150A: return NV2A_SOP_INVERT;
    case 0x8507: return NV2A_SOP_INCR;
    case 0x8508: return NV2A_SOP_DECR;
    default:     return NV2A_SOP_KEEP;
    }
}

/* The stencil value op makes of v (before the write mask). */
static inline uint32_t nv2a_stencil_op_apply(uint32_t op, uint32_t v,
                                             uint32_t ref)
{
    switch (op) {
    case NV2A_SOP_ZERO:     return 0;
    case NV2A_SOP_REPLACE:  return ref;
    case NV2A_SOP_INCR_SAT: return v < 255u ? v + 1u : 255u;
    case NV2A_SOP_DECR_SAT: return v ? v - 1u : 0u;
    case NV2A_SOP_INVERT:   return ~v;
    case NV2A_SOP_INCR:     return v + 1u;
    case NV2A_SOP_DECR:     return v - 1u;
    default:                return v;
    }
}

/* SET_SURFACE_FORMAT's zeta field: 1 = Z16, else Z24S8. The largest depth
 * value, and a CLEAR_SURFACE value as depth in [0,1]. */
static inline float nv2a_zmax(uint32_t surface_format)
{
    return ((surface_format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
}

static inline float nv2a_zclear_depth(uint32_t surface_format, uint32_t zclear)
{
    return ((surface_format >> 4) & 0xF) == 1
         ? (float)(zclear & 0xFFFFu) / 65535.0f
         : (float)(zclear >> 8) / 16777215.0f;
}

/* Blend factors (SET_BLEND_FUNC_SFACTOR / DFACTOR take GL's codes). */
enum nv2a_blend {
    NV2A_BF_ZERO = 0, NV2A_BF_ONE,
    NV2A_BF_SRC_COLOR, NV2A_BF_INV_SRC_COLOR,
    NV2A_BF_SRC_ALPHA, NV2A_BF_INV_SRC_ALPHA,
    NV2A_BF_DST_ALPHA, NV2A_BF_INV_DST_ALPHA,
    NV2A_BF_DST_COLOR, NV2A_BF_INV_DST_COLOR,
    NV2A_BF_SRC_ALPHA_SAT,
    NV2A_BF_CONST_COLOR, NV2A_BF_INV_CONST_COLOR,
    NV2A_BF_CONST_ALPHA, NV2A_BF_INV_CONST_ALPHA,
    NV2A_BF_UNKNOWN
};

static inline uint32_t nv2a_blend_from_gl(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return NV2A_BF_ZERO;
    case 0x0001: return NV2A_BF_ONE;
    case 0x0300: return NV2A_BF_SRC_COLOR;
    case 0x0301: return NV2A_BF_INV_SRC_COLOR;
    case 0x0302: return NV2A_BF_SRC_ALPHA;
    case 0x0303: return NV2A_BF_INV_SRC_ALPHA;
    case 0x0304: return NV2A_BF_DST_ALPHA;
    case 0x0305: return NV2A_BF_INV_DST_ALPHA;
    case 0x0306: return NV2A_BF_DST_COLOR;
    case 0x0307: return NV2A_BF_INV_DST_COLOR;
    case 0x0308: return NV2A_BF_SRC_ALPHA_SAT;
    case 0x8001: return NV2A_BF_CONST_COLOR;
    case 0x8002: return NV2A_BF_INV_CONST_COLOR;
    case 0x8003: return NV2A_BF_CONST_ALPHA;
    case 0x8004: return NV2A_BF_INV_CONST_ALPHA;
    default:     return NV2A_BF_UNKNOWN;
    }
}

/* The factor as used on the alpha channel: a _COLOR factor reads alpha. */
static inline uint32_t nv2a_blend_for_alpha(uint32_t bf)
{
    switch (bf) {
    case NV2A_BF_SRC_COLOR:     return NV2A_BF_SRC_ALPHA;
    case NV2A_BF_INV_SRC_COLOR: return NV2A_BF_INV_SRC_ALPHA;
    case NV2A_BF_DST_COLOR:     return NV2A_BF_DST_ALPHA;
    case NV2A_BF_INV_DST_COLOR: return NV2A_BF_INV_DST_ALPHA;
    default:                    return bf;
    }
}

/* A factor for the channel at bit `sh` of the ARGB words, 0..255, as the CPU
 * blender computes it. ponytail: SRC_ALPHA_SAT and unknown codes read as
 * INV_DST_COLOR here, as the CPU path always has. */
static inline uint32_t nv2a_blend_factor_u8(uint32_t bf, uint32_t src,
                                            uint32_t dst, uint32_t constant,
                                            uint32_t sh)
{
    switch (bf) {
    case NV2A_BF_ZERO:            return 0;
    case NV2A_BF_ONE:             return 255;
    case NV2A_BF_SRC_COLOR:       return (src >> sh) & 0xFF;
    case NV2A_BF_INV_SRC_COLOR:   return 255 - ((src >> sh) & 0xFF);
    case NV2A_BF_SRC_ALPHA:       return src >> 24;
    case NV2A_BF_INV_SRC_ALPHA:   return 255 - (src >> 24);
    case NV2A_BF_DST_ALPHA:       return dst >> 24;
    case NV2A_BF_INV_DST_ALPHA:   return 255 - (dst >> 24);
    case NV2A_BF_DST_COLOR:       return (dst >> sh) & 0xFF;
    case NV2A_BF_CONST_COLOR:     return (constant >> sh) & 0xFF;
    case NV2A_BF_INV_CONST_COLOR: return 255 - ((constant >> sh) & 0xFF);
    case NV2A_BF_CONST_ALPHA:     return constant >> 24;
    case NV2A_BF_INV_CONST_ALPHA: return 255 - (constant >> 24);
    case NV2A_BF_SRC_ALPHA_SAT: {         /* min(As, 1 - Ad); 1 for alpha */
        uint32_t sa = src >> 24, ida = 255 - (dst >> 24);
        return sh == 24 ? 255 : (sa < ida ? sa : ida);
    }
    default:                      return 255 - ((dst >> sh) & 0xFF);
    }
}

/* The constant blend factor an API with one RGBA constant needs, from
 * SET_BLEND_COLOR (ARGB) and the two factors: with only _ALPHA constants the
 * alpha is replicated into all four, a mix of _COLOR and _ALPHA keeps the
 * colour (*mixed set, for the caller to report). With no constant factor in
 * use, (1,1,1,1) and 0 is returned. */
int nv2a_blend_constant(int enable, uint32_t sfactor, uint32_t dfactor,
                        uint32_t color, float bf[4], int *mixed);

/* ------------------------------------------------------------------------ */
/* Fog                                                                       */
/* ------------------------------------------------------------------------ */

/* SET_FOG_MODE, decoded: func 0 linear, 1 exp, 2 exp2; abs_ for the _ABS
 * forms; edge_one when an infinite or NaN fog distance gives 1 (LINEAR,
 * LINEAR_ABS, EXP) rather than 0. Returns 0 for a mode that is not set. */
static inline int nv2a_fog_mode(uint32_t mode, uint32_t *func, uint32_t *abs_,
                                uint32_t *edge_one)
{
    switch (mode) {
    case 0x2601: *func = 0; *abs_ = 0; *edge_one = 1; return 1;  /* LINEAR */
    case 0x0804: *func = 0; *abs_ = 1; *edge_one = 1; return 1;  /* LINEAR_ABS */
    case 0x0800: *func = 1; *abs_ = 0; *edge_one = 1; return 1;  /* EXP */
    case 0x0802: *func = 1; *abs_ = 1; *edge_one = 0; return 1;  /* EXP_ABS */
    case 0x0801: *func = 2; *abs_ = 0; *edge_one = 0; return 1;  /* EXP2 */
    case 0x0803: *func = 2; *abs_ = 1; *edge_one = 0; return 1;  /* EXP2_ABS */
    default:     *func = 0; *abs_ = 0; *edge_one = 0; return 0;
    }
}

/* The fog factor for fog distance d (xemu vsh.c), with SET_FOG_PARAMS p0,
 * p1; 1 when fog is off or the mode is not set. */
static inline float nv2a_fog_factor(int enable, uint32_t mode, float p0,
                                    float p1, float d)
{
    uint32_t func, abs_, edge_one;
    int known;
    float f;

    if (!enable)
        return 1.0f;
    known = nv2a_fog_mode(mode, &func, &abs_, &edge_one);
    if (isinf(d))
        return edge_one ? 1.0f : 0.0f;
    if (!known)
        return 1.0f;
    if (func == 0)
        f = p0 + d * p1 - 1.0f;
    else if (func == 1)
        f = p0 + exp2f(d * p1 * 16.0f) - 1.5f;
    else
        f = p0 + exp2f(-d * d * p1 * p1 * 32.0f) - 1.5f;
    if (abs_)
        f = fabsf(f);
    if (isnan(f))
        return edge_one ? 1.0f : 0.0f;
    return f;
}

/* ------------------------------------------------------------------------ */
/* Shader constants                                                          */
/* ------------------------------------------------------------------------ */

static inline void nv2a_argb_to_float4(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

/* The combiner pixel shader's constants. This layout is the shader ABI any
 * emitter (HLSL now, MSL later) must honour: the same layout as
 * NV2APSConstants in d3d8_combiners.h, which nv2a_pb_d3d11.c checks field by
 * field with _Static_assert, and the cbuffer PS_CB_DECL declares there. */
struct nv2a_ps_consts {
    float    c0[8][4], c1[8][4], fc0[4], fc1[4], fog_color[4];
    float    alpha_ref;
    uint32_t alpha_func;        /* enum nv2a_cmp + 1: 1 NEVER .. 8 ALWAYS */
    uint32_t alpha_test_enable, fog_enable;
    uint32_t alpha_only[4];     /* 1: rgb reads 1 (A8); 2: alpha reads 1 (no alpha) */
    float    tex_scale[4][4];   /* xy: texcoord multiplier (1/size for linear) */
    uint32_t tex_mode[4];       /* stage texture-shader mode, 0 = none */
};

/* Fill pc from the combiner registers and the four stages. A stage that
 * samples a render target instead of guest memory has its linear scale set
 * by the caller (1/target size). */
void nv2a_ps_consts_fill(const struct nv2a_pb_vsh *v,
                         const struct nv2a_stage st[4],
                         struct nv2a_ps_consts *pc);

/* The vertex stage's viewport and fog constants: c = 2/w, -2/h, 1/zmax, 0,
 * -1, 1, 0, 0, fog params 0..2, 0; u = fog on, fog func, fog abs, fog
 * edge-one, specular on, light_ctl bit 17 (separate specular), 0, 0. */
struct nv2a_vp_consts {
    float    c[12];
    uint32_t u[8];
};

void nv2a_vp_consts_fill(const struct nv2a_pb_vsh *v, uint32_t surface_format,
                         uint32_t w, uint32_t h, struct nv2a_vp_consts *o);

/* ------------------------------------------------------------------------ */
/* Visibility-test reports (GET_REPORT)                                      */
/* ------------------------------------------------------------------------ */

/* A report's 16 bytes at guest va: timestamp (two words), count, status.
 * The count is stored before the status, with a fence between, so a title
 * polling the status from another core never reads a stale count. */
void nv2a_report_write(uint8_t *mem, uint32_t va, uint32_t count);

/* Mark a report pending: status 0xFFFFFFFF, which GetVisibilityTestResult
 * reads as D3DERR_TESTINCOMPLETE. */
void nv2a_report_set_pending(uint8_t *mem, uint32_t va);

/* API-independent bookkeeping for occlusion queries.
 *
 * Each stretch with SET_ZPASS_PIXEL_COUNT_ENABLE on is one query (begun at
 * enable, or at a clear while enabled; ended at disable or the report), and
 * a report sums the stretches since the last clear. Reports go on a pending
 * list and are polled; one still pending after NV2A_OCC_LATE_US, or with
 * more than NV2A_OCC_SEG stretches, completes as visible (NV2A_OCC_VISIBLE).
 * A GET_REPORT naming a va that still has one pending supersedes it: the old
 * queries go back unread, and nothing is written for the old test. The
 * backend supplies the queries through nv2a_occ_ops; handles are opaque. */
#define NV2A_OCC_SEG      8         /* stretches per report */
#define NV2A_OCC_PENDING  128
#define NV2A_OCC_VISIBLE  0x10000u
#define NV2A_OCC_LATE_US  250000u

struct nv2a_occ_ops {
    void *(*get)(void *ctx);                        /* NULL if none */
    void  (*put)(void *ctx, void *q);               /* back to the pool */
    void  (*begin)(void *ctx, void *q);
    void  (*end)(void *ctx, void *q);
    /* 1 and *count when q's result is in; flush: may push the GPU. */
    int   (*result)(void *ctx, void *q, int flush, uint64_t *count);
    uint64_t (*now_us)(void *ctx);
    void *ctx;
};

struct nv2a_occ_report {
    uint32_t va;
    int      n, overflow;
    uint64_t t0;
    void    *q[NV2A_OCC_SEG];
};

struct nv2a_occ {
    const struct nv2a_occ_ops *ops;
    uint8_t *mem;                       /* guest memory base */
    void    *seg[NV2A_OCC_SEG];         /* since the last clear */
    int      nseg, open, en, ovf;
    struct nv2a_occ_report pend[NV2A_OCC_PENDING];
    int      npend;
    /* For the flip log: reports, completed visible (count != 0, fallbacks
     * included), late fallbacks, superseded, and microseconds waited in
     * sync mode. */
    uint64_t reports, vis, late, super, sync_us;
    /* Render scale of the targets counted (0 or 1 = guest size): a count
     * is samples, N*N of them per guest pixel, so it is divided back. */
    unsigned scale;
};

void nv2a_occ_init(struct nv2a_occ *o, const struct nv2a_occ_ops *ops,
                   uint8_t *mem);
void nv2a_occ_zpass(struct nv2a_occ *o, int enable);
void nv2a_occ_zpass_clear(struct nv2a_occ *o);
/* GET_REPORT at va; sync waits (up to NV2A_OCC_LATE_US) for the result. */
void nv2a_occ_report(struct nv2a_occ *o, uint32_t va, int sync);
void nv2a_occ_poll(struct nv2a_occ *o, int flush);
/* nv2a_occ_init takes the scale from nv2a_host_opts(); this overrides it. */
void nv2a_occ_set_scale(struct nv2a_occ *o, unsigned scale);
/* A summed sample count at `scale` as the guest-size count: / scale^2,
 * rounded, a non-zero count staying >= 1 (visible never turns hidden),
 * saturated to 32 bits. */
uint32_t nv2a_occ_scale_count(uint64_t samples, unsigned scale);

/* ---- Host options: enhancements the backends and the presenter honour ----
 *
 * Filled once by the opt-in enhancements layer (src/enhance, xbox_enhance_init)
 * before the guest starts, read-only afterwards. All zero is stock: guest-size
 * targets, nearest letterbox, windowed. Without the layer nothing sets them. */
enum nv2a_present_filter {
    NV2A_PRESENT_NEAREST = 0,   /* stock: letterbox, nearest */
    NV2A_PRESENT_LINEAR,        /* letterbox, bilinear */
    NV2A_PRESENT_INTEGER        /* largest whole multiple that fits, centred */
};

struct nv2a_host_opts {
    unsigned render_scale;      /* 0 or 1 = guest size; 2..4 */
    int      present_filter;    /* enum nv2a_present_filter */
    int      fullscreen;        /* borderless desktop fullscreen at start */
};

void nv2a_host_opts_set(const struct nv2a_host_opts *o);
const struct nv2a_host_opts *nv2a_host_opts(void);
/* render_scale as a factor: 1 when unset. */
unsigned nv2a_host_render_scale(void);

/* Host size of a guest w x h target at `scale`: w*N x h*N, N lowered for
 * this target until both fit max_dim. Returns the N used (>= 1). */
unsigned nv2a_host_size(uint32_t w, uint32_t h, unsigned scale, uint32_t max_dim,
                        uint32_t *hw, uint32_t *hh);

/* Where a src_w x src_h frame goes in a dst_w x dst_h output (device
 * pixels). NEAREST/LINEAR: the largest rectangle of the frame's aspect that
 * fits, centred. INTEGER: the largest whole multiple that fits, centred; if
 * 1x does not fit, the NEAREST/LINEAR fit. Returns 1 when the result is the
 * integer placement, 0 for a fit (the caller filters a downscaled fit
 * linearly). A zero size gives an empty rectangle. */
struct nv2a_rect { int32_t x, y, w, h; };
int nv2a_present_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int filter, struct nv2a_rect *r);

/* 32-bit pixels: nearest N x upscale of a w x h image (src_pitch bytes a
 * row) into dst (w*n x h*n, dst_pitch bytes a row), and the box filter back:
 * each n x n block of src (w*n x h*n) averaged per byte channel, rounded,
 * into dst (w x h). */
void nv2a_upscale_nearest32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                            unsigned n, uint8_t *dst, uint32_t dst_pitch);
void nv2a_downscale_box32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                          unsigned n, uint8_t *dst, uint32_t dst_pitch);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_BACKEND_COMMON_H */
