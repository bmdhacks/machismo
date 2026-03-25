/*
 * libSystem.B.dylib shim for aarch64 Linux.
 *
 * Maps Apple libSystem symbols to glibc equivalents. Standard C/POSIX
 * symbols (malloc, pthread_create, open, strlen, etc.) resolve through
 * normal glibc linking — this file only implements Apple-specific symbols
 * that don't exist on Linux.
 *
 * Built as a shared library:
 *   gcc -shared -fPIC -o libsystem_shim.so libsystem_shim.c -lm -lpthread -ldl
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <wctype.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include "heap_trace.h"

/* ===== Mach time ===== */

/* mach_absolute_time returns nanoseconds on arm64 (timebase is always 1:1) */
uint64_t mach_absolute_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct mach_timebase_info_data {
	uint32_t numer;
	uint32_t denom;
};

int mach_timebase_info(struct mach_timebase_info_data *info)
{
	/* On arm64, the timebase is always 1:1 (nanoseconds) */
	info->numer = 1;
	info->denom = 1;
	return 0; /* KERN_SUCCESS */
}

/* ===== Mach ports / task info (stubs) ===== */

/* mach_task_self_ is a global variable in libSystem, not a function */
uint32_t mach_task_self_ = 0x103; /* dummy port number */

/* task_info — stub, returns KERN_FAILURE */
int task_info(uint32_t target_task, uint32_t flavor,
              void *task_info_out, uint32_t *task_info_count)
{
	(void)target_task; (void)flavor;
	(void)task_info_out; (void)task_info_count;
	return 5; /* KERN_FAILURE */
}

/* mach_msg — stub, returns MACH_MSG_SUCCESS (0) */
int mach_msg(void *msg, uint32_t option, uint32_t send_size,
             uint32_t rcv_size, uint32_t rcv_name,
             uint32_t timeout, uint32_t notify)
{
	(void)msg; (void)option; (void)send_size;
	(void)rcv_size; (void)rcv_name; (void)timeout; (void)notify;
	return 0;
}

/* mach_port_deallocate — stub */
int mach_port_deallocate(uint32_t task, uint32_t name)
{
	(void)task; (void)name;
	return 0;
}

/* ===== Apple stdio globals ===== */

/* Apple's libSystem exports these as global FILE* pointers.
 * We define them as initialized globals pointing to glibc's streams.
 * Note: these are initialized at load time via __attribute__((constructor)). */
FILE* __stdinp;
FILE* __stdoutp;
FILE* __stderrp;

__attribute__((constructor))
static void init_stdio_globals(void)
{
	__stdinp  = stdin;
	__stdoutp = stdout;
	__stderrp = stderr;
	heap_trace_init();
}

__attribute__((destructor))
static void fini_shim(void)
{
	heap_trace_close();
}

/* ===== errno ===== */

/* Apple's __error() returns a pointer to errno (like __errno_location on Linux) */
int* __error(void)
{
	return __errno_location();
}

/* ===== Stack protector ===== */

/* __chkstk_darwin is a stack probing function. On Linux, the kernel handles
 * stack growth automatically via guard pages, so this is a no-op. */
void __chkstk_darwin(void)
{
	/* no-op */
}

/* __stack_chk_guard and __stack_chk_fail are provided by glibc.
 * We don't need to define them — they'll resolve through normal linking. */

/* ===== Math shims ===== */

/* Adapted from Darling's src/libm/Source/sincos.c */

struct __float2 { float __sinval; float __cosval; };
struct __double2 { double __sinval; double __cosval; };

struct __float2 __sincosf_stret(float v)
{
	struct __float2 rv = { sinf(v), cosf(v) };
	return rv;
}

struct __double2 __sincos_stret(double v)
{
	struct __double2 rv = { sin(v), cos(v) };
	return rv;
}

struct __float2 __sincospif_stret(float v)
{
	return __sincosf_stret(v * (float)M_PI);
}

struct __double2 __sincospi_stret(double v)
{
	return __sincos_stret(v * M_PI);
}

/* Adapted from Darling's src/libm/Source/exp10.c */

double __exp10(double x)
{
	return pow(10.0, x);
}

float __exp10f(float x)
{
	return powf(10.0f, x);
}

/* ===== HOME path rewrite ===== */

/*
 * macOS games use $HOME/Library/Preferences/ for save data.
 * Redirect $HOME to a "userdata" directory next to the game binary
 * so we don't pollute the user's Linux home directory with macOS
 * directory structures.
 *
 * The game calls getenv("HOME") and appends "/Library/Preferences/...".
 * By returning "./userdata" (relative to CWD, which machismo sets to the
 * binary's directory), saves go to <game_dir>/userdata/Library/Preferences/.
 */
static char fake_home[4096] = {0};

static const char* get_fake_home(void)
{
	if (fake_home[0])
		return fake_home;

	/* Build path: <directory of game binary>/userdata */
	char exe[4096];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len > 0) {
		exe[len] = '\0';
		/* Find last '/' to get directory */
		char *slash = strrchr(exe, '/');
		if (slash) {
			*slash = '\0';
			snprintf(fake_home, sizeof(fake_home), "%.4085s/userdata", exe);
		}
	}
	if (!fake_home[0])
		snprintf(fake_home, sizeof(fake_home), "./userdata");

	fprintf(stderr, "libsystem_shim: HOME rewritten to %s\n", fake_home);
	return fake_home;
}

