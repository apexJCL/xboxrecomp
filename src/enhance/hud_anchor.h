/*
 * hud_anchor.h - the HUD layer: a game's HUD elements moved to the edges of
 * a wide display.aspect, in guest memory, where the game hands over their
 * vertices (openspec change hud-anchors).
 *
 * A mod registers the game's HUD table (TOML text it embeds, the schema in
 * docs/runtime/config.md) through api->hud_table: the canvas the game's
 * HUD coordinates live in, the vertex layout, the wrapper functions whose
 * callers name an element, the groups with their kind, and one row per
 * call site. From the pre callback of its hook on the call that takes the
 * vertices it calls api->hud_apply, which finds the element by return
 * address and shifts, clips or stretches the vertices for this frame; with
 * XBOX_HUD_RESTORE they are put back by api->hud_restore at that hook's
 * post, so a buffer the game keeps never accumulates a shift.
 *
 * The layer is game-agnostic: every VA, size and kind comes from the table.
 * It is active only while its owner mod is on, a table is registered and
 * the canvas's target has a Hor+ pad (display.aspect wider than 4:3). While
 * it is not, every entry point returns at its first test and the backends
 * see no lift (nv2a_aspect_hud_set), so stock and 4:3 are unchanged.
 *
 * Threads: hud_layer_table on the init thread; hud_layer_frame on the
 * guest's frame thread; the wrappers, apply and restore on any guest thread
 * (the call chain and the saved vertices are per thread).
 */
#ifndef HUD_ANCHOR_H
#define HUD_ANCHOR_H

#include <stddef.h>
#include <stdint.h>

#include "mod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vertices saved for XBOX_HUD_RESTORE; a call with more is left alone. */
#define HUD_RESTORE_MAX 1024u
/* RESTORE calls open at once per thread (nested); a deeper one is
 * skipped. */
#define HUD_SAVE_MAX 3u
/* The call chain's depth per thread; a deeper push is dropped. */
#define HUD_CHAIN_MAX 8u

/* The hook service the table's wrappers are hooked through (mod.c passes
 * its api->hook; a test, its own). */
typedef int (*hud_hook_fn)(uint32_t va, xbox_mod_hook_fn pre, xbox_mod_hook_fn post,
                           uint32_t flags, void *user);

/* Parse and register a table. Each [[wrapper]] is hooked through `hook`.
 * The number of sites, or < 0 with one "[hud] ..." line and nothing kept:
 * a parse error, an unknown version, table, key, group or kind, a
 * duplicate ret_va, a bad canvas or layout, a wrapper with no hook slot,
 * or a table already registered. */
int hud_layer_table(const char *toml, size_t len, hud_hook_fn hook);

/* Where guest memory is mapped: a pointer to a base that may still be 0
 * (mod.c's api.mem, which it fills on the first frame or hook). */
void hud_layer_set_mem(const uintptr_t *base);

/* The keys (enhance.c, at start): display.hud_aspect as aw:ah (0:0 =
 * display, the frame's pad) and display.hud_margin. Any thread. */
void hud_layer_set_keys(unsigned aw, unsigned ah, int margin);
/* display.hud_margin alone, live (the menu). Clamped to 0..64. */
void hud_layer_set_margin(int margin);
int hud_layer_margin(void);

/* Once per guest frame (xbox_mod_frame), on the guest's thread, outside
 * every wrapper: bumps the call chain's epoch, reads the canvas and the
 * target, and sets the active flag from owner_on (the owner mod on), the
 * table and the pad, telling the backends (nv2a_aspect_hud_set). Returns
 * the flag. */
int hud_layer_frame(int owner_on);

/* api->hud_apply and api->hud_restore (mod.h). */
int hud_layer_apply(const struct xbox_mod_call *c, const struct xbox_hud_call *d);
void hud_layer_restore(const struct xbox_mod_call *c);
/* A post of the owner's hook while the owner is off (mod.c): the wrapper
 * pop and the restore the mod's own post would have done. */
void hud_layer_post_off(const struct xbox_mod_call *c);

int hud_layer_active(void);
int hud_layer_has_table(void);
/* The pad the edge kinds move by now, guest pixels: 0 while inactive. */
int hud_layer_pad_hud(void);
/* The canvas's target pad, guest pixels (0 at 4:3). */
int hud_layer_pad(void);

/* Tests and shutdown: drop the table and every state. The wrappers stay
 * hooked in the hook service, and do nothing until a new table lists
 * them again. */
void hud_layer_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* HUD_ANCHOR_H */
