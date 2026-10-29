# Bundled present shaders

RetroArch slang presets run by librashader when `present.shader` names one
(`docs/runtime/config.md`). The files are unmodified copies from
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) at commit
`2b0530bac0e8083d9d643c194929dc0a6b69bd35`, in the same layout, so the
presets' relative paths resolve as upstream. The top-level `<name>.slangp`
files are ours: a `#reference` line each, so a bare name in the config maps
to `shaders/<name>.slangp` beside the executable.

Two of them set calmer curvature than upstream, which bends a 4:3 picture a
lot on a small screen. The in-game Shaders page resets to these values, and
`[present.shader_params.<name>]` in the config overrides them:

| Name | Parameter | Ours | Upstream |
|---|---|---|---|
| `crt-lottes` | `warpX` | 0.01 | 0.031 |
| `crt-lottes` | `warpY` | 0.02 | 0.041 |
| `crt-lottes-fast` | `CURVATURE` | 0.01 | 0.02 |

Only public-domain, MIT and Unlicense files are bundled; each file keeps its
licence header.

| Name | Upstream preset | Licence |
|---|---|---|
| `crt-lottes` | `crt/crt-lottes.slangp` | public domain (Timothy Lottes) |
| `crt-lottes-fast` | `crt/crt-lottes-fast.slangp` | public domain (Timothy Lottes, CRTS) |
| `crt-hyllian` | `crt/crt-hyllian-fast.slangp` | MIT (Hyllian) |
| `fsr` | `edge-smoothing/fsr/fsr.slangp` | Unlicense; `ffx_a.h`, `ffx_fsr1.h` MIT (AMD) |

The five-pass `crt/crt-hyllian.slangp` is not bundled: its support passes and
LUT images carry no licence. Point `present.shader` at your own copy to use it.

## librashader

The presets run in [librashader](https://github.com/SnowflakePowered/librashader)
v0.12.0, the upstream prebuilt `librashader.dll` / `librashader.dylib` beside
the executable, unmodified. It is MPL-2.0: the licence is
`LICENSE-librashader.txt` in this directory, and the source is at
https://github.com/SnowflakePowered/librashader/tree/librashader-v0.12.0.
On Windows it needs the Microsoft Visual C++ Redistributable (x64); without
it the game says so in its log and presents without the shader.
