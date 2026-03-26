/*
 * Trampoline patcher for statically-linked functions in Mach-O binaries.
 *
 * Overwrites function entry points with 16-byte arm64 trampolines
 * that jump to native Linux .so implementations.
 *
 * Trampoline (16 bytes):
 *   ldr x16, #8       // load target address from literal pool
 *   br  x16            // branch to native function
 *   .quad <target>     // 8-byte native function address
 *
 * x16 (IP0) is the intra-procedure-call scratch register on arm64,
 * safe to clobber at function entry.
 */

#include "trampoline.h"
#include "macho_defs.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

/* arm64 trampoline island approach:
 *
 * Problem: inline 16-byte trampolines overflow into adjacent functions
 * when the patched function is smaller than 16 bytes (73 bgfx functions).
 *
 * Solution: write only a 4-byte B (branch) instruction at the function
 * entry, targeting a 16-byte island in a nearby executable pool.
 * The island contains the full indirect branch: ldr x16, #8; br x16; .quad addr
 *
 * B instruction range: ±128MB — pool must be within 128MB of __TEXT.
 */

#define TRAMPOLINE_LDR_X16  0x58000050  /* ldr x16, #8 */
#define TRAMPOLINE_BR_X16   0xd61f0200  /* br x16 */
#define ISLAND_SIZE         16

struct trampoline_island {
	uint32_t ldr_x16;      /* ldr x16, #8 */
	uint32_t br_x16;       /* br x16 */
	uint64_t target_addr;  /* native function address */
};

/* Island pool — executable memory near __TEXT */
#define ISLAND_POOL_SIZE (1024 * 1024)  /* 1MB, enough for ~65K islands */
static uint8_t* island_pool = NULL;
static size_t island_pool_size = 0;
static size_t island_pool_used = 0;

void trampoline_set_pool(void* base, size_t size)
{
	island_pool = (uint8_t*)base;
	island_pool_size = size;
	island_pool_used = 0;
	fprintf(stderr, "trampoline: island pool at %p (%zu KB)\n",
	        base, size / 1024);
}

static int init_island_pool(uintptr_t text_base, size_t text_size)
{
	if (island_pool) return 0;  /* already set via trampoline_set_pool */

	/* Fallback: try to mmap near __TEXT (works on kernels >= 4.17
	 * that support MAP_FIXED_NOREPLACE; on older kernels the adjacent
	 * pool from the loader should have been provided instead). */
	uintptr_t page_size = sysconf(_SC_PAGESIZE);
	size_t alloc_size = ISLAND_POOL_SIZE;
	uintptr_t try_addrs[] = {
		(text_base + text_size + page_size - 1) & ~(page_size - 1),
		((text_base + text_size + 64*1024*1024) & ~(page_size - 1)),
		text_base >= alloc_size + page_size ?
			(text_base - alloc_size) & ~(page_size - 1) : 0,
	};
	for (int i = 0; i < (int)(sizeof(try_addrs)/sizeof(try_addrs[0])); i++) {
		if (try_addrs[i] == 0) continue;
		island_pool = mmap((void*)try_addrs[i], alloc_size,
		                   PROT_READ | PROT_WRITE | PROT_EXEC,
		                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		                   -1, 0);
		if (island_pool != MAP_FAILED && (uintptr_t)island_pool == try_addrs[i]) {
			island_pool_size = alloc_size;
			island_pool_used = 0;
			fprintf(stderr, "trampoline: island pool at %p (fallback mmap)\n",
			        island_pool);
			return 0;
		}
		if (island_pool != MAP_FAILED) {
			munmap(island_pool, alloc_size);
			island_pool = NULL;
		}
	}
	fprintf(stderr, "trampoline: failed to allocate island pool near __TEXT\n");
	return -1;
}

/* Allocate an island and return its address, or 0 on failure */
static uintptr_t alloc_island(uintptr_t native_addr)
{
	if (!island_pool || island_pool_used + ISLAND_SIZE > island_pool_size)
		return 0;

	struct trampoline_island* island =
		(struct trampoline_island*)(island_pool + island_pool_used);
	island->ldr_x16 = TRAMPOLINE_LDR_X16;
	island->br_x16 = TRAMPOLINE_BR_X16;
	island->target_addr = native_addr;

	uintptr_t addr = (uintptr_t)island;
	island_pool_used += ISLAND_SIZE;
	return addr;
}

