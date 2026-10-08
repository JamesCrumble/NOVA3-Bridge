/*
 * com/gameloft/android/ANMP/GloftN3HM/GL2JNIActivity - the Java half of this
 * game, and the only class the engine calls back into during normal play.
 *
 * The table below is transcribed from the donor rather than from the APK: the
 * run of .rodata at c7c6b4 is the classpath followed by every method name the
 * engine looks up, with the descriptor written out beside any name whose shape
 * is not implied by its return type. Fifty-one entries, and the block ends at
 * the "trying to set context %d" log line, so the boundary is not a judgement
 * call either.
 *
 * On Android most of these are declared on GL2JNILib rather than on the
 * activity; the engine looks them all up on the activity's class anyway, and
 * that is what determines what has to be registered here. The APK is still the
 * authority on the *signatures* - GL2JNILib.java declares every one of them -
 * so the two sources agree on names and complement each other on shapes.
 *
 * Almost everything answers with the quietest defensible constant. Three do
 * real work:
 *
 *   setupPaths     hands the engine its four directories (nova3_set_paths)
 *   GetSDFolder    the writable data directory
 *   GetWindow*     the panel this port drives
 *
 * Two answer FALSE where TRUE looks friendlier, and the reason is the same for
 * both: nothing in this port persists a preference, so IsFirstTimeLaunch and
 * IsNewDay would be true on *every* launch. A first-run path that never ends
 * is worse than one that never runs.
 */

#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "app_exit.h"
#include "io_paths.h"
#include "trace.h"

#include "nova3.h"
#include "nova3_classes.h"

static const char kPackage[] = "com/gameloft/android/ANMP/GloftN3HM";

GloftGL2JNIActivity g_activity;

/* The panel, so GetWindowWidth/Height agree with the resize main.cpp performs.
 * Set once from main() rather than duplicated as a constant here, because a
 * disagreement between the two shows up as a viewport the engine lays its UI
 * out against and the driver never sees. */
static int g_width  = 640;
static int g_height = 480;

void nova3_set_view_size(int w, int h)
{
    g_width  = w;
    g_height = h;
}

/* ------------------------------------------------------------ the paths */

static void Activity_setupPaths(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    nova3_set_paths(env);
}

static jobject Activity_GetSDFolder(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    /* On Android this is ".../Android/data/<pkg>/files", the directory the
     * game writes trophies and its own state into - not the obb directory. */
    return (jobject)env->NewStringUTF(io_writable_dir());
}

static jbyteArray Activity_getResource(JNIEnv *env, jclass clazz, jstring name)
{
    (void)clazz;
    /*
     * On Android this reads a file out of the APK through
     * Class.getResourceAsStream. There is no APK here and everything the game
     * needs is in the expansion files, so the honest answer is "empty".
     *
     * Empty and not NULL: the caller passes what it gets straight to
     * GetArrayLength, and a NULL there faults inside the fake JVM with the
     * game's return address in lr, which reads as a loader bug.
     */
    trace("GL2JNIActivity.getResource('%s') - answered empty",
          name ? ((String *)name)->str : "");
    return env->NewByteArray(0);
}

/* ------------------------------------------------------------- the view */

static void Activity_createView(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    /* The window and the GL context already exist - main() made them before
     * the module was linked, because the game's 100 gl* imports are bound by
     * asking the driver and there is no driver to ask without a context. */
    trace("GL2JNIActivity.createView - the loader already owns the window");
}

static void Activity_setViewSettings(JNIEnv *env, jclass clazz,
                                     jint pixel, jint depth, jint stencil,
                                     jint csaa, jint depth_non_linear)
{
    (void)env; (void)clazz;
    /*
     * The engine telling Java which EGL config it wants. On Android those five
     * numbers feed the config chooser; here SDL has already chosen, so this is
     * recorded and not acted on - but it is worth recording, because a
     * mismatch between what the engine asked for and what the context has is
     * the first thing to check when the depth buffer misbehaves on hardware.
     */
    trace("setViewSettings(pixel=%d depth=%d stencil=%d csaa=%d nonlinear=%d)",
          pixel, depth, stencil, csaa, depth_non_linear);
}

static jboolean Activity_setCurrentContext(JNIEnv *env, jclass clazz, jint index)
{
    (void)env; (void)clazz;
    /*
     * The engine can ask for extra GL contexts (setNumExtraContext) and then
     * switch between them for background loading. This port drives one
     * context, so index 0 is the only one that exists and anything else is
     * refused rather than silently accepted - accepting it would leave the
     * engine drawing into a context it thinks it changed.
     */
    return index == 0 ? JNI_TRUE : JNI_FALSE;
}

