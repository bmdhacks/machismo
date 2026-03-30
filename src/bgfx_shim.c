/*
 * bgfx init wrapper — intercepts bgfx_init to fix renderer type
 * and platform data for Linux.
 *
 * At bgfx API v118 (game's version):
 *   BGFX_RENDERER_TYPE_OPENGLES = 8
 *   BGFX_RENDERER_TYPE_OPENGL   = 9
 *   BGFX_RENDERER_TYPE_VULKAN   = 10
 *
 * bgfx_platform_data_t layout (v118, 5 fields):
 *   void* ndt;          // offset 0
 *   void* nwh;          // offset 8
 *   void* context;      // offset 16
 *   void* backBuffer;   // offset 24
 *   void* backBufferDS; // offset 32
 *
 * bgfx_init_t layout (v118):
 *   uint32_t type;      // offset 0  (renderer_type enum)
 *   uint16_t vendorId;  // offset 4
 *   uint16_t deviceId;  // offset 6
 *   uint64_t caps;      // offset 8
 *   bool debug;         // offset 16
 *   bool profile;       // offset 17
 *   [padding to 24]
 *   platform_data;      // offset 24 (40 bytes)
 *   resolution;         // offset 64
 *   ...
 *
 * We use raw byte offsets to avoid header version mismatches.
 */

#include "bgfx_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

/* bgfx renderer type enum values at API v118 */
#define BGFX_V118_TYPE_OPENGLES 8
#define BGFX_V118_TYPE_OPENGL   9
#define BGFX_V118_TYPE_VULKAN   10

/* Offsets into bgfx_init_t (v118) */
#define INIT_TYPE_OFFSET        0
#define INIT_PLATFORM_OFFSET    24  /* after type+vendor+device+caps+debug+profile+pad */
#define PLATFORM_NDT_OFFSET     0
#define PLATFORM_NWH_OFFSET     8
#define PLATFORM_CTX_OFFSET    16

/* SDL2 function pointers — resolved at runtime via dlsym */
typedef struct SDL_Window SDL_Window;

typedef struct {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
} SDL_version_t;

/* SDL_SysWMinfo subsystem enum (SDL2 header order) */
#define SDL_SYSWM_X11      2
#define SDL_SYSWM_WAYLAND  6
#define SDL_SYSWM_KMSDRM  13

static void* (*sdl_GetWindowFromID)(uint32_t id) = NULL;
static int (*sdl_GetWindowWMInfo)(SDL_Window*, void* info) = NULL;
static void (*sdl_GetVersion)(SDL_version_t* ver) = NULL;
static void (*sdl_GetWindowSize)(void*, int*, int*) = NULL;
static void (*sdl_GL_GetDrawableSize)(void*, int*, int*) = NULL;

static void* real_bgfx_init_fn = NULL;
static void* real_bgfx_reset_fn = NULL;
static void* real_bgfx_frame_fn = NULL;
static int forced_renderer_type = BGFX_V118_TYPE_OPENGLES;

/* KMSDRM: bgfx skips eglSwapBuffers when context is provided externally.
 * We capture the EGL state during init and swap manually after each frame. */
static void* kmsdrm_egl_display = NULL;
static void* kmsdrm_egl_surface = NULL;

/* Wayland EGL window wrapping — resolved at runtime from libwayland-egl.so.1 */
static void* wayland_egl_lib = NULL;
static void* (*wl_egl_window_create_fn)(void* surface, int width, int height) = NULL;
static void  (*wl_egl_window_destroy_fn)(void* egl_window) = NULL;
static void* wayland_egl_window = NULL;  /* must outlive bgfx */

/* Captured SDL window from SDL_CreateWindow wrapper */
static void* captured_sdl_window = NULL;

void bgfx_shim_set_real_init(void* func)
{
	real_bgfx_init_fn = func;
}

void bgfx_shim_set_real_frame(void* func)
{
	real_bgfx_frame_fn = func;
}

