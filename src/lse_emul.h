#ifndef _LSE_EMUL_H_
#define _LSE_EMUL_H_

#include <stdint.h>
#include <stddef.h>

/*
 * ARMv8.1 LSE (Large System Extensions) atomic instruction emulation.
 *
 * Apple Silicon (A12+) supports LSE atomics (LDADD, SWP, CAS, etc.)
 * which are not available on ARMv8.0 cores (Cortex-A35/A53/A55).
 *
 * This module replaces each LSE instruction in-place with a branch
 * to an island containing an equivalent LDXR/STXR loop.
 */

/* Patch all LSE atomic instructions in a code region.
 * code:      start of executable code (must be writable)
 * code_size: size of code region in bytes
 * pool:      pointer to current position in island pool (advanced on return)
 * pool_end:  end of island pool
 * Returns:   number of instructions patched */
int lse_emul_patch(uint32_t *code, size_t code_size,
                   uint32_t **pool, uint32_t *pool_end);

#endif /* _LSE_EMUL_H_ */
