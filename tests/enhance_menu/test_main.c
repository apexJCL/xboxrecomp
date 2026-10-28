/*
 * enhance_menu test: the in-game menu (src/enhance/enhance_menu.c) as a
 * title runs it, through xbox_enhance_init and xbox_InputGetState against a
 * fake input backend with its own clock, drawn by the real overlay:
 *
 *   start     menu.enabled off logs off and arms nothing; on logs the
 *             menu.open bindings
 *   navigate  F10 opens, the pages wrap, the selection skips info rows,
 *             the title reads a neutral pad with a frozen packet number,
 *             Esc closes and the held key stays the host's to its release
 *   mods      a toggle through on_enable runs at the next xbox_mod_frame;
 *             a mod without on_enable is "applies at restart"; a mod's
 *             item on its sub-list
 *   capture   a key, a held pad chord, a refusal (a key the guest uses),
 *             Esc cancels, menu.open cannot be cleared
 *   save      config.toml gets only the changed keys: the binding as an
 *             array, an item, a value back at its default removed
 *   mouse     the Controls page's Mouse group and the capture choice
 *   snapshot  the pack's miss ring copied at the flip only while shown
 *   pages     each page's draw list at 1920x1080 against a pinned sum
 *
 * Runs in its working directory, where it writes config/config.toml. A
 * checksum of 0 below means "not pinned yet": the run prints the value.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_menu.h"
#include "host_pad.h"
#include "input_backend.h"
#include "input_names.h"
#include "keyboard.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_int.h"
#include "nv2a_pb_state.h"
#include "nv2a_tex_pack.h"
#include "pad_map.h"
#include "recomp_env.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define rmdir _rmdir
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_TRUNC(p) _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_TRUNC(p) open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

static int s_fail, s_checks;
static int s_pin;   /* --pin: print the page sums instead of checking them */

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs for what the runtime would link ---------------------------- */

static int s_pacing = 1;
int xbox_PacingMode(void);
int xbox_PacingMode(void) { return s_pacing; }
void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { s_pacing = mode; }

ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }

static struct nv2a_pb_present s_present = { 0x00C00000u, 640, 480, 2560, 0 };
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

/* ---- the fake pad backend --------------------------------------------- */

#define VK_F5  0x74
#define VK_F7  0x76
#define VK_F10 0x79

static uint64_t s_now = 1000;
static XBOX_INPUT_STATE f_pad[4];
static int f_conn[4] = { 1, 0, 0, 0 };
static uint32_t f_extra[4];

static uint64_t f_clock(void) { return __atomic_load_n(&s_now, __ATOMIC_ACQUIRE); }
static void advance(uint64_t ms) { __atomic_add_fetch(&s_now, ms, __ATOMIC_ACQ_REL); }

static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    *s = f_pad[p];
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    XBOX_INPUT_STATE s;
    DWORD rc = f_get(p, &s);
    if (rc == ERROR_SUCCESS) {
        xbox_PadRawFromGamepad(&s.Gamepad, extras ? f_extra[p] : 0, raw);
        *packet = s.dwPacketNumber;
    }
    return rc;
}
static DWORD f_read_raw(DWORD p, InputRaw *raw)
{
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_PadRawFromGamepad(&f_pad[p].Gamepad, f_extra[p], raw);
    return ERROR_SUCCESS;
}

static const xbox_input_backend k_fake = {
    NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, f_read_raw, NULL, NULL };
const xbox_input_backend xbox_input_host_backend = {
    NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

static void pad_clear(void)
{
    memset(f_pad, 0, sizeof f_pad);
    memset(f_extra, 0, sizeof f_extra);
    f_conn[0] = 1;
}
static void hold_btn(WORD w, int on)
{
    if (on)
        f_pad[0].Gamepad.wButtons |= w;
    else
        f_pad[0].Gamepad.wButtons &= (WORD)~w;
}
static void hold_a(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_A] = on ? 255 : 0; }

static XBOX_INPUT_STATE poll0(void)
{
    XBOX_INPUT_STATE st;
    memset(&st, 0, sizeof st);
    xbox_InputGetState(0, &st);
    return st;
}

/* The flip's part: the live options requested apply there. */
static void flip(void)
{
    nv2a_host_opts_apply();
    nv2a_tex_pack_snapshot();
}

/* A key press and release, each seen by a poll a little apart, a flip
 * after each. */
static void tap(int vk)
{
    xbox_InputKeyEvent(vk, 1);
    advance(16);
    poll0();
    flip();
    xbox_InputKeyEvent(vk, 0);
    advance(16);
    poll0();
    flip();
}

/* ---- stderr capture ---------------------------------------------------- */

static int s_saved_fd = -1;
static char s_cap[32768];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("menu_stderr.txt");
    if (s_saved_fd < 0 || fd < 0) { fprintf(stderr, "cannot capture stderr\n"); exit(2); }
    DUP2(fd, 2);
    CLOSE(fd);
}

static const char *capture_end(void)
{
    FILE *f;
    size_t n = 0;
    fflush(stderr);
    DUP2(s_saved_fd, 2);
    CLOSE(s_saved_fd);
    s_cap[0] = 0;
    f = fopen("menu_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    {
        char *r = s_cap, *w = s_cap;
        for (; *r; r++)
            if (*r != '\r')
                *w++ = *r;
        *w = 0;
    }
    return s_cap;
}

/* ---- setup -------------------------------------------------------------- */

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static char s_file[8192];

static const char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    s_file[0] = 0;
    if (!f)
        return s_file;
    n = fread(s_file, 1, sizeof s_file - 1, f);
    fclose(f);
    s_file[n] = 0;
    return s_file;
}

static const char *start_with(const char *toml, const struct xbox_mod *const *mods)
{
    const char *out;
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    nv2a_overlay_reset_for_test();
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeysClear();
    pad_clear();
    if (toml)
        write_file("config/config.toml", toml);
    else
        remove("config/config.toml");
    xbox_mod_register_table(mods);
    capture_begin();
    xbox_enhance_init(".", NULL);
    out = capture_end();
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    poll0();
    return out;
}

#define ARMED "[menu]\nenabled = true\n"

