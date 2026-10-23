/*
 * sweet_drm_mali: see sweet_drm_mali.h and README.md. Counterpart of sailfish tools/sweetgl/sweet_drm.c for Mali.
 *
 * Conventions (libdrm, [guess: libdrm source not at hand]): drmIoctl/drmCloseBufferHandle/drmSyncobj* return 0, or -1 with
 * errno set; drmGetCap/drmPrime* return -errno (and set errno too). Mesa only tests the result for != 0 / >= 0.
 * One mutex protects everything; it is NOT held while waiting for the GPU.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef SWEET_DRM_HOST_TEST
#include "fake_iokit.h"
#else
#include <IOKit/IOKitLib.h>
#endif

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "darwin_drm.h"
#include "sweet_drm_mali.h"

/* Kernel side: bsd/dev/sweet_mali.h (patch 0104). */
struct sweet_mali_info {
	uint32_t gpu_id, l2_features, tiler_features, mem_features, mmu_features, as_present, js_present;
	uint32_t thread_max_threads, thread_max_workgroup, thread_max_barrier, thread_features, texture_features[4], afbc_features, shader_present;
	uint64_t va_start, va_end;
};
_Static_assert(sizeof(struct sweet_mali_info) == 88, "must match the kernel struct (arm64 natural alignment)");

enum { SEL_QUERY, SEL_BO_NEW, SEL_BO_CLOSE, SEL_SUBMIT, SEL_WAIT };
#define SWEET_MALI_EXEC   1u
#define SWEET_MALI_REQ_FS 1u
#define WAIT_CHUNK_US     1000000u          /* the kernel clamps one wait to 60 s; loop in 1 s slices */
#define NOFENCE           UINT64_MAX        /* syncobj without a fence (created unsignalled / reset) */
#define MAX_HANDLES       (1u << 20)
#define MAX_FDS           (1u << 16)

struct bo {
	bool used, heap;
	uint64_t va, size, map;
};
struct sobj {
	bool used;
	uint64_t seq;                       /* 0 = signalled, NOFENCE = no fence, else kernel seqno */
};
enum { FD_NONE, FD_SYNC, FD_SURFACE };
struct fdrec {
	uint8_t kind;
	uint64_t seq, size;
	uint32_t surface;
};

/* The one device context, created lazily (ponytail: one device per process, like sailfish). */
static struct {
	pthread_mutex_t lock;
	bool up;
	io_connect_t conn;
	int dev_fd;
	struct sweet_mali_info info;
	uint64_t last_seq, done_seq;        /* last submitted / highest seqno known complete (in-order queue) */
	struct bo *bos;
	uint32_t nbos;
	struct sobj *sobjs;
	uint32_t nsobjs;
	struct fdrec *fds;
	uint32_t nfds;
} C = { .lock = PTHREAD_MUTEX_INITIALIZER, .dev_fd = -1 };

static int
fail(int e)
{
	errno = e;
	return -1;
}

static int
res(int neg_errno)
{
	return neg_errno ? fail(-neg_errno) : 0;
}

static int
kr_errno(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnSuccess: return 0;
	case kIOReturnTimeout: return ETIMEDOUT;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnNoMemory:
	case kIOReturnNoResources: return ENOMEM;
	case kIOReturnNotFound: return ENOENT;
	case kIOReturnBusy: return EBUSY;
	case kIOReturnNotPermitted: return EPERM;
	case kIOReturnNotReady: return ENODEV;
	case kIOReturnUnsupported: return ENOSYS;
	default: return EIO;
	}
}

/* Lock held. Returns 0 or -errno. in/out may be NULL. */
static int
kcall(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
	uint32_t n = nout;
	kern_return_t kr = IOConnectCallMethod(C.conn, sel, in, nin, NULL, 0, out, out ? &n : NULL, NULL, NULL);
	if (kr != kIOReturnSuccess) {
		return -kr_errno(kr);
	}
	return out && n != nout ? -EIO : 0;
}

