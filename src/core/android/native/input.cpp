// SPDX-License-Identifier: Zlib
// JNI callbacks for the source-generated bridge in the j4niwzis fork.
#include "bridge.hpp"
#include <algorithm>
#include <mutex>
#include <deque>

namespace native_activity {
static jclass activity_class, view_class, connection_class, runner_class;
static jobject input_view, connection, runner;
static int input_type = 1;
static std::u16string committed;

enum class Action { show_keyboard, hide_keyboard, title, minimize, open_url, document };
struct Request {
    Action action;
    int type = 0;
    SDL_Rect rect{};
    std::string text;
};
static std::mutex requests_mutex;
static std::deque<Request> requests;

static bool enqueue(Request request)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity || !runner) return SDL_SetError("NativeActivity is not ready");
    {
        const std::lock_guard<std::mutex> lock(requests_mutex);
        requests.push_back(std::move(request));
    }
    env->CallVoidMethod(activity, env->GetMethodID(activity_class, "runOnUiThread", "(Ljava/lang/Runnable;)V"), runner);
    return !exception(env, "schedule UI callback");
}

static jobject editable(JNIEnv *env, jobject self)
{
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    return env->CallObjectMethod(self, env->GetMethodID(base, "getEditable", "()Landroid/text/Editable;"));
}

static std::u16string characters(JNIEnv *env, jobject sequence)
{
    if (!sequence) return {};
    auto text = static_cast<jstring>(env->CallObjectMethod(sequence,
        env->GetMethodID(env->GetObjectClass(sequence), "toString", "()Ljava/lang/String;")));
    const jsize size = env->GetStringLength(text);
    const jchar *chars = env->GetStringChars(text, nullptr);
    if (!chars) return {};
    std::u16string result(chars, chars + size);
    env->ReleaseStringChars(text, chars);
    return result;
}

static size_t codepoints(const std::u16string &text, size_t begin = 0, size_t end = SIZE_MAX)
{
    end = std::min(end, text.size());
    size_t count = 0;
    for (size_t i = begin; i < end; ++i, ++count) {
        if (text[i] >= 0xd800 && text[i] <= 0xdbff && i + 1 < end &&
            text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) ++i;
    }
    return count;
}

static std::string text_utf8(JNIEnv *env, const std::u16string &text)
{
    return utf8(env, env->NewString(reinterpret_cast<const jchar *>(text.data()), jsize(text.size())));
}

static void synchronize_text(JNIEnv *env, jobject self)
{
    if (!connection || !env->IsSameObject(self, connection)) return;
    jobject content = editable(env, self);
    if (!content || exception(env, "read editor")) return;
    auto text = characters(env, content);
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    jint first = env->CallStaticIntMethod(base, env->GetStaticMethodID(base,
        "getComposingSpanStart", "(Landroid/text/Spannable;)I"), content);
    jint last = env->CallStaticIntMethod(base, env->GetStaticMethodID(base,
        "getComposingSpanEnd", "(Landroid/text/Spannable;)I"), content);
    std::u16string composition;
    if (first >= 0 && last >= first && size_t(last) <= text.size()) {
        composition = text.substr(size_t(first), size_t(last - first));
        text.erase(size_t(first), size_t(last - first));
    }
    size_t common = 0;
    while (common < text.size() && common < committed.size() && text[common] == committed[common]) ++common;
    // A common high surrogate is not a common character.
    if (common && common < committed.size() && committed[common - 1] >= 0xd800 && committed[common - 1] <= 0xdbff) --common;
    Android_LockActivityMutex();
    for (size_t n = codepoints(committed, common); n; --n) SDL_SendKeyboardKeyAutoRelease(0, SDL_SCANCODE_BACKSPACE);
    if (common < text.size()) SDL_SendKeyboardText(text_utf8(env, text.substr(common)).c_str());
    SDL_SendEditingText(text_utf8(env, composition).c_str(), int(codepoints(composition)), 0);
    Android_UnlockActivityMutex();
    committed = std::move(text);
}

static jboolean commit(JNIEnv *env, jobject self, jobject text, jint cursor)
{
    Frame frame(env);
    if (!frame) return false;
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    bool done = env->CallNonvirtualBooleanMethod(self, base, env->GetMethodID(base,
        "commitText", "(Ljava/lang/CharSequence;I)Z"), text, cursor);
    if (done && !exception(env, "commit text")) synchronize_text(env, self);
    return done;
}

static jboolean compose(JNIEnv *env, jobject self, jobject text, jint cursor)
{
    Frame frame(env);
    if (!frame) return false;
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    bool done = env->CallNonvirtualBooleanMethod(self, base, env->GetMethodID(base,
        "setComposingText", "(Ljava/lang/CharSequence;I)Z"), text, cursor);
    if (done && !exception(env, "compose text")) synchronize_text(env, self);
    return done;
}

