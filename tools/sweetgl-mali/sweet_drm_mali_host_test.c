/* Host test for sweet_drm_mali.c against fake_iokit.c. Build and run: host-test.sh MESA_DIR (Linux, no Apple headers). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fake_iokit.h"
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "darwin_drm.h"
#include "sweet_drm_mali.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define ERR(rc, e) CHECK((rc) == -1 && errno == (e))

/* Mesa's clock (util/os_time.c on the real build) */
static int64_t g_now;
int64_t os_time_get_nano(void) { return g_now; }

static int devfd = -1;
static int io(unsigned long req, void *arg) { return drmIoctl(devfd, req, arg); }

static uint64_t
param(uint32_t id, int *rc)
{
	struct drm_panfrost_get_param p = { .param = id };
	*rc = io(DRM_IOCTL_PANFROST_GET_PARAM, &p);
	return p.value;
}

static uint32_t
mkbo(uint32_t size, uint32_t flags, uint64_t *va, uint32_t *got)
{
	struct drm_panfrost_create_bo r = { .size = size, .flags = flags };
	if (io(DRM_IOCTL_PANFROST_CREATE_BO, &r)) {
		return 0;
	}
	if (va) *va = r.offset;
	if (got) *got = r.size;
	return r.handle;
}

static int
submit(uint64_t jc, uint32_t req, uint32_t out_sync)
{
	struct drm_panfrost_submit s = { .jc = jc, .requirements = req, .out_sync = out_sync };
	return io(DRM_IOCTL_PANFROST_SUBMIT, &s);
}

