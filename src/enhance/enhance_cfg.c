/*
 * enhance_cfg.c - the enhancements file under the environment; see
 * enhance_cfg.h.
 */
#include "enhance_cfg.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENHANCE_MAX_BINDS 64
#define ENHANCE_FILES 2   /* title, root */

typedef struct {
    char key[64];
    recomp_env_id id;
    int warned;
} Bind;

static Bind s_binds[ENHANCE_MAX_BINDS];
static int s_n_binds;
static int s_builtin_bound;

static recomp_cfg *s_file[ENHANCE_FILES];
/* Per file entry: bit 0 read by a lookup, bit 1 type warning printed. */
static unsigned char *s_flags[ENHANCE_FILES];

static void bind_builtins(void)
{
    if (s_builtin_bound)
        return;
    s_builtin_bound = 1;
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    enhance_cfg_bind_env("render.scale", RENV_RENDER_SCALE);
    enhance_cfg_bind_env("display.aspect", RENV_DISPLAY_ASPECT);
    enhance_cfg_bind_env("present.filter", RENV_PRESENT_FILTER);
    enhance_cfg_bind_env("present.fullscreen", RENV_PRESENT_FULLSCREEN);
    enhance_cfg_bind_env("present.pacing", RENV_PRESENT_PACING);
#endif
}

int enhance_cfg_bind_env(const char *key, recomp_env_id id)
{
    int i;
    if (!key || strlen(key) >= sizeof s_binds[0].key)
        return 0;
    for (i = 0; i < s_n_binds; i++)
        if (!strcmp(s_binds[i].key, key)) {
            s_binds[i].id = id;
            s_binds[i].warned = 0;
            return 1;
        }
    if (s_n_binds == ENHANCE_MAX_BINDS)
        return 0;
    strcpy(s_binds[s_n_binds].key, key);
    s_binds[s_n_binds].id = id;
    s_binds[s_n_binds].warned = 0;
    s_n_binds++;
    return 1;
}

static Bind *bind_of(const char *key)
{
    int i;
    bind_builtins();
    for (i = 0; i < s_n_binds; i++)
        if (!strcmp(s_binds[i].key, key))
            return &s_binds[i];
    return NULL;
}

/* The env value for `key`, or NULL (unbound, unset or empty). */
static int file_of(const char *key, recomp_cfg_type *type);

/* A file key the environment overrides was still read: the user set it, and
 * an "unused key" line for it would send them looking for a typo. */
static const char *env_of(const char *key, Bind **out)
{
    Bind *b = bind_of(key);
    const char *v = b ? recomp_env(b->id) : NULL;
    recomp_cfg_type t;
    *out = b;
    if (!v || !*v)
        return NULL;
    file_of(key, &t);
    return v;
}

static void env_bad(Bind *b, const char *v, const char *want)
{
    if (b->warned)
        return;
    b->warned = 1;
    fprintf(stderr, "[ENHANCE] %s=%s is not %s; ignored\n",
            recomp_env_name(b->id), v, want);
}

/* ── files ─────────────────────────────────────────────────────────── */

void enhance_cfg_shutdown(void)
{
    int i;
    for (i = 0; i < ENHANCE_FILES; i++) {
        recomp_cfg_free(s_file[i]);
        s_file[i] = NULL;
        free(s_flags[i]);
        s_flags[i] = NULL;
    }
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Load `path` into slot `i`. 1 loaded, 0 absent (when optional), -1 error. */
static int load_slot(int i, const char *path, int optional)
{
    char err[512];
    recomp_cfg *c;
    if (optional && !file_exists(path))
        return 0;
    c = recomp_cfg_load_file(path, err, sizeof err);
    if (!c) {
        fprintf(stderr, "[ENHANCE] config error: %s (file ignored)\n", err);
        return -1;
    }
    s_file[i] = c;
    s_flags[i] = (unsigned char *)calloc(recomp_cfg_count(c) + 1, 1);
    return 1;
}

int enhance_cfg_init(const char *root, const char *title)
{
    const char *over;
    char path[1024];
    int loaded = 0, bad = 0, r, i;
    size_t keys = 0;

    enhance_cfg_shutdown();
    bind_builtins();
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    over = recomp_env(RENV_ENHANCE_CONFIG);
#else
    over = NULL;
#endif
    if (over && *over) {
        if (strcmp(over, "none") != 0) {
            r = load_slot(0, over, 0);
            loaded += r > 0;
            bad |= r < 0;
        }
    } else {
        if (root && title && *title) {
            snprintf(path, sizeof path, "%s/%s/enhance.toml", root, title);
            r = load_slot(0, path, 1);
            loaded += r > 0;
            bad |= r < 0;
        }
        snprintf(path, sizeof path, "%s/enhance.toml", root && *root ? root : ".");
        r = load_slot(1, path, 1);
        loaded += r > 0;
        bad |= r < 0;
    }

    fprintf(stderr, "[ENHANCE] config: ");
    if (!s_file[0] && !s_file[1])
        fprintf(stderr, "%s", over && !strcmp(over, "none") ? "none (RECOMP_ENHANCE_CONFIG)" : "none");
    for (i = 0; i < ENHANCE_FILES; i++)
        if (s_file[i]) {
            fprintf(stderr, "%s%s", i && s_file[0] ? ", " : "", recomp_cfg_name(s_file[i]));
            keys += recomp_cfg_count(s_file[i]);
        }
    fprintf(stderr, " (%zu keys)\n", keys);
    for (i = 0; i < s_n_binds; i++) {
        const char *v = recomp_env(s_binds[i].id);
        if (v && *v)
            fprintf(stderr, "[ENHANCE] %s = %s from %s\n", s_binds[i].key, v,
                    recomp_env_name(s_binds[i].id));
    }
    return bad ? -1 : loaded;
}

const recomp_cfg *enhance_cfg_file(int i)
{
    return i >= 0 && i < ENHANCE_FILES ? s_file[i] : NULL;
}

/* The first file holding `key`: its slot, or -1. Marks the entry read. */
static int file_of(const char *key, recomp_cfg_type *type)
{
    int i;
    for (i = 0; i < ENHANCE_FILES; i++) {
        long k = recomp_cfg_find(s_file[i], key);
        if (k >= 0) {
            if (s_flags[i])
                s_flags[i][k] |= 1;
            *type = recomp_cfg_type_of(s_file[i], key);
            return i;
        }
    }
    return -1;
}

const recomp_cfg *enhance_cfg_lookup(const char *key)
{
    recomp_cfg_type t;
    int f = file_of(key, &t);
    return f < 0 ? NULL : s_file[f];
}

static void file_bad(int f, const char *key, const char *want)
{
    long k = recomp_cfg_find(s_file[f], key);
    if (!s_flags[f] || (s_flags[f][k] & 2))
        return;
    s_flags[f][k] |= 2;
    fprintf(stderr, "[ENHANCE] %s:%d: %s should be %s; ignored\n",
            recomp_cfg_name(s_file[f]), recomp_cfg_line(s_file[f], key), key, want);
}

/* ── lookups ───────────────────────────────────────────────────────── */

const char *enhance_cfg_string(const char *key, const char *def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v)
        return v;
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_STRING) {
        file_bad(f, key, "a string");
        return def;
    }
    return recomp_cfg_string(s_file[f], key, def);
}

