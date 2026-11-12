/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu_lockstat.h"
#include "recomp_env.h"
#include "apu.h"
#include "fpconv.h"
#include <stdarg.h>

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUVoiceFilter *f = &d->vp.filters[v];
    memset(&f->svf, 0, sizeof(f->svf));
    memset(&f->peq, 0, sizeof(f->peq));
    memset(&f->i3dl2, 0, sizeof(f->i3dl2));
    memset(&f->interp, 0, sizeof(f->interp));
    f->interp_pos = 1 << 16;    /* see voice_resample */
    f->resample_buf_pos = f->resample_buf_len = 0;
    f->gains = false;           /* a new voice fades in from silence */
    f->pitched = false;
    f->dls2_set = false;
    f->peq_set = false;
    memset(&f->lfo_phase, 0, sizeof(f->lfo_phase));
    f->lfo_armed = false;
    f->hrtf.params_set = false;
    hrtf_filter_clear_history(&f->hrtf);
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

/* The curves below are xemu's after PR #3105 ("MCPX improvements",
 * hw/xbox/mcpx/apu/vp/vp.c), measured there on hardware.
 *
 * Filter coefficients take 2^(x / 4096) to a 1024th of an octave, then to
 * the nearest 1/65536, at most FFFFh (measured: D97Eh gave 3043h). */
static float filter_coefficient(double x)
{
    int k = (int)floor(x / 4.0);
    int octave = (int)floor(k / 1024.0);
    double c = ldexp(exp2((k - octave * 1024) / 1024.0) * 65536.0, octave);
    return (float)(fmin(round(c), 65535.0) / 65536.0);
}

/* Route gains in 1/16384, attenuations in 1/64 dB (measured): straight lines
 * between whole decibels through round(4095 * 10^(-k / 20)), rounded down;
 * from 48 dB a straight line to nothing at 63 dB. Attenuation 0 is unity. */
static uint32_t attenuation_nodes[64];

static void attenuation_init(void)
{
    for (int k = 0; k <= 48; k++)
        attenuation_nodes[k] = (uint32_t)floor(4095.0 * pow(10.0, -k / 20.0) + 0.5);
    for (int k = 49; k < 64; k++)
        attenuation_nodes[k] = attenuation_nodes[48] * (63 - k) / 15;
}

static uint32_t attenuation_gain(int att)
{
    if (att <= 0)
        return 16384;
    if (att >= 63 * 64)
        return 0;
    uint32_t n0 = attenuation_nodes[att >> 6];
    uint32_t n1 = attenuation_nodes[(att >> 6) + 1];
    return (256 * n0 - 4 * (n0 - n1) * (uint32_t)(att & 63)) >> 6;
}

static float attenuate(uint16_t vol)
{
    return attenuation_gain(vol & 0xFFF) / 16384.0f;
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

/* The title's front-end methods (VOICE_OFF and VOICE_PAUSE, from its own
 * thread, outside d->lock) and the frame thread's voice processing write bits
 * of the same voice words. As a plain read-modify-write, one side could put
 * back bits the other had just changed: the frame thread's per-frame
 * NEW_VOICE clear restored an ACTIVE bit that VOICE_OFF had cleared, the
 * voice played on with no idle-voice trap, and DirectSound's buffer teardown
 * spun until its 500 ms watchdog forced the voice off (BLiNX 2 froze for
 * 0.5 s or 1 s at a time, most often in Time Stop, which stops and restarts
 * voices every frame). The MCPX front end and voice processor never lose
 * each other's writes, so the masked bits change by compare-and-swap,
 * without a lock.
 *
 * That makes updates of disjoint bits safe only. A caller that reads a field,
 * computes from it and writes it back (envelope state and counters, CBO, an
 * end-of-buffer voice_off decided from a CBO read earlier) can still lose to
 * the other thread writing the same field in between. */
static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t *word = (uint32_t *)mcpx_apu_phys(voice + offset);
    uint32_t bits = (val << ctz32(mask)) & mask;
    assert(((uintptr_t)word & 3) == 0);
    uint32_t old = qatomic_load32(word);
    for (;;) {
        /* Most writes (the per-frame NEW_VOICE clear above all) change
         * nothing; skip the locked instruction for those. */
        if ((old & mask) == bits)
            return;
        uint32_t seen = qatomic_cmpxchg32(word, old, (old & ~mask) | bits);
        if (seen == old)
            return;
        old = seen;
    }
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* NV1BA0_PIO_VOICE_LOCK, and VOICE_ON/RELEASE around their own writes: a
 * bit flip, with no d->lock.
 *
 * On the MCPX a PIO method is posted into the front end's FIFO (PIO_FREE
 * counts the room left); the CPU's store never waits for the voice
 * processor. xemu takes d->lock here and signals d->cond. Taking the lock
 * made the title wait out the rest of the current se_frame (~80 us, 5% of
 * the time, measured under Proton), and while the frame thread never let go
 * of it, Burnout 3 froze on its vehicle select, blocked here at raised IRQL.
 * So the title side stays a lock-free atomic RMW on the 64-bit word (the
 * title thread and a DPC on the timer thread both write here).
 *
 * The other half of xemu's design is kept on the mix side: the VP holds a
 * frame while a voice it is about to process is locked
 * (vp_hold_locked_voices), so it never mixes a voice the title is
 * mid-update on. An unlock wakes that hold, and only that: the broadcast goes
 * out only while the frame thread is waiting in it, because a signal on
 * every VOICE_LOCK woke the frame thread's pacing wait about 1000 times a
 * second under Proton. It is a broadcast because d->cond can have other
 * waiters (in tests/apu_voice_lock the idle frame thread took every signal
 * on macOS, so the hold never saw one). A wake can be missed: voice_lock does
 * not take d->lock, so it can land after the hold's last check of the bit
 * and before it sleeps. The hold's 1 ms timed wait then sees the bit, so
 * correctness never rests on the wake. */
