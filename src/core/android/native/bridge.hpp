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

#include <string>
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
}
