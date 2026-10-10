/*
 * enhance_cfg test: the TOML subset (recomp_cfg.c), its errors with line
 * numbers, and the env > title file > root file > default layering
 * (enhance_cfg.c), xbox_enhance_init (enhance.c) and recomp_exe_dir. Runs in its working directory, where it writes small
 * files under ecfg_root/.
 */
#include "enhance.h"
#include "enhance_cfg.h"
#include "nv2a_backend_common.h"
#include "recomp_cfg.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

/* ── the subset ────────────────────────────────────────────────────── */

static void test_subset(void)
{
    static const char text[] =
        "\xEF\xBB\xBF# enhancements\n"
        "title = \"Example\"   # trailing comment\n"
        "\n"
        "[render]\n"
        "scale = 2\n"
        "big = 1_000_000\n"
        "neg = -42\n"
        "hex = 0xFF_FF\n"
        "oct = 0o17\n"
        "bin = 0b101\n"
        "sharp = 0.5\n"
        "exp = 1e3\n"
        "nexp = -2.5E-2\n"
        "huge = inf\n"
        "notnum = nan\n"
        "filter = 'C:\\path\\raw'\n"
        "esc = \"a\\tb\\n\\\"q\\\" \\u00e9\"\n"
        "\"quoted-key\" = true\n"
        "vsync = false\n"
        "\r\n"
        "[game]\n"
        "mode = \"fast\"\n"
        "\n"
        "[aspect.program]\n"
        "a1b2c3 = \"hud\"\n"
        "\n"
        "[display]\n"
        "aspect = \"16:9\"\n"
        "sizes = [1, 2, 3]\n"
        "mixed = [ \"x\", 2.5, true, ]  # trailing comma\n"
        "multi = [\n"
        "  \"one\",   # first\n"
        "  \"two\"\n"
        "]\n"
        "empty = []\n"
        "dotted.inner = 7\n";
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(text, "subset", err, sizeof err);
    static const char *const modes[] = { "stock", "fast", "free", NULL };

    CHECK(c != NULL);
    if (!c) { fprintf(stderr, "%s\n", err); return; }
    CHECK(!strcmp(recomp_cfg_string(c, "title", ""), "Example"));
    CHECK(recomp_cfg_int(c, "render.scale", 0) == 2);
    CHECK(recomp_cfg_int(c, "render.big", 0) == 1000000);
    CHECK(recomp_cfg_int(c, "render.neg", 0) == -42);
    CHECK(recomp_cfg_int(c, "render.hex", 0) == 0xFFFF);
    CHECK(recomp_cfg_int(c, "render.oct", 0) == 15);
    CHECK(recomp_cfg_int(c, "render.bin", 0) == 5);
    CHECK(recomp_cfg_float(c, "render.sharp", 0) == 0.5);
    CHECK(recomp_cfg_float(c, "render.exp", 0) == 1000.0);
    CHECK(fabs(recomp_cfg_float(c, "render.nexp", 0) + 0.025) < 1e-12);
    CHECK(isinf(recomp_cfg_float(c, "render.huge", 0)));
    CHECK(isnan(recomp_cfg_float(c, "render.notnum", 0)));
    CHECK(recomp_cfg_float(c, "render.scale", 0) == 2.0);       /* int as float */
    CHECK(!strcmp(recomp_cfg_string(c, "render.filter", ""), "C:\\path\\raw"));
    CHECK(!strcmp(recomp_cfg_string(c, "render.esc", ""), "a\tb\n\"q\" \xC3\xA9"));
    CHECK(recomp_cfg_bool(c, "render.quoted-key", 0) == 1);
    CHECK(recomp_cfg_bool(c, "render.vsync", 1) == 0);
    CHECK(recomp_cfg_choice(c, "game.mode", modes, -1) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "aspect.program.a1b2c3", ""), "hud"));
    CHECK(!strcmp(recomp_cfg_string(c, "display.aspect", ""), "16:9"));
    CHECK(recomp_cfg_int(c, "display.dotted.inner", 0) == 7);

    /* defaults: missing key, wrong type, NULL config */
    CHECK(recomp_cfg_int(c, "render.missing", 9) == 9);
    CHECK(recomp_cfg_int(c, "render.sharp", 9) == 9);
    CHECK(recomp_cfg_bool(c, "render.scale", 1) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "render.scale", "d"), "d"));
    CHECK(recomp_cfg_type_of(c, "render") == RCFG_NONE);
    CHECK(recomp_cfg_int(NULL, "render.scale", 3) == 3);
    CHECK(recomp_cfg_choice(c, "display.aspect", modes, 0) == 0);

    /* arrays */
    CHECK(recomp_cfg_type_of(c, "display.sizes") == RCFG_ARRAY);
    CHECK(recomp_cfg_array_len(c, "display.sizes") == 3);
    CHECK(recomp_cfg_array_int(c, "display.sizes", 2, 0) == 3);
    CHECK(recomp_cfg_array_int(c, "display.sizes", 3, -1) == -1);
    CHECK(recomp_cfg_array_len(c, "display.mixed") == 3);
    CHECK(!strcmp(recomp_cfg_array_string(c, "display.mixed", 0, ""), "x"));
    CHECK(recomp_cfg_array_float(c, "display.mixed", 1, 0) == 2.5);
    CHECK(recomp_cfg_array_bool(c, "display.mixed", 2, 0) == 1);
    CHECK(recomp_cfg_array_type(c, "display.mixed", 1) == RCFG_FLOAT);
    CHECK(recomp_cfg_array_len(c, "display.multi") == 2);
    CHECK(!strcmp(recomp_cfg_array_string(c, "display.multi", 1, ""), "two"));
    CHECK(recomp_cfg_type_of(c, "display.empty") == RCFG_ARRAY);
    CHECK(recomp_cfg_array_len(c, "display.empty") == 0);
    CHECK(recomp_cfg_array_len(c, "render.scale") == 0);

    /* iteration and lines */
    CHECK(!strcmp(recomp_cfg_key_at(c, 0), "title"));
    CHECK(recomp_cfg_line(c, "render.scale") == 5);
    CHECK(recomp_cfg_line(c, "game.mode") == 22);
    CHECK(recomp_cfg_line(c, "display.dotted.inner") == 36);
    CHECK(recomp_cfg_find(c, "nope") == -1);
    recomp_cfg_free(c);

    c = recomp_cfg_load_string("", "empty", err, sizeof err);
    CHECK(c && recomp_cfg_count(c) == 0);
    recomp_cfg_free(c);
    c = recomp_cfg_load_string("# only a comment", "comment", err, sizeof err);
    CHECK(c && recomp_cfg_count(c) == 0);
    recomp_cfg_free(c);
}

