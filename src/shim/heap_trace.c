/*
 * Heaptrack-compatible allocation tracer for machismo.
 *
 * Activated via MACHISMO_HEAPTRACK=<filename> environment variable.
 * Writes heaptrack v1 text trace file viewable in heaptrack_gui / heaptrack_print.
 *
 * Based on gc_trace.c from the HashLink VM (github.com/HaxeFoundation/hashlink).
 * Frame pointer walking + dladdr for native symbol resolution.
 */
#define _GNU_SOURCE
#include "heap_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>

int heap_trace_active = 0;

static void heap_trace_signal_handler(int sig);

/* C++ symbol demangling via __cxa_demangle (resolved from libstdc++/libc++ at runtime) */
typedef char *(*cxa_demangle_fn)(const char *, char *, size_t *, int *);
static cxa_demangle_fn demangle_fn = NULL;

/* Guard against recursive tracing (our own mallocs during trace) */
static __thread int trace_recursion = 0;

/* Optional Mach-O symbol resolver (set by machismo after loading symbols) */
static heap_trace_sym_resolver_fn sym_resolver = NULL;

/* ---- Trace file state ---- */
static FILE *trace_file = NULL;
static struct timespec trace_start_time;
static int trace_alloc_count = 0;
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Output buffer ---- */
#define TRACE_BUF_SIZE (1 << 16)
static char *trace_buf = NULL;
static int trace_buf_pos = 0;

static void trace_flush(void)
{
	if (trace_buf_pos > 0 && trace_file) {
		fwrite(trace_buf, 1, trace_buf_pos, trace_file);
		trace_buf_pos = 0;
	}
}

static void trace_write(const char *data, int len)
{
	if (trace_buf_pos + len > TRACE_BUF_SIZE)
		trace_flush();
	if (len > TRACE_BUF_SIZE) {
		fwrite(data, 1, len, trace_file);
		return;
	}
	memcpy(trace_buf + trace_buf_pos, data, len);
	trace_buf_pos += len;
}

static void trace_printf(const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0) {
		if (n >= (int)sizeof(tmp)) {
			/* Truncated — ensure newline termination so records
			 * don't run together in the trace file */
			n = sizeof(tmp) - 1;
			tmp[n - 1] = '\n';
		}
		trace_write(tmp, n);
	}
}

/* ---- Hash functions ---- */

static unsigned int hash_str(const char *s)
{
	unsigned int h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static unsigned int hash_ptr(void *p)
{
	uintptr_t v = (uintptr_t)p;
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned int)v;
}

static unsigned int hash_trace(uintptr_t ip_hex, int parent)
{
	uintptr_t v = ip_hex ^ ((uintptr_t)parent * 2654435761u);
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned int)v;
}

static unsigned int hash_alloc_info(uint64_t size, int trace_idx)
{
	uintptr_t v = (uintptr_t)size ^ ((uintptr_t)trace_idx * 2654435761u);
	v ^= v >> 16;
	v *= 0x9e3779b9;
	v ^= v >> 16;
	return (unsigned int)v;
}

/* Try to demangle a C++ symbol. Returns a malloc'd string on success, NULL on failure. */
static char *try_demangle(const char *name)
{
	if (!demangle_fn || !name)
		return NULL;
	/* Skip leading underscore (Mach-O convention) for demangling */
	const char *to_demangle = name;
	if (name[0] == '_' && name[1] == '_' && name[2] == 'Z')
		to_demangle = name + 1;
	else if (name[0] == '_' && name[1] == 'Z')
		to_demangle = name;  /* already without extra underscore */
	else
		return NULL;  /* not a mangled C++ name */
	int status = -1;
	char *demangled = demangle_fn(to_demangle, NULL, NULL, &status);
	return (status == 0) ? demangled : NULL;
}

/* ==== String intern table: char* content -> 1-based index ==== */

typedef struct {
	char *key;
	int index;
} str_entry;