void bgfx_shim_set_renderer(const char* renderer_name)
{
	if (!renderer_name) return;
	if (strcmp(renderer_name, "opengles") == 0)
		forced_renderer_type = BGFX_V118_TYPE_OPENGLES;
	else if (strcmp(renderer_name, "opengl") == 0)
		forced_renderer_type = BGFX_V118_TYPE_OPENGL;
	else if (strcmp(renderer_name, "vulkan") == 0)
		forced_renderer_type = BGFX_V118_TYPE_VULKAN;
	else
		fprintf(stderr, "bgfx_shim: unknown renderer '%s', using opengles\n", renderer_name);
}

static void resolve_sdl_funcs(void)
{
	if (sdl_GetWindowFromID) return;

	/* SDL2 should already be loaded by the trampoline */
	sdl_GetWindowFromID = dlsym(RTLD_DEFAULT, "SDL_GetWindowFromID");
	sdl_GetWindowWMInfo = dlsym(RTLD_DEFAULT, "SDL_GetWindowWMInfo");
	sdl_GetVersion = dlsym(RTLD_DEFAULT, "SDL_GetVersion");
	sdl_GetWindowSize = dlsym(RTLD_DEFAULT, "SDL_GetWindowSize");
	sdl_GL_GetDrawableSize = dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");

	if (!sdl_GetWindowFromID || !sdl_GetWindowWMInfo) {
		fprintf(stderr, "bgfx_shim: warning: SDL2 functions not found, "
		        "cannot get native window handle\n");
	}
}

/* Get actual framebuffer size (physical pixels), falling back to window size */
static void get_drawable_size(void* window, int* w, int* h)
{
	*w = 0; *h = 0;
	if (sdl_GL_GetDrawableSize)
		sdl_GL_GetDrawableSize(window, w, h);
	if (*w <= 0 || *h <= 0) {
		if (sdl_GetWindowSize)
			sdl_GetWindowSize(window, w, h);
	}
}

static int resolve_wayland_egl(void)
{
	if (wl_egl_window_create_fn) return 1;

	wayland_egl_lib = dlopen("libwayland-egl.so.1", RTLD_LAZY);
	if (!wayland_egl_lib) {
		fprintf(stderr, "bgfx_shim: cannot load libwayland-egl.so.1: %s\n", dlerror());
		return 0;
	}
	wl_egl_window_create_fn = dlsym(wayland_egl_lib, "wl_egl_window_create");
	wl_egl_window_destroy_fn = dlsym(wayland_egl_lib, "wl_egl_window_destroy");
	if (!wl_egl_window_create_fn || !wl_egl_window_destroy_fn) {
		fprintf(stderr, "bgfx_shim: wl_egl_window functions not found\n");
		dlclose(wayland_egl_lib);
		wayland_egl_lib = NULL;
		return 0;
	}
	return 1;
}

/*
 * SDL_SysWMinfo is a complex struct with version + subsystem + union.
 * We define a minimal version that covers X11 and Wayland.
 * Layout (SDL2):
 *   SDL_version version;   // 3 bytes
 *   [1 byte pad]
 *   int subsystem;         // 4 bytes (offset 4)
 *   union {
 *     struct { void* display; unsigned long window; ... } x11;  // offset 8
 *     struct { void* display; void* surface; ... } wl;          // offset 8
 *   } info;
 */
typedef struct {
	uint8_t ver_major, ver_minor, ver_patch;
	uint8_t pad;
	int32_t subsystem;
	/* X11 union member: display at offset 8, window at offset 16 */
	void* x11_display;
	unsigned long x11_window;
	/* (rest of union we don't need) */
	char rest[256];
} sdl_wminfo_t;

