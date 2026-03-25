/*
 * Compact unwind → DWARF .eh_frame converter for Mach-O on Linux.
 *
 * Apple's arm64 Mach-O binaries store exception handling info in a compact
 * unwind format (__TEXT,__unwind_info) that Linux libunwind/libgcc can't parse.
 * This module converts compact unwind entries to standard DWARF .eh_frame FDEs
 * and registers them with the system unwinder.
 *
 * The LSDA (Language-Specific Data Area) tables in __TEXT,__gcc_except_tab
 * are already in standard Itanium C++ ABI format, shared by both Apple and
 * Linux. We just need to point the DWARF FDEs at them.
 *
 * Reference: llvm-project/libunwind/include/mach-o/compact_unwind_encoding.h
 */

#include "eh_frame.h"
#include "macho_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <dlfcn.h>

/* ========== Compact Unwind Encoding Constants (ARM64) ========== */

#define UNWIND_IS_NOT_FUNCTION_START  0x80000000
#define UNWIND_HAS_LSDA              0x40000000
#define UNWIND_PERSONALITY_MASK       0x30000000

#define UNWIND_ARM64_MODE_MASK        0x0F000000
#define UNWIND_ARM64_MODE_FRAMELESS   0x02000000
#define UNWIND_ARM64_MODE_DWARF       0x03000000
#define UNWIND_ARM64_MODE_FRAME       0x04000000

#define UNWIND_ARM64_FRAME_X19_X20_PAIR   0x00000001
#define UNWIND_ARM64_FRAME_X21_X22_PAIR   0x00000002
#define UNWIND_ARM64_FRAME_X23_X24_PAIR   0x00000004
#define UNWIND_ARM64_FRAME_X25_X26_PAIR   0x00000008
#define UNWIND_ARM64_FRAME_X27_X28_PAIR   0x00000010
#define UNWIND_ARM64_FRAME_D8_D9_PAIR     0x00000100
#define UNWIND_ARM64_FRAME_D10_D11_PAIR   0x00000200
#define UNWIND_ARM64_FRAME_D12_D13_PAIR   0x00000400
#define UNWIND_ARM64_FRAME_D14_D15_PAIR   0x00000800

#define UNWIND_ARM64_FRAMELESS_STACK_SIZE_MASK  0x00FFF000
#define UNWIND_ARM64_DWARF_SECTION_OFFSET       0x00FFFFFF

/* ========== Compact Unwind Section Structures ========== */

struct unwind_info_section_header {
	uint32_t version;
	uint32_t commonEncodingsArraySectionOffset;
	uint32_t commonEncodingsArrayCount;
	uint32_t personalityArraySectionOffset;
	uint32_t personalityArrayCount;
	uint32_t indexSectionOffset;
	uint32_t indexCount;
};

struct unwind_info_section_header_index_entry {
	uint32_t functionOffset;
	uint32_t secondLevelPagesSectionOffset;
	uint32_t lsdaIndexArraySectionOffset;
};

struct unwind_info_section_header_lsda_index_entry {
	uint32_t functionOffset;
	uint32_t lsdaOffset;
};

struct unwind_info_regular_second_level_entry {
	uint32_t functionOffset;
	uint32_t encoding;
};

#define UNWIND_SECOND_LEVEL_REGULAR   2
#define UNWIND_SECOND_LEVEL_COMPRESSED 3

struct unwind_info_compressed_second_level_page_header {
	uint32_t kind;
	uint16_t entryPageOffset;
	uint16_t entryCount;
	uint16_t encodingsPageOffset;
	uint16_t encodingsCount;
};

struct unwind_info_regular_second_level_page_header {
	uint32_t kind;
	uint16_t entryPageOffset;
	uint16_t entryCount;
};

/* ========== DWARF .eh_frame Constants ========== */

/* DWARF register numbers for AArch64 */
#define DWARF_REG_X19  19
#define DWARF_REG_X20  20
#define DWARF_REG_X21  21
#define DWARF_REG_X22  22
#define DWARF_REG_X23  23
#define DWARF_REG_X24  24
#define DWARF_REG_X25  25
#define DWARF_REG_X26  26
#define DWARF_REG_X27  27
#define DWARF_REG_X28  28
#define DWARF_REG_FP   29  /* x29 / frame pointer */
#define DWARF_REG_LR   30  /* x30 / link register */
#define DWARF_REG_SP   31
/* DWARF floating point: D8-D15 are 72-79 in DWARF numbering */
#define DWARF_REG_D8   72
#define DWARF_REG_D9   73
#define DWARF_REG_D10  74
#define DWARF_REG_D11  75
#define DWARF_REG_D12  76
#define DWARF_REG_D13  77
#define DWARF_REG_D14  78
#define DWARF_REG_D15  79

