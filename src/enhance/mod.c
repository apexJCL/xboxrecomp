/*
 * mod.c - see mod.h. The tables are filled during init on the main thread,
 * before the guest and the render thread start, and only read after: the
 * frame and draw paths take no lock. Shutdown leaves them as they are: the
 * guest's thread or the render thread may be inside a callback then, and
 * the filter pointer (nv2a_batch_filter_set) is what stops new asks.
 */
#include "mod.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_actions.h"
#include "enhance_cfg.h"
#ifdef XBOXRECOMP_ENHANCE
#include "nv2a_overlay.h"
#endif
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
    if (s_init_owner < 0)
        return enhance_actions_armed();
    return enhance_actions_arm_owned(key, s_init_owner);
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

void xbox_mod_register_table(const struct xbox_mod *const *mods)
{
    s_table = mods;
}

int xbox_mod_init(void)
{
    int i;

    s_nrunning = s_nframe = s_nfilter = s_nitems = s_count = 0;
    s_any_pending = 0;
    s_shut = 0;
    memset(s_on_enable, 0, sizeof s_on_enable);
    memset(s_on_enable_user, 0, sizeof s_on_enable_user);
    memset(&s_api, 0, sizeof s_api);
    enhance_actions_drop_mods();
    if (!s_table || !s_table[0]) {
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

    for (i = 0; s_table[i]; i++) {
        const struct xbox_mod *m = s_table[i];
        int rc = 0, held = enhance_input_captured();

        if (i >= MOD_MAX) {
            fprintf(stderr, LOGP "%s: more than %d mods; left out\n",
                    m->name ? m->name : "?", MOD_MAX);
            continue;
        }
        s_count = i + 1;
        s_started[i] = 0;
        s_pending[i] = PENDING_NONE;
        snprintf(s_key[i], sizeof s_key[i], "mods.%s", m->name ? m->name : "?");
        /* The Mods page's key: a mod switched off there is not even inited
         * at the next start, so it registers nothing and costs nothing. */
        s_want[i] = enhance_cfg_bool(s_key[i], 1);
        if (!s_want[i]) {
            s_state[i] = XBOX_MOD_OFF;
            fprintf(stderr, LOGP "%s: off (%s)\n", m->name ? m->name : "?", s_key[i]);
            enhance_cfg_mark_read_prefix(m->name);
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
            fprintf(stderr, LOGP "%s: init returned %d; left out\n",
                    m->name ? m->name : "?", rc);
            s_state[i] = XBOX_MOD_FAILED;
            continue;
        }
        s_running[s_nrunning++] = m;
        s_started[i] = 1;
        s_state[i] = XBOX_MOD_ON;
    }

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

void xbox_mod_frame(void)
{
    int i;

    if (!s_nrunning)
        return;
    mod_mem();
    if (__atomic_load_n(&s_shut, __ATOMIC_ACQUIRE))
        return;
    /* The Mods page's toggles, on the thread the mods' frames run on. */
    if (__atomic_load_n(&s_any_pending, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&s_any_pending, 0, __ATOMIC_ACQ_REL)) {
        /* Cleared before the scan: a toggle stored during it sets the flag
         * again and is seen next frame at the latest. */
        for (i = 0; i < s_count; i++) {
            int on = __atomic_exchange_n(&s_pending[i], PENDING_NONE, __ATOMIC_ACQ_REL);
            if (on != PENDING_NONE && s_on_enable[i])
                s_on_enable[i](on, s_on_enable_user[i]);
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
    return __atomic_load_n(&s_want[i], __ATOMIC_RELAXED);
}

enum xbox_mod_state xbox_mod_set_enabled(int i, int on)
{
    enum xbox_mod_state st;

    if (i < 0 || i >= s_count)
        return XBOX_MOD_FAILED;
    on = on != 0;
    __atomic_store_n(&s_want[i], on, __ATOMIC_RELAXED);
    if (s_state[i] == XBOX_MOD_FAILED)
        return XBOX_MOD_FAILED;     /* the key still changes, for the next start */
    if (xbox_mod_has_on_enable(i)) {
        __atomic_store_n(&s_pending[i], on, __ATOMIC_RELEASE);
        __atomic_store_n(&s_any_pending, 1, __ATOMIC_RELEASE);
        st = on ? XBOX_MOD_ON : XBOX_MOD_OFF;
    } else if (on == s_started[i]) {
        st = on ? XBOX_MOD_ON : XBOX_MOD_OFF;
    } else {
        st = XBOX_MOD_RESTART;
    }
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
