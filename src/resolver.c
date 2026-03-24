/*
 * Mach-O Chained Fixup Resolver for aarch64 Linux.
 *
 * Parses LC_DYLD_CHAINED_FIXUPS from a loaded Mach-O binary and resolves
 * symbol bindings by mapping dylib names to native Linux .so files.
 *
 * Supports:
 *   - DYLD_CHAINED_PTR_64_OFFSET (format 6) — used by modern arm64 binaries
 *   - DYLD_CHAINED_PTR_64 (format 2) — older 64-bit format
 *   - DYLD_CHAINED_IMPORT (format 1) — standard import table
 *
 * Reference: Apple's fixup-chains.h, dyld MachOLoaded.cpp
 */

#include "resolver.h"
#include "dylib_loader.h"
#include "macho_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>

/*
 * Try macOS ↔ Linux C++ mangling variants for the same function.
 * See trampoline.c for detailed rationale — combinatorial y↔m substitution.
 */
static void* try_mangling_variants(void* lib, const char* name)
{
	size_t len = strlen(name);

	int y_pos[16];
	int ny = 0;
	for (size_t i = 0; i < len && ny < 16; i++) {
		if (name[i] == 'y') y_pos[ny++] = i;
	}

	char* buf = malloc(len + 1);
	if (!buf) return NULL;

	/* Try all non-empty subsets of y→m */
	for (int mask = 1; mask < (1 << ny); mask++) {
		memcpy(buf, name, len + 1);
		for (int j = 0; j < ny; j++) {
			if (mask & (1 << j))
				buf[y_pos[j]] = 'm';
		}
		void* addr = dlsym(lib, buf);
		if (addr) {
			fprintf(stderr, "resolver: mangling fallback: %s -> %s\n", name, buf);
			free(buf);
			return addr;
		}
	}

	/* Reverse: m→y */
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
			fprintf(stderr, "resolver: mangling fallback: %s -> %s\n", name, buf);
			free(buf);
			return addr;
		}
	}

	free(buf);
	return NULL;
}

/* ---- Chained fixup structures (from Apple's fixup-chains.h) ---- */

#define LC_DYLD_CHAINED_FIXUPS  (0x34 | LC_REQ_DYLD)
#define LC_DYLD_EXPORTS_TRIE    (0x33 | LC_REQ_DYLD)

struct linkedit_data_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t dataoff;
	uint32_t datasize;
};

struct dyld_chained_fixups_header {
	uint32_t fixups_version;
	uint32_t starts_offset;
	uint32_t imports_offset;
	uint32_t symbols_offset;
	uint32_t imports_count;
	uint32_t imports_format;
	uint32_t symbols_format;
};

struct dyld_chained_starts_in_image {
	uint32_t seg_count;
	uint32_t seg_info_offset[];
};

struct dyld_chained_starts_in_segment {
	uint32_t size;
	uint16_t page_size;
	uint16_t pointer_format;
	uint64_t segment_offset;
	uint32_t max_valid_pointer;
	uint16_t page_count;
	uint16_t page_start[];
};

/* Pointer formats */
#define DYLD_CHAINED_PTR_64           2
#define DYLD_CHAINED_PTR_64_OFFSET    6

/* Import formats */
#define DYLD_CHAINED_IMPORT           1
#define DYLD_CHAINED_IMPORT_ADDEND    2
#define DYLD_CHAINED_IMPORT_ADDEND64  3

/* Page start sentinel */
#define DYLD_CHAINED_PTR_START_NONE   0xFFFF

/* Chained pointer bitfield access for DYLD_CHAINED_PTR_64 and PTR_64_OFFSET */
#define FIXUP_PTR_BIND(raw)     (((raw) >> 63) & 1)
#define FIXUP_PTR_NEXT(raw)     (((raw) >> 51) & 0xFFF)
#define FIXUP_BIND_ORDINAL(raw) ((raw) & 0xFFFFFF)
#define FIXUP_BIND_ADDEND(raw)  (((raw) >> 24) & 0xFF)
#define FIXUP_REBASE_TARGET(raw)  ((raw) & 0xFFFFFFFFF)     /* 36 bits */
#define FIXUP_REBASE_HIGH8(raw)   (((raw) >> 36) & 0xFF)

struct dyld_chained_import {
	uint32_t lib_ordinal  :  8;
	uint32_t weak_import  :  1;
	uint32_t name_offset  : 23;
};

struct dyld_chained_import_addend {
	uint32_t lib_ordinal  :  8;
	uint32_t weak_import  :  1;
	uint32_t name_offset  : 23;
	int32_t  addend;
};

struct dyld_chained_import_addend64 {
	uint64_t lib_ordinal  : 16;
	uint64_t weak_import  :  1;
	uint64_t reserved     : 15;
	uint64_t name_offset  : 32;
	uint64_t addend;
};

/* ---- Dylib mapping ---- */

#define MAX_DYLIBS 64
#define MAX_MAPPINGS 128
#define MAX_NAME 256

/* Actions for unmapped dylibs */
enum dylib_action {
	DYLIB_MAP,     /* map to a Linux .so */
	DYLIB_STUB,    /* return stub (NULL/0) for all symbols */
	DYLIB_SKIP,    /* silently skip — symbols remain unresolved */
	DYLIB_MACHO,   /* look up in a loaded Mach-O dylib */
};

