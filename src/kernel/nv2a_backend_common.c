/**
 * NV2A state logic shared by the pushbuffer backends: the out-of-line half
 * of nv2a_backend_common.h. No OS or graphics headers.
 */
#include "nv2a_backend_common.h"

#include <stdatomic.h>

uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(fmt), tb, l, n = 0;
    if (bb || d3d8_format_is_swizzled(fmt)) {
        tb = bb ? 0 : nv2a_tex_texel_bytes(nv2a_tex_linear_twin(fmt));
        if (!bb && !tb)
            return 0;
        for (l = 0; l < levels; l++) {
            n += bb ? ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb : w * h * tb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return n;
    }
    tb = nv2a_tex_texel_bytes(fmt);
    if (!tb || !pitch)
        return 0;
    return pitch * (h - 1u) + w * tb;
}

uint32_t nv2a_tex_face_stride(uint32_t fmt, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t levels)
{
    uint32_t n = nv2a_tex_size_from_format(fmt)
               ? nv2a_tex_extent(fmt, w, h, pitch, levels ? levels : 1u)
               : pitch * h;
    return (n + 127u) & ~127u;
}

uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h)
{
    uint64_t l[4] = { h, h ^ 1u, h ^ 2u, h ^ 3u };
    uint32_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        l[0] = (l[0] ^ w[0]) * 0x100000001B3ull;
        l[1] = (l[1] ^ w[1]) * 0x100000001B3ull;
        l[2] = (l[2] ^ w[2]) * 0x100000001B3ull;
        l[3] = (l[3] ^ w[3]) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        l[0] = (l[0] ^ p[i]) * 0x100000001B3ull;
    return (l[0] ^ (l[1] << 1) ^ (l[2] << 2) ^ (l[3] << 3)) * 0x100000001B3ull;
}

static _Atomic uint64_t s_rt_hashes;

static uint64_t rt_own_hash(const uint8_t *mem, uint32_t addr, uint32_t pitch, uint32_t h)
{
    /* The end of the window addr lies in; an address in neither (a test's
     * own surface) is read as the seed and a write-back would read it. */
    uint64_t top = addr >= NV2A_RT_CONTIG_BASE
                       ? (uint64_t)NV2A_RT_CONTIG_BASE + NV2A_RT_CONTIG_SIZE
                 : addr < NV2A_RT_WINDOW ? NV2A_RT_WINDOW : 0x100000000ull;
    uint64_t n = (uint64_t)pitch * h;
    if (n > top - addr)
        n = top - addr;
    atomic_fetch_add_explicit(&s_rt_hashes, 1, memory_order_relaxed);
    return nv2a_tex_hash(mem + addr, (uint32_t)n, NV2A_TEX_HASH_SEED);
}

void nv2a_rt_own_reset(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                       uint32_t pitch, uint32_t h, uint32_t flip)
{
    o->mem_hash = rt_own_hash(mem, addr, pitch, h);
    /* Knowledge of this instant, not a check for the flip: a title write
     * later in the same flip (a CPU fill before its next draw) must still
     * be seen by the next check. */
    o->checked_flip = flip - 1u;
}

int nv2a_rt_own_stale(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                      uint32_t pitch, uint32_t h, uint32_t flip)
{
    if (o->draw_flip == flip || o->checked_flip == flip)
        return 0;
    o->checked_flip = flip;
    return rt_own_hash(mem, addr, pitch, h) != o->mem_hash;
}

uint64_t nv2a_rt_own_hashes(void)
{
    return atomic_load_explicit(&s_rt_hashes, memory_order_relaxed);
}

size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < levels; l++)
        n += (size_t)nv2a_tex_level_dim(w, l) * nv2a_tex_level_dim(h, l);
    return n;
}

uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, bb = d3d8_format_dxt_block_bytes(fmt);
    if (bb) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                d3d8_dxt_decode_texel(mem, fmt, x, y, w, &argb);
                out[(size_t)y * w + x] = argb;
            }
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb;
    } else {
        uint32_t mx, my, lin = nv2a_tex_linear_twin(fmt);
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                nv2a_tex_decode_texel(lin, mem, swizzle_deposit(x, mx) | oy,
                                      pal, pal_len, &argb);
                out[(size_t)y * w + x] = argb;
            }
        }
        return w * h * nv2a_tex_texel_bytes(lin);
    }
}

