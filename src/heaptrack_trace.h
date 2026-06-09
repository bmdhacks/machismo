#ifndef _MACHISMO_HEAPTRACK_TRACE_H_
#define _MACHISMO_HEAPTRACK_TRACE_H_

#include <stddef.h>

/*
 * Built-in heaptrack-compatible allocation tracer for machismo.
 *
 * Emits a heaptrack v1 text trace (viewable in heaptrack_print / heaptrack_gui)
 * directly from the loader instead of using heaptrack's LD_PRELOAD malloc
 * interceptor — heaptrack cannot symbolicate the guest's anonymously-mapped
 * Mach-O image, but machismo owns the guest symbol tables and can.
 *
 * Ported from /home/bmd/hhg/hashlink/src/gc_trace.c (the working reference
 * implementation); the deltas are (1) allocations are sourced from the
 * libsystem_shim malloc hooks and (2) guest frames are symbolicated against a
 * reverse Mach-O LC_SYMTAB index instead of HashLink's JIT resolver.
 *
 * Activated via the MACHISMO_HEAPTRACK=<path> environment variable. When unset,
 * machismo_heaptrack_active stays 0 and the shim never calls the hooks.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Fast-path activation flag. The shim loads &this via dlsym and checks it
 * before calling the hooks; 0 => tracing disabled (near-zero overhead). */
extern int machismo_heaptrack_active;

/* Lifecycle (called from machismo.c main). */
void machismo_heaptrack_init(void);    /* reads MACHISMO_HEAPTRACK; no-op if unset */
void machismo_heaptrack_close(void);   /* flush + close; registered via atexit */

/* Register a loaded Mach-O image's LC_SYMTAB into the reverse symbol index.
 * Call once per image (main binary + each Mach-O dylib) after load, before the
 * guest runs. module_name is copied. */
void machismo_heaptrack_add_image(void *mh, unsigned long slide,
                                  const char *module_name);

/* Allocation hooks — invoked by the shim (resolved via dlsym RTLD_DEFAULT). */
void machismo_heaptrack_alloc(void *ptr, size_t size);
void machismo_heaptrack_free(void *ptr);
void machismo_heaptrack_realloc(void *old_ptr, void *new_ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _MACHISMO_HEAPTRACK_TRACE_H_ */