void* sdl_create_window_wrapper(const char* title, int x, int y, int w, int h, uint32_t flags)
{
	typedef void* (*sdl_create_window_fn)(const char*, int, int, int, int, uint32_t);
	static sdl_create_window_fn real_fn = NULL;
	if (!real_fn)
		real_fn = dlsym(RTLD_DEFAULT, "SDL_CreateWindow");
	if (!real_fn) {
		fprintf(stderr, "bgfx_shim: SDL_CreateWindow not found\n");
		return NULL;
	}
#define SDL_WINDOW_FULLSCREEN         0x00000001
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_WINDOW_ALLOW_HIGHDPI      0x00002000
	/* Detect KMSDRM: check SDL_VIDEODRIVER or absence of DISPLAY/WAYLAND.
	 * On KMSDRM, fullscreen is the only valid mode for EGL surface creation. */
	const char *video_drv = getenv("SDL_VIDEODRIVER");
	int is_kmsdrm = (video_drv && strcmp(video_drv, "KMSDRM") == 0)
	             || (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"));
	if (is_kmsdrm) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		fprintf(stderr, "bgfx_shim: KMSDRM detected, forcing fullscreen\n");
	}
	/* Keep ALLOW_HIGHDPI — the game was built for Retina displays and
	 * handles HiDPI itself. Stripping it causes a drawable/logical size
	 * mismatch on HiDPI screens (bgfx renders at logical size into a
	 * physical-size surface, showing only a quarter of the frame). */
	void* win = real_fn(title, x, y, w, h, flags);
	if (win) {
		captured_sdl_window = win;
		if (!sdl_GetWindowSize)
			sdl_GetWindowSize = dlsym(RTLD_DEFAULT, "SDL_GetWindowSize");
		if (!sdl_GL_GetDrawableSize)
			sdl_GL_GetDrawableSize = dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");
		int ww = 0, wh = 0;
		get_drawable_size(win, &ww, &wh);
		fprintf(stderr, "bgfx_shim: captured SDL window %p (%dx%d, flags=0x%x)\n",
		        win, ww, wh, flags);
	}
	return win;
}

int sdl_set_window_fullscreen_wrapper(void* window, uint32_t flags)
{
	/* Block fullscreen transitions. On macOS, fullscreen changes the display
	 * mode to match the game's resolution (960x540). On Linux/Wayland, the
	 * display mode can't change, so fullscreen gives us a native-res surface
	 * while the game's viewports/cameras stay at 960x540 — broken. */
	(void)window;
	fprintf(stderr, "bgfx_shim: blocked SDL_SetWindowFullscreen(0x%x)\n", flags);
	return 0;
}

