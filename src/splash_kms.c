/*
 * splash_kms.c — early in-process KMS load splash. See splash_kms.h.
 *
 * Why this exists and how it hands the display off cleanly:
 *
 * The slow part of launch (LSE patch, fixup resolve, game asset/shader/audio
 * load) happens long before the game's render backend is even chosen and SDL is
 * initialized, so we can't reuse the real GL/VK context to show feedback — we'd
 * have nothing to show until the very moment the black screen ends. Instead this
 * does a raw KMS dumb-buffer blit: decode the PNG, scale-to-fit with letterbox
 * bars, modeset it onto the panel, then IMMEDIATELY drmDropMaster while leaving
 * our framebuffer bound as the CRTC scanout.
 *
 * A CRTC keeps scanning out its last-set framebuffer regardless of who holds the
 * DRM master, so after the drop the splash stays on screen with NO process master
 * — which is exactly what lets the game take the display over without a fight:
 *   - GLES: SDL's KMSDRM driver grabs the master and modesets its own buffer on
 *     the first eglSwapBuffers (deferred modeset), replacing the splash with the
 *     first real frame — no black gap.
 *   - Vulkan: the libmali ICD presents only via VK_KHR_display direct mode and
 *     must own the display; with the master free it enumerates the panel and
 *     acquires it. (This retires the old cross-process splash-kill workaround.)
 *
 * libdrm is resolved at RUNTIME via dlopen, never at link time: machismo is built
 * on a host that has libdrm but is deployed to handhelds that may not ship
 * libdrm.so.2. A NEEDED dependency would make the dynamic loader reject machismo
 * ("error while loading shared libraries: libdrm.so.2") before any of its code —
 * including this file's own fail-soft path — gets to run, bricking launch on
 * those devices. With dlopen, an absent libdrm just no-ops the splash.
 *
 * Fail-soft throughout: any failure logs and returns <0; the game proceeds with
 * the old black screen. Skipped under X11/Wayland (a desktop dev host, where the
 * compositor owns the display and there is nothing to splash over).
 */

#ifdef HAVE_LIBDRM

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* Vendored single-header decoders (bimg's stb). Kept file-local: no other TU in
 * the machismo exe pulls these implementations (verified), and STATIC avoids
 * exporting stbi_* through machismo's -rdynamic dynamic symbol table. */
#define STBI_STATIC
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../extern/bimg/3rdparty/stb/stb_image.h"
#define STBIR_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../extern/bimg/3rdparty/stb/stb_image_resize.h"

#include "splash_kms.h"

/* --- libdrm resolved at runtime (see file header) ------------------------- *
 * Only the struct-returning/allocating helpers are dlsym'd; the trivial ioctl
 * wrappers (drmIoctl/drmSetMaster/drmDropMaster) are reimplemented inline below
 * so they never depend on the library being present beyond /dev/dri itself. */
typedef drmModeResPtr       (*pfn_GetResources)(int);
typedef void                (*pfn_FreeResources)(drmModeResPtr);
typedef drmModeConnectorPtr (*pfn_GetConnector)(int, uint32_t);
typedef void                (*pfn_FreeConnector)(drmModeConnectorPtr);
typedef drmModeEncoderPtr   (*pfn_GetEncoder)(int, uint32_t);
typedef void                (*pfn_FreeEncoder)(drmModeEncoderPtr);
typedef int                 (*pfn_AddFB)(int, uint32_t, uint32_t, uint8_t, uint8_t,
                                         uint32_t, uint32_t, uint32_t *);
typedef int                 (*pfn_RmFB)(int, uint32_t);
typedef int                 (*pfn_SetCrtc)(int, uint32_t, uint32_t, uint32_t, uint32_t,
                                           uint32_t *, int, drmModeModeInfoPtr);

static struct {
	void             *lib;
	pfn_GetResources  GetResources;
	pfn_FreeResources FreeResources;
	pfn_GetConnector  GetConnector;
	pfn_FreeConnector FreeConnector;
	pfn_GetEncoder    GetEncoder;
	pfn_FreeEncoder   FreeEncoder;
	pfn_AddFB         AddFB;
	pfn_RmFB          RmFB;
	pfn_SetCrtc       SetCrtc;
} drm;

/* dlopen libdrm.so.2 and bind the helpers. Returns 0 (and leaves the splash a
 * no-op) when the library or any expected symbol is absent. Idempotent. */
static int splash_drm_load(void)
{
	if (drm.lib)
		return 1;
	void *h = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "machismo: splash: libdrm.so.2 unavailable (%s) — no splash\n",
		        dlerror() ? dlerror() : "");
		return 0;
	}
	drm.GetResources  = (pfn_GetResources)  dlsym(h, "drmModeGetResources");
	drm.FreeResources = (pfn_FreeResources) dlsym(h, "drmModeFreeResources");
	drm.GetConnector  = (pfn_GetConnector)  dlsym(h, "drmModeGetConnector");
	drm.FreeConnector = (pfn_FreeConnector) dlsym(h, "drmModeFreeConnector");
	drm.GetEncoder    = (pfn_GetEncoder)    dlsym(h, "drmModeGetEncoder");
	drm.FreeEncoder   = (pfn_FreeEncoder)   dlsym(h, "drmModeFreeEncoder");
	drm.AddFB         = (pfn_AddFB)         dlsym(h, "drmModeAddFB");
	drm.RmFB          = (pfn_RmFB)          dlsym(h, "drmModeRmFB");
	drm.SetCrtc       = (pfn_SetCrtc)       dlsym(h, "drmModeSetCrtc");
	if (!drm.GetResources || !drm.FreeResources || !drm.GetConnector ||
	    !drm.FreeConnector || !drm.GetEncoder || !drm.FreeEncoder ||
	    !drm.AddFB || !drm.RmFB || !drm.SetCrtc) {
		fprintf(stderr, "machismo: splash: libdrm.so.2 missing symbols — no splash\n");
		dlclose(h);
		memset(&drm, 0, sizeof drm);
		return 0;
	}
	drm.lib = h;
	return 1;
}