struct dylib_entry {
	int ordinal;                     /* 1-based ordinal from LC_LOAD_DYLIB order */
	char name[MAX_NAME];             /* dylib install name (basename) */
	enum dylib_action action;
	char so_path[MAX_NAME];          /* Linux .so path (for DYLIB_MAP) or Mach-O path */
	void* handle;                    /* dlopen handle (DYLIB_MAP) */
	struct macho_dylib_info* macho_info; /* loaded Mach-O dylib (DYLIB_MACHO) */
};

struct dylib_mapping {
	char dylib_pattern[MAX_NAME];
	char so_path[MAX_NAME];          /* "STUB", "SKIP", or a .so path */
};

/* ---- Segment info for converting file offsets to memory ---- */

struct seg_info {
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	int prot;
};

/* ---- Resolver state ---- */

struct resolver_state {
	struct mach_header_64* mh;
	uintptr_t slide;
	uintptr_t mh_addr;

	/* Load commands data */
	struct dylib_entry dylibs[MAX_DYLIBS];
	int ndylibs;

	/* Segment info */
	struct seg_info linkedit;
	struct seg_info segments[16];
	int nsegments;

	/* Chained fixups from LC_DYLD_CHAINED_FIXUPS */
	uint32_t fixups_dataoff;
	uint32_t fixups_datasize;
	bool has_chained_fixups;

	/* Dylib mapping config */
	struct dylib_mapping mappings[MAX_MAPPINGS];
	int nmappings;

	/* Stats */
	int binds_resolved;
	int binds_stubbed;
	int binds_failed;
	int rebases_applied;
};

/* ---- C++ constructor/destructor ABI adapters ----
 *
 * Apple's ARM64 ABI: C1/C2 constructors and D1/D2 destructors return `this`
 * in x0. Standard Itanium ABI on Linux: they return void (x0 is clobbered).
 *
 * When a Mach-O binary calls a constructor in a native Linux .so, the caller
 * expects x0 = this afterward. We wrap such calls with a small trampoline:
 *
 *   stp x30, x0, [sp, #-16]!  // save LR + this
 *   ldr x16, [pc, #20]        // load real function address
 *   blr x16                   // call real ctor/dtor
 *   ldp x30, x0, [sp], #16   // restore LR + this
 *   ret
 *   nop
 *   .quad <real_addr>
 *
 * IMPORTANT: This adapter shifts sp by -16 before calling. This is safe for
 * constructors where ALL parameters fit in registers (≤8 float/SIMD params
 * in s0-s7, ≤7 integer params in x1-x7 since x0=this). But constructors
 * with stack-passed parameters (e.g., sf::Transform(9 floats)) would have
 * those parameters corrupted because [sp] now points to saved x30 instead
 * of the 9th parameter.
 *
 * For constructors with potential stack-passed parameters, we skip the
 * adapter entirely. In practice, Linux constructors usually preserve x0
 * (the this pointer is used throughout and not explicitly clobbered), and
 * most callers save the pointer before calling the constructor anyway.
 */

#include <unistd.h>

/* Pool of executable trampoline memory */
static uint8_t* ctor_tramp_pool = NULL;
static size_t ctor_tramp_used = 0;
static size_t ctor_tramp_capacity = 0;
static int ctor_tramp_count = 0;

struct ctor_abi_trampoline {
	uint32_t stp_lr_x0;     /* stp x30, x0, [sp, #-16]! — save LR + this */
	uint32_t ldr_x16;       /* ldr x16, [pc, #20]       — load target */
	uint32_t blr_x16;       /* blr x16                  — call (clobbers x0,x16,x17,x30) */
	uint32_t ldp_lr_x0;     /* ldp x30, x0, [sp], #16  — restore LR + this */
	uint32_t ret;            /* ret                      — return to caller */
	uint32_t pad;            /* nop (alignment for .quad) */
	uint64_t target_addr;   /* real function address */
};

/* Check if a mangled symbol name is a C++ constructor or destructor */
static int is_ctor_or_dtor(const char* name)
{
	if (!name) return 0;
	/* Itanium mangling: _ZN...C1E, _ZN...C2E (ctors), _ZN...D1E, _ZN...D2E (dtors)
	 * Also C1Ev, C2Ev, D1Ev, D2Ev for void param ctors/dtors */
	const char* p = name;
	/* Skip leading underscore (Mach-O convention) */
	if (*p == '_') p++;
	/* Must start with _ZN or _ZNK */
	if (p[0] != '_' || p[1] != 'Z') return 0;

	/* Search for C1E, C2E, D0E, D1E, D2E patterns */
	for (; *p; p++) {
		if ((p[0] == 'C' || p[0] == 'D') &&
		    (p[1] == '0' || p[1] == '1' || p[1] == '2') &&
		    p[2] == 'E') {
			return 1;
		}
	}
	return 0;
}

/* Check if a ctor/dtor has parameters that would be passed on the stack.
 * ARM64 passes floats in s0-s7 (8 regs) and ints in x1-x7 (7 regs, x0=this).
 * If more params than registers, the extras go on the stack and the ctor
 * adapter's sp shift would corrupt them.
 *
 * We detect this from the Itanium-mangled parameter types after the 'E'.
 * Single-letter type codes: f=float, d=double go in float regs;
 * i,j,l,m,x,y,c,s,b,a,h,t,w go in integer regs.
 * Complex types (pointers, references, classes) also use integer regs.
 */
