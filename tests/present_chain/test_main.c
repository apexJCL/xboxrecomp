/*
 * present_chain - see CMakeLists.txt. One case per process: the loader
 * remembers its first answer for the session, as the runtime wants.
 *
 *   present_chain_test CASE MODULE_DIR
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "librashader.h"
#include "nv2a_present_chain.h"
#include "platform/recomp_exe_dir.h"

static int s_fail;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            s_fail = 1;                                                              \
        }                                                                            \
    } while (0)

static void requested_is(const char *want)
{
    char b[64];
    nv2a_present_chain_requested(b, sizeof b);
    if (strcmp(b, want)) {
        fprintf(stderr, "FAIL requested \"%s\", want \"%s\"\n", b, want);
        s_fail = 1;
    }
}

static void case_request(void)
{
    unsigned g;

    CHECK(nv2a_present_chain_gen() == 0);
    CHECK(!nv2a_present_chain_wanted());
    requested_is("none");

    nv2a_present_chain_request("crt-lottes");
    g = nv2a_present_chain_gen();
    CHECK(g == 1);
    CHECK(nv2a_present_chain_wanted());
    requested_is("crt-lottes");

    /* A failure for an older generation leaves a newer request alone. */
    nv2a_present_chain_request("fsr");
    nv2a_present_chain_failed(g);
    CHECK(nv2a_present_chain_wanted());
    requested_is("fsr");

    /* The current one turns the chain off until the next request. */
    nv2a_present_chain_failed(nv2a_present_chain_gen());
    CHECK(!nv2a_present_chain_wanted());
    requested_is("fsr");
    nv2a_present_chain_request("crt-hyllian");
    CHECK(nv2a_present_chain_wanted());

    nv2a_present_chain_request("none");
    CHECK(!nv2a_present_chain_wanted());
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_request(NULL);
    CHECK(!nv2a_present_chain_wanted());
    requested_is("none");
    nv2a_present_chain_request("");
    requested_is("none");
    CHECK(nv2a_present_chain_gen() == 7);

    /* The menu's choices are the bundled names. */
    CHECK(!strcmp(nv2a_present_chain_names[0], "crt-lottes"));
    CHECK(nv2a_present_chain_names[4] == NULL);
}

static void write_file(const char *p)
{
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("shaders = 0\n", f);
        fclose(f);
    }
}

static void case_preset(void)
{
    const char *dir = recomp_exe_dir();
    char out[1024], want[1024], sub[1024];
    unsigned g = 99;

    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 0);
    CHECK(g == 0);

    /* A bundled name with no shaders/ beside the executable: not found. */
    snprintf(want, sizeof want, "%s/shaders/crt-lottes.slangp", dir);
    remove(want);
    nv2a_present_chain_request("crt-lottes");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == -1);

    snprintf(sub, sizeof sub, "%s/shaders", dir);
    make_dir(sub);
    write_file(want);
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 1);
    CHECK(!strcmp(out, want));
    CHECK(g == nv2a_present_chain_gen());

    /* A path relative to the executable, then an absolute one. */
    nv2a_present_chain_request("shaders/crt-lottes.slangp");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 1);
    CHECK(!strcmp(out, want));
    nv2a_present_chain_request(want);
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 1);
    CHECK(!strcmp(out, want));

    /* A .slangp name without a directory is a path, not a bundled name. */
    nv2a_present_chain_request("no-such.slangp");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == -1);
    nv2a_present_chain_request("no-such-preset");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == -1);
    remove(want);
}

/* The loader refuses the library: said once, and it stays refused. */
static void refused(const char *path)
{
    char e[64];
    nv2a_present_chain_set_lib(path);
    CHECK(nv2a_present_chain_lib() == NULL);
    CHECK(nv2a_present_chain_lib() == NULL);
    nv2a_present_chain_error(NULL, e, sizeof e);
    CHECK(!strcmp(e, "unknown error"));
}