bool bgfx_init_wrapper(const void* _init)
{
	if (!real_bgfx_init_fn) {
		fprintf(stderr, "bgfx_shim: real_bgfx_init not set!\n");
		return 0;
	}

	/* Make a mutable copy of the init struct.
	 * bgfx_init_t at v118 is ~120 bytes. Copy generously. */
	uint8_t init_copy[256];
	memcpy(init_copy, _init, sizeof(init_copy));

	/* Force renderer type */
	uint32_t* type_ptr = (uint32_t*)(init_copy + INIT_TYPE_OFFSET);
	*type_ptr = forced_renderer_type;

	/* Detect KMSDRM from environment (same check as sdl_create_window_wrapper).
	 * Used as fallback when SDL_GetWindowWMInfo returns subsystem=0. */
	const char *video_drv = getenv("SDL_VIDEODRIVER");
	int is_kmsdrm = (video_drv && strcmp(video_drv, "KMSDRM") == 0)
	             || (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"));

	/* Fix platform data — get real window handle */
	resolve_sdl_funcs();

	if (sdl_GetWindowWMInfo && sdl_GetVersion) {
		SDL_Window* win = (SDL_Window*)captured_sdl_window;
		if (!win && sdl_GetWindowFromID)
			win = sdl_GetWindowFromID(1);  /* fallback */
		if (win) {
			sdl_wminfo_t wminfo;
			memset(&wminfo, 0, sizeof(wminfo));
			/* Fill in compiled SDL version */
			sdl_GetVersion((SDL_version_t*)&wminfo);
			if (sdl_GetWindowWMInfo(win, &wminfo)) {
				void** ndt = (void**)(init_copy + INIT_PLATFORM_OFFSET + PLATFORM_NDT_OFFSET);
				void** nwh = (void**)(init_copy + INIT_PLATFORM_OFFSET + PLATFORM_NWH_OFFSET);

				if (wminfo.subsystem == SDL_SYSWM_X11) {
					*ndt = wminfo.x11_display;
					*nwh = (void*)(uintptr_t)wminfo.x11_window;
					fprintf(stderr, "bgfx_shim: X11 display=%p window=0x%lx\n",
					        wminfo.x11_display, wminfo.x11_window);
				} else if (wminfo.subsystem == SDL_SYSWM_WAYLAND) {
					void* wl_display = wminfo.x11_display;
					void* wl_surface = (void*)(uintptr_t)wminfo.x11_window;
					*ndt = wl_display;
					/* bgfx v118 can't use wl_surface directly --
					 * wrap it in a wl_egl_window via libwayland-egl */
					if (resolve_wayland_egl()) {
						int w = 960, h = 540;
						if (sdl_GetWindowSize)
							sdl_GetWindowSize(win, &w, &h);
						wayland_egl_window = wl_egl_window_create_fn(wl_surface, w, h);
						if (wayland_egl_window) {
							*nwh = wayland_egl_window;
							fprintf(stderr, "bgfx_shim: Wayland display=%p egl_window=%p (%dx%d)\n",
							        wl_display, wayland_egl_window, w, h);
						} else {
							*nwh = wl_surface;
							fprintf(stderr, "bgfx_shim: wl_egl_window_create failed\n");
						}
					} else {
						*nwh = wl_surface;
						fprintf(stderr, "bgfx_shim: Wayland (no egl wrapper) display=%p surface=%p\n",
						        wl_display, wl_surface);
					}
				} else if (wminfo.subsystem == SDL_SYSWM_KMSDRM
			           || (wminfo.subsystem == 0 && is_kmsdrm)) {
					if (wminfo.subsystem == 0)
						fprintf(stderr, "bgfx_shim: subsystem=0 but KMSDRM env detected, using KMSDRM path\n");
					/* KMSDRM: SDL keeps the GBM surface internal (not in WMInfo).
					 * Create an SDL GL context so EGL is fully initialized,
					 * then pass the current EGL display/context/surface to bgfx
					 * so it shares SDL's context instead of creating a new one. */
					typedef void* (*sdl_gl_create_fn)(void*);
					sdl_gl_create_fn gl_create =
						(sdl_gl_create_fn)dlsym(RTLD_DEFAULT, "SDL_GL_CreateContext");
					if (gl_create) {
						void *gl_ctx = gl_create(win);
						if (gl_ctx) {
							fprintf(stderr, "bgfx_shim: KMSDRM SDL GL context created: %p\n", gl_ctx);
						} else {
							fprintf(stderr, "bgfx_shim: KMSDRM SDL_GL_CreateContext failed\n");
						}
					}

					/* Grab EGL state that SDL set up.  Multiple strategies
					 * because the Mali blob and Mesa's libEGL dispatch can
					 * coexist with separate thread-local state.  All three
					 * (display/context/surface) must come from the same
					 * EGL implementation. */
					typedef void* (*egl_get_fn)(void);
					typedef void* (*egl_get_surface_fn)(int);
					typedef void* (*sdl_gl_getproc_fn)(const char*);

					void *egl_display = NULL, *egl_context = NULL, *egl_surface = NULL;

					/* Helper: given a dlsym source, try to grab all three */
					#define TRY_EGL_FROM(src, label) do { \
						egl_get_fn _gd = (egl_get_fn)dlsym(src, "eglGetCurrentDisplay"); \
						if (_gd && (_gd())) { \
							egl_display = _gd(); \
							egl_get_fn _gc = (egl_get_fn)dlsym(src, "eglGetCurrentContext"); \
							egl_get_surface_fn _gs = (egl_get_surface_fn)dlsym(src, "eglGetCurrentSurface"); \
							egl_context = _gc ? _gc() : NULL; \
							egl_surface = _gs ? _gs(0x3059 /*EGL_DRAW*/) : NULL; \
							fprintf(stderr, "bgfx_shim: resolved EGL via %s\n", label); \
						} \
					} while(0)

					/* Strategy 1: RTLD_DEFAULT — works when libEGL is global */
					TRY_EGL_FROM(RTLD_DEFAULT, "RTLD_DEFAULT");

					/* Strategy 2: explicit libEGL.so.1 */
					if (!egl_display) {
						void *egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
						if (!egl_lib)
							egl_lib = dlopen("libEGL.so.1", RTLD_NOW);
						if (egl_lib)
							TRY_EGL_FROM(egl_lib, "libEGL.so.1");
					}

					/* Strategy 3: SDL_GL_GetProcAddress — routes through
					 * whatever EGL implementation SDL actually loaded
					 * (e.g. libmali.so vs Mesa dispatch) */
					if (!egl_display) {
						sdl_gl_getproc_fn gl_getproc =
							(sdl_gl_getproc_fn)dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
						if (gl_getproc) {
							egl_get_fn gd = (egl_get_fn)gl_getproc("eglGetCurrentDisplay");
							if (gd && gd()) {
								egl_display = gd();
								egl_get_fn gc = (egl_get_fn)gl_getproc("eglGetCurrentContext");
								egl_get_surface_fn gs = (egl_get_surface_fn)gl_getproc("eglGetCurrentSurface");
								egl_context = gc ? gc() : NULL;
								egl_surface = gs ? gs(0x3059) : NULL;
								fprintf(stderr, "bgfx_shim: resolved EGL via SDL_GL_GetProcAddress\n");
							}
						}
					}

					/* Strategy 4: libmali.so directly (ARM Mali blob) */
					if (!egl_display) {
						void *mali_lib = dlopen("libmali.so", RTLD_NOW | RTLD_NOLOAD);
						if (mali_lib)
							TRY_EGL_FROM(mali_lib, "libmali.so");
					}

					#undef TRY_EGL_FROM

					if (egl_display) {
						*ndt = egl_display;
						*nwh = egl_surface;
						void** ctx = (void**)(init_copy + INIT_PLATFORM_OFFSET + PLATFORM_CTX_OFFSET);
						*ctx = egl_context;
						kmsdrm_egl_display = egl_display;
						kmsdrm_egl_surface = egl_surface;
						fprintf(stderr, "bgfx_shim: KMSDRM EGL display=%p context=%p surface=%p\n",
						        egl_display, egl_context, egl_surface);
					} else {
						/* Strategy 5: GBM device from wminfo — let bgfx
						 * create its own EGL display/context/surface.
						 * KMSDRM wminfo union (at offset 8):
						 *   +0: int dev_index, +4: int drm_fd,
						 *   +8: gbm_device*, +16: gbm_surface* */
						uint8_t *info_base = (uint8_t *)&wminfo.x11_display;
						void *gbm_dev     = *(void **)(info_base + 8);
						void *gbm_surface = *(void **)(info_base + 16);
						*ndt = gbm_dev;
						*nwh = gbm_surface;
						fprintf(stderr, "bgfx_shim: EGL query failed, falling back to GBM: dev=%p surface=%p\n",
						        gbm_dev, gbm_surface);
					}
				} else {
					fprintf(stderr, "bgfx_shim: unknown SDL subsystem %d\n",
					        wminfo.subsystem);
				}

			} else {
				fprintf(stderr, "bgfx_shim: SDL_GetWindowWMInfo failed\n");
			}
		} else {
			fprintf(stderr, "bgfx_shim: SDL_GetWindowFromID(1) returned NULL\n");
		}
	}

	/* Clamp transient buffer sizes.
	 * The game's Init struct requests 8MB transient VB (RenderDoc shows only
	 * 67KB actually used per frame).  On Mali with shared memory this is
	 * catastrophic.  Clamp to match our build-time config.h overrides. */
#define INIT_LIMITS_TRANSIENT_VB_OFFSET  92
#define INIT_LIMITS_TRANSIENT_IB_OFFSET  96
#define MAX_TRANSIENT_VB  (1<<20)    /* 1MB */
#define MAX_TRANSIENT_IB  (64<<10)   /* 64KB */
	uint32_t* tvb = (uint32_t*)(init_copy + INIT_LIMITS_TRANSIENT_VB_OFFSET);
	uint32_t* tib = (uint32_t*)(init_copy + INIT_LIMITS_TRANSIENT_IB_OFFSET);
	if (*tvb > MAX_TRANSIENT_VB) *tvb = MAX_TRANSIENT_VB;
	if (*tib > MAX_TRANSIENT_IB) *tib = MAX_TRANSIENT_IB;

	/* Override init resolution to match actual drawable size (physical pixels) */
#define INIT_RESOLUTION_FORMAT_OFFSET 64
#define INIT_RESOLUTION_WIDTH_OFFSET  68
#define INIT_RESOLUTION_HEIGHT_OFFSET 72
#define INIT_RESOLUTION_RESET_OFFSET  76
#define BGFX_RESET_MSAA_MASK 0x00000070
	if (captured_sdl_window) {
		int ww = 0, wh = 0;
		get_drawable_size(captured_sdl_window, &ww, &wh);
		uint32_t* res_w = (uint32_t*)(init_copy + INIT_RESOLUTION_WIDTH_OFFSET);
		uint32_t* res_h = (uint32_t*)(init_copy + INIT_RESOLUTION_HEIGHT_OFFSET);
		if (ww > 0 && wh > 0 && (*res_w != (uint32_t)ww || *res_h != (uint32_t)wh)) {
			fprintf(stderr, "bgfx_shim: init resolution %ux%u -> %dx%d (matching window)\n",
			        *res_w, *res_h, ww, wh);
			*res_w = ww;
			*res_h = wh;
		}
	}

	/* Strip MSAA from resolution flags — Mali G31 can't afford the extra
	 * full-resolution RGBA8 + depth textures for MSAA resolve. */
	uint32_t* reset_flags = (uint32_t*)(init_copy + INIT_RESOLUTION_RESET_OFFSET);
	*reset_flags &= ~BGFX_RESET_MSAA_MASK;

	/* Clear the game's callback and allocator pointers.
	 * The game's callback is a Mach-O object whose cacheReadSize/cacheRead
	 * methods return stale Metal shader cache data, causing bgfx to skip
	 * glLinkProgram (it thinks programs are cached).  With NULL, bgfx uses
	 * its internal CallbackStub which reads from temp/ (doesn't exist). */
#define INIT_CALLBACK_OFFSET  104  /* after resolution(20) + limits(16) + pad, 8-aligned */
#define INIT_ALLOCATOR_OFFSET 112
	void** cb  = (void**)(init_copy + INIT_CALLBACK_OFFSET);
	void** alloc = (void**)(init_copy + INIT_ALLOCATOR_OFFSET);
	*cb = NULL;
	*alloc = NULL;

	/* Call real bgfx_init with our modified copy */
	typedef bool (*bgfx_init_fn)(const void*);
	return ((bgfx_init_fn)real_bgfx_init_fn)(init_copy);
}