/* libdrm's drmIoctl is just an EINTR/EAGAIN retry loop around ioctl(2); inline
 * it (and the two _IO master toggles) so they stay off the dlopen path. */
static int splash_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;
	do { ret = ioctl(fd, request, arg); } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret;
}
static int splash_set_master(int fd)  { return splash_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL); }
static int splash_drop_master(int fd) { return splash_ioctl(fd, DRM_IOCTL_DROP_MASTER, NULL); }

/* Live splash state, kept for teardown. fd < 0 means "nothing shown". */
static struct {
	int      fd;
	uint32_t fb_id;
	uint32_t dumb_handle;
	void    *map;
	size_t   map_size;
} g_splash = { .fd = -1 };

int machismo_splash_show(const char *png_path)
{
	/* Desktop dev host: a compositor owns the display, nothing to splash over. */
	if (getenv("WAYLAND_DISPLAY") || getenv("DISPLAY"))
		return -1;
	if (!png_path || !*png_path)
		return -1;
	/* Resolve libdrm before touching anything; no-op the splash if it's absent. */
	if (!splash_drm_load())
		return -1;

	/* Cleanup state — declared up front so the single error path can unwind
	 * whatever has been acquired so far. */
	int                fd      = -1;
	drmModeRes        *res     = NULL;
	drmModeConnector  *conn    = NULL;
	unsigned char     *img     = NULL;
	unsigned char     *scaled  = NULL;
	void              *map     = MAP_FAILED;
	uint32_t           fb_id   = 0;
	struct drm_mode_create_dumb creq;
	memset(&creq, 0, sizeof creq);   /* creq.handle stays 0 until CREATE_DUMB */

	/* Decode the PNG as RGBA8. */
	int iw = 0, ih = 0, ic = 0;
	img = stbi_load(png_path, &iw, &ih, &ic, 4);
	if (!img) {
		fprintf(stderr, "machismo: splash: cannot decode %s: %s\n",
		        png_path, stbi_failure_reason());
		return -1;
	}

	/* Find a DRM card with a connected output. */
	for (int c = 0; c < 8 && fd < 0; c++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", c);
		int f = open(path, O_RDWR | O_CLOEXEC);
		if (f < 0)
			continue;
		drmModeRes *r = drm.GetResources(f);
		if (!r) { close(f); continue; }
		drmModeConnector *cc = NULL;
		for (int i = 0; i < r->count_connectors; i++) {
			drmModeConnector *t = drm.GetConnector(f, r->connectors[i]);
			if (t && t->connection == DRM_MODE_CONNECTED && t->count_modes > 0) {
				cc = t;
				break;
			}
			if (t) drm.FreeConnector(t);
		}
		if (cc) { fd = f; res = r; conn = cc; }
		else    { drm.FreeResources(r); close(f); }
	}
	if (fd < 0) {
		fprintf(stderr, "machismo: splash: no connected DRM output\n");
		goto fail;
	}

	/* Pick the connector's preferred mode (fall back to its first). */
	drmModeModeInfo mode = conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = conn->modes[i];
			break;
		}
	uint32_t W = mode.hdisplay, H = mode.vdisplay;

	/* Find a CRTC: prefer the connector's current encoder, else the first CRTC
	 * any of its encoders can drive. */
	uint32_t crtc_id = 0;
	if (conn->encoder_id) {
		drmModeEncoder *enc = drm.GetEncoder(fd, conn->encoder_id);
		if (enc) { crtc_id = enc->crtc_id; drm.FreeEncoder(enc); }
	}
	for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
		drmModeEncoder *e = drm.GetEncoder(fd, conn->encoders[i]);
		if (!e) continue;
		for (int j = 0; j < res->count_crtcs; j++)
			if (e->possible_crtcs & (1u << j)) { crtc_id = res->crtcs[j]; break; }
		drm.FreeEncoder(e);
	}
	if (!crtc_id) {
		fprintf(stderr, "machismo: splash: no CRTC for output\n");
		goto fail;
	}

	/* Become master to modeset (root on the handheld; granted to the sole opener
	 * on a free console otherwise). Best-effort — failure surfaces at SetCrtc. */
	splash_set_master(fd);

	/* Allocate + map a dumb buffer at panel resolution (XRGB8888). */
	creq.width = W;
	creq.height = H;
	creq.bpp = 32;
	if (splash_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		fprintf(stderr, "machismo: splash: CREATE_DUMB %ux%u failed\n", W, H);
		goto fail;
	}
	if (drm.AddFB(fd, W, H, 24, 32, creq.pitch, creq.handle, &fb_id) < 0) {
		fprintf(stderr, "machismo: splash: AddFB failed\n");
		goto fail;
	}
	struct drm_mode_map_dumb mreq;
	memset(&mreq, 0, sizeof mreq);
	mreq.handle = creq.handle;
	if (splash_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		fprintf(stderr, "machismo: splash: MAP_DUMB failed\n");
		goto fail;
	}
	map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
	if (map == MAP_FAILED) {
		fprintf(stderr, "machismo: splash: mmap failed\n");
		goto fail;
	}

	/* Clear to black, then scale the image to fit (preserve aspect, letterbox)
	 * and blit centered, converting RGBA -> XRGB8888 (LE word 0x00RRGGBB). */
	memset(map, 0, creq.size);
	double sx = (double)W / iw, sy = (double)H / ih;
	double s = sx < sy ? sx : sy;
	int dw = (int)(iw * s), dh = (int)(ih * s);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	scaled = (unsigned char *)malloc((size_t)dw * dh * 4);
	if (scaled && stbir_resize_uint8(img, iw, ih, 0, scaled, dw, dh, 0, 4)) {
		int ox = ((int)W - dw) / 2, oy = ((int)H - dh) / 2;
		for (int y = 0; y < dh; y++) {
			uint32_t *drow = (uint32_t *)((uint8_t *)map + (size_t)(oy + y) * creq.pitch) + ox;
			const unsigned char *srow = scaled + (size_t)y * dw * 4;
			for (int x = 0; x < dw; x++) {
				uint32_t r = srow[x * 4 + 0], g = srow[x * 4 + 1], b = srow[x * 4 + 2];
				drow[x] = (r << 16) | (g << 8) | b;
			}
		}
	}

	/* Modeset the splash onto the CRTC. */
	if (drm.SetCrtc(fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, &mode) < 0) {
		fprintf(stderr, "machismo: splash: SetCrtc failed\n");
		goto fail;
	}

	/* Drop the master: the CRTC keeps scanning out our framebuffer, but with no
	 * process holding the master SDL/Vulkan can take the display over cleanly. */
	splash_drop_master(fd);

	g_splash.fd          = fd;
	g_splash.fb_id       = fb_id;
	g_splash.dumb_handle = creq.handle;
	g_splash.map         = map;
	g_splash.map_size    = creq.size;

	fprintf(stderr, "machismo: splash up (%ux%u, image %dx%d) — DRM master dropped\n",
	        W, H, iw, ih);
	free(scaled);
	drm.FreeConnector(conn);
	drm.FreeResources(res);
	stbi_image_free(img);
	return 0;

