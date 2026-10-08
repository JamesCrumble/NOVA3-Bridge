/*
 * Input for N.O.V.A. 3, and the autopilot that proves the game advances.
 *
 * The control scheme is not invented here. This build ships a MOGA/PowerA
 * gamepad path in its own Java layer (GL2JNIActivity.a(KeyEvent), decompiled
 * from the donor APK), and that switch statement is the game's own answer to
 * "which number means which button":
 *
 *     dpad up/down/left/right   19 20 21 22        passed through
 *     BUTTON_A   (96)        -> 23                 KEYCODE_DPAD_CENTER
 *     BUTTON_B   (97)        -> 227                GL2JNIActivity.L
 *     BUTTON_X   (99)        -> 99
 *     BUTTON_Y   (100)       -> 100
 *     L1 (102) / R1 (103)    -> 102 / 103
 *     L2 (104) / R2 (105)    -> 102 / 103          the triggers alias the bumpers
 *     START      (108)       -> 108
 *     SELECT     (109)       -> 23 in a menu, 200 in play
 *
 * So the engine takes Android keycodes essentially unchanged, and the port's
 * job is to send the ones a gamepad would have sent. Inventing a mapping is
 * the mistake every port that tried it got wrong in a way a player noticed.
 *
 * Two engine switches have to be thrown for any of it to be read at all:
 * nativeSetIsMOGA(true) and nativePowerStatus(true). Without them this build
 * runs its touch-only path, and a port that sends perfectly correct keycodes
 * into it looks exactly like a port whose keycodes are wrong - which is the
 * failure that cost a sibling port three trips to the SD card.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "platform.h"
#include "so_util.h"
#include "jni.h"

#include "input_bridge.h"
#include <stdlib.h>

#include "port_env.h"
#include "gl_bridge.h"
#include "trace.h"

#include "nova3.h"

/* portbase's activity counter. Declared where it is used: the autopilot reads
 * it to tell "the picture changed" apart from "new content was loaded". */
extern "C" long android_gl_textures_uploaded(void);

/* ------------------------------------------------- the engine's entry points */

/*
 * The two joystick setters take floats and therefore need ABI_ATTR.
 *
 * The guest is a softfp build: it passes floats in core registers. This loader
 * is hardfp and passes them in VFP registers. A call through a plain pointer
 * puts the values where the game will not look, and what arrives is whatever
 * r0-r3 happened to hold - a stick that moves the camera to nowhere, with
 * nothing in any log to say why. pcs("aapcs") is what makes the call site
 * agree with the callee. This is the most expensive lesson the sibling port
 * paid for; see portbase/loader/platform.h.
 *
 * Everything else below takes ints or booleans, which both ABIs pass the same
 * way, so only these two carry the attribute.
 */
typedef void (*fn_key_t)(JNIEnv *, jclass, jint);
typedef void (*fn_touch_t)(JNIEnv *, jclass, jint, jint, jint, jint);
typedef void (*fn_bool_t)(JNIEnv *, jclass, jboolean);
typedef jboolean (*fn_query_t)(JNIEnv *, jclass);
typedef void (ABI_ATTR *fn_stick2_t)(JNIEnv *, jclass, jfloat, jfloat);
typedef void (ABI_ATTR *fn_stick3_t)(JNIEnv *, jclass, jfloat, jfloat, jfloat);

static fn_key_t    g_on_key_down;
static fn_key_t    g_on_key_up;
static fn_touch_t  g_touch_event;
static fn_bool_t   g_set_is_moga;
static fn_bool_t   g_set_xperia_play;
static fn_bool_t   g_set_so01d;
static fn_bool_t   g_power_status;
static fn_query_t  g_is_main_menu;
static fn_stick2_t g_left_stick;
static fn_stick3_t g_right_stick;

static JNIEnv *g_env;
static int     g_width  = 640;
static int     g_height = 480;

/* ------------------------------------------------------------ the keycodes */

enum {
    KEY_DPAD_UP     = 19,
    KEY_DPAD_DOWN   = 20,
    KEY_DPAD_LEFT   = 21,
    KEY_DPAD_RIGHT  = 22,
    KEY_DPAD_CENTER = 23,   /* accept, in menus (and R3 in the MOGA table) */
    KEY_BUTTON_A    = 124,  /* GL2JNIActivity.ac - what BUTTON_A becomes in play */
    KEY_B           = 227,  /* GL2JNIActivity.L */
    KEY_X           = 99,
    KEY_Y           = 100,
    KEY_L1          = 102,
    KEY_R1          = 103,
    KEY_START       = 108,
    KEY_SELECT_PLAY = 200,  /* SELECT outside a menu */
};