/* DW_CFA opcodes */
#define DW_CFA_advance_loc        0x40  /* + delta (6 bits) */
#define DW_CFA_offset             0x80  /* + reg (6 bits), ULEB128 offset */
#define DW_CFA_nop                0x00
#define DW_CFA_offset_extended    0x05  /* ULEB128 reg, ULEB128 offset */
#define DW_CFA_def_cfa            0x0c  /* ULEB128 reg, ULEB128 offset */
#define DW_CFA_def_cfa_offset     0x0e  /* ULEB128 offset */

/* DW_EH_PE pointer encodings */
#define DW_EH_PE_absptr    0x00
#define DW_EH_PE_omit      0xff

/* ========== _dl_find_object Interposition ========== */

/*
 * GCC 15 + glibc 2.35: _Unwind_Find_FDE uses _dl_find_object as the sole
 * FDE lookup path. When it returns -1 (not in any ELF), the unwinder gives up
 * WITHOUT checking __register_frame'd objects. This breaks dynamic FDE
 * registration for non-ELF code (JIT, Mach-O, etc.).
 *
 * We interpose _dl_find_object: for PCs in the Mach-O __TEXT range, return
 * our synthetic .eh_frame_hdr. For all other PCs, call the real function.
 */

static uintptr_t macho_text_start = 0;
static uintptr_t macho_text_end = 0;
static size_t text_size = 0;
static void* macho_eh_frame = NULL;
static size_t macho_eh_frame_size = 0;
static void* macho_eh_frame_hdr = NULL;
static size_t macho_eh_frame_hdr_size = 0;
struct dl_find_object;
static int (*real_dl_find_object)(void*, struct dl_find_object*) = NULL;

/*
 * struct dl_find_object layout (glibc 2.35+):
 *   unsigned long long dlfo_flags;        // 0
 *   void* dlfo_map_start;                 // 8
 *   void* dlfo_map_end;                   // 16
 *   struct link_map* dlfo_link_map;       // 24
 *   void* dlfo_eh_frame;                  // 32
 */
#define DLFO_FLAGS       0
#define DLFO_MAP_START   8
#define DLFO_MAP_END     16
#define DLFO_LINK_MAP    24
#define DLFO_EH_FRAME    32
#define DLFO_SIZE        40

/* Our interposed _dl_find_object.
 * Uses struct dl_find_object* to match glibc 2.35+ declaration in <dlfcn.h>.
 * The struct is forward-declared above for older glibc that lacks it. */
int _dl_find_object(void* pc, struct dl_find_object* result_ptr)
{
	void *result = (void *)result_ptr;
	uintptr_t addr = (uintptr_t)pc;

	/* Check if address is in the Mach-O __TEXT range */
	if (macho_eh_frame_hdr && addr >= macho_text_start && addr < macho_text_end) {
		/* Fill in the result struct for the Mach-O */
		memset(result, 0, DLFO_SIZE);
		*(void**)((char*)result + DLFO_MAP_START) = (void*)macho_text_start;
		*(void**)((char*)result + DLFO_MAP_END) = (void*)macho_text_end;
		*(void**)((char*)result + DLFO_EH_FRAME) = macho_eh_frame_hdr;
		return 0;  /* success */
	}

	/* Fall through to real _dl_find_object for ELF objects */
	if (real_dl_find_object)
		return real_dl_find_object(pc, result_ptr);

	return -1;  /* not found */
}

/* ========== Emit Helpers ========== */

/* Buffer for building the synthetic .eh_frame */
static uint8_t* ehf_buf = NULL;
static size_t ehf_pos = 0;
static size_t ehf_capacity = 0;

static void ehf_ensure(size_t need)
{
	/* Buffer is pre-allocated via mmap; just check bounds */
	if (ehf_pos + need > ehf_capacity) {
		fprintf(stderr, "eh_frame: buffer overflow (pos=%zu, need=%zu, cap=%zu)\n",
		        ehf_pos, need, ehf_capacity);
	}
}

static void ehf_u8(uint8_t v)
{
	ehf_ensure(1);
	ehf_buf[ehf_pos++] = v;
}

static void ehf_u32(uint32_t v)
{
	ehf_ensure(4);
	memcpy(ehf_buf + ehf_pos, &v, 4);
	ehf_pos += 4;
}

static void ehf_u64(uint64_t v)
{
	ehf_ensure(8);
	memcpy(ehf_buf + ehf_pos, &v, 8);
	ehf_pos += 8;
}

/* Emit ULEB128 */
static void ehf_uleb128(uint64_t v)
{
	do {
		uint8_t byte = v & 0x7f;
		v >>= 7;
		if (v) byte |= 0x80;
		ehf_u8(byte);
	} while (v);
}

/* Emit SLEB128 */
static void ehf_sleb128(int64_t v)
{
	int more = 1;
	while (more) {
		uint8_t byte = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;
		ehf_u8(byte);
	}
}