static int ctor_has_stack_params(const char* name)
{
	if (!name) return 0;
	const char* p = name;
	if (*p == '_') p++;
	if (p[0] != '_' || p[1] != 'Z') return 0;

	/* Find the parameter list after C[012]E or D[012]E */
	const char* params = NULL;
	for (; *p; p++) {
		if ((p[0] == 'C' || p[0] == 'D') &&
		    (p[1] == '0' || p[1] == '1' || p[1] == '2') &&
		    p[2] == 'E') {
			params = p + 3;
			break;
		}
	}
	if (!params || !*params || *params == 'v') return 0; /* no params or void */

	int float_count = 0;
	int int_count = 0;
	for (p = params; *p; p++) {
		switch (*p) {
		case 'f': case 'd': /* float, double → float regs */
			float_count++;
			break;
		case 'i': case 'j': case 'l': case 'm': /* int types → int regs */
		case 'x': case 'y': case 'c': case 's':
		case 'b': case 'a': case 'h': case 't': case 'w':
			int_count++;
			break;
		default:
			/* Complex type (pointer, reference, class, etc.) — stop
			 * counting simple types. Complex types use int regs but
			 * we can't easily count them, so be conservative and
			 * assume no overflow. */
			return (float_count > 8 || int_count > 7);
		}
	}
	return (float_count > 8 || int_count > 7);
}

/* Create an ABI adapter trampoline that saves/restores x0 around the call */
static uintptr_t wrap_ctor_for_apple_abi(uintptr_t real_addr)
{
	/* Allocate trampoline pool on first use */
	if (!ctor_tramp_pool) {
		long page_size = sysconf(_SC_PAGESIZE);
		ctor_tramp_capacity = page_size * 4; /* 4 pages, room for ~500 trampolines */
		ctor_tramp_pool = mmap(NULL, ctor_tramp_capacity,
		                       PROT_READ | PROT_WRITE | PROT_EXEC,
		                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (ctor_tramp_pool == MAP_FAILED) {
			fprintf(stderr, "resolver: failed to allocate ctor trampoline pool\n");
			ctor_tramp_pool = NULL;
			return real_addr; /* fall back to unwrapped */
		}
	}

	if (ctor_tramp_used + sizeof(struct ctor_abi_trampoline) > ctor_tramp_capacity) {
		fprintf(stderr, "resolver: ctor trampoline pool exhausted\n");
		return real_addr;
	}

	struct ctor_abi_trampoline* t =
		(struct ctor_abi_trampoline*)(ctor_tramp_pool + ctor_tramp_used);
	ctor_tramp_used += sizeof(struct ctor_abi_trampoline);

	t->stp_lr_x0     = 0xA9BF03FE; /* stp x30, x0, [sp, #-16]! — save LR + this */
	t->ldr_x16      = 0x580000B0; /* ldr x16, [pc, #20] — load target from .quad */
	t->blr_x16      = 0xD63F0200; /* blr x16 — call real ctor/dtor */
	t->ldp_lr_x0    = 0xA8C103FE; /* ldp x30, x0, [sp], #16 — restore LR + this */
	t->ret           = 0xD65F03C0; /* ret */
	t->pad           = 0xD503201F; /* nop (alignment for .quad) */
	t->target_addr   = real_addr;

	__builtin___clear_cache((char*)t, (char*)(t + 1));
	ctor_tramp_count++;

	return (uintptr_t)t;
}

/* ---- Mach-O symbol table lookup ----
 *
 * Resolves symbols defined in the Mach-O binary itself.
 * Used for lib_ordinal -1 (main executable) and -3 (weak lookup).
 */
static uintptr_t lookup_macho_symbol(struct resolver_state* rs, const char* name)
{
	struct mach_header_64* mh = rs->mh;
	uint8_t* cmds = (uint8_t*)(mh + 1);
	uint32_t p = 0;
	struct symtab_command* symtab = NULL;

	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command* lc = (struct load_command*)&cmds[p];
		if (lc->cmd == LC_SYMTAB)
			symtab = (struct symtab_command*)lc;
		p += lc->cmdsize;
	}
	if (!symtab) return 0;

	/* Convert file offsets to memory addresses using segment mappings */
	struct nlist_64* syms = NULL;
	char* strtab = NULL;

	/* Find LINKEDIT segment to convert file offsets */
	p = 0;
	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command* lc = (struct load_command*)&cmds[p];
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (symtab->symoff >= seg->fileoff &&
			    symtab->symoff < seg->fileoff + seg->filesize) {
				uintptr_t base = seg->vmaddr + rs->slide;
				syms = (struct nlist_64*)(base + (symtab->symoff - seg->fileoff));
				strtab = (char*)(base + (symtab->stroff - seg->fileoff));
			}
		}
		p += lc->cmdsize;
	}
	if (!syms || !strtab) return 0;

	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		struct nlist_64* nl = &syms[i];
		if (nl->n_type & N_STAB) continue;
		if ((nl->n_type & N_TYPE) != N_SECT) continue;
		if (nl->n_strx >= symtab->strsize) continue;

		const char* sym = &strtab[nl->n_strx];
		if (strcmp(sym, name) == 0)
			return nl->n_value + rs->slide;
	}
	return 0;
}

/* ---- Forward declarations ---- */

