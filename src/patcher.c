/*
 * Generic binary patcher for loaded Mach-O binaries.
 *
 * Applies instruction-level patches from a config file to the loaded
 * binary's memory. Each patch overwrites 4 bytes (one arm64 instruction)
 * at a specified virtual address.
 */

#include "patcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

int patcher_apply(const char* patch_file, uintptr_t slide)
{
	FILE* f = fopen(patch_file, "r");
	if (!f) {
		fprintf(stderr, "patcher: cannot open %s\n", patch_file);
		return -1;
	}

	long page_size = sysconf(_SC_PAGESIZE);
	int applied = 0;
	int line_num = 0;
	char line[256];

	while (fgets(line, sizeof(line), f)) {
		line_num++;

		/* Strip comments and whitespace */
		char* comment = strchr(line, '#');
		if (comment) *comment = '\0';

		/* Skip empty lines */
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0' || *p == '\n') continue;

		/* Parse: <hex_address> <hex_instruction> */
		uint64_t addr;
		uint32_t insn;
		if (sscanf(p, "0x%lx %x", &addr, &insn) != 2 &&
		    sscanf(p, "%lx %x", &addr, &insn) != 2) {
			fprintf(stderr, "patcher: %s:%d: bad format\n", patch_file, line_num);
			continue;
		}

		uintptr_t target = (uintptr_t)addr + slide;

		/* Make the page writable */
		uintptr_t page = target & ~(uintptr_t)(page_size - 1);
		if (mprotect((void*)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
			fprintf(stderr, "patcher: %s:%d: mprotect failed at 0x%lx\n",
			        patch_file, line_num, (unsigned long)target);
			continue;
		}

		/* Read old instruction for logging */
		uint32_t old_insn = *(uint32_t*)target;

		/* Write the patch */
		*(uint32_t*)target = insn;

		/* Flush instruction cache */
		__builtin___clear_cache((char*)target, (char*)(target + 4));

		/* Restore page protection */
		mprotect((void*)page, page_size, PROT_READ | PROT_EXEC);

		fprintf(stderr, "patcher: 0x%lx: %08x -> %08x\n",
		        (unsigned long)target, old_insn, insn);
		applied++;
	}

	fclose(f);
	fprintf(stderr, "patcher: applied %d patches from %s\n", applied, patch_file);
	return applied;
}