/* The row with this label on the page shown: its value, or NULL. */
static char s_val[128];
static const char *row_value(const char *label)
{
    char l[96];
    int n = enhance_menu_rows();
    for (int i = 0; i < n; i++) {
        enhance_menu_row(i, l, sizeof l, s_val, sizeof s_val, NULL);
        if (!strcmp(l, label))
            return s_val;
    }
    return NULL;
}

static const char *selected_label(void)
{
    static char l[96];
    enhance_menu_row(enhance_menu_selected(), l, sizeof l, NULL, 0, NULL);
    return l;
}

static void open_menu(void)
{
    tap(VK_F10);
}

/* ---- a mod with on_enable, an action and an item; one without --------- */

static int s_enable_calls = -1, s_enable_last = -1, s_enable_thread_frame;
static int s_in_frame;
static void tm_enable(int on, void *u)
{
    (void)u;
    s_enable_calls++;
    s_enable_last = on;
    s_enable_thread_frame = s_in_frame;
}
static void tm_frame(void *u) { (void)u; }
static const char *const k_levels[] = { "low", "high", NULL };
static const struct enhance_item k_tm_level = {
    .key = "testmod.level", .label = "Level", .kind = ENHANCE_ITEM_CHOICE,
    .choices = k_levels, .def = "low" };
static int tm_init(const struct xbox_mod_api *api)
{
    static const char *const defs[] = { "F6", NULL };
    s_enable_calls = 0;
    api->on_frame(tm_frame, NULL);
    api->action_register("testmod.poke", "Poke", defs);
    api->menu_add(&k_tm_level);
    return api->on_enable(tm_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_tm = { "testmod", tm_init, NULL, NULL };

static int pm_init(const struct xbox_mod_api *api) { (void)api; return 0; }
static const struct xbox_mod k_pm = { "plainmod", pm_init, NULL, NULL };

static const struct xbox_mod *const k_mods[] = { &k_tm, &k_pm, NULL };

/* ---- 1. start ------------------------------------------------------------- */

static void test_start(void)
{
    const char *out = start_with(NULL, NULL);
    CHECK(strstr(out, "[ENHANCE] menu.enabled=off\n") != NULL);
    open_menu();
    CHECK(!enhance_menu_is_open());

    out = start_with(ARMED, NULL);
    CHECK(strstr(out, "[ENHANCE] menu.enabled=on (F10, guide+a, paddle1)\n") != NULL);
    set_env("RECOMP_MENU", "0");
    out = start_with(ARMED, NULL);
    CHECK(strstr(out, "[ENHANCE] menu.enabled=off\n") != NULL);
    set_env("RECOMP_MENU", NULL);
}

/* ---- 2. navigation and the neutral pad ---------------------------------- */

static void test_navigate(void)
{
    XBOX_INPUT_STATE st, st2;
    start_with(ARMED, k_mods);
    f_pad[0].dwPacketNumber = 41;
    st = poll0();
    open_menu();
    CHECK(enhance_menu_is_open());
    CHECK(enhance_menu_page() == 0);
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    CHECK(enhance_menu_page_count() == 8);

    /* The title: no buttons, one packet number while the pad changes. A
     * trigger: the menu reads none, so the press does nothing there. */
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    f_pad[0].dwPacketNumber = 50;
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0);
    f_pad[0].dwPacketNumber = 51;
    advance(16);
    st2 = poll0();
    CHECK(st2.dwPacketNumber == st.dwPacketNumber);
    CHECK(st.dwPacketNumber != 41 && st.dwPacketNumber != 42 && st.dwPacketNumber != 50);
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 0;
    advance(16);
    poll0();

    /* Q from Render wraps to Save, E back. */
    tap('Q');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Save"));
    tap('E');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    CHECK(!strcmp(selected_label(), "Render scale"));
    xbox_InputKeyEvent(0x28, 1);    /* Down */
    advance(16);
    poll0();
    CHECK(!strcmp(selected_label(), "Anisotropic filtering"));
    /* Held: the first repeat after 150 ms, then every 60. */
    advance(100);
    poll0();
    CHECK(!strcmp(selected_label(), "Anisotropic filtering"));
    advance(60);
    poll0();
    CHECK(!strcmp(selected_label(), "Scaling filter"));
    advance(60);
    poll0();
    CHECK(!strcmp(selected_label(), "Fullscreen"));
    xbox_InputKeyEvent(0x28, 0);
    advance(16);
    poll0();
    /* Up from the first row wraps to the last. */
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x26);
    CHECK(!strcmp(selected_label(), "Present surface"));

    /* Right on Render scale: 1 -> 2, at the flip; unsaved. */
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x27);
    CHECK(!strcmp(row_value("Render scale"), "2"));
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x27);
    tap(0x27);
    tap(0x27);
    CHECK(!strcmp(row_value("Render scale"), "4"));     /* clamped */
    tap(0x0D);                                          /* OK wraps */
    CHECK(!strcmp(row_value("Render scale"), "1"));
    CHECK(enhance_menu_unsaved() == 0);

    /* A choice wraps both ways. */
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x25);
    CHECK(!strcmp(row_value("Scaling filter"), "integer"));
    tap(0x27);
    CHECK(!strcmp(row_value("Scaling filter"), "nearest"));

    /* Esc closes; the title gets nothing while Esc is down, and the pad
     * button held through the close stays the host's to its release. */
    hold_a(1);
    advance(16);
    poll0();
    xbox_InputKeyEvent(0x1B, 1);
    advance(16);
    st = poll0();
    CHECK(!enhance_menu_is_open());
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    xbox_InputKeyEvent(0x1B, 0);
    advance(16);
    st = poll0();
    advance(1100);
    st = poll0();
    CHECK(!enhance_menu_is_open());
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    hold_a(0);
    advance(16);
    poll0();
    hold_a(1);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    hold_a(0);
    advance(16);
    poll0();

    /* F10 again opens, and F10 while open closes. */
    open_menu();
    CHECK(enhance_menu_is_open());
    open_menu();
    CHECK(!enhance_menu_is_open());
}

/* ---- 3. Mods ------------------------------------------------------------- */

