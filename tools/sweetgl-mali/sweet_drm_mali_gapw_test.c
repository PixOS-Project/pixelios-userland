/* Host test for the window pc sampler of sweet_drm_mali.c (P-G1 slice 12d, T-baf5; v2 slice 12e, T-bdc1): SWEETGL_GAPW, sweet_gapt_mark(1/2) as window flag, sampler slots, rings, GAPW rows
 * with prev/next tags (the kernel calls that open and close the gap), 8 frames, no stop on a full ring, every group printed (GAPW_TOP_SMALL build: bound 32).
 * One process per mode (the knobs are read once): argv[1] = "on" (GAPT=1 GAPW=1), "off" (GAPT=1, GAPW unset), "bad" (GAPW=x), "nogapt" (GAPW=1, GAPT unset).
 * The kernel is played by the test: sweet_gapw_suspend/getstate/resume are fake hooks (a fake register file whose frame chain lives on the test thread's REAL stack, so the
 * stack-bounds check of the sampler sees real bounds); the sampler pthread is replaced by sweet_gapw_tick() called from the test, so every pass is deterministic.
 * Fake clock: every read returns T and then adds STEP (1 us). stderr is redirected to a file and read back. */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fake_iokit.h"
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "darwin_drm.h"
#include "sweet_drm_mali.h"

extern uint64_t (*sweet_gapt_clock)(void);
void sweet_gapt_mark(int id);
extern int (*sweet_gapw_suspend)(uint64_t tid);
extern int (*sweet_gapw_getstate)(uint64_t tid, uint64_t r[4]);
extern int (*sweet_gapw_resume)(uint64_t tid);
void sweet_gapw_tick(void);
extern int sweet_gapw_nobounds;

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int64_t os_time_get_nano(void) { return 0; }

#define STEP 1000ull
static uint64_t T = 1000000000ull;
static uint64_t clk(void) { uint64_t r = T; T += STEP; return r; }
static void jump_us(uint64_t us) { T += us * 1000ull; }

static int devfd = -1;
static char errpath[] = "/tmp/sdm-gapw-XXXXXX";
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
static int wait_bo(void)
{
	struct drm_panfrost_wait_bo w = { .handle = bo0, .timeout_ns = 0 };
	return drmIoctl(devfd, DRM_IOCTL_PANFROST_WAIT_BO, &w);
}

static char *errtext(void)
{
	static char buf[1 << 18];
	FILE *f = fopen(errpath, "r");
	size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
	if (f) fclose(f);
	buf[n] = 0;
	return buf;
}
static size_t errlen(void)
{
	struct stat st;
	return stat(errpath, &st) == 0 ? (size_t)st.st_size : 0;
}
static unsigned count_in(const char *text, const char *pat)
{
	unsigned n = 0;
	for (const char *p = strstr(text, pat); p; p = strstr(p + 1, pat)) n++;
	return n;
}

/* ---- the fake kernel ---- */
struct fregs { uint64_t tid; uint64_t r[4]; int susp_rc, get_rc, res_rc; };
static struct fregs fr[4];
static unsigned nfr;
static unsigned n_susp, n_get, n_res, stopped;
static int printed_while_stopped;
static size_t len_at_stop;
static int nest_tick;                   /* the next stop runs a nested sampler pass (drop path)  */
static int suspend_closes_window;       /* the target "leaves the window" between the sampler's pre-check and its stop */
static struct fregs *find(uint64_t tid)
{
	for (unsigned i = 0; i < nfr; i++) if (fr[i].tid == tid) return &fr[i];
	return NULL;
}
static int f_suspend(uint64_t tid)
{
	struct fregs *f = find(tid);
	n_susp++;
	if (!f || f->susp_rc) return -1;
	stopped++;
	len_at_stop = errlen();
	if (suspend_closes_window) { suspend_closes_window = 0; sweet_gapt_mark(2); }
	if (nest_tick) { nest_tick = 0; sweet_gapw_tick(); }      /* a second producer: its sample takes the last ring slot while this stop is still going on */
	return 0;
}
static int f_getstate(uint64_t tid, uint64_t r[4])
{
	struct fregs *f = find(tid);
	n_get++;
	CHECK(stopped > 0);
	if (!f || f->get_rc) return -1;
	memcpy(r, f->r, sizeof f->r);
	return 0;
}
static int f_resume(uint64_t tid)
{
	n_res++;
	if (stopped) { stopped--; if (errlen() != len_at_stop) printed_while_stopped = 1; }
	(void)tid;
	return find(tid) && !find(tid)->res_rc ? 0 : -1;
}