/* Pad to alignment */
static void ehf_align(size_t align)
{
	while (ehf_pos % align)
		ehf_u8(DW_CFA_nop);
}

/* ========== CIE/FDE Emitters ========== */

/*
 * Emit a CIE (Common Information Entry).
 *
 * If personality_addr is 0, emits a "zR" CIE (no personality/LSDA).
 * If personality_addr is nonzero, emits a "zPLR" CIE (with personality + LSDA).
 *
 * Returns the offset of the CIE in the buffer (for FDE back-references).
 */
static size_t emit_cie(uintptr_t personality_addr)
{
	size_t cie_start = ehf_pos;
	int has_personality = (personality_addr != 0);

	/* Placeholder for length (will patch) */
	ehf_u32(0);
	size_t length_start = ehf_pos;

	/* CIE ID = 0 */
	ehf_u32(0);

	/* Version */
	ehf_u8(1);

	/* Augmentation string */
	if (has_personality) {
		ehf_u8('z'); ehf_u8('P'); ehf_u8('L'); ehf_u8('R'); ehf_u8(0);
	} else {
		ehf_u8('z'); ehf_u8('R'); ehf_u8(0);
	}

	/* Code alignment factor: 4 (arm64 instructions) */
	ehf_uleb128(4);

	/* Data alignment factor: -8 */
	ehf_sleb128(-8);

	/* Return address column: 30 (LR) */
	ehf_uleb128(DWARF_REG_LR);

	/* Augmentation data */
	if (has_personality) {
		/* Augmentation data length:
		 * P: 1 (encoding) + 8 (absptr) = 9
		 * L: 1 (encoding)
		 * R: 1 (encoding)
		 * Total = 11 */
		ehf_uleb128(11);

		/* P: personality encoding + pointer */
		ehf_u8(DW_EH_PE_absptr);
		ehf_u64(personality_addr);

		/* L: LSDA encoding */
		ehf_u8(DW_EH_PE_absptr);

		/* R: FDE pointer encoding */
		ehf_u8(DW_EH_PE_absptr);
	} else {
		/* Augmentation data length: R = 1 */
		ehf_uleb128(1);

		/* R: FDE pointer encoding */
		ehf_u8(DW_EH_PE_absptr);
	}

	/* Initial instructions: CFA = SP + 0 (before prologue) */
	ehf_u8(DW_CFA_def_cfa);
	ehf_uleb128(DWARF_REG_SP);
	ehf_uleb128(0);

	/* Pad to pointer-size alignment */
	ehf_align(8);

	/* Patch length */
	uint32_t length = (uint32_t)(ehf_pos - length_start);
	memcpy(ehf_buf + cie_start, &length, 4);

	return cie_start;
}

/*
 * Emit an FDE for a FRAME-mode function.
 *
 * FRAME mode: FP/LR pushed, then register pairs below FP.
 * CFA = FP + 16, LR at [CFA-8], FP at [CFA-16], saved pairs below.
 */