static void test_mods(void)
{
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(row_value("testmod") && !strcmp(row_value("testmod"), "true"));
    CHECK(row_value("plainmod") && !strcmp(row_value("plainmod"), "true"));
    CHECK(xbox_mod_has_on_enable(0) && !xbox_mod_has_on_enable(1));

    /* Left toggles off: recorded now, on_enable at the next frame. */
    CHECK(enhance_menu_select_row("testmod"));
    tap(0x25);
    CHECK(!xbox_mod_enabled(0));
    CHECK(s_enable_calls == 0);
    s_in_frame = 1;
    xbox_mod_frame();
    s_in_frame = 0;
    CHECK(s_enable_calls == 1 && s_enable_last == 0 && s_enable_thread_frame);
    CHECK(!strcmp(row_value("testmod"), "false"));
    CHECK(enhance_menu_unsaved() == 1);

    /* Enter lists the mod's own items. */
    tap(0x0D);
    CHECK(row_value("Enabled") && !strcmp(row_value("Enabled"), "false"));
    CHECK(row_value("Level") && !strcmp(row_value("Level"), "low"));
    CHECK(enhance_menu_select_row("Enabled"));
    tap(0x0D);
    xbox_mod_frame();
    CHECK(s_enable_calls == 2 && s_enable_last == 1);
    CHECK(enhance_menu_select_row("Level"));
    tap(0x27);
    CHECK(!strcmp(row_value("Level"), "high"));
    /* Back leaves the sub-list, not the menu. */
    tap(0x08);
    CHECK(enhance_menu_is_open());
    CHECK(row_value("plainmod") != NULL);

    /* A mod without on_enable: the toggle is for the next start. */
    CHECK(enhance_menu_select_row("plainmod"));
    tap(0x27);
    CHECK(xbox_mod_state(1) == XBOX_MOD_RESTART);
    CHECK(strstr(enhance_menu_status(), "applies at restart") != NULL);
    open_menu();
}

/* A descriptor mod's toggle is its enable setting (mod-settings D4): Save
 * writes that key with the choice text, never mods."<id>", and turning it
 * on drops a stale mods."<id>" = false that would hold it off again. */
static const char *const k_dm_offon[] = { "off", "on", NULL };
static const struct xbox_mod_setting k_dm_set[] = {
    { "enabled", "dmod.enabled", NULL, NULL, "Descriptor mod", NULL, NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, k_dm_offon },
};
static const struct xbox_mod_desc k_dm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "Descriptor mod", NULL, NULL, NULL, k_dm_set, 1, NULL, 0
};
static void dm_enable(int on, void *u) { (void)u; (void)on; }
static int dm_init(const struct xbox_mod_api *api)
{
    return api->on_enable(dm_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_dm = { "dmod", dm_init, NULL, &k_dm_desc };
static const struct xbox_mod *const k_dmods[] = { &k_dm, NULL };

static int pending_has(const char *key, const char *val)
{
    const char *keys[16], *vals[16];
    int n = enhance_menu_pending(keys, vals, 16);
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key))
            return val ? vals[i] && !strcmp(vals[i], val) : vals[i] == NULL;
    return 0;
}

static int pending_any(const char *key)
{
    const char *keys[16], *vals[16];
    int n = enhance_menu_pending(keys, vals, 16);
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key))
            return 1;
    return 0;
}

static void test_mods_desc_toggle(void)
{
    const char *f;
    start_with(ARMED, k_dmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(row_value("Descriptor mod") && !strcmp(row_value("Descriptor mod"), "false"));
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x27);
    xbox_mod_frame();
    CHECK(xbox_mod_enabled(0));
    CHECK(pending_has("dmod.enabled", "\"on\""));
    CHECK(!pending_any("mods.dmod"));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "[dmod]\nenabled = \"on\"") != NULL);
    CHECK(strstr(f, "dmod = ") == NULL);
    open_menu();

    /* Back off is the stock: the key goes, not a mods.dmod = false. */
    start_with(ARMED "[dmod]\nenabled = \"on\"\n", k_dmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(!strcmp(row_value("Descriptor mod"), "true"));
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x25);
    xbox_mod_frame();
    CHECK(pending_has("dmod.enabled", NULL));
    CHECK(!pending_any("mods.dmod"));
    open_menu();

    /* A stale switch off in the file goes when the toggle turns on. */
    start_with(ARMED "[mods]\ndmod = false\n", k_dmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x27);
    xbox_mod_frame();
    CHECK(pending_has("dmod.enabled", "\"on\""));
    CHECK(pending_has("mods.dmod", NULL));
    open_menu();
}

/* A chord already bound in another order or case is the same chord (N5):
 * capturing it again adds nothing. */
static void test_capture_duplicate(void)
{
    start_with(ARMED "[input.actions]\n\"testmod.poke\" = [\"Left_Stick_Click+BACK@400\"]\n", k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(enhance_menu_select_row("Poke"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    advance(16);
    poll0();
    advance(450);
    poll0();
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "is bound already") != NULL);
    /* The table parses pad names in lowercase only, so the row says this
     * one never fires (mod-settings B.4). */
    CHECK(!strcmp(row_value("Poke"), "Left_Stick_Click+BACK@400 (refused)"));
    CHECK(enhance_menu_unsaved() == 0);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();
    open_menu();
}

/* A key another action owns is refused, naming the owner: the
 * Deck grip Steam Input sends as F10 must not land on the stats strip
 * while F10 still opens the menu, or one press fires both. The capture
 * itself fires nothing, so the menu stays open through it. */
static void test_capture_owned(void)
{
    char before[128];
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(enhance_menu_select_row("Stats strip"));
    CHECK(row_value("Stats strip") != NULL);
    snprintf(before, sizeof before, "%s", row_value("Stats strip") ? s_val : "");
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(VK_F10);
    CHECK(!enhance_menu_capturing());
    CHECK(enhance_menu_is_open());
    CHECK(strstr(enhance_menu_status(), "F10 refused: Open the menu has it (F10); clear it there first") != NULL);
    CHECK(row_value("Stats strip") && !strcmp(s_val, before));
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());
}

/* ---- 4. Controls: capture, refusal, clear; 5. Save ----------------------- */

