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
 * Beyond malloc, the tracer also records RESIDENT-relevant mmaps — anonymous,
 * tmpfs/shm and device mappings (e.g. /dev/mali0 GPU memory); regular-file
 * page cache is skipped — fed from the libsystem shim (guest) and from the
 * exe-level mmap/munmap interposers at the bottom of this file (host libs).
 * Each carries a synthetic leaf frame ("mmap(anonymous)", "mmap(/dev/...)")
 * so flamegraphs group mapped memory by category. Periodic `R` records
 * (/proc/self/statm) give the GUI the total-RSS curve next to the tracked
 * total, so any remaining blind spot is a visible gap, not a silent miss.
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mman.h>

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

	/* heaptrack_init runs BEFORE image registration (machismo.c main), so with
	 * the exe-level interposers this function's own allocations (the g_syms
	 * realloc, the module strdup) would feed the tracer — whose ht_lookup_sym
	 * walks g_syms, i.e. the exact array mid-realloc (use-after-free, hit on
	 * the first post-restructure run). The tracer must never trace its own
	 * bookkeeping: latch the re-entrancy guard while mutating the index. */
	g_in_hook = 1;

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

	/* Keep the index sorted on exit: ht_resolve_unlocked reads it with NO lock
	 * once guest threads exist. Image registration is strictly single-threaded
	 * (machismo main, before the guest entry), so sorting here is race-free and
	 * the lazy sort in the read path is gone (it would be a data race). */
	qsort(g_syms, g_nsyms, sizeof(*g_syms), ht_sym_cmp);
	g_syms_sorted = 1;

	g_in_hook = 0;
}

/* Nearest preceding symbol. Returns 1 + fills *out_fn / *out_mod (+ the matched
 * symbol's runtime base in *out_base if non-NULL), else 0.
 * Called with NO lock — safe because the index is append-then-sort inside
 * add_image (single-threaded load phase, self-feeds latched) and frozen by
 * the time concurrent readers exist. Never sorts here: that was a data race.
 * Non-static + async-signal-safe (pure reads over the frozen sorted array, no
 * locks/malloc) so the crash handler can reuse it from a signal context. */
int ht_lookup_sym(uint64_t ip, const char **out_fn, const char **out_mod,
                  uint64_t *out_base) {
	if (!g_nsyms || !g_syms_sorted) return 0;
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
	if (out_base) *out_base = s->addr;
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

/* ---- Unlocked symbolication ----
 *
 * LOCK ORDER INVARIANT: g_lock is a LEAF lock — nothing that runs under it
 * may take a foreign lock. dladdr takes glibc's loader lock, and glibc's
 * dl* entry points malloc WHILE HOLDING that lock (proven on device:
 * dlsym's failing-lookup path calls _dl_exception_create_format -> malloc ->
 * our interposer -> wants g_lock, while a tracing thread held g_lock and sat
 * in dladdr wanting the loader lock — ABBA deadlock at SDL_Init's udev
 * probing). So symbolication (dladdr + __cxa_demangle) happens OUT HERE,
 * before g_lock is taken, and intern_ip only consumes the results. */

struct ht_ipres {
	void *addr;
	const char *fn;      /* display name (demangled when dem is set) */
	const char *mod;
	char *dem;           /* owned __cxa_demangle buffer; freed by the hook */
};

/* Pure probe of the ip intern table (caller holds g_lock): index or 0. */
static int lookup_ip(void *addr) {
	if (!ip_table_cap) return 0;
	unsigned int h = hash_ptr(addr) & (ip_table_cap - 1);
	while (ip_table[h].key) {
		if (ip_table[h].key == addr)
			return ip_table[h].index;
		h = (h + 1) & (ip_table_cap - 1);
	}
	return 0;
}

/* Resolve one IP with NO tracer lock held. Guest frames hit the reverse
 * Mach-O index (read-only once tracing is active; sorted eagerly in init);
 * host frames fall back to dladdr. */
static void ht_resolve_unlocked(struct ht_ipres *r) {
	const char *fn = NULL, *mod = NULL;
	r->fn = "??"; r->mod = "??"; r->dem = NULL;
	if (ht_lookup_sym((uint64_t)(uintptr_t)r->addr, &fn, &mod, NULL)) {
		r->mod = mod;
		r->fn = fn[0] == '_' ? fn + 1 : fn;          /* drop Mach-O '_' */
	} else {
		Dl_info info;
		if (dladdr(r->addr, &info) && info.dli_sname) {
			r->mod = info.dli_fname ? info.dli_fname : "??";
			r->fn = info.dli_sname;
		}
	}
	if (r->fn[0] == '_' && r->fn[1] == 'Z') {
		int status = 0;
		char *dem = __cxa_demangle(r->fn, NULL, NULL, &status);
		if (dem) { r->dem = dem; r->fn = dem; }
	}
}

/* Phase 1: under a brief g_lock, find which frame IPs are not yet interned;
 * phase 2: resolve those unlocked. Returns the count filled into res[].
 * Caller must NOT hold g_lock. Append-only table => an IP known here is
 * still known at intern time, and one we resolved that another thread interns
 * meanwhile just goes unused. */
static int ht_presolve(void **frames, int n, struct ht_ipres *res) {
	int need = 0;
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < n; i++) {
		if (lookup_ip(frames[i])) continue;
		int dup = 0;
		for (int j = 0; j < need; j++)
			if (res[j].addr == frames[i]) { dup = 1; break; }
		if (!dup)
			res[need++].addr = frames[i];
	}
	pthread_mutex_unlock(&g_lock);
	for (int i = 0; i < need; i++)
		ht_resolve_unlocked(&res[i]);
	return need;
}

