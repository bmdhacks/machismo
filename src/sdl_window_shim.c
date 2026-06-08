/*
 * sdl_window_shim.c — renderer-neutral SDL window glue.
 *
 * Window creation/capture + native-handle plumbing shared by every SDL-based
 * machismo port (bgfx, Sugar, Gothic). See sdl_window_shim.h for the rationale;
 * this code was previously embedded in bgfx_shim.c and is otherwise unchanged.
 */
#include "sdl_window_shim.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>

/* SDL window flag bits (SDL2 values; identical under SDL3/SDL2-compat). */
#define SDL_WINDOW_FULLSCREEN         0x00000001
#define SDL_WINDOW_OPENGL             0x00000002
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_WINDOW_ALLOW_HIGHDPI      0x00002000
#define SDL_WINDOW_METAL              0x20000000

/* SDL_GLattr enum values (SDL2 ABI; stable). Used to request a GLES3 config
 * before creating a KMSDRM GL window so SDL builds a GBM-for-GL surface that
 * SDL_GL_CreateContext can bind. */
#define SDL_GL_DOUBLEBUFFER           5
#define SDL_GL_DEPTH_SIZE             6
#define SDL_GL_STENCIL_SIZE           7
#define SDL_GL_CONTEXT_MAJOR_VERSION  17
#define SDL_GL_CONTEXT_MINOR_VERSION  18
#define SDL_GL_CONTEXT_PROFILE_MASK   21
#define SDL_GL_CONTEXT_PROFILE_ES     0x0004

typedef struct {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
} SDL_version_t;

/* SDL2 function pointers — resolved at runtime via dlsym (SDL2 is already
 * loaded by the trampoline). */
static void* (*sdl_GetWindowFromID)(uint32_t id) = NULL;
static int   (*sdl_GetWindowWMInfo)(void*, void* info) = NULL;
static void  (*sdl_GetVersion)(SDL_version_t* ver) = NULL;
static void  (*sdl_GetWindowSize)(void*, int*, int*) = NULL;
static void  (*sdl_GL_GetDrawableSize)(void*, int*, int*) = NULL;

/* The SDL window captured by sdl_create_window_wrapper. */
static void* captured_sdl_window = NULL;

static void resolve_sdl_funcs(void)
{
	if (sdl_GetWindowFromID) return;

	sdl_GetWindowFromID    = dlsym(RTLD_DEFAULT, "SDL_GetWindowFromID");
	sdl_GetWindowWMInfo    = dlsym(RTLD_DEFAULT, "SDL_GetWindowWMInfo");
	sdl_GetVersion         = dlsym(RTLD_DEFAULT, "SDL_GetVersion");
	sdl_GetWindowSize      = dlsym(RTLD_DEFAULT, "SDL_GetWindowSize");
	sdl_GL_GetDrawableSize = dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");

	if (!sdl_GetWindowFromID || !sdl_GetWindowWMInfo) {
		fprintf(stderr, "sdl_window_shim: warning: SDL2 functions not found, "
		        "cannot get native window handle\n");
	}
}

void* sdl_window_get_captured(void)
{
	return captured_sdl_window;
}

void* sdl_window_from_id(uint32_t id)
{
	resolve_sdl_funcs();
	return sdl_GetWindowFromID ? sdl_GetWindowFromID(id) : NULL;
}

void sdl_window_get_size(void* window, int* w, int* h)
{
	*w = 0; *h = 0;
	resolve_sdl_funcs();
	if (sdl_GetWindowSize)
		sdl_GetWindowSize(window, w, h);
}

void sdl_window_get_drawable_size(void* window, int* w, int* h)
{
	*w = 0; *h = 0;
	resolve_sdl_funcs();
	if (sdl_GL_GetDrawableSize)
		sdl_GL_GetDrawableSize(window, w, h);
	if (*w <= 0 || *h <= 0)
		sdl_window_get_size(window, w, h);
}

int sdl_window_fill_wminfo(void* window, sdl_wminfo_t* out)
{
	resolve_sdl_funcs();
	if (!sdl_GetWindowWMInfo || !sdl_GetVersion || !window)
		return 0;
	memset(out, 0, sizeof(*out));
	sdl_GetVersion((SDL_version_t*)out);   /* version gate for WMInfo */
	return sdl_GetWindowWMInfo(window, out) ? 1 : 0;
}

