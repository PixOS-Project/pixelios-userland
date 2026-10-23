/*
 * sweet_drm_mali: libdrm + panfrost DRM emulation for Mesa panfrost on iOS (no DRM here).
 *
 * Implements, in user space on top of the SweetMali IOKit user client (xnu iokit/Kernel/arm/SweetMali.cpp), the libdrm
 * functions declared by Mesa's fake <xf86drm.h> (patch 0002) plus the BO map hooks of patch 0004. The drm* functions are
 * declared by xf86drm.h and darwin_drm_bo_map/unmap by darwin_drm.h; this header only has what is specific to us.
 *
 * Kernel selector contract (IOConnectCallMethod; all scalar except 0):
 *   0 query         out struct sweet_mali_info
 *   1 boNew         in: size, flags (1 = executable)       out: handle, GPU VA, allocated size (16 KiB rounded)
 *   2 boClose       in: handle
 *   3 submit        in: job chain GPU VA, requirements (1 = fragment)   out: seqno (non-zero, increasing)
 *   4 wait          in: seqno, relative timeout in microseconds (kernel clamps to 60 s; Timeout = not done yet)
 *   memoryType=handle of IOConnectMapMemory64: CPU mapping of the BO (kIOMapWriteCombineCache)
 *   5 importSurface  in: IOSurface id   out (7): handle, GPU VA (< 4 GiB), length, bytesPerRow, width, height, pixelFormat
 *                    [src xnu/patches/0105 SweetMali.cpp importSurface]. Same handle table as boNew (boClose frees it,
 *                    clientMemoryForType maps it). NotFound = unknown id, Unsupported = length not a 16 KiB multiple /
 *                    not a kernel buffer (-> -ENOENT / -ENOSYS here).
 */
#ifndef SWEET_DRM_MALI_H
#define SWEET_DRM_MALI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the kernel reported for an imported IOSurface (selector 5 outputs). */
struct sweet_drm_surface {
	uint32_t handle;
	uint64_t va, len;
	uint32_t bytes_per_row, width, height, pixel_format;
};

/* Open the device context (first call connects to SweetMali) and return a dup() of a real fd (/dev/null) to hand to
 * panfrost_create_screen() as the DRM device fd. -1 with errno set (ENODEV: no service, EBUSY: second client) on failure;
 * the GPU may come up a while after boot, so callers may retry. */
int sweet_drm_mali_open(void);

/* Tell drmPrimeFDToHandle that `fd` stands for IOSurface `iosurface_id`. fd must be a real fd whose lseek(SEEK_END) size is
 * `size` (e.g. an unlinked temp file after ftruncate): Mesa takes the buffer size from lseek (pan_kmod.c:172). 0 or -errno.
 * Re-registering an fd number overwrites; call unregister when closing the fd. */
int sweet_drm_mali_register_surface(int fd, uint32_t iosurface_id, uint64_t size);
void sweet_drm_mali_unregister_surface(int fd);

/* Import IOSurface `id` through selector 5. Idempotent while the import is live (same record back, one kernel mapping).
 * 0 or -errno (ENOENT unknown id, ENOSYS unsupported surface, ENODEV no GPU service). The import is owned by this side until
 * drmPrimeFDToHandle hands it to Mesa; from then on Mesa's GEM_CLOSE (resource destroyed) calls boClose. */
int sweet_drm_mali_import_surface(uint32_t id, struct sweet_drm_surface *out);
/* Close an import that Mesa never took (e.g. the attach failed after the import): boClose. -ENOENT if none; 0 and a no-op if Mesa
 * owns it. */
int sweet_drm_mali_release_surface(uint32_t id);
/* The EAGL shim's weak hook `sweet_gl_kernel_import` (eagl_objc.m): import + GPU VA. */
int sweet_gl_kernel_import(uint32_t surface_id, uint64_t *va);

#ifdef __cplusplus
}
#endif
#endif
