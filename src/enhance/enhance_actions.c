/*
 * enhance_actions.c - see enhance_actions.h. The rules (docs/runtime/
 * config.md, "[input.actions]"), so a binding never steals what the title
 * uses:
 *
 *   1. a single source the pad map gives the game is refused; sources the
 *      game uses only in a chord of two or more with a hold of at least
 *      200 ms;
 *   2. a key the guest receives (the runtime key table, the walk key, the
 *      mouse release key; modifiers count) is refused;
 *   3. a chord is consumed: from the moment all its buttons are down the
 *      guest sees them released until each is physically released, and
 *      input.chord_delay_ms holds a chord member back from the guest for
 *      that long after its press;
 *   5. actions read the host pad, never the state a script made;
 *   6. only feature keys arm the hook;
 *   7. a modifier chord (guide+a) needs its unmapped members first; a
 *      mapped member pressed while they are held is the guest's no more
 *      until its release, even when a modifier is let go first; one pressed
 *      before them passes and does not complete the chord.
 *
 * Bindings name host sources (input_map.h's names; a guest control's name
 * means the source the identity gives it), read from the raw host pad
 * before [input.pad], so a remap never moves a binding (gamepad-input-ux
 * D4). Which members the game uses depends on the map, so a pad binding's
 * kind is derived from it at every rebuild: host-only, a guest chord, a
 * modifier chord (its unmapped members the modifiers), or inactive with a
 * warning. Masking clears the guest controls a consumed source feeds
 * through either map, and centres the half of a guest stick axis a
 * consumed stick direction feeds.
 *
 * Threads: the poll thread runs the filter, the ack thread the flip tick,
 * any thread may rebuild. One poll thread per title is assumed (cat polls
 * under its LOCK(), input_core from the guest's thread): keyboard.c's live
 * table swap relies on it too. One spin lock covers the bindings table and
 * the per-port state, held only for a step and never across backend I/O:
 * the tick reads its host pads between two holds, and only tries the lock,
 * so the flip never waits on input. A rebuild builds its table outside the
 * lock and swaps the pointer under it, so the old table is never freed
 * under a step. Nothing called under the lock may take it again (it is not
 * recursive): enhance_actions_mask_until_release is for a menu or a mod's
 * callback, never from inside the filter.
 */
#include "enhance_actions.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_cfg.h"
#include "input_names.h"
#include "keyboard.h"
#include "pad_map.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#ifdef XBOXRECOMP_ENHANCE
#include "nv2a_overlay.h"
#endif
#include "recomp_env.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define spin_yield() SwitchToThread()
#else
#include <sched.h>
#define spin_yield() sched_yield()
#endif

#define PREFIX        "input.actions."
#define NAME_MAX_     48
#define TEXT_MAX      48
#define PORTS         4
#define HOLD_MAX_MS   2000
#define CHORD_MIN_MS  200
#define DELAY_MAX_MS  100
#define STALE_MS      100      /* the tick reads a port not polled for this long */
#define ANALOG_BIT0   8
/* A guest stick's half (up, down, left, right; the left stick first): a
 * consumed stick direction has no button to clear, so its half of the
 * axis is centred instead. */
#define STICK_BIT0    16
#define GUEST_BITS    0xFFFFFFu
#define SRC_BITS      32       /* input_map.h's sources, bit i = source i */
#define ARM_KEYS      8
#define LOG_PER_SEC   10

/* ---- locks ------------------------------------------------------------- */

static int s_step_lock;     /* the table pointer and the per-port state */
static int s_reg_lock;      /* the registry and a rebuild */

static void spin_lock(int *l)
{
    unsigned spins = 0;

    /* Held for a step, so a spin is short; but the holder may be preempted,
     * and on a busy host (Proton's threads plus the guest's) a waiter that
     * never yields can keep it off the core (input_core's last_lock). */
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(l, __ATOMIC_RELAXED))
            if (++spins >= 64) {
                spins = 0;
                spin_yield();
            }
}

static int spin_try(int *l)
{
    return !__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE);
}

