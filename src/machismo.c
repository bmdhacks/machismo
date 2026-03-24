/*
 * Machismo — Mach-O Loader for aarch64 Linux.
 *
 * Based on Darling's mldr, stripped of darlingserver dependencies,
 * adapted for aarch64, with simplified library handling.
 *
 * Copyright (C) 2017 Lubos Dolezel (original Darling code)
 * Modified for standalone aarch64 use.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "macho_defs.h"
#include <dlfcn.h>
#include <endian.h>
#include <pthread.h>
#include "commpage.h"
#include "loader.h"
#include "resolver.h"
#include "trampoline.h"
#include "gdb_jit.h"
#include "patcher.h"
#include "config.h"
#include "bgfx_shim.h"
#include "eh_frame.h"
#include "dylib_loader.h"
#include <sys/resource.h>
#include <pthread.h>
#include <sys/utsname.h>

/* VM_PROT_* and SEG_DATA are defined in macho_defs.h */

#ifndef PAGE_SIZE
#	define PAGE_SIZE	4096
#endif
#define PAGE_ALIGN(x) (x & ~(PAGE_SIZE-1))

static void load64(int fd, bool expect_dylinker, struct load_results* lr);
static void load_fat(int fd, cpu_type_t cpu, bool expect_dylinker, char** argv, struct load_results* lr);
static void load(const char* path, cpu_type_t cpu, bool expect_dylinker, char** argv, struct load_results* lr);
static int native_prot(int prot);
static void setup_space(struct load_results* lr, bool is_64_bit);
static void start_thread(struct load_results* lr);
static bool is_kernel_at_least(int major, int minor);
static void* compatible_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
static void setup_stack64(const char* filepath, struct load_results* lr);

/* UUID of the main executable */
uint8_t exe_uuid[16];

__attribute__((used))
struct load_results machismo_load_results = {0};

void* __machismo_main_stack_top = NULL;

static int kernel_major = -1;
static int kernel_minor = -1;