/* Lock held. */
static int
ensure_ctx(void)
{
	if (C.up) {
		return 0;
	}
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetMali"));
	if (!svc) {
		return -ENODEV;
	}
	io_connect_t c = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
	IOObjectRelease(svc);
	if (kr != kIOReturnSuccess) {
		return -(kr == kIOReturnBusy ? EBUSY : ENODEV);
	}
	struct sweet_mali_info info;
	size_t isz = sizeof(info);
	kr = IOConnectCallMethod(c, SEL_QUERY, NULL, 0, NULL, 0, NULL, NULL, &info, &isz);
	int fd = kr == kIOReturnSuccess && isz == sizeof(info) ? open("/dev/null", O_RDWR | O_CLOEXEC) : -1;
	if (fd < 0) {
		int e = kr == kIOReturnSuccess ? errno : kr_errno(kr);
		IOServiceClose(c);
		return -(e ? e : EIO);
	}
	C.conn = c;
	C.info = info;
	C.dev_fd = fd;
	C.up = true;
	return 0;
}

/* Returns 0 with the lock held, or -errno with it released. */
static int
enter(void)
{
	pthread_mutex_lock(&C.lock);
	int e = ensure_ctx();
	if (e) {
		pthread_mutex_unlock(&C.lock);
	}
	return e;
}

static void
leave(void)
{
	pthread_mutex_unlock(&C.lock);
}

/* Lock held: make *tab (element size esz, *n entries) hold index idx. */
static int
grow(void **tab, uint32_t *n, size_t esz, uint32_t idx, uint32_t limit)
{
	if (idx < *n) {
		return 0;
	}
	if (idx >= limit) {
		return -ENOMEM;
	}
	uint32_t m = *n ? *n : 64;
	while (m <= idx) {
		m *= 2;
	}
	void *p = realloc(*tab, (size_t)m * esz);
	if (!p) {
		return -ENOMEM;
	}
	memset((char *)p + (size_t)*n * esz, 0, (size_t)(m - *n) * esz);
	*tab = p;
	*n = m;
	return 0;
}

static struct bo *
bo_get(uint32_t h)
{
	return h >= 1 && h <= C.nbos && C.bos[h - 1].used ? &C.bos[h - 1] : NULL;
}

static struct sobj *
so_get(uint32_t h)
{
	return h >= 1 && h <= C.nsobjs && C.sobjs[h - 1].used ? &C.sobjs[h - 1] : NULL;
}

/* Lock held: record a kernel handle (new BO or imported surface). */
static int
bo_add(uint32_t h, uint64_t va, uint64_t size, bool heap)
{
	if (h == 0 || bo_get(h)) {
		return -EIO;                /* the kernel handed out a handle we think is live */
	}
	int e = grow((void **)&C.bos, &C.nbos, sizeof(*C.bos), h - 1, MAX_HANDLES);
	if (e) {
		return e;
	}
	C.bos[h - 1] = (struct bo){ .used = true, .heap = heap, .va = va, .size = size };
	return 0;
}

/* Lock held. Unmap the CPU mapping if any. */
static void
bo_unmap_cpu(uint32_t h, struct bo *b)
{
	if (b->map) {
		IOConnectUnmapMemory64(C.conn, h, mach_task_self(), b->map);
		b->map = 0;
	}
}

/* Wait until kernel seqno `seq` completed. rel_ns: relative timeout, INT64_MAX = forever, <= 0 = poll.
 * Lock NOT held (the context must be up). 0, -ETIMEDOUT, or -errno. */
static int
wait_seq(uint64_t seq, int64_t rel_ns)
{
	pthread_mutex_lock(&C.lock);
	bool done = seq == 0 || seq <= C.done_seq;
	pthread_mutex_unlock(&C.lock);
	if (done) {
		return 0;
	}
	bool forever = rel_ns == INT64_MAX;
	uint64_t rem = rel_ns <= 0 ? 0 : (uint64_t)rel_ns / 1000 + ((uint64_t)rel_ns % 1000 != 0);   /* us, rounded up */
	for (;;) {
		uint64_t chunk = rem > WAIT_CHUNK_US ? WAIT_CHUNK_US : rem;
		uint64_t in[2] = { seq, chunk };
		/* no lock: the connection never changes once up, and other threads must keep submitting */
		kern_return_t kr = IOConnectCallMethod(C.conn, SEL_WAIT, in, 2, NULL, 0, NULL, NULL, NULL, NULL);
		int e = -kr_errno(kr);
		if (e == 0) {
			pthread_mutex_lock(&C.lock);
			if (seq > C.done_seq) {
				C.done_seq = seq;
			}
			pthread_mutex_unlock(&C.lock);
		}
		if (e != -ETIMEDOUT) {
			return e;
		}
		if (!forever) {
			rem -= chunk;
			if (rem == 0) {
				return -ETIMEDOUT;
			}
		}
	}
}

