/* Host test for the BO recycle pool and size classes of sweet_drm_mali.c (P-G1 slice 7, T-bf4f). Run by host-test.sh in its own processes (the knobs are read once
 * per process): argv[1] = "pool" (SWEETGL_BOCNT=1 SWEETGL_BOCUT=1 SWEETGL_BOPOOL=1), "nocut" (BOCNT=1 BOPOOL=1, no BOCUT: the pool must refuse itself loudly),
 * "off" (BOCNT=1 BOCUT=1, no BOPOOL: behaves like slice 6). stderr is redirected to a file and read back; counters are summed over every window line. */
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

int64_t os_time_get_nano(void) { return 0; }

static int devfd = -1;
static unsigned unmap_fail_want;
static int io(unsigned long req, void *arg) { return drmIoctl(devfd, req, arg); }
static char errpath[] = "/tmp/sdm-bopool-XXXXXX";

static int
mk(uint32_t size, uint32_t flags, struct drm_panfrost_create_bo *r)
{
	*r = (struct drm_panfrost_create_bo){ .size = size, .flags = flags };
	return io(DRM_IOCTL_PANFROST_CREATE_BO, r);
}

static int
submit_bos(const uint32_t *hs, uint32_t n)
{
	struct drm_panfrost_submit s = { .jc = 0x2000000, .bo_handles = (uint64_t)(uintptr_t)hs, .bo_handle_count = n };
	return io(DRM_IOCTL_PANFROST_SUBMIT, &s);
}

static int
wait_poll(uint32_t h)
{
	struct drm_panfrost_wait_bo w = { .handle = h, .timeout_ns = 0 };
	return io(DRM_IOCTL_PANFROST_WAIT_BO, &w);
}

static void *
mapbo(uint32_t h, size_t sz)
{
	struct drm_panfrost_mmap_bo m = { .handle = h };
	if (io(DRM_IOCTL_PANFROST_MMAP_BO, &m)) {
		return MAP_FAILED;
	}
	return darwin_drm_bo_map(devfd, sz, PROT_READ | PROT_WRITE, MAP_SHARED, m.offset);
}

static char *
errtext(void)
{
	static char buf[65536];
	FILE *f = fopen(errpath, "r");
	size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
	if (f) fclose(f);
	buf[n] = 0;
	return buf;
}

/* sum of " key N" over every line that starts (after the log prefix) with `prefix` */
static unsigned long long
sum(const char *prefix, const char *key)
{
	unsigned long long t = 0;
	char pat[64];
	snprintf(pat, sizeof pat, " %s ", key);
	for (const char *l = strstr(errtext(), prefix); l; l = strstr(l + 1, prefix)) {
		const char *eol = strchr(l, '\n');
		const char *k = strstr(l, pat);
		if (k && (!eol || k < eol)) {
			t += strtoull(k + strlen(pat), NULL, 10);
		}
	}
	return t;
}

