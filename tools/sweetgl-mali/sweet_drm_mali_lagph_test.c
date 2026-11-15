/* Host test for the lag-phase witness of sweet_drm_mali.c (LAGPH, T-9577). One process per mode (the knob is read once per process): argv[1] = on | off | bad | cap.
 * Fake wall clock, fake thread CPU clock, fake cpu number. stderr is redirected to a file and read back. */
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

extern uint64_t (*sweet_lagph_clock)(void);
extern uint64_t (*sweet_lagph_cpuclock)(void);
extern unsigned (*sweet_lagph_cpunum)(void);
extern void sweet_lagph_flush_exit(void);

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int64_t os_time_get_nano(void) { return 0; }

static uint64_t W = 5000000000ull, C = 100000000ull;
static unsigned cpu_now = 1;
static int cpu_fail;
static uint64_t wclk(void) { return W; }
static uint64_t cclk(void) { return cpu_fail ? 0 : C; }
static unsigned cnum(void) { return cpu_now; }

static int devfd = -1;
static char errpath[] = "/tmp/sdm-lagph-XXXXXX";
static char *errtext(void)
{
	static char buf[8 << 20];
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
static int submit_bo(uint32_t h)
{
	struct drm_panfrost_submit s = { .jc = 0x2000000, .bo_handles = (uint64_t)(uintptr_t)&h, .bo_handle_count = 1 };
	return drmIoctl(devfd, DRM_IOCTL_PANFROST_SUBMIT, &s);
}
static uint32_t bo0;
static void *other_thread(void *arg)
{
	(void)arg;
	CHECK(submit_bo(bo0) == 0);   /* no flush exit on THIS thread: must be counted as unarmed, never attributed */
	return NULL;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	int efd = mkstemp(errpath);
	if (efd < 0 || dup2(efd, 2) < 0) { printf("FAIL cannot redirect stderr\n"); return 1; }
	fake_reset();
	unsetenv("SWEETGL_BOCNT");
	unsetenv("SWEETGL_GAPT");
	if (!strcmp(mode, "on") || !strcmp(mode, "cap")) setenv("SWEETGL_LAGPH", "1", 1);
	else if (!strcmp(mode, "off")) unsetenv("SWEETGL_LAGPH");
	else if (!strcmp(mode, "bad")) setenv("SWEETGL_LAGPH", "x", 1);
	else { printf("FAIL usage: mode on|off|bad|cap\n"); return 1; }
	sweet_lagph_clock = wclk;
	sweet_lagph_cpuclock = cclk;
	sweet_lagph_cpunum = cnum;
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);
	struct drm_panfrost_create_bo r = { .size = 16384, .flags = PANFROST_BO_NOEXEC };
	CHECK(drmIoctl(devfd, DRM_IOCTL_PANFROST_CREATE_BO, &r) == 0);
	bo0 = r.handle;

	if (!strcmp(mode, "on")) {
		CHECK(count_lines("SWEETGL: LAGPH on pid ") == 1);
		/* frame 1: flush exit at W=5.000000000 s, C=100 ms on cpu 1; first submit 4700 us later, 1900 us of CPU, on cpu 4 */
		sweet_lagph_flush_exit();
		W += 4700000ull; C += 1900000ull; cpu_now = 4;
		CHECK(submit_bo(bo0) == 0);
		CHECK(count_lines("SWEETGL: LAGPH n ") == 1);
		CHECK(strstr(errtext(), "SWEETGL: LAGPH n 1 tid ") != NULL);
		CHECK(strstr(errtext(), "fe_ns 5000000000 sub_ns 5004700000 wall_us 4700 cpu_us 1900 cpu_a 1 cpu_b 4\n") != NULL);
		/* a second submit in the same frame is not a row */
		W += 1000; C += 1000;
		CHECK(submit_bo(bo0) == 0);
		CHECK(count_lines("SWEETGL: LAGPH n ") == 1);
		CHECK(count_lines("SWEETGL: LAGPH unarmed submits 1 rows 1 cpufail 0") == 1);
		/* a submit on another thread, no flush exit there: unarmed, no row */
		pthread_t t;
		sweet_lagph_flush_exit();           /* arms THIS thread only */
		CHECK(pthread_create(&t, NULL, other_thread, NULL) == 0);
		pthread_join(t, NULL);
		CHECK(count_lines("SWEETGL: LAGPH n ") == 1);
		/* CPU clock failure: cpu_us 0 and the failure is counted, loud in the next unarmed line (unarmed count 3 is not a report point, 257 is: drive it) */
		cpu_fail = 1;
		sweet_lagph_flush_exit();
		W += 3000000ull;
		CHECK(submit_bo(bo0) == 0);
		CHECK(strstr(errtext(), "wall_us 3000 cpu_us 0 cpu_a 4 cpu_b 4\n") != NULL);
		cpu_fail = 0;
		for (int i = 0; i < 260; i++) CHECK(submit_bo(bo0) == 0);
		CHECK(strstr(errtext(), "cpufail 2") != NULL);   /* flush exit + the failed row's own read */
		CHECK(strstr(errtext(), "tid 0x") == NULL);
	} else if (!strcmp(mode, "off")) {
		sweet_lagph_flush_exit();
		W += 4700000ull;
		CHECK(submit_bo(bo0) == 0);
		CHECK(count_lines("LAGPH") == 0);
	} else if (!strcmp(mode, "bad")) {
		CHECK(count_lines("SWEETGL: LAGPH unavailable: SWEETGL_LAGPH=\"x\" is not 0 or 1, lag phases OFF") == 1);
		sweet_lagph_flush_exit();
		CHECK(submit_bo(bo0) == 0);
		CHECK(count_lines("LAGPH n ") == 0);
	} else if (!strcmp(mode, "cap")) {
		for (unsigned i = 0; i < 40003; i++) {
			sweet_lagph_flush_exit();
			W += 1000; C += 500;
			CHECK(submit_bo(bo0) == 0);
		}
		CHECK(count_lines("SWEETGL: LAGPH n ") == 40000);
		CHECK(count_lines("SWEETGL: LAGPH cap reached 40000 rows") == 1);
	}
	printf("%s: %d checks, %d failed\n", mode, checks, fails);
	unlink(errpath);
	if (fails) return 1;
	printf("RESULT: PASS lagph %s\n", mode);
	return 0;
}
