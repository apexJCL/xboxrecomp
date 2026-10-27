/*
 * enhance_menu.c - the in-game enhancements menu; see enhance_menu.h.
 *
 * Three parts:
 *   - the slots: one per item (the toolkit's, a title's, a mod's, plus the
 *     Mods page's enable toggle per mod), each with its value as text and
 *     the value as last loaded or saved, so Save writes only what moved;
 *   - the state machine: pages, rows, selection, capture, Save and Reset,
 *     driven by enhance_menu_event (the ctest calls it directly);
 *   - the hook: the host input into events (D4: first repeat at 150 ms,
 *     then every 60 ms), the neutral pad for the title, and the draw.
 *
 * Every step runs under the overlay's menu-step lock: the title's poll and
 * the flip tick may both call the hook, and only one of them may step.
 */
#include "enhance_menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "input_map.h"
#include "input_names.h"
#include "kernel_pacing.h"
#include "keyboard.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_int.h"
#include "nv2a_stats.h"
#include "nv2a_tex_pack.h"
#include "recomp_cfg.h"
#include "recomp_env.h"

#define LOGP "[ENHANCE] "

#define PORTS        4
#define VAL_MAX      96
#define ROWS_MAX     192
#define REPEAT_FIRST 150u   /* D4: a held direction repeats after 150 ms */
#define REPEAT_NEXT  60u    /* ... then every 60 ms */
#define CHORD_MS     400u   /* a captured pad chord is the set held this long */
#define CAPTURE_MS   10000u /* a capture with no input gives up */
#define CLOSE_MS     1000u  /* the neutral pad after closing, at most */
#define LIVE_DRAW_MS 250u   /* a page of live numbers redraws this often */
#define STATUS_MS    3000u  /* a message stays in the footer this long */

enum page {
    PAGE_RENDER, PAGE_PACK, PAGE_GAME, PAGE_MODS, PAGE_STATS, PAGE_CONTROLS,
    PAGE_DEBUG, PAGE_SAVE, PAGE_COUNT
};
static const char *const s_page_name[PAGE_COUNT] = {
    "Render", "Pack", "Game", "Mods", "Stats", "Controls", "Debug", "Save"
};

/* ---- the toolkit's items ------------------------------------------------ */

static const char *const k_aniso[] = { "0", "2", "4", "8", "16", NULL };
static const char *const k_filter[] = { "nearest", "linear", "integer", NULL };
static const char *const k_pacing[] = { "spin", "sleep", NULL };
static const char *const k_aspect[] = { "4:3", "16:9", "16:10", "21:9", NULL };
static const char *const k_movie[] = { "black", "fit", "fill", NULL };
static const char *const k_surface[] = { "auto", "window", "internal", NULL };
static const char *const k_onoff[] = { "off", "on", NULL };
/* input.mouse_capture in the menu's order; the enum's is focus, click, never. */
static const char *const k_capture[] = { "click", "focus", "never", NULL };

/* pack_off as the menu last asked for it: the pack toggle and the two kind
 * toggles share the field, and a request only lands at the next flip, so
 * reading nv2a_host_opts back between two changes would lose the first. */
static unsigned s_pack_off;

static int on_text(const char *v)
{
    return v && (!strcmp(v, "true") || !strcmp(v, "on"));
}

static void req(void (*edit)(struct nv2a_host_opts *, const char *), const char *v,
                unsigned mask)
{
    struct nv2a_host_opts o = *nv2a_host_opts();
    edit(&o, v);
    nv2a_host_opts_request(&o, mask);
}

static void e_scale(struct nv2a_host_opts *o, const char *v) { o->render_scale = (unsigned)atoi(v); }
static void e_aniso(struct nv2a_host_opts *o, const char *v) { o->aniso = (unsigned)atoi(v); }
static void e_full(struct nv2a_host_opts *o, const char *v) { o->fullscreen = on_text(v); }
static void e_overscan(struct nv2a_host_opts *o, const char *v) { o->overscan = (unsigned)atoi(v); }
static void e_pack(struct nv2a_host_opts *o, const char *v) { (void)v; o->pack_off = s_pack_off; }

static int index_of(const char *const *c, const char *v)
{
    for (int i = 0; c[i]; i++)
        if (!strcmp(c[i], v))
            return i;
    return -1;
}

static void e_filter(struct nv2a_host_opts *o, const char *v)
{
    int i = index_of(k_filter, v);
    o->present_filter = i < 0 ? NV2A_PRESENT_NEAREST : i;
}

static void e_movie(struct nv2a_host_opts *o, const char *v)
{
    int i = index_of(k_movie, v);
    o->movie_mode = i < 0 ? NV2A_MOVIE_BLACK : i;
}

static void set_scale(const struct enhance_item *it, const char *v) { (void)it; req(e_scale, v, NV2A_OPT_SCALE); }
static void set_aniso(const struct enhance_item *it, const char *v) { (void)it; req(e_aniso, v, NV2A_OPT_ANISO); }
static void set_filter(const struct enhance_item *it, const char *v) { (void)it; req(e_filter, v, NV2A_OPT_FILTER); }
static void set_full(const struct enhance_item *it, const char *v) { (void)it; req(e_full, v, NV2A_OPT_FULLSCREEN); }
static void set_overscan(const struct enhance_item *it, const char *v) { (void)it; req(e_overscan, v, NV2A_OPT_OVERSCAN); }
static void set_movie(const struct enhance_item *it, const char *v) { (void)it; req(e_movie, v, NV2A_OPT_MOVIE); }

static void set_pacing(const struct enhance_item *it, const char *v)
{
    (void)it;
    xbox_enhance_set_pacing(strcmp(v, "spin") ? XBOX_PACING_SLEEP : XBOX_PACING_SPIN);
}

/* The pack toggle and the kinds: bits of the one pack_off field. */
static void set_pack_bit(const struct enhance_item *it, const char *v)
{
    unsigned bit = (unsigned)(uintptr_t)it->user;
    if (on_text(v))
        s_pack_off &= ~bit;
    else
        s_pack_off |= bit;
    req(e_pack, v, NV2A_OPT_PACK);
}

static void set_stats(const struct enhance_item *it, const char *v)
{
    (void)it;
    nv2a_overlay_set_stats(on_text(v));
}

static void set_capture(const struct enhance_item *it, const char *v)
{
    xbox_mouse_opts o;
    int i = index_of(k_capture, v);
    (void)it;
    xbox_InputMouseGetOpts(&o);
    o.capture = i == 0 ? XBOX_MOUSE_CAPTURE_CLICK
              : i == 2 ? XBOX_MOUSE_CAPTURE_NEVER : XBOX_MOUSE_CAPTURE_FOCUS;
    /* Through the options ring: the window thread applies it. */
    xbox_InputMouseOpts(&o);
}

static const char *get_scale(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%u", nv2a_host_render_scale());
    return b;
}

static const char *get_aniso(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%u", nv2a_host_opts()->aniso);
    return b;
}

static const char *get_filter(const struct enhance_item *it, char *b, size_t n)
{
    int f = nv2a_host_opts()->present_filter;
    (void)it;
    snprintf(b, n, "%s", k_filter[f >= 0 && f <= 2 ? f : 0]);
    return b;
}

static const char *get_full(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", nv2a_host_opts()->fullscreen ? "true" : "false");
    return b;
}

static const char *get_pacing(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", xbox_PacingMode() == XBOX_PACING_SPIN ? "spin" : "sleep");
    return b;
}

static const char *get_overscan(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%u", nv2a_host_opts()->overscan);
    return b;
}

static const char *get_movie(const struct enhance_item *it, char *b, size_t n)
{
    int m = nv2a_host_opts()->movie_mode;
    (void)it;
    snprintf(b, n, "%s", k_movie[m >= 0 && m <= 2 ? m : 0]);
    return b;
}

static const char *get_pack_bit(const struct enhance_item *it, char *b, size_t n)
{
    unsigned bit = (unsigned)(uintptr_t)it->user;
    snprintf(b, n, "%s", (s_pack_off & bit) ? "off" : "on");
    if (it->kind == ENHANCE_ITEM_TOGGLE)
        snprintf(b, n, "%s", (s_pack_off & bit) ? "false" : "true");
    return b;
}

static const char *get_stats(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", nv2a_overlay_stats() ? "true" : "false");
    return b;
}

static const char *get_capture(const struct enhance_item *it, char *b, size_t n)
{
    xbox_mouse_opts o;
    (void)it;
    xbox_InputMouseGetOpts(&o);
    snprintf(b, n, "%s", o.capture == XBOX_MOUSE_CAPTURE_CLICK ? "click"
                         : o.capture == XBOX_MOUSE_CAPTURE_NEVER ? "never" : "focus");
    return b;
}

#define ITEM(k, l, p, kd) .key = k, .label = l, .page = p, .kind = kd

