/*
 * Generic binary patcher for loaded Mach-O binaries.
 *
 * Reads a patch file and applies instruction-level patches to the
 * loaded binary. Used for game-specific fixes like bypassing Steam
 * checks, framerate caps, etc.
 *
 * Patch file format (one per line):
 *   <virtual_address_hex> <instruction_hex> [# comment]
 *
 * Example:
 *   0x10030285c 1F2003D5  # NOP out Steam failure check
 */

#ifndef PATCHER_H
#define PATCHER_H

#include <stdint.h>

/*
 * Apply patches from a file to the loaded binary.
 *
 * Parameters:
 *   patch_file - path to the patch config file
 *   slide      - ASLR slide (added to addresses in the patch file)
 *
 * Returns:
 *   Number of patches applied, or -1 on error
 */
int patcher_apply(const char* patch_file, uintptr_t slide);

#endif /* PATCHER_H */