/*
 * SELECT is the one button whose meaning depends on where the engine is, and
 * the engine answers that question itself. Following its own Java rather than
 * picking one: in a menu SELECT is "accept", in play it is the 200 the engine
 * treats as a modifier.
 */
static int select_keycode(void)
{
    return nova3_is_main_menu() ? KEY_DPAD_CENTER : KEY_SELECT_PLAY;
}

static void send_key(int code, bool down)
{
    /* The first hardware trip had no per-event trace and the log could not
     * say which codes the engine received. Capped: enough to read a session,
     * not enough to drown it. */
    static int logged;
    if (logged < 200) {
        logged++;
        trace("input: key %d %s (menu=%d)", code, down ? "down" : "up",
              (int)nova3_is_main_menu());
    }
    if (down) {
        if (g_on_key_down)
            g_on_key_down(g_env, NULL, code);
    } else {
        if (g_on_key_up)
            g_on_key_up(g_env, NULL, code);
    }
}

/* ---------------------------------------------------------------- the cursor */

/*
 * A pointer the engine can be poked with.
 *
 * The touch path is how menus are driven on Android and it is the only way to
 * reach anything the gamepad path has no button for. portbase draws the
 * pointer and asks for its position through android_input_cursor_position;
 * everything below just keeps the coordinates and turns presses into
 * touchEvent calls.
 *
 * The action codes are MotionEvent's own - 0 down, 1 up, 2 move - which is
 * what the engine's Java passes through untouched. The donor also uses 100 and
 * 4 as synthetic values in the MOGA path, so those two are not free to reuse.
 */
static float g_cursor_x = 320.0f;
static float g_cursor_y = 240.0f;
static bool  g_cursor_down;
static int   g_cursor_visible = 1;

enum { TOUCH_DOWN = 0, TOUCH_UP = 1, TOUCH_MOVE = 2 };

static void send_touch(int action)
{
    static int logged;
    if (logged < 120) {
        logged++;
        trace("input: touch action=%d at %d,%d (menu=%d)", action,
              (int)g_cursor_x, (int)g_cursor_y, (int)nova3_is_main_menu());
    }
    if (!g_touch_event)
        return;
    g_touch_event(g_env, NULL, action, (jint)g_cursor_x, (jint)g_cursor_y, 0);
}

extern "C" void android_input_cursor_position(float *x, float *y, int *visible)
{
    if (x)       *x = g_cursor_x;
    if (y)       *y = g_cursor_y;
    /* Visible only where it can act: in play a pointer over a reticle is
     * noise, and hiding it is what the always-visible default got wrong.
     * <PREFIX>_CURSOR=0 hides it for hosts that drive the pointer by touch. */
    static int allowed = -1;
    if (allowed < 0)
        allowed = port_getenv_long("CURSOR", 1) != 0;
    if (visible) *visible = allowed && g_cursor_visible && nova3_is_main_menu();
}

void android_input_cursor_set(float x, float y)
{
    g_cursor_x = x < 0 ? 0 : (x > g_width  - 1 ? g_width  - 1 : x);
    g_cursor_y = y < 0 ? 0 : (y > g_height - 1 ? g_height - 1 : y);
    if (g_cursor_down)
        send_touch(TOUCH_MOVE);
}

void android_input_cursor_press(bool down)
{
    if (down == g_cursor_down)
        return;
    g_cursor_down = down;
    send_touch(down ? TOUCH_DOWN : TOUCH_UP);
}

/* ---------------------------------------------------------------- the sticks */

/*
 * Right stick: the scaling is the engine's, not a taste.
 *
 * GL2JNIView's frame callback calls nativeSetPowerARightJoystick(x * 7, y * 7,
 * accel) where accel ramps 1.3 -> 2.2 the longer the stick is held away from
 * centre. The engine expects that ramp; sending a constant makes the camera
 * feel like it has no acceleration curve, which is a thing a player notices
 * and a harness cannot see.
 */
static float g_right_x, g_right_y;
static float g_left_x,  g_left_y;
static Uint32 g_right_centred_at;
static long  g_input_ticks;