static void spin_unlock(int *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

/* ---- the registry ------------------------------------------------------ */

struct action {
    char name[NAME_MAX_];
    char label[64];
    char defs[ENHANCE_ACTION_BINDS][TEXT_MAX];
    int  ndefs;
    int  owner;             /* mod table index, -1 the toolkit */
    int  live;              /* 0: a dropped mod's slot, never reused (ids stay) */
    int  pressed;           /* atomic: fires not yet taken */
    uint32_t down;          /* atomic: bit per binding held past its hold */
};

static struct action s_act[ENHANCE_ACTIONS_MAX];
static int s_nact;
static int s_started;

/* ---- bindings ---------------------------------------------------------- */

/* B_PAD: parsed, kind not derived yet; B_OFF: inactive under this map. */
enum { B_KEY, B_CHORD, B_HOST, B_MOD, B_PAD, B_OFF };

_Static_assert(INPUT_SRC_COUNT <= SRC_BITS, "a source bit each");

struct binding {
    int action, idx;        /* idx: the bit in s_act[action].down */
    int kind;
    unsigned port;
    uint32_t bits;          /* every pad member: bit per source */
    uint32_t guest;         /* the members the map gives the game */
    uint32_t modmask;       /* B_MOD: the members it does not (the modifiers) */
    int hold_ms, has_hold;
    int key;                /* B_KEY */
    int mods[3], nmods;     /* B_KEY: modifier codes, as written */
    char text[TEXT_MAX];
    /* The action's name when the table was built, for the fire log: the
     * registry is s_reg_lock's, and a dropped action's trailing slot may be
     * reused before the rebuild that follows the drop swaps this table out.
     * Until then a fire may count on the reused slot; the next step on the
     * new table no longer does. */
    char name[NAME_MAX_];
    /* state, under the step lock */
    int active, fired;
    int held_over;          /* held when the table was built: no fire, not down */
    uint64_t since;
    uint32_t modpress;      /* B_MOD: guest members pressed with the modifiers held */
};

struct table {
    int n;
    int delay_ms;
    uint32_t feeds[SRC_BITS];       /* per source, the guest bits it feeds */
    uint32_t chord_members[PORTS];  /* sources of guest chords, for chord_delay_ms */
    unsigned ports;                 /* bit per port a pad binding reads */
    int keys;                       /* any B_KEY */
    struct binding b[];
};

struct port_state {
    uint32_t held, consumed;    /* bit per source */
    uint64_t press_ms[32];
    uint64_t last_poll;
    int polled;
    int slot;               /* the backend slot behind the port, -1 unknown */
};

static struct table *s_table;           /* under s_step_lock */
static struct port_state s_port[PORTS]; /* under s_step_lock */
static int s_delay_ms;

/* ---- arming ------------------------------------------------------------ */

static int s_armed;
static char s_arm_keys[ARM_KEYS][NAME_MAX_];
static int s_arm_owner[ARM_KEYS];   /* the mod that armed it, -1 the toolkit */
static int s_narm;

static void disarm_where(int all_mods, int owner);

/* ---- the fire log, at most LOG_PER_SEC lines a second ------------------- */

static uint64_t s_log_sec;
static int s_log_n, s_log_dropped;

/* The toolkit's actions that act at the fire (menu.open toasts until the
 * menu, 3D, takes it over). Fires run under s_step_lock, so the strip's
 * read-then-set never races another fire. */
static int s_stats_act = -1;
static int s_menu_act = -1;

static void toolkit_fire(int action)
{
#ifdef XBOXRECOMP_ENHANCE
    if (action == s_stats_act)
        nv2a_overlay_set_stats(!nv2a_overlay_stats());
    /* Until the menu lands, a toast shows that the binding reached us: on
     * the Deck a grip goes through Steam Input to a key, and a log line
     * alone cannot be checked from the couch. Fires only while armed. */
    else if (action == s_menu_act)
        nv2a_overlay_toast("Menu: not in this build yet");
#else
    (void)action;
#endif
}

static void fire_log(const struct binding *b, uint64_t now)
{
    uint64_t sec = now / 1000;
    if (sec != s_log_sec) {
        if (s_log_dropped)
            fprintf(stderr, "[ACTION] %d more fires not logged\n", s_log_dropped);
        s_log_sec = sec;
        s_log_n = s_log_dropped = 0;
    }
    if (s_log_n++ >= LOG_PER_SEC) {
        s_log_dropped++;
        return;
    }
    fprintf(stderr, "[ACTION] %s via %s\n", b->name, b->text);
}

/* ---- names ------------------------------------------------------------- */

/* Control index to its bit, in upstream's InputControl order: the first
 * eight are the wButtons bits in bit order, A..right_trigger the analog
 * buttons in the XBOX_BUTTON_* order (Black before White). A stick
 * direction has none. */
static uint32_t control_bit(int ctl)
{
    if (ctl >= INPUT_DPAD_UP && ctl <= INPUT_RIGHT_THUMB)
        return 1u << ctl;
    if (ctl >= INPUT_A && ctl <= INPUT_RIGHT_TRIGGER)
        return 1u << (ANALOG_BIT0 + ctl - INPUT_A);
    return 0;
}

/* A pad name to its source, 0 if none: lowercase as written (so "A" stays
 * the key and "a" is the button), a source name, or a guest control's name
 * for the source the identity gives it (white is left_shoulder whatever
 * [input.pad] says), so a binding means the same buttons under any map. */
static int pad_name_src(const char *s)
{
    xbox_pad_setup id;
    int src, ctl;

    for (const char *p = s; *p; p++)
        if (isupper((unsigned char)*p))
            return 0;
    src = xbox_InputMap_SourceFromName(s);
    if (src > INPUT_SRC_NONE)
        return src;
    ctl = xbox_InputMap_ControlFromName(s);
    if (ctl < 0 || ctl >= INPUT_PAD_CONTROL_COUNT)
        return 0;
    xbox_PadSetupIdentity(&id);
    return id.map.source[ctl];
}

static int modifier_family(int code)
{
    switch (code) {
    case XBOX_VK_SHIFT: case XBOX_VK_LSHIFT: case XBOX_VK_RSHIFT:     return 1;
    case XBOX_VK_CONTROL: case XBOX_VK_LCONTROL: case XBOX_VK_RCONTROL: return 2;
    case XBOX_VK_MENU: case XBOX_VK_LMENU: case XBOX_VK_RMENU:         return 3;
    }
    return 0;
}

static int either_side(int code)
{
    return code == XBOX_VK_SHIFT || code == XBOX_VK_CONTROL || code == XBOX_VK_MENU;
}

/* Whether a key the guest receives is the same key as code: equal, or the
 * same modifier where either side is the either-side code (LShift is held
 * whenever Shift is, and Shift with LShift), or Return and keypad Enter. */
static int same_key(int a, int b)
{
    int fa, fb;
    if (a == b)
        return 1;
    if ((a == XBOX_VK_RETURN && b == XBOX_VK_KP_ENTER)
        || (b == XBOX_VK_RETURN && a == XBOX_VK_KP_ENTER))
        return 1;
    fa = modifier_family(a);
    fb = modifier_family(b);
    return fa && fa == fb && (either_side(a) || either_side(b));
}

/* Rule 2: what in the effective key map has code, or NULL. */
static const char *guest_key_use(int code, char *buf, size_t size)
{
    xbox_mouse_opts mo;
    int walk = xbox_InputKeyboardWalkKey();

    for (int ctl = 0; ctl < INPUT_CONTROL_COUNT; ctl++) {
        int codes[XBOX_BIND_MAX], mice[XBOX_BIND_MAX];
        int n = xbox_InputBindingsOf(ctl, codes, mice, XBOX_BIND_MAX);
        for (int k = 0; k < n; k++)
            if (!mice[k] && same_key(codes[k], code)) {
                snprintf(buf, size, "bound to the guest's %s (input.bindings)",
                         xbox_InputMap_ControlName(ctl));
                return buf;
            }
    }
    if (walk && same_key(walk, code))
        return "the walk key (input.walk_key)";
    xbox_InputMouseGetOpts(&mo);
    if (mo.release_key && same_key(mo.release_key, code))
        return "the mouse release key (input.mouse_release_key)";
    return NULL;
}

/* ---- the parser -------------------------------------------------------- */

static void warn(const char *where, const char *text, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[INPUT] action %s: '%s' refused: ", where, text);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void warn_off(const char *where, const char *text, const char *was,
                     const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[INPUT] action %s: '%s' inactive: ", where, text);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (was)
        fprintf(stderr, " (was %s)", was);
    fputc('\n', stderr);
}

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    memmove(s, s + i, n - i + 1);
}