static void ht_ipres_free(struct ht_ipres *res, int nres) {
	for (int i = 0; i < nres; i++)
		free(res[i].dem);
}

/* Intern an IP (caller holds g_lock). New IPs take their names from the
 * pre-resolved hints; a fresh IP missing from the hints can only mean the
 * caller skipped presolve — emitted as "??" rather than resolved here (no
 * dladdr under g_lock, see above). */
static int intern_ip(void *addr, const struct ht_ipres *res, int nres) {
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

	const struct ht_ipres *r = NULL;
	for (int j = 0; j < nres; j++)
		if (res[j].addr == addr) { r = &res[j]; break; }

	int mod_idx  = intern_string(r ? r->mod : "??");
	int fn_idx   = intern_string(r ? r->fn  : "??");
	int file_idx = intern_string("??");

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

/* ==== Synthetic tag IPs: category leaf frames for mmap events ==== */
/* A fabricated, never-real instruction pointer per category string
 * ("mmap(anonymous)", "mmap(/dev/mali0)", ...) emitted as a normal `i` record
 * so it can be the innermost trace node — flamegraphs then group mapped memory
 * by category, separate from the heap. The fake addresses live far above any
 * mappable user address. Index discipline: every `i` line emitted (here or in
 * intern_ip) must bump ip_next_index exactly once, in emission order — both
 * run under g_lock. */

#define HT_TAG_MAX 32
static struct {
	char *tag;
	int ip_idx;
	uintptr_t addr;
} tag_ips[HT_TAG_MAX];
static int tag_ip_count = 0;

static int intern_tag_ip(const char *tag, uintptr_t *out_addr) {
	for (int i = 0; i < tag_ip_count; i++) {
		if (strcmp(tag_ips[i].tag, tag) == 0) {
			*out_addr = tag_ips[i].addr;
			return tag_ips[i].ip_idx;
		}
	}
	int slot = tag_ip_count;
	if (slot == HT_TAG_MAX - 1)                 /* table full: last slot becomes  */
		tag = "mmap(other)";                    /* the catch-all, ending growth   */
	uintptr_t addr = 0xfff0000000000000ull + (uintptr_t)slot + 1;
	int mod_idx  = intern_string("<mmap>");
	int fn_idx   = intern_string(tag);
	int file_idx = intern_string("??");
	int idx = ip_next_index++;
	trace_printf("i %lx %x %x %x %x\n",
		(unsigned long)addr, mod_idx, fn_idx, file_idx, 0);
	tag_ips[slot].tag = strdup(tag);
	tag_ips[slot].ip_idx = idx;
	tag_ips[slot].addr = addr;
	tag_ip_count++;
	*out_addr = addr;
	return idx;
}

/* ==== Tracked mmap regions (for munmap, incl. partial unmaps) ==== */
/* Separate from the malloc ptr map: munmap may release any sub-range of a
 * mapping, so we keep (addr, len) and split survivors. Linear scan — mmap
 * rate is orders of magnitude below malloc rate. */

struct ht_region {
	char *addr;
	size_t len;
	int ainfo_idx;
	int trace_idx;
};
static struct ht_region *regions = NULL;
static int region_count = 0, region_cap = 0;

static void region_add(char *addr, size_t len, int ainfo_idx, int trace_idx) {
	if (region_count == region_cap) {
		region_cap = region_cap ? region_cap * 2 : 256;
		regions = (struct ht_region *)realloc(regions, region_cap * sizeof(*regions));
	}
	regions[region_count].addr = addr;
	regions[region_count].len = len;
	regions[region_count].ainfo_idx = ainfo_idx;
	regions[region_count].trace_idx = trace_idx;
	region_count++;
}

/* ---- RSS ground truth ---- */
/* Periodic `R <resident pages>` records (scaled by the pagesize from the `I`
 * header) give heaptrack the total-RSS curve next to the tracked total — any
 * residual blind spot (glibc-internal maps, ioctl-only GPU memory) shows up as
 * a visible gap instead of silently not existing. /proc/self/statm field 2. */
static void emit_rss_locked(void) {
	char buf[128];
	int fd = open("/proc/self/statm", O_RDONLY);
	if (fd < 0) return;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return;
	buf[n] = 0;
	unsigned long vsz = 0, rss = 0;
	if (sscanf(buf, "%lu %lu", &vsz, &rss) == 2 && rss)
		trace_printf("R %lx\n", rss);
}

/* ---- Frame pointer walking ---- */

#define HT_MAX_DEPTH 64

/* Innermost frames that are always our own tracer (the return address into
 * machismo_heaptrack_{alloc,realloc,mmap}, which now capture directly);
 * dropped so the trace leaf is the real allocation primitive (malloc/calloc/
 * the mmap wrapper) like normal heaptrack output. noinline on ht_capture
 * pins this count. */
#define HT_SKIP_FRAMES 1

/* Per-thread cache of the pthread stack range. pthread_getattr_np is cheap for
 * pthread-created threads but for the INITIAL thread it parses the whole of
 * /proc/self/maps with getdelim — and the guest's main thread IS the initial
 * thread (machismo switches its stack, no pthread_create). Calling it per
 * event made every main-thread allocation cost a full maps parse; with the
 * exe-level malloc interposers feeding host traffic too (SDL_Init's udev scan
 * alone is tens of thousands of main-thread strdups) startup degraded into an
 * apparent hang. The range is a per-thread constant — resolve it once. */
static __thread void *t_stk_lo, *t_stk_hi;
static __thread int   t_stk_init;

__attribute__((noinline))
static int ht_capture(void **buf, int max) {
	void **fp = (void **)__builtin_frame_address(0);

	if (!t_stk_init) {
		pthread_attr_t a;
		if (pthread_getattr_np(pthread_self(), &a) == 0) {
			void *base; size_t sz;
			if (pthread_attr_getstack(&a, &base, &sz) == 0) {
				t_stk_lo = base;
				t_stk_hi = (char *)base + sz;
			}
			pthread_attr_destroy(&a);
		}
		t_stk_init = 1;
	}

	/* Pick whichever known range actually contains fp: the cached pthread
	 * stack (worker threads; machismo's original C stack for the initial
	 * thread), else the custom Mach-O stack the guest main thread switched to
	 * (pthread_getattr_np cannot describe it); else walk unbounded and rely on
	 * the monotonic/alignment checks. */
	void *lo = NULL, *hi = NULL;
	if (t_stk_hi && (void *)fp >= t_stk_lo && (void *)fp < t_stk_hi) {
		lo = t_stk_lo;
		hi = t_stk_hi;
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

/* ---- Core emit (caller holds g_lock + g_in_hook; frames captured and
 * presolved by the public hook BEFORE locking — see ht_presolve) ---- */

static void do_alloc_locked(void *ptr, size_t size, void **frames, int nframes,
                            const struct ht_ipres *res, int nres) {
	/* Drop our own innermost frame so malloc/calloc is the leaf. */
	int skip = nframes > HT_SKIP_FRAMES ? HT_SKIP_FRAMES : 0;

	/* Build trace tree: outermost to innermost. */
	int parent_trace = 0;
	for (int i = nframes - 1; i >= skip; i--) {
		int ip_idx = intern_ip(frames[i], res, nres);
		parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
	}

	int ainfo_idx = intern_alloc_info((uint64_t)size, parent_trace);
	ptr_map_insert(ptr, ainfo_idx);

	if ((++trace_alloc_count & 0x3FF) == 0) {
		trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
		emit_rss_locked();
		/* Push to the OS periodically. The game's normal dev-quit is a hard
		 * terminate (SDL_NO_SIGNAL_HANDLERS) that never runs atexit, so without
		 * this the buffered tail is lost. The trace is append-only line records,
		 * so a kill leaves a valid prefix (heaptrack tolerates a torn final line). */
		trace_flush();
		fflush(trace_file);
	}

	trace_printf("+ %x\n", ainfo_idx);   /* v1: + <alloc_info_idx> */
}

/* mmap event: like do_alloc_locked, but the trace gets an extra synthetic
 * innermost node (the category tag) and the region is remembered for munmap. */
static void do_mmap_locked(void *ptr, size_t len, const char *tag,
                           void **frames, int nframes,
                           const struct ht_ipres *res, int nres) {
	int skip = nframes > HT_SKIP_FRAMES ? HT_SKIP_FRAMES : 0;

	int parent_trace = 0;
	for (int i = nframes - 1; i >= skip; i--) {
		int ip_idx = intern_ip(frames[i], res, nres);
		parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
	}
	uintptr_t tag_addr;
	int tag_ip = intern_tag_ip(tag, &tag_addr);
	parent_trace = intern_trace(tag_ip, tag_addr, parent_trace);

	int ainfo_idx = intern_alloc_info((uint64_t)len, parent_trace);
	region_add((char *)ptr, len, ainfo_idx, parent_trace);
	trace_printf("+ %x\n", ainfo_idx);

	/* Mapping changes are rare and load-bearing: timestamp + RSS + flush each. */
	trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
	emit_rss_locked();
	trace_flush();
	fflush(trace_file);
}

static void do_munmap_locked(void *ptr, size_t len) {
	char *us = (char *)ptr, *ue = us + len;
	int hit = 0;
	for (int i = 0; i < region_count; ) {
		struct ht_region r = regions[i];
		char *rs = r.addr, *re = rs + r.len;
		if (re <= us || rs >= ue) { i++; continue; }
		/* Overlap: retire the record, re-add surviving head/tail at the same
		 * trace so partial unmaps keep the leak accounting exact. Don't advance
		 * i — the swapped-in element still needs examining; survivors appended
		 * at the end can't overlap [us,ue). */
		hit = 1;
		trace_printf("- %x\n", r.ainfo_idx);
		regions[i] = regions[--region_count];
		if (rs < us) {
			int a = intern_alloc_info((uint64_t)(us - rs), r.trace_idx);
			region_add(rs, (size_t)(us - rs), a, r.trace_idx);
			trace_printf("+ %x\n", a);
		}
		if (re > ue) {
			int a = intern_alloc_info((uint64_t)(re - ue), r.trace_idx);
			region_add(ue, (size_t)(re - ue), a, r.trace_idx);
			trace_printf("+ %x\n", a);
		}
	}
	if (hit) {
		trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
		emit_rss_locked();
		trace_flush();
		fflush(trace_file);
	}
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
	void *frames[HT_MAX_DEPTH];
	struct ht_ipres res[HT_MAX_DEPTH];
	int n = ht_capture(frames, HT_MAX_DEPTH);
	int nres = ht_presolve(frames, n, res);     /* dladdr/demangle, no g_lock */
	pthread_mutex_lock(&g_lock);
	do_alloc_locked(ptr, size, frames, n, res, nres);
	pthread_mutex_unlock(&g_lock);
	ht_ipres_free(res, nres);
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

/* TMPFS_MAGIC (linux/magic.h, avoided to keep includes portable). */
#define HT_TMPFS_MAGIC 0x01021994

void machismo_heaptrack_mmap(void *ptr, size_t length, int prot, int flags, int fd) {
	(void)prot;
	if (!trace_file || !ptr || ptr == MAP_FAILED || !length || g_in_hook) return;

	/* Residency filter: we only track memory that stays resident under
	 * pressure. Anonymous (by FLAG, not fd — callers may pass a stale fd that
	 * the kernel ignores under MAP_ANONYMOUS) and tmpfs/shm-backed (memfd —
	 * S_ISREG but on tmpfs, swap-backed not disk-evictable) and device
	 * mappings (/dev/mali0 etc. — pinned kernel/CMA) count; regular-file
	 * mappings are evictable page cache and are skipped, same definition as
	 * smaps_rollup Anonymous. */
	int track = 1;
	char tag[96] = "mmap(anonymous)";
	if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) != 0) {
			track = 0;
		} else if (S_ISREG(st.st_mode)) {
			struct statfs sfs;
			if (fstatfs(fd, &sfs) != 0 || sfs.f_type != HT_TMPFS_MAGIC)
				track = 0;                    /* disk-backed — skip */
		}
		if (track) {
			char link[48], path[80];
			snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
			ssize_t n = readlink(link, path, sizeof(path) - 1);
			if (n > 0) {
				path[n] = 0;
				snprintf(tag, sizeof(tag), "mmap(%s)", path);
			} else {
				snprintf(tag, sizeof(tag), "mmap(fd)");
			}
		}
	}

	/* MAP_FIXED REPLACES whatever was mapped there — model it as an implicit
	 * munmap of overlapping tracked ranges even when the new mapping itself is
	 * filtered out. The canonical case is the gothic shim's mmap_bank: a big
	 * anonymous reservation whose tail is immediately overlaid with the
	 * (evictable, skipped) file — without the punch-out the whole bank would
	 * stay miscounted as resident anonymous memory forever. */
	if (!track && !(flags & MAP_FIXED)) return;

	g_in_hook = 1;
	void *frames[HT_MAX_DEPTH];
	struct ht_ipres res[HT_MAX_DEPTH];
	int n = 0, nres = 0;
	if (track) {
		n = ht_capture(frames, HT_MAX_DEPTH);
		nres = ht_presolve(frames, n, res);     /* dladdr/demangle, no g_lock */
	}
	pthread_mutex_lock(&g_lock);
	if (flags & MAP_FIXED)
		do_munmap_locked(ptr, length);
	if (track)
		do_mmap_locked(ptr, length, tag, frames, n, res, nres);
	pthread_mutex_unlock(&g_lock);
	ht_ipres_free(res, nres);
	g_in_hook = 0;
}

void machismo_heaptrack_munmap(void *ptr, size_t length) {
	if (!trace_file || !ptr || !length || g_in_hook) return;
	g_in_hook = 1;
	pthread_mutex_lock(&g_lock);
	do_munmap_locked(ptr, length);
	pthread_mutex_unlock(&g_lock);
	g_in_hook = 0;
}

void machismo_heaptrack_realloc(void *old_ptr, void *new_ptr, size_t size) {
	if (!trace_file || g_in_hook) return;
	/* realloc failure: real_realloc returned NULL but old_ptr is still live —
	 * don't record a free for it. realloc(ptr,0) frees and returns NULL. */
	if (!new_ptr && size != 0) return;
	g_in_hook = 1;
	void *frames[HT_MAX_DEPTH];
	struct ht_ipres res[HT_MAX_DEPTH];
	int n = 0, nres = 0;
	if (new_ptr) {
		n = ht_capture(frames, HT_MAX_DEPTH);
		nres = ht_presolve(frames, n, res);     /* dladdr/demangle, no g_lock */
	}
	pthread_mutex_lock(&g_lock);
	if (old_ptr) do_free_locked(old_ptr);          /* heaptrack: realloc = free+alloc */
	if (new_ptr) do_alloc_locked(new_ptr, size, frames, n, res, nres);
	pthread_mutex_unlock(&g_lock);
	ht_ipres_free(res, nres);
	g_in_hook = 0;
}

/* ---- Process-wide mmap/munmap interposition (host libraries) ---- */
/* The GUEST's mmap reaches the libsystem shim's Darwin-flag-translating wrapper
 * via per-handle dlsym (dylib_map), so it is covered there. HOST libraries
 * (libmali, mesa, SDL, ALSA) resolve mmap through the global scope instead —
 * and there libc.so.6 precedes the dlopened shim (it is a startup dependency),
 * so they bind straight to glibc and their mappings would never be seen. The
 * EXECUTABLE is always FIRST in the global scope, so defining mmap/munmap here
 * (this file links into the machismo exe, which is -rdynamic) wins for every
 * host library — on shared-memory Mali devices that is precisely the GPU
 * mapping traffic (/dev/mali0) we most want in the trace. The real operation
 * is a raw syscall: no symbol-resolution ambiguity, no recursion, and
 * byte-identical behavior when tracing is off (the hook no-ops on
 * trace_file == NULL). The shim's own wrapper is unaffected (its RTLD_NEXT
 * lookup resolves glibc's symbol directly, never re-entering the global
 * scope), so guest traffic feeds the tracer exactly once — from the shim.
 * glibc-INTERNAL mmaps (malloc arenas, thread stacks) call internal aliases
 * and bypass this; for arenas that is what prevents double counting against
 * the malloc-layer hooks. */

#include <sys/syscall.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *ret = (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
	if (ret != MAP_FAILED)
		machismo_heaptrack_mmap(ret, length, prot, flags, fd);
	return ret;
}

