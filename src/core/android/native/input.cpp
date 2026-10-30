// SPDX-License-Identifier: Zlib
// JNI callbacks for the source-generated bridge in the j4niwzis fork.
#include "bridge.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <optional>
#include <tuple>

namespace native_activity {
static jclass activity_class, view_class, connection_class, runner_class, receiver_class, push_receiver_class;
static jobject input_view, connection, runner;
static int input_type = 1;
static void take_request(JNIEnv *env, jobject intent);
static std::u16string committed;

enum class Action { show_keyboard, hide_keyboard, title, minimize, open_url, document, ping, editor_state };
struct Request {
    Action action;
    int type = 0;
    SDL_Rect rect{};
    std::string text;
    Uint64 stamp = 0;  // a ping's: when it was asked, answered as the UI thread runs it
    int anchor = 0, caret = 0;  // an editor state's selection, in bytes of `text`
};

// The field typed into, as the app says it is (SDL_SetAndroidEditorState):
// the input connection's text is it, and the input method edits it whole --
// its own caret where the app's is. Said as a whole too, what the input
// method made of it (SDL_TakeAndroidEditorState). Where the app says
// nothing, the input method is given an empty text, and what it types is
// told as keys and text at the app's caret, as before.
static bool state_mode = false;
static std::u16string field_text;
static jint field_anchor = 0, field_caret = 0;
static std::mutex edited_mutex;
static std::optional<std::tuple<std::string, int, int>> edited_state;

// UTF-8 and UTF-16, and offsets between them.
static std::u16string utf16_of(const std::string &text)
{
    std::u16string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        char32_t point = length == 1 ? lead : length == 2 ? (lead & 0x1f) : length == 3 ? (lead & 0x0f) : (lead & 0x07);
        for (size_t k = 1; k < length && i + k < text.size(); ++k)
            point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        i += length;
        if (point >= 0x10000) {
            point -= 0x10000;
            out.push_back(char16_t(0xd800 + (point >> 10)));
            out.push_back(char16_t(0xdc00 + (point & 0x3ff)));
        } else {
            out.push_back(char16_t(point));
        }
    }
    return out;
}
static std::string utf8_of(const std::u16string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        char32_t point = text[i];
        if (point >= 0xd800 && point <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
            point = 0x10000 + ((point - 0xd800) << 10) + (text[++i] - 0xdc00);
        if (point < 0x80) {
            out.push_back(char(point));
        } else if (point < 0x800) {
            out.push_back(char(0xc0 | (point >> 6)));
            out.push_back(char(0x80 | (point & 0x3f)));
        } else if (point < 0x10000) {
            out.push_back(char(0xe0 | (point >> 12)));
            out.push_back(char(0x80 | ((point >> 6) & 0x3f)));
            out.push_back(char(0x80 | (point & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (point >> 18)));
            out.push_back(char(0x80 | ((point >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((point >> 6) & 0x3f)));
            out.push_back(char(0x80 | (point & 0x3f)));
        }
    }
    return out;
}
// A byte offset into UTF-8 text as code units of the same text in UTF-16.
static jint units_at(const std::string &text, size_t bytes)
{
    jint units = 0;
    for (size_t i = 0; i < text.size() && i < bytes;) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        units += length == 4 ? 2 : 1;
        i += length;
    }
    return units;
}
static std::mutex requests_mutex;
static std::deque<Request> requests;

// What the UI thread was told, for the app thread (tell, drain_input): held
// only as long as it takes to add or take, never across SDL's own work.
static std::mutex said_mutex;
static std::deque<input_said> said_queue;

void tell(input_said what)
{
    {
        const std::lock_guard<std::mutex> lock(said_mutex);
        said_queue.push_back(std::move(what));
    }
    if (app && app->looper) ALooper_wake(app->looper);
}

namespace {
struct give {
    void operator()(const said::text &one) const { SDL_SendKeyboardText(one.utf8.c_str()); }
    void operator()(const said::editing &one) const { SDL_SendEditingText(one.utf8.c_str(), one.length, 0); }
    void operator()(const said::tapped &one) const { SDL_SendKeyboardKeyAutoRelease(0, one.code); }
    void operator()(const said::key_down &one) const { Android_OnKeyDown(one.code); }
    void operator()(const said::key_up &one) const { Android_OnKeyUp(one.code); }
    void operator()(const said::keyboard_shown &) const { SDL_SendScreenKeyboardShown(); }
    void operator()(const said::keyboard_hidden &) const { SDL_SendScreenKeyboardHidden(); }
};
}

void drain_input()
{
    std::deque<input_said> work;
    {
        const std::lock_guard<std::mutex> lock(said_mutex);
        work.swap(said_queue);
    }
    if (work.empty()) return;
    Android_LockActivityMutex();
    for (const input_said &one : work) std::visit(give{}, one);
    Android_UnlockActivityMutex();
}

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

// The connection's text made the field's, its selection the field's, and
// what the input method composes let go: the input method told where it is.
static void seed(JNIEnv *env, jobject with)
{
    jobject content = editable(env, with);
    if (!content || exception(env, "read editor to seed")) return;
    jclass editable_class = env->FindClass("android/text/Editable");
    jclass sequence = env->FindClass("java/lang/CharSequence");
    const jint length = env->CallIntMethod(content, env->GetMethodID(sequence, "length", "()I"));
    jstring text = env->NewString(reinterpret_cast<const jchar *>(field_text.data()), jsize(field_text.size()));
    env->CallObjectMethod(content, env->GetMethodID(editable_class, "replace", "(IILjava/lang/CharSequence;)Landroid/text/Editable;"), 0, length, text);
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    env->CallStaticVoidMethod(base, env->GetStaticMethodID(base, "removeComposingSpans", "(Landroid/text/Spannable;)V"), content);
    jclass selection = env->FindClass("android/text/Selection");
    const jint size = jint(field_text.size());
    env->CallStaticVoidMethod(selection, env->GetStaticMethodID(selection, "setSelection", "(Landroid/text/Spannable;II)V"), content,
                              std::clamp(field_anchor, 0, size), std::clamp(field_caret, 0, size));
    if (exception(env, "seed editor")) return;
    committed = field_text;
}

static void synchronize_text(JNIEnv *env, jobject self)
{
    if (!connection || !env->IsSameObject(self, connection)) return;
    jobject content = editable(env, self);
    if (!content || exception(env, "read editor")) return;
    if (state_mode) {
        // What the input method made, whole: its text, composition and all,
        // and its selection -- for the app to take as it is.
        const auto now = characters(env, content);
        jclass selection = env->FindClass("android/text/Selection");
        const jint start = env->CallStaticIntMethod(selection, env->GetStaticMethodID(selection,
            "getSelectionStart", "(Ljava/lang/CharSequence;)I"), content);
        const jint end = env->CallStaticIntMethod(selection, env->GetStaticMethodID(selection,
            "getSelectionEnd", "(Ljava/lang/CharSequence;)I"), content);
        if (exception(env, "read editor state")) return;
        const auto clamp = [&](jint at) { return size_t(std::clamp(at, jint(0), jint(now.size()))); };
        const std::string text = utf8_of(now);
        const int anchor = int(utf8_of(now.substr(0, clamp(start))).size());
        const int caret = int(utf8_of(now.substr(0, clamp(end))).size());
        {
            const std::lock_guard<std::mutex> lock(edited_mutex);
            edited_state = std::tuple{text, anchor, caret};
        }
        committed = now;
        // The app woken for it: an empty composition, which the field it
        // takes the state for lets go anyway.
        tell(said::editing{"", 0});
        return;
    }
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
    for (size_t n = codepoints(committed, common); n; --n) tell(said::tapped{SDL_SCANCODE_BACKSPACE});
    if (common < text.size()) tell(said::text{text_utf8(env, text.substr(common))});
    tell(said::editing{text_utf8(env, composition), int(codepoints(composition))});
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
    // The app can contain text typed before this input connection existed --
    // not where it said what its field is: the connection's text is all of it.
    if (state_mode) return true;
    for (size_t n = available_before; n < size_t(before); ++n) tell(said::tapped{SDL_SCANCODE_BACKSPACE});
    for (size_t n = available_after; n < size_t(after); ++n) tell(said::tapped{SDL_SCANCODE_DELETE});
    return true;
}
static jboolean select_text(JNIEnv *env, jobject self, jint start, jint end)
{
    Frame frame(env);
    if (!frame) return false;
    jclass base = env->FindClass("android/view/inputmethod/BaseInputConnection");
    bool done = env->CallNonvirtualBooleanMethod(self, base, env->GetMethodID(base, "setSelection", "(II)Z"), start, end);
    if (done && !exception(env, "select text")) synchronize_text(env, self);
    return done;
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
    // Back, where the app traps it, is the app's -- as the native input queue
    // gives it (activity.cpp): from the input view, which has the focus while
    // a text field does, it went to Android, and the back gesture left the
    // app instead of stepping back in it. With the keyboard up, the IME takes
    // Back first (pre_ime) and closes it. Volume keys stay Android's.
    if (code == AKEYCODE_BACK && !Android_BackButtonTrapped()) return false;
    if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN) return false;
    // Where the app said what its field is, the connection's text is the
    // field's: a key that types or deletes -- a digit, a space, Backspace,
    // as a keyboard sends some of them as keys -- is done to that text, and
    // the field takes it with the rest of what the input method made. Told
    // to the app as text and keys beside it, the field had it and the
    // connection did not: the next state the input method made, from its
    // text without it, took it out again -- digits gone at once, a space or
    // a letter that would not be deleted (a tester's, on Android).
    if (state_mode && connection) {
        const jint c = action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP
                           ? env->CallIntMethod(event, env->GetMethodID(cls, "getUnicodeChar", "()I")) : 0;
        if (exception(env, "read key's character")) return false;
        const bool deletes = code == AKEYCODE_DEL || code == AKEYCODE_FORWARD_DEL;
        const bool types = c >= 32 && c <= 0x10ffff;
        if (deletes || types) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                jobject content = editable(env, connection);
                jclass selection = env->FindClass("android/text/Selection");
                const jint start = env->CallStaticIntMethod(selection, env->GetStaticMethodID(selection,
                    "getSelectionStart", "(Ljava/lang/CharSequence;)I"), content);
                const jint end = env->CallStaticIntMethod(selection, env->GetStaticMethodID(selection,
                    "getSelectionEnd", "(Ljava/lang/CharSequence;)I"), content);
                if (exception(env, "read selection for a key")) return false;
                if (types || start != end) {
                    // Typed over what is selected, or what is selected deleted.
                    char text[5]{};
                    if (types) SDL_UCS4ToUTF8(c, text);
                    commit(env, connection, env->NewStringUTF(text), 1);
                } else if (code == AKEYCODE_DEL) {
                    erase(env, connection, 1, 0, true);
                } else {
                    erase(env, connection, 0, 1, true);
                }
            }
            return !exception(env, "key into the field's text");
        }
    }
    if (action == AKEY_EVENT_ACTION_DOWN) tell(said::key_down{code});
    else if (action == AKEY_EVENT_ACTION_UP) tell(said::key_up{code});
    if (action == AKEY_EVENT_ACTION_DOWN) {
        jint c = env->CallIntMethod(event, env->GetMethodID(cls, "getUnicodeChar", "()I"));
        if (c >= 32 && c <= 0x10ffff) {
            char text[5]{};
            SDL_UCS4ToUTF8(c, text);
            tell(said::text{text});
        }
    }
    return !exception(env, "key input");
}