static jint Activity_GetWindowWidth(JNIEnv *e, jclass c)  { (void)e; (void)c; return g_width;  }
static jint Activity_GetWindowHeight(JNIEnv *e, jclass c) { (void)e; (void)c; return g_height; }

/* ----------------------------------------------------------- the device */

static jobject Activity_GetDeviceName(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (jobject)env->NewStringUTF("Anbernic R36S");
}

static jobject Activity_GetPhoneModel(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (jobject)env->NewStringUTF("R36S");
}

static jobject Activity_GetDeviceFirmware(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (jobject)env->NewStringUTF("4.4.4");
}

static jobject Activity_GetPhoneLanguage(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    /*
     * "en", unconditionally. The engine maps this onto its own language table
     * (the ten codes in GL2JNILib.java) and a code outside that table indexes
     * past the end of it; the host's locale is not guaranteed to be one of the
     * ten, so it is not consulted.
     */
    return (jobject)env->NewStringUTF("en");
}

static jobject Activity_GetUDID(JNIEnv *env, jclass clazz, jboolean encoded)
{
    (void)clazz; (void)encoded;
    return (jobject)env->NewStringUTF("r36s0000deadbeef");
}

static jobject Activity_GetMAC(JNIEnv *env, jclass clazz, jint hashed)
{
    (void)clazz; (void)hashed;
    return (jobject)env->NewStringUTF("DE:AD:00:BE:EF:00");
}

static jint   Activity_GetPhoneCPUCores(JNIEnv *e, jclass c) { (void)e; (void)c; return 4; }
static jfloat Activity_GetPhoneCPUFreq(JNIEnv *e, jclass c)  { (void)e; (void)c; return 1500.0f; }
static jfloat Activity_GetPhoneMemory(JNIEnv *e, jclass c)   { (void)e; (void)c; return 1024.0f; }

static jint Activity_UseEffectSpec(JNIEnv *e, jclass c)
{
    (void)e; (void)c;
    /*
     * 0. On Android this is 1 only for Motorola, the Kindle Fire and the Xperia
     * "ZEUS" devices (GL2JNILib.UseEffectSpec), so 0 is what every other phone
     * of the era got and is the path the engine is best tested on.
     */
    return 0;
}

static jboolean Activity_IsTegra4(JNIEnv *e, jclass c)    { (void)e; (void)c; return JNI_FALSE; }
static jboolean Activity_IsHTCPhone(JNIEnv *e, jclass c)  { (void)e; (void)c; return JNI_FALSE; }
static jboolean Activity_IsAmazonNub(JNIEnv *e, jclass c) { (void)e; (void)c; return JNI_FALSE; }

static jint Activity_HasGyroscope(JNIEnv *e, jclass c)
{
    (void)e; (void)c;
    /* No gyroscope and no accelerometer on a handheld. Answering yes would put
     * the engine's motion-aim path in charge of the camera with nothing ever
     * feeding it. */
    return 0;
}

/* No network. Both answers together are what keeps the engine out of its
 * online lobby, which would otherwise sit waiting on a server that has been
 * gone for a decade. */
static jint Activity_IsWifiEnabled(JNIEnv *e, jclass c) { (void)e; (void)c; return 0; }
static jint Activity_Is3gEnabled(JNIEnv *e, jclass c)   { (void)e; (void)c; return 0; }

static jbyteArray Activity_GetFileFromURL(JNIEnv *env, jclass clazz, jstring url)
{
    (void)clazz;
    trace("GetFileFromURL('%s') - no network in this port",
          url ? ((String *)url)->str : "");
    return env->NewByteArray(0);
}

/* ------------------------------------------------------ stored settings */

static jboolean Activity_IsFirstTimeLaunch(JNIEnv *env, jclass clazz, jstring key)
{
    (void)env; (void)clazz; (void)key;
    /* FALSE - see the header comment. Nothing persists preferences here, so
     * TRUE would be TRUE forever. */
    return JNI_FALSE;
}

static jboolean Activity_IsNewDay(JNIEnv *e, jclass c) { (void)e; (void)c; return JNI_FALSE; }

static jint Activity_GetInt(JNIEnv *env, jclass clazz, jstring key, jint fallback)
{
    (void)env; (void)clazz; (void)key;
    return fallback;
}

static void Activity_PutInt(JNIEnv *e, jclass c, jstring k, jint v) { (void)e; (void)c; (void)k; (void)v; }

/* ------------------------------------------------------- the keyboard */

