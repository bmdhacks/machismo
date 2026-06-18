/*
 * splash_kms.h — early in-process KMS load splash.
 *
 * machismo's load (LSE patching, chained-fixup resolve, then the game's own
 * asset / shader / audio loading) can take several seconds on a low-power
 * handheld before the game's renderer presents its first frame — a black screen
 * the whole time. machismo_splash_show() puts the port's splash image on the
 * panel the moment machismo starts and keeps it there until the game's first
 * real frame replaces it. See splash_kms.c for the (deliberately lightweight,
 * GL/VK-free) mechanism and the DRM-master handoff that lets SDL/Vulkan take the
 * display over without a fight.
 *
 * Both functions are fail-soft and safe to call when no KMS display is present
 * (e.g. a desktop dev host); the splash is simply skipped.
 */
#ifndef MACHISMO_SPLASH_KMS_H
#define MACHISMO_SPLASH_KMS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Modeset the PNG at png_path onto the primary connected display, then drop the
 * DRM master while leaving the image scanning out. Returns 0 if the splash is up,
 * <0 if it was skipped or failed (non-fatal — the caller proceeds regardless). */
int machismo_splash_show(const char *png_path);

/* Free the splash's leftover dumb framebuffer and close the DRM fd. Call once the
 * game's own framebuffer owns the CRTC (a few frames after the first present);
 * idempotent and a no-op if no splash was shown. Also released by the kernel at
 * process exit, so this is a tidiness call, not a correctness requirement. */
void machismo_splash_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* MACHISMO_SPLASH_KMS_H */