/* LFS-built libraries import mmap64, not mmap — libmali (GLIBC_2.17 mmap64 +
 * munmap) and mesa's libgallium both do, so without this alias every GPU
 * mapping bypassed the tracer (verified: an Asahi trace contained zero
 * /dev/dri maps). On aarch64 off_t is 64-bit and glibc's mmap64 IS mmap; an
 * unversioned exe definition satisfies the versioned reference. */
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return mmap(addr, length, prot, flags, fd, offset);
}

/* ---- Process-wide malloc-family interposition (host libraries) ---- */
/* Same scope trick as mmap above: host libraries (libmali, mesa, SDL,
 * libgothic_patches' C++ containers) bind malloc/free through the global
 * scope, where the exe is first — defining them here captures every HOST
 * heap allocation with a call stack. Motivated by the Mina device leak:
 * RssAnon climbed ~60M/min while every GUEST allocation was provably flat,
 * i.e. the leak lives in host-library malloc traffic the tracer previously
 * could not see (libmali's CPU-side buffer shadows being the suspect).
 *
 * The GUEST does NOT double-feed: the libsystem shim resolves its real_malloc
 * via dlsym(RTLD_NEXT) from its own (dlopened, late) position in the link-map
 * chain — the exe precedes it and is never searched — so guest traffic goes
 * shim_malloc -> glibc directly and is fed to the tracer once, by the shim's
 * hooks. Host traffic goes exe interposer -> glibc and is fed once, here.
 * Tracer-internal allocations are already filtered: the public hooks no-op
 * under g_in_hook (set around all tracer work).
 *
 * Forwarding is 1:1 to glibc (resolved once via dlsym(RTLD_NEXT)); a tiny
 * static arena serves the recursive callocs dlsym itself makes during
 * resolution. free()/realloc() of arena pointers are handled (never passed
 * to glibc). aligned variants are interposed too — drivers commonly
 * posix_memalign their staging memory. */