int main(int argc, char** argv, char** envp)
{
	char *filename;

	machismo_load_results.kernfd = -1;
	machismo_load_results.argc = argc;
	machismo_load_results.argv = argv;

	while (envp[machismo_load_results.envc] != NULL) {
		++machismo_load_results.envc;
	}
	machismo_load_results.envp = envp;

	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <mach-o-binary> [args...]\n", argv[0]);
		return 1;
	}

	filename = argv[1];

	/* Load the Mach-O binary */
	load(filename, 0, false, argv, &machismo_load_results);

	/* Adjust argv: remove our own argv[0] so the loaded binary sees its path as argv[0] */
	machismo_load_results.argc = argc - 1;
	machismo_load_results.argv = argv + 1;

	/* Load config file — look next to the binary, or use MACHISMO_CONFIG */
	machismo_config_t cfg = {0};
	{
		const char* cfg_path = getenv("MACHISMO_CONFIG");
		int cfg_loaded = 0;

		if (cfg_path) {
			/* MACHISMO_CONFIG=none disables config loading (for tests) */
			if (strcmp(cfg_path, "none") != 0)
				cfg_loaded = (config_load(cfg_path, &cfg) == 0);
		} else {
			char conf_buf[4096];

			/* 1. Look for machismo.conf in the current working directory */
			cfg_loaded = (config_load("machismo.conf", &cfg) == 0);
			if (cfg_loaded) cfg_path = "machismo.conf";

			/* 2. Look for machismo.conf next to the game binary */
			if (!cfg_loaded) {
				const char* binary = machismo_load_results.argv[0];
				const char* slash = strrchr(binary, '/');
				if (slash) {
					size_t dirlen = slash - binary;
					snprintf(conf_buf, sizeof(conf_buf), "%.*s/machismo.conf",
					         (int)dirlen, binary);
				} else {
					snprintf(conf_buf, sizeof(conf_buf), "machismo.conf");
				}
				cfg_loaded = (config_load(conf_buf, &cfg) == 0);
				if (cfg_loaded)
					cfg_path = conf_buf;
			}
		}

		if (cfg_loaded) {
			fprintf(stderr, "machismo: loaded config from %s\n", cfg_path);
		} else {
			/* Fallback to env vars for backward compat */
			const char* map = getenv("MACHISMO_DYLIB_MAP");
			if (map) cfg.dylib_map = strdup(map);
			const char* pat = getenv("MACHISMO_PATCHES");
			if (pat) cfg.patches = strdup(pat);
			/* Legacy single trampoline from env */
			const char* tlib = getenv("MACHISMO_TRAMPOLINE_LIB");
			if (tlib) {
				cfg.trampolines[0].lib = strdup(tlib);
				const char* tpfx = getenv("MACHISMO_TRAMPOLINE_PREFIX");
				cfg.trampolines[0].prefixes[0] = strdup(tpfx ? tpfx : "_SDL_");
				cfg.trampolines[0].num_prefixes = 1;
				cfg.num_trampolines = 1;
			}
		}
	}

	/* Resolve chained fixups — patch GOT with native Linux .so addresses.
	 * The resolver handles MACHO: entries in dylib_map.conf by loading
	 * Mach-O dylibs and looking up symbols in their LC_SYMTAB.
	 * Order: resolve main exe first (which triggers dylib loading),
	 * then resolve each loaded dylib's own fixups. */
	if (machismo_load_results.mh && cfg.dylib_map) {
		resolver_resolve_fixups((void*)machismo_load_results.mh,
		                        machismo_load_results.slide, cfg.dylib_map);

		/* Resolve chained fixups for loaded Mach-O dylibs.
		 * Each dylib has its own LC_DYLD_CHAINED_FIXUPS that need patching.
		 * Dependencies between dylibs are handled because they're all
		 * already loaded — the resolver finds them via MACHO: entries. */
		for (int i = 0; i < g_num_macho_dylibs; i++) {
			struct macho_dylib_info *mdi = &g_macho_dylibs[i];
			fprintf(stderr, "machismo: resolving fixups for Mach-O dylib '%s'\n", mdi->path);
			resolver_resolve_fixups((void*)mdi->mh, mdi->slide, cfg.dylib_map);
		}

		/* Fix macOS pthread signatures in dylib __DATA segments.
		 * Same scan as for the main exe — find 0x32AAABA7 patterns
		 * and reinitialize as PTHREAD_MUTEX_RECURSIVE. */
		for (int i = 0; i < g_num_macho_dylibs; i++) {
			struct macho_dylib_info *mdi = &g_macho_dylibs[i];
			struct mach_header_64 *mh = mdi->mh;
			uint8_t *lcmds = (uint8_t*)(mh + 1);
			uint32_t lp = 0;
			for (uint32_t li = 0; li < mh->ncmds && lp < mh->sizeofcmds; li++) {
				struct segment_command_64 *seg = (struct segment_command_64*)&lcmds[lp];
				if (seg->cmd == LC_SEGMENT_64 &&
				    (seg->initprot & 2) /* writable */) {
					uintptr_t seg_start = seg->vmaddr + mdi->slide;
					uintptr_t seg_end = seg_start + seg->vmsize;
					uint32_t *scan = (uint32_t*)seg_start;
					int fixed = 0;
					while ((uintptr_t)scan + 4 <= seg_end) {
						if (*scan == 0x32AAABA7) {
							pthread_mutexattr_t attr;
							pthread_mutexattr_init(&attr);
							pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
							pthread_mutex_init((pthread_mutex_t*)scan, &attr);
							pthread_mutexattr_destroy(&attr);
							fixed++;
							scan = (uint32_t*)((uint8_t*)scan + sizeof(pthread_mutex_t));
						} else {
							scan++;
						}
					}
					if (fixed > 0)
						fprintf(stderr, "machismo: fixed %d macOS pthread sigs in dylib '%s' segment %s\n",
						        fixed, mdi->path, seg->segname);
				}
				lp += seg->cmdsize;
			}
		}

		/* Run static initializers for loaded Mach-O dylibs.
		 * Must be after fixup resolution (function pointers in
		 * __mod_init_func need to be rebased). */
		for (int i = 0; i < g_num_macho_dylibs; i++) {
			dylib_loader_run_inits(&g_macho_dylibs[i]);
		}
	}

	/* Register Mach-O exception handling frames with system unwinder.
	 * Must be after resolver (GOT entries for personality functions must be patched)
	 * but before any Mach-O code runs (__init_offsets, trampolines, _main). */
	if (machismo_load_results.mh) {
		eh_frame_register_macho((void*)machismo_load_results.mh,
		                        machismo_load_results.slide);
		/* NOTE: Do NOT register eh_frame for loaded Mach-O dylibs yet.
		 * The _dl_find_object interposition uses a single text range —
		 * registering dylibs would overwrite the main exe's range and
		 * break C++ exception handling for the main binary. */
	}

	/* Apply trampolines from config */
	if (machismo_load_results.mh) {
		for (int i = 0; i < cfg.num_trampolines; i++) {
			machismo_trampoline_config_t* tc = &cfg.trampolines[i];
			if (!tc->lib || tc->num_prefixes == 0) continue;

			trampoline_override_t* overrides = NULL;
			int num_overrides = 0;

			/* SDL-specific: capture window + block fullscreen + diagnostics */
			for (int j = 0; j < tc->num_prefixes; j++) {
				if (strcmp(tc->prefixes[j], "_SDL_") == 0) {
					static trampoline_override_t sdl_ov[] = {
						{ "_SDL_CreateWindow", NULL },
						{ "_SDL_SetWindowFullscreen", NULL },
						{ "_SDL_GetWindowSize", NULL },
						{ "_SDL_GL_GetDrawableSize", NULL },
					};
					sdl_ov[0].target = (void*)sdl_create_window_wrapper;
					sdl_ov[1].target = (void*)sdl_set_window_fullscreen_wrapper;
					sdl_ov[2].target = (void*)sdl_get_window_size_wrapper;
					sdl_ov[3].target = (void*)sdl_gl_get_drawable_size_wrapper;
					overrides = sdl_ov;
					num_overrides = 4;
					break;
				}
			}

			/* bgfx-specific: set up init, frame, and diagnostic wrappers */
			if (tc->init_wrapper) {
				void* h = dlopen(tc->lib, RTLD_LAZY | RTLD_GLOBAL);
				if (h) {
					bgfx_shim_set_real_init(dlsym(h, "bgfx_init"));
					bgfx_shim_set_real_frame(dlsym(h, "bgfx_frame"));
					bgfx_shim_set_real_reset(dlsym(h, "bgfx_reset"));
					bgfx_shim_set_real_set_view_rect(dlsym(h, "bgfx_set_view_rect"));
					bgfx_shim_set_real_set_view_transform(dlsym(h, "bgfx_set_view_transform"));
					bgfx_shim_set_real_get_caps(dlsym(h, "bgfx_get_caps"));
					bgfx_shim_set_renderer(tc->renderer);
					static trampoline_override_t bgfx_ov[] = {
						{ "_bgfx_init", NULL },
						{ "__ZN4bgfx4initERKNS_4InitE", NULL },
						{ "_bgfx_frame", NULL },
						{ "__ZN4bgfx5frameEb", NULL },
						{ "_bgfx_reset", NULL },
						{ "__ZN4bgfx5resetEjjjNS_13TextureFormat4EnumE", NULL },
						{ "_bgfx_set_view_rect", NULL },
						{ "__ZN4bgfx11setViewRectEttttt", NULL },
						{ "_bgfx_set_view_transform", NULL },
						{ "__ZN4bgfx16setViewTransformEtPKvS1_", NULL },
						{ "_bgfx_get_caps", NULL },
						{ "__ZN4bgfx7getCapsEv", NULL },
						{ "__ZN4bgfx7Encoder12setTransformEPKvt", NULL },
						{ "__ZN4bgfx7Encoder6submitEtNS_13ProgramHandleEjh", NULL },
					};
					bgfx_ov[0].target = (void*)bgfx_init_wrapper;
					bgfx_ov[1].target = (void*)bgfx_init_wrapper;
					bgfx_ov[2].target = (void*)bgfx_frame_wrapper;
					bgfx_ov[3].target = (void*)bgfx_frame_wrapper;
					bgfx_ov[4].target = (void*)bgfx_reset_wrapper;
					bgfx_ov[5].target = (void*)bgfx_reset_wrapper;
					bgfx_ov[6].target = (void*)bgfx_set_view_rect_wrapper;
					bgfx_ov[7].target = (void*)bgfx_set_view_rect_wrapper;
					bgfx_ov[8].target = (void*)bgfx_set_view_transform_wrapper;
					bgfx_ov[9].target = (void*)bgfx_set_view_transform_wrapper;
					bgfx_ov[10].target = (void*)bgfx_get_caps_wrapper;
					bgfx_ov[11].target = (void*)bgfx_get_caps_wrapper;
					bgfx_shim_set_real_encoder_set_transform(
						dlsym(h, "_ZN4bgfx7Encoder12setTransformEPKvt"));
					bgfx_ov[12].target = (void*)bgfx_encoder_set_transform_wrapper;
					bgfx_shim_set_real_encoder_submit(
						dlsym(h, "_ZN4bgfx7Encoder6submitEtNS_13ProgramHandleEjh"));
					bgfx_ov[13].target = (void*)bgfx_encoder_submit_wrapper;
					overrides = bgfx_ov;
					num_overrides = 14;
				} else {
					fprintf(stderr, "machismo: warning: cannot load %s: %s\n",
					        tc->lib, dlerror());
				}
			}

			trampoline_patch_lib((void*)machismo_load_results.mh,
			                     machismo_load_results.slide,
			                     tc->lib,
			                     (const char**)tc->prefixes,
			                     tc->num_prefixes,
			                     overrides, num_overrides);
		}

		/* Guard __DATA pages against stale library state access.
		 * Collect all non-STUB prefixes for the data guard. */
		const char* guard_prefixes[64];
		int num_guard_prefixes = 0;
		for (int i = 0; i < cfg.num_trampolines; i++) {
			machismo_trampoline_config_t* tc2 = &cfg.trampolines[i];
			if (!tc2->lib || strcmp(tc2->lib, "STUB") == 0) continue;
			for (int j = 0; j < tc2->num_prefixes; j++) {
				if (num_guard_prefixes < 64)
					guard_prefixes[num_guard_prefixes++] = tc2->prefixes[j];
			}
		}
		if (num_guard_prefixes > 0) {
			trampoline_guard_stale_data((void*)machismo_load_results.mh,
			                            machismo_load_results.slide,
			                            guard_prefixes, num_guard_prefixes);
		}
	}

	/* Register Mach-O symbols with GDB for backtraces */
	if (machismo_load_results.mh) {
		gdb_jit_register_macho((void*)machismo_load_results.mh,
		                       machismo_load_results.slide);
	}

	/* Apply game-specific binary patches */
	if (cfg.patches) {
		patcher_apply(cfg.patches, machismo_load_results.slide);
	}

	/* Set up the Mach-O stack layout */
	setup_stack64(filename, &machismo_load_results);

	__machismo_main_stack_top = (void*)machismo_load_results.stack_top;

	start_thread(&machismo_load_results);

	__builtin_unreachable();
}