static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    uint64_t mask = 1ULL << (v % 64);
    assert(v < MCPX_HW_MAX_VOICES);
    if (lock) {
        qatomic_or64(&d->vp.voice_locked[v / 64], mask);
    } else {
        qatomic_and64(&d->vp.voice_locked[v / 64], ~mask);
        if (qatomic_load64(&d->vp.hold_waiting))
            qemu_cond_broadcast(&d->cond);
    }
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    uint64_t mask = 1ULL << (v % 64);
    assert(v < MCPX_HW_MAX_VOICES);
    return (qatomic_load64(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

/* HRIR taps are sign-magnitude with a 3-bit exponent and 4-bit mantissa,
 * from 1/2048 to 31/32 (measured; xemu PR #3105). Read as two's-complement
 * int8 they came out wrong in sign and size for every negative tap. */
static float hrir_tap_to_float(uint8_t tap)
{
    int e = (tap >> 4) & 7;
    int m = tap & 15;
    float v = (e ? (16 + m) << (e - 1) : m) / 2048.0f;
    return (tap & 0x80) ? -v : v;
}

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               uint8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] =
        hrir_tap_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK ||
                   ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK ||
                   ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        /* The entry is read every frame (voice_process), so rewriting it or
         * moving the voice to another takes effect on the next frame. */
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_DELAY:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_LFO_DELAY, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_MOD:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_MOD, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        /* Output buffer handles index the GP FIFO table (GPFADDR, entries 0
         * to GPFMAXSGE inclusive) and share their numbers with the inbuf
         * handles, so writing them to VPSGEADDR overwrote voice pages (JSRF's
         * stream ring lost its first page to its output buffer). The target
         * table is inferred: xemu leaves this method a FIXME in vp.c. GP was
         * chosen over EP on JSRF's evidence: it remaps handles 0-1 with
         * GPFMAXSGE=1 (EPFMAXSGE=5). With the table unset or the handle out
         * of range there is no entry to write, so the write is dropped,
         * reported once in case a title needs the EP table instead. */
        if (!d->regs[NV_PAPU_GPFADDR] ||
            d->vp.outbuf_sge_handle > d->regs[NV_PAPU_GPFMAXSGE]) {
            static int s_dropped_logged;
            if (!s_dropped_logged) {
                s_dropped_logged = 1;
                fprintf(stderr, "[APU] outbuf SGE write dropped: handle %u, "
                        "GPFADDR 0x%08X, GPFMAXSGE %u (reported once)\n",
                        (unsigned)d->vp.outbuf_sge_handle,
                        (unsigned)d->regs[NV_PAPU_GPFADDR],
                        (unsigned)d->regs[NV_PAPU_GPFMAXSGE]);
            }
            break;
        }
        hwaddr sge_address =
            d->regs[NV_PAPU_GPFADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            /* Latched here, once per trap; update_irq no longer re-derives
             * it from FECTL (see there). */
            qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
            d->set_irq = true;
            apu_irq_stats.traps++;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            uint8_t left0 = (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            uint8_t right0 = (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            uint8_t left1 = (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            uint8_t right1 = (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            uint8_t left30 = (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            uint8_t right30 = (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float envelope_level(MCPXAPUState *d, uint16_t v, uint32_t lvl_reg,
                            uint32_t lvl_mask, double units)
{
    int byte = (int)units / 8;
    voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)(byte < 255 ? byte : 255));
    return (float)(units / 2048.0);
}

/* Steps an envelope by one frame and returns its level. xemu's after PR
 * #3105 (measured on hardware there):
 * - Each phase steps before its frame plays; delay and hold last 16T + 1
 *   frames. Attack rises to 2048ths in R * 16 frames.
 * - Decay falls by e^(-1/2R) a frame from full to the sustain level, s/255;
 *   a decay rate of 0 holds one frame at full.
 * - Release holds the level byte (as 256ths) for a frame, falls by
 *   e^(-1/2R) a frame, and from 11R frames runs in a straight line to
 *   nothing at 15R, when the envelope ends.
 * - The F envelope reads 0 while off. */
static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    uint32_t count = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
    uint32_t attack_rate =
        voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
    uint32_t hold_time =
        voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
    uint32_t decay_rate =
        voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
    uint32_t sustain_level =
        voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
    uint32_t release_rate = voice_get_mask(d, v, rr_reg, rr_mask);
    double sustain = floor(sustain_level * 2048.0 / 255.0);
    bool amplitude = count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT;

    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        return envelope_level(d, v, lvl_reg, lvl_mask, amplitude ? 2048 : 0);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY:
        if (count == 0)
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1u);
        else
            count--;
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, v, lvl_reg, lvl_mask, 0);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        count = count + 1 < 0xFFFF ? count + 1 : 0xFFFF;
        double units = 2048;
        if (attack_rate != 0)
            units = fmin(2048, floor(2048.0 * count / (attack_rate * 16)));
        if (attack_rate == 0 || count >= attack_rate * 16) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1u);
            count = hold_time * 16;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, v, lvl_reg, lvl_mask, units);
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD:
        if (count == 0)
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1u);
        else
            count--;
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, v, lvl_reg, lvl_mask, 2048);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        double units = 2048;
        if (decay_rate != 0) {
            count = count + 1 < 0xFFFF ? count + 1 : 0xFFFF;
            units = floor(2048.0 * exp(-(double)count / (2 * decay_rate)));
        }
        if (decay_rate == 0 || units <= sustain) {
            units = decay_rate == 0 ? 2048 : sustain;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1u);
            count = 0;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, v, lvl_reg, lvl_mask, units);
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return (float)(sustain / 2048.0);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        /* The count runs down from R * 16 */
        uint32_t t = release_rate * 16 > count ? release_rate * 16 - count : 0;
        if (release_rate == 0 || t >= release_rate * 15) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1u);
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
            return 0.0f;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask,
                       count ? count - 1 : 0);
        double start = voice_get_mask(d, v, lvl_reg, lvl_mask) * 8.0;
        double fall = floor(start * exp(-(double)t / (2 * release_rate)));
        double tail = floor(start * 2.0 * (release_rate * 15 - t) /
                                (2048.0 * release_rate) + 0.5);
        return (float)((t < release_rate * 11 ? fmax(fall, tail) : tail) / 2048.0);
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (amplitude)
            voice_off(d, v);
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* Steps the two LFOs by one frame (xemu PR #3105, measured): each 16-bit
 * phase advances by its delta a frame once its delay, in frames, has
 * passed, as a bipolar triangle rising first. At full value a unit of depth
 * is 8 attenuation units, 32 pitch units or 256 cutoff units. */
static void voice_step_lfos(MCPXAPUState *d, uint16_t v, float *attenuation,
                            float *pitch, float *cutoff)
{
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];
    if (!filter->lfo_armed) {
        uint32_t misc = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF);
        filter->lfo_wait[0] = (misc & NV_PAVS_VOICE_CFG_MISC_LFOA_DELAY_ENABLE) ?
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_LFO_DELAY,
                                     NV_PAVS_VOICE_PAR_LFO_DELAY_LFOA) : 0;
        filter->lfo_wait[1] = (misc & NV_PAVS_VOICE_CFG_MISC_LFOF_DELAY_ENABLE) ?
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_LFO_DELAY,
                                     NV_PAVS_VOICE_PAR_LFO_DELAY_LFOF) : 0;
        filter->lfo_armed = true;
    }

    const uint32_t delta_mask[2] = { NV_PAVS_VOICE_TAR_LFO_ENV_LFOA_DELTA,
                                     NV_PAVS_VOICE_TAR_LFO_ENV_LFOF_DELTA };
    float value[2] = { 0.0f, 0.0f };
    for (int k = 0; k < 2; k++) {
        if (filter->lfo_wait[k]) {
            filter->lfo_wait[k]--;
            continue;
        }
        filter->lfo_phase[k] = (uint16_t)(filter->lfo_phase[k] +
            voice_get_mask(d, v, NV_PAVS_VOICE_TAR_LFO_ENV, delta_mask[k]));
        int p = filter->lfo_phase[k];
        value[k] = (p < 0x4000 ? p : p < 0xC000 ? 0x8000 - p : p - 0x10000) /
                   16384.0f;
    }

    int8_t a_amplitude = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_LFO_MOD,
                                   NV_PAVS_VOICE_TAR_LFO_MOD_LFOA_AMPLITUDE);
    int8_t a_pitch = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_LFO_MOD,
                                   NV_PAVS_VOICE_TAR_LFO_MOD_LFOA_PITCH);
    int8_t a_cutoff = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_LFO_MOD,
                                   NV_PAVS_VOICE_TAR_LFO_MOD_LFOA_CUTOFF);
    int8_t f_pitch = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_LFO_MOD,
                                   NV_PAVS_VOICE_TAR_LFO_MOD_LFOF_PITCH);
    *attenuation = a_amplitude * 8.0f * value[0];
    *pitch = (a_pitch * value[0] + f_pitch * value[1]) * 32.0f;
    *cutoff = a_cutoff * 256.0f * value[0];
}