/* Shared return-0 stub island (allocated once, reused for all stubs) */
static uintptr_t stub_island_addr = 0;

static uintptr_t get_stub_island(void)
{
	if (stub_island_addr) return stub_island_addr;
	if (!island_pool || island_pool_used + ISLAND_SIZE > island_pool_size)
		return 0;

	uint32_t* code = (uint32_t*)(island_pool + island_pool_used);
	code[0] = 0x52800000;  /* mov w0, #0 */
	code[1] = 0xD65F03C0;  /* ret */
	code[2] = 0xD503201F;  /* nop */
	code[3] = 0xD503201F;  /* nop */

	stub_island_addr = (uintptr_t)code;
	island_pool_used += ISLAND_SIZE;
	return stub_island_addr;
}

/*
 * Try macOS ↔ Linux C++ mangling variants for the same function.
 * macOS uint64_t = unsigned long long (mangling 'y'),
 * Linux uint64_t = unsigned long (mangling 'm'). Both 8 bytes on aarch64.
 * Same for int64_t: macOS 'x' (long long), Linux 'l' (long).
 *
 * Can't do global replacement because 'y' appears inside name components
 * (e.g. "Memory" → "6MemoryE"). Instead, try all 2^n subsets of y-positions,
 * replacing each subset with 'm'. With n typically 1-3, this is fast.
 */
static void* try_mangling_variants(void* lib, const char* name)
{
	size_t len = strlen(name);

	/* Collect positions of 'y' characters */
	int y_pos[16];
	int ny = 0;
	for (size_t i = 0; i < len && ny < 16; i++) {
		if (name[i] == 'y') y_pos[ny++] = i;
	}

	if (ny == 0) return NULL;

	char* buf = malloc(len + 1);
	if (!buf) return NULL;

	/* Try all non-empty subsets of y→m replacements */
	for (int mask = 1; mask < (1 << ny); mask++) {
		memcpy(buf, name, len + 1);
		for (int j = 0; j < ny; j++) {
			if (mask & (1 << j))
				buf[y_pos[j]] = 'm';
		}
		void* addr = dlsym(lib, buf);
		if (addr) {
			fprintf(stderr, "trampoline: mangling fallback: %s -> %s\n", name, buf);
			free(buf);
			return addr;
		}
	}

	/* Reverse direction: try subsets of m→y */
	int m_pos[16];
	int nm = 0;
	for (size_t i = 0; i < len && nm < 16; i++) {
		if (name[i] == 'm') m_pos[nm++] = i;
	}

	for (int mask = 1; mask < (1 << nm); mask++) {
		memcpy(buf, name, len + 1);
		for (int j = 0; j < nm; j++) {
			if (mask & (1 << j))
				buf[m_pos[j]] = 'y';
		}
		void* addr = dlsym(lib, buf);
		if (addr) {
			fprintf(stderr, "trampoline: mangling fallback: %s -> %s\n", name, buf);
			free(buf);
			return addr;
		}
	}

	free(buf);
	return NULL;
}

/*
 * Abort handler for un-trampolined Mach-O function calls.
 * Any call to a bgfx/SDL2 function still pointing to Mach-O code is a bug.
 */
static void __attribute__((noreturn)) trampoline_abort(const char* symbol_name)
{
	fprintf(stderr, "\nFATAL: un-trampolined Mach-O call to %s\n", symbol_name);
	abort();
}

/* Shared abort caller island: ldr x16, #8; br x16; .quad &trampoline_abort */
static uintptr_t abort_caller_addr = 0;

static uintptr_t get_abort_caller(void)
{
	if (abort_caller_addr) return abort_caller_addr;
	if (!island_pool || island_pool_used + ISLAND_SIZE > island_pool_size)
		return 0;

	struct trampoline_island* island =
		(struct trampoline_island*)(island_pool + island_pool_used);
	island->ldr_x16 = TRAMPOLINE_LDR_X16;
	island->br_x16 = TRAMPOLINE_BR_X16;
	island->target_addr = (uintptr_t)&trampoline_abort;

	abort_caller_addr = (uintptr_t)island;
	island_pool_used += ISLAND_SIZE;
	return abort_caller_addr;
}

/*
 * Allocate an abort trap island for a specific symbol.
 * Layout (16 bytes): ldr x0, #8; b <abort_caller>; .quad name_ptr
 * When called, x0 = symbol name pointer, then branches to shared
 * abort caller which calls trampoline_abort(x0).
 */
