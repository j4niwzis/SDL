// SPDX-License-Identifier: Zlib
// NativeActivity backend for the j4niwzis fork.
#include "bridge.hpp"
#include <android/configuration.h>
#include <android/log.h>
#include <android/window.h>
#include <pthread.h>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <thread>

extern "C" void SDL_NativeGlue_onCreate(ANativeActivity *, void *, size_t);

namespace native_activity {
android_app *app = nullptr;
JavaVM *vm = nullptr;
jobject activity = nullptr;
SDL_Mutex *activity_mutex = nullptr;
static SDL_NativeActivityMain main_function;
static SDL_Mutex *event_mutex;
static std::vector<SDL_AndroidLifecycleEvent> pending;
static pthread_key_t attachment;
static pthread_once_t attachment_once = PTHREAD_ONCE_INIT;
static std::atomic<bool> resumed;
static bool quitting;
static std::atomic<bool> back_trapped;
// The window's insets moved -- the keyboard shown or hidden -- on the UI thread:
// the safe area read again by the app thread.
static std::atomic<bool> insets_moved;
// The app thread in ALooper_pollOnce -- serving what the UI thread and the
// input queue wait on -- and when it last came out of it.
static std::atomic<bool> app_polling;
static std::atomic<Uint64> app_left_poll;
static std::atomic<bool> watching;

static void detach(void *value)
{
    if (value && vm) vm->DetachCurrentThread();
}

static void make_attachment_key()
{
    pthread_key_create(&attachment, detach);
}

bool exception(JNIEnv *env, const char *operation)
{
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    SDL_SetError("Android: %s failed", operation);
    return true;
}

std::string utf8(JNIEnv *env, jstring text)
{
    if (!text) return {};
    const auto count = env->GetStringLength(text);
    const auto *chars = env->GetStringChars(text, nullptr);
    if (!chars) return {};
    // JNI's GetStringUTFChars uses modified UTF-8, which corrupts emoji
    // when handed to SDL. Convert the actual UTF-16 code units instead.
    std::string result;
    for (jsize i = 0; i < count; ++i) {
        Uint32 c = chars[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < count &&
            chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (chars[++i] - 0xdc00);
        } else if (c >= 0xd800 && c <= 0xdfff) {
            c = 0xfffd;
        }
        char bytes[5]{};
        char *end = SDL_UCS4ToUTF8(c, bytes);
        result.append(bytes, end);
    }
    env->ReleaseStringChars(text, chars);
    return result;
}

jstring string(JNIEnv *env, const char *text)
{
    if (!text) return nullptr;
    // Android is little endian on all supported targets.
    char *wide = SDL_iconv_string("UTF-16LE", "UTF-8", text, SDL_strlen(text) + 1);
    if (!wide) return nullptr;
    const auto *units = reinterpret_cast<const jchar *>(wide);
    jsize length = 0;
    while (units[length]) ++length;
    jstring result = env->NewString(units, length);
    SDL_free(wide);
    return result;
}

jobject service(JNIEnv *env, const char *name)
{
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    return method ? env->CallObjectMethod(activity, method, env->NewStringUTF(name)) : nullptr;
}

struct Edges
{
    int left = 0, top = 0, right = 0, bottom = 0;
};

// What the status and navigation bars, a display cutout and the keyboard cover of
// the window, in pixels: WindowInsets.getInsets(systemBars() | displayCutout() | ime())
// from API 30, the system window insets (the keyboard's with them, under adjustResize)
// before it; nothing before API 23 or before the view is attached. Since API 35 the
// window is edge to edge and is not resized for the keyboard: only its inset says
// where it is.
static std::optional<Edges> system_bars_in(JNIEnv *env)
{
    const auto object_of = [env](jobject target, const char *name, const char *signature) -> jobject {
        if (!target) return nullptr;
        jmethodID method = env->GetMethodID(env->GetObjectClass(target), name, signature);
        if (!method) { env->ExceptionClear(); return nullptr; }
        jobject result = env->CallObjectMethod(target, method);
        return exception(env, name) ? nullptr : result;
    };
    jobject window = object_of(activity, "getWindow", "()Landroid/view/Window;");
    jobject decor = object_of(window, "getDecorView", "()Landroid/view/View;");
    jobject insets = object_of(decor, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
    if (!insets) return std::nullopt;

    const auto int_of = [env](jobject target, const char *name) {
        jmethodID method = env->GetMethodID(env->GetObjectClass(target), name, "()I");
        if (!method) { env->ExceptionClear(); return 0; }
        const jint value = env->CallIntMethod(target, method);
        return exception(env, name) ? 0 : int(value);
    };
    const auto type_of = [env](jclass types, const char *name) {
        jmethodID method = env->GetStaticMethodID(types, name, "()I");
        if (!method) { env->ExceptionClear(); return 0; }
        const jint value = env->CallStaticIntMethod(types, method);
        return exception(env, name) ? 0 : int(value);
    };
    jclass types = env->FindClass("android/view/WindowInsets$Type");
    if (!types) {
        env->ExceptionClear();
        return Edges{int_of(insets, "getSystemWindowInsetLeft"), int_of(insets, "getSystemWindowInsetTop"),
                     int_of(insets, "getSystemWindowInsetRight"), int_of(insets, "getSystemWindowInsetBottom")};
    }
    const jint mask = type_of(types, "systemBars") | type_of(types, "displayCutout") | type_of(types, "ime");
    jmethodID get = env->GetMethodID(env->GetObjectClass(insets), "getInsets", "(I)Landroid/graphics/Insets;");
    if (!get) { env->ExceptionClear(); return std::nullopt; }
    jobject sides = env->CallObjectMethod(insets, get, mask);
    if (exception(env, "getInsets") || !sides) return std::nullopt;
    jclass sides_class = env->GetObjectClass(sides);
    const auto side = [env, sides, sides_class](const char *name) {
        jfieldID field = env->GetFieldID(sides_class, name, "I");
        if (!field) { env->ExceptionClear(); return 0; }
        return int(env->GetIntField(sides, field));
    };
    return Edges{side("left"), side("top"), side("right"), side("bottom")};
}

// The app thread never returns to Java, so its local references are freed by a frame.
static std::optional<Edges> system_bars(JNIEnv *env)
{
    if (!env || !activity || env->PushLocalFrame(16) != JNI_OK) return std::nullopt;
    const auto edges = system_bars_in(env);
    env->PopLocalFrame(nullptr);
    return edges;
}

void insets_changed()
{
    insets_moved = true;
    if (app && app->looper) ALooper_wake(app->looper);
}

void resize()
{
    if (!app || !app->window) return;
    const int width = ANativeWindow_getWidth(app->window);
    const int height = ANativeWindow_getHeight(app->window);
    int dpi = AConfiguration_getDensity(app->config);
    if (dpi <= 0 || dpi >= ACONFIGURATION_DENSITY_ANY) dpi = 160;
    Android_SetScreenResolution(width, height, width, height, float(dpi) / 160.0f, 60.0f);
    Android_SetFormat(ANativeWindow_getFormat(app->window), ANativeWindow_getFormat(app->window));
    if (Android_Window) Android_SendResize(Android_Window);
    // Since API 35 the window is drawn edge to edge: the content rect then covers the
    // system bars, so what they cover is read from the window's insets as well.
    const ARect &rect = app->contentRect;
    Edges edges = rect.right > rect.left && rect.bottom > rect.top
        ? Edges{rect.left, rect.top, SDL_max(0, width - rect.right), SDL_max(0, height - rect.bottom)}
        : Edges{};
    if (const auto bars = system_bars(Android_JNI_GetEnv())) {
        edges = {SDL_max(edges.left, bars->left), SDL_max(edges.top, bars->top),
                 SDL_max(edges.right, bars->right), SDL_max(edges.bottom, bars->bottom)};
    }
    Android_SetWindowSafeAreaInsets(edges.left, edges.right, edges.top, edges.bottom);
}

static void release_surface()
{
    if (!Android_Window || !Android_Window->internal) return;
    auto *data = Android_Window->internal;
#ifdef SDL_VIDEO_OPENGL_EGL
    if ((Android_Window->flags & SDL_WINDOW_OPENGL) && !data->backup_done) {
        data->egl_context = SDL_GL_GetCurrentContext();
        data->has_swap_interval = SDL_GL_GetSwapInterval(&data->swap_interval);
        SDL_GL_MakeCurrent(Android_Window, nullptr);
        data->backup_done = true;
    }
    if (data->egl_surface != EGL_NO_SURFACE) {
        SDL_EGL_DestroySurface(SDL_GetVideoDevice(), data->egl_surface);
        data->egl_surface = EGL_NO_SURFACE;
        SDL_SetPointerProperty(SDL_GetWindowProperties(Android_Window), SDL_PROP_WINDOW_ANDROID_SURFACE_POINTER, nullptr);
    }
#endif
    if (data->native_window) {
        ANativeWindow_release(data->native_window);
        data->native_window = nullptr;
        SDL_SetPointerProperty(SDL_GetWindowProperties(Android_Window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    }
}

static void command(android_app *, int32_t cmd)
{
    Android_LockActivityMutex();
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        resize();
        if (Android_Window && Android_Window->internal) {
            auto *data = Android_Window->internal;
            data->native_window = Android_JNI_GetNativeWindow();
            SDL_SetPointerProperty(SDL_GetWindowProperties(Android_Window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, data->native_window);
#ifdef SDL_VIDEO_OPENGL_EGL
            if ((Android_Window->flags & SDL_WINDOW_OPENGL) && !data->framebuffer) {
                data->egl_surface = SDL_EGL_CreateSurface(SDL_GetVideoDevice(), Android_Window, data->native_window);
                SDL_SetPointerProperty(SDL_GetWindowProperties(Android_Window), SDL_PROP_WINDOW_ANDROID_SURFACE_POINTER, data->egl_surface);
            }
#endif
        }
        if (resumed) Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_RESUME);
        break;
    case APP_CMD_TERM_WINDOW:
        release_surface();
        Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_PAUSE);
        break;
    case APP_CMD_RESUME:
        resumed = true;
        if (app->window) Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_RESUME);
        break;
    case APP_CMD_PAUSE:
        resumed = false;
        Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_PAUSE);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
    case APP_CMD_CONFIG_CHANGED:
        resize();
        break;
    case APP_CMD_GAINED_FOCUS:
        if (Android_Window) SDL_SetKeyboardFocus(Android_Window);
        break;
    case APP_CMD_LOST_FOCUS:
        SDL_SetKeyboardFocus(nullptr);
        break;
    case APP_CMD_LOW_MEMORY:
        Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_LOWMEMORY);
        break;
    case APP_CMD_DESTROY:
        quitting = true;
        Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_DESTROY);
        break;
    default:
        break;
    }
    Android_UnlockActivityMutex();
}