/* One binding into *b, or 0 with a warning. */
static int parse_binding(const char *where, const char *text, struct binding *b)
{
    char buf[TEXT_MAX], *tok[8], *s, *at;
    int ntok = 0, hold = 0, has_hold = 0, allpad = 1;
    int src[8];

    memset(b, 0, sizeof *b);
    if (strlen(text) >= sizeof buf) {
        warn(where, text, "longer than %d characters", TEXT_MAX - 1);
        return 0;
    }
    snprintf(b->text, sizeof b->text, "%s", text);
    snprintf(buf, sizeof buf, "%s", text);
    trim(buf);
    s = buf;

    /* padN: */
    if (!strncmp(s, "pad", 3) && s[3] >= '1' && s[3] <= '4' && s[4] == ':') {
        b->port = (unsigned)(s[3] - '1');
        s += 5;
    }
    /* @ms on the whole binding */
    at = strchr(s, '@');
    if (at) {
        char *end;
        long v;
        *at = 0;
        v = strtol(at + 1, &end, 10);
        trim(end);
        if (end == at + 1 || *end || v < 0 || v > HOLD_MAX_MS) {
            warn(where, text, "the hold after '@' must be 0..%d ms", HOLD_MAX_MS);
            return 0;
        }
        hold = (int)v;
        has_hold = 1;
    }
    for (char *p = s;;) {
        char *plus = strchr(p, '+');
        if (ntok == 8) {
            warn(where, text, "more than 8 members");
            return 0;
        }
        if (plus)
            *plus = 0;
        trim(p);
        if (!*p) {
            warn(where, text, "an empty member");
            return 0;
        }
        tok[ntok++] = p;
        if (!plus)
            break;
        p = plus + 1;
    }
    for (int i = 0; i < ntok; i++) {
        src[i] = pad_name_src(tok[i]);
        if (!src[i])
            allpad = 0;
    }
    b->hold_ms = hold;
    b->has_hold = has_hold;

    /* A pad binding: its kind waits for the map (classify, at build). */
    if (allpad) {
        for (int i = 0; i < ntok; i++) {
            if (b->bits & (1u << src[i])) {
                warn(where, text, "'%s' twice", tok[i]);
                return 0;
            }
            b->bits |= 1u << src[i];
        }
        b->kind = B_PAD;
        return 1;
    }

    /* Name every member first: an old pad name in a chord
     * (back+lthumb@400) is an unknown name, not a key misusing '@'. */
    for (int i = 0; i < ntok; i++) {
        int code, is_mouse;
        if (!src[i] && !xbox_InputNameToCode(tok[i], &code, &is_mouse)) {
            warn(where, text, "unknown name '%s' (keys as in docs/input.md,"
                 " pad buttons in lowercase)", tok[i]);
            return 0;
        }
    }

    /* A key, after its modifiers. */
    if (has_hold || b->port) {
        warn(where, text, "'@' and 'padN:' are for pad bindings");
        return 0;
    }
    for (int i = 0; i < ntok; i++) {
        int code, is_mouse;
        char why[96];
        const char *use;
        if (!xbox_InputNameToCode(tok[i], &code, &is_mouse)) {
            warn(where, text, "unknown name '%s' (keys as in docs/input.md,"
                 " pad buttons in lowercase)", tok[i]);
            return 0;
        }
        if (is_mouse) {
            warn(where, text, "'%s' is a mouse input; actions take keys and pad"
                 " buttons", tok[i]);
            return 0;
        }
        if (i < ntok - 1 && !modifier_family(code)) {
            warn(where, text, "'%s' is not a modifier (Ctrl, Shift, Alt)", tok[i]);
            return 0;
        }
        use = guest_key_use(code, why, sizeof why);
        if (use) {
            warn(where, text, "'%s' is %s; the game keeps it", tok[i], use);
            return 0;
        }
        if (i < ntok - 1)
            b->mods[b->nmods++] = code;
        else
            b->key = code;
    }
    b->kind = B_KEY;
    return 1;
}

/* ---- the step ------------------------------------------------------------ */

/* The sources down on the raw host pad. A source counts once its 0..255
 * reading (PadMap's >> 7) passes the Duke's analog threshold: under the
 * identity that is exactly when the guest button it feeds reads down, as
 * the bindings read it before the map existed; a button source is 0 or
 * 32767. */
static uint32_t host_bits(const struct xbox_host_pad *h)
{
    uint32_t b = 0;
    if (!h || !h->connected)
        return 0;
    for (int i = INPUT_SRC_NONE + 1; i < INPUT_SRC_COUNT; i++)
        if ((h->raw.src[i] >> 7) > XBOX_ANALOG_BUTTON_THRESHOLD)
            b |= 1u << i;
    return b;
}