static uintptr_t make_abort_island(const char* symbol_name)
{
	uintptr_t caller = get_abort_caller();
	if (!caller) return 0;

	if (!island_pool || island_pool_used + ISLAND_SIZE > island_pool_size)
		return 0;

	uint32_t* code = (uint32_t*)(island_pool + island_pool_used);
	uintptr_t island_addr = (uintptr_t)code;

	/* ldr x0, #+8 — load name pointer from 8 bytes ahead */
	code[0] = 0x58000040;

	/* b <abort_caller> */
	int64_t offset = (int64_t)caller - (int64_t)(island_addr + 4);
	if (offset < -128*1024*1024 || offset >= 128*1024*1024 || (offset & 3)) {
		fprintf(stderr, "trampoline: abort caller too far from island\n");
		return 0;
	}
	uint32_t imm26 = (uint32_t)((offset >> 2) & 0x03FFFFFF);
	code[1] = 0x14000000 | imm26;

	/* .quad symbol_name — pointer to the name string (in Mach-O strtab, always valid) */
	*(uint64_t*)&code[2] = (uint64_t)symbol_name;

	island_pool_used += ISLAND_SIZE;
	return island_addr;
}

/* Convert a file offset to a memory address using segment mappings */
static void* fileoff_to_mem(void* mh, uintptr_t slide, uint32_t fileoff)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	for (uint32_t i = 0; i < header->ncmds; i++) {
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

/* Make a range of __TEXT pages writable for patching.
 * Called once before writing all trampolines, then restored after. */
static long sys_page_size;
static uintptr_t text_patch_base;
static size_t text_patch_size;

static int make_text_writable(void* mh, uintptr_t slide)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	sys_page_size = sysconf(_SC_PAGESIZE);
	if (sys_page_size <= 0) sys_page_size = 4096;

	/* Find __TEXT segment and make it writable */
	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (strcmp(seg->segname, "__TEXT") == 0) {
				text_patch_base = (seg->vmaddr + slide) & ~(uintptr_t)(sys_page_size - 1);
				uintptr_t end = seg->vmaddr + slide + seg->vmsize;
				end = (end + sys_page_size - 1) & ~(uintptr_t)(sys_page_size - 1);
				text_patch_size = end - text_patch_base;

				if (mprotect((void*)text_patch_base, text_patch_size,
				             PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
					fprintf(stderr, "trampoline: mprotect __TEXT writable failed: %s\n",
							strerror(errno));
					return -1;
				}
				/* Allocate island pool near __TEXT */
				if (init_island_pool(text_patch_base, text_patch_size) < 0) {
					fprintf(stderr, "trampoline: failed to allocate island pool\n");
					return -1;
				}
				return 0;
			}
		}
		cmd_ptr += lc->cmdsize;
	}
	fprintf(stderr, "trampoline: __TEXT segment not found\n");
	return -1;
}

static void restore_text_protection(void)
{
	if (text_patch_base && text_patch_size) {
		mprotect((void*)text_patch_base, text_patch_size, PROT_READ | PROT_EXEC);
	}
}

/* Write a trampoline at the given address.
 * Writes a 4-byte B instruction to an island containing the full indirect branch. */
static int write_trampoline(uintptr_t func_addr, uintptr_t native_addr)
{
	uintptr_t island_addr = alloc_island(native_addr);
	if (!island_addr) {
		fprintf(stderr, "trampoline: island pool exhausted\n");
		return -1;
	}

	/* Compute B offset: (target - pc) / 4, must fit in 26-bit signed field */
	int64_t offset = (int64_t)island_addr - (int64_t)func_addr;
	if (offset < -128*1024*1024 || offset >= 128*1024*1024 || (offset & 3) != 0) {
		fprintf(stderr, "trampoline: island at %p too far from %p (offset %ld)\n",
		        (void*)island_addr, (void*)func_addr, (long)offset);
		return -1;
	}

	/* Encode: B imm26 — opcode 0x14000000 | (imm26 & 0x03FFFFFF) */
	uint32_t imm26 = (uint32_t)((offset >> 2) & 0x03FFFFFF);
	uint32_t b_insn = 0x14000000 | imm26;

	/* Write single 4-byte instruction at function entry */
	*(uint32_t*)func_addr = b_insn;

	/* Flush instruction cache for the patched site */
	__builtin___clear_cache((char*)func_addr, (char*)(func_addr + 4));

	return 0;
}