void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, l;
    if (nv2a_tex_size_from_format(fmt)) {
        for (l = 0; l < levels; l++) {
            uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
            mem += nv2a_tex_decode_level(mem, fmt, lw, lh, pal, pal_len, out);
            out += (size_t)lw * lh;
        }
        return;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *row = mem + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t argb = 0;
            nv2a_tex_decode_texel(fmt, row, x, pal, pal_len, &argb);
            out[(size_t)y * w + x] = argb;
        }
    }
}

void nv2a_stage_decode(int n, const uint32_t *r, const uint8_t *set,
                       int shader_set, uint32_t shader_prog,
                       nv2a_dma_resolve_fn resolve, unsigned flags,
                       struct nv2a_stage *t)
{
    uint32_t fmt = r[0x04 / 4], ctl0 = r[0x0C / 4];
    int on = (ctl0 >> 30) & 1, mode, from_fmt;

    memset(t, 0, sizeof *t);
    t->offset = r[0] ? resolve(r[0]) : 0;
    t->color = (fmt >> 8) & 0xFF;
    from_fmt = nv2a_tex_size_from_format(t->color);
    if (from_fmt) {
        t->width  = 1u << ((fmt >> 20) & 0xF);
        t->height = 1u << ((fmt >> 24) & 0xF);
    } else {
        t->width  = r[0x1C / 4] >> 16;
        t->height = r[0x1C / 4] & 0xFFFF;
    }
    /* CONTROL1's pitch means nothing to a swizzled or DXT texture, and
     * left in a cache key a stale one splits it in two. */
    t->pitch  = (from_fmt && (flags & NV2A_STAGE_PITCH_LINEAR))
              ? 0 : r[0x10 / 4] >> 16;
    t->addr_u =  r[0x08 / 4]       & 0xF;
    t->addr_v = (r[0x08 / 4] >> 8) & 0xF;
    t->filter = r[0x14 / 4];
    t->cube = (int)((fmt >> 2) & 1u);
    t->dims = (fmt >> 4) & 0xFu;
    t->fmt_levels = (fmt >> 16) & 0xFu;
    if (!t->fmt_levels)
        t->fmt_levels = 1;
    /* P8: SET_TEXTURE_PALETTE (0x1B20). */
    if (t->color == NV2A_TEX_P8)
        nv2a_tex_palette_decode(r[0x20 / 4], resolve, &t->palette, &t->pal_len);
    t->levels = 1;
    if (from_fmt && (flags & NV2A_STAGE_MIPS)) {
        /* As xemu's pgraph_get_texture_shape: the format's count, no more
         * than MAX_LOD_CLAMP + 1, none smaller than 1x1. The clamps are
         * whole levels (xemu reads the 12-bit fields as is). */
        uint32_t lu = (fmt >> 20) & 0xF, lv = (fmt >> 24) & 0xF;
        uint32_t lmax = (ctl0 >> 6) & 0xFFF, lmin = (ctl0 >> 18) & 0xFFF;
        uint32_t lv_n = (fmt >> 16) & 0xF;
        if (lv_n > lmax + 1u) lv_n = lmax + 1u;
        if (lv_n > (lu > lv ? lu : lv) + 1u) lv_n = (lu > lv ? lu : lv) + 1u;
        t->levels = lv_n ? lv_n : 1u;
        t->lod_min = lmin < t->levels - 1u ? lmin : t->levels - 1u;
        t->lod_max = lmax < t->levels - 1u ? lmax : t->levels - 1u;
    }
    t->valid = t->offset && t->width && t->height
            && (from_fmt || t->pitch)
            && (!(flags & NV2A_STAGE_P8_PALETTE)
                || t->color != NV2A_TEX_P8 || t->palette);
    if (n == 0 && !set[0x0C / 4])
        on = 1;                         /* never configured: counts as on */
    if (shader_set)
        mode = (int)((shader_prog >> (5 * n)) & 0x1F);
    else
        mode = n == 0 ? 1 : 0;
    t->raw_mode = mode;
    t->on = on;
    /* PASSTHRU and CLIPPLANE have no texture to be valid: they stay. The
     * GPU backends drop CLIPPLANE to 0 themselves (no per-pixel kill yet). */
    if (mode != 4 && mode != 5 && (!on || !t->valid))
        mode = 0;
    t->mode = mode;
}

