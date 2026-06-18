/*
 * fps.c — env-gated frame-rate reporter shared across ports. See fps.h.
 *
 * Kept dependency-free (just libc + CLOCK_MONOTONIC) so any present path can
 * call it, and cheap-to-skip when MACHISMO_FPS is unset.
 */
#include "fps.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void machismo_fps_tick(void)
{
	/* enabled: -1 = not yet checked, 0 = off, 1 = on. */
	static int      enabled  = -1;
	static double   interval = 1.0;   /* seconds between reports */
	static unsigned frames   = 0;     /* frames since last report */
	static double   t_last   = 0.0;   /* monotonic seconds at last report (0 = unseeded) */

	if (enabled < 0) {
		const char *v = getenv("MACHISMO_FPS");
		if (!v || !*v) { enabled = 0; return; }
		enabled = 1;
		double iv = atof(v);          /* value as interval seconds, if numeric */
		if (iv > 0.0)
			interval = iv;
	}
	if (!enabled)
		return;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;

	/* Seed the window on the first tick; don't count the partial interval. */
	if (t_last == 0.0) {
		t_last = now;
		frames = 0;
		return;
	}

	frames++;
	double dt = now - t_last;
	if (dt >= interval && frames > 0) {
		double fps = (double)frames / dt;
		fprintf(stderr, "[machismo fps] %.1f fps  (%.2f ms/frame, %u frames / %.2fs)\n",
		        fps, 1000.0 * dt / (double)frames, frames, dt);
		frames = 0;
		t_last = now;
	}
}
