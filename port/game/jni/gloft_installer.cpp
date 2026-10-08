/*
 * The two DRM classes, which the binary names in pieces.
 *
 * GameInstaller and GDRMPolicy both live under the game's own package and the
 * donor never spells either out: it holds the package prefix (be9dbc), the
 * tails "installer/GameInstaller" (be9de0) and "GDRMPolicy" (bea008), and an
 * "L%s/%s;" (be9db4) it assembles them with at the point of use. The two full
 * names below are that assembly done by hand, which is why a whole-classpath
 * grep over the donor does not find them.
 *
 * Both are on the boot path - the licence check runs before the title screen -
 * so a missing one is not a late surprise; it is the first thing that goes
 * wrong, and it goes wrong as a fault inside the fake JVM rather than as a
 * message naming a class.
 */

#include <stdio.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "nova3_classes.h"

static const char kPackage[] = "com/gameloft/android/ANMP/GloftN3HM";

/* ------------------------------------------------------ GameInstaller */

GloftGameInstaller GloftGameInstaller::m_sInstance;

const FieldId gloftGameInstallerFields[] = {
    REGISTER_STATIC_FIELD(GloftGameInstaller, m_sInstance),
    REGISTER_FIELD(GloftGameInstaller, mDeviceInfo),
    {NULL},
};

static char g_game_installer_path[128];

Class GloftGameInstaller::clazz = {
    .classpath       = g_game_installer_path,
    .classname       = "GameInstaller",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = gloftGameInstallerFields,
    .instance_size   = sizeof(GloftGameInstaller),
};

/* --------------------------------------------------------- GDRMPolicy */

/*
 * Where the licence answer would be written down.
 *
 * The policy stores its verdict, a validity timestamp and a retry counter
 * through these two methods so a later launch can skip the server round trip.
 * There is no server, and the round trip is answered from the native side
 * (nativeAllow is an export of the .so, not a Java method), so writing any of
 * it down would only create state that could later disagree with the answer.
 *
 * The two descriptors are literals in the donor, at bea328 and bea37c,
 * immediately after the names they belong to.
 */
static void GDRMPolicy_UpdatePreferences(JNIEnv *env, jclass clazz,
                                         jstring key, jstring value, jint mode)
{
    (void)env; (void)clazz; (void)value; (void)mode;
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("GDRMPolicy.UpdatePreferences('%s', ...) and later calls dropped - "
              "nothing in this port caches a licence verdict",
              key ? ((String *)key)->str : "");
    }
}

static void GDRMPolicy_UpdatePreferences2(JNIEnv *env, jclass clazz,
                                          jstring key, jlong value, jint mode)
{
    (void)env; (void)clazz; (void)key; (void)value; (void)mode;
}

const ManagedMethod gloftGDRMPolicyMethods[] = {
    ManagedMethod::RegisterStatic<&GDRMPolicy_UpdatePreferences>(
        GloftGDRMPolicy::clazz, "UpdatePreferences",
        "(Ljava/lang/String;Ljava/lang/String;I)V"),
    ManagedMethod::RegisterStatic<&GDRMPolicy_UpdatePreferences2>(
        GloftGDRMPolicy::clazz, "UpdatePreferences2",
        "(Ljava/lang/String;JI)V"),
    {NULL},
};

static char g_gdrm_policy_path[128];

Class GloftGDRMPolicy::clazz = {
    .classpath       = g_gdrm_policy_path,
    .classname       = "GDRMPolicy",
    .managed_methods = gloftGDRMPolicyMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(GloftGDRMPolicy),
};

/* ----------------------------------------------------------- registry */

static int register_gloft_installer(void)
{
    snprintf(g_game_installer_path, sizeof(g_game_installer_path),
             "%s/installer/GameInstaller", kPackage);
    snprintf(g_gdrm_policy_path, sizeof(g_gdrm_policy_path),
             "%s/installer/GDRMPolicy", kPackage);

    /* The instance field the device-id chain reads off m_sInstance. Wired
     * here, from the same initialiser that registers, so the order between the
     * two is not left to chance. */
    GloftGameInstaller::m_sInstance.mDeviceInfo = &g_telephony_manager;

    ClassRegistry::register_class(GloftGameInstaller::clazz);
    ClassRegistry::register_class(GloftGDRMPolicy::clazz);
    return 0;
}

static const int registered = register_gloft_installer();