static int parse_load_commands(struct resolver_state* rs);
static int load_mapping_config(struct resolver_state* rs, const char* path);
static int open_dylibs(struct resolver_state* rs);
static int process_chained_fixups(struct resolver_state* rs);
static int walk_chain(struct resolver_state* rs, uint64_t* chain_start, uint16_t pointer_format,
                      const struct dyld_chained_fixups_header* header, const char* chain_data);
static void close_dylibs(struct resolver_state* rs);
static const char* basename_from_path(const char* path);

/* ---- Implementation ---- */

int resolver_resolve_fixups(void* mh, uintptr_t slide, const char* map_file)
{
	struct resolver_state rs = {0};
	int ret = -1;

	rs.mh = (struct mach_header_64*)mh;
	rs.slide = slide;
	rs.mh_addr = (uintptr_t)mh;

	fprintf(stderr, "resolver: mach header at %p, slide=0x%lx\n", mh, slide);

	/* Step 1: Parse load commands to find dylibs, segments, fixup info */
	if (parse_load_commands(&rs) < 0) {
		fprintf(stderr, "resolver: failed to parse load commands\n");
		goto out;
	}

	if (!rs.has_chained_fixups) {
		fprintf(stderr, "resolver: no LC_DYLD_CHAINED_FIXUPS found — nothing to resolve\n");
		ret = 0;
		goto out;
	}

	fprintf(stderr, "resolver: found %d dylibs, chained fixups at file offset 0x%x (size %u)\n",
			rs.ndylibs, rs.fixups_dataoff, rs.fixups_datasize);

	/* Step 2: Load dylib mapping config */
	if (load_mapping_config(&rs, map_file) < 0) {
		fprintf(stderr, "resolver: warning: no mapping config loaded, using defaults\n");
	}

	/* Step 3: Open Linux .so files for each mapped dylib */
	if (open_dylibs(&rs) < 0) {
		fprintf(stderr, "resolver: failed to open dylibs\n");
		goto out;
	}

	/* Step 4: Walk chained fixups and resolve */
	if (process_chained_fixups(&rs) < 0) {
		fprintf(stderr, "resolver: failed processing fixups\n");
		goto out;
	}

	fprintf(stderr, "resolver: done — %d binds resolved, %d stubbed, %d failed, %d rebases, %d ctor/dtor ABI adapters\n",
			rs.binds_resolved, rs.binds_stubbed, rs.binds_failed, rs.rebases_applied, ctor_tramp_count);

	ret = 0;

out:
	close_dylibs(&rs);
	return ret;
}

/* ---- Parse Mach-O load commands ---- */

static int parse_load_commands(struct resolver_state* rs)
{
	uint8_t* cmd_ptr = (uint8_t*)(rs->mh + 1); /* right after the header */
	int dylib_ordinal = 0;

	for (uint32_t i = 0; i < rs->mh->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;

		switch (lc->cmd) {
		case LC_LOAD_DYLIB:
		case LC_LOAD_WEAK_DYLIB:
		case LC_REEXPORT_DYLIB: {
			struct dylib_command* dc = (struct dylib_command*)lc;
			const char* name = ((char*)dc) + dc->dylib.name.offset;

			dylib_ordinal++;
			if (dylib_ordinal <= MAX_DYLIBS) {
				struct dylib_entry* de = &rs->dylibs[dylib_ordinal - 1];
				de->ordinal = dylib_ordinal;
				const char* base = basename_from_path(name);
				strncpy(de->name, base, MAX_NAME - 1);
				de->action = DYLIB_SKIP; /* default: skip until mapped */
				de->handle = NULL;
			}
			break;
		}
		case LC_SEGMENT_64: {
			struct segment_command_64* seg = (struct segment_command_64*)lc;

			if (strcmp(seg->segname, "__LINKEDIT") == 0) {
				rs->linkedit.vmaddr = seg->vmaddr;
				rs->linkedit.vmsize = seg->vmsize;
				rs->linkedit.fileoff = seg->fileoff;
				rs->linkedit.filesize = seg->filesize;
			}

			if (rs->nsegments < 16) {
				struct seg_info* si = &rs->segments[rs->nsegments];
				si->vmaddr = seg->vmaddr;
				si->vmsize = seg->vmsize;
				si->fileoff = seg->fileoff;
				si->filesize = seg->filesize;
				si->prot = seg->initprot;
				rs->nsegments++;
			}
			break;
		}
		case LC_DYLD_CHAINED_FIXUPS: {
			struct linkedit_data_command* ldc = (struct linkedit_data_command*)lc;
			rs->fixups_dataoff = ldc->dataoff;
			rs->fixups_datasize = ldc->datasize;
			rs->has_chained_fixups = true;
			break;
		}
		}

		cmd_ptr += lc->cmdsize;
	}

	rs->ndylibs = dylib_ordinal;
	return 0;
}

/* ---- Convert file offset to memory address via LINKEDIT mapping ---- */

static void* fileoff_to_mem(struct resolver_state* rs, uint32_t fileoff)
{
	/* __LINKEDIT maps a range of file offsets into memory */
	if (fileoff >= rs->linkedit.fileoff &&
	    fileoff < rs->linkedit.fileoff + rs->linkedit.filesize) {
		return (void*)(rs->linkedit.vmaddr + rs->slide +
		               (fileoff - rs->linkedit.fileoff));
	}
	/* Fall back: try all segments */
	for (int i = 0; i < rs->nsegments; i++) {
		struct seg_info* si = &rs->segments[i];
		if (fileoff >= si->fileoff && fileoff < si->fileoff + si->filesize) {
			return (void*)(si->vmaddr + rs->slide + (fileoff - si->fileoff));
		}
	}
	return NULL;
}

