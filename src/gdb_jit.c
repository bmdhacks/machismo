/*
 * GDB JIT Debug Symbol Registration for Mach-O binaries.
 *
 * Implements the GDB JIT Compilation Interface to register Mach-O symbols
 * with GDB at runtime. Parses LC_SYMTAB, builds a minimal in-memory ELF
 * object with .symtab/.strtab sections, and notifies GDB.
 *
 * Reference: HashLink JIT (jit_elf.c) and
 * https://bernsteinbear.com/blog/gdb-jit/
 */

#include "gdb_jit.h"
#include "macho_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ========== GDB JIT Interface ========== */

typedef enum {
	JIT_NOACTION = 0,
	JIT_REGISTER_FN,
	JIT_UNREGISTER_FN
} jit_actions_t;

struct jit_code_entry {
	struct jit_code_entry *next_entry;
	struct jit_code_entry *prev_entry;
	const char *symfile_addr;
	uint64_t symfile_size;
};

struct jit_descriptor {
	uint32_t version;
	uint32_t action_flag;
	struct jit_code_entry *relevant_entry;
	struct jit_code_entry *first_entry;
};

/* These symbols MUST have these exact names and NOT be static.
 * GDB looks for them by name. */
struct jit_descriptor __jit_debug_descriptor = {1, JIT_NOACTION, NULL, NULL};

__attribute__((noinline)) void __jit_debug_register_code(void) {
	__asm__ volatile("" ::: "memory");
}

/* ========== ELF Constants ========== */

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_REL          1
#define EM_AARCH64      183

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_NOBITS      8

#define SHF_ALLOC       2
#define SHF_EXECINSTR   4

#define STB_GLOBAL      1
#define STT_FUNC        2
#define STT_NOTYPE      0

#define ELF64_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0xf))

/* ELF64 structures */
typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t sh_name;
	uint32_t sh_type;
	uint64_t sh_flags;
	uint64_t sh_addr;
	uint64_t sh_offset;
	uint64_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint64_t sh_addralign;
	uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
	uint32_t st_name;
	unsigned char st_info;
	unsigned char st_other;
	uint16_t st_shndx;
	uint64_t st_value;
	uint64_t st_size;
} Elf64_Sym;

/* ========== ELF Writer ========== */

typedef struct {
	unsigned char *buf;
	int pos;
	int size;
} elf_writer;

static void ew_init(elf_writer *w, int initial_size)
{
	w->buf = (unsigned char *)malloc(initial_size);
	w->pos = 0;
	w->size = initial_size;
}

static void ew_grow(elf_writer *w, int needed)
{
	if (w->pos + needed > w->size) {
		int new_size = w->size * 2;
		while (w->pos + needed > new_size)
			new_size *= 2;
		w->buf = (unsigned char *)realloc(w->buf, new_size);
		w->size = new_size;
	}
}

static void ew_bytes(elf_writer *w, const void *data, int len)
{
	ew_grow(w, len);
	memcpy(w->buf + w->pos, data, len);
	w->pos += len;
}

static void ew_u8(elf_writer *w, uint8_t val)
{
	ew_grow(w, 1);
	w->buf[w->pos++] = val;
}

static void ew_u16(elf_writer *w, uint16_t val)
{
	ew_grow(w, 2);
	memcpy(w->buf + w->pos, &val, 2);
	w->pos += 2;
}

static void ew_u32(elf_writer *w, uint32_t val)
{
	ew_grow(w, 4);
	memcpy(w->buf + w->pos, &val, 4);
	w->pos += 4;
}

static void ew_u64(elf_writer *w, uint64_t val)
{
	ew_grow(w, 8);
	memcpy(w->buf + w->pos, &val, 8);
	w->pos += 8;
}

static void ew_pad(elf_writer *w, int alignment)
{
	int pad = ((w->pos + alignment - 1) & ~(alignment - 1)) - w->pos;
	if (pad > 0) {
		ew_grow(w, pad);
		memset(w->buf + w->pos, 0, pad);
		w->pos += pad;
	}
}