/* ============================================================
 * Per-voice source statistics (RECOMP_APU_TRACE)
 *
 * What each voice fed the mixer over the last second: its format word, where
 * its data was read from and how that address resolved, the amplitude
 * envelope and the first two volumes/bins, plus the RMS and zero-crossing
 * rate of the fetched samples. The zero-crossing rate separates music
 * (~0.1-0.2) from white noise (~0.5) without a spectrum, so a voice that
 * reads garbage shows up by itself.
 *
 * RECOMP_APU_VOICE_DUMP=<voice> also writes that voice's fetched samples,
 * before gain, as raw s16 stereo 48 kHz to RECOMP_APU_VOICE_DUMP_FILE
 * (default voice_dump.raw in the working directory).
 * ============================================================ */

/* Trace lines from inside the VP (voice positions, [APU-VSTAT], [APU-RING])
 * are formatted here while the frame thread holds d->lock and written by
 * mcpx_apu_vp_trace_flush after it lets go. Under Wine each stderr write is
 * a slow host call: printed in place, the once-a-second VSTAT burst held the
 * lock ~45 ms and the position bursts 2-8 ms, and every VOICE_LOCK that
 * came in meanwhile waited them out. Only the frame thread
 * writes and flushes, so the buffer needs no lock of its own. A full
 * VSTAT pass over every voice is ~50 KB. */
#define VP_TRACE_BUF (64 * 1024)
static char s_trace_buf[VP_TRACE_BUF];
static size_t s_trace_len;
static unsigned s_trace_dropped;

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void vp_trace(const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room = sizeof(s_trace_buf) - s_trace_len;

    va_start(ap, fmt);
    n = vsnprintf(s_trace_buf + s_trace_len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        s_trace_buf[s_trace_len] = '\0';    /* drop the partial line */
        s_trace_dropped++;
        return;
    }
    s_trace_len += (size_t)n;
}

void mcpx_apu_vp_trace_flush(void)
{
    if (!s_trace_len && !s_trace_dropped)
        return;
    if (s_trace_len)
        fwrite(s_trace_buf, 1, s_trace_len, stderr);
    if (s_trace_dropped)
        fprintf(stderr, "[APU] trace lines dropped: %u\n", s_trace_dropped);
    fflush(stderr);
    s_trace_len = 0;
    s_trace_dropped = 0;
}

struct VpVoiceStat {
    uint32_t frames, zc;
    double   sq;
    float    last, ea, rate;
    uint32_t addr, fmt;
    uint16_t vol[8];
    uint8_t  bin[8];
    const char *cls;
};
static struct VpVoiceStat s_vstat[MCPX_HW_MAX_VOICES];
static int s_vstat_on = -1;
static int s_vdump_voice = -1;      /* -2: every voice, one file each */
static FILE *s_vdump;
static FILE *s_vdump_all[MCPX_HW_MAX_VOICES];
static uint8_t s_vdump_failed[MCPX_HW_MAX_VOICES]; /* open failed: no retry */
static char s_vdump_prefix[512];
static unsigned s_vstat_frames, s_vstat_secs;

static void vp_stat_init(void)
{
    const char *e;
    s_vstat_on = recomp_env_on(RENV_APU_TRACE);
    e = recomp_env(RENV_APU_VOICE_DUMP);
    if (e && *e) {
        const char *path = recomp_env(RENV_APU_VOICE_DUMP_FILE);
        if (!strcmp(e, "all")) {
            /* RECOMP_APU_VOICE_DUMP=all: <file>.<voice>.raw for each voice
             * that renders anything (voice slots are not stable between
             * runs, so a sound cannot be picked by number in advance). */
            s_vdump_voice = -2;
            snprintf(s_vdump_prefix, sizeof(s_vdump_prefix), "%s",
                     path && *path ? path : "voice_dump");
            fprintf(stderr, "[APU] voice dump: every voice -> %s.<voice>.raw\n",
                    s_vdump_prefix);
            return;
        }
        s_vdump_voice = atoi(e);
        s_vdump = fopen(path && *path ? path : "voice_dump.raw", "wb");
        fprintf(stderr, "[APU] voice dump: voice %d -> %s\n", s_vdump_voice,
                s_vdump ? (path && *path ? path : "voice_dump.raw") : "(open failed)");
    }
}

static void vp_stat_samples(uint32_t v, const float samples[][2], int n,
                            hwaddr first_addr, uint32_t fmt)
{
    struct VpVoiceStat *s = &s_vstat[v];
    if (s_vstat_on < 0) vp_stat_init();
    if (!s_vstat_on && (int)v != s_vdump_voice && s_vdump_voice != -2) return;
    if (n <= 0) return;
    if (s->frames == 0) {
        s->addr = (uint32_t)first_addr;
        s->cls = mcpx_apu_phys_data_class(first_addr);
        s->fmt = fmt;
    }
    for (int i = 0; i < n; i++) {
        float x = samples[i][0];
        s->sq += (double)x * x;
        if ((x < 0) != (s->last < 0)) s->zc++;
        s->last = x;
    }
    s->frames += (uint32_t)n;
    if (s_vdump_voice == -2 && !s_vdump_all[v] && !s_vdump_failed[v]) {
        char name[600];
        snprintf(name, sizeof(name), "%s.%u.raw", s_vdump_prefix, (unsigned)v);
        s_vdump_all[v] = fopen(name, "wb");
        s_vdump_failed[v] = s_vdump_all[v] == NULL;
    }
    {
        FILE *df = s_vdump_voice == -2 ? s_vdump_all[v]
                 : (int)v == s_vdump_voice ? s_vdump : NULL;
        int16_t out[NUM_SAMPLES_PER_FRAME * 2];
        int m = n < NUM_SAMPLES_PER_FRAME ? n : NUM_SAMPLES_PER_FRAME;
        if (!df)
            return;
        for (int i = 0; i < m; i++) {
            for (int c = 0; c < 2; c++) {
                float x = samples[i][c] * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                out[2 * i + c] = (int16_t)x;
            }
        }
        fwrite(out, sizeof(int16_t) * 2, (size_t)m, df);
    }
}