char *shim_getenv(const char *name) __asm__("getenv");
char *shim_getenv(const char *name)
{
	if (name && strcmp(name, "HOME") == 0)
		return (char*)get_fake_home();

	/* Forward everything else to real getenv.
	 * Use dlsym to avoid recursion since we replaced getenv. */
	static char *(*real_getenv)(const char*) = NULL;
	if (!real_getenv)
		real_getenv = dlsym(RTLD_NEXT, "getenv");
	return real_getenv(name);
}

/* ===== _NSGetExecutablePath ===== */

int _NSGetExecutablePath(char *buf, uint32_t *bufsize)
{
	if (!buf || !bufsize) return -1;

	ssize_t len = readlink("/proc/self/exe", buf, *bufsize - 1);
	if (len < 0) return -1;

	if ((uint32_t)len >= *bufsize) {
		*bufsize = (uint32_t)len + 1;
		return -1; /* buffer too small */
	}

	buf[len] = '\0';
	*bufsize = (uint32_t)len;
	return 0;
}

/* ===== Apple locale / ctype ===== */

/* _DefaultRuneLocale — Apple's locale data structure.
 * Games typically don't use it directly; it's referenced by the ctype macros.
 * Provide a minimal stub that won't crash if accessed. */
struct _RuneLocale {
	char __magic[8];
	char __encoding[32];
	/* Followed by large lookup tables — we zero-fill */
	uint32_t __runetype[256];
	int32_t  __maplower[256];
	int32_t  __mapupper[256];
};

static struct _RuneLocale _default_rune_locale;

/* Export as the Apple symbol name */
struct _RuneLocale* _DefaultRuneLocale = &_default_rune_locale;

/* ___maskrune — Apple's character classification */
int __maskrune(int c, unsigned long mask)
{
	/* Map to standard C iswctype where possible.
	 * For basic ASCII this is good enough. */
	(void)mask;
	if (c < 0 || c > 127) return 0;
	return isascii(c) ? (int)(mask & 0) : 0; /* conservative: return 0 */
}

/* ___tolower / ___toupper — Apple exports these as function pointers */
int __tolower(int c)
{
	return tolower(c);
}

int __toupper(int c)
{
	return toupper(c);
}

/* ===== BSD string functions ===== */

/* strlcpy/strlcat — available in glibc 2.38+, but provide fallbacks
 * for older systems */
#if !defined(__GLIBC__) || (__GLIBC__ < 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38)

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t srclen = strlen(src);
	if (size > 0) {
		size_t copylen = srclen < size - 1 ? srclen : size - 1;
		memcpy(dst, src, copylen);
		dst[copylen] = '\0';
	}
	return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
	size_t dstlen = strnlen(dst, size);
	if (dstlen == size) return size + strlen(src);
	return dstlen + strlcpy(dst + dstlen, src, size - dstlen);
}

#endif

/* ===== memset_pattern ===== */

void memset_pattern4(void *dst, const void *pattern, size_t len)
{
	const uint8_t *p = (const uint8_t *)pattern;
	uint8_t *d = (uint8_t *)dst;
	while (len >= 4) {
		memcpy(d, p, 4);
		d += 4;
		len -= 4;
	}
	if (len > 0) memcpy(d, p, len);
}

void memset_pattern8(void *dst, const void *pattern, size_t len)
{
	const uint8_t *p = (const uint8_t *)pattern;
	uint8_t *d = (uint8_t *)dst;
	while (len >= 8) {
		memcpy(d, p, 8);
		d += 8;
		len -= 8;
	}
	if (len > 0) memcpy(d, p, len);
}

void memset_pattern16(void *dst, const void *pattern, size_t len)
{
	const uint8_t *p = (const uint8_t *)pattern;
	uint8_t *d = (uint8_t *)dst;
	while (len >= 16) {
		memcpy(d, p, 16);
		d += 16;
		len -= 16;
	}
	if (len > 0) memcpy(d, p, len);
}

/* ===== pthread mutex ABI translation ===== */

/*
 * macOS pthread_mutex_t is 64 bytes: { long __sig; char __opaque[56]; }
 * where __sig = 0x32AAABA7 for PTHREAD_MUTEX_INITIALIZER.
 *
 * Linux/glibc aarch64 pthread_mutex_t is 48 bytes with PTHREAD_MUTEX_INITIALIZER = {0}.
 *
 * Since Linux's struct fits within macOS's, we can re-initialize in-place.
 * We detect macOS-format by checking for the signature at offset 0.
 */

#define DARWIN_PTHREAD_MUTEX_SIG 0x32AAABA7L
#define DARWIN_PTHREAD_RMUTEX_SIG 0x32AAABA1L
#define DARWIN_PTHREAD_COND_SIG 0x3CB0B1BBL

/*
 * We need wrappers that detect macOS-format pthread objects and re-init
 * them for Linux before use. The wrappers are exported with the standard
 * names (pthread_mutex_lock, etc.) so the resolver's dlsym finds them.
 * Inside, we call the real glibc functions via RTLD_NEXT.
 */

#include <dlfcn.h>

