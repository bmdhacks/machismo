#ifndef _MACHISMO_LOADER_H_
#define _MACHISMO_LOADER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
};

#endif // _MACHISMO_LOADER_H_