static void vp_stat_report(MCPXAPUState *d)
{
    if (s_vstat_on <= 0 && s_vdump_voice == -1) return;
    if (++s_vstat_frames % 1500) return;
    /* Flush the dumps once a second, trace or not: a watchdog _exit or a
     * crash skips the stdio buffers. */
    if (s_vdump) fflush(s_vdump);
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (s_vdump_all[v]) fflush(s_vdump_all[v]);
    if (s_vstat_on <= 0) return;
    s_vstat_secs++;
    /* Totals since init: frames the mix held for a locked voice, holds that
     * ran out VP_HOLD_MAX_US, the longest, and holds an unlock's wake
     * ended inside the first 1 ms wait. */
    vp_trace("[APU-HOLD] t=%us holds=%llu timeouts=%llu max_us=%llu wakes=%llu\n",
             s_vstat_secs, (unsigned long long)d->vp.hold_count,
             (unsigned long long)d->vp.hold_timeouts,
             (unsigned long long)d->vp.hold_max_us,
             (unsigned long long)d->vp.hold_wakes);
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpVoiceStat *s = &s_vstat[v];
        if (!s->frames) continue;
        vp_trace("[APU-VSTAT] t=%us v=%u fmt=%08X stream=%u st=%u ss=%u cs=%u spb=%u"
                " rate=%.3f ea=%.2f vol=%03X/%03X/%03X/%03X/%03X/%03X/%03X/%03X"
                " bin=%u/%u/%u/%u/%u/%u/%u/%u addr=%08X %s"
                " frames=%u rms=%.0f zcr=%.2f\n",
                s_vstat_secs, v, s->fmt,
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK)),
                s->rate, s->ea, s->vol[0], s->vol[1], s->vol[2], s->vol[3],
                s->vol[4], s->vol[5], s->vol[6], s->vol[7],
                s->bin[0], s->bin[1], s->bin[2], s->bin[3],
                s->bin[4], s->bin[5], s->bin[6], s->bin[7], s->addr,
                s->cls ? s->cls : "?", s->frames,
                sqrt(s->sq / s->frames) * 32768.0, (double)s->zc / s->frames);
        s->frames = 0; s->zc = 0; s->sq = 0;
    }
}

/* ============================================================
 * Ring monitor (RECOMP_TRACE=apu_ring)
 *
 * A title that streams compressed audio decodes it on the CPU into a looping
 * PCM buffer voice and keeps writing ahead of the play cursor. If the writer
 * falls behind, the VP plays the previous lap's data again: no digital zero,
 * nothing a check on the mix can see, and only a stutter to the ear.
 *
 * Each looping PCM buffer is split into 1 KB blocks, and the samples the VP
 * fetches are hashed per block as it plays them. When the cursor leaves a
 * block it played whole, that hash is compared with the one from the
 * block's previous play. Once some block of a buffer has played differently
 * on two laps, the buffer is a ring (one loaded once and looped -- a sound
 * effect -- never changes and is never judged). In a ring, a non-silent
 * block that plays exactly as it did one lap before is a stale-lap replay.
 *
 * "lead" is how far ahead of the cursor the writer is: the blocks after the
 * one being entered whose memory has changed since they were last played,
 * in source frames.
 *
 *   [APU-RING] stale v=<voice> blk=<b>/<n> lap=<laps> ms=<wall ms>
 *       first 200 stale-lap replays
 *   [APU-RING] t=<s> v=<voice> bytes=<ring size> entries=<blocks played>
 *       stale=<this second> stale_total=<run> lead_min=<frames> lead_avg=<frames>
 *       once a VP second for each ring that played
 * ============================================================ */

#define RING_BLK      1024u
#define RING_MAX_BLKS 1024u

struct VpRing {
    uint32_t ba, bytes, nblk;
    uint32_t cur;                 /* block being played, ~0: none */
    uint32_t cur_frames;          /* frames of it played so far */
    uint64_t acc;                 /* hash of those frames */
    bool     acc_sound;
    uint32_t laps;
    bool     dynamic;
    uint32_t entries, stale, stale_total;
    uint32_t lead_min, lead_n;
    uint64_t lead_sum;
    uint64_t played[RING_MAX_BLKS];   /* hash of its last whole play */
    uint64_t mem[RING_MAX_BLKS];      /* memory hash when it was last entered */
    uint8_t  seen[RING_MAX_BLKS];     /* bit 0: played, bit 1: mem valid */
};
static struct VpRing *s_ring[MCPX_HW_MAX_VOICES];
static int s_ring_on = -1;
static unsigned s_ring_logged;
static unsigned s_ring_frames, s_ring_secs;

static uint64_t ring_hash_block(MCPXAPUState *d, uint32_t ba, uint32_t off,
                                uint32_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    while (len) {
        uint32_t lin = ba + off;
        uint32_t run = TARGET_PAGE_SIZE - lin % TARGET_PAGE_SIZE;
        if (run > len) run = len;
        const uint8_t *p = mcpx_apu_phys_data(
            get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, lin));
        uint32_t i = 0;
        for (; i + 8 <= run; i += 8) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < run; i++)
            h = (h ^ p[i]) * 0x100000001B3ull;
        off += run;
        len -= run;
    }
    return h;
}

/* The cursor left block r->cur. */
static void ring_close_block(struct VpRing *r, uint32_t v, uint32_t frames_per_blk)
{
    uint32_t b = r->cur;
    if (b == ~0u || r->cur_frames < frames_per_blk)
        return;                              /* not played whole */
    if (r->seen[b] & 1) {
        if (r->acc != r->played[b]) {
            r->dynamic = true;
        } else if (r->dynamic && r->acc_sound) {
            r->stale++;
            r->stale_total++;
            if (s_ring_logged < 200) {
                s_ring_logged++;
                vp_trace("[APU-RING] stale v=%u blk=%u/%u lap=%u ms=%llu\n",
                        v, b, r->nblk, r->laps,
                        (unsigned long long)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) / 1000));
            }
        }
    }
    r->played[b] = r->acc;
    r->seen[b] |= 1;
}

/* The VP fetched n frames of a looping PCM buffer voice, the first at
 * offset first; frames are frame_bytes apart in the buffer. */