static void ew_zero(elf_writer *w, int count)
{
	ew_grow(w, count);
	memset(w->buf + w->pos, 0, count);
	w->pos += count;
}

/* ========== Mach-O Symbol Extraction ========== */

/* Internal symbol record for sorting */
struct macho_sym {
	uint64_t addr;     /* virtual address (with slide) */
	uint32_t strtab_offset; /* offset into our ELF strtab */
	uint8_t  is_func;  /* 1 if text symbol */
};

/* Retained after gdb_jit_register_macho for runtime symbol lookup.
 * Array supports multiple binaries (main exe + dylibs). */
#define MAX_SYMTABS 9  /* 1 main exe + MAX_MACHO_DYLIBS (8) */

struct symtab_entry {
	struct macho_sym *syms;
	char *strtab;
	int nsyms;
};

static struct symtab_entry saved_symtabs[MAX_SYMTABS];
static int num_saved_symtabs = 0;

static int sym_addr_cmp(const void *a, const void *b)
{
	const struct macho_sym *sa = (const struct macho_sym *)a;
	const struct macho_sym *sb = (const struct macho_sym *)b;
	if (sa->addr < sb->addr) return -1;
	if (sa->addr > sb->addr) return 1;
	return 0;
}

/* Convert a Mach-O file offset to memory address using segment mappings */
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

/* Find __TEXT segment address range */
static int find_text_segment(void* mh, uintptr_t slide,
                             uint64_t *text_addr, uint64_t *text_size)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (strcmp(seg->segname, "__TEXT") == 0) {
				*text_addr = seg->vmaddr + slide;
				*text_size = seg->vmsize;
				return 1;
			}
		}
		cmd_ptr += lc->cmdsize;
	}
	return 0;
}

/* Section indices for the minimal ELF */
enum {
	SEC_NULL = 0,
	SEC_TEXT,
	SEC_SYMTAB,
	SEC_STRTAB,
	SEC_SHSTRTAB,
	SEC_COUNT
};