/*
 * Sprint, and how to stop it.
 *
 * The engine has no "stop sprinting" input: its gesture is touchEvent(100)
 * and the sprint lasts as long as FORWARD stays held. On a phone that ends by
 * itself, because forward is a finger on a virtual stick. Here the left stick
 * walks by becoming the forward key, so a player who taps the up arrow while
 * walking is holding forward continuously and the sprint never ends - two
 * field reports, on different firmwares, both said the pause menu was the only
 * way out. Releasing the stick does end it, which is why it never showed up in
 * testing here.
 *
 * So the arrow is a switch, and turning it off means saying what the engine is
 * waiting for: forward, released. The stick keeps reporting its real position;
 * the release is a few frames of forward suppressed on the way out, after
 * which walking resumes on its own.
 */
static bool g_sprinting;
static bool g_arrow_forward;
static int  g_forward_suppress;

/*
 * The left stick walks by BECOMING the dpad. nativeSetPowerALeftJoystick is
 * exported but nothing in the game's own Java ever calls it, and on hardware
 * feeding it moved nothing: movement in this engine is the four dpad
 * keycodes. Threshold with hysteresis, so a worn stick cannot flutter a key.
 */
static void stick_as_dpad(void)
{
    static bool held[4]; /* up, down, left, right */
    const struct { float v; bool positive; int code; } lanes[4] = {
        { g_left_y, false, KEY_DPAD_UP    },
        { g_left_y, true,  KEY_DPAD_DOWN  },
        { g_left_x, false, KEY_DPAD_LEFT  },
        { g_left_x, true,  KEY_DPAD_RIGHT },
    };
    for (int i = 0; i < 4; i++) {
        float a = lanes[i].positive ? lanes[i].v : -lanes[i].v;
        bool want = held[i] ? (a > 0.30f) : (a > 0.45f);
        /* Lane 0 is forward, and the sprint switch borrows it: while the
         * release is in flight the stick's own position does not matter. */
        if (i == 0 && g_forward_suppress > 0)
            want = false;
        if (want != held[i]) {
            held[i] = want;
            send_key(lanes[i].code, want);
        }
    }
    if (g_forward_suppress > 0)
        g_forward_suppress--;
    /* Forward genuinely let go - by the stick and by the arrow - is what ends
     * a sprint on its own, so the switch has to agree or the next tap would
     * read as "stop" when the player means "go". */
    if (g_sprinting && !held[0] && !g_arrow_forward && g_forward_suppress == 0)
        g_sprinting = false;
}

static void push_sticks(void)
{
    stick_as_dpad();
    if (g_left_stick)
        g_left_stick(g_env, NULL, g_left_x, g_left_y);

    /*
     * In a menu there is no camera, and the menus are touch-first: the
     * campaign/multiplayer screen is a carousel that answers only to taps.
     * The engine itself says which world we are in (nativeIsMainMenuOrIGM),
     * so the right stick drives the pointer here and the camera in play -
     * nothing to hold, nothing to teach. First seen on hardware: the menu
     * drew, one focused button answered a key, and nothing else could be
     * reached.
     */
    if (nova3_is_main_menu()) {
        if (g_right_x != 0.0f || g_right_y != 0.0f) {
            float x, y;
            int visible;
            android_input_cursor_position(&x, &y, &visible);
            android_input_cursor_set(x + g_right_x * 9.0f,
                                     y + g_right_y * 9.0f);
        }
        if (g_right_stick)
            g_right_stick(g_env, NULL, 0.0f, 0.0f, 1.0f);
        return;
    }

    if (!g_right_stick)
        return;

    float accel;
    if (g_right_x == 0.0f && g_right_y == 0.0f) {
        g_right_centred_at = SDL_GetTicks();
        accel = 0.3f;
    } else {
        /* The Java's MOGA ramp started at 0.3 and took a second to peak -
         * "heavy" was the exact word from hardware. Higher floor, faster
         * climb; the ceiling is the engine's own. */
        Uint32 held = SDL_GetTicks() - g_right_centred_at;
        accel = held > 450 ? 1.2f : held > 250 ? 1.0f : held > 100 ? 0.8f : 0.6f;
    }

    /* Camera speed, tunable - through the ACCELERATION term, not the
     * magnitude. Scaling x*7 up was tried twice on hardware (160%, 240%) and
     * changed nothing perceptible: the engine clamps the joystick vector, so
     * past the clamp the magnitude is a dead knob. The third argument is the
     * multiplier the engine applies after the clamp - the Java ramps it
     * 1.3..2.2 with hold time - and THAT is the lever. NOVA3_RPAD_SCALE is
     * percent of the stock ramp. */
    static float scale = -1.0f;
    if (scale < 0.0f) {
        const char *v = port_getenv("RPAD_SCALE");
        int pct = v && *v ? atoi(v) : 100;  /* matches the launcher export */
        if (pct < 25)  pct = 25;
        if (pct > 400) pct = 400;
        scale = (float)pct / 100.0f;
    }
    g_right_stick(g_env, NULL, g_right_x * 7.0f, g_right_y * 7.0f,
                  (accel + 1.0f) * scale);
}