static str_entry *str_table = NULL;
static int str_table_cap = 0;
static int str_table_count = 0;
static int str_next_index = 1;

static void str_table_grow(void)
{
	int new_cap = str_table_cap ? str_table_cap * 2 : 1024;
	str_entry *new_table = (str_entry *)calloc(new_cap, sizeof(str_entry));
	for (int i = 0; i < str_table_cap; i++) {
		if (str_table[i].key) {
			unsigned int h = hash_str(str_table[i].key) & (new_cap - 1);
			while (new_table[h].key)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = str_table[i];
		}
	}
	free(str_table);
	str_table = new_table;
	str_table_cap = new_cap;
}

static int intern_string(const char *s)
{
	if (!s || !*s) s = "??";
	if (str_table_count * 10 >= str_table_cap * 7)
		str_table_grow();
	unsigned int h = hash_str(s) & (str_table_cap - 1);
	while (str_table[h].key) {
		if (strcmp(str_table[h].key, s) == 0)
			return str_table[h].index;
		h = (h + 1) & (str_table_cap - 1);
	}
	int idx = str_next_index++;
	char *copy = strdup(s);
	str_table[h].key = copy;
	str_table[h].index = idx;
	str_table_count++;
	trace_printf("s %s\n", copy);
	return idx;
}

/* ==== IP intern table: void* -> 1-based index ==== */

typedef struct {
	void *key;
	int index;
} ip_entry;

static ip_entry *ip_table = NULL;
static int ip_table_cap = 0;
static int ip_table_count = 0;
static int ip_next_index = 1;

static void ip_table_grow(void)
{
	int new_cap = ip_table_cap ? ip_table_cap * 2 : 4096;
	ip_entry *new_table = (ip_entry *)calloc(new_cap, sizeof(ip_entry));
	for (int i = 0; i < ip_table_cap; i++) {
		if (ip_table[i].key) {
			unsigned int h = hash_ptr(ip_table[i].key) & (new_cap - 1);
			while (new_table[h].key)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = ip_table[i];
		}
	}
	free(ip_table);
	ip_table = new_table;
	ip_table_cap = new_cap;
}

/*
 * intern_ip_preresolved: intern an IP using pre-resolved dladdr results.
 *
 * dladdr() acquires the dynamic linker's internal dl_load_lock. If we called
 * dladdr while holding trace_lock, and another thread held dl_load_lock and
 * called malloc (→ heap_trace_alloc → trace_lock), we'd deadlock.
 *
 * Fix: callers pre-resolve via dladdr() BEFORE acquiring trace_lock, then
 * pass the results here. Only intern_string/try_demangle run under the lock
 * — these call malloc (safe: trace_recursion guard) but not dladdr.
 */
static int intern_ip_preresolved(void *addr, Dl_info *dl, int dl_valid,
                                  const char *macho_sym)
{
	if (ip_table_count * 10 >= ip_table_cap * 7)
		ip_table_grow();
	unsigned int h = hash_ptr(addr) & (ip_table_cap - 1);
	while (ip_table[h].key) {
		if (ip_table[h].key == addr)
			return ip_table[h].index;
		h = (h + 1) & (ip_table_cap - 1);
	}

	int idx = ip_next_index++;
	ip_table[h].key = addr;
	ip_table[h].index = idx;
	ip_table_count++;

	int fn_idx = 0, file_idx = 0, line = 0;
	int mod_idx = 0;

	if (dl_valid < 0) {
		/* Thread-local cache said "seen" but not in ip_table (cache collision).
		 * We skipped dladdr — write raw address with unknown symbols. Rare. */
		mod_idx = intern_string("??");
		fn_idx = intern_string("??");
		file_idx = intern_string("??");
	} else if (dl_valid) {
		mod_idx = intern_string(dl->dli_fname ? dl->dli_fname : "??");
		char *demangled = try_demangle(dl->dli_sname);
		fn_idx = intern_string(demangled ? demangled : (dl->dli_sname ? dl->dli_sname : "??"));
		free(demangled);
		file_idx = intern_string("??");
	} else {
		mod_idx = intern_string("macho");
		char *demangled = try_demangle(macho_sym);
		fn_idx = intern_string(demangled ? demangled : (macho_sym ? macho_sym : "??"));
		free(demangled);
		file_idx = intern_string("??");
	}

	trace_printf("i %lx %x %x %x %x\n",
		(unsigned long)(uintptr_t)addr, mod_idx, fn_idx, file_idx, line);
	return idx;
}