/* ── errors carry the line ─────────────────────────────────────────── */

static void expect_error(const char *text, const char *want, int line)
{
    char err[256], prefix[32];
    recomp_cfg *c = recomp_cfg_load_string(text, "e", err, sizeof err);
    s_checks++;
    snprintf(prefix, sizeof prefix, "e:%d: ", line);
    if (c || strncmp(err, prefix, strlen(prefix)) || !strstr(err, want)) {
        s_fail++;
        fprintf(stderr, "FAIL error for %s\n  want \"%s%s\"\n  got  \"%s\"\n",
                text, prefix, want, c ? "(parsed)" : err);
    }
    recomp_cfg_free(c);
}

static void test_errors(void)
{
    expect_error("a = 1\nb = \n", "expected a value", 2);
    expect_error("a = 1\na = 2\n", "duplicate key 'a' (first set on line 1)", 2);
    expect_error("[r]\nx = 1\n[r]\n", "table [r] defined twice", 3);
    expect_error("x = 1\n[x]\n", "'x' is a value", 2);
    expect_error("x = 1\nx.y = 2\n", "'x' is a value", 2);
    expect_error("[t.a]\nx = 1\n[t]\na = 2\n", "'t.a' is a table", 4);
    expect_error("x.y = 1\nx = 2\n", "'x' is a table", 2);
    expect_error("s = \"open\n", "unterminated string", 1);
    expect_error("\n\ns = \"\"\"multi\"\"\"\n", "multi-line strings", 3);
    expect_error("t = {a = 1}\n", "inline tables", 1);
    expect_error("[[arr]]\n", "arrays of tables", 1);
    expect_error("a = [[1]]\n", "nested arrays", 1);
    expect_error("d = 1979-05-27T07:32:00\n", "dates and times", 1);
    expect_error("f = linear\n", "strings need quotes", 1);
    expect_error("n = 012\n", "leading zero", 1);
    expect_error("n = 1__0\n", "misplaced '_'", 1);
    expect_error("n = 1.\n", "invalid number", 1);
    expect_error("n = 99999999999999999999\n", "out of range", 1);
    expect_error("a = 1 2\n", "expected end of line", 1);
    expect_error("a 1\n", "expected '='", 1);
    expect_error("= 1\n", "expected a key", 1);
    expect_error("[render\n", "expected ']'", 1);
    expect_error("a = [1,\n2\n", "unterminated array", 3);
    expect_error("a = [1 2]\n", "expected ',' or ']'", 1);
    expect_error("s = \"bad \\q\"\n", "bad escape", 1);
    expect_error("b = True\n", "strings need quotes", 1);

    /* strict pass (review of 409a8e8): each was accepted before */
    /* 1. '_' only between two digits */
    expect_error("n = 0x_FF\n", "misplaced '_'", 1);
    expect_error("n = 1_e5\n", "misplaced '_'", 1);
    expect_error("n = 1e_5\n", "misplaced '_'", 1);
    expect_error("n = 1_.5\n", "misplaced '_'", 1);
    expect_error("n = 1._5\n", "misplaced '_'", 1);
    expect_error("n = i_nf\n", "strings need quotes", 1);
    expect_error("n = 0_x10\n", "misplaced '_'", 1);
    expect_error("n = 0xFF_\n", "misplaced '_'", 1);
    expect_error("n = 0_1\n", "leading zero", 1);
    /* 2. one prefix, digits of its base only */
    expect_error("n = 0x0x1F\n", "invalid number", 1);
    expect_error("n = 0x\n", "invalid number", 1);
    expect_error("n = 0o8\n", "invalid number", 1);
    expect_error("n = 0b102\n", "invalid number", 1);
    expect_error("n = 0x-1\n", "invalid number", 1);
    expect_error("n = 1e\n", "invalid number", 1);
    expect_error("n = 1.5.2\n", "invalid number", 1);
    expect_error("n = 1e5e5\n", "invalid number", 1);
    expect_error("d = 1979-05-27\n", "dates and times", 1);
    /* 3. a table made by a dotted key cannot be reopened by a header, nor a
     *    header's table extended by a dotted key */
    expect_error("a.b = 1\n[a]\n", "defined by a dotted key", 2);
    expect_error("[t]\na.x = 1\n[t.a]\n", "defined by a dotted key", 3);
    expect_error("[a.b]\nx = 1\n[a]\nb.y = 2\n", "defined by a header", 4);
    /* 4. \e is TOML 1.1 only */
    expect_error("s = \"\\e[0m\"\n", "bad escape", 1);
    /* 6. DEL, control characters and invalid UTF-8 */
    expect_error("x = 1\ns = \"a\x7f\"\n", "control character in string", 2);
    expect_error("x = 1\n# bell \x07\n", "control character in comment", 2);
    expect_error("x = 1\n# del \x7f\n", "control character in comment", 2);
    expect_error("# cr \r alone\n", "control character in comment", 1);
    expect_error("x = 1\n\ns = \"\xff\"\n", "invalid UTF-8", 3);
    expect_error("s = \"\xc0\xaf\"\n", "invalid UTF-8", 1);          /* overlong '/' */
    expect_error("s = \"\xed\xa0\x80\"\n", "invalid UTF-8", 1);      /* surrogate */
    expect_error("s = \"\xf4\x90\x80\x80\"\n", "invalid UTF-8", 1); /* > U+10FFFF */
    expect_error("# \xe2\x82\n", "invalid UTF-8", 1);                  /* truncated */
    {
        char err[256];
        CHECK(recomp_cfg_load_file("ecfg_does_not_exist.toml", err, sizeof err) == NULL);
        CHECK(strstr(err, "cannot open") != NULL);
    }
}

