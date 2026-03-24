/*
 * Standalone thread support for Mach-O loader on aarch64 Linux.
 * Stripped of all darlingserver RPC dependencies.
 */

#include "threads.h"
#include <pthread.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <setjmp.h>
#include <stdio.h>

#include "dthreads.h"

static __thread jmp_buf t_jmpbuf;
static __thread void* t_freeaddr;
static __thread size_t t_freesize;

typedef void (*thread_ep)(void**, int, ...);
struct arg_struct
{
	thread_ep entry_point;
	uintptr_t real_entry_point;
	uintptr_t arg1;
	uintptr_t arg2;
	uintptr_t arg3;
	union {
		void* _backwards_compat;
		int port;
	};
	unsigned long pth_obj_size;
	void* pth;
	darling_thread_create_callbacks_t callbacks;
	uintptr_t stack_bottom;
	uintptr_t stack_addr;
	bool is_workqueue;
};

static void* darling_thread_entry(void* p);

#ifndef PTHREAD_STACK_MIN
#	define PTHREAD_STACK_MIN 16384
#endif

#define DEFAULT_DTHREAD_GUARD_SIZE 0x1000

static inline void *align_16(uintptr_t ptr) {
	return (void *) ((uintptr_t) ptr & ~(uintptr_t) 15);
}

static dthread_t dthread_structure_init(dthread_t dthread, size_t guard_size, void* stack_addr, size_t stack_size, void* base_addr, size_t total_size) {
	dthread->sig = (uintptr_t)dthread;

	dthread->tsd[DTHREAD_TSD_SLOT_PTHREAD_SELF] = dthread;
	dthread->tsd[DTHREAD_TSD_SLOT_ERRNO] = &dthread->err_no;
	dthread->tsd[DTHREAD_TSD_SLOT_PTHREAD_QOS_CLASS] = (void*)(uintptr_t)(DTHREAD_DEFAULT_PRIORITY);
	dthread->tsd[DTHREAD_TSD_SLOT_PTR_MUNGE] = 0;
	dthread->tl_has_custom_stack = 0;
	dthread->lock = (darwin_os_unfair_lock){0};

	dthread->stackaddr = stack_addr;
	dthread->stackbottom = (char*)stack_addr - stack_size;
	dthread->freeaddr = base_addr;
	dthread->freesize = total_size;
	dthread->guardsize = guard_size;

	dthread->cancel_state = DTHREAD_CANCEL_ENABLE | DTHREAD_CANCEL_DEFERRED;

	dthread->tl_joinable = 1;
	dthread->inherit = DTHREAD_INHERIT_SCHED;
	dthread->tl_policy = DARWIN_POLICY_TIMESHARE;

	return dthread;
}

static dthread_t dthread_structure_allocate(size_t stack_size, size_t guard_size, void** stack_addr) {
	size_t total_size = guard_size + stack_size + sizeof(struct _dthread);

	void* base_addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	mprotect(base_addr, guard_size, PROT_NONE);

	*stack_addr = ((char*)base_addr) + stack_size + guard_size;

	dthread_t dthread = (dthread_t)*stack_addr;
	memset(dthread, 0, sizeof(struct _dthread));

	return dthread_structure_init(dthread, guard_size, *stack_addr, stack_size, base_addr, total_size);
}

void* __darling_thread_create(unsigned long stack_size, unsigned long pth_obj_size,
				void* entry_point, uintptr_t real_entry_point,
				uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
				darling_thread_create_callbacks_t callbacks, void* pth)
{
	struct arg_struct args = {
		.entry_point      = (thread_ep)entry_point,
		.real_entry_point = real_entry_point,
		.arg1             = arg1,
		.arg2             = arg2,
		.arg3             = arg3,
		.port             = 0,
		.pth_obj_size     = pth_obj_size,
		.pth              = NULL,
		.callbacks        = callbacks,
		.stack_addr       = 0,
		.is_workqueue     = real_entry_point == 0,
	};
	pthread_attr_t attr;
	pthread_t nativeLibcThread;

	pthread_attr_init(&attr);

	if (pth == NULL || args.is_workqueue) {
		pth = dthread_structure_allocate(stack_size, DEFAULT_DTHREAD_GUARD_SIZE, (void**)&args.stack_addr);
	} else if (!args.is_workqueue) {
		args.stack_addr = arg2;
	}

	args.stack_bottom = args.stack_addr - stack_size;

	pthread_attr_setstacksize(&attr, 4096);

	args.pth = pth;
	pthread_create(&nativeLibcThread, &attr, darling_thread_entry, &args);
	pthread_attr_destroy(&attr);

	while (args.pth != NULL)
		sched_yield();

	return pth;
}