void load(const char* path, cpu_type_t forced_arch, bool expect_dylinker, char** argv, struct load_results* lr)
{
	int fd;
	uint32_t magic;

	fd = open(path, O_RDONLY);
	if (fd == -1)
	{
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		exit(1);
	}

	if (read(fd, &magic, sizeof(magic)) != sizeof(magic))
	{
		fprintf(stderr, "Cannot read the file header of %s.\n", path);
		exit(1);
	}

	if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64)
	{
		lseek(fd, 0, SEEK_SET);
		load64(fd, expect_dylinker, lr);
	}
	else if (magic == MH_MAGIC || magic == MH_CIGAM)
	{
		fprintf(stderr, "32-bit Mach-O not supported on aarch64: %s\n", path);
		exit(1);
	}
	else if (magic == FAT_MAGIC || magic == FAT_CIGAM)
	{
		lseek(fd, 0, SEEK_SET);
		load_fat(fd, forced_arch, expect_dylinker, argv, lr);
	}
	else
	{
		fprintf(stderr, "Unknown file format: %s (magic: 0x%08x)\n", path, magic);
		exit(1);
	}

	close(fd);
}

static void load_fat(int fd, cpu_type_t forced_arch, bool expect_dylinker, char** argv, struct load_results* lr) {
	struct fat_header fhdr;
	struct fat_arch best_arch = {0};

	best_arch.cputype = CPU_TYPE_ANY;

	if (read(fd, &fhdr, sizeof(fhdr)) != sizeof(fhdr))
	{
		fprintf(stderr, "Cannot read fat file header.\n");
		exit(1);
	}

	const bool swap = fhdr.magic == FAT_CIGAM;

#define SWAP32(x) x = __bswap_32(x)

	if (swap)
		SWAP32(fhdr.nfat_arch);

	uint32_t i;
	for (i = 0; i < fhdr.nfat_arch; i++)
	{
		struct fat_arch arch;

		if (read(fd, &arch, sizeof(arch)) != sizeof(arch))
		{
			fprintf(stderr, "Cannot read fat_arch header.\n");
			exit(1);
		}

		if (swap)
		{
			SWAP32(arch.cputype);
			SWAP32(arch.cpusubtype);
			SWAP32(arch.offset);
			SWAP32(arch.size);
			SWAP32(arch.align);
		}

		if (!forced_arch)
		{
			/* On aarch64, prefer ARM64 slice */
			if (arch.cputype == CPU_TYPE_ARM64)
				best_arch = arch;
		}
		else
		{
			if (arch.cputype == forced_arch)
				best_arch = arch;
		}
	}

	if (best_arch.cputype == CPU_TYPE_ANY)
	{
		fprintf(stderr, "No arm64 architecture found in fat binary.\n");
		exit(1);
	}

	if (lseek(fd, best_arch.offset, SEEK_SET) == -1)
	{
		fprintf(stderr, "Cannot seek to selected arch in fat binary.\n");
		exit(1);
	}

	if (best_arch.cputype & CPU_ARCH_ABI64) {
		load64(fd, expect_dylinker, lr);
	} else {
		fprintf(stderr, "32-bit slices not supported on aarch64.\n");
		exit(1);
	}
}