/* ------------------------------------------------------------------- SDL in */

static const struct {
    SDL_GameControllerButton button;
    int                      code;
} kButtons[] = {
    /* 124, not 23: with a PowerA pad connected the Activity translates
     * BUTTON_A through its `ac` constant before it reaches the engine, and 23
     * is what R3 sends. First hardware run: every face button dead in play. */
    /* 227 throws the selected item - found on hardware via the left arrow -
     * and it lands on A, which is where the Control Scheme screen draws the
     * grenade circle. */
    { SDL_CONTROLLER_BUTTON_A,             KEY_B           },
    /* Hardware-settled: 123 is Interact/Jump/Fly (19 presses of sustained
     * play), on the bottom face button, exactly where the in-game Control
     * Scheme draws the cross. */
    { SDL_CONTROLLER_BUTTON_B,             123             },
    /* 121 is the one modified-layer code still untested on hardware; the
     * Control Scheme screen still owes us Grenade and Reload homes. */
    { SDL_CONTROLLER_BUTTON_X,             121             },
    { SDL_CONTROLLER_BUTTON_Y,             KEY_Y           },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  KEY_L1          },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, KEY_R1          },
    /* handled below: with the stick walking, up-arrow is sprint */
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     KEY_DPAD_DOWN   },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     KEY_DPAD_LEFT   },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    KEY_DPAD_RIGHT  },
    /* SWEEP: the two stick clicks carry the last untested codes. The Java's
     * own R3 sends 23; L3 is free to test 120. */
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    KEY_DPAD_CENTER },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK,     120             },
    { SDL_CONTROLLER_BUTTON_START,         KEY_START       },
};

/*
 * The face buttons, translated by layout before anything reads them.
 *
 * The kButtons table and the menu tap were settled on the ArkOS family, whose
 * SDL mappings deliver the layout this file was authored against. muOS curates
 * its mappings so the logical buttons already match the printed labels, and
 * the same table lands double-swapped there - the RG 40XX-H report is exact:
 * the menu tap lives on SDL A, physical A arrives as SDL B, and "the A button
 * doesn't seem to do anything". Same trait, same fix as the sibling ports:
 * the launcher sets the default by CFW, the variable overrides either way.
 *
 * One translation point, before the event is examined: every branch below
 * (the menu tap, the arrows, the table) sees the translated button, so the
 * swap cannot miss a callsite.
 */
static Uint8 face_translate(Uint8 b)
{
    static int swap = -1;
    if (swap < 0) {
        const char *v = port_getenv("FACE_LAYOUT");
        swap = (v && strcasecmp(v, "xbox") == 0) ? 1 : 0;
        trace("input: face layout %s", swap ? "xbox" : "nintendo");
    }
    if (!swap)
        return b;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;
    default:                      return b;
    }
}