/* Check if a symbol name matches any of the given prefixes */
static int matches_prefix(const char* name, const char** prefixes, int num_prefixes)
{
	for (int i = 0; i < num_prefixes; i++) {
		if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
			return 1;
	}
	return 0;
}

/* Check if a symbol has an override, return target or NULL */
static void* find_override(const char* name, trampoline_override_t* overrides, int num_overrides)
{
	for (int i = 0; i < num_overrides; i++) {
		if (strcmp(name, overrides[i].symbol_name) == 0)
			return overrides[i].target;
	}
	return NULL;
}

int trampoline_patch_lib(void* mh, uintptr_t slide,
                         const char* lib_path,
                         const char** prefixes, int num_prefixes,
                         trampoline_override_t* overrides, int num_overrides)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	/* Find LC_SYMTAB */
	struct symtab_command* symtab = NULL;
	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SYMTAB) {
			symtab = (struct symtab_command*)lc;
			break;
		}
		cmd_ptr += lc->cmdsize;
	}

	if (!symtab) {
		fprintf(stderr, "trampoline: no LC_SYMTAB found\n");
		return -1;
	}

	/* STUB mode: redirect all matching symbols to a return-0 stub */
	int stub_mode = (lib_path && strcmp(lib_path, "STUB") == 0);

	/* Load the native library (skip in stub mode) */
	void* native_lib = NULL;
	if (!stub_mode) {
		native_lib = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
		if (!native_lib) {
			fprintf(stderr, "trampoline: cannot load %s: %s\n", lib_path, dlerror());
			return -1;
		}
	}

	/* Get nlist array and string table from mapped memory */
	struct nlist_64* nlist_arr = (struct nlist_64*)fileoff_to_mem(mh, slide, symtab->symoff);
	char* strtab = (char*)fileoff_to_mem(mh, slide, symtab->stroff);

	if (!nlist_arr || !strtab) {
		fprintf(stderr, "trampoline: cannot locate symbol table in memory\n");
		return -1;
	}

	/* Make __TEXT writable for all patches at once */
	if (make_text_writable(mh, slide) < 0) {
		return -1;
	}

	int patched = 0;
	int mangling_fixed = 0;
	int trapped = 0;

	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		struct nlist_64* sym = &nlist_arr[i];

		/* Skip non-global, non-defined symbols */
		if ((sym->n_type & N_TYPE) != N_SECT) continue;
		if (!(sym->n_type & N_EXT)) continue;
		if (sym->n_type & N_STAB) continue;

		/* Get symbol name */
		if (sym->n_strx >= symtab->strsize) continue;
		const char* name = strtab + sym->n_strx;

		/* Check prefix match */
		if (!matches_prefix(name, prefixes, num_prefixes)) continue;

		/* Compute address in mapped memory */
		uintptr_t func_addr = sym->n_value + slide;

		/* Only trampoline symbols in __TEXT — skip data symbols */
		if (func_addr < text_patch_base ||
		    func_addr >= text_patch_base + text_patch_size) {
			continue;
		}

		/* Check for override first */
		void* target = find_override(name, overrides, num_overrides);

		if (!target && stub_mode) {
			/* In stub mode, all symbols go to return-0 stub */
			target = (void*)get_stub_island();
		}

		if (!target && native_lib) {
			/* Strip leading underscore for dlsym lookup */
			const char* lookup_name = name;
			if (lookup_name[0] == '_') lookup_name++;

			target = dlsym(native_lib, lookup_name);

			/* Fallback: try macOS↔Linux C++ mangling variants */
			if (!target && strncmp(lookup_name, "_ZN", 3) == 0) {
				target = try_mangling_variants(native_lib, lookup_name);
				if (target) mangling_fixed++;
			}
		}

		if (!target && !stub_mode) {
			/* No native match — trap this function so we know immediately
			 * if Mach-O code tries to call it. */
			uintptr_t abort_island = make_abort_island(name);
			if (abort_island) {
				extern int machismo_verbose;
				if (machismo_verbose)
					fprintf(stderr, "trampoline: TRAPPED (no native match): %s\n", name);
				if (write_trampoline(func_addr, abort_island) == 0) {
					trapped++;
				}
			}
			continue;
		}

		if (!target) {
			/* STUB mode with no target — shouldn't happen, but skip */
			continue;
		}

		/* Write trampoline */
		if (write_trampoline(func_addr, (uintptr_t)target) == 0) {
			patched++;
		} else {
			fprintf(stderr, "trampoline: failed to patch %s at %p\n", name, (void*)func_addr);
		}
	}

	/* Restore __TEXT to read+execute */
	restore_text_protection();

	fprintf(stderr, "trampoline: %d patched, %d mangling-fixed, %d trapped from %s\n",
			patched, mangling_fixed, trapped, lib_path);

	/* Don't dlclose — the game needs the native library to stay loaded */
	return patched;
}

