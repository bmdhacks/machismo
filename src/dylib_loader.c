/*
 * dylib_loader.c — Load Mach-O dylibs into memory on aarch64 Linux
 *
 * Loads .dylib files (fat or thin arm64 Mach-O) by mapping their segments
 * into the process address space. Unlike loader.c (which handles the main
 * executable), this has no entry point, stack, or commpage setup — dylibs
 * are just code + data that gets symbol-resolved and called into.
 *
 * Used to load the game's original macOS SFML dylibs for perfect ABI match,
 * avoiding vtable layout mismatches from our rebuilt SFML.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "dylib_loader.h"
#include "macho_defs.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define PAGE_ALIGN(x)   ((x) & ~(PAGE_SIZE - 1))
#define PAGE_ROUNDUP(x) (((((x)-1) / PAGE_SIZE) + 1) * PAGE_SIZE)

struct macho_dylib_info g_macho_dylibs[MAX_MACHO_DYLIBS];
int g_num_macho_dylibs = 0;

/* Convert Mach-O VM_PROT_* to Linux PROT_* */
static int native_prot(int vmprot)
{
	int p = 0;
	if (vmprot & 1) p |= PROT_READ;
	if (vmprot & 2) p |= PROT_WRITE;
	if (vmprot & 4) p |= PROT_EXEC;
	return p;
}

/*
 * Extract the arm64 slice offset and size from a fat binary.
 * Returns 0 on success, -1 if no arm64 slice found.
 */
static int find_arm64_slice(int fd, uint32_t *out_offset, uint32_t *out_size)
{
	uint32_t magic;
	lseek(fd, 0, SEEK_SET);
	if (read(fd, &magic, 4) != 4)
		return -1;

	if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
		/* Fat binary — search for arm64 slice */
		int swap = (magic == FAT_CIGAM);
		uint32_t narch;
		read(fd, &narch, 4);
		if (swap) narch = __builtin_bswap32(narch);

		for (uint32_t i = 0; i < narch; i++) {
			struct fat_arch fa;
			if (read(fd, &fa, sizeof(fa)) != sizeof(fa))
				return -1;
			uint32_t cpu = swap ? __builtin_bswap32(fa.cputype) : fa.cputype;
			uint32_t offset = swap ? __builtin_bswap32(fa.offset) : fa.offset;
			uint32_t size = swap ? __builtin_bswap32(fa.size) : fa.size;
			if (cpu == CPU_TYPE_ARM64) {
				*out_offset = offset;
				*out_size = size;
				return 0;
			}
		}
		fprintf(stderr, "dylib_loader: no arm64 slice in fat binary\n");
		return -1;
	}

	/* Not fat — assume thin binary, entire file is the slice */
	*out_offset = 0;
	struct stat st;
	fstat(fd, &st);
	*out_size = (uint32_t)st.st_size;
	return 0;
}

