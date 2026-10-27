/*
 * enhance_actions.h - host actions: named things the host does (open the
 * menu, toggle the stats strip, a mod's free camera) bound to keys, host-only
 * pad buttons and pad chords, without taking an input the title uses
 * (opt-in, XBOXRECOMP_ENHANCE=ON; docs/runtime/config.md, "[input.actions]").
 *
 * The toolkit and the mods register actions; [input.actions] binds them
 * (the highest file that names an action decides all of its bindings, as
 * [input.bindings] does for a control), else the action's defaults apply.
 * Bindings are read from the host side only: the pad filter
 * (xbox_enhance_pad_filter, mod.h) sees the host pad behind each poll, and
 * the flip tick reads it itself when the title has stopped polling (a
 * loading screen, a title that polls only in play). A pad the title gets
 * from a script never fires anything.
 *
 * Nothing runs unless a feature key arms the hook (menu.enabled,
 * overlay.stats, or a mod's own enable key through its API): the startup
 * line is "[ENHANCE] actions hook=off" or "[ENHANCE] actions hook=on
 * (<keys>)", and a golden run has hook=off.
 */
#ifndef ENHANCE_ACTIONS_H
#define ENHANCE_ACTIONS_H

#include <stddef.h>
#include <stdint.h>

#include "host_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENHANCE_ACTIONS_MAX   64
#define ENHANCE_ACTION_BINDS  8     /* bindings per action */
#define ENHANCE_ACTION_TEXT   48    /* a binding's text, with its NUL */

/* Register an action. name is "<owner>.<verb>" in lowercase ASCII,
 * label what a menu shows, defaults a NULL-terminated list of bindings (or
 * NULL). Returns its id, or -1 (a duplicate name, a bad name, a full
 * table), with a line. Copies everything. After enhance_actions_start the
 * bindings are rebuilt at once. */
int enhance_action_register(const char *name, const char *label,
                            const char *const *defaults);
/* The same with an owner: the mod table index, so a mod whose init fails
 * leaves no action behind (enhance_actions_drop_owner). -1 is the toolkit. */
int enhance_action_register_owned(const char *name, const char *label,
                                  const char *const *defaults, int owner);
void enhance_actions_drop_owner(int owner);
/* Drop every mod's actions (xbox_mod_init starting over). */
void enhance_actions_drop_mods(void);

/* How many times the action fired since the last call (0 for a bad id).
 * Any thread; each fire is counted once. */
int enhance_action_pressed(int id);
/* Whether one of the action's bindings is held now, past its hold. A
 * binding inside a longer one fires at its release and is never down. */
int enhance_action_down(int id);
/* The id of a name, -1 when none; the name of an id, NULL. */
int enhance_action_find(const char *name);
const char *enhance_action_name(int id);

/* xbox_enhance_init: the toolkit's actions (menu.open, overlay.stats,
 * shot.window) into a cleared registry, before the mods' init. */
void enhance_actions_init(void);
/* After the mods' init: reads [input.actions] and input.chord_delay_ms,
 * builds the bindings, arms from the feature keys and logs the hook line. */
void enhance_actions_start(void);
/* Re-read [input.actions] and swap the bindings in (the menu, a reload).
 * Any thread; a poll in flight finishes on the old table. A held input
 * must be released before a new binding fires on it. */
void enhance_actions_rebuild(void);

/* Arm the hook when the config key is true: 1 when armed after the call.
 * A mod passes its enable key ("camera.free"). Before start the key is
 * kept for the hook line; after, a newly armed hook logs the line again. */
int enhance_actions_arm(const char *key);
/* The same for a mod's init (owner: its mod table index): the key goes
 * when the mod is dropped (enhance_actions_drop_owner). */
int enhance_actions_arm_owned(const char *key, int owner);
int enhance_actions_armed(void);
/* 1 when key is one the hook is armed by (its config value was true):
 * enhance.c reads overlay.stats through this, so the key has one reader. */
int enhance_actions_keyed(const char *key);

/* Report these guest bits (wButtons bits 0..7, analog index i as bit
 * 8 + i) released on port until each is physically released: the button
 * that closed the menu must not reach the title as a press. */
void enhance_actions_mask_until_release(unsigned port, uint32_t bits);

/* From the flip (nv2a_flip_hook_set): when a port has not been polled for
 * 100 ms, read its host pad and step the keys, host-only buttons and
 * modifier chords. Returns at once when disarmed. */
void enhance_actions_tick(void);

/* The pad filter's work (xbox_enhance_pad_filter calls it when armed). */
void enhance_actions_filter(unsigned port, const struct xbox_host_pad *host,
                            XBOX_INPUT_STATE *st);

/* The in-game menu (enhance_menu.c). */
int enhance_action_count(void);             /* ids are 0..count-1; dropped ones name NULL */
const char *enhance_action_label(int id);
/* The bindings in effect: the menu's (set_bindings), else the highest file's
 * [input.actions] entry, else the defaults. Returns how many. */
int enhance_action_bindings(int id, char out[][ENHANCE_ACTION_TEXT], int max);
int enhance_action_default_bindings(int id, char out[][ENHANCE_ACTION_TEXT], int max);
/* Bind the action to these n texts at once, over the files (n 0 unbinds;
 * n -1 drops the menu's list, back to the files and defaults), and
 * rebuild. Each text should have passed enhance_action_check. */
int enhance_action_set_bindings(int id, const char *const *texts, int n);
/* Whether text is a binding the rules accept under the current map: 1 with
 * its kind in why ("host-only", "a guest chord"...), 0 with the reason the
 * log would give. Logs nothing. */
int enhance_action_check(const char *text, char *why, size_t n);

/* The action other than except whose binding in effect takes the same
 * input as text (the same key and modifiers, or the same pad members on
 * the same port, whatever the hold), with that binding's text in bound;
 * -1 when none. One input fires one action: the menu's capture refuses
 * what another action owns, and a rebuild keeps one of two such bindings
 * (menu.open's, else the written one over a default, else the first
 * registered). */
int enhance_action_owner(const char *text, int except, char *bound, size_t n);
/* While on (the menu's capture), nothing fires: an input held meanwhile
 * fires only after its release. Turning it off takes effect one port-0
 * step later for keys and port 0 (at once for other ports), so a key that ended the capture before any step saw it held
 * does not fire either. */
void enhance_actions_quiet(int on);

/* Called after every step (the pad filter's, on the poll thread, with st;
 * the flip tick's for port 0 when the title has stopped polling, st NULL),
 * outside the step lock, so it may call enhance_actions_mask_until_release.
 * held: the host sources down, bit per input_map.h source. While a hook is
 * set, a menu.open fire is a request (enhance_actions_menu_request takes
 * it) instead of the "Menu is off" toast. */
typedef void (*enhance_menu_hook_fn)(unsigned port, uint32_t held,
                                     XBOX_INPUT_STATE *st, uint64_t now_ms);
void enhance_actions_set_menu_hook(enhance_menu_hook_fn fn);
int enhance_actions_menu_request(void);

/* Disarm, clear the registry, the bindings and the per-port state (tests). */
void enhance_actions_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_ACTIONS_H */
