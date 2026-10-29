/*
 * nv2a_present_chain.h - present.shader: a RetroArch slang preset run by
 * librashader in place of the final present draw (Metal and D3D11).
 *
 * The library is loaded at run time, never linked: librashader's D3D11
 * runtime does not build with MinGW, so the Windows exe calls the upstream
 * MSVC dll through plain C and COM pointers, and the macOS dylib goes the
 * same way so there is one code path. A missing or mismatched library turns
 * the feature off with one log line; the present stays stock.
 *
 * This module owns the request (the key, or the menu's live choice), the
 * library and the preset path. Each backend owns its filter chain: it
 * compares nv2a_present_chain_gen() with the generation its chain was built
 * for on every present, and rebuilds when it moved. Nothing here loads while
 * the request is "none", so the stock path costs one load of an int.
 */
#ifndef NV2A_PRESENT_CHAIN_H
#define NV2A_PRESENT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The presets the toolkit bundles (assets/shaders), in menu order after
 * "none". A request may also be a path to any .slangp. One list: the menu's
 * choices are built from the macro, so the two cannot drift. */
#define NV2A_PRESENT_CHAIN_NAMES "crt-lottes", "crt-lottes-fast", "crt-hyllian", "fsr"
extern const char *const nv2a_present_chain_names[];

/* The request: "none" (or NULL, or empty) for the stock present, a bundled
 * name, or a .slangp path (absolute, or relative to the executable's
 * directory). Moves the generation; any thread. */
void nv2a_present_chain_request(const char *shader);

/* The library path override (present.shader_lib); NULL or empty: the
 * default beside the executable. Before the first present. */
void nv2a_present_chain_set_lib(const char *path);

/* The current request, copied into out ("none" when off). */
void nv2a_present_chain_requested(char *out, size_t cap);

/* Moves on every request and on nv2a_present_chain_failed(); 0 before the
 * first request. */
unsigned nv2a_present_chain_gen(void);

/* 1 when a shader is requested and has not failed for this generation:
 * the backend should run (or build) its chain. One atomic load. */
int nv2a_present_chain_wanted(void);

/* The library, loaded on first use: NULL when it is missing or its ABI
 * does not match (said once per path). */
struct nv2a_lrs {
    void *(*sym)(const char *name);   /* any librashader export, or NULL */
    unsigned abi, api;
};
const struct nv2a_lrs *nv2a_present_chain_lib(void);

/* The presenter that runs chains (Metal's, D3D11's) says so once it is up.
 * One that has none never does: the SDL renderer, or Windows' GDI window
 * under the CPU rasteriser. name: for the log ("Metal", "D3D11"). */
void nv2a_present_chain_host(const char *name);

/* 1 when a shader can run here: a chain presenter is up and the library
 * loads (loaded now if it was not tried yet). 0 with the reason in why. It
 * does not look at the request: "none" is still supported. Any thread. */
int nv2a_present_chain_supported(char *why, size_t cap);

/* The preset file for the current request into out. Returns 1 found,
 * 0 for "none", -1 not found (said, with what was tried). gen_out gets the
 * generation the path belongs to. */
int nv2a_present_chain_preset(char *out, size_t cap, unsigned *gen_out);

/* The backend could not build the chain for generation gen (said by the
 * backend): wanted() is 0 until the next request, so a broken preset is
 * not retried every frame. */
void nv2a_present_chain_failed(unsigned gen);

/* The rate the chain's frames arrive at, for the presets' frame-rate
 * uniform (librashader's frames_per_second): a moving average of the
 * intervals between calls, so a game that presents at 30 says 30, not the
 * display's 60. now_ns: a monotonic time of this present
 * (xbox_HostNowNs). 60 until two presents; a pause over a second starts
 * over. The backend's present thread only. */
float nv2a_present_chain_rate(uint64_t now_ns);

/* librashader's message for an error object into out, and frees the error.
 * err is a libra_error_t. */
void nv2a_present_chain_error(void *err, char *out, size_t cap);