int trampoline_patch_overrides(void* mh, uintptr_t slide, void* override_handle)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	/* Find LC_SYMTAB */
	struct symtab_command* symtab = NULL;
	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SYMTAB) {
			symtab = (struct symtab_command*)lc;
			break;
		}
		cmd_ptr += lc->cmdsize;
	}

	if (!symtab) {
		fprintf(stderr, "trampoline: override: no LC_SYMTAB found\n");
		return -1;
	}

	struct nlist_64* nlist_arr = (struct nlist_64*)fileoff_to_mem(mh, slide, symtab->symoff);
	char* strtab = (char*)fileoff_to_mem(mh, slide, symtab->stroff);

	if (!nlist_arr || !strtab) {
		fprintf(stderr, "trampoline: override: cannot locate symbol table\n");
		return -1;
	}

	/* Make __TEXT writable (idempotent if already writable from a prior call) */
	if (make_text_writable(mh, slide) < 0)
		return -1;

	int patched = 0;

	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		struct nlist_64* sym = &nlist_arr[i];

		if ((sym->n_type & N_TYPE) != N_SECT) continue;
		if (!(sym->n_type & N_EXT)) continue;
		if (sym->n_type & N_STAB) continue;
		if (sym->n_strx >= symtab->strsize) continue;

		const char* name = strtab + sym->n_strx;
		uintptr_t func_addr = sym->n_value + slide;

		/* Only patch symbols in __TEXT */
		if (func_addr < text_patch_base ||
		    func_addr >= text_patch_base + text_patch_size)
			continue;

		/* Strip one leading underscore (Mach-O convention) for dlsym */
		const char* lookup_name = name;
		if (lookup_name[0] == '_') lookup_name++;

		void* target = dlsym(override_handle, lookup_name);
		if (!target) continue;

		if (write_trampoline(func_addr, (uintptr_t)target) == 0) {
			fprintf(stderr, "trampoline: override: %s -> %p\n", name, target);
			patched++;
		}
	}

	restore_text_protection();

	fprintf(stderr, "trampoline: override: %d functions replaced\n", patched);
	return patched;
}

/* Legacy env-var based API */
int trampoline_patch(void* mh, uintptr_t slide)
{
	const char* lib_path = getenv("MACHISMO_TRAMPOLINE_LIB");
	const char* prefix = getenv("MACHISMO_TRAMPOLINE_PREFIX");

	if (!lib_path) lib_path = "libSDL2-2.0.so.0";
	if (!prefix) prefix = "_SDL_";

	const char* prefixes[] = { prefix };
	return trampoline_patch_lib(mh, slide, lib_path, prefixes, 1, NULL, 0);
}

/*
 * Stale __DATA access detection.
 *
 * After all trampolines are applied, find library data symbols in __DATA
 * and guard pages that contain ONLY library symbols. Any access to a guarded
 * page means un-trampolined or inlined code is touching Mach-O library state
 * instead of native .so state — always a bug.
 */

/* Track guarded page ranges for SIGSEGV handler */
#define MAX_GUARDED_PAGES 4096
static struct {
	uintptr_t base;
	size_t size;
} guarded_pages[MAX_GUARDED_PAGES];
static int num_guarded_pages = 0;

