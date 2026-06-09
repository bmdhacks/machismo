/*
 * Built-in heaptrack-compatible allocation tracer for machismo.
 *
 * Ported near-verbatim from /home/bmd/hhg/hashlink/src/gc_trace.c (the working
 * reference). The intern tables (string / IP / trace-tree / alloc-info), the
 * ptr->ainfo map, the buffered writer, the frame-pointer walk and the per-event
 * emit are HashLink's; the machismo-specific changes are:
 *   - allocations are fed by the libsystem_shim malloc hooks (not a GC),
 *   - IP symbolication uses a reverse Mach-O LC_SYMTAB index (built from the
 *     loaded guest images) instead of HashLink's hl_setup.resolve_symbol,
 *   - a global mutex + per-thread reentrancy guard make the hooks safe to call
 *     concurrently from many guest threads.
 *
 * Writes the heaptrack v1 interpreted text format ("v 10400 1"): per allocation
 * it emits interned strings (s), instruction pointers (i), trace-tree nodes (t),
 * alloc-info records (a) and the event itself (+ / -). The local heaptrack
 * (1.6.80, file format v3) still reads fileVersion>=1. Plain text, uncompressed.
 *
 * Activated via MACHISMO_HEAPTRACK=<path>.
 */
#include "heaptrack_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>

#include "macho_defs.h"

/* Exported by machismo.c: top of the custom Mach-O stack used by the guest's
 * main thread (pthread_getattr_np cannot describe that stack). */
extern void *__machismo_main_stack_top;

/* Itanium C++ ABI demangler (libstdc++). Declared here to keep this a C TU. */
extern char *__cxa_demangle(const char *mangled, char *out, size_t *len, int *status);

int machismo_heaptrack_active = 0;

/* ---- Trace file state ---- */
static FILE *trace_file = NULL;
static struct timespec trace_start_time;
static int trace_alloc_count = 0;

/* Concurrency: one lock around the intern tables + writer, plus a per-thread
 * guard so a hook never traces allocations made inside the hook itself. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int g_in_hook = 0;

/* ---- Output buffer ---- */
#define TRACE_BUF_SIZE (1 << 16)
static char *trace_buf = NULL;
static int trace_buf_pos = 0;

static void trace_flush(void) {
	if (trace_buf_pos > 0 && trace_file) {
		fwrite(trace_buf, 1, trace_buf_pos, trace_file);
		trace_buf_pos = 0;
	}
}

static void trace_write(const char *data, int len) {
	if (trace_buf_pos + len > TRACE_BUF_SIZE)
		trace_flush();
	if (len > TRACE_BUF_SIZE) {
		fwrite(data, 1, len, trace_file);
		return;
	}
	memcpy(trace_buf + trace_buf_pos, data, len);
	trace_buf_pos += len;
}

static void trace_printf(const char *fmt, ...) {
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0)
		trace_write(tmp, n);
}

/* ---- Hash functions ---- */

static unsigned int hash_str(const char *s) {
	unsigned int h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static unsigned int hash_ptr(void *p) {
	uintptr_t v = (uintptr_t)p;
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned int)v;
}

static unsigned int hash_trace(uintptr_t ip_hex, int parent) {
	uintptr_t v = ip_hex ^ ((uintptr_t)parent * 2654435761u);
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned int)v;
}

