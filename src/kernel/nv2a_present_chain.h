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

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PRESENT_CHAIN_H */
