/*
 * mod.c - see mod.h. The tables are filled during init on the main thread,
 * before the guest and the render thread start, and only read after: the
 * frame and draw paths take no lock. Shutdown leaves them as they are: the
 * guest's thread or the render thread may be inside a callback then, and
 * the filter pointer (nv2a_batch_filter_set) is what stops new asks.
 */
#include "mod.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_actions.h"
#include "enhance_cfg.h"
#ifdef XBOXRECOMP_ENHANCE
#include "nv2a_overlay.h"
#endif
#include "enhance_menu.h"
#include "recomp_cfg.h"
#include "recomp_exit_hook.h"
#include "xbox_memory_layout.h"

#define LOGP "[MOD] "

struct frame_cb { xbox_mod_frame_fn fn; void *user; int owner; };
struct filter_cb { xbox_mod_batch_fn fn; void *user; int owner; };
struct item_ref { const struct enhance_item *it; int owner; };

#define MOD_MAX 64
#define PENDING_NONE (-1)

static const struct xbox_mod *const *s_table;
static const struct xbox_mod *s_running[MOD_MAX];
static int s_nrunning;
static int s_init_owner = -1;       /* the table index whose init is running */

static struct frame_cb s_frame[XBOX_MOD_MAX_FRAME];
static int s_nframe;
static struct filter_cb s_filter[XBOX_MOD_MAX_FILTER];
static int s_nfilter;

static struct item_ref s_items[XBOX_MOD_MAX_ITEMS];
static int s_nitems;

/* Per table index, for the Mods page. s_want and s_pending are written by
 * the menu (the poll or flip thread) and read by the guest's thread. */
static int s_count;                         /* table entries, up to MOD_MAX */
static enum xbox_mod_state s_state[MOD_MAX];
static int s_started[MOD_MAX];              /* 1 when its init ran and held */
static int s_want[MOD_MAX];
static int s_pending[MOD_MAX];              /* PENDING_NONE, 0 or 1 */
static int s_any_pending;                   /* 1 once a toggle waits: the stock frame skips the scan */
static void (*s_on_enable[MOD_MAX])(int on, void *user);
static void *s_on_enable_user[MOD_MAX];
static char s_key[MOD_MAX][80];             /* "mods.<name>" */

/* A descriptor mod's resolved state (mod.h, "the descriptor"). The values
 * are doubles stored as their bits in a uint64_t, so a relaxed atomic load
 * reads one without a lock on every host (api->setting, any thread). */
#define SET_MAX     64      /* settings and controls per descriptor */
#define KEY_MAX     96
/* A setting's or control's full key: under enhance_cfg's env binding
 * (Bind.key[64]) and the menu's toggle key, so an env row can never be
 * dropped for length. */
#define DESC_KEY_MAX 64
#define CTL_BINDS   ENHANCE_ACTION_BINDS

enum {
    SRC_ENV = 0, SRC_USER, SRC_USER_ALIAS, SRC_DEFAULTS, SRC_DEFAULTS_ALIAS,
    SRC_DEFAULT, SRC_MENU
};
static const char *const s_src_name[] = {
    "env", "user", "user_alias", "defaults", "defaults_alias", "default", "menu"
};

struct set_rt {
    uint64_t cur;               /* what the mod reads */
    uint64_t want;              /* the menu's value; a RESTART one only moves this */
    int src;                    /* SRC_* */
    int forced;                 /* an env row set it: read-only */
    int pending;                /* on_setting queued for the next frame */
    int env;                    /* recomp_env id, or -1 */
    char key[KEY_MAX];          /* the full key */
};

struct ctl_rt {
    int action;                 /* ACTION: the id, else -1 */
    int src;                    /* SRC_*: BUTTON and STICK */
    int env;
    int n;
    char key[KEY_MAX];          /* "input.controls.<id>.<ctl>" */
    char bind[CTL_BINDS][ENHANCE_ACTION_TEXT];
};

struct mod_rt {
    struct xbox_mod_desc d;     /* the descriptor, fields past its size zeroed */
    int has_desc;               /* m->desc was given */
    int valid;                  /* it passed validation */
    int enable;                 /* the enable setting's index, or -1 */
    const char *outcome;        /* "yes", "dormant", "no", "failed" */
    char reason[160];
    struct set_rt *set;
    struct ctl_rt *ctl;
    void (*on_setting)(uint32_t idx, double value, void *user);
    void *on_setting_user;
    int arm_idx;                /* the setting actions_arm named, or -1 */
    enum xbox_mod_state start;  /* the state init left it in */
};

static struct mod_rt s_mod[MOD_MAX];
static double ld_val(const uint64_t *p);
static int s_menu_on;               /* menu.enabled at init: the dormant rule */

/* The pending rewrites (D4): each user-file alias that won its row. */
#define RW_MAX 64
static struct { char key[KEY_MAX], value[256], old[KEY_MAX]; } s_rw[RW_MAX];
static int s_nrw;

static int s_shut;                  /* 1 once shutdown has run */
static int s_exit_hooked;

static struct xbox_mod_api s_api;

