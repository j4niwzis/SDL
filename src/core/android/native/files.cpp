// SPDX-License-Identifier: Zlib
// NativeActivity backend for the j4niwzis fork.
#include "bridge.hpp"
#include <android/asset_manager.h>
#include <algorithm>
#include <climits>
#include <mutex>
#include <unistd.h>

using namespace native_activity;

namespace {
std::mutex path_mutex;
std::string internal_path, external_path, cache_path;
const char *path(std::string &cached, const char *method, bool external = false)
{
    const std::lock_guard<std::mutex> lock(path_mutex);
    if (!cached.empty()) return cached.c_str();
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return nullptr;
    jmethodID get = env->GetMethodID(env->GetObjectClass(activity), method,
        external ? "(Ljava/lang/String;)Ljava/io/File;" : "()Ljava/io/File;");
    jobject file = external ? env->CallObjectMethod(activity, get, nullptr) : env->CallObjectMethod(activity, get);
    if (exception(env, "storage directory") || !file) return nullptr;
    auto name = static_cast<jstring>(env->CallObjectMethod(file, env->GetMethodID(env->GetObjectClass(file), "getAbsolutePath", "()Ljava/lang/String;")));
    if (exception(env, "storage path")) return nullptr;
    cached = utf8(env, name);
    return cached.c_str();
}
}

const char *SDL_GetAndroidInternalStoragePath() { return path(internal_path, "getFilesDir"); }
const char *SDL_GetAndroidExternalStoragePath() { return path(external_path, "getExternalFilesDir", true); }
const char *SDL_GetAndroidCachePath() { return path(cache_path, "getCacheDir"); }
Uint32 SDL_GetAndroidExternalStorageState()
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame) return 0;
    jclass cls = env->FindClass("android/os/Environment");
    auto state = static_cast<jstring>(env->CallStaticObjectMethod(cls, env->GetStaticMethodID(cls, "getExternalStorageState", "()Ljava/lang/String;")));
    if (exception(env, "external storage state")) return 0;
    auto value = utf8(env, state);
    if (value == "mounted") return SDL_ANDROID_EXTERNAL_STORAGE_READ | SDL_ANDROID_EXTERNAL_STORAGE_WRITE;
    if (value == "mounted_ro") return SDL_ANDROID_EXTERNAL_STORAGE_READ;
    return 0;
}

int Android_JNI_OpenFileDescriptor(const char *uri, const char *mode)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return -1;
    jclass cls = env->FindClass("android/net/Uri");
    jobject parsed = env->CallStaticObjectMethod(cls, env->GetStaticMethodID(cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), string(env, uri));
    jobject resolver = env->CallObjectMethod(activity, env->GetMethodID(env->GetObjectClass(activity), "getContentResolver", "()Landroid/content/ContentResolver;"));
    // SDL passes fopen modes; ContentResolver requires Android's exact modes.
    const bool plus = SDL_strchr(mode, '+') != nullptr;
    const char *access = *mode == 'w' ? (plus ? "rwt" : "wt") : *mode == 'a' ? (plus ? "rw" : "wa") : plus ? "rw" : "r";
    jobject descriptor = env->CallObjectMethod(resolver, env->GetMethodID(env->GetObjectClass(resolver), "openFileDescriptor",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;"), parsed, string(env, access));
    if (exception(env, "open document") || !descriptor) return -1;
    int fd = env->CallIntMethod(descriptor, env->GetMethodID(env->GetObjectClass(descriptor), "detachFd", "()I"));
    if (exception(env, "detach document descriptor")) return -1;
    if (*mode == 'a' && plus && lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        SDL_SetError("Document does not support append");
        return -1;
    }
    return fd;
}

bool Android_JNI_FileOpen(void **userdata, const char *name, const char *mode)
{
    *userdata = nullptr;
    if (!app || !name || !mode || *mode != 'r' || SDL_strchr(mode, '+')) return SDL_SetError("Assets are read-only");
    auto *asset = AAssetManager_open(app->activity->assetManager, name, AASSET_MODE_RANDOM);
    if (!asset) return SDL_SetError("Asset not found: %s", name);
    *userdata = asset;
    return true;
}
Sint64 Android_JNI_FileSize(void *value) { return AAsset_getLength64(static_cast<AAsset *>(value)); }
Sint64 Android_JNI_FileSeek(void *value, Sint64 offset, SDL_IOWhence whence)
{
    const int origins[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    if (unsigned(whence) >= SDL_arraysize(origins)) { SDL_InvalidParamError("whence"); return -1; }
    return AAsset_seek64(static_cast<AAsset *>(value), offset, origins[whence]);
}
size_t Android_JNI_FileRead(void *value, void *buffer, size_t size, SDL_IOStatus *status)
{
    const int count = AAsset_read(static_cast<AAsset *>(value), buffer, SDL_min(size, size_t(INT_MAX)));
    if (count < 0) { *status = SDL_IO_STATUS_ERROR; SDL_SetError("Asset read failed"); return 0; }
    *status = count == 0 && size ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
    return size_t(count);
}
size_t Android_JNI_FileWrite(void *, const void *, size_t, SDL_IOStatus *status)
{
    *status = SDL_IO_STATUS_READONLY;
    SDL_SetError("Assets are read-only");
    return 0;
}
bool Android_JNI_FileClose(void *value) { AAsset_close(static_cast<AAsset *>(value)); return true; }

static jobjectArray assets(JNIEnv *env, const char *path)
{
    jobject manager = env->CallObjectMethod(activity, env->GetMethodID(env->GetObjectClass(activity), "getAssets", "()Landroid/content/res/AssetManager;"));
    return static_cast<jobjectArray>(env->CallObjectMethod(manager, env->GetMethodID(env->GetObjectClass(manager), "list", "(Ljava/lang/String;)[Ljava/lang/String;"), string(env, path)));
}
bool Android_JNI_EnumerateAssetDirectory(const char *path, SDL_EnumerateDirectoryCallback callback, void *userdata)
{
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame || !activity) return false;
    jobjectArray names = assets(env, path);
    if (exception(env, "list assets") || !names) return false;
    std::string dirname = path;
    if (!dirname.empty() && dirname.back() != '/') dirname += '/';
    for (jsize i = 0; i < env->GetArrayLength(names); ++i) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        const auto result = callback(userdata, dirname.c_str(), utf8(env, name).c_str());
        env->DeleteLocalRef(name);
        if (result == SDL_ENUM_FAILURE) return SDL_SetError("Asset enumeration callback failed");
        if (result == SDL_ENUM_SUCCESS) break;
    }
    return true;
}
bool Android_JNI_GetAssetPathInfo(const char *path, SDL_PathInfo *info)
{
    SDL_zero(*info);
    if (!app) return false;
    if (auto *asset = AAssetManager_open(app->activity->assetManager, path, AASSET_MODE_UNKNOWN)) {
        info->type = SDL_PATHTYPE_FILE;
        info->size = AAsset_getLength64(asset);
        AAsset_close(asset);
        return true;
    }
    auto *env = Android_JNI_GetEnv();
    Frame frame(env);
    if (!frame) return false;
    auto list = assets(env, path);
    if (exception(env, "asset info")) return false;
    if (!*path || (list && env->GetArrayLength(list))) { info->type = SDL_PATHTYPE_DIRECTORY; return true; }
    return SDL_SetError("Asset not found: %s", path);
}

