#pragma once

#include "jni.h"
#include "jni_internals.h"

/*
 * The Java classes this game asks for.
 *
 * The list is not guesswork. Every name below is a classpath-shaped string in
 * the donor:
 *
 *   strings -a libNOVA3_neon.so | grep -E '^(com|java|android)/'
 *
 * which returns exactly thirteen, plus the two that are assembled at runtime
 * from a package prefix, a tail and an "L%s/%s;" (be9db4) - the same trick the
 * sibling port's binary uses, and the reason a whole-classpath grep is a floor
 * and not a ceiling.
 *
 * Grouped by the file that implements them:
 *
 *   gl2jni_activity.cpp  com/gameloft/android/ANMP/GloftN3HM/GL2JNIActivity
 *                        and GLUtils/Device
 *   gloft_installer.cpp  installer/GameInstaller, installer/GDRMPolicy
 *   android_app.cpp      Context, ContentResolver, Build, Build$VERSION,
 *                        SystemProperties, Settings$Secure, TelephonyManager,
 *                        Bundle, Process, ViewRoot, UUID
 *
 * Two rules run through all of them, and both are pitfalls this project has
 * already paid for:
 *
 *   - Never return NULL to the engine. It stores what it gets and dereferences
 *     it later, so a null comes back as a fault somewhere unrelated. Every
 *     object-returning method answers with a real instance of a registered
 *     class, which is also what makes a later method call on it resolve.
 *
 *   - Method lookup is an exact strcmp on name AND descriptor. A signature
 *     that merely looks plausible never matches, and the engine then gets the
 *     same NULL as if the class were missing, with nothing in the log to tell
 *     the two apart. Every descriptor that takes arguments was read out of the
 *     donor's .rodata (the block at c7c6b4 is the whole GL2JNIActivity table,
 *     name and signature interleaved); the no-argument ones are the only shape
 *     their return type allows.
 */

#define NOVA3_CLASS(name)                       \
    class name : public Object {                \
    public:                                     \
        static Class clazz;                     \
        Class *_getClass() { return &clazz; }   \
    }

/* com/gameloft/android/ANMP/GloftN3HM/GL2JNIActivity - the class the whole
 * game talks to. Everything device-shaped goes through it. */
NOVA3_CLASS(GloftGL2JNIActivity);

/* com/gameloft/android/ANMP/GloftN3HM/GLUtils/Device */
NOVA3_CLASS(GloftDevice);

/* installer/GDRMPolicy - where the licence answer would be cached. */
NOVA3_CLASS(GloftGDRMPolicy);

NOVA3_CLASS(AndroidBundle);
NOVA3_CLASS(AndroidProcess);
NOVA3_CLASS(AndroidSystemProperties);
NOVA3_CLASS(AndroidSettingsSecure);
NOVA3_CLASS(AndroidTelephonyManager);
NOVA3_CLASS(AndroidContentResolver);
NOVA3_CLASS(AndroidViewRoot);
NOVA3_CLASS(JavaUuid);
NOVA3_CLASS(JavaObject);

#undef NOVA3_CLASS

/*
 * The classes the game reads a *field* off, rather than calling.
 *
 * These cannot use the macro above because a field has to exist as a real
 * member to have an address: REGISTER_STATIC_FIELD records &Class::member, and
 * the fake JVM hands the engine that address directly.
 */
class AndroidContext : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

class AndroidBuild : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    /* Read by the device-id fallback chain (be9ca0/be9cb4). */
    static String SERIAL;
};

class AndroidBuildVersion : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    /* Named at be9c30/be9c4c. Declared, deliberately not registered - see the
     * comment on its Class in android_app.cpp. */
    static int SDK_INT;
};

/*
 * com/gameloft/android/ANMP/GloftN3HM/installer/GameInstaller.
 *
 * The DRM's entry point, and the only class in this port that needs both kinds
 * of field: the game reads the singleton off the class (m_sInstance, be9df8)
 * and then the telephony manager off the instance (mDeviceInfo, be9b38,
 * declared "Landroid/telephony/TelephonyManager;" right after it).
 *
 * m_sInstance is a static member of its own type on purpose. portbase answers
 * a static object field with the field's address, so the field has to *be* the
 * object - a pointer member would hand the engine a pointer to a pointer.
 */
class GloftGameInstaller : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static GloftGameInstaller m_sInstance;

    /* Instance fields are dereferenced properly, so this one is a pointer. */
    AndroidTelephonyManager *mDeviceInfo;
};

/*
 * The singletons the engine is handed.
 *
 * On Android these are distinct objects with state; here they exist so that a
 * method looked up on what getContentResolver() returned resolves to
 * something. They are deliberately global rather than allocated per call: the
 * engine keeps the pointers it is given for the life of the process and
 * compares some of them, and a fresh object per call would break that
 * silently.
 */
extern GloftGL2JNIActivity     g_activity;
extern AndroidTelephonyManager g_telephony_manager;
extern AndroidContentResolver  g_content_resolver;
extern AndroidViewRoot         g_view_root;