/* What the strict pass must still accept. */
static void test_strict_valid(void)
{
    static const char text[] =
        "hex = 0xdead_BEEF\n"
        "big = 1_000.000_5e1_0\n"
        "zero = 0\nzf = 0.0\nzexp = 0e0\nneg = -0.5\npos = +1.5\n"
        "a.b = 1\n"
        "a.c = 2\n"                      /* a dotted table extended by dotted keys */
        "[x.y.z]\nk = 1\n"
        "[x]\nv = 2\n"                  /* implicit (header) table, defined later */
        "[p]\nq.r = 1\n"
        "[p.q.s]\nt = 1\n"              /* sub-table of a dotted-defined table */
        "u = \"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae\"  # \xc3\xa9 ok\n";
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(text, "valid", err, sizeof err);
    double d;
    CHECK(c != NULL);
    if (!c) { fprintf(stderr, "%s\n", err); return; }
    CHECK(recomp_cfg_int(c, "hex", 0) == 0xdeadbeefLL);
    CHECK(recomp_cfg_float(c, "big", 0) == 1000.0005e10);
    CHECK(recomp_cfg_int(c, "zero", 1) == 0);
    CHECK(recomp_cfg_float(c, "zexp", 1) == 0.0);
    CHECK(recomp_cfg_float(c, "neg", 0) == -0.5);
    CHECK(recomp_cfg_float(c, "pos", 0) == 1.5);
    CHECK(recomp_cfg_int(c, "a.c", 0) == 2);
    CHECK(recomp_cfg_int(c, "x.v", 0) == 2);
    CHECK(recomp_cfg_int(c, "p.q.s.t", 0) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "p.q.s.u", ""), "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae"));
    recomp_cfg_free(c);

    CHECK(recomp_cfg_parse_double("2.5", &d) && d == 2.5);
    CHECK(recomp_cfg_parse_double("-inf", &d) && isinf(d) && d < 0);
    CHECK(!recomp_cfg_parse_double("2,5", &d));
    CHECK(!recomp_cfg_parse_double("0x1p3", &d));
    CHECK(!recomp_cfg_parse_double(" 1", &d));
    CHECK(!recomp_cfg_parse_double("", &d));
}