/* ---- version / caps ---- */

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	int e = enter();
	if (e) {
		errno = -e;
		return NULL;
	}
	leave();
	static const char name[] = "panfrost", desc[] = "SweetMali user-space emulation";
	drmVersionPtr v = calloc(1, sizeof(*v) + sizeof(name) + sizeof(desc) + 1);
	if (!v) {
		return NULL;
	}
	char *p = (char *)(v + 1);
	v->version_major = 1;
	v->version_minor = 3;       /* 1.3: timestamps; not 1.6 (no WB-mmap / coherency queries, research 32 s.2.2 row 1) */
	v->name = p;
	v->name_len = (int)sizeof(name) - 1;
	memcpy(p, name, sizeof(name));
	p += sizeof(name);
	v->desc = p;
	v->desc_len = (int)sizeof(desc) - 1;
	memcpy(p, desc, sizeof(desc));
	p += sizeof(desc);
	v->date = p;                /* empty string */
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	free(v);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	if (value) {
		*value = 0;
	}
	errno = EINVAL;
	return -EINVAL;
}

/* ---- ioctls ---- */

/* Lock held. As Linux: unknown param -> EINVAL. Params the kernel's struct sweet_mali_info does not carry are EINVAL too. */
static int
get_param(struct drm_panfrost_get_param *p)
{
	const struct sweet_mali_info *i = &C.info;
	uint64_t v;

	if (p->pad) {
		return -EINVAL;
	}
	switch (p->param) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID: v = i->gpu_id >> 16; break;
	case DRM_PANFROST_PARAM_GPU_REVISION: v = i->gpu_id & 0xffff; break;
	case DRM_PANFROST_PARAM_SHADER_PRESENT: v = i->shader_present; break;
	case DRM_PANFROST_PARAM_AS_PRESENT: v = i->as_present; break;
	case DRM_PANFROST_PARAM_JS_PRESENT: v = i->js_present; break;
	case DRM_PANFROST_PARAM_L2_FEATURES: v = i->l2_features; break;
	case DRM_PANFROST_PARAM_TILER_FEATURES: v = i->tiler_features; break;
	case DRM_PANFROST_PARAM_MEM_FEATURES: v = i->mem_features; break;
	case DRM_PANFROST_PARAM_MMU_FEATURES: v = i->mmu_features; break;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: v = i->thread_features; break;
	case DRM_PANFROST_PARAM_MAX_THREADS: v = i->thread_max_threads; break;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: v = i->thread_max_workgroup; break;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: v = i->thread_max_barrier; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES0: v = i->texture_features[0]; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES1: v = i->texture_features[1]; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES2: v = i->texture_features[2]; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES3: v = i->texture_features[3]; break;
	case DRM_PANFROST_PARAM_AFBC_FEATURES: v = i->afbc_features; break;
	case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES: v = 1u << PANFROST_JM_CTX_PRIORITY_MEDIUM; break;  /* default ctx only */
	case DRM_PANFROST_PARAM_SELECTED_COHERENCY: v = DRM_PANFROST_GPU_COHERENCY_NONE; break;
	default: return -EINVAL;
	}
	p->value = v;
	return 0;
}

