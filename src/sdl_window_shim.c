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
#include <sys/ioctl.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>

/* SDL window flag bits (SDL2 values; identical under SDL3/SDL2-compat). */
#define SDL_WINDOW_FULLSCREEN         0x00000001
#define SDL_WINDOW_OPENGL             0x00000002
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_WINDOW_ALLOW_HIGHDPI      0x00002000
#define SDL_WINDOW_VULKAN             0x10000000
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

/* Render-backend hint pushed by the loader before the game creates its window.
 * -1 = unknown (not a backend-selecting override lib), 0 = GLES, 1 = Vulkan.
 * Set from machismo.c via the override lib's gothic_backend_is_vulkan(), so the
 * window's GPU flag (SDL_WINDOW_VULKAN vs SDL_WINDOW_OPENGL) always matches the
 * backend the renderer actually installs. */
static int backend_is_vulkan = -1;

void sdl_window_set_vulkan(int is_vulkan)
{
	backend_is_vulkan = is_vulkan ? 1 : 0;
}

/* Hand the KMSDRM display off to a Vulkan driver that owns it directly.
 *
 * SDL's KMSDRM video driver grabs the DRM master when it modesets the window. A
 * Vulkan ICD that presents via VK_KHR_display direct mode (libmali — it offers no
 * VK_EXT_acquire_drm_display / lease, so it cannot take a display from another
 * master) then enumerates ZERO displays ("Vulkan can't find any displays") because
 * the master is held elsewhere. Dropping SDL's master lets the Vulkan driver
 * acquire the display. SDL keeps its fd open and still services input (evdev needs
 * no master); for a fullscreen handheld game that hands the screen to Vulkan this
 * is the intended ownership transfer. No-op (and harmless) off KMSDRM.
 *
 * Returns the ioctl rc (0 = master dropped), or -1 if not a KMSDRM window. */
int sdl_window_kmsdrm_drop_master(void* window)
{
	(void)window;
#ifndef DRM_IOCTL_DROP_MASTER
#define DRM_IOCTL_DROP_MASTER 0x641f   /* _IO('d', 0x1f) */
#endif
	/* Find the DRM master fd by inspecting our open fds rather than via SDL's
	 * WMinfo struct (whose KMSDRM layout is ABI-fragile and silently mis-read on
	 * the last attempt): scan /proc/self/fd for /dev/dri/card* and drop master on
	 * each. The fd SDL set master on succeeds; the rest return EINVAL/EACCES and are
	 * ignored. */
	DIR *d = opendir("/proc/self/fd");
	if (!d) {
		fprintf(stderr, "sdl_window_shim: drop_master: opendir(/proc/self/fd) failed: %s\n",
		        strerror(errno));
		return -1;
	}
	struct dirent *e;
	int dropped = 0, dri_seen = 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		char path[64], target[256];
		snprintf(path, sizeof path, "/proc/self/fd/%s", e->d_name);
		ssize_t n = readlink(path, target, sizeof target - 1);
		if (n <= 0)
			continue;
		target[n] = '\0';
		if (strncmp(target, "/dev/dri/", 9) != 0)
			continue;
		dri_seen++;
		int fd = atoi(e->d_name);
		int is_card = (strncmp(target, "/dev/dri/card", 13) == 0);
		if (!is_card) {
			fprintf(stderr, "sdl_window_shim: drop_master: fd=%d %s (render node, skip)\n",
			        fd, target);
			continue;
		}
		int rc = ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
		fprintf(stderr, "sdl_window_shim: drop_master: fd=%d %s rc=%d (%s)\n",
		        fd, target, rc, rc == 0 ? "was master -> dropped" : strerror(errno));
		if (rc == 0)
			dropped++;
	}
	closedir(d);
	if (!dri_seen)
		fprintf(stderr, "sdl_window_shim: drop_master: NO /dev/dri/* fd open in this process "
		        "(SDL not on KMSDRM, or the display master is held by another process)\n");
	else if (!dropped)
		fprintf(stderr, "sdl_window_shim: drop_master: %d dri fd(s) but none held master\n", dri_seen);
	return dropped ? 0 : -1;
}

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
	/* On KMSDRM, fullscreen is the only valid mode for EGL/Vulkan surface creation. */
	if (sdl_window_is_kmsdrm_env()) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		fprintf(stderr, "sdl_window_shim: KMSDRM detected, forcing fullscreen\n");
	}
	/* For a Metal-origin window, pick the GPU flag matching the active backend.
	 * SDL2 rejects SDL_WINDOW_OPENGL|SDL_WINDOW_VULKAN together.
	 * GLES: on KMSDRM SDL must own the GBM/EGL context (SDL_WINDOW_OPENGL +
	 *       SDL_GL_CreateContext); on Wayland/X11 we drive EGL from the native
	 *       handle and don't need any SDL GPU flag.
	 * Vulkan: SDL_Vulkan_CreateSurface requires SDL_WINDOW_VULKAN on all backends
	 *         (KMSDRM uses VK_KHR_display under the hood, Wayland uses wl_surface). */
	if (stripped_metal) {
		/* Match the window's GPU flag to the render backend. The loader pushes the
		 * choice (sdl_window_set_vulkan) before this point, using the SAME memoized
		 * decision the renderer's select_backend() makes — so a default/auto-prefer
		 * run that lands on Vulkan still gets SDL_WINDOW_VULKAN here (the previous
		 * code only checked GOTHIC_BACKEND==vulkan and mis-flagged the auto case as
		 * GLES, breaking SDL_Vulkan_CreateSurface). If no hint was pushed (non-Gothic
		 * override lib), fall back to the env var. */
		int want_vulkan = backend_is_vulkan;
		if (want_vulkan < 0) {
			const char *want_backend = getenv("GOTHIC_BACKEND");
			want_vulkan = want_backend && strcmp(want_backend, "vulkan") == 0;
		}
		if (want_vulkan) {
			flags |= SDL_WINDOW_VULKAN;
			fprintf(stderr, "sdl_window_shim: render backend = Vulkan → SDL_WINDOW_VULKAN\n");
		} else if (sdl_window_is_kmsdrm_env()) {
			/* GLES on KMSDRM: SDL must own the GBM/EGL surface. */
			flags |= SDL_WINDOW_OPENGL;
			request_gles3_window_config();
			fprintf(stderr, "sdl_window_shim: KMSDRM + Metal-origin window → added "
			        "SDL_WINDOW_OPENGL (SDL owns GBM/EGL)\n");
		}
		/* GLES on Wayland/X11: no SDL GPU flag needed; EGL from native handle. */
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