/* Include the 64-bit loader template */
#define GEN_64BIT
#include "loader.c"
#include "stack.c"
#undef GEN_64BIT

int native_prot(int prot)
{
	int protOut = 0;

	if (prot & VM_PROT_READ)
		protOut |= PROT_READ;
	if (prot & VM_PROT_WRITE)
		protOut |= PROT_WRITE;
	if (prot & VM_PROT_EXECUTE)
		protOut |= PROT_EXEC;

	return protOut;
}

static void setup_space(struct load_results* lr, bool is_64_bit) {
	commpage_setup(is_64_bit);

	lr->stack_top = commpage_address(true);

	struct rlimit limit;
	getrlimit(RLIMIT_STACK, &limit);
	unsigned long size = PAGE_SIZE * 16;
	if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < size) {
		size = limit.rlim_cur;
	}

	if (compatible_mmap((void*)(lr->stack_top - size), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_GROWSDOWN, -1, 0) == MAP_FAILED) {
		/* Try without MAP_FIXED_NOREPLACE */
		if (compatible_mmap((void*)(lr->stack_top - size), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_GROWSDOWN, -1, 0) == MAP_FAILED) {
			fprintf(stderr, "Failed to allocate stack of %lu bytes: %d (%s)\n", size, errno, strerror(errno));
			exit(1);
		}
	}

	lr->kernfd = -1;
}

