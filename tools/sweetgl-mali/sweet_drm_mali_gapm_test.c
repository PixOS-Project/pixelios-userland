/* Host test for the Mesa time marks of sweet_drm_mali.c (P-G1 slice 12b, T-6717): sweet_gapt_mark(1..3) + the GAPM rows that split a submit -> wait gap.
 * One process per mode (the knob is read once): argv[1] = "on" (SWEETGL_GAPT=1, fake clock), "off" (unset), "bad" (SWEETGL_GAPT=1, bad mark ids).
 * Fake clock: every read returns T and then adds STEP (1 us), including the read inside sweet_gapt_mark, so every segment is known exactly (see seg_expect).
 * The Mesa side is not built here: the test plays Mesa by calling sweet_gapt_mark() between its own kernel calls. stderr is redirected to a file and read back. */
#include <errno.h>
#include <pthread.h>
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

extern uint64_t (*sweet_gapt_clock)(void);
void sweet_gapt_mark(int id);

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int64_t os_time_get_nano(void) { return 0; }

#define STEP 1000ull
static uint64_t T = 1000000000ull;
static uint64_t clk(void) { uint64_t r = T; T += STEP; return r; }
static void jump_us(uint64_t us) { T += us * 1000ull; }
static void back_us(uint64_t us) { T -= us * 1000ull; }       /* the fake clock goes backwards: the only way to put a mark before the submit return or after the wait entry */

static int devfd = -1;
static char errpath[] = "/tmp/sdm-gapm-XXXXXX";
static uint32_t bo0;

static int mk(void)
{
	struct drm_panfrost_create_bo r = { .size = 16384, .flags = PANFROST_BO_NOEXEC };
	int e = drmIoctl(devfd, DRM_IOCTL_PANFROST_CREATE_BO, &r);
	bo0 = r.handle;
	return e;
}
static int submit(void)
{
	uint32_t h = bo0;
	struct drm_panfrost_submit s = { .jc = 0x2000000, .bo_handles = (uint64_t)(uintptr_t)&h, .bo_handle_count = 1 };
	return drmIoctl(devfd, DRM_IOCTL_PANFROST_SUBMIT, &s);
}
static int wait_bo(void)       /* nothing completes in the fake: one kernel wait that times out (kind wait) */
{
	struct drm_panfrost_wait_bo w = { .handle = bo0, .timeout_ns = 0 };
	return drmIoctl(devfd, DRM_IOCTL_PANFROST_WAIT_BO, &w);
}