/* The key and exactly its modifiers: Ctrl+F6 is not F6, and F6 with Ctrl
 * held is not F6, so two bindings never both fire. */
static int key_cond(const struct binding *b)
{
    int want[4] = { 0, 0, 0, 0 }, self = modifier_family(b->key);

    if (!xbox_FramebufferKeyDown(b->key))
        return 0;
    for (int i = 0; i < b->nmods; i++) {
        if (!xbox_FramebufferKeyDown(b->mods[i]))
            return 0;
        want[modifier_family(b->mods[i])] = 1;
    }
    if (self != 1 && !want[1] && xbox_FramebufferKeyDown(XBOX_VK_SHIFT))
        return 0;
    if (self != 2 && !want[2] && xbox_FramebufferKeyDown(XBOX_VK_CONTROL))
        return 0;
    if (self != 3 && !want[3] && xbox_FramebufferKeyDown(XBOX_VK_MENU))
        return 0;
    return 1;
}

/* One step of port's bindings with the host pad's bits. tick: the flip's
 * step, which leaves guest chords alone (only a poll can consume them). */
static void step(struct table *t, unsigned port, uint32_t held, int tick, uint64_t now)
{
    struct port_state *ps = &s_port[port];
    uint32_t fresh = held & ~ps->held;

    ps->held = held;
    ps->consumed &= held;
    for (int i = 0; i < 32; i++)
        if (fresh & (1u << i))
            ps->press_ms[i] = now;
    if (!t)
        return;
    for (int i = 0; i < t->n; i++) {
        struct binding *b = &t->b[i];
        int cond = 0;

        if (b->kind == B_OFF || (b->kind == B_KEY ? port != 0 : b->port != port))
            continue;
        if (tick && b->kind == B_CHORD && b->guest)
            continue;
        switch (b->kind) {
        case B_KEY:
            cond = key_cond(b);
            break;
        case B_HOST:
        case B_CHORD:
            cond = (held & b->bits) == b->bits;
            if (cond)
                ps->consumed |= b->guest;
            break;
        case B_MOD: {
            int mods = (held & b->modmask) == b->modmask;
            b->modpress &= held;
            if (mods) {
                uint32_t m = fresh & b->guest;
                b->modpress |= m;
                ps->consumed |= m;
            }
            cond = mods && (b->modpress & b->guest) == b->guest;
            break;
        }
        }
        if (!cond) {
            b->active = b->fired = b->held_over = 0;
        } else if (!b->active) {
            b->active = 1;
            b->fired = 0;
            b->since = now;
        }
        if (b->active && !b->fired && now - b->since >= (uint64_t)b->hold_ms) {
            b->fired = 1;
            __atomic_add_fetch(&s_act[b->action].pressed, 1, __ATOMIC_ACQ_REL);
            fire_log(b, now);
            toolkit_fire(b->action);
        }
        if (b->active && b->fired && !b->held_over)
            __atomic_or_fetch(&s_act[b->action].down, 1u << b->idx, __ATOMIC_ACQ_REL);
        else
            __atomic_and_fetch(&s_act[b->action].down, ~(1u << b->idx), __ATOMIC_ACQ_REL);
    }
}

/* What the guest does not see on port: the guest controls fed by the
 * consumed sources and by chord members inside chord_delay_ms of their
 * press. */
static uint32_t guest_mask(const struct table *t, unsigned port, uint64_t now)
{
    const struct port_state *ps = &s_port[port];
    uint32_t src = ps->consumed, mask = 0;

    if (!t)
        return 0;
    if (t->delay_ms) {
        uint32_t wait = ps->held & t->chord_members[port] & ~src;
        for (int i = 0; i < SRC_BITS; i++)
            if ((wait & (1u << i)) && now - ps->press_ms[i] < (uint64_t)t->delay_ms)
                src |= 1u << i;
    }
    for (int i = 0; i < SRC_BITS; i++)
        if (src & (1u << i))
            mask |= t->feeds[i];
    return mask & GUEST_BITS;
}

/* Centre *v when it points the way a masked half does: the other half
 * still reaches the game, so a consumed "up" never hides a push down. */
static void mask_axis(int16_t *v, uint32_t mask, int pos_bit, int neg_bit)
{
    if (((mask >> pos_bit) & 1u) && *v > 0)
        *v = 0;
    if (((mask >> neg_bit) & 1u) && *v < 0)
        *v = 0;
}

static void apply_mask(XBOX_INPUT_STATE *st, uint32_t mask)
{
    XBOX_GAMEPAD *g = &st->Gamepad;

    g->wButtons &= (WORD)~(mask & 0xFFu);
    for (int i = 0; i < 8; i++)
        if (mask & (1u << (ANALOG_BIT0 + i)))
            g->bAnalogButtons[i] = 0;
    /* Up is Y positive, as in InputRaw. */
    mask_axis(&g->sThumbLY, mask, STICK_BIT0 + 0, STICK_BIT0 + 1);
    mask_axis(&g->sThumbLX, mask, STICK_BIT0 + 3, STICK_BIT0 + 2);
    mask_axis(&g->sThumbRY, mask, STICK_BIT0 + 4, STICK_BIT0 + 5);
    mask_axis(&g->sThumbRX, mask, STICK_BIT0 + 7, STICK_BIT0 + 6);
}

