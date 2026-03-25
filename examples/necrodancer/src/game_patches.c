/*
 * NecroDancer game function replacements.
 *
 * Compiled as libgame_patches.so — loaded by machismo's override trampoline.
 * Each exported function replaces the Mach-O function with the same mangled name.
 *
 * Build: linked into necrodancer_patches target in CMakeLists.txt
 * Config: [trampoline.game] override_lib = ./libgame_patches.so
 */

#define _GNU_SOURCE
#include "stubs/apple_abi.h"
#include "stubs/datastream.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

/*
 * DataStream::preloadFile() — mmap replacement.
 *
 * Original (decompiled at 0x1001dfaac):
 *   1. malloc(file_size) via operator new
 *   2. fread(buf, file_size, 1, fp)
 *   3. fclose(fp)
 *
 * Replacement:
 *   1. mmap(fd, PROT_READ, MAP_PRIVATE) — kernel pages in on demand
 *   2. Register with mmap_registry so free() → munmap()
 *   3. fclose(fp)
 *
 * On a 1GB handheld, this saves ~42 MB of heap and lets the kernel
 * reclaim pages under memory pressure (re-faults from the file).
 */
MACHO_FUNC(_ZN10DataStream11preloadFileEv, uint64_t, void *self)
{
	FILE *fp = DS_FILE_PTR(self);
	if (!fp)
		return 1;

	uint64_t size = DS_FILE_SIZE(self);
	int fd = fileno(fp);

	void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped == MAP_FAILED) {
		fprintf(stderr, "game_patches: mmap failed for preloadFile (%llu bytes)\n",
		        (unsigned long long)size);
		fclose(fp);
		DS_FILE_PTR(self) = NULL;
		DS_FLAGS(self) = 0;
		return 0;
	}

	fprintf(stderr, "game_patches: preloadFile: mmap'd %llu bytes at %p (fd=%d)\n",
	        (unsigned long long)size, mapped, fd);

	/* Register with shim's mmap registry so free() calls munmap() */
	typedef void (*registry_fn)(void *, size_t);
	static registry_fn reg = NULL;
	if (!reg)
		reg = (registry_fn)dlsym(RTLD_DEFAULT, "mmap_registry_add");
	if (reg)
		reg(mapped, (size_t)size);

	/* Free any existing buffer (goes through shim_free → real_free or munmap) */
	char *old = DS_BUF_START(self);
	if (old)
		free(old);

	/* Set up the vector pointers to the mmap'd region */
	DS_BUF_START(self) = (char *)mapped;
	DS_BUF_END(self)   = (char *)mapped + size;
	DS_BUF_CAP(self)   = (char *)mapped + size;

	fclose(fp);
	DS_FILE_PTR(self) = NULL;
	DS_FLAGS(self) = 0;

	return 1;
}

/* ====================================================================
 * Lazy audio loading — addStreamingSource + play() replacements.
 *
 * The game eagerly loads all ~1,550 audio files into memory (~290 MB).
 * We defer loading until play() is called, so idle sources use ~360 bytes.
 * Level transitions call stopAll() which destroys everything naturally.
 *
 * Lazy sentinel: path string at +0x30, NULL at +0x38.
 * After lazy load: +0x30 = NULL, Music at +0x48 is initialized.
 * ==================================================================== */

/* ====================================================================
 * Early init: patch SFMLErrorHandler::overflow before any SFML errors.
 *
 * The game's SFMLErrorHandler::overflow() calls an unresolved symbol
 * and crashes whenever SFML tries to print an error message. Patch it
 * to a safe no-op at .so load time.
 * ==================================================================== */

/* Cached function pointers (resolved once on first use) */
static uintptr_t streaming_vtable = 0;

typedef void (*streaming_ctor_fn_t)(void *self, void *vec);
static streaming_ctor_fn_t streaming_ctor_fn = NULL;

typedef uint32_t (*add_source_fn_t)(void *self, void *unique_ptr);
static add_source_fn_t add_source_fn = NULL;

typedef uint8_t (*music_open_fn_t)(void *music, void *path_str);
static music_open_fn_t music_openFromFile_fn = NULL;

static int audio_patches_initialized = 0;

/*
 * Resolve machismo symbols at runtime via dlsym (machismo is the host
 * executable — these aren't in a shared library we can link against).
 */