struct macho_dylib_info *dylib_loader_load(const char *path)
{
	if (g_num_macho_dylibs >= MAX_MACHO_DYLIBS) {
		fprintf(stderr, "dylib_loader: too many Mach-O dylibs (max %d)\n", MAX_MACHO_DYLIBS);
		return NULL;
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "dylib_loader: cannot open '%s': %s\n", path, strerror(errno));
		return NULL;
	}

	/* Find arm64 slice in fat binary */
	uint32_t fat_offset = 0, fat_size = 0;
	if (find_arm64_slice(fd, &fat_offset, &fat_size) < 0) {
		close(fd);
		return NULL;
	}

	/* Read Mach-O header */
	struct mach_header_64 header;
	lseek(fd, fat_offset, SEEK_SET);
	if (read(fd, &header, sizeof(header)) != sizeof(header)) {
		fprintf(stderr, "dylib_loader: cannot read mach header from '%s'\n", path);
		close(fd);
		return NULL;
	}

	if (header.magic != MH_MAGIC_64) {
		fprintf(stderr, "dylib_loader: bad magic 0x%x in '%s'\n", header.magic, path);
		close(fd);
		return NULL;
	}

	if (header.filetype != MH_DYLIB) {
		fprintf(stderr, "dylib_loader: expected MH_DYLIB, got filetype %u in '%s'\n",
		        header.filetype, path);
		close(fd);
		return NULL;
	}

	/* mmap the load commands for parsing */
	size_t cmds_total = sizeof(header) + header.sizeofcmds;
	void *cmds_map = mmap(NULL, PAGE_ROUNDUP(cmds_total), PROT_READ, MAP_PRIVATE, fd, fat_offset);
	if (cmds_map == MAP_FAILED) {
		fprintf(stderr, "dylib_loader: cannot mmap commands: %s\n", strerror(errno));
		close(fd);
		return NULL;
	}
	uint8_t *cmds = (uint8_t *)cmds_map + sizeof(header);

	/*
	 * Pass 1: Calculate total VM span for reservation.
	 * Dylibs are always PIC, so we always compute a slide.
	 */
	uintptr_t vm_low = (uintptr_t)-1, vm_high = 0;
	for (uint32_t i = 0, p = 0; i < header.ncmds && p < header.sizeofcmds; i++) {
		struct segment_command_64 *seg = (struct segment_command_64 *)&cmds[p];
		if (seg->cmd == LC_SEGMENT_64 && strcmp(seg->segname, "__PAGEZERO") != 0) {
			if (seg->vmaddr < vm_low)
				vm_low = seg->vmaddr;
			if (seg->vmaddr + seg->vmsize > vm_high)
				vm_high = seg->vmaddr + seg->vmsize;
		}
		p += seg->cmdsize;
	}

	if (vm_low == (uintptr_t)-1 || vm_high == 0) {
		fprintf(stderr, "dylib_loader: no loadable segments in '%s'\n", path);
		munmap(cmds_map, PAGE_ROUNDUP(cmds_total));
		close(fd);
		return NULL;
	}

	uintptr_t vm_span = vm_high - vm_low;

	/* Reserve address space, then release (to get a base address) */
	void *reservation = mmap(NULL, vm_span, PROT_NONE,
	                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (reservation == MAP_FAILED) {
		fprintf(stderr, "dylib_loader: cannot reserve %lu bytes: %s\n",
		        (unsigned long)vm_span, strerror(errno));
		munmap(cmds_map, PAGE_ROUNDUP(cmds_total));
		close(fd);
		return NULL;
	}
	munmap(reservation, vm_span);
	uintptr_t slide = (uintptr_t)reservation - vm_low;

	/*
	 * Pass 2: Map each segment.
	 */
	struct mach_header_64 *mapped_header = NULL;
	struct macho_dylib_info *info = &g_macho_dylibs[g_num_macho_dylibs];
	memset(info, 0, sizeof(*info));

	for (uint32_t i = 0, p = 0; i < header.ncmds && p < header.sizeofcmds; i++) {
		struct segment_command_64 *seg = (struct segment_command_64 *)&cmds[p];

		if (seg->cmd == LC_SEGMENT_64 && strcmp(seg->segname, "__PAGEZERO") != 0) {
			uintptr_t addr = seg->vmaddr + slide;
			int maxprot = native_prot(seg->maxprot);
			int initprot = native_prot(seg->initprot);
			int useprot = (initprot & PROT_EXEC) ? maxprot : initprot;

			/* Map anonymous for zero-fill portion */
			if (seg->vmsize > 0) {
				void *rv = mmap((void *)addr, seg->vmsize, useprot,
				                MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
				if (rv == MAP_FAILED) {
					fprintf(stderr, "dylib_loader: cannot mmap segment %s at %p: %s\n",
					        seg->segname, (void *)addr, strerror(errno));
					munmap(cmds_map, PAGE_ROUNDUP(cmds_total));
					close(fd);
					return NULL;
				}
			}

			/* Map file-backed portion over the anonymous mapping */
			if (seg->filesize > 0) {
				void *rv = mmap((void *)addr, seg->filesize, useprot,
				                MAP_FIXED | MAP_PRIVATE, fd, seg->fileoff + fat_offset);
				if (rv == MAP_FAILED) {
					fprintf(stderr, "dylib_loader: cannot mmap segment %s file data: %s\n",
					        seg->segname, strerror(errno));
					munmap(cmds_map, PAGE_ROUNDUP(cmds_total));
					close(fd);
					return NULL;
				}

				if (seg->fileoff == 0)
					mapped_header = (struct mach_header_64 *)addr;
			}

			/* Record __TEXT info */
			if (strcmp(seg->segname, "__TEXT") == 0) {
				info->text_base = addr;
				info->text_size = seg->vmsize;
			}
		}

		p += seg->cmdsize;
	}

	if (!mapped_header) {
		fprintf(stderr, "dylib_loader: no mapped header found in '%s'\n", path);
		munmap(cmds_map, PAGE_ROUNDUP(cmds_total));
		close(fd);
		return NULL;
	}

	/*
	 * Pass 3: Find LC_SYMTAB and locate symbol/string tables via __LINKEDIT.
	 */
	struct symtab_command *symtab_cmd = NULL;
	uint8_t *mapped_cmds = (uint8_t *)(mapped_header + 1);

	for (uint32_t i = 0, p = 0; i < mapped_header->ncmds && p < mapped_header->sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)&mapped_cmds[p];
		if (lc->cmd == LC_SYMTAB)
			symtab_cmd = (struct symtab_command *)lc;
		p += lc->cmdsize;
	}

	if (symtab_cmd) {
		/* Find the segment containing the symtab file offsets (usually __LINKEDIT) */
		for (uint32_t i = 0, p = 0; i < mapped_header->ncmds && p < mapped_header->sizeofcmds; i++) {
			struct load_command *lc = (struct load_command *)&mapped_cmds[p];
			if (lc->cmd == LC_SEGMENT_64) {
				struct segment_command_64 *seg = (struct segment_command_64 *)lc;
				if (symtab_cmd->symoff >= seg->fileoff &&
				    symtab_cmd->symoff < seg->fileoff + seg->filesize) {
					uintptr_t base = seg->vmaddr + slide;
					info->symtab = (struct nlist_64 *)(base + (symtab_cmd->symoff - seg->fileoff));
					info->strtab = (char *)(base + (symtab_cmd->stroff - seg->fileoff));
					info->nsyms = symtab_cmd->nsyms;
					info->strsize = symtab_cmd->strsize;
				}
			}
			p += lc->cmdsize;
		}
	}

	/* Fill info struct */
	strncpy(info->path, path, sizeof(info->path) - 1);
	info->mh = mapped_header;
	info->slide = slide;

	g_num_macho_dylibs++;

	munmap(cmds_map, PAGE_ROUNDUP(cmds_total));
	close(fd);

	fprintf(stderr, "dylib_loader: loaded '%s' at %p (slide=0x%lx, %u symbols)\n",
	        path, (void *)mapped_header, (unsigned long)slide, info->nsyms);

	return info;
}