void enhance_actions_filter(unsigned port, const struct xbox_host_pad *host,
                            XBOX_INPUT_STATE *st)
{
    uint64_t now;
    uint32_t mask;

    if (port >= PORTS || !__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    now = xbox_InputNowMs();
    spin_lock(&s_step_lock);
    s_port[port].last_poll = now;
    s_port[port].polled = 1;
    /* -1 too: a port whose pad went away has no slot for the tick to read. */
    s_port[port].slot = host ? host->slot : -1;
    step(s_table, port, host_bits(host), 0, now);
    mask = guest_mask(s_table, port, now);
    spin_unlock(&s_step_lock);
    if (st)
        apply_mask(st, mask);
}

/* A port the tick steps: one its table reads that the title has not
 * polled for STALE_MS. Under the step lock. */
static int tick_wants(const struct table *t, unsigned port, uint64_t now)
{
    const struct port_state *ps = &s_port[port];

    if (!(t->ports & (1u << port)) && !(port == 0 && t->keys))
        return 0;
    return !(ps->polled && now - ps->last_poll < STALE_MS);
}

/* The slot the tick reads for port, -1 none. A port never polled is its
 * own slot (input_core's ports are). Once polled, the title's slot, and
 * none is not read: in a title that maps pads itself (cat), port n may be
 * another pad's slot. Under the step lock. */
static int tick_slot(unsigned port)
{
    return s_port[port].polled ? s_port[port].slot : (int)port;
}

void enhance_actions_tick(void)
{
    uint64_t now;
    struct table *t;
    unsigned want = 0, read = 0;
    int slot[PORTS], polled[PORTS];
    uint64_t last[PORTS];
    uint32_t got[PORTS];
    DWORD rc[PORTS];

    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    /* Which ports to read, and their slots. The clock is read under the
     * lock: a poll's last_poll from after it would wrap now - last_poll. */
    if (!spin_try(&s_step_lock))
        return;     /* a poll is stepping now */
    now = xbox_InputNowMs();
    t = s_table;
    for (unsigned port = 0; t && port < PORTS; port++) {
        if (!tick_wants(t, port, now))
            continue;
        want |= 1u << port;
        slot[port] = tick_slot(port);
        last[port] = s_port[port].last_poll;
        polled[port] = s_port[port].polled;
        if ((t->ports & (1u << port)) && slot[port] >= 0)
            read |= 1u << port;
    }
    spin_unlock(&s_step_lock);
    if (!want)
        return;
    /* The backend outside the lock: XInput under Wine can take a while. */
    for (unsigned port = 0; port < PORTS; port++) {
        struct xbox_host_pad h;
        if (!(read & (1u << port)))
            continue;
        rc[port] = xbox_input_backend_read_host((DWORD)slot[port], &h);
        got[port] = rc[port] == ERROR_SUCCESS ? host_bits(&h) : 0;
    }
    if (!spin_try(&s_step_lock))
        return;
    now = xbox_InputNowMs();
    t = s_table;
    for (unsigned port = 0; t && port < PORTS; port++) {
        struct port_state *ps = &s_port[port];
        uint32_t held = ps->held;

        /* A poll during the read has stepped the port with a newer state:
         * stepping it again with ours would refire a press or start a hold
         * early. The slot too: a poll in the same millisecond keeps
         * last_poll. Or the table changed. */
        if (!(want & (1u << port)) || ps->last_poll != last[port] ||
            ps->polled != polled[port] || tick_slot(port) != slot[port] ||
            !tick_wants(t, port, now))
            continue;
        if (t->ports & (1u << port)) {
            if (slot[port] < 0)
                held = 0;   /* no pad behind it now */
            else if (!(read & (1u << port)))
                continue;   /* the new table reads a port we did not */
            else if (rc[port] == ERROR_SUCCESS)
                held = got[port];
            else if (rc[port] != XBOX_INPUT_BUSY)
                held = 0;   /* no pad: a modifier held as it went away is let go */
        }
        step(t, port, held, 1, now);
    }
    spin_unlock(&s_step_lock);
}

void enhance_actions_mask_until_release(unsigned port, uint32_t bits)
{
    if (port >= PORTS)
        return;
    spin_lock(&s_step_lock);
    /* The held sources that feed them: the map may give a control two. */
    for (int i = 0; s_table && i < SRC_BITS; i++)
        if (s_table->feeds[i] & bits & GUEST_BITS)
            s_port[port].consumed |= (1u << i) & s_port[port].held;
    spin_unlock(&s_step_lock);
}

/* ---- the registry's calls --------------------------------------------- */

static int good_name(const char *s)
{
    int dot = 0;
    if (!s || !*s || strlen(s) >= NAME_MAX_ || *s == '.')
        return 0;
    for (; *s; s++) {
        if (*s == '.')
            dot = 1;
        else if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_'))
            return 0;
    }
    return dot && s[-1] != '.';
}

/* 1 if a is b or a dotted prefix of it ("menu.open" of "menu.open.all"). */
static int name_prefix(const char *a, const char *b)
{
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (b[n] == 0 || b[n] == '.');
}

/* A live action that x.y beside x.y.z would clash with: written bare in
 * TOML, one is a value and the other a table under it. */
static int clashing(const char *name)
{
    for (int i = 0; i < s_nact; i++)
        if (s_act[i].live && (name_prefix(s_act[i].name, name) ||
                              name_prefix(name, s_act[i].name)))
            return i;
    return -1;
}

int enhance_action_find(const char *name)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    if (!name)
        return -1;
    for (int i = 0; i < n; i++)
        if (s_act[i].live && !strcmp(s_act[i].name, name))
            return i;
    return -1;
}

const char *enhance_action_name(int id)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    return id >= 0 && id < n && s_act[id].live ? s_act[id].name : NULL;
}