uint32_t nv2a_prim_to_list(uint32_t prim, const uint16_t *v, uint32_t n,
                           uint32_t base, uint32_t *out,
                           enum nv2a_topology *topo)
{
    uint32_t k = 0, i;

#define PUT(x) (out[k++] = (uint32_t)(x) - base)
    switch (prim) {
    case NV_PRIM_POINTS:
        *topo = NV2A_TOPO_POINTS;
        for (i = 0; i < n; i++) PUT(v[i]);
        break;
    case NV_PRIM_LINES:
        *topo = NV2A_TOPO_LINES;
        for (i = 0; i + 1 < n; i += 2) { PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:
        *topo = NV2A_TOPO_LINES;
        for (i = 0; i + 1 < n; i++) { PUT(v[i]); PUT(v[i + 1]); }
        if (prim == NV_PRIM_LINE_LOOP && n > 2) { PUT(v[n - 1]); PUT(v[0]); }
        break;
    case NV_PRIM_TRIANGLES:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 2 < n; i += 3) { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) { PUT(v[i + 1]); PUT(v[i]); PUT(v[i + 2]); }
            else       { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 1; i + 1 < n; i++) { PUT(v[0]); PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_QUADS:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 3 < n; i += 4) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]);
            PUT(v[i]); PUT(v[i + 2]); PUT(v[i + 3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 3 < n; i += 2) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 3]);
            PUT(v[i]); PUT(v[i + 3]); PUT(v[i + 2]);
        }
        break;
    default:
        *topo = NV2A_TOPO_NONE;
        return 0;
    }
#undef PUT
    return k;
}

uint32_t nv2a_vsh_program_hash(const struct nv2a_pb_vsh *v, uint32_t *len)
{
    uint32_t h = 2166136261u, n = 0, pc, k;
    for (pc = v->start; pc < NV2A_VSH_SLOTS; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ v->prog[pc][k]) * 16777619u;
        n++;
        if (v->prog[pc][3] & 1u)        /* FINAL */
            break;
    }
    if (len)
        *len = n;
    return h;
}

