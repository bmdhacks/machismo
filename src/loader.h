#ifndef _MACHISMO_LOADER_H_
#define _MACHISMO_LOADER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Pool space reserved adjacent to the Mach-O segments for branch islands.
 * Guaranteed within ±128MB of __TEXT. Carved out by the LSE and trampoline
 * code via machismo_pool_alloc(). */
#define MACHISMO_POOL_PADDING (2 * 1024 * 1024)  /* 2MB */

struct load_results {
	unsigned long mh;
	unsigned long entry_point;
	unsigned long stack_size;
	unsigned long dyld_all_image_location;
	unsigned long dyld_all_image_size;
	uint8_t uuid[16];

	unsigned long vm_addr_max;
	bool _32on64;
	unsigned long base;
	uint32_t bprefs[4];
	char* root_path;
	size_t root_path_length;
	unsigned long stack_top;
	unsigned long slide;
	bool lc_main;  /* true if entry is via LC_MAIN (C calling convention) */
	char** applep; /* apple[] parameter for LC_MAIN calling convention */
	int kernfd;

	size_t argc;
	size_t envc;
	char** argv;
	char** envp;

	/* Adjacent pool for branch islands (LSE emulation, trampolines) */
	void* pool_base;
	size_t pool_size;
	size_t pool_used;
};

/* Allocate a chunk from the adjacent pool. Returns NULL if exhausted. */
static inline void* machismo_pool_alloc(struct load_results* lr, size_t size)
{
	size_t page = 4096;
	size_t aligned_off = (lr->pool_used + page - 1) & ~(page - 1);
	if (!lr->pool_base || aligned_off + size > lr->pool_size)
		return NULL;
	void* ptr = (char*)lr->pool_base + aligned_off;
	lr->pool_used = aligned_off + size;
	return ptr;
}

#endif // _MACHISMO_LOADER_H_