/* closes and creates that reached the fake kernel */
static unsigned kcloses(void) { return fake_count(2); }
static unsigned kcreates(void) { return fake_count(1); }

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	int efd = mkstemp(errpath);
	if (efd < 0 || dup2(efd, 2) < 0) { printf("FAIL cannot redirect stderr\n"); return 1; }
	fake_reset();
	setenv("SWEETGL_BOCNT", "1", 1);
	if (!strcmp(mode, "pool")) { setenv("SWEETGL_BOCUT", "1", 1); setenv("SWEETGL_BOPOOL", "1", 1); }
	else if (!strcmp(mode, "nocut")) { setenv("SWEETGL_BOCUT", "0", 1); setenv("SWEETGL_BOPOOL", "1", 1); }
	else if (!strcmp(mode, "off")) { setenv("SWEETGL_BOCUT", "1", 1); setenv("SWEETGL_BOPOOL", "0", 1); }
	else if (!strcmp(mode, "default")) { unsetenv("SWEETGL_BOCUT"); unsetenv("SWEETGL_BOPOOL"); }   /* T-bf4f: both default ON, only an explicit 0 turns them off */
	else { printf("FAIL usage: mode pool|nocut|off|default\n"); return 1; }
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);
	int pool = !strcmp(mode, "pool") || !strcmp(mode, "default");
	struct drm_panfrost_create_bo r;

	if (pool) {
		/* an IOSurface import is never pooled: releasing it closes the kernel BO at once (checked first: with a pool full of entries any put would also evict) */
		fake_add_surface(1234, 0x10000, 4096, 256, 64, 0x42475241);
		struct sweet_drm_surface si;
		unsigned k3 = kcloses();
		CHECK(sweet_drm_mali_import_surface(1234, &si) == 0 && si.handle != 0);
		CHECK(sweet_drm_mali_release_surface(1234) == 0 && kcloses() == k3 + 1);
	}
	/* size classes: 300000 -> kernel 16 KB steps give 311296, the class (step 32768) 327680; only with the pool on */
	CHECK(mk(300000, PANFROST_BO_NOEXEC, &r) == 0);
	CHECK(r.size == (pool ? 327680u : 311296u));
	uint32_t big = r.handle;
	CHECK(mk(300000, PANFROST_BO_NOEXEC | PANFROST_BO_HEAP, &r) == 0);
	CHECK(r.size == 311296u);                                /* heap BOs are never rounded */
	uint32_t heap = r.handle;
	const unsigned kc0 = kcloses();
	CHECK(drmCloseBufferHandle(devfd, heap) == 0 && kcloses() == kc0 + 1);     /* and never pooled */
	CHECK(drmCloseBufferHandle(devfd, big) == 0);
	CHECK(kcloses() == kc0 + (pool ? 1u : 2u));                    /* pool: kept alive, no kernel close */
	CHECK(fake.live_bos == (pool ? 1u : 0u));
	if (pool) {
		ERR(wait_poll(big), ENOENT);                         /* a pooled handle is invisible to Mesa */
		struct drm_panfrost_get_bo_offset go = { .handle = big };
		ERR(io(DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go), ENOENT);
		ERR(drmCloseBufferHandle(devfd, big), EINVAL);       /* and cannot be closed twice */
		/* a same-class request takes it back: no kernel create, the same handle, VA and size */
		unsigned c0 = kcreates();
		CHECK(mk(310000, PANFROST_BO_NOEXEC, &r) == 0);
		CHECK(r.handle == big && r.size == 327680u && kcreates() == c0);
		CHECK(wait_poll(big) == 0);                          /* seq was reset */
		CHECK(drmCloseBufferHandle(devfd, big) == 0);
		/* different EXEC flag: a miss */
		CHECK(mk(310000, 0, &r) == 0 && r.handle != big && kcreates() == c0 + 1);
		uint32_t ex = r.handle;
		/* a different class: a miss */
		CHECK(mk(400000, PANFROST_BO_NOEXEC, &r) == 0 && r.handle != big && kcreates() == c0 + 2);
		uint32_t other = r.handle;
		/* a busy pooled BO is skipped (never waited for), a finished one is taken */
		void *p = mapbo(other, 0x1000);
		CHECK(p != MAP_FAILED && fake.maps == 1);
		uint32_t l[1] = { other };
		CHECK(submit_bos(l, 1) == 0);
		uint64_t s = fake.seq;
		fake.completed = s - 1;
		CHECK(darwin_drm_bo_unmap(p, 0x1000) == 0);
		CHECK(drmCloseBufferHandle(devfd, other) == 0);
		unsigned c1 = kcreates();
		CHECK(mk(400000, PANFROST_BO_NOEXEC, &r) == 0 && r.handle != other && kcreates() == c1 + 1);   /* busy: fresh BO */
		uint32_t fresh = r.handle;
		fake.fail_sel[4] = kIOReturnBadArgument;             /* the poll itself fails once: skipped, counted as an error, never as busy */
		CHECK(mk(400000, PANFROST_BO_NOEXEC, &r) == 0 && r.handle != other && kcreates() == c1 + 2);
		CHECK(drmCloseBufferHandle(devfd, r.handle) == 0);   /* a second idle BO of the class joins the pool after `other` */
		c1++;
		fake.completed = s;
		CHECK(drmCloseBufferHandle(devfd, fresh) == 0);
		CHECK(mk(400000, PANFROST_BO_NOEXEC, &r) == 0 && r.handle == other && kcreates() == c1 + 1);    /* done: recycled, oldest first */
		CHECK(mapbo(other, 0x1000) == p && fake.maps == 1);  /* the kept CPU mapping comes back with it */
		CHECK(drmCloseBufferHandle(devfd, other) == 0);
		(void)ex;
		/* byte cap: 24 MB BOs, three do not fit in 64 MB, the oldest is really closed */
		unsigned k0 = kcloses();
		uint32_t m[3];
		for (int i = 0; i < 3; i++) {
			CHECK(mk(24u << 20, PANFROST_BO_NOEXEC, &r) == 0 && r.size == (24u << 20));
			m[i] = r.handle;
		}
		for (int i = 0; i < 3; i++) {
			CHECK(drmCloseBufferHandle(devfd, m[i]) == 0);
		}
		CHECK(kcloses() == k0 + 5);                       /* 73.5 MB would be pooled: the 4 small entries (big, fresh, fresh2, other) and the first 24 MB are really closed */
		/* count cap: fill with small BOs, a failed eviction is counted and the pool stays over nothing it cannot hold */
		uint32_t sm[140];
		for (int i = 0; i < 140; i++) {
			CHECK(mk(16384, PANFROST_BO_NOEXEC, &r) == 0);
			sm[i] = r.handle;
		}
		unsigned k1 = kcloses();
		for (int i = 0; i < 128; i++) {
			CHECK(drmCloseBufferHandle(devfd, sm[i]) == 0);
		}
		CHECK(kcloses() > k1);                               /* the earlier 24 MB entries and the first small ones were evicted to fit */
		unsigned k2 = kcloses();
		fake.fail_sel[2] = kIOReturnBusy;                    /* the next kernel close fails once: the eviction */
		CHECK(drmCloseBufferHandle(devfd, sm[128]) == 0);
		CHECK(kcloses() == k2 + 2);                          /* the failed eviction + the new BO closed for real */
		for (int i = 129; i < 140; i++) {
			CHECK(drmCloseBufferHandle(devfd, sm[i]) == 0);
		}
		/* L-01: BOs with a CPU mapping leave through pool eviction. The mapping must be dropped BEFORE the kernel close: afterwards the kernel cannot find the BO and the
		 * mapping stays in the task for good. Three mapped 24 MB BOs are pooled (the cut keeps their maps), then 128 unmapped small BOs push every older entry out. */
		uint32_t lm[3];
		for (int i = 0; i < 3; i++) {
			CHECK(mk(24u << 20, PANFROST_BO_NOEXEC, &r) == 0);
			lm[i] = r.handle;
			void *q = mapbo(lm[i], 0x1000);
			CHECK(q != MAP_FAILED);
			CHECK(darwin_drm_bo_unmap(q, 0x1000) == 0);          /* Mesa's unmap before its cache put: kept by the cut */
		}
		CHECK(fake.live_maps >= 3);
		for (int i = 0; i < 3; i++) {
			CHECK(drmCloseBufferHandle(devfd, lm[i]) == 0);
		}
		uint32_t fl[128];
		for (int i = 0; i < 128; i++) {
			CHECK(mk(20000, PANFROST_BO_NOEXEC, &r) == 0);         /* a class of its own (32 KB): never a pool hit on the 16 KB entries */
			fl[i] = r.handle;
		}
		for (int i = 0; i < 128; i++) {
			CHECK(drmCloseBufferHandle(devfd, fl[i]) == 0);
		}
		if (fake.unmap_fails || fake.live_maps || fake.orphan_maps) {
			printf("L-01 evict: unmap_fails %u live_maps %u orphan_maps %u (want 0 0 0)\n", fake.unmap_fails, fake.live_maps, fake.orphan_maps);
		}
		CHECK(fake.unmap_fails == 0);                        /* no unmap ever reached a closed BO (also covers the evictions of the earlier steps) */
		CHECK(fake.live_maps == 0);                          /* every pooled BO left is unmapped, so no mapping may be left in the task */
		CHECK(fake.orphan_maps == 0);                        /* and no BO was closed with its mapping still live */
		/* a failing unmap is never silent: one injected failure on an eviction is logged and counted, the BO is still closed */
		CHECK(mk(24u << 20, PANFROST_BO_NOEXEC, &r) == 0);
		uint32_t uf = r.handle;
		CHECK(mapbo(uf, 0x1000) != MAP_FAILED);
		CHECK(drmCloseBufferHandle(devfd, uf) == 0);
		fake.unmap_result = kIOReturnNotFound;
		unsigned k4 = kcloses();
		for (int i = 0; i < 128; i++) {
			CHECK(mk(40000, PANFROST_BO_NOEXEC, &r) == 0);
			fl[i] = r.handle;
		}
		for (int i = 0; i < 128; i++) {
			CHECK(drmCloseBufferHandle(devfd, fl[i]) == 0);
		}
		CHECK(fake.unmap_fails == 1 && fake.orphan_maps == 1 && kcloses() == k4 + 128);
		unmap_fail_want = 1;
		/* L-01 class: darwin_drm_bo_map refuses a mapping shorter than asked and drops it. (a) the drop works: nothing left in the task, nothing counted or logged,
		 * and the record holds no mapping (the next map is a fresh kernel map, never a hit on the short one). */
		CHECK(mk(3u << 20, PANFROST_BO_NOEXEC, &r) == 0);
		uint32_t sh = r.handle;
		unsigned m0 = fake.maps, u0 = fake.unmaps, l0 = fake.live_maps;
		fake.map_short = 0x1000;
		errno = 0;
		CHECK(mapbo(sh, 0x4000) == MAP_FAILED && errno == EINVAL);
		CHECK(fake.maps == m0 + 1 && fake.unmaps == u0 + 1 && fake.live_maps == l0 && fake.unmap_fails == 1);
		/* (b) the drop fails: the short mapping stays in the task; that is loud and counted with its own bytes, and still never handed out */
		fake.map_short = 0x1000;
		fake.unmap_result = kIOReturnBusy;
		errno = 0;
		CHECK(mapbo(sh, 0x4000) == MAP_FAILED && errno == EINVAL);
		CHECK(fake.maps == m0 + 2 && fake.unmaps == u0 + 1 && fake.live_maps == l0 + 1 && fake.unmap_fails == 2);
		CHECK(mapbo(sh, 0x4000) != MAP_FAILED && fake.maps == m0 + 3);   /* a full-size mapping by a new kernel map, not the leaked short one from the record */
		unmap_fail_want = 2;
	}
	sleep(2);                                                /* a window boundary; the next ioctl prints the window that ended */
	struct drm_panfrost_get_param gp = { .param = DRM_PANFROST_PARAM_GPU_PROD_ID };
	CHECK(io(DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0);
	const char *t = errtext();
	if (pool) {
		CHECK(strstr(t, "SWEETGL: BOPOOL on pid "));
		CHECK(strstr(t, "SWEETGL: BOPOOL pid "));
		CHECK(strstr(t, "SWEETGL: BOSZ pid "));
		CHECK(!strstr(t, "unavailable"));
		CHECK(sum("SWEETGL: BOPOOL pid ", "hit") == 2);
		CHECK(sum("SWEETGL: BOPOOL pid ", "hit_bytes") == 327680ull + 425984ull);
		CHECK(sum("SWEETGL: BOPOOL pid ", "busy") == 1);
		CHECK(sum("SWEETGL: BOPOOL pid ", "evict_fail") == 1);
		CHECK(sum("SWEETGL: BOPOOL pid ", "err") == 1);
		CHECK(sum("SWEETGL: BOPOOL pid ", "put") >= 128);
		CHECK(sum("SWEETGL: BOPOOL pid ", "evict") >= 2);
		CHECK(sum("SWEETGL: BOCNT pid ", "create") == kcreates());      /* the BOCNT create count is kernel creates only */
		CHECK(sum("SWEETGL: BOCNT pid ", "close") + 1 == kcloses());   /* the one injected failed close is not counted */
		CHECK(sum("SWEETGL: BOCNT pid ", "unmap_fail") == unmap_fail_want);     /* L-01: only the two injected ones (eviction, short map) */
		CHECK(sum("SWEETGL: BOCNT pid ", "unmap_fail_bytes") == (24ull << 20) + 0x1000);
		CHECK(strstr(t, "SWEET GAP: mali BO CPU unmap failed, the mapping stays in the task (leak) x1 (?)\n"));
		CHECK(strstr(t, "SWEET GAP: mali BO CPU unmap failed, the mapping stays in the task (leak) x2 (?)\n"));
		CHECK(!strstr(t, "(leak) x3 "));
		CHECK(strstr(t, " n 2 where bo_map_short handle ") && strstr(t, " kr 0xe00002d5 bytes 4096 total_bytes 25169920\n"));
		CHECK(strstr(t, "SWEETGL: UNMAPFAIL pid ") && strstr(t, " n 1 where pool_evict handle ") && strstr(t, " kr 0xe00002f0 bytes 25165824 total_bytes 25165824\n"));
	} else if (!strcmp(mode, "nocut")) {
		CHECK(strstr(t, "SWEETGL: BOCNT unavailable: SWEETGL_BOPOOL=1 needs SWEETGL_BOCUT=1 (per-BO seqno), pool OFF"));
		CHECK(!strstr(t, "SWEETGL: BOPOOL"));
	} else {
		CHECK(!strstr(t, "SWEETGL: BOPOOL"));
		CHECK(strstr(t, "SWEETGL: BOSZ pid "));              /* the size histogram follows BOCNT */
	}
	unlink(errpath);
	printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
	return fails != 0;
}
