#ifndef _SDL_WINDOW_SHIM_H_
#define _SDL_WINDOW_SHIM_H_

#include <stdint.h>

/*
 * Renderer-neutral SDL window glue.
 *
 * machismo installs sdl_create_window_wrapper / sdl_set_window_fullscreen_wrapper
 * as trampoline overrides for ANY game whose SDL2 is trampolined (the `_SDL_`
 * prefix; see machismo.c). That's every SDL port regardless of renderer:
 * NecroDancer (bgfx), Shotgun King / Wratch's Den / Scavenger (Sugar), and
 * Mina (the Gothic GLES backend). None of this is bgfx-specific — it used to
 * live in bgfx_shim.c only by accident of history.
 *
 * The window captured here is reached by the per-game renderer shim (which is a
 * separate .so) via dlsym(RTLD_DEFAULT, "sdl_window_get_captured"); the loader
 * is built -rdynamic so these symbols are visible.
 */

/* SDL_SysWMinfo subsystem enum (SDL2 header order). */
#define SDL_SYSWM_X11      2
#define SDL_SYSWM_WAYLAND  6
#define SDL_SYSWM_KMSDRM  13

/*
 * Minimal SDL_SysWMinfo: version(3)+pad, subsystem, then the union. For X11 the
 * union is {Display* display; Window window; ...}; for Wayland {wl_display*
 * display; wl_surface* surface; ...}; for KMSDRM {int dev_index; int drm_fd;
 * gbm_device*; gbm_surface*; ...}. display/first handle at offset 8, second at
 * offset 16 — readers that need the KMSDRM gbm pointers index past x11_display.
 */
typedef struct {
	uint8_t       ver_major, ver_minor, ver_patch, pad;
	int32_t       subsystem;
	void         *x11_display;   /* offset 8  (also wl_display / gbm base)        */
	unsigned long x11_window;    /* offset 16 (X11 Window XID / wl_surface*)      */
	char          rest[256];
} sdl_wminfo_t;

/* Trampoline override targets (installed by machismo for every `_SDL_` prefix).
 *
 * sdl_create_window_wrapper strips SDL_WINDOW_METAL (Apple-Silicon Mac builds
 * request a Metal window; native Linux SDL has no Metal backend, so the call
 * would return NULL and strand any engine waiting on its window handle), forces
 * fullscreen on KMSDRM, and captures the resulting SDL_Window*. */
void* sdl_create_window_wrapper(const char* title, int x, int y, int w, int h, unsigned int flags);

/* Tell the window shim which GPU flag the next created window needs (1 = Vulkan →
 * SDL_WINDOW_VULKAN, 0 = GLES → SDL_WINDOW_OPENGL on KMSDRM). The loader sets this
 * from the render override lib's backend decision before the window is created, so
 * the flag matches the backend that will actually be installed. */
void sdl_window_set_vulkan(int is_vulkan);

/* Drop SDL's DRM master on the KMSDRM display so a direct-display Vulkan driver
 * (libmali VK_KHR_display) can acquire it. Returns the ioctl rc (0 = ok), or -1
 * off KMSDRM. See the .c for why this is required. */
int sdl_window_kmsdrm_drop_master(void* window);

/* Blocks fullscreen transitions (macOS changes the display mode to the game's
 * resolution; Linux/Wayland can't, so fullscreen yields a native-res surface
 * while the game's viewports stay at its internal resolution — broken). */
int sdl_set_window_fullscreen_wrapper(void* window, unsigned int flags);

/* The SDL_Window* captured by sdl_create_window_wrapper (NULL until the game
 * creates its window). Lets a renderer shim reach the window without guessing
 * its SDL window-ID — SDL2-compat/SDL3 do not number the first window 1. */
void* sdl_window_get_captured(void);

/* Fallback window lookup by SDL window-ID (legacy; prefer the captured window). */
void* sdl_window_from_id(uint32_t id);

/* Physical framebuffer size (SDL_GL_GetDrawableSize), falling back to the
 * logical window size; and the logical window size directly. */
void sdl_window_get_drawable_size(void* window, int* w, int* h);
void sdl_window_get_size(void* window, int* w, int* h);

/* Fill *out with the window's native handle info: sets the compiled SDL version
 * then calls SDL_GetWindowWMInfo. Returns 1 on success, 0 if unavailable. */
int sdl_window_fill_wminfo(void* window, sdl_wminfo_t* out);

/* True if the environment indicates a KMSDRM (no X11/Wayland) target. */
int sdl_window_is_kmsdrm_env(void);

#endif /* _SDL_WINDOW_SHIM_H_ */