static void stale_data_sigsegv(int sig, siginfo_t* info, void* ucontext)
{
	uintptr_t fault_addr = (uintptr_t)info->si_addr;

	/* Check if fault is in a guarded page */
	for (int i = 0; i < num_guarded_pages; i++) {
		if (fault_addr >= guarded_pages[i].base &&
		    fault_addr < guarded_pages[i].base + guarded_pages[i].size) {
			ucontext_t* uc = (ucontext_t*)ucontext;
			uintptr_t pc = uc->uc_mcontext.pc;
			fprintf(stderr,
				"\nFATAL: stale Mach-O data access at %p from PC=%p\n"
				"This means un-trampolined or inlined code is accessing\n"
				"Mach-O library state instead of native .so state.\n",
				(void*)fault_addr, (void*)pc);
			abort();
		}
	}

	/* Not our fault — re-raise with default handler */
	struct sigaction sa;
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGSEGV, &sa, NULL);
	raise(SIGSEGV);
}

void trampoline_guard_stale_data(void* mh, uintptr_t slide,
                                  const char** prefixes, int num_prefixes)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) page_size = 4096;

	/* Find __DATA segment boundaries */
	uintptr_t data_base = 0, data_end = 0;
	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (strcmp(seg->segname, "__DATA") == 0) {
				data_base = seg->vmaddr + slide;
				data_end = data_base + seg->vmsize;
				break;
			}
		}
		cmd_ptr += lc->cmdsize;
	}
	if (!data_base) return;

	size_t num_pages = (data_end - data_base + page_size - 1) / page_size;
	if (num_pages == 0) return;

	/* Bitmap: has_library[i] = page i has a library symbol
	 *         has_other[i]   = page i has a non-library symbol */
	uint8_t* has_library = calloc(num_pages, 1);
	uint8_t* has_other = calloc(num_pages, 1);
	if (!has_library || !has_other) {
		free(has_library);
		free(has_other);
		return;
	}

	/* Find LC_SYMTAB */
	struct symtab_command* symtab = NULL;
	cmd_ptr = (uint8_t*)(header + 1);
	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SYMTAB) {
			symtab = (struct symtab_command*)lc;
			break;
		}
		cmd_ptr += lc->cmdsize;
	}
	if (!symtab) { free(has_library); free(has_other); return; }

	struct nlist_64* nlist_arr = (struct nlist_64*)fileoff_to_mem(mh, slide, symtab->symoff);
	char* strtab = (char*)fileoff_to_mem(mh, slide, symtab->stroff);
	if (!nlist_arr || !strtab) { free(has_library); free(has_other); return; }

	int lib_data_syms = 0;

	/* Walk all symbols, categorize pages */
	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		struct nlist_64* sym = &nlist_arr[i];
		if ((sym->n_type & N_TYPE) != N_SECT) continue;
		if (sym->n_type & N_STAB) continue;
		if (sym->n_strx >= symtab->strsize) continue;

		uintptr_t addr = sym->n_value + slide;
		if (addr < data_base || addr >= data_end) continue;

		size_t page_idx = (addr - data_base) / page_size;
		if (page_idx >= num_pages) continue;

		const char* name = strtab + sym->n_strx;
		if (matches_prefix(name, prefixes, num_prefixes)) {
			has_library[page_idx] = 1;
			lib_data_syms++;
			extern int machismo_verbose;
			if (machismo_verbose)
				fprintf(stderr, "trampoline: stale data symbol: %s at %p\n",
				        name, (void*)addr);
		} else {
			has_other[page_idx] = 1;
		}
	}

	if (lib_data_syms == 0) {
		free(has_library);
		free(has_other);
		return;
	}

	/* Guard pages that have ONLY library symbols */
	int guarded = 0;
	int mixed = 0;
	for (size_t i = 0; i < num_pages; i++) {
		if (!has_library[i]) continue;
		if (has_other[i]) {
			mixed++;
			continue;
		}
		if (num_guarded_pages >= MAX_GUARDED_PAGES) break;

		uintptr_t page_addr = data_base + i * page_size;
		if (mprotect((void*)page_addr, page_size, PROT_NONE) == 0) {
			guarded_pages[num_guarded_pages].base = page_addr;
			guarded_pages[num_guarded_pages].size = page_size;
			num_guarded_pages++;
			guarded++;
		}
	}

	free(has_library);
	free(has_other);

	/* Install SIGSEGV handler */
	if (guarded > 0) {
		struct sigaction sa;
		sa.sa_sigaction = stale_data_sigsegv;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, NULL);
	}

	fprintf(stderr, "trampoline: __DATA guard: %d library data symbols, "
	        "%d pages guarded, %d mixed pages (cannot guard)\n",
	        lib_data_syms, guarded, mixed);
}