void bgfx_shim_set_real_reset(void* func)
{
	real_bgfx_reset_fn = func;
}

void bgfx_reset_wrapper(uint32_t width, uint32_t height, uint32_t flags, uint8_t format)
{
	/* Strip MSAA — can't afford extra resolve textures on Mali */
	flags &= ~BGFX_RESET_MSAA_MASK;

	/* Override resolution to match actual drawable size. The game hardcodes
	 * its internal resolution but the display may be different (e.g. 640x480
	 * handheld vs game's 960x540, or HiDPI scaling). */
	if (captured_sdl_window) {
		int ww = 0, wh = 0;
		get_drawable_size(captured_sdl_window, &ww, &wh);
		if (ww > 0 && wh > 0 && ((uint32_t)ww != width || (uint32_t)wh != height)) {
			fprintf(stderr, "bgfx_shim: reset %ux%u -> %dx%d (matching window)\n",
			        width, height, ww, wh);
			width = ww;
			height = wh;
		}
	}

	if (!real_bgfx_reset_fn) {
		fprintf(stderr, "bgfx_shim: real_bgfx_reset not set!\n");
		return;
	}
	typedef void (*bgfx_reset_fn)(uint32_t, uint32_t, uint32_t, uint8_t);
	((bgfx_reset_fn)real_bgfx_reset_fn)(width, height, flags, format);
}