static void emit_fde_frame(size_t cie_offset, uintptr_t func_addr,
                           uint32_t func_size, uint32_t encoding,
                           uintptr_t lsda_addr)
{
	size_t fde_start = ehf_pos;
	int has_lsda = (lsda_addr != 0);

	/* Placeholder for length */
	ehf_u32(0);
	size_t length_start = ehf_pos;

	/* CIE pointer: offset from this field back to the CIE */
	ehf_u32((uint32_t)(ehf_pos - cie_offset));

	/* Initial location (absptr = 8 bytes) */
	ehf_u64(func_addr);

	/* Address range */
	ehf_u64(func_size);

	/* Augmentation data */
	if (has_lsda) {
		ehf_uleb128(8);  /* LSDA pointer = 8 bytes */
		ehf_u64(lsda_addr);
	} else {
		ehf_uleb128(0);
	}

	/* CFI instructions for FRAME mode */

	/* After prologue (advance 1 instruction = 4 bytes, so delta=1 in code_align units) */
	ehf_u8(DW_CFA_advance_loc | 1);

	/* CFA = FP + 16 */
	ehf_u8(DW_CFA_def_cfa);
	ehf_uleb128(DWARF_REG_FP);
	ehf_uleb128(16);

	/* LR at CFA-8 (offset 1 in data_align units, since data_align=-8) */
	ehf_u8(DW_CFA_offset | DWARF_REG_LR);
	ehf_uleb128(1);

	/* FP at CFA-16 (offset 2) */
	ehf_u8(DW_CFA_offset | DWARF_REG_FP);
	ehf_uleb128(2);

	/* Saved X register pairs — stored below FP/LR in order */
	int slot = 3;  /* starts at CFA-24 */
	uint32_t x_pairs = encoding & 0x1F;  /* bits 4-0 */
	if (x_pairs & UNWIND_ARM64_FRAME_X19_X20_PAIR) {
		ehf_u8(DW_CFA_offset | DWARF_REG_X19); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset | DWARF_REG_X20); ehf_uleb128(slot++);
	}
	if (x_pairs & UNWIND_ARM64_FRAME_X21_X22_PAIR) {
		ehf_u8(DW_CFA_offset | DWARF_REG_X21); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset | DWARF_REG_X22); ehf_uleb128(slot++);
	}
	if (x_pairs & UNWIND_ARM64_FRAME_X23_X24_PAIR) {
		ehf_u8(DW_CFA_offset | DWARF_REG_X23); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset | DWARF_REG_X24); ehf_uleb128(slot++);
	}
	if (x_pairs & UNWIND_ARM64_FRAME_X25_X26_PAIR) {
		ehf_u8(DW_CFA_offset | DWARF_REG_X25); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset | DWARF_REG_X26); ehf_uleb128(slot++);
	}
	if (x_pairs & UNWIND_ARM64_FRAME_X27_X28_PAIR) {
		ehf_u8(DW_CFA_offset | DWARF_REG_X27); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset | DWARF_REG_X28); ehf_uleb128(slot++);
	}

	/* Saved D register pairs — use DW_CFA_offset_extended since
	 * DWARF D register numbers (72-79) don't fit in DW_CFA_offset's 6-bit field */
	uint32_t d_pairs = (encoding >> 8) & 0x0F;  /* bits 11-8 */
	if (d_pairs & (UNWIND_ARM64_FRAME_D8_D9_PAIR >> 8)) {
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D8); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D9); ehf_uleb128(slot++);
	}
	if (d_pairs & (UNWIND_ARM64_FRAME_D10_D11_PAIR >> 8)) {
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D10); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D11); ehf_uleb128(slot++);
	}
	if (d_pairs & (UNWIND_ARM64_FRAME_D12_D13_PAIR >> 8)) {
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D12); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D13); ehf_uleb128(slot++);
	}
	if (d_pairs & (UNWIND_ARM64_FRAME_D14_D15_PAIR >> 8)) {
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D14); ehf_uleb128(slot++);
		ehf_u8(DW_CFA_offset_extended); ehf_uleb128(DWARF_REG_D15); ehf_uleb128(slot++);
	}

	ehf_align(8);

	/* Patch length */
	uint32_t length = (uint32_t)(ehf_pos - length_start);
	memcpy(ehf_buf + fde_start, &length, 4);
}

/*
 * Emit an FDE for a FRAMELESS function.
 *
 * FRAMELESS: No frame pointer. CFA = SP + stack_size.
 * LR is at the top of the saved area (SP + stack_size - 8) if saved.
 */
static void emit_fde_frameless(size_t cie_offset, uintptr_t func_addr,
                               uint32_t func_size, uint32_t encoding,
                               uintptr_t lsda_addr)
{
	size_t fde_start = ehf_pos;
	int has_lsda = (lsda_addr != 0);

	/* Stack size from encoding: bits 23-12, multiply by 16 */
	uint32_t stack_size = ((encoding & UNWIND_ARM64_FRAMELESS_STACK_SIZE_MASK) >> 12) * 16;

	ehf_u32(0);  /* length placeholder */
	size_t length_start = ehf_pos;

	ehf_u32((uint32_t)(ehf_pos - cie_offset));  /* CIE pointer */
	ehf_u64(func_addr);                          /* initial location */
	ehf_u64(func_size);                          /* address range */

	if (has_lsda) {
		ehf_uleb128(8);
		ehf_u64(lsda_addr);
	} else {
		ehf_uleb128(0);
	}

	/* CFI: after prologue, CFA = SP + stack_size */
	if (stack_size > 0) {
		ehf_u8(DW_CFA_advance_loc | 1);
		ehf_u8(DW_CFA_def_cfa_offset);
		ehf_uleb128(stack_size);

		/* LR is at top of frame: CFA - 8 = SP + stack_size - 8
		 * In data_align units (-8): offset = 1 */
		if (stack_size >= 8) {
			ehf_u8(DW_CFA_offset | DWARF_REG_LR);
			ehf_uleb128(1);
		}
	}

	ehf_align(8);

	uint32_t length = (uint32_t)(ehf_pos - length_start);
	memcpy(ehf_buf + fde_start, &length, 4);
}

/*
 * Emit a minimal FDE for a function with encoding 0 (no unwind info).
 * This is common for leaf functions that don't save any registers.
 */