/* ==== Trace tree intern table: (ip_hex, parent) -> 1-based index ==== */

typedef struct {
	uintptr_t ip_hex;
	int parent;
	int index;
} trace_entry;

static trace_entry *trace_table = NULL;
static int trace_table_cap = 0;
static int trace_table_count = 0;
static int trace_next_index = 1;

static void trace_table_grow(void)
{
	int new_cap = trace_table_cap ? trace_table_cap * 2 : 4096;
	trace_entry *new_table = (trace_entry *)calloc(new_cap, sizeof(trace_entry));
	for (int i = 0; i < trace_table_cap; i++) {
		if (trace_table[i].index) {
			unsigned int h = hash_trace(trace_table[i].ip_hex, trace_table[i].parent) & (new_cap - 1);
			while (new_table[h].index)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = trace_table[i];
		}
	}
	free(trace_table);
	trace_table = new_table;
	trace_table_cap = new_cap;
}

static int intern_trace(int ip_idx, uintptr_t ip_hex, int parent_trace)
{
	if (trace_table_count * 10 >= trace_table_cap * 7)
		trace_table_grow();
	unsigned int h = hash_trace(ip_hex, parent_trace) & (trace_table_cap - 1);
	while (trace_table[h].index) {
		if (trace_table[h].ip_hex == ip_hex && trace_table[h].parent == parent_trace)
			return trace_table[h].index;
		h = (h + 1) & (trace_table_cap - 1);
	}

	int idx = trace_next_index++;
	trace_table[h].ip_hex = ip_hex;
	trace_table[h].parent = parent_trace;
	trace_table[h].index = idx;
	trace_table_count++;

	trace_printf("t %x %x\n", ip_idx, parent_trace);
	return idx;
}

/* ==== Alloc info intern table: (size, trace_idx) -> 0-based index ==== */

typedef struct {
	uint64_t size;
	int trace_idx;
	int index;    /* 0-based, -1 = empty */
} ainfo_entry;

static ainfo_entry *ainfo_table = NULL;
static int ainfo_table_cap = 0;
static int ainfo_table_count = 0;
static int ainfo_next_index = 0;

static void ainfo_table_grow(void)
{
	int new_cap = ainfo_table_cap ? ainfo_table_cap * 2 : 4096;
	ainfo_entry *new_table = (ainfo_entry *)malloc(new_cap * sizeof(ainfo_entry));
	for (int i = 0; i < new_cap; i++)
		new_table[i].index = -1;
	for (int i = 0; i < ainfo_table_cap; i++) {
		if (ainfo_table[i].index >= 0) {
			unsigned int h = hash_alloc_info(ainfo_table[i].size, ainfo_table[i].trace_idx) & (new_cap - 1);
			while (new_table[h].index >= 0)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = ainfo_table[i];
		}
	}
	free(ainfo_table);
	ainfo_table = new_table;
	ainfo_table_cap = new_cap;
}

static int intern_alloc_info(uint64_t size, int trace_idx)
{
	if (ainfo_table_count * 10 >= ainfo_table_cap * 7)
		ainfo_table_grow();
	unsigned int h = hash_alloc_info(size, trace_idx) & (ainfo_table_cap - 1);
	while (ainfo_table[h].index >= 0) {
		if (ainfo_table[h].size == size && ainfo_table[h].trace_idx == trace_idx)
			return ainfo_table[h].index;
		h = (h + 1) & (ainfo_table_cap - 1);
	}
	int idx = ainfo_next_index++;
	ainfo_table[h].size = size;
	ainfo_table[h].trace_idx = trace_idx;
	ainfo_table[h].index = idx;
	ainfo_table_count++;
	trace_printf("a %lx %x\n", (unsigned long)size, trace_idx);
	return idx;
}