static const struct enhance_item k_items[] = {
    { ITEM("render.scale", "Render scale", "Render", ENHANCE_ITEM_RANGE), .lo = 1, .hi = 4,
      .step = 1, .set = set_scale, .get = get_scale, .live = ENHANCE_LIVE_FLUSH, .def = "1" },
    { ITEM("render.aniso", "Anisotropic filtering", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_aniso, .set = set_aniso, .get = get_aniso, .flags = ENHANCE_ITEM_NUMERIC,
      .def = "0" },
    { ITEM("present.filter", "Scaling filter", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_filter, .set = set_filter, .get = get_filter, .def = "nearest" },
    { ITEM("present.fullscreen", "Fullscreen", "Render", ENHANCE_ITEM_TOGGLE), .set = set_full,
      .get = get_full, .live = ENHANCE_LIVE_FLUSH, .def = "false" },
    { ITEM("present.pacing", "Frame pacing", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_pacing, .set = set_pacing, .get = get_pacing, .def = "sleep" },
    { ITEM("display.aspect", "Aspect ratio", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_aspect, .live = ENHANCE_LIVE_RESTART, .def = "4:3" },
    { ITEM("display.overscan", "Overscan crop", "Render", ENHANCE_ITEM_RANGE), .lo = 0,
      .hi = NV2A_OVERSCAN_MAX, .step = 4, .note = "4:3 only", .set = set_overscan,
      .get = get_overscan, .def = "0" },
    { ITEM("display.movie", "Movies", "Render", ENHANCE_ITEM_CHOICE), .choices = k_movie,
      .set = set_movie, .get = get_movie, .def = "black" },
    { ITEM("present.surface", "Present surface", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_surface, .live = ENHANCE_LIVE_RESTART, .def = "auto" },
    /* textures.pack names the file: the toggle is this session's only. */
    { ITEM("textures.pack", "Texture pack", "Pack", ENHANCE_ITEM_TOGGLE), .set = set_pack_bit,
      .get = get_pack_bit, .live = ENHANCE_LIVE_FLUSH, .flags = ENHANCE_ITEM_NOSAVE,
      .def = "true", .user = (void *)(uintptr_t)NV2A_PACK_OFF_ALL },
    { ITEM("textures.font", "Pack font", "Pack", ENHANCE_ITEM_CHOICE), .choices = k_onoff,
      .set = set_pack_bit, .get = get_pack_bit, .live = ENHANCE_LIVE_FLUSH, .def = "on",
      .user = (void *)(uintptr_t)NV2A_PACK_OFF_FONT },
    { ITEM("textures.text", "Pack text", "Pack", ENHANCE_ITEM_CHOICE), .choices = k_onoff,
      .set = set_pack_bit, .get = get_pack_bit, .live = ENHANCE_LIVE_FLUSH, .def = "on",
      .user = (void *)(uintptr_t)NV2A_PACK_OFF_TEXT },
    { ITEM("overlay.stats", "Stats strip", "Stats", ENHANCE_ITEM_TOGGLE), .set = set_stats,
      .get = get_stats, .def = "false" },
    { ITEM("input.mouse_capture", "Capture", "Controls", ENHANCE_ITEM_CHOICE),
      .choices = k_capture, .set = set_capture, .get = get_capture,
      .def = "focus" },
};
#define N_ITEMS ((int)(sizeof k_items / sizeof k_items[0]))

/* ---- slots ---------------------------------------------------------------- */

struct slot {
    const struct enhance_item *it;
    char key[64];
    char cur[VAL_MAX];          /* the value in force as the menu sees it */
    char base[VAL_MAX];         /* as loaded or last saved */
    int page;
    int mod;                    /* -1, or the mod an item or toggle is for */
    int toggle;                 /* the mod's enable toggle */
    int env;                    /* forced by the environment: read only */
};

#define MODS_MAX 32
#define SLOTS_MAX (ENHANCE_MENU_MAX_ITEMS + 2 * XBOX_MOD_MAX_ITEMS + MODS_MAX + 16)
static struct slot s_slot[SLOTS_MAX];
/* The Mods page's toggle per mod (mods.<name>), synthesized at the first
 * open. */
static struct enhance_item s_mod_toggle[MODS_MAX];
static char s_mod_key[MODS_MAX][64];
static int s_nslot, s_built;
static const struct enhance_item *s_title[ENHANCE_MENU_MAX_ITEMS];
static int s_ntitle;

int enhance_menu_add(const struct enhance_item *it)
{
    if (!it || !it->key || !it->label || s_ntitle >= ENHANCE_MENU_MAX_ITEMS)
        return 0;
    s_title[s_ntitle++] = it;
    return 1;
}

static int page_by_name(const char *name, int def)
{
    for (int p = 0; name && p < PAGE_COUNT; p++)
        if (!strcmp(name, s_page_name[p]))
            return p;
    return def;
}

/* The value as the config has it, as text in the item's own spelling. */
static void cfg_value(const struct enhance_item *it, const char *key, char *out, size_t n)
{
    const char *def = it->def ? it->def : "";
    switch (it->kind) {
    case ENHANCE_ITEM_TOGGLE:
        snprintf(out, n, "%s", enhance_cfg_bool(key, on_text(def)) ? "true" : "false");
        break;
    case ENHANCE_ITEM_CHOICE:
        if (it->flags & ENHANCE_ITEM_NUMERIC)
            snprintf(out, n, "%lld", enhance_cfg_int(key, atoll(def)));
        else
            snprintf(out, n, "%s", enhance_cfg_string(key, def));
        break;
    case ENHANCE_ITEM_RANGE:
        snprintf(out, n, "%g", enhance_cfg_float(key, atof(def)));
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void slot_read(struct slot *s)
{
    char buf[VAL_MAX];
    const char *v = s->it->get ? s->it->get(s->it, buf, sizeof buf) : NULL;
    if (v)
        snprintf(s->cur, sizeof s->cur, "%s", v);
    else
        cfg_value(s->it, s->key, s->cur, sizeof s->cur);
}

static void slot_add(const struct enhance_item *it, int page, int mod)
{
    struct slot *s;
    if (s_nslot >= SLOTS_MAX)
        return;
    s = &s_slot[s_nslot++];
    memset(s, 0, sizeof *s);
    s->it = it;
    snprintf(s->key, sizeof s->key, "%s", it->key);
    s->page = page;
    s->mod = mod;
    s->env = enhance_cfg_source_of(s->key) == ENHANCE_SRC_ENV;
    slot_read(s);
    memcpy(s->base, s->cur, sizeof s->base);
}

static void mod_toggle_set(const struct enhance_item *it, const char *v)
{
    xbox_mod_set_enabled((int)(intptr_t)it->user, on_text(v));
}

static const char *mod_toggle_get(const struct enhance_item *it, char *b, size_t n)
{
    snprintf(b, n, "%s", xbox_mod_enabled((int)(intptr_t)it->user) ? "true" : "false");
    return b;
}

static void build_slots(void)
{
    s_nslot = 0;
    s_pack_off = nv2a_host_opts()->pack_off;
    for (int i = 0; i < N_ITEMS; i++)
        slot_add(&k_items[i], page_by_name(k_items[i].page, PAGE_RENDER), -1);
    for (int i = 0; i < s_ntitle; i++)
        slot_add(s_title[i], page_by_name(s_title[i]->page, PAGE_GAME), -1);
    for (int m = 0; m < xbox_mod_count() && m < MODS_MAX; m++) {
        const struct enhance_item *items[XBOX_MOD_MAX_ITEMS];
        int n = xbox_mod_items(m, items, XBOX_MOD_MAX_ITEMS);
        struct enhance_item *t = &s_mod_toggle[m];
        snprintf(s_mod_key[m], sizeof s_mod_key[m], "mods.%s", xbox_mod_name(m));
        memset(t, 0, sizeof *t);
        t->key = s_mod_key[m];
        t->label = xbox_mod_name(m);
        t->kind = ENHANCE_ITEM_TOGGLE;
        t->set = mod_toggle_set;
        t->get = mod_toggle_get;
        /* Without on_enable the mod reads its key at init only. */
        t->live = xbox_mod_has_on_enable(m) ? ENHANCE_LIVE : ENHANCE_LIVE_RESTART;
        t->def = "true";
        t->user = (void *)(intptr_t)m;
        slot_add(t, PAGE_MODS, m);
        if (s_nslot)
            s_slot[s_nslot - 1].toggle = 1;
        for (int i = 0; i < n; i++)
            slot_add(items[i], PAGE_MODS, m);
    }
    s_built = 1;
}

static int slot_saveable(const struct slot *s)
{
    return !(s->it->flags & ENHANCE_ITEM_NOSAVE) && !s->env
        && s->it->kind != ENHANCE_ITEM_INFO && s->it->kind != ENHANCE_ITEM_ACTION;
}

/* The stock value: the defaults file's, else the item's code default. 0
 * when neither has one. */
static int slot_stock(const struct slot *s, char *out, size_t n)
{
    const recomp_cfg *d = enhance_cfg_defaults();
    const struct enhance_item *it = s->it;
    if (d && recomp_cfg_type_of(d, s->key) != RCFG_NONE) {
        switch (it->kind) {
        case ENHANCE_ITEM_TOGGLE:
            snprintf(out, n, "%s", recomp_cfg_bool(d, s->key, 0) ? "true" : "false");
            return 1;
        case ENHANCE_ITEM_CHOICE:
            if (it->flags & ENHANCE_ITEM_NUMERIC)
                snprintf(out, n, "%lld", recomp_cfg_int(d, s->key, 0));
            else
                snprintf(out, n, "%s", recomp_cfg_string(d, s->key, ""));
            return 1;
        case ENHANCE_ITEM_RANGE:
            snprintf(out, n, "%g", recomp_cfg_float(d, s->key, 0));
            return 1;
        default:
            break;
        }
    }
    if (!it->def)
        return 0;
    snprintf(out, n, "%s", it->def);
    return 1;
}

static void slot_set(struct slot *s, const char *v)
{
    snprintf(s->cur, sizeof s->cur, "%s", v);
    /* A RESTART item's set() records the value for the next start (a
     * mod's toggle); most have none and the value is only stored. */
    if (s->it->set)
        s->it->set(s->it, s->cur);
}

/* ---- actions the Controls page changed ---------------------------------- */

static unsigned char s_adirty[ENHANCE_ACTIONS_MAX];

typedef char bind_list[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];

/* The stock bindings: the defaults file's [input.actions] entry, else the
 * code defaults. */
static int action_stock(int id, bind_list out)
{
    const recomp_cfg *d = enhance_cfg_defaults();
    char key[96];
    int n = 0;
    snprintf(key, sizeof key, "input.actions.%s", enhance_action_name(id));
    if (d && recomp_cfg_type_of(d, key) == RCFG_ARRAY) {
        size_t len = recomp_cfg_array_len(d, key);
        for (size_t i = 0; i < len && n < ENHANCE_ACTION_BINDS; i++)
            snprintf(out[n++], ENHANCE_ACTION_TEXT, "%s",
                     recomp_cfg_array_string(d, key, i, ""));
        return n;
    }
    return enhance_action_default_bindings(id, out, ENHANCE_ACTION_BINDS);
}

static int lists_equal(bind_list a, int na, bind_list b, int nb)
{
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++)
        if (strcmp(a[i], b[i]))
            return 0;
    return 1;
}

static void action_set(int id, bind_list l, int n)
{
    const char *t[ENHANCE_ACTION_BINDS];
    for (int i = 0; i < n; i++)
        t[i] = l[i];
    enhance_action_set_bindings(id, t, n);
    if (id >= 0 && id < ENHANCE_ACTIONS_MAX)
        s_adirty[id] = 1;
}

static void join_bindings(int id, char *out, size_t n)
{
    bind_list l;
    int k = enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS);
    size_t at = 0;
    out[0] = 0;
    if (!k) {
        snprintf(out, n, "none");
        return;
    }
    for (int i = 0; i < k && at < n; i++)
        at += (size_t)snprintf(out + at, n - at, "%s%s", i ? ", " : "", l[i]);
}

/* ---- state --------------------------------------------------------------- */

enum { ST_CLOSED, ST_OPEN, ST_CLOSING };
enum row_type {
    R_SLOT, R_INFO, R_HEADER, R_BIND, R_MOD, R_SAVE, R_RESET
};

struct row {
    int type;
    int ref;            /* slot, action id or mod index */
    int sel;
    char label[64];
    char value[VAL_MAX];
    mu_Color *tint;     /* an info value's colour, NULL for the default */
};

static int s_state = ST_CLOSED;     /* written under the lock, read relaxed */
static int s_page;
static int s_sel[PAGE_COUNT];
static int s_scroll[PAGE_COUNT];
static int s_mod_sub = -1;          /* the mod whose items the Mods page lists */
static int s_confirm;               /* the Reset row asks again */
static char s_status[128];
static uint64_t s_status_t, s_now;

static struct row s_row[ROWS_MAX];
static int s_nrow;

/* capture */
enum { CAP_OFF, CAP_RELEASE, CAP_ARMED };
static struct {
    int phase, id;
    uint64_t t0, t_set;
    uint32_t max, last;
} s_cap;

/* Every phase change goes through here: while a capture runs the actions
 * fire nothing, so the input being captured never also does what it is
 * bound to now. */
static void cap_phase(int phase)
{
    s_cap.phase = phase;
    enhance_actions_quiet(phase != CAP_OFF);
}

/* input */
static uint32_t s_held[PORTS], s_prev_held;
static unsigned char s_kprev[256];
static DWORD s_pkt[PORTS], s_fpkt[PORTS];
static int s_dir, s_kbd;
static uint64_t s_dir_next, s_close_t, s_draw_t;
static uint64_t s_extra_t[INPUT_SRC_COUNT];
static int s_need_draw, s_snap_on, s_stats_on;
static int s_draw_w, s_draw_h;

/* rates over one-second windows (Stats, Debug) */
static uint64_t s_rate_t0, s_rate_c0[NV2A_STAT_COUNT];
static float s_rate[NV2A_STAT_COUNT];

static void status(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    s_status_t = s_now ? s_now : 1;
}

/* ---- rows ---------------------------------------------------------------- */

static struct row *row_add(int type, int ref, int sel, const char *label)
{
    struct row *r;
    if (s_nrow >= ROWS_MAX)
        return NULL;
    r = &s_row[s_nrow++];
    memset(r, 0, sizeof *r);
    r->type = type;
    r->ref = ref;
    r->sel = sel;
    snprintf(r->label, sizeof r->label, "%s", label ? label : "");
    return r;
}

static void info(const char *label, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void info(const char *label, const char *fmt, ...)
{
    struct row *r = row_add(R_INFO, -1, 0, label);
    va_list ap;
    if (!r)
        return;
    va_start(ap, fmt);
    vsnprintf(r->value, sizeof r->value, fmt, ap);
    va_end(ap);
}

static void header(const char *label)
{
    row_add(R_HEADER, -1, 0, label);
}

static void slot_rows(int page, int mod)
{
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        struct row *r;
        if (s->page != page || s->mod != mod || s->toggle)
            continue;
        r = row_add(R_SLOT, i, s->it->kind != ENHANCE_ITEM_INFO, s->it->label);
        if (r && s->it->kind == ENHANCE_ITEM_INFO)
            slot_read(s);
        if (r)
            snprintf(r->value, sizeof r->value, "%s", s->cur);
    }
}

static int slot_find(const char *key)
{
    for (int i = 0; i < s_nslot; i++)
        if (!strcmp(s_slot[i].key, key))
            return i;
    return -1;
}

static void slot_row(const char *key)
{
    int i = slot_find(key);
    struct row *r;
    if (i < 0)
        return;
    r = row_add(R_SLOT, i, 1, s_slot[i].it->label);
    if (r)
        snprintf(r->value, sizeof r->value, "%s", s_slot[i].cur);
}

static const char *miss_why(uint32_t w)
{
    switch (w) {
    case NV2A_TEX_PACK_MISS_PACK_OFF: return "pack off";
    case NV2A_TEX_PACK_MISS_KIND_OFF: return "kind off";
    case NV2A_TEX_PACK_MISS_REFUSED: return "refused";
    default: return "absent";
    }
}

static void miss_rows(int max)
{
    struct nv2a_tex_pack_miss m[8];
    unsigned n = nv2a_tex_pack_misses_snapshot(m, (unsigned)(max < 8 ? max : 8));
    if (!n) {
        info("Top misses", "none yet");
        return;
    }
    header("Top misses");
    for (unsigned i = 0; i < n; i++) {
        char label[32];
        snprintf(label, sizeof label, "%016llx", (unsigned long long)m[i].hash);
        info(label, "%ux%u fmt 0x%02x \xC3\x97%u %s", m[i].width, m[i].height, m[i].color,
             m[i].count, miss_why(m[i].why));
    }
}

static const char *backend_name(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    return b && *b ? b : "default";
}

static void rates(uint64_t now)
{
    uint64_t c[NV2A_STAT_COUNT];
    for (int k = 0; k < NV2A_STAT_COUNT; k++)
        c[k] = nv2a_stats_count((enum nv2a_stat)k);
    if (!s_rate_t0 || now < s_rate_t0) {
        s_rate_t0 = now ? now : 1;
        memcpy(s_rate_c0, c, sizeof c);
        return;
    }
    if (now - s_rate_t0 < 1000)
        return;
    for (int k = 0; k < NV2A_STAT_COUNT; k++)
        s_rate[k] = (float)(c[k] - s_rate_c0[k]) * 1000.0f / (float)(now - s_rate_t0);
    s_rate_t0 = now;
    memcpy(s_rate_c0, c, sizeof c);
}

static void names_of(uint32_t bits, char *out, size_t n)
{
    size_t at = 0;
    out[0] = 0;
    for (int src = 1; src < INPUT_SRC_COUNT && at < n; src++)
        if (bits & (1u << src))
            at += (size_t)snprintf(out + at, n - at, "%s%s", at ? ", " : "",
                                   xbox_InputMap_SourceName(src));
    if (!at)
        snprintf(out, n, "none");
}

static const char *trace_value(const char *v)
{
    return !v ? "off" : (!*v || !strcmp(v, "1")) ? "on" : v;
}

static void build_rows(void)
{
    char buf[VAL_MAX];
    s_nrow = 0;
    switch (s_page) {
    case PAGE_RENDER:
        slot_rows(PAGE_RENDER, -1);
        break;
    case PAGE_PACK: {
        unsigned n = nv2a_tex_pack_count();
        if (!strcmp(backend_name(), "cpu"))
            info("CPU path", "pack ignored");
        if (!n) {
            info("No pack loaded", "textures.pack");
        } else {
            slot_row("textures.pack");
            if (nv2a_tex_pack_kind_count(XRTP_KIND_FONT))
                slot_row("textures.font");
            else
                info("Pack font", "none in this pack");
            if (nv2a_tex_pack_kind_count(XRTP_KIND_TEXT))
                slot_row("textures.text");
            else
                info("Pack text", "none in this pack");
            info("Entries", "%u (%u art, %u font, %u text)", n,
                 nv2a_tex_pack_kind_count(XRTP_KIND_ART), nv2a_tex_pack_kind_count(XRTP_KIND_FONT),
                 nv2a_tex_pack_kind_count(XRTP_KIND_TEXT));
            info("Scale", "%ux, text %ux", nv2a_tex_pack_scale(), nv2a_tex_pack_text_scale());
            info("BC3 entries", "%u", nv2a_tex_pack_bc3_count());
            info("Hits", "%llu", (unsigned long long)nv2a_tex_pack_hits());
        }
        info("Misses", "%llu", (unsigned long long)nv2a_tex_pack_miss_total());
        miss_rows(4);
        break;
    }
    case PAGE_GAME:
        slot_rows(PAGE_GAME, -1);
        if (!s_nrow)
            info("No game options", "");
        break;
    case PAGE_MODS:
        if (s_mod_sub >= 0) {
            char key[64];
            int t;
            snprintf(key, sizeof key, "mods.%s", xbox_mod_name(s_mod_sub));
            t = slot_find(key);
            header(xbox_mod_name(s_mod_sub));
            if (t >= 0) {
                struct row *r = row_add(R_SLOT, t, 1, "Enabled");
                if (r)
                    snprintf(r->value, sizeof r->value, "%s", s_slot[t].cur);
            }
            slot_rows(PAGE_MODS, s_mod_sub);
            break;
        }
        for (int m = 0; m < xbox_mod_count(); m++) {
            struct row *r = row_add(R_MOD, m, 1, xbox_mod_name(m));
            if (r)
                snprintf(r->value, sizeof r->value, "%s", xbox_mod_state_name(xbox_mod_state(m)));
        }
        if (!s_nrow)
            info("No mods in this build", "");
        break;
    case PAGE_STATS: {
        struct nv2a_stats_snap st;
        const char *pres = nv2a_overlay_presenter();
        slot_rows(PAGE_STATS, -1);
        nv2a_stats_read(&st, nv2a_ovl_now_ns());
        info("Frame rate", "%.1f fps", st.frames && st.ms_avg > 0 ? 1000.0f / st.ms_avg : 0.0f);
        info("Frame time", "%.1f ms (max %.1f)", st.ms_avg, st.ms_max);
        info("Pacing", "%s", xbox_PacingMode() == XBOX_PACING_SPIN ? "spin" : "sleep");
        info("Flips", "%.0f/s", s_rate[NV2A_STAT_FLIPS]);
        info("Presents", "%.0f/s", s_rate[NV2A_STAT_PRESENTS]);
        info("Draws", "%.0f/s", s_rate[NV2A_STAT_DRAWS]);
        info("Texture uploads", "%.0f/s", s_rate[NV2A_STAT_TEX_UPLOADS]);
        info("Render scale", "%ux", nv2a_host_render_scale());
        info("Presenter", "%s", pres ? pres : "none yet");
        break;
    }
    case PAGE_CONTROLS: {
        xbox_mouse_opts mo;
        const char *rk;
        for (int id = 0; id < enhance_action_count(); id++) {
            struct row *r;
            if (!enhance_action_name(id))
                continue;
            r = row_add(R_BIND, id, 1, enhance_action_label(id));
            if (r)
                join_bindings(id, r->value, sizeof r->value);
        }
        header("Mouse");
        slot_row("input.mouse_capture");
        xbox_InputMouseGetOpts(&mo);
        rk = mo.release_key ? xbox_InputCodeName(mo.release_key, 0) : NULL;
        info("Release key", "%s", rk ? rk : "none");
        info("Free keys", "none (3F)");
        if (xbox_InputMouseCaptured())
            info("State", "captured");
        else if (mo.capture == XBOX_MOUSE_CAPTURE_CLICK)
            info("State", "free: click to capture");
        else
            info("State", "free: %s", rk ? rk : "no release key");
        break;
    }
    case PAGE_DEBUG: {
        const struct nv2a_ovl_atlas *a = nv2a_overlay_atlas();
        const char *pres = nv2a_overlay_presenter();
        uint32_t extra = 0, held = 0;
        static const int k_extra[] = {
            INPUT_SRC_GUIDE, INPUT_SRC_MISC1, INPUT_SRC_TOUCHPAD, INPUT_SRC_PADDLE1,
            INPUT_SRC_PADDLE2, INPUT_SRC_PADDLE3, INPUT_SRC_PADDLE4
        };
        for (unsigned i = 0; i < sizeof k_extra / sizeof k_extra[0]; i++)
            if (s_extra_t[k_extra[i]] && s_now - s_extra_t[k_extra[i]] < 1000)
                extra |= 1u << k_extra[i];
        for (int p = 0; p < PORTS; p++)
            held |= s_held[p];
        info("Backend", "%s", backend_name());
        info("Presenter", "%s", pres ? pres : "none yet");
        info("Host options gen", "%u", nv2a_host_opts_gen());
        info("Texture uploads", "%.0f/s", s_rate[NV2A_STAT_TEX_UPLOADS]);
        info("Pack misses", "%llu", (unsigned long long)nv2a_tex_pack_miss_total());
        miss_rows(3);
        names_of(extra, buf, sizeof buf);
        info("Extra buttons (1 s)", "%s", buf);
        names_of(held, buf, sizeof buf);
        info("Held", "%s", buf);
        if (a)
            info("Atlas", "%ux%u", a->w, a->h);
        else
            info("Atlas", "none");
        info("Missing glyphs", "n/a (3E)");
        info("Window shot", "RECOMP_WINDOW_SHOT only");
        header("Trace channels (RECOMP_TRACE)");
#define TRACE_ROW_(id, tier, name, legacy, help) \
        if (RECOMP_ENV_TIER_IS_TRACE_##tier) \
            info(name, "%s", trace_value(recomp_env(RENV_##id)));
#define RECOMP_ENV_TIER_IS_TRACE_TRACE 1
#define RECOMP_ENV_TIER_IS_TRACE_CONFIG 0
#define RECOMP_ENV_TIER_IS_TRACE_DEBUG 0
        RECOMP_ENV_KEYS(TRACE_ROW_)
#undef TRACE_ROW_
        break;
    }
    case PAGE_SAVE: {
        const char *path = enhance_cfg_user_path();
        int un = enhance_menu_unsaved();
        struct row *r = row_add(R_SAVE, -1, 1, "Save");
        if (r)
            snprintf(r->value, sizeof r->value, "%s", path ? path : "off (RECOMP_CONFIG=none)");
        info("Unsaved changes", "%d", un);
        r = row_add(R_RESET, -1, 1, s_confirm ? "Reset everything? A / Enter: yes" : "Reset to defaults");
        if (r)
            snprintf(r->value, sizeof r->value, "%s", s_confirm ? "B / Esc: no" : "");
        break;
    }
    default:
        break;
    }
    /* The selection on a selectable row, the first one when it is gone. */
    {
        int *sel = &s_sel[s_page];
        if (*sel >= s_nrow)
            *sel = s_nrow - 1;
        if (*sel < 0)
            *sel = 0;
        if (s_nrow && !s_row[*sel].sel) {
            int i;
            for (i = 0; i < s_nrow && !s_row[i].sel; i++)
                ;
            *sel = i < s_nrow ? i : 0;
        }
    }
}

/* ---- Save and Reset ------------------------------------------------------- */

static char s_pkey[SLOTS_MAX + ENHANCE_ACTIONS_MAX][96];
static char s_pval[SLOTS_MAX + ENHANCE_ACTIONS_MAX][VAL_MAX * 4];
static int s_pnull[SLOTS_MAX + ENHANCE_ACTIONS_MAX];

static int collect(void)
{
    int n = 0;
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        char stock[VAL_MAX];
        if (!slot_saveable(s) || !strcmp(s->cur, s->base))
            continue;
        snprintf(s_pkey[n], sizeof s_pkey[n], "%s", s->key);
        s_pnull[n] = slot_stock(s, stock, sizeof stock) && !strcmp(stock, s->cur);
        if (s->it->kind == ENHANCE_ITEM_CHOICE && !(s->it->flags & ENHANCE_ITEM_NUMERIC))
            recomp_cfg_quote(s->cur, s_pval[n], sizeof s_pval[n]);
        else
            snprintf(s_pval[n], sizeof s_pval[n], "%s", s->cur);
        n++;
    }
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++) {
        bind_list cur, stock;
        int nc, ns;
        size_t at;
        if (!s_adirty[id] || !enhance_action_name(id))
            continue;
        nc = enhance_action_bindings(id, cur, ENHANCE_ACTION_BINDS);
        ns = action_stock(id, stock);
        snprintf(s_pkey[n], sizeof s_pkey[n], "input.actions.%s", enhance_action_name(id));
        s_pnull[n] = lists_equal(cur, nc, stock, ns);
        at = (size_t)snprintf(s_pval[n], sizeof s_pval[n], "[");
        for (int k = 0; k < nc; k++) {
            char q[ENHANCE_ACTION_TEXT * 2 + 4];
            recomp_cfg_quote(cur[k], q, sizeof q);
            at += (size_t)snprintf(s_pval[n] + at, sizeof s_pval[n] - at, "%s%s",
                                   k ? ", " : "", q);
        }
        snprintf(s_pval[n] + at, sizeof s_pval[n] - at, "]");
        n++;
    }
    return n;
}

int enhance_menu_pending(const char **keys, const char **values, int max)
{
    int n = collect();
    for (int i = 0; i < n && i < max; i++) {
        keys[i] = s_pkey[i];
        values[i] = s_pnull[i] ? NULL : s_pval[i];
    }
    return n;
}

int enhance_menu_unsaved(void)
{
    int n = 0;
    for (int i = 0; i < s_nslot; i++)
        if (slot_saveable(&s_slot[i]) && strcmp(s_slot[i].cur, s_slot[i].base))
            n++;
    for (int id = 0; id < ENHANCE_ACTIONS_MAX; id++)
        n += s_adirty[id] != 0;
    return n;
}

/* Every missing directory above `path`, outermost first: a user config
 * path can sit several levels under a folder that does not exist yet.
 * Errors are left to the write that follows, which names the path. */
static void mkdir_parent(const char *path)
{
    char dir[1024];
    size_t n;
    snprintf(dir, sizeof dir, "%s", path);
    n = strlen(dir);
    for (size_t i = 1; i < n; i++) {
        int sep = dir[i] == '/';
#ifdef _WIN32
        sep = sep || dir[i] == '\\';
        if (i == 2 && dir[1] == ':')
            continue;   /* "C:\" is no directory to make */
#endif
        if (!sep || dir[i - 1] == '/' || dir[i - 1] == '\\')
            continue;
        dir[i] = 0;
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
        dir[i] = path[i];
    }
}

static void save(void)
{
    const char *path = enhance_cfg_user_path();
    const char *keys[sizeof s_pkey / sizeof s_pkey[0]];
    const char *vals[sizeof s_pkey / sizeof s_pkey[0]];
    char err[160];
    int n;
    if (!path) {
        status("Save is off (RECOMP_CONFIG=none)");
        return;
    }
    n = enhance_menu_pending(keys, vals, (int)(sizeof keys / sizeof keys[0]));
    if (!n) {
        status("Nothing to save");
        return;
    }
    mkdir_parent(path);
    if (recomp_cfg_patch(path, keys, vals, (size_t)n, err, sizeof err) < 0) {
        status("Save failed: %s", err);
        fprintf(stderr, LOGP "menu: save to %s failed: %s\n", path, err);
        return;
    }
    for (int i = 0; i < s_nslot; i++)
        memcpy(s_slot[i].base, s_slot[i].cur, sizeof s_slot[i].base);
    memset(s_adirty, 0, sizeof s_adirty);
    fprintf(stderr, LOGP "menu: saved %d key%s to %s\n", n, n == 1 ? "" : "s", path);
    status("Saved %d change%s", n, n == 1 ? "" : "s");
}

static void reset_all(void)
{
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        char stock[VAL_MAX];
        if (s->env || s->it->kind == ENHANCE_ITEM_INFO || s->it->kind == ENHANCE_ITEM_ACTION
            || !slot_stock(s, stock, sizeof stock) || !strcmp(stock, s->cur))
            continue;
        slot_set(s, stock);
    }
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++) {
        bind_list cur, stock;
        int nc, ns;
        if (!enhance_action_name(id))
            continue;
        nc = enhance_action_bindings(id, cur, ENHANCE_ACTION_BINDS);
        ns = action_stock(id, stock);
        if (!lists_equal(cur, nc, stock, ns))
            action_set(id, stock, ns);
    }
    status("Reset to defaults; Save keeps it");
}

/* ---- the state machine ---------------------------------------------------- */

static void stats_page_use(void)
{
    int want = s_state == ST_OPEN && (s_page == PAGE_STATS || s_page == PAGE_DEBUG);
    if (want != s_stats_on) {
        nv2a_stats_use(NV2A_STATS_PAGE, want);
        s_stats_on = want;
        s_rate_t0 = 0;
    }
}

static void snapshot_use(int on)
{
    if (on != s_snap_on) {
        nv2a_tex_pack_snapshot_want(on);
        s_snap_on = on;
    }
}

int enhance_menu_open(uint64_t now_ms)
{
    s_now = now_ms;
    if (!s_built)
        build_slots();
    /* What changed elsewhere since (the stats action, a mod's toggle)
     * shows as it is now; a change of the menu's own waits for Save. */
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        if (!strcmp(s->cur, s->base)) {
            slot_read(s);
            memcpy(s->base, s->cur, sizeof s->base);
        }
    }
    s_state = ST_OPEN;
    s_confirm = 0;
    cap_phase(CAP_OFF);
    /* One new number for the released pad, far from the real sequence: a
     * pad that changed once while the menu was open would otherwise come
     * back on this same number, and a title that skips a repeated number
     * would miss that real state (a stick held through the close). */
    for (int p = 0; p < PORTS; p++)
        s_fpkt[p] = s_pkt[p] + 0x40000000u;
    snapshot_use(1);
    stats_page_use();
    s_need_draw = 1;
    return 1;
}

void enhance_menu_close(void)
{
    if (s_state != ST_OPEN)
        return;
    s_state = ST_CLOSING;
    s_close_t = s_now;
    cap_phase(CAP_OFF);
    s_confirm = 0;
    snapshot_use(0);
    stats_page_use();
    nv2a_overlay_hide(NV2A_OVL_MENU);
}

int enhance_menu_is_open(void)
{
    return __atomic_load_n(&s_state, __ATOMIC_RELAXED) == ST_OPEN;
}

static void page_to(int p)
{
    s_page = (p + PAGE_COUNT) % PAGE_COUNT;
    s_mod_sub = -1;
    s_confirm = 0;
    stats_page_use();
}

static void move_sel(int d)
{
    int *sel = &s_sel[s_page];
    if (!s_nrow)
        return;
    for (int k = 0; k < s_nrow; k++) {
        *sel = (*sel + d + s_nrow) % s_nrow;
        if (s_row[*sel].sel)
            return;
    }
}

static void change_slot(struct slot *s, int dir, int ok)
{
    const struct enhance_item *it = s->it;
    char v[VAL_MAX];
    if (s->env) {
        status("Set by the environment");
        return;
    }
    switch (it->kind) {
    case ENHANCE_ITEM_TOGGLE:
        slot_set(s, on_text(s->cur) ? "false" : "true");
        break;
    case ENHANCE_ITEM_CHOICE: {
        int n = 0, i;
        if (!it->choices || !it->choices[0])
            return;
        while (it->choices[n])
            n++;
        i = index_of(it->choices, s->cur);
        i = i < 0 ? (dir < 0 ? n - 1 : 0) : (i + (dir < 0 ? -1 : 1) + n) % n;
        slot_set(s, it->choices[i]);
        break;
    }
    case ENHANCE_ITEM_RANGE: {
        double x = atof(s->cur), step = it->step > 0 ? it->step : 1;
        x += dir < 0 ? -step : step;
        if (x > it->hi + 1e-9)
            x = ok ? it->lo : it->hi;
        if (x < it->lo - 1e-9)
            x = it->lo;
        snprintf(v, sizeof v, "%g", x);
        slot_set(s, v);
        break;
    }
    case ENHANCE_ITEM_ACTION:
        if (ok && it->set)
            it->set(it, NULL);
        break;
    default:
        break;
    }
    if (it->live == ENHANCE_LIVE_RESTART && it->kind != ENHANCE_ITEM_ACTION)
        status("%s applies at restart", it->label);
}

static void clear_binding(int id)
{
    const char *name = enhance_action_name(id);
    if (name && !strcmp(name, "menu.open")) {
        status("menu.open keeps a binding: the menu could not open again");
        return;
    }
    enhance_action_set_bindings(id, NULL, 0);
    if (id >= 0 && id < ENHANCE_ACTIONS_MAX)
        s_adirty[id] = 1;
    status("%s: unbound", enhance_action_label(id));
}

int enhance_menu_event(enum enhance_menu_event ev, uint64_t now_ms)
{
    struct row *r;
    s_now = now_ms;
    if (s_state != ST_OPEN || ev == ENHANCE_EV_NONE)
        return 0;
    build_rows();
    r = s_nrow ? &s_row[s_sel[s_page]] : NULL;
    if (r && !r->sel)
        r = NULL;
    /* Leaving the Reset row or answering anything but OK cancels the ask. */
    if (s_confirm && !(r && r->type == R_RESET && ev == ENHANCE_EV_OK)) {
        s_confirm = 0;
        if (ev == ENHANCE_EV_BACK)
            return 1;
    }
    switch (ev) {
    case ENHANCE_EV_UP: move_sel(-1); break;
    case ENHANCE_EV_DOWN: move_sel(1); break;
    case ENHANCE_EV_PAGE_PREV: page_to(s_page - 1); break;
    case ENHANCE_EV_PAGE_NEXT: page_to(s_page + 1); break;
    case ENHANCE_EV_SAVE: save(); break;
    case ENHANCE_EV_CLOSE: enhance_menu_close(); break;
    case ENHANCE_EV_BACK:
        if (s_mod_sub >= 0) {
            s_sel[PAGE_MODS] = s_mod_sub;
            s_mod_sub = -1;
        } else {
            enhance_menu_close();
        }
        break;
    case ENHANCE_EV_CLEAR:
        if (r && r->type == R_BIND)
            clear_binding(r->ref);
        break;
    case ENHANCE_EV_LEFT:
    case ENHANCE_EV_RIGHT:
    case ENHANCE_EV_OK: {
        int dir = ev == ENHANCE_EV_LEFT ? -1 : 1, ok = ev == ENHANCE_EV_OK;
        if (!r)
            break;
        switch (r->type) {
        case R_SLOT:
            change_slot(&s_slot[r->ref], dir, ok);
            break;
        case R_MOD: {
            const struct enhance_item *items[1];
            char key[64];
            int t;
            if (ok && xbox_mod_items(r->ref, items, 1) > 0) {
                s_mod_sub = r->ref;
                s_sel[PAGE_MODS] = 0;
                break;
            }
            snprintf(key, sizeof key, "mods.%s", xbox_mod_name(r->ref));
            t = slot_find(key);
            if (t >= 0)
                change_slot(&s_slot[t], dir, ok);
            break;
        }
        case R_BIND:
            if (ok) {
                cap_phase(CAP_RELEASE);
                s_cap.id = r->ref;
                s_cap.t0 = now_ms;
                s_cap.max = s_cap.last = 0;
                status("Press a key or hold a pad chord (Esc cancels)");
            }
            break;
        case R_SAVE:
            if (ok)
                save();
            break;
        case R_RESET:
            if (!ok)
                break;
            if (s_confirm) {
                s_confirm = 0;
                reset_all();
            } else {
                s_confirm = 1;
            }
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return 1;
}

/* ---- capture -------------------------------------------------------------- */

static int is_modifier(int vk)
{
    return vk == XBOX_VK_SHIFT || vk == XBOX_VK_CONTROL || vk == XBOX_VK_MENU
        || (vk >= XBOX_VK_LSHIFT && vk <= XBOX_VK_RMENU);
}

static int any_key_down(void)
{
    /* 1..6 are the mouse buttons as VKs: a click is no key. */
    for (int vk = 8; vk < 256; vk++)
        if (xbox_FramebufferKeyDown(vk))
            return 1;
    return 0;
}

static const uint32_t k_host_only = (1u << INPUT_SRC_GUIDE) | (1u << INPUT_SRC_MISC1)
                                  | (1u << INPUT_SRC_TOUCHPAD) | (1u << INPUT_SRC_PADDLE1)
                                  | (1u << INPUT_SRC_PADDLE2) | (1u << INPUT_SRC_PADDLE3)
                                  | (1u << INPUT_SRC_PADDLE4);

/* 1 when two bindings name the same chord in any order and case: "a+guide"
 * is "guide+a". The hold ("@150") must match as well. */
static int chord_same(const char *a, const char *b)
{
    char ma[8][ENHANCE_ACTION_TEXT], mb[8][ENHANCE_ACTION_TEXT];
    const char *ha = strchr(a, '@'), *hb = strchr(b, '@');
    int na = 0, nb = 0;
    if ((ha ? strcmp(ha, hb ? hb : "") : hb != NULL) != 0)
        return 0;
    for (int side = 0; side < 2; side++) {
        const char *p = side ? b : a, *end = side ? (hb ? hb : b + strlen(b)) : (ha ? ha : a + strlen(a));
        char (*m)[ENHANCE_ACTION_TEXT] = side ? mb : ma;
        int *nm = side ? &nb : &na;
        while (p < end) {
            const char *q = memchr(p, '+', (size_t)(end - p));
            size_t len = (size_t)((q ? q : end) - p), i;
            if (*nm == 8 || len >= ENHANCE_ACTION_TEXT)
                return !strcmp(a, b);
            for (i = 0; i < len; i++)
                m[*nm][i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] - 'A' + 'a' : p[i]);
            m[(*nm)++][len] = 0;
            p = q ? q + 1 : end;
        }
    }
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++) {
        int found = 0;
        for (int j = 0; j < nb && !found; j++)
            if (mb[j][0] && !strcmp(ma[i], mb[j])) {
                mb[j][0] = 0;   /* each member counts once */
                found = 1;
            }
        if (!found)
            return 0;
    }
    return 1;
}

static void capture_finish(const char *text)
{
    bind_list l;
    char why[128], bound[ENHANCE_ACTION_TEXT];
    int n, id = s_cap.id, owner;
    cap_phase(CAP_OFF);
    if (!enhance_action_check(text, why, sizeof why)) {
        status("%s refused: %s", text, why);
        nv2a_overlay_toast("%s refused: %s", text, why);
        return;
    }
    n = enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS);
    for (int i = 0; i < n; i++)
        if (chord_same(l[i], text)) {
            status("%s is bound already", text);
            return;
        }
    /* One input, one action: a key or chord another action has would fire
     * both (a Deck grip that Steam Input sends as F10, menu.open's). Clear
     * it there first. */
    owner = enhance_action_owner(text, id, bound, sizeof bound);
    if (owner >= 0) {
        const char *who = enhance_action_label(owner);
        status("%s refused: %s has it (%s); clear it there first", text,
               who ? who : "another action", bound);
        nv2a_overlay_toast("%s refused: %s has it (%s); clear it there first", text,
                           who ? who : "another action", bound);
        return;
    }
    if (n >= ENHANCE_ACTION_BINDS) {
        status("%s refused: %d bindings at most", text, ENHANCE_ACTION_BINDS);
        return;
    }
    snprintf(l[n], ENHANCE_ACTION_TEXT, "%s", text);
    action_set(id, l, n + 1);
    status("%s: bound %s", enhance_action_label(id), text);
}

static void capture_pad_text(uint32_t set, char *out, size_t n)
{
    size_t at = 0;
    int members = 0;
    /* Host-only members first, as the defaults spell them ("guide+a"). */
    for (int pass = 0; pass < 2; pass++)
        for (int src = 1; src < INPUT_SRC_COUNT && at < n; src++)
            if ((set & (1u << src)) && ((k_host_only & (1u << src)) != 0) == (pass == 0)) {
                at += (size_t)snprintf(out + at, n - at, "%s%s", at ? "+" : "",
                                       xbox_InputMap_SourceName(src));
                members++;
            }
    /* A guest chord needs its hold (rule 3); a host-only member makes the
     * chord the host's at once, and one source has no chord to hold. */
    if (members > 1 && !(set & k_host_only) && at < n)
        snprintf(out + at, n - at, "@%u", CHORD_MS);
}

/* One capture step: 1 when it used the input. */
static int capture_step(uint32_t held, uint64_t now)
{
    char text[ENHANCE_ACTION_TEXT];
    if (s_cap.phase == CAP_OFF)
        return 0;
    if (now - s_cap.t0 >= CAPTURE_MS) {
        cap_phase(CAP_OFF);
        status("Capture timed out");
        return 1;
    }
    if (s_cap.phase == CAP_RELEASE) {
        /* The press that started the capture (A, Enter) must go first. */
        if (!held && !any_key_down())
            cap_phase(CAP_ARMED);
        return 1;
    }
    for (int vk = 8; vk < 256; vk++) {
        int down = xbox_FramebufferKeyDown(vk) != 0;
        size_t at = 0;
        const char *name;
        if (!down || s_kprev[vk] || is_modifier(vk))
            continue;
        if (vk == 0x1B) {   /* Esc */
            cap_phase(CAP_OFF);
            status("Capture cancelled");
            return 1;
        }
        name = xbox_InputCodeName(vk, 0);
        if (!name)
            continue;
        text[0] = 0;
        if (xbox_FramebufferKeyDown(XBOX_VK_CONTROL))
            at += (size_t)snprintf(text + at, sizeof text - at, "%s+", xbox_InputCodeName(XBOX_VK_CONTROL, 0));
        if (xbox_FramebufferKeyDown(XBOX_VK_SHIFT))
            at += (size_t)snprintf(text + at, sizeof text - at, "%s+", xbox_InputCodeName(XBOX_VK_SHIFT, 0));
        if (xbox_FramebufferKeyDown(XBOX_VK_MENU))
            at += (size_t)snprintf(text + at, sizeof text - at, "%s+", xbox_InputCodeName(XBOX_VK_MENU, 0));
        snprintf(text + at, sizeof text - at, "%s", name);
        capture_finish(text);
        return 1;
    }
    if (held) {
        if ((held | s_cap.max) != s_cap.max || held != s_cap.last)
            s_cap.t_set = now;
        s_cap.max |= held;
        s_cap.last = held;
        if (held == s_cap.max && now - s_cap.t_set >= CHORD_MS) {
            capture_pad_text(s_cap.max, text, sizeof text);
            capture_finish(text);
        }
    } else if (s_cap.max) {
        capture_pad_text(s_cap.max, text, sizeof text);
        capture_finish(text);
    }
    return 1;
}

int enhance_menu_capturing(void)
{
    return s_cap.phase != CAP_OFF;
}

/* ---- the input step ------------------------------------------------------- */

static const uint32_t k_up = (1u << INPUT_SRC_DPAD_UP) | (1u << INPUT_SRC_LSTICK_UP);
static const uint32_t k_down = (1u << INPUT_SRC_DPAD_DOWN) | (1u << INPUT_SRC_LSTICK_DOWN);
static const uint32_t k_left = (1u << INPUT_SRC_DPAD_LEFT) | (1u << INPUT_SRC_LSTICK_LEFT);
static const uint32_t k_right = (1u << INPUT_SRC_DPAD_RIGHT) | (1u << INPUT_SRC_LSTICK_RIGHT);

static int key_edge(int vk)
{
    return xbox_FramebufferKeyDown(vk) && !s_kprev[vk];
}

static void keys_latch(void)
{
    for (int vk = 0; vk < 256; vk++)
        s_kprev[vk] = xbox_FramebufferKeyDown(vk) != 0;
}

static void input_step(uint64_t now)
{
    uint32_t held = 0, press;
    int dir = 0, redraw = 0;
    for (int p = 0; p < PORTS; p++)
        held |= s_held[p];
    for (int src = 1; src < INPUT_SRC_COUNT; src++)
        if ((held & (1u << src)) & k_host_only)
            s_extra_t[src] = now ? now : 1;
    press = held & ~s_prev_held;

    if (capture_step(held, now)) {
        s_need_draw = 1;
        s_prev_held = held;
        keys_latch();
        return;
    }

    if (press)
        s_kbd = 0;
#define EV(e) do { redraw |= enhance_menu_event((e), now); } while (0)
    if (press & (1u << INPUT_SRC_SOUTH)) EV(ENHANCE_EV_OK);
    if (press & (1u << INPUT_SRC_EAST)) EV(ENHANCE_EV_BACK);
    if (press & (1u << INPUT_SRC_WEST)) EV(ENHANCE_EV_CLEAR);
    if (press & (1u << INPUT_SRC_NORTH)) EV(ENHANCE_EV_SAVE);
    if (press & (1u << INPUT_SRC_LEFT_SHOULDER)) EV(ENHANCE_EV_PAGE_PREV);
    if (press & (1u << INPUT_SRC_RIGHT_SHOULDER)) EV(ENHANCE_EV_PAGE_NEXT);
    {
        static const struct { int vk; enum enhance_menu_event ev; } k_keys[] = {
            { 0x0D, ENHANCE_EV_OK }, { 0x20, ENHANCE_EV_OK },
            { 0x1B, ENHANCE_EV_BACK }, { 0x08, ENHANCE_EV_BACK },
            { 'Q', ENHANCE_EV_PAGE_PREV }, { 0x21, ENHANCE_EV_PAGE_PREV },
            { 'E', ENHANCE_EV_PAGE_NEXT }, { 0x22, ENHANCE_EV_PAGE_NEXT },
            { 0x74, ENHANCE_EV_SAVE }, { 0x2E, ENHANCE_EV_CLEAR },
        };
        for (unsigned i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
            if (key_edge(k_keys[i].vk)) {
                s_kbd = 1;
                if (s_state == ST_OPEN)
                    EV(k_keys[i].ev);
            }
    }
    /* Directions repeat while held. */
    if ((held & k_up) || xbox_FramebufferKeyDown(0x26))
        dir = ENHANCE_EV_UP;
    else if ((held & k_down) || xbox_FramebufferKeyDown(0x28))
        dir = ENHANCE_EV_DOWN;
    else if ((held & k_left) || xbox_FramebufferKeyDown(0x25))
        dir = ENHANCE_EV_LEFT;
    else if ((held & k_right) || xbox_FramebufferKeyDown(0x27))
        dir = ENHANCE_EV_RIGHT;
    if (dir && xbox_FramebufferKeyDown(dir == ENHANCE_EV_UP ? 0x26 : dir == ENHANCE_EV_DOWN ? 0x28
                                       : dir == ENHANCE_EV_LEFT ? 0x25 : 0x27))
        s_kbd = 1;
    if (dir != s_dir) {
        s_dir = dir;
        s_dir_next = now + REPEAT_FIRST;
        if (dir && s_state == ST_OPEN)
            EV((enum enhance_menu_event)dir);
    } else if (dir && now >= s_dir_next) {
        s_dir_next = now + REPEAT_NEXT;
        if (s_state == ST_OPEN)
            EV((enum enhance_menu_event)dir);
    }
#undef EV
    if (redraw)
        s_need_draw = 1;
    s_prev_held = held;
    keys_latch();
}

/* ---- drawing -------------------------------------------------------------- */

static mu_Color with_alpha(mu_Color c, int a)
{
    c.a = (unsigned char)a;
    return c;
}

static int text_w(mu_Font f, const char *s)
{
    return nv2a_ovl_text_width(f, s, -1);
}

/* s cut to fit w with an ellipsis; returns the bytes to draw (out holds
 * the cut text). */
static const char *fit(mu_Font f, const char *s, int w, char *out, size_t n)
{
    size_t len = strlen(s);
    if (text_w(f, s) <= w)
        return s;
    while (len > 0) {
        len--;
        /* Back to a UTF-8 lead byte. */
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
            len--;
        snprintf(out, n, "%.*s\xE2\x80\xA6", (int)len, s);
        if (text_w(f, out) <= w)
            return out;
    }
    snprintf(out, n, "\xE2\x80\xA6");
    return out;
}

static const char *live_tag(int live)
{
    switch (live) {
    case ENHANCE_LIVE_FLUSH: return "at the flip";
    case ENHANCE_LIVE_STAGE: return "next stage";
    case ENHANCE_LIVE_RESTART: return "at restart";
    default: return "live";
    }
}

static void draw_pill(mu_Context *ctx, mu_Rect r, int on, int dim)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    int k = r.h - 6, kx = on ? r.x + r.w - k - 3 : r.x + 3;
    nv2a_ovl_ui_rrect(ctx, r, r.h / 2, 1.0f, on ? (dim ? t->text_dim : t->accent) : t->bg_row);
    nv2a_ovl_ui_rrect(ctx, mu_rect(kx, r.y + 3, k, k), k / 2, 1.0f, on ? t->text : t->text_dim);
}

static void draw_row(mu_Context *ctx, const struct row *rw, mu_Rect r, int selected, float s)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    mu_Font fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    mu_Font fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    mu_Font fv = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 1);
    int pad = nv2a_ovl_px(s, t->pad), th = nv2a_ovl_text_height(fb);
    int ty = r.y + (r.h - th) / 2, x = r.x + pad, right = r.x + r.w - pad;
    const struct slot *sl = rw->type == R_SLOT ? &s_slot[rw->ref] : NULL;
    const struct enhance_item *it = sl ? sl->it : NULL;
    int ro = sl && sl->env;
    char cut[VAL_MAX * 2];
    const char *v;

    if (rw->type == R_HEADER) {
        int hs = nv2a_ovl_text_height(fs);
        mu_draw_text(ctx, fs, rw->label, -1, mu_vec2(x, r.y + r.h - hs - nv2a_ovl_px(s, 4)), t->accent);
        return;
    }
    if (selected) {
        nv2a_ovl_ui_rrect(ctx, r, nv2a_ovl_px(s, t->radius) / 2, 1.0f, t->bg_hover);
        mu_draw_rect(ctx, mu_rect(r.x, r.y + nv2a_ovl_px(s, 6), nv2a_ovl_px(s, 3),
                                  r.h - 2 * nv2a_ovl_px(s, 6)), t->accent);
    }
    /* The label, then its class: a dot for a change that shows now, words
     * for one that waits. */
    mu_draw_text(ctx, fb, rw->label, -1, mu_vec2(x, ty), ro ? t->text_dim : t->text);
    x += text_w(fb, rw->label) + nv2a_ovl_px(s, 8);
    if (it && it->kind != ENHANCE_ITEM_INFO) {
        int fsh = nv2a_ovl_text_height(fs), fy = r.y + (r.h - fsh) / 2;
        if (ro) {
            mu_draw_text(ctx, fs, "(environment)", -1, mu_vec2(x, fy), t->text_dim);
            x += text_w(fs, "(environment)") + nv2a_ovl_px(s, 8);
        } else if (it->live == ENHANCE_LIVE || it->live == ENHANCE_LIVE_FLUSH) {
            int d = nv2a_ovl_px(s, 6);
            nv2a_ovl_ui_rrect(ctx, mu_rect(x, r.y + (r.h - d) / 2, d, d), d / 2, 1.0f,
                              with_alpha(t->accent, 200));
            x += d + nv2a_ovl_px(s, 8);
        } else {
            const char *w = it->live == ENHANCE_LIVE_RESTART ? "applies at restart" : "next stage";
            mu_draw_text(ctx, fs, w, -1, mu_vec2(x, fy),
                         it->live == ENHANCE_LIVE_RESTART ? t->warn : t->text_dim);
            x += text_w(fs, w) + nv2a_ovl_px(s, 8);
        }
        if (it->note) {
            mu_draw_text(ctx, fs, it->note, -1, mu_vec2(x, fy), t->text_dim);
            x += text_w(fs, it->note) + nv2a_ovl_px(s, 8);
        }
    }

    if (rw->type == R_MOD || (it && it->kind == ENHANCE_ITEM_TOGGLE)) {
        int on = rw->type == R_MOD ? xbox_mod_enabled(rw->ref) : on_text(rw->value);
        int ph = nv2a_ovl_px(s, 24), pw = nv2a_ovl_px(s, 44);
        draw_pill(ctx, mu_rect(right - pw, r.y + (r.h - ph) / 2, pw, ph), on, ro);
        if (rw->type == R_MOD) {
            enum xbox_mod_state st = xbox_mod_state(rw->ref);
            const struct enhance_item *items[1];
            const char *w = st == XBOX_MOD_FAILED ? "failed"
                          : st == XBOX_MOD_RESTART || !xbox_mod_has_on_enable(rw->ref)
                              ? "applies at restart" : NULL;
            int fsh = nv2a_ovl_text_height(fs), fy = r.y + (r.h - fsh) / 2;
            if (w)
                mu_draw_text(ctx, fs, w, -1, mu_vec2(x, fy),
                             st == XBOX_MOD_FAILED ? t->bad : t->warn);
            if (xbox_mod_items(rw->ref, items, 1) > 0)
                mu_draw_text(ctx, fb, ">", -1,
                             mu_vec2(right - pw - nv2a_ovl_px(s, 20), ty), t->text_dim);
        }
        return;
    }
    if (it && it->kind == ENHANCE_ITEM_RANGE) {
        int tw = nv2a_ovl_px(s, 160), tk = nv2a_ovl_px(s, 4), kn = nv2a_ovl_px(s, 14);
        int tx = right - tw, cy = r.y + r.h / 2;
        double f = it->hi > it->lo ? (atof(rw->value) - it->lo) / (it->hi - it->lo) : 0;
        int fx;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        fx = tx + (int)(f * tw + 0.5);
        nv2a_ovl_ui_rrect(ctx, mu_rect(tx, cy - tk / 2, tw, tk), tk / 2, 1.0f, t->bg_row);
        nv2a_ovl_ui_rrect(ctx, mu_rect(tx, cy - tk / 2, fx - tx, tk), tk / 2, 1.0f,
                          ro ? t->text_dim : t->accent);
        nv2a_ovl_ui_rrect(ctx, mu_rect(fx - kn / 2, cy - kn / 2, kn, kn), kn / 2, 1.0f, t->text);
        mu_draw_text(ctx, fv, rw->value, -1,
                     mu_vec2(tx - nv2a_ovl_px(s, 12) - text_w(fv, rw->value), ty), t->text);
        return;
    }
    if (it && it->kind == ENHANCE_ITEM_CHOICE) {
        int aw = text_w(fb, "<"), sp = nv2a_ovl_px(s, 10);
        int vw = text_w(fb, rw->value);
        mu_draw_text(ctx, fb, ">", -1, mu_vec2(right - aw, ty), t->text_dim);
        mu_draw_text(ctx, fb, rw->value, -1, mu_vec2(right - aw - sp - vw, ty),
                     ro ? t->text_dim : t->text);
        mu_draw_text(ctx, fb, "<", -1, mu_vec2(right - 2 * aw - 2 * sp - vw, ty), t->text_dim);
        return;
    }
    /* Text values (info, bindings, Save's path): right-aligned, cut to the
     * room the label left. */
    if (rw->type == R_BIND && s_cap.phase != CAP_OFF && s_cap.id == rw->ref) {
        v = fit(fb, "press a key or a chord\xE2\x80\xA6", right - x, cut, sizeof cut);
        mu_draw_text(ctx, fb, v, -1, mu_vec2(right - text_w(fb, v), ty), t->warn);
        return;
    }
    if (!rw->value[0])
        return;
    v = fit(fb, rw->value, right - x, cut, sizeof cut);
    mu_draw_text(ctx, fb, v, -1, mu_vec2(right - text_w(fb, v), ty),
                 rw->tint ? *rw->tint : t->text_dim);
}

static void draw_graph(mu_Context *ctx, mu_Rect g, float s)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float ms[120], top = 33.4f;
    unsigned n = nv2a_stats_frames(ms, 120);
    int bw;
    nv2a_ovl_ui_rrect(ctx, g, nv2a_ovl_px(s, t->radius) / 2, 1.0f, t->bg_row);
    if (!n)
        return;
    for (unsigned i = 0; i < n; i++)
        if (ms[i] > top)
            top = ms[i];
    bw = g.w / 120 > 0 ? g.w / 120 : 1;
    for (unsigned i = 0; i < n; i++) {
        int h = (int)(ms[i] / top * (float)(g.h - 4) + 0.5f);
        mu_Color c = ms[i] < 17.5f ? t->good : ms[i] < 34.0f ? t->warn : t->bad;
        if (h < 1)
            h = 1;
        mu_draw_rect(ctx, mu_rect(g.x + g.w - (int)(n - i) * bw, g.y + g.h - 2 - h, bw - (bw > 2), h), c);
    }
}

void enhance_menu_draw(uint64_t now_ms)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    mu_Context *ctx;
    int px, py, w, h, pad, gap, rowh, rgap, pw, ph, x0, y0, y, inner;
    int area_y, area_h, foot_h, vis, *scroll, sel;
    float s;
    mu_Font ft, fb, fs;

    s_now = now_ms;
    if (s_state != ST_OPEN)
        return;
    nv2a_overlay_placement(&px, &py, &w, &h);
    if (w <= 0 || h <= 0)
        w = 1920, h = 1080;
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    if (!ctx)
        return;
    rates(now_ms);
    build_rows();
    s_draw_w = w;
    s_draw_h = h;
    s = nv2a_ovl_ui_scale((uint32_t)h);
    ft = OVL_FONT(OVL_FONT_TITLE, nv2a_ovl_px(s, t->font_title), 0);
    fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    pad = nv2a_ovl_px(s, t->pad);
    gap = nv2a_ovl_px(s, t->gap);
    rowh = nv2a_ovl_px(s, t->row_h);
    rgap = nv2a_ovl_px(s, 4);

    /* R8: a panel on the left, 40% of the width up to 720 px at 1080; the
     * whole width on a narrow or portrait output. */
    pw = (int)(w * 0.40f);
    if (pw > nv2a_ovl_px(s, 720))
        pw = nv2a_ovl_px(s, 720);
    if (w < 640 || h > w)
        pw = w - 2 * pad;
    ph = h - 2 * pad;
    x0 = pad;
    y0 = pad;
    nv2a_ovl_ui_shadow(ctx, mu_rect(x0, y0, pw, ph), nv2a_ovl_px(s, t->radius),
                       nv2a_ovl_px(s, t->shadow), t->shadow_alpha);
    nv2a_ovl_ui_rrect(ctx, mu_rect(x0, y0, pw, ph), nv2a_ovl_px(s, t->radius), 1.0f, t->bg);
    inner = pw - 2 * pad;

    y = y0 + pad;
    mu_draw_text(ctx, ft, "Enhancements", -1, mu_vec2(x0 + pad, y), t->text);
    y += nv2a_ovl_text_height(ft) + gap;

    /* The tab strip, scrolled so the page shown is in view. */
    {
        int tx[PAGE_COUNT], tw[PAGE_COUNT], total = 0, off = 0, sp = nv2a_ovl_px(s, 20);
        int tabh = nv2a_ovl_text_height(fb);
        for (int p = 0; p < PAGE_COUNT; p++) {
            tx[p] = total;
            tw[p] = text_w(fb, s_page_name[p]);
            total += tw[p] + sp;
        }
        if (tx[s_page] + tw[s_page] > inner)
            off = tx[s_page] + tw[s_page] - inner;
        mu_push_clip_rect(ctx, mu_rect(x0 + pad, y, inner, tabh + nv2a_ovl_px(s, 8)));
        for (int p = 0; p < PAGE_COUNT; p++) {
            int xx = x0 + pad + tx[p] - off;
            mu_draw_text(ctx, fb, s_page_name[p], -1, mu_vec2(xx, y),
                         p == s_page ? t->text : t->text_dim);
            if (p == s_page)
                mu_draw_rect(ctx, mu_rect(xx, y + tabh + nv2a_ovl_px(s, 2), tw[p], nv2a_ovl_px(s, 3)),
                             t->accent);
        }
        mu_pop_clip_rect(ctx);
        y += tabh + nv2a_ovl_px(s, 8) + gap;
    }

    foot_h = nv2a_ovl_text_height(fs) + pad;
    area_y = y;
    area_h = y0 + ph - foot_h - gap - area_y;
    if (s_page == PAGE_STATS)
        area_h -= nv2a_ovl_px(s, 110) + gap;
    vis = area_h / (rowh + rgap);
    if (vis < 1)
        vis = 1;
    scroll = &s_scroll[s_page];
    sel = s_sel[s_page];
    if (sel < *scroll)
        *scroll = sel;
    if (sel >= *scroll + vis)
        *scroll = sel - vis + 1;
    if (*scroll > s_nrow - vis)
        *scroll = s_nrow - vis > 0 ? s_nrow - vis : 0;
    mu_push_clip_rect(ctx, mu_rect(x0, area_y, pw, area_h));
    for (int i = *scroll; i < s_nrow && i < *scroll + vis; i++) {
        mu_Rect r = mu_rect(x0 + pad / 2, area_y + (i - *scroll) * (rowh + rgap), pw - pad, rowh);
        draw_row(ctx, &s_row[i], r, i == sel && s_row[i].sel, s);
    }
    mu_pop_clip_rect(ctx);
    if (s_nrow > vis) {
        int bh = area_h * vis / s_nrow, by = area_y + (area_h - bh) * *scroll / (s_nrow - vis);
        int bw = nv2a_ovl_px(s, 4);
        nv2a_ovl_ui_rrect(ctx, mu_rect(x0 + pw - bw - nv2a_ovl_px(s, 4), by, bw, bh), bw / 2, 1.0f,
                          t->text_dim);
    }
    if (s_page == PAGE_STATS)
        draw_graph(ctx, mu_rect(x0 + pad, area_y + area_h + gap, inner, nv2a_ovl_px(s, 110)), s);

    /* The footer: the controls on the left, the news on the right. */
    {
        int fy = y0 + ph - foot_h + (foot_h - nv2a_ovl_text_height(fs)) / 2 - pad / 4;
        const char *hint = s_kbd
            ? "Arrows move \xC2\xB7 Enter select \xC2\xB7 Esc back \xC2\xB7 Q/E page \xC2\xB7 F5 save"
            : "A select \xC2\xB7 B back \xC2\xB7 X clear \xC2\xB7 Y save \xC2\xB7 LB/RB page";
        char right[160], cut[200];
        const char *rt;
        int un = enhance_menu_unsaved(), rw;
        const struct row *cur = s_nrow ? &s_row[sel] : NULL;
        const char *tag = cur && cur->type == R_SLOT ? live_tag(s_slot[cur->ref].it->live)
                        : cur && cur->type == R_MOD
                            ? live_tag(xbox_mod_has_on_enable(cur->ref) ? ENHANCE_LIVE
                                                                       : ENHANCE_LIVE_RESTART)
                        : cur && cur->type == R_BIND ? "live" : "";
        if (s_status[0] && s_status_t && now_ms - s_status_t < STATUS_MS)
            snprintf(right, sizeof right, "%s", s_status);
        else if (un)
            snprintf(right, sizeof right, "%d unsaved%s%s", un, *tag ? " \xC2\xB7 " : "", tag);
        else
            snprintf(right, sizeof right, "%s", tag);
        rt = fit(fs, right, inner / 2, cut, sizeof cut);
        rw = text_w(fs, rt);
        {
            char hcut[200];
            const char *hv = fit(fs, hint, inner - rw - gap, hcut, sizeof hcut);
            mu_draw_text(ctx, fs, hv, -1, mu_vec2(x0 + pad, fy), t->text_dim);
        }
        mu_draw_text(ctx, fs, rt, -1, mu_vec2(x0 + pad + inner - rw, fy),
                     s_status[0] && now_ms - s_status_t < STATUS_MS ? t->text : t->text_dim);
    }
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    s_draw_t = now_ms ? now_ms : 1;
    s_need_draw = 0;
}