/* 5. Floats do not depend on the locale (a comma-decimal locale used to
 *    make strtod stop at the '.'). Skipped when no such locale exists. */
static void test_locale(void)
{
    static const char *const names[] = { "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8",
                                         "fr_FR.utf8", "German_Germany.1252", NULL };
    int i;
    const char *got = NULL;
    for (i = 0; names[i] && !got; i++)
        got = setlocale(LC_NUMERIC, names[i]);
    if (!got || strcmp(localeconv()->decimal_point, ",") != 0) {
        printf("enhance_cfg: no comma-decimal locale here; locale check skipped\n");
        setlocale(LC_NUMERIC, "C");
        return;
    }
    {
        char err[256];
        double d = 0;
        recomp_cfg *c = recomp_cfg_load_string("f = 0.25\ng = 1.5e2\n", "loc", err, sizeof err);
        CHECK(c && recomp_cfg_float(c, "f", 0) == 0.25);
        CHECK(c && recomp_cfg_float(c, "g", 0) == 150.0);
        recomp_cfg_free(c);
        CHECK(recomp_cfg_parse_double("0.75", &d) && d == 0.75);
        CHECK(!recomp_cfg_parse_double("0,75", &d));
        recomp_env_set(RENV_RENDER_SCALE, "1.5");
        CHECK(enhance_cfg_float("render.scale", 0) == 1.5);
        recomp_env_set(RENV_RENDER_SCALE, NULL);
        printf("enhance_cfg: locale check ran under %s\n", got);
    }
    setlocale(LC_NUMERIC, "C");
}

/* ── env > title file > root file > default ────────────────────────── */

