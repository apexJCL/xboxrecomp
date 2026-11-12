# xbox_apu — MCPX APU Audio Emulation

Hardware emulation of the Xbox's MCPX APU (Audio Processing Unit), extracted from [xemu](https://github.com/xemu-project/xemu). The APU is a custom chip with three processors:

- **VP (Voice Processor)** — 256 hardware voices, ADPCM/PCM decode, pitch shifting, envelopes, HRTF 3D audio
- **GP (Global Processor)** — DSP for global effects (reverb, chorus). Runs the title's own DSP program on the dsp56300 JIT (`apu_dsp_emu.c`, `apu_dsp_run.c`) in builds with `XBOXRECOMP_APU_DSP` (`RECOMP_APU_DSP=0` falls back to the stub in `apu_dsp.c`, which mixes the bins down itself).
- **EP (Encode Processor)** — DSP for AC3/DTS encoding and the stereo downmix. Runs on the same JIT; its AC97 FIFO (the stereo mix of all six speaker channels) is what the speakers hear once it delivers (`RECOMP_APU_OUT`).

The voice processor (`apu_vp.c`) follows xemu's after PR #3105 ("MCPX improvements"). Audio output is SDL3 (macOS, Linux), XAudio2 or `waveOut` (Windows), 48 kHz stereo 16-bit.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `apu.h` | 93 | **Public API**: init, shutdown, MMIO, mixer voices |
| `apu_state.h` | 683 | APU state struct (VP, GP, EP, 256 voices) |
| `apu_regs.h` | 388 | MCPX APU register definitions and field masks |
| `apu_core.c` | 2,016 | MMIO handlers, the frame thread, pacing, the monitor period, test tone, software mixer |
| `apu_vp.c` | 2,261 | Voice processor: decode, resample, filters, envelopes, LFOs, HRTF, mix |
| `apu_dsp.c` | 506 | The DSP stub (`RECOMP_APU_DSP=0`): bins mixed down without the GP/EP, the doorbell ack |
| `apu_dsp_emu.c` / `.h` | 758 / 135 | One GP or EP core on the dsp56300 JIT: memory map, DMA engine, bootstrap, frame loop |
| `apu_dsp_run.c` | 770 | Runs both cores each APU frame: scratch/FIFO memory, the warm-up workers and their catch-up, the EP's stereo output |
| `apu_dsp_warm.h` | 69 | The warm-up schedule (tested in `tests/apu_dsp_warm`) |
| `apu_mmio_hook.c` | 506 | Trapped access to the APU window (VEH on Windows, SIGSEGV/SIGBUS on POSIX; x86-64 and arm64 decoders) |
| `apu_fault_bench.c` | 99 | `RECOMP_APU_FAULT_BENCH`, the trap cost benchmark |
| `apu_sdl.c` | 287 | SDL3 output (macOS, Linux); the dummy driver runs on `apu_vclock` |
| `apu_xaudio2.c` / `.h` | 285 / 113 | XAudio2 output (Windows); `.h` is the output contract both backends implement |
| `apu_vclock.c` / `.h` | 67 / 36 | A virtual 48 kHz device clock for headless runs |
| `apu_wav.c` / `.h` | 208 / 48 | `RECOMP_AUDIO_WAV`, the tap on the final mix |
| `apu_lockstat.c` / `.h` | 210 / 55 | `RECOMP_TRACE=apu_lock` lock and stage timing |
| `apu_shim.h` | 531 | QEMU compatibility (threads, atomics, clocks) |
| `apu_debug.h` | 104 | Debug logging macros |
| `fpconv.h` | 71 | Float/int sample conversion |

## Quick Start

```c
#include "apu.h"

// Initialize APU with pointer to Xbox RAM
// (ram_ptr = native pointer to Xbox VA 0x00000000)
MCPXAPUState *apu = mcpx_apu_init_standalone(ram_ptr);

// Test: play a 440Hz sine tone
mcpx_apu_play_test_tone(apu);

// --- OR use the software mixer for direct PCM playback ---

// Allocate a mixer voice
int voice = apu_mixer_alloc_voice();
APUMixerVoice *v = apu_mixer_get_voice(voice);

// Configure it
v->pcm_data = my_pcm_buffer;     // 16-bit signed PCM
v->pcm_bytes = buffer_size;
v->num_channels = 2;              // 1=mono, 2=stereo
v->sample_rate = 44100;           // Source sample rate
v->volume = 0.8f;                 // 0.0 - 1.0

// Play
apu_mixer_play(voice, 1);  // 1 = looping

// Stop and free
apu_mixer_stop(voice);
apu_mixer_free_voice(voice);

// Shutdown
mcpx_apu_shutdown(apu);
```

## Architecture

```
 Xbox game: its own DirectSound (statically linked, recompiled)
        │ APU registers, 0xFE800000+ (trapped: apu_mmio_hook.c)
        │ DSP memory and GPRST/EPRST (the MCPX window, untrapped)
        ▼
 ┌────────────┐ 32 mixbins  ┌────────────┐ 6 speaker channels ┌────────────┐
 │     VP     │ ──────────► │     GP     │ ─────────────────► │     EP     │
 │ 256 voices │  per frame  │ DSound's   │  scratch rings,    │ DSound's   │
 │ apu_vp.c   │  (32 smp)   │ GP program │  read a period     │ EP program │
 └────────────┘             │ on the JIT │  (256 smp) a time  │ on the JIT │
                            └────────────┘                    └─────┬──────┘
                                                    FIFO 0: stereo  │
                                                    downmix         ▼
                              period (256 samples) ──► SDL3 / XAudio2 / waveOut
                                                       RECOMP_AUDIO_WAV
```