int gdb_jit_register_macho(void* mh, uintptr_t slide)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);
	struct symtab_command* symtab_cmd = NULL;

	/* Find LC_SYMTAB */
	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SYMTAB)
			symtab_cmd = (struct symtab_command*)lc;
		cmd_ptr += lc->cmdsize;
	}

	if (!symtab_cmd || symtab_cmd->nsyms == 0) {
		fprintf(stderr, "gdb_jit: no symbol table found\n");
		return -1;
	}

	/* Get symbol table and string table from mapped memory */
	struct nlist_64* nlist_arr = (struct nlist_64*)fileoff_to_mem(mh, slide, symtab_cmd->symoff);
	char* macho_strtab = (char*)fileoff_to_mem(mh, slide, symtab_cmd->stroff);
	if (!nlist_arr || !macho_strtab) {
		fprintf(stderr, "gdb_jit: cannot map symbol/string table\n");
		return -1;
	}

	/* Find __TEXT segment for the .text section in our ELF */
	uint64_t text_addr = 0, text_size = 0;
	if (!find_text_segment(mh, slide, &text_addr, &text_size)) {
		fprintf(stderr, "gdb_jit: no __TEXT segment found\n");
		return -1;
	}

	/* First pass: count defined symbols and build strtab */
	elf_writer strtab;
	ew_init(&strtab, 256 * 1024);
	ew_u8(&strtab, 0); /* null string at offset 0 */

	/* Allocate symbol records (upper bound = nsyms) */
	struct macho_sym* syms = (struct macho_sym*)malloc(sizeof(struct macho_sym) * symtab_cmd->nsyms);
	int num_syms = 0;

	for (uint32_t i = 0; i < symtab_cmd->nsyms; i++) {
		struct nlist_64* nl = &nlist_arr[i];

		/* Skip stabs (debug entries) */
		if (nl->n_type & N_STAB)
			continue;

		/* Only defined-in-section symbols */
		if ((nl->n_type & N_TYPE) != N_SECT)
			continue;

		/* Get name */
		if (nl->n_strx == 0 || nl->n_strx >= symtab_cmd->strsize)
			continue;
		const char* name = macho_strtab + nl->n_strx;
		if (name[0] == '\0')
			continue;

		/* Only include symbols within __TEXT segment (functions for backtraces) */
		uint64_t sym_addr = nl->n_value + slide;
		if (sym_addr < text_addr || sym_addr >= text_addr + text_size)
			continue;

		/* Strip Mach-O leading underscore */
		const char* elf_name = name;
		if (elf_name[0] == '_')
			elf_name++;

		/* Skip if stripping left us with empty string */
		if (elf_name[0] == '\0')
			continue;

		syms[num_syms].addr = sym_addr;
		syms[num_syms].strtab_offset = strtab.pos;
		syms[num_syms].is_func = 1; /* all __TEXT symbols are code */

		int namelen = strlen(elf_name);
		ew_bytes(&strtab, elf_name, namelen + 1);
		num_syms++;
	}

	if (num_syms == 0) {
		free(syms);
		free(strtab.buf);
		fprintf(stderr, "gdb_jit: no symbols to register\n");
		return -1;
	}

	/* Sort by address to estimate function sizes */
	qsort(syms, num_syms, sizeof(struct macho_sym), sym_addr_cmp);

	/* ========== Build in-memory ELF ========== */

	/* .shstrtab section name strings */
	elf_writer shstrtab;
	ew_init(&shstrtab, 64);
	ew_u8(&shstrtab, 0); /* null string */
	int shstr_text = shstrtab.pos;
	ew_bytes(&shstrtab, ".text", 6);
	int shstr_symtab = shstrtab.pos;
	ew_bytes(&shstrtab, ".symtab", 8);
	int shstr_strtab = shstrtab.pos;
	ew_bytes(&shstrtab, ".strtab", 8);
	int shstr_shstrtab = shstrtab.pos;
	ew_bytes(&shstrtab, ".shstrtab", 10);

	/* Calculate layout */
	uint64_t ehdr_size = sizeof(Elf64_Ehdr);
	/* +1 for mandatory null symbol */
	uint64_t symtab_entry_count = 1 + (uint64_t)num_syms;
	uint64_t symtab_offset = ehdr_size;
	uint64_t symtab_size = symtab_entry_count * sizeof(Elf64_Sym);
	uint64_t strtab_offset = symtab_offset + symtab_size;
	uint64_t strtab_size = strtab.pos;
	uint64_t shstrtab_offset = strtab_offset + strtab_size;
	uint64_t shstrtab_size = shstrtab.pos;
	uint64_t shdr_offset = (shstrtab_offset + shstrtab_size + 7) & ~(uint64_t)7;

	/* Main ELF writer */
	elf_writer w;
	int total_size = (int)(shdr_offset + SEC_COUNT * sizeof(Elf64_Shdr));
	ew_init(&w, total_size + 64);

	/* ========== ELF Header ========== */
	ew_bytes(&w, "\x7f" "ELF", 4);
	ew_u8(&w, ELFCLASS64);
	ew_u8(&w, ELFDATA2LSB);
	ew_u8(&w, EV_CURRENT);
	ew_u8(&w, 0);                     /* OS/ABI */
	ew_zero(&w, 8);                   /* padding */
	ew_u16(&w, ET_REL);
	ew_u16(&w, EM_AARCH64);
	ew_u32(&w, EV_CURRENT);
	ew_u64(&w, 0);                    /* e_entry */
	ew_u64(&w, 0);                    /* e_phoff */
	ew_u64(&w, shdr_offset);          /* e_shoff */
	ew_u32(&w, 0);                    /* e_flags */
	ew_u16(&w, (uint16_t)sizeof(Elf64_Ehdr)); /* e_ehsize */
	ew_u16(&w, 0);                    /* e_phentsize */
	ew_u16(&w, 0);                    /* e_phnum */
	ew_u16(&w, (uint16_t)sizeof(Elf64_Shdr)); /* e_shentsize */
	ew_u16(&w, SEC_COUNT);            /* e_shnum */
	ew_u16(&w, SEC_SHSTRTAB);         /* e_shstrndx */

	/* ========== .symtab ========== */
	/* Null symbol (index 0, required) */
	ew_zero(&w, sizeof(Elf64_Sym));

	/* Function symbols — st_value is section-relative for ET_REL
	 * GDB computes: final_addr = sh_addr + st_value */
	for (int i = 0; i < num_syms; i++) {
		uint64_t func_size;
		if (i + 1 < num_syms)
			func_size = syms[i + 1].addr - syms[i].addr;
		else
			func_size = (text_addr + text_size) - syms[i].addr;

		/* Clamp: if size is 0 or unreasonably large, use 4 (one instruction) */
		if (func_size == 0 || func_size > 0x100000)
			func_size = 4;

		uint64_t section_offset = syms[i].addr - text_addr;

		ew_u32(&w, syms[i].strtab_offset);  /* st_name */
		ew_u8(&w, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC)); /* st_info */
		ew_u8(&w, 0);                       /* st_other */
		ew_u16(&w, SEC_TEXT);               /* st_shndx */
		ew_u64(&w, section_offset);         /* st_value (offset from sh_addr) */
		ew_u64(&w, func_size);              /* st_size */
	}

	/* ========== .strtab ========== */
	ew_bytes(&w, strtab.buf, strtab.pos);

	/* ========== .shstrtab ========== */
	ew_bytes(&w, shstrtab.buf, shstrtab.pos);

	/* ========== Section Headers ========== */
	ew_pad(&w, 8);

	/* SEC_NULL */
	ew_zero(&w, sizeof(Elf64_Shdr));

	/* SEC_TEXT — SHT_NOBITS, sh_addr = actual code address */
	ew_u32(&w, shstr_text);
	ew_u32(&w, SHT_NOBITS);
	ew_u64(&w, SHF_ALLOC | SHF_EXECINSTR);
	ew_u64(&w, text_addr);        /* sh_addr */
	ew_u64(&w, 0);                /* sh_offset (no file data) */
	ew_u64(&w, text_size);        /* sh_size */
	ew_u32(&w, 0);                /* sh_link */
	ew_u32(&w, 0);                /* sh_info */
	ew_u64(&w, 16);               /* sh_addralign */
	ew_u64(&w, 0);                /* sh_entsize */

	/* SEC_SYMTAB */
	ew_u32(&w, shstr_symtab);
	ew_u32(&w, SHT_SYMTAB);
	ew_u64(&w, 0);                /* sh_flags */
	ew_u64(&w, 0);                /* sh_addr */
	ew_u64(&w, symtab_offset);
	ew_u64(&w, symtab_size);
	ew_u32(&w, SEC_STRTAB);       /* sh_link -> .strtab */
	ew_u32(&w, 1);                /* sh_info: first non-local symbol */
	ew_u64(&w, 8);                /* sh_addralign */
	ew_u64(&w, sizeof(Elf64_Sym));

	/* SEC_STRTAB */
	ew_u32(&w, shstr_strtab);
	ew_u32(&w, SHT_STRTAB);
	ew_u64(&w, 0);
	ew_u64(&w, 0);
	ew_u64(&w, strtab_offset);
	ew_u64(&w, strtab_size);
	ew_u32(&w, 0);
	ew_u32(&w, 0);
	ew_u64(&w, 1);
	ew_u64(&w, 0);

	/* SEC_SHSTRTAB */
	ew_u32(&w, shstr_shstrtab);
	ew_u32(&w, SHT_STRTAB);
	ew_u64(&w, 0);
	ew_u64(&w, 0);
	ew_u64(&w, shstrtab_offset);
	ew_u64(&w, shstrtab_size);
	ew_u32(&w, 0);
	ew_u32(&w, 0);
	ew_u64(&w, 1);
	ew_u64(&w, 0);

	/* ========== Register with GDB ========== */
	struct jit_code_entry *entry = (struct jit_code_entry *)malloc(sizeof(struct jit_code_entry));
	if (!entry) {
		free(syms);
		free(strtab.buf);
		free(shstrtab.buf);
		free(w.buf);
		return -1;
	}

	entry->symfile_addr = (const char *)w.buf;
	entry->symfile_size = w.pos;
	entry->prev_entry = NULL;

	/* Link into descriptor's list */
	entry->next_entry = __jit_debug_descriptor.first_entry;
	if (__jit_debug_descriptor.first_entry)
		__jit_debug_descriptor.first_entry->prev_entry = entry;
	__jit_debug_descriptor.first_entry = entry;

	/* Notify GDB */
	__jit_debug_descriptor.relevant_entry = entry;
	__jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
	__jit_debug_register_code();

	fprintf(stderr, "gdb_jit: registered %d symbols (%d bytes ELF, code at %p-%p)\n",
	        num_syms, w.pos, (void*)text_addr, (void*)(text_addr + text_size));

	/* Keep syms + strtab for runtime symbol lookup (heap_trace, etc.)
	 * w.buf is owned by entry (GDB JIT), shstrtab is no longer needed. */
	if (num_saved_symtabs < MAX_SYMTABS) {
		saved_symtabs[num_saved_symtabs].syms = syms;
		saved_symtabs[num_saved_symtabs].strtab = strtab.buf;
		saved_symtabs[num_saved_symtabs].nsyms = num_syms;
		num_saved_symtabs++;
	} else {
		fprintf(stderr, "gdb_jit: too many symbol tables (max %d), skipping runtime lookup\n",
		        MAX_SYMTABS);
		free(syms);
		free(strtab.buf);
	}
	free(shstrtab.buf);

	return num_syms;
}