/* ---- Runtime parameters (present.shader_params) ----
 *
 * A preset declares parameters (#pragma parameter: name, label, range,
 * step). The backend that built a chain publishes the preset's table here;
 * the menu and the config set per-preset overrides; the backend follows
 * nv2a_present_chain_params_gen() with the chain's set_param. Overrides are
 * keyed by preset (the bundled name, or a path's file stem) so each preset
 * keeps its own. A value equal to the preset's own removes the override,
 * so the config only ever holds differences. */
#define NV2A_PRESENT_PARAMS_MAX 64
#define NV2A_PRESENT_OVERRIDES_MAX 256   /* over every preset */

struct nv2a_present_param {
    char name[64];
    char desc[96];
    float initial, min, max, step;   /* initial: the preset's value */
};

/* The preset key of a request ("crt-lottes", or a path's sanitised stem)
 * into out; "" for "none". */
void nv2a_present_chain_preset_key(const char *request, char *out, size_t cap);

/* Read a parsed preset's parameters (preset: a libra_shader_preset_t *)
 * into out. Returns the count (0 when the library lacks the call). */
int nv2a_present_chain_params_read(void *preset, struct nv2a_present_param *out, int cap);

/* The backend built its chain for generation gen from preset (a
 * libra_shader_preset_t *): read and publish its parameters as the active
 * table. Ignored when a newer request has arrived. */
void nv2a_present_chain_params_publish(void *preset, unsigned gen);

/* The chain was released or failed: no active table. */
void nv2a_present_chain_params_clear(void);

/* Moves on every publish, clear, set and reset. */
unsigned nv2a_present_chain_params_gen(void);

/* A copy of the active table into out (cap rows) and its preset key.
 * Returns the row count; 0 with key "" when no chain is up. */
int nv2a_present_chain_params(struct nv2a_present_param *out, int cap, char *key, size_t keycap);

/* The active preset's current value of name (override or initial); the
 * value is snapped to the row's range. Returns 0 when name is not active. */
int nv2a_present_chain_param_get(const char *name, float *out);

/* Set name on the active preset (clamped to its range); equal to the
 * preset's value removes the override. Returns 0 when not active. */
int nv2a_present_chain_param_set(const char *name, float v);

/* Remove the overrides of preset key (NULL: of every preset). */
void nv2a_present_chain_params_reset(const char *key);

/* The backend applies the parameters: when the params generation moved
 * past *applied, set(ctx, name, value) for every active row (override or
 * initial), then *applied takes the generation. The backend's present
 * thread, with its chain built. */
typedef void (*nv2a_present_param_set_fn)(void *ctx, const char *name, float v);
void nv2a_present_chain_params_apply(unsigned *applied, nv2a_present_param_set_fn set, void *ctx);

/* Config: an override read at start (any layer; a later call wins). It
 * counts as saved. */
void nv2a_present_chain_override_load(const char *key, const char *name, float v);

/* Overrides for Save: row i (from 0) while it returns 1. has: the
 * override is set (else it was removed since the last save); dirty: it
 * differs from what was loaded or saved last. */
int nv2a_present_chain_override_at(int i, char *key, size_t keycap, char *name,
                                   size_t namecap, float *v, int *has, int *dirty);

/* Save wrote the overrides: they are clean now. */
void nv2a_present_chain_overrides_saved(void);

/* The overrides that differ from what was loaded or saved. */
int nv2a_present_chain_overrides_dirty(void);

/* The override of name for preset key: 1 with *v set, 0 when none. */
int nv2a_present_chain_override_get(const char *key, const char *name, float *v);

/* Tests: drop the active table and every override, as a fresh start. */
void nv2a_present_chain_params_reset_for_test(void);

/* Forget the presenter and the library (closing it), so a test can load
 * another one or none. */
void nv2a_present_chain_lib_reset_for_test(void);

/* The descriptor dump: the parameters of request (a bundled name or a
 * path) parsed from its preset, with no GPU and no chain. The count, or -1
 * when the library or the preset is missing or does not parse. */
int nv2a_present_chain_describe(const char *request, struct nv2a_present_param *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PRESENT_CHAIN_H */