/* a frame chain on this thread's real stack: fp -> {next, 0xf1} -> ... -> {next, 0xf8} -> {next, 0xf9} -> {next, 0xfa} (records 9 and 10 are past GAPW_FR = 8) */
struct chain { uintptr_t w[20] __attribute__((aligned(16))); };
static void build_chain(struct chain *c, uint64_t r[4], uint64_t pc, uint64_t lr)
{
	for (int i = 0; i < 10; i++) { c->w[2 * i] = (uintptr_t)&c->w[2 * i + 2]; c->w[2 * i + 1] = 0xf1 + i; }
	r[0] = pc; r[1] = lr; r[2] = (uint64_t)(uintptr_t)&c->w[0]; r[3] = (uint64_t)(uintptr_t)c + 8;      /* sp != fp (and unaligned, so a walk started from sp finds nothing) */
}
static void set_regs(uint64_t tid, const uint64_t r[4])
{
	struct fregs *f = find(tid);
	if (!f) { f = &fr[nfr++]; memset(f, 0, sizeof *f); f->tid = tid; }
	memcpy(f->r, r, sizeof f->r);
}

/* a row group: "prev A next B n N dt_us D pc 0xPC off 0x0 img - lr 0xLR fr 0xf1,...,0xf8" inside the text after `from`; for tid. pv/nx = expected tags (NULL = any) */
static int has_row_t(const char *from, uint64_t tid, const char *pv, const char *nx, unsigned n, long dt_us, uint64_t pc, uint64_t lr)
{
	for (const char *p = strstr(from, "SWEETGL: GAPW pid "); p; p = strstr(p + 1, "SWEETGL: GAPW pid ")) {
		const char *e = strchr(p, '\n');
		char line[512];
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l >= sizeof line) continue;
		memcpy(line, p, l); line[l] = 0;
		unsigned long long rtid, rpc, rlr, f[8], rdt;
		unsigned rn, rsec;
		int pid;
		char img[64], rp[16], rx[16];
		unsigned long long off;
		if (sscanf(line, "SWEETGL: GAPW pid %d tid 0x%llx sec %u prev %15s next %15s n %u dt_us %llu pc 0x%llx off 0x%llx img %63s lr 0x%llx fr 0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx",
		    &pid, &rtid, &rsec, rp, rx, &rn, &rdt, &rpc, &off, img, &rlr, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7]) != 19) continue;
		if (rtid != tid || rn != n || rpc != pc || rlr != lr) continue;
		if (pv && strcmp(pv, rp)) continue;
		if (nx && strcmp(nx, rx)) continue;
		if (dt_us >= 0 && (long)rdt != dt_us) continue;
		int ok = !strcmp(img, "-") && !off;
		for (int i = 0; i < 8; i++) if (f[i] != 0xf1 + (unsigned)i) ok = 0;
		if (!ok) continue;
		return 1;
	}
	return 0;
}
static int has_row(const char *from, uint64_t tid, unsigned n, long dt_us, uint64_t pc, uint64_t lr)
{
	return has_row_t(from, tid, NULL, NULL, n, dt_us, pc, lr);
}
/* GAPWSUM of tid after `from`: fills v[] (indices V_*) */
enum { V_TICKS, V_KEPT, V_DROP, V_FULL, V_NOTWIN, V_BADSP, V_ERR, V_RERR, V_NOTAG, V_STOP, V_NOSLOT, V_DEAD, V_N };
static int sum_of(const char *from, uint64_t tid, unsigned long long v[V_N])
{
	char pat[64];
	snprintf(pat, sizeof pat, "SWEETGL: GAPWSUM pid ");
	for (const char *p = strstr(from, pat); p; p = strstr(p + 1, pat)) {
		unsigned long long rtid;
		unsigned rsec;
		int pid;
		if (sscanf(p, "SWEETGL: GAPWSUM pid %d tid 0x%llx sec %u ticks %llu kept %llu drop %llu full %llu notwin %llu badsp %llu err %llu rerr %llu notag %llu stop_us %llu noslot %llu dead %llu",
		    &pid, &rtid, &rsec, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) == 15 && rtid == tid) return 1;
	}
	return 0;
}
/* make the thread's window (>= 1 s) end: the next kernel call flushes. Returns the text offset to search from. */
static size_t flush_now(void)
{
	size_t at = errlen();
	jump_us(1500000);
	(void)mk();
	return at;
}