static void test_capture_and_save(void)
{
    const char *f;
    start_with(ARMED "[input]\nwalk_key = \"F8\"\n", k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(row_value("Poke") && !strcmp(row_value("Poke"), "F6"));

    /* A key: the Enter that started it goes first, then F7. */
    CHECK(enhance_menu_select_row("Poke"));
    xbox_InputKeyEvent(0x0D, 1);
    advance(16);
    poll0();
    CHECK(enhance_menu_capturing());
    advance(16);
    poll0();
    CHECK(enhance_menu_capturing());
    xbox_InputKeyEvent(0x0D, 0);
    advance(16);
    poll0();
    tap(VK_F7);
    CHECK(!enhance_menu_capturing());
    CHECK(!strcmp(row_value("Poke"), "F6, F7"));
    CHECK(enhance_menu_unsaved() == 1);
    /* Live at once: F7 pokes while the menu is shut. */

    /* A pad chord held 400 ms. */
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    advance(16);
    poll0();
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    advance(16);
    poll0();
    advance(200);
    poll0();
    CHECK(enhance_menu_capturing());
    advance(250);
    poll0();
    CHECK(!enhance_menu_capturing());
    CHECK(!strcmp(row_value("Poke"), "F6, F7, back+left_stick_click@400"));
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();

    /* Refused: the walk key. */
    tap(0x0D);
    tap(0x77);  /* F8 */
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "F8 refused") != NULL);
    CHECK(!strcmp(row_value("Poke"), "F6, F7, back+left_stick_click@400"));
    /* Esc cancels. */
    tap(0x0D);
    tap(0x1B);
    CHECK(!enhance_menu_capturing());
    CHECK(enhance_menu_is_open());
    CHECK(strstr(enhance_menu_status(), "cancelled") != NULL);
    /* menu.open keeps a binding. */
    CHECK(enhance_menu_select_row("Open the menu"));
    tap(0x2E);
    CHECK(strstr(enhance_menu_status(), "keeps a binding") != NULL);
    CHECK(!strcmp(row_value("Open the menu"), "F10, guide+a, paddle1"));

    /* An item, then one moved and moved back: neither that nor the
     * untouched keys reach the file. */
    CHECK(enhance_menu_set_page("Render"));
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x27);
    tap(0x25);
    {
        const char *keys[16], *vals[16];
        int n = enhance_menu_pending(keys, vals, 16);
        CHECK(n == 2);
        CHECK(n >= 1 && !strcmp(keys[0], "present.filter") && vals[0]
              && !strcmp(vals[0], "\"linear\""));
        CHECK(n >= 2 && !strcmp(keys[1], "input.actions.testmod.poke") && vals[1]
              && !strcmp(vals[1], "[\"F6\", \"F7\", \"back+left_stick_click@400\"]"));
    }
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 2 changes") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "filter = \"linear\"") != NULL);
    CHECK(strstr(f, "\"testmod.poke\" = [\"F6\", \"F7\", \"back+left_stick_click@400\"]") != NULL);
    CHECK(strstr(f, "scale") == NULL);
    CHECK(strstr(f, "walk_key = \"F8\"") != NULL);
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Nothing to save") != NULL);
    open_menu();

    /* The next start reads them back. */
    start_with(read_file("config/config.toml"), k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(row_value("Poke") && !strcmp(row_value("Poke"), "F6, F7, back+left_stick_click@400"));
    CHECK(enhance_menu_set_page("Render"));
    CHECK(row_value("Scaling filter") && !strcmp(row_value("Scaling filter"), "linear"));

    /* Back to the default: the key leaves the file. */
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x25);
    CHECK(!strcmp(row_value("Scaling filter"), "nearest"));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "filter") == NULL);
    CHECK(strstr(f, "testmod.poke") != NULL);

    /* Reset asks first, then puts the binding back to its defaults. */
    CHECK(enhance_menu_set_page("Save"));
    CHECK(enhance_menu_select_row("Reset to defaults"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Reset everything? A / Enter: yes"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Reset to defaults") != NULL);
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "testmod.poke") == NULL);
    CHECK(strstr(f, "walk_key = \"F8\"") != NULL);
    open_menu();

    /* RECOMP_CONFIG=none: Save is off. */
    set_env("RECOMP_CONFIG", "none");
    set_env("RECOMP_MENU", "1");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Save is off") != NULL);
    open_menu();
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_CONFIG", NULL);
}

/* ---- 6. the Mouse group ---------------------------------------------------- */