uintptr_t dylib_loader_lookup(struct macho_dylib_info *info, const char *name)
{
	if (!info || !info->symtab || !info->strtab)
		return 0;

	for (uint32_t i = 0; i < info->nsyms; i++) {
		struct nlist_64 *nl = &info->symtab[i];
		/* Skip debug/stab entries */
		if (nl->n_type & N_STAB) continue;
		/* Only look at defined symbols in a section */
		if ((nl->n_type & N_TYPE) != N_SECT) continue;
		/* Must be externally visible */
		if (!(nl->n_type & N_EXT)) continue;
		if (nl->n_strx >= info->strsize) continue;

		const char *sym = &info->strtab[nl->n_strx];
		if (strcmp(sym, name) == 0)
			return nl->n_value + info->slide;
	}
	return 0;
}

struct macho_dylib_info *dylib_loader_find(const char *basename)
{
	for (int i = 0; i < g_num_macho_dylibs; i++) {
		if (strstr(g_macho_dylibs[i].path, basename))
			return &g_macho_dylibs[i];
	}
	return NULL;
}

void dylib_loader_run_inits(struct macho_dylib_info *info)
{
	if (!info || !info->mh)
		return;

	struct mach_header_64 *mh = info->mh;
	uint8_t *cmds = (uint8_t *)(mh + 1);
	uint32_t p = 0;
	typedef void (*init_func_t)(void);

	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)&cmds[p];

		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *seg = (struct segment_command_64 *)lc;
			struct section_64 *sect = (struct section_64 *)(seg + 1);
			uint32_t nsects = seg->nsects;

			for (uint32_t j = 0; j < nsects; j++, sect++) {
				uint32_t sect_type = sect->flags & 0xff;

				if (sect_type == 0x16) {
					/* S_INIT_FUNC_OFFSETS: 32-bit offsets from mh */
					uint32_t *offsets = (uint32_t *)(sect->addr + info->slide);
					int count = sect->size / sizeof(uint32_t);
					fprintf(stderr, "dylib_loader: running %d init_offsets from '%s'\n",
					        count, info->path);
					for (int k = 0; k < count; k++) {
						uintptr_t func_addr = (uintptr_t)mh + offsets[k];
						((init_func_t)func_addr)();
					}
				} else if (sect_type == 0x09) {
					/* S_MOD_INIT_FUNC_POINTERS: array of function pointers */
					uintptr_t *funcs = (uintptr_t *)(sect->addr + info->slide);
					int count = sect->size / sizeof(uintptr_t);
					fprintf(stderr, "dylib_loader: running %d mod_init_funcs from '%s'\n",
					        count, info->path);
					for (int k = 0; k < count; k++) {
						/* Function pointers are already rebased by fixup resolution */
						((init_func_t)funcs[k])();
					}
				}
			}
		}

		p += lc->cmdsize;
	}
}
