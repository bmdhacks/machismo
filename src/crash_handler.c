/*
 * crash_handler.c — always-on fatal-signal handler with a cross-world
 * (ELF + Mach-O) hydrated backtrace. See crash_handler.h for the overview.
 *
 * Design notes / async-signal-safety:
 *  - Init (crash_handler_init) runs on the loader's main thread before the guest
 *    starts and may use ordinary libc (fopen/getline/sscanf, etc). It builds:
 *      * g_images[]  — text ranges of every Mach-O image (main exe + dylibs),
 *                      used to CLASSIFY a PC as Mach-O vs ELF.
 *      * g_maps[]/g_mods[] — executable /proc/self/maps ranges, used to label
 *                      ELF frames as `lib.so+0xoff`.
 *      * a self-pipe — the readability probe (write() returns EFAULT instead of
 *                      faulting on an unmapped source buffer).
 *  - The handler (crash_sigaction) is reached from a signal context, possibly on
 *    a corrupt heap or a wedged thread, so it uses ONLY async-signal-safe
 *    primitives: write/read/sigaltstack/sigaction/raise/sigprocmask/syscall and
 *    pure reads over the frozen tables + the loader's frozen symbol index. No
 *    malloc, no stdio, no dladdr, no __cxa_demangle.
 *  - Mach-O symbols are printed MANGLED (normalized `__Z`->`_Z` so c++filt reads
 *    them directly). Demangling in-handler would call malloc and risk a deadlock
 *    on a heap-corruption crash.
 *  - On exit the handler restores the default disposition and re-raises so the
 *    process dies with the correct signal (and can core-dump).
 */

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>

#include "macho_defs.h"
#include "loader.h"
#include "dylib_loader.h"
#include "heaptrack_trace.h"
#include "trampoline.h"
#include "crash_handler.h"

/* The main executable's load result (defined in machismo.c). */
extern struct load_results machismo_load_results;

/* ----- lookup tables (built at init, read-only in the handler) ----- */

struct macho_range {
	uintptr_t text_base;
	uintptr_t text_end;   /* text_base + __TEXT vmsize */
	const char *name;     /* basename; stable for the run */
};
#define CRASH_MAX_IMAGES (MAX_MACHO_DYLIBS + 1)
static struct macho_range g_images[CRASH_MAX_IMAGES];
static int g_num_images = 0;

struct elf_mod { const char *name; uintptr_t base; };   /* base = lowest mapping */
struct elf_range { uintptr_t lo, hi; int mod; };
#define CRASH_MAX_MODS  512
#define CRASH_MAX_MAPS  1024
static struct elf_mod   g_mods[CRASH_MAX_MODS];
static int              g_num_mods = 0;
static struct elf_range g_maps[CRASH_MAX_MAPS];
static int              g_num_maps = 0;
static char             g_strs[32 * 1024];
static size_t           g_strs_used = 0;

/* Self-pipe readability probe. */
static int g_probe_rd = -1, g_probe_wr = -1;

/* Per-thread alternate signal stack (auto-freed with the thread's TLS). 64KB is
 * comfortably above MINSIGSTKSZ on the targets (no SVE) and big enough for the
 * handler's small footprint. */
#define CRASH_ALTSTACK_SIZE (64 * 1024)
static __thread char t_altstack[CRASH_ALTSTACK_SIZE] __attribute__((aligned(16)));

static volatile sig_atomic_t g_in_handler = 0;
static int g_installed = 0;

static const int g_fatal_sigs[] = {
	SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT
};
#define NUM_FATAL_SIGS (sizeof(g_fatal_sigs) / sizeof(g_fatal_sigs[0]))
static struct sigaction g_prev[NUM_FATAL_SIGS];

/* ======================= async-signal-safe output ======================= */