typedef uintptr_t (*lookup_name_fn_t)(const char *);
typedef void *(*dylib_find_fn_t)(const char *);
typedef uintptr_t (*dylib_lookup_fn_t)(void *, const char *);

static void init_audio_patches(void)
{
	if (audio_patches_initialized)
		return;
	audio_patches_initialized = 1;

	/* Resolve machismo helper functions */
	lookup_name_fn_t gdb_jit_lookup_name =
		(lookup_name_fn_t)dlsym(RTLD_DEFAULT, "gdb_jit_lookup_name");
	dylib_find_fn_t dylib_loader_find =
		(dylib_find_fn_t)dlsym(RTLD_DEFAULT, "dylib_loader_find");
	dylib_lookup_fn_t dylib_loader_lookup =
		(dylib_lookup_fn_t)dlsym(RTLD_DEFAULT, "dylib_loader_lookup");

	if (!gdb_jit_lookup_name) {
		fprintf(stderr, "game_patches: WARNING: gdb_jit_lookup_name not available\n");
		return;
	}

	/* Look up StreamingAudioSource constructor in game binary */
	uintptr_t ctor = gdb_jit_lookup_name(
		"_ZN3wos3sfx20StreamingAudioSourceC1ENSt3__16vectorIcNS2_9allocatorIcEEEE");
	if (ctor)
		streaming_ctor_fn = (streaming_ctor_fn_t)ctor;
	else
		fprintf(stderr, "game_patches: WARNING: StreamingAudioSource ctor not found\n");

	/* Look up AudioPlayer::addSource in game binary */
	uintptr_t as = gdb_jit_lookup_name(
		"_ZN3wos3sfx11AudioPlayer9addSourceENSt3__110unique_ptrINS0_19AbstractAudioSourceENS2_14default_deleteIS4_EEEE");
	if (as)
		add_source_fn = (add_source_fn_t)as;
	else
		fprintf(stderr, "game_patches: WARNING: AudioPlayer::addSource not found\n");

	/* Look up sf::Music::openFromFile in SFML audio Mach-O dylib */
	if (dylib_loader_find && dylib_loader_lookup) {
		void *sfml = dylib_loader_find("sfml-audio");
		if (sfml) {
			uintptr_t ofn = dylib_loader_lookup(sfml,
				"__ZN2sf5Music12openFromFileERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE");
			if (ofn)
				music_openFromFile_fn = (music_open_fn_t)ofn;
			else
				fprintf(stderr, "game_patches: WARNING: sf::Music::openFromFile not found\n");
		} else {
			fprintf(stderr, "game_patches: WARNING: sfml-audio dylib not found\n");
		}
	}

	/* Patch SFMLErrorHandler::overflow to a safe no-op.
	 * The original calls an unresolved symbol and crashes when SFML
	 * tries to print error messages (e.g., when openFromMemory fails). */
	uintptr_t overflow_addr = gdb_jit_lookup_name(
		"_ZN16SFMLErrorHandler8overflowEi");
	if (overflow_addr) {
		/* Two-step W^X: remove exec first, write, then restore exec. */
		long pgsz = sysconf(_SC_PAGESIZE);
		if (pgsz <= 0) pgsz = 16384;  /* Apple Silicon default */
		uintptr_t page = overflow_addr & ~(uintptr_t)(pgsz - 1);
		if (mprotect((void *)page, pgsz, PROT_READ | PROT_WRITE) != 0) {
			fprintf(stderr, "game_patches: mprotect RW failed for overflow: %s\n",
			        strerror(errno));
		} else {
			volatile uint32_t *code = (volatile uint32_t *)overflow_addr;
			code[0] = 0x2a0103e0;  /* mov w0, w1 */
			code[1] = 0xd65f03c0;  /* ret */
			mprotect((void *)page, pgsz, PROT_READ | PROT_EXEC);
			__builtin___clear_cache((char *)overflow_addr,
			                        (char *)(overflow_addr + 8));
			fprintf(stderr, "game_patches: patched SFMLErrorHandler::overflow at %p\n",
			        (void *)overflow_addr);
		}
	}

	fprintf(stderr, "game_patches: audio patches init: ctor=%p addSource=%p openFromFile=%p\n",
	        (void *)streaming_ctor_fn, (void *)add_source_fn,
	        (void *)music_openFromFile_fn);
}

