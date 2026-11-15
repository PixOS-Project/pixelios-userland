/* Host test for the overlap pass-through of sweet_drm_mali.c (T-34eb, SWEET_MALI_REQ_OVL) and for the overlap rule of Mesa patch 0008 (pan_sweet_ovl.h, R0-R4 of
 * docs/reviews/T-34eb-proof.md H1). argv[1] = shim | rule. The header comes from the patched Mesa tree (host-test.sh: -I OVL_DIR; mutation-check.sh points OVL_DIR at a
 * broken copy). shim: stderr is redirected to a file and read back; BOCNT=1 makes the counter line print on the next second. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fake_iokit.h"
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "darwin_drm.h"
#include "sweet_drm_mali.h"
#include "pan_sweet_ovl.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int64_t os_time_get_nano(void) { return 0; }

static int devfd = -1;
static char errpath[] = "/tmp/sdm-ovl-XXXXXX";
static char *errtext(void)
{
	static char buf[1 << 16];
	FILE *f = fopen(errpath, "r");
	size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
	if (f) fclose(f);
	buf[n] = 0;
	return buf;
}
static unsigned count_lines(const char *pat)
{
	unsigned n = 0;
	for (const char *p = strstr(errtext(), pat); p; p = strstr(p + 1, pat)) n++;
	return n;
}
static int submit(uint32_t req)
{
	struct drm_panfrost_submit s = { .jc = 0x2000000, .requirements = req };
	return drmIoctl(devfd, DRM_IOCTL_PANFROST_SUBMIT, &s);
}
#define LASTIN1() (fake_last(3)->in[1])

static void shim_test(void)
{
	setenv("SWEETGL_BOCNT", "1", 1);
	setenv("SWEETGL_BOCUT", "0", 1);
	setenv("SWEETGL_BOPOOL", "0", 1);
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);
	CHECK(PANFROST_JD_REQ_SWEET_OVL == (1u << 31));
	/* 1: no previous submit: the bit is dropped (the kernel gets in[1] == 0) */
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL) == 0 && fake.seq == 1 && LASTIN1() == 0);
	CHECK(count_lines("SWEETGL: OVL dropped") == 1);
	CHECK(strstr(errtext(), "request is not an FS submit, previous submit was not an FS submit") != NULL);
	/* 2: FS, then a non-FS request with the bit: forwarded as kernel bit 2 */
	CHECK(submit(PANFROST_JD_REQ_FS) == 0 && LASTIN1() == 1);
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL) == 0 && fake.seq == 3 && LASTIN1() == 2);
	CHECK(fake_last(3)->in[0] == 0x2000000);
	/* 3: the previous submit was the forwarded one (not FS): dropped, plain */
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL) == 0 && LASTIN1() == 0);
	/* 4: an FS request carrying the bit never forwards it, but it is still an FS submit */
	CHECK(submit(PANFROST_JD_REQ_FS | PANFROST_JD_REQ_SWEET_OVL) == 0 && LASTIN1() == 1);
	CHECK(submit(PANFROST_JD_REQ_FS) == 0);                 /* the same, right after an FS submit: still no bit 2 on an FS job */
	CHECK(submit(PANFROST_JD_REQ_FS | PANFROST_JD_REQ_SWEET_OVL) == 0 && LASTIN1() == 1);
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL | PANFROST_JD_REQ_CYCLE_COUNT) == 0 && LASTIN1() == 2);   /* cycle count is not passed on; the bit is (prev was FS) */
	/* 5: a plain non-FS submit between: the next bit is dropped */
	CHECK(submit(PANFROST_JD_REQ_FS) == 0);
	CHECK(submit(0) == 0 && LASTIN1() == 0);
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL) == 0 && LASTIN1() == 0);
	/* 6: the previous FS submit failed in the kernel: no forwarding either */
	CHECK(submit(PANFROST_JD_REQ_FS) == 0);
	fake.fail_sel[3] = 1;                         /* one-shot kIOReturnError on submit */
	CHECK(submit(PANFROST_JD_REQ_FS) != 0);
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL) == 0 && LASTIN1() == 0);
	/* 7: a failing OVL request is not counted as forwarded */
	CHECK(submit(PANFROST_JD_REQ_FS) == 0);
	fake.fail_sel[3] = 1;
	CHECK(submit(PANFROST_JD_REQ_SWEET_OVL) != 0);
	/* 8: unknown requirement bits are still refused */
	CHECK(submit(1u << 30) == -1 && errno == EINVAL);
	/* totals: forwarded 2 (steps 2 and 4); dropped 6 (steps 1, 3, 4 with the FS request twice, 5, 6); the failed step 7 is counted as neither */
	sleep(2);                                     /* a window boundary; the next ioctl prints the window that ended */
	CHECK(submit(PANFROST_JD_REQ_FS) == 0);
	const char *t = errtext();
	CHECK(strstr(t, " submit_ovl 2 submit_ovl_drop 6 ") != NULL);
	CHECK(count_lines("SWEETGL: OVL dropped") == 1);                /* the loud line comes once */
}

