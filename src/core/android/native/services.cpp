// SPDX-License-Identifier: Zlib
// NativeActivity backend for the j4niwzis fork.
#include "bridge.hpp"
#include <android/configuration.h>
#include <sys/resource.h>

using namespace native_activity;

bool Android_JNI_SetClipboardText(const char *text)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return false;
    jobject manager = service(env, "clipboard");
    if (!manager) return false;
    jclass clip = env->FindClass("android/content/ClipData");
    jobject data = env->CallStaticObjectMethod(clip, env->GetStaticMethodID(clip, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;"), string(env, ""), string(env, text));
    env->CallVoidMethod(manager, env->GetMethodID(env->GetObjectClass(manager), "setPrimaryClip", "(Landroid/content/ClipData;)V"), data);
    return !exception(env, "set clipboard");
}

char *Android_JNI_GetClipboardText()
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return nullptr;
    jobject manager = service(env, "clipboard");
    if (!manager) return nullptr;
    jobject clip = env->CallObjectMethod(manager, env->GetMethodID(env->GetObjectClass(manager), "getPrimaryClip", "()Landroid/content/ClipData;"));
    if (exception(env, "get clipboard")) return nullptr;
    if (!clip) return SDL_strdup("");
    jclass cls = env->GetObjectClass(clip);
    if (env->CallIntMethod(clip, env->GetMethodID(cls, "getItemCount", "()I")) <= 0) return SDL_strdup("");
    jobject item = env->CallObjectMethod(clip, env->GetMethodID(cls, "getItemAt", "(I)Landroid/content/ClipData$Item;"), 0);
    jobject text = env->CallObjectMethod(item, env->GetMethodID(env->GetObjectClass(item), "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;"), activity);
    if (!text || exception(env, "clipboard text")) return SDL_strdup("");
    auto value = static_cast<jstring>(env->CallObjectMethod(text, env->GetMethodID(env->GetObjectClass(text), "toString", "()Ljava/lang/String;")));
    return exception(env, "clipboard text") ? nullptr : SDL_strdup(utf8(env, value).c_str());
}

bool Android_JNI_HasClipboardText()
{
    char *text = Android_JNI_GetClipboardText();
    bool present = text && *text;
    SDL_free(text);
    return present;
}

#ifndef SDL_AUDIO_DISABLED
void Android_StartAudioHotplug(SDL_AudioDevice **playback, SDL_AudioDevice **recording) { *playback = *recording = nullptr; }
void Android_StopAudioHotplug() {}
void Android_AudioThreadInit(SDL_AudioDevice *) { setpriority(PRIO_PROCESS, 0, -16); }
#endif

bool Android_JNI_GetLocale(char *buffer, size_t size)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame) return false;
    jclass cls = env->FindClass("java/util/Locale");
    jobject locale = env->CallStaticObjectMethod(cls, env->GetStaticMethodID(cls, "getDefault", "()Ljava/util/Locale;"));
    auto text = static_cast<jstring>(env->CallObjectMethod(locale, env->GetMethodID(cls, "toString", "()Ljava/lang/String;")));
    if (exception(env, "get locale")) return false;
    SDL_strlcpy(buffer, utf8(env, text).c_str(), size);
    return true;
}

char *SDL_GetAndroidPackageName()
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return nullptr;
    auto text = static_cast<jstring>(env->CallObjectMethod(activity,
        env->GetMethodID(env->GetObjectClass(activity), "getPackageName", "()Ljava/lang/String;")));
    return exception(env, "get package name") ? nullptr : SDL_strdup(utf8(env, text).c_str());
}