/*
 * Resolve a game resource name to an on-disk file path.
 *
 * Resource names come in as "ext/<subdir>/<file>.ogg" from the game's
 * resource system. The "ext/" prefix maps to the assets depot's data/
 * directory. We search for a matching file by:
 *   1. Using data_base_dir + name with ext/ stripped (if data_base_dir set)
 *   2. Trying the name as a relative path from CWD
 *   3. Trying common relative layouts (data/<rest>)
 */
static char data_base_dir[512] = {0};

static void init_data_base_dir(void)
{
	if (data_base_dir[0])
		return;

	/* Try NECRODANCER_DATA env var first */
	const char *env = getenv("NECRODANCER_DATA");
	if (env) {
		snprintf(data_base_dir, sizeof(data_base_dir), "%s", env);
		return;
	}

	/* Derive from CWD: game chdir's to .../MacOS/,
	 * assets are at .../depot_247082/.../Resources/data/
	 * Try common relative paths. */
	char cwd[512];
	if (getcwd(cwd, sizeof(cwd))) {
		/* Try: ../../../../depot_247082/NecroDancer.app/Contents/Resources/data */
		snprintf(data_base_dir, sizeof(data_base_dir),
			"%s/../../../../depot_247082/NecroDancer.app/Contents/Resources/data", cwd);
		if (access(data_base_dir, F_OK) == 0)
			return;

		/* Try: data/ relative to CWD (port layout) */
		snprintf(data_base_dir, sizeof(data_base_dir), "%s/data", cwd);
		if (access(data_base_dir, F_OK) == 0)
			return;

		/* Try: ../Resources/data (single app bundle) */
		snprintf(data_base_dir, sizeof(data_base_dir),
			"%s/../Resources/data", cwd);
		if (access(data_base_dir, F_OK) == 0)
			return;
	}

	/* Fallback: just use "data" relative to CWD */
	snprintf(data_base_dir, sizeof(data_base_dir), "data");
}

static int resolve_audio_path(const char *name, char *out, size_t outlen)
{
	init_data_base_dir();

	/* Strip "ext/" prefix if present */
	const char *rel = name;
	if (strncmp(rel, "ext/", 4) == 0)
		rel = name + 4;

	/* Try: data_base_dir/<rel> */
	snprintf(out, outlen, "%s/%s", data_base_dir, rel);
	if (access(out, F_OK) == 0) return 1;

	/* Try: name as-is (relative to CWD) */
	snprintf(out, outlen, "%s", name);
	if (access(out, F_OK) == 0) return 1;

	return 0;
}

/*
 * AudioPlayer::addStreamingSource — openFromFile replacement.
 *
 * Instead of loading the OGG file into a heap buffer and calling
 * openFromMemory, we construct the StreamingAudioSource with a tiny
 * dummy buffer (so the constructor initializes all infrastructure),
 * then immediately call sf::Music::openFromFile to stream from disk.
 *
 * This eliminates ~126 MB of in-heap OGG compressed data.
 * SFML streams directly from disk, only allocating its decode buffer.
 */
