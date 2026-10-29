/*
 * nv2a_present_chain.c - see nv2a_present_chain.h.
 */
#include "nv2a_present_chain.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "librashader.h"
#include "platform/recomp_exe_dir.h"

#define LOGP "[SHADER] "

const char *const nv2a_present_chain_names[] = { NV2A_PRESENT_CHAIN_NAMES, NULL };

/* The request string is short and changes only from the config at start
 * and the menu; a spin lock keeps a reader from copying half a write. */
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static char s_req[512] = "none";
static char s_lib_path[512];
static atomic_uint s_gen;
static atomic_int s_wanted;

static struct nv2a_lrs s_lrs;
static int s_lib_state;             /* 0 not tried, 1 loaded, -1 failed */
#ifdef _WIN32
static HMODULE s_mod;
#else
static void *s_mod;
#endif

static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire))
        ;
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
}

static int is_none(const char *s)
{
    return !s || !*s || !strcmp(s, "none");
}

static int path_is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\'
        || (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

void nv2a_present_chain_request(const char *shader)
{
    lock();
    snprintf(s_req, sizeof s_req, "%s", is_none(shader) ? "none" : shader);
    atomic_store(&s_wanted, !is_none(shader));
    atomic_fetch_add(&s_gen, 1);
    unlock();
}

void nv2a_present_chain_set_lib(const char *path)
{
    lock();
    snprintf(s_lib_path, sizeof s_lib_path, "%s", path ? path : "");
    unlock();
}

void nv2a_present_chain_requested(char *out, size_t cap)
{
    lock();
    snprintf(out, cap, "%s", s_req);
    unlock();
}

unsigned nv2a_present_chain_gen(void)
{
    return atomic_load(&s_gen);
}

int nv2a_present_chain_wanted(void)
{
    return atomic_load_explicit(&s_wanted, memory_order_relaxed);
}

float nv2a_present_chain_rate(uint64_t now_ns)
{
    static uint64_t last;
    static double avg_ns;           /* 0: no interval yet */
    double d;

    if (!last || now_ns <= last || now_ns - last > 1000000000ull) {
        last = now_ns;
        avg_ns = 0;
        return 60.0f;
    }
    d = (double)(now_ns - last);
    last = now_ns;
    /* An eighth per frame: steady within a few frames, deaf to one late
     * present. */
    avg_ns = avg_ns > 0 ? avg_ns + (d - avg_ns) / 8.0 : d;
    return (float)(1e9 / avg_ns);
}

void nv2a_present_chain_failed(unsigned gen)
{
    lock();
    /* Only the generation that failed: a request that arrived meanwhile
     * gets its own try. */
    if (gen == atomic_load(&s_gen)) {
        atomic_store(&s_wanted, 0);
    }
    unlock();
}

static void *lrs_sym(const char *name)
{
    if (!s_mod)
        return NULL;
#ifdef _WIN32
    return (void *)GetProcAddress(s_mod, name);
#else
    return dlsym(s_mod, name);
#endif
}

/* Default locations: beside the executable, and in a macOS app bundle's
 * Frameworks directory. */
static int lib_candidates(char paths[][1024], int max)
{
    const char *dir = recomp_exe_dir();
    int n = 0;
    (void)max;                          /* one candidate off macOS */
#if defined(_WIN32)
    snprintf(paths[n++], 1024, "%s/librashader.dll", dir);
#elif defined(__APPLE__)
    snprintf(paths[n++], 1024, "%s/librashader.dylib", dir);
    if (n < max)
        snprintf(paths[n++], 1024, "%s/../Frameworks/librashader.dylib", dir);
#else
    snprintf(paths[n++], 1024, "%s/librashader.so", dir);
#endif
    return n;
}

static int open_lib(const char *path, char *why, size_t cap)
{
#ifdef _WIN32
    wchar_t w[1024];
    DWORD err;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) {
        snprintf(why, cap, "bad path");
        return 0;
    }
    /* The dll's own directory first for its imports (d3dcompiler_47 can
     * sit beside it). */
    s_mod = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (s_mod)
        return 1;
    err = GetLastError();
    if (!file_exists(path)) {
        snprintf(why, cap, "not found");
    } else {
        /* The file is there, so an import is missing. The upstream dll is
         * built with MSVC and needs the VC++ runtime, which a clean
         * Windows may lack; the game does not ship it. */
        HMODULE vc = LoadLibraryA("vcruntime140.dll");
        HMODULE vc1 = LoadLibraryA("vcruntime140_1.dll");
        if (!vc || !vc1)
            snprintf(why, cap, "it needs the Microsoft Visual C++ Redistributable"
                     " (x64), which is not installed: get it from"
                     " https://aka.ms/vs/17/release/vc_redist.x64.exe (error %lu)",
                     (unsigned long)err);
        else
            snprintf(why, cap, "LoadLibrary error %lu", (unsigned long)err);
        if (vc)
            FreeLibrary(vc);
        if (vc1)
            FreeLibrary(vc1);
    }
    return 0;
#else
    const char *e;
    s_mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (s_mod)
        return 1;
    e = dlerror();
    snprintf(why, cap, "%s", file_exists(path) ? (e ? e : "dlopen failed") : "not found");
    return 0;
#endif
}

