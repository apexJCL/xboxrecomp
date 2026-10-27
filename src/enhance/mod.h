/*
 * mod.h - the enhancements layer's mods: game code built into the title's
 * executable that reads and steers the guest through one small API
 * (opt-in, XBOXRECOMP_ENHANCE=ON).
 *
 * A mod is a struct xbox_mod. The title lists its mods in a NULL-terminated
 * table and hands it over before xbox_enhance_init:
 *
 *   static const struct xbox_mod *const mods[] = { &free_camera, NULL };
 *   xbox_mod_register_table(mods);
 *   xbox_enhance_init(".", NULL);
 *
 * A call and not a self-registering macro: a static library drops an object
 * nothing names, a linker section is spelled differently for PE, Mach-O and
 * ELF, and the table gives the order. A title that calls nothing has no
 * mods and stays stock.
 *
 * xbox_enhance_init runs each mod's init, in table order, as its last step
 * (after the config files and the [input] keys, so the getters see every
 * file). The guest has not started then and its memory is not mapped yet:
 * a mod registers what it wants and touches the guest from its on_frame
 * callbacks on. A non-zero init leaves that mod out: what it registered is
 * dropped, its capture released and its shutdown never called. Shutdown
 * runs in reverse order, once, at exit (atexit, and recomp_exit_hook for
 * the Win32 windows' ExitProcess) or on xbox_mod_shutdown.
 *
 * Shutdown can run while the guest and the render thread still do: a
 * frame or a filter call may be in flight, or start just before the filter
 * is removed. A mod's shutdown therefore must not free or invalidate
 * anything its on_frame or filter callbacks use; restore guest state, give
 * input back, write files, and leave the memory to the process exit.
 *
 * The title's glue calls two entries: xbox_mod_frame once per guest frame,
 * from the guest's thread, and xbox_mod_input_captured from its pad read,
 * where a captured pad reads as released to the title.
 *
 * With no mods: no lines, no callbacks, and the draw paths' one branch
 * (nv2a_batch_skip) is never taken. Docs: docs/runtime/config.md, "Mods".
 */
#ifndef XBOX_ENHANCE_MOD_H
#define XBOX_ENHANCE_MOD_H

#include <stddef.h>
#include <stdint.h>

#include "enhance_actions.h"
#include "enhance_input.h"
#include "host_pad.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_MOD_API_VERSION 3u

struct xbox_mod_api;
struct xbox_mod_desc;
struct enhance_item;            /* enhance_menu.h */

struct xbox_mod {
    const char *name;           /* the log lines' [MOD name]; the mod's id */
    /* 0 to run; anything else leaves the mod out. May be NULL. */
    int (*init)(const struct xbox_mod_api *api);
    void (*shutdown)(void);     /* may be NULL */
    /* v3: what the mod is and what it reads, as const data the toolkit
     * reads before any init (below). NULL keeps the v2 behaviour. Appended:
     * this is a source contract for mods linked into the game, not a plugin
     * ABI (struct xbox_mod has no size field). */
    const struct xbox_mod_desc *desc;
};

/* ---- the descriptor (v3; docs/runtime/config.md, "Mods") ----------------
 *
 * Static data: the Mods page and the descriptor dump (RECOMP_MODS_DESCRIBE)
 * describe a mod that is off or never inited from it, without running its
 * code. Validated once at xbox_mod_init: a duplicate setting name or
 * control id, lo > hi, an enum with no choices, an unknown kind or a second
 * XBOX_SET_ENABLE fails the mod (XBOX_MOD_FAILED, one [MOD] line). Text is
 * UTF-8. The toolkit reads only the fields inside `size`, so fields can be
 * appended. */
#define XBOX_MOD_DESC_VERSION 1u

enum {
    XBOX_SET_BOOL = 0,
    XBOX_SET_INT,
    XBOX_SET_FLOAT,
    XBOX_SET_ENUM
};
#define XBOX_SET_ENABLE 0x1u    /* flags: this setting is the mod's switch */

