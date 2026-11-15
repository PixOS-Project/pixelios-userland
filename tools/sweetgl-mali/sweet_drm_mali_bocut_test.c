/* Host test for the BO churn cut and counters of sweet_drm_mali.c (P-G1 slice 6, T-2275). Run by host-test.sh in its own processes (the knobs are read once per
 * process): argv[1] = "cut" (SWEETGL_BOCUT=1 SWEETGL_BOCNT=1), "off" (neither set), "bad" (SWEETGL_BOCNT=yes SWEETGL_BOCUT=2). stderr is redirected to a file and read back. */
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
static int io(unsigned long req, void *arg) { return drmIoctl(devfd, req, arg); }
static char errpath[] = "/tmp/sdm-bocut-XXXXXX";

static uint32_t
mkbo(uint32_t size)
{
	struct drm_panfrost_create_bo r = { .size = size, .flags = PANFROST_BO_NOEXEC };
	return io(DRM_IOCTL_PANFROST_CREATE_BO, &r) ? 0 : r.handle;
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

/* whole stderr file, NUL terminated */
static char *
errtext(void)
{
	static char buf[16384];
	FILE *f = fopen(errpath, "r");
	size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
	if (f) fclose(f);
	buf[n] = 0;
	return buf;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	int efd = mkstemp(errpath);
	if (efd < 0 || dup2(efd, 2) < 0) { printf("FAIL cannot redirect stderr\n"); return 1; }
	fake_reset();
	if (!strcmp(mode, "cut")) { setenv("SWEETGL_BOCUT", "1", 1); setenv("SWEETGL_BOPOOL", "0", 1); setenv("SWEETGL_BOCNT", "1", 1); }   /* cut alone: the pool is default ON since T-bf4f */
	else if (!strcmp(mode, "bad")) { setenv("SWEETGL_BOPOOL", "0", 1); setenv("SWEETGL_BOCUT", "2", 1); setenv("SWEETGL_BOCNT", "yes", 1); }
	else if (!strcmp(mode, "off")) { setenv("SWEETGL_BOCUT", "0", 1); setenv("SWEETGL_BOPOOL", "0", 1); unsetenv("SWEETGL_BOCNT"); }
	else { printf("FAIL usage: mode cut|off|bad\n"); return 1; }
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);
	int cut = !strcmp(mode, "cut");

	uint32_t a = mkbo(0x4000), b = mkbo(0x4000), c = mkbo(0x4000);
	CHECK(a == 1 && b == 2 && c == 3);
	/* submit 1 lists a and b, submit 2 lists b only (seq 1 and 2); the fake kernel completes seq <= fake.completed */
	uint32_t l1[2] = { a, b }, l2[1] = { b }, bad[2] = { c, 99 };
	CHECK(submit_bos(l1, 2) == 0 && fake.seq == 1);
	CHECK(submit_bos(l2, 1) == 0 && fake.seq == 2);
	fake.completed = 1;                       /* the GPU finished submit 1 only */
	if (cut) {
		CHECK(wait_poll(a) == 0);             /* a was last listed by seq 1: done */
		ERR(wait_poll(b), ETIMEDOUT);         /* b by seq 2: busy */
		CHECK(wait_poll(c) == 0);             /* never listed: ready at once, no kernel call */
	} else {
		ERR(wait_poll(a), ETIMEDOUT);         /* device-wide: the last submit (2) is not done, so even a looks busy */
		ERR(wait_poll(c), ETIMEDOUT);
	}
	fake.completed = 2;
	CHECK(wait_poll(a) == 0 && wait_poll(b) == 0 && wait_poll(c) == 0);
	/* an unknown handle in the list does not fail the submit and is counted */
	CHECK(submit_bos(bad, 2) == 0 && fake.seq == 3);
	CHECK(submit_bos(NULL, 0) == 0 && fake.seq == 4);       /* no list: nothing to record */
	struct drm_panfrost_wait_bo wnoent = { .handle = 77 };
	ERR(io(DRM_IOCTL_PANFROST_WAIT_BO, &wnoent), ENOENT);

	/* unmap: kept with the cut (a re-map is a hit, no kernel map call), dropped otherwise; close always drops it */
	void *p = mapbo(a, 0x4000);
	CHECK(p != MAP_FAILED && fake.maps == 1);
	CHECK(darwin_drm_bo_unmap(p, 0x4000) == 0);
	if (cut) {
		CHECK(fake.unmaps == 0);
		CHECK(mapbo(a, 0x4000) == p && fake.maps == 1);
		CHECK(darwin_drm_bo_unmap(p, 0x4000) == 0 && fake.unmaps == 0);
		ERR(darwin_drm_bo_unmap((char *)p + 0x100000, 0x1000), EINVAL);     /* an address we never handed out is still refused */
	} else {
		CHECK(fake.unmaps == 1);
		CHECK(mapbo(a, 0x4000) != MAP_FAILED && fake.maps == 2);
		CHECK(darwin_drm_bo_unmap(p, 0x4000) == 0 && fake.unmaps == 2);
		/* L-01: a failed kernel unmap is reported to the caller and the record keeps the mapping (it is still in the task): the next map is the same address
		 * without a second kernel map, and the next unmap really drops it */
		CHECK(mapbo(a, 0x4000) == p && fake.maps == 3);
		fake.unmap_result = kIOReturnBusy;
		ERR(darwin_drm_bo_unmap(p, 0x4000), EBUSY);
		CHECK(fake.unmaps == 2 && fake.unmap_fails == 1 && fake.live_maps == 1);
		CHECK(mapbo(a, 0x4000) == p && fake.maps == 3);
		CHECK(darwin_drm_bo_unmap(p, 0x4000) == 0 && fake.unmaps == 3 && fake.live_maps == 0);
	}
	CHECK(mapbo(b, 0x4000) != MAP_FAILED);
	unsigned unmaps_before = fake.unmaps;
	CHECK(drmCloseBufferHandle(devfd, b) == 0);
	CHECK(fake.unmaps == unmaps_before + 1);                  /* close_bo drops a mapping that is still there */
	CHECK(drmCloseBufferHandle(devfd, a) == 0);
	CHECK(fake.unmaps == unmaps_before + (cut ? 2 : 1));      /* a: kept map dropped at close (cut) / already unmapped */

	sleep(2);                                                 /* a window boundary; the next ioctl prints the window that ended */
	struct drm_panfrost_get_bo_offset go = { .handle = c };
	CHECK(io(DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go) == 0);
	const char *t = errtext();
	if (cut) {
		CHECK(strstr(t, "SWEETGL: BOCNT on 1 cut 1 pid "));
		CHECK(strstr(t, "SWEETGL: BOCNT pid "));
		CHECK(strstr(t, " cut 1 create 3 create_bytes 49152 create_fail 0 close 2 map 2 map_hit 1 map_bytes 32768 unmap 2 unmap_kept 2 submit 4 submit_bos 5 submit_unk_bo 1 "));
		CHECK(strstr(t, " wait 6 wait_ready 5 wait_busy 1 wait_err 0 "));
		CHECK(strstr(t, " live_bos 1 unmap_fail 0 unmap_fail_bytes 0 unmap_fail_total 0\n"));
		CHECK(!strstr(t, "unavailable"));
	} else if (!strcmp(mode, "off")) {
		/* nothing is logged when the knobs are off, except the one unconditional GPU props line (87260fa) and the injected unmap failure, which is loud without any knob */
		static const char *const ok[] = { "SWEETGL: GPU props ", "SWEET GAP: mali BO CPU unmap failed, the mapping stays in the task (leak) x1 (?)\n", "SWEETGL: UNMAPFAIL pid " };
		unsigned seen[3] = { 0, 0, 0 }, other = 0;
		for (const char *l = t; *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l)) {
			int k = 0;
			while (k < 3 && strncmp(l, ok[k], strlen(ok[k])) != 0) k++;
			if (k < 3) seen[k]++; else other++;
		}
		CHECK(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && other == 0);
		CHECK(strstr(t, " n 1 where bo_unmap handle 1 kr 0xe00002d5 bytes 16384 total_bytes 16384\n"));
		CHECK(fake.unmap_fails == 1 && fake.orphan_maps == 0);
	} else {
		CHECK(strstr(t, "SWEETGL: BOCNT unavailable: SWEETGL_BOCNT=\"yes\" is not 0 or 1, knob OFF"));
		CHECK(strstr(t, "SWEETGL: BOCNT unavailable: SWEETGL_BOCUT=\"2\" is not 0 or 1, knob OFF"));
		CHECK(!strstr(t, "BOCNT pid "));                      /* both OFF: behaves like "off" */
		ERR(wait_poll(c), ETIMEDOUT);                       /* OFF means the old device-wide wait: submit 4 is not done */
	}
	unlink(errpath);
	printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
	return fails != 0;
}
