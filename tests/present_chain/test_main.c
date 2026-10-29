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