static unsigned int hash_alloc_info(uint64_t size, int trace_idx) {
	uintptr_t v = (uintptr_t)size ^ ((uintptr_t)trace_idx * 2654435761u);
	v ^= v >> 16;
	v *= 0x9e3779b9;
	v ^= v >> 16;
	return (unsigned int)v;
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

static void str_table_grow(void) {
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

static int intern_string(const char *s) {
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

/* ==== Reverse Mach-O symbol index: address -> (function, module) ==== */
/* Built from each loaded image's LC_SYMTAB (same walk as resolver.c's
 * build_extent_cache, but spanning all images and keeping the names). */

struct ht_sym {
	uint64_t addr;        /* n_value + slide (runtime address) */
	const char *name;     /* into the mapped Mach-O strtab (stable for the run) */
	const char *module;   /* strdup'd module name (stable for the run) */
};

static struct ht_sym *g_syms = NULL;
static size_t g_nsyms = 0;
static size_t g_syms_cap = 0;
static int g_syms_sorted = 0;

/* Reject a nearest-symbol match farther than this from the queried IP (we have
 * no per-symbol size here, so this caps mis-attribution of host/unknown IPs). */
#define HT_MAX_SYM_DISTANCE (1u << 20)   /* 1 MB */

static int ht_sym_cmp(const void *a, const void *b) {
	uint64_t va = ((const struct ht_sym *)a)->addr;
	uint64_t vb = ((const struct ht_sym *)b)->addr;
	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

void machismo_heaptrack_add_image(void *mh_ptr, unsigned long slide,
                                  const char *module_name) {
	struct mach_header_64 *mh = (struct mach_header_64 *)mh_ptr;
	if (!mh) return;

	uint8_t *cmds = (uint8_t *)(mh + 1);
	struct symtab_command *symtab = NULL;

	/* Find LC_SYMTAB. */
	uint32_t p = 0;
	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)&cmds[p];
		if (lc->cmd == LC_SYMTAB)
			symtab = (struct symtab_command *)lc;
		p += lc->cmdsize;
	}
	if (!symtab) return;

	/* Locate the nlist array + string table via the __LINKEDIT segment math. */
	struct nlist_64 *syms_mem = NULL;
	char *strtab = NULL;
	p = 0;
	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)&cmds[p];
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *seg = (struct segment_command_64 *)lc;
			if (symtab->symoff >= seg->fileoff &&
			    symtab->symoff < seg->fileoff + seg->filesize) {
				uintptr_t base = seg->vmaddr + slide;
				syms_mem = (struct nlist_64 *)(base + (symtab->symoff - seg->fileoff));
				strtab = (char *)(base + (symtab->stroff - seg->fileoff));
			}
		}
		p += lc->cmdsize;
	}
	if (!syms_mem || !strtab) return;

	const char *mod = strdup(module_name ? module_name : "??");

	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		struct nlist_64 *nl = &syms_mem[i];
		if (nl->n_type & N_STAB) continue;
		if ((nl->n_type & N_TYPE) != N_SECT) continue;
		if (nl->n_strx >= symtab->strsize) continue;
		if (nl->n_value == 0) continue;

		if (g_nsyms == g_syms_cap) {
			size_t nc = g_syms_cap ? g_syms_cap * 2 : 8192;
			g_syms = (struct ht_sym *)realloc(g_syms, nc * sizeof(*g_syms));
			g_syms_cap = nc;
		}
		g_syms[g_nsyms].addr = nl->n_value + slide;
		g_syms[g_nsyms].name = strtab + nl->n_strx;
		g_syms[g_nsyms].module = mod;
		g_nsyms++;
	}
	g_syms_sorted = 0;
}

/* Nearest preceding symbol. Returns 1 + fills *out_fn / *out_mod, else 0. */
static int ht_lookup_sym(uint64_t ip, const char **out_fn, const char **out_mod) {
	if (!g_nsyms) return 0;
	if (!g_syms_sorted) {
		qsort(g_syms, g_nsyms, sizeof(*g_syms), ht_sym_cmp);
		g_syms_sorted = 1;
	}
	/* Greatest addr <= ip. */
	size_t lo = 0, hi = g_nsyms;   /* find first addr > ip, then step back */
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (g_syms[mid].addr <= ip) lo = mid + 1;
		else hi = mid;
	}
	if (lo == 0) return 0;
	const struct ht_sym *s = &g_syms[lo - 1];
	if (ip - s->addr > HT_MAX_SYM_DISTANCE) return 0;
	*out_fn = s->name;
	*out_mod = s->module;
	return 1;
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