/*
 * Fix macOS pthread objects in the Mach-O binary.
 *
 * macOS statically initializes pthread_mutex_t with signature 0x32AAABA7
 * at offset 0. Linux PTHREAD_MUTEX_INITIALIZER is all zeros. Since native
 * Linux libraries (libc++, etc.) call glibc's pthread_mutex_lock directly
 * through their own ELF PLT/GOT, we can't intercept at the function level.
 * We must fix the data in-place before execution.
 *
 * Strategy: use LC_SYMTAB to find symbols in __DATA whose names contain
 * "mutex", "Mutex", "cond", "rwlock", or "once". Check for macOS pthread
 * signatures at those addresses and zero-initialize them for Linux.
 */

#define DARWIN_MUTEX_SIG      0x32AAABA7L
#define DARWIN_RMUTEX_SIG     0x32AAABA1L
#define DARWIN_COND_SIG       0x3CB0B1BBL
#define DARWIN_RWLOCK_SIG     0x2DA8B3B4L
#define DARWIN_ONCE_SIG       0x30B1BCBAL

/* Try to fix a macOS pthread signature at the given address.
 * Returns 1 if fixed, 0 if no macOS signature found. */
static int try_fixup_pthread_at(uintptr_t addr) {
	long *sig = (long *)addr;
	switch (*sig) {
	case DARWIN_MUTEX_SIG:
	case DARWIN_RMUTEX_SIG: {
		/* Initialize as recursive mutex for macOS compatibility.
		 * macOS game code may re-lock from the same thread. */
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		memset(sig, 0, 64);
		pthread_mutex_init((pthread_mutex_t*)sig, &attr);
		pthread_mutexattr_destroy(&attr);
		return 1;
	}
	case DARWIN_COND_SIG:
		memset(sig, 0, 48);
		pthread_cond_init((pthread_cond_t*)sig, NULL);
		return 1;
	case DARWIN_RWLOCK_SIG:
		memset(sig, 0, 64);
		pthread_rwlock_init((pthread_rwlock_t*)sig, NULL);
		return 1;
	case DARWIN_ONCE_SIG:
		memset(sig, 0, 16);
		return 1;
	}
	return 0;
}

