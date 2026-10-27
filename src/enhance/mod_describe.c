/*
 * mod_describe.c - the descriptor dump (mod.h; mod-settings D9):
 * RECOMP_MODS_DESCRIBE=json writes every mod's descriptor with the values
 * and bindings this run resolved, for a launcher or the CLI, before the
 * game starts and without running any mod's code.
 *
 * Schema 1. Fields are only added; one removed or changed in meaning bumps
 * "schema", since the readers ship apart from the game. A field that does
 * not apply to a setting or control is left out.
 */
#include "mod.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "enhance_menu.h"
#include "recomp_env.h"

#define LOGP "[MOD] "

/* A JSON string: quotes, backslash and control characters escaped, other
 * bytes as they are (the descriptor's text is UTF-8). NULL is null. */
static void jstr(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (!s) {
        fputs("null", f);
        return;
    }
    fputc('"', f);
    for (; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
    fputc('"', f);
}

/* A number; JSON has no inf or nan, so those are null. */
static void jnum(FILE *f, double v)
{
    char t[64];
    if (!isfinite(v)) {
        fputs("null", f);
        return;
    }
    xbox_mod_num_text(t, sizeof t, v);
    fputs(t, f);
}

/* ,"name": with the comma the field before it needs. */
static void jkey(FILE *f, int *first, const char *name)
{
    fputs(*first ? "" : ", ", f);
    *first = 0;
    jstr(f, name);
    fputs(": ", f);
}

static void jopt_str(FILE *f, int *first, const char *name, const char *v)
{
    if (!v)
        return;
    jkey(f, first, name);
    jstr(f, v);
}

static void jlist(FILE *f, const char *const *v, int n)
{
    fputc('[', f);
    for (int i = 0; v && (n < 0 ? v[i] != NULL : i < n); i++) {
        if (i)
            fputs(", ", f);
        jstr(f, v[i]);
    }
    fputc(']', f);
}

static const char *const s_kind[] = { "bool", "int", "float", "enum" };
static const char *const s_ctl_kind[] = { "action", "button", "stick", "info" };

static const char *live_name(int live)
{
    return live == ENHANCE_LIVE_ACTIVATE ? "activate"
         : live == ENHANCE_LIVE_RESTART ? "restart" : "live";
}

/* A setting's value in its kind's JSON form: a bool true/false, an enum
 * its choice text, a number as a number. */
static void jvalue(FILE *f, const struct xbox_mod_setting *s, double v)
{
    if (s->kind == XBOX_SET_BOOL)
        fputs(v ? "true" : "false", f);
    else if (s->kind == XBOX_SET_ENUM)
        jstr(f, s->choices[(int)v]);
    else
        jnum(f, v);
}

static void setting(FILE *f, int i, uint32_t k, const struct xbox_mod_setting *s)
{
    int first = 1;

    fputs("\n      {", f);
    jkey(f, &first, "name");
    jstr(f, s->name);
    jkey(f, &first, "key");
    jstr(f, xbox_mod_setting_key(i, k));
    jkey(f, &first, "kind");
    jstr(f, s_kind[s->kind]);
    jopt_str(f, &first, "label", s->label);
    jopt_str(f, &first, "help", s->help);
    jopt_str(f, &first, "unit", s->unit);
    if (s->flags & XBOX_SET_ENABLE) {
        jkey(f, &first, "enable");
        fputs("true", f);
    }
    jkey(f, &first, "default");
    jvalue(f, s, s->def);
    if (s->kind == XBOX_SET_INT || s->kind == XBOX_SET_FLOAT) {
        jkey(f, &first, "lo");
        jnum(f, s->lo);
        jkey(f, &first, "hi");
        jnum(f, s->hi);
        if (s->step > 0) {
            jkey(f, &first, "step");
            jnum(f, s->step);
        }
    }
    if (s->kind == XBOX_SET_ENUM) {
        jkey(f, &first, "choices");
        jlist(f, s->choices, -1);
    }
    jkey(f, &first, "live");
    jstr(f, live_name(s->live));
    jopt_str(f, &first, "env", xbox_mod_setting_env(i, k));
    if (s->aliases && s->aliases[0]) {
        jkey(f, &first, "aliases");
        jlist(f, s->aliases, -1);
    }
    jkey(f, &first, "value");
    jvalue(f, s, xbox_mod_setting_value(i, k));
    jkey(f, &first, "source");
    jstr(f, xbox_mod_setting_source(i, k));
    fputc('}', f);
}

static void control(FILE *f, int i, uint32_t k, const struct xbox_mod_control *c)
{
    char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    const char *bp[ENHANCE_ACTION_BINDS];
    const char *src;
    int first = 1, n, a = xbox_mod_control_action(i, k);

    fputs("\n      {", f);
    jkey(f, &first, "id");
    jstr(f, c->id);
    jkey(f, &first, "kind");
    jstr(f, s_ctl_kind[c->kind]);
    if (a >= 0) {
        jkey(f, &first, "action");
        jstr(f, enhance_action_name(a));
    }
    jopt_str(f, &first, "label", c->label);
    jopt_str(f, &first, "help", c->help);
    jkey(f, &first, "when");
    jstr(f, c->when);
    if (c->kind != XBOX_CTL_INFO || (c->defaults && c->defaults[0])) {
        jkey(f, &first, "defaults");
        jlist(f, c->defaults, -1);
    }
    if (c->kind != XBOX_CTL_INFO) {
        n = xbox_mod_control_bindings(i, k, b, ENHANCE_ACTION_BINDS, &src);
        for (int j = 0; j < n; j++)
            bp[j] = b[j];
        jkey(f, &first, "bindings");
        jlist(f, bp, n);
        /* The table drops what the rules refuse or another action owns,
         * as a start does; bindings stays what the config says, so the
         * launcher can show the player's own text and mark these. */
        if (a >= 0 && (n = enhance_action_refused(a, b, ENHANCE_ACTION_BINDS)) > 0) {
            for (int j = 0; j < n; j++)
                bp[j] = b[j];
            jkey(f, &first, "refused");
            jlist(f, bp, n);
        }
    }
    jopt_str(f, &first, "env", c->env);
    if (c->aliases && c->aliases[0]) {
        jkey(f, &first, "aliases");
        jlist(f, c->aliases, -1);
    }
    if (c->kind != XBOX_CTL_INFO) {
        jkey(f, &first, "source");
        jstr(f, src);
    }
    fputc('}', f);
}

static void mod(FILE *f, int i)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_of(i);
    int first = 1, e = xbox_mod_enable_setting(i);

    fputs("\n  {", f);
    jkey(f, &first, "id");
    jstr(f, xbox_mod_name(i));
    if (!xbox_mod_has_desc(i)) {
        jkey(f, &first, "descriptor");
        fputs("false", f);
        fputc('}', f);
        return;
    }
    if (d) {
        jopt_str(f, &first, "title", d->title);
        jopt_str(f, &first, "summary", d->summary);
        jopt_str(f, &first, "author", d->author);
        jopt_str(f, &first, "version", d->version_text);
    }
    jkey(f, &first, "descriptor");
    fputs("true", f);
    jkey(f, &first, "init");
    jstr(f, xbox_mod_init_outcome(i));
    if (!strcmp(xbox_mod_init_outcome(i), "failed")) {
        jkey(f, &first, "reason");
        jstr(f, xbox_mod_fail_reason(i));
    }
    if (!d) {
        fputc('}', f);
        return;     /* refused: its tables are not to be trusted */
    }
    if (e >= 0) {
        jkey(f, &first, "enable");
        jstr(f, xbox_mod_setting_key(i, (uint32_t)e));
    }
    jkey(f, &first, "settings");
    fputc('[', f);
    for (uint32_t k = 0; k < d->n_settings; k++) {
        if (k)
            fputc(',', f);
        setting(f, i, k, &d->settings[k]);
    }
    fputc(']', f);
    jkey(f, &first, "controls");
    fputc('[', f);
    for (uint32_t k = 0; k < d->n_controls; k++) {
        if (k)
            fputc(',', f);
        control(f, i, k, &d->controls[k]);
    }
    fputs("]}", f);
}