static void api_log(const struct xbox_mod *m, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[MOD %s] ", m && m->name ? m->name : "?");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* The "[MOD <name>] toast:" line, which names the mod, then the overlay's
 * TOAST layer (its own "[TOAST]" line; on screen only when it is armed,
 * which a feature key does). */
static void api_toast(const struct xbox_mod *m, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[MOD %s] toast: %s\n", m && m->name ? m->name : "?", line);
#ifdef XBOXRECOMP_ENHANCE
    nv2a_overlay_toast("%s", line);
#endif
}

static int api_action_register(const char *name, const char *label,
                               const char *const *defaults)
{
    if (s_init_owner < 0)
        return -1;
    return enhance_action_register_owned(name, label, defaults, s_init_owner);
}

static int api_actions_arm(const char *key)
{
    int i = s_init_owner;

    if (i < 0)
        return enhance_actions_armed();
    /* A descriptor mod's own switch: its value is the setting's (a choice
     * text "on" is no bool to enhance_cfg_bool), and a live switch from
     * the menu re-arms or drops it (switch_live). */
    for (uint32_t k = 0; key && s_mod[i].valid && k < s_mod[i].d.n_settings; k++) {
        const struct xbox_mod_setting *s = &s_mod[i].d.settings[k];
        if ((s->kind == XBOX_SET_BOOL || s->kind == XBOX_SET_ENUM) &&
            !strcmp(s_mod[i].set[k].key, key)) {
            s_mod[i].arm_idx = (int)k;
            return enhance_actions_arm_set(key, i, ld_val(&s_mod[i].set[k].cur) != 0);
        }
    }
    return enhance_actions_arm_owned(key, i);
}

static int api_on_frame(xbox_mod_frame_fn fn, void *user)
{
    if (s_init_owner < 0 || !fn || s_nframe >= XBOX_MOD_MAX_FRAME)
        return 0;
    s_frame[s_nframe++] = (struct frame_cb){ fn, user, s_init_owner };
    return 1;
}

static int api_batch_filter(xbox_mod_batch_fn fn, void *user)
{
    if (s_init_owner < 0 || !fn || s_nfilter >= XBOX_MOD_MAX_FILTER)
        return 0;
    s_filter[s_nfilter++] = (struct filter_cb){ fn, user, s_init_owner };
    return 1;
}

static int api_menu_add(const struct enhance_item *it)
{
    if (s_init_owner < 0 || !it || s_nitems >= XBOX_MOD_MAX_ITEMS)
        return 0;
    s_items[s_nitems++] = (struct item_ref){ it, s_init_owner };
    return 1;
}

static int api_on_enable(void (*fn)(int on, void *user), void *user)
{
    if (s_init_owner < 0 || s_init_owner >= MOD_MAX || !fn)
        return 0;
    s_on_enable[s_init_owner] = fn;
    s_on_enable_user[s_init_owner] = user;
    return 1;
}

/* api.mem, once the guest has mapped its memory (after xbox_enhance_init,
 * before its first frame or draw). Either the guest's thread (frames) or
 * the render thread (filters) may get there first; both store the same
 * value. A mod with only a filter never sees a frame, so both ask. */
static void mod_mem(void)
{
    if (!__atomic_load_n(&s_api.mem, __ATOMIC_ACQUIRE))
        __atomic_store_n(&s_api.mem, (uintptr_t)xbox_GetMemoryOffset(), __ATOMIC_RELEASE);
}

/* The one filter the backends see: the mods' filters ORed, in order. */
static int filter_all(const struct nv2a_batch_info *b)
{
    int i;
    mod_mem();
    for (i = 0; i < s_nfilter; i++)
        if (s_filter[i].fn(b, s_filter[i].user))
            return 1;
    return 0;
}

/* A failed init leaves nothing behind. */
static void drop_owner(int owner)
{
    int i, n;

    for (i = n = 0; i < s_nframe; i++)
        if (s_frame[i].owner != owner)
            s_frame[n++] = s_frame[i];
    s_nframe = n;
    for (i = n = 0; i < s_nfilter; i++)
        if (s_filter[i].owner != owner)
            s_filter[n++] = s_filter[i];
    s_nfilter = n;
    for (i = n = 0; i < s_nitems; i++)
        if (s_items[i].owner != owner)
            s_items[n++] = s_items[i];
    s_nitems = n;
    if (owner >= 0 && owner < MOD_MAX) {
        s_on_enable[owner] = NULL;
        s_on_enable_user[owner] = NULL;
        s_mod[owner].on_setting = NULL;
        s_mod[owner].on_setting_user = NULL;
        s_mod[owner].arm_idx = -1;
    }
    enhance_actions_drop_owner(owner);
}

static void exit_hook(const char *why)
{
    (void)why;
    xbox_mod_shutdown();
}

static void at_exit(void)
{
    xbox_mod_shutdown();
}

/* ---- numbers in the C locale ----------------------------------------------
 *
 * A config file and the dump are read on other machines: "0.5" in any
 * locale, never "0,5". The shortest text that reads back to the same
 * double, so 0.05 is "0.05" and not 0.050000000000000003. */
int xbox_mod_num_text(char *out, size_t n, double v)
{
    const struct lconv *lc = localeconv();
    const char *dp = lc && lc->decimal_point && *lc->decimal_point ? lc->decimal_point : ".";
    size_t dl = strlen(dp);
    char tmp[64];
    double back;
    int prec;

    if (!out || !n)
        return 0;
    for (prec = 15; prec <= 17; prec++) {
        char *p;
        snprintf(tmp, sizeof tmp, "%.*g", prec, v);
        if (dl != 1 || *dp != '.') {
            p = strstr(tmp, dp);
            if (p) {
                *p = '.';
                memmove(p + 1, p + dl, strlen(p + dl) + 1);
            }
        }
        if (!isfinite(v) || (recomp_cfg_parse_double(tmp, &back) && back == v))
            break;
    }
    snprintf(out, n, "%s", tmp);
    return (int)strlen(out);
}

static double ld_val(const uint64_t *p)
{
    uint64_t b = __atomic_load_n(p, __ATOMIC_RELAXED);
    double v;
    memcpy(&v, &b, sizeof v);
    return v;
}

static void st_val(uint64_t *p, double v)
{
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    __atomic_store_n(p, b, __ATOMIC_RELAXED);
}

/* ---- the descriptor ------------------------------------------------------- */

/* A field the mod's descriptor has: inside its size. */
#define DESC_HAS(sz, f) \
    ((sz) >= offsetof(struct xbox_mod_desc, f) + sizeof(((struct xbox_mod_desc *)0)->f))

static int n_choices(const struct xbox_mod_setting *s)
{
    int n = 0;
    while (s->choices && s->choices[n])
        n++;
    return n;
}

static int env_id(const char *name)
{
    for (int i = 0; name && i < (int)RENV_COUNT; i++) {
        const char *e = recomp_env_name((recomp_env_id)i);
        if (e && !strcmp(e, name))
            return i;
    }
    return -1;
}

/* Copy what the mod's descriptor holds into r->d, then check it (mod.h).
 * 0 with r->reason set when it is refused. */
static int desc_check(struct mod_rt *r, const struct xbox_mod *m)
{
    const struct xbox_mod_desc *in = m->desc;
    struct xbox_mod_desc *d = &r->d;
    uint32_t i, j;
    size_t sz;

    memset(d, 0, sizeof *d);
    r->enable = -1;
#define REFUSE(...) do { snprintf(r->reason, sizeof r->reason, __VA_ARGS__); return 0; } while (0)
    if (!m->name || !*m->name)
        REFUSE("the mod has no name");
    if (in->version == 0)
        REFUSE("descriptor version 0");
    sz = in->size;
    if (!DESC_HAS(sz, size))
        REFUSE("descriptor size %u is too small", (unsigned)sz);
    memcpy(d, in, sz < sizeof *d ? sz : sizeof *d);
    d->size = (uint32_t)sizeof *d;
    if (!DESC_HAS(sz, n_settings))
        d->settings = NULL, d->n_settings = 0;
    if (!DESC_HAS(sz, n_controls))
        d->controls = NULL, d->n_controls = 0;
    if (d->n_settings && !d->settings)
        REFUSE("%u settings and no table", d->n_settings);
    if (d->n_controls && !d->controls)
        REFUSE("%u controls and no table", d->n_controls);
    if (d->n_settings > SET_MAX || d->n_controls > SET_MAX)
        REFUSE("more than %d settings or controls", SET_MAX);
    for (i = 0; i < d->n_settings; i++) {
        const struct xbox_mod_setting *s = &d->settings[i];
        if (!s->name || !*s->name)
            REFUSE("setting %u has no name", i);
        for (j = 0; j < i; j++)
            if (!strcmp(d->settings[j].name, s->name))
                REFUSE("setting '%s' declared twice", s->name);
        if (s->kind > XBOX_SET_ENUM)
            REFUSE("setting '%s': unknown kind %u", s->name, s->kind);
        if (s->live != ENHANCE_LIVE && s->live != ENHANCE_LIVE_ACTIVATE &&
            s->live != ENHANCE_LIVE_RESTART)
            REFUSE("setting '%s': live class %u is not live, activate or restart",
                   s->name, s->live);
        if (s->env && env_id(s->env) < 0)
            REFUSE("setting '%s': no env row %s", s->name, s->env);
        if ((s->key ? strlen(s->key) : strlen("mods..") + strlen(m->name) + strlen(s->name))
            >= DESC_KEY_MAX)
            REFUSE("setting '%s': its key is %d bytes or longer", s->name, DESC_KEY_MAX);
        if (s->flags & XBOX_SET_ENABLE) {
            if (r->enable >= 0)
                REFUSE("a second enable setting '%s'", s->name);
            if (s->kind != XBOX_SET_BOOL && s->kind != XBOX_SET_ENUM)
                REFUSE("enable setting '%s' is not a bool or an enum", s->name);
            r->enable = (int)i;
        }
        switch (s->kind) {
        case XBOX_SET_BOOL:
            if (s->def != 0 && s->def != 1)
                REFUSE("setting '%s': default %g is not 0 or 1", s->name, s->def);
            break;
        case XBOX_SET_ENUM:
            if (!n_choices(s))
                REFUSE("enum setting '%s' has no choices", s->name);
            if (!(s->def >= 0 && s->def < n_choices(s)) || s->def != floor(s->def))
                REFUSE("enum setting '%s': default %g is not a choice", s->name, s->def);
            break;
        default:
            if (!(s->lo <= s->hi))
                REFUSE("setting '%s': lo %g > hi %g", s->name, s->lo, s->hi);
            if (!(s->step >= 0))
                REFUSE("setting '%s': step %g", s->name, s->step);
            if (!(s->def >= s->lo && s->def <= s->hi))
                REFUSE("setting '%s': default %g outside %g..%g", s->name, s->def, s->lo, s->hi);
            break;
        }
    }
    for (i = 0; i < d->n_controls; i++) {
        const struct xbox_mod_control *c = &d->controls[i];
        if (!c->id || !*c->id)
            REFUSE("control %u has no id", i);
        for (j = 0; j < i; j++)
            if (!strcmp(d->controls[j].id, c->id))
                REFUSE("control '%s' declared twice", c->id);
        if (c->kind > XBOX_CTL_INFO)
            REFUSE("control '%s': unknown kind %u", c->id, c->kind);
        if (c->env && env_id(c->env) < 0)
            REFUSE("control '%s': no env row %s", c->id, c->env);
        if (strlen("input.controls..") + strlen(m->name) + strlen(c->id) >= DESC_KEY_MAX)
            REFUSE("control '%s': its key is %d bytes or longer", c->id, DESC_KEY_MAX);
    }
#undef REFUSE
    return 1;
}

/* ---- reading a setting (D4) ---------------------------------------------- */

static const char *const s_kind_want[] = {
    "true or false", "an integer", "a number", "a string (one of the choices)"
};

static int word_is(const char *v, const char *w)
{
    for (; *v && *w; v++, w++)
        if ((*v | 0x20) != *w)
            return 0;
    return !*v && !*w;
}

/* Clamp to lo..hi, snap to lo + k * step, clamp again (the snap can step
 * past hi); an int is rounded. A value within a rounding error of the grid
 * is kept as it is, so 1.0 on a 0.05 grid from 0.05 stays 1.0. */
static double adjust(const struct xbox_mod_setting *s, double v)
{
    int nc;

    switch (s->kind) {
    case XBOX_SET_BOOL:
        return v != 0;
    case XBOX_SET_ENUM:
        nc = n_choices(s);
        v = floor(v + 0.5);
        return v < 0 ? 0 : v > nc - 1 ? nc - 1 : v;
    }
    if (isnan(v))
        v = s->def;
    if (v < s->lo)
        v = s->lo;
    if (v > s->hi)
        v = s->hi;
    if (s->step > 0) {
        double k = floor((v - s->lo) / s->step + 0.5), g = s->lo + k * s->step;
        if (fabs(g - v) > s->step * 1e-9)
            v = g;
        if (v > s->hi)
            v -= s->step;
        if (v < s->lo)
            v = s->lo;
    }
    if (s->kind == XBOX_SET_INT)
        v = floor(v + 0.5);
    return v;
}

static void adjust_line(const char *id, const struct xbox_mod_setting *s,
                        const char *raw, double used)
{
    char u[64];
    if (s->kind == XBOX_SET_ENUM)
        snprintf(u, sizeof u, "%s", s->choices[(int)used]);
    else if (s->kind == XBOX_SET_BOOL)
        snprintf(u, sizeof u, "%s", used ? "true" : "false");
    else
        xbox_mod_num_text(u, sizeof u, used);
    fprintf(stderr, LOGP "%s.%s: %s -> %s\n", id, s->name, raw, u);
}

/* The raw value of one text (env) or file row, by kind. 1 and *v when it
 * has the kind's type; an unknown enum text is the type with *bad set. */
static int from_text(const struct xbox_mod_setting *s, const char *t, double *v, int *bad)
{
    switch (s->kind) {
    case XBOX_SET_BOOL:
        if (word_is(t, "1") || word_is(t, "true") || word_is(t, "yes") || word_is(t, "on"))
            return *v = 1, 1;
        if (word_is(t, "0") || word_is(t, "false") || word_is(t, "no") || word_is(t, "off"))
            return *v = 0, 1;
        return 0;
    case XBOX_SET_ENUM:
        for (int i = 0; s->choices[i]; i++)
            if (!strcmp(s->choices[i], t))
                return *v = i, 1;
        *bad = 1;
        *v = s->def;
        return 1;
    }
    return recomp_cfg_parse_double(t, v);
}

static int from_file(const struct xbox_mod_setting *s, const recomp_cfg *c, const char *key,
                     double *v, int *bad, char *raw, size_t rawn)
{
    recomp_cfg_type t = recomp_cfg_type_of(c, key);

    switch (s->kind) {
    case XBOX_SET_BOOL:
        if (t != RCFG_BOOL)
            return 0;
        *v = recomp_cfg_bool(c, key, 0);
        snprintf(raw, rawn, "%s", *v ? "true" : "false");
        return 1;
    case XBOX_SET_ENUM:
        if (t != RCFG_STRING)
            return 0;
        snprintf(raw, rawn, "%s", recomp_cfg_string(c, key, ""));
        return from_text(s, raw, v, bad);
    }
    if (t != RCFG_INT && t != RCFG_FLOAT)
        return 0;
    if (t == RCFG_INT && s->kind == XBOX_SET_INT) {
        long long n = recomp_cfg_int(c, key, 0);
        *v = (double)n;
        snprintf(raw, rawn, "%lld", n);
    } else {
        *v = recomp_cfg_float(c, key, 0);
        xbox_mod_num_text(raw, rawn, *v);
    }
    return 1;
}

/* A TOML literal of a setting's value, for a pending rewrite. */
static void literal(const struct xbox_mod_setting *s, double v, char *out, size_t n)
{
    switch (s->kind) {
    case XBOX_SET_BOOL:
        snprintf(out, n, "%s", v ? "true" : "false");
        return;
    case XBOX_SET_INT:
        snprintf(out, n, "%lld", (long long)v);
        return;
    case XBOX_SET_ENUM:
        if (!recomp_cfg_quote(s->choices[(int)v], out, n))
            snprintf(out, n, "\"\"");
        return;
    }
    xbox_mod_num_text(out, n, v);
    if (!strpbrk(out, ".eEn"))      /* a float, not an int: 1.0 (n: inf, nan) */
        strncat(out, ".0", n - strlen(out) - 1);
}

static void rewrite_add(const char *key, const char *value, const char *old)
{
    if (s_nrw >= RW_MAX) {
        fprintf(stderr, LOGP "more than %d keys to rewrite; %s stays as it is\n", RW_MAX, old);
        return;
    }
    snprintf(s_rw[s_nrw].key, KEY_MAX, "%s", key);
    snprintf(s_rw[s_nrw].value, sizeof s_rw[s_nrw].value, "%s", value);
    snprintf(s_rw[s_nrw].old, KEY_MAX, "%s", old);
    s_nrw++;
}

static int row_src(int row)
{
    return row == ENHANCE_ROW_USER ? SRC_USER
         : row == ENHANCE_ROW_USER_ALIAS ? SRC_USER_ALIAS
         : row == ENHANCE_ROW_DEFAULTS ? SRC_DEFAULTS : SRC_DEFAULTS_ALIAS;
}

/* Resolve setting k of mod i by D4's table; a row of the wrong type logs
 * one line and the next row is read. */
static void setting_resolve(int i, uint32_t k)
{
    const char *id = s_table[i]->name;
    const struct xbox_mod_setting *s = &s_mod[i].d.settings[k];
    struct set_rt *r = &s_mod[i].set[k];
    struct enhance_cfg_hit hits[2 * 16 + 2];
    char raw[256];
    const char *env;
    double v = s->def;
    int nh, h, bad = 0, done = 0;

    if (s->key)
        snprintf(r->key, KEY_MAX, "%s", s->key);
    else
        snprintf(r->key, KEY_MAX, "mods.%s.%s", id, s->name);
    r->env = s->env ? env_id(s->env) : -1;
    r->src = SRC_DEFAULT;
    r->forced = 0;
    r->pending = 0;
    if (r->env >= 0 && !enhance_cfg_bind_env(r->key, (recomp_env_id)r->env))
        fprintf(stderr, LOGP "%s: env row %s not bound (key too long / table full)\n",
                r->key, s->env);
    /* Marks the key and every alias read in every file, env set or not. */
    nh = enhance_cfg_layered(r->key, s->aliases, hits, (int)(sizeof hits / sizeof hits[0]));
    env = enhance_cfg_env(r->key);
    if (env) {
        snprintf(raw, sizeof raw, "%s", env);
        if (from_text(s, env, &v, &bad)) {
            r->src = SRC_ENV;
            r->forced = 1;
            done = 1;
        } else {
            fprintf(stderr, LOGP "%s.%s: %s='%s' is not %s; ignored\n", id, s->name,
                    s->env, env, s_kind_want[s->kind]);
        }
    }
    for (h = 0; !done && h < nh; h++) {
        if (!from_file(s, hits[h].file, hits[h].key, &v, &bad, raw, sizeof raw)) {
            fprintf(stderr, LOGP "%s.%s: %s:%d: %s should be %s; ignored\n", id, s->name,
                    recomp_cfg_name(hits[h].file), recomp_cfg_line(hits[h].file, hits[h].key),
                    hits[h].key, s_kind_want[s->kind]);
            continue;
        }
        r->src = row_src(hits[h].row);
        done = 1;
    }
    if (!done) {
        v = s->def;
    } else if (bad) {
        /* An unknown choice: the default, and the key is left for the
         * player to fix (no rewrite of a value that was not read). */
        adjust_line(id, s, raw, v);
    } else {
        double used = adjust(s, v);
        if (used != v)
            adjust_line(id, s, raw, used);
        v = used;
        if (r->src == SRC_USER_ALIAS) {
            char lit[256];
            literal(s, v, lit, sizeof lit);
            rewrite_add(r->key, lit, hits[h - 1].key);
        }
    }
    st_val(&r->cur, v);
    st_val(&r->want, v);
}

/* A captured control's bindings ([input.controls], D3): env row, the
 * user files' key then aliases, the defaults file's, the defaults. Read
 * for the menu and the dump; the per-frame reader lands in phase D. */
static void control_resolve(int i, uint32_t k)
{
    const struct xbox_mod_control *c = &s_mod[i].d.controls[k];
    struct ctl_rt *r = &s_mod[i].ctl[k];
    struct enhance_cfg_hit hits[2 * 16 + 2];
    const char *env;
    int nh;

    r->n = 0;
    r->src = SRC_DEFAULT;
    snprintf(r->key, KEY_MAX, "input.controls.%s.%s", s_table[i]->name, c->id);
    r->env = c->env ? env_id(c->env) : -1;
    if (r->env >= 0 && !enhance_cfg_bind_env(r->key, (recomp_env_id)r->env))
        fprintf(stderr, LOGP "%s: env row %s not bound (key too long / table full)\n",
                r->key, c->env);
    nh = enhance_cfg_layered(r->key, c->aliases, hits, (int)(sizeof hits / sizeof hits[0]));
    env = enhance_cfg_env(r->key);
    if (env) {
        const char *p = env;
        r->src = SRC_ENV;
        while (*p && r->n < CTL_BINDS) {
            const char *e = strchr(p, ',');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            while (len && (*p == ' ' || *p == '\t'))
                p++, len--;
            while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
                len--;
            if (len >= ENHANCE_ACTION_TEXT)
                len = ENHANCE_ACTION_TEXT - 1;
            if (len) {
                memcpy(r->bind[r->n], p, len);
                r->bind[r->n++][len] = 0;
            }
            if (!e)
                break;
            p = e + 1;
        }
        return;
    }
    for (int h = 0; h < nh; h++) {
        const recomp_cfg *f = hits[h].file;
        const char *key = hits[h].key;
        recomp_cfg_type t = recomp_cfg_type_of(f, key);
        size_t len;
        if (t == RCFG_STRING) {
            snprintf(r->bind[r->n++], ENHANCE_ACTION_TEXT, "%s", recomp_cfg_string(f, key, ""));
        } else if (t == RCFG_ARRAY) {
            len = recomp_cfg_array_len(f, key);
            for (size_t e = 0; e < len && r->n < CTL_BINDS; e++)
                if (recomp_cfg_array_type(f, key, e) == RCFG_STRING)
                    snprintf(r->bind[r->n++], ENHANCE_ACTION_TEXT, "%s",
                             recomp_cfg_array_string(f, key, e, ""));
        } else {
            fprintf(stderr, LOGP "%s.%s: %s:%d: %s should be a list of strings; ignored\n",
                    s_table[i]->name, c->id, recomp_cfg_name(f), recomp_cfg_line(f, key), key);
            continue;
        }
        r->src = row_src(hits[h].row);
        if (r->src == SRC_USER_ALIAS) {
            char lit[256], q[64];
            size_t o = 0;
            lit[o++] = '[';
            for (int b = 0; b < r->n && o < sizeof lit - 2; b++)
                if (recomp_cfg_quote(r->bind[b], q, sizeof q))
                    o += (size_t)snprintf(lit + o, sizeof lit - o, "%s%s", b ? ", " : "", q);
            snprintf(lit + (o < sizeof lit - 1 ? o : sizeof lit - 2), 2, "]");
            rewrite_add(r->key, lit, key);
        }
        return;
    }
    for (int b = 0; c->defaults && c->defaults[b] && r->n < CTL_BINDS; b++)
        snprintf(r->bind[r->n++], ENHANCE_ACTION_TEXT, "%s", c->defaults[b]);
}

/* An ACTION control's user alias goes on the rewrite list like a
 * setting's: its bindings, as an array of strings. */
static void action_rewrite(int id)
{
    char key[KEY_MAX], name[KEY_MAX], lit[256], q[64];
    char texts[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    size_t o = 0;
    int n;

    if (enhance_action_source(id, key, sizeof key) != ENHANCE_ACT_SRC_USER_ALIAS)
        return;
    n = enhance_action_bindings(id, texts, ENHANCE_ACTION_BINDS);
    lit[o++] = '[';
    for (int b = 0; b < n && o < sizeof lit - 2; b++)
        if (recomp_cfg_quote(texts[b], q, sizeof q))
            o += (size_t)snprintf(lit + o, sizeof lit - o, "%s%s", b ? ", " : "", q);
    snprintf(lit + (o < sizeof lit - 1 ? o : sizeof lit - 2), 2, "]");
    snprintf(name, sizeof name, "input.actions.%s", enhance_action_name(id));
    rewrite_add(name, lit, key);
}

/* Settings, actions and captured controls of a valid descriptor, before
 * its init (so api->setting reads the value from init on). 0 with a
 * reason when an action is refused. */
static int desc_resolve(int i)
{
    struct mod_rt *r = &s_mod[i];
    const char *id = s_table[i]->name;
    uint32_t k;

    r->set = calloc(r->d.n_settings ? r->d.n_settings : 1, sizeof *r->set);
    r->ctl = calloc(r->d.n_controls ? r->d.n_controls : 1, sizeof *r->ctl);
    if (!r->set || !r->ctl) {
        snprintf(r->reason, sizeof r->reason, "out of memory");
        return 0;
    }
    for (k = 0; k < r->d.n_settings; k++)
        setting_resolve(i, k);
    for (k = 0; k < r->d.n_controls; k++) {
        const struct xbox_mod_control *c = &r->d.controls[k];
        struct ctl_rt *cr = &r->ctl[k];
        cr->action = -1;
        if (c->kind == XBOX_CTL_ACTION) {
            char name[KEY_MAX];
            snprintf(name, sizeof name, "%s.%s", id, c->id);
            cr->action = enhance_action_register_full(name, c->label, c->defaults, i,
                                                      c->aliases,
                                                      c->env ? env_id(c->env) : -1);
            if (cr->action < 0) {
                snprintf(r->reason, sizeof r->reason, "control '%s': action %s refused",
                         c->id, name);
                return 0;
            }
            action_rewrite(cr->action);
        } else if (c->kind != XBOX_CTL_INFO) {
            control_resolve(i, k);
        }
    }
    return 1;
}

static void desc_free(int i)
{
    free(s_mod[i].set);
    free(s_mod[i].ctl);
    s_mod[i].set = NULL;
    s_mod[i].ctl = NULL;
}

/* The mod whose descriptor this is: the mod's own pointer, or the copy
 * xbox_mod_desc_of hands out. */
static int mod_of(const struct xbox_mod_desc *desc)
{
    for (int i = 0; desc && i < s_count; i++)
        if (s_mod[i].valid && (s_table[i]->desc == desc || &s_mod[i].d == desc))
            return i;
    return -1;
}

static double api_setting(const struct xbox_mod_desc *desc, uint32_t idx)
{
    int i = mod_of(desc);
    if (i < 0 || idx >= s_mod[i].d.n_settings)
        return 0;
    return ld_val(&s_mod[i].set[idx].cur);
}

static int api_on_setting(const struct xbox_mod_desc *desc,
                          void (*fn)(uint32_t idx, double value, void *user), void *user)
{
    int i = mod_of(desc);
    if (s_init_owner < 0 || i != s_init_owner || !fn)
        return 0;
    s_mod[i].on_setting = fn;
    s_mod[i].on_setting_user = user;
    return 1;
}

static int api_control_action(const struct xbox_mod_desc *desc, uint32_t idx)
{
    return xbox_mod_control_action(mod_of(desc), idx);
}

/* Until the recorded pad state lands (phase D) no source is read. */
static int api_control_value(const struct xbox_mod_desc *desc, uint32_t idx,
                             struct xbox_ctl_value *out)
{
    (void)desc;
    (void)idx;
    if (out)
        memset(out, 0, sizeof *out);
    return 0;
}

void xbox_mod_register_table(const struct xbox_mod *const *mods)
{
    s_table = mods;
}

/* An enable setting's value as its [ENHANCE] line spells it. */
static const char *enable_text(int i)
{
    const struct xbox_mod_setting *s = &s_mod[i].d.settings[s_mod[i].enable];
    double v = ld_val(&s_mod[i].set[s_mod[i].enable].cur);
    if (s->kind == XBOX_SET_ENUM)
        return s->choices[(int)v];
    return v ? "on" : "off";
}

static int enable_on(int i)
{
    return s_mod[i].enable >= 0 && ld_val(&s_mod[i].set[s_mod[i].enable].cur) != 0;
}

/* The init rule (D4), from the descriptor only: a mod registers on_enable
 * from inside init, so the rule cannot ask for it. 1 to init, 2 to init
 * dormant, 0 not to. */
static int init_rule(int i)
{
    const struct xbox_mod_setting *e;

    if (!s_want[i])
        return 0;
    if (s_mod[i].enable < 0 || enable_on(i))
        return 1;
    e = &s_mod[i].d.settings[s_mod[i].enable];
    /* Off, but the menu can turn it on live: init now, so it can. */
    return s_menu_on && e->live == ENHANCE_LIVE ? 2 : 0;
}

static int mod_init(int describe)
{
    int i;

    for (i = 0; i < s_count; i++)
        desc_free(i);
    s_nrunning = s_nframe = s_nfilter = s_nitems = s_count = 0;
    s_any_pending = 0;
    s_nrw = 0;
    s_shut = 0;
    memset(s_on_enable, 0, sizeof s_on_enable);
    memset(s_on_enable_user, 0, sizeof s_on_enable_user);
    memset(s_mod, 0, sizeof s_mod);
    memset(&s_api, 0, sizeof s_api);
    enhance_actions_drop_mods();
    if (!s_table || !s_table[0]) {
        if (!describe)
            nv2a_batch_filter_set(NULL);
        return 0;   /* no lines: a title without mods logs as before */
    }

    s_api.version = XBOX_MOD_API_VERSION;
    s_api.size = sizeof s_api;
    s_api.cfg_string = enhance_cfg_string;
    s_api.cfg_int = enhance_cfg_int;
    s_api.cfg_float = enhance_cfg_float;
    s_api.cfg_bool = enhance_cfg_bool;
    s_api.cfg_choice = enhance_cfg_choice;
    s_api.cfg_bind_env = enhance_cfg_bind_env;
    s_api.log = api_log;
    s_api.on_frame = api_on_frame;
    s_api.input_state = enhance_input_state;
    s_api.input_capture = enhance_input_capture;
    s_api.batch_filter = api_batch_filter;
    s_api.action_register = api_action_register;
    s_api.action_pressed = enhance_action_pressed;
    s_api.action_down = enhance_action_down;
    s_api.toast = api_toast;
    s_api.actions_arm = api_actions_arm;
    s_api.menu_add = api_menu_add;
    s_api.on_enable = api_on_enable;
    s_api.setting = api_setting;
    s_api.on_setting = api_on_setting;
    s_api.control_action = api_control_action;
    s_api.control_value = api_control_value;
    s_menu_on = enhance_cfg_bool("menu.enabled", 0);

    for (i = 0; s_table[i]; i++) {
        const struct xbox_mod *m = s_table[i];
        const char *name = m->name ? m->name : "?";
        struct mod_rt *r;
        int rc = 0, held = enhance_input_captured(), rule;

        if (i >= MOD_MAX) {
            fprintf(stderr, LOGP "%s: more than %d mods; left out\n", name, MOD_MAX);
            continue;
        }
        r = &s_mod[i];
        s_count = i + 1;
        s_started[i] = 0;
        s_pending[i] = PENDING_NONE;
        r->enable = -1;
        r->arm_idx = -1;
        snprintf(s_key[i], sizeof s_key[i], "mods.%s", name);
        /* The Mods page's key: a mod switched off there is not even inited
         * at the next start, so it registers nothing and costs nothing. */
        s_want[i] = enhance_cfg_bool(s_key[i], 1);
        r->has_desc = m->desc != NULL;
        if (r->has_desc) {
            if (!desc_check(r, m) || !desc_resolve(i)) {
                fprintf(stderr, LOGP "%s: descriptor refused: %s; left out\n", name, r->reason);
                /* That line names the fault: its keys reported as unknown
                 * at shutdown too would only be noise (review F7). */
                {
                    char pre[KEY_MAX];
                    snprintf(pre, sizeof pre, "mods.%s", name);
                    enhance_cfg_mark_read_prefix(pre);
                    snprintf(pre, sizeof pre, "input.controls.%s", name);
                    enhance_cfg_mark_read_prefix(pre);
                }
                enhance_actions_drop_owner(i);
                r->valid = 0;
                r->outcome = "failed";
                s_state[i] = XBOX_MOD_FAILED;
                continue;
            }
            r->valid = 1;
            /* The golden reads the switch from this line, so it is printed
             * whether or not the mod is inited (D2), and it prints what the
             * run uses: mods."<id>" = false holds the mod off whatever its
             * switch says. An enum's off is index 0,
             * as xbox_mod_set_enabled writes it. */
            if (r->enable >= 0 && !s_want[i] && enable_on(i)) {
                const struct xbox_mod_setting *e = &r->d.settings[r->enable];
                fprintf(stderr, "[ENHANCE] %s=%s (held off by %s)\n", r->set[r->enable].key,
                        e->kind == XBOX_SET_ENUM ? e->choices[0] : "off", s_key[i]);
            } else if (r->enable >= 0) {
                fprintf(stderr, "[ENHANCE] %s=%s\n", r->set[r->enable].key, enable_text(i));
            }
        }
        rule = r->valid ? init_rule(i) : s_want[i];
        if (!rule) {
            r->outcome = "no";
            s_state[i] = r->start = XBOX_MOD_OFF;
            if (!s_want[i])
                fprintf(stderr, LOGP "%s: off (%s)\n", name, s_key[i]);
            /* A v2 mod's keys are its own business: none is a typo. A
             * descriptor mod's declared keys are read already, and the
             * rest are reported (D4, "unknown keys"). */
            if (!r->valid)
                enhance_cfg_mark_read_prefix(m->name);
            /* Registered and dormant: their bindings keep their inputs.
             * Called for every mod not inited, v2 included; one with no
             * registered actions has nothing to mark, so it is a no-op. */
            enhance_actions_set_dormant(i, 1);
            continue;
        }
        if (rule == 2)
            enhance_actions_set_dormant(i, 1);
        if (describe) {
            /* The dump says what this run would do and runs no mod code. */
            r->outcome = rule == 2 ? "dormant" : "yes";
            s_state[i] = r->start = rule == 2 ? XBOX_MOD_OFF : XBOX_MOD_ON;
            continue;
        }
        s_init_owner = i;
        if (m->init)
            rc = m->init(&s_api);
        s_init_owner = -1;
        if (rc) {
            drop_owner(i);
            /* Back to what the earlier mods left, either way. */
            enhance_input_capture(held);
            fprintf(stderr, LOGP "%s: init returned %d; left out\n", name, rc);
            snprintf(r->reason, sizeof r->reason, "init returned %d", rc);
            r->outcome = "failed";
            s_state[i] = r->start = XBOX_MOD_FAILED;
            continue;
        }
        s_running[s_nrunning++] = m;
        s_started[i] = 1;
        if (rule == 2) {
            r->outcome = "dormant";
            s_state[i] = r->start = XBOX_MOD_OFF;
            if (!s_on_enable[i])
                fprintf(stderr, LOGP "%s: inited dormant but gave no on_enable; turning"
                        " it on applies at restart\n", name);
        } else {
            r->outcome = "yes";
            s_state[i] = r->start = XBOX_MOD_ON;
        }
    }
    if (describe)
        return 0;

    /* Installed last, so a filter a failed init asked for is never seen. */
    nv2a_batch_filter_set(s_nfilter ? filter_all : NULL);
    if (s_nrunning && !s_exit_hooked) {
        s_exit_hooked = 1;
        atexit(at_exit);
        if (!recomp_exit_hook_add(exit_hook))
            fprintf(stderr, LOGP "no exit hook: a window's exit skips the shutdowns\n");
    }

    fprintf(stderr, LOGP "%d running:", s_nrunning);
    for (i = 0; i < s_nrunning; i++)
        fprintf(stderr, " %s", s_running[i]->name ? s_running[i]->name : "?");
    fprintf(stderr, " (%d frame, %d filter)\n", s_nframe, s_nfilter);
    return s_nrunning;
}

int xbox_mod_init(void)
{
    return mod_init(0);
}

int xbox_mod_init_describe(void)
{
    mod_init(1);
    return s_count;
}

void xbox_mod_frame(void)
{
    int i;

    if (!s_nrunning)
        return;
    mod_mem();
    if (__atomic_load_n(&s_shut, __ATOMIC_ACQUIRE))
        return;
    /* The Mods page's toggles and settings, on the thread the mods' frames
     * run on. */
    if (__atomic_load_n(&s_any_pending, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&s_any_pending, 0, __ATOMIC_ACQ_REL)) {
        /* Cleared before the scan: a change stored during it sets the flag
         * again and is seen next frame at the latest. */
        for (i = 0; i < s_count; i++) {
            int on = __atomic_exchange_n(&s_pending[i], PENDING_NONE, __ATOMIC_ACQ_REL);
            if (on != PENDING_NONE && s_on_enable[i])
                s_on_enable[i](on, s_on_enable_user[i]);
            for (uint32_t k = 0; s_mod[i].valid && k < s_mod[i].d.n_settings; k++)
                if (__atomic_exchange_n(&s_mod[i].set[k].pending, 0, __ATOMIC_ACQ_REL) &&
                    s_mod[i].on_setting && s_started[i])
                    s_mod[i].on_setting(k, ld_val(&s_mod[i].set[k].cur),
                                        s_mod[i].on_setting_user);
        }
    }
    if (!s_nframe)
        return;
    for (i = 0; i < s_nframe; i++)
        s_frame[i].fn(s_frame[i].user);
}

int xbox_mod_count(void)
{
    return s_count;
}

const char *xbox_mod_name(int i)
{
    if (i < 0 || i >= s_count || !s_table)
        return NULL;
    return s_table[i]->name ? s_table[i]->name : "?";
}

enum xbox_mod_state xbox_mod_state(int i)
{
    if (i < 0 || i >= s_count)
        return XBOX_MOD_FAILED;
    return __atomic_load_n(&s_state[i], __ATOMIC_RELAXED);
}

const char *xbox_mod_state_name(enum xbox_mod_state st)
{
    switch (st) {
    case XBOX_MOD_ON: return "on";
    case XBOX_MOD_OFF: return "off";
    case XBOX_MOD_FAILED: return "failed";
    case XBOX_MOD_RESTART: return "restart";
    }
    return "?";
}

int xbox_mod_has_on_enable(int i)
{
    return i >= 0 && i < s_count && s_started[i] && s_on_enable[i];
}

int xbox_mod_enabled(int i)
{
    if (i < 0 || i >= s_count)
        return 0;
    if (s_mod[i].valid && s_mod[i].enable >= 0)
        return __atomic_load_n(&s_want[i], __ATOMIC_RELAXED) &&
               ld_val(&s_mod[i].set[s_mod[i].enable].want) != 0;
    return __atomic_load_n(&s_want[i], __ATOMIC_RELAXED);
}

/* A live switch of a started mod: on_enable at the next frame, its actions
 * live or dormant now, and its arm key (actions_arm on its enable setting)
 * following them. */
static enum xbox_mod_state switch_live(int i, int on)
{
    __atomic_store_n(&s_pending[i], on, __ATOMIC_RELEASE);
    __atomic_store_n(&s_any_pending, 1, __ATOMIC_RELEASE);
    if (s_mod[i].valid) {
        enhance_actions_set_dormant(i, !on);
        if (s_mod[i].arm_idx >= 0)
            enhance_actions_arm_set(s_mod[i].set[s_mod[i].arm_idx].key, i, on);
    }
    return on ? XBOX_MOD_ON : XBOX_MOD_OFF;
}

enum xbox_mod_state xbox_mod_set_enabled(int i, int on)
{
    enum xbox_mod_state st;

    if (i < 0 || i >= s_count)
        return XBOX_MOD_FAILED;
    on = on != 0;
    if (s_mod[i].valid && s_mod[i].enable >= 0) {
        /* The Enabled row is the enable setting; turning it on also lifts a
         * stale mods."<id>" = false (D4). */
        double v = ld_val(&s_mod[i].set[s_mod[i].enable].want);
        if (on)
            __atomic_store_n(&s_want[i], 1, __ATOMIC_RELAXED);
        xbox_mod_setting_set(i, (uint32_t)s_mod[i].enable, on ? (v ? v : 1) : 0);
        return xbox_mod_state(i);
    }
    __atomic_store_n(&s_want[i], on, __ATOMIC_RELAXED);
    if (s_state[i] == XBOX_MOD_FAILED)
        return XBOX_MOD_FAILED;     /* the key still changes, for the next start */
    if (xbox_mod_has_on_enable(i))
        st = switch_live(i, on);
    else if (on == s_started[i])
        st = on ? XBOX_MOD_ON : XBOX_MOD_OFF;
    else
        st = XBOX_MOD_RESTART;
    __atomic_store_n(&s_state[i], st, __ATOMIC_RELAXED);
    return st;
}

int xbox_mod_items(int i, const struct enhance_item **out, int max)
{
    int k, n = 0;

    for (k = 0; k < s_nitems; k++)
        if (s_items[k].owner == i) {
            if (n < max && out)
                out[n] = s_items[k].it;
            n++;
        }
    return n < max ? n : max;
}

/* ---- descriptor mods ------------------------------------------------------ */

static const struct set_rt *set_of(int i, uint32_t idx)
{
    if (i < 0 || i >= s_count || !s_mod[i].valid || idx >= s_mod[i].d.n_settings)
        return NULL;
    return &s_mod[i].set[idx];
}

const struct xbox_mod_desc *xbox_mod_desc_of(int i)
{
    return i >= 0 && i < s_count && s_mod[i].valid ? &s_mod[i].d : NULL;
}

int xbox_mod_has_desc(int i)
{
    return i >= 0 && i < s_count && s_mod[i].has_desc;
}

const char *xbox_mod_fail_reason(int i)
{
    return i >= 0 && i < s_count ? s_mod[i].reason : "";
}

double xbox_mod_setting_value(int i, uint32_t idx)
{
    const struct set_rt *r = set_of(i, idx);
    return r ? ld_val(&r->want) : 0;
}

const char *xbox_mod_setting_source(int i, uint32_t idx)
{
    const struct set_rt *r = set_of(i, idx);
    return r ? s_src_name[__atomic_load_n(&r->src, __ATOMIC_RELAXED)] : "default";
}

int xbox_mod_setting_forced(int i, uint32_t idx)
{
    const struct set_rt *r = set_of(i, idx);
    return r && r->forced;
}

const char *xbox_mod_setting_key(int i, uint32_t idx)
{
    const struct set_rt *r = set_of(i, idx);
    return r ? r->key : NULL;
}

const char *xbox_mod_setting_env(int i, uint32_t idx)
{
    const struct set_rt *r = set_of(i, idx);
    return r && r->env >= 0 ? recomp_env_name((recomp_env_id)r->env) : NULL;
}

double xbox_mod_setting_set(int i, uint32_t idx, double v)
{
    struct set_rt *r = (struct set_rt *)set_of(i, idx);
    const struct xbox_mod_setting *s;
    int live;

    if (!r)
        return 0;
    if (r->forced)
        return ld_val(&r->want);
    s = &s_mod[i].d.settings[idx];
    v = adjust(s, v);
    st_val(&r->want, v);
    __atomic_store_n(&r->src, SRC_MENU, __ATOMIC_RELAXED);
    if ((int)idx == s_mod[i].enable) {
        enum xbox_mod_state st;
        int on = v != 0;
        if (s_state[i] == XBOX_MOD_FAILED)
            return v;
        live = s->live == ENHANCE_LIVE && xbox_mod_has_on_enable(i);
        if (live) {
            st_val(&r->cur, v);
            st = switch_live(i, on);
        } else if (on == (s_mod[i].start == XBOX_MOD_ON)) {
            st = s_mod[i].start;    /* back to what this run started with */
        } else {
            st = XBOX_MOD_RESTART;
        }
        __atomic_store_n(&s_state[i], st, __ATOMIC_RELAXED);
        return v;
    }
    if (s->live == ENHANCE_LIVE_RESTART)
        return v;       /* applies at the next start; the mod never hears of it */
    st_val(&r->cur, v);
    __atomic_store_n(&r->pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_any_pending, 1, __ATOMIC_RELEASE);
    return v;
}

const char *xbox_mod_init_outcome(int i)
{
    return i >= 0 && i < s_count && s_mod[i].outcome ? s_mod[i].outcome : "no";
}

int xbox_mod_enable_setting(int i)
{
    return i >= 0 && i < s_count && s_mod[i].valid ? s_mod[i].enable : -1;
}

int xbox_mod_control_action(int i, uint32_t idx)
{
    if (i < 0 || i >= s_count || !s_mod[i].valid || idx >= s_mod[i].d.n_controls)
        return -1;
    return s_mod[i].ctl[idx].action;
}

int xbox_mod_control_bindings(int i, uint32_t idx, char out[][ENHANCE_ACTION_TEXT], int max,
                              const char **src)
{
    const struct ctl_rt *c;
    int n = 0;

    if (src)
        *src = "default";
    if (i < 0 || i >= s_count || !s_mod[i].valid || idx >= s_mod[i].d.n_controls)
        return 0;
    c = &s_mod[i].ctl[idx];
    if (c->action >= 0) {
        if (src)
            *src = enhance_action_source_name(enhance_action_source(c->action, NULL, 0));
        return enhance_action_bindings(c->action, out, max);
    }
    if (src)
        *src = s_src_name[c->src];
    for (; n < c->n && n < max; n++)
        snprintf(out[n], ENHANCE_ACTION_TEXT, "%s", c->bind[n]);
    return n;
}

int xbox_mod_pending_rewrites(const char **keys, const char **values,
                              const char **olds, int max)
{
    int k;

    for (k = 0; k < s_nrw && k < max; k++) {
        if (keys)
            keys[k] = s_rw[k].key;
        if (values)
            values[k] = s_rw[k].value;
        if (olds)
            olds[k] = s_rw[k].old;
    }
    return s_nrw;
}

int xbox_mod_input_captured(void)
{
    return enhance_input_captured();
}

void xbox_mod_shutdown(void)
{
    int i;

    if (__atomic_exchange_n(&s_shut, 1, __ATOMIC_ACQ_REL))
        return;
    /* The render thread may still be drawing: it stops asking first. The
     * tables stay, so a callback already running reads what it read. */
    nv2a_batch_filter_set(NULL);
    for (i = s_nrunning - 1; i >= 0; i--)
        if (s_running[i]->shutdown)
            s_running[i]->shutdown();
    enhance_input_capture(0);
}