/* Check if a symbol name looks like a pthread object */
static int name_is_pthread_like(const char *name) {
	/* Check for common substrings in mangled/demangled names */
	if (!name) return 0;
	/* C++ std::mutex, std::recursive_mutex, etc. */
	if (strstr(name, "mutex") || strstr(name, "Mutex")) return 1;
	/* pthread_cond, condition_variable */
	if (strstr(name, "cond") || strstr(name, "Cond")) return 1;
	/* rwlock */
	if (strstr(name, "rwlock") || strstr(name, "RWLock") || strstr(name, "Rwlock")) return 1;
	/* C++ guard variables for static locals (often guard mutex-containing objects) */
	if (strstr(name, "_once") || strstr(name, "Once")) return 1;
	/* lock objects */
	if (strstr(name, "_lock") || strstr(name, "Lock")) return 1;
	return 0;
}

/* Convert a file offset to memory address using segment mappings */
static void* fixup_fileoff_to_mem(struct mach_header_64* mh, uintptr_t slide, uint32_t fileoff)
{
	uint8_t* cmd_ptr = (uint8_t*)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (fileoff >= seg->fileoff && fileoff < seg->fileoff + seg->filesize) {
				return (void*)(seg->vmaddr + slide + (fileoff - seg->fileoff));
			}
		}
		cmd_ptr += lc->cmdsize;
	}
	return NULL;
}

static void fixup_darwin_pthread_data(struct load_results* lr) {
	struct mach_header_64* mh = (struct mach_header_64*)lr->mh;
	uint8_t* cmds = (uint8_t*)(mh + 1);
	uint32_t p = 0;
	int fixed = 0;

	/* Scan all writable DATA segments for macOS pthread signatures.
	 * The signatures (0x32AAABA7 etc.) are stored as 8-byte-aligned longs
	 * and are distinctive enough that false positives are negligible. */
	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command* lc = (struct load_command*)&cmds[p];
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (seg->initprot & 0x02) { /* VM_PROT_WRITE */
				uintptr_t base = seg->vmaddr + lr->slide;
				/* Only scan the file-backed portion (not BSS) */
				size_t scan_size = seg->filesize;
				for (size_t off = 0; off + sizeof(long) <= scan_size; off += sizeof(long)) {
					if (try_fixup_pthread_at(base + off))
						fixed++;
				}
			}
		}
		p += lc->cmdsize;
	}

	if (fixed > 0)
		fprintf(stderr, "machismo: fixed %d macOS pthread objects in __DATA\n", fixed);
}