The frame thread (`apu_core.c`) runs one 32-sample frame at a time (1500 a second): the VP, then the GP, and the EP every eighth frame. The speakers get the EP's stereo output (its FIFO 0) once it delivers, and the GP's front pair before that (`RECOMP_APU_OUT=auto|ep|gp`). With `RECOMP_APU_DSP=0` the stub in `apu_dsp.c` mixes the bins itself (no reverb, and not the EP's downmix).

The GP and EP programs keep their positions in the rings between them in their own DMA descriptors, so each must run every frame, in order, or they drift apart for good. After a bootstrap the JIT compiles the whole program on the first frame, so for about a second a core's frames run on a warm-up worker (`apu_dsp_warm.h`). A frame that finds the worker busy is not heard, but the worker runs it afterwards (catch-up) before taking the next. The `[APU DSP] cycle cap hit ...` line counts skipped, caught-up and dropped frames.

## Two Audio Paths

### 1. Hardware APU Emulation (MMIO)

For games that program the APU directly. The game writes to MMIO registers at 0xFE800000+, which are intercepted by the VEH hook and routed to the APU emulation:

```c
// Game writes: voice 5, set buffer address
*(uint32_t*)(0xFE802000 + 5 * voice_stride + VOICE_BUF_OFFSET) = buffer_addr;

// VEH intercepts → mcpx_apu_mmio_write() → VP processes voice
```

The VP runs on the APU frame thread, mixing active voices at the hardware frame rate.

### 2. Software Mixer (Direct API)

For games where you've decoded the audio format yourself and just want to play PCM. The mixer provides 64 voice slots with:

- 16-bit signed PCM input (mono or stereo)
- 16.16 fixed-point resampling (any rate → 48kHz)
- Per-voice volume control
- Looping support

```c
int apu_mixer_alloc_voice(void);                    // Returns slot 0-63, or -1
void apu_mixer_free_voice(int slot);
APUMixerVoice *apu_mixer_get_voice(int slot);       // Configure voice params
void apu_mixer_play(int slot, int looping);          // Start playback
void apu_mixer_stop(int slot);                       // Stop playback
```

## MMIO Hook

The APU MMIO hook (`apu_mmio_hook.c`) traps accesses to the APU register range: a Vectored Exception Handler on Windows, a SIGSEGV/SIGBUS handler on Linux and macOS. It decodes the faulting instruction with the shared decoders (`src/platform/mmio_decode.h` for x86-64, `mmio_decode_a64.h` for arm64):

```
Access to 0xFE800000+
  → Fault (the page is not mapped)
  → Handler decodes the faulting instruction (MOV, ADD, XOR, ... / LDR, STR, ...)
  → Routes to mcpx_apu_mmio_read() or mcpx_apu_mmio_write()
  → Steps past the instruction and resumes
```

A plain `mov` to a constant address in the window is lifted to a direct model call (`XBOX_MMIO_R/W8/16/32`), so DirectSound's per-voice register updates never fault; the ALU, compare and exchange forms keep the fault path.

This allows recompiled code to access APU registers using normal memory operations, exactly as the original Xbox code did.

## Voice Processor Details

The VP processes 256 hardware voices per frame:

- **ADPCM decode**: Xbox ADPCM (4-bit, 64-sample blocks)
- **PCM formats**: 8-bit unsigned, 16-bit signed, 24-bit, 32-bit; stream voices fed through SSL segments
- **Pitch**: log2 fixed-point (4.12), 0 = 48 kHz; ramped across each frame; 8-point Lagrange resampling
- **Envelopes and LFOs**: the hardware's attack, decay and release curves; two LFOs
- **Filters**: the DLS2 state-variable low-pass, ParaEQ, I3DL2 direct/room one-poles
- **HRTF**: sign-magnitude HRIR taps on a 3D voice's first four routes
- **Mixbins**: 32, with per-bin headroom; voices route to any 8

These follow xemu PR #3105, measured on hardware there.

## Register Map (Key Registers)

```
Offset      Name                   Description
──────────────────────────────────────────────────
0x1000      NV_PAPU_ISTS           Interrupt status
0x1100      NV_PAPU_FECTL          Frontend control
0x2000      NV_PAPU_SECTL          Synth engine control
0x202C      NV_PAPU_VPVADDR        VP voice address base
0x3000+     NV_PAPU_GP*            Global Processor registers
0x4000+     NV_PAPU_EP*            Encode Processor registers
```

Full register definitions in `apu_regs.h`.

## Dependencies

- `apu_shim.h`: the QEMU types the xemu code expects
- SDL3 on macOS and Linux; `xaudio2_8` and `winmm` on Windows
- The dsp56300 JIT release archive for `XBOXRECOMP_APU_DSP` builds (fetched by CMake; see `CMakeLists.txt`)