static void module_path(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/fake_lrs_%s%s", dir, name, MOD_SUFFIX);
}

static int near(float got, float want)
{
    return got > want - 0.05f && got < want + 0.05f;
}

/* The frame-rate uniform follows the presents, not the display. */
static void case_rate(void)
{
    const uint64_t ms = 1000000ull;
    uint64_t t = 5000 * ms;
    float r = 0;
    int i;

    CHECK(near(nv2a_present_chain_rate(t), 60.0f));    /* first present */
    for (i = 0; i < 64; i++)
        r = nv2a_present_chain_rate(t += 33333333ull);  /* lock30 */
    CHECK(near(r, 30.0f));
    r = nv2a_present_chain_rate(t += 50 * ms);          /* one late present */
    CHECK(r > 25.0f && r < 30.0f);
    CHECK(near(nv2a_present_chain_rate(t += 2000 * ms), 60.0f));  /* a pause */
    for (i = 0; i < 64; i++)
        r = nv2a_present_chain_rate(t += 16666667ull);
    CHECK(near(r, 60.0f));
    CHECK(near(nv2a_present_chain_rate(t), 60.0f));     /* a clock that did not move */
}

static void load_params_lib(const char *dir)
{
    char p[1024];
    module_path(p, sizeof p, dir, "params");
    nv2a_present_chain_set_lib(p);
    CHECK(nv2a_present_chain_lib() != NULL);
}

/* What the backend's set_param saw, for params_apply. */
static struct { char name[64]; float v; } s_set[8];
static int s_nset;

static void record_set(void *ctx, const char *name, float v)
{
    CHECK(ctx == &s_nset);
    if (s_nset < 8) {
        snprintf(s_set[s_nset].name, sizeof s_set[0].name, "%s", name);
        s_set[s_nset].v = v;
    }
    s_nset++;
}

static float set_value(const char *name)
{
    for (int i = 0; i < s_nset && i < 8; i++)
        if (!strcmp(s_set[i].name, name))
            return s_set[i].v;
    return -99.0f;
}

static int override_row(const char *key, const char *name, float *v, int *has, int *dirty)
{
    char k[64], n[64];
    for (int i = 0; nv2a_present_chain_override_at(i, k, sizeof k, n, sizeof n, v, has, dirty); i++)
        if (!strcmp(k, key) && !strcmp(n, name))
            return 1;
    return 0;
}

/* The store: the table a backend publishes, the overrides the config and
 * the menu set, what Save writes, and what the backend applies. */