namespace native_activity {
bool request_document(int, const char *);
namespace {
constexpr int document_request = 0x5344;
std::mutex dialog_mutex;
struct Dialog {
    SDL_DialogFileCallback callback = nullptr;
    void *userdata = nullptr;
    bool multiple = false;
    std::vector<std::string> patterns;
    bool ready = false;
    bool error = false;
    std::vector<std::string> paths;
} dialog;

void complete(std::vector<std::string> paths, bool error)
{
    const std::lock_guard<std::mutex> lock(dialog_mutex);
    if (!dialog.callback) return;
    dialog.paths = std::move(paths);
    dialog.error = error;
    dialog.ready = true;
    if (app && app->looper) ALooper_wake(app->looper);
}
}

// Application callbacks run only while its native event loop is alive.
// The UI thread records results; it never calls through application userdata.
void pump_documents()
{
    Dialog finished;
    {
        const std::lock_guard<std::mutex> lock(dialog_mutex);
        if (!dialog.ready) return;
        finished = std::move(dialog);
        dialog = {};
    }
    if (!finished.callback) return;
    std::vector<const char *> pointers;
    for (const auto &path : finished.paths) pointers.push_back(path.c_str());
    pointers.push_back(nullptr);
    finished.callback(finished.userdata, finished.error ? nullptr : pointers.data(), -1);
}
void clear_documents()
{
    const std::lock_guard<std::mutex> lock(dialog_mutex);
    dialog = {};
}

void open_document_on_ui(JNIEnv *env, int type, const std::string &name)
{
    Frame frame(env);
    if (!frame) { complete({}, true); return; }
    bool multiple;
    std::vector<std::string> patterns;
    {
        const std::lock_guard<std::mutex> lock(dialog_mutex);
        if (!dialog.callback) return;
        multiple = dialog.multiple;
        patterns = dialog.patterns;
    }
    jclass cls = env->FindClass("android/content/Intent");
    const char *action = type == SDL_FILEDIALOG_SAVEFILE ? "android.intent.action.CREATE_DOCUMENT" :
                         type == SDL_FILEDIALOG_OPENFOLDER ? "android.intent.action.OPEN_DOCUMENT_TREE" : "android.intent.action.OPEN_DOCUMENT";
    jobject intent = env->NewObject(cls, env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V"), string(env, action));
    if (type != SDL_FILEDIALOG_OPENFOLDER) {
        env->CallObjectMethod(intent, env->GetMethodID(cls, "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;"), string(env, "android.intent.category.OPENABLE"));
        env->CallObjectMethod(intent, env->GetMethodID(cls, "setType", "(Ljava/lang/String;)Landroid/content/Intent;"), string(env, "*/*"));
        // Provider MIME filtering is advisory. Unknown extensions keep */*.
        std::vector<std::string> mime;
        jclass map_class = env->FindClass("android/webkit/MimeTypeMap");
        jobject map = env->CallStaticObjectMethod(map_class, env->GetStaticMethodID(map_class, "getSingleton", "()Landroid/webkit/MimeTypeMap;"));
        bool wildcard = patterns.empty();
        for (const auto &pattern : patterns) {
            size_t start = 0;
            do {
                const size_t end = pattern.find(';', start);
                auto extension = pattern.substr(start, end - start);
                auto found = static_cast<jstring>(env->CallObjectMethod(map, env->GetMethodID(map_class, "getMimeTypeFromExtension", "(Ljava/lang/String;)Ljava/lang/String;"), string(env, extension.c_str())));
                if (found) { mime.push_back(utf8(env, found)); env->DeleteLocalRef(found); }
                else wildcard = true;
                if (end == std::string::npos) break;
                start = end + 1;
            } while (start < pattern.size());
        }
        if (!wildcard && !mime.empty()) {
            jobjectArray types = env->NewObjectArray(jsize(mime.size()), env->FindClass("java/lang/String"), nullptr);
            for (jsize i = 0; i < jsize(mime.size()); ++i) {
                jstring value = string(env, mime[i].c_str());
                env->SetObjectArrayElement(types, i, value);
                env->DeleteLocalRef(value);
            }
            env->CallObjectMethod(intent, env->GetMethodID(cls, "putExtra", "(Ljava/lang/String;[Ljava/lang/String;)Landroid/content/Intent;"), string(env, "android.intent.extra.MIME_TYPES"), types);
        }
        env->CallObjectMethod(intent, env->GetMethodID(cls, "putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;"), string(env, "android.intent.extra.ALLOW_MULTIPLE"), multiple && type == SDL_FILEDIALOG_OPENFILE);
        if (type == SDL_FILEDIALOG_SAVEFILE && !name.empty()) {
            auto basename = name.substr(name.find_last_of('/') == std::string::npos ? 0 : name.find_last_of('/') + 1);
            env->CallObjectMethod(intent, env->GetMethodID(cls, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"), string(env, "android.intent.extra.TITLE"), string(env, basename.c_str()));
        }
    }
    env->CallObjectMethod(intent, env->GetMethodID(cls, "addFlags", "(I)Landroid/content/Intent;"), 1 | 2 | 64);
    if (!exception(env, "create document intent")) {
        env->CallVoidMethod(activity, env->GetMethodID(env->GetObjectClass(activity), "startActivityForResult", "(Landroid/content/Intent;I)V"), intent, document_request);
        if (!exception(env, "open document picker")) return;
    }
    complete({}, true);
}

void document_result(JNIEnv *env, jint request, jint result, jobject data)
{
    if (request != document_request) return;
    if (result != -1 || !data) { complete({}, false); return; }
    Frame frame(env);
    if (!frame) { complete({}, true); return; }
    jclass cls = env->GetObjectClass(data);
    jobject clip = env->CallObjectMethod(data, env->GetMethodID(cls, "getClipData", "()Landroid/content/ClipData;"));
    std::vector<std::string> paths;
    const auto add = [&](jobject uri) {
        if (!uri) return;
        auto text = static_cast<jstring>(env->CallObjectMethod(uri, env->GetMethodID(env->GetObjectClass(uri), "toString", "()Ljava/lang/String;")));
        paths.push_back(utf8(env, text));
        env->DeleteLocalRef(text);
        env->DeleteLocalRef(uri);
    };
    if (clip) {
        jclass clip_class = env->GetObjectClass(clip);
        const jint count = env->CallIntMethod(clip, env->GetMethodID(clip_class, "getItemCount", "()I"));
        for (jint i = 0; i < count; ++i) {
            jobject item = env->CallObjectMethod(clip, env->GetMethodID(clip_class, "getItemAt", "(I)Landroid/content/ClipData$Item;"), i);
            add(env->CallObjectMethod(item, env->GetMethodID(env->GetObjectClass(item), "getUri", "()Landroid/net/Uri;")));
            env->DeleteLocalRef(item);
        }
    } else {
        add(env->CallObjectMethod(data, env->GetMethodID(cls, "getData", "()Landroid/net/Uri;")));
    }
    complete(std::move(paths), exception(env, "document result"));
}
}

bool Android_JNI_ShowFileDialog(SDL_DialogFileCallback callback, void *userdata, const SDL_DialogFileFilter *filters,
                               int count, SDL_FileDialogType type, bool multiple, const char *initial)
{
    {
        const std::lock_guard<std::mutex> lock(dialog_mutex);
        if (dialog.callback) return SDL_SetError("A document picker is already open");
        dialog.callback = callback;
        dialog.userdata = userdata;
        dialog.multiple = multiple;
        for (int i = 0; i < count; ++i) dialog.patterns.emplace_back(filters[i].pattern);
    }
    if (request_document(int(type), initial)) return true;
    const std::lock_guard<std::mutex> lock(dialog_mutex);
    dialog = {};
    return false;
}