/* A setting the toolkit owns: it resolves the value (env row, then the
 * user files' key then aliases, then config.default.toml's key then
 * aliases, then def), clamps it to lo..hi and snaps it to lo + k * step
 * (each change logged "[MOD] <id>.<name>: <value> -> <used>"), and the mod
 * reads it with api->setting. Bool is 0/1, an enum the choice's index. */
struct xbox_mod_setting {
    const char *name;           /* "speed": key mods."<id>.speed" */
    const char *key;            /* NULL, or the full key ("camera.free_speed") */
    const char *env;            /* NULL, or a recomp_env row name ("RECOMP_...") */
    const char *const *aliases; /* retired full keys, NULL-terminated, or NULL */
    const char *label, *help, *unit;
    uint8_t kind;               /* XBOX_SET_* */
    uint8_t live;               /* enhance_item_live: LIVE, LIVE_ACTIVATE, LIVE_RESTART */
    uint8_t flags;              /* XBOX_SET_ENABLE */
    double def, lo, hi, step;   /* int and float: lo..hi, step 0 for none */
    const char *const *choices; /* enum: NULL-terminated; def is an index */
};

enum {
    XBOX_CTL_ACTION = 0,        /* a host hotkey: action "<id>.<control id>" */
    XBOX_CTL_BUTTON,            /* read every frame while the mod captures */
    XBOX_CTL_STICK,
    XBOX_CTL_INFO               /* shown, never bound ("Mouse: look") */
};
#define XBOX_CTL_SRC_LEFT  0u
#define XBOX_CTL_SRC_RIGHT 1u

struct xbox_mod_control {
    const char *id;             /* "toggle": action "<mod>.toggle" */
    const char *label, *help, *when;   /* when: "while flying", or NULL */
    uint8_t kind;               /* XBOX_CTL_* */
    const char *const *defaults;       /* binding strings, NULL-terminated */
    const char *const *aliases;        /* retired full keys, NULL-terminated */
    const char *env;            /* NULL, or a recomp_env row: its value
                                   ("F6,guide+y") replaces the bindings */
};

/* control_value's answer for a captured control. */
struct xbox_ctl_value {
    int16_t raw[2];             /* STICK: the axes as the pad reports them;
                                   BUTTON: raw[0] 0..255, the largest binding */
    uint8_t src;                /* STICK: XBOX_CTL_SRC_LEFT or _RIGHT */
    uint8_t connected;          /* 0: port 0 has no pad and no keyboard source */
};

struct xbox_mod_desc {
    uint32_t version;           /* XBOX_MOD_DESC_VERSION */
    uint32_t size;              /* sizeof(struct xbox_mod_desc) */
    const char *title, *summary, *author, *version_text;
    const struct xbox_mod_setting *settings;
    uint32_t n_settings;
    const struct xbox_mod_control *controls;
    uint32_t n_controls;
};

typedef void (*xbox_mod_frame_fn)(void *user);
/* Non-zero skips the batch (nv2a_backend_common.h, "Batch filter"). */
typedef int (*xbox_mod_batch_fn)(const struct nv2a_batch_info *b, void *user);

struct xbox_mod_api {
    uint32_t version;           /* XBOX_MOD_API_VERSION */
    uint32_t size;              /* sizeof(struct xbox_mod_api): fields are only added */

    /* The guest's memory: guest address va is host address mem + va. Set
     * before the first on_frame or filter call; 0 during init. */
    uintptr_t mem;

    /* The layer's config getters (enhance_cfg.h): env over the files over
     * the default. A mod's keys are flat names under [mods]
     * ("<name>.speed" = 2, beside its switch "<name>" = true). A key no
     * lookup reads is reported as a typo, except under a v2 mod switched
     * off by its key, whose code never ran to read them. A descriptor
     * mod's settings come through `setting`. */
    const char *(*cfg_string)(const char *key, const char *def);
    long long (*cfg_int)(const char *key, long long def);
    double (*cfg_float)(const char *key, double def);
    int (*cfg_bool)(const char *key, int def);
    int (*cfg_choice)(const char *key, const char *const *choices, int def);
    int (*cfg_bind_env)(const char *key, recomp_env_id id);