static void case_params(const char *dir)
{
    struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    char key[64];
    unsigned applied = 0, g, pg;
    int preset = 0, has = 0, dirty = 0;
    float v = 0;

    load_params_lib(dir);
    /* Config at start: one for the preset that will run, one for another. */
    nv2a_present_chain_override_load("crt-lottes", "maskDark", 0.8f);
    nv2a_present_chain_override_load("fsr", "FSR_SHARPENING", 0.1f);
    CHECK(nv2a_present_chain_overrides_dirty() == 0);
    CHECK(nv2a_present_chain_override_get("crt-lottes", "maskDark", &v) && near(v, 0.8f));

    /* No chain yet: nothing is active. */
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);
    CHECK(key[0] == 0);
    CHECK(!nv2a_present_chain_param_set("warpX", 0.0f));
    CHECK(!nv2a_present_chain_param_get("warpX", &v));

    /* A publish for an older request is dropped. */
    nv2a_present_chain_request("crt-lottes");
    g = nv2a_present_chain_gen();
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_params_publish(&preset, g);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);

    pg = nv2a_present_chain_params_gen();
    nv2a_present_chain_params_publish(&preset, nv2a_present_chain_gen());
    CHECK(nv2a_present_chain_params_gen() != pg);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 2);
    CHECK(!strcmp(key, "crt-lottes"));
    CHECK(!strcmp(t[0].name, "warpX") && near(t[0].initial, 0.01f) && near(t[0].max, 0.125f));
    CHECK(!strcmp(t[1].desc, "Lottes maskDark"));

    /* The backend applies every row: the config's override, the preset's
     * value for the rest. Nothing again until the generation moves. */
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 2);
    CHECK(near(set_value("maskDark"), 0.8f) && near(set_value("warpX"), 0.01f));
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 0);

    /* The menu sets a value: clamped to the range, live, unsaved. */
    CHECK(nv2a_present_chain_param_set("warpX", 9.0f));
    CHECK(nv2a_present_chain_param_get("warpX", &v) && near(v, 0.125f));
    CHECK(!nv2a_present_chain_param_set("noSuch", 1.0f));
    CHECK(nv2a_present_chain_overrides_dirty() == 1);
    CHECK(override_row("crt-lottes", "warpX", &v, &has, &dirty) && has && dirty);
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 2 && near(set_value("warpX"), 0.125f));

    /* Back to the preset's value (float steps add up): no override, so
     * nothing to save for warpX. */
    nv2a_present_chain_param_set("warpX", 0.0100001f);
    CHECK(override_row("crt-lottes", "warpX", &v, &has, &dirty) && !has && !dirty);
    CHECK(nv2a_present_chain_overrides_dirty() == 0);

    /* Removing a loaded override is a change Save writes (as a delete). */
    nv2a_present_chain_param_set("maskDark", 0.5f);
    CHECK(override_row("crt-lottes", "maskDark", &v, &has, &dirty) && !has && dirty);
    CHECK(nv2a_present_chain_overrides_dirty() == 1);
    nv2a_present_chain_overrides_saved();
    CHECK(nv2a_present_chain_overrides_dirty() == 0);

    /* A hand-edited value outside the range runs clamped. */
    nv2a_present_chain_override_load("crt-lottes", "maskDark", -3.0f);
    CHECK(nv2a_present_chain_param_get("maskDark", &v) && near(v, 0.0f));

    /* Reset to preset drops this preset's overrides, not another's. */
    nv2a_present_chain_param_set("warpX", 0.05f);
    nv2a_present_chain_params_reset("crt-lottes");
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "maskDark", NULL));
    CHECK(nv2a_present_chain_override_get("fsr", "FSR_SHARPENING", &v) && near(v, 0.1f));
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(near(set_value("maskDark"), 0.5f) && near(set_value("warpX"), 0.01f));
    /* Reset to defaults drops every preset's. */
    nv2a_present_chain_params_reset(NULL);
    CHECK(!nv2a_present_chain_override_get("fsr", "FSR_SHARPENING", NULL));

    /* The chain goes away: no table, and the generation moves. */
    pg = nv2a_present_chain_params_gen();
    nv2a_present_chain_params_clear();
    CHECK(nv2a_present_chain_params_gen() != pg);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);
    CHECK(!nv2a_present_chain_param_get("warpX", &v));

    /* A full table keeps what it holds and refuses the new row; once Save
     * has written the removals, the empty rows are reused. */
    nv2a_present_chain_params_reset_for_test();
    for (int i = 0; i < NV2A_PRESENT_OVERRIDES_MAX; i++) {
        char n[16];
        snprintf(n, sizeof n, "p%d", i);
        nv2a_present_chain_override_load("other", n, 1.0f);
    }
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_params_publish(&preset, nv2a_present_chain_gen());
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    CHECK(nv2a_present_chain_override_get("other", "p0", &v) && near(v, 1.0f));
    nv2a_present_chain_params_reset("other");
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    nv2a_present_chain_overrides_saved();
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(nv2a_present_chain_override_get("crt-lottes", "warpX", &v) && near(v, 0.05f));
    nv2a_present_chain_params_reset_for_test();
}

static void key_is(const char *req, const char *want)
{
    char k[64];
    nv2a_present_chain_preset_key(req, k, sizeof k);
    if (strcmp(k, want)) {
        fprintf(stderr, "FAIL key of \"%s\" is \"%s\", want \"%s\"\n", req ? req : "(null)", k,
                want);
        s_fail = 1;
    }
}

