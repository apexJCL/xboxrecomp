# Enhancements config (`enhance.toml`)

The enhancements layer (`src/enhance/`) is opt-in. It is built only with
`-DXBOXRECOMP_ENHANCE=ON` (off by default), and a title that turns it on calls
it. This page describes its config file: the format, where the file is read
from, how the environment overrides it, and the key names.

## Where the file is read from, and precedence

A title calls `xbox_enhance_init(root, title)` (`src/enhance/enhance.h`) once
at startup, after `recomp_env_init` and before the guest starts. `root` is
where the file lives. The file is runtime configuration that belongs to an
installed game, so a title passes the directory of its executable,
`recomp_exe_dir()` (`src/platform/recomp_exe_dir.h`: `_NSGetExecutablePath`
on macOS, `GetModuleFileNameA` on Windows, `/proc/self/exe` on Linux, the
working directory if none works). During development that is the build
directory; `RECOMP_ENHANCE_CONFIG=<path>` points anywhere else. A title with
one executable passes `title = NULL`, so the file is `<root>/enhance.toml`.

For a key, the first of these that has a value wins:

1. The environment variable bound to the key, for example `RECOMP_RENDER_SCALE`
   for `render.scale` (see [Environment](#environment)).
2. `<root>/<title>/enhance.toml`, the per-title file.
3. `<root>/enhance.toml`, the file shared by all titles.
4. The default that the module passes in.

`RECOMP_ENHANCE_CONFIG=<path>` replaces 2 and 3 with that one file.
`RECOMP_ENHANCE_CONFIG=none` reads no file, so only the environment and the
defaults apply.

If a file does not parse, the layer reports it with its line number and
ignores the whole file. A typo therefore never applies half a file:

```
[ENHANCE] config error: data/enhance.toml:3: duplicate key 'render.scale' (first set on line 2) (file ignored)
```

At startup the layer logs one line that names the files, and one line for
each environment override:

```
[ENHANCE] config: data/TITLE/enhance.toml, data/enhance.toml (6 keys)
[ENHANCE] render.scale = 2 from RECOMP_RENDER_SCALE
```

When a value has the wrong type (`scale = "2"`), the layer reports it once,
names the file and line, and uses the next tier. The layer can also list keys
that nothing read (`enhance_cfg_report_unused`), so that a misspelled key such
as `scael = 2` is visible instead of being ignored silently.

## File format: a subset of TOML

Every file that this reader accepts is valid [TOML](https://toml.io) 1.0. The file must be UTF-8.

```toml
# Comments start with '#' and run to the end of the line.

[render]                # a table; dotted names work too: [aspect.program]
scale = 2               # integer: 42, -7, 1_000, 0xFF, 0o17, 0b101
sharpness = 0.5         # float: 0.5, 1e3, -2.5E-2, inf, nan
filter = "linear"       # string: "..." with escapes \t \n \" \\ \uXXXX
shader_dir = 'C:\mods'  # literal string: '...' with no escapes
vsync = true            # bool: true or false (lower case)
sizes = [1, 2, 3]       # array of scalars; it can span lines,
modes = [               # hold comments, mix types and end with a comma
  "a",   # first
  "b",
]

[aspect]
program.a1b2c3 = "hud"  # dotted key: the same as [aspect.program] a1b2c3 = "hud"
```

The reader rejects the following. Each is reported as an error with a line
number:

- multi-line strings (`"""` and `'''`)
- inline tables (`{ a = 1 }`)
- arrays of tables (`[[x]]`)
- nested arrays
- dates and times
- unquoted strings (`filter = linear`; the error message says that strings
  need quotes)
- duplicate keys, a table defined twice, and a key that is used both as a
  value and as a table
- reopening a table that a dotted key defined (`a.b = 1`, then `[a]`), and
  using a dotted key to add to a table that a header defined
- `_` anywhere except between two digits (`1_000` and `0xFF_FF` are fine;
  `0x_FF` and `1_e5` are not), and a second base prefix (`0x0x1F`)
- the `\e` escape (TOML 1.1 only)
- invalid UTF-8, and control characters (DEL included, tab excepted) in
  strings and comments

Floats are parsed the same way in every locale: a process locale with a comma
decimal point does not change how `0.5` is read, either in the file or in the
environment.

Keys are bare (`A-Z a-z 0-9 _ -`) or quoted, with no `.` inside a quoted part.
A lookup uses the full dotted name, such as `render.scale`. A UTF-8 BOM and
CRLF line endings are accepted.

## Keys

The current keys are listed below. Every key defaults to the stock behaviour,
so an empty file (or no file) runs the game as it was. Golden runs always use
the stock settings.

| Key | Type | Values | Default | Env override |
|---|---|---|---|---|
| `render.scale` | int | internal resolution factor: 1, 2, 3, ... | `1` | `RECOMP_RENDER_SCALE` |
| `display.aspect` | string | `"4:3"` only for now (`"16:9"`, `"16:10"`, `"21:9"` are reported as not implemented, and 4:3 is used) | `"4:3"` | `RECOMP_DISPLAY_ASPECT` |
| `present.filter` | string | `"nearest"`, `"linear"`, `"integer"` | `"nearest"` | `RECOMP_PRESENT_FILTER` |
| `present.fullscreen` | bool | borderless fullscreen at start | `false` | `RECOMP_PRESENT_FULLSCREEN` |
| `present.pacing` | string | `"spin"` (stock), `"sleep"`: how a lowered guest busy-wait waits | `"spin"` | `RECOMP_PRESENT_PACING` |
| `aspect.program.<hash>` | string | `"hud"`, `"stretch"`, `"3d"`: a per-shader override of the aspect classifier | none | none |

A title adds its own tables (`[fps]`, `[fx]`, ...) in the same file. The
title reads them through the same layering and binds each to one of its own
environment variables, `enhance_cfg_bind_env("game.mode", RENV_GAME_MODE)`,
where `RENV_GAME_MODE` is a row of its `recomp_env_game.h`. A title that
offers only some of a key's values reads the key all the same and says in
one line which value it runs instead. Such keys change only that game's
code and leave the toolkit's backends alone, so every backend sees the same
guest values. The title documents its keys with its environment variables.

The input-selection keys (`[input]`) will be added later, when that work is
done.

### What the render and present keys do

`xbox_enhance_init` reads them once and hands them to the backends and the
presenter as `nv2a_host_opts` (`src/kernel/nv2a_backend_common.h`). Those
libraries never link the layer: without it the options are all zero, which
is stock. One line shows the values in force:

```
[ENHANCE] render.scale=2 present.filter=integer present.fullscreen=0 present.pacing=spin (display.aspect=4:3)
```

- **`render.scale = N`** (1..4; outside that it is clamped, with a line).
  Metal and D3D11 allocate every colour and depth target at N times its
  guest size and draw with an N-times viewport. The pixel-to-NDC mapping
  stays on the guest size, so every vertex program, the pre-transformed
  HUD ones included, lands where it did, at N times the pixels.
  Render-to-texture samples the scaled target (supersampled). The frame the
  guest sees is still guest-sized: Metal box-filters its write-back to
  guest memory, so `RECOMP_FB_DUMP` and `fb_dump_at` frames stay 640x480.
  The box filter averages the stored (gamma-encoded) values, not linear
  light, the same as xemu's downscale.
  Visibility-test counts are divided by N squared before the title reads
  them, a non-zero count staying non-zero. The window gets the full-size
  frame. The CPU rasteriser draws into guest memory at the guest pitch, so
  it ignores the key and says so once
  (`[ENHANCE] render.scale=N ignored: ...`).
  - Known differences from the console: post passes that sample with
    guest-texel offsets (blurs) cover N times fewer host pixels and look
    sharper; lines and points stay one host pixel wide.
  - D3D11 `d3d11_dump` frames of a scaled run are the render target at host
    size (1280x960 at 2x). Golden frames are compared at the stock size
    only, and a golden run pins `RECOMP_RENDER_SCALE=1`.
- **`present.filter`.** `nearest` is the stock letterbox. `linear` is the
  same letterbox with bilinear filtering. `integer` shows the frame at the
  largest whole multiple that fits, centred on black; a frame larger than
  the window (2x in a small window) is fitted with linear filtering instead
  of cropped. The SDL window (macOS, Linux, every backend) and the D3D11
  window follow it. The D3D11 window also follows resizes now; at stock it
  still copies the frame into its 640x480 back buffer, and anything else
  goes through a scaling blit. The Win32 GDI window of the CPU backend does
  not follow these keys yet.
- **`present.fullscreen`.** A borderless window covering the display at
  start, with no mode change (SDL `FULLSCREEN_DESKTOP`; D3D11 a `WS_POPUP`
  the primary monitor's size). There is no hotkey yet.
- **`present.pacing`.** Only for a title generated with
  `tools.recomp --spin-waits` ([lifting](../pipeline/04-lifting.md#spin-waits)),
  whose busy-wait loops call `RECOMP_SPIN_WAIT` on their back edge. `spin`
  (stock) does nothing there: the loop holds a host core as it holds the
  console's CPU. `sleep` blocks the thread until the kernel signals a state
  change (a vblank, an ISR or DPC, a fence write) or 1 ms passes
  (`src/kernel/kernel_pacing.h`), which frees that core. The frames are the
  guest's own either way; what can move is when the loop sees its exit,
  by up to the wake latency. Without lowered loops the key changes nothing.
  `RECOMP_TRACE=pacing` prints the flip intervals, process CPU and, per
  site, how its waits ended and how its loops left: on a wake, or on the
  1 ms timeout. A site whose loops keep leaving on the timeout (more than
  5% of exits) has a writer the kernel never signals; timeouts in the
  middle of a loop are normal.

### Naming rule

- Tables group keys by feature: `[render]`, `[present]`, `[display]`, `[aspect]`, and a title's own.
  Key names are lower case, with `_` between words.
- A key that has an environment override uses the variable `RECOMP_` + the
  dotted key in upper case with `.` replaced by `_`. For example,
  `render.scale` becomes `RECOMP_RENDER_SCALE`.
- A choice is a string from a fixed list, never a number code.

## Environment

The layer's variables belong to the **config** tier of `recomp_env.h`. They
exist only when the layer is built (`RECOMP_ENV_HAVE_ENHANCE_KEYS`):

| Variable | Meaning |
|---|---|
| `RECOMP_ENHANCE_CONFIG` | path of the one file to read, or `none` |
| `RECOMP_RENDER_SCALE` | overrides `render.scale` |
| `RECOMP_DISPLAY_ASPECT` | overrides `display.aspect` |
| `RECOMP_PRESENT_FILTER` | overrides `present.filter` |
| `RECOMP_PRESENT_FULLSCREEN` | overrides `present.fullscreen` |
| `RECOMP_PRESENT_PACING` | overrides `present.pacing` |

The layer converts an environment value to the type of the key:

- Integers are read like `strtoll(v, 0, 0)`, so `0x2` works.
- Booleans accept `1/0`, `true/false`, `yes/no` and `on/off`, in any case.
- A choice must be one of the listed strings.

An empty value counts as unset. A value that does not convert is reported once
and ignored, so the file decides.

The env-key names follow the naming rule. Design D6 used
`RECOMP_RENDER_ASPECT`; it became `RECOMP_DISPLAY_ASPECT` because the key is
`display.aspect`.

## API

There are two layers. Both are C11 and need only libc.

`recomp_cfg.h` is the reader. A title can use it on its own for any file:

```c
char err[256];
recomp_cfg *c = recomp_cfg_load_file("enhance.toml", err, sizeof err);  /* NULL + "file:line: msg" */
long long s = recomp_cfg_int(c, "render.scale", 1);      /* default when missing or wrong type */
double    f = recomp_cfg_float(c, "render.sharpness", 0); /* an int is accepted too */
const char *a = recomp_cfg_string(c, "display.aspect", "4:3");
int       v = recomp_cfg_bool(c, "render.vsync", 1);
static const char *const modes[] = { "stock", "fast", "free", NULL };
int       m = recomp_cfg_choice(c, "game.mode", modes, 0); /* index into the list */
size_t    n = recomp_cfg_array_len(c, "render.sizes");    /* + recomp_cfg_array_int/... (c, key, i, def) */
/* recomp_cfg_count / recomp_cfg_key_at / recomp_cfg_find / recomp_cfg_line for scans */
recomp_cfg_free(c);
```

Every getter accepts a NULL config and then returns its default.

`enhance_cfg.h` is the layering that modules use:

```c
xbox_enhance_init(recomp_exe_dir(), NULL);      /* the toolkit's keys: enhance.h */
enhance_cfg_init(data_root, title_dir);          /* the reader alone; logs the files */
enhance_cfg_bind_env("game.mode", RENV_GAME_MODE); /* a game key with an env override */
int scale = (int)enhance_cfg_int("render.scale", 1);
int mode  = enhance_cfg_choice("game.mode", modes, 0);
const recomp_cfg *c = enhance_cfg_lookup("render.sizes");   /* arrays: no env tier */
enhance_cfg_report_unused();                     /* after the modules' init */
```

`xbox_enhance_init` does not report unused keys itself: the title calls
`enhance_cfg_report_unused()` once, after it has read its own keys, so a
game key is never listed as unused.

Read the values during init (one thread) and keep them; the layer is not
meant to be read per frame. Tests: `tests/enhance_cfg` (ctest
`enhance_cfg`).

## Design note: why the reader is our own

The config format is TOML because the user asked for TOML, or failing that
ini. Every file the reader accepts is valid TOML, so the files can be checked
with any TOML tool.

Two existing C readers were considered:

- **tomlc99** (CK Tan, MIT). Complete TOML 1.0 with a tree API. Its README
  marks it obsolete in favour of tomlc17.
- **tomlc17** (CK Tan, MIT). The maintained successor. It is complete TOML
  1.0/1.1 and has dates, inline tables and arrays of tables.

Both licences are MIT, the same as the toolkit, so either could be vendored
with a NOTICE entry. Each is several thousand lines, and most of that code is
grammar this file does not need. Their errors are good, but their API is a
tree that we would wrap in a getter layer anyway.

The reader here, `recomp_cfg.c`, is about 900 lines including comments. It
supports exactly the needed subset:

- tables, dotted keys
- strings, integers, floats, booleans
- flat arrays
- comments

Its errors carry a line number, and every unsupported construct is reported
by name instead of being half-parsed. There is no third-party code to track,
which matters because the upstream toolkit's rule is no external
dependencies.

If a later need goes past the subset (inline tables for input bindings, for
example), the same API can be put on top of a vendored tomlc17 without
changing any caller.