/* ==== Pointer map: void* -> alloc_info_index (for frees) ==== */

typedef struct {
	void *key;
	int alloc_info_idx;
} ptr_entry;

static ptr_entry *ptr_table = NULL;
static int ptr_table_cap = 0;
static int ptr_table_count = 0;

static void ptr_table_grow(void)
{
	int new_cap = ptr_table_cap ? ptr_table_cap * 2 : (1 << 16);
	ptr_entry *new_table = (ptr_entry *)calloc(new_cap, sizeof(ptr_entry));
	for (int i = 0; i < ptr_table_cap; i++) {
		if (ptr_table[i].key) {
			unsigned int h = hash_ptr(ptr_table[i].key) & (new_cap - 1);
			while (new_table[h].key)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = ptr_table[i];
		}
	}
	free(ptr_table);
	ptr_table = new_table;
	ptr_table_cap = new_cap;
}

static void ptr_map_insert(void *ptr, int alloc_info_idx)
{
	if (ptr_table_count * 10 >= ptr_table_cap * 7)
		ptr_table_grow();
	unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
	while (ptr_table[h].key) {
		if (ptr_table[h].key == ptr) {
			ptr_table[h].alloc_info_idx = alloc_info_idx;
			return;
		}
		h = (h + 1) & (ptr_table_cap - 1);
	}
	ptr_table[h].key = ptr;
	ptr_table[h].alloc_info_idx = alloc_info_idx;
	ptr_table_count++;
}

static int ptr_map_remove(void *ptr)
{
	if (!ptr_table_cap) return -1;
	unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
	while (ptr_table[h].key) {
		if (ptr_table[h].key == ptr) {
			int idx = ptr_table[h].alloc_info_idx;
			ptr_table[h].key = NULL;
			ptr_table_count--;
			unsigned int j = (h + 1) & (ptr_table_cap - 1);
			while (ptr_table[j].key) {
				void *k = ptr_table[j].key;
				int v = ptr_table[j].alloc_info_idx;
				ptr_table[j].key = NULL;
				ptr_table_count--;
				ptr_map_insert(k, v);
				j = (j + 1) & (ptr_table_cap - 1);
			}
			return idx;
		}
		h = (h + 1) & (ptr_table_cap - 1);
	}
	return -1;
}

/* ---- Frame pointer walking ---- */

/* Strip Pointer Authentication Code (PAC) from return addresses.
 * Fedora Asahi enables PAC on system libraries, so saved LR values
 * on the stack have PAC signatures in the upper bits that must be
 * stripped before dladdr/symbol lookup will work. */
static inline void *strip_pac(void *ptr)
{
	/* XPACLRI (HINT #7) strips PAC from x30 without authenticating.
	 * We save/restore x30 around the hint using x16 (intra-procedure-call
	 * scratch, declared clobbered so the compiler won't keep live values
	 * there). NOP on pre-v8.3 cores, strips PAC on v8.3+. */
	uintptr_t result;
	__asm__ volatile(
		"mov x16, x30\n"
		"mov x30, %1\n"
		"hint #7\n"
		"mov %0, x30\n"
		"mov x30, x16"
		: "=r"(result)
		: "r"((uintptr_t)ptr)
		: "x16"
	);
	return (void *)result;
}

#define HEAP_TRACE_MAX_DEPTH 64

static int heap_trace_capture(void **buf, int max)
{
	int depth = 0;
	void **fp = (void **)__builtin_frame_address(0);

	while (fp && depth < max) {
		void *lr = fp[1];
		if (!lr) break;
		buf[depth++] = strip_pac(lr);
		void **next = (void **)fp[0];
		if (next <= fp) break;                  /* must advance */
		if ((uintptr_t)next & 0xF) break;       /* must be 16-byte aligned */
		fp = next;
	}
	return depth;
}