static void test_mouse(void)
{
    char l[96], v[128];
    int n, mouse = -1;
    start_with(ARMED, NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    n = enhance_menu_rows();
    for (int i = 0; i < n; i++) {
        int sel;
        enhance_menu_row(i, l, sizeof l, v, sizeof v, &sel);
        if (!strcmp(l, "Mouse")) {
            mouse = i;
            CHECK(!sel);
        }
    }
    CHECK(mouse > 0);
    /* The group is last: the capture choice, the release key, the free
     * keys and the state. */
    enhance_menu_row(mouse + 1, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Capture") && !strcmp(v, "focus"));
    enhance_menu_row(mouse + 2, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Release key") && !strcmp(v, "F9"));
    enhance_menu_row(mouse + 3, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Free keys"));
    enhance_menu_row(mouse + 4, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "State") && !strcmp(v, "free: F9"));
    CHECK(mouse + 5 == n);

    /* The choice: focus -> never -> click, applied through the options. */
    CHECK(enhance_menu_select_row("Capture"));
    tap(0x27);
    CHECK(!strcmp(row_value("Capture"), "never"));
    tap(0x27);
    CHECK(!strcmp(row_value("Capture"), "click"));
    {
        xbox_mouse_opts o;
        xbox_InputMouseGetOpts(&o);
        CHECK(o.capture == XBOX_MOUSE_CAPTURE_CLICK);
    }
    CHECK(!strcmp(row_value("State"), "free: click to capture"));
    {
        const char *keys[4], *vals[4];
        CHECK(enhance_menu_pending(keys, vals, 4) == 1);
        CHECK(!strcmp(keys[0], "input.mouse_capture") && !strcmp(vals[0], "\"click\""));
    }
    open_menu();
}

/* ---- 7. the miss snapshot ------------------------------------------------- */

static void test_snapshot(void)
{
    struct nv2a_tex_pack_miss m[4];
    start_with(ARMED, NULL);
    nv2a_tex_pack_miss(0x1111, 64, 64, 0x0C, 1);
    /* Nobody shows it: the flip takes no copy. */
    nv2a_tex_pack_snapshot();
    CHECK(nv2a_tex_pack_misses_snapshot(m, 4) == 0);
    open_menu();
    CHECK(enhance_menu_set_page("Pack"));
    nv2a_tex_pack_miss(0x2222, 128, 32, 0x12, 1);
    nv2a_tex_pack_miss(0x2222, 128, 32, 0x12, 1);
    nv2a_tex_pack_snapshot();
    CHECK(nv2a_tex_pack_misses_snapshot(m, 4) == 2);
    CHECK(m[0].hash == 0x2222 && m[0].count == 2 && m[1].hash == 0x1111);
    CHECK(row_value("No pack loaded") != NULL);
    CHECK(row_value("Misses") && !strcmp(row_value("Misses"), "3"));
    CHECK(row_value("0000000000002222") != NULL
          && strstr(row_value("0000000000002222"), "128x32") != NULL);
    open_menu();
    /* Closed: later misses are not copied. */
    nv2a_tex_pack_miss(0x3333, 8, 8, 0x0C, 1);
    nv2a_tex_pack_snapshot();
    CHECK(nv2a_tex_pack_misses_snapshot(m, 4) == 2);
}

/* ---- 8. each page's draw list -------------------------------------------- */

/* ---- 3b. a descriptor mod's page (mod-settings D5) ----------------------- */

static const char *const k_bm_hud[] = { "keep", "hide", NULL };
static const char *const k_bm_speed_alias[] = { "bmod_old_speed", NULL };
static const struct xbox_mod_setting k_bm_set[] = {
    { "enabled", "bmod.enabled", NULL, NULL, "Enabled", NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 1, 0, 0, 0, NULL },
    { "speed", "bmod.speed", NULL, k_bm_speed_alias, "Speed",
      "Units per second, times the base speed.", "x",
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0.05, 20, 0.05, NULL },
    { "hud", "bmod.hud", NULL, NULL, "HUD while flying", "Keep or hide the HUD.", NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE_ACTIVATE, 0, 0, 0, 0, 0, k_bm_hud },
    { "quality", "bmod.quality", NULL, NULL, "Quality", NULL, NULL,
      XBOX_SET_INT, ENHANCE_LIVE_RESTART, 0, 2, 0, 4, 1, NULL },
    { "pinned", "bmod.pinned", "RECOMP_HEAP_QUARANTINE", NULL, "Pinned", NULL, NULL,
      XBOX_SET_INT, ENHANCE_LIVE, 0, 0, 0, 100, 1, NULL },
};
static const char *const k_bm_toggle_def[] = { "F7", NULL };
static const char *const k_bm_rise_def[] = { "rt", "x", "R", NULL };
static const char *const k_bm_move_def[] = { "left_stick", NULL };
static const char *const k_bm_mouse_def[] = { "Look", NULL };
static const struct xbox_mod_control k_bm_ctl[] = {
    { "toggle", "Toggle flight", "Start or stop flying.", NULL, XBOX_CTL_ACTION, k_bm_toggle_def,
      NULL, NULL },
    { "rise", "Rise", NULL, "while flying", XBOX_CTL_BUTTON, k_bm_rise_def, NULL, NULL },
    { "move", "Move", NULL, "while flying", XBOX_CTL_STICK, k_bm_move_def, NULL, NULL },
    { "mouse", "Mouse", NULL, "while flying", XBOX_CTL_INFO, k_bm_mouse_def, NULL, NULL },
};
#define BM_SUMMARY "Fly a detached camera around the stage. The game pauses its camera " \
                   "while you fly, and the HUD can hide so the shot is clean."
static const struct xbox_mod_desc k_bm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "B mod", BM_SUMMARY, "The test suite", "1.0",
    k_bm_set, 5, k_bm_ctl, 4
};
static void bm_enable(int on, void *u) { (void)u; (void)on; }
static int bm_init(const struct xbox_mod_api *api)
{
    return api->on_enable(bm_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_bm = { "bmod", bm_init, NULL, &k_bm_desc };

/* Refused at validation (a second "speed"), its tables fine: the page
 * still lists what it offers, marked unavailable. */
static const struct xbox_mod_setting k_fm_set[] = {
    { "speed", "fmod.speed", NULL, NULL, "Speed", NULL, NULL,
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0, 2, 0, NULL },
    { "speed", "fmod.speed2", NULL, NULL, "Speed again", NULL, NULL,
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0, 2, 0, NULL },
};
static const char *const k_fm_def[] = { "F4", NULL };
static const struct xbox_mod_control k_fm_ctl[] = {
    { "go", "Go", NULL, NULL, XBOX_CTL_ACTION, k_fm_def, NULL, NULL },
    { "aim", "Aim", NULL, NULL, XBOX_CTL_INFO, k_bm_mouse_def, NULL, NULL },
};
static const struct xbox_mod_desc k_fm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "Failing mod", "Never runs.", NULL, NULL, k_fm_set, 2, k_fm_ctl, 2
};
static const struct xbox_mod k_fm = { "fmod", bm_init, NULL, &k_fm_desc };
static const struct xbox_mod *const k_bmods[] = { &k_tm, &k_bm, &k_fm, NULL };

static int row_index(const char *label)
{
    char l[96];
    for (int i = 0; i < enhance_menu_rows(); i++) {
        enhance_menu_row(i, l, sizeof l, NULL, 0, NULL);
        if (!strcmp(l, label))
            return i;
    }
    return -1;
}

/* The row after the one labelled label (the next row of a group). */
static const char *row_after(const char *label, int k)
{
    int i = row_index(label);
    if (i < 0 || !enhance_menu_row(i + k, NULL, 0, s_val, sizeof s_val, NULL))
        return NULL;
    return s_val;
}

static void open_detail(const char *mod)
{
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row(mod));
    tap(0x0D);
}

static void test_detail_rows(void)
{
    int i;
    set_env("RECOMP_HEAP_QUARANTINE", "7");
    start_with(ARMED "[bmod]\ntypo = 1\n", k_bmods);
    set_env("RECOMP_HEAP_QUARANTINE", NULL);
    open_menu();
    open_detail("B mod");
    /* The header, then Author, About (wrapped), State and Enabled. */
    CHECK(enhance_menu_row(0, NULL, 0, s_val, sizeof s_val, NULL) && !strcmp(s_val, "v1.0"));
    CHECK(row_value("B mod") != NULL);
    CHECK(row_value("Author") && !strcmp(s_val, "The test suite"));
    CHECK(row_value("About") != NULL);
    CHECK(row_value("State") && !strcmp(s_val, "On"));
    CHECK(row_value("Enabled") && !strcmp(s_val, "true"));
    /* No selectable text: the first stop is Enabled. */
    CHECK(!strcmp(selected_label(), "Enabled"));

    /* Controls: the action's bindings, the captured ones with the keys the
     * guest names come from, the info row; each with its note. */
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7"));
    i = row_index("Toggle flight");
    CHECK(i >= 0 && !strcmp(enhance_menu_row_help(i), "Start or stop flying."));
    /* Two guest bindings and a host key: one separator each, none left
     * by the host key (the test's input map gives RT 3 and X C). */
    CHECK(row_value("Rise") && !strcmp(s_val, "rt, x, R (keys 3, C)"));
    CHECK(row_value("Move") && !strncmp(s_val, "left_stick", 10));
    CHECK(row_value("Mouse") && !strcmp(s_val, "Look"));
    i = row_index("Rise");
    CHECK(i >= 0 && strstr(enhance_menu_row_help(i), "[input.controls] \"bmod.rise\"") != NULL);
    CHECK(enhance_menu_select_row("Rise"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "read-only for now") != NULL);

    /* Settings: each class as the descriptor gives it, the forced one
     * read-only with its env row named. */
    CHECK(row_value("Speed") && !strcmp(s_val, "1.00"));
    CHECK(enhance_menu_row_live(row_index("Speed")) == ENHANCE_LIVE);
    CHECK(!strcmp(enhance_menu_row_help(row_index("Speed")),
                  "Units per second, times the base speed."));
    CHECK(row_value("HUD while flying") && !strcmp(s_val, "keep"));
    CHECK(enhance_menu_row_live(row_index("HUD while flying")) == ENHANCE_LIVE_ACTIVATE);
    CHECK(row_value("Quality") && !strcmp(s_val, "2"));
    CHECK(enhance_menu_row_live(row_index("Quality")) == ENHANCE_LIVE_RESTART);
    CHECK(row_value("Pinned") && !strcmp(s_val, "7"));
    CHECK(enhance_menu_select_row("Pinned"));
    tap(0x27);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_HEAP_QUARANTINE") != NULL);
    CHECK(row_value("Pinned") && !strcmp(s_val, "7"));
    CHECK(!pending_any("bmod.pinned"));
    CHECK(row_index("Reset this mod") >= 0);
    /* The key under the mod's name nothing reads, kept. */
    CHECK(row_value("Unknown keys") != NULL);
    CHECK(row_value("bmod.typo") && !strcmp(s_val, "not read; kept"));

    /* A change moves the setting now and stages its key, written with the
     * kind's literal. */
    CHECK(enhance_menu_select_row("Speed"));
    tap(0x27);
    CHECK(row_value("Speed") && !strcmp(s_val, "1.05"));
    CHECK(xbox_mod_setting_value(1, 1) > 1.04 && xbox_mod_setting_value(1, 1) < 1.06);
    CHECK(pending_has("bmod.speed", "1.05"));
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x25);
    CHECK(enhance_menu_unsaved() == 0);

    /* Back returns to the list, on the mod's row. */
    tap(0x08);
    CHECK(enhance_menu_is_open() && !strcmp(selected_label(), "B mod"));

    /* The refused mod: its reason, every row unavailable, no Reset. */
    open_detail("Failing mod");
    CHECK(row_value("State") && !strncmp(s_val, "Failed: ", 8));
    CHECK(row_value("Go") && !strcmp(s_val, "unavailable"));
    CHECK(row_value("Aim") && !strcmp(s_val, "unavailable"));
    CHECK(row_value("Speed") && !strcmp(s_val, "unavailable"));
    CHECK(row_index("Reset this mod") < 0);
    tap(0x08);

    /* A mod with no descriptor keeps its name, State, Enabled and items. */
    open_detail("testmod");
    CHECK(row_value("testmod") != NULL);
    CHECK(row_value("State") && !strcmp(s_val, "On"));
    CHECK(row_value("Level") && !strcmp(s_val, "low"));
    CHECK(row_index("Controls") < 0);
    open_menu();
}

/* About wraps at the word that no longer fits, at any panel width: the
 * lines put back together are the summary, and a narrower panel takes
 * more of them. */
static int wrap_lines(int w, char *joined, size_t n)
{
    int i, lines = 0;
    size_t at = 0;
    char l[96];
    nv2a_overlay_set_placement(0, 0, w, 1080);
    open_menu();
    open_detail("B mod");
    i = row_index("About");
    joined[0] = 0;
    for (; i >= 0 && enhance_menu_row(i, l, sizeof l, s_val, sizeof s_val, NULL); i++) {
        if (lines && l[0])
            break;
        at += (size_t)snprintf(joined + at, n - at, "%s%s", lines ? " " : "", s_val);
        lines++;
    }
    open_menu();
    return lines;
}

static void test_detail_wrap(void)
{
    char a[512], b[512], c[512];
    int la, lb, lc;
    start_with(ARMED, k_bmods);
    la = wrap_lines(1920, a, sizeof a);
    lb = wrap_lines(1400, b, sizeof b);
    lc = wrap_lines(1100, c, sizeof c);
    CHECK(la >= 2 && lb > la && lc > lb);
    CHECK(!strcmp(a, BM_SUMMARY) && !strcmp(b, BM_SUMMARY) && !strcmp(c, BM_SUMMARY));
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
}

/* Reset this mod stages the mod's own settings and bindings back to the
 * stock; its switch, other mods and the unknown key are left alone. */
static void test_detail_reset(void)
{
    const char *keys[16], *vals[16], *f;
    int n;
    start_with(ARMED "[bmod]\nspeed = 2.0\nhud = \"hide\"\ntypo = 1\n[testmod]\nlevel = \"high\"\n"
                     "[input.actions]\n\"bmod.toggle\" = [\"F8\"]\n",
               k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("Speed") && !strcmp(s_val, "2.00"));
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F8"));
    CHECK(enhance_menu_select_row("Reset this mod"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "B mod: reset to defaults") != NULL);
    CHECK(row_value("Speed") && !strcmp(s_val, "1.00"));
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7"));
    n = enhance_menu_pending(keys, vals, 16);
    /* The patch, by hand: the three keys removed, nothing else. */
    CHECK(n == 3);
    CHECK(pending_has("bmod.speed", NULL) && pending_has("bmod.hud", NULL));
    CHECK(pending_has("input.actions.bmod.toggle", NULL));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "speed") == NULL && strstr(f, "hud") == NULL && strstr(f, "F8") == NULL);
    CHECK(strstr(f, "typo = 1") != NULL && strstr(f, "level = \"high\"") != NULL);
    open_menu();
}

