#ifndef _TRAMPOLINE_H_
#define _TRAMPOLINE_H_

#include <stdint.h>
#include <stddef.h>

/*
 * Provide a pre-allocated pool for branch islands.
 * Must be called before any trampoline_patch_* function.
 * The pool must be RWX and within ±128MB of the Mach-O __TEXT.
 */
void trampoline_set_pool(void* base, size_t size);

/*
 * Override: redirect specific symbols to custom functions instead of dlsym.
 */
typedef struct {
	const char* symbol_name;   /* Mach-O symbol name (with leading _) */
	void* target;              /* function pointer to redirect to */
} trampoline_override_t;

/*
 * Patch statically-linked functions matching any of the given prefixes
 * in a loaded Mach-O binary with trampolines to a native Linux .so.
 *
 * If overrides is non-NULL, matching symbols redirect to the override
 * target instead of the dlsym result.
 *
 * Returns number of functions patched, or -1 on error.
 */
int trampoline_patch_lib(void* mh, uintptr_t slide,
                         const char* lib_path,
                         const char** prefixes, int num_prefixes,
                         trampoline_override_t* overrides, int num_overrides);

/*
 * Patch Mach-O functions by exact symbol match against an override .so.
 *
 * For each defined N_SECT symbol in __TEXT, strips one leading underscore
 * and calls dlsym(override_handle, name). If found, writes a trampoline.
 *
 * By default only N_EXT (exported) symbols are considered. Set match_local
 * to also detour LOCAL defined symbols — required for hooking internal,
 * hidden-visibility engine methods (e.g. the Gothic RHI) that are present in
 * an unstripped binary's symtab but not exported. Matching is still by exact
 * mangled name, so only symbols the override .so actually exports are touched.
 *
 * Returns number of functions patched, or -1 on error.
 */
int trampoline_patch_overrides(void* mh, uintptr_t slide, void* override_handle,
                               int match_local);

/*
 * Legacy API — reads MACHISMO_TRAMPOLINE_LIB and MACHISMO_TRAMPOLINE_PREFIX env vars.
 * Kept for backward compatibility with tests.
 */
int trampoline_patch(void* mh, uintptr_t slide);

/*
 * Guard __DATA pages containing library symbols against stale access.
 * Call after all trampolines are applied. Aborts on access to guarded pages.
 *
 * prefixes/num_prefixes: union of all non-STUB trampoline prefixes.
 */
void trampoline_guard_stale_data(void* mh, uintptr_t slide,
                                  const char** prefixes, int num_prefixes);

/*
 * Report whether a faulting address lies in a __DATA page guarded by
 * trampoline_guard_stale_data (stale Mach-O library-data access). Used by the
 * unified crash handler to label such faults. Returns 1 if guarded, else 0.
 * Pure array reads — safe to call from a signal handler.
 */
int trampoline_is_guarded_fault(uintptr_t addr);

#endif /* _TRAMPOLINE_H_ */