/* ---- Dylib mapping config ---- */

static int load_mapping_config(struct resolver_state* rs, const char* path)
{
	FILE* f;
	char line[512];

	if (!path) {
		/* Try default location next to mldr */
		path = "dylib_map.conf";
	}

	f = fopen(path, "r");
	if (!f) {
		/* Not an error — will use defaults */
		return -1;
	}

	while (fgets(line, sizeof(line), f) && rs->nmappings < MAX_MAPPINGS) {
		/* Strip newline */
		char* nl = strchr(line, '\n');
		if (nl) *nl = '\0';

		/* Skip comments and empty lines */
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\0') continue;

		/* Parse: dylib_pattern = so_path */
		char* eq = strchr(p, '=');
		if (!eq) continue;

		*eq = '\0';
		char* key = p;
		char* val = eq + 1;

		/* Trim whitespace */
		while (*key == ' ' || *key == '\t') key++;
		char* end = key + strlen(key) - 1;
		while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';

		while (*val == ' ' || *val == '\t') val++;
		end = val + strlen(val) - 1;
		while (end > val && (*end == ' ' || *end == '\t')) *end-- = '\0';

		struct dylib_mapping* m = &rs->mappings[rs->nmappings++];
		strncpy(m->dylib_pattern, key, MAX_NAME - 1);
		strncpy(m->so_path, val, MAX_NAME - 1);
	}

	fclose(f);
	fprintf(stderr, "resolver: loaded %d dylib mappings from %s\n", rs->nmappings, path);
	return 0;
}

/* ---- Match a dylib name against mappings ---- */

static const struct dylib_mapping* find_mapping(struct resolver_state* rs, const char* dylib_name)
{
	for (int i = 0; i < rs->nmappings; i++) {
		/* Simple substring match — pattern can match anywhere in the name */
		if (strstr(dylib_name, rs->mappings[i].dylib_pattern)) {
			return &rs->mappings[i];
		}
	}
	return NULL;
}

/* ---- Open Linux .so files for mapped dylibs ---- */

static int open_dylibs(struct resolver_state* rs)
{
	for (int i = 0; i < rs->ndylibs && i < MAX_DYLIBS; i++) {
		struct dylib_entry* de = &rs->dylibs[i];
		const struct dylib_mapping* m = find_mapping(rs, de->name);

		if (!m) {
			fprintf(stderr, "resolver: dylib[%d] '%s' — no mapping, skipping\n",
					de->ordinal, de->name);
			de->action = DYLIB_SKIP;
			continue;
		}

		if (strcmp(m->so_path, "STUB") == 0) {
			fprintf(stderr, "resolver: dylib[%d] '%s' — stubbed\n",
					de->ordinal, de->name);
			de->action = DYLIB_STUB;
			continue;
		}

		if (strcmp(m->so_path, "SKIP") == 0) {
			fprintf(stderr, "resolver: dylib[%d] '%s' — skipped\n",
					de->ordinal, de->name);
			de->action = DYLIB_SKIP;
			continue;
		}

		/* MACHO: prefix — look up in loaded Mach-O dylib */
		if (strncmp(m->so_path, "MACHO:", 6) == 0) {
			const char* macho_path = m->so_path + 6;
			/* Check if already loaded */
			struct macho_dylib_info* mdi = dylib_loader_find(macho_path);
			if (!mdi) {
				/* Load it now */
				mdi = dylib_loader_load(macho_path);
			}
			if (mdi) {
				de->macho_info = mdi;
				de->action = DYLIB_MACHO;
				strncpy(de->so_path, macho_path, MAX_NAME - 1);
				fprintf(stderr, "resolver: dylib[%d] '%s' → MACHO '%s' — loaded (%u symbols)\n",
						de->ordinal, de->name, macho_path, mdi->nsyms);
			} else {
				fprintf(stderr, "resolver: dylib[%d] '%s' → MACHO '%s' — load FAILED\n",
						de->ordinal, de->name, macho_path);
				de->action = DYLIB_SKIP;
			}
			continue;
		}

		/* Try to dlopen the Linux .so */
		de->handle = dlopen(m->so_path, RTLD_LAZY | RTLD_GLOBAL);
		if (!de->handle) {
			fprintf(stderr, "resolver: dylib[%d] '%s' → '%s' — dlopen FAILED: %s\n",
					de->ordinal, de->name, m->so_path, dlerror());
			de->action = DYLIB_SKIP;
			continue;
		}

		strncpy(de->so_path, m->so_path, MAX_NAME - 1);
		de->action = DYLIB_MAP;
		fprintf(stderr, "resolver: dylib[%d] '%s' → '%s' — loaded\n",
				de->ordinal, de->name, m->so_path);
	}

	return 0;
}

/* ---- Make segment writable for patching ---- */

static void make_segment_writable(struct resolver_state* rs, uintptr_t addr)
{
	uintptr_t page = addr & ~(uintptr_t)0xFFF;
	/* Make the page writable so we can patch pointers */
	mprotect((void*)page, 0x4000, PROT_READ | PROT_WRITE);
}

