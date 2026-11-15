/* Host test for the user-mode gap timing of sweet_drm_mali.c (P-G1 slice 10, T-46d7). Run by host-test.sh in its own processes (the knob is read once per
 * process): argv[1] = "on" (SWEETGL_GAPT=1, fake clock), "off" (unset), "bad" (SWEETGL_GAPT=x), "tbfail" (SWEETGL_GAPT=1, mach_timebase_info "fails").
 * Fake clock: every read returns T and then adds STEP, so a kernel call measures exactly STEP inside and gaps are STEP + the jump the test makes between calls.
 * stderr is redirected to a file and read back. */
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
extern int sweet_gapt_tb_fail;
extern unsigned sweet_gapt_walk(uintptr_t fp, uintptr_t lo, uintptr_t hi, uint64_t *out, unsigned max);

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int64_t os_time_get_nano(void) { return 0; }

#define STEP 1000ull
static uint64_t T = 1000000000ull;
static uint64_t clk(void) { uint64_t r = T; T += STEP; return r; }
static void jump_us(uint64_t us) { T += us * 1000ull; }

static int devfd = -1;
/* noinline and not a tail call, so the return address drmIoctl sees lies inside io() and the frame above it is the caller (mk / submit_bo / wait_poll) */
__attribute__((noinline)) static int io(unsigned long req, void *arg)
{
	int r = drmIoctl(devfd, req, arg);
	__asm__ volatile("" ::: "memory");
	return r;
}
__attribute__((noinline)) static int io2(unsigned long req, void *arg)
{
	int r = drmIoctl(devfd, req, arg);
	__asm__ volatile("" ::: "memory");
	return r;
}
__attribute__((noinline)) static int syncwait(uint32_t so)
{
	int r = drmSyncobjWait(devfd, &so, 1, 0, 0, NULL);
	__asm__ volatile("" ::: "memory");
	return r;
}
__attribute__((noinline)) static int closer(uint32_t h)
{
	int r = drmCloseBufferHandle(devfd, h);
	__asm__ volatile("" ::: "memory");
	return r;
}
static char errpath[] = "/tmp/sdm-gapt-XXXXXX";

__attribute__((noinline)) static int mk(uint32_t size, struct drm_panfrost_create_bo *r)
{
	*r = (struct drm_panfrost_create_bo){ .size = size, .flags = PANFROST_BO_NOEXEC };
	return io(DRM_IOCTL_PANFROST_CREATE_BO, r);
}
__attribute__((noinline)) static int submit_bo(uint32_t h)
{
	struct drm_panfrost_submit s = { .jc = 0x2000000, .bo_handles = (uint64_t)(uintptr_t)&h, .bo_handle_count = 1 };
	return io(DRM_IOCTL_PANFROST_SUBMIT, &s);
}
__attribute__((noinline)) static int submit_bo2(uint32_t h)     /* the same submit through a second wrapper: the previous call's site differs */
{
	struct drm_panfrost_submit s = { .jc = 0x2000000, .bo_handles = (uint64_t)(uintptr_t)&h, .bo_handle_count = 1 };
	return io2(DRM_IOCTL_PANFROST_SUBMIT, &s);
}
__attribute__((noinline)) static int submit_sync(uint32_t h, uint32_t so)
{
	struct drm_panfrost_submit s = { .jc = 0x2000000, .bo_handles = (uint64_t)(uintptr_t)&h, .bo_handle_count = 1, .out_sync = so };
	return io(DRM_IOCTL_PANFROST_SUBMIT, &s);
}
__attribute__((noinline)) static int wait_poll(uint32_t h)
{
	struct drm_panfrost_wait_bo w = { .handle = h, .timeout_ns = 0 };
	return io(DRM_IOCTL_PANFROST_WAIT_BO, &w);
}

static char *errtext(void)
{
	static char buf[65536];
	FILE *f = fopen(errpath, "r");
	size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
	if (f) fclose(f);
	buf[n] = 0;
	return buf;
}

/* number of lines that contain `pat` */
static unsigned count_lines(const char *pat)
{
	unsigned n = 0;
	for (const char *p = strstr(errtext(), pat); p; p = strstr(p + 1, pat)) n++;
	return n;
}