/*
 * Set up Mach-O thread-local variable (TLV) image info.
 * The __DATA,__thread_data section contains initial values for TLVs,
 * and __DATA,__thread_bss is the zero-initialized portion.
 * These are needed by _tlv_bootstrap() in libsystem_shim.so.
 */
static void setup_tlv_image(struct load_results* lr)
{
	struct mach_header_64* mh = (struct mach_header_64*)lr->mh;
	uint8_t* cmds = (uint8_t*)(mh + 1);
	uint32_t p = 0;

	void* tlv_image_base = NULL;
	size_t tlv_image_size = 0;
	size_t tlv_bss_size = 0;

	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command* lc = (struct load_command*)&cmds[p];
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			struct section_64* sect = (struct section_64*)(seg + 1);
			for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
				/* S_THREAD_LOCAL_REGULAR = 0x11 */
				if ((sect->flags & 0xff) == 0x11) {
					tlv_image_base = (void*)(sect->addr + lr->slide);
					tlv_image_size = sect->size;
				}
				/* S_THREAD_LOCAL_ZEROFILL = 0x12 */
				if ((sect->flags & 0xff) == 0x12) {
					tlv_bss_size = sect->size;
				}
			}
		}
		p += lc->cmdsize;
	}

	if (tlv_image_size || tlv_bss_size) {
		fprintf(stderr, "machismo: TLV image at %p, size %zu + %zu bss\n",
		        tlv_image_base, tlv_image_size, tlv_bss_size);
	}

	/* Set globals in libsystem_shim.so — try multiple paths since
	 * the resolver may have loaded it with a relative or absolute path */
	const char* shim_paths[] = {
		"./libsystem_shim.so", "libsystem_shim.so",
		NULL
	};
	void* shim = NULL;
	for (int i = 0; shim_paths[i]; i++) {
		shim = dlopen(shim_paths[i], RTLD_NOLOAD | RTLD_LAZY);
		if (shim) break;
	}
	if (shim) {
		void** p_base = (void**)dlsym(shim, "__tlv_image_base");
		size_t* p_size = (size_t*)dlsym(shim, "__tlv_image_size");
		size_t* p_bss = (size_t*)dlsym(shim, "__tlv_bss_size");
		if (p_base) *p_base = tlv_image_base;
		if (p_size) *p_size = tlv_image_size;
		if (p_bss) *p_bss = tlv_bss_size;
		dlclose(shim);
		fprintf(stderr, "machismo: TLV info set in shim\n");
	} else {
		fprintf(stderr, "machismo: WARNING: cannot find libsystem_shim.so for TLV (%s)\n",
		        dlerror());
	}
}

/*
 * Run C++ static initializers from __init_offsets section.
 * This section contains 32-bit relative offsets from the Mach-O base
 * to void(void) initializer functions. On macOS, dyld runs these before main().
 */
static void run_init_offsets(struct load_results* lr) {
	struct mach_header_64* mh = (struct mach_header_64*)lr->mh;
	uint8_t* cmds = (uint8_t*)(mh + 1);
	uint32_t p = 0;

	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command* lc = (struct load_command*)&cmds[p];
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			struct section_64* sect = (struct section_64*)(seg + 1);
			for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
				/* S_INIT_FUNC_OFFSETS = 0x16 */
				if ((sect->flags & 0xff) == 0x16) {
					uint32_t* offsets = (uint32_t*)(lr->mh + sect->addr - seg->vmaddr + sect->offset
							+ (lr->slide - 0) /* offset within mapped segment */);
					/* offsets are at the mapped section address */
					offsets = (uint32_t*)(sect->addr + lr->slide);
					int count = sect->size / sizeof(uint32_t);
					fprintf(stderr, "machismo: running %d C++ static initializers from __init_offsets\n", count);
					typedef void (*init_func_t)(void);
					for (int j = 0; j < count; j++) {
						uintptr_t func_addr = lr->mh + offsets[j];
						init_func_t func = (init_func_t)func_addr;
						fprintf(stderr, "machismo: init[%d/%d] at 0x%lx\n", j, count, func_addr);
						func();
					}
					fprintf(stderr, "machismo: static initializers complete\n");
				}
			}
		}
		p += lc->cmdsize;
	}
}

