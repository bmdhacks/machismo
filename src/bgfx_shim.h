#ifndef _BGFX_SHIM_H_
#define _BGFX_SHIM_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * bgfx init wrapper — intercepts bgfx_init / bgfx::init to fix the
 * renderer type and platform data for Linux.
 *
 * The game fills bgfx_init_t with macOS-specific values (Metal renderer,
 * NSWindow handle). This wrapper:
 *   1. Forces renderer type to GLES/Vulkan/OpenGL
 *   2. Gets the real X11 window handle from SDL2
 *   3. Forwards to native bgfx_init
 */

/* Set the real bgfx_init function pointer (from dlsym on native .so) */
void bgfx_shim_set_real_init(void* func);

/* Set the real bgfx_frame function pointer (from dlsym on native .so) */
void bgfx_shim_set_real_frame(void* func);

/* Set the real bgfx_reset function pointer (from dlsym on native .so) */
void bgfx_shim_set_real_reset(void* func);

/* Set the desired renderer type name ("opengles", "vulkan", "opengl") */
void bgfx_shim_set_renderer(const char* renderer_name);

/* Wrapper function — use as trampoline override target for _bgfx_init
 * and __ZN4bgfx4initERKNS_4InitE */
bool bgfx_init_wrapper(const void* init);

/* Wrapper function — use as trampoline override target for _bgfx_frame.
 * Calls bgfx::touch(0) before every frame to ensure the view clear fires
 * even during scene transitions when no draws are submitted. */
uint32_t bgfx_frame_wrapper(bool capture);

/* Wrapper function — use as trampoline override target for _bgfx_reset.
 * Logs dimensions for debugging coordinate mismatches. */
void bgfx_reset_wrapper(uint32_t width, uint32_t height, uint32_t flags, uint8_t format);

/* SDL_CreateWindow wrapper — captures the window pointer for bgfx.
 * Use as trampoline override target for _SDL_CreateWindow. */
void* sdl_create_window_wrapper(const char* title, int x, int y, int w, int h, unsigned int flags);

/* SDL_SetWindowFullscreen wrapper — blocks fullscreen transitions.
 * Use as trampoline override target for _SDL_SetWindowFullscreen. */
int sdl_set_window_fullscreen_wrapper(void* window, unsigned int flags);

/* --- Diagnostic wrappers for camera/rendering offset investigation --- */

/* Set the real function pointers for diagnostic wrappers */
void bgfx_shim_set_real_set_view_rect(void* func);
void bgfx_shim_set_real_set_view_transform(void* func);
void bgfx_shim_set_real_get_caps(void* func);

/* Wrapper for bgfx_set_view_rect — logs view ID, origin, and dimensions.
 * Use as trampoline override for _bgfx_set_view_rect and C++ variant. */
void bgfx_set_view_rect_wrapper(uint16_t id, uint16_t x, uint16_t y,
                                 uint16_t width, uint16_t height);

/* Wrapper for bgfx_set_view_transform — logs full 4x4 view and projection matrices.
 * Use as trampoline override for _bgfx_set_view_transform and C++ variant. */
void bgfx_set_view_transform_wrapper(uint16_t id, const void* view, const void* proj);

/* Wrapper for bgfx_get_caps — logs rendererType, homogeneousDepth, originBottomLeft.
 * Use as trampoline override for _bgfx_get_caps and C++ variant. */
const void* bgfx_get_caps_wrapper(void);

/* Encoder::setTransform wrapper — logs per-draw model matrices. */
void bgfx_shim_set_real_encoder_set_transform(void* func);
uint32_t bgfx_encoder_set_transform_wrapper(void* encoder, const void* mtx, uint16_t num);

/* Encoder::submit wrapper — logs which view each draw goes to. */
void bgfx_shim_set_real_encoder_submit(void* func);
void bgfx_encoder_submit_wrapper(void* encoder, uint16_t id, uint64_t program_and_depth,
                                  uint32_t extra1, uint8_t flags);

/* SDL_GetWindowSize wrapper — logs window dimensions for debugging. */
void sdl_get_window_size_wrapper(void* window, int* w, int* h);

/* SDL_GL_GetDrawableSize wrapper — logs drawable dimensions for debugging. */
void sdl_gl_get_drawable_size_wrapper(void* window, int* w, int* h);

#endif /* _BGFX_SHIM_H_ */