static float axis_norm(Sint16 v)
{
    /* A dead zone wide enough for a worn analogue stick. Below it the value is
     * exactly zero, because the right-stick acceleration ramp above restarts
     * only on a true centre and a stick that never quite centres would sit at
     * maximum acceleration forever. */
    const float dead = 8000.0f;
    float f = (float)v;
    if (f > -dead && f < dead)
        return 0.0f;
    f /= 32767.0f;
    return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

bool android_input_event(const SDL_Event *event)
{
    switch (event->type) {
    case SDL_QUIT:
        return false;

    case SDL_CONTROLLERDEVICEADDED:
        SDL_GameControllerOpen(event->cdevice.which);
        return true;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        bool down = event->type == SDL_CONTROLLERBUTTONDOWN;
        Uint8 btn = face_translate(event->cbutton.button);

        if (btn == SDL_CONTROLLER_BUTTON_BACK) {
            send_key(select_keycode(), down);
            return true;
        }
        /* SWEEP: walking lives on the stick now, so two arrows can carry the
         * remaining throw-item candidates while menus keep their real codes. */
        if (!nova3_is_main_menu() &&
            btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            send_key(109, down);     /* quick weapon change, hardware-settled */
            return true;
        }
        /* The two codes still without a known effect ride the free arrows -
         * one of them should be reload. */
        if (!nova3_is_main_menu() &&
            btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
            send_key(122, down);
            return true;
        }
        if (!nova3_is_main_menu() &&
            btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            send_key(99, down);  /* reload, hardware-settled */
            return true;
        }
        /* Sprint. The Activity's own sprint gesture is touchEvent(100) plus
         * the forward key held (its L3 case and its double-tap-up case both
         * spell it that way); the up arrow now does the same, since the
         * stick took over plain walking. In menus it stays a plain up. */
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            if (nova3_is_main_menu()) {
                send_key(KEY_DPAD_UP, down);
                return true;
            }
            if (down && g_sprinting) {
                /* Second tap: stop. The engine is waiting for forward to be
                 * let go, and the stick will not do it, so this press says it
                 * instead - and walks nowhere itself. */
                g_sprinting = false;
                g_forward_suppress = 8;
                trace("input: sprint off (forward released for 8 frames)");
                return true;
            }
            if (down) {
                if (g_touch_event) {
                    trace("input: touch action=100 (sprint)");
                    g_touch_event(g_env, NULL, 100, 0, 0, 0);
                }
                g_sprinting = true;
            }
            g_arrow_forward = down;
            send_key(KEY_DPAD_UP, down);
            return true;
        }
        /* Menus are touch-first; A is the finger. In play it stays a key. */
        if (btn == SDL_CONTROLLER_BUTTON_A &&
            nova3_is_main_menu()) {
            android_input_cursor_press(down);
            return true;
        }
        /*
         * Every button, named by its SDL index, mapped or not.
         *
         * A field report said a button "does nothing" and the log could not
         * settle it: an index this table has no row for produced no line at
         * all, so a firmware whose pad reports different indices looked
         * exactly like a firmware whose engine ignores the key. One line per
         * press ends that guess - it is what the sibling ports already do.
         */
        bool mapped = false;
        for (unsigned i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
            if (kButtons[i].button == btn) {
                trace("input: controller button=%u %s -> key %d",
                      (unsigned int)btn, down ? "down" : "up", kButtons[i].code);
                send_key(kButtons[i].code, down);
                mapped = true;
                break;
            }
        }
        if (!mapped)
            trace("input: controller button=%u %s -> UNMAPPED (nothing sent)",
                  (unsigned int)btn, down ? "down" : "up");
        return true;
    }

    case SDL_CONTROLLERAXISMOTION:
        switch (event->caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:  g_left_x  = axis_norm(event->caxis.value); break;
        case SDL_CONTROLLER_AXIS_LEFTY:  g_left_y  = axis_norm(event->caxis.value); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: g_right_x = axis_norm(event->caxis.value); break;
        case SDL_CONTROLLER_AXIS_RIGHTY: g_right_y = axis_norm(event->caxis.value); break;
        /* The triggers alias the bumpers, following the engine's own table:
         * L2 arrives as 104 and is rewritten to 102, R2 as 105 to 103. */
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: {
            /* Aim, like the shoulder above it. In menus, the finger. */
            static bool was_down;
            bool now_down = event->caxis.value > 16000;
            if (nova3_is_main_menu()) {
                was_down = false;
                android_input_cursor_press(now_down);
            } else if (now_down != was_down) {
                was_down = now_down;
                send_key(KEY_L1, now_down);
            }
            break;
        }
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
            /* Fire, same code the shoulder sends - trigger ergonomics. In a
             * menu the trigger is the finger at the pointer. */
            static bool was_down;
            bool now_down = event->caxis.value > 16000;
            if (nova3_is_main_menu()) {
                was_down = false;
                android_input_cursor_press(now_down);
            } else if (now_down != was_down) {
                was_down = now_down;
                send_key(KEY_R1, now_down);
            }
            break;
        }
        default: break;
        }
        return true;

    case SDL_MOUSEMOTION:
        android_input_cursor_set((float)event->motion.x, (float)event->motion.y);
        return true;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        android_input_cursor_set((float)event->button.x, (float)event->button.y);
        android_input_cursor_press(event->type == SDL_MOUSEBUTTONDOWN);
        return true;

    default:
        return true;
    }
}