/* The config section a preset's overrides live under. */
static void case_params_key(void)
{
    key_is("crt-lottes", "crt-lottes");
    key_is("none", "");
    key_is(NULL, "");
    key_is("", "");
    key_is("C:\\mods\\My Shader.slangp", "My_Shader");
    key_is("/home/deck/shaders/crt-royale.v2.slangp", "crt-royale_v2");
    key_is("shaders/x.slangp", "x");
}

/* The dump's parse: no GPU and no chain, and the active table untouched.
 * nolib: the default library path, which is not there. */
static void case_describe(const char *dir, int nolib)
{
    struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    char ok[1024], broken[1024], key[64];

    if (nolib) {
        CHECK(nv2a_present_chain_describe("crt-lottes", t, NV2A_PRESENT_PARAMS_MAX) == -1);
        return;
    }
    load_params_lib(dir);
    snprintf(ok, sizeof ok, "%s/pc_describe_ok.slangp", recomp_exe_dir());
    snprintf(broken, sizeof broken, "%s/pc_describe_broken.slangp", recomp_exe_dir());
    write_file(ok);
    write_file(broken);
    CHECK(nv2a_present_chain_describe(ok, t, NV2A_PRESENT_PARAMS_MAX) == 2);
    CHECK(!strcmp(t[1].name, "maskDark") && near(t[1].step, 0.05f));
    CHECK(nv2a_present_chain_describe(ok, t, 1) == 1);
    CHECK(nv2a_present_chain_describe(broken, t, NV2A_PRESENT_PARAMS_MAX) == -1);
    CHECK(nv2a_present_chain_describe("no-such-preset", t, NV2A_PRESENT_PARAMS_MAX) == -1);
    CHECK(nv2a_present_chain_describe("none", t, NV2A_PRESENT_PARAMS_MAX) == -1);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);
    remove(ok);
    remove(broken);
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    const char *dir = argc > 2 ? argv[2] : ".";
    char p[1024];

    if (!strcmp(c, "request")) {
        case_request();
    } else if (!strcmp(c, "rate")) {
        case_rate();
    } else if (!strcmp(c, "preset")) {
        case_preset();
    } else if (!strcmp(c, "missing")) {
        refused("no-such-librashader" MOD_SUFFIX);
    } else if (!strcmp(c, "bad_abi") || !strcmp(c, "old_api") || !strcmp(c, "no_exports")) {
        module_path(p, sizeof p, dir, c);
        refused(p);
    } else if (!strcmp(c, "params")) {
        case_params(dir);
    } else if (!strcmp(c, "params_key")) {
        case_params_key();
    } else if (!strcmp(c, "describe") || !strcmp(c, "describe_nolib")) {
        case_describe(dir, c[8] != 0);
    } else if (!strcmp(c, "good")) {
        const struct nv2a_lrs *l;
        /* Relative to the executable's directory, where the stand-in is. */
        snprintf(p, sizeof p, "fake_lrs_good%s", MOD_SUFFIX);
        if (strcmp(dir, recomp_exe_dir()))
            module_path(p, sizeof p, dir, "good");
        nv2a_present_chain_set_lib(p);
        l = nv2a_present_chain_lib();
        CHECK(l != NULL);
        if (l) {
            CHECK(l->abi == LIBRASHADER_CURRENT_ABI);
            CHECK(l->api == LIBRASHADER_CURRENT_VERSION);
            CHECK(l->sym("libra_instance_abi_version") != NULL);
            CHECK(l->sym("libra_preset_create") == NULL);
        }
        CHECK(nv2a_present_chain_lib() == l);
    } else {
        fprintf(stderr, "unknown case \"%s\"\n", c);
        return 2;
    }
    if (!s_fail)
        printf("present_chain %s: ok\n", c);
    return s_fail;
}
