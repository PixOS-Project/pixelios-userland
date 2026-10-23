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
 *   5 importSurface  NOT IMPLEMENTED IN THE KERNEL YET.  in: IOSurface id   out: handle, GPU VA, size
 *                    The handle must live in the same table as boNew handles (boClose frees it, clientMemoryForType
 *                    maps it); the GPU VA must be < 4 GiB. Until it exists, drmPrimeFDToHandle returns -ENOSYS.
 */
#ifndef SWEET_DRM_MALI_H
#define SWEET_DRM_MALI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the device context (first call connects to SweetMali) and return a dup() of a real fd (/dev/null) to hand to
 * panfrost_create_screen() as the DRM device fd. -1 with errno set (ENODEV: no service, EBUSY: second client) on failure;
 * the GPU may come up a while after boot, so callers may retry. */
int sweet_drm_mali_open(void);

/* Tell drmPrimeFDToHandle that `fd` stands for IOSurface `iosurface_id`. fd must be a real fd whose lseek(SEEK_END) size is
 * `size` (e.g. an unlinked temp file after ftruncate): Mesa takes the buffer size from lseek (pan_kmod.c:172). 0 or -errno.
 * Re-registering an fd number overwrites; call unregister when closing the fd. */
int sweet_drm_mali_register_surface(int fd, uint32_t iosurface_id, uint64_t size);
void sweet_drm_mali_unregister_surface(int fd);

#ifdef __cplusplus
}
#endif
#endif
