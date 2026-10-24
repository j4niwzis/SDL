// SPDX-License-Identifier: Zlib
// NativeActivity backend for the j4niwzis fork.
#pragma once

extern "C" {
#include "SDL_internal.h"
#include "../SDL_android.h"
#include "../../../video/android/SDL_androidvideo.h"
#include "../../../video/android/SDL_androidwindow.h"
#include "../../../video/android/SDL_androidevents.h"
#include "../../../video/android/SDL_androidkeyboard.h"
#include "../../../video/android/SDL_androidtouch.h"
#include "../../../events/SDL_keyboard_c.h"
#include "../../../events/SDL_mouse_c.h"
#include "../../../events/SDL_touch_c.h"
#include "../../../events/SDL_windowevents_c.h"
#include <android_native_app_glue.h>
#include <SDL3/SDL_nativeactivity.h>
}

#include <atomic>
#include <string>
#include <variant>
#include <vector>

namespace native_activity {
extern android_app *app;
extern JavaVM *vm;
extern jobject activity;
extern SDL_Mutex *activity_mutex;

// JNI local references belong to the thread which created them.
class Frame {
public:
    explicit Frame(JNIEnv *env) : env_(env), valid_(env && env->PushLocalFrame(64) == JNI_OK) {}
    ~Frame() { if (valid_) env_->PopLocalFrame(nullptr); }
    explicit operator bool() const { return valid_; }
private:
    JNIEnv *env_;
    bool valid_;
};

bool exception(JNIEnv *env, const char *operation);
std::string utf8(JNIEnv *env, jstring text);
jstring string(JNIEnv *env, const char *text);
jobject service(JNIEnv *env, const char *name);
bool register_bridge(JNIEnv *env);
void release_bridge(JNIEnv *env);
void show_keyboard(int input_type, const SDL_Rect &rect);
void hide_keyboard();
void document_result(JNIEnv *env, jint request, jint result, jobject data);
void pump_documents();
void clear_documents();
void resize();
// The window's insets moved, told from the UI thread: the app thread resizes.
void insets_changed();

// What the UI thread was told -- typed, committed, a key, the keyboard shown
// or hidden -- handed to the app thread, which gives it to SDL. The UI thread
// never waits on the activity mutex: the app thread holds it across every
// frame's eglSwapBuffers, and a swap that blocked held up Android's own
// thread behind it -- "isn't responding", over and over, while the app
// itself went on.
namespace said {
struct text { std::string utf8; };
struct editing { std::string utf8; int length; };
struct tapped { SDL_Scancode code; };  // pressed and let go: Backspace, Delete
struct key_down { int code; };
struct key_up { int code; };
struct keyboard_shown {};
struct keyboard_hidden {};
}
using input_said = std::variant<said::text, said::editing, said::tapped, said::key_down, said::key_up,
                                said::keyboard_shown, said::keyboard_hidden>;
// The watchdog's ping (watch, in activity.cpp): asked of the UI thread
// through runOnUiThread, answered there with when it was asked.
void ping_ui(Uint64 stamp);
extern std::atomic<Uint64> ui_answered;

// From the UI thread: kept, and the app thread's looper woken.
void tell(input_said what);
// On the app thread, as it waits for events: what the UI thread told, given
// to SDL in the order it came.
void drain_input();
}