static void wr(const char *s, size_t n)
{
	while (n) {
		ssize_t k = write(STDERR_FILENO, s, n);
		if (k < 0) { if (errno == EINTR) continue; break; }
		if (k == 0) break;
		s += k; n -= (size_t)k;
	}
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void wrs(const char *s) { if (s) wr(s, slen(s)); }
static void wrc(char c) { wr(&c, 1); }

static void wrhex(uint64_t v)
{
	static const char hd[] = "0123456789abcdef";
	char buf[16];
	int i = 16;
	if (!v) buf[--i] = '0';
	else while (v) { buf[--i] = hd[v & 0xf]; v >>= 4; }
	wr("0x", 2);
	wr(buf + i, (size_t)(16 - i));
}

static void wrdec(long v)
{
	char buf[24];
	int i = 24;
	int neg = v < 0;
	unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
	if (!u) buf[--i] = '0';
	else while (u) { buf[--i] = (char)('0' + (u % 10)); u /= 10; }
	if (neg) buf[--i] = '-';
	wr(buf + i, (size_t)(24 - i));
}

/* ======================= readability probe ======================= */

/* Returns 1 if [p, p+n) is readable, 0 if not. Uses a non-blocking self-pipe:
 * write() validates the source buffer in-kernel and returns EFAULT rather than
 * faulting. Fully async-signal-safe. */
static int readable(const void *p, size_t n)
{
	char drain[256];
	if (g_probe_wr < 0) return 1;            /* probe unavailable: best effort */
	while (read(g_probe_rd, drain, sizeof drain) > 0) { }   /* keep pipe empty */
	for (;;) {
		ssize_t k = write(g_probe_wr, p, n);
		if (k >= 0) {
			while (read(g_probe_rd, drain, sizeof drain) > 0) { }
			return 1;
		}
		if (errno == EINTR) continue;
		if (errno == EFAULT) return 0;
		return 1;   /* EAGAIN (shouldn't happen post-drain) / other: assume ok */
	}
}

/* ======================= symbolication ======================= */

static int in_any_text(uint64_t ip)
{
	for (int i = 0; i < g_num_images; i++)
		if (ip >= g_images[i].text_base && ip < g_images[i].text_end) return 1;
	for (int i = 0; i < g_num_maps; i++)
		if (ip >= g_maps[i].lo && ip < g_maps[i].hi) return 1;
	return 0;
}

static int macho_image_of(uint64_t ip)
{
	for (int i = 0; i < g_num_images; i++)
		if (ip >= g_images[i].text_base && ip < g_images[i].text_end) return i;
	return -1;
}

/* Emit "  #N <tag> 0xIP  module`symbol+0xoff". */
static void print_frame(int idx, uint64_t ip, const char *tag)
{
	wrs("  #");
	if (idx < 10) wrc('0');
	wrdec(idx);
	wrc(' ');
	wrs(tag);                /* " (pc) " / " (lr) " / "      " */
	wrhex(ip);
	wrs("  ");

	int mi = macho_image_of(ip);
	if (mi >= 0) {
		const char *fn = NULL, *mod = NULL;
		uint64_t base = 0;
		if (ht_lookup_sym(ip, &fn, &mod, &base)) {
			/* Mach-O names carry a leading underscore; C++ names then read
			 * `__Z…`. Drop ONE underscore so the token is a valid `_Z…` for
			 * offline c++filt. */
			const char *p = fn;
			if (p[0] == '_' && p[1] == '_' && p[2] == 'Z') p = fn + 1;
			wrs(mod ? mod : g_images[mi].name);
			wrc('`');
			wrs(p);
			wrc('+');
			wrhex(ip - base);
		} else {
			wrs(g_images[mi].name);
			wrc('+');
			wrhex(ip - g_images[mi].text_base);
		}
	} else {
		int found = 0;
		for (int i = 0; i < g_num_maps; i++) {
			if (ip >= g_maps[i].lo && ip < g_maps[i].hi) {
				const struct elf_mod *m = &g_mods[g_maps[i].mod];
				wrs(m->name);
				wrc('+');
				wrhex(ip - m->base);
				found = 1;
				break;
			}
		}
		if (!found) wrs("??");
	}
	wrc('\n');
}

/* ======================= frame walk ======================= */

#define CRASH_MAX_DEPTH 64

static void walk_stack(uint64_t pc, uint64_t fp, uint64_t lr)
{
	int idx = 0;

	/* Frame 0 is always the faulting PC. */
	print_frame(idx++, pc, " (pc) ");

	/* The first return slot the fp-walk will emit (so we don't duplicate it via
	 * lr). 16-byte alignment is mandatory for x29 on arm64. */
	uint64_t fp_lr = 0;
	int fp_ok = fp && !(fp & 0xf) && readable((void *)(uintptr_t)fp, 16);
	if (fp_ok) fp_lr = ((uint64_t *)(uintptr_t)fp)[1];

	/* Emit lr when it adds a frame the fp-walk would otherwise miss — chiefly a
	 * fault inside a frameless leaf (e.g. memcpy with a bad pointer), where fp
	 * is still the caller's frame and lr holds the real return address into the
	 * immediate caller. Tagged "(lr)" because in a settled frame it can be a
	 * within-function artifact. Skip it when it equals the fp-walk's first slot
	 * (the common settled case) to avoid an exact duplicate. */
	if (lr && lr != pc && lr != fp_lr && in_any_text(lr))
		print_frame(idx++, lr, " (lr) ");

	uint64_t cur = fp;
	for (int d = 0; d < CRASH_MAX_DEPTH && cur; d++) {
		if (cur & 0xf) break;                                 /* x29 alignment */
		if (!readable((void *)(uintptr_t)cur, 16)) break;
		uint64_t ret  = ((uint64_t *)(uintptr_t)cur)[1];      /* saved lr  */
		uint64_t next = ((uint64_t *)(uintptr_t)cur)[0];      /* saved fp  */
		if (!ret) break;
		/* A return address is always executable code. If it isn't, the chain
		 * has ended (the terminator frame) or the stack is corrupt — stop here
		 * rather than chase garbage into uninitialized stack. */
		if (!in_any_text(ret)) break;
		print_frame(idx++, ret, "      ");
		if (next <= cur) break;                               /* must ascend */
		if (next - cur > (64u << 20)) break;                  /* sanity cap  */
		cur = next;
	}
}

/* ======================= register dump ======================= */

static void dump_regs(const ucontext_t *uc)
{
	const unsigned long long *r = (const unsigned long long *)uc->uc_mcontext.regs;
	wrs("regs:\n");
	for (int i = 0; i <= 30; i += 2) {
		wrs("  x");
		wrdec(i);
		wrs(i < 10 ? " = " : "= ");
		wrhex(r[i]);
		wrs("   x");
		wrdec(i + 1);
		wrs((i + 1) < 10 ? " = " : "= ");
		wrhex(r[i + 1]);
		wrc('\n');
	}
	wrs("  sp = ");     wrhex(uc->uc_mcontext.sp);
	wrs("   pc = ");    wrhex(uc->uc_mcontext.pc);
	wrs("   pstate = "); wrhex(uc->uc_mcontext.pstate);
	wrc('\n');
}

/* ======================= signal naming ======================= */

static const char *signame(int s)
{
	switch (s) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGILL:  return "SIGILL";
	case SIGFPE:  return "SIGFPE";
	case SIGTRAP: return "SIGTRAP";
	case SIGABRT: return "SIGABRT";
	default:      return "signal";
	}
}