void android_input_tick(void)
{
    g_input_ticks++;
    /*
     * Say "a pad is connected" again once someone is listening. The boot-time
     * call runs before the engine's App exists, and flags written into state
     * that is not there yet are written into nothing - the tutorial then
     * says "Tap to shoot" and gameplay ignores every keycode, which is
     * exactly what the first mission looked like on hardware.
     */
    if (g_input_ticks == 200) {
        nova3_input_select_gamepad_mode();
        trace("input mode: re-asserted at tick 200");
    }
    push_sticks();
}

/* ------------------------------------------------------- the emulator channel */

/*
 * How the emulator harness presses buttons. Wired from the start rather than once real
 * input works: an emulator that can play the game is worth several trips to
 * the SD card, and these two are the whole interface.
 */
static const struct { const char *name; int code; } kControls[] = {
    { "a",      KEY_DPAD_CENTER },
    { "b",      KEY_B           },
    { "x",      KEY_X           },
    { "y",      KEY_Y           },
    { "l1",     KEY_L1          },
    { "r1",     KEY_R1          },
    { "l2",     KEY_L1          },
    { "r2",     KEY_R1          },
    { "start",  KEY_START       },
    { "up",     KEY_DPAD_UP     },
    { "down",   KEY_DPAD_DOWN   },
    { "left",   KEY_DPAD_LEFT   },
    { "right",  KEY_DPAD_RIGHT  },
};

bool android_input_inject_control(const char *name, bool down)
{
    if (!name)
        return false;

    if (strcmp(name, "select") == 0) {
        send_key(select_keycode(), down);
        return true;
    }
    for (unsigned i = 0; i < sizeof(kControls) / sizeof(kControls[0]); i++) {
        if (strcmp(kControls[i].name, name) == 0) {
            send_key(kControls[i].code, down);
            return true;
        }
    }
    return false;
}

/*
 * Raw touch, the way the phone build's own onTouchEvent passes it: action 1 is
 * a finger going down (ACTION_DOWN and ACTION_POINTER_DOWN alike), 2 a move -
 * sent for every finger on the screen - and 0 a finger lifted. Coordinates are
 * view pixels, id is MotionEvent's pointer id. Not MotionEvent's own action
 * numbers: down and up are the other way round.
 */
bool android_input_inject_touch(int action, int x, int y, int id)
{
    /* The same null singleton a too-early key faults on. */
    if (g_input_ticks < 150 || !g_touch_event)
        return false;
    static int logged;
    if (action != 2 && logged < 200) {
        logged++;
        trace("input: touch %d at %d,%d id %d (menu=%d)", action, x, y, id,
              (int)nova3_is_main_menu());
    }
    g_touch_event(g_env, NULL, action, x, y, id);
    return true;
}

bool android_input_inject_key(int code, bool down)
{
    if (g_input_ticks < 150)
        return false;
    send_key(code, down);
    return true;
}