static void start_thread(struct load_results* lr) {
	if (lr->lc_main) {
		/* LC_MAIN: entry point is main(argc, argv, envp, applep).
		 * Call it as a C function with args in registers. */
		typedef int (*main_func_t)(int, char**, char**, char**);
		main_func_t entry = (main_func_t)lr->entry_point;

		/* Fix macOS pthread objects before any Mach-O code runs */
		fixup_darwin_pthread_data(lr);

		/* Set up TLV image info for _tlv_bootstrap */
		setup_tlv_image(lr);

		/* Run C++ static initializers before main */
		run_init_offsets(lr);

		/* chdir to the binary's directory so the game finds its data files */
		{
			char* binary_path = strdup(lr->argv[0] ? lr->argv[0] : "");
			char* slash = strrchr(binary_path, '/');
			if (slash) {
				*slash = '\0';
				if (chdir(binary_path) == 0)
					fprintf(stderr, "machismo: chdir to '%s'\n", binary_path);
			}
			free(binary_path);
		}

		fprintf(stderr, "machismo: calling _main at %p (argc=%zu)\n",
				(void*)lr->entry_point, lr->argc);

		int ret = entry((int)lr->argc, lr->argv, lr->envp, lr->applep);
		_exit(ret);
	}

	/* LC_UNIXTHREAD: set up stack and jump directly */
#ifdef __aarch64__
	__asm__ volatile(
		"mov x29, #0\n"
		"mov x30, #0\n"
		"mov sp, %1\n"
		"br %0"
		::
		"r"(lr->entry_point),
		"r"(lr->stack_top)
		:
	);
#elif defined(__x86_64__)
	__asm__ volatile(
		"mov %1, %%rsp\n"
		"jmpq *%0"
		::
		"m"(lr->entry_point),
		"r"(lr->stack_top)
		:
	);
#else
#       error Unsupported platform!
#endif
}

static bool is_kernel_at_least(int major, int minor) {
	if (kernel_major == -1) {
		struct utsname uname_info;
		if (uname(&uname_info) == -1) {
			return false;
		}
		kernel_major = 0;
		kernel_minor = 0;
		size_t pos = 0;
		while (uname_info.release[pos] != '\0' && uname_info.release[pos] != '.') {
			kernel_major = kernel_major * 10 + uname_info.release[pos] - '0';
			++pos;
		}
		++pos;
		while (uname_info.release[pos] != '\0' && uname_info.release[pos] != '.') {
			kernel_minor = kernel_minor * 10 + uname_info.release[pos] - '0';
			++pos;
		}
	}

	if (major != kernel_major) {
		return kernel_major > major;
	}

	return kernel_minor >= minor;
}

void* compatible_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	bool fixed_noreplace_hack = false;
	if ((flags & MAP_FIXED_NOREPLACE) && !is_kernel_at_least(4, 17)) {
		flags &= ~MAP_FIXED_NOREPLACE;
		fixed_noreplace_hack = true;
	}
	void* result = mmap(addr, length, prot, flags, fd, offset);
	if ((result == (void*)MAP_FAILED) && (flags & MAP_GROWSDOWN) && (errno == EOPNOTSUPP)) {
		result = mmap(addr, length, prot, (flags & ~MAP_GROWSDOWN), fd, offset);
	}
	if (fixed_noreplace_hack) {
		if (result != addr && result != (void*)MAP_FAILED) {
			errno = ESRCH;
			munmap(addr, length);
			return MAP_FAILED;
		}
	}
	return result;
}