int enhance_action_register_owned(const char *name, const char *label,
                                  const char *const *defaults, int owner)
{
    struct action *a;
    int id, started, c;

    spin_lock(&s_reg_lock);
    if (!good_name(name)) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: names are <owner>.<verb>"
                " in lowercase\n", name ? name : "(null)");
        return -1;
    }
    if (enhance_action_find(name) >= 0) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: already registered\n", name);
        return -1;
    }
    if ((c = clashing(name)) >= 0) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: it and '%s' cannot both be"
                " TOML keys (one would be a table of the other)\n", name, s_act[c].name);
        return -1;
    }
    if (s_nact >= ENHANCE_ACTIONS_MAX) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: %d actions already\n", name,
                ENHANCE_ACTIONS_MAX);
        return -1;
    }
    id = s_nact;
    a = &s_act[id];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->label, sizeof a->label, "%s", label ? label : name);
    for (int i = 0; defaults && defaults[i] && a->ndefs < ENHANCE_ACTION_BINDS; i++)
        snprintf(a->defs[a->ndefs++], TEXT_MAX, "%s", defaults[i]);
    a->owner = owner;
    a->live = 1;
    __atomic_store_n(&s_nact, id + 1, __ATOMIC_RELEASE);
    started = s_started;
    spin_unlock(&s_reg_lock);
    if (started)
        enhance_actions_rebuild();
    return id;
}

int enhance_action_register(const char *name, const char *label,
                            const char *const *defaults)
{
    return enhance_action_register_owned(name, label, defaults, -1);
}

static void drop_where(int all_mods, int owner)
{
    int started;
    spin_lock(&s_reg_lock);
    for (int i = 0; i < s_nact; i++)
        if (s_act[i].owner >= 0 && (all_mods || s_act[i].owner == owner))
            s_act[i].live = 0;
    /* Mods start over from slot 0 of theirs: trailing dead slots go. */
    while (s_nact && !s_act[s_nact - 1].live)
        s_nact--;
    started = s_started;
    spin_unlock(&s_reg_lock);
    disarm_where(all_mods, owner);
    if (started)
        enhance_actions_rebuild();
}

void enhance_actions_drop_owner(int owner)
{
    drop_where(0, owner);
}

void enhance_actions_drop_mods(void)
{
    drop_where(1, 0);
}

int enhance_action_pressed(int id)
{
    if (id < 0 || id >= ENHANCE_ACTIONS_MAX)
        return 0;
    return __atomic_exchange_n(&s_act[id].pressed, 0, __ATOMIC_ACQ_REL);
}

int enhance_action_down(int id)
{
    if (id < 0 || id >= ENHANCE_ACTIONS_MAX)
        return 0;
    return __atomic_load_n(&s_act[id].down, __ATOMIC_ACQUIRE) != 0;
}

/* ---- building the table ------------------------------------------------ */

/* The bindings of one action from the highest file that names it (all of
 * them: an empty list unbinds), else its defaults. *n is how many texts. */
static int config_texts(const struct action *a, const char *texts[ENHANCE_ACTION_BINDS],
                        char where[96])
{
    char key[sizeof PREFIX + NAME_MAX_];
    const recomp_cfg *c;
    int n = 0;

    snprintf(key, sizeof key, PREFIX "%s", a->name);
    c = enhance_cfg_lookup(key);
    if (c) {
        recomp_cfg_type t = recomp_cfg_type_of(c, key);
        snprintf(where, 96, "%s (%s:%d)", key, recomp_cfg_name(c), recomp_cfg_line(c, key));
        if (t == RCFG_STRING) {
            texts[n++] = recomp_cfg_string(c, key, "");
            return n;
        }
        if (t == RCFG_ARRAY) {
            size_t len = recomp_cfg_array_len(c, key);
            for (size_t i = 0; i < len; i++) {
                if (recomp_cfg_array_type(c, key, i) != RCFG_STRING) {
                    fprintf(stderr, "[INPUT] action %s: element %zu is not a"
                            " string; skipped\n", where, i + 1);
                    continue;
                }
                if (n == ENHANCE_ACTION_BINDS) {
                    fprintf(stderr, "[INPUT] action %s: more than %d bindings;"
                            " the rest skipped\n", where, ENHANCE_ACTION_BINDS);
                    break;
                }
                texts[n++] = recomp_cfg_array_string(c, key, i, "");
            }
            return n;
        }
        fprintf(stderr, "[INPUT] action %s: should be a list of strings; using"
                " the defaults\n", where);
    }
    snprintf(where, 96, "%s (default)", a->name);
    for (int i = 0; i < a->ndefs; i++)
        texts[n++] = a->defs[i];
    return n;
}

/* What the map gives the game: each source's guest bits (feeds) and the
 * sources the game reads at all. A physical stick's directions feed the
 * halves of the guest stick a map selects it for (raw is inverted before
 * the map, so the directions match the guest's). */
static uint32_t map_feeds(const xbox_pad_setup *ps, uint32_t feeds[SRC_BITS])
{
    const InputPadMap *m[2] = { &ps->map, &ps->alt };
    uint32_t used = 0;

    memset(feeds, 0, SRC_BITS * sizeof feeds[0]);
    for (int k = 0; k < 2; k++) {
        for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++) {
            int src = m[k]->source[c];
            if (src > INPUT_SRC_NONE && src < INPUT_SRC_COUNT) {
                feeds[src] |= control_bit(c);
                used |= 1u << src;
            }
        }
        for (int st = 0; st < 2; st++) {
            int which = st ? m[k]->right_stick : m[k]->left_stick;
            int first = which == INPUT_STICK_LEFT ? INPUT_SRC_LSTICK_UP
                      : which == INPUT_STICK_RIGHT ? INPUT_SRC_RSTICK_UP : 0;
            for (int d = 0; first && d < 4; d++) {
                feeds[first + d] |= 1u << (STICK_BIT0 + st * 4 + d);
                used |= 1u << (first + d);
            }
        }
    }
    return used;
}

/* "guide (the game's start), south (a)": who took srcs from the host. */
static void took(const xbox_pad_setup *ps, uint32_t srcs, char *buf, size_t size)
{
    size_t n = 0;

    buf[0] = 0;
    for (int i = 0; i < SRC_BITS && n < size; i++) {
        const char *ctl = "a stick";
        if (!(srcs & (1u << i)))
            continue;
        for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++)
            if (ps->map.source[c] == i || ps->alt.source[c] == i) {
                ctl = xbox_InputMap_ControlName(c);
                break;
            }
        n += (size_t)snprintf(buf + n, size - n, n ? ", %s (%s)" : "%s (the game's %s)",
                              xbox_InputMap_SourceName(i), ctl);
    }
}