/* ---- rule ---- */
static const uint32_t A_WR[] = { 4, 9 }, A_ALL[] = { 2, 4, 7, 9, 12 };
static const uint32_t B_VT[] = { 3, 7, 20 }, B_VTW[] = { 20 };
static const uint32_t NONE[] = { 0 };

static struct sweet_ovl_prev prev_ok(const void *dev)
{
	return (struct sweet_ovl_prev){ .dev = dev, .valid = true, .half = 0, .wr = A_WR, .n_wr = 2, .all = A_ALL, .n_all = 5 };
}
static struct sweet_ovl_cur cur_ok(const void *dev)
{
	return (struct sweet_ovl_cur){ .dev = dev, .has_tiler = true, .half = 1, .in_sync = false, .vt = B_VT, .n_vt = 3, .vtw = B_VTW, .n_vtw = 1 };
}

static void rule_test(void)
{
	int d1, d2;
	struct sweet_ovl_prev p = prev_ok(&d1);
	struct sweet_ovl_cur c = cur_ok(&d1);
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_SAFE);
	/* R0: no valid previous FS / another device */
	p.valid = false;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_NO_PREV_FS);
	p = prev_ok(&d2);
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_NO_PREV_FS);
	/* R1: same half, unknown half of either side */
	p = prev_ok(&d1); p.half = 1;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_HALF_SAME);
	p = prev_ok(&d1); p.half = -1;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_HALF_UNKNOWN);
	p = prev_ok(&d1); c.half = -1;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_HALF_UNKNOWN);
	c = cur_ok(&d1); c.half = 0;                       /* both halves 0 */
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_HALF_SAME);
	/* a compute-only B (no tiler job) ignores R1 entirely: same, unknown on both sides */
	c = cur_ok(&d1); c.has_tiler = false; c.half = -1; p.half = -1;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_SAFE);
	c.half = 0; p.half = 0;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_SAFE);
	/* R2: B's vertex/tiler BO 4 is in A's write set */
	static const uint32_t vt2[] = { 3, 4, 20 };
	p = prev_ok(&d1); c = cur_ok(&d1); c.vt = vt2;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_R2);
	/* R2 hit at the last element of both sorted arrays (merge walk reaches the end) */
	static const uint32_t vt2e[] = { 1, 9 };
	c = cur_ok(&d1); c.vt = vt2e; c.n_vt = 2;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_R2);
	/* R3: B writes BO 12 in the vertex/compute stage, A only reads it */
	static const uint32_t vtw3[] = { 12 };
	{
		static const uint32_t vt3[] = { 3, 12, 20 };         /* vtw is a subset of vt */
		c = cur_ok(&d1); c.vt = vt3; c.vtw = vtw3; c.n_vtw = 1;
		CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_R3);
	}
	/* a BO B only READS in the vertex stage that A only reads is fine */
	{
		static const uint32_t vt4[] = { 2, 12 };
		c = cur_ok(&d1); c.vt = vt4; c.n_vt = 2; c.vtw = NONE; c.n_vtw = 0;
		CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_SAFE);
	}
	/* R4: in_sync */
	c = cur_ok(&d1); c.in_sync = true;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_INSYNC);
	/* empty sets are safe */
	c = cur_ok(&d1); c.n_vt = c.n_vtw = 0; p.n_wr = p.n_all = 0;
	CHECK(sweet_ovl_check(&p, &c) == SWEET_OVL_SAFE);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	int efd = mkstemp(errpath);
	if (efd < 0 || dup2(efd, 2) < 0) { printf("FAIL cannot redirect stderr\n"); return 1; }
	fake_reset();
	if (!strcmp(mode, "shim")) shim_test();
	else if (!strcmp(mode, "rule")) rule_test();
	else { printf("FAIL usage: mode shim|rule\n"); return 1; }
	printf("%s: %d checks, %d failed\n", mode, checks, fails);
	unlink(errpath);
	if (fails) return 1;
	printf("RESULT: PASS ovl %s\n", mode);
	return 0;
}