/* ---- the hook --------------------------------------------------------------- */

static void neutral(unsigned port, XBOX_INPUT_STATE *st)
{
    memset(&st->Gamepad, 0, sizeof st->Gamepad);
    st->dwPacketNumber = s_fpkt[port];
}

void enhance_menu_hook(unsigned port, uint32_t held, XBOX_INPUT_STATE *st, uint64_t now_ms)
{
    int release = 0;
    if (port >= PORTS)
        return;
    if (!nv2a_overlay_menu_trylock()) {
        /* The other thread steps; the title still reads no pad. */
        if (st && __atomic_load_n(&s_state, __ATOMIC_RELAXED) != ST_CLOSED)
            neutral(port, st);
        return;
    }
    s_now = now_ms;
    s_held[port] = held;
    switch (s_state) {
    case ST_CLOSED:
        if (st)
            s_pkt[port] = st->dwPacketNumber;
        if (enhance_actions_menu_request()) {
            uint32_t all = 0;
            for (int p = 0; p < PORTS; p++)
                all |= s_held[p];
            enhance_menu_open(now_ms);
            /* What is held as it opens (the chord that opened it) is no
             * press. */
            s_prev_held = all;
            s_dir = -1;
            keys_latch();
        }
        break;
    case ST_OPEN:
        /* Quiet keeps menu.open from firing during a capture; the phase
         * test stays in case a request was queued before it began. */
        if (enhance_actions_menu_request() && s_cap.phase == CAP_OFF)
            enhance_menu_close();
        else
            input_step(now_ms);
        break;
    case ST_CLOSING:
        if (enhance_actions_menu_request()) {
            enhance_menu_open(now_ms);
            keys_latch();
        }
        break;
    default:
        break;
    }
    if (s_state == ST_OPEN) {
        int w, h, px, py, live = s_page == PAGE_STATS || s_page == PAGE_DEBUG
                                 || s_page == PAGE_PACK || s_page == PAGE_CONTROLS;
        nv2a_overlay_placement(&px, &py, &w, &h);
        if (w <= 0 || h <= 0)
            w = 1920, h = 1080;
        if (s_need_draw || w != s_draw_w || h != s_draw_h
            || (live && now_ms - s_draw_t >= LIVE_DRAW_MS)
            || (s_status_t && now_ms - s_status_t >= STATUS_MS && now_ms - s_draw_t >= STATUS_MS))
            enhance_menu_draw(now_ms);
    }
    if (s_state == ST_CLOSING) {
        uint32_t all = 0;
        for (int p = 0; p < PORTS; p++)
            all |= s_held[p];
        if ((!all && !any_key_down()) || now_ms - s_close_t >= CLOSE_MS) {
            s_state = ST_CLOSED;
            release = 1;
        }
    }
    /* The closing poll too: the filter took this poll's mask before the
     * release below marks the held buttons. */
    if (st && (s_state != ST_CLOSED || release))
        neutral(port, st);
    nv2a_overlay_menu_unlock();
    /* A button still down past the close is the title's again only once
     * it is let go, so the closing press never reaches the game. */
    if (release)
        for (unsigned p = 0; p < PORTS; p++)
            enhance_actions_mask_until_release(p, 0xFFFFFFFFu);
}