int
main(void)
{
	int rc;
	fake_reset();
	g_now = 1000000000;

	/* --- lazy connect, retry, error mapping --- */
	fake.service_present = 0;
	CHECK(drmGetVersion(-1) == NULL && errno == ENODEV);
	ERR(sweet_drm_mali_open(), ENODEV);
	fake.service_present = 1;
	fake.open_result = kIOReturnBusy;
	ERR(sweet_drm_mali_open(), EBUSY);
	fake.open_result = 0;
	fake.fail_sel[0] = kIOReturnNotReady;
	ERR(sweet_drm_mali_open(), ENODEV);
	CHECK(fake.opens == 1 && fake.closes == 1);    /* the failed query closed its connection again */
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0 && fake.opens == 2 && fcntl(devfd, F_GETFD) >= 0);
	CHECK(fake_count(0) == 2);

	/* --- version, caps --- */
	drmVersionPtr v = drmGetVersion(devfd);
	CHECK(v && strcmp(v->name, "panfrost") == 0 && v->name_len == 8 && v->version_major == 1 && v->version_minor == 3 && v->date_len == 0);
	drmFreeVersion(v);
	uint64_t cap = 5;
	CHECK(drmGetCap(devfd, DRM_CAP_PRIME, &cap) == -EINVAL && errno == EINVAL && cap == 0);
	CHECK(fake_count(0) == 2);                     /* the query is cached */

	/* --- GET_PARAM --- */
	static const struct { uint32_t id; uint64_t v; } want[] = {
		{ DRM_PANFROST_PARAM_GPU_PROD_ID, 0x9202 }, { DRM_PANFROST_PARAM_GPU_REVISION, 0x0010 },
		{ DRM_PANFROST_PARAM_L2_FEATURES, FAKE_INFO_WORD(1) }, { DRM_PANFROST_PARAM_TILER_FEATURES, FAKE_INFO_WORD(2) },
		{ DRM_PANFROST_PARAM_MEM_FEATURES, FAKE_INFO_WORD(3) }, { DRM_PANFROST_PARAM_MMU_FEATURES, FAKE_INFO_WORD(4) },
		{ DRM_PANFROST_PARAM_AS_PRESENT, FAKE_INFO_WORD(5) }, { DRM_PANFROST_PARAM_JS_PRESENT, FAKE_INFO_WORD(6) },
		{ DRM_PANFROST_PARAM_MAX_THREADS, FAKE_INFO_WORD(7) }, { DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ, FAKE_INFO_WORD(8) },
		{ DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ, FAKE_INFO_WORD(9) }, { DRM_PANFROST_PARAM_THREAD_FEATURES, FAKE_INFO_WORD(10) },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES0, FAKE_INFO_WORD(11) }, { DRM_PANFROST_PARAM_TEXTURE_FEATURES1, FAKE_INFO_WORD(12) },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES2, FAKE_INFO_WORD(13) }, { DRM_PANFROST_PARAM_TEXTURE_FEATURES3, FAKE_INFO_WORD(14) },
		{ DRM_PANFROST_PARAM_AFBC_FEATURES, FAKE_INFO_WORD(15) }, { DRM_PANFROST_PARAM_SHADER_PRESENT, FAKE_INFO_WORD(16) },
		{ DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES, 1u << PANFROST_JM_CTX_PRIORITY_MEDIUM },
		{ DRM_PANFROST_PARAM_SELECTED_COHERENCY, DRM_PANFROST_GPU_COHERENCY_NONE },
	};
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		uint64_t got = param(want[i].id, &rc);
		CHECK(rc == 0 && got == want[i].v);
		if (rc || got != want[i].v) printf("  param %u: rc=%d got=%#llx want=%#llx\n", want[i].id, rc, (unsigned long long)got, (unsigned long long)want[i].v);
	}
	CHECK(((param(DRM_PANFROST_PARAM_GPU_PROD_ID, &rc) << 16) | param(DRM_PANFROST_PARAM_GPU_REVISION, &rc)) == 0x92020010u);   /* what Mesa composes */
	static const uint32_t nope[] = { DRM_PANFROST_PARAM_TILER_PRESENT, DRM_PANFROST_PARAM_L2_PRESENT, DRM_PANFROST_PARAM_CORE_FEATURES,
		DRM_PANFROST_PARAM_COHERENCY_FEATURES, DRM_PANFROST_PARAM_JS_FEATURES0, DRM_PANFROST_PARAM_NR_CORE_GROUPS,
		DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY, 9999 };
	for (unsigned i = 0; i < sizeof(nope) / sizeof(nope[0]); i++) {
		param(nope[i], &rc);
		ERR(rc, EINVAL);
	}
	struct drm_panfrost_get_param padp = { .param = DRM_PANFROST_PARAM_GPU_PROD_ID, .pad = 1 };
	ERR(io(DRM_IOCTL_PANFROST_GET_PARAM, &padp), EINVAL);
	CHECK(fake_count(0) == 2);

	/* --- CREATE_BO --- */
	uint64_t va1, va2, va3;
	uint32_t sz1, sz2, sz3;
	uint32_t b1 = mkbo(0x4000, 0, &va1, &sz1);
	CHECK(b1 == 1 && va1 == 0x2000000 && sz1 == 0x4000);
	CHECK(fake_last(1)->nin == 2 && fake_last(1)->in[0] == 0x4000 && fake_last(1)->in[1] == 1);          /* not NOEXEC -> exec */
	uint32_t b2 = mkbo(100, PANFROST_BO_NOEXEC, &va2, &sz2);
	CHECK(b2 == 2 && sz2 == 0x4000 && va2 == va1 + 0x4000 && fake_last(1)->in[0] == 100 && fake_last(1)->in[1] == 0);
	uint32_t b3 = mkbo(128u << 20, PANFROST_BO_NOEXEC | PANFROST_BO_HEAP, &va3, &sz3);
	CHECK(b3 == 3 && sz3 == (128u << 20) && fake_last(1)->in[0] == (128u << 20));                        /* heap: eager, full size */
	CHECK(mkbo(0x1000, PANFROST_BO_WB_MMAP | PANFROST_BO_NOEXEC, NULL, NULL) == 4 && fake_last(1)->in[1] == 0);
	CHECK(mkbo(0x1000, PANFROST_BO_HEAP | PANFROST_BO_WB_MMAP, NULL, NULL) == 0 && errno == EINVAL);
	CHECK(mkbo(0x1000, 8, NULL, NULL) == 0 && errno == EINVAL);
	CHECK(mkbo(0, 0, NULL, NULL) == 0 && errno == EINVAL);
	unsigned nbo = fake_count(1);
	fake.fail_sel[1] = kIOReturnNoMemory;
	CHECK(mkbo(0x1000, 0, NULL, NULL) == 0 && errno == ENOMEM);
	CHECK(fake_count(1) == nbo + 1);
	struct drm_panfrost_create_bo cpad = { .size = 1, .pad = 1 };
	ERR(io(DRM_IOCTL_PANFROST_CREATE_BO, &cpad), EINVAL);
	struct drm_panfrost_get_bo_offset go = { .handle = b2 };
	CHECK(io(DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go) == 0 && go.offset == va2);
	go.handle = 77;
	ERR(io(DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go), ENOENT);
	go.handle = 0;
	ERR(io(DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go), ENOENT);

	/* --- MMAP_BO + hooks --- */
	struct drm_panfrost_mmap_bo mm = { .handle = b1 };
	CHECK(io(DRM_IOCTL_PANFROST_MMAP_BO, &mm) == 0 && mm.offset == ((uint64_t)b1 << 32));
	void *p = darwin_drm_bo_map(devfd, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, mm.offset);
	CHECK(p != MAP_FAILED && fake.maps == 1 && fake.last_map_opt == (kIOMapAnywhere | kIOMapWriteCombineCache));
	if (p != MAP_FAILED) memset(p, 0xab, 0x4000);
	CHECK(darwin_drm_bo_map(devfd, 0x4000, PROT_READ, MAP_SHARED, mm.offset) == p && fake.maps == 1);
	CHECK(darwin_drm_bo_map(devfd, 0x8000, PROT_READ, MAP_SHARED, mm.offset) == MAP_FAILED && errno == EINVAL);   /* bigger than the BO */
	CHECK(darwin_drm_bo_map(devfd, 0x1000, PROT_READ, MAP_SHARED, mm.offset + 1) == MAP_FAILED && errno == EINVAL);
	CHECK(darwin_drm_bo_map(devfd, 0x1000, PROT_READ, MAP_SHARED, (uint64_t)50 << 32) == MAP_FAILED && errno == EINVAL);
	CHECK(darwin_drm_bo_unmap(p, 0x4000) == 0 && fake.unmaps == 1 && fake.last_unmap_addr == (uint64_t)(uintptr_t)p);
	ERR(darwin_drm_bo_unmap(p, 0x4000), EINVAL);
	ERR(darwin_drm_bo_unmap(NULL, 0), EINVAL);
	fake.map_result = kIOReturnNotPermitted;
	CHECK(darwin_drm_bo_map(devfd, 0x4000, PROT_READ, MAP_SHARED, mm.offset) == MAP_FAILED && errno == EPERM);
	fake.map_result = 0;
	struct drm_panfrost_mmap_bo mh = { .handle = b3 };
	ERR(io(DRM_IOCTL_PANFROST_MMAP_BO, &mh), EINVAL);        /* heap BOs have no CPU mapping */
	mh.handle = 99;
	ERR(io(DRM_IOCTL_PANFROST_MMAP_BO, &mh), ENOENT);
	mh.handle = b1; mh.flags = 1;
	ERR(io(DRM_IOCTL_PANFROST_MMAP_BO, &mh), EINVAL);

	/* --- SUBMIT --- */
	CHECK(submit(va1, 0, 0) == 0 && fake.seq == 1 && fake_last(3)->in[0] == va1 && fake_last(3)->in[1] == 0);
	CHECK(submit(va1 + 8, PANFROST_JD_REQ_FS, 0) == 0 && fake.seq == 2 && fake_last(3)->in[0] == va1 + 8 && fake_last(3)->in[1] == 1);
	CHECK(submit(va1, PANFROST_JD_REQ_CYCLE_COUNT, 0) == 0 && fake_last(3)->in[1] == 0);       /* only FS is passed on */
	CHECK(submit(va1, PANFROST_JD_REQ_FS | PANFROST_JD_REQ_CYCLE_COUNT, 0) == 0 && fake_last(3)->in[1] == 1);
	unsigned nsub = fake_count(3);
	ERR(submit(va1, 4, 0), EINVAL);
	ERR(submit(0, 0, 0), EINVAL);
	ERR(submit(va1, 0, 55), ENOENT);                          /* unknown out_sync: refused before the kernel is called */
	CHECK(fake_count(3) == nsub);
	struct drm_panfrost_submit sp = { .jc = va1, .pad = 3 };
	ERR(io(DRM_IOCTL_PANFROST_SUBMIT, &sp), EINVAL);
	uint32_t so1 = 0;
	CHECK(drmSyncobjCreate(devfd, 0, &so1) == 0 && so1 == 1);
	uint64_t seq_before = fake.seq;
	fake.fail_sel[3] = kIOReturnNotPermitted;
	ERR(submit(va1, 0, so1), EPERM);
	int sf = -7;
	ERR(drmSyncobjExportSyncFile(devfd, so1, &sf), EINVAL);   /* still without fence: the failed submit did not touch it */
	CHECK(submit(va1, PANFROST_JD_REQ_FS, so1) == 0 && fake.seq == seq_before + 1);
	uint64_t s_so1 = fake.seq;

	/* --- WAIT_BO: RELATIVE timeouts (patch 0003) --- */
	struct drm_panfrost_wait_bo wb = { .handle = b1, .timeout_ns = 0 };
	unsigned w0 = fake_count(4);
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb), ETIMEDOUT);
	CHECK(fake_count(4) == w0 + 1 && fake_last(4)->in[0] == s_so1 && fake_last(4)->in[1] == 0);       /* waits for the last submit, polls */
	wb.timeout_ns = 2500000;
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb), ETIMEDOUT);
	CHECK(fake_count(4) == w0 + 2 && fake_last(4)->in[1] == 2500);
	wb.timeout_ns = 1500000001;                                /* 1 s + 0.5 s + 1 ns: two slices, microseconds rounded up */
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb), ETIMEDOUT);
	CHECK(fake_count(4) == w0 + 4 && fake.calls[fake.ncalls - 2].in[1] == 1000000 && fake_last(4)->in[1] == 500001);
	wb.timeout_ns = -5;
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb), ETIMEDOUT);
	CHECK(fake_last(4)->in[1] == 0);
	wb.timeout_ns = INT64_MAX;                                 /* forever: loops over 1 s slices until done */
	fake.timeouts = 0;
	fake.complete_after_waits = 3;
	unsigned w1 = fake_count(4);
	CHECK(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb) == 0 && fake_count(4) == w1 + 4 && fake_last(4)->in[1] == 1000000);     /* 3 Timeouts, then done */
	fake.complete_after_waits = 0;
	CHECK(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb) == 0 && fake_count(4) == w1 + 4);         /* known complete: no kernel call */
	wb.handle = 99;
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wb), ENOENT);
	struct drm_panfrost_wait_bo wpad = { .handle = b1, .pad = 1 };
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wpad), EINVAL);
	CHECK(drmSyncobjWait(devfd, &so1, 1, 0, 0, NULL) == 0);       /* out_sync got the returned seqno, and it is complete now */

	/* --- syncobj wait: ABSOLUTE timeouts --- */
	uint32_t a = 0, b = 0;
	CHECK(drmSyncobjCreate(devfd, 0, &a) == 0 && drmSyncobjCreate(devfd, 0, &b) == 0);
	CHECK(submit(va1, 0, a) == 0);
	uint64_t seq_a = fake.seq;
	CHECK(submit(va1, 0, b) == 0);
	uint64_t seq_b = fake.seq;
	uint32_t first = 99;
	unsigned w2 = fake_count(4);
	ERR(drmSyncobjWait(devfd, &a, 1, g_now - 5, 0, NULL), ETIME);          /* a deadline in the past: poll */
	CHECK(fake_count(4) == w2 + 1 && fake_last(4)->in[0] == seq_a && fake_last(4)->in[1] == 0);
	ERR(drmSyncobjWait(devfd, &a, 1, g_now + 3000000, 0, NULL), ETIME);    /* 3 ms from now */
	CHECK(fake_last(4)->in[1] == 3000);
	ERR(drmSyncobjWait(devfd, &a, 1, g_now, 0, NULL), ETIME);              /* exactly now: poll */
	CHECK(fake_last(4)->in[1] == 0);
	uint32_t both[2] = { b, a };
	fake.completed = seq_a;
	CHECK(drmSyncobjWait(devfd, both, 2, g_now - 1, 0, &first) == 0 && first == 1);               /* ANY: a is done (index 1) */
	ERR(drmSyncobjWait(devfd, both, 2, g_now - 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, &first), ETIME);
	CHECK(fake_last(4)->in[0] == seq_b);                                                          /* ALL waits for the later one */
	fake.completed = seq_b;
	first = 99;
	CHECK(drmSyncobjWait(devfd, both, 2, g_now - 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, &first) == 0 && first == 0);
	fake.timeouts = 0;
	CHECK(submit(va1, 0, a) == 0);
	fake.complete_after_waits = 2;
	CHECK(drmSyncobjWait(devfd, &a, 1, INT64_MAX, 0, NULL) == 0 && fake_last(4)->in[1] == 1000000);
	fake.complete_after_waits = 0;
	uint32_t bad = 4000;
	ERR(drmSyncobjWait(devfd, &bad, 1, 0, 0, NULL), ENOENT);
	ERR(drmSyncobjWait(devfd, &a, 0, 0, 0, NULL), EINVAL);
	uint64_t pt = 1;
	ERR(drmSyncobjTimelineWait(devfd, &a, &pt, 1, 0, 0, NULL), EINVAL);
	pt = 0;
	CHECK(drmSyncobjTimelineWait(devfd, &a, &pt, 1, 0, 0, NULL) == 0);

	/* --- syncobj create / signal / reset / export / import / transfer --- */
	uint32_t sg;
	CHECK(drmSyncobjCreate(devfd, DRM_SYNCOBJ_CREATE_SIGNALED, &sg) == 0);
	unsigned w3 = fake_count(4);
	CHECK(drmSyncobjWait(devfd, &sg, 1, 0, 0, NULL) == 0 && fake_count(4) == w3);                 /* signalled: no kernel call */
	CHECK(drmSyncobjReset(devfd, &sg, 1) == 0);
	ERR(drmSyncobjWait(devfd, &sg, 1, 0, 0, NULL), EINVAL);                                       /* no fence (ponytail ceiling) */
	CHECK(drmSyncobjSignal(devfd, &sg, 1) == 0 && drmSyncobjWait(devfd, &sg, 1, 0, 0, NULL) == 0);
	CHECK(drmSyncobjReset(devfd, &sg, 1) == 0 && drmSyncobjTimelineSignal(devfd, &sg, &pt, 1) == 0 && drmSyncobjWait(devfd, &sg, 1, 0, 0, NULL) == 0);
	pt = 3;
	ERR(drmSyncobjTimelineSignal(devfd, &sg, &pt, 1), EINVAL);
	ERR(drmSyncobjCreate(devfd, 2, &sg), EINVAL);
	ERR(drmSyncobjSignal(devfd, &bad, 1), ENOENT);
	ERR(drmSyncobjQuery2(devfd, &sg, &pt, 1, 0), ENOSYS);

	CHECK(submit(va1, 0, a) == 0);                              /* a -> fresh pending seqno */
	uint64_t seq_a2 = fake.seq;
	int ef = -1, ef2 = -1;
	CHECK(drmSyncobjExportSyncFile(devfd, a, &ef) == 0 && ef >= 0 && fcntl(ef, F_GETFD) >= 0);
	uint32_t imp = 0;
	CHECK(drmSyncobjCreate(devfd, 0, &imp) == 0 && drmSyncobjImportSyncFile(devfd, imp, ef) == 0);
	close(ef);
	ERR(drmSyncobjWait(devfd, &imp, 1, 0, 0, NULL), ETIME);
	CHECK(fake_last(4)->in[0] == seq_a2);                       /* the import carried a's seqno */
	fake.completed = seq_a2;
	CHECK(drmSyncobjWait(devfd, &imp, 1, 0, 0, NULL) == 0);
	CHECK(drmSyncobjHandleToFD(devfd, a, &ef2) == 0 && ef2 >= 0);
	uint32_t back = 0;
	CHECK(drmSyncobjFDToHandle(devfd, ef2, &back) == 0 && back != 0 && back != a && drmSyncobjWait(devfd, &back, 1, 0, 0, NULL) == 0);
	ERR(drmSyncobjFDToHandle(devfd, 999, &back), EINVAL);
	ERR(drmSyncobjImportSyncFile(devfd, imp, 999), EINVAL);
	ERR(drmSyncobjImportSyncFile(devfd, 4000, ef2), ENOENT);
	ERR(drmSyncobjHandleToFD(devfd, 4000, &ef), ENOENT);
	close(ef2);
	uint32_t t1 = 0, t2 = 0;
	CHECK(drmSyncobjCreate(devfd, 0, &t1) == 0 && drmSyncobjCreate(devfd, DRM_SYNCOBJ_CREATE_SIGNALED, &t2) == 0);
	CHECK(drmSyncobjTransfer(devfd, t1, 0, t2, 0, 0) == 0 && drmSyncobjWait(devfd, &t1, 1, 0, 0, NULL) == 0);
	ERR(drmSyncobjTransfer(devfd, t1, 1, t2, 0, 0), EINVAL);
	ERR(drmSyncobjTransfer(devfd, t1, 0, 4000, 0, 0), ENOENT);
	/* destroy, double destroy, handle reuse */
	CHECK(drmSyncobjDestroy(devfd, t1) == 0);
	ERR(drmSyncobjDestroy(devfd, t1), EINVAL);
	uint32_t again = 0;
	CHECK(drmSyncobjCreate(devfd, 0, &again) == 0 && again == t1);

	/* --- BO close, double close, kernel error, reuse --- */
	unsigned c0 = fake_count(2);
	CHECK(drmCloseBufferHandle(devfd, b2) == 0 && fake_count(2) == c0 + 1 && fake_last(2)->in[0] == b2);
	ERR(drmCloseBufferHandle(devfd, b2), EINVAL);
	CHECK(fake_count(2) == c0 + 1);                             /* the double close never reached the kernel */
	ERR(drmCloseBufferHandle(devfd, 0), EINVAL);
	CHECK(mkbo(0x4000, 0, NULL, NULL) == b2);                   /* kernel reuses the lowest free slot, so do we */
	fake.fail_sel[2] = kIOReturnTimeout;
	ERR(drmCloseBufferHandle(devfd, b2), ETIMEDOUT);
	go.handle = b2;
	CHECK(io(DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go) == 0);       /* kernel kept the BO: so do we */
	CHECK(drmCloseBufferHandle(devfd, b2) == 0);
	struct drm_panfrost_mmap_bo mc = { .handle = b1 };           /* closing a mapped BO unmaps it first */
	CHECK(io(DRM_IOCTL_PANFROST_MMAP_BO, &mc) == 0);
	unsigned u0 = fake.unmaps;
	CHECK(darwin_drm_bo_map(devfd, 0x1000, PROT_READ, MAP_SHARED, mc.offset) != MAP_FAILED);
	CHECK(drmCloseBufferHandle(devfd, b1) == 0 && fake.unmaps == u0 + 1);

	/* --- other ioctls --- */
	struct drm_panfrost_madvise mad = { .handle = b3, .madv = PANFROST_MADV_DONTNEED };
	CHECK(io(DRM_IOCTL_PANFROST_MADVISE, &mad) == 0 && mad.retained == 1);
	mad.madv = 2;
	ERR(io(DRM_IOCTL_PANFROST_MADVISE, &mad), EINVAL);
	mad.madv = 0; mad.handle = 99;
	ERR(io(DRM_IOCTL_PANFROST_MADVISE, &mad), ENOENT);
	struct drm_panfrost_sync_bo sy = { 0 };
	struct drm_panfrost_query_bo_info qi = { .handle = b3 };
	struct drm_panfrost_set_label_bo sl = { .handle = b3 };
	struct drm_panfrost_jm_ctx_create jc = { 0 };
	ERR(io(DRM_IOCTL_PANFROST_SYNC_BO, &sy), ENOSYS);
	ERR(io(DRM_IOCTL_PANFROST_QUERY_BO_INFO, &qi), ENOSYS);
	ERR(io(DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), ENOSYS);
	ERR(io(DRM_IOCTL_PANFROST_JM_CTX_CREATE, &jc), ENOSYS);
	ERR(io(DRM_IOCTL_PANFROST_GET_PARAM, NULL), EFAULT);
	ERR(io(DRM_IOCTL_GEM_CLOSE + 1, &sy), EINVAL);                   /* not a driver command */
	ERR(io(0x1234, &sy), EINVAL);

	/* --- prime --- */
	char tmpl[] = "/tmp/sdm-hostXXXXXX";
	int pf = mkstemp(tmpl);
	unlink(tmpl);
	CHECK(pf >= 0 && ftruncate(pf, 0x10000) == 0);
	CHECK(sweet_drm_mali_register_surface(pf, 1234, 0x8000) == -EINVAL);   /* lseek(SEEK_END) says 0x10000 */
	CHECK(sweet_drm_mali_register_surface(pf, 1234, 0x10000) == 0);
	uint32_t ph = 0;
	CHECK(drmPrimeFDToHandle(devfd, pf, &ph) == -ENOSYS && errno == ENOSYS && ph == 0);   /* kernel selector 5 does not exist yet */
	CHECK(drmPrimeFDToHandle(devfd, pf + 100, &ph) == -EINVAL);
	sweet_drm_mali_unregister_surface(pf);
	CHECK(drmPrimeFDToHandle(devfd, pf, &ph) == -EINVAL);
	int pfd = -1;
	CHECK(drmPrimeHandleToFD(devfd, b3, DRM_CLOEXEC | DRM_RDWR, &pfd) == -ENOSYS);
	close(pf);

	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
