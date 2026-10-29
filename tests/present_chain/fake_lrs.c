/*
 * fake_lrs.c - a stand-in for librashader with only the version exports,
 * built once per (ABI, API) pair the loader must accept or refuse.
 */
#include <stddef.h>

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