static int32_t input(android_app *, AInputEvent *event)
{
    if (!Android_Window) return 0;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        const int action = AMotionEvent_getAction(event);
        const int kind = action & AMOTION_EVENT_ACTION_MASK;
        const size_t selected = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        const size_t count = AMotionEvent_getPointerCount(event);
        for (size_t i = 0; i < count; ++i) {
            if (kind != AMOTION_EVENT_ACTION_MOVE && kind != AMOTION_EVENT_ACTION_CANCEL && i != selected) continue;
            Android_OnTouch(Android_Window, AInputEvent_getDeviceId(event), AMotionEvent_getPointerId(event, i), kind,
                AMotionEvent_getX(event, i) / SDL_max(1, Android_SurfaceWidth),
                AMotionEvent_getY(event, i) / SDL_max(1, Android_SurfaceHeight), AMotionEvent_getPressure(event, i));
        }
        return 1;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const int code = AKeyEvent_getKeyCode(event);
        const int action = AKeyEvent_getAction(event);
        if (code == AKEYCODE_BACK && !back_trapped) return 0;
        if (action == AKEY_EVENT_ACTION_DOWN) Android_OnKeyDown(code);
        else if (action == AKEY_EVENT_ACTION_UP) Android_OnKeyUp(code);
        else return 0;
        // Hardware keyboard characters come from Android's keymap, including
        // the active layout. The IME handles its own input through the bridge.
        if (action == AKEY_EVENT_ACTION_DOWN && !SDL_ScreenKeyboardShown(Android_Window)) {
            JNIEnv *env = Android_JNI_GetEnv();
            Frame frame(env);
            if (frame) {
                jclass cls = env->FindClass("android/view/KeyCharacterMap");
                jmethodID load = env->GetStaticMethodID(cls, "load", "(I)Landroid/view/KeyCharacterMap;");
                jobject map = env->CallStaticObjectMethod(cls, load, AInputEvent_getDeviceId(event));
                jmethodID get = env->GetMethodID(cls, "get", "(II)I");
                jint c = map ? env->CallIntMethod(map, get, code, AKeyEvent_getMetaState(event)) : 0;
                if (!exception(env, "keyboard map") && c >= 32 && c <= 0x10ffff) {
                    char text[5]{};
                    SDL_UCS4ToUTF8(c, text);
                    SDL_SendKeyboardText(text);
                }
            }
        }
        return 1;
    }
    return 0;
}

