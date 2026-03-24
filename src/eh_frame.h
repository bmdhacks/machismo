/*
 * Compact unwind → DWARF .eh_frame converter for Mach-O on Linux.
 *
 * Parses Apple's __TEXT,__unwind_info section and generates a synthetic
 * DWARF .eh_frame section that can be registered with the system unwinder
 * via __register_frame(). This enables C++ exceptions thrown in Mach-O
 * code to be caught by try/catch handlers.
 */

#ifndef _MACHISMO_EH_FRAME_H_
#define _MACHISMO_EH_FRAME_H_

#include <stdint.h>

/*
 * Register Mach-O exception handling frames with the system unwinder.
 *
 * Parses __TEXT,__unwind_info (compact unwind), converts all entries to
 * DWARF .eh_frame FDEs, and registers them via __register_frame().
 * Also registers the existing __TEXT,__eh_frame section.
 *
 * Must be called after the resolver (GOT entries for personality functions
 * must be patched) but before any Mach-O code runs (__init_offsets, _main).
 *
 * Returns 0 on success, -1 on failure.
 */
int eh_frame_register_macho(void* mh, uintptr_t slide);

#endif /* _MACHISMO_EH_FRAME_H_ */