static void vp_ring_check(MCPXAPUState *d, uint32_t v, uint32_t ba,
                          uint32_t ebo, uint32_t frame_bytes, uint32_t first,
                          const float samples[][2], int n)
{
    struct VpRing *r;
    uint32_t bytes = (ebo + 1) * frame_bytes;
    uint32_t fpb;

    if (s_ring_on < 0)
        s_ring_on = recomp_env_on(RENV_APU_RING);
    if (!s_ring_on || v >= MCPX_HW_MAX_VOICES || bytes < 2 * RING_BLK ||
        frame_bytes == 0 || RING_BLK % frame_bytes)
        return;
    fpb = RING_BLK / frame_bytes;
    r = s_ring[v];
    if (!r) {
        r = s_ring[v] = calloc(1, sizeof(*r));
        if (!r) return;
        r->ba = ~0u;
    }
    if (r->ba != ba || r->bytes != bytes) {   /* a new buffer on this voice */
        uint32_t keep_total = r->stale_total;
        memset(r, 0, sizeof(*r));
        r->ba = ba;
        r->bytes = bytes;
        r->nblk = bytes / RING_BLK;           /* a partial last block is skipped */
        if (r->nblk > RING_MAX_BLKS) r->nblk = RING_MAX_BLKS;
        r->cur = ~0u;
        r->lead_min = ~0u;
        r->stale_total = keep_total;
    }

    for (int i = 0; i < n; i++) {
        uint32_t b = (first + (uint32_t)i) / fpb;
        if (b != r->cur) {
            ring_close_block(r, v, fpb);
            if (b < r->cur && r->cur != ~0u)
                r->laps++;
            r->cur = b;
            r->cur_frames = 0;
            r->acc = 0x9E3779B97F4A7C15ull;
            r->acc_sound = false;
            if (b < r->nblk) {
                /* Entering b: the writer's lead, then b's memory hash. */
                uint64_t h = ring_hash_block(d, ba, b * RING_BLK, RING_BLK);
                r->entries++;
                if (r->dynamic) {
                    uint32_t k, lead = 0;
                    for (k = 1; k < r->nblk; k++) {
                        uint32_t bb = (b + k) % r->nblk;
                        if (!(r->seen[bb] & 2))
                            break;
                        if (ring_hash_block(d, ba, bb * RING_BLK, RING_BLK) == r->mem[bb])
                            break;
                        lead++;
                    }
                    lead *= fpb;
                    if (lead < r->lead_min) r->lead_min = lead;
                    r->lead_sum += lead;
                    r->lead_n++;
                }
                r->mem[b] = h;
                r->seen[b] |= 2;
            }
        }
        if (b >= r->nblk)
            continue;
        {
            uint32_t w[2];
            memcpy(w, samples[i], sizeof(w));
            r->acc = (r->acc ^ (((uint64_t)w[0] << 32) | w[1])) * 0x100000001B3ull;
            r->acc ^= r->acc >> 29;
            if (w[0] | w[1]) r->acc_sound = true;
            r->cur_frames++;
        }
    }
}