static char *errtext(void)
{
	static char buf[1 << 17];
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

/* GAPM row of (tid_filter, sec, seg): n and us; returns 1 if found, with the nomark tail in *tail (the rest of the line) */
static int gapm(unsigned long long tid_filter, unsigned sec, const char *seg, unsigned long long *n, unsigned long long *us, char *tail)
{
	const char *t = errtext();
	for (const char *p = strstr(t, "SWEETGL: GAPM pid "); p; p = strstr(p + 1, "SWEETGL: GAPM pid ")) {
		int pid, used = 0;
		unsigned long long tid, rn, rus;
		unsigned rsec;
		char rseg[32];
		if (sscanf(p, "SWEETGL: GAPM pid %d tid 0x%llx sec %u seg %31s n %llu us %llu%n", &pid, &tid, &rsec, rseg, &rn, &rus, &used) != 6) continue;
		if ((tid_filter && tid != tid_filter) || rsec != sec || strcmp(rseg, seg)) continue;
		*n = rn; *us = rus;
		if (tail) { const char *e = strchr(p + used, '\n'); size_t l = e ? (size_t)(e - (p + used)) : 0; memcpy(tail, p + used, l); tail[l] = 0; }
		return 1;
	}
	return 0;
}
/* tid of the first GAPTSUM line of the window that has `calls` calls */
static unsigned long long tid_of_calls(unsigned calls)
{
	char pat[64];
	snprintf(pat, sizeof pat, " calls %u gap_us", calls);
	for (const char *p = strstr(errtext(), "SWEETGL: GAPTSUM pid "); p; p = strstr(p + 1, "SWEETGL: GAPTSUM pid ")) {
		const char *e = strchr(p, '\n');
		const char *c = strstr(p, pat);
		unsigned long long tid;
		if (c && e && c < e && sscanf(p, "SWEETGL: GAPTSUM pid %*d tid 0x%llx", &tid) == 1) return tid;
	}
	return 0;
}
/* GAPT row `prev submit next wait` gap_us of a tid and window */
static unsigned long long gapt_submit_wait_us(unsigned long long tid, unsigned sec, unsigned long long *n)
{
	char pat[96];
	snprintf(pat, sizeof pat, "tid 0x%llx sec %u prev submit next wait n ", tid, sec);
	const char *p = strstr(errtext(), pat);
	unsigned long long rn = 0, us = 0;
	if (!p || sscanf(p + strlen(pat), "%llu gap_us %llu", &rn, &us) != 2) return ~0ull;
	*n = rn;
	return us;
}

static void *thread_main(void *arg)       /* its own marks: none, so its submit -> wait gaps are all nomark; main's marks must not leak in */
{
	(void)arg;
	CHECK(mk() == 0);
	for (int i = 0; i < 6; i++) { jump_us(7); CHECK(submit() == 0); jump_us(11); (void)wait_bo(); }
	jump_us(1500000);
	CHECK(mk() == 0);
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	int efd = mkstemp(errpath);
	if (efd < 0 || dup2(efd, 2) < 0) { printf("FAIL cannot redirect stderr\n"); return 1; }
	fake_reset();
	unsetenv("SWEETGL_BOCNT");
	if (!strcmp(mode, "on") || !strcmp(mode, "bad")) setenv("SWEETGL_GAPT", "1", 1);
	else if (!strcmp(mode, "off")) unsetenv("SWEETGL_GAPT");
	else { printf("FAIL usage: mode on|off|bad\n"); return 1; }
	setenv("SWEETGL_BOCUT", "0", 1);
	setenv("SWEETGL_BOPOOL", "0", 1);
	sweet_gapt_clock = clk;
	sweet_gapt_mark(1);                  /* before the first kernel call and before the library is open: no thread state yet, must be ignored */
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);

	if (!strcmp(mode, "on")) {
		CHECK(mk() == 0);
		/* frame k: submit, then +a mark1 +b mark2 +c mark3 +d wait. Each mark read adds a 1 us clock step, so the segments are 1+a, 1+b, 1+c, 1+d (us). */
		static const unsigned f[3][4] = { { 100, 20, 30, 40 }, { 5, 6, 7, 8 }, { 1000, 2000, 300, 4 } };
		unsigned long long seg[4] = { 0, 0, 0, 0 };
		for (int k = 0; k < 3; k++) {
			CHECK(submit() == 0);
			jump_us(f[k][0]); sweet_gapt_mark(1);
			jump_us(f[k][1]); sweet_gapt_mark(2);
			jump_us(f[k][2]); sweet_gapt_mark(3);
			jump_us(f[k][3]); (void)wait_bo();
			for (int i = 0; i < 4; i++) seg[i] += 1 + f[k][i];
		}
		/* a mark seen twice in one gap: the LAST reading wins (mark1 again 50 us later) */
		CHECK(submit() == 0);
		jump_us(10); sweet_gapt_mark(1);
		jump_us(50); sweet_gapt_mark(1);
		jump_us(2); sweet_gapt_mark(2);
		jump_us(3); sweet_gapt_mark(3);
		jump_us(4); (void)wait_bo();
		seg[0] += 1 + 10 + 1 + 50; seg[1] += 1 + 2; seg[2] += 1 + 3; seg[3] += 1 + 4;       /* submit_m1 runs to the second mark1 */
		/* marks, then a different kernel call: the marks belong to the gap that ended there and must not reach the next submit -> wait gap */
		CHECK(submit() == 0);
		jump_us(5); sweet_gapt_mark(1); sweet_gapt_mark(2); sweet_gapt_mark(3);
		jump_us(5); CHECK(mk() == 0);                  /* submit -> bo_new: ignored by GAPM */
		CHECK(submit() == 0);
		jump_us(9); (void)wait_bo();                   /* no marks since the last kernel call: nomark, miss1 miss2 miss3 */
		/* mark2 missing: miss2 */
		CHECK(submit() == 0);
		jump_us(3); sweet_gapt_mark(1);
		jump_us(3); sweet_gapt_mark(3);
		jump_us(3); (void)wait_bo();
		/* out of order: mark3 before mark1 (readings 3 < 1) -> order */
		CHECK(submit() == 0);
		jump_us(3); sweet_gapt_mark(3);
		jump_us(3); sweet_gapt_mark(1);
		jump_us(3); sweet_gapt_mark(2);
		jump_us(3); (void)wait_bo();
		/* the order counter names the first violating boundary pair { submit return t0, m1, m2, m3, wait entry t1 }; the case above was m3 < m2 = pair m2_m3 */
		CHECK(submit() == 0);                           /* t0_m1: mark 1 read before the submit returned */
		back_us(50); sweet_gapt_mark(1);
		jump_us(200); sweet_gapt_mark(2);
		jump_us(1); sweet_gapt_mark(3);
		jump_us(1); (void)wait_bo();
		CHECK(submit() == 0);                           /* m1_m2: mark 2 before mark 1 (3 after both) */
		jump_us(10); sweet_gapt_mark(2);
		jump_us(10); sweet_gapt_mark(1);
		jump_us(10); sweet_gapt_mark(3);
		jump_us(10); (void)wait_bo();
		CHECK(submit() == 0);                           /* m3_t1: the wait entry is read before mark 3 */
		jump_us(10); sweet_gapt_mark(1);
		jump_us(10); sweet_gapt_mark(2);
		jump_us(10); sweet_gapt_mark(3);
		back_us(25); (void)wait_bo();
		CHECK(submit() == 0);                           /* m3_t1 again (so that no two pair counters are equal and a swapped column shows) */
		jump_us(5); sweet_gapt_mark(1);
		jump_us(5); sweet_gapt_mark(2);
		jump_us(5); sweet_gapt_mark(3);
		back_us(12); (void)wait_bo();
		CHECK(submit() == 0);                           /* two violations: m2 < m1 and m3 < m2 -> the FIRST pair (m1_m2) only */
		jump_us(30); sweet_gapt_mark(3);
		jump_us(10); sweet_gapt_mark(2);
		jump_us(10); sweet_gapt_mark(1);
		jump_us(10); (void)wait_bo();
		/* a mark BEFORE the submit returned cannot exist (cleared at each kernel return), a mark made after the wait entry belongs to the next gap: not in this one */
		pthread_t th;
		CHECK(pthread_create(&th, NULL, thread_main, NULL) == 0);
		pthread_join(th, NULL);
		jump_us(1500000);
		CHECK(mk() == 0);                               /* ends window 0 of the main thread */
		/* calls: main thread 27 = mk + 4 frames x 2 + (submit, mk, submit, wait) + 2 x 2 + 5 x 2 (order pairs) + the window-ending mk; second thread 13 = mk + 6 x 2 + the window-ending mk */
		unsigned long long mtid = tid_of_calls(27), ttid = tid_of_calls(13);
		CHECK(mtid != 0 && ttid != 0 && mtid != ttid);
		unsigned long long n[5], us[5];
		static const char *const nm[5] = { "submit_m1", "m1_m2", "m2_m3", "m3_wait", "nomark" };
		char tail[256] = "";
		for (int i = 0; i < 5; i++) CHECK(gapm(mtid, 0, nm[i], &n[i], &us[i], i == 4 ? tail : NULL));
		/* 4 good gaps: three frames + the repeated mark; the 4th is mark-count 4. */
		for (int i = 0; i < 4; i++) { CHECK(n[i] == 4); CHECK(us[i] == seg[i]); }
		/* nomark: the no-marks gap, the miss2 gap, the order gap, the five pair gaps = 8 gaps */
		CHECK(n[4] == 8);
		CHECK(strstr(tail, " nomark_n 8 miss1 1 miss2 2 miss3 1 order 6 badid 0 order_pair t0_m1 1 m1_m2 2 m2_m3 1 m3_t1 2"));
		/* the sum of all rows equals the GAPT submit -> wait gap of the same thread and window, exactly */
		unsigned long long gn = 0, gus = gapt_submit_wait_us(mtid, 0, &gn);
		CHECK(gn == 4 + 8 && gus != ~0ull);
		CHECK(us[0] + us[1] + us[2] + us[3] + us[4] == gus);
		/* second thread: no marks of its own, never the main thread's: 6 gaps, all nomark, miss 1..3 = 6 */
		char tail2[256] = "";
		unsigned long long tn, tus;
		CHECK(gapm(ttid, 0, "nomark", &tn, &tus, tail2));
		CHECK(tn == 6 && strstr(tail2, " nomark_n 6 miss1 6 miss2 6 miss3 6 order 0 badid 0 order_pair t0_m1 0 m1_m2 0 m2_m3 0 m3_t1 0"));
		unsigned long long tnomark_us = tus;
		for (int i = 0; i < 4; i++) { CHECK(gapm(ttid, 0, nm[i], &tn, &tus, NULL)); CHECK(tn == 0 && tus == 0); }
		unsigned long long gn2 = 0, gus2 = gapt_submit_wait_us(ttid, 0, &gn2);
		CHECK(gn2 == 6 && gus2 == tnomark_us);
		CHECK(count_lines("GAPM unavailable") == 0);
		/* window 1 starts clean: the counters were cleared at the flush (the window-ending mk is its first call; 6 full frames follow) */
		CHECK(!gapm(mtid, 1, "nomark", &tn, &tus, NULL));
		unsigned long long seg1[4] = { 0, 0, 0, 0 };
		for (int k = 0; k < 6; k++) {
			CHECK(submit() == 0);
			jump_us(10 + k); sweet_gapt_mark(1);
			jump_us(20 + k); sweet_gapt_mark(2);
			jump_us(30 + k); sweet_gapt_mark(3);
			jump_us(40 + k); (void)wait_bo();
			for (int i = 0; i < 4; i++) seg1[i] += 1 + 10 * (i + 1) + k;
		}
		jump_us(1500000);
		CHECK(mk() == 0);
		for (int i = 0; i < 4; i++) { CHECK(gapm(mtid, 1, nm[i], &n[i], &us[i], NULL)); CHECK(n[i] == 6 && us[i] == seg1[i]); }
		CHECK(gapm(mtid, 1, "nomark", &n[4], &us[4], tail) && n[4] == 0 && us[4] == 0 && strstr(tail, " nomark_n 0 miss1 0 miss2 0 miss3 0 order 0 badid 0 order_pair t0_m1 0 m1_m2 0 m2_m3 0 m3_t1 0"));
		gus = gapt_submit_wait_us(mtid, 1, &gn);
		CHECK(gn == 6 && us[0] + us[1] + us[2] + us[3] == gus);
		/* window 2: >= 10 calls but no submit -> wait gap: no GAPM rows at all (a window without such a gap prints nothing) */
		for (int k = 0; k < 11; k++) { jump_us(3); CHECK(mk() == 0); }
		jump_us(1500000);
		CHECK(mk() == 0);
		CHECK(!gapm(mtid, 2, "nomark", &tn, &tus, NULL) && !gapm(mtid, 2, "submit_m1", &tn, &tus, NULL));
	} else if (!strcmp(mode, "off")) {
		CHECK(mk() == 0);
		for (int k = 0; k < 12; k++) {
			CHECK(submit() == 0);
			jump_us(10); sweet_gapt_mark(1); sweet_gapt_mark(2); sweet_gapt_mark(3);
			jump_us(10); (void)wait_bo();
		}
		jump_us(1500000);
		CHECK(mk() == 0);
		CHECK(!strstr(errtext(), "GAPM") && !strstr(errtext(), "GAPT"));
		CHECK(fake_count(1) == 2);       /* behaviour unchanged: both create_bo calls reached the kernel */
	} else {
		CHECK(mk() == 0);
		for (int k = 0; k < 12; k++) {
			CHECK(submit() == 0);
			jump_us(10); sweet_gapt_mark(1);
			jump_us(10); (void)wait_bo();
		}
		sweet_gapt_mark(0); sweet_gapt_mark(4); sweet_gapt_mark(-7); sweet_gapt_mark(1000);
		jump_us(1500000);
		CHECK(mk() == 0);
		CHECK(count_lines("SWEETGL: GAPM unavailable: mark id 0 (only 1..3 exist)") == 1);     /* said once, for the first bad id */
		CHECK(count_lines("GAPM unavailable") == 1);
		unsigned long long mtid = tid_of_calls(25), n, us;
		char tail[256] = "";
		CHECK(mtid != 0 && gapm(mtid, 0, "nomark", &n, &us, tail));
		CHECK(n == 12 && strstr(tail, " nomark_n 12 miss1 0 miss2 12 miss3 12 order 0 badid 4 order_pair t0_m1 0 m1_m2 0 m2_m3 0 m3_t1 0"));      /* every gap has mark1 only: nomark, never dropped */
	}
	if (getenv("GAPM_DUMP")) {
		fputs(errtext(), stdout);
	}
	unlink(errpath);
	printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
	return fails != 0;
}