static void* darling_thread_entry(void* p)
{
	struct arg_struct* in_args = (struct arg_struct*) p;
	struct arg_struct args;

	memcpy(&args, in_args, sizeof(args));

	dthread_t dthread = args.pth;
	uintptr_t* flags = args.is_workqueue ? &args.arg2 : &args.arg3;

	/* No darlingserver RPC — just set up TSD base directly */
	if (args.callbacks && args.callbacks->thread_set_tsd_base) {
		args.callbacks->thread_set_tsd_base(&dthread->tsd[0], 0);
	}
	*flags |= args.is_workqueue ? DWQ_FLAG_THREAD_TSD_BASE_SET : DTHREAD_START_TSD_BASE_SET;

	/* Get thread self port via callback if available */
	if (args.callbacks && args.callbacks->thread_self_trap) {
		int thread_self_port = args.callbacks->thread_self_trap();
		dthread->tsd[DTHREAD_TSD_SLOT_MACH_THREAD_SELF] = (void*)(intptr_t)thread_self_port;
		args.port = thread_self_port;
	}

	in_args->pth = NULL;

	if (setjmp(t_jmpbuf))
	{
		munmap(t_freeaddr, t_freesize);
		pthread_detach(pthread_self());
		return NULL;
	}

	void *stack_ptr = align_16(args.stack_addr);

	uintptr_t arg3_val = args.real_entry_point;
	if (arg3_val == 0) {
		arg3_val = (uintptr_t) args.stack_bottom;
	}

#ifdef __aarch64__
	register void*     r_arg1 asm("x0") = args.pth;
	register int       r_arg2 asm("w1") = args.port;
	register uintptr_t r_arg3 asm("x2") = arg3_val;
	register uintptr_t r_arg4 asm("x3") = args.arg1;
	register uintptr_t r_arg5 asm("x4") = args.arg2;
	register uintptr_t r_arg6 asm("x5") = args.arg3;

	asm volatile(
		"mov x29, #0\n"
		"mov x30, #0\n"
		"mov sp, %[stack_ptr]\n"
		"br %[entry_point]" ::
		"r"(r_arg1), "r"(r_arg2), "r"(r_arg3), "r"(r_arg4), "r"(r_arg5), "r"(r_arg6),
		[entry_point] "r"(args.entry_point),
		[stack_ptr] "r"(stack_ptr)
	);
#elif defined(__x86_64__)
	register void*     r_arg1 asm("rdi") = args.pth;
	register int       r_arg2 asm("esi") = args.port;
	register uintptr_t r_arg3 asm("rdx") = arg3_val;
	register uintptr_t r_arg4 asm("rcx") = args.arg1;
	register uintptr_t r_arg5 asm("r8")  = args.arg2;
	register uintptr_t r_arg6 asm("r9")  = args.arg3;

	asm volatile(
		"xorq %%rbp, %%rbp\n"
		"movq %[stack_ptr], %%rsp\n"
		"pushq $0\n"
		"jmp *%[entry_point]" ::
		"r"(r_arg1),"r"(r_arg2),"r"(r_arg3),"r"(r_arg4),"r"(r_arg5),"r"(r_arg6),
		[entry_point] "r"(args.entry_point),
		[stack_ptr] "r"(stack_ptr)
	);
#else
	#error Not implemented
#endif
	__builtin_unreachable();
}

int __darling_thread_terminate(void* stackaddr,
				unsigned long freesize, unsigned long pthobj_size)
{
	(void)pthobj_size;

	if (getpid() == syscall(SYS_gettid))
	{
		/* Main thread — just hang around */
		sigset_t mask;
		memset(&mask, 0, sizeof(mask));

		while (1)
			sigsuspend(&mask);
	}

	t_freeaddr = stackaddr;
	t_freesize = freesize;

	longjmp(t_jmpbuf, 1);

	__builtin_unreachable();
}

extern void* __machismo_main_stack_top;

void* __darling_thread_get_stack(void)
{
	return __machismo_main_stack_top;
}