bool android_input_inject_stick(const char *name, float x, float y)
{
    if (!name)
        return false;
    if (strcmp(name, "left") == 0) {
        g_left_x = x; g_left_y = y;
        return true;
    }
    if (strcmp(name, "right") == 0) {
        g_right_x = x; g_right_y = y;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ the query */

bool nova3_is_main_menu(void)
{
    /*
     * The getter is a JNI export into engine code, and before the engine has
     * built its App the call lands on a null this - the same null the first
     * too-early key found in EventManager::raiseSync. The autopilot waits 120
     * frames for exactly this reason; the pointer logic that now also asks
     * "are we in a menu?" every tick has to wait with it.
     */
    if (g_input_ticks < 150)
        return false;
    if (!g_is_main_menu)
        return false;
    return g_is_main_menu(g_env, NULL) != JNI_FALSE;
}

bool android_input_in_menu(void)
{
    return nova3_is_main_menu();
}

/* ------------------------------------------------------------------- autopilot */

/*
 * M7: does the game *advance*, or is it drawing a menu nobody is pressing?
 *
 * A title screen renders at full speed, opens its assets and passes M6 whole,
 * so the frame counter cannot tell the two apart. The autopilot presses
 * buttons on a schedule and then asks whether anything came of it.
 *
 * "Anything came of it" is deliberately two facts and not one. The framebuffer
 * hash alone counts every animated menu as progress; the texture counter alone
 * counts a background load that never reaches the screen. A scene change here
 * is both: the picture is different AND the engine uploaded texture data since
 * the last look. Neither happens on its own while a menu waits for input.
 *
 * Off unless asked for, and asked for automatically when the run is bounded -
 * a frame limit is set by the harness and never by a player, so this cannot
 * start pressing buttons under someone's hands.
 */
static long   g_auto_keys;
static long   g_auto_scenes;
static uint32_t g_last_hash;
static long   g_last_textures = -1;
static bool   g_auto_ready;
static bool   g_auto_enabled;

static const int kAutopilotKeys[] = {
    KEY_DPAD_CENTER,  /* accept */
    KEY_START,        /* some screens want start rather than A */
    KEY_DPAD_DOWN,    /* move the selection, in case neither applies */
    KEY_DPAD_CENTER,
    KEY_B,            /* and back out again, so a dead end is not the end */
    KEY_DPAD_CENTER,
};

static bool autopilot_enabled(void)
{
    if (!g_auto_ready) {
        g_auto_ready = true;
        g_auto_enabled = port_getenv_bool(
            "AUTOPILOT", port_getenv_long("FRAME_LIMIT", 0) != 0 ? 1 : 0);
        if (g_auto_enabled)
            trace("autopilot on: synthetic input every 40 frames");
    }
    return g_auto_enabled;
}

/*
 * Nothing is pressed until the engine has drawn this many frames.
 *
 * A key sent before the game is up is not ignored - it faults. The first
 * autopilot press landed on frame 0, ahead of the very first step(), and died
 * inside EventManager::raiseSync with `this` null:
 *
 *     assert 0 != Singleton failed 109 .../EventManager.h
 *     SIGSEGV at 0x00000020   pc = +0x0010d4fc   ldr r0, [r5, #32]
 *
 * The engine builds its event manager during its own first frames, and until
 * it has, the input path it exports leads to a null singleton. This is not an
 * autopilot quirk: it is true of a player's thumb too, which is why the wait
 * is here rather than in the harness.
 */
static const long kAutopilotStart = 120;

void android_input_autopilot_tick(long frame)
{
    if (!autopilot_enabled() || frame < kAutopilotStart)
        return;

    frame -= kAutopilotStart;

    /* One key every 40 frames, held for 4. Under llvmpipe a frame is most of a
     * second, so this is several seconds per press - slow enough that a menu
     * has time to react and the next press is not swallowed as a repeat. */
    long phase = frame % 40;
    long index = (frame / 40) % (long)(sizeof(kAutopilotKeys) / sizeof(kAutopilotKeys[0]));

    if (phase == 0) {
        send_key(kAutopilotKeys[index], true);
        g_auto_keys++;
    } else if (phase == 4) {
        send_key(kAutopilotKeys[index], false);
    }
}

static uint32_t fnv1a(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

void android_input_autopilot_sample(long frame)
{
    if (!autopilot_enabled() || frame % 30 != 0)
        return;

    static void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *);
    if (!read_pixels) {
        read_pixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))
            SDL_GL_GetProcAddress("glReadPixels");
        if (!read_pixels) {
            trace("autopilot: no glReadPixels - scene changes cannot be counted");
            g_auto_enabled = false;
            return;
        }
    }

    /* One row from the middle of the screen. A strip is enough to notice a
     * screen change and costs almost nothing next to a software-rasterised
     * frame. */
    static unsigned char row[4 * 1920];
    int w = g_width > 1920 ? 1920 : g_width;
    read_pixels(0, g_height / 2, w, 1, 0x1908 /* GL_RGBA */,
                0x1401 /* GL_UNSIGNED_BYTE */, row);

    uint32_t hash     = fnv1a(row, (size_t)w * 4);
    long     textures = android_gl_textures_uploaded();

    if (g_last_textures >= 0 && hash != g_last_hash && textures > g_last_textures) {
        g_auto_scenes++;
        trace("autopilot: scene changed at frame %ld (textures %ld -> %ld, "
              "mainmenu=%d)", frame, g_last_textures, textures,
              (int)nova3_is_main_menu());
    }

    g_last_hash     = hash;
    g_last_textures = textures;
}