static void emit_fde_minimal(size_t cie_offset, uintptr_t func_addr,
                             uint32_t func_size, uintptr_t lsda_addr)
{
	size_t fde_start = ehf_pos;
	int has_lsda = (lsda_addr != 0);

	ehf_u32(0);  /* length placeholder */
	size_t length_start = ehf_pos;

	ehf_u32((uint32_t)(ehf_pos - cie_offset));
	ehf_u64(func_addr);
	ehf_u64(func_size);

	if (has_lsda) {
		ehf_uleb128(8);
		ehf_u64(lsda_addr);
	} else {
		ehf_uleb128(0);
	}

	/* No CFI instructions — CFA stays as initial (SP + 0) */

	ehf_align(8);

	uint32_t length = (uint32_t)(ehf_pos - length_start);
	memcpy(ehf_buf + fde_start, &length, 4);
}

/* ========== Compact Unwind Parser ========== */

/* Parsed function entry from compact unwind */
struct cu_entry {
	uint32_t func_offset;    /* from text_base (unslid) */
	uint32_t func_size;      /* computed from next entry */
	uint32_t encoding;
	uint32_t lsda_offset;   /* from text_base, or 0 if none */
};

/*
 * Collect all compact unwind entries into a flat array.
 * Returns count, or -1 on error. Caller must free *out_entries.
 */
static int collect_entries(const uint8_t* unwind_info, size_t unwind_size,
                          const uint32_t* common_encodings, uint32_t common_count,
                          struct cu_entry** out_entries)
{
	const struct unwind_info_section_header* hdr =
		(const struct unwind_info_section_header*)unwind_info;

	if (hdr->version != 1) {
		fprintf(stderr, "eh_frame: unsupported __unwind_info version %u\n", hdr->version);
		return -1;
	}

	/* First pass: count entries to allocate */
	int total = 0;
	const struct unwind_info_section_header_index_entry* indices =
		(const struct unwind_info_section_header_index_entry*)
		(unwind_info + hdr->indexSectionOffset);

	for (uint32_t idx = 0; idx + 1 < hdr->indexCount; idx++) {
		uint32_t page_offset = indices[idx].secondLevelPagesSectionOffset;
		if (page_offset == 0 || page_offset >= unwind_size) continue;

		uint32_t kind = *(const uint32_t*)(unwind_info + page_offset);
		if (kind == UNWIND_SECOND_LEVEL_COMPRESSED) {
			const struct unwind_info_compressed_second_level_page_header* ph =
				(const struct unwind_info_compressed_second_level_page_header*)
				(unwind_info + page_offset);
			total += ph->entryCount;
		} else if (kind == UNWIND_SECOND_LEVEL_REGULAR) {
			const struct unwind_info_regular_second_level_page_header* ph =
				(const struct unwind_info_regular_second_level_page_header*)
				(unwind_info + page_offset);
			total += ph->entryCount;
		}
	}

	if (total == 0) {
		*out_entries = NULL;
		return 0;
	}

	struct cu_entry* entries = calloc(total, sizeof(struct cu_entry));
	if (!entries) return -1;