/* Lock held. */
static int
create_bo(struct drm_panfrost_create_bo *r)
{
	const uint32_t known = PANFROST_BO_NOEXEC | PANFROST_BO_HEAP | PANFROST_BO_WB_MMAP;
	if (r->size == 0 || (r->flags & ~known) || r->pad ||
	    ((r->flags & PANFROST_BO_HEAP) && (r->flags & PANFROST_BO_WB_MMAP))) {
		return -EINVAL;
	}
	/* HEAP: no alloc-on-fault here, the whole size is backed eagerly (research 32 s.2.6 item 1). WB_MMAP is accepted and ignored:
	 * mappings are write-combined (version 1.3 never asks for it anyway). */
	uint64_t in[2] = { r->size, (r->flags & PANFROST_BO_NOEXEC) ? 0 : SWEET_MALI_EXEC }, out[3];
	int e = kcall(SEL_BO_NEW, in, 2, out, 3);
	if (e) {
		return e;
	}
	if (out[0] > UINT32_MAX) {
		return -EIO;
	}
	e = bo_add((uint32_t)out[0], out[1], out[2], r->flags & PANFROST_BO_HEAP);
	if (e) {
		if (e != -EIO) {            /* -EIO = duplicate handle, that BO is live: keep it */
			uint64_t h = out[0];
			(void)kcall(SEL_BO_CLOSE, &h, 1, NULL, 0);    /* table full: give the kernel BO back */
		}
		return e;
	}
	r->handle = (uint32_t)out[0];
	r->size = (uint32_t)out[2];
	r->offset = out[1];
	return 0;
}

/* Lock held. */
static int
close_bo(uint32_t handle)
{
	struct bo *b = bo_get(handle);
	if (!b) {
		return -EINVAL;
	}
	bo_unmap_cpu(handle, b);
	uint64_t h = handle;
	int e = kcall(SEL_BO_CLOSE, &h, 1, NULL, 0);
	b->used = false;
	return e;
}

/* Lock held. */
static int
submit(struct drm_panfrost_submit *r)
{
	const uint32_t known = PANFROST_JD_REQ_FS | PANFROST_JD_REQ_CYCLE_COUNT;
	if (r->jc == 0 || (r->requirements & ~known) || r->pad) {
		return -EINVAL;
	}
	if (r->out_sync && !so_get(r->out_sync)) {
		return -ENOENT;
	}
	/* in_syncs and bo_handles are ignored: one in-order queue, and the kernel orders fragment after tiler (research 32 s.2.2 row 9). */
	uint64_t in[2] = { r->jc, (r->requirements & PANFROST_JD_REQ_FS) ? SWEET_MALI_REQ_FS : 0 }, seq[1];
	int e = kcall(SEL_SUBMIT, in, 2, seq, 1);
	if (e) {
		return e;
	}
	if (seq[0] == 0) {
		return -EIO;
	}
	C.last_seq = seq[0];
	if (r->out_sync) {
		so_get(r->out_sync)->seq = seq[0];
	}
	return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (!arg) {
		return fail(EFAULT);
	}
	int e = enter();
	if (e) {
		return fail(-e);
	}
	if (request == DRM_IOCTL_GEM_CLOSE) {
		e = close_bo(((struct drm_gem_close *)arg)->handle);
		leave();
		return res(e);
	}
	if (((request >> 8) & 0xff) != DRM_IOCTL_BASE || (request & 0xff) < DRM_COMMAND_BASE || (request & 0xff) >= DRM_COMMAND_END) {
		leave();
		return fail(EINVAL);
	}
	switch ((request & 0xff) - DRM_COMMAND_BASE) {
	case DRM_PANFROST_GET_PARAM:
		e = get_param(arg);
		break;
	case DRM_PANFROST_CREATE_BO:
		e = create_bo(arg);
		break;
	case DRM_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *r = arg;
		struct bo *b = bo_get(r->handle);
		e = r->pad ? -EINVAL : b ? 0 : -ENOENT;
		if (!e) {
			r->offset = b->va;
		}
		break;
	}
	case DRM_PANFROST_MMAP_BO: {
		/* The cookie is handle << 32; darwin_drm_bo_map() turns it back into the IOConnectMapMemory64 address. */
		struct drm_panfrost_mmap_bo *r = arg;
		struct bo *b = bo_get(r->handle);
		e = r->flags ? -EINVAL : !b ? -ENOENT : b->heap ? -EINVAL : 0;   /* as Linux: no CPU access to heap BOs */
		if (!e) {
			r->offset = (uint64_t)r->handle << 32;
		}
		break;
	}
	case DRM_PANFROST_WAIT_BO: {
		/* timeout_ns is RELATIVE here (patch 0003 converts in panfrost_kmod.c under __APPLE__). Waits for the last submit of the
		 * device, a superset of the BO's own (the bo_handles list is ignored). */
		struct drm_panfrost_wait_bo *r = arg;
		uint64_t seq = C.last_seq;
		e = r->pad ? -EINVAL : bo_get(r->handle) ? 0 : -ENOENT;
		leave();
		if (!e) {
			e = wait_seq(seq, r->timeout_ns);
		}
		return res(e);
	}
	case DRM_PANFROST_SUBMIT:
		e = submit(arg);
		break;
	case DRM_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *r = arg;
		e = r->madv > PANFROST_MADV_DONTNEED ? -EINVAL : bo_get(r->handle) ? 0 : -ENOENT;
		if (!e) {
			r->retained = 1;
		}
		break;
	}
	case DRM_PANFROST_SYNC_BO:          /* only for WB_MMAP BOs, version >= 1.6 */
	case DRM_PANFROST_QUERY_BO_INFO:    /* version >= 1.6 */
	case DRM_PANFROST_SET_LABEL_BO:     /* skipped below version 1.4 */
	case DRM_PANFROST_JM_CTX_CREATE:    /* default context 0 only */
	case DRM_PANFROST_JM_CTX_DESTROY:
	case DRM_PANFROST_PERFCNT_ENABLE:
	case DRM_PANFROST_PERFCNT_DUMP:
		e = -ENOSYS;
		break;
	default:
		e = -EINVAL;
		break;
	}
	leave();
	return res(e);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close c = { .handle = handle };
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* ---- BO CPU mapping hooks (patch 0004) ---- */