/* ---- Timestamp ---- */

static long trace_elapsed_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - trace_start_time.tv_sec) * 1000L +
		(now.tv_nsec - trace_start_time.tv_nsec) / 1000000L;
}

/* ---- Signal handler for flushing on termination ---- */

static void heap_trace_signal_handler(int sig)
{
	heap_trace_close();
	/* Re-raise with default handler */
	signal(sig, SIG_DFL);
	raise(sig);
}

/* ---- Public API ---- */

void heap_trace_init(void)
{
	const char *filename = getenv("MACHISMO_HEAPTRACK");
	if (!filename || !*filename)
		return;

	trace_file = fopen(filename, "w");
	if (!trace_file) {
		fprintf(stderr, "heap_trace: failed to open %s for writing\n", filename);
		return;
	}

	trace_buf = (char *)malloc(TRACE_BUF_SIZE);
	trace_buf_pos = 0;

	/* Resolve __cxa_demangle for C++ symbol demangling */
	if (!demangle_fn)
		demangle_fn = (cxa_demangle_fn)dlsym(RTLD_DEFAULT, "__cxa_demangle");

	clock_gettime(CLOCK_MONOTONIC, &trace_start_time);

	/* Header: heaptrack version 1.4.0, file format version 1 */
	trace_printf("v 10400 1\n");

	/* X record: command line (parsed in processed file format).
	 * Note: 'x' (lowercase) is raw-only and causes a parse error. */
	char exe[1024];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len > 0) {
		exe[len] = 0;
		trace_printf("X %s\n", exe);
	} else {
		trace_printf("X machismo\n");
	}

	trace_printf("c 0\n");
	trace_flush();  /* flush header immediately so file isn't empty on crash */

	/* Install signal handlers to flush trace on unexpected termination */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = heap_trace_signal_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	heap_trace_active = 1;
	fprintf(stderr, "heap_trace: tracing allocations to %s\n", filename);
}

void heap_trace_close(void)
{
	if (!trace_file) return;
	heap_trace_active = 0;

	pthread_mutex_lock(&trace_lock);
	trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
	trace_flush();
	fclose(trace_file);
	trace_file = NULL;
	pthread_mutex_unlock(&trace_lock);

	if (str_table) {
		for (int i = 0; i < str_table_cap; i++)
			free(str_table[i].key);
		free(str_table);
		str_table = NULL;
	}
	str_table_cap = str_table_count = 0;
	str_next_index = 1;

	free(ip_table);
	ip_table = NULL;
	ip_table_cap = ip_table_count = 0;
	ip_next_index = 1;

	free(trace_table);
	trace_table = NULL;
	trace_table_cap = trace_table_count = 0;
	trace_next_index = 1;

	free(ainfo_table);
	ainfo_table = NULL;
	ainfo_table_cap = ainfo_table_count = 0;
	ainfo_next_index = 0;

	free(ptr_table);
	ptr_table = NULL;
	ptr_table_cap = ptr_table_count = 0;

	free(trace_buf);
	trace_buf = NULL;
	trace_buf_pos = 0;

	fprintf(stderr, "heap_trace: closed (%d allocations recorded)\n", trace_alloc_count);
}

/* Direct-mapped IP cache.
 * Avoids calling dladdr() for IPs we've already resolved — after warmup,
 * 95%+ of frames are repeats from the same call sites. Without this cache,
 * calling dladdr 64 times per allocation makes the game impossibly slow.
 *
 * Not __thread: TLS in LD_PRELOAD'd libraries has a small size limit (~4KB)
 * and 8KB would cause stack smashing. A static cache has benign races —
 * worst case is a redundant dladdr call, no correctness issue. */
#define IP_CACHE_BITS 10
#define IP_CACHE_SIZE (1 << IP_CACHE_BITS)
static void *ip_cache[IP_CACHE_SIZE];