/* A user-file alias goes under its key at the first Save, edits or none;
 * Enabled turned on drops a mods."<id>" = false. */
static void test_detail_save(void)
{
    const char *f;
    start_with("bmod_old_speed = 3.0\n" ARMED, k_bmods);
    open_menu();
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(pending_has("bmod.speed", "3.0") && pending_has("bmod_old_speed", NULL));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "bmod_old_speed") == NULL && strstr(f, "speed = 3.0") != NULL);
    CHECK(!pending_any("bmod.speed"));
    open_menu();

    start_with(ARMED "[mods]\nbmod = false\n", k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("State") && !strcmp(s_val, "Off (mods.\"bmod\" = false)"));
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7 (mod off)"));
    CHECK(enhance_menu_select_row("Enabled"));
    tap(0x27);
    xbox_mod_frame();
    CHECK(pending_has("mods.bmod", NULL));
    open_menu();

    /* The player edits the new key while the old one is still in the
     * file: the edit wins, the alias's value is not written over it, and
     * the old key goes. */
    start_with("bmod_old_speed = 3.0\n" ARMED, k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("Speed") && !strcmp(s_val, "3.00"));
    CHECK(enhance_menu_select_row("Speed"));
    tap(0x27);
    CHECK(pending_has("bmod.speed", "3.05") && pending_has("bmod_old_speed", NULL));
    CHECK(!pending_has("bmod.speed", "3.0"));
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 change, 1 renamed key") != NULL);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "bmod_old_speed") == NULL && strstr(f, "speed = 3.05") != NULL);
    CHECK(strstr(f, "speed = 3.0\n") == NULL);
    open_menu();

    /* An alias holding the stock: the old key goes, nothing is written
     * under the new one (no key the player never set). */
    start_with("bmod_old_speed = 1.0\n" ARMED, k_bmods);
    open_menu();
    CHECK(!pending_any("bmod.speed") && pending_has("bmod_old_speed", NULL));
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 renamed key") != NULL);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "speed") == NULL);
    open_menu();
}

