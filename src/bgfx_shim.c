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
#define SDL_SYSWM_KMSDRM  15

static void* (*sdl_GetWindowFromID)(uint32_t id) = NULL;
static int (*sdl_GetWindowWMInfo)(SDL_Window*, void* info) = NULL;
static void (*sdl_GetVersion)(SDL_version_t* ver) = NULL;
static void (*sdl_GetWindowSize)(void*, int*, int*) = NULL;

static void* real_bgfx_init_fn = NULL;
static void* real_bgfx_frame_fn = NULL;
static int forced_renderer_type = BGFX_V118_TYPE_OPENGLES;

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

	if (!sdl_GetWindowFromID || !sdl_GetWindowWMInfo) {
		fprintf(stderr, "bgfx_shim: warning: SDL2 functions not found, "
		        "cannot get native window handle\n");
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
	/* Strip fullscreen — force windowed for development.
	 * Target hardware (KMSDRM) will need this re-enabled. */
	if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) {
		fprintf(stderr, "bgfx_shim: stripping fullscreen flags 0x%x\n", flags);
		flags &= ~(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	/* Strip ALLOW_HIGHDPI — prevents drawable/window size mismatch. */
	if (flags & SDL_WINDOW_ALLOW_HIGHDPI) {
		fprintf(stderr, "bgfx_shim: stripping SDL_WINDOW_ALLOW_HIGHDPI\n");
		flags &= ~SDL_WINDOW_ALLOW_HIGHDPI;
	}
	void* win = real_fn(title, x, y, w, h, flags);
	if (win) {
		captured_sdl_window = win;
		fprintf(stderr, "bgfx_shim: captured SDL window %p (requested %dx%d, flags=0x%x)\n",
		        win, w, h, flags);
		/* Log actual sizes for HiDPI debugging */
		if (sdl_GetWindowSize) {
			int ww = 0, wh = 0;
			sdl_GetWindowSize(win, &ww, &wh);
			fprintf(stderr, "bgfx_shim: SDL_GetWindowSize = %dx%d\n", ww, wh);
		}
		typedef void (*sdl_gl_get_drawable_fn)(void*, int*, int*);
		sdl_gl_get_drawable_fn gl_drawable =
			(sdl_gl_get_drawable_fn)dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");
		if (gl_drawable) {
			int dw = 0, dh = 0;
			gl_drawable(win, &dw, &dh);
			fprintf(stderr, "bgfx_shim: SDL_GL_GetDrawableSize = %dx%d\n", dw, dh);
		}
	}
	return win;
}

int sdl_set_window_fullscreen_wrapper(void* window, uint32_t flags)
{
	fprintf(stderr, "bgfx_shim: blocking SDL_SetWindowFullscreen(flags=0x%x)\n", flags);
	(void)window;
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
	fprintf(stderr, "bgfx_shim: original renderer type = %u, forcing %d\n",
	        *type_ptr, forced_renderer_type);
	*type_ptr = forced_renderer_type;

	/* Fix platform data — get real X11 window handle */
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
				} else if (wminfo.subsystem == SDL_SYSWM_KMSDRM) {
					/* KMSDRM: gbm_device as ndt, gbm_surface as nwh — works directly with EGL */
					*ndt = wminfo.x11_display;
					*nwh = (void*)(uintptr_t)wminfo.x11_window;
					fprintf(stderr, "bgfx_shim: KMSDRM gbm_dev=%p gbm_surface=%p\n", *ndt, *nwh);
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

	/* Clear the game's callback and allocator pointers.
	 * The game's callback is a Mach-O object whose cacheReadSize/cacheRead
	 * methods return stale Metal shader cache data, causing bgfx to skip
	 * glLinkProgram (it thinks programs are cached).  With NULL, bgfx uses
	 * its internal CallbackStub which reads from temp/ (doesn't exist). */
#define INIT_CALLBACK_OFFSET  104  /* after resolution(20) + limits(16) + pad, 8-aligned */
#define INIT_ALLOCATOR_OFFSET 112
	void** cb  = (void**)(init_copy + INIT_CALLBACK_OFFSET);
	void** alloc = (void**)(init_copy + INIT_ALLOCATOR_OFFSET);
	fprintf(stderr, "bgfx_shim: clearing game callback=%p allocator=%p\n", *cb, *alloc);
	*cb = NULL;
	*alloc = NULL;

	/* Call real bgfx_init with our modified copy */
	typedef bool (*bgfx_init_fn)(const void*);
	return ((bgfx_init_fn)real_bgfx_init_fn)(init_copy);
}

static void* real_bgfx_reset_fn = NULL;

void bgfx_shim_set_real_reset(void* func)
{
	real_bgfx_reset_fn = func;
}

void bgfx_reset_wrapper(uint32_t width, uint32_t height, uint32_t flags, uint8_t format)
{
	fprintf(stderr, "bgfx_shim: bgfx::reset(%u x %u, flags=0x%x, fmt=%u)\n",
	        width, height, flags, format);

	/* Log SDL sizes for comparison */
	resolve_sdl_funcs();
	if (sdl_GetWindowSize && captured_sdl_window) {
		int ww = 0, wh = 0;
		sdl_GetWindowSize(captured_sdl_window, &ww, &wh);
		fprintf(stderr, "bgfx_shim:   SDL_GetWindowSize = %dx%d\n", ww, wh);
	}
	typedef void (*sdl_gl_get_drawable_fn)(void*, int*, int*);
	sdl_gl_get_drawable_fn gl_drawable =
		(sdl_gl_get_drawable_fn)dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");
	if (gl_drawable && captured_sdl_window) {
		int dw = 0, dh = 0;
		gl_drawable(captured_sdl_window, &dw, &dh);
		fprintf(stderr, "bgfx_shim:   SDL_GL_GetDrawableSize = %dx%d\n", dw, dh);
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
	return ((bgfx_frame_fn)real_bgfx_frame_fn)(capture);
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
	static int log_count = 0;
	if (log_count < 200) {
		fprintf(stderr, "bgfx_shim: setViewRect(view=%u, x=%u, y=%u, w=%u, h=%u)\n",
		        id, x, y, width, height);
		log_count++;
	}
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

void sdl_get_window_size_wrapper(void* window, int* w, int* h)
{
	typedef void (*fn_t)(void*, int*, int*);
	static fn_t real_fn = NULL;
	if (!real_fn)
		real_fn = (fn_t)dlsym(RTLD_DEFAULT, "SDL_GetWindowSize");
	if (real_fn)
		real_fn(window, w, h);
	static int log_count = 0;
	if (log_count < 30) {
		fprintf(stderr, "bgfx_shim: SDL_GetWindowSize -> %dx%d\n",
		        w ? *w : -1, h ? *h : -1);
		log_count++;
	}
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
	static int draw_count = 0;

	if (draw_count < 50) {
		const float* m = (const float*)mtx;
		if (m) {
			fprintf(stderr, "bgfx_shim: setTransform[%d] translate=(%.1f, %.1f, %.1f) scale=(%.3f, %.3f)\n",
			        draw_count, m[12], m[13], m[14],
			        m[0], m[5]);
		}
		draw_count++;
	}
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
	static int log_count = 0;
	if (log_count < 20) {
		fprintf(stderr, "bgfx_shim: Encoder::submit(view=%u)\n", id);
		log_count++;
	}
	typedef void (*fn_t)(void*, uint16_t, uint64_t, uint32_t, uint8_t);
	((fn_t)real_encoder_submit_fn)(encoder, id, program_and_depth, extra1, flags);
}

void sdl_gl_get_drawable_size_wrapper(void* window, int* w, int* h)
{
	typedef void (*fn_t)(void*, int*, int*);
	static fn_t real_fn = NULL;
	if (!real_fn)
		real_fn = (fn_t)dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");
	if (real_fn)
		real_fn(window, w, h);
	static int log_count = 0;
	if (log_count < 30) {
		fprintf(stderr, "bgfx_shim: SDL_GL_GetDrawableSize -> %dx%d\n",
		        w ? *w : -1, h ? *h : -1);
		log_count++;
	}
}