/* ========== Runtime symbol lookup ========== */

const char *gdb_jit_lookup_addr(uintptr_t addr)
{
	const char *best_name = NULL;
	uint64_t best_addr = 0;

	for (int t = 0; t < num_saved_symtabs; t++) {
		struct symtab_entry *st = &saved_symtabs[t];
		if (!st->syms || st->nsyms == 0)
			continue;

		/* Binary search for the largest sym.addr <= addr */
		int lo = 0, hi = st->nsyms - 1;
		int idx = -1;
		while (lo <= hi) {
			int mid = lo + (hi - lo) / 2;
			if (st->syms[mid].addr <= addr) {
				idx = mid;
				lo = mid + 1;
			} else {
				hi = mid - 1;
			}
		}
		if (idx < 0)
			continue;

		/* Check that addr is within a reasonable range of the symbol
		 * (use next symbol's addr as upper bound, or 1MB max) */
		uint64_t upper;
		if (idx + 1 < st->nsyms)
			upper = st->syms[idx + 1].addr;
		else
			upper = st->syms[idx].addr + (1 << 20);
		if (addr >= upper)
			continue;

		/* Keep closest match (largest sym.addr that's still <= target) */
		if (st->syms[idx].addr > best_addr) {
			best_addr = st->syms[idx].addr;
			best_name = st->strtab + st->syms[idx].strtab_offset;
		}
	}
	return best_name;
}

uintptr_t gdb_jit_lookup_name(const char *name)
{
	if (!name)
		return 0;

	for (int t = 0; t < num_saved_symtabs; t++) {
		struct symtab_entry *st = &saved_symtabs[t];
		if (!st->syms || st->nsyms == 0)
			continue;

		for (int i = 0; i < st->nsyms; i++) {
			const char *sym = st->strtab + st->syms[i].strtab_offset;
			if (strcmp(sym, name) == 0)
				return st->syms[i].addr;
		}
	}
	return 0;
}
