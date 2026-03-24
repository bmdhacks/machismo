/*
 * GDB JIT Debug Symbol Registration for Mach-O binaries.
 *
 * Builds an in-memory ELF object from the Mach-O symbol table and
 * registers it with GDB via the JIT Compilation Interface. This allows
 * GDB to show proper function names in backtraces for Mach-O code.
 *
 * See: https://sourceware.org/gdb/current/onlinedocs/gdb/JIT-Interface.html
 */

#ifndef GDB_JIT_H
#define GDB_JIT_H

#include <stdint.h>

/*
 * Register Mach-O symbols with GDB's JIT interface.
 *
 * Parses LC_SYMTAB from the loaded Mach-O header, builds a minimal
 * in-memory ELF with .symtab/.strtab, and registers it so GDB can
 * resolve function names in backtraces.
 *
 * Parameters:
 *   mh    - Pointer to the mapped mach_header_64
 *   slide - ASLR slide applied to the binary
 *
 * Returns:
 *   Number of symbols registered, or -1 on failure
 */
int gdb_jit_register_macho(void* mh, uintptr_t slide);

#endif /* GDB_JIT_H */
