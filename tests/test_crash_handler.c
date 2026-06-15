/*
 * test_crash_handler — exercises the crash handler end-to-end without the loader
 * or a display. A child process installs the handler, faults through a known
 * call chain, and the parent asserts that a hydrated backtrace is printed, the
 * frame chain is walked, the right symbolication branch is taken, and the
 * process re-raises and dies with the correct signal.
 *
 * Three scenarios:
 *   - SIGSEGV (ELF):   null deref; frames labeled via /proc/self/maps ranges.
 *   - SIGSEGV (Mach-O):a synthetic main image + ht_lookup_sym stub; verifies the
 *                      Mach-O branch: classification, `__Z`->`_Z` normalization,
 *                      and the `module`symbol+0xoff` format.
 *   - SIGABRT:         abort(); verifies the handler fires + re-raises SIGABRT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include "crash_handler.h"
#include "loader.h"
#include "dylib_loader.h"
#include "macho_defs.h"

/* ---- Stubs for symbols crash_handler.c imports from the rest of machismo ---- */

struct load_results machismo_load_results = {0};
struct macho_dylib_info g_macho_dylibs[MAX_MACHO_DYLIBS];
int g_num_macho_dylibs = 0;

/* Synthetic Mach-O image used by scenario "macho". */
static int g_fake_macho = 0;
static uintptr_t g_fake_base = 0;
#define FAKE_TEXT_SIZE 0x200000

int ht_lookup_sym(uint64_t ip, const char **fn, const char **mod, uint64_t *base)
{
	if (!g_fake_macho) return 0;
	if (ip < g_fake_base || ip >= g_fake_base + FAKE_TEXT_SIZE) return 0;
	*fn  = "__ZN8MachoSym4testEv";   /* C++ name with Mach-O double underscore */
	*mod = "fake-macho.dylib";
	if (base) *base = g_fake_base;   /* offset printed = ip - base */
	return 1;
}

int trampoline_is_guarded_fault(uintptr_t addr) { (void)addr; return 0; }

/* ---- Crash payloads ---- */

static volatile int *g_null = 0;

__attribute__((noinline)) static void level3(void) { *g_null = 42; /* SIGSEGV */ }
__attribute__((noinline)) static void level2(void) { level3(); asm volatile(""); }
__attribute__((noinline)) static void level1(void) { level2(); asm volatile(""); }

/* Build a one-segment (__TEXT) Mach-O image covering the test's code so the
 * handler classifies the faulting frames as Mach-O and symbolicates them via
 * the ht_lookup_sym stub. */
static struct { struct mach_header_64 mh; struct segment_command_64 text; } g_fake;

static void setup_fake_macho(void)
{
	uintptr_t a = (uintptr_t)(void *)&level3;
	g_fake.mh.ncmds = 1;
	g_fake.mh.sizeofcmds = sizeof(struct segment_command_64);
	g_fake.text.cmd = LC_SEGMENT_64;
	g_fake.text.cmdsize = sizeof(struct segment_command_64);
	strcpy(g_fake.text.segname, "__TEXT");
	g_fake.text.vmaddr = a & ~(uintptr_t)0xFFFFF;   /* 1MB-align down */
	g_fake.text.vmsize = FAKE_TEXT_SIZE;
	machismo_load_results.mh = (unsigned long)(void *)&g_fake;
	machismo_load_results.slide = 0;
	g_fake_base = (uintptr_t)g_fake.text.vmaddr;
	g_fake_macho = 1;
}

enum { MODE_SEGV_ELF, MODE_SEGV_MACHO, MODE_ABORT };

static int run_crash_child(int mode, char *out, size_t outsz)
{
	int pfd[2];
	if (pipe(pfd) != 0) { perror("pipe"); exit(2); }

	pid_t pid = fork();
	if (pid == 0) {
		struct rlimit z = { 0, 0 };
		setrlimit(RLIMIT_CORE, &z);
		dup2(pfd[1], STDERR_FILENO);
		close(pfd[0]);
		close(pfd[1]);
		if (mode == MODE_SEGV_MACHO) setup_fake_macho();
		crash_handler_init();
		if (mode == MODE_ABORT) abort();
		else level1();
		_exit(0);   /* unreachable */
	}

	close(pfd[1]);
	size_t n = 0;
	ssize_t k;
	while (n < outsz - 1 && (k = read(pfd[0], out + n, outsz - 1 - n)) > 0)
		n += (size_t)k;
	out[n] = 0;
	close(pfd[0]);

	int st = 0;
	waitpid(pid, &st, 0);
	return st;
}

static void check(const char *what, int cond, int *fails)
{
	if (!cond) { printf("FAIL: %s\n", what); (*fails)++; }
}

int main(void)
{
	static char buf[1 << 16];
	int fails = 0;

	printf("=== scenario 1: SIGSEGV, ELF frames ===\n");
	int st = run_crash_child(MODE_SEGV_ELF, buf, sizeof buf);
	fputs(buf, stdout);
	check("child killed by SIGSEGV",
	      WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, &fails);
	check("crash header present", strstr(buf, "machismo crash handler") != NULL, &fails);
	check("signal name SIGSEGV present", strstr(buf, "SIGSEGV") != NULL, &fails);
	check("register dump present", strstr(buf, "regs:") != NULL, &fails);
	check("backtrace present", strstr(buf, "backtrace") != NULL, &fails);
	check("frame #00 present", strstr(buf, "#00") != NULL, &fails);
	check("multiple frames unwound (#03)", strstr(buf, "#03") != NULL, &fails);
	check("ELF module labeled", strstr(buf, "test_crash_handler") != NULL, &fails);

	printf("\n=== scenario 2: SIGSEGV, Mach-O frames (synthetic image) ===\n");
	st = run_crash_child(MODE_SEGV_MACHO, buf, sizeof buf);
	fputs(buf, stdout);
	check("child killed by SIGSEGV",
	      WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, &fails);
	check("Mach-O image counted at init", strstr(buf, "1 Mach-O image") != NULL, &fails);
	check("Mach-O module`symbol printed",
	      strstr(buf, "fake-macho.dylib`_ZN8MachoSym4testEv") != NULL, &fails);
	check("__Z normalized to single-underscore _Z (c++filt-ready)",
	      strstr(buf, "`__ZN8MachoSym4testEv") == NULL, &fails);

	printf("\n=== scenario 3: SIGABRT ===\n");
	st = run_crash_child(MODE_ABORT, buf, sizeof buf);
	fputs(buf, stdout);
	check("child killed by SIGABRT",
	      WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, &fails);
	check("signal name SIGABRT present", strstr(buf, "SIGABRT") != NULL, &fails);

	printf("\nRESULT: %s (%d failure%s)\n",
	       fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
