# NativeActivity profile of the j4niwzis SDL fork

This experimental profile replaces SDLActivity with Android's NativeActivity
and four small generated callback classes. It builds `classes.dex` directly
from C++, without Java source, `android.jar`, D8, R8, Gradle, or a prebuilt DEX.
The regular SDLActivity backend remains the default.

The native backend covers a single window, EGL/GLES, touch, keyboard and IME
composition, clipboard, document selection and `content://` streams, storage,
locale, audio, and lifecycle events. Framework work runs on Android's UI
thread; application code and document callbacks run on the native event loop.
UTF-16 text is converted to real UTF-8, including supplementary characters.

The initial profile disables controllers, haptics and cameras. Runtime
permission prompts, custom cursors, relative mouse mode and native message
boxes are not implemented. Orientation follows the manifest. This is not a
drop-in replacement for every SDLActivity integration. IME behavior, surface
recreation and document providers still require testing on physical devices.

## Build

Build the host DEX generator with CMake 4.3.4 or later, Ninja and a compiler
supporting C++23 modules and `import std`:

```sh
cmake -S build-scripts/native-activity/dex -B build-dex -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-23 \
  -DCMAKE_CXX_FLAGS='-stdlib=libc++ -fno-experimental-new-constant-interpreter'
cmake --build build-dex
build-dex/sdl-native-dex --out build-dex/classes.dex
python3 build-scripts/native-activity/dex/verify.py build-dex/classes.dex
```

The verifier checks the DEX checksums, empty table offsets, and every native
method signature against its JNI registration. AOSP's optional `dexdump -c`
and `dexdump -d` provide independent format verification and disassembly.

Configure SDL with an Android toolchain and AOSP's native_app_glue sources:

```sh
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/android-minimal/cmake/target.cmake \
  -DSDL_ANDROID_NATIVE_ACTIVITY=ON \
  -DSDL_ANDROID_NATIVE_APP_GLUE=/path/to/android-minimal/src/ndk/sources/android/native_app_glue \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_VULKAN=OFF \
  -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_INSTALL=OFF
cmake --build build-android
```

The app exports `ANativeActivity_onCreate` and forwards its arguments, plus
its native main function, to `SDL_AndroidNativeActivity` from
`SDL3/SDL_nativeactivity.h`. Its manifest uses
`org.libsdl.nativeapp.NativeActivity`; `android.app.lib_name` identifies the
application library. Package the generated DEX, `libSDL3.so`, the application
library and its runtime dependencies. The application entry point must be
exported from its shared library. `main` runs on the native thread after the
first window is available. Callbacks stop when it returns.

The validated build uses ARM64, API 27, Clang 23 and source-built libc++ 22.
Both native libraries use 16 KB ELF load alignment. Runtime testing should
cover a second launch in the same process, pause/resume, rotation, keyboard
dismissal, non-Latin composition, emoji replacement/deletion, clipboard focus,
and local and cloud document providers. A successful compile does not verify
those behaviors.

## Licensing and origin

The native SDL backend is under the Zlib license. This fork's additions are
identified in their source headers; they are not upstream SDL APIs.

The independent host generator in `dex/` is AGPL-3.0-only. Its DEX writer and
import-std setup derive from `j4niwzis/osu-cpp` at
`b4f1bff76aa9bf6ddb702f0357c5a0978e178bfc`; its license is in `dex/COPYING`.
The generator's implementation is not linked into SDL. Do not describe the
whole source tree, including that tool, as exclusively Zlib-licensed.