/* ---- start ------------------------------------------------------------------ */

void enhance_menu_start(void)
{
    char binds[160], l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    int id, n;
    size_t at = 0;
    if (!enhance_actions_keyed("menu.enabled")) {
        fprintf(stderr, LOGP "menu.enabled=off\n");
        return;
    }
    if (nv2a_overlay_arm(1u << NV2A_OVL_MENU, 1080) < 0) {
        fprintf(stderr, LOGP "menu.enabled=on but the overlay could not arm; no menu\n");
        return;
    }
    enhance_actions_set_menu_hook(enhance_menu_hook);
    id = enhance_action_find("menu.open");
    n = id >= 0 ? enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS) : 0;
    binds[0] = 0;
    for (int i = 0; i < n && at < sizeof binds; i++)
        at += (size_t)snprintf(binds + at, sizeof binds - at, "%s%s", i ? ", " : "", l[i]);
    fprintf(stderr, LOGP "menu.enabled=on (%s)\n", n ? binds : "menu.open unbound");
}

/* ---- introspection ---------------------------------------------------------- */

int enhance_menu_page_count(void) { return PAGE_COUNT; }

const char *enhance_menu_page_name(int page)
{
    return page >= 0 && page < PAGE_COUNT ? s_page_name[page] : NULL;
}