// "Isn't responding", while the app goes on drawing: what keeps Android's UI
// thread or the app thread from answering, said in logcat (tag mux-watch).
// Each touch is finished on the app thread and acknowledged to the system on
// the UI thread (InputQueue's MSG_FINISH_INPUT), so either one held up past
// five seconds is an ANR. Once a second the UI thread is pinged; unanswered
// for two, its Java stack is logged -- and the app thread said where it has
// stayed out of its looper as long.
constexpr Uint64 kStalledMs = 2000;

// A line of the watchdog's: in logcat under its tag, and on stderr too --
// where the program keeps its own log (mux carries stderr to logcat and
// keeps it for Copy log), it is among the rest.
[[gnu::format(printf, 1, 2)]] static void say_watch(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    __android_log_write(ANDROID_LOG_WARN, "mux-watch", line);
    std::fprintf(stderr, "[mux-watch] %s\n", line);
}

static void log_main_stack(JNIEnv *env, Uint64 waited)
{
    say_watch("UI thread has not answered for %llu ms; it is at:",
                        static_cast<unsigned long long>(waited));
    Frame frame(env);
    if (!frame) return;
    jclass looper = env->FindClass("android/os/Looper");
    jobject main = looper ? env->CallStaticObjectMethod(looper, env->GetStaticMethodID(looper, "getMainLooper", "()Landroid/os/Looper;")) : nullptr;
    jobject thread = main ? env->CallObjectMethod(main, env->GetMethodID(looper, "getThread", "()Ljava/lang/Thread;")) : nullptr;
    jclass thread_class = env->FindClass("java/lang/Thread");
    auto frames = thread ? static_cast<jobjectArray>(env->CallObjectMethod(thread,
        env->GetMethodID(thread_class, "getStackTrace", "()[Ljava/lang/StackTraceElement;"))) : nullptr;
    if (exception(env, "UI thread stack") || !frames) return;
    jclass element = env->FindClass("java/lang/StackTraceElement");
    jmethodID text = env->GetMethodID(element, "toString", "()Ljava/lang/String;");
    const jsize count = std::min<jsize>(env->GetArrayLength(frames), 40);
    for (jsize i = 0; i < count; ++i) {
        jobject one = env->GetObjectArrayElement(frames, i);
        auto line = static_cast<jstring>(env->CallObjectMethod(one, text));
        if (exception(env, "UI thread frame")) return;
        say_watch("    at %s", utf8(env, line).c_str());
        env->DeleteLocalRef(line);
        env->DeleteLocalRef(one);
    }
}