void *
darwin_drm_bo_map(int dev_fd, size_t size, int prot, int flags, uint64_t offset)
{
	(void)dev_fd;
	(void)prot;
	(void)flags;
	uint32_t handle = (uint32_t)(offset >> 32);
	if ((offset & 0xffffffffu) != 0 || enter()) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	struct bo *b = bo_get(handle);
	void *ret = MAP_FAILED;
	if (!b || b->heap || size > b->size) {
		errno = EINVAL;
	} else if (b->map) {
		ret = (void *)(uintptr_t)b->map;
	} else {
		mach_vm_address_t addr = 0;
		mach_vm_size_t sz = 0;
		kern_return_t kr = IOConnectMapMemory64(C.conn, handle, mach_task_self(), &addr, &sz, kIOMapAnywhere | kIOMapWriteCombineCache);
		if (kr != kIOReturnSuccess) {
			errno = kr_errno(kr);
		} else if (sz < size) {
			IOConnectUnmapMemory64(C.conn, handle, mach_task_self(), addr);
			errno = EINVAL;
		} else {
			b->map = addr;
			ret = (void *)(uintptr_t)addr;
		}
	}
	leave();
	return ret;
}

int
darwin_drm_bo_unmap(void *addr, size_t size)
{
	(void)size;
	if (!addr || enter()) {
		return fail(EINVAL);
	}
	int ret = fail(EINVAL);
	for (uint32_t i = 0; i < C.nbos; i++) {
		if (C.bos[i].used && C.bos[i].map == (uint64_t)(uintptr_t)addr) {
			bo_unmap_cpu(i + 1, &C.bos[i]);
			ret = 0;
			break;
		}
	}
	leave();
	return ret;
}

/* ---- syncobj emulation (research 32 s.2.5): an entry is one kernel seqno ---- */

/* Lock held. */
static int
so_new(uint64_t seq, uint32_t *handle)
{
	uint32_t i = 0;
	while (i < C.nsobjs && C.sobjs[i].used) {
		i++;
	}
	int e = grow((void **)&C.sobjs, &C.nsobjs, sizeof(*C.sobjs), i, MAX_HANDLES);
	if (e) {
		return e;
	}
	C.sobjs[i] = (struct sobj){ .used = true, .seq = seq };
	*handle = i + 1;
	return 0;
}