/* Real glibc pthread function pointers, resolved at load time */
static int (*real_pthread_mutex_lock)(pthread_mutex_t *);
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *);
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *);
static int (*real_pthread_mutex_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
static int (*real_pthread_mutex_destroy)(pthread_mutex_t *);
static int (*real_pthread_cond_wait)(pthread_cond_t *, pthread_mutex_t *);
static int (*real_pthread_cond_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static int (*real_pthread_cond_signal)(pthread_cond_t *);
static int (*real_pthread_cond_broadcast)(pthread_cond_t *);
static int (*real_pthread_cond_init)(pthread_cond_t *, const pthread_condattr_t *);
static int (*real_pthread_cond_destroy)(pthread_cond_t *);
static int (*real_pthread_rwlock_rdlock)(pthread_rwlock_t *);
static int (*real_pthread_rwlock_wrlock)(pthread_rwlock_t *);
static int (*real_pthread_rwlock_unlock)(pthread_rwlock_t *);

__attribute__((constructor(101)))
static void init_pthread_wrappers(void)
{
	/* Resolve real glibc pthread functions via RTLD_NEXT.
	 * This skips our own definitions and finds glibc's. */
	real_pthread_mutex_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
	real_pthread_mutex_unlock = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
	real_pthread_mutex_trylock = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
	real_pthread_mutex_init = dlsym(RTLD_NEXT, "pthread_mutex_init");
	real_pthread_mutex_destroy = dlsym(RTLD_NEXT, "pthread_mutex_destroy");
	real_pthread_cond_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
	real_pthread_cond_timedwait = dlsym(RTLD_NEXT, "pthread_cond_timedwait");
	real_pthread_cond_signal = dlsym(RTLD_NEXT, "pthread_cond_signal");
	real_pthread_cond_broadcast = dlsym(RTLD_NEXT, "pthread_cond_broadcast");
	real_pthread_cond_init = dlsym(RTLD_NEXT, "pthread_cond_init");
	real_pthread_cond_destroy = dlsym(RTLD_NEXT, "pthread_cond_destroy");
	real_pthread_rwlock_rdlock = dlsym(RTLD_NEXT, "pthread_rwlock_rdlock");
	real_pthread_rwlock_wrlock = dlsym(RTLD_NEXT, "pthread_rwlock_wrlock");
	real_pthread_rwlock_unlock = dlsym(RTLD_NEXT, "pthread_rwlock_unlock");

	if (!real_pthread_mutex_lock)
		fprintf(stderr, "libsystem_shim: WARNING: could not resolve real pthread_mutex_lock\n");
	else
		fprintf(stderr, "libsystem_shim: pthread wrappers initialized (real=%p)\n", real_pthread_mutex_lock);
}

/* Check if this looks like a macOS-initialized mutex and reinit for Linux */
static inline void fixup_mutex(pthread_mutex_t *mutex)
{
	long *sig = (long *)mutex;
	if (*sig == DARWIN_PTHREAD_MUTEX_SIG || *sig == DARWIN_PTHREAD_RMUTEX_SIG) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		memset(mutex, 0, sizeof(pthread_mutex_t));
		real_pthread_mutex_init(mutex, &attr);
		pthread_mutexattr_destroy(&attr);
	}
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	fixup_mutex(mutex);
	/* Upgrade zero-initialized (PTHREAD_MUTEX_INITIALIZER) mutexes to
	 * recursive. macOS game code may re-lock from the same thread, which
	 * works on macOS but deadlocks on Linux with PTHREAD_MUTEX_NORMAL. */
	static const char zeros[sizeof(pthread_mutex_t)] = {0};
	if (memcmp(mutex, zeros, sizeof(pthread_mutex_t)) == 0) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		real_pthread_mutex_init(mutex, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	return real_pthread_mutex_lock(mutex);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	return real_pthread_mutex_unlock(mutex);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	fixup_mutex(mutex);
	return real_pthread_mutex_trylock(mutex);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	memset(mutex, 0, sizeof(pthread_mutex_t));
	/* On macOS, PTHREAD_MUTEX_DEFAULT allows same-thread re-locking without
	 * deadlock in some configurations. Make all mutexes recursive to match
	 * this behavior, unless the caller explicitly sets a type. */
	if (!attr) {
		pthread_mutexattr_t rattr;
		pthread_mutexattr_init(&rattr);
		pthread_mutexattr_settype(&rattr, PTHREAD_MUTEX_RECURSIVE);
		int ret = real_pthread_mutex_init(mutex, &rattr);
		pthread_mutexattr_destroy(&rattr);
		return ret;
	}
	return real_pthread_mutex_init(mutex, attr);
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	return real_pthread_mutex_destroy(mutex);
}

/* Same issue for pthread_cond_t */
static inline void fixup_cond(pthread_cond_t *cond)
{
	long *sig = (long *)cond;
	if (*sig == DARWIN_PTHREAD_COND_SIG) {
		memset(cond, 0, sizeof(pthread_cond_t));
		real_pthread_cond_init(cond, NULL);
	}
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	fixup_cond(cond);
	fixup_mutex(mutex);
	return real_pthread_cond_wait(cond, mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime)
{
	fixup_cond(cond);
	fixup_mutex(mutex);
	return real_pthread_cond_timedwait(cond, mutex, abstime);
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	fixup_cond(cond);
	return real_pthread_cond_signal(cond);
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
	fixup_cond(cond);
	return real_pthread_cond_broadcast(cond);
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	memset(cond, 0, sizeof(pthread_cond_t));
	return real_pthread_cond_init(cond, attr);
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	return real_pthread_cond_destroy(cond);
}

/* pthread_rwlock_t also has macOS signature: 0x2DA8B3B4 */
#define DARWIN_PTHREAD_RWLOCK_SIG 0x2DA8B3B4L

static inline void fixup_rwlock(pthread_rwlock_t *rwlock)
{
	long *sig = (long *)rwlock;
	if (*sig == DARWIN_PTHREAD_RWLOCK_SIG) {
		memset(rwlock, 0, sizeof(pthread_rwlock_t));
		pthread_rwlock_init(rwlock, NULL);
	}
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
	fixup_rwlock(rwlock);
	return real_pthread_rwlock_rdlock(rwlock);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
	fixup_rwlock(rwlock);
	return real_pthread_rwlock_wrlock(rwlock);
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
	return real_pthread_rwlock_unlock(rwlock);
}

/* ===== pthread Apple extensions ===== */

/* pthread_mach_thread_np — returns the "Mach thread port" for a pthread.
 * We return the Linux TID as a reasonable approximation. */
uint32_t pthread_mach_thread_np(pthread_t thread)
{
	(void)thread;
	return (uint32_t)syscall(SYS_gettid);
}

/* pthread_threadid_np — get a unique 64-bit thread ID */
int pthread_threadid_np(pthread_t thread, uint64_t *thread_id)
{
	(void)thread;
	if (!thread_id) return EINVAL;
	*thread_id = (uint64_t)syscall(SYS_gettid);
	return 0;
}

/* ===== sysctl ===== */

/* Minimal sysctl stub — games typically use it for CPU info.
 * Returns ENOTSUP for everything. */
int sysctl(const int *name, unsigned int namelen,
           void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen)
{
	(void)name; (void)namelen; (void)oldp; (void)oldlenp;
	(void)newp; (void)newlen;
	errno = ENOTSUP;
	return -1;
}

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                 const void *newp, size_t newlen)
{
	(void)name; (void)oldp; (void)oldlenp;
	(void)newp; (void)newlen;
	errno = ENOTSUP;
	return -1;
}

/* ===== Blocks runtime (stubs) ===== */

/* These are only used by Objective-C code paths (Cocoa/AppKit SDL2 backend).
 * After SDL2 trampolining, they should never be called. Provide stubs that
 * warn if hit. */

void *_NSConcreteGlobalBlock = NULL;
void *_NSConcreteStackBlock = NULL;

void _Block_object_assign(void *dst, const void *src, int flags)
{
	(void)dst; (void)src; (void)flags;
	fprintf(stderr, "libsystem_shim: WARNING: _Block_object_assign called (ObjC code path?)\n");
}

void _Block_object_dispose(const void *obj, int flags)
{
	(void)obj; (void)flags;
	fprintf(stderr, "libsystem_shim: WARNING: _Block_object_dispose called (ObjC code path?)\n");
}

/* ===== Grand Central Dispatch ===== */

/*
 * Minimal dispatch implementation for bx/bgfx threading.
 *
 * Imported symbols (from llvm-nm -U):
 *   __dispatch_main_q          — global DATA object (queue)
 *   _dispatch_async            — function
 *   _dispatch_data_create      — function
 *   _dispatch_get_global_queue — function
 *   _dispatch_release          — function
 *   _dispatch_retain           — function (implicit)
 *   _dispatch_semaphore_create — function
 *   _dispatch_semaphore_signal — function
 *   _dispatch_semaphore_wait   — function
 *   _dispatch_sync             — function
 *   _dispatch_time             — function
 *
 * Note: macOS dispatch_get_main_queue() is a macro for &_dispatch_main_q,
 * so the binary imports the global, not the function.
 */
#include <semaphore.h>

#define DISPATCH_OBJ_SEMAPHORE 1
#define DISPATCH_OBJ_QUEUE     2

struct dispatch_object {
	int type;
	int refcount;
};

struct dispatch_semaphore {
	int type;  /* DISPATCH_OBJ_SEMAPHORE */
	int refcount;
	sem_t sem;
};

struct dispatch_queue {
	int type;  /* DISPATCH_OBJ_QUEUE */
	int refcount;
	char label[64];
};

/* Global queues — the binary imports _dispatch_main_q as a DATA symbol.
 * After Mach-O underscore stripping, resolver looks for "_dispatch_main_q". */
struct dispatch_queue _dispatch_main_q = { DISPATCH_OBJ_QUEUE, 100, "com.apple.main-thread" };
static struct dispatch_queue _dispatch_global_default = { DISPATCH_OBJ_QUEUE, 100, "com.apple.root.default" };

/* ---- Semaphores (real, backed by POSIX sem_t) ---- */

void* dispatch_semaphore_create(long value)
{
	struct dispatch_semaphore *ds = (struct dispatch_semaphore *)calloc(1, sizeof(*ds));
	if (!ds) return NULL;
	ds->type = DISPATCH_OBJ_SEMAPHORE;
	ds->refcount = 1;
	sem_init(&ds->sem, 0, (unsigned int)value);
	return ds;
}

long dispatch_semaphore_wait(void *dsema, uint64_t timeout)
{
	struct dispatch_semaphore *ds = (struct dispatch_semaphore *)dsema;
	if (!ds) return -1;
	if (timeout == ~(uint64_t)0) {
		/* DISPATCH_TIME_FOREVER */
		return sem_wait(&ds->sem) == 0 ? 0 : -1;
	} else if (timeout == 0) {
		/* DISPATCH_TIME_NOW */
		return sem_trywait(&ds->sem) == 0 ? 0 : -1;
	} else {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		uint64_t ns = timeout;
		ts.tv_sec += ns / 1000000000ULL;
		ts.tv_nsec += ns % 1000000000ULL;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		return sem_timedwait(&ds->sem, &ts) == 0 ? 0 : -1;
	}
}

long dispatch_semaphore_signal(void *dsema)
{
	struct dispatch_semaphore *ds = (struct dispatch_semaphore *)dsema;
	if (!ds) return 0;
	sem_post(&ds->sem);
	return 0;
}

/* ---- Queues (stubs — async/sync execute inline) ---- */

void* dispatch_get_global_queue(long priority, unsigned long flags)
{
	(void)priority; (void)flags;
	return &_dispatch_global_default;
}

void* dispatch_queue_create(const char *label, void *attr)
{
	(void)attr;
	struct dispatch_queue *q = (struct dispatch_queue *)calloc(1, sizeof(*q));
	if (!q) return NULL;
	q->type = DISPATCH_OBJ_QUEUE;
	q->refcount = 1;
	if (label) strncpy(q->label, label, sizeof(q->label) - 1);
	return q;
}

/* ---- Async/sync (execute blocks inline — no real concurrency) ---- */

typedef void (*dispatch_block_t)(void);

void dispatch_async(void *queue, void *block)
{
	(void)queue;
	if (block) {
		/* Execute the block synchronously.
		 * Apple Blocks ABI: the function pointer is at offset 16 in the block literal. */
		void **blk = (void **)block;
		void (*invoke)(void *) = (void (*)(void *))blk[2];
		if (invoke) invoke(block);
	}
}

void dispatch_sync(void *queue, void *block)
{
	(void)queue;
	if (block) {
		void **blk = (void **)block;
		void (*invoke)(void *) = (void (*)(void *))blk[2];
		if (invoke) invoke(block);
	}
}

void dispatch_once(long *predicate, void *block)
{
	if (__sync_bool_compare_and_swap(predicate, 0, 1)) {
		if (block) {
			void **blk = (void **)block;
			void (*invoke)(void *) = (void (*)(void *))blk[2];
			if (invoke) invoke(block);
		}
		__sync_synchronize();
		*predicate = ~0L;
	} else {
		while (*predicate != ~0L)
			sched_yield();
	}
}

/* ---- Dispatch time ---- */

#define DISPATCH_TIME_NOW     0ULL
#define DISPATCH_TIME_FOREVER (~0ULL)

uint64_t dispatch_time(uint64_t when, int64_t delta)
{
	if (when == DISPATCH_TIME_FOREVER)
		return DISPATCH_TIME_FOREVER;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	if (when == DISPATCH_TIME_NOW)
		when = now;
	return when + (uint64_t)delta;
}

uint64_t dispatch_walltime(const struct timespec *when, int64_t delta)
{
	uint64_t t;
	if (when) {
		t = (uint64_t)when->tv_sec * 1000000000ULL + (uint64_t)when->tv_nsec;
	} else {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		t = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	}
	return t + (uint64_t)delta;
}

/* ---- Dispatch data (stub) ---- */

void* dispatch_data_create(const void *buffer, size_t size,
                           void *queue, void *destructor)
{
	(void)buffer; (void)size; (void)queue; (void)destructor;
	return NULL;
}

/* ---- Retain/release ---- */

void dispatch_release(void *object)
{
	if (!object) return;
	struct dispatch_object *obj = (struct dispatch_object *)object;
	if (obj->refcount > 99) return; /* global queues, never free */
	if (__sync_sub_and_fetch(&obj->refcount, 1) <= 0) {
		if (obj->type == DISPATCH_OBJ_SEMAPHORE) {
			struct dispatch_semaphore *ds = (struct dispatch_semaphore *)object;
			sem_destroy(&ds->sem);
		}
		free(object);
	}
}

void dispatch_retain(void *object)
{
	if (!object) return;
	struct dispatch_object *obj = (struct dispatch_object *)object;
	__sync_add_and_fetch(&obj->refcount, 1);
}

/* ===== Thread-local variable support (stubs) ===== */

void _tlv_atexit(void (*func)(void *), void *arg)
{
	(void)func; (void)arg;
	/* Best-effort: ignore TLV cleanup registration */
}

/*
 * Mach-O thread-local variable (TLV) bootstrap.
 *
 * Mach-O TLV descriptors have the layout:
 *   struct TLVDescriptor {
 *       void* (*thunk)(TLVDescriptor*);  // this function
 *       unsigned long key;                // pthread_key_t (0 = uninitialized)
 *       unsigned long offset;             // offset within TLV image
 *   };
 *
 * The thunk is called when a TLV is first accessed. It must return
 * a pointer to per-thread storage for the variable at `offset`.
 * We allocate a per-thread block using pthread keys and copy the
 * initial TLV image from __thread_data on first use.
 */

/* TLV image info — set by machismo before execution */
void* __tlv_image_base = NULL;   /* base of __DATA,__thread_data */
size_t __tlv_image_size = 0;     /* size of __thread_data */
size_t __tlv_bss_size = 0;       /* size of __thread_bss (after thread_data) */

struct tlv_descriptor {
	void* (*thunk)(struct tlv_descriptor*);
	unsigned long key;
	unsigned long offset;
};

static pthread_mutex_t tlv_mutex = PTHREAD_MUTEX_INITIALIZER;

static void tlv_destructor(void* block)
{
	free(block);
}

void* _tlv_bootstrap(struct tlv_descriptor* desc)
{
	/* Lazily create a pthread key for this TLV group */
	if (desc->key == 0) {
		pthread_mutex_lock(&tlv_mutex);
		if (desc->key == 0) {
			pthread_key_t key;
			pthread_key_create(&key, tlv_destructor);
			desc->key = (unsigned long)key + 1; /* +1 so 0 means uninitialized */
		}
		pthread_mutex_unlock(&tlv_mutex);
	}

	pthread_key_t key = (pthread_key_t)(desc->key - 1);
	void* block = pthread_getspecific(key);

	if (!block) {
		/* Allocate per-thread TLV block */
		size_t total = __tlv_image_size + __tlv_bss_size;
		if (total < 4096) total = 4096; /* minimum size */
		block = calloc(1, total);

		/* Copy initial values from __thread_data */
		if (__tlv_image_base && __tlv_image_size > 0)
			memcpy(block, __tlv_image_base, __tlv_image_size);

		pthread_setspecific(key, block);
	}

	return (char*)block + desc->offset;
}

/* ===== Darwin-suffixed symbol variants ===== */

/* Some symbols in the binary have $DARWIN_EXTSN suffix. The resolver strips
 * these suffixes during lookup, so fopen$DARWIN_EXTSN maps to fopen.
 * No action needed here — they resolve through normal glibc. */

/* ===== vfork ===== */

/* vfork is deprecated but available on Linux. We redirect to fork for safety. */
pid_t vfork(void)
{
	return fork();
}

/* ===== 128-bit division ===== */

/* __udivti3 is provided by libgcc / compiler-rt.
 * It should resolve through normal linking. No stub needed. */

/* ===== C++ exception unwinding ===== */

/* _Unwind_Resume is imported by the Mach-O from libSystem.B.dylib.
 * On macOS, libSystem re-exports it from libunwind. On Linux, it's
 * in libgcc_s.so.1. We forward to the real implementation via dlsym
 * so the resolver can find it when resolving libSystem symbols. */
void _Unwind_Resume(void* exception_object)
{
	static void (*real_unwind_resume)(void*) = NULL;
	if (!real_unwind_resume) {
		real_unwind_resume = dlsym(RTLD_NEXT, "_Unwind_Resume");
		if (!real_unwind_resume)
			real_unwind_resume = dlsym(RTLD_DEFAULT, "_Unwind_Resume");
	}
	if (real_unwind_resume)
		real_unwind_resume(exception_object);
}

/* ===== stat() ABI translation ===== */

/*
 * macOS aarch64 and Linux aarch64 have different struct stat layouts.
 * macOS st_mode is uint16_t at offset 4; Linux st_mode is uint32_t at
 * offset 16. The game allocates a Darwin-sized buffer (144 bytes) and
 * reads fields at Darwin offsets, so we call the real Linux stat via
 * syscall and convert the result to Darwin layout.
 */

struct darwin_stat {
	int32_t         st_dev;
	uint16_t        st_mode;
	uint16_t        st_nlink;
	uint64_t        st_ino;
	uint32_t        st_uid;
	uint32_t        st_gid;
	int32_t         st_rdev;
	int32_t         __pad0;
	struct timespec st_atimespec;
	struct timespec st_mtimespec;
	struct timespec st_ctimespec;
	struct timespec st_birthtimespec;
	int64_t         st_size;
	int64_t         st_blocks;
	int32_t         st_blksize;
	uint32_t        st_flags;
	uint32_t        st_gen;
	int32_t         st_lspare;
	int64_t         st_qspare[2];
};

_Static_assert(sizeof(struct darwin_stat) == 144,
	"darwin_stat must be 144 bytes");

static void linux_to_darwin_stat(const struct stat *ls, struct darwin_stat *ds)
{
	memset(ds, 0, sizeof(*ds));
	ds->st_dev        = (int32_t)ls->st_dev;
	ds->st_mode       = (uint16_t)ls->st_mode;
	ds->st_nlink      = (uint16_t)ls->st_nlink;
	ds->st_ino        = ls->st_ino;
	ds->st_uid        = ls->st_uid;
	ds->st_gid        = ls->st_gid;
	ds->st_rdev       = (int32_t)ls->st_rdev;
	ds->st_atimespec  = ls->st_atim;
	ds->st_mtimespec  = ls->st_mtim;
	ds->st_ctimespec  = ls->st_ctim;
	ds->st_birthtimespec = ls->st_ctim; /* no birth time on Linux */
	ds->st_size       = ls->st_size;
	ds->st_blocks     = ls->st_blocks;
	ds->st_blksize    = ls->st_blksize;
}

/*
 * Use syscall(SYS_newfstatat) directly to avoid recursion — our
 * wrappers redefine stat/fstat/lstat, so calling glibc's stat()
 * would call ourselves.
 */

int stat_darwin(const char *path, void *buf)
{
	struct stat ls;
	int ret = syscall(SYS_newfstatat, AT_FDCWD, path, &ls, 0);
	if (ret == 0)
		linux_to_darwin_stat(&ls, (struct darwin_stat *)buf);
	else
		ret = -1;  /* syscall returns -errno on error */
	return ret;
}

int fstat_darwin(int fd, void *buf)
{
	struct stat ls;
	int ret = syscall(SYS_fstat, fd, &ls);
	if (ret == 0)
		linux_to_darwin_stat(&ls, (struct darwin_stat *)buf);
	else
		ret = -1;
	return ret;
}

int lstat_darwin(const char *path, void *buf)
{
	struct stat ls;
	int ret = syscall(SYS_newfstatat, AT_FDCWD, path, &ls, AT_SYMLINK_NOFOLLOW);
	if (ret == 0)
		linux_to_darwin_stat(&ls, (struct darwin_stat *)buf);
	else
		ret = -1;
	return ret;
}

int fstatat_darwin(int dirfd, const char *path, void *buf, int flags)
{
	struct stat ls;
	int ret = syscall(SYS_newfstatat, dirfd, path, &ls, flags);
	if (ret == 0)
		linux_to_darwin_stat(&ls, (struct darwin_stat *)buf);
	else
		ret = -1;
	return ret;
}

/* Export under standard names so the resolver finds them when the game
 * imports stat/fstat/lstat from libSystem.B.dylib.
 * Use __asm__ labels to avoid conflicting with glibc's prototypes. */
int shim_stat(const char *path, void *buf) __asm__("stat");
int shim_stat(const char *path, void *buf) { return stat_darwin(path, buf); }

int shim_fstat(int fd, void *buf) __asm__("fstat");
int shim_fstat(int fd, void *buf) { return fstat_darwin(fd, buf); }

int shim_lstat(const char *path, void *buf) __asm__("lstat");
int shim_lstat(const char *path, void *buf) { return lstat_darwin(path, buf); }

int shim_fstatat(int dirfd, const char *path, void *buf, int flags) __asm__("fstatat");
int shim_fstatat(int dirfd, const char *path, void *buf, int flags) { return fstatat_darwin(dirfd, path, buf, flags); }

/* ===== readdir() ABI translation ===== */

/*
 * macOS struct dirent (64-bit):
 *   uint64_t  d_ino;       // offset 0
 *   uint64_t  d_seekoff;   // offset 8  (macOS-only)
 *   uint16_t  d_reclen;    // offset 16
 *   uint16_t  d_namlen;    // offset 18 (macOS-only)
 *   uint8_t   d_type;      // offset 20
 *   char      d_name[1024];// offset 21
 *   [padding to 1048]
 *
 * Linux struct dirent (aarch64):
 *   ino_t     d_ino;       // offset 0  (8 bytes)
 *   off_t     d_off;       // offset 8  (8 bytes)
 *   uint16_t  d_reclen;    // offset 16
 *   uint8_t   d_type;      // offset 18
 *   char      d_name[256]; // offset 19
 *
 * The game reads d_type at offset 20 and d_name at offset 21 (macOS).
 * We convert the Linux result into a static Darwin-layout buffer.
 */

struct darwin_dirent {
	uint64_t  d_ino;
	uint64_t  d_seekoff;
	uint16_t  d_reclen;
	uint16_t  d_namlen;
	uint8_t   d_type;
	char      d_name[1024];
	/* padding to 8-byte alignment */
	char      __pad[3];
};

_Static_assert(sizeof(struct darwin_dirent) == 1048,
	"darwin_dirent must be 1048 bytes");

/*
 * Thread-local buffer for the converted dirent, since readdir returns
 * a pointer to an internal buffer (not caller-allocated).
 */
static __thread struct darwin_dirent tls_darwin_dirent;

struct dirent *shim_readdir(DIR *dirp) __asm__("readdir");
struct dirent *shim_readdir(DIR *dirp)
{
	struct dirent *le = readdir(dirp);
	if (!le)
		return NULL;

	struct darwin_dirent *de = &tls_darwin_dirent;
	memset(de, 0, sizeof(*de));
	de->d_ino     = le->d_ino;
	de->d_seekoff = 0;  /* not available on Linux */
	de->d_reclen  = sizeof(struct darwin_dirent);
	de->d_type    = le->d_type;

	size_t namelen = strlen(le->d_name);
	if (namelen >= sizeof(de->d_name))
		namelen = sizeof(de->d_name) - 1;
	memcpy(de->d_name, le->d_name, namelen);
	de->d_name[namelen] = '\0';
	de->d_namlen = (uint16_t)namelen;

	return (struct dirent *)(void *)de;
}

struct dirent *shim_readdir_r(DIR *dirp, void *entry, void **result) __asm__("readdir_r");
struct dirent *shim_readdir_r(DIR *dirp, void *entry, void **result)
{
	/* readdir_r is deprecated but some code uses it.
	 * Convert into the caller's darwin_dirent buffer. */
	struct dirent linux_entry;
	struct dirent *linux_result = NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	int ret = readdir_r(dirp, &linux_entry, &linux_result);
#pragma GCC diagnostic pop
	if (ret != 0 || !linux_result) {
		if (result) *(void**)result = NULL;
		return (struct dirent *)(intptr_t)ret;
	}

	struct darwin_dirent *de = (struct darwin_dirent *)entry;
	memset(de, 0, sizeof(*de));
	de->d_ino     = linux_result->d_ino;
	de->d_seekoff = 0;
	de->d_reclen  = sizeof(struct darwin_dirent);
	de->d_type    = linux_result->d_type;

	size_t namelen = strlen(linux_result->d_name);
	if (namelen >= 1024) namelen = 1023;
	memcpy(de->d_name, linux_result->d_name, namelen);
	de->d_name[namelen] = '\0';
	de->d_namlen = (uint16_t)namelen;

	if (result) *(void**)result = de;
	return (struct dirent *)(intptr_t)ret;
}

/* ===== mmap registry ===== */

/* Track mmap'd regions so shim_free can munmap instead of free.
 * Used by game function replacements (e.g., DataStream::preloadFile → mmap). */

#include <sys/mman.h>

#define MMAP_REGISTRY_MAX 16

static struct {
	void *addr;
	size_t size;
} mmap_registry[MMAP_REGISTRY_MAX];
static int mmap_registry_count = 0;

void mmap_registry_add(void *addr, size_t size)
{
	if (mmap_registry_count >= MMAP_REGISTRY_MAX) {
		fprintf(stderr, "mmap_registry: full, cannot register %p\n", addr);
		return;
	}
	mmap_registry[mmap_registry_count].addr = addr;
	mmap_registry[mmap_registry_count].size = size;
	mmap_registry_count++;
	fprintf(stderr, "mmap_registry: registered %p (%zu bytes)\n", addr, size);
}

/* Returns size if found and removed, 0 if not found */
static size_t mmap_registry_remove(void *addr)
{
	for (int i = 0; i < mmap_registry_count; i++) {
		if (mmap_registry[i].addr == addr) {
			size_t size = mmap_registry[i].size;
			mmap_registry[i] = mmap_registry[--mmap_registry_count];
			return size;
		}
	}
	return 0;
}

/* ===== Heap allocation wrappers ===== */

/* Intercept malloc/free/calloc/realloc for:
 * 1. Zero-initialization (macOS malloc compat)
 * 2. Heaptrack-compatible allocation tracing (MACHISMO_HEAPTRACK env var)
 *
 * Uses dlsym(RTLD_NEXT) to find the real glibc functions.
 * The __asm__ labels export these under standard names so the resolver
 * finds them when the Mach-O imports from libSystem.B.dylib. */

typedef void *(*real_malloc_fn)(size_t);
typedef void  (*real_free_fn)(void *);
typedef void *(*real_calloc_fn)(size_t, size_t);
typedef void *(*real_realloc_fn)(void *, size_t);

static real_malloc_fn  real_malloc  = NULL;
static real_free_fn    real_free    = NULL;
static real_calloc_fn  real_calloc  = NULL;
static real_realloc_fn real_realloc = NULL;

/* Bootstrap: dlsym itself may call malloc, so we need a tiny fallback
 * allocator for the very first calls before dlsym resolves. */
static char bootstrap_buf[4096];
static int bootstrap_pos = 0;

static void resolve_real_funcs(void)
{
	if (real_malloc) return;
	real_malloc  = (real_malloc_fn)dlsym(RTLD_NEXT, "malloc");
	real_free    = (real_free_fn)dlsym(RTLD_NEXT, "free");
	real_calloc  = (real_calloc_fn)dlsym(RTLD_NEXT, "calloc");
	real_realloc = (real_realloc_fn)dlsym(RTLD_NEXT, "realloc");
}

void *shim_malloc(size_t size) __asm__("malloc");
void *shim_malloc(size_t size)
{
	if (!real_malloc) {
		resolve_real_funcs();
		if (!real_malloc) {
			/* Bootstrap: return zeroed memory from static buffer */
			int aligned = (bootstrap_pos + 15) & ~15;
			if (aligned + (int)size <= (int)sizeof(bootstrap_buf)) {
				void *p = bootstrap_buf + aligned;
				memset(p, 0, size);
				bootstrap_pos = aligned + size;
				return p;
			}
			return NULL;
		}
	}
	/* Zero-initialize for macOS compat (macOS malloc returns zeroed pages) */
	void *p = real_malloc(size);
	if (p) {
		memset(p, 0, size);
		if (heap_trace_active)
			heap_trace_alloc(p, size);
	}
	return p;
}

void shim_free(void *ptr) __asm__("free");
void shim_free(void *ptr)
{
	if (!ptr) return;
	/* Don't free bootstrap allocations */
	if ((char *)ptr >= bootstrap_buf &&
	    (char *)ptr < bootstrap_buf + sizeof(bootstrap_buf))
		return;
	/* Check mmap registry: munmap instead of free for mmap'd regions */
	size_t mmap_size = mmap_registry_remove(ptr);
	if (mmap_size) {
		if (heap_trace_active)
			heap_trace_free(ptr);
		munmap(ptr, mmap_size);
		return;
	}
	if (heap_trace_active)
		heap_trace_free(ptr);
	if (!real_free) resolve_real_funcs();
	if (real_free) real_free(ptr);
}

void *shim_calloc(size_t nmemb, size_t size) __asm__("calloc");
void *shim_calloc(size_t nmemb, size_t size)
{
	if (!real_calloc) {
		resolve_real_funcs();
		if (!real_calloc) {
			/* Bootstrap fallback */
			size_t total = nmemb * size;
			return shim_malloc(total);
		}
	}
	void *p = real_calloc(nmemb, size);
	if (p && heap_trace_active)
		heap_trace_alloc(p, nmemb * size);
	return p;
}

void *shim_realloc(void *ptr, size_t size) __asm__("realloc");
void *shim_realloc(void *ptr, size_t size)
{
	if (!real_realloc) resolve_real_funcs();
	if (!real_realloc) return NULL;
	void *new_ptr = real_realloc(ptr, size);
	if (heap_trace_active)
		heap_trace_realloc(ptr, new_ptr, size);
	return new_ptr;
}