static void watch()
{
    JNIEnv *env = Android_JNI_GetEnv();
    Uint64 asked = 0;
    bool told_ui = false, told_app = false;
    while (watching) {
        SDL_Delay(250);
        const Uint64 now = SDL_GetTicks();
        // Paused, the activity is not asked to answer anything.
        if (!resumed) {
            asked = 0;
            told_ui = told_app = false;
            continue;
        }
        if (asked != 0 && ui_answered < asked) {
            if (!told_ui && now - asked > kStalledMs) {
                log_main_stack(env, now - asked);
                told_ui = true;
            }
        } else {
            if (told_ui)
                say_watch("UI thread answered after %llu ms",
                                    static_cast<unsigned long long>(now - asked));
            told_ui = false;
            if (asked == 0 || now - asked >= 1000) {
                asked = now;
                ping_ui(now);
            }
        }
        const Uint64 left = app_left_poll;
        // Left after `now` was read -- the app thread polled again since --
        // is no stall: unsigned, `now - left` came out as 2^64 - 1 ms.
        const bool out = !app_polling && left != 0 && left <= now && now - left > kStalledMs;
        if (out && !told_app)
            say_watch("app thread out of its looper for %llu ms (in a frame or the app's work)",
                                static_cast<unsigned long long>(now - left));
        if (!out && told_app)
            say_watch("app thread back in its looper");
        told_app = out;
    }
}
} // namespace native_activity

