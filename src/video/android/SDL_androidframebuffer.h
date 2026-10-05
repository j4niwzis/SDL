/* SPDX-License-Identifier: Zlib
 * A window framebuffer without GL, for the j4niwzis fork: what the program
 * draws on the CPU, posted to the ANativeWindow through ANativeWindow_lock.
 * Not an upstream SDL backend.
 */
#include "SDL_internal.h"

#ifndef SDL_androidframebuffer_h_
#define SDL_androidframebuffer_h_

extern bool Android_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch);
extern bool Android_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects);
extern void Android_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window);

#endif // SDL_androidframebuffer_h_
