/*
 * fake_lrs.c - a stand-in for librashader with only the version exports,
 * built once per (ABI, API) pair the loader must accept or refuse.
 * FAKE_PARAMS adds the preset calls the parameter store and the descriptor
 * dump use, with a fixed two-parameter table, so they run without the real
 * library or a GPU.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#ifdef FAKE_NO_EXPORTS
EXPORT int fake_lrs_unrelated(void) { return 0; }
#else
EXPORT size_t libra_instance_abi_version(void) { return FAKE_ABI; }
EXPORT size_t libra_instance_api_version(void) { return FAKE_API; }
#endif

#ifdef FAKE_PARAMS
/* Mirrors libra_preset_param_t and libra_preset_param_list_t. */
struct fake_param {
    const char *name, *description;
    float initial, minimum, maximum, step;
};
struct fake_list {
    const struct fake_param *parameters;
    uint64_t length;
};

static int s_preset = 1;
static const struct fake_param s_table[] = {
    {"warpX", "warpX", 0.01f, 0.0f, 0.125f, 0.01f},
    {"maskDark", "Lottes maskDark", 0.5f, 0.0f, 2.0f, 0.05f},
};

/* A file whose name has "broken" in it does not parse: an error object
 * (any non-NULL pointer; there is no libra_error_write, so the loader says
 * "unknown error"). */
EXPORT void *libra_preset_create(const char *filename, void **out)
{
    if (strstr(filename, "broken"))
        return &s_preset;
    *out = &s_preset;
    return NULL;
}
EXPORT void *libra_preset_free(void **preset)
{
    *preset = NULL;
    return NULL;
}
EXPORT void *libra_preset_get_runtime_params(const void *preset, struct fake_list *out)
{
    (void)preset;
    out->parameters = s_table;
    out->length = sizeof s_table / sizeof s_table[0];
    return NULL;
}
EXPORT void *libra_preset_free_runtime_params(struct fake_list list)
{
    (void)list;
    return NULL;
}
#endif
