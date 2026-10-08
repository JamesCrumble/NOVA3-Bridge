/*
 * The Android platform classes this game reaches for.
 *
 * All of them exist for one job: the device id. The GDRM check builds one out
 * of whatever the platform will give it, and the donor spells the whole chain
 * out in one run of .rodata between be9ad0 and be9ff4, each step next to the
 * log line that announces it:
 *
 *   IMEI          TelephonyManager.getDeviceId()
 *   Build.SERIAL  gated on Build$VERSION.SDK_INT
 *   ro.serialno   SystemProperties.get()
 *   ANDROID_ID    Settings$Secure.getString(getContentResolver(), "android_id")
 *   a random UUID UUID.randomUUID().toString()
 *
 * That chain runs before the title screen, so a gap in it is not a late
 * surprise - it is the first thing that goes wrong.
 *
 * Bundle, Process and ViewRoot are named in the donor too but sit outside that
 * chain; they are registered with the methods the engine's own Java layer uses
 * on them, and every one is a no-op here.
 */

#include <string.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "nova3_classes.h"

AndroidTelephonyManager g_telephony_manager;
AndroidContentResolver  g_content_resolver;
AndroidViewRoot         g_view_root;

/* ------------------------------------------- the device-id fallback chain */

/*
 * A single made-up identity, answered by every link of the chain.
 *
 * Which link the game lands on does not matter here - what matters is that the
 * answer is the same on every run, because the engine salts its own stored
 * state with it. A random UUID would make each launch look like a different
 * device.
 */
static const char kDeviceId[] = "r36s0000deadbeef";

String AndroidBuild::SERIAL(kDeviceId);
int    AndroidBuildVersion::SDK_INT = 19;

const FieldId androidBuildFields[] = {
    REGISTER_STATIC_FIELD(AndroidBuild, SERIAL),
    {NULL},
};

Class AndroidBuild::clazz = {
    .classpath       = "android/os/Build",
    .classname       = "Build",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = androidBuildFields,
    .instance_size   = sizeof(AndroidBuild),
};

/*
 * Build.VERSION.SDK_INT is deliberately NOT registered, and that needs saying.
 *
 * portbase's static-field accessor returns the field's *address* rather than
 * its contents (jni/jni.cpp, iface_GetStaticField: `return (T)(f->offset)`).
 * For an object field that is exactly right - the address of a String member
 * is the String - and for an int field it is not: the game would receive a
 * pointer value where it expects 19, and it would be a large number, so every
 * "is this at least Android X" test would pass for the wrong reason.
 *
 * Left unregistered, GetStaticFieldID says so in the log and the read comes
 * back 0, which reads as "older than any version being tested for" - a defined
 * answer, and one the device-id chain has a path for (it falls through to
 * ro.serialno). Same call the sibling port made, and it is still portbase's to
 * fix rather than this port's to work around.
 */
Class AndroidBuildVersion::clazz = {
    .classpath       = "android/os/Build$VERSION",
    .classname       = "Build$VERSION",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidBuildVersion),
};

static jobject SystemProperties_get(JNIEnv *env, jclass clazz, jstring key)
{
    (void)clazz;
    const char *name = key ? ((String *)key)->str : "";
    trace("SystemProperties.get('%s')", name);
    if (strcmp(name, "ro.serialno") == 0)
        return (jobject)env->NewStringUTF(kDeviceId);
    return (jobject)env->NewStringUTF("");
}

const ManagedMethod androidSystemPropertiesMethods[] = {
    ManagedMethod::RegisterStatic<&SystemProperties_get>(
        AndroidSystemProperties::clazz, "get",
        "(Ljava/lang/String;)Ljava/lang/String;"),
    {NULL},
};

Class AndroidSystemProperties::clazz = {
    .classpath       = "android/os/SystemProperties",
    .classname       = "SystemProperties",
    .managed_methods = androidSystemPropertiesMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidSystemProperties),
};

static jobject SettingsSecure_getString(JNIEnv *env, jclass clazz,
                                        jobject resolver, jstring key)
{
    (void)clazz; (void)resolver;
    trace("Settings.Secure.getString('%s')", key ? ((String *)key)->str : "");
    return (jobject)env->NewStringUTF(kDeviceId);
}

const ManagedMethod androidSettingsSecureMethods[] = {
    ManagedMethod::RegisterStatic<&SettingsSecure_getString>(
        AndroidSettingsSecure::clazz, "getString",
        "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;"),
    {NULL},
};

