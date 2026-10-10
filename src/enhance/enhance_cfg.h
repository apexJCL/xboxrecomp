/*
 * enhance_cfg.h - the enhancements file, layered under the environment.
 *
 * Opt-in (XBOXRECOMP_ENHANCE=ON). Reads enhance.toml (recomp_cfg.h) and
 * answers typed lookups by dotted key, in this order:
 *
 *   1. the environment variable bound to the key, when set
 *      (RECOMP_RENDER_SCALE for render.scale; recomp_env.h config tier)
 *   2. <root>/<title>/enhance.toml
 *   3. <root>/enhance.toml
 *   4. the caller's default
 *
 * RECOMP_ENHANCE_CONFIG=<path> replaces 2 and 3 with that one file;
 * RECOMP_ENHANCE_CONFIG=none reads no file. A file that does not parse is
 * reported (with its line) and skipped as a whole, so a typo never half
 * applies. Docs: docs/runtime/enhance-config.md.
 *
 * Call enhance_cfg_init once at startup, then read the keys during module
 * init (one thread) and keep the values: a lookup records that the key was
 * read, for enhance_cfg_report_unused.
 */
#ifndef ENHANCE_CFG_H
#define ENHANCE_CFG_H

#include "recomp_cfg.h"
#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENHANCE_SRC_DEFAULT = 0,
    ENHANCE_SRC_ENV,
    ENHANCE_SRC_FILE
} enhance_cfg_source;

/* Load the files. `root` is the data root, `title` the per-title directory
 * name under it (either may be NULL). Logs one line:
 *   [ENHANCE] config: <path>[, <path>] (N keys)
 * Returns the number of files loaded, or -1 if one failed to parse. A second
 * call reloads (for tests). */
int enhance_cfg_init(const char *root, const char *title);
void enhance_cfg_shutdown(void);

/* Bind a key to an environment variable of recomp_env's table. The toolkit
 * binds its own (render.scale, display.aspect); a game binds its keys
 * (e.g. game.mode -> RENV_GAME_MODE) before reading them. A later bind of the same
 * key replaces the earlier one. Returns 0 when the table is full. */
int enhance_cfg_bind_env(const char *key, recomp_env_id id);

/* Typed lookups, env over file over default. An env value that does not
 * convert to the type, or a file value of another type, is reported once
 * and skipped (the next tier answers). */
const char *enhance_cfg_string(const char *key, const char *def);
long long enhance_cfg_int(const char *key, long long def);
double enhance_cfg_float(const char *key, double def);
int enhance_cfg_bool(const char *key, int def);   /* env: 1/0 true/false yes/no on/off */
int enhance_cfg_choice(const char *key, const char *const *choices, int def);

/* Where `key`'s value comes from, for the init log line of a module. */
enhance_cfg_source enhance_cfg_source_of(const char *key);

/* The file that holds `key` (title file first), or NULL, and the key counts
 * as read. For arrays, which have no env tier:
 *   const recomp_cfg *c = enhance_cfg_lookup("render.modes");
 *   n = recomp_cfg_array_len(c, "render.modes");                       */
const recomp_cfg *enhance_cfg_lookup(const char *key);

/* The parsed files, 0 title and 1 root (NULL when absent), for prefix scans
 * such as aspect.program.<hash>; pass each key used to enhance_cfg_lookup. */
const recomp_cfg *enhance_cfg_file(int i);

/* Log the file keys no lookup has read so far ("unused key render.scael,
 * enhance.toml:3"), so a typo is visible. Call after the modules' init.
 * Returns the count. */
int enhance_cfg_report_unused(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_CFG_H */