static jboolean finish(JNIEnv *env, jobject self)
{
    Frame frame(env);
    if (!frame) return false;
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    bool done = env->CallNonvirtualBooleanMethod(self, base, env->GetMethodID(base, "finishComposingText", "()Z"));
    if (done && !exception(env, "finish composition")) synchronize_text(env, self);
    return done;
}

static jboolean erase(JNIEnv *env, jobject self, jint before, jint after, bool points)
{
    if (before < 0 || after < 0) return false;
    Frame frame(env);
    if (!frame) return false;
    jobject content = editable(env, self);
    const auto old = characters(env, content);
    jclass selection = env->FindClass("android/text/Selection");
    jint cursor = env->CallStaticIntMethod(selection, env->GetStaticMethodID(selection,
        "getSelectionStart", "(Ljava/lang/CharSequence;)I"), content);
    cursor = std::clamp(cursor, 0, jint(old.size()));
    const size_t available_before = points ? codepoints(old, 0, cursor) : size_t(cursor);
    const size_t available_after = points ? codepoints(old, cursor) : old.size() - size_t(cursor);
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    const char *method = points ? "deleteSurroundingTextInCodePoints" : "deleteSurroundingText";
    bool done = env->CallNonvirtualBooleanMethod(self, base, env->GetMethodID(base, method, "(II)Z"), before, after);
    if (!done || exception(env, "delete text")) return false;
    synchronize_text(env, self);
    // The app can contain text typed before this input connection existed.
    Android_LockActivityMutex();
    for (size_t n = available_before; n < size_t(before); ++n) SDL_SendKeyboardKeyAutoRelease(0, SDL_SCANCODE_BACKSPACE);
    for (size_t n = available_after; n < size_t(after); ++n) SDL_SendKeyboardKeyAutoRelease(0, SDL_SCANCODE_DELETE);
    Android_UnlockActivityMutex();
    return true;
}
static jboolean erase_units(JNIEnv *e, jobject s, jint b, jint a) { return erase(e, s, b, a, false); }
static jboolean erase_points(JNIEnv *e, jobject s, jint b, jint a) { return erase(e, s, b, a, true); }

static jboolean key(JNIEnv *env, jobject, jobject event)
{
    Frame frame(env);
    if (!frame || !event) return false;
    jclass cls = env->GetObjectClass(event);
    jint code = env->CallIntMethod(event, env->GetMethodID(cls, "getKeyCode", "()I"));
    jint action = env->CallIntMethod(event, env->GetMethodID(cls, "getAction", "()I"));
    if (exception(env, "read key event")) return false;
    // Let Android handle system navigation and volume keys.
    if (code == AKEYCODE_BACK || code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN) return false;
    Android_LockActivityMutex();
    if (action == AKEY_EVENT_ACTION_DOWN) Android_OnKeyDown(code);
    else if (action == AKEY_EVENT_ACTION_UP) Android_OnKeyUp(code);
    if (action == AKEY_EVENT_ACTION_DOWN) {
        jint c = env->CallIntMethod(event, env->GetMethodID(cls, "getUnicodeChar", "()I"));
        if (c >= 32 && c <= 0x10ffff) {
            char text[5]{};
            SDL_UCS4ToUTF8(c, text);
            SDL_SendKeyboardText(text);
        }
    }
    Android_UnlockActivityMutex();
    return !exception(env, "key input");
}

static jboolean view_key(JNIEnv *env, jobject self, jint, jobject event) { return key(env, self, event); }
static jboolean pre_ime(JNIEnv *env, jobject, jint code, jobject event)
{
    if (code == AKEYCODE_BACK && env->CallIntMethod(event,
        env->GetMethodID(env->GetObjectClass(event), "getAction", "()I")) == AKEY_EVENT_ACTION_UP) {
        Android_LockActivityMutex();
        SDL_SendScreenKeyboardHidden();
        Android_UnlockActivityMutex();
    }
    return false; // The IME still receives Back and performs the dismissal.
}

static jobject insets(JNIEnv *env, jobject self, jobject value)
{
    if (SDL_GetAndroidSDKVersion() >= 30) {
        jclass type = env->FindClass("android/view/WindowInsets$Type");
        jint mask = env->CallStaticIntMethod(type, env->GetStaticMethodID(type, "ime", "()I"));
        bool visible = env->CallBooleanMethod(value, env->GetMethodID(env->GetObjectClass(value), "isVisible", "(I)Z"), mask);
        if (!exception(env, "keyboard insets")) {
            Android_LockActivityMutex();
            if (visible) SDL_SendScreenKeyboardShown();
            else SDL_SendScreenKeyboardHidden();
            Android_UnlockActivityMutex();
        }
    }
    jclass base = env->FindClass("android/view/View");
    return env->CallNonvirtualObjectMethod(self, base, env->GetMethodID(base, "onApplyWindowInsets",
        "(Landroid/view/WindowInsets;)Landroid/view/WindowInsets;"), value);
}