uint32_t bgfx_frame_wrapper(bool capture)
{
	if (!real_bgfx_frame_fn) {
		fprintf(stderr, "bgfx_shim: real_bgfx_frame not set!\n");
		return 0;
	}

	/* Always touch view 0 before frame submission.
	 *
	 * The game's displayImpl only calls bgfx::touch(0) when no RenderFrame
	 * was submitted.  But submitFrame can return early (RenderFrame data ptr
	 * is NULL during scene transitions) while the caller still thinks draws
	 * were submitted — skipping touch(0).  Without touch, bgfx never
	 * processes view 0, and the clear never fires → ghosting.
	 *
	 * touch(0) is a no-op if the view already has draw calls, so calling it
	 * unconditionally is safe. */
	typedef void (*bgfx_touch_fn)(uint16_t);
	static bgfx_touch_fn touch_fn = NULL;
	if (!touch_fn)
		touch_fn = (bgfx_touch_fn)dlsym(RTLD_DEFAULT, "bgfx_touch");
	if (touch_fn)
		touch_fn(0);

	/* Force a full-framebuffer clear via bgfx_set_view_clear.
	 * Re-apply every frame to ensure clear fires even if game doesn't set it.
	 * flags=3 = BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH, rgba=black, depth=1.0 */
	typedef void (*bgfx_set_view_clear_fn)(uint16_t, uint16_t, uint32_t, float, uint8_t);
	static bgfx_set_view_clear_fn set_view_clear_fn = NULL;
	if (!set_view_clear_fn)
		set_view_clear_fn = (bgfx_set_view_clear_fn)dlsym(RTLD_DEFAULT, "bgfx_set_view_clear");
	if (set_view_clear_fn)
		set_view_clear_fn(0, 0x0003, 0x000000ff, 1.0f, 0);

	/* Force view 0 rect to full window every frame.
	 * If updateSurfaceSize missed or the BackbufferRatio setViewRect from
	 * initGraphics is stale, this ensures view 0 always covers the window. */
	typedef void (*bgfx_set_view_rect_fn)(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t);
	static bgfx_set_view_rect_fn force_rect_fn = NULL;
	if (!force_rect_fn)
		force_rect_fn = (bgfx_set_view_rect_fn)dlsym(RTLD_DEFAULT, "bgfx_set_view_rect");
	if (force_rect_fn && captured_sdl_window && sdl_GetWindowSize) {
		int ww = 0, wh = 0;
		sdl_GetWindowSize(captured_sdl_window, &ww, &wh);
		force_rect_fn(0, 0, 0, (uint16_t)ww, (uint16_t)wh);
	}

	typedef uint32_t (*bgfx_frame_fn)(bool);
	uint32_t result = ((bgfx_frame_fn)real_bgfx_frame_fn)(capture);

	/* KMSDRM: bgfx doesn't swap when context was provided externally.
	 * Use SDL_GL_SwapWindow (not raw eglSwapBuffers) so SDL can do
	 * the GBM surface lock and DRM page flip that KMSDRM requires. */
	if (kmsdrm_egl_display && captured_sdl_window) {
		typedef void (*sdl_swap_fn)(void*);
		static sdl_swap_fn swap_fn = NULL;
		if (!swap_fn)
			swap_fn = (sdl_swap_fn)dlsym(RTLD_DEFAULT, "SDL_GL_SwapWindow");
		if (swap_fn)
			swap_fn(captured_sdl_window);
	}

	return result;
}