static jbyteArray Activity_GetKeyboardText(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return env->NewByteArray(0);
}

static jint Activity_IsKeyboardVisible(JNIEnv *e, jclass c) { (void)e; (void)c; return 0; }

static void Activity_SetKeyboard(JNIEnv *env, jclass clazz,
                                 jint mode, jstring text, jint max_len)
{
    (void)env; (void)clazz; (void)text;
    /* There is no soft keyboard here. Announced once because a game waiting
     * for typed text that never arrives looks like a freeze. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("SetKeyboard(mode=%d max=%d) - no soft keyboard in this port; "
              "text entry screens will stay empty", mode, max_len);
    }
}

/* ------------------------------------------------------------- the rest */

static void Activity_ExitGame(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    /*
     * The game's own exit. portbase records it and the frame loop consumes it;
     * nothing is torn down here, because ExitGame arrives from inside the
     * engine's call stack and unwinding the process from there is a crash on
     * the way out rather than an exit. See portbase/android/app_exit.h.
     */
    android_app_request_exit("the game called GL2JNILib.ExitGame()");
}

static void Activity_ShowToast(JNIEnv *env, jclass clazz, jstring text)
{
    (void)clazz;
    trace("toast: %s", text ? ((String *)text)->str : "");
}

static void Activity_ShowAlert(JNIEnv *env, jclass clazz, jstring title, jstring body)
{
    (void)clazz;
    trace("alert: %s / %s",
          title ? ((String *)title)->str : "",
          body  ? ((String *)body)->str  : "");
}

static jobject Activity_getVersion(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (jobject)env->NewStringUTF("1.0.7");
}

static void Activity_SendToBackground(JNIEnv *e, jclass c)                  { (void)e; (void)c; }
static void Activity_KeepScreenOn(JNIEnv *e, jclass c, jint on)             { (void)e; (void)c; (void)on; }
static void Activity_ShowLoadingScreen(JNIEnv *e, jclass c, jboolean s)     { (void)e; (void)c; (void)s; }
static void Activity_SetGameState(JNIEnv *e, jclass c, jint s)              { (void)e; (void)c; (void)s; }
static void Activity_SetActionPhase(JNIEnv *e, jclass c, jint p)            { (void)e; (void)c; (void)p; }
static void Activity_SetOrientation(JNIEnv *e, jclass c, jint o)            { (void)e; (void)c; (void)o; }
static void Activity_setNoSwapFrames(JNIEnv *e, jclass c, jint n)           { (void)e; (void)c; (void)n; }
static void Activity_setGameLang(JNIEnv *e, jclass c, jint l)               { (void)e; (void)c; (void)l; }
static void Activity_TrackAndroidHits(JNIEnv *e, jclass c, jint w)          { (void)e; (void)c; (void)w; }
static void Activity_IGPLaunch(JNIEnv *e, jclass c, jint i)                 { (void)e; (void)c; (void)i; }
static void Activity_GLLiveLaunch(JNIEnv *e, jclass c, jint a, jint b)      { (void)e; (void)c; (void)a; (void)b; }
static void Activity_GLLiveNotifyTrophy(JNIEnv *e, jclass c, jint t)        { (void)e; (void)c; (void)t; }
static void Activity_BrowserLaunch(JNIEnv *e, jclass c, jstring u)          { (void)e; (void)c; (void)u; }
static void Activity_setCalibratingGyro(JNIEnv *e, jclass c, jboolean b)    { (void)e; (void)c; (void)b; }
static void Activity_resetGyroCalibration(JNIEnv *e, jclass c)              { (void)e; (void)c; }
static void Activity_enableAccelerometer(JNIEnv *e, jclass c, jboolean b, jfloat f) { (void)e; (void)c; (void)b; (void)f; }
static void Activity_enableGyroscope(JNIEnv *e, jclass c, jboolean b, jfloat f)     { (void)e; (void)c; (void)b; (void)f; }

const ManagedMethod gl2jniActivityMethods[] = {
    ManagedMethod::RegisterStatic<&Activity_getResource>(
        GloftGL2JNIActivity::clazz, "getResource", "(Ljava/lang/String;)[B"),
    ManagedMethod::RegisterStatic<&Activity_setupPaths>(
        GloftGL2JNIActivity::clazz, "setupPaths", "()V"),
    ManagedMethod::RegisterStatic<&Activity_createView>(
        GloftGL2JNIActivity::clazz, "createView", "()V"),
    ManagedMethod::RegisterStatic<&Activity_setViewSettings>(
        GloftGL2JNIActivity::clazz, "setViewSettings", "(IIIII)V"),
    ManagedMethod::RegisterStatic<&Activity_setCurrentContext>(
        GloftGL2JNIActivity::clazz, "setCurrentContext", "(I)Z"),
    ManagedMethod::RegisterStatic<&Activity_enableAccelerometer>(
        GloftGL2JNIActivity::clazz, "enableAccelerometer", "(ZF)V"),
    ManagedMethod::RegisterStatic<&Activity_enableGyroscope>(
        GloftGL2JNIActivity::clazz, "enableGyroscope", "(ZF)V"),
    ManagedMethod::RegisterStatic<&Activity_GetWindowWidth>(
        GloftGL2JNIActivity::clazz, "GetWindowWidth", "()I"),
    ManagedMethod::RegisterStatic<&Activity_GetWindowHeight>(
        GloftGL2JNIActivity::clazz, "GetWindowHeight", "()I"),
    ManagedMethod::RegisterStatic<&Activity_GLLiveLaunch>(
        GloftGL2JNIActivity::clazz, "GLLiveLaunch", "(II)V"),
    ManagedMethod::RegisterStatic<&Activity_GLLiveNotifyTrophy>(
        GloftGL2JNIActivity::clazz, "GLLiveNotifyTrophy", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_IGPLaunch>(
        GloftGL2JNIActivity::clazz, "IGPLaunch", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_setCalibratingGyro>(
        GloftGL2JNIActivity::clazz, "setCalibratingGyro", "(Z)V"),
    ManagedMethod::RegisterStatic<&Activity_resetGyroCalibration>(
        GloftGL2JNIActivity::clazz, "resetGyroCalibration", "()V"),
    ManagedMethod::RegisterStatic<&Activity_GetKeyboardText>(
        GloftGL2JNIActivity::clazz, "GetKeyboardText", "()[B"),
    ManagedMethod::RegisterStatic<&Activity_SetKeyboard>(
        GloftGL2JNIActivity::clazz, "SetKeyboard", "(ILjava/lang/String;I)V"),
    ManagedMethod::RegisterStatic<&Activity_IsKeyboardVisible>(
        GloftGL2JNIActivity::clazz, "IsKeyboardVisible", "()I"),
    ManagedMethod::RegisterStatic<&Activity_SetActionPhase>(
        GloftGL2JNIActivity::clazz, "SetActionPhase", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_BrowserLaunch>(
        GloftGL2JNIActivity::clazz, "BrowserLaunch", "(Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&Activity_ExitGame>(
        GloftGL2JNIActivity::clazz, "ExitGame", "()V"),
    ManagedMethod::RegisterStatic<&Activity_SendToBackground>(
        GloftGL2JNIActivity::clazz, "SendToBackground", "()V"),
    ManagedMethod::RegisterStatic<&Activity_setNoSwapFrames>(
        GloftGL2JNIActivity::clazz, "setNoSwapFrames", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_setGameLang>(
        GloftGL2JNIActivity::clazz, "setGameLang", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_IsHTCPhone>(
        GloftGL2JNIActivity::clazz, "IsHTCPhone", "()Z"),
    ManagedMethod::RegisterStatic<&Activity_HasGyroscope>(
        GloftGL2JNIActivity::clazz, "HasGyroscope", "()I"),
    ManagedMethod::RegisterStatic<&Activity_IsWifiEnabled>(
        GloftGL2JNIActivity::clazz, "IsWifiEnabled", "()I"),
    ManagedMethod::RegisterStatic<&Activity_Is3gEnabled>(
        GloftGL2JNIActivity::clazz, "Is3gEnabled", "()I"),
    ManagedMethod::RegisterStatic<&Activity_GetUDID>(
        GloftGL2JNIActivity::clazz, "GetUDID", "(Z)Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_GetMAC>(
        GloftGL2JNIActivity::clazz, "GetMAC", "(I)Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_GetPhoneCPUFreq>(
        GloftGL2JNIActivity::clazz, "GetPhoneCPUFreq", "()F"),
    ManagedMethod::RegisterStatic<&Activity_GetPhoneCPUCores>(
        GloftGL2JNIActivity::clazz, "GetPhoneCPUCores", "()I"),
    ManagedMethod::RegisterStatic<&Activity_UseEffectSpec>(
        GloftGL2JNIActivity::clazz, "UseEffectSpec", "()I"),
    ManagedMethod::RegisterStatic<&Activity_GetPhoneMemory>(
        GloftGL2JNIActivity::clazz, "GetPhoneMemory", "()F"),
    ManagedMethod::RegisterStatic<&Activity_GetFileFromURL>(
        GloftGL2JNIActivity::clazz, "GetFileFromURL", "(Ljava/lang/String;)[B"),
    ManagedMethod::RegisterStatic<&Activity_GetSDFolder>(
        GloftGL2JNIActivity::clazz, "GetSDFolder", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_ShowToast>(
        GloftGL2JNIActivity::clazz, "ShowToast", "(Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&Activity_KeepScreenOn>(
        GloftGL2JNIActivity::clazz, "KeepScreenOn", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_GetPhoneModel>(
        GloftGL2JNIActivity::clazz, "GetPhoneModel", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_IsFirstTimeLaunch>(
        GloftGL2JNIActivity::clazz, "IsFirstTimeLaunch", "(Ljava/lang/String;)Z"),
    ManagedMethod::RegisterStatic<&Activity_IsTegra4>(
        GloftGL2JNIActivity::clazz, "IsTegra4", "()Z"),
    ManagedMethod::RegisterStatic<&Activity_GetPhoneLanguage>(
        GloftGL2JNIActivity::clazz, "GetPhoneLanguage", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_IsNewDay>(
        GloftGL2JNIActivity::clazz, "IsNewDay", "()Z"),
    ManagedMethod::RegisterStatic<&Activity_GetDeviceName>(
        GloftGL2JNIActivity::clazz, "GetDeviceName", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_GetDeviceFirmware>(
        GloftGL2JNIActivity::clazz, "GetDeviceFirmware", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Activity_IsAmazonNub>(
        GloftGL2JNIActivity::clazz, "IsAmazonNub", "()Z"),
    ManagedMethod::RegisterStatic<&Activity_PutInt>(
        GloftGL2JNIActivity::clazz, "PutInt", "(Ljava/lang/String;I)V"),
    ManagedMethod::RegisterStatic<&Activity_GetInt>(
        GloftGL2JNIActivity::clazz, "GetInt", "(Ljava/lang/String;I)I"),
    ManagedMethod::RegisterStatic<&Activity_ShowLoadingScreen>(
        GloftGL2JNIActivity::clazz, "ShowLoadingScreen", "(Z)V"),
    ManagedMethod::RegisterStatic<&Activity_ShowAlert>(
        GloftGL2JNIActivity::clazz, "ShowAlert",
        "(Ljava/lang/String;Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&Activity_SetGameState>(
        GloftGL2JNIActivity::clazz, "SetGameState", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_TrackAndroidHits>(
        GloftGL2JNIActivity::clazz, "TrackAndroidHits", "(I)V"),
    /* The two the donor names further down the same section, after the
     * setCurrentContext log lines: SetOrientation (c7cb44) and getVersion
     * (c7cb90). Both really are the activity's own on Android. */
    ManagedMethod::RegisterStatic<&Activity_SetOrientation>(
        GloftGL2JNIActivity::clazz, "SetOrientation", "(I)V"),
    ManagedMethod::RegisterStatic<&Activity_getVersion>(
        GloftGL2JNIActivity::clazz, "getVersion", "()Ljava/lang/String;"),
    {NULL},
};

static char g_activity_path[128];

Class GloftGL2JNIActivity::clazz = {
    .classpath       = g_activity_path,
    .classname       = "GL2JNIActivity",
    .managed_methods = gl2jniActivityMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(GloftGL2JNIActivity),
};

/* ------------------------------------------------------------- Device */

static jobject Device_getUserAgent(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (jobject)env->NewStringUTF("Mozilla/5.0 (Linux; Android 4.4.4)");
}

const ManagedMethod gloftDeviceMethods[] = {
    ManagedMethod::RegisterStatic<&Device_getUserAgent>(
        GloftDevice::clazz, "getUserAgent", "()Ljava/lang/String;"),
    {NULL},
};

Class GloftDevice::clazz = {
    /* Spelled out rather than composed: this is one of the two classpaths the
     * donor carries whole (c7cdc8), so the literal is the evidence. */
    .classpath       = "com/gameloft/android/ANMP/GloftN3HM/GLUtils/Device",
    .classname       = "Device",
    .managed_methods = gloftDeviceMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(GloftDevice),
};

/* ----------------------------------------------------------- registry */

static int register_gl2jni_classes(void)
{
    snprintf(g_activity_path, sizeof(g_activity_path),
             "%s/GL2JNIActivity", kPackage);

    ClassRegistry::register_class(GloftGL2JNIActivity::clazz);
    ClassRegistry::register_class(GloftDevice::clazz);
    return 0;
}

static const int registered = register_gl2jni_classes();
