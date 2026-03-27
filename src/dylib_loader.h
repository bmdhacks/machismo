#ifndef _DYLIB_LOADER_H_
#define _DYLIB_LOADER_H_

#include <stdint.h>
#include <stddef.h>
#include "macho_defs.h"

#define MAX_MACHO_DYLIBS 8

/* Pool padding reserved adjacent to dylib segments for branch islands
 * (LSE emulation, trampolines). Guaranteed within ±128MB of __TEXT. */
#define DYLIB_POOL_PADDING (64 * 1024)  /* 64KB per dylib */

struct macho_dylib_info {
	char path[256];
	struct mach_header_64 *mh;       /* mapped header in memory */
	uintptr_t slide;                 /* ASLR slide */
	uintptr_t text_base;             /* __TEXT vmaddr + slide */
	uintptr_t text_size;             /* __TEXT vmsize */

	/* Adjacent pool for branch islands */
	void *pool_base;
	size_t pool_size;
	size_t pool_used;

	/* LC_SYMTAB (mapped via __LINKEDIT) */
	struct nlist_64 *symtab;
	char *strtab;
	uint32_t nsyms;
	uint32_t strsize;
};

extern struct macho_dylib_info g_macho_dylibs[MAX_MACHO_DYLIBS];
extern int g_num_macho_dylibs;

/*
 * Load a Mach-O dylib from a fat/universal or thin binary.
 * Maps segments into memory, records symbol table location.
 * Returns pointer to the macho_dylib_info entry, or NULL on failure.
 */
struct macho_dylib_info *dylib_loader_load(const char *path);

/*
 * Look up an exported symbol in a loaded Mach-O dylib.
 * Returns the symbol's runtime address (vmaddr + slide), or 0 if not found.
 * The name should include the leading underscore (Mach-O convention).
 */
uintptr_t dylib_loader_lookup(struct macho_dylib_info *info, const char *name);

/*
 * Find an already-loaded Mach-O dylib by basename substring match.
 * Returns pointer to the macho_dylib_info, or NULL if not found.
 */
struct macho_dylib_info *dylib_loader_find(const char *basename);

/*
 * Run __mod_init_func and S_INIT_FUNC_OFFSETS initializers for a loaded dylib.
 * Must be called after the dylib's chained fixups have been resolved.
 */
void dylib_loader_run_inits(struct macho_dylib_info *info);

/* Allocate a chunk from the dylib's adjacent pool. Returns NULL if exhausted. */
static inline void *dylib_pool_alloc(struct macho_dylib_info *info, size_t size)
{
	size_t page = 4096;
	size_t aligned_off = (info->pool_used + page - 1) & ~(page - 1);
	if (!info->pool_base || aligned_off + size > info->pool_size)
		return NULL;
	void *ptr = (char *)info->pool_base + aligned_off;
	info->pool_used = aligned_off + size;
	return ptr;
}

#endif /* _DYLIB_LOADER_H_ */