static void vp_ring_report(unsigned secs)
{
    if (s_ring_on <= 0) return;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpRing *r = s_ring[v];
        if (!r || !r->entries || !r->dynamic) {
            if (r) r->entries = 0;
            continue;
        }
        vp_trace("[APU-RING] t=%us v=%u bytes=%u entries=%u stale=%u"
                " stale_total=%u lead_min=%u lead_avg=%llu\n",
                secs, v, r->bytes, r->entries, r->stale, r->stale_total,
                r->lead_n ? r->lead_min : 0,
                (unsigned long long)(r->lead_n ? r->lead_sum / r->lead_n : 0));
        r->entries = 0;
        r->stale = 0;
        r->lead_min = ~0u;
        r->lead_sum = 0;
        r->lead_n = 0;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    /* Room for the largest block the MCPX accepts (see the check below). */
    uint32_t adpcm_block[36 * ADPCM_MAX_BLOCKS / 4];
    int16_t adpcm_decoded[65 * ADPCM_MAX_BLOCKS * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        /* The block is 36 bytes per channel times the descriptor's 5-bit
         * samples-per-block field, read into the fixed buffers above. A
         * value past what they hold overran the stack. */
        if (samples_per_block > ADPCM_MAX_BLOCKS) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "[APU] voice %u: ADPCM samples per block %u "
                        "is above %d; voice skipped\n",
                        v, samples_per_block, ADPCM_MAX_BLOCKS);
            return -1;
        }
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    hwaddr stat_addr = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (sample_count == 0 && s_vstat_on != 0) {
            uint32_t lin = adpcm ? (cbo / ADPCM_SAMPLES_PER_BLOCK) * (uint32_t)block_size
                                 : cbo * (uint32_t)block_size;
            stat_addr = stream ? segment_offset + lin
                               : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                              0xFFFFFFFF, ba + lin);
        }
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys_data(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_data(addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_data(addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_data(addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_data(addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_data(addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    vp_stat_samples(v, samples, sample_count, stat_addr,
                    voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                   0xFFFFFFFF));
    if (!stream && loop && !adpcm && sample_count > 0)
        vp_ring_check(d, v, ba, ebo, (uint32_t)block_size,
                      cbo - (uint32_t)sample_count, samples, sample_count);

    /* EBO is the last sample (inclusive): the fetch loop reads while
     * cbo <= ebo, a stream segment sets it to its length - 1, and the ring
     * check spans ebo + 1 samples. So the voice wraps, ends or moves to the
     * next segment only once sample ebo has been read. Testing cbo >= ebo
     * here dropped sample ebo whenever a fetch stopped just before it. */
    if (cbo > ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling: xemu's 8-point Lagrange interpolator (PR #3105)
 *
 * Hardware resamples with 8-point Lagrange interpolation, each weight
 * rounded to 1/65536: at half a sample the weights are 1225, -245, 49 and
 * -5 / 2048 each side, and eighths of a sample match to the bit (measured
 * there). Between 256ths of a sample the weights are interpolated (assumed).
 *
 * This replaces the port's linear interpolator. Two things differ from
 * xemu's, both about when source is read, not what is played:
 *
 * - xemu's callback fetches a whole frame (32 source samples) whenever its
 *   buffer runs out, so a voice reads up to a frame ahead of what it has
 *   played. Here a frame reads exactly the source its 32 outputs consume
 *   (the sum of their steps), in pieces of up to 32. A one-shot that ends
 *   is then still heard to its last sample in the frame its fetch ended
 *   (up to 4 samples can fall past that frame), and a looping buffer
 *   rewritten each frame (Burnout 3's effect returns from the GP) is read
 *   in the frame it was written.
 * - xemu starts the cursor 5 samples in, so its first output is source
 *   sample 0 with samples 1-4 already read as look-ahead taps. Read at the
 *   end of one frame, those taps belong to the next frame's buffer, and
 *   at rate 1 the next frame would open with 4 samples of the old one.
 *   Here the cursor starts 1 in: the output runs 4 samples behind the
 *   source, which is the same filter delayed, and each frame reads only
 *   that frame's source.
 *
 * A voice that runs dry plays silence for the rest of the frame (xemu's
 * "starvation" fill).
 * ============================================================ */

static float lagrange_rows[257][8];

static void lagrange_init(void)
{
    for (int r = 0; r <= 256; r++) {
        for (int k = -3; k <= 4; k++) {
            double w = 1.0;
            for (int j = -3; j <= 4; j++)
                if (j != k)
                    w *= (r / 256.0 - j) / (k - j);
            lagrange_rows[r][k + 3] = (float)(round(w * 65536.0) / 65536.0);
        }
    }
}

/* Up to `want` (at most a frame) of source into the voice's resample_buf,
 * silence past the end.
 * An active voice that keeps returning nothing is an underrun, not a reason
 * to spin: this runs with d->lock held, and asking again cannot change the
 * answer (Burnout 3's vehicle select sat in such a loop while the title
 * waited in voice_lock). */
static void voice_resample_fill(MCPXAPUState *d, uint16_t v, int want)
{
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];
    int sample_count = 0, empty = 0;
    if (want > NUM_SAMPLES_PER_FRAME)
        want = NUM_SAMPLES_PER_FRAME;
    filter->resample_buf_len = want;
    while (sample_count < want) {
        if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                            NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            break;
        int count = voice_get_samples(
            d, v, (float (*)[2])&filter->resample_buf[2 * sample_count],
            want - sample_count);
        if (count < 0)
            break;
        if (count == 0 && ++empty > 4)
            break;
        sample_count += count;
    }
    if (sample_count < want)
        memset(&filter->resample_buf[2 * sample_count], 0,
               2 * (want - sample_count) * sizeof(float));
}

/* steps are the cursor's advance per output sample in 16.16 */
static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, const uint32_t *steps)
{
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];
    filter->voice = v;

    /* The source these outputs consume: each output first takes the whole
     * samples its cursor has passed. Less what resample_buf still holds. */
    int64_t need = 0;
    uint32_t pos = filter->interp_pos;
    for (int n = 0; n < requested_num; n++) {
        need += pos >> 16;
        pos = (pos & 0xFFFF) + steps[n];
    }
    need -= filter->resample_buf_len - filter->resample_buf_pos;

    for (int n = 0; n < requested_num; n++) {
        while (filter->interp_pos >= 0x10000) {
            if (filter->resample_buf_pos >= filter->resample_buf_len) {
                int want = need > NUM_SAMPLES_PER_FRAME ? NUM_SAMPLES_PER_FRAME
                                                        : (int)need;
                voice_resample_fill(d, v, want > 0 ? want : 1);
                need -= filter->resample_buf_len;
                filter->resample_buf_pos = 0;
            }
            memmove(filter->interp[0], filter->interp[1],
                    sizeof(filter->interp[0]) * 7);
            filter->interp[7][0] =
                filter->resample_buf[2 * filter->resample_buf_pos];
            filter->interp[7][1] =
                filter->resample_buf[2 * filter->resample_buf_pos + 1];
            filter->resample_buf_pos++;
            filter->interp_pos -= 0x10000;
        }
        uint32_t frac = filter->interp_pos;
        const float *lo = lagrange_rows[frac >> 8];
        const float *hi = lagrange_rows[(frac >> 8) + 1];
        float a = (frac & 0xFF) / 256.0f;
        for (int ch = 0; ch < 2; ch++) {
            float acc = filter->interp[3][ch];
            if (frac) {
                acc = 0.0f;
                for (int k = 0; k < 8; k++)
                    acc += (lo[k] + (hi[k] - lo[k]) * a) * filter->interp[k][ch];
            }
            samples[n][ch] = acc;
        }
        filter->interp_pos += steps[n];
    }
    return requested_num;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

/* A multipass voice's source: its bin, read as 24-bit samples and scaled up
 * by 2^(format headroom), undoing the bin headroom its voices were mixed
 * with (xemu PR #3105, measured). */
static void get_multipass_samples(MCPXAPUState *d,
                                  float mixbins[][NUM_SAMPLES_PER_FRAME],
                                  uint16_t v, float samples[][2])
{
    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
    dbg->multipass_bin = (uint8_t)mp_bin;
    int headroom = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_HEADROOM);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float x = floorf(mixbins[mp_bin][i] * 8388608.0f + 0.5f);
        x = fminf(fmaxf(x, -8388608.0f), 8388607.0f) / 8388608.0f;
        samples[i][0] = ldexpf(x, headroom);
        samples[i][1] = samples[i][0];
    }
    if (voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                       NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX))
        memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
}

/* One voice's frame, as xemu's voice_process after PR #3105 ("MCPX
 * improvements", measured on hardware there): LFOs, pitch ramped across the
 * frame, Lagrange resampling, 3D voices as mono, DLS2/ParaEQ/I3DL2 filters,
 * HRTF ears on a 3D voice's first four routes and integer route gains
 * ramped from the last frame's. Kept from the port: a voice that ended
 * during its fetch still mixes the frame it fetched, the RECOMP_APU_TRACE
 * statistics and RECOMP_APU_HRTF. */
static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    ef_value = fminf(fmaxf(ef_value, 0.0f), 1.0f);

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float lfo_attenuation, lfo_pitch, lfo_cutoff;
    voice_step_lfos(d, v, &lfo_attenuation, &lfo_pitch, &lfo_cutoff);
    double pitch = p + ps * 32.0 * ef_value + lfo_pitch;
    float rate = 1.0f / powf(2.0f, (float)(pitch / 4096.0));
    dbg->rate = rate;

    /* The pitch moves from the last frame's to this frame's across the frame
     * in its log form (measured: pitch 0 to -1000h advanced the cursor 23
     * samples on that frame, the sum of 2^(-n/32)). */
    double pitch_from = filter->pitched ? filter->pitch : pitch;
    uint32_t steps[NUM_SAMPLES_PER_FRAME];
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        double p_i = pitch_from + (pitch - pitch_from) * i / 32.0;
        steps[i] = (uint32_t)(exp2(p_i / 4096.0) * 65536.0);
    }
    filter->pitch = (float)pitch;
    filter->pitched = true;

    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    ea_value = fminf(fmaxf(ea_value, 0.0f), 1.0f);

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        get_multipass_samples(d, mixbins, v, samples);
    } else {
        if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                            NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            return;
        /* Always a whole frame: a voice that runs dry mid-frame plays
         * silence for the rest of it. A voice that ended during the fetch (a
         * one-shot past EBO, a non-persist stream out of packets) still
         * mixes the frame it fetched; the frames after are not mixed. */
        voice_resample(d, v, samples, NUM_SAMPLES_PER_FRAME, steps);
    }

    /* A 3D voice plays its channel 0 alone, as a mono voice (measured). */
    if (v < MCPX_HW_MAX_3D_VOICES && stereo) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            samples[i][1] = samples[i][0];
        stereo = false;
        channels = 1;
    }

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }
    if (s_vstat_on > 0) {
        s_vstat[v].ea = ea_value;
        s_vstat[v].rate = rate;
        for (int i = 0; i < 8; i++) {
            s_vstat[v].vol[i] = vol[i];
            s_vstat[v].bin[i] = (uint8_t)bin[i];
        }
    }

    if (voice_should_mute(v)) return;

    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);

    /* RECOMP_APU_HRTF=0 turns the HRTF off (the ears then carry the dry
     * voice): on by default, as in xemu. */
    static int hrtf_on = -1;
    if (hrtf_on < 0) {
        const char *e = recomp_env(RENV_APU_HRTF);
        hrtf_on = !(e && e[0] == '0');
    }

    bool three_d = v < MCPX_HW_MAX_3D_VOICES;
    uint16_t hrtf_handle = HRTF_NULL_HANDLE;
    if (three_d)
        hrtf_handle = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                               NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
    /* A 3D voice without an HRTF entry filters as a 2D voice, with no I3DL2
     * (measured). */
    bool spatial = three_d && hrtf_handle != HRTF_NULL_HANDLE;
    bool lpf, peq;
    if (spatial) {
        /* 1:DLS2+I3DL2 2:ParaEQ+I3DL2 3:I3DL2 */
        lpf = (fmode == 1);
        peq = (fmode == 2);
    } else {
        /* 0:Bypass 1:DLS2 2:ParaEQ 3(Mono):DLS2+ParaEQ 3(Stereo):Bypass */
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
        peq = stereo ? (fmode == 2) : (fmode >= 2);
    }
    /* The F envelope bends both filters' frequencies by its FC scale, 256
     * units a step at full. Each coefficient moves from the last frame's to
     * this frame's across the frame, linearly in f (measured). */
    int8_t fc_scale = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV1,
                                             NV_PAVS_VOICE_CFG_ENV1_EF_FCSCALE);
    double fc_bend = fc_scale * 256.0 * ef_value + lfo_cutoff;

    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = filter_coefficient(fc + fc_bend);
            float f_from = filter->dls2_set ? filter->dls2_f[ch] : fc_f;
            filter->dls2_f[ch] = fc_f;
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = q / (1.0f * 0x8000);
            sv_filter *svf = &filter->svf[ch];
            setup_svf(svf, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                svf->f = f_from + (fc_f - f_from) * (i / 32.0f);
                samples[i][ch] = run_svf(svf, samples[i][ch]);
            }
        }
    }
    filter->dls2_set = lpf;

    /* ParaEQ (measured on hardware): the DLS2 filter's SVF as a peaking EQ,
     * y = x + G * q * band, with f = 2^(FC / 4096), G = gain / 8192 and
     * q = 2^-Q. Its word is FCB on mono 2D voices, FCA on 3D voices and
     * FCA/FCB per channel on stereo voices. */
    if (peq) {
        int q_field = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                                     NV_PAVS_VOICE_CFG_MISC_PARAEQ_Q);
        float q_f = ldexpf(1.0f, -q_field);
        for (int ch = 0; ch < 2; ch++) {
            hwaddr reg = spatial ? NV_PAVS_VOICE_TAR_FCA :
                         stereo  ? NV_PAVS_VOICE_TAR_FCA + ch * 4 :
                                   NV_PAVS_VOICE_TAR_FCB;
            uint32_t c = voice_get_mask(d, v, reg, 0xFFFFFFFF);
            float fc_f = filter_coefficient((int16_t)(c & 0xFFFF) + fc_bend);
            float f_from = filter->peq_set ? filter->peq_f[ch] : fc_f;
            filter->peq_f[ch] = fc_f;
            float g = (int16_t)(c >> 16) / 8192.0f * q_f;
            sv_filter *svf = &filter->peq[ch];
            setup_svf(svf, fc_f, q_f, F_BP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                svf->f = f_from + (fc_f - f_from) * (i / 32.0f);
                samples[i][ch] = svf_round(samples[i][ch] +
                                           g * run_svf(svf, samples[i][ch]));
            }
        }
    }
    filter->peq_set = peq;

    /* I3DL2 (measured on hardware): one-pole low-passes y = (1 - a) x + a y'
     * with a = FCB 15:0 / 65536 on routes 0-4 and 6, and FCB 31:16 / 65536
     * on routes 5 and 7. */
    float room[NUM_SAMPLES_PER_FRAME];
    bool i3dl2 = spatial && fmode != 0 && !stereo;
    if (i3dl2) {
        uint32_t fcb = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF);
        float a_direct = (fcb & 0xFFFF) / 65536.0f;
        float a_room = (fcb >> 16) / 65536.0f;
        float *state = filter->i3dl2;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float x = samples[i][0];
            state[0] = svf_round(x + (state[0] - x) * a_direct);
            state[1] = svf_round(x + (state[1] - x) * a_room);
            samples[i][0] = samples[i][1] = state[0];
            room[i] = state[1];
        }
    }

    /* Routes 0-1 of a 3D voice carry the HRTF's left ear and 2-3 its right,
     * and are silent without an HRTF entry; its other routes carry the voice
     * itself (measured on hardware). The port fed a 3D voice's own channels
     * to those routes, so every 3D voice reached both "ears" unfiltered
     * whatever its position, and without an entry it was heard where the
     * hardware is silent. */
    float ears[NUM_SAMPLES_PER_FRAME][2];
    if (three_d) {
        if (spatial && g_config.audio.hrtf && hrtf_on) {
            /* The entry is read every frame, so rewriting it or moving the
             * voice to another entry takes effect on the next frame. */
            assert(hrtf_handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&filter->hrtf,
                                          d->vp.hrtf.entries[hrtf_handle].hrir,
                                          d->vp.hrtf.entries[hrtf_handle].itd);
            hrtf_filter_process(&filter->hrtf, samples, ears);
        } else {
            memcpy(ears, samples, sizeof(ears));
        }
    }

    /* A route's gain, in 1/16384, is its attenuation's times the envelope's
     * level in 2048ths, rounded down. It moves from where the last frame
     * ended to this frame's across the frame, each sample's rounded down, and
     * a new voice fades in from silence (measured). The bin's headroom
     * applies to HRTF submixes too; the HRTF headroom has no measurable
     * effect on hardware. */
    uint32_t level = (uint32_t)lrintf(ea_value * 2048.0f);
    for (int b = 0; b < 8; b++) {
        if (three_d && b < 4 && hrtf_handle == HRTF_NULL_HANDLE) {
            filter->route_gain[b] = 0.0f;
            continue;
        }
        float from = filter->gains ? filter->route_gain[b] : 0.0f;
        float to = 0.0f;
        if (vol[b] != 0xFFF) {
            int att = vol[b] + (int)floorf(lfo_attenuation);
            to = (float)((attenuation_gain(att) * level) >> 11);
        }
        filter->route_gain[b] = to;
        if (from == 0.0f && to == 0.0f)
            continue;
        float scale = 1.0f / (float)(16384 << d->vp.submix_headroom[bin[b]]);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float g = floorf(from + (to - from) * (i / 32.0f)) * scale;
            float x = (three_d && b < 4)            ? ears[i][b / 2] :
                      (i3dl2 && (b == 5 || b == 7)) ? room[i] :
                                                      samples[i][b % channels];
            mixbins[bin[b]][i] += x * g;
        }
    }
    filter->gains = true;

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