	/* Second pass: collect entries */
	int count = 0;
	for (uint32_t idx = 0; idx + 1 < hdr->indexCount; idx++) {
		uint32_t page_offset = indices[idx].secondLevelPagesSectionOffset;
		if (page_offset == 0 || page_offset >= unwind_size) continue;

		uint32_t page_func_base = indices[idx].functionOffset;
		uint32_t kind = *(const uint32_t*)(unwind_info + page_offset);

		/* LSDA index for this range */
		uint32_t lsda_start_off = indices[idx].lsdaIndexArraySectionOffset;
		uint32_t lsda_end_off = indices[idx + 1].lsdaIndexArraySectionOffset;
		const struct unwind_info_section_header_lsda_index_entry* lsda_arr =
			(const struct unwind_info_section_header_lsda_index_entry*)
			(unwind_info + lsda_start_off);
		int lsda_count = (lsda_end_off - lsda_start_off) /
		                 sizeof(struct unwind_info_section_header_lsda_index_entry);

		if (kind == UNWIND_SECOND_LEVEL_COMPRESSED) {
			const struct unwind_info_compressed_second_level_page_header* ph =
				(const struct unwind_info_compressed_second_level_page_header*)
				(unwind_info + page_offset);

			const uint32_t* page_entries =
				(const uint32_t*)(unwind_info + page_offset + ph->entryPageOffset);
			const uint32_t* page_encodings =
				(const uint32_t*)(unwind_info + page_offset + ph->encodingsPageOffset);

			for (uint32_t i = 0; i < ph->entryCount && count < total; i++) {
				uint32_t entry = page_entries[i];
				uint32_t func_offset = page_func_base + (entry & 0x00FFFFFF);
				uint32_t enc_idx = (entry >> 24) & 0xFF;

				uint32_t encoding;
				if (enc_idx < common_count)
					encoding = common_encodings[enc_idx];
				else if (enc_idx - common_count < ph->encodingsCount)
					encoding = page_encodings[enc_idx - common_count];
				else
					encoding = 0;

				/* LSDA lookup: binary search */
				uint32_t lsda_off = 0;
				if (encoding & UNWIND_HAS_LSDA) {
					int lo = 0, hi = lsda_count - 1;
					while (lo <= hi) {
						int mid = (lo + hi) / 2;
						if (lsda_arr[mid].functionOffset == func_offset) {
							lsda_off = lsda_arr[mid].lsdaOffset;
							break;
						} else if (lsda_arr[mid].functionOffset < func_offset) {
							lo = mid + 1;
						} else {
							hi = mid - 1;
						}
					}
				}

				entries[count].func_offset = func_offset;
				entries[count].encoding = encoding;
				entries[count].lsda_offset = lsda_off;
				count++;
			}
		} else if (kind == UNWIND_SECOND_LEVEL_REGULAR) {
			const struct unwind_info_regular_second_level_page_header* ph =
				(const struct unwind_info_regular_second_level_page_header*)
				(unwind_info + page_offset);

			const struct unwind_info_regular_second_level_entry* page_entries =
				(const struct unwind_info_regular_second_level_entry*)
				(unwind_info + page_offset + ph->entryPageOffset);

			for (uint32_t i = 0; i < ph->entryCount && count < total; i++) {
				uint32_t func_offset = page_entries[i].functionOffset;
				uint32_t encoding = page_entries[i].encoding;

				uint32_t lsda_off = 0;
				if (encoding & UNWIND_HAS_LSDA) {
					int lo = 0, hi = lsda_count - 1;
					while (lo <= hi) {
						int mid = (lo + hi) / 2;
						if (lsda_arr[mid].functionOffset == func_offset) {
							lsda_off = lsda_arr[mid].lsdaOffset;
							break;
						} else if (lsda_arr[mid].functionOffset < func_offset) {
							lo = mid + 1;
						} else {
							hi = mid - 1;
						}
					}
				}

				entries[count].func_offset = func_offset;
				entries[count].encoding = encoding;
				entries[count].lsda_offset = lsda_off;
				count++;
			}
		}
	}

	/* Compute function sizes from sorted offsets.
	 * Entries should already be in order since we walk pages in order,
	 * but we sort anyway for safety. */
	/* Simple insertion sort — entries are nearly sorted already */
	for (int i = 1; i < count; i++) {
		struct cu_entry tmp = entries[i];
		int j = i - 1;
		while (j >= 0 && entries[j].func_offset > tmp.func_offset) {
			entries[j + 1] = entries[j];
			j--;
		}
		entries[j + 1] = tmp;
	}

	/* Set function sizes from gaps */
	for (int i = 0; i < count - 1; i++) {
		entries[i].func_size = entries[i + 1].func_offset - entries[i].func_offset;
	}
	/* Last function: use a reasonable default (256 bytes) */
	if (count > 0) {
		entries[count - 1].func_size = 256;
	}

	*out_entries = entries;
	return count;
}

/* ========== Main Entry Point ========== */