int xbox_mod_describe_wanted(void)
{
    const char *v = recomp_env(RENV_MODS_DESCRIBE);

    if (!v || !*v)
        return 0;
    if (!strcmp(v, "json"))
        return 1;
    fprintf(stderr, LOGP "RECOMP_MODS_DESCRIBE='%s' is not json; ignored\n", v);
    return 0;
}

int xbox_mod_describe_write(const char *path)
{
    FILE *f = stdout;
    int i, n = xbox_mod_count(), bad;

    if (path && *path) {
        f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, LOGP "RECOMP_MODS_DESCRIBE_OUT=%s: cannot write it (%s)\n", path,
                    strerror(errno));
            return 1;
        }
    }
    fprintf(f, "{\"schema\": 1, \"api\": %u, \"mods\": [", XBOX_MOD_API_VERSION);
    for (i = 0; i < n; i++) {
        if (i)
            fputc(',', f);
        mod(f, i);
    }
    fputs(n ? "\n]}\n" : "]}\n", f);
    bad = ferror(f);
    if (f != stdout)
        bad |= fclose(f) != 0;
    else
        bad |= fflush(f) != 0;
    if (bad) {
        fprintf(stderr, LOGP "RECOMP_MODS_DESCRIBE_OUT=%s: the write failed\n",
                path && *path ? path : "(stdout)");
        return 1;
    }
    return 0;
}