static jboolean view_key(JNIEnv *env, jobject self, jint, jobject event) { return key(env, self, event); }
static jboolean pre_ime(JNIEnv *env, jobject, jint code, jobject event)
{
    if (code == AKEYCODE_BACK && env->CallIntMethod(event,
        env->GetMethodID(env->GetObjectClass(event), "getAction", "()I")) == AKEY_EVENT_ACTION_UP) {
        tell(said::keyboard_hidden{});
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
            if (visible) tell(said::keyboard_shown{});
            else tell(said::keyboard_hidden{});
        }
    }
    insets_changed();
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
    // The field's text and selection, where the app said them: the input
    // method starts from where the app is.
    if (state_mode) {
        seed(env, made);
        env->SetIntField(info, env->GetFieldID(cls, "initialSelStart", "I"), field_anchor);
        env->SetIntField(info, env->GetFieldID(cls, "initialSelEnd", "I"), field_caret);
    }
    return made;
}

static void create(JNIEnv *env, jobject self, jobject saved)
{
    jclass base = env->FindClass("android/app/NativeActivity");
    env->CallNonvirtualVoidMethod(self, base, env->GetMethodID(base, "onCreate", "(Landroid/os/Bundle;)V"), saved);
    if (exception(env, "create native activity")) return;
    // Started by a notification pressed: what it asks.
    take_request(env, env->CallObjectMethod(self, env->GetMethodID(base, "getIntent", "()Landroid/content/Intent;")));
    exception(env, "read starting intent");
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
            // The params the parent gave the view, resized -- never new plain
            // ViewGroup.LayoutParams: the content FrameLayout casts its
            // children's to MarginLayoutParams as it measures them
            // (measureChildWithMargins), and threw ClassCastException.
            jclass params = env->FindClass("android/view/ViewGroup$LayoutParams");
            jobject layout = env->CallObjectMethod(input_view, env->GetMethodID(view, "getLayoutParams", "()Landroid/view/ViewGroup$LayoutParams;"));
            if (layout) {
                env->SetIntField(layout, env->GetFieldID(params, "width", "I"), SDL_max(1, request.rect.w));
                env->SetIntField(layout, env->GetFieldID(params, "height", "I"), SDL_max(1, request.rect.h));
                env->CallVoidMethod(input_view, env->GetMethodID(view, "setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V"), layout);
            }
            env->CallVoidMethod(input_view, env->GetMethodID(view, "setX", "(F)V"), float(request.rect.x));
            env->CallVoidMethod(input_view, env->GetMethodID(view, "setY", "(F)V"), float(request.rect.y));
            env->CallBooleanMethod(input_view, env->GetMethodID(view, "requestFocus", "()Z"));
            jobject manager = service(env, "input_method");
            jclass cls = env->GetObjectClass(manager);
            env->CallVoidMethod(manager, env->GetMethodID(cls, "restartInput", "(Landroid/view/View;)V"), input_view);
            if (env->CallBooleanMethod(manager, env->GetMethodID(cls, "showSoftInput", "(Landroid/view/View;I)Z"), input_view, 0))
                tell(said::keyboard_shown{});
            break;
        }
        case Action::hide_keyboard: {
            jobject manager = service(env, "input_method");
            jobject token = env->CallObjectMethod(input_view, env->GetMethodID(view, "getWindowToken", "()Landroid/os/IBinder;"));
            env->CallBooleanMethod(manager, env->GetMethodID(env->GetObjectClass(manager), "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z"), token, 0);
            env->CallVoidMethod(input_view, env->GetMethodID(view, "clearFocus", "()V"));
            // No field typed into: the next says what it is, else nothing.
            state_mode = false;
            tell(said::keyboard_hidden{});
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
        case Action::ping:
            ui_answered = request.stamp;
            break;
        case Action::editor_state: {
            field_text = utf16_of(request.text);
            field_anchor = units_at(request.text, size_t(request.anchor));
            field_caret = units_at(request.text, size_t(request.caret));
            state_mode = true;
            if (!connection || !input_view) break;
            // What the input method made since the app last heard of it --
            // not taken by the app yet, or typed after the state it took --
            // is newer than what the app says now: the app's state was
            // made before it. Seeded over it, what was typed in between was
            // gone from the connection, and the next state the input method
            // made, from what was left, took it out of the app's field too:
            // words, and whole lines, went from what was being written (a
            // user's). Left, the app takes the input method's first: a
            // change of its own made in between gives way to what was
            // typed, never the other way round.
            {
                bool pending;
                {
                    const std::lock_guard<std::mutex> lock(edited_mutex);
                    pending = edited_state.has_value();
                }
                jobject content = editable(env, connection);
                if (pending || !content || characters(env, content) != committed) break;
            }
            seed(env, connection);
            // The input method told where the selection is now, and that
            // nothing is being composed.
            jobject manager = service(env, "input_method");
            if (manager)
                env->CallVoidMethod(manager, env->GetMethodID(env->GetObjectClass(manager), "updateSelection", "(Landroid/view/View;IIII)V"),
                                    input_view, std::min(field_anchor, field_caret), std::max(field_anchor, field_caret), -1, -1);
            break;
        }
        }
        exception(env, "UI request");
    }
}

