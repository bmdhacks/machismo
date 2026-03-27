#include <stdint.h>
#include "macho_defs.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#include "loader.h"

static int native_prot(int prot);
static void load(const char* path, cpu_type_t cpu, bool expect_dylinker, char** argv, struct load_results* lr);
static void setup_space(struct load_results* lr, bool is_64_bit);

#ifndef PAGE_SIZE
#	define PAGE_SIZE	4096
#endif
#define PAGE_ALIGN(x) ((x) & ~(PAGE_SIZE-1))
#define PAGE_ROUNDUP(x) (((((x)-1) / PAGE_SIZE)+1) * PAGE_SIZE)

/* VM_PROT_* defined in macho_defs.h */

/*
 * We only generate the 64-bit loader for aarch64.
 * This file is #included from mldr.c with GEN_64BIT defined.
 */

#if defined(GEN_64BIT)
#   define FUNCTION_NAME load64
#   define SEGMENT_STRUCT segment_command_64
#   define SEGMENT_COMMAND LC_SEGMENT_64
#   define MACH_HEADER_STRUCT mach_header_64
#   define SECTION_STRUCT section_64
#	define MAP_EXTRA 0
#elif defined(GEN_32BIT)
#   define FUNCTION_NAME load32
#   define SEGMENT_STRUCT segment_command
#   define SEGMENT_COMMAND LC_SEGMENT
#   define MACH_HEADER_STRUCT mach_header
#   define SECTION_STRUCT section
#	define MAP_EXTRA MAP_32BIT
#else
#   error See above
#endif