static void test_layers(void)
{
    static const char *const modes[] = { "stock", "fast", "free", NULL };
    static const char *const aspect[] = { "4:3", "16:9", "16:10", "21:9", NULL };

    MKDIR("ecfg_root");
    MKDIR("ecfg_root/TITLE");
    write_file("ecfg_root/enhance.toml",
               "[render]\nscale = 3\nfilter = \"nearest\"\n"
               "[display]\naspect = \"16:10\"\n"
               "[game]\nmode = \"free\"\n"
               "[extra]\nunread = 1\n");
    write_file("ecfg_root/TITLE/enhance.toml",
               "[render]\nscale = 2\nsharp = 0.25\nvsync = true\nmodes = [1, 2]\n"
               "[game]\nmode = \"fast\"\n"
               "[display]\nbadtype = 5\n");

    recomp_env_set(RENV_ENHANCE_CONFIG, NULL);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);

    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 2);
    /* title file over root file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_FILE);
    /* root file where the title file is silent */
    CHECK(!strcmp(enhance_cfg_string("render.filter", "linear"), "nearest"));
    CHECK(enhance_cfg_choice("display.aspect", aspect, 0) == 2);
    /* default when neither has it */
    CHECK(enhance_cfg_int("render.missing", 7) == 7);
    CHECK(enhance_cfg_source_of("render.missing") == ENHANCE_SRC_DEFAULT);
    CHECK(enhance_cfg_float("render.sharp", 1.0) == 0.25);
    CHECK(enhance_cfg_float("render.scale", 1.0) == 2.0);
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    /* wrong type in the file: the default, not a coercion */
    CHECK(!strcmp(enhance_cfg_string("display.badtype", "dflt"), "dflt"));
    {
        const recomp_cfg *c = enhance_cfg_lookup("render.modes");
        CHECK(c == enhance_cfg_file(0));
        CHECK(recomp_cfg_array_len(c, "render.modes") == 2);
        CHECK(enhance_cfg_lookup("nope") == NULL);
    }

    /* game.mode stands for a game key: unbound, the file answers; bound, env wins. */
    CHECK(enhance_cfg_choice("game.mode", modes, 0) == 1);
    CHECK(enhance_cfg_bind_env("game.mode", RENV_LOG_LEVEL));   /* any config id */
    recomp_env_set(RENV_LOG_LEVEL, "free");
    CHECK(enhance_cfg_choice("game.mode", modes, 0) == 2);
    CHECK(enhance_cfg_source_of("game.mode") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_LOG_LEVEL, "bogus");                    /* invalid: file */
    CHECK(enhance_cfg_choice("game.mode", modes, 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, NULL);

    /* env over both files */
    recomp_env_set(RENV_RENDER_SCALE, "4");
    CHECK(enhance_cfg_int("render.scale", 1) == 4);
    CHECK(enhance_cfg_float("render.scale", 1) == 4.0);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_RENDER_SCALE, "0x2");
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, "two");                   /* not a number: file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, "");                      /* empty = unset */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, "21:9");
    CHECK(enhance_cfg_choice("display.aspect", aspect, 0) == 3);
    CHECK(!strcmp(enhance_cfg_string("display.aspect", ""), "21:9"));
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);

    /* bool spellings from env */
    CHECK(enhance_cfg_bind_env("render.vsync", RENV_LOG_LEVEL));
    recomp_env_set(RENV_LOG_LEVEL, "Off");
    CHECK(enhance_cfg_bool("render.vsync", 1) == 0);
    recomp_env_set(RENV_LOG_LEVEL, "yes");
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, "maybe");                    /* file: true */
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, NULL);

    /* only extra.unread was never read */
    CHECK(enhance_cfg_report_unused() == 1);

    /* real environment, read through recomp_env */
    set_env("RECOMP_RENDER_SCALE", "5");
    recomp_env_reload();
    CHECK(enhance_cfg_int("render.scale", 1) == 5);
    set_env("RECOMP_RENDER_SCALE", NULL);
    recomp_env_reload();
    CHECK(enhance_cfg_int("render.scale", 1) == 2);

    /* RECOMP_ENHANCE_CONFIG: one file, or none */
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_root/enhance.toml");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 1);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 0);
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
    CHECK(enhance_cfg_file(0) == NULL && enhance_cfg_file(1) == NULL);
    recomp_env_set(RENV_RENDER_SCALE, "2");                     /* env with no file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_root/missing.toml");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == -1);
    recomp_env_set(RENV_ENHANCE_CONFIG, NULL);

    /* no title file: root only; a broken title file is skipped whole */
    CHECK(enhance_cfg_init("ecfg_root", "OTHER") == 1);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    write_file("ecfg_root/TITLE/enhance.toml", "[render]\nscale = 2\nscale = 3\n");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == -1);
    CHECK(enhance_cfg_file(0) == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    enhance_cfg_shutdown();
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
}

/* ── xbox_enhance_init: the toolkit's keys into nv2a_host_opts ─────── */

/* The kernel's pacing switch (kernel_pacing.c), which this test does not
 * link: record what the layer hands it. */
static int s_pacing_mode = -1;
void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { s_pacing_mode = mode; }

static void enhance_reset_env(void)
{
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    recomp_env_set(RENV_PRESENT_FILTER, NULL);
    recomp_env_set(RENV_PRESENT_FULLSCREEN, NULL);
    recomp_env_set(RENV_PRESENT_PACING, NULL);
}