long android_input_autopilot_keys(void)   { return g_auto_keys; }
long android_input_autopilot_scenes(void) { return g_auto_scenes; }

/* ------------------------------------------------------------------- wiring */

void android_input_init(so_module *mod, _JNIEnv *env, int width, int height)
{
    g_env    = (JNIEnv *)env;
    g_width  = width;
    g_height = height;
    g_cursor_x = width  / 2.0f;
    g_cursor_y = height / 2.0f;

    g_on_key_down     = (fn_key_t)   so_symbol(mod, NOVA3_GL2JNILIB("OnKeyDown"));
    g_on_key_up       = (fn_key_t)   so_symbol(mod, NOVA3_GL2JNILIB("OnKeyUp"));
    g_touch_event     = (fn_touch_t) so_symbol(mod, NOVA3_GL2JNILIB("touchEvent"));
    g_set_is_moga     = (fn_bool_t)  so_symbol(mod, NOVA3_GL2JNILIB("nativeSetIsMOGA"));
    g_set_xperia_play = (fn_bool_t)  so_symbol(mod, NOVA3_GL2JNILIB("nativeSetXperiaPlay"));
    g_set_so01d       = (fn_bool_t)  so_symbol(mod, NOVA3_GL2JNILIB("nativeSetSO01D"));
    g_power_status    = (fn_bool_t)  so_symbol(mod, NOVA3_GL2JNILIB("nativePowerStatus"));
    g_is_main_menu    = (fn_query_t) so_symbol(mod, NOVA3_GL2JNILIB("nativeIsMainMenuOrIGM"));
    g_left_stick      = (fn_stick2_t)so_symbol(mod, NOVA3_GL2JNILIB("nativeSetPowerALeftJoystick"));
    g_right_stick     = (fn_stick3_t)so_symbol(mod, NOVA3_GL2JNILIB("nativeSetPowerARightJoystick"));

    trace("input: keys=%p touch=%p sticks=%p/%p mainmenu=%p",
          (void *)g_on_key_down, (void *)g_touch_event,
          (void *)g_left_stick, (void *)g_right_stick, (void *)g_is_main_menu);

    SDL_GameControllerOpen(0);
}

/*
 * The engine's input mode, set after its own init has built the state these
 * flags live in.
 *
 * Called from main() rather than from android_input_init(), which runs before
 * GL2JNILib.init(): the Android order is init() first and the flags after, and
 * a flag written into state the engine has not created yet is written into
 * nothing.
 */
/*
 * Whether a PowerA pad counts as connected. A handheld always has one; a phone
 * (<PREFIX>_TOUCH=1) starts without and the host says when a pad comes and
 * goes, as the phone build's MOGA listener does.
 */
static int g_pad_connected = -1;

static bool pad_connected(void)
{
    if (g_pad_connected < 0)
        g_pad_connected = port_getenv_long("TOUCH", 0) ? 0 : 1;
    return g_pad_connected != 0;
}

void android_input_set_pad(bool connected)
{
    g_pad_connected = connected ? 1 : 0;
    /* Before tick 200 the re-assert below delivers it. */
    if (g_input_ticks >= 200 && g_power_status)
        g_power_status(g_env, NULL, connected ? JNI_TRUE : JNI_FALSE);
    trace("input mode: PowerA %s", connected ? "connected" : "disconnected");
}

void nova3_input_select_gamepad_mode(void)
{
    /* MOGA, not Xperia Play. The activity's own boot sets isMOGA on every
     * device and reserves the Xperia flags for two specific handsets; those
     * flags also switch the engine to a slide-out keyboard layout that has no
     * meaning here. */
    if (g_set_is_moga)     g_set_is_moga(g_env, NULL, JNI_TRUE);
    if (g_set_xperia_play) g_set_xperia_play(g_env, NULL, JNI_FALSE);
    if (g_set_so01d)       g_set_so01d(g_env, NULL, JNI_FALSE);
    /* A PowerA pad is "connected". This is what gates the right-stick path in
     * the game's own frame callback, so without it the camera stick is read by
     * nothing. Without it the engine is an ordinary touch phone: its own
     * on-screen sticks and buttons, driven by touchEvent. */
    bool pad = pad_connected();
    if (g_power_status)    g_power_status(g_env, NULL, pad ? JNI_TRUE : JNI_FALSE);

    trace("input mode: %s", pad ? "MOGA gamepad, PowerA connected"
                                : "touch phone, no PowerA");
}