int eh_frame_register_macho(void* mh, uintptr_t slide)
{
	struct mach_header_64* header = (struct mach_header_64*)mh;
	uint8_t* cmd_ptr = (uint8_t*)(header + 1);

	/* Locate sections */
	uintptr_t text_base = 0;     /* __TEXT vmaddr (unslid) */
	const uint8_t* unwind_info = NULL;
	size_t unwind_size = 0;

	for (uint32_t i = 0; i < header->ncmds; i++) {
		struct load_command* lc = (struct load_command*)cmd_ptr;
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64* seg = (struct segment_command_64*)lc;
			if (strcmp(seg->segname, "__TEXT") == 0) {
				text_base = seg->vmaddr;
				text_size = seg->vmsize;
				struct section_64* sect = (struct section_64*)(seg + 1);
				for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
					if (strcmp(sect->sectname, "__unwind_info") == 0) {
						unwind_info = (const uint8_t*)(sect->addr + slide);
						unwind_size = sect->size;
					}
				}
			}
		}
		cmd_ptr += lc->cmdsize;
	}

	if (!unwind_info) {
		fprintf(stderr, "eh_frame: no __unwind_info section found\n");
		return -1;
	}

	/* Parse header */
	const struct unwind_info_section_header* hdr =
		(const struct unwind_info_section_header*)unwind_info;

	fprintf(stderr, "eh_frame: __unwind_info v%u: %u common encodings, %u personalities, %u index entries\n",
	        hdr->version, hdr->commonEncodingsArrayCount,
	        hdr->personalityArrayCount, hdr->indexCount);

	/* Read common encodings */
	const uint32_t* common_encodings =
		(const uint32_t*)(unwind_info + hdr->commonEncodingsArraySectionOffset);

	/* Resolve personality functions from GOT entries */
	uintptr_t personalities[4] = {0};
	const int32_t* personality_arr =
		(const int32_t*)(unwind_info + hdr->personalityArraySectionOffset);
	for (uint32_t i = 0; i < hdr->personalityArrayCount && i < 4; i++) {
		/* Personality array stores offsets from __TEXT base to a GOT entry.
		 * The GOT entry (after resolver patching) contains the real function pointer. */
		uintptr_t got_addr = text_base + slide + personality_arr[i];
		uintptr_t* got_entry = (uintptr_t*)got_addr;
		personalities[i] = *got_entry;
		fprintf(stderr, "eh_frame: personality[%u]: GOT at %p → %p\n",
		        i + 1, (void*)got_addr, (void*)personalities[i]);
	}

	/* Collect all compact unwind entries */
	struct cu_entry* entries = NULL;
	int count = collect_entries(unwind_info, unwind_size,
	                           common_encodings, hdr->commonEncodingsArrayCount,
	                           &entries);
	if (count < 0) return -1;
	if (count == 0) {
		fprintf(stderr, "eh_frame: no compact unwind entries found\n");
		return 0;
	}

	/* Count stats */
	int n_frame = 0, n_frameless = 0, n_dwarf = 0, n_lsda = 0, n_zero = 0;
	for (int i = 0; i < count; i++) {
		uint32_t mode = entries[i].encoding & UNWIND_ARM64_MODE_MASK;
		if (mode == UNWIND_ARM64_MODE_FRAME) n_frame++;
		else if (mode == UNWIND_ARM64_MODE_FRAMELESS) n_frameless++;
		else if (mode == UNWIND_ARM64_MODE_DWARF) n_dwarf++;
		else n_zero++;
		if (entries[i].encoding & UNWIND_HAS_LSDA) n_lsda++;
	}
	fprintf(stderr, "eh_frame: %d entries: %d FRAME, %d FRAMELESS, %d DWARF, %d zero-enc, %d with LSDA\n",
	        count, n_frame, n_frameless, n_dwarf, n_zero, n_lsda);

	/* Allocate buffer for synthetic .eh_frame.
	 * Generous estimate: 80 bytes per FDE + CIE overhead. */
	ehf_capacity = (size_t)count * 80 + 4096;
	ehf_buf = mmap(NULL, ehf_capacity, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ehf_buf == MAP_FAILED) {
		fprintf(stderr, "eh_frame: failed to allocate %zu bytes\n", ehf_capacity);
		free(entries);
		return -1;
	}
	ehf_pos = 0;

	/* Emit CIEs — one with personality, one without */
	uintptr_t gxx_personality = (hdr->personalityArrayCount > 0) ? personalities[0] : 0;
	size_t cie_with_personality = emit_cie(gxx_personality);
	size_t cie_without_personality = emit_cie(0);

	/* Emit FDEs */
	int fdes_emitted = 0;
	for (int i = 0; i < count; i++) {
		uint32_t encoding = entries[i].encoding;
		uint32_t mode = encoding & UNWIND_ARM64_MODE_MASK;
		uintptr_t func_addr = text_base + slide + entries[i].func_offset;
		uint32_t func_size = entries[i].func_size;
		uintptr_t lsda_addr = 0;
		int has_lsda = (encoding & UNWIND_HAS_LSDA);

		/* Compute LSDA address */
		if (has_lsda && entries[i].lsda_offset) {
			lsda_addr = text_base + slide + entries[i].lsda_offset;
		}

		/* Select CIE based on LSDA presence */
		size_t cie = has_lsda ? cie_with_personality : cie_without_personality;

		/* Skip DWARF-mode entries — they're already in existing .eh_frame */
		if (mode == UNWIND_ARM64_MODE_DWARF)
			continue;

		/* Skip NOT_FUNCTION_START continuations */
		if (encoding & UNWIND_IS_NOT_FUNCTION_START)
			continue;

		if (mode == UNWIND_ARM64_MODE_FRAME) {
			emit_fde_frame(cie, func_addr, func_size, encoding, lsda_addr);
		} else if (mode == UNWIND_ARM64_MODE_FRAMELESS) {
			emit_fde_frameless(cie, func_addr, func_size, encoding, lsda_addr);
		} else {
			/* Encoding mode 0 or unknown — emit minimal FDE */
			emit_fde_minimal(cie, func_addr, func_size, lsda_addr);
		}
		fdes_emitted++;
	}

	/* Zero terminator */
	ehf_u32(0);

	free(entries);

	fprintf(stderr, "eh_frame: generated %d FDEs (%zu bytes) in synthetic .eh_frame\n",
	        fdes_emitted, ehf_pos);

	/*
	 * Hook _dl_find_object to make our .eh_frame visible to the unwinder.
	 *
	 * GCC 15+ with glibc 2.35+ uses _dl_find_object() as the sole FDE lookup
	 * mechanism. When _dl_find_object returns -1 (address not in any loaded ELF),
	 * _Unwind_Find_FDE returns NULL immediately WITHOUT searching __register_frame'd
	 * objects. This means __register_frame is effectively broken.
	 *
	 * Our workaround: interpose _dl_find_object via the libgcc_s GOT. When the
	 * query address is in the Mach-O's __TEXT range, we return our synthetic
	 * .eh_frame_hdr. Otherwise, call the real _dl_find_object.
	 */

	/* Store pointers for the hook */
	macho_text_start = text_base + slide;
	macho_text_end = macho_text_start + text_size;
	macho_eh_frame = ehf_buf;
	macho_eh_frame_size = ehf_pos;

	/* Build .eh_frame_hdr — a binary search index for the unwinder.
	 * Format: 1-byte version, 1-byte eh_frame_ptr_enc, 1-byte fde_count_enc,
	 * 1-byte table_enc, then eh_frame_ptr, fde_count, and sorted table entries.
	 * We use DW_EH_PE_absptr (0x00) for all encodings for simplicity. */
	{
		size_t hdr_size = 4 + 8 + 8 + (size_t)fdes_emitted * 16;
		macho_eh_frame_hdr = mmap(NULL, hdr_size, PROT_READ | PROT_WRITE,
		                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (macho_eh_frame_hdr == MAP_FAILED) {
			fprintf(stderr, "eh_frame: failed to allocate .eh_frame_hdr\n");
			return -1;
		}

		uint8_t* h = (uint8_t*)macho_eh_frame_hdr;
		h[0] = 1;                 /* version */
		h[1] = DW_EH_PE_absptr;  /* eh_frame_ptr encoding */
		h[2] = DW_EH_PE_absptr;  /* fde_count encoding */
		h[3] = DW_EH_PE_absptr;  /* table encoding */
		size_t hp = 4;

		/* eh_frame_ptr: pointer to .eh_frame section */
		memcpy(h + hp, &macho_eh_frame, 8); hp += 8;

		/* fde_count */
		uint64_t fde_count_val = fdes_emitted;
		memcpy(h + hp, &fde_count_val, 8); hp += 8;

		/* Table: sorted (initial_location, fde_offset) pairs.
		 * Walk the synthetic .eh_frame to extract FDE locations. */
		size_t pos = 0;
		int table_idx = 0;
		while (pos < ehf_pos - 4) {
			uint32_t length;
			memcpy(&length, ehf_buf + pos, 4);
			if (length == 0) break;

			uint32_t cie_id;
			memcpy(&cie_id, ehf_buf + pos + 4, 4);

			if (cie_id != 0) {
				/* FDE — read initial_location */
				uint64_t init_loc;
				memcpy(&init_loc, ehf_buf + pos + 8, 8);

				/* Table entry: (initial_location, fde_ptr) — both absptr */
				uint64_t fde_ptr = (uint64_t)(uintptr_t)(ehf_buf + pos);
				memcpy(h + hp, &init_loc, 8); hp += 8;
				memcpy(h + hp, &fde_ptr, 8); hp += 8;
				table_idx++;
			}

			pos += 4 + length;
		}

		macho_eh_frame_hdr_size = hp;
		fprintf(stderr, "eh_frame: built .eh_frame_hdr (%zu bytes, %d entries)\n",
		        hp, table_idx);
	}

	/* Register with __register_frame so the traditional unwinder path
	 * (GCC <15 / glibc <2.35) can find our FDEs. This is the only
	 * mechanism on older systems — _dl_find_object doesn't exist. */
	{
		void (*reg_frame)(const void *) = dlsym(RTLD_DEFAULT, "__register_frame");
		if (reg_frame) {
			reg_frame(ehf_buf);
			fprintf(stderr, "eh_frame: registered %d FDEs via __register_frame\n",
			        fdes_emitted);
		} else {
			fprintf(stderr, "eh_frame: WARNING: __register_frame not found\n");
		}
	}

	/* Resolve real _dl_find_object from glibc.
	 * Our interposed _dl_find_object (defined above) is called by libgcc_s
	 * because the main executable's symbols take precedence over glibc's.
	 * We need the real one for forwarding non-Mach-O queries. */
	{
		/* RTLD_NEXT from the main executable points to glibc's version */
		real_dl_find_object = dlsym(RTLD_NEXT, "_dl_find_object");
		if (real_dl_find_object)
			fprintf(stderr, "eh_frame: _dl_find_object hook active for %p..%p\n",
			        (void*)macho_text_start, (void*)macho_text_end);
		else
			fprintf(stderr, "eh_frame: using __register_frame only "
			        "(no _dl_find_object in this glibc)\n");
	}

	return 0;
}