/* One parsed `SWEETGL: GAPS pid ...` row. */
struct srow { unsigned sec; char prev[16], next[16]; unsigned long long psite, s0, s1, s2, s3, n, gap_us, lt_us; };
static unsigned parse_rows(struct srow *out, unsigned max, unsigned long long tid_filter)
{
	unsigned k = 0;
	const char *t = errtext();
	for (const char *p = strstr(t, "SWEETGL: GAPS pid "); p && k < max; p = strstr(p + 1, "SWEETGL: GAPS pid ")) {
		struct srow r;
		int pid;
		unsigned long long tid;
		if (sscanf(p, "SWEETGL: GAPS pid %d tid 0x%llx sec %u prev %15s next %15s psite 0x%llx site 0x%llx,0x%llx,0x%llx,0x%llx n %llu gap_us %llu lt1ms_us %llu",
		    &pid, &tid, &r.sec, r.prev, r.next, &r.psite, &r.s0, &r.s1, &r.s2, &r.s3, &r.n, &r.gap_us, &r.lt_us) != 13) {
			CHECK(!"GAPS row does not parse");
			continue;
		}
		if (tid_filter && tid != tid_filter) continue;
		out[k++] = r;
	}
	return k;
}
/* sum of the rows of one window and pair */
static unsigned long long lt_sum;    /* lt1ms_us of the rows sum_pair added up */
static void sum_pair(const struct srow *r, unsigned nr, unsigned sec, const char *prev, const char *next, unsigned long long *n, unsigned long long *gap, unsigned *rows)
{
	*n = *gap = 0; *rows = 0; lt_sum = 0;
	for (unsigned i = 0; i < nr; i++) {
		if (r[i].sec == sec && !strcmp(r[i].prev, prev) && !strcmp(r[i].next, next)) { *n += r[i].n; *gap += r[i].gap_us; lt_sum += r[i].lt_us; (*rows)++; }
	}
}
static int in_fn(unsigned long long addr, const void *fn, unsigned long long len)
{
	return addr >= (unsigned long long)(uintptr_t)fn && addr < (unsigned long long)(uintptr_t)fn + len;
}
#define MANY10(x) x x x x x x x x x x

