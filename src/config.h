/*
 * INI-style config file parser for machismo.
 *
 * Format:
 *   [general]
 *   dylib_map = dylib_map.conf
 *   patches = patches/necrodancer.conf
 *
 *   [trampoline.sdl2]
 *   lib = libSDL2-2.0.so.0
 *   prefix = _SDL_
 *
 *   [trampoline.bgfx]
 *   lib = ./build-bgfx/libbgfx-shared.so
 *   prefix = _bgfx_
 *   prefix = __ZN4bgfx
 *   init_wrapper = true
 *   renderer = opengles
 */

#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_MAX_TRAMPOLINES 8
#define CONFIG_MAX_PREFIXES    8

typedef struct {
	char* name;                              /* section suffix, e.g. "sdl2" */
	char* lib;                               /* native .so path */
	char* prefixes[CONFIG_MAX_PREFIXES];     /* symbol prefix patterns */
	int num_prefixes;
	int init_wrapper;                        /* intercept bgfx init */
	char* renderer;                          /* force renderer type */
} machismo_trampoline_config_t;

typedef struct {
	char* dylib_map;
	char* patches;
	machismo_trampoline_config_t trampolines[CONFIG_MAX_TRAMPOLINES];
	int num_trampolines;
} machismo_config_t;

/* Load config from file. Returns 0 on success, -1 if file not found (not an error). */
int config_load(const char* path, machismo_config_t* cfg);

/* Free all allocated strings in config. */
void config_free(machismo_config_t* cfg);

#endif /* CONFIG_H */