static void *(*r_malloc)(size_t);
static void *(*r_calloc)(size_t, size_t);
static void *(*r_realloc)(void *, size_t);
static void  (*r_free)(void *);
static void *(*r_memalign)(size_t, size_t);
static int   (*r_posix_memalign)(void **, size_t, size_t);
static void *(*r_aligned_alloc)(size_t, size_t);

/* Lock-order-inversion guard. The tracer symbolicates (dladdr) while holding
 * g_lock, and dladdr takes glibc's loader lock; dlopen HOLDS the loader lock
 * across its internal mallocs AND the new library's constructors. Feeding
 * those allocations would make a dlopen thread want g_lock while a tracing
 * thread holding g_lock wants the loader lock — ABBA deadlock (hit on device:
 * SDL_Init's backend-dlopen storm vs. early-boot alloc tracing). So dlopen/
 * dlclose run with feeds suppressed on their thread; the allocations still
 * FORWARD to glibc, they just go unrecorded (their later frees feed
 * do_free_locked, which ignores unknown pointers). dlsym is not wrapped: its
 * dlerror strdup happens after the loader lock is released. */
static __thread int g_in_dl;
static void *(*r_dlopen)(const char *, int);
static int   (*r_dlclose)(void *);

void *dlopen(const char *file, int mode)
{
	if (!r_dlopen)
		r_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
	g_in_dl++;
	void *h = r_dlopen(file, mode);
	g_in_dl--;
	return h;
}