static bool feature(const char *name)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return false;
    jobject manager = env->CallObjectMethod(activity, env->GetMethodID(env->GetObjectClass(activity),
        "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    bool found = env->CallBooleanMethod(manager, env->GetMethodID(env->GetObjectClass(manager),
        "hasSystemFeature", "(Ljava/lang/String;)Z"), string(env, name));
    return !exception(env, "get system feature") && found;
}
bool SDL_IsChromebook() { return feature("org.chromium.arc.device_management"); }
bool SDL_IsDeXMode() { return false; }
bool SDL_IsAndroidTV() { return feature("android.software.leanback"); }
bool SDL_IsAndroidTablet() { return app && AConfiguration_getSmallestScreenWidthDp(app->config) >= 600; }
SDL_FormFactor SDL_GetAndroidDeviceFormFactor()
{
    if (SDL_IsAndroidTV()) return SDL_FORMFACTOR_TV;
    return SDL_IsAndroidTablet() ? SDL_FORMFACTOR_TABLET : SDL_FORMFACTOR_PHONE;
}
void SDL_SendAndroidBackButton() { Android_JNI_MinimizeWindow(); }

bool SDL_RequestAndroidPermission(const char *permission, SDL_RequestAndroidPermissionCallback callback, void *userdata)
{
    if (!permission || !callback) return SDL_InvalidParamError("permission/callback");
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return false;
    const jint result = env->CallIntMethod(activity, env->GetMethodID(env->GetObjectClass(activity),
        "checkSelfPermission", "(Ljava/lang/String;)I"), string(env, permission));
    if (exception(env, "check permission")) return false;
    if (result != 0) return SDL_SetError("NativeActivity: runtime permission prompts are not implemented");
    callback(userdata, permission, true);
    return true;
}

int Android_JNI_CreateCustomCursor(SDL_Surface *, int, int) { SDL_Unsupported(); return 0; }
void Android_JNI_DestroyCustomCursor(int) {}
bool Android_JNI_SetCustomCursor(int) { return SDL_Unsupported(); }
bool Android_JNI_SetSystemCursor(int) { return SDL_Unsupported(); }
bool Android_JNI_SupportsRelativeMouse() { return false; }
bool Android_JNI_SetRelativeMouseEnabled(bool enabled) { return enabled ? SDL_Unsupported() : true; }
bool Android_JNI_ShowMessageBox(const SDL_MessageBoxData *, int *) { return SDL_Unsupported(); }
bool Android_JNI_SendMessage(int, int) { return SDL_Unsupported(); }
bool SDL_SendAndroidMessage(Uint32 command, int param) { return Android_JNI_SendMessage(int(command), param); }
bool Android_JNI_ShowToast(const char *, int, int, int, int) { return SDL_Unsupported(); }
bool SDL_ShowAndroidToast(const char *text, int duration, int gravity, int x, int y) { return Android_JNI_ShowToast(text, duration, gravity, x, y); }
void Android_JNI_GetManifestEnvironmentVariables() {}

#ifndef SDL_POWER_DISABLED
int Android_JNI_GetPowerInfo(int *plugged, int *charged, int *battery, int *seconds, int *percent)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return -1;
    jclass filter = env->FindClass("android/content/IntentFilter");
    jobject made = env->NewObject(filter, env->GetMethodID(filter, "<init>", "(Ljava/lang/String;)V"), string(env, "android.intent.action.BATTERY_CHANGED"));
    jobject intent = env->CallObjectMethod(activity, env->GetMethodID(env->GetObjectClass(activity), "registerReceiver",
        "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;"), nullptr, made);
    if (exception(env, "get battery") || !intent) return -1;
    jclass cls = env->GetObjectClass(intent);
    jmethodID get = env->GetMethodID(cls, "getIntExtra", "(Ljava/lang/String;I)I");
    const auto value = [&](const char *name) { return env->CallIntMethod(intent, get, string(env, name), -1); };
    if (plugged) *plugged = value("plugged") > 0;
    if (charged) *charged = value("status") == 5;
    if (battery) *battery = env->CallBooleanMethod(intent, env->GetMethodID(cls, "getBooleanExtra", "(Ljava/lang/String;Z)Z"), string(env, "present"), JNI_FALSE);
    if (seconds) *seconds = -1;
    if (percent) {
        const int level = value("level"), scale = value("scale");
        *percent = scale > 0 && level >= 0 ? SDL_clamp(level * 100 / scale, 0, 100) : -1;
    }
    return exception(env, "battery state") ? -1 : 0;
}
#endif