int enhance_menu_page(void) { return s_page; }

int enhance_menu_set_page(const char *name)
{
    int p = page_by_name(name, -1);
    if (p < 0)
        return 0;
    page_to(p);
    return 1;
}

int enhance_menu_rows(void)
{
    build_rows();
    return s_nrow;
}

int enhance_menu_selected(void)
{
    build_rows();
    return s_sel[s_page];
}

int enhance_menu_row(int i, char *label, size_t ln, char *value, size_t vn, int *selectable)
{
    if (i < 0 || i >= s_nrow)
        return 0;
    if (label && ln)
        snprintf(label, ln, "%s", s_row[i].label);
    if (value && vn) {
        if (s_row[i].type == R_MOD)
            snprintf(value, vn, "%s", xbox_mod_enabled(s_row[i].ref) ? "true" : "false");
        else
            snprintf(value, vn, "%s", s_row[i].value);
    }
    if (selectable)
        *selectable = s_row[i].sel;
    return 1;
}

int enhance_menu_select_row(const char *label)
{
    build_rows();
    for (int i = 0; i < s_nrow; i++)
        if (s_row[i].sel && !strcmp(s_row[i].label, label)) {
            s_sel[s_page] = i;
            return 1;
        }
    return 0;
}

const char *enhance_menu_status(void) { return s_status; }