    /* One stderr line, "[MOD <m->name>] ..."; the newline is added. */
    void (*log)(const struct xbox_mod *m, const char *fmt, ...);

    /* Init only. fn(user) runs once per guest frame from xbox_mod_frame, on
     * the guest's thread, mods in table order and each mod's in the order
     * it asked. Returns 0 when the table is full (XBOX_MOD_MAX_FRAME). */
    int (*on_frame)(xbox_mod_frame_fn fn, void *user);

    /* Port 0 and the mouse as enhance_input_state gives them: the title's
     * last pad read (a mod never polls: a poll moves the keyboard's stick
     * ramp) and, while captured, the mouse's raw motion. Any thread. */
    void (*input_state)(struct enhance_input_state *out);
    /* Takes port 0 from the title (1) or gives it back (0); see
     * enhance_input_capture. The last call wins across mods. */
    void (*input_capture)(int on);

    /* Init only. fn(b, user) is asked before each GPU draw, on the render
     * (ack) thread; any filter's non-zero skips the batch. Returns 0 when
     * the table is full (XBOX_MOD_MAX_FILTER). */
    int (*batch_filter)(xbox_mod_batch_fn fn, void *user);

    /* ---- version 2 (check size before using these) ---------------------
     * Host actions (enhance_actions.h): a named thing the mod does, bound
     * by [input.actions] "<name>" (or its defaults) to keys, host-only
     * buttons and pad chords that never take an input the title uses. */

    /* Init only. name "<mod>.<verb>" in lowercase, label for the menu,
     * defaults a NULL-terminated list ("F6", "guide+y") or NULL. Returns
     * the id, or -1 with a line (duplicate, bad name, full). */
    int (*action_register)(const char *name, const char *label,
                           const char *const *defaults);
    /* Fires since the last call (any thread); held now past its hold. A
     * binding inside a longer one on its port (guide in guide+a, whoever
     * owns that) fires at its release instead and never reads as down, so
     * a hold-to-act action needs a binding no other one contains. */
    int (*action_pressed)(int id);
    int (*action_down)(int id);
    /* Init only. An item (enhance_menu.h) on the mod's own sub-page of the
     * menu's Mods page: entering the mod's row lists them. The item is kept,
     * not copied, so it must outlive the process (a static). Its key
     * persists through Save like the toolkit's. Returns 0 when the table is
     * full (XBOX_MOD_MAX_ITEMS) or the item has no key or label. */
    int (*menu_add)(const struct enhance_item *it);
    /* Init only. fn(on, user) when the Mods page turns the mod off (0) or
     * on (1). The toolkit records the request and calls fn from the next
     * xbox_mod_frame, on the guest's thread, so the mod changes guest
     * memory and its own state from the thread its on_frame uses. Only
     * the last toggle before a frame reaches fn, so fn may be told the
     * state the mod is already in: it must be idempotent. Off stops
     * nothing by itself: on_frame, the filters and armed actions keep
     * being called, and the mod gates them while it is off. A mod that
     * gives none shows "applies at restart" on its row. Returns 0 when fn
     * is NULL or outside init. */
    int (*on_enable)(void (*fn)(int on, void *user), void *user);
    /* A short message on screen; a "[MOD <name>] toast:" line until the
     * overlay lands (phase 3B). */
    void (*toast)(const struct xbox_mod *m, const char *fmt, ...);
    /* Arm the actions hook when the config key (the mod's enable key,
     * "camera.free") is true; returns whether the hook is armed. Nothing
     * fires and nothing is filtered unless some feature key arms it.
     * From init only: a mod left out takes its key with it. */
    int (*actions_arm)(const char *key);

    /* ---- version 3 (check size before using these) ---------------------
     * The descriptor (struct xbox_mod_desc above); desc is the mod's own. */

