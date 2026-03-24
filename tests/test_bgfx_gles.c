/*
 * Minimal bgfx GLES test — creates an SDL2 window, initializes bgfx with
 * OpenGL ES, clears to blue for 60 frames, then exits.
 *
 * Build:
 *   gcc -o test_bgfx_gles test_bgfx_gles.c \
 *     -I../bgfx/include -I../bx/include \
 *     -L../build-bgfx -lbgfx-shared \
 *     $(sdl2-config --cflags --libs) -lGL -ldl
 *
 * Run:
 *   LD_LIBRARY_PATH=../build-bgfx ./test_bgfx_gles
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <bgfx/c99/bgfx.h>

#define WIDTH 640
#define HEIGHT 480

int main(int argc, char** argv)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window* win = SDL_CreateWindow("bgfx GLES test",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	/* Get native window handle */
	SDL_SysWMinfo wmi;
	SDL_VERSION(&wmi.version);
	if (!SDL_GetWindowWMInfo(win, &wmi)) {
		fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	/* Set up bgfx platform data */
	bgfx_platform_data_t pd;
	memset(&pd, 0, sizeof(pd));

	switch (wmi.subsystem) {
	case SDL_SYSWM_X11:
		pd.ndt = wmi.info.x11.display;
		pd.nwh = (void*)(uintptr_t)wmi.info.x11.window;
		fprintf(stderr, "X11: display=%p window=0x%lx\n",
			pd.ndt, (unsigned long)(uintptr_t)pd.nwh);
		break;
	case SDL_SYSWM_WAYLAND:
		pd.ndt = wmi.info.wl.display;
		pd.nwh = wmi.info.wl.surface;
		fprintf(stderr, "Wayland: display=%p surface=%p\n", pd.ndt, pd.nwh);
		break;
	default:
		fprintf(stderr, "Unknown SDL subsystem: %d\n", wmi.subsystem);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	bgfx_set_platform_data(&pd);

	/* Init bgfx with GLES renderer */
	bgfx_init_t init;
	bgfx_init_ctor(&init);
	init.type = BGFX_RENDERER_TYPE_OPENGLES;
	init.resolution.width = WIDTH;
	init.resolution.height = HEIGHT;
	init.resolution.reset = BGFX_RESET_VSYNC;
	init.platformData = pd;

	if (!bgfx_init(&init)) {
		fprintf(stderr, "bgfx_init failed\n");
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	fprintf(stderr, "bgfx initialized: renderer=%s\n",
		bgfx_get_renderer_name(bgfx_get_renderer_type()));

	bgfx_set_view_clear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
		0x2040a0ff, 1.0f, 0);
	bgfx_set_view_rect(0, 0, 0, WIDTH, HEIGHT);

	/* Render 60 frames (blue screen) */
	for (int frame = 0; frame < 60; frame++) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) goto done;
		}

		bgfx_touch(0);
		bgfx_frame(false);
	}

done:
	fprintf(stderr, "Rendered 60 frames OK\n");
	bgfx_shutdown();
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