static inline int ip_seen_recently(void *addr)
{
	unsigned int slot = hash_ptr(addr) & (IP_CACHE_SIZE - 1);
	if (ip_cache[slot] == addr)
		return 1;
	ip_cache[slot] = addr;
	return 0;
}

void heap_trace_alloc(void *ptr, size_t size)
{
	if (!trace_file || !ptr) return;
	if (trace_recursion) return;
	trace_recursion = 1;

	void *frames[HEAP_TRACE_MAX_DEPTH];
	int nframes = heap_trace_capture(frames, HEAP_TRACE_MAX_DEPTH);

	/* Pre-resolve dladdr for NEW frames only, OUTSIDE the lock.
	 * dladdr() acquires dl_load_lock internally — if we held trace_lock
	 * while calling dladdr, and another thread held dl_load_lock and
	 * called malloc (→ heap_trace_alloc → trace_lock), we'd deadlock.
	 *
	 * The thread-local cache avoids redundant dladdr calls for IPs
	 * we've already resolved. dl_found[i] == -1 means "already cached,
	 * skip dladdr". If a cache collision causes a miss, intern_ip_preresolved
	 * falls back to unknown ("??") symbols — rare and harmless. */
	Dl_info dl_info[HEAP_TRACE_MAX_DEPTH];
	int dl_found[HEAP_TRACE_MAX_DEPTH];
	const char *macho_syms[HEAP_TRACE_MAX_DEPTH];
	for (int i = 0; i < nframes; i++) {
		if (ip_seen_recently(frames[i])) {
			dl_found[i] = -1;  /* already in ip_table (probably) */
			macho_syms[i] = NULL;
		} else {
			dl_found[i] = dladdr(frames[i], &dl_info[i]);
			macho_syms[i] = (!dl_found[i] && sym_resolver) ?
				sym_resolver((uintptr_t)frames[i]) : NULL;
		}
	}

	pthread_mutex_lock(&trace_lock);

	/* Build trace tree: outermost to innermost */
	int parent_trace = 0;
	for (int i = nframes - 1; i >= 0; i--) {
		int ip_idx = intern_ip_preresolved(frames[i],
			&dl_info[i], dl_found[i], macho_syms[i]);
		parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
	}

	int ainfo_idx = intern_alloc_info((uint64_t)size, parent_trace);
	ptr_map_insert(ptr, ainfo_idx);

	if ((++trace_alloc_count & 0x3FF) == 0)
		trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());

	trace_printf("+ %x\n", ainfo_idx);

	pthread_mutex_unlock(&trace_lock);
	trace_recursion = 0;
}

void heap_trace_free(void *ptr)
{
	if (!trace_file || !ptr) return;
	if (trace_recursion) return;
	trace_recursion = 1;

	pthread_mutex_lock(&trace_lock);

	int ainfo_idx = ptr_map_remove(ptr);
	if (ainfo_idx >= 0)
		trace_printf("- %x\n", ainfo_idx);

	pthread_mutex_unlock(&trace_lock);
	trace_recursion = 0;
}

void heap_trace_realloc(void *old_ptr, void *new_ptr, size_t new_size)
{
	if (!trace_file) return;
	if (trace_recursion) return;

	/* Model realloc as free(old) + alloc(new) */
	if (old_ptr && old_ptr != new_ptr) {
		trace_recursion = 1;
		pthread_mutex_lock(&trace_lock);
		int ainfo_idx = ptr_map_remove(old_ptr);
		if (ainfo_idx >= 0)
			trace_printf("- %x\n", ainfo_idx);
		pthread_mutex_unlock(&trace_lock);
		trace_recursion = 0;
	}

	if (new_ptr)
		heap_trace_alloc(new_ptr, new_size);
}

void heap_trace_set_symbol_resolver(heap_trace_sym_resolver_fn fn)
{
	sym_resolver = fn;
}