static uint32_t bo0;
static void *thread_main(void *arg)
{
	(void)arg;
	struct drm_panfrost_create_bo r;
	CHECK(mk(16384, &r) == 0);
	jump_us(40);
	CHECK(submit_bo(bo0) == 0);
	jump_us(1500000);                      /* a window ends: this thread prints ITS table */
	CHECK(mk(16384, &r) == 0);
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
	if (!strcmp(mode, "on")) setenv("SWEETGL_GAPT", "1", 1);
	else if (!strcmp(mode, "off")) unsetenv("SWEETGL_GAPT");
	else if (!strcmp(mode, "bad")) setenv("SWEETGL_GAPT", "x", 1);
	else if (!strcmp(mode, "tbfail")) { setenv("SWEETGL_GAPT", "1", 1); sweet_gapt_tb_fail = 1; }
	else if (!strcmp(mode, "top") || !strcmp(mode, "lost") || !strcmp(mode, "nest") || !strcmp(mode, "walk")) setenv("SWEETGL_GAPT", "1", 1);
	else { printf("FAIL usage: mode on|off|bad|tbfail|top|lost|nest|walk\n"); return 1; }
	if (!strcmp(mode, "nest")) { setenv("SWEETGL_BOCUT", "0", 1); setenv("SWEETGL_BOPOOL", "0", 1); }
	sweet_gapt_clock = clk;
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);
	struct drm_panfrost_create_bo r;

	if (!strcmp(mode, "top") || !strcmp(mode, "lost")) {
		/* 40 gaps of 11 us from 40 distinct call sites + the window-ending gap from a 41st: 41 table rows. top: TAB_N 512, TOP 32 -> 32 printed + 9 in GAPSREST.
		 * lost: the binary is built with -DGAPT_TAB_N=16 -> 16 rows stored, 15 gaps lost (counted in GAPSREST lost_n, never dropped silently). */
		CHECK(mk(16384, &r) == 0);
		MANY10(jump_us(10); CHECK(mk(16384, &r) == 0);)
		MANY10(jump_us(10); CHECK(mk(16384, &r) == 0);)
		MANY10(jump_us(10); CHECK(mk(16384, &r) == 0);)
		MANY10(jump_us(10); CHECK(mk(16384, &r) == 0);)
		jump_us(1500000);
		CHECK(mk(16384, &r) == 0);                  /* its gap (1500001 us) ends window 0 */
		const char *t = errtext();
		struct srow rows[64];
		unsigned nr = parse_rows(rows, 64, 0);
		unsigned long long pn = 0, pgap = 0;
		for (unsigned i = 0; i < nr; i++) { CHECK(rows[i].sec == 0); pn += rows[i].n; pgap += rows[i].gap_us; }
		const char *rest = strstr(t, "SWEETGL: GAPSREST pid ");
		unsigned long long rsites = 0, rn = 0, rgap = 0, ln = 0, lgap = 0, nsn = 0;
		CHECK(rest && sscanf(strstr(rest, " sites "), " sites %llu n %llu gap_us %llu lost_n %llu lost_us %llu nosite_n %llu", &rsites, &rn, &rgap, &ln, &lgap, &nsn) == 6);
		CHECK(count_lines("SWEETGL: GAPSREST pid ") == 1);
		CHECK(pn + rn + ln == 41 && pgap + rgap + lgap == 40 * 11 + 1500001 && nsn == 0);    /* every gap is somewhere */
		if (!strcmp(mode, "top")) {
			CHECK(nr == 32 && rsites == 9 && rn == 9 && rgap == 99 && ln == 0 && lgap == 0);
			CHECK(rows[0].gap_us == 1500001 && rows[0].n == 1);      /* sorted by gap time, biggest first */
			for (unsigned i = 1; i < nr; i++) CHECK(rows[i].gap_us == 11 && rows[i].n == 1);
			for (unsigned i = 0; i < nr; i++) for (unsigned j = i + 1; j < nr; j++) CHECK(rows[i].s1 != rows[j].s1 || rows[i].s2 != rows[j].s2);   /* distinct call sites */
			/* window 1: the table was cleared at the end of window 0 (12 gaps from one line + the window-ending gap = 13, in 2 rows) */
			for (int i = 0; i < 12; i++) { jump_us(10); CHECK(mk(16384, &r) == 0); }
			jump_us(1500000);
			CHECK(mk(16384, &r) == 0);
			struct srow r1[64];
			unsigned n1 = parse_rows(r1, 64, 0), w1 = 0;
			unsigned long long w1n = 0;
			for (unsigned i = 0; i < n1; i++) if (r1[i].sec == 1) { w1++; w1n += r1[i].n; }
			CHECK(w1 == 2 && w1n == 13);
		} else {
			CHECK(nr == 16 && rsites == 0 && rn == 0 && ln == 25);
		}
		printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
		unlink(errpath);
		return fails != 0;
	}
	if (!strcmp(mode, "walk")) {
		/* the frame walker on synthetic stacks: frame record = [saved fp, return address], 16-byte aligned, callers at higher addresses */
		static uintptr_t st[16] __attribute__((aligned(16)));
		uint64_t o[8];
		uintptr_t lo = (uintptr_t)&st[0], hi = (uintptr_t)&st[16];
		memset(st, 0, sizeof st);
		st[0] = (uintptr_t)&st[4];  st[1] = 0xA;
		st[4] = (uintptr_t)&st[8];  st[5] = 0xB;
		st[8] = (uintptr_t)&st[12]; st[9] = 0xC;
		st[12] = 0;                 st[13] = 0xD;
		CHECK(sweet_gapt_walk(lo, lo, hi, o, 3) == 3 && o[0] == 0xA && o[1] == 0xB && o[2] == 0xC);      /* bounded by max */
		CHECK(sweet_gapt_walk(lo, lo, hi, o, 8) == 4 && o[3] == 0xD);                                    /* the chain ends at a saved fp of 0: that record is read, then stop */
		CHECK(sweet_gapt_walk(lo, lo, (uintptr_t)&st[13], o, 8) == 3);                                    /* the last record does not fit below hi */
		CHECK(sweet_gapt_walk(lo, (uintptr_t)&st[4], hi, o, 8) == 0);                                     /* below the stack bottom */
		CHECK(sweet_gapt_walk(lo + 8, lo, hi, o, 8) == 0);                                                /* unaligned */
		CHECK(sweet_gapt_walk(lo, 0, hi, o, 8) == 0);                                                     /* no bounds known: no walk */
		CHECK(sweet_gapt_walk(lo, lo, hi, o, 0) == 0);
		st[4] = (uintptr_t)&st[0];                                                                        /* a saved fp that points back down: stop, never loop */
		CHECK(sweet_gapt_walk(lo, lo, hi, o, 8) == 2 && o[0] == 0xA && o[1] == 0xB);
		printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
		unlink(errpath);
		return fails != 0;
	}

	if (!strcmp(mode, "nest")) {
		/* drmCloseBufferHandle calls drmIoctl: the outermost entry point owns the site, so site[0] is in closer(), not in libdrm's own frame */
		for (int i = 0; i < 12; i++) {
			CHECK(mk(16384, &r) == 0);
			jump_us(10);
			CHECK(closer(r.handle) == 0);
			jump_us(10);
		}
		struct drm_panfrost_create_bo b2;
		uint32_t so = 0;
		CHECK(mk(16384, &b2) == 0 && drmSyncobjCreate(devfd, 0, &so) == 0);
		for (int v = 0; v < 2; v++) {
			jump_us(10);
			CHECK((v ? submit_bo2(b2.handle) : submit_bo(b2.handle)) == 0);
			jump_us(10);
			CHECK(mk(16384, &r) == 0);            /* one source line: the next site is the same, the previous call came through io() or io2() */
		}
		jump_us(10);
		CHECK(submit_sync(b2.handle, so) == 0);
		jump_us(10);
		CHECK(syncwait(so) == -1);                /* a pending seqno: drmSyncobjWait calls the kernel's wait */
		jump_us(1500000);
		CHECK(mk(16384, &r) == 0);
		struct srow rows[64];
		unsigned nr = parse_rows(rows, 64, 0), nclose = 0, nsub = 0, nsw = 0;
		const struct srow *sub[2] = { NULL, NULL };
		for (unsigned i = 0; i < nr; i++) {
			if (!strcmp(rows[i].next, "bo_close")) { nclose += rows[i].n; CHECK(in_fn(rows[i].s0, closer, 200)); CHECK(in_fn(rows[i].s1, main, 20000)); }
			if (!strcmp(rows[i].next, "bo_new")) CHECK(in_fn(rows[i].s0, io, 200) && in_fn(rows[i].s1, mk, 200));
			if (!strcmp(rows[i].prev, "bo_close")) CHECK(in_fn(rows[i].psite, closer, 200));
			if (!strcmp(rows[i].prev, "submit") && !strcmp(rows[i].next, "bo_new") && nsub < 2) sub[nsub++] = &rows[i];
			if (!strcmp(rows[i].prev, "submit") && !strcmp(rows[i].next, "wait")) { nsw++; CHECK(in_fn(rows[i].s0, syncwait, 200) && in_fn(rows[i].psite, io, 200)); }
		}
		CHECK(nclose == 12);
		CHECK(nsub == 2 && nsw == 1);
		if (nsub == 2) {
			/* same (prev, next, next site), different previous site: two rows, one with psite in io() and one in io2() */
			CHECK(sub[0]->s0 == sub[1]->s0 && sub[0]->s1 == sub[1]->s1 && sub[0]->s2 == sub[1]->s2 && sub[0]->psite != sub[1]->psite);
			CHECK((in_fn(sub[0]->psite, io, 200) && in_fn(sub[1]->psite, io2, 200)) || (in_fn(sub[1]->psite, io, 200) && in_fn(sub[0]->psite, io2, 200)));
		}
		printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
		unlink(errpath);
		return fails != 0;
	}

	/* 12 kernel calls in window 0, gaps (us incl. the 1 us clock step): new, +100 submit, +200 submit, +300 submit, +5000 wait, +50 submit, +50 submit, +10 new x5 */
	CHECK(mk(16384, &r) == 0);
	bo0 = r.handle;
	jump_us(100);     CHECK(submit_bo(bo0) == 0);
	jump_us(200);     CHECK(submit_bo(bo0) == 0);
	jump_us(300);     CHECK(submit_bo(bo0) == 0);
	jump_us(5000);    CHECK(wait_poll(bo0) == -1 && errno == ETIMEDOUT);     /* nothing completed in the fake: one wait kcall, Timeout */
	jump_us(50);      CHECK(submit_bo(bo0) == 0);
	jump_us(50);      CHECK(submit_bo(bo0) == 0);
	for (int i = 0; i < 5; i++) { jump_us(10); CHECK(mk(16384, &r) == 0); }
	CHECK(fake_count(1) == 6 && fake_count(3) == 5 && fake_count(4) == 1);
	jump_us(1500000);                      /* the 13th call ends window 0: its own gap is booked into window 0 first, then the table is printed */
	CHECK(mk(16384, &r) == 0);
	jump_us(10);      CHECK(submit_bo(bo0) == 0);
	jump_us(10);      CHECK(submit_bo(bo0) == 0);
	pthread_t th;                          /* a second thread keeps its own table */
	CHECK(pthread_create(&th, NULL, thread_main, NULL) == 0);
	CHECK(pthread_join(th, NULL) == 0);
	jump_us(1500000);                      /* main thread, window 1 ends (3 + 2 calls: below the detail threshold) */
	CHECK(mk(16384, &r) == 0);

	const char *t = errtext();
	if (!strcmp(mode, "on")) {
		CHECK(strstr(t, "SWEETGL: GAPT on pid "));
		CHECK(!strstr(t, "unavailable"));
		/* window 0 of the main thread; the 13th call's 1.5 s gap sits in [new -> new] */
		CHECK(strstr(t, " sec 0 prev bo_new next submit n 1 gap_us 101 lt1ms_n 1 lt1ms_us 101 max_us 101\n"));
		CHECK(strstr(t, " sec 0 prev submit next submit n 3 gap_us 553 lt1ms_n 3 lt1ms_us 553 max_us 301\n"));   /* 201 + 301 + 51 */
		CHECK(strstr(t, " sec 0 prev submit next wait n 1 gap_us 5001 lt1ms_n 0 lt1ms_us 0 max_us 5001\n"));
		CHECK(strstr(t, " sec 0 prev wait next submit n 1 gap_us 51 lt1ms_n 1 lt1ms_us 51 max_us 51\n"));
		CHECK(strstr(t, " sec 0 prev submit next bo_new n 1 gap_us 11 lt1ms_n 1 lt1ms_us 11 max_us 11\n"));
		CHECK(strstr(t, " sec 0 prev bo_new next bo_new n 5 gap_us 1500045 lt1ms_n 4 lt1ms_us 44 max_us 1500001\n"));   /* 4 x 11 + the window-ending gap */
		CHECK(strstr(t, " sec 0 kind bo_new n 6 in_us 6 max_us 1\n"));
		CHECK(strstr(t, " sec 0 kind submit n 5 in_us 5 max_us 1\n"));
		CHECK(strstr(t, " sec 0 kind wait n 1 in_us 1 max_us 1\n"));
		CHECK(!strstr(t, " kind wait_poll ") && !strstr(t, " kind import ") && !strstr(t, " kind bo_close "));
		CHECK(strstr(t, " sec 0 win_ms 1505 calls 12 gap_us 1505762 in_us 12 first 1\n"));    /* 101 + 553 + 5001 + 51 + 11 + 1500045; the 13th call's own gap is in it, its in-call time is not */
		/* window 1 of the main thread: the 13th call (its in-call time lands in the window it opened) + 2 submits = 3 calls, below the detail threshold: SUM only */
		CHECK(!strstr(t, " sec 1 prev ") && !strstr(t, " sec 1 kind "));
		CHECK(strstr(t, " sec 1 win_ms 3000 calls 3 gap_us 3000070 in_us 3 first 0\n"));     /* the fake clock is global, so the second thread's 1.5 s is inside this thread's last gap */
		/* call sites (slice 11): the rows of a pair add up to the GAPT line, the frames are the ones the test's own functions make */
		{
			unsigned long long mtid = 0;
			const char *q = strstr(t, "SWEETGL: GAPTSUM pid ");
			CHECK(q && sscanf(strstr(q, " tid 0x"), " tid 0x%llx", &mtid) == 1);
			struct srow rows[64];
			unsigned nr = parse_rows(rows, 64, mtid), rn = 0;
			unsigned long long n, gap;
			unsigned nrows;
			for (unsigned i = 0; i < nr; i++) {
				if (rows[i].sec == 0) {
					rn += rows[i].n;
					CHECK(in_fn(rows[i].s0, io, 200));                 /* the caller of drmIoctl */
					CHECK(rows[i].psite == rows[i].s0);                /* the previous call came through io() too */
					CHECK(in_fn(rows[i].s2, main, 20000));             /* the frame above mk/submit_bo/wait_poll */
				}
			}
			CHECK(rn == 12);
			sum_pair(rows, nr, 0, "submit", "submit", &n, &gap, &nrows);
			CHECK(n == 3 && gap == 553 && nrows == 3);                 /* three call lines in main -> three rows */
			for (unsigned i = 0; i < nr; i++) if (!strcmp(rows[i].prev, "submit") && !strcmp(rows[i].next, "submit")) CHECK(in_fn(rows[i].s1, submit_bo, 200));
			sum_pair(rows, nr, 0, "submit", "wait", &n, &gap, &nrows);
			CHECK(n == 1 && gap == 5001 && nrows == 1 && lt_sum == 0);
			for (unsigned i = 0; i < nr; i++) if (!strcmp(rows[i].next, "wait")) CHECK(in_fn(rows[i].s1, wait_poll, 200));
			sum_pair(rows, nr, 0, "bo_new", "bo_new", &n, &gap, &nrows);
			CHECK(n == 5 && gap == 1500045 && nrows == 2 && lt_sum == 44);   /* the loop line (4 gaps) and the window-ending line; only the 4 short gaps are < 1 ms */
			sum_pair(rows, nr, 0, "bo_new", "submit", &n, &gap, &nrows);
			CHECK(n == 1 && gap == 101 && nrows == 1);
			sum_pair(rows, nr, 0, "wait", "submit", &n, &gap, &nrows);
			CHECK(n == 1 && gap == 51 && nrows == 1);
			CHECK(strstr(t, " sec 0 sites 0 n 0 gap_us 0 lost_n 0 lost_us 0 nosite_n 0\n"));
			CHECK(strstr(t, "SWEETGL: GAPS on pid ") && strstr(t, " base 0x0 depth 4\n"));
		}
		/* the second thread: its own tid, first 1, its own table (2 calls: 40 us between them, then the window-ending 1.5 s) */
		CHECK(count_lines("SWEETGL: GAPTSUM pid ") == 3);
		CHECK(strstr(t, " sec 0 win_ms 1500 calls 2 gap_us 1500042 in_us 2 first 1\n"));
		{
			char a[32] = "", b[32] = "";
			int n = 0;
			for (const char *p = strstr(t, "GAPTSUM pid "); p && n < 3; p = strstr(p + 1, "GAPTSUM pid "), n++) {
				const char *q = strstr(p, " tid ");
				char id[32] = "";
				if (q) sscanf(q + 5, "%31s", id);
				if (!a[0]) strcpy(a, id); else if (strcmp(a, id)) strcpy(b, id);
			}
			CHECK(a[0] && b[0] && strcmp(a, b));      /* two distinct tids */
		}
	} else if (!strcmp(mode, "off")) {
		CHECK(!strstr(t, "GAPT"));
	} else if (!strcmp(mode, "bad")) {
		CHECK(strstr(t, "SWEETGL: GAPT unavailable: SWEETGL_GAPT=\"x\" is not 0 or 1, gap timing OFF\n"));
		CHECK(count_lines("SWEETGL: GAPT") == 1);
	} else {
		CHECK(strstr(t, "SWEETGL: GAPT unavailable: mach_timebase_info failed, gap timing OFF\n"));
		CHECK(count_lines("SWEETGL: GAPT") == 1);
	}
	/* behaviour is the same in every mode: all kernel calls reached the fake and none failed */
	CHECK(fake_count(1) == 10 && fake_count(3) == 8);
	if (getenv("GAPT_DUMP")) {
		fputs(errtext(), stdout);
	}
	unlink(errpath);
	printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
	return fails != 0;
}