MACHO_FUNC(
	_ZN3wos3sfx11AudioPlayer18addStreamingSourceENSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE,
	uint32_t, void *self, void *name_str)
{
	init_audio_patches();

	if (!streaming_ctor_fn || !add_source_fn || !music_openFromFile_fn) {
		fprintf(stderr, "game_patches: addStreamingSource: missing symbols, skipping\n");
		return 0;
	}

	const char *name = apple_string_cstr((apple_string_t *)name_str);

	/* Resolve resource name → file path */
	char path[512];
	if (!resolve_audio_path(name, path, sizeof(path))) {
		fprintf(stderr, "game_patches: addStreamingSource: not found: %s\n", name);
		return 0;
	}

	/* Construct StreamingAudioSource with a tiny dummy buffer.
	 * openFromMemory(dummy, 4) will fail to parse as audio, but all
	 * object infrastructure (vtables, Music, Mutex) gets initialized. */
	void *src = calloc(1, 0x168);
	if (!src)
		return 0;

	char *dummy = (char *)malloc(4);
	memset(dummy, 0, 4);
	struct { char *data; char *end; char *cap; } vec = {
		dummy, dummy + 4, dummy + 4
	};
	streaming_ctor_fn(src, &vec);

	/* Free the dummy buffer that the constructor moved to +0x30 */
	char *moved_dummy = *(char **)((char *)src + 0x30);
	if (moved_dummy)
		free(moved_dummy);
	*(char **)((char *)src + 0x30) = NULL;
	*(char **)((char *)src + 0x38) = NULL;
	*(char **)((char *)src + 0x40) = NULL;

	/* Now open the real file — SFML streams from disk, no heap buffer */
	void *music = (char *)src + 0x48;
	apple_string_t path_str;
	path_str.data = path;
	path_str.size = strlen(path);
	path_str.cap  = path_str.size | ((uint64_t)1 << 63);

	uint8_t ok = music_openFromFile_fn(music, &path_str);
	*(uint8_t *)((char *)src + 0x148) = ok;

	if (!ok) {
		fprintf(stderr, "game_patches: addStreamingSource: openFromFile failed: %s\n", path);
		/* Destroy and return 0 */
		typedef void (*dtor_fn_t)(void *);
		dtor_fn_t dtor = (dtor_fn_t)(*(uintptr_t *)(*(uintptr_t *)src + 8));
		dtor(src);
		free(src);
		return 0;
	}

	/* Register with AudioPlayer */
	void *uptr = src;
	uint32_t id = add_source_fn(self, &uptr);

	/* If addSource didn't consume the unique_ptr, destroy the source */
	if (uptr) {
		typedef void (*dtor_fn_t)(void *);
		dtor_fn_t dtor = (dtor_fn_t)(*(uintptr_t *)(*(uintptr_t *)uptr + 8));
		dtor(uptr);
	}

	return id;
}

/*
 * AbstractSFMLAudioSource::play() — lazy load trigger.
 *
 * If the source is a lazy StreamingAudioSource (path at +0x30, NULL at +0x38),
 * load the audio file via sf::Music::openFromFile before playing.
 * Then re-implement the original 3-instruction play():
 *   getSource() [vtable offset 200] → sf::SoundStream::play() [vtable offset 0x10]
 */
#if 0  /* temporarily disabled to isolate crash */
MACHO_FUNC(
	_ZN3wos3sfx23AbstractSFMLAudioSource4playEv,
	void, void *self)
{
	/* Check: is this a lazy StreamingAudioSource? */
	if (streaming_vtable && *(uintptr_t *)self == streaming_vtable) {
		char *path = *(char **)((char *)self + 0x30);
		char *end  = *(char **)((char *)self + 0x38);

		if (path && !end) {
			/* Lazy load: call sf::Music::openFromFile on Music at +0x48 */
			void *music = (char *)self + 0x48;

			apple_string_t path_str;
			path_str.data = path;
			path_str.size = strlen(path);
			path_str.cap  = path_str.size | ((uint64_t)1 << 63);

			uint8_t ok = music_openFromFile_fn(music, &path_str);

			if (ok) {
				fprintf(stderr, "game_patches: lazy load: %s\n", path);
			} else {
				fprintf(stderr, "game_patches: lazy load FAILED: %s\n", path);
			}

			free(path);
			*(char **)((char *)self + 0x30) = NULL;
			*(char **)((char *)self + 0x38) = NULL;
		}
	}

	/* Re-implement original play():
	 *   void *source = getSource();      // vtable[25] (offset 200)
	 *   source->play();                  // source vtable[2] (offset 0x10)
	 */
	typedef void *(*vmethod_t)(void *);
	uintptr_t vtable = *(uintptr_t *)self;
	vmethod_t getSource = *(vmethod_t *)(vtable + 200);

	fprintf(stderr, "game_patches: play() self=%p vtable=%p getSource=%p\n",
	        self, (void *)vtable, (void *)getSource);

	void *source = getSource(self);

	fprintf(stderr, "game_patches: play() source=%p source_vtable=%p\n",
	        source, (void *)(*(uintptr_t *)source));

	vmethod_t play_fn = *(vmethod_t *)(*(uintptr_t *)source + 0x10);
	fprintf(stderr, "game_patches: play() play_fn=%p\n", (void *)play_fn);

	play_fn(source);
	fprintf(stderr, "game_patches: play() done\n");
}
#endif /* temporarily disabled */