static const char *codename(int s, int c)
{
	if (s == SIGSEGV) {
		if (c == SEGV_MAPERR) return "SEGV_MAPERR";
		if (c == SEGV_ACCERR) return "SEGV_ACCERR";
	} else if (s == SIGBUS) {
		if (c == BUS_ADRALN) return "BUS_ADRALN";
		if (c == BUS_ADRERR) return "BUS_ADRERR";
		if (c == BUS_OBJERR) return "BUS_OBJERR";
	} else if (s == SIGILL) {
		if (c == ILL_ILLOPC) return "ILL_ILLOPC";
		if (c == ILL_ILLOPN) return "ILL_ILLOPN";
		if (c == ILL_PRVOPC) return "ILL_PRVOPC";
	} else if (s == SIGFPE) {
		if (c == FPE_INTDIV) return "FPE_INTDIV";
		if (c == FPE_FLTDIV) return "FPE_FLTDIV";
	}
	return NULL;
}

/* ======================= the handler ======================= */

static void crash_sigaction(int sig, siginfo_t *info, void *ucv)
{
	if (g_in_handler) {                 /* nested fault: die immediately */
		signal(sig, SIG_DFL);
		raise(sig);
		_exit(128 + sig);
	}
	g_in_handler = 1;

	const ucontext_t *uc = (const ucontext_t *)ucv;
	uint64_t pc    = uc->uc_mcontext.pc;
	uint64_t fp    = uc->uc_mcontext.regs[29];
	uint64_t lr    = uc->uc_mcontext.regs[30];
	uint64_t fault = info ? (uint64_t)(uintptr_t)info->si_addr : 0;
	int code = info ? info->si_code : 0;

	wrs("\n======== machismo crash handler ========\n");
	wrs("signal: ");
	wrs(signame(sig));
	wrs(" (");
	wrdec(sig);
	wrs(")   code: ");
	wrdec(code);
	{
		const char *cn = codename(sig, code);
		if (cn) { wrs(" ("); wrs(cn); wrc(')'); }
	}
	if (sig == SIGSEGV || sig == SIGBUS) {
		wrs("   fault addr: ");
		wrhex(fault);
	}
	wrc('\n');
	wrs("tid: ");
	wrdec((long)syscall(SYS_gettid));
	wrc('\n');

	if (sig == SIGSEGV && trampoline_is_guarded_fault((uintptr_t)fault))
		wrs("note: fault is in a guarded __DATA page — un-trampolined or inlined "
		    "code is touching Mach-O library state instead of the native .so.\n");

	dump_regs(uc);

	wrs("backtrace (innermost first):\n");
	walk_stack(pc, fp, lr);

	wrs("note: Mach-O symbols are mangled; pipe this log through c++filt to "
	    "demangle.\n");
	wrs("=========================================\n");

	/* Restore the default disposition, unblock, and re-raise so the process
	 * dies with the correct signal (and can core-dump) regardless of whether
	 * the signal came from a hardware fault or kill(). */
	signal(sig, SIG_DFL);
	sigset_t m;
	sigemptyset(&m);
	sigaddset(&m, sig);
	pthread_sigmask(SIG_UNBLOCK, &m, NULL);
	raise(sig);
	_exit(128 + sig);
}

