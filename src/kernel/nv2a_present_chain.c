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
static char s_lib_why[640];         /* why it failed, for the menu */
/* The load runs once: the present thread (a chain to build) and the menu
 * (is the Shaders page shown) may both ask first. Its own flag, since the
 * request lock is held only for copies. */
static atomic_flag s_load_lock = ATOMIC_FLAG_INIT;
static char s_host[16];             /* the chain presenter, "" none */
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

#ifdef _WIN32
/* The first dll that <w>'s import table names and the loader cannot find,
 * into <name>, or 0. Error 126 alone does not say which import is missing;
 * the hint names the package that brings it. Each import is looked for
 * beside <w> first, as LOAD_WITH_ALTERED_SEARCH_PATH does when open_lib
 * loads it, then through the standard search. Only direct imports: a
 * missing dll further down the chain leaves the plain error. */
static int missing_import(const wchar_t *w, char *name, size_t cap)
{
    HMODULE img = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    const unsigned char *base = (const unsigned char *)img;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_IMPORT_DESCRIPTOR *imp;
    wchar_t beside[1024];
    size_t dirlen = 0, i;
    DWORD size;
    int found = 0;

    if (!img)
        return 0;
    for (i = 0; w[i]; i++)
        if (w[i] == L'\\' || w[i] == L'/')
            dirlen = i + 1;
    /* A file that is not a PE32+ image (the loader maps what it can) has
     * no import table to trust: leave the plain error. */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        goto done;
    nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        goto done;
    size = nt->OptionalHeader.SizeOfImage;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        goto done;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || dir->VirtualAddress >= size)
        goto done;
    for (imp = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
         (const unsigned char *)(imp + 1) <= base + size && imp->Name && !found; imp++) {
        const char *dll;
        HMODULE h;
        if (imp->Name >= size || !memchr(base + imp->Name, 0, size - imp->Name))
            break;
        dll = (const char *)(base + imp->Name);
        if (dirlen && dirlen + strlen(dll) < 1024) {
            memcpy(beside, w, dirlen * sizeof *w);
            if (MultiByteToWideChar(CP_ACP, 0, dll, -1, beside + dirlen,
                                    (int)(1024 - dirlen)) &&
                GetFileAttributesW(beside) != INVALID_FILE_ATTRIBUTES)
                continue;
        }
        h = LoadLibraryA(dll);
        if (h) {
            FreeLibrary(h);
        } else {
            snprintf(name, cap, "%s", dll);
            found = 1;
        }
    }
done:
    FreeLibrary(img);
    return found;
}

static const char *import_hint(const char *dll)
{
    if (!_strnicmp(dll, "d3dx9_", 6))
        return ": it comes with the DirectX End-User Runtime,"
               " https://www.microsoft.com/download/details.aspx?id=35";
    if (!_strnicmp(dll, "msvcp140", 8) || !_strnicmp(dll, "vcruntime140", 12))
        return ": it comes with the Microsoft Visual C++ Redistributable (x64),"
               " https://aka.ms/vs/17/release/vc_redist.x64.exe";
    return "";
}
#endif

static int open_lib(const char *path, char *why, size_t cap)
{
#ifdef _WIN32
    wchar_t w[1024];
    char dll[128];
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
    } else if (missing_import(w, dll, sizeof dll)) {
        /* The toolkit's own x86-64 build imports only system dlls; an
         * upstream or user-supplied build may need D3DX9 or the VC++
         * runtime, which a stock Windows lacks. */
        snprintf(why, cap, "needs %s, which is not installed%s (LoadLibrary error %lu)",
                 dll, import_hint(dll), (unsigned long)err);
    } else if (err == ERROR_MOD_NOT_FOUND) {
        snprintf(why, cap, "needs a DLL that is missing (LoadLibrary error %lu)",
                 (unsigned long)err);
    } else {
        snprintf(why, cap, "failed to load (LoadLibrary error %lu)", (unsigned long)err);
    }
    return 0;
#else
    const char *e;
    s_mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (s_mod)
        return 1;
    e = dlerror();
    if (!file_exists(path))
        snprintf(why, cap, "not found");
    else
        snprintf(why, cap, "failed to load: %s", e ? e : "dlopen failed");
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

static const struct nv2a_lrs *lib_load(void);

const struct nv2a_lrs *nv2a_present_chain_lib(void)
{
    const struct nv2a_lrs *l;
    while (atomic_flag_test_and_set_explicit(&s_load_lock, memory_order_acquire))
        ;
    l = s_lib_state ? (s_lib_state > 0 ? &s_lrs : NULL) : lib_load();
    atomic_flag_clear_explicit(&s_load_lock, memory_order_release);
    return l;
}