Class AndroidSettingsSecure::clazz = {
    .classpath       = "android/provider/Settings$Secure",
    .classname       = "Settings$Secure",
    .managed_methods = androidSettingsSecureMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidSettingsSecure),
};

static jobject TelephonyManager_getDeviceId(JNIEnv *env, jobject self)
{
    (void)self;
    trace("TelephonyManager.getDeviceId()");
    return (jobject)env->NewStringUTF("351066496380730");
}

const ManagedMethod androidTelephonyManagerMethods[] = {
    ManagedMethod::Register<&TelephonyManager_getDeviceId>(
        AndroidTelephonyManager::clazz, "getDeviceId", "()Ljava/lang/String;"),
    {NULL},
};

Class AndroidTelephonyManager::clazz = {
    .classpath       = "android/telephony/TelephonyManager",
    .classname       = "TelephonyManager",
    .managed_methods = androidTelephonyManagerMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidTelephonyManager),
};

Class AndroidContentResolver::clazz = {
    .classpath       = "android/content/ContentResolver",
    .classname       = "ContentResolver",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidContentResolver),
};

/* ------------------------------------------------------------ Context */

/*
 * The only method the donor names on Context is getContentResolver
 * (be9e1c/be9e30), one step of the chain above. getSystemService is absent
 * from this binary's strings - unlike the sibling's - so it is not registered:
 * a method nothing asks for is a claim about the engine that has not been
 * checked, and the log names anything that turns out to be missing.
 */
static jobject Context_getContentResolver(JNIEnv *env, jobject self)
{
    (void)env; (void)self;
    return (jobject)&g_content_resolver;
}

const ManagedMethod androidContextMethods[] = {
    ManagedMethod::Register<&Context_getContentResolver>(
        AndroidContext::clazz, "getContentResolver",
        "()Landroid/content/ContentResolver;"),
    {NULL},
};

Class AndroidContext::clazz = {
    .classpath       = "android/content/Context",
    .classname       = "Context",
    .managed_methods = androidContextMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidContext),
};

/* --------------------------------------------------- UUID and Object */

static jobject Uuid_randomUUID(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    /* The last resort of the device-id chain. Fixed rather than random, for
     * the reason in the kDeviceId comment above. */
    static JavaUuid the_uuid;
    return (jobject)&the_uuid;
}

static jobject Uuid_toString(JNIEnv *env, jobject self)
{
    (void)self;
    return (jobject)env->NewStringUTF("00000000-0000-4000-8000-r36s0deadbeef");
}

const ManagedMethod javaUuidMethods[] = {
    ManagedMethod::RegisterStatic<&Uuid_randomUUID>(
        JavaUuid::clazz, "randomUUID", "()Ljava/util/UUID;"),
    ManagedMethod::Register<&Uuid_toString>(
        JavaUuid::clazz, "toString", "()Ljava/lang/String;"),
    {NULL},
};

Class JavaUuid::clazz = {
    .classpath       = "java/util/UUID",
    .classname       = "UUID",
    .managed_methods = javaUuidMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(JavaUuid),
};

static jobject Object_toString(JNIEnv *env, jobject self)
{
    (void)self;
    return (jobject)env->NewStringUTF("java.lang.Object");
}

const ManagedMethod javaObjectMethods[] = {
    ManagedMethod::Register<&Object_toString>(
        JavaObject::clazz, "toString", "()Ljava/lang/String;"),
    {NULL},
};

Class JavaObject::clazz = {
    .classpath       = "java/lang/Object",
    .classname       = "Object",
    .managed_methods = javaObjectMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(JavaObject),
};

/* ------------------------------------------- Bundle, Process, ViewRoot */

/*
 * A Bundle that forgets everything, a Process that ignores priorities and a
 * ViewRoot with nothing behind it. All three are named in the donor and none
 * is on the boot path; they are here so that a lookup resolves rather than
 * returning the NULL the engine would dereference.
 *
 * Storage would be the wrong kind of work in the Bundle's case: what the game
 * keeps in one is the GDRM policy's cached verdict, and this port answers the
 * licence check unconditionally. Remembering an answer that is already given
 * only creates state that can later disagree with it.
 */
