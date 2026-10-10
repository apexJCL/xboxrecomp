/**
 * Xbox Input Compatibility Layer
 *
 * The host backends behind input_core.c (input_backend.h): each one reads
 * a host pad and maps it to the Duke layout through the table in
 * input_map.c, so both hosts map controls the same way.
 *
 *   _WIN32 -> Windows XInput (also Proton/Wine)
 *   POSIX  -> SDL2 GameController
 */

#include "input_backend.h"
#include "recomp_env.h"
#include "input_map.h"
#include "keyboard.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

/* The keyboard on port 0 is shared with the SDL backend (keyboard.c); the
 * window procedures in fb_present.c and nv2a_pb_d3d11.c record the keys. */

_Static_assert(sizeof(XINPUT_GAMEPAD) == sizeof(XBOX_HOST_XINPUT_GAMEPAD)
               && offsetof(XINPUT_GAMEPAD, bLeftTrigger) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, bLeftTrigger)
               && offsetof(XINPUT_GAMEPAD, sThumbRY) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, sThumbRY),
               "XBOX_HOST_XINPUT_GAMEPAD must match XINPUT_GAMEPAD");

static DWORD xi_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    XINPUT_STATE xi;
    XBOX_HOST_XINPUT_GAMEPAD pad;
    DWORD rc = XInputGetState(port, &xi);

    if (rc != ERROR_SUCCESS)
        return rc;
    memcpy(&pad, &xi.Gamepad, sizeof pad);
    out->dwPacketNumber = xi.dwPacketNumber;
    xbox_InputMapXInput(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
}

static DWORD xi_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    XINPUT_VIBRATION xv;
    xv.wLeftMotorSpeed  = v->wLeftMotorSpeed;
    xv.wRightMotorSpeed = v->wRightMotorSpeed;
    return XInputSetState(port, &xv);
}

static uint64_t xi_now_ms(void) { return GetTickCount64(); }

/* Under Wine XInputGetState has been seen to return ERROR_SUCCESS and an
 * idle pad for an empty user index. If a Wine/Proton build does that, the
 * connection probe has to move to XInputGetCapabilities. */
const xbox_input_backend xbox_input_host_backend = {
    NULL, xi_get_state, xi_set_state, xbox_InputKeyboardPoll, xi_now_ms
};

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <time.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];

/* Set by the window (src/video/fb_present_sdl.c) when the process main thread
 * runs the SDL event loop. Its SDL_PumpEvents already updates the
 * controllers, so this side only reads them: one event loop, not two threads
 * racing to pump. */
static volatile int g_main_pump;
void xbox_InputSetMainPump(int on) { g_main_pump = on; }

/* Whether a controller is open, i.e. whether SDL may still be in use here. */
int xbox_InputAnyOpen(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            return 1;
    return 0;
}
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

_Static_assert(SDL_CONTROLLER_BUTTON_A == XBOX_SDL_BUTTON_A
               && SDL_CONTROLLER_BUTTON_START == XBOX_SDL_BUTTON_START
               && SDL_CONTROLLER_BUTTON_LEFTSHOULDER == XBOX_SDL_BUTTON_LEFTSHOULDER
               && SDL_CONTROLLER_BUTTON_DPAD_RIGHT == XBOX_SDL_BUTTON_DPAD_RIGHT,
               "XBOX_SDL_BUTTON_* must match SDL_GameControllerButton");
_Static_assert(SDL_CONTROLLER_AXIS_LEFTX == XBOX_SDL_AXIS_LEFTX
               && SDL_CONTROLLER_AXIS_RIGHTY == XBOX_SDL_AXIS_RIGHTY
               && SDL_CONTROLLER_AXIS_TRIGGERRIGHT == XBOX_SDL_AXIS_TRIGGERRIGHT,
               "XBOX_SDL_AXIS_* must match SDL_GameControllerAxis");

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. Hotplug
 * (SDL_CONTROLLERDEVICEADDED/REMOVED in the main loop) is not handled yet:
 * the controllers present at init are the ones used. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot])
            g_pads[slot] = SDL_GameControllerOpen(i);
        slot++;
    }
}

static void sdl_init(void)
{
    /* Game controllers pull in SDL events; keep SIGINT/SIGTERM default. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

static DWORD sdl_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    SDL_GameController *c = g_pads[port];
    XBOX_HOST_SDL_GAMEPAD pad;

    if (!c || !SDL_GameControllerGetAttached(c))
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_main_pump)
        SDL_GameControllerUpdate();

    for (int b = 0; b < XBOX_SDL_BUTTON_COUNT; b++)
        pad.button[b] = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b) ? 1 : 0;
    for (int a = 0; a < XBOX_SDL_AXIS_COUNT; a++)
        pad.axis[a] = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a);
    out->dwPacketNumber = ++g_packet[port];
    xbox_InputMapSdl(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
}

static DWORD sdl_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    SDL_GameController *c = g_pads[port];

    if (!c || !SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        return ERROR_DEVICE_NOT_CONNECTED;
    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). 0 stops it. */
    SDL_GameControllerRumble(c, v->wLeftMotorSpeed, v->wRightMotorSpeed,
                             (v->wLeftMotorSpeed || v->wRightMotorSpeed) ? 1000 : 0);
    return ERROR_SUCCESS;
}

static uint64_t sdl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const xbox_input_backend xbox_input_host_backend = {
    sdl_init, sdl_get_state, sdl_set_state, xbox_InputKeyboardPoll, sdl_now_ms
};

#endif /* _WIN32 */