static sem_t s_b_ready, s_b_go, s_b_done;
static uint64_t b_tid;
static void *thread_b(void *arg)
{
	(void)arg;
	struct chain c;
	uint64_t r[4];
	b_tid = (uint64_t)(uintptr_t)pthread_self();
	CHECK(mk() == 0);                                  /* its own state: slot attached, window closed */
	build_chain(&c, r, 0x7000, 0x7100);
	set_regs(b_tid, r);
	CHECK(submit() == 0);
	sweet_gapt_mark(1);                                /* B's window opens; main's stays as main left it */
	sem_post(&s_b_ready);
	sem_wait(&s_b_go);                                 /* main ticks here */
	sweet_gapt_mark(2);
	size_t at = flush_now();                           /* B's own flush prints B's rows */
	unsigned long long v[V_N];
	const char *txt = errtext() + at;
	CHECK(has_row(txt, b_tid, 2, -1, 0x7000, 0x7100));
	CHECK(sum_of(txt, b_tid, v) && v[V_TICKS] == 2 && v[V_KEPT] == 2 && v[V_DEAD] == 0);
	/* three suspend errors in a row disable the slot: the sampler stops asking, one loud line, dead 1 in the summary */
	sweet_gapt_mark(1);
	find(b_tid)->susp_rc = -1;
	sem_post(&s_b_ready);
	sem_wait(&s_b_go);
	at = flush_now();
	txt = errtext() + at;
	CHECK(sum_of(txt, b_tid, v) && v[V_TICKS] == 3 && v[V_ERR] == 3 && v[V_DEAD] == 1);
	{
		char pat[128];
		snprintf(pat, sizeof pat, "SWEETGL: GAPW unavailable: tid 0x%llx disabled after 3 consecutive suspend/get_state errors", (unsigned long long)b_tid);
		CHECK(count_in(errtext(), pat) == 1);
		at = flush_now();                                  /* another window: the dead slot is reported in the summary again but the loud line is not repeated */
		CHECK(sum_of(errtext() + at, b_tid, v) && v[V_DEAD] == 1);
		CHECK(count_in(errtext(), pat) == 1);
	}
	sem_post(&s_b_done);
	return NULL;
}