static void test_enhance_init(void)
{
    const struct nv2a_host_opts *o = nv2a_host_opts();

    /* No file, no env: stock. */
    enhance_reset_env();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    CHECK(o->render_scale == 1 && o->present_filter == NV2A_PRESENT_NEAREST && o->fullscreen == 0);
    CHECK(nv2a_host_render_scale() == 1);

    /* The file answers. */
    write_file("ecfg_init.toml",
               "[render]\nscale = 2\n[present]\nfilter = \"integer\"\nfullscreen = true\n");
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_init.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0);
    CHECK(o->render_scale == 2 && o->present_filter == NV2A_PRESENT_INTEGER && o->fullscreen == 1);

    /* Env over file, every key; bool spellings. */
    recomp_env_set(RENV_RENDER_SCALE, "3");
    recomp_env_set(RENV_PRESENT_FILTER, "linear");
    recomp_env_set(RENV_PRESENT_FULLSCREEN, "off");
    CHECK(xbox_enhance_init(".", NULL) == 0);
    CHECK(o->render_scale == 3 && o->present_filter == NV2A_PRESENT_LINEAR && o->fullscreen == 0);
    recomp_env_set(RENV_PRESENT_FULLSCREEN, "Yes");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->fullscreen == 1);

    /* An invalid filter is reported and the file answers (integer); with
     * no file, nearest. */
    recomp_env_set(RENV_PRESENT_FILTER, "bicubic");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->present_filter == NV2A_PRESENT_INTEGER);
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->present_filter == NV2A_PRESENT_NEAREST);
    write_file("ecfg_bad.toml", "[present]\nfilter = \"sharp\"\n");
    recomp_env_set(RENV_PRESENT_FILTER, NULL);
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_bad.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->present_filter == NV2A_PRESENT_NEAREST);

    /* Scale out of range clamps to 1..4. */
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    recomp_env_set(RENV_RENDER_SCALE, "9");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 4);
    recomp_env_set(RENV_RENDER_SCALE, "0");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 1);
    recomp_env_set(RENV_RENDER_SCALE, "-2");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 1);

    /* A non-4:3 aspect is reported, nothing else changes. */
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, "16:9");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 1);

    /* present.pacing: spin by default, from the file, env over file, and a
     * bad value reported with the file answering. */
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    s_pacing_mode = -1;
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 0);
    write_file("ecfg_pacing.toml", "[present]\npacing = \"sleep\"\n");
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_pacing.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 1);
    recomp_env_set(RENV_PRESENT_PACING, "spin");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 0);
    CHECK(enhance_cfg_report_unused() == 0);    /* overridden, not unused */
    recomp_env_set(RENV_PRESENT_PACING, "nap");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 1);
    recomp_env_set(RENV_PRESENT_PACING, NULL);
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    recomp_env_set(RENV_PRESENT_PACING, "sleep");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 1);
    recomp_env_set(RENV_PRESENT_PACING, NULL);

    /* A file that does not parse: -1, stock. */
    write_file("ecfg_broken.toml", "[render]\nscale = \n");
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_broken.toml");
    CHECK(xbox_enhance_init(".", NULL) == -1 && o->render_scale == 1);

    enhance_reset_env();
    recomp_env_set(RENV_ENHANCE_CONFIG, NULL);
    enhance_cfg_shutdown();
}

/* ── recomp_exe_dir ────────────────────────────────────────────────── */

static void test_exe_dir(void)
{
    const char *d = recomp_exe_dir();
    struct stat st;
    CHECK(d && *d);
    CHECK(stat(d, &st) == 0 && (st.st_mode & S_IFDIR));
    CHECK(recomp_exe_dir() == d);          /* cached */
    {
        /* This test's own binary sits in that directory. */
        char p[4096];
        FILE *f;
#ifdef _WIN32
        snprintf(p, sizeof p, "%s/enhance_cfg_test.exe", d);
#else
        snprintf(p, sizeof p, "%s/enhance_cfg_test", d);
#endif
        f = fopen(p, "rb");
        CHECK(f != NULL);
        if (f)
            fclose(f);
    }
}

int main(void)
{
    test_subset();
    test_errors();
    test_strict_valid();
    test_locale();
    test_layers();
    test_enhance_init();
    test_exe_dir();
    printf("enhance_cfg: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