static void ip_table_grow(void) {
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

/* Intern a function name, demangling Itanium C++ names ("_Z...") first so the
 * trace shows readable signatures. Runs once per unique IP (intern_ip dedupes),
 * so the per-call __cxa_demangle cost is amortized away. */
static int intern_symbol(const char *name) {
	if (name && name[0] == '_' && name[1] == 'Z') {
		int status = 0;
		char *dem = __cxa_demangle(name, NULL, NULL, &status);
		if (dem) {
			int idx = intern_string(dem);
			free(dem);
			return idx;
		}
	}
	return intern_string(name);
}

static int intern_ip(void *addr) {
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

	int mod_idx, fn_idx, file_idx;

	/* Guest frames: reverse Mach-O symbol index. */
	const char *fn = NULL, *mod = NULL;
	if (ht_lookup_sym((uint64_t)(uintptr_t)addr, &fn, &mod)) {
		mod_idx = intern_string(mod);
		fn_idx = intern_symbol(fn[0] == '_' ? fn + 1 : fn);  /* drop Mach-O '_' */
		file_idx = intern_string("??");
	} else {
		/* Host frames (machismo exe + native .so): dladdr. */
		Dl_info info;
		if (dladdr(addr, &info) && info.dli_sname) {
			mod_idx = intern_string(info.dli_fname ? info.dli_fname : "??");
			fn_idx = intern_symbol(info.dli_sname);
			file_idx = intern_string("??");
		} else {
			mod_idx = intern_string("??");
			fn_idx = intern_string("??");
			file_idx = intern_string("??");
		}
	}

	/* file/line unresolved (function+module symbolication only) */
	trace_printf("i %lx %x %x %x %x\n",
		(unsigned long)(uintptr_t)addr, mod_idx, fn_idx, file_idx, 0);
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

static void trace_table_grow(void) {
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

static int intern_trace(int ip_idx, uintptr_t ip_hex, int parent_trace) {
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
/* Emits "a <size> <trace_idx>" records for heaptrack format v1+ */

typedef struct {
	uint64_t size;
	int trace_idx;
	int index;    /* 0-based, -1 = empty */
} ainfo_entry;

static ainfo_entry *ainfo_table = NULL;
static int ainfo_table_cap = 0;
static int ainfo_table_count = 0;
static int ainfo_next_index = 0;

static void ainfo_table_grow(void) {
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

/* Returns 0-based alloc info index. Emits "a" record on first insertion. */
static int intern_alloc_info(uint64_t size, int trace_idx) {
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
	void *key;         /* NULL = empty */
	int alloc_info_idx;
} ptr_entry;

static ptr_entry *ptr_table = NULL;
static int ptr_table_cap = 0;
static int ptr_table_count = 0;

static void ptr_table_grow(void) {
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

static void ptr_map_insert(void *ptr, int alloc_info_idx) {
	if (ptr_table_count * 10 >= ptr_table_cap * 7)
		ptr_table_grow();
	unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
	while (ptr_table[h].key) {
		if (ptr_table[h].key == ptr) {
			/* Overwrite (reallocation of same address) */
			ptr_table[h].alloc_info_idx = alloc_info_idx;
			return;
		}
		h = (h + 1) & (ptr_table_cap - 1);
	}
	ptr_table[h].key = ptr;
	ptr_table[h].alloc_info_idx = alloc_info_idx;
	ptr_table_count++;
}

/* Returns alloc_info_idx, or -1 if not found. Removes the entry. */
static int ptr_map_remove(void *ptr) {
	if (!ptr_table_cap) return -1;
	unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
	while (ptr_table[h].key) {
		if (ptr_table[h].key == ptr) {
			int idx = ptr_table[h].alloc_info_idx;
			/* Tombstone: mark empty and rehash following cluster */
			ptr_table[h].key = NULL;
			ptr_table_count--;
			/* Rehash entries that might have been displaced past this slot */
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

#define HT_MAX_DEPTH 64

/* Innermost frames that are always our own tracer (the return addresses into
 * do_alloc_locked and machismo_heaptrack_{alloc,realloc}); dropped so the
 * trace leaf is the real allocation primitive (malloc/calloc) like normal
 * heaptrack output. noinline on ht_capture + do_alloc_locked pins this count. */
#define HT_SKIP_FRAMES 2

__attribute__((noinline))
static int ht_capture(void **buf, int max) {
	void **fp = (void **)__builtin_frame_address(0);

	/* Determine stack bounds for the current thread. Worker threads created
	 * through the shim use real pthread stacks (pthread_getattr_np works); the
	 * guest's main thread runs on machismo's custom Mach-O stack, which it does
	 * not. Pick whichever range actually contains fp; else walk unbounded and
	 * rely on the monotonic/alignment checks. */
	void *lo = NULL, *hi = NULL;
	pthread_attr_t a;
	if (pthread_getattr_np(pthread_self(), &a) == 0) {
		void *base; size_t sz;
		if (pthread_attr_getstack(&a, &base, &sz) == 0 &&
		    (void *)fp >= base && (void *)fp < (void *)((char *)base + sz)) {
			lo = base;
			hi = (char *)base + sz;
		}
		pthread_attr_destroy(&a);
	}
	if (!hi && __machismo_main_stack_top &&
	    (void *)fp < __machismo_main_stack_top) {
		hi = __machismo_main_stack_top;
		lo = (char *)hi - (64 << 20);   /* generous 64MB lower bound */
	}

	int depth = 0;
	while (fp && depth < max) {
		void *lr = fp[1];
		if (!lr) break;
		buf[depth++] = lr;
		void **next = (void **)fp[0];
		if (next <= fp) break;            /* frame chain must ascend */
		if ((uintptr_t)next & 0xF) break; /* arm64 x29 is 16-byte aligned */
		if (hi && (void *)next >= hi) break;
		if (lo && (void *)next < lo) break;
		fp = next;
	}
	return depth;
}

/* ---- Timestamp ---- */

static long trace_elapsed_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - trace_start_time.tv_sec) * 1000L +
		(now.tv_nsec - trace_start_time.tv_nsec) / 1000000L;
}

/* ---- Core emit (caller holds g_lock + g_in_hook) ---- */

__attribute__((noinline))
static void do_alloc_locked(void *ptr, size_t size) {
	void *frames[HT_MAX_DEPTH];
	int nframes = ht_capture(frames, HT_MAX_DEPTH);

	/* Drop our own innermost frames so malloc/calloc is the leaf. */
	int skip = nframes > HT_SKIP_FRAMES ? HT_SKIP_FRAMES : 0;

	/* Build trace tree: outermost to innermost. */
	int parent_trace = 0;
	for (int i = nframes - 1; i >= skip; i--) {
		int ip_idx = intern_ip(frames[i]);
		parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
	}

	int ainfo_idx = intern_alloc_info((uint64_t)size, parent_trace);
	ptr_map_insert(ptr, ainfo_idx);

	if ((++trace_alloc_count & 0x3FF) == 0) {
		trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
		/* Push to the OS periodically. The game's normal dev-quit is a hard
		 * terminate (SDL_NO_SIGNAL_HANDLERS) that never runs atexit, so without
		 * this the buffered tail is lost. The trace is append-only line records,
		 * so a kill leaves a valid prefix (heaptrack tolerates a torn final line). */
		trace_flush();
		fflush(trace_file);
	}

	trace_printf("+ %x\n", ainfo_idx);   /* v1: + <alloc_info_idx> */
}

static void do_free_locked(void *ptr) {
	int ainfo_idx = ptr_map_remove(ptr);
	if (ainfo_idx < 0) return;            /* untracked pointer — skip */
	trace_printf("- %x\n", ainfo_idx);    /* v1: - <alloc_info_idx> */
}

/* ---- Public hooks (called by the shim) ---- */

void machismo_heaptrack_alloc(void *ptr, size_t size) {
	if (!trace_file || !ptr || g_in_hook) return;
	g_in_hook = 1;
	pthread_mutex_lock(&g_lock);
	do_alloc_locked(ptr, size);
	pthread_mutex_unlock(&g_lock);
	g_in_hook = 0;
}

void machismo_heaptrack_free(void *ptr) {
	if (!trace_file || !ptr || g_in_hook) return;
	g_in_hook = 1;
	pthread_mutex_lock(&g_lock);
	do_free_locked(ptr);
	pthread_mutex_unlock(&g_lock);
	g_in_hook = 0;
}

void machismo_heaptrack_realloc(void *old_ptr, void *new_ptr, size_t size) {
	if (!trace_file || g_in_hook) return;
	/* realloc failure: real_realloc returned NULL but old_ptr is still live —
	 * don't record a free for it. realloc(ptr,0) frees and returns NULL. */
	if (!new_ptr && size != 0) return;
	g_in_hook = 1;
	pthread_mutex_lock(&g_lock);
	if (old_ptr) do_free_locked(old_ptr);          /* heaptrack: realloc = free+alloc */
	if (new_ptr) do_alloc_locked(new_ptr, size);
	pthread_mutex_unlock(&g_lock);
	g_in_hook = 0;
}

/* ---- Lifecycle ---- */

void machismo_heaptrack_init(void) {
	const char *filename = getenv("MACHISMO_HEAPTRACK");
	if (!filename || !*filename)
		return;

	trace_file = fopen(filename, "w");
	if (!trace_file) {
		fprintf(stderr, "[heaptrack] failed to open %s for writing\n", filename);
		return;
	}

	trace_buf = (char *)malloc(TRACE_BUF_SIZE);
	trace_buf_pos = 0;

	clock_gettime(CLOCK_MONOTONIC, &trace_start_time);

	/* Header: heaptrack version 1.4.0, file format version 1. */
	trace_printf("v 10400 1\n");

	char exe[1024];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len > 0) {
		exe[len] = 0;
		trace_printf("x %lx %s\n", (unsigned long)len, exe);
	} else {
		trace_printf("x 8 machismo\n");
	}

	trace_printf("c 0\n");

	machismo_heaptrack_active = 1;
	fprintf(stderr, "[heaptrack] tracing allocations to %s\n", filename);
}

void machismo_heaptrack_close(void) {
	if (!trace_file) return;
	pthread_mutex_lock(&g_lock);
	machismo_heaptrack_active = 0;

	trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
	trace_flush();
	fclose(trace_file);
	trace_file = NULL;
	pthread_mutex_unlock(&g_lock);
}
