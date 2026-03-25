#ifndef HEAP_TRACE_H
#define HEAP_TRACE_H

#include <stddef.h>
#include <stdint.h>

/* Heaptrack-compatible allocation tracer for machismo.
 *
 * Activated via MACHISMO_HEAPTRACK=<filename> environment variable.
 * Writes heaptrack v1 text trace viewable in heaptrack_gui.
 */

void heap_trace_init(void);
void heap_trace_close(void);
void heap_trace_alloc(void *ptr, size_t size);
void heap_trace_free(void *ptr);
void heap_trace_realloc(void *old_ptr, void *new_ptr, size_t new_size);

/* Register a symbol resolver for Mach-O addresses.
 * The callback takes an address and returns a symbol name, or NULL. */
typedef const char *(*heap_trace_sym_resolver_fn)(uintptr_t addr);
void heap_trace_set_symbol_resolver(heap_trace_sym_resolver_fn fn);

extern int heap_trace_active;

#endif
