/* SPDX-License-Identifier: Zlib
 * NativeActivity extension in the j4niwzis fork; not an upstream SDL API.
 */
#ifndef SDL_nativeactivity_h_
#define SDL_nativeactivity_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_begin_code.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeActivity;
typedef int (SDLCALL *SDL_NativeActivityMain)(int argc, char **argv);

/* Call from the application's ANativeActivity_onCreate entry point.
 * The main function runs on the native app-glue thread, after a window exists.
 * Use org.libsdl.nativeapp.NativeActivity from the generated classes.dex.
 */
extern SDL_DECLSPEC void SDLCALL SDL_AndroidNativeActivity(
    struct ANativeActivity *activity, void *saved_state, size_t saved_size,
    SDL_NativeActivityMain main_function);

#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>
#endif