    /* Setting idx's value now: a relaxed atomic load, any thread. Bool
     * 0/1, an enum the choice's index. 0 for a bad desc or idx. The
     * values it reads stay valid until process exit: shutdown frees
     * nothing, and only a re-init (the tests) frees them, so a callback
     * still in flight at shutdown reads safely. */
    double (*setting)(const struct xbox_mod_desc *desc, uint32_t idx);
    /* Init only. fn(idx, value, user) after the menu changes a setting,
     * from the next xbox_mod_frame on the guest's thread (as on_enable).
     * A RESTART setting only records the value and never calls back. */
    int (*on_setting)(const struct xbox_mod_desc *desc,
                      void (*fn)(uint32_t idx, double value, void *user), void *user);
    /* The action id of ACTION control idx (registered by the toolkit
     * before init as "<id>.<control id>"), for action_pressed and
     * action_down; -1 for another kind or a bad idx. */
    int (*control_action)(const struct xbox_mod_desc *desc, uint32_t idx);
    /* A captured control's value: fills *out, returns 1 when it reads a
     * source. Until the recorded pad state lands (mod-settings phase D)
     * it reports not connected: out zeroed, 0. */
    int (*control_value)(const struct xbox_mod_desc *desc, uint32_t idx,
                         struct xbox_ctl_value *out);
};

#define XBOX_MOD_MAX_FRAME   16
#define XBOX_MOD_MAX_FILTER  8
#define XBOX_MOD_MAX_ITEMS   32

/* Guest memory through the API: XBOX_MOD_MEM32(api, 0x0035C2A0) = 1. The
 * guest is little-endian and so is every host the toolkit runs on. */
#define XBOX_MOD_MEM32(api, va) (*(volatile uint32_t *)((api)->mem + (uint32_t)(va)))
#define XBOX_MOD_MEMF(api, va)  (*(volatile float *)((api)->mem + (uint32_t)(va)))

/* The title's table, NULL-terminated and kept (not copied): before
 * xbox_enhance_init. A second call replaces the first; NULL clears it. */
void xbox_mod_register_table(const struct xbox_mod *const *mods);

/* Called by xbox_enhance_init. Runs the inits; returns the mods running. */
int xbox_mod_init(void);

/* The title's glue, once per guest frame on the guest's thread. */
void xbox_mod_frame(void);

/* The title's pad glue: whether a mod has port 0 (enhance_input_captured). */
int xbox_mod_input_captured(void);

/* The title's pad glue, after its own merge (keyboard, script): the host
 * pad behind port (host_pad.h; connected = 0 for a script or no pad) and
 * the state the title is about to use, from which a consumed chord's
 * buttons are taken out (enhance_actions.h). Returns at its first test
 * while the hook is disarmed. A title on xbox_InputGetState gets this from
 * input_core.c and does not call it. */
void xbox_enhance_pad_filter(DWORD port, const struct xbox_host_pad *host,
                             XBOX_INPUT_STATE *st);

/* ---- the menu's Mods page ---------------------------------------------
 *
 * The toolkit keeps the table, each init's result and the enable state.
 * `mods.<name>` (bool, default true) is the persisted key: a mod set false
 * is not inited at start. Index i is the table's. */
enum xbox_mod_state {
    XBOX_MOD_ON = 0,
    XBOX_MOD_OFF,           /* off now: by its key at start, or live */
    XBOX_MOD_FAILED,        /* init returned non-zero */
    XBOX_MOD_RESTART        /* the change applies at the next start */
};
int xbox_mod_count(void);                   /* entries in the table */
const char *xbox_mod_name(int i);
enum xbox_mod_state xbox_mod_state(int i);
const char *xbox_mod_state_name(enum xbox_mod_state s);   /* "on", ... */
int xbox_mod_has_on_enable(int i);
int xbox_mod_enabled(int i);                /* the key's value as set now */
/* The Mods page's toggle (any thread): through on_enable at the next
 * xbox_mod_frame when the mod gave one and runs, else recorded for the
 * next start. Returns the new state. */
enum xbox_mod_state xbox_mod_set_enabled(int i, int on);
/* The items mod i added with menu_add, in order; returns the count. */
int xbox_mod_items(int i, const struct enhance_item **out, int max);