void nv2a_present_chain_host(const char *name)
{
    lock();
    snprintf(s_host, sizeof s_host, "%s", name && *name ? name : "?");
    unlock();
}

int nv2a_present_chain_supported(char *why, size_t cap)
{
    char host[sizeof s_host];
    lock();
    memcpy(host, s_host, sizeof host);
    unlock();
    if (!host[0]) {
        snprintf(why, cap, "this presenter runs no shader chain (Metal and D3D11 do)");
        return 0;
    }
    if (!nv2a_present_chain_lib()) {
        snprintf(why, cap, "%s", s_lib_why);
        return 0;
    }
    if (cap)
        why[0] = 0;
    return 1;
}

void nv2a_present_chain_lib_reset_for_test(void)
{
    close_lib();
    s_lib_state = 0;
    s_lib_why[0] = 0;
    memset(&s_lrs, 0, sizeof s_lrs);
    lock();
    s_host[0] = 0;
    unlock();
}

static const struct nv2a_lrs *lib_load(void)
{
    char paths[2][1024], why[512] = "", over[512];
    PFN_libra_instance_abi_version abi;
    PFN_libra_instance_api_version api;
    int n, i;

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
        snprintf(s_lib_why, sizeof s_lib_why, "librashader %s", why);
        return NULL;
    }
    abi = (PFN_libra_instance_abi_version)lrs_sym("libra_instance_abi_version");
    api = (PFN_libra_instance_api_version)lrs_sym("libra_instance_api_version");
    if (!abi || !api) {
        fprintf(stderr, LOGP "present.shader off: %s has no librashader version"
                " exports\n", paths[i]);
        snprintf(s_lib_why, sizeof s_lib_why, "librashader has no version exports");
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
        snprintf(s_lib_why, sizeof s_lib_why, "librashader is ABI %u API %u, not 0.12",
                 s_lrs.abi, s_lrs.api);
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

/* The preset file for request req into out: 1 found, -1 not (said when
 * say is set, with what was tried). */
static int preset_path(const char *req, char *out, size_t cap, int say)
{
    const char *dir = recomp_exe_dir();

    if (has_path_chars(req)) {
        if (path_is_absolute(req))
            snprintf(out, cap, "%s", req);
        else
            snprintf(out, cap, "%s/%s", dir, req);
        if (file_exists(out))
            return 1;
        if (say)
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
    if (say)
        fprintf(stderr, LOGP "present.shader=%s: no shaders/%s.slangp beside the executable"
                " (%s)\n", req, req, dir);
    return -1;
}

int nv2a_present_chain_preset(char *out, size_t cap, unsigned *gen_out)
{
    char req[512];
    unsigned gen;

    lock();
    snprintf(req, sizeof req, "%s", s_req);
    gen = atomic_load(&s_gen);
    unlock();
    if (gen_out)
        *gen_out = gen;
    if (is_none(req))
        return 0;
    return preset_path(req, out, cap, 1);
}

int nv2a_present_chain_describe(const char *request, struct nv2a_present_param *out, int cap)
{
    PFN_libra_preset_create pc;
    PFN_libra_preset_free pf;
    libra_shader_preset_t preset = NULL;
    libra_error_t e;
    char path[1024], why[256];
    int n;

    /* The file first: a dump with no preset on disk loads no library. */
    if (is_none(request) || preset_path(request, path, sizeof path, 0) < 0
        || !nv2a_present_chain_lib())
        return -1;
    pc = (PFN_libra_preset_create)lrs_sym("libra_preset_create");
    pf = (PFN_libra_preset_free)lrs_sym("libra_preset_free");
    if (!pc || !pf)
        return -1;
    e = pc(path, &preset);
    if (e) {
        nv2a_present_chain_error(e, why, sizeof why);
        fprintf(stderr, LOGP "%s: %s\n", path, why);
        return -1;
    }
    n = nv2a_present_chain_params_read(&preset, out, cap);
    pf(&preset);
    return n;
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

/* ---- Runtime parameters ---- */

/* Overrides of every preset seen this session: the config's, then the
 * menu's. A row is kept after its override is removed, so Save can delete
 * the key it once wrote. */
#define OVR_MAX NV2A_PRESENT_OVERRIDES_MAX
struct ovr {
    char key[64], name[64];
    float v, saved_v;
    unsigned char has, saved_has;
};

static struct nv2a_present_param s_params[NV2A_PRESENT_PARAMS_MAX];
static int s_nparams;
static char s_params_key[64];
static atomic_uint s_pgen;
static struct ovr s_ovr[OVR_MAX];
static int s_novr;

void nv2a_present_chain_preset_key(const char *request, char *out, size_t cap)
{
    const char *b, *s;
    size_t n, i, o = 0;

    if (!cap)
        return;
    out[0] = 0;
    if (is_none(request))
        return;
    b = request;
    if (has_path_chars(request)) {
        for (s = request; *s; s++)
            if (*s == '/' || *s == '\\')
                b = s + 1;
    }
    n = strlen(b);
    if (n > 7 && !strcmp(b + n - 7, ".slangp"))
        n -= 7;
    /* A config section name: keep it to what a bare TOML key allows. */
    for (i = 0; i < n && o + 1 < cap; i++) {
        char c = b[i];
        out[o++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '_') ? c : '_';
    }
    out[o] = 0;
}

int nv2a_present_chain_params_read(void *preset, struct nv2a_present_param *out, int cap)
{
    PFN_libra_preset_get_runtime_params get =
        (PFN_libra_preset_get_runtime_params)lrs_sym("libra_preset_get_runtime_params");
    PFN_libra_preset_free_runtime_params fr =
        (PFN_libra_preset_free_runtime_params)lrs_sym("libra_preset_free_runtime_params");
    libra_preset_param_list_t list;
    libra_error_t e;
    int n = 0;
    uint64_t i;

    if (!get || !fr || !preset)
        return 0;
    memset(&list, 0, sizeof list);
    e = get((const libra_shader_preset_t *)preset, &list);
    if (e) {
        char why[256];
        nv2a_present_chain_error(e, why, sizeof why);
        fprintf(stderr, LOGP "preset parameters unreadable: %s\n", why);
        return 0;
    }
    for (i = 0; i < list.length && n < cap; i++) {
        const libra_preset_param_t *p = &list.parameters[i];
        struct nv2a_present_param *d = &out[n];
        if (!p->name || !p->name[0])
            continue;
        snprintf(d->name, sizeof d->name, "%s", p->name);
        snprintf(d->desc, sizeof d->desc, "%s", p->description ? p->description : "");
        d->initial = p->initial;
        d->min = p->minimum;
        d->max = p->maximum > p->minimum ? p->maximum : p->minimum;
        d->step = p->step > 0 ? p->step : 0;
        n++;
    }
    fr(list);
    return n;
}

static struct ovr *ovr_find(const char *key, const char *name)
{
    int i;
    for (i = 0; i < s_novr; i++)
        if (!strcmp(s_ovr[i].key, key) && !strcmp(s_ovr[i].name, name))
            return &s_ovr[i];
    return NULL;
}

static struct ovr *ovr_add(const char *key, const char *name)
{
    static int s_full_said;
    struct ovr *o = ovr_find(key, name);
    int i;
    if (o)
        return o;
    if (s_novr < OVR_MAX) {
        o = &s_ovr[s_novr++];
    } else {
        /* A row with no value now and none on disk has nothing left for
         * Save to delete: reuse it, so a long session of slider moves
         * across presets does not fill the table. */
        for (i = 0; i < s_novr && !o; i++)
            if (!s_ovr[i].has && !s_ovr[i].saved_has)
                o = &s_ovr[i];
        if (!o) {
            if (!s_full_said) {
                s_full_said = 1;
                fprintf(stderr, LOGP "shader_params: %d overrides held, %s.%s not kept\n",
                        OVR_MAX, key, name);
            }
            return NULL;
        }
    }
    memset(o, 0, sizeof *o);
    snprintf(o->key, sizeof o->key, "%s", key);
    snprintf(o->name, sizeof o->name, "%s", name);
    return o;
}

static const struct nv2a_present_param *active_find(const char *name)
{
    int i;
    for (i = 0; i < s_nparams; i++)
        if (!strcmp(s_params[i].name, name))
            return &s_params[i];
    return NULL;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The value a row runs at: its override, clamped to the preset's range (a
 * hand-edited config can hold anything), or the preset's own. */
static float active_value(const struct nv2a_present_param *p)
{
    const struct ovr *o = ovr_find(s_params_key, p->name);
    return o && o->has ? clampf(o->v, p->min, p->max) : p->initial;
}

void nv2a_present_chain_params_publish(void *preset, unsigned gen)
{
    struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    char req[512], key[64];
    int n = nv2a_present_chain_params_read(preset, t, NV2A_PRESENT_PARAMS_MAX);

    lock();
    if (gen == atomic_load(&s_gen)) {
        snprintf(req, sizeof req, "%s", s_req);
        nv2a_present_chain_preset_key(req, key, sizeof key);
        memcpy(s_params, t, (size_t)n * sizeof t[0]);
        s_nparams = n;
        snprintf(s_params_key, sizeof s_params_key, "%s", key);
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

void nv2a_present_chain_params_clear(void)
{
    lock();
    if (s_nparams || s_params_key[0]) {
        s_nparams = 0;
        s_params_key[0] = 0;
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

unsigned nv2a_present_chain_params_gen(void)
{
    return atomic_load(&s_pgen);
}

int nv2a_present_chain_params(struct nv2a_present_param *out, int cap, char *key, size_t keycap)
{
    int n;
    lock();
    n = s_nparams < cap ? s_nparams : cap;
    if (n > 0)
        memcpy(out, s_params, (size_t)n * sizeof s_params[0]);
    if (key && keycap)
        snprintf(key, keycap, "%s", s_params_key);
    unlock();
    return n;
}

int nv2a_present_chain_param_get(const char *name, float *out)
{
    const struct nv2a_present_param *p;
    lock();
    p = active_find(name);
    if (p && out)
        *out = active_value(p);
    unlock();
    return p != NULL;
}

int nv2a_present_chain_param_set(const char *name, float v)
{
    const struct nv2a_present_param *p;
    struct ovr *o;
    float eps;

    lock();
    p = active_find(name);
    if (!p) {
        unlock();
        return 0;
    }
    v = clampf(v, p->min, p->max);
    /* Within a thousandth of a step of the preset's value is the preset's
     * value: menu steps add up in floats. */
    eps = p->step > 0 ? p->step * 0.001f : 1e-6f;
    o = ovr_find(s_params_key, name);
    if (v > p->initial - eps && v < p->initial + eps) {
        if (o)
            o->has = 0;
    } else if ((o = ovr_add(s_params_key, name)) != NULL) {
        o->v = v;
        o->has = 1;
    }
    atomic_fetch_add(&s_pgen, 1);
    unlock();
    return 1;
}

void nv2a_present_chain_params_reset(const char *key)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++)
        if (!key || !strcmp(s_ovr[i].key, key))
            s_ovr[i].has = 0;
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

void nv2a_present_chain_params_apply(unsigned *applied, nv2a_present_param_set_fn set, void *ctx)
{
    struct { char name[64]; float v; } t[NV2A_PRESENT_PARAMS_MAX];
    unsigned g = atomic_load(&s_pgen);
    int n, i;

    if (g == *applied)
        return;
    lock();
    g = atomic_load(&s_pgen);
    n = s_nparams;
    for (i = 0; i < n; i++) {
        memcpy(t[i].name, s_params[i].name, sizeof t[i].name);
        t[i].v = active_value(&s_params[i]);
    }
    unlock();
    /* Every row, not only the one that moved: a reset or a new preset
     * changes many, and a set_param is a store into the chain's table. */
    for (i = 0; i < n; i++)
        set(ctx, t[i].name, t[i].v);
    *applied = g;
}

void nv2a_present_chain_override_load(const char *key, const char *name, float v)
{
    struct ovr *o;
    lock();
    if ((o = ovr_add(key, name)) != NULL) {
        o->v = o->saved_v = v;
        o->has = o->saved_has = 1;
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

static int ovr_dirty(const struct ovr *o)
{
    return o->has != o->saved_has || (o->has && o->v != o->saved_v);
}

int nv2a_present_chain_override_at(int i, char *key, size_t keycap, char *name,
                                   size_t namecap, float *v, int *has, int *dirty)
{
    int ok;
    lock();
    ok = i >= 0 && i < s_novr;
    if (ok) {
        const struct ovr *o = &s_ovr[i];
        if (key && keycap)
            snprintf(key, keycap, "%s", o->key);
        if (name && namecap)
            snprintf(name, namecap, "%s", o->name);
        if (v)
            *v = o->v;
        if (has)
            *has = o->has;
        if (dirty)
            *dirty = ovr_dirty(o);
    }
    unlock();
    return ok;
}

void nv2a_present_chain_overrides_saved(void)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++) {
        s_ovr[i].saved_v = s_ovr[i].v;
        s_ovr[i].saved_has = s_ovr[i].has;
    }
    unlock();
}

int nv2a_present_chain_overrides_dirty(void)
{
    int i, d = 0;
    lock();
    for (i = 0; i < s_novr; i++)
        d += ovr_dirty(&s_ovr[i]);
    unlock();
    return d;
}

int nv2a_present_chain_override_get(const char *key, const char *name, float *v)
{
    const struct ovr *o;
    int has;
    lock();
    o = ovr_find(key, name);
    has = o && o->has;
    if (has && v)
        *v = o->v;
    unlock();
    return has;
}

void nv2a_present_chain_params_reset_for_test(void)
{
    lock();
    s_nparams = 0;
    s_params_key[0] = 0;
    s_novr = 0;
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}