/* RECOMP_APU_TRACE: every 60 VP frames, each active voice's play offset and
 * end offset, in samples (CBO and EBO as the VP holds them). A title that
 * times video or game events off a voice's play position follows this
 * clock; set against the flip count it shows whether audio runs fast or
 * slow relative to the frames. */
static unsigned s_pos_frames;

static void vp_trace_positions(MCPXAPUState *d)
{
    if (s_vstat_on < 0)
        vp_stat_init();
    if (s_vstat_on <= 0 || ++s_pos_frames % 60)
        return;

    for (int list = 0; list < 3; list++) {
        uint32_t h = d->regs[voice_list_regs[list].top];
        for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            uint16_t v = (uint16_t)h;
            if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                               NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                vp_trace("[APU] voice %u pos=%u/%u\n", (unsigned)v,
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                                       NV_PAVS_VOICE_PAR_OFFSET_CBO),
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                       NV_PAVS_VOICE_PAR_NEXT_EBO));
            h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
}

/* The longest the mix holds a frame for locked voices: 2 ms, three VP
 * frames, plus however late the scheduler runs the frame thread again. A
 * title updates a voice with a handful of PIO stores between
 * VOICE_LOCK(1) and VOICE_LOCK(0), microseconds when its thread runs, so
 * the bound only matters when that thread is preempted mid-update or never
 * unlocks. One 1 ms poll, as in xemu, followed by yields up to the bound (a
 * second poll would cross it), lets an update that straddles the poll
 * finish. 2 ms is well under the device queue's low mark (half the device
 * buffers, tens of ms), so a held frame is absorbed by the pacing slack and
 * cannot cause an underrun on its own. Past the bound the frame mixes the
 * voice as it stands, which is what the port did before the hold. xemu has
 * no bound: there a voice left locked stops the whole mix. */
