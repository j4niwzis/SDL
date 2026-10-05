/* SPDX-License-Identifier: Zlib
 * A window framebuffer without GL, for the j4niwzis fork: what the program
 * draws on the CPU, posted to the ANativeWindow through ANativeWindow_lock.
 * Where there is no usable GLES -- a device whose GPU has no driver, or
 * whose GLES is emulated on the CPU -- this is how a window is shown at all,
 * rather than SDL's framebuffer emulated on a GLES texture. Not an upstream
 * SDL backend.
 */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_ANDROID

#include "../SDL_sysvideo.h"
#include "../../core/android/SDL_android.h"
#include "SDL_androidframebuffer.h"
#include "SDL_androidwindow.h"

#include <android/native_window.h>

void Android_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *data = window->internal;
    if (data) {
        SDL_free(data->pixels);
        data->pixels = NULL;
        data->pixels_w = 0;
        data->pixels_h = 0;
    }
}

bool Android_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_WindowData *data = window->internal;
    int w = 0, h = 0;

    if (!data) {
        return SDL_SetError("Window has no native data");
    }
    Android_DestroyWindowFramebuffer(_this, window);
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return SDL_SetError("Window has no size");
    }

    Android_LockActivityMutex();
#ifdef SDL_VIDEO_OPENGL_EGL
    /* An ANativeWindow takes buffers from one producer at a time: a window
       the CPU draws into is no EGL surface, now or after the activity resumes
       (see APP_CMD_INIT_WINDOW in core/android/native/activity.cpp). */
    if (data->egl_surface != EGL_NO_SURFACE) {
        SDL_EGL_DestroySurface(_this, data->egl_surface);
        data->egl_surface = EGL_NO_SURFACE;
        SDL_SetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_SURFACE_POINTER, NULL);
    }
#endif
    data->framebuffer = true;
    if (data->native_window) {
        ANativeWindow_setBuffersGeometry(data->native_window, w, h, WINDOW_FORMAT_RGBX_8888);
    }
    Android_UnlockActivityMutex();

    data->pixels = SDL_calloc((size_t)w * (size_t)h, 4);
    if (!data->pixels) {
        return false;
    }
    data->pixels_w = w;
    data->pixels_h = h;

    // WINDOW_FORMAT_RGBX_8888 is R, G, B, X in memory: SDL's XBGR8888 on a
    // little-endian machine, which every Android ABI is.
    *format = SDL_PIXELFORMAT_XBGR8888;
    *pixels = data->pixels;
    *pitch = w * 4;
    return true;
}

bool Android_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_WindowData *data = window->internal;
    bool result = true;

    if (!data || !data->pixels) {
        return SDL_SetError("Window has no framebuffer");
    }

    Android_LockActivityMutex();
    // Paused, with no surface: nothing to show it on, and nothing lost --
    // the next frame after the window comes back is drawn whole.
    if (data->native_window) {
        ARect dirty;
        ANativeWindow_Buffer buffer;
        int i, y;

        // The union of what changed. The buffer locked keeps what was posted
        // before outside it, and the bounds may come back grown: a new
        // buffer, or one the system dropped, is to be filled whole.
        if (rects && numrects > 0) {
            SDL_Rect all = rects[0];
            for (i = 1; i < numrects; ++i) {
                SDL_GetRectUnion(&all, &rects[i], &all);
            }
            dirty.left = all.x;
            dirty.top = all.y;
            dirty.right = all.x + all.w;
            dirty.bottom = all.y + all.h;
        } else {
            dirty.left = 0;
            dirty.top = 0;
            dirty.right = data->pixels_w;
            dirty.bottom = data->pixels_h;
        }

        // Set again each time: a surface made anew after a resume has the
        // window's own geometry until it is told.
        ANativeWindow_setBuffersGeometry(data->native_window, data->pixels_w, data->pixels_h, WINDOW_FORMAT_RGBX_8888);
        if (ANativeWindow_lock(data->native_window, &buffer, &dirty) < 0) {
            result = SDL_SetError("ANativeWindow_lock failed");
        } else {
            const int right = SDL_min(SDL_min(dirty.right, buffer.width), data->pixels_w);
            const int bottom = SDL_min(SDL_min(dirty.bottom, buffer.height), data->pixels_h);
            const int left = SDL_max(dirty.left, 0);
            const int top = SDL_max(dirty.top, 0);
            if (right > left) {
                for (y = top; y < bottom; ++y) {
                    SDL_memcpy((Uint8 *)buffer.bits + ((size_t)y * buffer.stride + left) * 4,
                               (const Uint8 *)data->pixels + ((size_t)y * data->pixels_w + left) * 4,
                               (size_t)(right - left) * 4);
                }
            }
            ANativeWindow_unlockAndPost(data->native_window);
        }
    }
    Android_UnlockActivityMutex();
    return result;
}

#endif // SDL_VIDEO_DRIVER_ANDROID