/* ======================= init helpers ======================= */

static int macho_text_range(struct mach_header_64 *mh, uintptr_t slide,
                            uintptr_t *base, uintptr_t *end)
{
	if (!mh) return 0;
	uint8_t *cmds = (uint8_t *)(mh + 1);
	uint32_t p = 0;
	for (uint32_t i = 0; i < mh->ncmds && p < mh->sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)&cmds[p];
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *seg = (struct segment_command_64 *)lc;
			if (strcmp(seg->segname, "__TEXT") == 0) {
				*base = (uintptr_t)(seg->vmaddr + slide);
				*end  = *base + (uintptr_t)seg->vmsize;
				return 1;
			}
		}
		p += lc->cmdsize;
	}
	return 0;
}

static void build_images(void)
{
	uintptr_t b, e;
	struct mach_header_64 *mh = (struct mach_header_64 *)machismo_load_results.mh;
	if (macho_text_range(mh, machismo_load_results.slide, &b, &e)) {
		g_images[g_num_images].text_base = b;
		g_images[g_num_images].text_end  = e;
		g_images[g_num_images].name      = "macho-main";
		g_num_images++;
	}
	for (int i = 0; i < g_num_macho_dylibs && g_num_images < CRASH_MAX_IMAGES; i++) {
		uintptr_t tb = g_macho_dylibs[i].text_base;
		uintptr_t te = tb + g_macho_dylibs[i].text_size;
		const char *bn = strrchr(g_macho_dylibs[i].path, '/');
		bn = bn ? bn + 1 : g_macho_dylibs[i].path;
		g_images[g_num_images].text_base = tb;
		g_images[g_num_images].text_end  = te;
		g_images[g_num_images].name      = bn;   /* path[] is stable for the run */
		g_num_images++;
	}
}