#define VP_HOLD_MAX_US 2000

/* xemu's voice_work_dispatch wait (hw/xbox/mcpx/apu/vp/vp.c at 36334d9,
 * lines 1721-1738): no voice queued for this frame is processed while the
 * title has it locked, so a voice is never mixed half-updated (new pitch
 * with the old buffer, say). Called with d->lock held, which the wait and
 * the yields drop, so nothing that takes the lock waits behind the hold.
 * The title side never waits at all (voice_lock).
 *
 * A voice still locked when the bound runs out is exempt from the hold until
 * the title unlocks it: a title that leaves a voice locked costs one bounded
 * wait, not 2 ms on every frame after it (which would run the mix at a third
 * of real time). */
static void vp_hold_locked_voices(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    uint64_t queued[4] = { 0 }, held[4];
    int64_t start_us = 0, now_us;
    bool any, woke = false;
    int waits = 0;

    for (int i = 0; i < vwd->queue_len; i++) {
        unsigned v = (unsigned)vwd->queue[i].voice;
        queued[v / 64] |= 1ULL << (v % 64);
    }

    for (;;) {
        any = false;
        for (int w = 0; w < 4; w++) {
            uint64_t locked = qatomic_load64(&d->vp.voice_locked[w]);
            d->vp.hold_exempt[w] &= locked;
            held[w] = locked & queued[w] & ~d->vp.hold_exempt[w];
            any |= held[w] != 0;
        }
        if (!any) {
            if (woke)
                d->vp.hold_wakes++;
            break;
        }
        if (!start_us) {
            start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            /* Set before the next check of the bits, so an unlock either is
             * seen by that check or sees hold_waiting and broadcasts. */
            qatomic_or64(&d->vp.hold_waiting, 1);
            continue;
        }
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (now_us - start_us >= VP_HOLD_MAX_US) {
            for (int w = 0; w < 4; w++)
                d->vp.hold_exempt[w] |= held[w];
            d->vp.hold_timeouts++;
            break;
        }
        if (now_us - start_us + 1000 > VP_HOLD_MAX_US) {
            /* A 1 ms wait now would end past the bound (on a loaded Mac
             * holds reached 3.9 ms when the bound was checked only after
             * each wait): yield with the lock dropped for the rest of it.
             * When nothing else can run, the yield returns at once, so the
             * tail of the bound spins on unlock/lock; with lockstat on,
             * each turn adds a FRAME wait sample. */
            woke = false;
            apu_lockstat_unlock(d, APU_LOCK_FRAME);
            SwitchToThread();
            apu_lockstat_lock(d, APU_LOCK_FRAME);
        } else {
            /* A first wait cut short usually means voice_lock's broadcast
             * ended it: hold_wakes counts those, so a test can tell the
             * wake works and not just the poll (apu_state.h lists what
             * else can end the wait early). */
            bool first = waits++ == 0;
            apu_lockstat_hold_end(APU_LOCK_FRAME);
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
            apu_lockstat_hold_begin(APU_LOCK_FRAME);
            woke = first && qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us < 900;
        }
    }

    if (start_us) {
        uint64_t us = (uint64_t)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us);
        qatomic_and64(&d->vp.hold_waiting, 0);
        d->vp.hold_count++;
        if (us > d->vp.hold_max_us)
            d->vp.hold_max_us = us;
    }
}

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    /* Walk the lists first and process after, as xemu does, so the hold can
     * see every voice this frame will touch before it touches any. */
    vwd->queue_len = 0;
    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                /* One trap at a time: once the front end is trapped, a second
                 * idle voice in the same frame would overwrite FEDECPARAM
                 * before the ISR has read the first. The rest trap on later
                 * frames, after the ISR has resumed the front end. */
                if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                        != NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                    fe_method(d, SE2FE_IDLE_VOICE, v);
            } else if (vwd->queue_len < MCPX_HW_MAX_VOICES) {
                vwd->queue[vwd->queue_len].voice = v;
                vwd->queue[vwd->queue_len].list = list;
                vwd->queue_len++;
            } else {
                static int queue_full_warned;
                if (!queue_full_warned) {
                    queue_full_warned = 1;
                    DPRINTF("Voice work queue full, voice %u not mixed\n", v);
                }
            }
            d->regs[current] = d->regs[next];
        }
    }

    vp_hold_locked_voices(d);

    /* Process voices directly (single-threaded) */
    for (int i = 0; i < vwd->queue_len; i++)
        voice_process(d, mixbins, d->vp.sample_buf,
                      (uint16_t)vwd->queue[i].voice, vwd->queue[i].list);
    vwd->queue_len = 0;

    vp_trace_positions(d);
    vp_stat_report(d);
    if (s_ring_on > 0 && ++s_ring_frames % 1500 == 0)
        vp_ring_report(++s_ring_secs);

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
    lagrange_init();
    attenuation_init();
}

/* Shutdown, after the frame thread has stopped: close the voice dumps, free
 * the ring monitors and clear the statistics, so a new instance reads the
 * switches again and starts from nothing. */
void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
    if (s_vdump)
        fclose(s_vdump);
    s_vdump = NULL;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (s_vdump_all[v])
            fclose(s_vdump_all[v]);
        s_vdump_all[v] = NULL;
        free(s_ring[v]);
        s_ring[v] = NULL;
    }
    memset(s_vdump_failed, 0, sizeof(s_vdump_failed));
    memset(s_vstat, 0, sizeof(s_vstat));
    s_vstat_on = -1;
    s_vdump_voice = -1;
    s_vstat_frames = s_vstat_secs = s_pos_frames = 0;
    s_ring_on = -1;
    s_ring_logged = 0;
    s_ring_frames = s_ring_secs = 0;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    memset(d->vp.hold_exempt, 0, sizeof(d->vp.hold_exempt));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