static jboolean editor(JNIEnv *, jobject) { return true; }

static jobject create_connection(JNIEnv *env, jobject self, jobject info)
{
    jclass cls = env->GetObjectClass(info);
    env->SetIntField(info, env->GetFieldID(cls, "inputType", "I"), input_type);
    // IME_FLAG_NO_EXTRACT_UI | IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_PERSONALIZED_LEARNING.
    env->SetIntField(info, env->GetFieldID(cls, "imeOptions", "I"), 0x10000000 | 0x02000000 | 0x01000000);
    if (connection) env->DeleteGlobalRef(connection);
    jobject made = env->NewObject(connection_class, env->GetMethodID(connection_class,
        "<init>", "(Landroid/view/View;Z)V"), self, JNI_TRUE);
    connection = env->NewGlobalRef(made);
    committed.clear();
    return made;
}

static void create(JNIEnv *env, jobject self, jobject saved)
{
    jclass base = env->FindClass("android/app/NativeActivity");
    env->CallNonvirtualVoidMethod(self, base, env->GetMethodID(base, "onCreate", "(Landroid/os/Bundle;)V"), saved);
    if (exception(env, "create native activity")) return;
    jobject view = env->NewObject(view_class, env->GetMethodID(view_class, "<init>", "(Landroid/content/Context;)V"), self);
    input_view = env->NewGlobalRef(view);
    runner = env->NewGlobalRef(env->NewObject(runner_class, env->GetMethodID(runner_class, "<init>", "()V")));
    jclass cls = env->FindClass("android/view/View");
    env->CallVoidMethod(view, env->GetMethodID(cls, "setFocusable", "(Z)V"), JNI_TRUE);
    env->CallVoidMethod(view, env->GetMethodID(cls, "setFocusableInTouchMode", "(Z)V"), JNI_TRUE);
    env->CallVoidMethod(view, env->GetMethodID(cls, "setAlpha", "(F)V"), 0.0f);
    jclass params = env->FindClass("android/view/ViewGroup$LayoutParams");
    jobject layout = env->NewObject(params, env->GetMethodID(params, "<init>", "(II)V"), 1, 1);
    env->CallVoidMethod(self, env->GetMethodID(base, "addContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V"), view, layout);
    exception(env, "create input view");
}

void open_document_on_ui(JNIEnv *, int, const std::string &);