static const char *intern(const char *s)
{
	size_t L = strlen(s) + 1;
	if (g_strs_used + L > sizeof g_strs) return NULL;
	char *dst = g_strs + g_strs_used;
	memcpy(dst, s, L);
	g_strs_used += L;
	return dst;
}

static int find_or_add_mod(const char *name, uintptr_t start)
{
	for (int i = 0; i < g_num_mods; i++)
		if (strcmp(g_mods[i].name, name) == 0) return i;
	if (g_num_mods >= CRASH_MAX_MODS) return -1;
	const char *nm = intern(name);
	if (!nm) return -1;
	g_mods[g_num_mods].name = nm;
	g_mods[g_num_mods].base = start;   /* maps are address-sorted: first = lowest */
	return g_num_mods++;
}

/* Parse /proc/self/maps once at init (ordinary libc is fine here) into the ELF
 * range/module tables used to label native frames. */
static void parse_maps(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, f)) > 0) {
		unsigned long lo, hi, off;
		char perms[8] = {0};
		if (sscanf(line, "%lx-%lx %7s %lx", &lo, &hi, perms, &off) != 4)
			continue;
		/* pathname is the first '/' or '[' on the line, else anonymous. */
		char *slash = strchr(line, '/');
		char *brack = strchr(line, '[');
		char *pp = (slash && (!brack || slash < brack)) ? slash : brack;
		if (!pp) {
			/* Anonymous mapping. Record it only if executable (JIT code, e.g.
			 * LuaJIT) so frames returning into it are recognized as code and
			 * the walk isn't truncated. Each region is independent — own base,
			 * no dedup. */
			if (perms[2] == 'x' && g_num_maps < CRASH_MAX_MAPS &&
			    g_num_mods < CRASH_MAX_MODS) {
				g_mods[g_num_mods].name = "[anon]";
				g_mods[g_num_mods].base = (uintptr_t)lo;
				g_maps[g_num_maps].lo  = (uintptr_t)lo;
				g_maps[g_num_maps].hi  = (uintptr_t)hi;
				g_maps[g_num_maps].mod = g_num_mods;
				g_num_mods++;
				g_num_maps++;
			}
			continue;
		}
		size_t L = strlen(pp);
		while (L && (pp[L - 1] == '\n' || pp[L - 1] == '\r')) pp[--L] = 0;
		char *bn = strrchr(pp, '/');
		bn = bn ? bn + 1 : pp;
		int mi = find_or_add_mod(bn, (uintptr_t)lo);
		if (mi < 0) continue;
		if (perms[2] == 'x' && g_num_maps < CRASH_MAX_MAPS) {
			g_maps[g_num_maps].lo  = (uintptr_t)lo;
			g_maps[g_num_maps].hi  = (uintptr_t)hi;
			g_maps[g_num_maps].mod = mi;
			g_num_maps++;
		}
	}
	free(line);
	fclose(f);
}

/* ======================= public API ======================= */

void crash_handler_register_thread(void)
{
	stack_t ss;
	ss.ss_sp    = t_altstack;
	ss.ss_size  = sizeof t_altstack;
	ss.ss_flags = 0;
	sigaltstack(&ss, NULL);
}

void crash_handler_init(void)
{
	if (g_installed) return;

	build_images();
	parse_maps();

	int fds[2];
	if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0) {
		g_probe_rd = fds[0];
		g_probe_wr = fds[1];
	}

	crash_handler_register_thread();   /* main thread's alt-stack */

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = crash_sigaction;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	for (size_t i = 0; i < NUM_FATAL_SIGS; i++)
		sigaddset(&sa.sa_mask, g_fatal_sigs[i]);   /* no interleaved invocations */
	for (size_t i = 0; i < NUM_FATAL_SIGS; i++)
		sigaction(g_fatal_sigs[i], &sa, &g_prev[i]);

	g_installed = 1;
	fprintf(stderr, "machismo: crash handler installed "
	        "(SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP/SIGABRT; %d Mach-O image(s), "
	        "%d ELF code range(s))\n", g_num_images, g_num_maps);
}