/* ---- Process chained fixups ---- */

static int process_chained_fixups(struct resolver_state* rs)
{
	/* Get the chained fixups data from mapped memory */
	char* chain_data = (char*)fileoff_to_mem(rs, rs->fixups_dataoff);
	if (!chain_data) {
		fprintf(stderr, "resolver: cannot locate chained fixups data in memory\n");
		return -1;
	}

	struct dyld_chained_fixups_header* header = (struct dyld_chained_fixups_header*)chain_data;

	fprintf(stderr, "resolver: fixups version=%u, imports_count=%u, imports_format=%u\n",
			header->fixups_version, header->imports_count, header->imports_format);

	/* Get the starts-in-image structure */
	struct dyld_chained_starts_in_image* starts =
		(struct dyld_chained_starts_in_image*)(chain_data + header->starts_offset);

	fprintf(stderr, "resolver: %u segments with fixup chains\n", starts->seg_count);

	/* Walk each segment's chains */
	for (uint32_t seg = 0; seg < starts->seg_count; seg++) {
		if (starts->seg_info_offset[seg] == 0)
			continue;

		struct dyld_chained_starts_in_segment* seg_starts =
			(struct dyld_chained_starts_in_segment*)(
				(char*)starts + starts->seg_info_offset[seg]);

		fprintf(stderr, "resolver: segment %u: page_size=0x%x, pointer_format=%u, "
				"segment_offset=0x%lx, page_count=%u\n",
				seg, seg_starts->page_size, seg_starts->pointer_format,
				(unsigned long)seg_starts->segment_offset, seg_starts->page_count);

		if (seg_starts->pointer_format != DYLD_CHAINED_PTR_64 &&
		    seg_starts->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
			fprintf(stderr, "resolver: unsupported pointer format %u in segment %u\n",
					seg_starts->pointer_format, seg);
			continue;
		}

		/* Make the target segment writable for patching */
		uintptr_t seg_base = rs->mh_addr + seg_starts->segment_offset;
		make_segment_writable(rs, seg_base);

		/* Walk each page */
		for (uint16_t page = 0; page < seg_starts->page_count; page++) {
			uint16_t page_start = seg_starts->page_start[page];

			if (page_start == DYLD_CHAINED_PTR_START_NONE)
				continue;

			/* Calculate address of first fixup in this page */
			uintptr_t page_addr = seg_base + (page * seg_starts->page_size);
			uint64_t* chain_ptr = (uint64_t*)(page_addr + page_start);

			if (walk_chain(rs, chain_ptr, seg_starts->pointer_format,
			               header, chain_data) < 0) {
				fprintf(stderr, "resolver: error walking chain at page %u of segment %u\n",
						page, seg);
			}
		}
	}

	return 0;
}

/* ---- Resolve a single bind entry ---- */