static jobject  Bundle_init(JNIEnv *e, jobject s, jclass c)                  { (void)e; (void)c; return (jobject)s; }
static void     Bundle_putString(JNIEnv *e, jobject s, jstring k, jstring v) { (void)e; (void)s; (void)k; (void)v; }
static void     Bundle_putInt(JNIEnv *e, jobject s, jstring k, jint v)       { (void)e; (void)s; (void)k; (void)v; }
static void     Bundle_putLong(JNIEnv *e, jobject s, jstring k, jlong v)     { (void)e; (void)s; (void)k; (void)v; }
static jint     Bundle_getInt(JNIEnv *e, jobject s, jstring k)               { (void)e; (void)s; (void)k; return 0; }
static jlong    Bundle_getLong(JNIEnv *e, jobject s, jstring k)              { (void)e; (void)s; (void)k; return 0; }
static jboolean Bundle_containsKey(JNIEnv *e, jobject s, jstring k)          { (void)e; (void)s; (void)k; return JNI_FALSE; }

static jobject Bundle_getString(JNIEnv *env, jobject self, jstring key)
{
    (void)self; (void)key;
    return (jobject)env->NewStringUTF("");
}

const ManagedMethod androidBundleMethods[] = {
    REGISTER_INIT_METHOD(AndroidBundle, Bundle_init, "()V"),
    ManagedMethod::Register<&Bundle_putString>(
        AndroidBundle::clazz, "putString", "(Ljava/lang/String;Ljava/lang/String;)V"),
    ManagedMethod::Register<&Bundle_getString>(
        AndroidBundle::clazz, "getString", "(Ljava/lang/String;)Ljava/lang/String;"),
    ManagedMethod::Register<&Bundle_putInt>(
        AndroidBundle::clazz, "putInt", "(Ljava/lang/String;I)V"),
    ManagedMethod::Register<&Bundle_getInt>(
        AndroidBundle::clazz, "getInt", "(Ljava/lang/String;)I"),
    ManagedMethod::Register<&Bundle_putLong>(
        AndroidBundle::clazz, "putLong", "(Ljava/lang/String;J)V"),
    ManagedMethod::Register<&Bundle_getLong>(
        AndroidBundle::clazz, "getLong", "(Ljava/lang/String;)J"),
    ManagedMethod::Register<&Bundle_containsKey>(
        AndroidBundle::clazz, "containsKey", "(Ljava/lang/String;)Z"),
    {NULL},
};

Class AndroidBundle::clazz = {
    .classpath       = "android/os/Bundle",
    .classname       = "Bundle",
    .managed_methods = androidBundleMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidBundle),
};

static void Process_setThreadPriority(JNIEnv *env, jclass clazz, jint priority)
{
    (void)env; (void)clazz;
    /*
     * Ignored on purpose. Android's scale runs -20 (urgent audio) to 19, and
     * the values the engine passes are meaningful only against Android's
     * scheduler policy. Handing them to setpriority() here would need
     * CAP_SYS_NICE for anything negative and would fail silently without it.
     */
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("Process.setThreadPriority(%d) and later calls ignored", priority);
    }
}

const ManagedMethod androidProcessMethods[] = {
    ManagedMethod::RegisterStatic<&Process_setThreadPriority>(
        AndroidProcess::clazz, "setThreadPriority", "(I)V"),
    {NULL},
};

Class AndroidProcess::clazz = {
    .classpath       = "android/os/Process",
    .classname       = "Process",
    .managed_methods = androidProcessMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidProcess),
};

Class AndroidViewRoot::clazz = {
    .classpath       = "android/view/ViewRoot",
    .classname       = "ViewRoot",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidViewRoot),
};

/* Registration order does not matter - lookup is by name - but keeping it in
 * one place makes it obvious when a class was written and never registered,
 * which is a failure with no error message. */
static const int registered[] = {
    ClassRegistry::register_class(AndroidContext::clazz),
    ClassRegistry::register_class(AndroidContentResolver::clazz),
    ClassRegistry::register_class(AndroidBuild::clazz),
    ClassRegistry::register_class(AndroidBuildVersion::clazz),
    ClassRegistry::register_class(AndroidSystemProperties::clazz),
    ClassRegistry::register_class(AndroidSettingsSecure::clazz),
    ClassRegistry::register_class(AndroidTelephonyManager::clazz),
    ClassRegistry::register_class(AndroidBundle::clazz),
    ClassRegistry::register_class(AndroidProcess::clazz),
    ClassRegistry::register_class(AndroidViewRoot::clazz),
    ClassRegistry::register_class(JavaUuid::clazz),
    ClassRegistry::register_class(JavaObject::clazz),
};