long long enhance_cfg_int(const char *key, long long def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v) {
        char *end;
        long long n;
        errno = 0;
        n = strtoll(v, &end, 0);
        if (end != v && !*end && errno != ERANGE)
            return n;
        env_bad(b, v, "an integer");
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_INT) {
        file_bad(f, key, "an integer");
        return def;
    }
    return recomp_cfg_int(s_file[f], key, def);
}

double enhance_cfg_float(const char *key, double def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v) {
        double d;
        if (recomp_cfg_parse_double(v, &d))
            return d;
        env_bad(b, v, "a number");
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_FLOAT && t != RCFG_INT) {
        file_bad(f, key, "a number");
        return def;
    }
    return recomp_cfg_float(s_file[f], key, def);
}

static int word_is(const char *v, const char *w)
{
    for (; *v && *w; v++, w++)
        if (tolower((unsigned char)*v) != *w)
            return 0;
    return !*v && !*w;
}

int enhance_cfg_bool(const char *key, int def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v) {
        if (word_is(v, "1") || word_is(v, "true") || word_is(v, "yes") || word_is(v, "on"))
            return 1;
        if (word_is(v, "0") || word_is(v, "false") || word_is(v, "no") || word_is(v, "off"))
            return 0;
        env_bad(b, v, "a bool (1/0, true/false, yes/no, on/off)");
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_BOOL) {
        file_bad(f, key, "true or false");
        return def;
    }
    return recomp_cfg_bool(s_file[f], key, def);
}

int enhance_cfg_choice(const char *key, const char *const *choices, int def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    char want[256];
    size_t n = 0;
    int i, f;

    want[0] = 0;
    for (i = 0; choices && choices[i] && n + 1 < sizeof want; i++) {
        int w = snprintf(want + n, sizeof want - n, "%s%s", i ? "|" : "one of ", choices[i]);
        if (w < 0)
            break;
        n += (size_t)w;
    }
    if (v) {
        for (i = 0; choices && choices[i]; i++)
            if (!strcmp(choices[i], v))
                return i;
        env_bad(b, v, want);
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    i = recomp_cfg_choice(s_file[f], key, choices, -1);
    if (i < 0) {
        file_bad(f, key, want);
        return def;
    }
    return i;
}

enhance_cfg_source enhance_cfg_source_of(const char *key)
{
    Bind *b;
    int i;
    if (env_of(key, &b))
        return ENHANCE_SRC_ENV;
    for (i = 0; i < ENHANCE_FILES; i++)
        if (recomp_cfg_find(s_file[i], key) >= 0)
            return ENHANCE_SRC_FILE;
    return ENHANCE_SRC_DEFAULT;
}

int enhance_cfg_report_unused(void)
{
    int i, n = 0;
    size_t k;
    for (i = 0; i < ENHANCE_FILES; i++)
        for (k = 0; k < recomp_cfg_count(s_file[i]); k++) {
            const char *key = recomp_cfg_key_at(s_file[i], k);
            int used = s_flags[i] ? s_flags[i][k] & 1 : 1, j;
            /* A key the title file overrides is used even if the root's
             * copy was never read. */
            for (j = 0; !used && j < i; j++)
                used = recomp_cfg_find(s_file[j], key) >= 0;
            if (!used) {
                fprintf(stderr, "[ENHANCE] unused key %s, %s:%d\n", key,
                        recomp_cfg_name(s_file[i]), recomp_cfg_line(s_file[i], key));
                n++;
            }
        }
    return n;
}
