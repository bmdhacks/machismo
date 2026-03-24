#ifndef DYLD_THREADS_H
#define DYLD_THREADS_H
#include <stdint.h>
#include "elfcalls.h"

#ifdef __cplusplus
extern "C" {
#endif

void* __darling_thread_create(unsigned long stack_size, unsigned long pthobj_size,
				void* entry_point, uintptr_t arg3,
				uintptr_t arg4, uintptr_t arg5, uintptr_t arg6,
				darling_thread_create_callbacks_t, void* dthread);
int __darling_thread_terminate(void* stackaddr,
				unsigned long freesize, unsigned long pthobj_size);
void* __darling_thread_get_stack(void);

#ifdef __cplusplus
}
#endif

#endif
