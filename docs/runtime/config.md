# Configuration (`config.toml`, `config.default.toml`)

The enhancements layer (`src/enhance/`) is opt-in. It is built only with
`-DXBOXRECOMP_ENHANCE=ON` (off by default), and a title that turns it on calls
it. This page describes its configuration: the two files and how they layer,
where they are read from, how the environment overrides them, the format and
the key names.

## Two files and the environment

A title calls `xbox_enhance_init(root, title)` (`src/enhance/enhance.h`) once
at startup, after `recomp_env_init` and before the guest starts. `root` is
the user data root, the directory that holds the player's files; a title
passes `"."`, the working directory, which a bundle's launcher sets to the
bundle. A title with one executable passes `title = NULL`.

There are two kinds of file:

- **`config.default.toml`**, the defaults that ship with the game. It sits
  beside the executable, `recomp_exe_dir()` (`src/platform/recomp_exe_dir.h`:
  `_NSGetExecutablePath` on macOS, `GetModuleFileNameA` on Windows,
  `/proc/self/exe` on Linux, the working directory if none works); on macOS
  a `.app` keeps it in `Contents/Resources`, which the layer looks in when
  the executable's directory has none. It is read, never copied, so a new
  version's keys and values reach every player. Nobody edits it: an update
  replaces it.
- **`config.toml`**, the player's file. It holds only the keys the player
  changed. An update never writes it.

For a key, the first of these that has a value wins:

| Tier | Source | Log source |
|---|---|---|
| 1 | the environment variable bound to the key, for example `RECOMP_RENDER_SCALE` for `render.scale` (see [Environment](#environment)) | `env` |
| 2 | `<root>/<title>/config.toml`, the per-title file | `file` |
| 3 | `<root>/config/config.toml`, the file shared by all titles | `file` |
| 4 | `config.default.toml` beside the executable | `defaults` |
| 5 | the default that the module passes in | `default` |

`RECOMP_CONFIG=<path>` replaces tiers 2 and 3 with that one file; the
defaults file is still read under it. `RECOMP_CONFIG=none` reads no file at
all, the defaults file included, so only the environment and the code
defaults apply: a golden run pins it, so a shipped default never changes a
golden. `RECOMP_ENHANCE_CONFIG`, the variable's earlier name, is still read,
with one `[ENV] RECOMP_ENHANCE_CONFIG is deprecated; use RECOMP_CONFIG` line.

When the player's file is absent and its directory exists, the layer writes
it as a short comment header that says what goes there (the per-title file
when the title's directory exists, else the shared one; a `RECOMP_CONFIG`
file whose directory exists gets one too). A `RECOMP_CONFIG` file whose
directory is missing is an error. Under `none` nothing is written.

### Migration from `enhance.toml`

An earlier version named the player's file `enhance.toml`, with the same
keys. When a location the layer reads as `config.toml` is absent and an
`enhance.toml` is beside it, the layer renames it once and says so:

```
[ENHANCE] config: renamed ./config/enhance.toml to config.toml (one-time migration)
```

Only a file named `config.toml` is a candidate (so a `RECOMP_CONFIG` file
with another name never is), `<root>/enhance.toml` itself is not, and
nothing is renamed under `none`. A rename that fails is reported once; the old file is left alone and
not read, and the next run tries again.

### Errors and the log

If a file does not parse, the layer reports it with its line number and
ignores the whole file; the other files still apply. A typo therefore never
applies half a file:

```
[ENHANCE] config error: ./config/config.toml:3: duplicate key 'render.scale' (first set on line 2) (file ignored)
```

At startup the layer logs one line that names the files, lowest layer first,
each with its own key count, and one line for each environment override:

```
[ENHANCE] config: /opt/game/config.default.toml (6 keys), ./config/config.toml (1 key)
[ENHANCE] render.scale = 2 from RECOMP_RENDER_SCALE
```

With `RECOMP_CONFIG=none` the line is `[ENHANCE] config: none (RECOMP_CONFIG)`,
and with no file found it is `[ENHANCE] config: none`.

When a value has the wrong type (`scale = "2"`), the layer reports it once,
names the file and line, and uses the next tier. The layer can also list keys
that nothing read (`enhance_cfg_report_unused`), in either file, so that a
misspelled key such as `scael = 2` is visible instead of being ignored
silently.

## File format: a subset of TOML

Every file that this reader accepts is valid [TOML](https://toml.io) 1.0. The file must be UTF-8.

```toml
# Comments start with '#' and run to the end of the line.

[render]                # a table; dotted names work too: [input.bindings]
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

[input]
bindings.a = "Space"    # dotted key: the same as [input.bindings] a = "Space"
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
The one exception is `[input.actions]` (below), whose keys are action names.
A lookup uses the full dotted name, such as `render.scale`. A UTF-8 BOM and
CRLF line endings are accepted.

## Keys

The current keys are listed below. Every key defaults to the stock behaviour
except `present.pacing`, which defaults to `"sleep"` (the frames and pace
match the console's spin, and a host core is freed), so an empty file (or no
file) runs the game as it was apart from that wait. Golden runs always use
the stock settings, `present.pacing = "spin"` included.

| Key | Type | Values | Default | Env override |
|---|---|---|---|---|
| `render.scale` | int | internal resolution factor: 1, 2, 3, ... | `1` | `RECOMP_RENDER_SCALE` |
| `render.aniso` | int | anisotropic filtering on mipmapped textures: `0` (off), `2`..`16` | `0` | `RECOMP_RENDER_ANISO` |
| `display.aspect` | string | `"W:H"`: `"4:3"` (stock) or a wider ratio such as `"16:9"`, `"16:10"`, `"21:9"`: Hor+ on Metal and D3D11 ([Hor+](#hor-widescreen-displayaspect)); the CPU rasteriser ignores it with a log line | `"4:3"` | `RECOMP_DISPLAY_ASPECT` |
| `display.movie` | string | `"black"`, `"fit"`, `"fill"`: with a wide aspect, a full-screen movie or still is pillarboxed, stretched to the width, or scaled to cover it (top and bottom cropped) | `"black"` | `RECOMP_DISPLAY_MOVIE` |
| `display.overscan` | int | guest pixels cropped from each left and right edge at present, top and bottom in proportion: `0` (stock) to `64` ([Overscan](#display-overscan-displayoverscan)); ignored with a wide `display.aspect` and by the CPU rasteriser, each with a log line | `0` | `RECOMP_DISPLAY_OVERSCAN` |
| `present.filter` | string | `"nearest"`, `"linear"`, `"integer"` | `"nearest"` | `RECOMP_PRESENT_FILTER` |
| `present.fullscreen` | bool | borderless fullscreen at start | `false` | `RECOMP_PRESENT_FULLSCREEN` |
| `present.pacing` | string | `"sleep"`, `"spin"` (stock, the console-original timing): how a lowered guest busy-wait waits | `"sleep"` | `RECOMP_PRESENT_PACING` |
| `present.surface` | string | `"auto"`, `"window"`, `"internal"`: who scales the frame to the screen, the game or the compositor | `"auto"` | `RECOMP_PRESENT_SURFACE` |
| `textures.pack` | string | a texture pack file ([texture-pack.md](../formats/texture-pack.md)); relative to the executable's directory | none | `RECOMP_TEXTURES_PACK` |
| `textures.font` | string | `"on"`, `"off"`: the pack's font-atlas entries | `"on"` | `RECOMP_TEXTURES_FONT` |
| `textures.text` | string | `"on"`, `"off"`: the pack's baked-text entries | `"on"` | `RECOMP_TEXTURES_TEXT` |
| `overlay.stats` | bool | the stats strip in the window's top right corner: fps, frame ms (average and worst of the last second), pacing, scale and backend, at 2 Hz; never in a frame dump | `false` | `RECOMP_OVERLAY_STATS` |

A title adds its own tables (`[fps]`, `[fx]`, ...) in the same files, and
lists them with their stock values in its `config.default.toml`. The
title reads them through the same layering and binds each to one of its own
environment variables, `enhance_cfg_bind_env("game.mode", RENV_GAME_MODE)`,
where `RENV_GAME_MODE` is a row of its `recomp_env_game.h`. A title that
offers only some of a key's values reads the key all the same and says in
one line which value it runs instead. Such keys change only that game's
code and leave the toolkit's backends alone, so every backend sees the same
guest values. The title documents its keys with its environment variables.

The keyboard and mouse keys (`[input]`, `[input.bindings]`) have their own
section, [Input](#input), below.

### Hor+ widescreen (display.aspect)

A ratio wider than 4:3 widens the picture instead of stretching it. Every
4:3-shaped colour and depth target is allocated wider by a pad a side
(`nv2a_host_pad`: 107 guest pixels for 640x480 at 16:9) and the title's
columns are mapped centered in it, so the game's own perspective projection
fills the pads with more of the world. The projection is not patched: what
the title culls at the old frustum's edge pops in at the screen's edge.
Guest memory and the write-back keep the 640x480 centre columns, so the
guest and the goldens see the stock frame. A window (not fullscreen) opens
at the padded frame's shape, `nv2a_host_window_w`: 854x480 at 16:9 and
scale 1, so the pads show 1:1; fullscreen keeps the display's size and
letterboxes. A square target (BLiNX 2's screen-space reflections) is
widened by the same ratio plus an eighth of its width a side, so a
reflection read the water bumps past the frame's edge still finds the
mirrored view instead of the target's clamped edge column.

Each batch is placed by one rule table (`nv2a_aspect_place`,
`src/kernel/nv2a_backend_common.h`), after a 2D test that runs the batch's
vertex program on the CPU and checks that every vertex shares one `oPos.w`,
an exact whole number from 1 to 1024 (`nv2a_aspect_layer_w`,
`nv2a_aspect_scan`, gated per program). `oPos.xy` are pixels whatever `w`
is, and BLiNX 2 draws parts of its stage HUD at a fixed view depth of 2 or 3
and its title movie at 100:

- 3D: centered; a clip box at the frame's edges reaches the target's edges.
- 2D, not full width (HUD, menus, text): centered and scissored to the 4:3
  frame, so pieces the title parks off-screen never show in the pads. The
  HUD keeps the title's 4:3 layout; there is no option to move or scale
  it. A 2D batch's triangles say nothing of which HUD element they belong
  to, so moving pieces by their position split groups the title draws as
  one (BLiNX 2's item counter from its items, a dialogue face from its
  box).
- 2D, full width, sampling only render targets, nothing, or a guest
  texture no wider than a quarter of the frame (fades, post passes):
  stretched over the whole target. On a square target that is the ratio's
  pad only (`nv2a_host_pad_ratio`), not the overscan, so a full-width copy
  between the frame and a square lines up, and reads of a padded square
  by such a pass take the same columns.
- 2D, full width over a wider guest texture (a movie, a still):
  `display.movie`.
- A quad of at most 6 vertices sharing one positive `w` that is not a
  screen layer (`nv2a_aspect_layer_w`; `nv2a_aspect_screen_quad`) is not 2D by the test above, but it is
  stretched in either of two cases:
  - it samples only padded render targets and spans exactly the frame's
    width: a pass laid over the frame (JSRF's Can boost). Such a quad
    wholly in the top or bottom third also marks that third as holding a
    bar, for this flip and the next;
  - it is drawn with the depth test off, over no guest picture, within the
    frame's width and height, reaching its left or right edge, and wholly
    in a third that holds a bar (`nv2a_aspect_band`): a piece of a
    letterbox band. Burnout 3's crash cam draws its bars, their edge lines
    (as half-width quads) and the list backing that way at one `w`, so they
    all reach the target's edges together while the text inside stays in
    the 4:3 frame. Its front-end menu frame has the same shape but no bar,
    so it keeps its 4:3 place.
- A batch drawn with the depth test off whose vertices (all of them under
  64, about 32 evenly spaced beyond) share one `w` under 1 that is the reciprocal of a whole number from 2 to
  1024 (`nv2a_aspect_flat_w`, `nv2a_aspect_flat`) is a flat near layer: it
  keeps its centred place but is scissored to the 4:3 frame like 2D.
  Burnout 3 draws its whole front end at rhw 10 (`w` 0.1); its menu grid
  overhangs the frame to x 656 and text slides in from off screen, which
  stock hides past the screen's edge. Its race's place tags over the rival
  cars use the same program at the car's computed depth, between 0.54 and
  1, where no reciprocal lies, so they still follow their car into the
  pads. The known limit: camera-locked geometry drawn without depth at a
  round view distance (`w` 0.5, 0.25, 0.1), such as a full-screen flash
  or tint quad or a lens overlay, is taken too. If it overhangs the 4:3
  frame on purpose, it is cut at the old 4:3 edge: a pad tinted only up
  to the 4:3 frame is this case.

`RECOMP_TRACE=aspect` prints one `[aspect.program]` line per program,
texture and placement, and the target size per flip, and one
`[aspect.flat]` line per program and `w` the first time a flat near layer
is clipped.

### Display overscan (display.overscan)

Some titles leave a dark border at the left and right edges of their 4:3
frame, because the televisions they were made for hid the outer few percent
of the picture. `display.overscan = n` does what such a TV did: it cuts `n`
guest columns from each side and `n * h / w` guest rows from the top and
bottom, then zooms the rest uniformly into the place the whole frame would
take. 640x480 at 9 shows guest (9, 6.75) to (631, 473.25). The value is in
guest pixels, so it reads the same at every `render.scale`. Above 64 is
clamped to 64 and below 0 to 0, each with a warning.

- **Present only.** The crop is a source rectangle on the Metal and D3D11
  present. The guest frame, the host targets, the write-back, `fb_dump`,
  `metal_host_dump` and the D3D11 dump are unchanged; `window_shot` reads
  the window, so it shows the crop.
- **Filter.** With `nearest`, a zoom of 622 columns into the window draws
  uneven columns; `present.filter = "linear"` hides that. Under `integer`
  the vertical cut is rounded to whole host rows (7 at scale 1), so the
  picture's aspect is off by up to one row.
- **Not with Hor+.** With a `display.aspect` wider than 4:3 the 4:3 frame's
  edges sit inside the padded frame, where no source rectangle can cut them,
  so a non-zero value is reported once and 0 is used.
- **Every frame.** The crop applies to every screen, so a title whose border
  shows only on some screens loses those columns everywhere.
- Ignored by the CPU rasteriser (one log line) and by the Metal readback
  debug present (`metal_present=readback`). A non-zero value adds
  ` display.overscan=N` to the `[ENHANCE]` line, after `display.aspect`.

### What the render and present keys do

`xbox_enhance_init` reads them once and hands them to the backends and the
presenter as `nv2a_host_opts` (`src/kernel/nv2a_backend_common.h`). Those
libraries never link the layer: without it the options are all zero, which
is stock. One line shows the values in force:

```
[ENHANCE] render.scale=2 render.aniso=0 present.filter=integer present.fullscreen=0 present.pacing=sleep present.surface=window(auto) (display.aspect=4:3)
```

A change while the game runs (the in-game menu) goes through
`nv2a_host_opts_request(o, mask)`, from any thread: it is queued, and the
Metal and D3D11 backends apply it with `nv2a_host_opts_apply()` at the
next flip, right after its present: the one point where no target is bound
and no upload is in flight, and the frame shown is still sized by the scale
it was drawn at. A new `render.scale` then drains the pending occlusion
queries at the old scale and drops every render target, depth
buffer and texture (Metal writes a target back
first, so the next one seeds from it), with one line
(`render.scale=N from this flip: targets and textures dropped`); a pack
change makes every texture upload again. The CPU and null backends apply
too and repeat their "ignored" line on a scale or overscan change.

The other live keys need nothing at the flip but the stored value:
`render.aniso` is part of the sampler key at each draw, `display.movie`
is read at each movie draw, and `display.overscan` at each present (a
non-zero overscan under a wide `display.aspect` is refused, as at start,
and the aspect itself is start-only). `present.filter` and
`present.fullscreen` are the presenter's: the SDL window's thread sees the
options generation move and sets the texture's scale mode or calls
`SDL_SetWindowFullscreen`; the D3D11 window, which the walker thread owns,
swaps its style with `SetWindowPos` in the same on_flip, and the next
flip resizes the swap chain. `present.pacing` changes through
`xbox_enhance_set_pacing(mode)` (`enhance.h`), from any thread. Each shows
within two flips. `RECOMP_DEBUG=opts_at=<flip>:<key>=<value>,...` makes the
same requests at given flips, to check a switch without the menu.

- **`render.scale = N`** (1..4; outside that it is clamped, with a line).
  Metal and D3D11 allocate every colour and depth target at N times its
  guest size and draw with an N-times viewport. The pixel-to-NDC mapping
  stays on the guest size, so every vertex program, the pre-transformed
  HUD ones included, lands where it did, at N times the pixels.
  Render-to-texture samples the scaled target (supersampled). A bilinear
  linear-format (rect) texture drawn 1:1 to guest pixels, such as a movie
  frame, is sampled where the 1x render samples it: each N x N block of
  host pixels reads its guest pixel's 1x sample point, so the padding past
  the picture does not bleed into the last row and column. The frame the
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
- **`render.aniso = N`** (2..16; 0 or 1 is off, the stock filtering;
  outside 0..16 it is clamped, with a line). D3D11 and Metal sample a
  texture anisotropically when it has mip levels, its minification filters
  and it is not a cube map: the world's surfaces, sharp at grazing angles. A
  texture without mips is left alone, since those are render targets read
  back (bloom, blur and composite passes), linear-format movie frames and
  lookup tables: screen-aligned or data, and they shimmer under it. Both
  backends filter min, mag and mip linearly under it. Live; the CPU
  backend ignores it.
- **`present.filter`.** `nearest` is the stock letterbox. `linear` is the
  same letterbox with bilinear filtering. `integer` shows the frame at the
  largest whole multiple that fits, centered on black; a frame larger than
  the window (2x in a small window) is fitted with linear filtering instead
  of cropped. The SDL window (macOS, Linux, every backend) and the D3D11
  window follow it. The D3D11 window also follows resizes now; at stock it
  still copies the frame into its 640x480 back buffer, and anything else
  goes through a scaling blit. The Win32 GDI window of the CPU backend does
  not follow these keys yet.
- **`present.fullscreen`.** A borderless window covering the display at
  start, with no mode change (SDL `SDL_WINDOW_FULLSCREEN` at the desktop mode; D3D11 a `WS_POPUP`
  the primary monitor's size). Live: on and off while the game runs (the
  D3D11 window goes back to the size it had, or to the starting size). The
  CPU backend's Win32 GDI window does not follow it. On macOS this is not a
  Spaces fullscreen: the presenter sets `SDL_VIDEO_MAC_FULLSCREEN_SPACES=0`
  as its default, because a Spaces toggle in an app that is not frontmost
  does nothing while SDL reports success. Setting that variable to `1` in
  the environment brings Spaces back.
- **`present.surface`.** Who scales the frame to the screen. `window` is
  the stock behaviour: the swap chain follows the window, and the game
  scales the frame into it as `present.filter` says. `internal` makes the
  surface the frame itself, 640x480 times `render.scale` (1280x960 at 2x):
  D3D11 creates the window's client area and the swap chain at that size
  (a `WS_POPUP` at the top-left corner under `present.fullscreen`, without
  asking the screen size), resizes the window and the swap chain only when
  the menu changes `render.scale`, and copies the frame into it under every
  filter. Dragging the edge of a desktop window stretches the swap chain to
  the window without keeping the aspect (DXGI's stretch), where Metal
  letterboxes. A scaling compositor then fits it to
  the output with its own filter. That is the point under gamescope (Steam
  Game Mode): Steam's *Scaling filter* (FSR, linear, nearest, ...) applies
  to the surface gamescope receives, and FSR upscales only a surface
  smaller than its output, so with `window` (a surface the screen's size)
  it has nothing to do. On the Steam Deck use `render.scale = 1` for FSR:
  at 2x the 1280x960 surface is taller than the 800-line output and is
  downsampled instead (a sharp picture, but not FSR). On a desktop without
  a scaling compositor `internal` shows the frame unscaled. `auto` (the
  default) means `internal` when the launcher sets `RECOMP_HOST_GAMESCOPE`
  (the SteamOS bundle's `launch.sh` does under gamescope) and `window`
  otherwise; the runtime does not look for gamescope itself. A bare `.exe`
  started under gamescope without the launcher sets
  `RECOMP_PRESENT_SURFACE=internal` (or `surface = "internal"`) by hand.
  The `[ENHANCE]` line names the result and its reason:
  `present.surface=internal(auto: gamescope)`, `window(auto)`, `internal`.
  The key is read once, when the window is made; the menu cannot change it.
  The Metal window (macOS) keeps its drawable at the frame's size under
  `internal` and lets the layer scale it, letterboxed, nearest for
  `nearest` and linear otherwise; the macOS launcher sets no hint, so
  `auto` gives `window` there unless `RECOMP_HOST_GAMESCOPE` is set by hand. The
  SDL renderer window (the CPU backend) ignores the key with one line.
- **`present.pacing`.** Only for a title generated with
  `tools.recomp --spin-waits` ([lifting](../pipeline/04-lifting.md#spin-waits)),
  whose busy-wait loops call `RECOMP_SPIN_WAIT` on their back edge. `sleep`
  (the default) blocks the thread until the kernel signals a state change
  (a vblank, an ISR or DPC, a fence write) or 1 ms passes
  (`src/kernel/kernel_pacing.h`), which frees a host core. `spin` is the
  console-original timing and does nothing there: the loop holds a host
  core as it holds the console's CPU. Set it with `[present]`
  `pacing = "spin"` in the player's `config.toml`, or
  `RECOMP_PRESENT_PACING=spin`; a title's `config.default.toml` can ship
  either, and its value wins over this default. Without the layer the
  kernel stays at `spin`. The frames are the
  guest's own either way; what can move is when the loop sees its exit,
  by up to the wake latency. Without lowered loops the key changes nothing.
  A site lowered with `"sleep": "kick_event"` (`RECOMP_SPIN_WAIT_KICK`)
  sleeps only while the walker's kick event wait is on as well, and spins
  otherwise, so with `kick_event` off it is `spin` at either value.
  `RECOMP_TRACE=pacing` prints the flip intervals, process CPU and, per
  site, how its waits ended and how its loops left: on a wake, or on the
  1 ms timeout. A site whose loops keep leaving on the timeout (more than
  5% of exits) has a writer the kernel never signals; timeouts in the
  middle of a loop are normal.

### Texture packs

- **`textures.pack = "<file>"`.** A pack of host-side texture replacements
  ([texture-pack.md](../formats/texture-pack.md)), keyed by the hash the
  Metal and D3D11 backends already compute for every upload. A relative
  path is resolved against the executable's directory, where
  `config.default.toml` is, so a bundle's defaults name `enhance/hud.pack`
  (a `.app` keeps the pack beside the executable too). The layer
  loads the pack's index at startup and logs one line after the settings
  line: `[ENHANCE] textures.pack=none` when unset, or
  `[ENHANCE] textures.pack=enhance/hud.pack (41 entries: 30 art, 1 font, 10 text; scale 4, text 2; 10 bc3) textures.font=on textures.text=on`
  (the path ends at the first space). A
  file that is missing, has another magic or version, a non-zero reserved
  field, flags the version does not define, an entry outside the file, an
  entry with more levels than its size allows
  (`1 + floor(log2(max(w, h)))`), a BC3 entry whose size is not a multiple
  of 4, or an unsorted index is refused whole
  (`[ENHANCE] textures.pack=<path>: <reason> (<resolved>); no pack`) and
  the run stays stock. On an upload (a cache miss or a texture whose
  bytes changed since the last flip) a backend looks the texel hash up
  and, on a hit whose entry has the guest texture's aspect ratio, creates
  the host texture at the pack entry's size and level count from the
  pack's texels (if the device refuses that texture, the guest texels go
  up instead); everything else (cache key,
  eviction, the per-flip rehash, samplers, UVs) is as it was, so a texture
  the game rewrites stops matching and returns to its guest bytes. Without
  a pack the only added work per upload is the entry-count test. The CPU
  rasteriser samples guest memory and ignores the key with one line
  (`[ENHANCE] textures.pack ignored: ...`). Every 600 flips a backend
  with a pack logs its hit count; under `RECOMP_TRACE=tex_pack` it also
  prints the uploads the pack did not replace, one
  `[TEXPACK] miss <hash> <w>x<h> <format> x<count> candidate|linear/mip`
  line each (texture-pack.md, "Misses"), which says what a pack still
  lacks. A texture the pack holds but did not use carries a tag:
  ` (text off)` (or art, font), ` (pack off)`, or ` (entry refused)` when
  the backend could not take the entry (a GPU without BC3, such as an
  older iPad, says so once; a texture too large; a failed read).
- **`textures.font = "on" | "off"`, `textures.text = "on" | "off"`.** A
  version 2 pack marks each entry as art, the font atlas or baked text.
  `off` makes the pack skip that kind, so the game's own glyphs or text
  images are drawn, for a player who prefers them or a language the pack
  does not cover. Both are read whether or not a pack is set; with one
  loaded they are in its log line. A change made at run time (the
  in-game menu, through `nv2a_host_opts_request`) takes effect at the next
  flip: the backends upload again every texture made under the old
  setting. A golden run must show
  `textures.pack=none`: the golden check fails any other value.

### Input

`[input]` turns the keyboard and mouse on as a source on port 0 (player 1)
and tunes them; `[input.bindings]` says which key or mouse input drives
each of the Duke's controls. `xbox_enhance_init` reads both through
`src/enhance/enhance_input.c` into `src/input` (`keyboard.c`, `mouse.c`).
The keys and mouse merge onto a pad on port 0: buttons are OR'd, the larger
analog value wins, and a stick the keys or mouse move replaces the pad's.
A title that never calls `xbox_enhance_init` keeps the compiled-in key
table of `src/input/README.md` and no mouse.

| Key | Type | Values | Default | Env override |
|---|---|---|---|---|
| `input.keyboard` | bool | the keys drive port 0 | `false` | `RECOMP_KEYBOARD` |
| `input.mouse` | bool | the mouse drives port 0: motion is a stick, buttons and the wheel as bound | `false` | `RECOMP_MOUSE` |
| `input.mouse_look` | string | `"right_stick"`, `"left_stick"`, `"none"`: the stick mouse motion drives | `"right_stick"` | none |
| `input.mouse_sensitivity` | float | `0.1` .. `10.0`; at `1.0`, 40 counts in one poll is full deflection | `1.0` | none |
| `input.mouse_invert_y` | bool | mouse forward looks down | `false` | none |
| `input.mouse_invert_x` | bool | | `false` | none |
| `input.mouse_min_deflection` | float | `0.0` .. `0.9`: any motion deflects the stick at least this much | `0.45` | none |
| `input.mouse_capture` | string | `"focus"`: the cursor is grabbed while the window has focus (the click that brings the window forward drives nothing: Win32 says which click that is, and on SDL and under a window manager that focuses first it is the first press within 250 ms of the focus); `"click"`: the first click in the window grabs it (and drives nothing); `"never"` | `"focus"` | none |
| `input.mouse_release_key` | string | a key name: frees the cursor, and grabs it again; `""` or `"none"`: no release key | `"F9"` | none |
| `input.wheel_pulse_ms` | int | `20` .. `2000`: how long one wheel notch holds its control | `100` | none |
| `input.stick_ramp_ms` | int | `0` .. `2000`: a stick key goes from 0.41 of full deflection to full over this time; `0` is full at once | `0` | none |
| `input.walk_key` | string | a key name: while held, the stick keys give `walk_scale` of full; `""` or `"none"`: no walk key | none | none |
| `input.walk_scale` | float | `0.41` .. `1.0` (outside is clamped) | `1.0` | none |
| `input.chord_delay_ms` | int | `0` .. `100`: a member of a host-action pad chord reaches the game this long after its press, so a chord completed inside the window never does ([input.actions], rule 3) | `0` | `RECOMP_INPUT_CHORD_DELAY_MS` |
| `[input.actions]` | table | an action name to a binding or a list of them; `[]` unbinds (below) | the actions' defaults | none |

These defaults are the layer's. The three stick knobs default to the
legacy keyboard (no ramp, no walk key), so a title that ships no `[input]`
keys keeps it; BLiNX 2's `config.default.toml` sets 60 ms, `LAlt` and
`0.5`. A title's `config.default.toml` also sets the two switches (BLiNX 2
ships both `true`), and `RECOMP_CONFIG=none` reads no
file, so both stay off unless the environment turns them on.
`RECOMP_KEYBOARD` and `RECOMP_MOUSE` predate the naming rule and keep their
names.

The floor of 0.41 is there because titles commonly ignore a stick below
about 0.4 of full deflection (BLiNX 2 does): a tap of a stick key or a
slow mouse motion that stayed under it would do nothing.

**Bindings.** Each key of `[input.bindings]` is a control, and its value
a string or a list of strings naming inputs:

```toml
[input.bindings]
a = "Space"
start = ["Enter", "Escape"]
b = "mouse1"
left_thumb = []             # unbound
```

Controls, by upstream's names (`src/input/input_map.c`): `dpad_up
dpad_down dpad_left dpad_right start back left_thumb right_thumb a b x y
black white left_trigger right_trigger left_stick_up left_stick_down
left_stick_left left_stick_right right_stick_up right_stick_down
right_stick_left right_stick_right`.

Inputs, in any case, by the label the host layout gives the key (the key
labelled Z is `Z` on AZERTY too): `A`..`Z`, `0`..`9`, `F1`..`F12`,
and upstream's key names (`xbox_InputMap_KeyFromName`, listed under
"Key names" in `src/input/README.md`): `Space`, `Enter` (or `Return`),
`Escape` (or `Esc`), `Backspace`, `Tab`, `Pause`, `CapsLock`, `LShift`
`RShift` `Shift`, `LCtrl` `RCtrl` `Ctrl` (or `Control`), `LAlt` `RAlt`
`Alt` (the unsided names match either side), `Up` `Down` `Left` `Right`,
`Insert` `Delete` `Home` `End` `PageUp` `PageDown`, `Numpad0`..`Numpad9`,
`NumpadAdd` `NumpadSubtract` `NumpadMultiply` `NumpadDivide`
`NumpadDecimal`, the OEM keys (`Comma` `Period` `Slash` `Semicolon`
`LeftBracket` `RightBracket` ...), and the mouse buttons `Mouse1`..`Mouse5`
(left, right, middle, back, forward). The fork adds `NumpadEnter`,
`WheelUp` and `WheelDown`. The Cmd and Windows keys are not bindable: they
belong to the system. The table lives in `src/input/input_names.c` over
upstream's `input_map.c`; `RECOMP_TRACE=key` prints each key's name as
it is pressed. A control holds up to 8 inputs.

**Resolution.**

- Per control, the highest file that names it wins whole: the player's
  `start = "Enter"` replaces the defaults' `["Enter", "Escape"]`, and
  `a = []` unbinds.
- An input a higher file names under one control is removed from every
  control a lower file gave it, so binding `Escape` to `back` in your file
  takes it off `start` without touching `start`.
- Inside one file, an input named under two controls is reported and the
  second ignored.
- A control no file names keeps the compiled-in table's keys, less any a
  file binds elsewhere.

**Errors** never stop the game and leave the rest of the file applied; each
is one line, with the file and the line of the key:

```
[INPUT] config error: config/config.toml:7: unknown control 'jmp' under [input.bindings] (controls: dpad_up dpad_down ... right_stick_right); ignored
[INPUT] config error: config/config.toml:8: unknown input 'Spacebar' in input.bindings.x (see docs/input.md); skipped
[INPUT] config error: config/config.toml:3: 'J' already bound to input.bindings.x on line 2; ignored here
[INPUT] config error: config/config.toml:9: input.bindings.a is a string or a list of strings (got integer); ignored
[INPUT] config error: config/config.toml:2: input.mouse_sensitivity 0 out of range 0.1 .. 10; using 1
[INPUT] config error: config/config.toml:3: input.mouse_look 'camera' is not one of right_stick, left_stick, none; using right_stick
[INPUT] config error: config/config.toml:5: input.stick_ramp_ms should be an integer (got string); using the default
```

`F9`, `F10` and `F11` are reserved (freeing the cursor, `menu.open`'s
default in `[input.actions]`, fullscreen); binding one is applied with a
warning in the same format, `... input.bindings.white 'F10' is menu.open's
default ([input.actions]); bound anyway`. A key bound here is then refused
for a host action (rule 2 of `[input.actions]`), so F10 bound to a control
leaves `menu.open` on its pad bindings.

The release key and the walk key act before the bindings, so a control
bound to either never sees it. That gets a warning, as does setting both to
the same key:

```
[INPUT] config warning: input.mouse_release_key 'F9' is also bound to input.bindings.y
[INPUT] config warning: input.walk_key and input.mouse_release_key are the same key
```

`RECOMP_KBM_SCRIPT` plays through the keyboard and mouse, so with both off
it is ignored, with `[INPUT] kbm_script ignored: keyboard and mouse off`.

**Log.** One line at init: the switches, the look target and sensitivity,
and how many controls each file decided, lowest file first:

```
[INPUT] config: keyboard=on mouse=on look=right_stick sens=1.0 bindings=24 (<exe dir>/config.default.toml) +1 (./config/config.toml)
[INPUT] config: keyboard=off mouse=off (RECOMP_CONFIG=none)
```

The `[ENHANCE]` lines do not mention `[input]`, and the golden runs, which
run under `RECOMP_CONFIG=none`, keep both off.

### Host actions (`[input.actions]`)

An action is something the host does rather than the game: open the menu,
toggle the stats strip, a mod's free camera. `src/enhance/enhance_actions.c`
binds actions to keys, to host-only pad buttons and to pad chords, and
never gives the game a press it would not have had: it reads the host pad
beside the state the game gets, and only ever takes presses out of that
state (a consumed chord). Remapping the game's own controls stays with
`[input.bindings]`.

```toml
[input.actions]
menu.open = ["F10", "guide+a", "paddle1", "back+left_thumb@400"]
camera.free.toggle = "F6"
overlay.stats = []                  # unbound
```

The action name is a bare dotted key, as above, or quoted: `"menu.open" =
[...]` is the same entry, which is what a TOML writer emits for it. A quoted
name must be the whole key directly under `[input.actions]` (or
`actions."menu.open"` under `[input]`); writing both spellings of one
action is a duplicate key. Because a bare `menu.open` is a table `menu` in
TOML, no action is named after another plus a part (`menu.open` and
`menu.open.all`); the registry refuses the second. Per action, the highest file that names it decides all of its
bindings, as for a control under `[input.bindings]`; an action no file
names keeps its defaults. An action holds up to 8 bindings.

The toolkit's actions:

| Action | Default bindings | Does |
|---|---|---|
| `menu.open` | `F10`, `guide+a`, `paddle1` | opens and closes the menu (until the menu lands, a toast: "Menu: not in this build yet") |
| `overlay.stats` | none | toggles the stats strip |
| `shot.window` | none | a window shot (a log line for now) |

A game's `config.default.toml` adds to these, for example a fallback chord
for hosts where Guide is not seen (BLiNX 2: `back+left_thumb@400`). A mod
registers its own actions, named after the mod (`camera.free.toggle`).

**A binding** is one of:

- **A key**, by the names of `[input.bindings]`, optionally after
  modifiers: `F10`, `ctrl+F6`. A plain key fires only with no modifier
  held, and `ctrl+F6` only with Ctrl and no other modifier. Mouse inputs
  are not bindings.
- **A host-only button** alone: `guide`, `misc1` (share, capture),
  `paddle1` .. `paddle4` (upper right, upper left, lower right, lower
  left: the Elite's P1, P3, P2, P4, the Deck's R4, L4, R5, L5, and the
  DualSense Edge's right back button, left back button, right Fn button
  and left Fn button), and `touchpad`. The Duke has none of them, so the
  game never sees one.
- **A pad chord**: two to eight buttons joined by `+`, from `a b x y
  black white left_trigger right_trigger start back left_thumb
  right_thumb dpad_up dpad_down dpad_left dpad_right` and the host-only names, with an
  optional hold `@<ms>` (`0` .. `2000`). It is read on port 0 (player
  1) unless it starts with `pad2:` .. `pad4:`. A chord whose first member
  is host-only and whose others are guest buttons (`guide+a`) is a
  modifier chord (rule 7).

**Rules**, so a binding never takes what the game uses. A refused binding
gets one line and the rest of the action's bindings stand:

1. A single guest button (`a`, `start`) is refused. A chord of guest
   buttons needs a hold of at least 200 ms.
2. A key the game receives is refused, and the game keeps it. The check
   is against the effective key map: the compiled-in table, then
   `[input.bindings]`, `input.walk_key` and `input.mouse_release_key`.
   Modifiers count: Ctrl and Shift drive the game in the compiled-in
   table, so `ctrl+F6` is refused unless the player unbinds them.
3. A chord is consumed. From the moment all of its buttons are down, they
   reach the game released until each is let go. A chord let go before
   its hold fires nothing. The game does still see the first button's
   press, and masking then gives it a release the player did not make:
   with `back+start`, START pauses BLiNX 2 and is then released. So pick
   members the game ignores on a lone press (a button with no use plus
   one used only in menus, as `back+left_thumb` in BLiNX 2), or set
   `input.chord_delay_ms`, which holds each chord member back from the
   game for that long after its press. That costs the same latency on
   those buttons, which is why it is off by default.
4. The defaults use only F-keys, host-only buttons, modifier chords and
   chords with a hold.
5. A scripted or replayed pad (a game harness's) never fires an action:
   actions read the host pad, which the script does not touch. A key from
   `RECOMP_KBM_SCRIPT` does, because it goes through the same key table
   as a real key; that is how an unattended run drives the free camera.
6. Only a feature key arms the actions: `menu.enabled`, `overlay.stats`,
   or a mod's enable key (`camera.free`). Registering an action never
   does. Disarmed, nothing is read or masked. The start line says which:

   ```
   [ENHANCE] actions hook=off
   [ENHANCE] actions hook=on (menu.enabled, camera.free)
   ```

   A golden run (`RECOMP_CONFIG=none`) has `hook=off`. The overlay's
   stats and toast layers are armed with the hook and only then: stock
   bakes no font, and a mod's toast is then its log line only.
7. A modifier chord (`guide+a`) fires on the guest button's press while
   the host-only button is held; no hold is needed. While the host-only
   button is held, the guest members of every modifier chord on that port
   reach the game released from their press until they are let go, even
   when the host-only button goes first, so the game never sees the A of
   `guide+a`. A guest member pressed before the host-only one goes to the
   game and does not complete the chord. The button that closes the menu
   stays masked after the close until it is let go.

The game may stop polling the pad (a loading screen). The flip then reads
the host pad itself when a port has not been polled for 100 ms, so keys,
host-only buttons and modifier chords still fire there. Plain chords of
guest buttons fire only from a poll.

**Errors and the log.** Each refusal names the action, the file and the
line (or `(default)` for a default), and the binding:

```
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'a' refused: a single guest button would take it from the game; use a chord of two with a hold (back+left_thumb@400) or a host-only button (guide, paddle1)
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'back+left_thumb' refused: a chord of guest buttons needs a hold of at least 200 ms (back+left_thumb@400)
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'a+guide' refused: a chord with a host-only button needs it first and only once (guide+a)
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'Z' refused: 'Z' is bound to the guest's a (input.bindings); the game keeps it
[INPUT] action input.actions.menu.open (./config/config.toml:3): more than 8 bindings; the rest skipped
[INPUT] config warning: ./config/config.toml:4: no action named 'menu.opne'
[INPUT] config warning: input.chord_delay_ms=500 out of 0..100; using 100
```

Every fire is logged, at most 10 lines a second:

```
[ACTION] menu.open via guide+a
[ACTION] 2 more fires not logged
```

**Guide per host.** `guide+a` needs the host to hand the Guide button to
the game. Where it does not, F10 and the game's fallback chord still work.

| Host | Backend | Guide seen? | What to do |
|---|---|---|---|
| macOS | SDL3 (GameController or HIDAPI) | Usually. macOS may keep the Home button: System Settings > Game Controllers > the controller, then let apps use the Home button. | the setting; F10; the fallback chord |
| Linux desktop, native | SDL3 (evdev or HIDAPI) | Yes, unless Steam runs with Steam Input on for the pad, which keeps Guide for its overlay | Steam Input off for the title; F10; the fallback chord |
| Steam Deck | XInput through Steam's virtual pad (Proton), or SDL3 | No: SteamOS keeps the Steam button and its chords for itself | R4: bound to F10 in Steam Input under Proton (below), `paddle1` where SDL3 reads the pad; the fallback chord |
| Windows, native | XInput (`XInputGetStateEx`) | Yes, unless the Xbox Game Bar takes the Xbox button: Windows 11: Settings > Gaming > Game Bar, "Allow your controller to open Game Bar", off | the setting; F10; the fallback chord |
| Windows build under Proton (desktop Linux) | XInput through Wine | Only with Steam Input off for the title | Steam Input off; F10; the fallback chord |

Only Guide reaches a Windows build: XInput has no paddles, share button or
touchpad. A chord with Guide stays incomplete where Guide is not seen, and
its guest member then reaches the game as usual.

**The Steam Deck and Steam Input.** On the Deck the menu goes on R4, the
upper right back grip. SteamOS keeps the Steam button and its chords for
itself, so `guide+a` never reaches the game there. Under Proton, Steam Input is on by default and the game
sees Steam's virtual pad, an XInput pad with no grips (L4, L5, R4, R5).
So bind R4 to a key once, in Steam Input:

1. With the game selected in the library (or from the in-game Steam
   menu), open the controller settings: the controller icon, then *Edit
   Layout* (*Controller Settings* in Desktop mode).
2. Pick *Back Grip Buttons*, then *R4*, then *Keyboard* and the key
   `F10`.
3. Back out; Steam saves the layout for that game.

R4 then sends F10, which fires `menu.open` like a real key (the log shows
`[ACTION] menu.open via F10`); a second press closes the menu. Another
grip can carry another action's key the same way, for example R5 to `F6`
for BLiNX 2's free camera. Leave L4 and L5 unbound for now: they are kept
for quick save and quick load (F5 and F9), and save states have not
landed yet. F9 frees the cursor today (`input.mouse_release_key`).

A build that reads the pad through SDL3 sees R4 as `paddle1`, which
`menu.open` has by default, so there it needs no Steam Input binding.
That will be a native Linux build, once there is one, with Steam Input
off for the title (SDL's HIDAPI Steam Deck driver). Today the SDL3 case
is macOS, where an Elite's P1 opens the menu by default. An Elite's
paddles and the DualSense Edge's back and Fn buttons show as `paddle1`
.. `paddle4` only where the firmware and SDL report them. An Elite
profile that copies a face button reports the face button instead,
which is a guest button.

### Naming rule

- Tables group keys by feature: `[render]`, `[present]`, `[display]`, and a title's own.
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
| `RECOMP_CONFIG` | path of the player's file, which replaces both `config.toml` files, or `none` for no file at all (the defaults included). `RECOMP_ENHANCE_CONFIG` is its earlier name, read with a deprecation line |
| `RECOMP_RENDER_SCALE` | overrides `render.scale` |
| `RECOMP_DISPLAY_ASPECT` | overrides `display.aspect` |
| `RECOMP_DISPLAY_MOVIE` | overrides `display.movie` |
| `RECOMP_DISPLAY_OVERSCAN` | overrides `display.overscan` |
| `RECOMP_PRESENT_FILTER` | overrides `present.filter` |
| `RECOMP_PRESENT_FULLSCREEN` | overrides `present.fullscreen` |
| `RECOMP_PRESENT_PACING` | overrides `present.pacing` |
| `RECOMP_PRESENT_SURFACE` | overrides `present.surface` |
| `RECOMP_HOST_GAMESCOPE` | set by the SteamOS launcher (`1`) when it runs under gamescope; `present.surface = "auto"` then means `internal`. No file key binds it; a line in the bundle's `config/launch.env` overrides it |
| `RECOMP_TEXTURES_PACK` | overrides `textures.pack` |
| `RECOMP_TEXTURES_FONT` | overrides `textures.font` |
| `RECOMP_TEXTURES_TEXT` | overrides `textures.text` |
| `RECOMP_OVERLAY_STATS` | overrides `overlay.stats` |
| `RECOMP_KEYBOARD` | overrides `input.keyboard` (an older name, outside the naming rule) |
| `RECOMP_MOUSE` | overrides `input.mouse` |

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
recomp_cfg *c = recomp_cfg_load_file("config.toml", err, sizeof err);   /* NULL + "file:line: msg" */
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
xbox_enhance_init(".", NULL);                   /* the toolkit's keys: enhance.h */
enhance_cfg_init(data_root, title_dir);          /* the reader alone; logs the files */
const char *u = enhance_cfg_user_path();         /* the player's file, for a writer; NULL under none */
const recomp_cfg *d = enhance_cfg_defaults();    /* config.default.toml as loaded, or NULL */
enhance_cfg_bind_env("game.mode", RENV_GAME_MODE); /* a game key with an env override */
int scale = (int)enhance_cfg_int("render.scale", 1);
int mode  = enhance_cfg_choice("game.mode", modes, 0);
const recomp_cfg *c = enhance_cfg_lookup("render.sizes");   /* arrays: no env tier */
enhance_cfg_report_unused();                     /* after the modules' init */
```

A settings writer (the in-game menu) writes only the keys whose value
differs from `enhance_cfg_defaults()` to `enhance_cfg_user_path()`, so the
player's file stays a list of changes. `enhance_cfg_source_of(key)` says
`ENHANCE_SRC_DEFAULTS` for a value from the defaults file.

`xbox_enhance_init` does not report unused keys itself: the title calls
`enhance_cfg_report_unused()` once, after it has read its own keys, so a
game key is never listed as unused.

Read the values during init (one thread) and keep them; the layer is not
meant to be read per frame. Tests: `tests/enhance_cfg` (ctest
`enhance_cfg`).

## Mods

A mod is game code built into the title's executable that reads and steers
the guest through one small API (`src/enhance/mod.h`). It exists only with
`XBOXRECOMP_ENHANCE=ON`. A title with no mods logs and runs exactly as
before. The draw paths then have one untaken branch.

The title lists its mods in a NULL-terminated table and hands it over before
`xbox_enhance_init`, which runs each `init` in table order as its last step:

```c
static int cam_init(const struct xbox_mod_api *api)
{
    s_api = api;
    s_speed = api->cfg_float("mods.free_camera.speed", 1.0);
    api->on_frame(cam_frame, NULL);
    api->batch_filter(cam_hide_hud, NULL);
    return 0;                                   /* non-zero leaves the mod out */
}
static const struct xbox_mod free_camera = { "free_camera", cam_init, cam_shutdown };
static const struct xbox_mod *const mods[] = { &free_camera, NULL };

xbox_mod_register_table(mods);
xbox_enhance_init(".", NULL);
```

The table is handed over with a call, not a self-registering macro, for
three reasons:

- A static library drops an object that nothing names.
- A linker section is spelled differently for PE, Mach-O and ELF.
- The table fixes the order.

The title's glue calls two entries:

- `xbox_mod_frame()` once per guest frame, on the guest's thread.
- `xbox_mod_input_captured()` from its pad read. While it returns true, the
  title must see a released pad.

`struct xbox_mod_api` (version 1, `XBOX_MOD_API_VERSION`) holds:

| Member | What it gives |
| --- | --- |
| `mem`, `XBOX_MOD_MEM32(api, va)`, `XBOX_MOD_MEMF(api, va)` | The guest's memory. It is valid from the first `on_frame` or filter call; it is 0 during init. |
| `cfg_string`, `cfg_int`, `cfg_float`, `cfg_bool`, `cfg_choice`, `cfg_bind_env` | The getters above. A mod's keys go under `[mods.<name>]` by convention and count as used. |
| `log(mod, fmt, ...)` | One `[MOD <name>]` line. |
| `on_frame(fn, user)` | Called from `xbox_mod_frame`, in table order. Init only; up to `XBOX_MOD_MAX_FRAME`. |
| `input_state(out)` | The title's last port-0 read (the keyboard merged), and the mouse's raw counts and buttons while captured. A mod never polls, because a poll would move the keyboard's stick ramp. |
| `input_capture(on)` | Takes port 0. While captured, the mouse's motion goes to the mod instead of the look stick, and the title's pad glue releases the pad. Either edge drops the motion gathered so far. |
| `batch_filter(fn, user)` | Asked before each Metal or D3D11 draw, on the render thread, with the program hash and draw state (`struct nv2a_batch_info`). If any filter returns non-zero, the batch is skipped. `present_offset` is the surface the last flip presented, to tell a back buffer from an offscreen target. A skipped batch inside a visibility test counts as occluded. Init only; up to `XBOX_MOD_MAX_FILTER`. The CPU rasteriser does not ask. |

A failed init leaves nothing behind. Its registrations are dropped and the
capture goes back to what the earlier mods left. Shutdown runs in reverse
order, once, at exit. The guest and the render thread may still be running
then, and a frame or filter call may be in flight. So a mod's shutdown must
not free anything its callbacks use.
Hotkeys come from `xbox_FramebufferKeyDown` and the names in
`src/input/input_names.h`. Tests: `tests/enhance_mod` (ctest `enhance_mod`).

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
