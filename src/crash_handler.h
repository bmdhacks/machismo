#ifndef _MACHISMO_CRASH_HANDLER_H_
#define _MACHISMO_CRASH_HANDLER_H_

/*
 * Always-on fatal-signal crash handler.
 *
 * machismo runs an Apple-arm64 Mach-O binary natively alongside native ELF
 * .so's, so a crash's call stack spans two worlds: ELF frames the Linux dynamic
 * linker knows about, and Mach-O game frames it does not. This handler walks the
 * x29 frame-pointer chain (guaranteed by -fno-omit-frame-pointer + the Apple
 * ABI) from the faulting context and symbolicates BOTH worlds — Mach-O frames
 * via the loader's own reverse symbol index (ht_lookup_sym), ELF frames via a
 * /proc/self/maps range table — printing a hydrated backtrace to stderr.
 *
 * The handler body is strictly async-signal-safe: only raw syscalls (write,
 * read, raise, sigaction, ...) and stack-local buffers. No malloc, no stdio, no
 * dladdr, no __cxa_demangle (symbols are emitted mangled; demangle offline with
 * c++filt). See crash_handler.c for the full rationale.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Install the handler for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP/SIGABRT. Call
 * once from machismo main, after all Mach-O images + the trampoline __DATA guard
 * pages exist and before transferring control to the guest. Builds the image /
 * maps lookup tables, the readability-probe pipe, and the main thread's
 * alternate signal stack, then installs the handlers. */
void crash_handler_init(void);

/* Install an alternate signal stack for the calling thread so a stack-overflow
 * fault can still run the handler. Call from each guest worker thread's entry.
 * The signal disposition is process-wide and already covers every thread for
 * ordinary (non-overflow) faults; this only adds overflow resilience. */
void crash_handler_register_thread(void);

#ifdef __cplusplus
}
#endif

#endif /* _MACHISMO_CRASH_HANDLER_H_ */