/* ---- descriptor mods (the Mods page detail view, the dump) -------------
 *
 * xbox_mod_desc_of: mod i's descriptor, NULL for a v2 mod or a refused one
 * (xbox_mod_fail_reason says why). Settings are by index into it. */
const struct xbox_mod_desc *xbox_mod_desc_of(int i);
const char *xbox_mod_fail_reason(int i);    /* "" unless failed */
double xbox_mod_setting_value(int i, uint32_t idx);
/* The precedence row the value came from: "env", "user", "user_alias",
 * "defaults", "defaults_alias", "default" (or "menu" once set live). */
const char *xbox_mod_setting_source(int i, uint32_t idx);
int xbox_mod_setting_forced(int i, uint32_t idx);   /* env-set: read-only */
const char *xbox_mod_setting_key(int i, uint32_t idx);   /* the full key */
/* The menu's change (any thread): clamped and snapped as at load, stored,
 * and the mod's on_setting queued for the next xbox_mod_frame (a RESTART
 * setting only records it). The enable setting switches the mod as
 * xbox_mod_set_enabled does. Returns the value stored; refused (the old
 * value) when env forces it. */
double xbox_mod_setting_set(int i, uint32_t idx, double v);
/* What the init rule did this run, as the dump spells it: "yes",
 * "dormant", "no" or "failed". */
const char *xbox_mod_init_outcome(int i);
/* The enable setting's index, or -1 (none, or no descriptor). */
int xbox_mod_enable_setting(int i);
/* The action id of mod i's control idx, -1 for none. */
int xbox_mod_control_action(int i, uint32_t idx);

/* 1 when mod i has a descriptor, valid or refused (a refused one fails). */
int xbox_mod_has_desc(int i);
/* The env row bound to setting idx ("RECOMP_..."), or NULL. */
const char *xbox_mod_setting_env(int i, uint32_t idx);
/* Control idx's bindings as this run resolved them, into out (up to max),
 * and *src the precedence row's name ("default", "env", "user", ...): an
 * ACTION's are its action's, a BUTTON's or STICK's come from
 * [input.controls] "<id>.<control id>" (D3). Returns the count. */
int xbox_mod_control_bindings(int i, uint32_t idx, char out[][ENHANCE_ACTION_TEXT], int max,
                              const char **src);

/* User-file aliases waiting to be rewritten under the current key (D4):
 * the menu's Save writes key = value and removes old. Returns the count;
 * fills up to max of each (pointers into the toolkit's own storage, valid
 * until the next xbox_mod_init). */
int xbox_mod_pending_rewrites(const char **keys, const char **values,
                              const char **olds, int max);

/* The dump's xbox_mod_init: resolve every descriptor, register and
 * dormant-mark the actions and compute the init rule, but run no init
 * (RECOMP_MODS_DESCRIBE). */
int xbox_mod_init_describe(void);

/* ---- the descriptor dump (mod_describe.c; D9) ---------------------------
 *
 * RECOMP_MODS_DESCRIBE=json asks xbox_enhance_init for the dump instead of
 * the start: xbox_mod_init_describe, the bindings built, the JSON written
 * to RECOMP_MODS_DESCRIBE_OUT (replaced) or stdout, and exit with the
 * write's result. */
/* 1 for "json"; any other set value logs one [MOD] line and gives 0. */
int xbox_mod_describe_wanted(void);
/* Schema 1 of every mod in the table, as xbox_mod_init_describe left them,
 * to path (NULL or "" for stdout). 0, or 1 with one [MOD] line when the file
 * cannot be written. */
int xbox_mod_describe_write(const char *path);
/* A number as the config files and the dump spell it: the C locale's '.',
 * the shortest text that reads back to v. Returns the length. */
int xbox_mod_num_text(char *out, size_t n, double v);

/* Removes the filter, stops the frame dispatch, then the mods' shutdowns,
 * in reverse order, and the capture released. Runs once; the exit paths
 * call it. A later xbox_mod_init starts over (tests). */
void xbox_mod_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_ENHANCE_MOD_H */