void FUNCTION_NAME(int fd, bool expect_dylinker, struct load_results* lr)
{
	struct MACH_HEADER_STRUCT header;
	uint8_t* cmds;
	uintptr_t entryPoint = 0, entryPointDylinker = 0;
	struct MACH_HEADER_STRUCT* mappedHeader = NULL;
	uintptr_t slide = 0;
	uintptr_t mmapSize = 0;
	uint32_t fat_offset;
	void* tmp_map_base = NULL;

	(void)entryPointDylinker;

	if (!expect_dylinker)
	{
		setup_space(lr, true);
	}

	fat_offset = lseek(fd, 0, SEEK_CUR);

	if (read(fd, &header, sizeof(header)) != sizeof(header))
	{
		fprintf(stderr, "Cannot read the mach header.\n");
		exit(1);
	}

	if (header.filetype != (expect_dylinker ? MH_DYLINKER : MH_EXECUTE))
	{
		fprintf(stderr, "Found unexpected Mach-O file type: %u\n", header.filetype);
		exit(1);
	}

	tmp_map_base = mmap(NULL, PAGE_ROUNDUP(sizeof(header) + header.sizeofcmds), PROT_READ, MAP_PRIVATE, fd, fat_offset);
	if (tmp_map_base == MAP_FAILED) {
		fprintf(stderr, "Failed to mmap header + commands\n");
		exit(1);
	}

	cmds = (void*)((char*)tmp_map_base + sizeof(header));

	/* All macOS arm64 binaries are PIE — Apple has required it since
	 * OS X 10.7, and ARM Macs (M1+) only exist on macOS 11+. */
	if (!(header.flags & MH_PIE) && header.filetype != MH_DYLINKER) {
		fprintf(stderr, "machismo: non-PIE binaries are not supported\n");
		exit(1);
	}

	{
		uintptr_t base = -1;

		for (uint32_t i = 0, p = 0; i < header.ncmds; i++)
		{
			struct SEGMENT_STRUCT* seg = (struct SEGMENT_STRUCT*) &cmds[p];

			if (seg->cmd == SEGMENT_COMMAND && strcmp(seg->segname, "__PAGEZERO") != 0)
			{
				if (base == (uintptr_t)-1)
				{
					base = seg->vmaddr;
				}
				mmapSize = seg->vmaddr + seg->vmsize - base;
			}

			p += seg->cmdsize;
		}

		/* Reserve the segment range plus extra space for branch island
		 * pools (LSE emulation, trampolines).  The pool tail is kept
		 * mapped so it is guaranteed adjacent to __TEXT. */
		uintptr_t page_sz = sysconf(_SC_PAGESIZE);
		size_t mmapAligned = (mmapSize + page_sz - 1) & ~(page_sz - 1);
		size_t totalSize = mmapAligned + MACHISMO_POOL_PADDING;
		slide = (uintptr_t) mmap((void*) base, totalSize, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_EXTRA, -1, 0);
		if (slide == (uintptr_t)MAP_FAILED)
		{
			fprintf(stderr, "Cannot mmap anonymous memory range: %s\n", strerror(errno));
			exit(1);
		}

		/* Unmap only the segment portion (page-aligned) — keep pool tail */
		munmap((void*)slide, mmapAligned);

		/* Make the pool tail RWX for branch islands */
		void* pool_base = (void*)(slide + mmapAligned);
		mprotect(pool_base, MACHISMO_POOL_PADDING,
		         PROT_READ | PROT_WRITE | PROT_EXEC);
		lr->pool_base = pool_base;
		lr->pool_size = MACHISMO_POOL_PADDING;
		lr->pool_used = 0;

		if (slide + totalSize > lr->vm_addr_max)
			lr->vm_addr_max = lr->base = slide + totalSize;
		slide -= base;
	}

	for (uint32_t i = 0, p = 0; i < header.ncmds && p < header.sizeofcmds; i++)
	{
		struct load_command* lc;

		lc = (struct load_command*) &cmds[p];

		switch (lc->cmd)
		{
			case SEGMENT_COMMAND:
			{
				struct SEGMENT_STRUCT* seg = (struct SEGMENT_STRUCT*) lc;
				void* rv;

				int maxprot = native_prot(seg->maxprot);
				int initprot = native_prot(seg->initprot);
				int useprot = (initprot & PROT_EXEC) ? maxprot : initprot;

				if (seg->filesize < seg->vmsize)
				{
					unsigned long addr = seg->vmaddr;

					if (addr != 0)
						addr += slide;

					/* MAP_FIXED is safe — PIE reservation guarantees this range is free */
					rv = mmap((void*)addr, seg->vmsize, useprot, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
					if (rv == (void*)MAP_FAILED)
					{
						if (seg->vmaddr == 0 && useprot == 0) {
							rv = 0;
						} else {
							fprintf(stderr, "Cannot mmap segment %s at %p: %s\n", seg->segname, (void*)(uintptr_t)seg->vmaddr, strerror(errno));
							exit(1);
						}
					}
				}

				if (seg->filesize > 0)
				{
					unsigned long addr = seg->vmaddr + slide;
					rv = mmap((void*)addr, seg->filesize, useprot,
							MAP_FIXED | MAP_PRIVATE, fd, seg->fileoff + fat_offset);
					if (rv == (void*)MAP_FAILED)
					{
						if (seg->vmaddr == 0 && useprot == 0) {
							rv = 0;
						} else {
							fprintf(stderr, "Cannot mmap segment %s at %p: %s\n", seg->segname, (void*)(uintptr_t)seg->vmaddr, strerror(errno));
							exit(1);
						}
					}

					if (seg->fileoff == 0)
						mappedHeader = (struct MACH_HEADER_STRUCT*) (seg->vmaddr + slide);
				}

				if (seg->vmaddr + slide + seg->vmsize > lr->vm_addr_max)
					lr->vm_addr_max = seg->vmaddr + slide + seg->vmsize;

				if (strcmp(SEG_DATA, seg->segname) == 0)
				{
					struct SECTION_STRUCT* sect = (struct SECTION_STRUCT*) (seg+1);
					struct SECTION_STRUCT* end = (struct SECTION_STRUCT*) (&cmds[p + lc->cmdsize]);

					while (sect < end)
					{
						if (strncmp(sect->sectname, "__all_image_info", 16) == 0)
						{
							lr->dyld_all_image_location = slide + sect->addr;
							lr->dyld_all_image_size = sect->size;
							break;
						}
						sect++;
					}
				}
				break;
			}
			case LC_UNIXTHREAD:
			{
				/*
				 * On arm64, the thread state in LC_UNIXTHREAD is arm_thread_state64_t.
				 * Layout: flavor(4) + count(4) + 33 x uint64_t registers (x0-x28, fp, lr, sp, pc)
				 * pc is at offset: 2 (lc header) + 2 (flavor+count) + 32 (x0-x28,fp,lr,sp) = index 36
				 * In uint64_t units from start of lc: lc is 8 bytes header = 1 uint64_t,
				 * plus flavor+count = 8 bytes = 1 uint64_t, so pc is at uint64_t[34]
				 *
				 * For x86_64: pc (rip) is at uint64_t[18] (original Darling code)
				 */
#ifdef __aarch64__
				entryPoint = ((uint64_t*) lc)[34];
#elif defined(__x86_64__)
#ifdef GEN_64BIT
				entryPoint = ((uint64_t*) lc)[18];
#endif
#endif
#ifdef GEN_32BIT
				entryPoint = ((uint32_t*) lc)[14];
#endif
				entryPoint += slide;
				break;
			}
			case LC_LOAD_DYLINKER:
			{
				/* In standalone mode, we don't load Apple's dyld.
				 * Just warn and continue — we'll handle linking ourselves. */
				struct dylinker_command* dy = (struct dylinker_command*) lc;
				const char* dylinker_name = ((char*) dy) + dy->name.offset;
				fprintf(stderr, "machismo: LC_LOAD_DYLINKER found (%s) — ignored in standalone mode\n", dylinker_name);
				break;
			}
			case LC_MAIN:
			{
				struct entry_point_command* ee = (struct entry_point_command*) lc;
				if (ee->stacksize > lr->stack_size)
					lr->stack_size = ee->stacksize;
				lr->lc_main = true;
				/* Compute entry point: mh base + entryoff */
				/* We set this after all segments are mapped, using mappedHeader */
				/* For now, store the offset and resolve after the loop */
				if (mappedHeader) {
					entryPoint = (uintptr_t)mappedHeader + ee->entryoff;
				} else {
					/* mappedHeader not set yet — defer. Store negative offset as marker. */
					/* We'll fix this up after the loop. */
					lr->stack_size = ee->stacksize;
					/* Store entryoff in entry_point temporarily; we'll add mh later */
					entryPoint = ee->entryoff;
					/* Mark that this is relative, not absolute */
					entryPointDylinker = 1; /* reuse as "needs fixup" flag */
				}
				break;
			}
			case LC_UUID:
			{
				if (header.filetype == MH_EXECUTE)
				{
					struct uuid_command* ue = (struct uuid_command*) lc;
					memcpy(lr->uuid, ue->uuid, sizeof(ue->uuid));
				}
				break;
			}
		}

		p += lc->cmdsize;
	}

	if (header.filetype == MH_EXECUTE) {
		lr->mh = (uintptr_t) mappedHeader;
		lr->slide = slide;
	}

	/* Fix up LC_MAIN entry point if mappedHeader wasn't available during parsing */
	if (entryPointDylinker == 1 && mappedHeader) {
		entryPoint = (uintptr_t)mappedHeader + entryPoint;
		entryPointDylinker = 0;
	}

	if (entryPoint && !lr->entry_point)
		lr->entry_point = entryPoint;

	if (tmp_map_base)
		munmap(tmp_map_base, PAGE_ROUNDUP(sizeof(header) + header.sizeofcmds));
}


#undef FUNCTION_NAME
#undef SEGMENT_STRUCT
#undef SEGMENT_COMMAND
#undef MACH_HEADER_STRUCT
#undef SECTION_STRUCT
#undef MAP_EXTRA