/* ------------------------------------------------------------------ */
/* Diagnostic wrappers — log rendering state for offset investigation */
/* ------------------------------------------------------------------ */

static void* real_bgfx_set_view_rect_fn = NULL;
static void* real_bgfx_set_view_transform_fn = NULL;
static void* real_bgfx_get_caps_fn = NULL;

void bgfx_shim_set_real_set_view_rect(void* func)
{
	real_bgfx_set_view_rect_fn = func;
}

void bgfx_shim_set_real_set_view_transform(void* func)
{
	real_bgfx_set_view_transform_fn = func;
}

void bgfx_shim_set_real_get_caps(void* func)
{
	real_bgfx_get_caps_fn = func;
}

void bgfx_set_view_rect_wrapper(uint16_t id, uint16_t x, uint16_t y,
                                 uint16_t width, uint16_t height)
{
	typedef void (*fn_t)(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t);
	((fn_t)real_bgfx_set_view_rect_fn)(id, x, y, width, height);
}

void bgfx_set_view_transform_wrapper(uint16_t id, const void* view, const void* proj)
{
	typedef void (*fn_t)(uint16_t, const void*, const void*);
	((fn_t)real_bgfx_set_view_transform_fn)(id, view, proj);
}

const void* bgfx_get_caps_wrapper(void)
{
	static int logged = 0;
	typedef const void* (*fn_t)(void);
	const void* caps = ((fn_t)real_bgfx_get_caps_fn)();
	if (!logged && caps) {
		const uint8_t* c = (const uint8_t*)caps;
		/* bgfx::Caps layout (v118):
		 *   uint32_t rendererType      @ 0
		 *   ...supported                @ 4  (uint64_t)
		 *   uint16_t vendorId           @ 12
		 *   uint16_t deviceId           @ 14
		 *   ...
		 *   bool homogeneousDepth       @ 20  (0x14)
		 *   bool originBottomLeft       @ 21  (0x15)
		 */
		fprintf(stderr, "bgfx_shim: getCaps() -> %p\n"
		        "  rendererType=%u, homogeneousDepth=%u, originBottomLeft=%u\n",
		        caps,
		        *(const uint32_t*)(c + 0),
		        c[20],
		        c[21]);
		logged = 1;
	}
	return caps;
}


