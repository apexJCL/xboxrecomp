/**
 * pb_vsh_cgen: the vertex-constant write generation.
 *
 * The D3D11 backend skips its 3 KB per-batch compare of c[] while
 * nv2a_pb_vsh_consts_gen() is unchanged since its last upload, so a write to
 * c[] that does not move the generation would leave stale constants on the
 * GPU. Each kind of c[] writer is fed here and must move it: the
 * TRANSFORM_CONSTANT window, the viewport offset and scale (mirrored into
 * c[58] and c[59]) and the fixed-function matrices. A method that does not
 * touch c[] (blend enable, the program window) must leave it, or the skip
 * never fires. Values are checked through nv2a_pb_vsh_state() too, so the
 * test also fails if a writer moves the generation but writes elsewhere.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

static int s_fail;

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void expect(const char *what, int ok)
{
    fprintf(stderr, "[pb_vsh_cgen] %-56s %s\n", what, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

int main(void)
{
    static uint8_t xbe[0x1000];
    const struct nv2a_pb_vsh *v;
    uint32_t g;
    int k;

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_vsh_cgen] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);
    v = nv2a_pb_vsh_state();

    g = nv2a_pb_vsh_consts_gen();
    M(0x0304, 1);                          /* SET_BLEND_ENABLE */
    M(0x0B00, 0x12345678u);                /* SET_TRANSFORM_PROGRAM[0] */
    M(0x1EA4, 106);                        /* CONSTANT_LOAD: slot 106, no write yet */
    expect("blend, program word and constant load: unchanged",
           nv2a_pb_vsh_consts_gen() == g);

    for (k = 0; k < 4; k++) {
        g = nv2a_pb_vsh_consts_gen();
        M(0x0B80 + 4 * k, f2u(2.0f + (float)k));   /* SET_TRANSFORM_CONSTANT */
        if (nv2a_pb_vsh_consts_gen() == g)
            break;
    }
    expect("TRANSFORM_CONSTANT: every component moves it", k == 4);
    expect("TRANSFORM_CONSTANT: lands in slot 106",
           v->c[106][0] == 2.0f && v->c[106][3] == 5.0f);

    g = nv2a_pb_vsh_consts_gen();
    M(0x0A20, f2u(320.0f));                /* SET_VIEWPORT_OFFSET x */
    expect("VIEWPORT_OFFSET: moves it",
           nv2a_pb_vsh_consts_gen() != g && v->c[0x3B][0] == 320.0f);

    g = nv2a_pb_vsh_consts_gen();
    M(0x0AF4, f2u(-240.0f));               /* SET_VIEWPORT_SCALE y */
    expect("VIEWPORT_SCALE: moves it",
           nv2a_pb_vsh_consts_gen() != g && v->c[0x3A][1] == -240.0f);

    g = nv2a_pb_vsh_consts_gen();
    M(0x0480, f2u(7.0f));                  /* SET_MODEL_VIEW_MATRIX0[0] */
    expect("fixed-function matrix: moves it", nv2a_pb_vsh_consts_gen() != g);

    if (s_fail) {
        fprintf(stderr, "[pb_vsh_cgen] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[pb_vsh_cgen] done: all checks pass\n");
    return 0;
}