void enhance_menu_reset_for_test(void)
{
    if (s_snap_on)
        nv2a_tex_pack_snapshot_want(0);
    if (s_stats_on)
        nv2a_stats_use(NV2A_STATS_PAGE, 0);
    s_snap_on = s_stats_on = 0;
    s_state = ST_CLOSED;
    s_page = 0;
    memset(s_sel, 0, sizeof s_sel);
    memset(s_scroll, 0, sizeof s_scroll);
    s_mod_sub = -1;
    s_confirm = 0;
    s_status[0] = 0;
    s_status_t = s_now = 0;
    s_nrow = 0;
    memset(&s_cap, 0, sizeof s_cap);
    enhance_actions_quiet(0);
    memset(s_held, 0, sizeof s_held);
    s_prev_held = 0;
    memset(s_kprev, 0, sizeof s_kprev);
    memset(s_pkt, 0, sizeof s_pkt);
    memset(s_fpkt, 0, sizeof s_fpkt);
    s_dir = 0;
    s_kbd = 0;
    s_dir_next = s_close_t = s_draw_t = 0;
    memset(s_extra_t, 0, sizeof s_extra_t);
    s_need_draw = 0;
    s_draw_w = s_draw_h = 0;
    s_rate_t0 = 0;
    memset(s_rate, 0, sizeof s_rate);
    memset(s_adirty, 0, sizeof s_adirty);
    s_nslot = 0;
    s_built = 0;
    s_ntitle = 0;
    s_pack_off = 0;
}