/* Encoder::setTransform wrapper — logs model matrices.
 * C++ member function: this=encoder, _mtx=matrix, _num=count.
 * On aarch64, 'this' is in x0, so we receive it as the first param. */
static void* real_encoder_set_transform_fn = NULL;

void bgfx_shim_set_real_encoder_set_transform(void* func)
{
	real_encoder_set_transform_fn = func;
}

uint32_t bgfx_encoder_set_transform_wrapper(void* encoder, const void* mtx, uint16_t num)
{
	typedef uint32_t (*fn_t)(void*, const void*, uint16_t);
	return ((fn_t)real_encoder_set_transform_fn)(encoder, mtx, num);
}

/* Encoder::submit wrapper — tracks frame boundaries for transform logging */
static void* real_encoder_submit_fn = NULL;

void bgfx_shim_set_real_encoder_submit(void* func)
{
	real_encoder_submit_fn = func;
}

void bgfx_encoder_submit_wrapper(void* encoder, uint16_t id, uint64_t program_and_depth,
                                  uint32_t extra1, uint8_t flags)
{
	typedef void (*fn_t)(void*, uint16_t, uint64_t, uint32_t, uint8_t);
	((fn_t)real_encoder_submit_fn)(encoder, id, program_and_depth, extra1, flags);
}