/* Lock held: a fresh real fd recorded as kind/seq. -errno. */
static int
fd_new(uint8_t kind, uint64_t seq, int *out)
{
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);   /* any real fd; Mesa exports, imports and closes it back to back */
	if (fd < 0) {
		return -errno;
	}
	int e = (uint32_t)fd >= MAX_FDS ? -EMFILE : grow((void **)&C.fds, &C.nfds, sizeof(*C.fds), (uint32_t)fd, MAX_FDS);
	if (e) {
		close(fd);
		return e;
	}
	C.fds[fd] = (struct fdrec){ .kind = kind, .seq = seq };
	*out = fd;
	return 0;
}

/* Lock held. */
static struct fdrec *
fd_get(int fd, uint8_t kind)
{
	return fd >= 0 && (uint32_t)fd < C.nfds && C.fds[fd].kind == kind ? &C.fds[fd] : NULL;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	if (!handle || (flags & ~(uint32_t)DRM_SYNCOBJ_CREATE_SIGNALED)) {
		return fail(EINVAL);
	}
	int e = enter();
	if (!e) {
		e = so_new((flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 0 : NOFENCE, handle);
		leave();
	}
	return res(e);
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	int e = enter();
	if (!e) {
		struct sobj *s = so_get(handle);
		e = s ? 0 : -EINVAL;
		if (s) {
			s->used = false;
		}
		leave();
	}
	return res(e);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd;
	int e = enter();
	if (!e) {
		struct sobj *s = so_get(handle);
		e = !obj_fd ? -EINVAL : s ? fd_new(FD_SYNC, s->seq, obj_fd) : -ENOENT;
		leave();
	}
	return res(e);
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	int e = enter();
	if (!e) {
		struct sobj *s = so_get(handle);
		e = !sync_file_fd ? -EINVAL : !s ? -ENOENT : s->seq == NOFENCE ? -EINVAL : fd_new(FD_SYNC, s->seq, sync_file_fd);
		leave();
	}
	return res(e);
}

/* ponytail: a snapshot of the seqno, not a shared live object; fine while a syncobj gets its fence once (Mesa JM). */
int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	int e = enter();
	if (!e) {
		struct fdrec *r = fd_get(obj_fd, FD_SYNC);
		e = !handle ? -EINVAL : r ? so_new(r->seq, handle) : -EINVAL;
		leave();
	}
	return res(e);
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	int e = enter();
	if (!e) {
		struct sobj *s = so_get(handle);
		struct fdrec *r = fd_get(sync_file_fd, FD_SYNC);
		e = !s ? -ENOENT : !r ? -EINVAL : 0;
		if (!e) {
			s->seq = r->seq;
		}
		leave();
	}
	return res(e);
}

