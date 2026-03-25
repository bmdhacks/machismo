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

/*
 * Look up a Mach-O symbol name by address.
 * Returns the symbol name (stripped of leading underscore), or NULL.
 * Uses the symbol table retained from gdb_jit_register_macho.
 */
const char *gdb_jit_lookup_addr(uintptr_t addr);

/*
 * Look up a Mach-O symbol address by name (forward lookup).
 * The name should NOT have the leading Mach-O underscore
 * (e.g., "_ZN3wos..." not "__ZN3wos...").
 * Returns the runtime address, or 0 if not found.
 * Linear scan — call once at init and cache the result.
 */
uintptr_t gdb_jit_lookup_name(const char *name);

#endif /* GDB_JIT_H */