int sdl_window_is_kmsdrm_env(void)
{
	const char *video_drv = getenv("SDL_VIDEODRIVER");
	return (video_drv && strcmp(video_drv, "KMSDRM") == 0)
	    || (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"));
}

/* Request a GLES3 config so SDL's KMSDRM backend builds a GBM-for-GL surface.
 * Must be called before SDL_CreateWindow. Renderers that drive their own EGL
 * from a native handle (Wayland/X11) never reach this. */
static void request_gles3_window_config(void)
{
	typedef int (*setattr_fn)(int attr, int value);
	setattr_fn set_attr = (setattr_fn)dlsym(RTLD_DEFAULT, "SDL_GL_SetAttribute");
	if (!set_attr) {
		fprintf(stderr, "sdl_window_shim: SDL_GL_SetAttribute not found; "
		        "KMSDRM GL window may use defaults\n");
		return;
	}
	set_attr(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	set_attr(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	set_attr(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	set_attr(SDL_GL_DEPTH_SIZE, 24);
	set_attr(SDL_GL_STENCIL_SIZE, 8);
	set_attr(SDL_GL_DOUBLEBUFFER, 1);
}

void* sdl_create_window_wrapper(const char* title, int x, int y, int w, int h, unsigned int flags)
{
	typedef void* (*sdl_create_window_fn)(const char*, int, int, int, int, unsigned int);
	static sdl_create_window_fn real_fn = NULL;
	if (!real_fn)
		real_fn = dlsym(RTLD_DEFAULT, "SDL_CreateWindow");
	if (!real_fn) {
		fprintf(stderr, "sdl_window_shim: SDL_CreateWindow not found\n");
		return NULL;
	}

	/* Apple-Silicon Mac builds create a Metal window (SDL_WINDOW_METAL); native
	 * Linux SDL has no Metal backend, so SDL_CreateWindow with that flag fails
	 * and returns NULL — which silently strands any engine that waits on its
	 * window handle (Mina's Gothic engine deadlocks here). We replace the Metal
	 * renderer with EGL/GLES built from the window's native handle, so the flag
	 * is never wanted on Linux: strip it unconditionally. */
	int stripped_metal = 0;
	if (flags & SDL_WINDOW_METAL) {
		flags &= ~(unsigned int)SDL_WINDOW_METAL;
		stripped_metal = 1;
		fprintf(stderr, "sdl_window_shim: stripped SDL_WINDOW_METAL (no Metal on Linux)\n");
	}
	/* On KMSDRM, fullscreen is the only valid mode for EGL surface creation. */
	if (sdl_window_is_kmsdrm_env()) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		fprintf(stderr, "sdl_window_shim: KMSDRM detected, forcing fullscreen\n");
		/* A Metal-origin window is a GPU window; on Linux that GPU path is GLES.
		 * On KMSDRM there is no native window handle to build our own EGL surface
		 * from (SDL holds DRM master and keeps the GBM surface internal), so SDL
		 * must own the GL context. Mark the window SDL_WINDOW_OPENGL and request a
		 * GLES3 config so SDL_GL_CreateContext can bind it (see the Gothic GLES
		 * backend's KMSDRM path in gl_context.cpp). On Wayland/X11 we instead drive
		 * EGL from the native handle, so this is gated to KMSDRM only. */
		if (stripped_metal) {
			flags |= SDL_WINDOW_OPENGL;
			request_gles3_window_config();
			fprintf(stderr, "sdl_window_shim: KMSDRM + Metal-origin window → added "
			        "SDL_WINDOW_OPENGL (SDL owns GBM/EGL)\n");
		}
	}
	/* Keep ALLOW_HIGHDPI — the game was built for Retina displays and handles
	 * HiDPI itself. Stripping it causes a drawable/logical size mismatch on
	 * HiDPI screens (renders at logical size into a physical-size surface,
	 * showing only a quarter of the frame). */
	void* win = real_fn(title, x, y, w, h, flags);
	if (win) {
		captured_sdl_window = win;
		int ww = 0, wh = 0;
		sdl_window_get_drawable_size(win, &ww, &wh);
		fprintf(stderr, "sdl_window_shim: captured SDL window %p (%dx%d, flags=0x%x)\n",
		        win, ww, wh, flags);
	} else {
		/* SDL_CreateWindow returned NULL — the engine waits forever on a window
		 * that never appears (seen on Allwinner H700 / Mali-G31 KMSDRM). Surface
		 * SDL's own reason so the failure is diagnosable instead of silent: the
		 * usual cause on KMSDRM is that no EGL config matches the requested GLES3
		 * visual (depth/stencil/double-buffer) on this driver. */
		const char* (*get_error)(void) =
		    (const char* (*)(void))dlsym(RTLD_DEFAULT, "SDL_GetError");
		const char* err = get_error ? get_error() : NULL;
		fprintf(stderr, "sdl_window_shim: SDL_CreateWindow FAILED (flags=0x%x): %s\n",
		        flags, (err && *err) ? err : "(no SDL error reported)");
	}
	return win;
}

int sdl_set_window_fullscreen_wrapper(void* window, unsigned int flags)
{
	/* Block fullscreen transitions. On macOS, fullscreen changes the display
	 * mode to match the game's resolution (e.g. 960x540). On Linux/Wayland the
	 * display mode can't change, so fullscreen gives a native-res surface while
	 * the game's viewports/cameras stay at the internal resolution — broken. */
	(void)window;
	fprintf(stderr, "sdl_window_shim: blocked SDL_SetWindowFullscreen(0x%x)\n", flags);
	return 0;
}