int dlclose(void *handle)
{
	if (!r_dlclose)
		r_dlclose = (int (*)(void *))dlsym(RTLD_NEXT, "dlclose");
	g_in_dl++;
	int rc = r_dlclose(handle);
	g_in_dl--;
	return rc;
}

static char   hboot_buf[65536] __attribute__((aligned(16)));
static size_t hboot_off;
static int    hresolving;

static int hboot_owns(const void *p)
{
	return (const char *)p >= hboot_buf &&
	       (const char *)p <  hboot_buf + sizeof(hboot_buf);
}

static void *hboot_alloc(size_t n)
{
	size_t need = (n + 15) & ~(size_t)15;
	size_t off = __atomic_fetch_add(&hboot_off, need, __ATOMIC_RELAXED);
	if (off + need > sizeof(hboot_buf)) return NULL;
	return hboot_buf + off;          /* bss: already zeroed, never reused */
}

static void hresolve(void)
{
	if (r_free) return;
	hresolving = 1;
	r_malloc         = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
	r_calloc         = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
	r_realloc        = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
	r_memalign       = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
	r_posix_memalign = (int (*)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
	r_aligned_alloc  = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
	hresolving = 0;
	/* r_free last: it doubles as the "resolved" flag for the fast path. */
	__atomic_store_n(&r_free, (void (*)(void *))dlsym(RTLD_NEXT, "free"),
	                 __ATOMIC_RELEASE);
}

void *malloc(size_t size)
{
	if (!r_free) {
		if (hresolving) return hboot_alloc(size);
		hresolve();
	}
	void *p = r_malloc(size);
	if (p && !g_in_dl) machismo_heaptrack_alloc(p, size);
	return p;
}

void *calloc(size_t nmemb, size_t size)
{
	if (!r_free) {
		if (hresolving) return hboot_alloc(nmemb * size);
		hresolve();
	}
	void *p = r_calloc(nmemb, size);
	if (p && !g_in_dl) machismo_heaptrack_alloc(p, nmemb * size);
	return p;
}

void *realloc(void *ptr, size_t size)
{
	if (!r_free) {
		if (hresolving) return hboot_alloc(size);
		hresolve();
	}
	if (hboot_owns(ptr)) {                     /* migrate out of the arena */
		void *p = r_malloc(size);
		if (p) {
			size_t avail = (size_t)(hboot_buf + sizeof(hboot_buf) - (char *)ptr);
			memcpy(p, ptr, size < avail ? size : avail);
			if (!g_in_dl) machismo_heaptrack_alloc(p, size);
		}
		return p;
	}
	void *np = r_realloc(ptr, size);
	if (!g_in_dl)
		machismo_heaptrack_realloc(ptr, np, size);  /* self-guards NULL/size==0 */
	return np;
}

void free(void *ptr)
{
	if (!ptr || hboot_owns(ptr)) return;
	if (!r_free) hresolve();
	if (!g_in_dl) machismo_heaptrack_free(ptr);
	r_free(ptr);
}

void *memalign(size_t alignment, size_t size)
{
	if (!r_free) hresolve();
	void *p = r_memalign ? r_memalign(alignment, size) : NULL;
	if (p && !g_in_dl) machismo_heaptrack_alloc(p, size);
	return p;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (!r_free) hresolve();
	if (!r_posix_memalign) return 12 /* ENOMEM */;
	int rc = r_posix_memalign(memptr, alignment, size);
	if (rc == 0 && *memptr && !g_in_dl) machismo_heaptrack_alloc(*memptr, size);
	return rc;
}

void *aligned_alloc(size_t alignment, size_t size)
{
	if (!r_free) hresolve();
	void *p = r_aligned_alloc ? r_aligned_alloc(alignment, size) : NULL;
	if (p && !g_in_dl) machismo_heaptrack_alloc(p, size);
	return p;
}

int munmap(void *addr, size_t length)
{
	int ret = (int)syscall(SYS_munmap, addr, length);
	if (ret == 0)
		machismo_heaptrack_munmap(addr, length);
	return ret;
}

/* ---- Lifecycle ---- */

void machismo_heaptrack_init(void) {
	const char *filename = getenv("MACHISMO_HEAPTRACK");
	if (!filename || !*filename)
		return;

	/* The exe-level malloc interposers feed the hooks process-wide, and this
	 * function's own allocations (trace_buf, stdio) route through them with
	 * trace_file already set but the tracer half-initialized — hold the
	 * re-entrancy latch for the whole setup. */
	g_in_hook = 1;

	trace_file = fopen(filename, "w");
	if (!trace_file) {
		fprintf(stderr, "[heaptrack] failed to open %s for writing\n", filename);
		g_in_hook = 0;
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

	/* System info: pagesize + physical pages. heaptrack scales the periodic
	 * `R <resident pages>` records by this pagesize for the total-RSS curve. */
	trace_printf("I %lx %lx\n",
		(unsigned long)sysconf(_SC_PAGESIZE),
		(unsigned long)sysconf(_SC_PHYS_PAGES));

	trace_printf("c 0\n");

	machismo_heaptrack_active = 1;
	g_in_hook = 0;
	/* Self-check: a canary pair proves the exe-level malloc interposers feed
	 * the tracer — every trace starts with one matched +/- (a single 32-byte
	 * "temporary allocation" in heaptrack terms). If a trace lacks it, host
	 * interposition is broken; guest (shim-fed) events are separate. The asm
	 * barrier stops GCC eliding the dead malloc/free pair. */
	void *canary = malloc(32);
	__asm__ volatile("" : : "r"(canary) : "memory");
	free(canary);

	/* Land header + canary on disk now — a run killed before the first
	 * periodic flush should still leave a valid trace. */
	g_in_hook = 1;
	pthread_mutex_lock(&g_lock);
	trace_flush();
	fflush(trace_file);
	pthread_mutex_unlock(&g_lock);
	g_in_hook = 0;
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