/* The capture refuses an input a dormant action holds, naming the mod and
 * the label, and a key a mod's captured control names (D3 rule 2). */
static void test_detail_conflicts(void)
{
    start_with(ARMED "[bmod]\nenabled = false\n", k_bmods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(enhance_menu_select_row("Stats strip"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(0x76);  /* F7 */
    CHECK(strstr(enhance_menu_status(),
                 "F7 refused: B mod: Toggle flight has it (F7) (mod off); clear it there first")
          != NULL);
    CHECK(enhance_menu_select_row("Stats strip"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap('R');
    CHECK(strstr(enhance_menu_status(), "R refused: B mod: Rise uses it") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
}

/* The Controls page: the toolkit's actions, then each mod's under its
 * title, a dormant mod's group saying so; Mouse last. */
static void test_controls_groups(void)
{
    char l[96];
    int t, m, b, mo;
    start_with(ARMED "[bmod]\nenabled = false\n", k_bmods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    t = row_index("Toolkit");
    m = row_index("testmod");
    b = row_index("B mod (mod off)");
    mo = row_index("Mouse");
    CHECK(t == 0 && m > t && b > m && mo > b);
    CHECK(row_index("Game") < 0);
    CHECK(enhance_menu_row(t + 1, l, sizeof l, NULL, 0, NULL) && !strcmp(l, "Open the menu"));
    CHECK(enhance_menu_row(m + 1, l, sizeof l, NULL, 0, NULL) && !strcmp(l, "Poke"));
    CHECK(enhance_menu_row(b + 1, l, sizeof l, NULL, 0, NULL) && !strcmp(l, "Toggle flight"));
    /* Captured controls show on the mod's own page only; the group
     * header says the mod is off, not each row. */
    CHECK(row_index("Rise") < 0);
    CHECK(row_after("B mod (mod off)", 1) && !strcmp(s_val, "F7"));
    open_menu();

    /* An action the title registers with no owner: its own group, between
     * the toolkit's and the mods'. */
    {
        static const char *const k_jump[] = { "F12", NULL };
        int g;
        start_with(ARMED, k_bmods);
        CHECK(enhance_action_register("game.jump", "Jump", k_jump) >= 0);
        open_menu();
        CHECK(enhance_menu_set_page("Controls"));
        t = row_index("Toolkit");
        g = row_index("Game");
        m = row_index("testmod");
        b = row_index("B mod");
        CHECK(t == 0 && g > t && m > g && b > m);
        CHECK(enhance_menu_row(g + 1, l, sizeof l, s_val, sizeof s_val, NULL) && !strcmp(l, "Jump")
              && !strcmp(s_val, "F12"));
        open_menu();
    }
}

static uint32_t sum_of(const struct nv2a_ovl_list *l)
{
    return nv2a_overlay_list_sum(l->vtx, l->nvtx, l->idx, l->nidx, l->cmd, l->ncmd);
}

static void test_pages(void)
{
    /* Render, Pack, Game, Mods, Stats, Controls, Debug, Save. */
    static const uint32_t k_sum[8] = {
        0x5220FD06u, 0xC5E14D56u, 0xC918F275u, 0xEF138353u,
        0x7B20659Eu, 0x13A93A10u, 0x2DA2FE24u, 0x4D09DFE6u };
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_is_open());
    for (int p = 0; p < enhance_menu_page_count(); p++) {
        const struct nv2a_ovl_list *l;
        uint32_t want = p < 8 ? k_sum[p] : 0;   /* a ninth page lands unpinned */
        CHECK(enhance_menu_set_page(enhance_menu_page_name(p)));
        enhance_menu_draw(100000);
        l = nv2a_overlay_acquire(NV2A_OVL_MENU);
        CHECK(l != NULL);
        if (!l)
            continue;
        CHECK(l->ncmd > 0 && l->sum == sum_of(l));
        /* A page with no pin fails: --pin prints every sum to pin a new
         * or changed page. The sum is over positions quantised to 1/16 px
         * (nv2a_overlay_list_sum), so a compiler's last float bit or a
         * host's libm does not move it; only a layout change does. */
        if (s_pin)
            printf("page %s = 0x%08X\n", enhance_menu_page_name(p), l->sum);
        else if (l->sum != want) {
            s_fail++;
            fprintf(stderr, "FAIL page %s: 0x%08X, want 0x%08X%s\n", enhance_menu_page_name(p),
                    l->sum, want, want ? "" : " (unpinned: run with --pin)");
        }
    }
    open_menu();
    CHECK(!(nv2a_overlay_visible() & (1u << NV2A_OVL_MENU)));
}

/* Save under a root whose folders do not exist yet makes all of them (N7). */
static void test_save_deep(void)
{
    const char *f;
    remove("deep/a/b/config/config.toml");
    rmdir("deep/a/b/config");
    rmdir("deep/a/b");
    rmdir("deep/a");
    rmdir("deep");
    set_env("RECOMP_MENU", "1");
    start_with(NULL, NULL);
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    capture_begin();
    xbox_enhance_init("deep/a/b", NULL);
    capture_end();
    poll0();
    open_menu();
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 change") != NULL);
    f = read_file("deep/a/b/config/config.toml");
    CHECK(strstr(f, "filter = \"linear\"") != NULL);
    open_menu();
    set_env("RECOMP_MENU", NULL);
    remove("deep/a/b/config/config.toml");
    rmdir("deep/a/b/config");
    rmdir("deep/a/b");
    rmdir("deep/a");
    rmdir("deep");
}

/* A Save that leaves keys out for [table]s in a config under a long path
 * (a macOS ~/Library/Application Support/... one) names every key in the
 * log, and the status line gives the count. */
#define LONGROOT "lr_Library_Application_Support_with_a_long_user_name_and_a_game_folder" \
                 "_name_that_runs_on_well_past_what_the_old_buffer_held_xx"
static void test_save_tables(void)
{
    const char *f, *log;
    MKDIR(LONGROOT);
    MKDIR(LONGROOT "/config");
    write_file(LONGROOT "/config/config.toml",
               ARMED "[mods.testmod]\nx = 1\n[mods.plainmod]\ny = 1\n");
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    nv2a_overlay_reset_for_test();
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeysClear();
    pad_clear();
    xbox_mod_register_table(k_mods);
    capture_begin();
    xbox_enhance_init(LONGROOT, NULL);
    capture_end();
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    poll0();
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row("testmod"));
    tap(0x25);
    CHECK(enhance_menu_select_row("plainmod"));
    tap(0x27);
    CHECK(enhance_menu_set_page("Render"));
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    capture_begin();
    tap(VK_F5);
    log = capture_end();
    CHECK(strstr(enhance_menu_status(), "Saved; 2 keys left out") != NULL);
    CHECK(strstr(log, "2 keys left out") != NULL);
    CHECK(strstr(log, "mods.testmod is the table [mods.testmod]") != NULL);
    CHECK(strstr(log, "; mods.plainmod is the table [mods.plainmod]; write its keys flat"
                      " under [mods] (left as it is)\n") != NULL);
    f = read_file(LONGROOT "/config/config.toml");
    CHECK(strstr(f, "filter = \"linear\"") != NULL);
    CHECK(strstr(f, "testmod = ") == NULL && strstr(f, "plainmod = ") == NULL);
    open_menu();
    remove(LONGROOT "/config/config.toml");
    rmdir(LONGROOT "/config");
    rmdir(LONGROOT);
}

/* A title's STAGE item keeps its row as before the descriptor classes:
 * its label, its value and its class. */
static const char *const k_gm_lv[] = { "near", "far", NULL };
static const struct enhance_item k_gm_stage = {
    "fx.lod", "Draw distance", "Game", ENHANCE_ITEM_CHOICE, k_gm_lv, 0, 0, 0, NULL,
    NULL, NULL, ENHANCE_LIVE_STAGE, 0, "near", NULL
};
static void test_stage_row(void)
{
    int i;
    start_with(ARMED, NULL);
    CHECK(enhance_menu_add(&k_gm_stage));
    open_menu();
    CHECK(enhance_menu_set_page("Game"));
    i = row_index("Draw distance");
    CHECK(i >= 0 && enhance_menu_row_live(i) == ENHANCE_LIVE_STAGE);
    CHECK(row_value("Draw distance") && !strcmp(s_val, "near"));
    CHECK(enhance_menu_select_row("Draw distance"));
    tap(0x27);
    CHECK(row_value("Draw distance") && !strcmp(s_val, "far"));
    CHECK(pending_has("fx.lod", "\"far\""));
    CHECK(enhance_menu_row_live(-1) == -1);
    open_menu();
}

int main(int argc, char **argv)
{
    s_pin = argc > 1 && !strcmp(argv[1], "--pin");
    MKDIR("config");
    set_env("RECOMP_CONFIG", NULL);
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_TRACE", NULL);
    set_env("RECOMP_PB_BACKEND", NULL);
    test_start();
    test_navigate();
    test_mods();
    test_mods_desc_toggle();
    test_capture_and_save();
    test_capture_duplicate();
    test_capture_owned();
    test_detail_rows();
    test_detail_wrap();
    test_detail_reset();
    test_detail_save();
    test_detail_conflicts();
    test_controls_groups();
    test_mouse();
    test_snapshot();
    test_pages();
    test_save_deep();   /* after the pins: its Save moves the Debug page's options gen */
    test_save_tables();
    test_stage_row();
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    enhance_actions_reset();
    remove("config/config.toml");
    remove("menu_stderr.txt");
    printf("enhance_menu: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