static uintptr_t resolve_import(struct resolver_state* rs,
                                uint32_t ordinal,
                                const struct dyld_chained_fixups_header* header,
                                const char* chain_data,
                                int64_t addend)
{
	if (ordinal >= header->imports_count) {
		fprintf(stderr, "resolver: bind ordinal %u out of range (max %u)\n",
				ordinal, header->imports_count);
		return 0;
	}

	/* Get the import entry */
	const char* imports_base = chain_data + header->imports_offset;
	const char* symbols_base = chain_data + header->symbols_offset;

	int lib_ordinal;
	int weak;
	uint32_t name_offset;

	switch (header->imports_format) {
	case DYLD_CHAINED_IMPORT: {
		const struct dyld_chained_import* imp =
			&((const struct dyld_chained_import*)imports_base)[ordinal];
		lib_ordinal = (int8_t)imp->lib_ordinal; /* sign-extend 8-bit */
		weak = imp->weak_import;
		name_offset = imp->name_offset;
		break;
	}
	case DYLD_CHAINED_IMPORT_ADDEND: {
		const struct dyld_chained_import_addend* imp =
			&((const struct dyld_chained_import_addend*)imports_base)[ordinal];
		lib_ordinal = (int8_t)imp->lib_ordinal;
		weak = imp->weak_import;
		name_offset = imp->name_offset;
		addend += imp->addend;
		break;
	}
	case DYLD_CHAINED_IMPORT_ADDEND64: {
		const struct dyld_chained_import_addend64* imp =
			&((const struct dyld_chained_import_addend64*)imports_base)[ordinal];
		lib_ordinal = (int16_t)imp->lib_ordinal;
		weak = imp->weak_import;
		name_offset = imp->name_offset;
		addend += imp->addend;
		break;
	}
	default:
		fprintf(stderr, "resolver: unsupported import format %u\n", header->imports_format);
		return 0;
	}

	const char* sym_name = symbols_base + name_offset;

	/* Strip leading underscore (Mach-O convention) */
	const char* lookup_name = sym_name;
	if (lookup_name[0] == '_')
		lookup_name++;

	if (strstr(sym_name, "registr") && strstr(sym_name, "s_instance") && !strstr(sym_name, "ZGV"))
		fprintf(stderr, "resolver: TRACE s_instance: ordinal=%u lib_ordinal=%d weak=%d name='%s'\n",
				ordinal, lib_ordinal, weak, sym_name);

	/* Special ordinals */
	if (lib_ordinal == -1) {
		/* BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE — look up in our own symbol table */
		uintptr_t addr = lookup_macho_symbol(rs, sym_name);
		if (addr) {
			rs->binds_resolved++;
			return addr + addend;
		}
		rs->binds_failed++;
		goto alloc_stub_slot;
	}
	if (lib_ordinal == -2) {
		/* BIND_SPECIAL_DYLIB_FLAT_LOOKUP — search all */
		void* addr = dlsym(RTLD_DEFAULT, lookup_name);
		if (addr) {
			uintptr_t result = (uintptr_t)addr + addend;
			if (is_ctor_or_dtor(sym_name) && !ctor_has_stack_params(sym_name))
				result = wrap_ctor_for_apple_abi(result);
			rs->binds_resolved++;
			return result;
		}
		if (!weak) rs->binds_failed++;
		goto alloc_stub_slot;
	}
	if (lib_ordinal == -3) {
		/* BIND_SPECIAL_DYLIB_WEAK_LOOKUP — search Mach-O binary first, then .so files */
		uintptr_t macho_addr = lookup_macho_symbol(rs, sym_name);
		if (macho_addr) {
			rs->binds_resolved++;
			return macho_addr + addend;
		}
		void* addr = dlsym(RTLD_DEFAULT, lookup_name);
		if (addr) {
			uintptr_t result = (uintptr_t)addr + addend;
			if (is_ctor_or_dtor(sym_name) && !ctor_has_stack_params(sym_name))
				result = wrap_ctor_for_apple_abi(result);
			rs->binds_resolved++;
			if (result == 0)
				fprintf(stderr, "resolver: WARNING: weak sym '%s' resolved to %p + addend %ld = 0!\n",
						sym_name, addr, (long)addend);
			return result;
		}
		/* For unresolved weak data references (guard variables, etc.),
		 * provide a unique zero-initialized slot for each bind so that
		 * writes to one don't corrupt another (e.g., atomic counter vs mutex). */
		static uint8_t* weak_pool = NULL;
		static size_t weak_pool_used = 0;
		static size_t weak_pool_capacity = 0;
		if (!weak_pool) {
			weak_pool_capacity = 64 * 4096; /* 256KB — room for many weak slots */
			weak_pool = mmap(NULL, weak_pool_capacity, PROT_READ | PROT_WRITE,
			                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
			if (weak_pool == MAP_FAILED) weak_pool = NULL;
		}
		if (weak_pool) {
			/* Allocate 128 bytes per weak symbol (enough for any pthread type) */
			size_t slot_size = 128;
			if (weak_pool_used + slot_size <= weak_pool_capacity) {
				uintptr_t slot = (uintptr_t)(weak_pool + weak_pool_used);
				weak_pool_used += slot_size;
				rs->binds_stubbed++;
				if (strstr(sym_name, "s_instance"))
					fprintf(stderr, "resolver: WEAK POOL gave %s -> %p\n", sym_name, (void*)slot);
				return slot;
			}
		}
		rs->binds_stubbed++;
		goto alloc_stub_slot;
	}

	/* Normal library ordinal (1-based) */
	if (lib_ordinal < 1 || lib_ordinal > rs->ndylibs) {
		fprintf(stderr, "resolver: bind ordinal %u references lib_ordinal %d (out of range)\n",
				ordinal, lib_ordinal);
		rs->binds_failed++;
		goto alloc_stub_slot;
	}

	struct dylib_entry* de = &rs->dylibs[lib_ordinal - 1];

	switch (de->action) {
	case DYLIB_MAP: {
		void* addr = dlsym(de->handle, lookup_name);
		/* Fallback: try macOS↔Linux C++ mangling variants */
		if (!addr && strncmp(lookup_name, "_ZN", 3) == 0)
			addr = try_mangling_variants(de->handle, lookup_name);
		if (addr) {
			uintptr_t result = (uintptr_t)addr + addend;
			/* Wrap C++ ctors/dtors for Apple ARM64 ABI compatibility:
			 * Apple ABI returns `this` in x0, Linux ABI returns void */
			if (is_ctor_or_dtor(sym_name)) {
				if (ctor_has_stack_params(sym_name)) {
					fprintf(stderr, "resolver: ctor/dtor SKIP adapter (stack params): %s\n", sym_name);
				} else {
					fprintf(stderr, "resolver: ctor/dtor ABI wrap: %s\n", sym_name);
					result = wrap_ctor_for_apple_abi(result);
				}
			}
			rs->binds_resolved++;
			return result;
		}
		if (!weak) {
			/* Only print for first few failures to avoid spam */
			if (rs->binds_failed < 20) {
				fprintf(stderr, "resolver: symbol '%s' not found in '%s'\n",
						lookup_name, de->so_path);
			}
			rs->binds_failed++;
		} else {
			rs->binds_stubbed++;
		}
		goto alloc_stub_slot;
	}
	case DYLIB_MACHO: {
		/* Look up symbol in loaded Mach-O dylib's LC_SYMTAB.
		 * Mach-O symbols have a leading underscore, but lookup_name
		 * has already been stripped. Use the original sym_name. */
		uintptr_t addr = dylib_loader_lookup(de->macho_info, sym_name);
		/* Fallback: try macOS↔Linux C++ mangling variants */
		if (!addr && strstr(sym_name, "_ZN")) {
			/* For Mach-O→Mach-O, mangling should match, but try anyway */
			char* variant = strdup(sym_name);
			if (variant) {
				for (char* c = variant; *c; c++) {
					if (*c == 'y') { *c = 'm'; break; }
					else if (*c == 'm') { *c = 'y'; break; }
				}
				addr = dylib_loader_lookup(de->macho_info, variant);
				free(variant);
			}
		}
		if (addr) {
			/* NO ctor/dtor ABI adapter needed — both sides are Apple ABI.
			 * The caller (Mach-O) expects ctor to return this in x0,
			 * and the callee (Mach-O dylib) does return this in x0. */
			rs->binds_resolved++;
			return addr + addend;
		}
		if (!weak) {
			if (rs->binds_failed < 20) {
				fprintf(stderr, "resolver: symbol '%s' not found in MACHO '%s'\n",
						lookup_name, de->so_path);
			}
			rs->binds_failed++;
		} else {
			rs->binds_stubbed++;
		}
		goto alloc_stub_slot;
	}
	case DYLIB_STUB:
		rs->binds_stubbed++;
		goto alloc_stub_slot;
	case DYLIB_SKIP:
		if (!weak) {
			rs->binds_failed++;
		} else {
			rs->binds_stubbed++;
		}
		goto alloc_stub_slot;
	}

alloc_stub_slot:
	/* Provide a unique executable stub for unresolved binds.
	 * Each stub is a "mov w0, #0; ret" (return 0) so that stubbed
	 * functions don't crash when called through __stubs trampolines.
	 * Also safe as data: first 8 bytes are nonzero so NULL checks pass. */
	{
		static uint8_t* stub_pool = NULL;
		static size_t stub_pool_used = 0;
		static size_t stub_pool_capacity = 0;
		if (!stub_pool) {
			stub_pool_capacity = 256 * 4096; /* 1MB */
			stub_pool = mmap(NULL, stub_pool_capacity,
			                 PROT_READ | PROT_WRITE | PROT_EXEC,
			                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
			if (stub_pool == MAP_FAILED) stub_pool = NULL;
		}
		if (stub_pool && stub_pool_used + 128 <= stub_pool_capacity) {
			uintptr_t slot = (uintptr_t)(stub_pool + stub_pool_used);
			/* Write arm64: mov w0, #0; ret */
			uint32_t* code = (uint32_t*)(stub_pool + stub_pool_used);
			code[0] = 0x52800000; /* mov w0, #0 */
			code[1] = 0xD65F03C0; /* ret */
			stub_pool_used += 128;
			return slot;
		}
	}
	return 0;
}

/* ---- Walk a single fixup chain ---- */

static int walk_chain(struct resolver_state* rs, uint64_t* chain_start, uint16_t pointer_format,
                      const struct dyld_chained_fixups_header* header, const char* chain_data)
{
	uint64_t* loc = chain_start;
	bool is_offset = (pointer_format == DYLD_CHAINED_PTR_64_OFFSET);

	for (;;) {
		uint64_t raw = *loc;
		uint32_t next = FIXUP_PTR_NEXT(raw);

		if (FIXUP_PTR_BIND(raw)) {
			/* Bind: resolve symbol from a dylib */
			uint32_t ordinal = FIXUP_BIND_ORDINAL(raw);
			int64_t addend = (int64_t)(int8_t)FIXUP_BIND_ADDEND(raw);

			uintptr_t resolved = resolve_import(rs, ordinal, header, chain_data, addend);
			if (resolved == 0 && getenv("MACHISMO_VERBOSE_BINDS")) {
				/* Log unresolved binds for debugging */
				const char* symbols_base = chain_data + header->symbols_offset;
				const char* imports_base = chain_data + header->imports_offset;
				const struct dyld_chained_import* imp =
					&((const struct dyld_chained_import*)imports_base)[ordinal];
				const char* sym_name = &symbols_base[imp->name_offset];
				fprintf(stderr, "resolver: NULL bind at %p: %s (ordinal %u, lib_ordinal %d)\n",
						(void*)loc, sym_name, ordinal, (int)(int8_t)imp->lib_ordinal);
			}
			*loc = resolved;
		} else {
			/* Rebase: fix up an internal pointer for ASLR slide */
			uint64_t target = FIXUP_REBASE_TARGET(raw);
			uint8_t high8 = FIXUP_REBASE_HIGH8(raw);

			uintptr_t value;
			if (is_offset) {
				/* PTR_64_OFFSET: target is offset from mach header */
				value = rs->mh_addr + target;
			} else {
				/* PTR_64: target is vmaddr, apply slide */
				value = target + rs->slide;
			}

			/* Apply high8 */
			value |= ((uintptr_t)high8 << 56);

			*loc = value;
			rs->rebases_applied++;
		}

		if (next == 0)
			break;

		/* Stride is 4 bytes for DYLD_CHAINED_PTR_64 and PTR_64_OFFSET */
		loc = (uint64_t*)((uint8_t*)loc + (next * 4));
	}

	return 0;
}

/* ---- Close dlopen handles ---- */

static void close_dylibs(struct resolver_state* rs)
{
	/* Don't actually close — the game needs these to stay loaded */
	(void)rs;
}

/* ---- Utility: extract basename from dylib path ---- */

static const char* basename_from_path(const char* path)
{
	const char* slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}