fail:
	free(scaled);
	if (map != MAP_FAILED && map) munmap(map, creq.size);
	if (fb_id) drm.RmFB(fd, fb_id);
	if (creq.handle) {
		struct drm_mode_destroy_dumb d;
		memset(&d, 0, sizeof d);
		d.handle = creq.handle;
		splash_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	if (fd >= 0) splash_drop_master(fd);   /* harmless if never master */
	if (conn) drm.FreeConnector(conn);
	if (res) drm.FreeResources(res);
	if (fd >= 0) close(fd);
	if (img) stbi_image_free(img);
	return -1;
}

void machismo_splash_teardown(void)
{
	if (g_splash.fd < 0)
		return;
	if (g_splash.map && g_splash.map != MAP_FAILED)
		munmap(g_splash.map, g_splash.map_size);
	if (g_splash.fb_id && drm.RmFB)
		drm.RmFB(g_splash.fd, g_splash.fb_id);
	if (g_splash.dumb_handle) {
		struct drm_mode_destroy_dumb d;
		memset(&d, 0, sizeof d);
		d.handle = g_splash.dumb_handle;
		splash_ioctl(g_splash.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	close(g_splash.fd);
	g_splash.fd = -1;
	g_splash.map = NULL;
	g_splash.fb_id = 0;
	g_splash.dumb_handle = 0;
}

#else  /* !HAVE_LIBDRM — no KMS splash; functions are inert. */

#include "splash_kms.h"
int  machismo_splash_show(const char *png_path) { (void)png_path; return -1; }
void machismo_splash_teardown(void) {}

#endif /* HAVE_LIBDRM */