static const char *kind_name(const struct binding *b)
{
    switch (b->kind) {
    case B_HOST:  return "host-only";
    case B_CHORD: return b->guest ? "a guest chord" : "a host-only chord";
    case B_MOD:   return "a modifier chord";
    case B_OFF:   return "inactive";
    }
    return "a key";
}

/* The kind of a pad binding under the current map (gamepad-input-ux D4),
 * with one line when it is inactive or its kind differs from the binding
 * of the same action and text in old. */
static void classify(struct binding *b, const char *where, const xbox_pad_setup *ps,
                     uint32_t used, const struct table *old)
{
    const char *was = NULL;
    uint32_t mapped = b->bits & used, free_ = b->bits & ~used;
    char who[160];
    int single = !(b->bits & (b->bits - 1));

    b->guest = mapped;
    if (single)
        b->kind = mapped ? B_OFF : B_HOST;
    else if (!mapped)
        b->kind = B_CHORD;          /* nothing reaches the game */
    else if (!free_)
        b->kind = b->has_hold && b->hold_ms >= CHORD_MIN_MS ? B_CHORD : B_OFF;
    else {
        b->kind = B_MOD;
        b->modmask = free_;
    }
    for (int i = 0; old && i < old->n; i++)
        if (!strcmp(old->b[i].name, b->name) && !strcmp(old->b[i].text, b->text)) {
            if (strcmp(kind_name(&old->b[i]), kind_name(b)))
                was = kind_name(&old->b[i]);
            break;
        }
    if (b->kind == B_OFF) {
        took(ps, mapped, who, sizeof who);
        if (single)
            warn_off(where, b->text, was, "%s goes to the game; a single source the game"
                     " uses would take it from the game; use a chord of two with a hold"
                     " (back+left_thumb@400) or a source the game does not use (guide,"
                     " paddle1)", who);
        else
            warn_off(where, b->text, was, "every member goes to the game (%s); such a"
                     " chord needs a hold of at least %d ms (@400)", who, CHORD_MIN_MS);
    } else if (was) {
        fprintf(stderr, "[INPUT] action %s: '%s' is now %s (was %s)\n", where, b->text,
                kind_name(b), was);
    }
}

/* Under s_reg_lock; old is the table in use, for the kind changes. */
static struct table *build(const struct table *old)
{
    struct table *t;
    xbox_pad_setup ps;
    uint32_t used;
    int cap = 1;

    for (int i = 0; i < s_nact; i++)
        cap += ENHANCE_ACTION_BINDS;
    t = calloc(1, sizeof *t + (size_t)cap * sizeof t->b[0]);
    if (!t)
        return NULL;
    t->delay_ms = s_delay_ms;
    xbox_InputPadSetup(&ps);
    used = map_feeds(&ps, t->feeds);
    for (int i = 0; i < s_nact; i++) {
        const char *texts[ENHANCE_ACTION_BINDS];
        char where[96];
        int n, idx = 0;
        if (!s_act[i].live)
            continue;
        n = config_texts(&s_act[i], texts, where);
        for (int k = 0; k < n; k++) {
            struct binding *b = &t->b[t->n];
            if (!texts[k] || !*texts[k])
                continue;
            if (!parse_binding(where, texts[k], b))
                continue;
            b->action = i;
            b->idx = idx++;
            snprintf(b->name, sizeof b->name, "%s", s_act[i].name);
            if (b->kind == B_PAD)
                classify(b, where, &ps, used, old);
            /* A held input must be let go before a new binding fires on it,
             * and does not read as down until then. */
            b->active = b->fired = b->held_over = 1;
            if (b->kind == B_KEY) {
                t->keys = 1;
            } else if (b->kind != B_OFF) {
                t->ports |= 1u << b->port;
                if (b->kind == B_CHORD)
                    t->chord_members[b->port] |= b->guest;
            }
            t->n++;
        }
    }
    return t;
}

/* Keys under [input.actions] that name no action: a typo is visible. */
static void report_unknown(void)
{
    for (int f = 0; f < 3; f++) {
        const recomp_cfg *c = enhance_cfg_file(f);
        const char *k;
        for (size_t i = 0; c && (k = recomp_cfg_key_at(c, i)); i++) {
            if (strncmp(k, PREFIX, sizeof PREFIX - 1))
                continue;
            if (enhance_action_find(k + sizeof PREFIX - 1) < 0)
                fprintf(stderr, "[INPUT] config warning: %s:%d: no action named"
                        " '%s'\n", recomp_cfg_name(c), recomp_cfg_line(c, k),
                        k + sizeof PREFIX - 1);
            enhance_cfg_lookup(k);   /* reported here, not as unused */
        }
    }
}

static void read_delay(void)
{
    long long v;
    enhance_cfg_bind_env("input.chord_delay_ms", RENV_INPUT_CHORD_DELAY_MS);
    v = enhance_cfg_int("input.chord_delay_ms", 0);
    if (v < 0 || v > DELAY_MAX_MS) {
        fprintf(stderr, "[INPUT] config warning: input.chord_delay_ms=%lld out of"
                " 0..%d; using %d\n", v, DELAY_MAX_MS, v < 0 ? 0 : DELAY_MAX_MS);
        v = v < 0 ? 0 : DELAY_MAX_MS;
    }
    s_delay_ms = (int)v;
}

