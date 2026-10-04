// SPDX-License-Identifier: AGPL-3.0-only
// Four callback classes, emitted directly. No Java compiler, Android API
// stubs, D8, Gradle, or prebuilt classes.dex participates in this build.
import std;
import dex.file;

namespace {
constexpr std::uint32_t public_ = 0x1, protected_ = 0x4, static_ = 0x8;
constexpr std::uint32_t final_ = 0x10, native_ = 0x100, constructor_ = 0x10000;
const std::string prefix = "Lorg/libsdl/nativeapp/";

std::vector<dex::Class> describe(dex::Pool &pool) {
  std::vector<dex::Class> classes;
  const auto constructor = [&](dex::Class &type, const std::string &base,
                               std::vector<std::string> parameters) {
    const auto registers = static_cast<std::uint16_t>(parameters.size() + 1);
    auto code = std::make_shared<dex::Code>(pool, registers, registers, registers);
    const dex::Method method{base, "<init>", "V", parameters};
    if (parameters.empty()) code->callDirect({0}, method);
    else if (parameters.size() == 1) code->callDirect({0, 1}, method);
    else code->callDirect({0, 1, 2}, method);
    code->returnVoid();
    type.method("<init>", "V", std::move(parameters), public_ | constructor_, code, true);
  };
  const auto method = [](dex::Class &type, std::string name, std::string result,
                         std::vector<std::string> parameters, std::uint32_t access = public_) {
    type.method(std::move(name), std::move(result), std::move(parameters), access | native_, nullptr, false);
  };

  dex::Class activity(pool, prefix + "NativeActivity;", "Landroid/app/NativeActivity;", {}, public_ | final_);
  constructor(activity, "Landroid/app/NativeActivity;", {});
  auto init = std::make_shared<dex::Code>(pool, 1, 0, 1);
  init->constantString(0, "SDL3");
  init->callStatic({0}, {"Ljava/lang/System;", "loadLibrary", "V", {"Ljava/lang/String;"}});
  init->returnVoid();
  activity.method("<clinit>", "V", {}, static_ | constructor_, init, true);
  method(activity, "onCreate", "V", {"Landroid/os/Bundle;"}, protected_);
  method(activity, "onActivityResult", "V", {"I", "I", "Landroid/content/Intent;"}, protected_);
  classes.push_back(std::move(activity));

  dex::Class view(pool, prefix + "InputView;", "Landroid/view/View;", {}, public_ | final_);
  constructor(view, "Landroid/view/View;", {"Landroid/content/Context;"});
  method(view, "onCheckIsTextEditor", "Z", {});
  method(view, "onCreateInputConnection", "Landroid/view/inputmethod/InputConnection;", {"Landroid/view/inputmethod/EditorInfo;"});
  method(view, "onKeyDown", "Z", {"I", "Landroid/view/KeyEvent;"});
  method(view, "onKeyUp", "Z", {"I", "Landroid/view/KeyEvent;"});
  method(view, "onKeyPreIme", "Z", {"I", "Landroid/view/KeyEvent;"});
  method(view, "onApplyWindowInsets", "Landroid/view/WindowInsets;", {"Landroid/view/WindowInsets;"});
  classes.push_back(std::move(view));

  dex::Class connection(pool, prefix + "InputConnection;", "Landroid/view/inputmethod/BaseInputConnection;", {}, public_ | final_);
  constructor(connection, "Landroid/view/inputmethod/BaseInputConnection;", {"Landroid/view/View;", "Z"});
  method(connection, "commitText", "Z", {"Ljava/lang/CharSequence;", "I"});
  method(connection, "setComposingText", "Z", {"Ljava/lang/CharSequence;", "I"});
  method(connection, "finishComposingText", "Z", {});
  method(connection, "deleteSurroundingText", "Z", {"I", "I"});
  method(connection, "deleteSurroundingTextInCodePoints", "Z", {"I", "I"});
  method(connection, "sendKeyEvent", "Z", {"Landroid/view/KeyEvent;"});
  classes.push_back(std::move(connection));

  dex::Class runner(pool, prefix + "Runner;", "Ljava/lang/Object;", {"Ljava/lang/Runnable;"}, public_ | final_);
  constructor(runner, "Ljava/lang/Object;", {});
  method(runner, "run", "V", {});
  classes.push_back(std::move(runner));
  return classes;
}
}

int main(int argc, char **argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--out") {
    std::println(std::cerr, "usage: sdl-native-dex --out <classes.dex>");
    return 2;
  }
  dex::Pool pool;
  (void)describe(pool);
  pool.freeze();
  auto classes = describe(pool);
  const auto bytes = dex::write(pool, classes);
  std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    std::println(std::cerr, "cannot write {}", argv[2]);
    return 1;
  }
  std::println("{}: {} bytes, four native callback classes", argv[2], bytes.size());
}