static void *thread_fill(void *arg)
{
	(void)arg;
	CHECK(mk() == 0);
	sweet_gapt_mark(1);                  /* no slot: no window, no crash */
	sweet_gapt_mark(2);
	(void)wait_bo();
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
	if (!strcmp(mode, "nogapt")) { unsetenv("SWEETGL_GAPT"); setenv("SWEETGL_GAPW", "1", 1); }
	else if (!strcmp(mode, "on")) { setenv("SWEETGL_GAPT", "1", 1); setenv("SWEETGL_GAPW", "1", 1); }
	else if (!strcmp(mode, "off")) { setenv("SWEETGL_GAPT", "1", 1); unsetenv("SWEETGL_GAPW"); }
	else if (!strcmp(mode, "bad")) { setenv("SWEETGL_GAPT", "1", 1); setenv("SWEETGL_GAPW", "x", 1); }
	else { printf("FAIL usage: mode on|off|bad|nogapt\n"); return 1; }
	setenv("SWEETGL_BOCUT", "0", 1);
	setenv("SWEETGL_BOPOOL", "0", 1);
	sweet_gapt_clock = clk;
	sweet_gapw_suspend = f_suspend;
	sweet_gapw_getstate = f_getstate;
	sweet_gapw_resume = f_resume;
	devfd = sweet_drm_mali_open();
	CHECK(devfd >= 0);
	uint64_t me = (uint64_t)(uintptr_t)pthread_self();
	struct chain c;
	uint64_t r[4];
	build_chain(&c, r, 0x1000, 0x2000);
	set_regs(me, r);

	if (strcmp(mode, "on")) {
		/* every other mode: no slot, no sampling, nothing sampler-related except the one loud line (bad / nogapt) */
		CHECK(mk() == 0);
		sweet_gapt_mark(1);
		CHECK(submit() == 0);
		sweet_gapt_mark(1);
		for (int i = 0; i < 5; i++) sweet_gapw_tick();
		(void)flush_now();
		CHECK(n_susp == 0 && n_get == 0 && n_res == 0);
		CHECK(count_in(errtext(), "SWEETGL: GAPW pid ") == 0);
		CHECK(count_in(errtext(), "SWEETGL: GAPWSUM") == 0);
		CHECK(count_in(errtext(), "SWEETGL: GAPWREST") == 0);
		CHECK(count_in(errtext(), "SWEETGL: GAPW on") == 0);
		if (!strcmp(mode, "bad")) {
			CHECK(count_in(errtext(), "SWEETGL: GAPW unavailable: SWEETGL_GAPW=\"x\" is not 0 or 1, window sampler OFF") == 1);
		} else if (!strcmp(mode, "nogapt")) {
			CHECK(count_in(errtext(), "SWEETGL: GAPW unavailable: SWEETGL_GAPW=\"1\" needs SWEETGL_GAPT=1 (gap timing is off), window sampler OFF") == 1);
			CHECK(count_in(errtext(), "SWEETGL: GAPT") == 0);
		} else {
			CHECK(count_in(errtext(), "GAPW unavailable") == 0);     /* off: silent, as every default-off knob */
			CHECK(count_in(errtext(), "SWEETGL: GAPT on pid") == 1);
		}
		printf("%s %s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", mode, checks, fails);
		return fails ? 1 : 0;
	}

	/* ---- on ---- */
#ifdef GAPW_TOP_SMALL
#define TOP_N 32
	CHECK(count_in(errtext(), "SWEETGL: GAPW on pid") == 1 && count_in(errtext(), " period_us 2500 ring 256 frames 8 top 32 v2\n") == 1);
#else
#define TOP_N 256
	CHECK(count_in(errtext(), "SWEETGL: GAPW on pid") == 1 && count_in(errtext(), " period_us 2500 ring 256 frames 8 top 256 v2\n") == 1);
#endif
	CHECK(mk() == 0);
	sweet_gapw_tick();                                   /* window closed: no stop at all */
	CHECK(n_susp == 0);

	/* A. aggregation + dt: 5 x pc 0x1000, 2 x pc 0x1004 (other lr), 1 x pc 0x1008; the window opens at mark 1, the first tick is 300 us later */
	CHECK(submit() == 0);
	sweet_gapt_mark(1);
	jump_us(300);
	for (int i = 0; i < 5; i++) sweet_gapw_tick();
	build_chain(&c, r, 0x1004, 0x2004); set_regs(me, r);
	for (int i = 0; i < 2; i++) sweet_gapw_tick();
	build_chain(&c, r, 0x1008, 0x2008); set_regs(me, r);
	sweet_gapw_tick();
	CHECK(n_susp == 8 && n_get == 8 && n_res == 8 && stopped == 0);
	build_chain(&c, r, 0x1008, 0x2010); set_regs(me, r);
	sweet_gapt_mark(2);
	sweet_gapw_tick();                                   /* mark 2 closed the window */
	CHECK(n_susp == 8);
	sweet_gapt_mark(1);                                  /* a window the next kernel entry closes (no mark 2) */
	sweet_gapw_tick();
	CHECK(n_susp == 9);
	(void)wait_bo();                                     /* kernel entry: closes the window */
	sweet_gapw_tick();
	CHECK(n_susp == 9);
	CHECK(!printed_while_stopped);
	build_chain(&c, r, 0x1000, 0x2000); set_regs(me, r);
	size_t at = flush_now();
	const char *txt = errtext() + at;
	/* dt: mark 1 reads T, the first tick reads T+1000+300000: 301 us for the first sample; the later ticks add 3 clock reads (suspend t0, stop read, ...) so check the first group by its mean range */
	CHECK(has_row(txt, me, 5, 305, 0x1000, 0x2000));     /* dt of the five samples 301 303 305 307 309 us: the group mean */
	CHECK(has_row(txt, me, 2, -1, 0x1004, 0x2004));
	CHECK(has_row(txt, me, 1, -1, 0x1008, 0x2008));
	CHECK(has_row(txt, me, 1, -1, 0x1008, 0x2010));      /* same pc, other lr: its own group */
	CHECK(has_row(txt, me, 1, -1, 0x1000, 0x2000) == 0);
	{
		const char *p1 = strstr(txt, "pc 0x1000 "), *p2 = strstr(txt, "pc 0x1004 "), *p3 = strstr(txt, "pc 0x1008 ");
		CHECK(p1 && p2 && p3 && p1 < p2 && p2 < p3);      /* sorted by n, descending */
	}
	unsigned long long v[V_N];
	CHECK(sum_of(txt, me, v));
	CHECK(v[V_TICKS] == 9 && v[V_KEPT] == 9 && v[V_DROP] == 0 && v[V_FULL] == 0 && v[V_NOTWIN] == 0 && v[V_BADSP] == 0 && v[V_ERR] == 0 && v[V_RERR] == 0 && v[V_NOTAG] == 0 && v[V_NOSLOT] == 0 && v[V_DEAD] == 0);   /* 9 ticks, all 9 kept (8 + the window that the kernel entry closed) */
	CHECK(v[V_TICKS] == v[V_KEPT] + v[V_DROP] + v[V_FULL] + v[V_NOTWIN] + v[V_BADSP] + v[V_ERR]);      /* ticks = kept + drop + full + notwin + badsp + err */
	CHECK(v[V_STOP] == 9);                                /* 9 stops of one 1 us clock step each */
	/* both windows of this gap (mark 1..2 and mark 1..next kernel entry) belong to the one gap submit -> wait: every row carries those tags, none is unknown */
	CHECK(has_row_t(txt, me, "submit", "wait", 5, 305, 0x1000, 0x2000) && has_row_t(txt, me, "submit", "wait", 2, -1, 0x1004, 0x2004));
	CHECK(has_row_t(txt, me, "submit", "wait", 1, -1, 0x1008, 0x2008) && has_row_t(txt, me, "submit", "wait", 1, -1, 0x1008, 0x2010));
	CHECK(count_in(txt, "prev ? ") == 0 && count_in(txt, " next ? ") == 0);
	CHECK(count_in(txt, "SWEETGL: GAPWREST pid") == 1);
	CHECK(strstr(strstr(txt, "SWEETGL: GAPWREST pid"), "groups 0 n 0"));

	/* B. dt of a single sample: mark 1, +400 us, one tick: dt_us 401 */
	build_chain(&c, r, 0x1100, 0x2100); set_regs(me, r);
	sweet_gapt_mark(1);
	jump_us(400);
	sweet_gapw_tick();
	sweet_gapt_mark(2);
	at = flush_now();
	txt = errtext() + at;
	CHECK(has_row(txt, me, 1, 401, 0x1100, 0x2100));
	CHECK(sum_of(txt, me, v) && v[V_STOP] == 1);                 /* the stop time is per window, not cumulative */

	/* C. every way a tick can end: notwin (the window closes between the pre-check and the stop), badsp (sp outside the stack), get_state error, suspend error; counted, never dropped silently */
	build_chain(&c, r, 0x1200, 0x2200); set_regs(me, r);
	sweet_gapt_mark(1);
	suspend_closes_window = 1;
	sweet_gapw_tick();                                   /* notwin; resumed */
	CHECK(n_res == n_susp);
	sweet_gapt_mark(1);
	{
		uint64_t bad[4] = { 0x1300, 0x2300, r[2], 0 };   /* sp 0 */
		set_regs(me, bad);
		sweet_gapw_tick();                               /* badsp */
		bad[3] = (uint64_t)(uintptr_t)&c + (1ull << 40); /* above the stack */
		set_regs(me, bad);
		sweet_gapw_tick();                               /* badsp */
	}
	set_regs(me, r);
	find(me)->get_rc = -1;
	sweet_gapw_tick();                                   /* get_state error: resumed, err */
	CHECK(n_res == n_susp);
	find(me)->get_rc = 0;
	find(me)->susp_rc = -1;
	sweet_gapw_tick();                                   /* suspend error: err, no resume (it never stopped) */
	CHECK(n_res + 1 == n_susp);
	find(me)->susp_rc = 0;
	sweet_gapw_tick();                                   /* a good sample resets the error run: 2 errors do not disable the slot */
	sweet_gapw_tick();
	find(me)->get_rc = -1;
	sweet_gapw_tick();                                   /* a third error, but not in a row: still not disabled */
	find(me)->get_rc = 0;
	find(me)->res_rc = -1;
	sweet_gapw_tick();                                   /* resume fails: counted as rerr, the sample itself is kept */
	find(me)->res_rc = 0;
	sweet_gapt_mark(2);
	CHECK(!printed_while_stopped);
	at = flush_now();
	txt = errtext() + at;
	CHECK(sum_of(txt, me, v));
	CHECK(v[V_TICKS] == 9 && v[V_KEPT] == 3 && v[V_DROP] == 0 && v[V_FULL] == 0 && v[V_NOTWIN] == 1 && v[V_BADSP] == 2 && v[V_ERR] == 3 && v[V_RERR] == 1 && v[V_STOP] == 9 && v[V_DEAD] == 0);   /* stop_us 9: seven 1 us stops and the notwin one 2 us (its hook reads the clock in mark 2) */
	CHECK(v[V_TICKS] == v[V_KEPT] + v[V_DROP] + v[V_FULL] + v[V_NOTWIN] + v[V_BADSP] + v[V_ERR]);
	CHECK(count_in(errtext(), "disabled after") == 0);

	/* D. ring full (v2): 256 + 44 ticks in one window: 256 kept, the other 44 are `full` and cost NO stop (v1 stopped the thread 44 more times and dropped the samples); drop stays 0 */
	sweet_gapt_mark(1);
	unsigned susp0 = n_susp, res0 = n_res;
	for (int i = 0; i < 300; i++) sweet_gapw_tick();
	CHECK(n_susp == susp0 + 256 && n_res == res0 + 256 && stopped == 0);
	sweet_gapt_mark(2);
	at = flush_now();
	txt = errtext() + at;
	CHECK(sum_of(txt, me, v));
	CHECK(v[V_TICKS] == 300 && v[V_KEPT] == 256 && v[V_DROP] == 0 && v[V_FULL] == 44 && v[V_STOP] == 256 && v[V_DEAD] == 0);
	CHECK(v[V_TICKS] == v[V_KEPT] + v[V_DROP] + v[V_FULL] + v[V_NOTWIN] + v[V_BADSP] + v[V_ERR]);
	CHECK(has_row(txt, me, 256, -1, 0x1200, 0x2200));
	/* the ring is empty again after the flush: a new window fills it from zero */
	sweet_gapt_mark(1);
	for (int i = 0; i < 3; i++) sweet_gapw_tick();
	sweet_gapt_mark(2);
	at = flush_now();
	CHECK(has_row(errtext() + at, me, 3, -1, 0x1200, 0x2200));

	/* D2. drop: only reachable with a second producer (a nested pass inside the stop): 255 samples in the ring, the nested pass fills the last slot, the outer sample finds the ring full
	 * after its stop: counted as drop (never overwrites), ticks = kept + drop + full + ... still adds up */
	sweet_gapt_mark(1);
	for (int i = 0; i < 255; i++) sweet_gapw_tick();
	nest_tick = 1;
	sweet_gapw_tick();
	sweet_gapt_mark(2);
	at = flush_now();
	txt = errtext() + at;
	CHECK(sum_of(txt, me, v) && v[V_TICKS] == 257 && v[V_KEPT] == 256 && v[V_DROP] == 1 && v[V_FULL] == 0);
	CHECK(v[V_TICKS] == v[V_KEPT] + v[V_DROP] + v[V_FULL] + v[V_NOTWIN] + v[V_BADSP] + v[V_ERR]);
	CHECK(has_row(txt, me, 256, -1, 0x1200, 0x2200));      /* the 256 kept samples are all intact */

	/* E. 40 groups: v2 prints every group (bound TOP_N = 256; the GAPW_TOP_SMALL build keeps 32 and sums the rest in GAPWREST) */
	sweet_gapt_mark(1);
	for (int i = 0; i < 40; i++) {
		build_chain(&c, r, 0x5000 + 4 * i, 0x6000); set_regs(me, r);
		sweet_gapw_tick();
		if (i < 3) sweet_gapw_tick();                       /* groups 0..2 have n 2 and come first */
	}
	sweet_gapt_mark(2);
	at = flush_now();
	txt = errtext() + at;
	CHECK(count_in(txt, "SWEETGL: GAPW pid") == (TOP_N < 40 ? TOP_N : 40));
	CHECK(has_row(txt, me, 2, -1, 0x5000, 0x6000) && has_row(txt, me, 2, -1, 0x5008, 0x6000));
	CHECK(strstr(txt, "SWEETGL: GAPWREST pid"));
	if (TOP_N < 40) {
		CHECK(strstr(strstr(txt, "SWEETGL: GAPWREST pid"), "groups 8 n 8"));
	} else {
		CHECK(strstr(strstr(txt, "SWEETGL: GAPWREST pid"), "groups 0 n 0"));
		CHECK(has_row(txt, me, 1, -1, 0x5000 + 4 * 39, 0x6000));        /* the last group prints too */
	}

	/* E2. tags: the same (pc, lr, frames) in two gaps with different closing kernel calls are two groups; a gap with two windows is one tag;
	 * the gap closed by the call that also flushes (a window of >= 1 s) is tagged by THAT call, not `?` */
	build_chain(&c, r, 0x3000, 0x3100); set_regs(me, r);
	(void)wait_bo();
	CHECK(submit() == 0);
	sweet_gapt_mark(1);                                  /* gap 1: submit -> wait, two windows */
	sweet_gapw_tick(); sweet_gapw_tick();
	sweet_gapt_mark(2);
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	(void)wait_bo();                                     /* closes gap 1: prev submit next wait, 3 samples */
	sweet_gapt_mark(1);                                  /* gap 2: wait -> bo_new (also the flushing call), same pc */
	sweet_gapw_tick(); sweet_gapw_tick();
	jump_us(1500000);
	CHECK(mk() == 0);                                    /* closes gap 2: prev wait next bo_new, 2 samples; this call flushes */
	at = errlen();
	{
		/* the flush ran inside mk(): its text is already in the file, find it from the last GAPWSUM-less position */
		const char *all = errtext();
		const char *q = all + (at > 6000 ? at - 6000 : 0);
		CHECK(has_row_t(q, me, "submit", "wait", 3, -1, 0x3000, 0x3100));
		CHECK(has_row_t(q, me, "wait", "bo_new", 2, -1, 0x3000, 0x3100));
		CHECK(has_row_t(q, me, "submit", "wait", 5, -1, 0x3000, 0x3100) == 0);   /* never merged across gaps */
	}

	/* E3. a gap whose closing call is lost (the 4096-entry table slot of its serial is reused by a later sampled gap before the flush) prints `prev ? next ?` and counts notag; never a guess */
	build_chain(&c, r, 0x3200, 0x3300); set_regs(me, r);
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	(void)wait_bo();                                     /* gap A: sampled, booked */
	for (int i = 0; i < 4095; i++) (void)wait_bo();      /* 4095 empty gaps: 4096 serials later the table slot of gap A is the same */
	build_chain(&c, r, 0x3400, 0x3500); set_regs(me, r);
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	(void)wait_bo();                                     /* gap A + 4096: sampled, booked over A's slot */
	at = flush_now();
	txt = errtext() + at;
	CHECK(has_row_t(txt, me, "?", "?", 1, -1, 0x3200, 0x3300));
	CHECK(has_row_t(txt, me, "wait", "wait", 1, -1, 0x3400, 0x3500));
	CHECK(sum_of(txt, me, v) && v[V_NOTAG] == 1 && v[V_KEPT] == 2);

	/* E4. same prev, different closing call, same (pc, lr, frames): two groups (wait -> wait, wait -> bo_new) */
	build_chain(&c, r, 0x3600, 0x3700); set_regs(me, r);
	(void)wait_bo();
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	(void)wait_bo();                                     /* prev wait next wait */
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	CHECK(mk() == 0);                                    /* prev wait next bo_new */
	at = flush_now();
	txt = errtext() + at;
	{
		const char *q = errtext() + (at > 3000 ? at - 3000 : 0);
		CHECK(has_row_t(q, me, "wait", "wait", 1, -1, 0x3600, 0x3700) && has_row_t(q, me, "wait", "bo_new", 1, -1, 0x3600, 0x3700));
		CHECK(has_row_t(q, me, "wait", "wait", 2, -1, 0x3600, 0x3700) == 0 && has_row_t(q, me, "wait", "bo_new", 2, -1, 0x3600, 0x3700) == 0);
	}

	/* E5. the table keeps gaps 2048 serials apart (half of GAPW_NX = 4096): both samples are tagged, nothing is `?` */
	build_chain(&c, r, 0x3800, 0x3900); set_regs(me, r);
	(void)wait_bo();
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	(void)wait_bo();
	for (int i = 0; i < 2047; i++) (void)wait_bo();      /* the next sampled gap is serial A + 2048: the same slot in a 2048-entry table */
	build_chain(&c, r, 0x3a00, 0x3b00); set_regs(me, r);
	sweet_gapt_mark(1);
	sweet_gapw_tick();
	(void)wait_bo();
	at = flush_now();
	txt = errtext() + at;
	CHECK(has_row_t(txt, me, "wait", "wait", 1, -1, 0x3800, 0x3900) && has_row_t(txt, me, "wait", "wait", 1, -1, 0x3a00, 0x3b00));
	CHECK(sum_of(txt, me, v) && v[V_NOTAG] == 0 && v[V_KEPT] == 2);

	/* F. a second thread has its own slot, window and rows; main's window is closed while B's is open, so only B is stopped */
	build_chain(&c, r, 0x1000, 0x2000); set_regs(me, r);
	sem_init(&s_b_ready, 0, 0); sem_init(&s_b_go, 0, 0); sem_init(&s_b_done, 0, 0);
	pthread_t th;
	CHECK(pthread_create(&th, NULL, thread_b, NULL) == 0);
	sem_wait(&s_b_ready);
	unsigned s0 = n_susp;
	sweet_gapw_tick();
	sweet_gapw_tick();
	CHECK(n_susp == s0 + 2);                               /* only B's slot was stopped */
	sem_post(&s_b_go);
	sem_wait(&s_b_ready);
	for (int i = 0; i < 4; i++) sweet_gapw_tick();         /* B's suspend now fails: 3 errors disable it, the 4th tick does not ask */
	CHECK(n_susp == s0 + 2 + 3);
	sem_post(&s_b_go);
	sem_wait(&s_b_done);
	pthread_join(th, NULL);

	/* a thread whose stack bounds are unknown is not sampled: one loud line, no slot taken */
	sweet_gapw_nobounds = 1;
	{ pthread_t t; CHECK(pthread_create(&t, NULL, thread_fill, NULL) == 0); pthread_join(t, NULL); }
	sweet_gapw_nobounds = 0;
	{
		unsigned lines = 0;     /* lines carrying both the GAPW prefix and the no-bounds text (the GAPS line has its own, different text) */
		for (const char *p = strstr(errtext(), "SWEETGL: GAPW unavailable: tid 0x"); p; p = strstr(p + 1, "SWEETGL: GAPW unavailable: tid 0x")) {
			const char *e = strchr(p, '\n');
			const char *q = strstr(p, " has no stack bounds, the thread is not sampled");
			if (q && e && q < e) lines++;
		}
		CHECK(lines == 1);
	}
	/* G. more than GAPW_MAX_SLOTS (16) threads: main and B hold two slots, 14 more fill the table, the 15th gets none: one loud line, counted as noslot, the thread works as before */
	for (int i = 0; i < 15; i++) {
		pthread_t t;
		CHECK(pthread_create(&t, NULL, thread_fill, NULL) == 0);
		pthread_join(t, NULL);
	}
	CHECK(count_in(errtext(), "SWEETGL: GAPW unavailable: more than 16 threads, tid 0x") == 1);
	at = flush_now();
	CHECK(sum_of(errtext() + at, me, v) && v[V_NOSLOT] == 1);

	CHECK(!printed_while_stopped);
	printf("%s on: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails ? 1 : 0;
}