static void run(JNIEnv *env, jobject)
{
    std::deque<Request> work;
    {
        const std::lock_guard<std::mutex> lock(requests_mutex);
        work.swap(requests);
    }
    for (const auto &request : work) {
        Frame frame(env);
        if (!frame || !activity || !input_view) return;
        jclass view = env->FindClass("android/view/View");
        switch (request.action) {
        case Action::show_keyboard: {
            input_type = request.type;
            jclass params = env->FindClass("android/view/ViewGroup$LayoutParams");
            jobject layout = env->NewObject(params, env->GetMethodID(params, "<init>", "(II)V"),
                                           SDL_max(1, request.rect.w), SDL_max(1, request.rect.h));
            env->CallVoidMethod(input_view, env->GetMethodID(view, "setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V"), layout);
            env->CallVoidMethod(input_view, env->GetMethodID(view, "setX", "(F)V"), float(request.rect.x));
            env->CallVoidMethod(input_view, env->GetMethodID(view, "setY", "(F)V"), float(request.rect.y));
            env->CallBooleanMethod(input_view, env->GetMethodID(view, "requestFocus", "()Z"));
            jobject manager = service(env, "input_method");
            jclass cls = env->GetObjectClass(manager);
            env->CallVoidMethod(manager, env->GetMethodID(cls, "restartInput", "(Landroid/view/View;)V"), input_view);
            if (env->CallBooleanMethod(manager, env->GetMethodID(cls, "showSoftInput", "(Landroid/view/View;I)Z"), input_view, 0)) {
                Android_LockActivityMutex();
                SDL_SendScreenKeyboardShown();
                Android_UnlockActivityMutex();
            }
            break;
        }
        case Action::hide_keyboard: {
            jobject manager = service(env, "input_method");
            jobject token = env->CallObjectMethod(input_view, env->GetMethodID(view, "getWindowToken", "()Landroid/os/IBinder;"));
            env->CallBooleanMethod(manager, env->GetMethodID(env->GetObjectClass(manager), "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z"), token, 0);
            env->CallVoidMethod(input_view, env->GetMethodID(view, "clearFocus", "()V"));
            Android_LockActivityMutex();
            SDL_SendScreenKeyboardHidden();
            Android_UnlockActivityMutex();
            break;
        }
        case Action::title:
            env->CallVoidMethod(activity, env->GetMethodID(activity_class, "setTitle", "(Ljava/lang/CharSequence;)V"), string(env, request.text.c_str()));
            break;
        case Action::minimize:
            env->CallBooleanMethod(activity, env->GetMethodID(activity_class, "moveTaskToBack", "(Z)Z"), JNI_TRUE);
            break;
        case Action::open_url: {
            jclass uri = env->FindClass("android/net/Uri");
            jobject parsed = env->CallStaticObjectMethod(uri, env->GetStaticMethodID(uri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), string(env, request.text.c_str()));
            jclass intent = env->FindClass("android/content/Intent");
            jobject made = env->NewObject(intent, env->GetMethodID(intent, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"), env->NewStringUTF("android.intent.action.VIEW"), parsed);
            env->CallVoidMethod(activity, env->GetMethodID(activity_class, "startActivity", "(Landroid/content/Intent;)V"), made);
            break;
        }
        case Action::document:
            open_document_on_ui(env, request.type, request.text);
            break;
        }
        exception(env, "UI request");
    }
}

static void result(JNIEnv *env, jobject, jint request, jint answer, jobject intent)
{
    document_result(env, request, answer, intent);
}

bool register_bridge(JNIEnv *env)
{
    const auto load = [env](const char *name, jclass &where, const JNINativeMethod *methods, int count) {
        jclass cls = env->FindClass(name);
        if (!cls || env->RegisterNatives(cls, methods, count) != JNI_OK) return false;
        where = static_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
        return where != nullptr;
    };
#define NATIVE(name, signature, function) {const_cast<char *>(name), const_cast<char *>(signature), reinterpret_cast<void *>(function)}
    const JNINativeMethod activity_methods[] = {
        NATIVE("onCreate", "(Landroid/os/Bundle;)V", create),
        NATIVE("onActivityResult", "(IILandroid/content/Intent;)V", result)
    };
    const JNINativeMethod view_methods[] = {
        NATIVE("onCheckIsTextEditor", "()Z", editor),
        NATIVE("onCreateInputConnection", "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;", create_connection),
        NATIVE("onKeyDown", "(ILandroid/view/KeyEvent;)Z", view_key),
        NATIVE("onKeyUp", "(ILandroid/view/KeyEvent;)Z", view_key),
        NATIVE("onKeyPreIme", "(ILandroid/view/KeyEvent;)Z", pre_ime),
        NATIVE("onApplyWindowInsets", "(Landroid/view/WindowInsets;)Landroid/view/WindowInsets;", insets)
    };
    const JNINativeMethod connection_methods[] = {
        NATIVE("commitText", "(Ljava/lang/CharSequence;I)Z", commit),
        NATIVE("setComposingText", "(Ljava/lang/CharSequence;I)Z", compose),
        NATIVE("finishComposingText", "()Z", finish),
        NATIVE("deleteSurroundingText", "(II)Z", erase_units),
        NATIVE("deleteSurroundingTextInCodePoints", "(II)Z", erase_points),
        NATIVE("sendKeyEvent", "(Landroid/view/KeyEvent;)Z", key)
    };
    const JNINativeMethod runner_methods[] = {NATIVE("run", "()V", run)};
#undef NATIVE
    return load("org/libsdl/nativeapp/NativeActivity", activity_class, activity_methods, SDL_arraysize(activity_methods)) &&
           load("org/libsdl/nativeapp/InputView", view_class, view_methods, SDL_arraysize(view_methods)) &&
           load("org/libsdl/nativeapp/InputConnection", connection_class, connection_methods, SDL_arraysize(connection_methods)) &&
           load("org/libsdl/nativeapp/Runner", runner_class, runner_methods, SDL_arraysize(runner_methods));
}

void release_bridge(JNIEnv *env)
{
    const std::lock_guard<std::mutex> lock(requests_mutex);
    requests.clear();
    for (jobject *ref : {&input_view, &connection, &runner}) {
        if (*ref) env->DeleteGlobalRef(*ref);
        *ref = nullptr;
    }
    committed.clear();
}

void show_keyboard(int type, const SDL_Rect &rect) { enqueue({Action::show_keyboard, type, rect, {}}); }
void hide_keyboard() { enqueue({Action::hide_keyboard}); }
bool request_document(int type, const char *name) { return enqueue({Action::document, type, {}, name ? name : ""}); }
} // namespace native_activity

void Android_JNI_SetActivityTitle(const char *title) { native_activity::enqueue({native_activity::Action::title, 0, {}, title}); }
void Android_JNI_MinimizeWindow() { native_activity::enqueue({native_activity::Action::minimize}); }
bool Android_JNI_OpenURL(const char *url) { return native_activity::enqueue({native_activity::Action::open_url, 0, {}, url}); }
