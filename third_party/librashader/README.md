# librashader C header

`librashader.h` from the
[librashader](https://github.com/SnowflakePowered/librashader) v0.12.0 release
zips (C ABI 2, API 5), unmodified. The header is MIT (its comment); the
library is MPL-2.0 and is never linked: `src/kernel/nv2a_present_chain.c`
loads the prebuilt upstream `librashader.dylib` / `librashader.dll` at run
time, and the packaging that ships it adds the MPL-2.0 notice and the source
link.