static int
syncobj_wait(uint32_t *handles, const uint64_t *points, unsigned n, int64_t abs_timeout_ns, unsigned flags, uint32_t *first)
{
	if (!handles || n == 0) {
		return fail(EINVAL);
	}
	for (unsigned i = 0; points && i < n; i++) {
		if (points[i]) {
			return fail(EINVAL);        /* binary syncobjs only */
		}
	}
	int e = enter();
	if (e) {
		return fail(-e);
	}
	bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	uint64_t seq = 0;
	unsigned idx = 0;
	for (unsigned i = 0; i < n && !e; i++) {
		struct sobj *s = so_get(handles[i]);
		e = !s ? -ENOENT : s->seq == NOFENCE ? -EINVAL : 0;   /* ponytail: no WAIT_FOR_SUBMIT blocking, a fenceless syncobj is an error */
		/* in-order queue: ALL completes with the largest seqno, ANY with the smallest */
		if (!e && (i == 0 || (all ? s->seq > seq : s->seq < seq))) {
			seq = s->seq;
			idx = i;
		}
	}
	leave();
	if (!e && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
		e = wait_seq(seq, darwin_drm_abs_timeout_to_rel_ns(abs_timeout_ns));
		if (e == -ETIMEDOUT) {
			e = -ETIME;
		}
	}
	if (!e && first) {
		*first = all ? 0 : idx;
	}
	return res(e);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	return syncobj_wait(handles, NULL, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	return syncobj_wait(handles, points, num_handles, timeout_nsec, flags, first_signaled);
}

/* Reset (to == NOFENCE) and Signal (to == 0) over a handle list; all handles are checked first. */
static int
set_all(const uint32_t *handles, uint32_t count, uint64_t to)
{
	if (!handles || count == 0) {
		return fail(EINVAL);
	}
	int e = enter();
	if (e) {
		return fail(-e);
	}
	for (uint32_t i = 0; i < count && !e; i++) {
		e = so_get(handles[i]) ? 0 : -ENOENT;
	}
	for (uint32_t i = 0; i < count && !e; i++) {
		so_get(handles[i])->seq = to;
	}
	leave();
	return res(e);
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	return set_all(handles, handle_count, NOFENCE);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	return set_all(handles, handle_count, 0);
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	(void)fd;
	for (uint32_t i = 0; points && i < handle_count; i++) {
		if (points[i]) {
			return fail(EINVAL);
		}
	}
	return set_all(handles, handle_count, 0);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	/* Not designed in research 32 and not reachable from the JM path; the value Linux reports for a binary syncobj is unchecked. */
	(void)fd;
	(void)handles;
	(void)points;
	(void)handle_count;
	(void)flags;
	return fail(ENOSYS);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	(void)fd;
	if (dst_point || src_point || flags) {
		return fail(EINVAL);
	}
	int e = enter();
	if (!e) {
		struct sobj *d = so_get(dst_handle), *s = so_get(src_handle);
		e = d && s ? 0 : -ENOENT;
		if (!e) {
			d->seq = s->seq;
		}
		leave();
	}
	return res(e);
}

/* ---- prime (dma-buf) emulation ---- */

int
sweet_drm_mali_register_surface(int fd, uint32_t iosurface_id, uint64_t size)
{
	if (fd < 0 || size == 0 || (uint64_t)lseek(fd, 0, SEEK_END) != size) {
		return -EINVAL;              /* lseek(SEEK_END) is the size Mesa will see */
	}
	int e = enter();
	if (e) {
		return e;
	}
	e = (uint32_t)fd >= MAX_FDS ? -EMFILE : grow((void **)&C.fds, &C.nfds, sizeof(*C.fds), (uint32_t)fd, MAX_FDS);
	if (!e) {
		C.fds[fd] = (struct fdrec){ .kind = FD_SURFACE, .surface = iosurface_id, .size = size };
	}
	leave();
	return e;
}

void
sweet_drm_mali_unregister_surface(int fd)
{
	pthread_mutex_lock(&C.lock);
	if (fd >= 0 && (uint32_t)fd < C.nfds && C.fds[fd].kind == FD_SURFACE) {
		C.fds[fd].kind = FD_NONE;
	}
	pthread_mutex_unlock(&C.lock);
}

/* TODO(kernel selector 5, importSurface): in IOSurface id, out handle, GPU VA, size. Does not exist in SweetMali.cpp yet, so this is
 * a stub. When it lands: kcall(5, &id, 1, out, 3) and return out[0..2]; see the contract in sweet_drm_mali.h. */
static int
kernel_import_surface(uint32_t surface_id, uint32_t *handle, uint64_t *va, uint64_t *size)
{
	(void)surface_id;
	(void)handle;
	(void)va;
	(void)size;
	return -ENOSYS;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	int e = enter();
	if (e) {
		errno = -e;
		return e;
	}
	struct fdrec *r = fd_get(prime_fd, FD_SURFACE);
	uint32_t h = 0;
	uint64_t va = 0, sz = 0;
	e = !handle || !r ? -EINVAL : kernel_import_surface(r->surface, &h, &va, &sz);
	if (!e) {
		e = bo_add(h, va, sz, false);
		if (!e) {
			*handle = h;
		}
	}
	leave();
	errno = -e;
	return e;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -ENOSYS;
}

/* ---- device fd ---- */

int
sweet_drm_mali_open(void)
{
	int e = enter();
	if (e) {
		return fail(-e);
	}
	int fd = fcntl(C.dev_fd, F_DUPFD_CLOEXEC, 0);
	e = fd < 0 ? -errno : 0;
	leave();
	return res(e) ? -1 : fd;
}