static void result(JNIEnv *env, jobject, jint request, jint answer, jobject intent)
{
    document_result(env, request, answer, intent);
}

// What an intent that brought the activity up asks of the app -- a
// notification's: its chat -- kept for the app to take
// (SDL_TakeAndroidRequest), and taken off the intent, so that the activity
// made again does not ask it twice.
static std::mutex requested_mutex;
static std::deque<std::string> requested;
static constexpr const char *kRequestExtra = "org.libsdl.app.request";
static void take_request(JNIEnv *env, jobject intent)
{
    if (!intent) return;
    jclass cls = env->GetObjectClass(intent);
    jstring key = env->NewStringUTF(kRequestExtra);
    auto said = static_cast<jstring>(env->CallObjectMethod(intent,
        env->GetMethodID(cls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;"), key));
    if (exception(env, "read intent") || !said) return;
    std::string text = utf8(env, said);
    env->CallVoidMethod(intent, env->GetMethodID(cls, "removeExtra", "(Ljava/lang/String;)V"), key);
    exception(env, "consume intent");
    // A reply typed in a notification (RemoteInput, org.libsdl.app.reply):
    // its text after the request, a line on.
    jclass remote = env->FindClass("android/app/RemoteInput");
    if (remote && !exception(env, "find RemoteInput")) {
        jobject results = env->CallStaticObjectMethod(remote, env->GetStaticMethodID(remote, "getResultsFromIntent",
            "(Landroid/content/Intent;)Landroid/os/Bundle;"), intent);
        if (results && !exception(env, "read reply")) {
            jobject reply = env->CallObjectMethod(results, env->GetMethodID(env->GetObjectClass(results), "getCharSequence",
                "(Ljava/lang/String;)Ljava/lang/CharSequence;"), env->NewStringUTF("org.libsdl.app.reply"));
            if (reply && !exception(env, "read reply text")) {
                auto typed = static_cast<jstring>(env->CallObjectMethod(reply,
                    env->GetMethodID(env->GetObjectClass(reply), "toString", "()Ljava/lang/String;")));
                if (typed && !exception(env, "reply as text")) text += "\n" + utf8(env, typed);
            }
        }
    }
    const std::lock_guard<std::mutex> lock(requested_mutex);
    requested.push_back(std::move(text));
}
// A broadcast from a notification: its request kept, and the app woken
// for it -- an empty composition, which nothing takes for anything.
static void receive(JNIEnv *env, jobject, jobject, jobject intent)
{
    Frame frame(env);
    if (!frame) return;
    take_request(env, intent);
    tell(said::editing{"", 0});
}
// What UnifiedPush's distributor tells the app, kept for it to take
// (SDL_WaitAndroidPush) as "<what>\n<token>\n<value>": endpoint (its URL),
// message (its id, to acknowledge), unregistered, failed (the reason). Only what the connector's
// specification names; any other action dropped.
static std::mutex pushed_mutex;
static std::condition_variable pushed_ready;
static std::deque<std::string> pushed;
static bool push_woken = false;
static std::string extra_string(JNIEnv *env, jobject intent, const char *name)
{
    auto said = static_cast<jstring>(env->CallObjectMethod(intent, env->GetMethodID(env->GetObjectClass(intent), "getStringExtra",
        "(Ljava/lang/String;)Ljava/lang/String;"), env->NewStringUTF(name)));
    if (exception(env, "read push extra") || !said) return {};
    return utf8(env, said);
}
static void push_receive(JNIEnv *env, jobject, jobject, jobject intent)
{
    Frame frame(env);
    if (!frame || !intent) return;
    auto action = static_cast<jstring>(env->CallObjectMethod(intent, env->GetMethodID(env->GetObjectClass(intent), "getAction",
        "()Ljava/lang/String;")));
    if (exception(env, "read push action") || !action) return;
    const std::string named = utf8(env, action);
    struct kind { const char *action, *what, *extra; };
    static constexpr kind kinds[] = {
        {"org.unifiedpush.android.connector.NEW_ENDPOINT", "endpoint", "endpoint"},
        {"org.unifiedpush.android.connector.MESSAGE", "message", "id"},
        {"org.unifiedpush.android.connector.UNREGISTERED", "unregistered", nullptr},
        {"org.unifiedpush.android.connector.REGISTRATION_FAILED", "failed", "reason"},
    };
    const auto found = std::ranges::find(kinds, std::string_view(named), &kind::action);
    if (found == std::ranges::end(kinds)) return;
    std::string text = std::string(found->what) + "\n" + extra_string(env, intent, "token") + "\n" +
                       (found->extra ? extra_string(env, intent, found->extra) : std::string());
    {
        const std::lock_guard<std::mutex> lock(pushed_mutex);
        pushed.push_back(std::move(text));
    }
    pushed_ready.notify_all();
}
static void new_intent(JNIEnv *env, jobject self, jobject intent)
{
    jclass base = env->FindClass("android/app/NativeActivity");
    env->CallNonvirtualVoidMethod(self, base, env->GetMethodID(base, "onNewIntent", "(Landroid/content/Intent;)V"), intent);
    env->CallVoidMethod(self, env->GetMethodID(base, "setIntent", "(Landroid/content/Intent;)V"), intent);
    if (exception(env, "new intent")) return;
    take_request(env, intent);
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
        NATIVE("onActivityResult", "(IILandroid/content/Intent;)V", result),
        NATIVE("onNewIntent", "(Landroid/content/Intent;)V", new_intent)
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
        NATIVE("sendKeyEvent", "(Landroid/view/KeyEvent;)Z", key),
        NATIVE("setSelection", "(II)Z", select_text)
    };
    const JNINativeMethod runner_methods[] = {NATIVE("run", "()V", run)};
    const JNINativeMethod receiver_methods[] = {
        NATIVE("onReceive", "(Landroid/content/Context;Landroid/content/Intent;)V", receive)
    };
    const JNINativeMethod push_receiver_methods[] = {
        NATIVE("onReceive", "(Landroid/content/Context;Landroid/content/Intent;)V", push_receive)
    };
#undef NATIVE
    return load("org/libsdl/nativeapp/NativeActivity", activity_class, activity_methods, SDL_arraysize(activity_methods)) &&
           load("org/libsdl/nativeapp/InputView", view_class, view_methods, SDL_arraysize(view_methods)) &&
           load("org/libsdl/nativeapp/InputConnection", connection_class, connection_methods, SDL_arraysize(connection_methods)) &&
           load("org/libsdl/nativeapp/Runner", runner_class, runner_methods, SDL_arraysize(runner_methods)) &&
           load("org/libsdl/nativeapp/Receiver", receiver_class, receiver_methods, SDL_arraysize(receiver_methods)) &&
           load("org/libsdl/nativeapp/PushReceiver", push_receiver_class, push_receiver_methods, SDL_arraysize(push_receiver_methods));
}

void release_bridge(JNIEnv *env)
{
    const std::lock_guard<std::mutex> lock(requests_mutex);
    requests.clear();
    {
        const std::lock_guard<std::mutex> said_lock(said_mutex);
        said_queue.clear();
    }
    for (jobject *ref : {&input_view, &connection, &runner}) {
        if (*ref) env->DeleteGlobalRef(*ref);
        *ref = nullptr;
    }
    committed.clear();
}

std::atomic<Uint64> ui_answered{0};
void ping_ui(Uint64 stamp) { enqueue({Action::ping, 0, {}, {}, stamp}); }
void show_keyboard(int type, const SDL_Rect &rect) { enqueue({Action::show_keyboard, type, rect, {}}); }
void hide_keyboard() { enqueue({Action::hide_keyboard}); }
bool request_document(int type, const char *name) { return enqueue({Action::document, type, {}, name ? name : ""}); }
} // namespace native_activity

void Android_JNI_SetActivityTitle(const char *title) { native_activity::enqueue({native_activity::Action::title, 0, {}, title}); }
void Android_JNI_MinimizeWindow() { native_activity::enqueue({native_activity::Action::minimize}); }
bool Android_JNI_OpenURL(const char *url) { return native_activity::enqueue({native_activity::Action::open_url, 0, {}, url}); }

// What UnifiedPush's distributor said, the oldest first, waited for: its
// length, and copied into `out` (cut to `size`, ended with a NUL); -1 where
// SDL_WakeAndroidPush woke the wait with nothing come.
extern "C" int SDL_WaitAndroidPush(char *out, int size)
{
    std::unique_lock<std::mutex> lock(native_activity::pushed_mutex);
    native_activity::pushed_ready.wait(lock, [] { return native_activity::push_woken || !native_activity::pushed.empty(); });
    if (native_activity::pushed.empty()) {
        native_activity::push_woken = false;
        return -1;
    }
    const std::string text = std::move(native_activity::pushed.front());
    native_activity::pushed.pop_front();
    if (out && size > 0) {
        const auto kept = std::min<std::size_t>(text.size(), static_cast<std::size_t>(size - 1));
        std::copy_n(text.data(), kept, out);
        out[kept] = '\0';
    }
    return static_cast<int>(text.size());
}
// The wait above ended, from another thread: the app stopping its push.
extern "C" void SDL_WakeAndroidPush(void)
{
    {
        const std::lock_guard<std::mutex> lock(native_activity::pushed_mutex);
        native_activity::push_woken = true;
    }
    native_activity::pushed_ready.notify_all();
}
// What an intent asked of the app, the oldest first: its length, and copied
// into `out` (cut to `size`, ended with a NUL); 0 where nothing was asked.
extern "C" int SDL_TakeAndroidRequest(char *out, int size)
{
    const std::lock_guard<std::mutex> lock(native_activity::requested_mutex);
    if (native_activity::requested.empty() || !out || size <= 0) return 0;
    const std::string said = std::move(native_activity::requested.front());
    native_activity::requested.pop_front();
    const size_t copied = std::min(said.size(), size_t(size - 1));
    std::copy_n(said.data(), copied, out);
    out[copied] = '\0';
    return int(copied) + 1;
}

// The field typed into, as the app says it is: its text, and its selection
// in bytes of it.
extern "C" void SDL_SetAndroidEditorState(const char *utf8, int anchor, int caret)
{
    native_activity::enqueue({native_activity::Action::editor_state, 0, {}, utf8 ? utf8 : "", 0, anchor, caret});
}
// What the input method made of it, whole: copied, taken, its selection in
// bytes, and its length with its NUL; where `out` is too small or none,
// that length negated and nothing taken; 0 where it made nothing new.
extern "C" int SDL_TakeAndroidEditorState(char *out, int size, int *anchor, int *caret)
{
    const std::lock_guard<std::mutex> lock(native_activity::edited_mutex);
    if (!native_activity::edited_state) return 0;
    const auto &[text, from, to] = *native_activity::edited_state;
    const int needed = int(text.size()) + 1;
    if (!out || size < needed) return -needed;
    std::copy_n(text.data(), text.size(), out);
    out[text.size()] = '\0';
    if (anchor) *anchor = from;
    if (caret) *caret = to;
    native_activity::edited_state.reset();
    return needed;
}