void enhance_actions_rebuild(void)
{
    struct table *t, *old;

    /* The registry lock spans the swap: build reads the table in use for
     * the kind changes, and another rebuild must not free it meanwhile. */
    spin_lock(&s_reg_lock);
    read_delay();
    t = build(s_table);
    if (!t) {
        spin_unlock(&s_reg_lock);
        return;
    }
    spin_lock(&s_step_lock);
    old = s_table;
    s_table = t;
    /* The down bits index the old table's bindings; the new one sets its
     * own from the next step. */
    for (int i = 0; i < ENHANCE_ACTIONS_MAX; i++)
        __atomic_store_n(&s_act[i].down, 0u, __ATOMIC_RELEASE);
    spin_unlock(&s_step_lock);
    spin_unlock(&s_reg_lock);
    free(old);
}

/* ---- arming -------------------------------------------------------------- */

static void hook_line(void)
{
    if (!s_armed) {
        fprintf(stderr, "[ENHANCE] actions hook=off\n");
        return;
    }
    fprintf(stderr, "[ENHANCE] actions hook=on (");
    for (int i = 0; i < s_narm; i++)
        fprintf(stderr, i ? ", %s" : "%s", s_arm_keys[i]);
    fprintf(stderr, ")\n");
}

static int arm_key(const char *key, int owner)
{
    if (!key || !enhance_cfg_bool(key, 0))
        return 0;
    for (int i = 0; i < s_narm; i++)
        if (!strcmp(s_arm_keys[i], key)) {
            if (owner < 0)
                s_arm_owner[i] = -1;    /* the toolkit's key stays */
            return 1;
        }
    if (s_narm < ARM_KEYS) {
        s_arm_owner[s_narm] = owner;
        snprintf(s_arm_keys[s_narm++], NAME_MAX_, "%s", key);
    }
    return 1;
}

/* The title's pad glue (mod.h) and input_core's seam. */
void xbox_enhance_pad_filter(DWORD port, const struct xbox_host_pad *host,
                             XBOX_INPUT_STATE *st)
{
    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    enhance_actions_filter((unsigned)port, host, st);
}

static void install(void)
{
    xbox_InputSetPadFilter(xbox_enhance_pad_filter);
#ifdef XBOXRECOMP_ENHANCE
    nv2a_flip_hook_set(enhance_actions_tick);
#endif
}

int enhance_actions_arm(const char *key)
{
    return enhance_actions_arm_owned(key, -1);
}

int enhance_actions_arm_owned(const char *key, int owner)
{
    int was = __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE);
    if (!arm_key(key, owner))
        return was;
    if (!s_started)
        return 1;   /* start arms and logs */
    if (!was) {
        __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
        install();
        hook_line();
    }
    return 1;
}

int enhance_actions_armed(void)
{
    return __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE);
}

int enhance_actions_keyed(const char *key)
{
    for (int i = 0; key && i < s_narm; i++)
        if (!strcmp(s_arm_keys[i], key))
            return 1;
    return 0;
}

/* A mod left out (its init failed) or dropped takes the keys it armed
 * with; the last one gone disarms the hook. */
static void disarm_where(int all_mods, int owner)
{
    int n = 0;

    for (int i = 0; i < s_narm; i++) {
        if (s_arm_owner[i] >= 0 && (all_mods || s_arm_owner[i] == owner))
            continue;
        if (n != i) {
            memcpy(s_arm_keys[n], s_arm_keys[i], NAME_MAX_);
            s_arm_owner[n] = s_arm_owner[i];
        }
        n++;
    }
    if (n == s_narm)
        return;
    s_narm = n;
    if (!n && s_started && __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_armed, 0, __ATOMIC_RELEASE);
        xbox_InputSetPadFilter(NULL);
#ifdef XBOXRECOMP_ENHANCE
        nv2a_flip_hook_set(NULL);
#endif
        hook_line();
    }
}

/* ---- the toolkit's actions and the start ------------------------------ */

void enhance_actions_init(void)
{
    /* paddle1 is the Deck's R4 (an Elite's P1). Steam keeps the Steam
     * button and Steam+A is its screen recording, so guide+a is no use on
     * the Deck. The SDL3 backend reads the paddle; XInput has none, so
     * under Proton Steam Input maps R4 to F10 instead (config.md, "Host
     * actions"). Host-only, so it never takes a press from the game. */
    static const char *const menu_open[] = { "F10", "guide+a", "paddle1", NULL };

    enhance_actions_reset();
    s_menu_act = enhance_action_register("menu.open", "Open the menu", menu_open);
    s_stats_act = enhance_action_register("overlay.stats", "Stats strip", NULL);
    enhance_action_register("shot.window", "Window screenshot", NULL);
}

void enhance_actions_start(void)
{
    int keyed;

    /* Feature keys only (rule 6): the menu and the stats strip, plus a
     * mod's own key it armed with during its init. */
    arm_key("menu.enabled", -1);
    arm_key("overlay.stats", -1);
    keyed = s_narm > 0;
    report_unknown();
    enhance_actions_rebuild();
    spin_lock(&s_reg_lock);
    s_started = 1;
    spin_unlock(&s_reg_lock);
    if (keyed) {
        __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
        install();
    }
    hook_line();
    fflush(stderr);
}

void enhance_actions_reset(void)
{
    __atomic_store_n(&s_armed, 0, __ATOMIC_RELEASE);
    xbox_InputSetPadFilter(NULL);
#ifdef XBOXRECOMP_ENHANCE
    nv2a_flip_hook_set(NULL);
#endif
    spin_lock(&s_reg_lock);
    s_nact = 0;
    s_started = 0;
    s_narm = 0;
    s_delay_ms = 0;
    memset(s_act, 0, sizeof s_act);
    spin_unlock(&s_reg_lock);
    spin_lock(&s_step_lock);
    free(s_table);
    s_table = NULL;
    memset(s_port, 0, sizeof s_port);
    for (int i = 0; i < PORTS; i++)
        s_port[i].slot = -1;
    spin_unlock(&s_step_lock);
    s_log_sec = 0;
    s_log_n = s_log_dropped = 0;
}
