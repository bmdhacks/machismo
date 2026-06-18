#ifndef MACHISMO_FPS_H
#define MACHISMO_FPS_H

/*
 * machismo_fps_tick — env-gated frame-rate reporter.
 *
 * Call exactly once per *displayed* frame from each port's present path. It is
 * disabled (and effectively free — one relaxed load after the first call)
 * unless MACHISMO_FPS is set in the environment. When the value parses to a
 * positive number it is used as the report interval in seconds (default 1.0);
 * e.g. MACHISMO_FPS=2 reports every two seconds, MACHISMO_FPS=1 (or any
 * non-numeric value) every second. Reports go to stderr.
 *
 * The internal accumulators are unsynchronised: a given game has a single
 * present path driven from a single thread, so this is called serially. (Some
 * ports may tick it once more from a shutdown handler on another thread — a
 * benign one-off as the process tears down.)
 *
 * Exported from the -rdynamic loader so shims that own their own present path
 * (e.g. the Gothic renderer) can resolve it with dlsym(RTLD_DEFAULT, ...).
 */
#ifdef __cplusplus
extern "C" {
#endif

void machismo_fps_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* MACHISMO_FPS_H */