using namespace native_activity;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *machine, void *)
{
    vm = machine;
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || !register_bridge(env)) return JNI_ERR;
    return JNI_VERSION_1_6;
}

extern "C" void SDL_AndroidNativeActivity(ANativeActivity *native, void *saved, size_t size, SDL_NativeActivityMain entry)
{
    vm = native->vm;
    activity = native->env->NewGlobalRef(native->clazz);
    if (!activity_mutex) activity_mutex = SDL_CreateMutex();
    if (!event_mutex) event_mutex = SDL_CreateMutex();
    main_function = entry;
    resumed = false;
    quitting = false;
    pending.clear();
    SDL_NativeGlue_onCreate(native, saved, size);
}

extern "C" void android_main(android_app *native)
{
    app = native;
    app->onAppCmd = command;
    app->onInputEvent = input;
    while (!app->window && !app->destroyRequested) {
        int events;
        android_poll_source *source = nullptr;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0 && source) source->process(app, source);
    }
    if (!app->destroyRequested) {
        char name[] = "SDL-native-activity";
        char *arguments[] = {name, nullptr};
        SDL_SetMainReady();
        watching = true;
        std::thread watchdog(watch);
        main_function(1, arguments);
        watching = false;
        watchdog.join();
        clear_documents();
        ANativeActivity_finish(app->activity);
    }
    // native_app_glue owns the activity until onDestroy; do not retain it
    // beyond that callback. No callbacks into application code follow exit.
    while (!app->destroyRequested) {
        int events;
        android_poll_source *source = nullptr;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0 && source) source->process(app, source);
    }
    JNIEnv *env = Android_JNI_GetEnv();
    release_bridge(env);
    env->DeleteGlobalRef(activity);
    activity = nullptr;
    app = nullptr;
}

JNIEnv *Android_JNI_GetEnv()
{
    if (!vm) return nullptr;
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_once(&attachment_once, make_attachment_key);
    pthread_setspecific(attachment, env);
    return env;
}

bool Android_JNI_SetupThread() { return Android_JNI_GetEnv() != nullptr; }
void *SDL_GetAndroidJNIEnv() { return Android_JNI_GetEnv(); }
void *SDL_GetAndroidActivity() { auto *env = Android_JNI_GetEnv(); return env && activity ? env->NewLocalRef(activity) : nullptr; }
int SDL_GetAndroidSDKVersion() { return app ? app->activity->sdkVersion : 0; }
void Android_LockActivityMutex() { SDL_LockMutex(activity_mutex); }
void Android_UnlockActivityMutex() { SDL_UnlockMutex(activity_mutex); }
void Android_SetAllowRecreateActivity(bool) {}
void Android_JNI_SetBackButtonTrapActive(bool enabled) { back_trapped = enabled; }
bool Android_BackButtonTrapped(void) { return back_trapped; }