static void close_lib(void)
{
    if (!s_mod)
        return;
#ifdef _WIN32
    FreeLibrary(s_mod);
#else
    dlclose(s_mod);
#endif
    s_mod = NULL;
}

const struct nv2a_lrs *nv2a_present_chain_lib(void)
{
    char paths[2][1024], why[512] = "", over[512];
    PFN_libra_instance_abi_version abi;
    PFN_libra_instance_api_version api;
    int n, i;

    if (s_lib_state)
        return s_lib_state > 0 ? &s_lrs : NULL;
    s_lib_state = -1;
    lock();
    snprintf(over, sizeof over, "%s", s_lib_path);
    unlock();
    if (over[0]) {
        if (path_is_absolute(over))
            snprintf(paths[0], sizeof paths[0], "%s", over);
        else
            snprintf(paths[0], sizeof paths[0], "%s/%s", recomp_exe_dir(), over);
        n = 1;
    } else {
        n = lib_candidates(paths, 2);
    }
    for (i = 0; i < n; i++)
        if (open_lib(paths[i], why, sizeof why))
            break;
    if (i == n) {
        fprintf(stderr, LOGP "present.shader off: librashader %s (%s)\n", why, paths[n - 1]);
        return NULL;
    }
    abi = (PFN_libra_instance_abi_version)lrs_sym("libra_instance_abi_version");
    api = (PFN_libra_instance_api_version)lrs_sym("libra_instance_api_version");
    if (!abi || !api) {
        fprintf(stderr, LOGP "present.shader off: %s has no librashader version"
                " exports\n", paths[i]);
        close_lib();
        return NULL;
    }
    s_lrs.abi = (unsigned)abi();
    s_lrs.api = (unsigned)api();
    /* ABI changes break the structs we pass; the API only grows, so an
     * older one lacks calls or fields we use. */
    if (s_lrs.abi != LIBRASHADER_CURRENT_ABI || s_lrs.api < LIBRASHADER_CURRENT_VERSION) {
        fprintf(stderr, LOGP "present.shader off: %s is ABI %u API %u; this build needs"
                " ABI %d API %d or later (librashader 0.12)\n", paths[i], s_lrs.abi,
                s_lrs.api, LIBRASHADER_CURRENT_ABI, LIBRASHADER_CURRENT_VERSION);
        close_lib();
        return NULL;
    }
    s_lrs.sym = lrs_sym;
    s_lib_state = 1;
    fprintf(stderr, LOGP "librashader ABI %u API %u: %s\n", s_lrs.abi, s_lrs.api, paths[i]);
    return &s_lrs;
}

static int has_path_chars(const char *s)
{
    size_t n = strlen(s);
    return strchr(s, '/') || strchr(s, '\\') || (n > 7 && !strcmp(s + n - 7, ".slangp"));
}

int nv2a_present_chain_preset(char *out, size_t cap, unsigned *gen_out)
{
    char req[512];
    const char *dir = recomp_exe_dir();
    unsigned gen;

    lock();
    snprintf(req, sizeof req, "%s", s_req);
    gen = atomic_load(&s_gen);
    unlock();
    if (gen_out)
        *gen_out = gen;
    if (is_none(req))
        return 0;
    if (has_path_chars(req)) {
        if (path_is_absolute(req))
            snprintf(out, cap, "%s", req);
        else
            snprintf(out, cap, "%s/%s", dir, req);
        if (file_exists(out))
            return 1;
        fprintf(stderr, LOGP "present.shader=%s: no such file (%s)\n", req, out);
        return -1;
    }
    snprintf(out, cap, "%s/shaders/%s.slangp", dir, req);
    if (file_exists(out))
        return 1;
#ifdef __APPLE__
    snprintf(out, cap, "%s/../Resources/shaders/%s.slangp", dir, req);
    if (file_exists(out))
        return 1;
#endif
    fprintf(stderr, LOGP "present.shader=%s: no shaders/%s.slangp beside the executable"
            " (%s)\n", req, req, dir);
    return -1;
}

void nv2a_present_chain_error(void *err, char *out, size_t cap)
{
    PFN_libra_error_write wr = (PFN_libra_error_write)lrs_sym("libra_error_write");
    PFN_libra_error_free_string fs = (PFN_libra_error_free_string)lrs_sym("libra_error_free_string");
    PFN_libra_error_free fe = (PFN_libra_error_free)lrs_sym("libra_error_free");
    libra_error_t e = (libra_error_t)err;
    char *m = NULL;

    snprintf(out, cap, "unknown error");
    if (!e)
        return;
    if (wr && wr(e, &m) == 0 && m) {
        snprintf(out, cap, "%s", m);
        if (fs)
            fs(&m);
    }
    if (fe)
        fe(&e);
}