int nv2a_blend_constant(int enable, uint32_t sfactor, uint32_t dfactor,
                        uint32_t color, float bf[4], int *mixed)
{
    uint32_t s = nv2a_blend_from_gl(sfactor), d = nv2a_blend_from_gl(dfactor);
    int col = s == NV2A_BF_CONST_COLOR || s == NV2A_BF_INV_CONST_COLOR
           || d == NV2A_BF_CONST_COLOR || d == NV2A_BF_INV_CONST_COLOR;
    int alp = s == NV2A_BF_CONST_ALPHA || s == NV2A_BF_INV_CONST_ALPHA
           || d == NV2A_BF_CONST_ALPHA || d == NV2A_BF_INV_CONST_ALPHA;

    *mixed = 0;
    if (!enable || (!col && !alp)) {
        bf[0] = bf[1] = bf[2] = bf[3] = 1.0f;
        return 0;
    }
    bf[3] = (float)(color >> 24) / 255.0f;
    *mixed = col && alp;
    if (col) {
        bf[0] = (float)((color >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((color >>  8) & 0xFF) / 255.0f;
        bf[2] = (float)( color        & 0xFF) / 255.0f;
    } else {
        bf[0] = bf[1] = bf[2] = bf[3];
    }
    return 1;
}

void nv2a_ps_consts_fill(const struct nv2a_pb_vsh *v,
                         const struct nv2a_stage st[4],
                         struct nv2a_ps_consts *pc)
{
    int n;

    memset(pc, 0, sizeof *pc);
    for (n = 0; n < 4; n++) {
        const struct nv2a_stage *t = &st[n];
        int scaled = nv2a_tex_size_from_format(t->color);
        pc->tex_mode[n] = (uint32_t)t->mode;
        pc->tex_scale[n][0] = scaled || !t->width ? 1.0f : 1.0f / (float)t->width;
        pc->tex_scale[n][1] = scaled || !t->height ? 1.0f : 1.0f / (float)t->height;
    }
    for (n = 0; n < 8; n++) {
        nv2a_argb_to_float4(v->rc_f0[n], pc->c0[n]);
        nv2a_argb_to_float4(v->rc_f1[n], pc->c1[n]);
    }
    nv2a_argb_to_float4(v->rc_sf[0], pc->fc0);
    nv2a_argb_to_float4(v->rc_sf[1], pc->fc1);
    nv2a_argb_to_float4(v->fog_color, pc->fog_color);
    {   /* the CPU path reads the fog colour register as RGBA, alpha 1 */
        float t = pc->fog_color[0];
        pc->fog_color[0] = pc->fog_color[2];
        pc->fog_color[2] = t;
        pc->fog_color[3] = 1.0f;
    }
    pc->alpha_ref = (float)v->atest_ref / 255.0f;
    pc->alpha_test_enable = v->atest_enable ? 1u : 0u;
    pc->alpha_func = nv2a_cmp_from_gl(v->atest_func) + 1;
}

void nv2a_vp_consts_fill(const struct nv2a_pb_vsh *v, uint32_t surface_format,
                         uint32_t w, uint32_t h, struct nv2a_vp_consts *o)
{
    uint32_t func, abs_, edge_one;

    o->c[0] = 2.0f / (float)w;
    o->c[1] = -2.0f / (float)h;
    o->c[2] = 1.0f / nv2a_zmax(surface_format);
    o->c[3] = 0.0f;
    o->c[4] = -1.0f;
    o->c[5] = 1.0f;
    o->c[6] = 0.0f;
    o->c[7] = 0.0f;
    /* Fog and specular, as the CPU path's vsh_fog_factor and vsh_transform
     * apply them (xemu vsh.c). */
    o->c[8] = v->fog_param[0];
    o->c[9] = v->fog_param[1];
    o->c[10] = v->fog_param[2];
    o->c[11] = 0.0f;
    memset(o->u, 0, sizeof o->u);
    if (nv2a_fog_mode(v->fog_mode, &func, &abs_, &edge_one)) {
        o->u[1] = func;
        o->u[2] = abs_;
        o->u[3] = edge_one;
        o->u[0] = v->fog_enable ? 1u : 0u;
    }
    o->u[4] = v->spec_enable ? 1u : 0u;
    o->u[5] = (v->light_ctl >> 17) & 1u;
}

void nv2a_report_write(uint8_t *mem, uint32_t va, uint32_t count)
{
    volatile uint32_t *r = (volatile uint32_t *)(mem + va);
    r[0] = 0;                              /* timestamp */
    r[1] = 0;
    r[2] = count;
    /* status: done, as a release store, so the count (and the rest of the
     * report) is visible before it on any host, arm64 included. */
    atomic_store_explicit((_Atomic uint32_t *)(mem + va + 12), 0u,
                          memory_order_release);
}

void nv2a_report_set_pending(uint8_t *mem, uint32_t va)
{
    ((volatile uint32_t *)(mem + va))[3] = 0xFFFFFFFFu;
}

void nv2a_occ_init(struct nv2a_occ *o, const struct nv2a_occ_ops *ops,
                   uint8_t *mem)
{
    memset(o, 0, sizeof *o);
    o->ops = ops;
    o->mem = mem;
    o->scale = nv2a_host_render_scale();
}

void nv2a_occ_set_scale(struct nv2a_occ *o, unsigned scale)
{
    o->scale = scale;
}

uint32_t nv2a_occ_scale_count(uint64_t samples, unsigned scale)
{
    if (scale > 1 && samples) {
        uint64_t d = (uint64_t)scale * scale;
        /* Rounded to nearest; a few samples of a sliver still mean
         * "visible", so a non-zero count never rounds to 0. */
        samples = (samples + d / 2) / d;
        if (!samples)
            samples = 1;
    }
    return samples > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)samples;
}

static void occ_begin(struct nv2a_occ *o)
{
    void *q;
    if (o->open)
        return;
    if (o->nseg >= NV2A_OCC_SEG || !(q = o->ops->get(o->ops->ctx))) {
        o->ovf = 1;
        return;
    }
    o->ops->begin(o->ops->ctx, q);
    o->seg[o->nseg++] = q;
    o->open = 1;
}

static void occ_end(struct nv2a_occ *o)
{
    if (!o->open)
        return;
    o->ops->end(o->ops->ctx, o->seg[o->nseg - 1]);
    o->open = 0;
}

/* 1 when every query of the report has its result (summed into *count). */
static int occ_ready(struct nv2a_occ *o, const struct nv2a_occ_report *p,
                     int flush, uint32_t *count)
{
    uint64_t sum = 0;
    int i;
    for (i = 0; i < p->n; i++) {
        uint64_t v = 0;
        if (!o->ops->result(o->ops->ctx, p->q[i], flush, &v))
            return 0;
        sum += v;
    }
    *count = nv2a_occ_scale_count(sum, o->scale);
    return 1;
}

static void occ_finish(struct nv2a_occ *o, struct nv2a_occ_report *p,
                       uint32_t count)
{
    int i;
    nv2a_report_write(o->mem, p->va, count);
    o->vis += count != 0;
    for (i = 0; i < p->n; i++)
        o->ops->put(o->ops->ctx, p->q[i]);
    p->n = 0;
}

void nv2a_occ_poll(struct nv2a_occ *o, int flush)
{
    uint64_t now;
    int i, j;
    if (!o->npend)
        return;
    now = o->ops->now_us(o->ops->ctx);
    for (i = j = 0; i < o->npend; i++) {
        struct nv2a_occ_report *p = &o->pend[i];
        uint32_t count;
        if (p->overflow) {
            occ_finish(o, p, NV2A_OCC_VISIBLE);
        } else if (occ_ready(o, p, flush, &count)) {
            occ_finish(o, p, count);
        } else if (now - p->t0 > NV2A_OCC_LATE_US) {
            o->late++;
            occ_finish(o, p, NV2A_OCC_VISIBLE);
        } else {
            o->pend[j++] = *p;
        }
    }
    o->npend = j;
}

void nv2a_occ_zpass(struct nv2a_occ *o, int enable)
{
    o->en = enable;
    if (enable)
        occ_begin(o);
    else
        occ_end(o);
}

void nv2a_occ_zpass_clear(struct nv2a_occ *o)
{
    int i;
    occ_end(o);
    for (i = 0; i < o->nseg; i++)
        o->ops->put(o->ops->ctx, o->seg[i]);   /* ended; Begin on reuse drops it */
    o->nseg = 0;
    o->ovf = 0;
    if (o->en)
        occ_begin(o);
}

void nv2a_occ_report(struct nv2a_occ *o, uint32_t va, int sync)
{
    struct nv2a_occ_report *p;
    int i;

    occ_end(o);
    /* Supersede a report still pending at this va before any poll: a poll
     * here could finish the old one over the title's reset. */
    for (i = 0; i < o->npend; i++)
        if (o->pend[i].va == va) {
            int k;
            for (k = 0; k < o->pend[i].n; k++)
                o->ops->put(o->ops->ctx, o->pend[i].q[k]);
            memmove(&o->pend[i], &o->pend[i + 1],
                    (size_t)(o->npend - i - 1) * sizeof o->pend[0]);
            o->npend--;
            o->super++;
            break;              /* at most one: each push supersedes */
        }
    nv2a_occ_poll(o, 0);
    if (o->npend >= NV2A_OCC_PENDING)
        nv2a_occ_poll(o, 1);
    if (o->npend >= NV2A_OCC_PENDING) {
        /* Still full: the oldest is answered as visible. */
        o->late++;
        occ_finish(o, &o->pend[0], NV2A_OCC_VISIBLE);
        memmove(&o->pend[0], &o->pend[1],
                (size_t)(--o->npend) * sizeof o->pend[0]);
    }
    /* Pending until done, whatever the title left in the status. */
    nv2a_report_set_pending(o->mem, va);
    p = &o->pend[o->npend++];
    p->va = va;
    p->n = o->nseg;
    p->overflow = o->ovf;
    p->t0 = o->ops->now_us(o->ops->ctx);
    for (i = 0; i < o->nseg; i++)
        p->q[i] = o->seg[i];
    /* The report owns them now; a report with no clear before the next one
     * counts from here (the XDK always clears first). */
    o->nseg = 0;
    o->ovf = 0;
    o->reports++;
    if (sync) {
        uint64_t t = o->ops->now_us(o->ops->ctx);
        uint32_t count;
        while (!occ_ready(o, p, 1, &count)
               && o->ops->now_us(o->ops->ctx) - t < NV2A_OCC_LATE_US)
            ;
        o->sync_us += o->ops->now_us(o->ops->ctx) - t;
        nv2a_occ_poll(o, 1);
    }
    if (o->en)
        occ_begin(o);
}

/* ---- Host options (enhancements) ------------------------------------------ */

static struct nv2a_host_opts s_host_opts;   /* zero: stock */

void nv2a_host_opts_set(const struct nv2a_host_opts *o)
{
    s_host_opts = *o;
}

const struct nv2a_host_opts *nv2a_host_opts(void)
{
    return &s_host_opts;
}

unsigned nv2a_host_render_scale(void)
{
    return s_host_opts.render_scale > 1 ? s_host_opts.render_scale : 1;
}

unsigned nv2a_host_size(uint32_t w, uint32_t h, unsigned scale, uint32_t max_dim,
                        uint32_t *hw, uint32_t *hh)
{
    unsigned n = scale > 1 ? scale : 1;
    while (n > 1 && ((uint64_t)w * n > max_dim || (uint64_t)h * n > max_dim))
        n--;
    *hw = w * n;
    *hh = h * n;
    return n;
}

int nv2a_present_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int filter, struct nv2a_rect *r)
{
    uint64_t fw, fh;

    r->x = r->y = r->w = r->h = 0;
    if (!src_w || !src_h || !dst_w || !dst_h)
        return 0;
    if (filter == NV2A_PRESENT_INTEGER) {
        uint32_t k = dst_w / src_w, kh = dst_h / src_h;
        if (kh < k)
            k = kh;
        if (k >= 1) {
            r->w = (int32_t)(src_w * k);
            r->h = (int32_t)(src_h * k);
            r->x = (int32_t)(dst_w - (uint32_t)r->w) / 2;
            r->y = (int32_t)(dst_h - (uint32_t)r->h) / 2;
            return 1;
        }
    }
    /* Fit: the axis that limits gets the whole output, the other the
     * frame's aspect, rounded to the nearest pixel. */
    if ((uint64_t)dst_w * src_h <= (uint64_t)dst_h * src_w) {
        fw = dst_w;
        fh = ((uint64_t)dst_w * src_h + src_w / 2) / src_w;
    } else {
        fh = dst_h;
        fw = ((uint64_t)dst_h * src_w + src_h / 2) / src_h;
    }
    r->w = (int32_t)fw;
    r->h = (int32_t)fh;
    r->x = (int32_t)((dst_w - fw) / 2);
    r->y = (int32_t)((dst_h - fh) / 2);
    return 0;
}

void nv2a_upscale_nearest32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                            unsigned n, uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t x, y;
    unsigned i, j;
    for (y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(const void *)(src + (size_t)y * src_pitch);
        uint32_t *d = (uint32_t *)(void *)(dst + (size_t)y * n * dst_pitch);
        for (x = 0; x < w; x++)
            for (i = 0; i < n; i++)
                d[(size_t)x * n + i] = s[x];
        for (j = 1; j < n; j++)
            memcpy(dst + ((size_t)y * n + j) * dst_pitch, d, (size_t)w * n * 4u);
    }
}

void nv2a_downscale_box32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                          unsigned n, uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t x, y, nn = n * n, half = nn / 2;
    unsigned i, j, c;
    for (y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * dst_pitch;
        for (x = 0; x < w; x++) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (j = 0; j < n; j++) {
                const uint8_t *s = src + ((size_t)y * n + j) * src_pitch + (size_t)x * n * 4u;
                for (i = 0; i < n; i++, s += 4)
                    for (c = 0; c < 4; c++)
                        sum[c] += s[c];
            }
            for (c = 0; c < 4; c++)
                d[(size_t)x * 4u + c] = (uint8_t)((sum[c] + half) / nn);
        }
    }
}