void Android_SendLifecycleEvent(SDL_AndroidLifecycleEvent event)
{
    SDL_LockMutex(event_mutex);
    if (event == SDL_ANDROID_LIFECYCLE_DESTROY) pending.clear();
    if (std::find(pending.begin(), pending.end(), event) == pending.end()) pending.push_back(event);
    if (app && app->looper) ALooper_wake(app->looper);
    SDL_UnlockMutex(event_mutex);
}

bool Android_WaitLifecycleEvent(SDL_AndroidLifecycleEvent *event, Sint64 timeout)
{
    const Uint64 start = SDL_GetTicksNS();
    for (;;) {
        pump_documents();
        drain_input();
        if (insets_moved.exchange(false)) {
            Android_LockActivityMutex();
            resize();
            Android_UnlockActivityMutex();
        }
        SDL_LockMutex(event_mutex);
        if (!pending.empty()) {
            *event = pending.front();
            pending.erase(pending.begin());
            SDL_UnlockMutex(event_mutex);
            return true;
        }
        SDL_UnlockMutex(event_mutex);
        if (!app || quitting) return false;
        Sint64 remaining = timeout < 0 ? -1 : SDL_max(Sint64(0), timeout - Sint64(SDL_GetTicksNS() - start));
        int milliseconds = remaining < 0 ? -1 : int(SDL_min(Sint64(INT_MAX), (remaining + 999999) / 1000000));
        int events;
        android_poll_source *source = nullptr;
        app_polling = true;
        int result = ALooper_pollOnce(milliseconds, nullptr, &events, reinterpret_cast<void **>(&source));
        if (result >= 0 && source) source->process(app, source);
        app_left_poll = SDL_GetTicks();
        app_polling = false;
        if (result == ALOOPER_POLL_TIMEOUT || result == ALOOPER_POLL_ERROR) return false;
        if (result == ALOOPER_POLL_WAKE) {
            *event = SDL_ANDROID_LIFECYCLE_WAKE;
            return true;
        }
    }
}

ANativeWindow *Android_JNI_GetNativeWindow()
{
    if (!app || !app->window) return nullptr;
    ANativeWindow_acquire(app->window);
    return app->window;
}

SDL_DisplayOrientation Android_JNI_GetDisplayNaturalOrientation() { return SDL_ORIENTATION_PORTRAIT; }
SDL_DisplayOrientation Android_JNI_GetDisplayCurrentOrientation()
{
    return app && AConfiguration_getOrientation(app->config) == ACONFIGURATION_ORIENTATION_LAND
        ? SDL_ORIENTATION_LANDSCAPE : SDL_ORIENTATION_PORTRAIT;
}
void Android_JNI_InitTouch() { SDL_AddTouch(1, SDL_TOUCH_DEVICE_DIRECT, "Android touchscreen"); }
void Android_JNI_ShowScreenKeyboard(int type, SDL_Rect *rect) { show_keyboard(type, *rect); }
void Android_JNI_HideScreenKeyboard() { hide_keyboard(); }
bool Android_JNI_ShouldMinimizeOnFocusLoss() { return false; }
void Android_JNI_SetOrientation(int, int, int, const char *) {} // Follow the activity's manifest and configuration.
void Android_JNI_SetWindowStyle(bool fullscreen)
{
    if (app) ANativeActivity_setWindowFlags(app->activity, fullscreen ? AWINDOW_FLAG_FULLSCREEN : 0,
                                          fullscreen ? 0 : AWINDOW_FLAG_FULLSCREEN);
}
bool Android_JNI_SuspendScreenSaver(bool suspend)
{
    if (!app) return false;
    ANativeActivity_setWindowFlags(app->activity, suspend ? AWINDOW_FLAG_KEEP_SCREEN_ON : 0,
                                  suspend ? 0 : AWINDOW_FLAG_KEEP_SCREEN_ON);
    return true;
}
