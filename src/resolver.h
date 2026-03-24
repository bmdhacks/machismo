#ifndef _RESOLVER_H_
#define _RESOLVER_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Resolve chained fixups in a loaded Mach-O binary.
 *
 * Parses LC_DYLD_CHAINED_FIXUPS, walks the pointer chains in __DATA_CONST
 * and __DATA segments, and patches bind slots with native Linux .so symbols
 * via dlopen/dlsym. Also applies rebase fixups for ASLR slide.
 *
 * Parameters:
 *   mh        - pointer to the mapped mach_header_64 in memory
 *   slide     - ASLR slide applied during loading (usually mh - preferred_load_address)
 *   map_file  - path to dylib mapping config file (NULL for default behavior)
 *
 * Returns 0 on success, -1 on error.
 */
int resolver_resolve_fixups(void* mh, uintptr_t slide, const char* map_file);

#endif /* _RESOLVER_H_ */
