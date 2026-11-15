/*
 * sweet_drm_mali: see sweet_drm_mali.h and README.md. Independent implementation of the libdrm API shape for Mali.
 *
 * Conventions (libdrm, [guess: libdrm source not at hand]): drmIoctl/drmCloseBufferHandle/drmSyncobj* return 0, or -1 with
 * errno set; drmGetCap/drmPrime* return -errno (and set errno too). Mesa only tests the result for != 0 / >= 0.
 * One mutex protects everything; it is NOT held while waiting for the GPU.
 */
#ifdef SWEET_DRM_HOST_TEST
#define _GNU_SOURCE   /* pthread_getattr_np for the gap-site stack bounds */
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

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

enum { SEL_QUERY, SEL_BO_NEW, SEL_BO_CLOSE, SEL_SUBMIT, SEL_WAIT, SEL_IMPORT };
#define SWEET_MALI_EXEC   1u
#define SWEET_MALI_REQ_FS 1u
#define SWEET_MALI_REQ_OVL 2u               /* kernel header value (T-34eb): this job may start on JS1 beside one running FS job; the kernel needs sweetmaliovl=1 or it ignores and counts it */
#define WAIT_CHUNK_US     1000000u          /* the kernel clamps one wait to 60 s; loop in 1 s slices */
#define NOFENCE           UINT64_MAX        /* syncobj without a fence (created unsignalled / reset) */
#define MAX_HANDLES       (1u << 20)
#define MAX_FDS           (1u << 16)

struct bo {
	bool used, heap;
	bool exec;                          /* created without PANFROST_BO_NOEXEC (pool key, T-bf4f) */
	bool pooled;                        /* closed by Mesa but kept alive in the BO pool (SWEETGL_BOPOOL, T-bf4f): invisible to bo_get */
	uint64_t va, size, map;
	uint32_t surf;                      /* IOSurface id if this BO is an import (selector 5), else 0 */
	bool taken;                         /* an import handed to Mesa by drmPrimeFDToHandle (then Mesa owns the close) */
	uint64_t seq;                       /* kernel seqno of the last submit that listed this BO in bo_handles (0 = never); used by WAIT_BO when SWEETGL_BOCUT=1 */
	struct sweet_drm_surface info;
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

/* The one device context, created lazily (ponytail: one device per process). */
static struct {
	pthread_mutex_t lock;
	bool up;
	io_connect_t conn;
	int dev_fd;
	struct sweet_mali_info info;
	uint64_t last_seq, done_seq;        /* last submitted / highest seqno known complete (in-order queue) */
	bool prev_fs;                       /* the shim's own previous submit succeeded and was an FS submit (T-34eb OVL pass-through check) */
	struct bo *bos;
	uint32_t nbos;
	struct sobj *sobjs;
	uint32_t nsobjs;
	struct fdrec *fds;
	uint32_t nfds;
} C = { .lock = PTHREAD_MUTEX_INITIALIZER, .dev_fd = -1 };

/* ---- BO churn counters and the churn cut (P-G1 slice 6, T-2275) ----
 * Why (T-1380 proof): backboardd's hot thread spends 13.4 of 47.9 ms per distinct frame creating, mapping, unmapping and closing Mali BOs. Mesa has a BO cache
 * (pan_bo.c panfrost_bo_cache_fetch/put); two things defeat it here [src, Mesa 26.2.5]:
 *  1. WAIT_BO waited for the LAST SUBMIT OF THE DEVICE (bo_handles ignored), so fetch(dontwait) (pan_bo.c:210-214) finds every cached BO "busy" as soon as the
 *     frame's first batch is submitted and allocates a fresh BO. With SWEETGL_BOCUT=1 WAIT_BO waits for the last submit that LISTED this BO (Linux panfrost does
 *     the same through the BO's fences; Mesa lists every BO of the batch incl. both pools, pan_jm.c:101-150).
 *  2. panfrost_bo_unreference munmaps BEFORE it puts the BO in the cache (pan_bo.c:445-453), so every recycle paid unmap + map. With SWEETGL_BOCUT=1
 *     darwin_drm_bo_unmap keeps the CPU mapping (darwin_drm_bo_map already returns b->map when it is set); close_bo drops it.
 * SWEETGL_BOCNT=1 logs one line per second (printed lazily by the first ioctl of the next second, no thread). Both knobs: unset/""/"0" off, "1" on, anything
 * else = one loud line and OFF. Delivered by the sweetenv boot-arg like SWEETGL_FLUSHT. Default off: a process without the env behaves as before. */
enum {
	N_CREATE, N_CREATE_B, N_CREATE_FAIL, N_CLOSE, N_MAP, N_MAP_HIT, N_MAP_B, N_UNMAP, N_UNMAP_KEPT, N_SUBMIT, N_SUBMIT_BOS, N_SUBMIT_UNK, N_SUBMIT_OVL, N_SUBMIT_OVL_DROP,
	N_WAIT, N_WAIT_READY, N_WAIT_BUSY, N_WAIT_ERR, N_WAIT_KCALL, N_MADV,
	N_POOL_HIT, N_POOL_HIT_B, N_POOL_MISS, N_POOL_PUT, N_POOL_EVICT, N_POOL_EVICT_FAIL, N_POOL_BUSY, N_POOL_ERR, N_UNMAP_FAIL, N_UNMAP_FAIL_B, N_COUNT
};
#define SZ_BUCKETS 16                   /* create requests by floor(log2(size)) - 12: 4 KB .. 128 MB+, T-bf4f */
static uint64_t cnt[N_COUNT], szh[SZ_BUCKETS];
static int cfg_cnt, cfg_cut, cfg_pool, cfg_done;
static struct {
	pthread_mutex_t mu;
	bool have;
	time_t sec0;
	unsigned cur;
} win = { .mu = PTHREAD_MUTEX_INITIALIZER };
static uint64_t unmap_fail_n, unmap_fail_b;   /* L-01: failed CPU unmaps since process start (count, bytes); counted with or without SWEETGL_BOCNT */
#define CNT(i, v) do { if (cfg_cnt) __atomic_fetch_add(&cnt[i], (uint64_t)(v), __ATOMIC_RELAXED); } while (0)

/* ---- BO recycle pool (P-G1 slice 7, T-bf4f) ----
 * Why (T-2275 proof): after the cut backboardd still makes 1.57 BOs of 1.23 MB per distinct frame; the kernel boNew (patches 0104/0105) bzeroes and flushes every
 * 16 KB, ~1.7 ms per create, and Mesa's BO cache (pan_bo.c:206-221) only fits a request into a cached BO of the SAME floor-log2 bucket that is at least as big, and
 * drops it after ~2 s. SWEETGL_BOPOOL=1 (needs BOCUT=1: it relies on the per-BO seqno) does two things [src]:
 *  1. rounds a create up to a size class (16 KB steps to 64 KB; above that the next multiple of 1/8 of the power of two below, <= 12.5 % waste above 128 KB) and returns the CLASS
 *     size to Mesa (panfrost_kmod_bo_alloc takes req.size back), so a later request of nearly the same size fits a cached BO;
 *  2. keeps a BO Mesa closes (CPU map and kernel BO alive, handle invisible to Mesa) in a pool of at most POOL_MAX_BOS / POOL_MAX_BYTES and hands it to the next
 *     create of the same class and EXEC flag once its last submit finished (non-blocking poll of the kernel seqno; a busy one is skipped, never waited for).
 * A recycled BO is NOT zeroed, exactly like a BO Mesa's own cache returns (R1: Mesa already tolerates it; the run checks for MMU faults/job errors).
 * Heap BOs and IOSurface imports are never pooled or rounded. Default ON since T-bf4f (BOCUT too); SWEETGL_BOPOOL=0 / SWEETGL_BOCUT=0 turn them off. */
#define POOL_MAX_BOS   128
#define POOL_MAX_BYTES (64ull << 20)
static uint32_t pool[POOL_MAX_BOS];     /* handles, oldest first; lock held */
static uint32_t npool;
static uint64_t pool_bytes;

/* "1" = on, "0" = off, unset/"" = def (the knob's default), else -1 (bad). */
static int
cfg_parse(const char *e, int def)
{
	if (!e || !e[0]) {
		return def;
	}
	if (e[0] == '0' && !e[1]) {
		return 0;
	}
	return e[0] == '1' && !e[1] ? 1 : -1;
}

static int
cfg_knob(const char *name, int def)
{
	const char *e = getenv(name);
	int v = cfg_parse(e, def);
	if (v < 0) {
		fprintf(stderr, "SWEETGL: BOCNT unavailable: %s=\"%.16s\" is not 0 or 1, knob OFF\n", name, e);
		v = 0;
	}
	return v;
}

static void gapt_cfg_init(void);
static int lagph_on(void);

/* Lock held, once. */
static void
cfg_init(void)
{
	cfg_done = 1;
	cfg_cnt = cfg_knob("SWEETGL_BOCNT", 0);
	cfg_cut = cfg_knob("SWEETGL_BOCUT", 1);
	cfg_pool = cfg_knob("SWEETGL_BOPOOL", 1);
	if (cfg_pool && !cfg_cut) {
		fprintf(stderr, "SWEETGL: BOCNT unavailable: SWEETGL_BOPOOL=1 needs SWEETGL_BOCUT=1 (per-BO seqno), pool OFF\n");
		cfg_pool = 0;
	}
	if (cfg_cnt || cfg_cut || cfg_pool) {
		fprintf(stderr, "SWEETGL: BOCNT on %d cut %d pid %d\n", cfg_cnt, cfg_cut, (int)getpid());
	}
	gapt_cfg_init();
	(void)lagph_on();   /* T-9577: read SWEETGL_LAGPH once, under the lock, before any thread submits */
	if (cfg_pool) {
		fprintf(stderr, "SWEETGL: BOPOOL on pid %d max_bos %d max_bytes %llu\n", (int)getpid(), POOL_MAX_BOS, (unsigned long long)POOL_MAX_BYTES);
	}
}

/* Lock held: print the window that just ended, if the second changed and anything was counted. */
static void
cnt_tick(void)
{
	struct timespec ts;
	if (!cfg_cnt || clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return;
	}
	pthread_mutex_lock(&win.mu);
	if (!win.have) {
		win.have = true;
		win.sec0 = ts.tv_sec;
		win.cur = 0;
	}
	unsigned wid = ts.tv_sec > win.sec0 ? (unsigned)(ts.tv_sec - win.sec0) : 0;
	if (wid > win.cur) {
		unsigned long long v[N_COUNT], h[SZ_BUCKETS];
		uint64_t any = 0, anyh = 0;
		for (int i = 0; i < N_COUNT; i++) {
			v[i] = __atomic_exchange_n(&cnt[i], 0, __ATOMIC_RELAXED);
			any |= v[i];
		}
		for (int i = 0; i < SZ_BUCKETS; i++) {
			h[i] = __atomic_exchange_n(&szh[i], 0, __ATOMIC_RELAXED);
			anyh |= h[i];
		}
		if (any) {
			unsigned live = 0, imp = 0;
			uint64_t live_b = 0, imp_b = 0, map_b = 0;
			for (uint32_t i = 0; i < C.nbos; i++) {   /* sprint GPU 2026-11-10: backboardd grows ~250 MB per nav rep; split the live set by kind */
				if (!C.bos[i].used || C.bos[i].pooled) continue;
				live++; live_b += C.bos[i].size;
				if (C.bos[i].surf) { imp++; imp_b += C.bos[i].size; }
				if (C.bos[i].map) map_b += C.bos[i].size;
			}
			fprintf(stderr, "SWEETGL: BOLIVE pid %d sec %u live %u live_mb %llu imports %u import_mb %llu mapped_mb %llu pooled %u pooled_mb %llu\n", (int)getpid(), win.cur,
			    live, (unsigned long long)(live_b >> 20), imp, (unsigned long long)(imp_b >> 20), (unsigned long long)(map_b >> 20), npool, (unsigned long long)(pool_bytes >> 20));
			fprintf(stderr, "SWEETGL: BOCNT pid %d sec %u cut %d create %llu create_bytes %llu create_fail %llu close %llu map %llu map_hit %llu map_bytes %llu "
			    "unmap %llu unmap_kept %llu submit %llu submit_bos %llu submit_unk_bo %llu submit_ovl %llu submit_ovl_drop %llu wait %llu wait_ready %llu wait_busy %llu wait_err %llu "
			    "wait_kcall %llu madv %llu live_bos %u unmap_fail %llu unmap_fail_bytes %llu unmap_fail_total %llu\n",
			    (int)getpid(), win.cur, cfg_cut, v[N_CREATE], v[N_CREATE_B], v[N_CREATE_FAIL], v[N_CLOSE], v[N_MAP], v[N_MAP_HIT], v[N_MAP_B],
			    v[N_UNMAP], v[N_UNMAP_KEPT], v[N_SUBMIT], v[N_SUBMIT_BOS], v[N_SUBMIT_UNK], v[N_SUBMIT_OVL], v[N_SUBMIT_OVL_DROP], v[N_WAIT], v[N_WAIT_READY], v[N_WAIT_BUSY], v[N_WAIT_ERR],
			    v[N_WAIT_KCALL], v[N_MADV], live, v[N_UNMAP_FAIL], v[N_UNMAP_FAIL_B], (unsigned long long)__atomic_load_n(&unmap_fail_n, __ATOMIC_RELAXED));
			if (cfg_pool) {
				fprintf(stderr, "SWEETGL: BOPOOL pid %d sec %u hit %llu hit_bytes %llu miss %llu put %llu evict %llu evict_fail %llu busy %llu err %llu pooled %u pooled_bytes %llu\n",
				    (int)getpid(), win.cur, v[N_POOL_HIT], v[N_POOL_HIT_B], v[N_POOL_MISS], v[N_POOL_PUT], v[N_POOL_EVICT], v[N_POOL_EVICT_FAIL],
				    v[N_POOL_BUSY], v[N_POOL_ERR], npool, (unsigned long long)pool_bytes);
			}
		}
		if (anyh) {
			fprintf(stderr, "SWEETGL: BOSZ pid %d sec %u log2_4k", (int)getpid(), win.cur);
			for (int i = 0; i < SZ_BUCKETS; i++) {
				fprintf(stderr, " %llu", h[i]);
			}
			fprintf(stderr, "\n");
		}
		win.cur = wid;
	}
	pthread_mutex_unlock(&win.mu);
}

/* ---- user-mode gap timing (P-G1 slice 10, T-46d7; log-only; env SWEETGL_GAPT=1, default off) ----
 * Why (T-fb3e proof): the kernel submit holds 1.4 of the 10.6 ms per distinct frame the sampler files under selector 3; the other ~9.3 ms is CPU the hot thread
 * runs between the return of one kernel call and the next kernel entry [inferred]. This times, per thread, the wall time from the return of every SweetMali
 * IOConnectCallMethod to the entry of the next one, grouped by (previous call, next call), and the wall time inside each call kind. Wall time, not CPU: a gap
 * that includes a preemption or a block in another syscall is counted whole (the lt1ms columns keep the short gaps apart, which are CPU-like work). No behaviour
 * change: two clock reads per kernel call, no syscall (mach_absolute_time reads the counter), nothing is printed from the hot path except once per second per thread
 * (printed by that thread's own next call; the print time is outside every measured interval). Thread-local, so no lock and no cross-thread sharing.
 * Per thread and window (>= 1 s), written to stderr like BOCNT:
 *   SWEETGL: GAPT pid P tid T sec S prev A next B n N gap_us U lt1ms_n N1 lt1ms_us U1 max_us M      one line per (A, B) seen, only if the window had >= 10 calls
 *   SWEETGL: GAPTIN pid P tid T sec S kind K n N in_us U max_us M                                  wall time inside the kernel call, same condition
 *   SWEETGL: GAPTSUM pid P tid T sec S win_ms W calls N gap_us U in_us I first F                   always, last line of the group
 * Call sites (slice 11, same knob): the same window also prints, only if the window had >= 10 calls,
 *   SWEETGL: GAPS pid P tid T sec S prev A next B psite 0xA site 0xB,0xC,0xD,0xE n N gap_us U lt1ms_us U1   the top GAPT_SITE_TOP (prev, next, sites) by gap time; psite = site[0] of the
 *                                      previous call, site = the next call's caller chain (site[0] = the caller of drmIoctl & co, then three frames above it)
 *   SWEETGL: GAPSREST pid P tid T sec S sites N n N gap_us U lost_n N lost_us U nosite_n N          everything not printed above (sites = rows, lost = table full), nosite = gaps whose next call had no entry scope
 * and once per process `SWEETGL: GAPS on pid P base 0xB depth D` (B = the image's mach header, to turn the addresses into dylib offsets; 0 on the host build).
 * Mesa time marks (slice 12b, T-6717; Mesa patch 0005): Mesa calls sweet_gapt_mark(id) at the end of panfrost_batch_cleanup (1), the entry of panfrost_batch_init (2)
 * and the entry of panfrost_bo_cache_fetch (3); the mark stores a clock reading in the calling thread's state (the LAST reading of each id since the previous kernel call
 * returned wins) and nothing else. When a gap is booked as submit -> wait and the three marks are present and ordered (submit return <= 1 <= 2 <= 3 <= wait entry) it is
 * split into the segments submit_m1 (rest of the submit path + Mesa batch cleanup), m1_m2 (the caller's own work until the next batch starts), m2_m3 (batch init until the
 * cache fetch) and m3_wait (the fetch until its kernel wait). Otherwise the whole gap is booked as nomark (a missing or out-of-order mark is counted, never dropped), so
 * the segments plus nomark always sum to the GAPT `prev submit next wait` gap of the same thread and second. Printed with the GAPT rows (window with >= 10 calls):
 *   SWEETGL: GAPM pid P tid T sec S seg A n N us U                    A = submit_m1 m1_m2 m2_m3 m3_wait
 *   SWEETGL: GAPM pid P tid T sec S seg nomark n N us U nomark_n N miss1 A miss2 B miss3 C order O badid X order_pair t0_m1 a m1_m2 b m2_m3 c m3_t1 d
 *                                      (miss* = gaps where that mark was absent, order = present but out of order; order_pair splits order by the FIRST violating boundary pair of
 *                                      { submit return, mark 1, mark 2, mark 3, wait entry }, a+b+c+d = O; slice 12c, T-a479)
 * A mark id outside 1..3 is counted (badid) and prints `SWEETGL: GAPM unavailable: mark id N` once per process.
 * Window pc sampler (slice 12d, T-baf5; env SWEETGL_GAPW=1 on top of SWEETGL_GAPT=1, default off; design note docs/research/129-gapw-window-sampler-design.md): while a thread is between
 * Mesa mark 1 and mark 2 (or, if mark 2 never comes, the next SweetMali kernel entry) a sampler pthread suspends it every ~2.5 ms (400 Hz, +-25 % jitter), reads pc/lr/fp/sp, walks three frames
 * and resumes it. The sampler takes no lock, allocates nothing and prints nothing while a thread is stopped. Samples go to a per-thread SPSC ring; the thread's own flush aggregates identical
 * (prev, next, pc, lr, frames) and prints, with the GAPT rows (rows are printed whenever anything was counted):
 *   SWEETGL: GAPW pid P tid T sec S prev A next B n N dt_us D pc 0xA off 0xO img NAME lr 0xB fr 0xC,..,0xK   top GAPW_TOP groups by n (v2: GAPW_TOP = GAPW_RING = every group); dt_us = mean time since mark 1;
 *                                      off/img = pc in its image (dladdr); GAPW_FR = 8 frames; prev/next = the kernel-call kinds that open and close the gap the sample was taken in (the GAPT
 *                                      `prev .. next ..` cell of that gap; `?` = the closing call was not found in the owner's table, counted in notag)
 *   SWEETGL: GAPWREST pid P tid T sec S groups G n N                                           the groups not printed above
 *   SWEETGL: GAPWSUM pid P tid T sec S ticks K kept N drop D full F notwin W badsp B err E rerr R notag Q stop_us U noslot X dead Y
 *                                      (ticks = kept + drop + full + notwin + badsp + err; full = ring full BEFORE the stop: the thread is not suspended at all (v1 suspended it and then dropped);
 *                                      drop = ring full after the stop (cannot happen: only the sampler fills the ring; kept as an invariant counter); notwin = window ended before the stop; badsp = sp
 *                                      outside the thread's stack; err = suspend/get_state failed; rerr = resume failed; notag = kept samples drained without a prev/next tag; stop_us = total time
 *                                      the thread was stopped; noslot = threads past GAPW_MAX_SLOTS; dead = disabled)
 * kinds: bo_new bo_close submit wait (wait_seq, may block on the GPU) import wait_poll (the non-blocking poll of the BO pool); start = the thread's first call.
 * Knob like the others: unset/""/"0" off, "1" on, else one loud line and OFF; no mach_timebase_info on Apple = loud line and OFF. */
enum { GK_NONE, GK_NEW, GK_CLOSE, GK_SUBMIT, GK_WAIT, GK_IMPORT, GK_POLL, GK_N };
static const char *const gk_name[GK_N] = { "start", "bo_new", "bo_close", "submit", "wait", "import", "wait_poll" };
#define GAPT_DETAIL_MIN_CALLS 10
#define GAPT_SHORT_NS         1000000ull
#define GAPT_WINDOW_NS        1000000000ull
struct gapt_cell {
	uint64_t n, ns, max, sn, sns;     /* all gaps; gaps below GAPT_SHORT_NS */
};
/* Call-site tags (P-G1 slice 11, T-d815): the exported entry points (drmIoctl, drmSyncobj*Wait, ...) record, in a scope, the return addresses of
 * their caller chain: site[0] = the caller of the entry point (the Mesa/CA code that made the call), site[1..3] = the frames above it. A frame-pointer
 * walk, bounded by the thread's stack and by GAPT_SITE_DEPTH, no syscall. Every gap is then booked under (previous kind, next kind, previous call's
 * site[0], next call's site[0..2]) in a small per-thread hash table (calloc'ed on first use, never on the measured path of a later call). */
#define GAPT_SITE_DEPTH 4
#ifndef GAPT_TAB_N
#define GAPT_TAB_N      512               /* power of two (the host test builds a 16-row table to reach the lost path) */
#endif
#define GAPT_SITE_TOP   32                /* lines per thread and window; the rest is summed in GAPSREST */
#define GM_MARKS 3
enum { GM_SUB_M1, GM_M1_M2, GM_M2_M3, GM_M3_WAIT, GM_NOMARK, GM_N };
static const char *const gm_name[GM_N] = { "submit_m1", "m1_m2", "m2_m3", "m3_wait", "nomark" };
/* Boundary pairs of an out-of-order gap (slice 12c, T-a479): the FIRST pair (b[i-1], b[i]) of { submit return, mark 1, mark 2, mark 3, wait entry } with b[i] < b[i-1]; index 1..GM_MARKS+1 */
static const char *const gm_pair_name[GM_MARKS + 2] = { "-", "t0_m1", "m1_m2", "m2_m3", "m3_t1" };
struct gapt_site {
	bool used;
	uint8_t prev, next;
	uint64_t psite, site[GAPT_SITE_DEPTH];
	uint64_t n, ns, sns;              /* all gaps, gaps below GAPT_SHORT_NS */
};
struct gapw_slot;
struct gapt_tls {
	bool init;
	uint8_t prev;                     /* GK_NONE until this thread's first call */
	uint64_t prev_t;                  /* ns, when the previous call returned */
	uint64_t win0, tid, first;
	unsigned win_id;
	struct gapt_cell cell[GK_N][GK_N];     /* [previous kind][next kind] */
	uint64_t in_n[GK_N], in_ns[GK_N], in_max[GK_N];
	uintptr_t stack_lo, stack_hi;     /* 0/0 = unknown: no frame walk */
	unsigned depth;                   /* nesting of exported entry points on this thread */
	uint64_t cur[GAPT_SITE_DEPTH];    /* the outermost entry point's caller chain while depth > 0 */
	uint64_t psite;                   /* site[0] of the call that returned last (0 = none) */
	struct gapt_site *tab;            /* GAPT_TAB_N entries, NULL until the first booked gap */
	bool tab_failed;
	uint64_t lost_n, lost_ns, nosite_n;    /* table full / gaps whose next call had no entry point scope; per window */
	uint64_t mk[GM_MARKS + 1];        /* Mesa time marks 1..3 (index 0 unused): clock reading, 0 = not seen since the previous kernel call returned */
	uint64_t gm_n[GM_N], gm_ns[GM_N]; /* submit -> wait gaps split by segment (GM_NOMARK = unsplit); per window */
	uint64_t gm_miss[GM_MARKS + 1], gm_order, gm_badid;
	uint64_t gm_order_pair[GM_MARKS + 2];   /* gm_order split by the first violating boundary pair (index 1..GM_MARKS+1; sums to gm_order) */
	struct gapw_slot *slot;           /* GAPW: this thread's sampler slot (heap, never freed), NULL = not sampled */
};
static __thread struct gapt_tls gt;
static int cfg_gapt, cfg_gapw;
static void gapw_slot_attach(struct gapt_tls *g);
static void gapw_cfg_init(void);
#ifdef __APPLE__
static unsigned gapt_numer = 1, gapt_denom = 1;
#endif
#ifdef SWEET_DRM_HOST_TEST
uint64_t (*sweet_gapt_clock)(void);   /* host test: fake clock, ns */
int sweet_gapt_tb_fail;               /* host test: mach_timebase_info "fails" */
#endif

static uint64_t
gapt_now(void)
{
#ifdef SWEET_DRM_HOST_TEST
	if (sweet_gapt_clock) {
		return sweet_gapt_clock();
	}
#endif
#ifdef __APPLE__
	uint64_t t = mach_absolute_time();
	return (t / gapt_denom) * gapt_numer + (t % gapt_denom) * gapt_numer / gapt_denom;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* Lock held, once (cfg_init). */
static void
gapt_cfg_init_inner(void)
{
	const char *e = getenv("SWEETGL_GAPT");
	int v = cfg_parse(e, 0);
	if (v < 0) {
		fprintf(stderr, "SWEETGL: GAPT unavailable: SWEETGL_GAPT=\"%.16s\" is not 0 or 1, gap timing OFF\n", e);
		return;
	}
	if (v == 1) {
		bool ok = true;
#ifdef __APPLE__
		mach_timebase_info_data_t tb;
		ok = mach_timebase_info(&tb) == 0 && tb.denom != 0;
		if (ok) {
			gapt_numer = tb.numer;
			gapt_denom = tb.denom;
		}
#endif
#ifdef SWEET_DRM_HOST_TEST
		if (sweet_gapt_tb_fail) {
			ok = false;
		}
#endif
		if (!ok) {
			fprintf(stderr, "SWEETGL: GAPT unavailable: mach_timebase_info failed, gap timing OFF\n");
			return;
		}
		cfg_gapt = 1;
		fprintf(stderr, "SWEETGL: GAPT on pid %d\n", (int)getpid());
		unsigned long long base = 0;
#ifdef __APPLE__
		Dl_info di;
		if (dladdr((const void *)&gapt_cfg_init, &di) && di.dli_fbase) {
			base = (unsigned long long)(uintptr_t)di.dli_fbase;
		} else {
			fprintf(stderr, "SWEETGL: GAPS unavailable: dladdr found no image base, the site addresses cannot be turned into offsets\n");
		}
#endif
		fprintf(stderr, "SWEETGL: GAPS on pid %d base 0x%llx depth %d\n", (int)getpid(), base, GAPT_SITE_DEPTH);
	}
}

static void
gapt_cfg_init(void)
{
	gapt_cfg_init_inner();
	if (cfg_gapt) {
		gapw_cfg_init();
	} else {
		const char *w = getenv("SWEETGL_GAPW");
		if (cfg_parse(w, 0) != 0) {
			fprintf(stderr, "SWEETGL: GAPW unavailable: SWEETGL_GAPW=\"%.16s\" needs SWEETGL_GAPT=1 (gap timing is off), window sampler OFF\n", w);
		}
	}
}

/* ---- LAGPH: where the lag between the end of one frame and its first Mali submit goes (T-9577, log-only, env SWEETGL_LAGPH=1, default off) ----
 * sweet_lagph_flush_exit() is called by the glFlush/glFinish timing hook (gli_table.c, needs SWEETGL_FLUSHT=1) right after the one flush of a frame returns. The NEXT
 * submit() on the same thread prints ONE line: wall time and thread CPU time (CLOCK_THREAD_CPUTIME_ID) between the two points, and the CPU number the thread was on at
 * each (TPIDR_EL0 & 0xfff; [read from source] xnu libsyscall/os/tsd.h _os_cpu_number, osfmk/arm64/cswitch.s writes CPU_NUMBER_GS there; map number -> core class with the
 * `SWEET CPU: cpu=N midr=` rows). wall - cpu = time the thread was NOT running (blocked or preempted). A submit on a thread with no pending flush exit is counted
 * (`unarmed`), never dropped silently; a failed CPU clock is counted (`cpufail`). The clock is mach_absolute_time in ns, the kernel's uptime ns, so a row joins the
 * `SWEET SWAPT: ... now` of the same frame: now - fe_ns = flush exit -> swap, sub_ns - now = swap -> first submit. At most LAGPH_MAX rows, then one loud cap line. */
#define LAGPH_MAX 40000u
struct lagph_tls { uint64_t fe_ns, fe_cpu; unsigned fe_cpun; int armed; };
static __thread struct lagph_tls lp_tls;
static int cfg_lagph = -1;                           /* -1 not read, 0 off, 1 on */
static unsigned lp_rows, lp_unarmed, lp_cpufail, lp_cap_said;
static uint64_t lp_unarmed_last;
#ifdef __APPLE__
static unsigned lp_numer = 1, lp_denom = 1;
#endif
#ifdef SWEET_DRM_HOST_TEST
uint64_t (*sweet_lagph_clock)(void);                 /* host test: fake wall clock, ns */
uint64_t (*sweet_lagph_cpuclock)(void);              /* host test: fake thread CPU clock, ns (0 = failed) */
unsigned (*sweet_lagph_cpunum)(void);                /* host test: fake cpu number */
void sweet_lagph_reset(void) { cfg_lagph = -1; lp_rows = lp_unarmed = lp_cpufail = lp_cap_said = 0; lp_unarmed_last = 0; memset(&lp_tls, 0, sizeof lp_tls); }
#endif

static uint64_t
lagph_wall(void)
{
#ifdef SWEET_DRM_HOST_TEST
	if (sweet_lagph_clock) {
		return sweet_lagph_clock();
	}
#endif
#ifdef __APPLE__
	uint64_t t = mach_absolute_time();
	return (t / lp_denom) * lp_numer + (t % lp_denom) * lp_numer / lp_denom;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* thread CPU time in ns; 0 = the clock failed (counted by the caller) */
static uint64_t
lagph_cpu(void)
{
#ifdef SWEET_DRM_HOST_TEST
	if (sweet_lagph_cpuclock) {
		return sweet_lagph_cpuclock();
	}
#endif
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned
lagph_cpunum(void)
{
#ifdef SWEET_DRM_HOST_TEST
	if (sweet_lagph_cpunum) {
		return sweet_lagph_cpunum();
	}
#endif
#if defined(__APPLE__) && defined(__aarch64__)
	uint64_t v;
	__asm__ __volatile__("mrs %0, TPIDR_EL0" : "=r"(v));
	return (unsigned)(v & 0xfff);
#else
	return 0xffffu;   /* not available on this build: the row says so */
#endif
}

static int
lagph_on(void)
{
	if (cfg_lagph >= 0) {
		return cfg_lagph;
	}
	const char *e = getenv("SWEETGL_LAGPH");
	int v = cfg_parse(e, 0);
	if (v < 0) {
		fprintf(stderr, "SWEETGL: LAGPH unavailable: SWEETGL_LAGPH=\"%.16s\" is not 0 or 1, lag phases OFF\n", e);
		v = 0;
	}
#ifdef __APPLE__
	if (v == 1) {
		mach_timebase_info_data_t tb;
		if (mach_timebase_info(&tb) != 0 || tb.denom == 0) {
			fprintf(stderr, "SWEETGL: LAGPH unavailable: mach_timebase_info failed, lag phases OFF\n");
			v = 0;
		} else {
			lp_numer = tb.numer;
			lp_denom = tb.denom;
		}
	}
#endif
	if (v == 1) {
		fprintf(stderr, "SWEETGL: LAGPH on pid %d cpunum %s\n", (int)getpid(),
		        lagph_cpunum() == 0xffffu ? "unavailable" : "TPIDR_EL0&0xfff");
	}
	cfg_lagph = v;
	return v;
}

void
sweet_lagph_flush_exit(void)
{
	if (!lagph_on()) {
		return;
	}
	lp_tls.fe_ns = lagph_wall();
	lp_tls.fe_cpu = lagph_cpu();
	lp_tls.fe_cpun = lagph_cpunum();
	lp_tls.armed = 1;
	if (lp_tls.fe_cpu == 0) {
		__atomic_fetch_add(&lp_cpufail, 1, __ATOMIC_RELAXED);
	}
}

/* Top of submit(), lock held. Every 256th unarmed submit and the cap are reported on the spot, so a missing flush hook shows as a line, not as silence. */
static void
lagph_submit(void)
{
	if (!lagph_on()) {
		return;
	}
	if (!lp_tls.armed) {
		lp_unarmed++;
		if ((lp_unarmed & 255u) == 1u) {
			fprintf(stderr, "SWEETGL: LAGPH unarmed submits %u rows %u cpufail %u (a submit with no flush exit before it on this thread)\n", lp_unarmed, lp_rows, lp_cpufail);
		}
		return;
	}
	lp_tls.armed = 0;
	if (lp_rows >= LAGPH_MAX) {
		if (!lp_cap_said) {
			lp_cap_said = 1;
			fprintf(stderr, "SWEETGL: LAGPH cap reached %u rows (unarmed %u cpufail %u)\n", LAGPH_MAX, lp_unarmed, lp_cpufail);
		}
		return;
	}
	uint64_t ns = lagph_wall(), cpu = lagph_cpu();
	unsigned cn = lagph_cpunum();
	if (cpu == 0) {
		lp_cpufail++;
	}
	uint64_t wall_us = ns > lp_tls.fe_ns ? (ns - lp_tls.fe_ns) / 1000 : 0;
	uint64_t cpu_us = (cpu && lp_tls.fe_cpu && cpu >= lp_tls.fe_cpu) ? (cpu - lp_tls.fe_cpu) / 1000 : 0;
	lp_rows++;
	fprintf(stderr, "SWEETGL: LAGPH n %u tid %llu fe_ns %llu sub_ns %llu wall_us %llu cpu_us %llu cpu_a %u cpu_b %u\n", lp_rows,
	        (unsigned long long)(uintptr_t)pthread_self(), (unsigned long long)lp_tls.fe_ns, (unsigned long long)ns,
	        (unsigned long long)wall_us, (unsigned long long)cpu_us, lp_tls.fe_cpun, cn);
}

/* Frame-pointer walk (arm64 Darwin keeps x29 chains; the host test builds with -fno-omit-frame-pointer). fp = a frame record [saved fp, return address]; out[] gets
 * the return addresses, caller first. Stops at an unaligned or out-of-stack record, at a saved fp that does not grow the stack upwards, or after `max`. */
static unsigned
gapt_walk(uintptr_t fp, uintptr_t lo, uintptr_t hi, uint64_t *out, unsigned max)
{
	unsigned n = 0;
	while (n < max && lo && (fp & 15) == 0 && fp >= lo && fp + 16 <= hi) {
		const uintptr_t *f = (const uintptr_t *)fp;
		out[n++] = f[1];
		if (f[0] <= fp) {
			break;
		}
		fp = f[0];
	}
	return n;
}

#ifdef SWEET_DRM_HOST_TEST
unsigned sweet_gapt_walk(uintptr_t fp, uintptr_t lo, uintptr_t hi, uint64_t *out, unsigned max)   /* host test: synthetic frame chains */
{
	return gapt_walk(fp, lo, hi, out, max);
}
#endif

/* First use on this thread: tid and stack bounds. t = window start stamp, or 0 when called from an entry scope (no clock read there: the first gapt_before sets it). */
static void
gapt_tls_init(struct gapt_tls *g, uint64_t t)
{
	g->init = true;
	g->win0 = t;
#ifdef __APPLE__
	uint64_t id = 0;
	pthread_threadid_np(NULL, &id);
	g->tid = id;
	pthread_t me = pthread_self();
	g->stack_hi = (uintptr_t)pthread_get_stackaddr_np(me);
	g->stack_lo = g->stack_hi - (uintptr_t)pthread_get_stacksize_np(me);
#else
	g->tid = (uint64_t)(uintptr_t)pthread_self();
	pthread_attr_t at;
	void *addr = NULL;
	size_t sz = 0;
	if (pthread_getattr_np(pthread_self(), &at) == 0) {
		if (pthread_attr_getstack(&at, &addr, &sz) == 0) {
			g->stack_lo = (uintptr_t)addr;
			g->stack_hi = (uintptr_t)addr + sz;
		}
		pthread_attr_destroy(&at);
	}
#endif
	if (!g->stack_lo || g->stack_hi <= g->stack_lo) {
		g->stack_lo = g->stack_hi = 0;
		fprintf(stderr, "SWEETGL: GAPS unavailable: tid 0x%llx has no stack bounds, only site[0] is recorded\n", (unsigned long long)g->tid);
	}
	gapw_slot_attach(g);
}

/* Scope of an exported entry point: the outermost one records its caller chain. ra = __builtin_return_address(0), fp = __builtin_frame_address(0) of the entry point. */
struct gapt_scope {
	bool on;
};
static inline __attribute__((always_inline)) struct gapt_scope
gapt_scope_enter(void *ra, void *fp)
{
	struct gapt_scope s = { false };
	if (!cfg_gapt) {
		return s;
	}
	struct gapt_tls *g = &gt;
	if (!g->init) {
		gapt_tls_init(g, 0);
	}
	s.on = true;
	if (g->depth++ == 0) {
		memset(g->cur, 0, sizeof g->cur);
		unsigned n = gapt_walk((uintptr_t)fp, g->stack_lo, g->stack_hi, g->cur, GAPT_SITE_DEPTH);
		if (n == 0) {
			g->cur[0] = (uint64_t)(uintptr_t)ra;    /* no walk possible: the return address alone */
		}
	}
	return s;
}
static inline void
gapt_scope_leave(struct gapt_scope *s)
{
	if (s->on) {
		gt.depth--;
	}
}
#define GAPT_SCOPE() struct gapt_scope gapt_scope_v __attribute__((cleanup(gapt_scope_leave), unused)) = gapt_scope_enter(__builtin_return_address(0), __builtin_frame_address(0))

static unsigned
gapt_site_hash(uint8_t prev, uint8_t next, uint64_t psite, const uint64_t *site)
{
	uint64_t h = 1469598103934665603ull;
	uint64_t v[2 + 1 + GAPT_SITE_DEPTH] = { prev, next, psite };
	for (int i = 0; i < GAPT_SITE_DEPTH; i++) {
		v[3 + i] = site[i];
	}
	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
		h = (h ^ v[i]) * 1099511628211ull;
		h ^= h >> 29;
	}
	return (unsigned)(h & (GAPT_TAB_N - 1));
}

/* Book one gap under its call sites. Not on the measured path of the call being timed: it runs after the clock read of gapt_before. */
static void
gapt_site_book(struct gapt_tls *g, uint8_t prev, uint8_t next, uint64_t d)
{
	const uint64_t zero[GAPT_SITE_DEPTH] = { 0 };
	const uint64_t *site = g->depth ? g->cur : zero;
	if (!site[0]) {
		g->nosite_n++;
	}
	if (!g->tab && !g->tab_failed) {
		g->tab = calloc(GAPT_TAB_N, sizeof *g->tab);
		if (!g->tab) {
			g->tab_failed = true;
			fprintf(stderr, "SWEETGL: GAPS unavailable: tid 0x%llx cannot allocate the site table, its gaps are counted as lost\n", (unsigned long long)g->tid);
		}
	}
	if (!g->tab) {
		g->lost_n++;
		g->lost_ns += d;
		return;
	}
	unsigned h = gapt_site_hash(prev, next, g->psite, site);
	for (unsigned i = 0; i < GAPT_TAB_N; i++) {
		struct gapt_site *e = &g->tab[(h + i) & (GAPT_TAB_N - 1)];
		if (!e->used) {
			e->used = true;
			e->prev = prev;
			e->next = next;
			e->psite = g->psite;
			memcpy(e->site, site, sizeof e->site);
		} else if (e->prev != prev || e->next != next || e->psite != g->psite || memcmp(e->site, site, sizeof e->site)) {
			continue;
		}
		e->n++;
		e->ns += d;
		if (d < GAPT_SHORT_NS) {
			e->sns += d;
		}
		return;
	}
	g->lost_n++;
	g->lost_ns += d;
}

static int
gapt_site_cmp(const void *a, const void *b)
{
	const struct gapt_site *x = *(const struct gapt_site *const *)a, *y = *(const struct gapt_site *const *)b;
	return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

/* Print this thread's top sites and the rest, clear the table. Called from gapt_flush when the window has >= GAPT_DETAIL_MIN_CALLS calls. */
static void
gapt_site_flush(struct gapt_tls *g, bool detail)
{
	if (g->tab) {
		struct gapt_site *rows[GAPT_TAB_N];
		unsigned nr = 0;
		for (unsigned i = 0; i < GAPT_TAB_N; i++) {
			if (g->tab[i].used) {
				rows[nr++] = &g->tab[i];
			}
		}
		qsort(rows, nr, sizeof rows[0], gapt_site_cmp);
		uint64_t rest_n = 0, rest_ns = 0;
		unsigned rest_rows = 0;
		for (unsigned i = 0; i < nr; i++) {
			const struct gapt_site *e = rows[i];
			if (detail && i < GAPT_SITE_TOP) {
				fprintf(stderr, "SWEETGL: GAPS pid %d tid 0x%llx sec %u prev %s next %s psite 0x%llx site 0x%llx,0x%llx,0x%llx,0x%llx n %llu gap_us %llu lt1ms_us %llu\n",
				    (int)getpid(), (unsigned long long)g->tid, g->win_id, gk_name[e->prev], gk_name[e->next], (unsigned long long)e->psite,
				    (unsigned long long)e->site[0], (unsigned long long)e->site[1], (unsigned long long)e->site[2], (unsigned long long)e->site[3],
				    (unsigned long long)e->n, (unsigned long long)(e->ns / 1000), (unsigned long long)(e->sns / 1000));
			} else {
				rest_rows++;
				rest_n += e->n;
				rest_ns += e->ns;
			}
		}
		if (detail) {
			fprintf(stderr, "SWEETGL: GAPSREST pid %d tid 0x%llx sec %u sites %u n %llu gap_us %llu lost_n %llu lost_us %llu nosite_n %llu\n",
			    (int)getpid(), (unsigned long long)g->tid, g->win_id, rest_rows, (unsigned long long)rest_n, (unsigned long long)(rest_ns / 1000),
			    (unsigned long long)g->lost_n, (unsigned long long)(g->lost_ns / 1000), (unsigned long long)g->nosite_n);
		}
		memset(g->tab, 0, GAPT_TAB_N * sizeof *g->tab);
	} else if (detail) {
		fprintf(stderr, "SWEETGL: GAPSREST pid %d tid 0x%llx sec %u sites 0 n 0 gap_us 0 lost_n %llu lost_us %llu nosite_n %llu\n",
		    (int)getpid(), (unsigned long long)g->tid, g->win_id, (unsigned long long)g->lost_n, (unsigned long long)(g->lost_ns / 1000),
		    (unsigned long long)g->nosite_n);
	}
	g->lost_n = g->lost_ns = g->nosite_n = 0;
}

/* ---- GAPW: window pc sampler (P-G1 slice 12d, T-baf5; docs/research/129-gapw-window-sampler-design.md) ----
 * One slot per sampled thread: heap, allocated by the thread itself BEFORE it is published, never freed (a stale thread-local pointer can therefore never be read by the sampler).
 * Producer = the sampler pthread (ring head, counters), consumer = the thread's own gapt_flush (ring tail, counters read with exchange). */
#define GAPW_RING        256u               /* power of two */
#define GAPW_MAX_SLOTS   16u
#ifndef GAPW_TOP
#define GAPW_TOP         GAPW_RING          /* v2: print every group (the ring holds at most GAPW_RING samples per flush); the host test builds a small bound to reach GAPWREST */
#endif
#define GAPW_FR          8
#define GAPW_NX          4096u              /* power of two: closing-kind table, direct mapped by gap serial */
#define GAPW_PERIOD_US   2500u              /* 400 Hz, +-25 % jitter so the samples do not lock to the 60 Hz frame */
#define GAPW_DEAD_ERRS   3u                 /* consecutive suspend/get_state errors after which a slot is disabled (thread gone or port reused) */

struct gapw_sample {
	uint64_t pc, lr, fr[GAPW_FR], dt;       /* dt = ns since mark 1 */
	uint32_t gap;                           /* serial of the gap (kernel entry to kernel entry) the sample was taken in */
};
struct gapw_nx {                            /* owner-only: the gap `gap` was opened by kind prev and closed by kind next (written only for gaps that hold a sample) */
	uint32_t gap;
	uint8_t prev, next;
};
struct gapw_slot {
	uint64_t tid;
	uintptr_t stack_lo, stack_hi;
	uint32_t port;                          /* mach thread port (0 on the host build) */
	uint32_t win;                           /* 1 between mark 1 and mark 2 / the next kernel entry */
	uint32_t dead, errrun, dead_said;
	uint64_t mk1;                           /* clock at the last mark 1 */
	uint32_t head, tail;                    /* monotonic; head written by the sampler, tail by the owner */
	uint32_t gap;                           /* serial of the open gap, from 1; written by the owner at every kernel entry, read by the sampler while the owner is stopped */
	uint32_t sampled;                       /* serial of the gap of the newest sample (0 = none); written by the sampler before the resume, read by the owner */
	uint64_t ticks, kept, drop, full, notwin, badsp, err, rerr, stop_ns;
	uint64_t notag;                         /* owner only */
	struct gapw_sample ring[GAPW_RING];
	struct gapw_nx nx[GAPW_NX];
};
struct gapw_regs {
	uint64_t pc, lr, fp, sp;
};
struct gapw_ops {
	int (*suspend)(struct gapw_slot *);
	int (*getstate)(struct gapw_slot *, struct gapw_regs *);
	int (*resume)(struct gapw_slot *);
};
#ifdef SWEET_DRM_HOST_TEST
int sweet_gapw_nobounds;   /* host test: the next thread to attach reports unknown stack bounds */
#endif
static struct gapw_slot *gapw_slots[GAPW_MAX_SLOTS];
static uint32_t gapw_nslots, gapw_noslot;

#define GAPW_LOAD(p)     __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define GAPW_ADD(p, v)   ((void)__atomic_fetch_add(&(p), (v), __ATOMIC_RELAXED))

#if defined(__APPLE__) && defined(__arm64__)
static int
gapw_suspend(struct gapw_slot *s)
{
	return thread_suspend((thread_act_t)s->port) == KERN_SUCCESS ? 0 : -1;
}
static int
gapw_getstate(struct gapw_slot *s, struct gapw_regs *r)
{
	arm_thread_state64_t ts;
	mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
	if (thread_get_state((thread_act_t)s->port, ARM_THREAD_STATE64, (thread_state_t)&ts, &n) != KERN_SUCCESS) {
		return -1;
	}
	r->pc = (uint64_t)arm_thread_state64_get_pc(ts);
	r->lr = (uint64_t)arm_thread_state64_get_lr(ts);
	r->fp = (uint64_t)arm_thread_state64_get_fp(ts);
	r->sp = (uint64_t)arm_thread_state64_get_sp(ts);
	return 0;
}
static int
gapw_resume(struct gapw_slot *s)
{
	return thread_resume((thread_act_t)s->port) == KERN_SUCCESS ? 0 : -1;
}
static const struct gapw_ops gapw_ops_real = { gapw_suspend, gapw_getstate, gapw_resume };
static const struct gapw_ops *
gapw_ops(void)
{
	return &gapw_ops_real;
}
#elif defined(SWEET_DRM_HOST_TEST)
/* host test: the test plays the kernel (suspend/get_state/resume of a fake thread) and calls sweet_gapw_tick() itself */
int (*sweet_gapw_suspend)(uint64_t tid);
int (*sweet_gapw_getstate)(uint64_t tid, uint64_t r[4]);     /* r = pc, lr, fp, sp */
int (*sweet_gapw_resume)(uint64_t tid);
static int gapw_h_suspend(struct gapw_slot *s) { return sweet_gapw_suspend ? sweet_gapw_suspend(s->tid) : -1; }
static int gapw_h_getstate(struct gapw_slot *s, struct gapw_regs *r)
{
	uint64_t v[4] = { 0 };
	if (!sweet_gapw_getstate || sweet_gapw_getstate(s->tid, v) != 0) {
		return -1;
	}
	r->pc = v[0]; r->lr = v[1]; r->fp = v[2]; r->sp = v[3];
	return 0;
}
static int gapw_h_resume(struct gapw_slot *s) { return sweet_gapw_resume ? sweet_gapw_resume(s->tid) : -1; }
static const struct gapw_ops gapw_ops_host = { gapw_h_suspend, gapw_h_getstate, gapw_h_resume };
static const struct gapw_ops *
gapw_ops(void)
{
	return &gapw_ops_host;
}
#else
static const struct gapw_ops *
gapw_ops(void)
{
	return NULL;
}
#endif

/* Open (open = 1, t = the mark-1 clock) or close the window of this thread's slot. Called by the owner thread only. */
static void
gapw_window(struct gapw_slot *s, int open, uint64_t t)
{
	if (open) {
		__atomic_store_n(&s->mk1, t ? t : 1, __ATOMIC_RELAXED);
	}
	__atomic_store_n(&s->win, open ? 1u : 0u, __ATOMIC_RELEASE);
}

/* Owner thread, at every kernel entry right after the window was closed (so no sampler can still be inside this gap): books the closing kind of the gap that ends now,
 * but only when the gap holds a sample (`sampled` is written by the sampler under the stop, before the resume, so it is visible here), then opens the next gap. */
static void
gapw_gap_close(struct gapw_slot *s, uint8_t prev, uint8_t next)
{
	uint32_t gap = s->gap;
	if (__atomic_load_n(&s->sampled, __ATOMIC_RELAXED) == gap) {
		struct gapw_nx *e = &s->nx[gap & (GAPW_NX - 1)];
		e->gap = gap;
		e->prev = prev;
		e->next = next;
	}
	__atomic_store_n(&s->gap, gap + 1, __ATOMIC_RELAXED);
}

/* Called once by the owner thread from gapt_tls_init (stack bounds are known). */
static void
gapw_slot_attach(struct gapt_tls *g)
{
	if (!cfg_gapw) {
		return;
	}
#ifdef SWEET_DRM_HOST_TEST
	if (sweet_gapw_nobounds) {
		g->stack_lo = 0;
	}
#endif
	if (!g->stack_lo) {
		fprintf(stderr, "SWEETGL: GAPW unavailable: tid 0x%llx has no stack bounds, the thread is not sampled\n", (unsigned long long)g->tid);
		return;
	}
	struct gapw_slot *s = calloc(1, sizeof *s);
	if (!s) {
		fprintf(stderr, "SWEETGL: GAPW unavailable: no memory for the slot of tid 0x%llx, the thread is not sampled\n", (unsigned long long)g->tid);
		return;
	}
	s->tid = g->tid;
	s->gap = 1;
	s->stack_lo = g->stack_lo;
	s->stack_hi = g->stack_hi;
#ifdef __APPLE__
	s->port = (uint32_t)pthread_mach_thread_np(pthread_self());
#endif
	uint32_t i = __atomic_fetch_add(&gapw_nslots, 1, __ATOMIC_RELAXED);
	if (i >= GAPW_MAX_SLOTS) {
		if (!__atomic_fetch_add(&gapw_noslot, 1, __ATOMIC_RELAXED)) {
			fprintf(stderr, "SWEETGL: GAPW unavailable: more than %u threads, tid 0x%llx and later threads are not sampled (counted as noslot)\n", GAPW_MAX_SLOTS,
			    (unsigned long long)g->tid);
		}
		free(s);
		return;
	}
	g->slot = s;
	__atomic_store_n(&gapw_slots[i], s, __ATOMIC_RELEASE);
}

static void
gapw_err(struct gapw_slot *s)
{
	GAPW_ADD(s->err, 1);
	if (++s->errrun >= GAPW_DEAD_ERRS) {
		__atomic_store_n(&s->dead, 1u, __ATOMIC_RELAXED);
	}
}

/* One sample of one slot. Between suspend and resume: two kernel calls, register reads, a bounded stack read (gapt_walk checks the stack bounds), stores into preallocated memory.
 * No malloc, no stdio, no lock, no dladdr (design note 129, hazard table). */
static void
gapw_sample_slot(struct gapw_slot *s, const struct gapw_ops *ops)
{
	if (GAPW_LOAD(s->dead) || !__atomic_load_n(&s->win, __ATOMIC_ACQUIRE)) {
		return;
	}
	GAPW_ADD(s->ticks, 1);
	uint32_t h = s->head;
	if (h - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE) >= GAPW_RING) {
		GAPW_ADD(s->full, 1);   /* v2: a full ring costs no stop (v1 stopped the thread, then dropped the sample); only this thread fills the ring, so the room is still there after the stop */
		return;
	}
	uint64_t t0 = gapt_now();
	if (ops->suspend(s) != 0) {
		gapw_err(s);
		return;
	}
	struct gapw_regs r = { 0, 0, 0, 0 };
	struct gapw_sample smp;
	memset(&smp, 0, sizeof smp);
	int why = 0;          /* 0 = sample, 1 = window ended, 2 = get_state failed, 3 = sp outside the stack */
	if (!__atomic_load_n(&s->win, __ATOMIC_ACQUIRE)) {
		why = 1;
	} else if (ops->getstate(s, &r) != 0) {
		why = 2;
	} else if (r.sp < s->stack_lo || r.sp >= s->stack_hi) {
		why = 3;
	} else {
		smp.pc = r.pc;
		smp.lr = r.lr;
		gapt_walk((uintptr_t)r.fp, s->stack_lo, s->stack_hi, smp.fr, GAPW_FR);
		uint64_t m1 = __atomic_load_n(&s->mk1, __ATOMIC_RELAXED);
		smp.dt = m1 && t0 >= m1 ? t0 - m1 : 0;
		smp.gap = __atomic_load_n(&s->gap, __ATOMIC_RELAXED);   /* the owner is stopped: its gap serial cannot move */
		__atomic_store_n(&s->sampled, smp.gap, __ATOMIC_RELAXED);
	}
	int rr = ops->resume(s);
	GAPW_ADD(s->stop_ns, gapt_now() - t0);
	if (rr != 0) {
		GAPW_ADD(s->rerr, 1);
	}
	if (why == 2) {
		gapw_err(s);
		return;
	}
	s->errrun = 0;
	if (why == 1) {
		GAPW_ADD(s->notwin, 1);
	} else if (why == 3) {
		GAPW_ADD(s->badsp, 1);
	} else {
		h = s->head;   /* re-read: with one sampler thread this is the value from before the stop; the check stays so a second producer (or a bug) is a counted drop, never an overwrite */
		if (h - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE) >= GAPW_RING) {
			GAPW_ADD(s->drop, 1);
		} else {
			s->ring[h & (GAPW_RING - 1)] = smp;
			__atomic_store_n(&s->head, h + 1, __ATOMIC_RELEASE);
			GAPW_ADD(s->kept, 1);
		}
	}
}

static void
gapw_tick(const struct gapw_ops *ops)
{
	uint32_t n = __atomic_load_n(&gapw_nslots, __ATOMIC_ACQUIRE);
	if (n > GAPW_MAX_SLOTS) {
		n = GAPW_MAX_SLOTS;
	}
	for (uint32_t i = 0; i < n; i++) {
		struct gapw_slot *s = __atomic_load_n(&gapw_slots[i], __ATOMIC_ACQUIRE);
		if (s) {
			gapw_sample_slot(s, ops);
		}
	}
}

#ifdef SWEET_DRM_HOST_TEST
void sweet_gapw_tick(void)   /* host test: one sampler pass over every slot */
{
	gapw_tick(gapw_ops());
}
#endif

#if defined(__APPLE__) && defined(__arm64__)
static void *
gapw_main(void *arg)
{
	(void)arg;
	/* warm-up outside any suspension: the first MIG call of this thread may allocate its reply port */
	mach_port_t me = mach_thread_self();
	arm_thread_state64_t ts;
	mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
	(void)thread_get_state(me, ARM_THREAD_STATE64, (thread_state_t)&ts, &n);
	mach_port_deallocate(mach_task_self(), me);
	uint32_t rng = 0x9e3779b9u ^ (uint32_t)getpid();
	for (;;) {
		gapw_tick(gapw_ops());
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		usleep(GAPW_PERIOD_US * 3 / 4 + rng % (GAPW_PERIOD_US / 2));
	}
	return NULL;
}
#endif

/* Lock held, once, after "GAPS on". */
static void
gapw_cfg_init(void)
{
	const char *e = getenv("SWEETGL_GAPW");
	int v = cfg_parse(e, 0);
	if (v < 0) {
		fprintf(stderr, "SWEETGL: GAPW unavailable: SWEETGL_GAPW=\"%.16s\" is not 0 or 1, window sampler OFF\n", e);
		return;
	}
	if (v != 1) {
		return;
	}
	if (!gapw_ops()) {
		fprintf(stderr, "SWEETGL: GAPW unavailable: no sampler on this platform (needs Darwin arm64), window sampler OFF\n");
		return;
	}
#if defined(__APPLE__) && defined(__arm64__)
	pthread_t th;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 64 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&th, &at, gapw_main, NULL);
	pthread_attr_destroy(&at);
	if (rc != 0) {
		fprintf(stderr, "SWEETGL: GAPW unavailable: pthread_create failed rc %d, window sampler OFF\n", rc);
		return;
	}
#endif
	cfg_gapw = 1;
	fprintf(stderr, "SWEETGL: GAPW on pid %d period_us %u ring %u frames %d top %u v2\n", (int)getpid(), GAPW_PERIOD_US, GAPW_RING, GAPW_FR, (unsigned)GAPW_TOP);
}

struct gapw_grp {
	struct gapw_sample k;
	uint8_t prev, next;                     /* GK_N = unknown */
	uint64_t n, dt;
};
static const char *
gapw_kname(uint8_t k)
{
	return k < GK_N ? gk_name[k] : "?";
}
/* group key: (prev, next, pc, lr, frames); the gap serial and dt are not part of it */
static int
gapw_cmp_key(const void *a, const void *b)
{
	const struct gapw_grp *x = a, *y = b;
	uint64_t xv[2 + 2 + GAPW_FR] = { x->prev, x->next, x->k.pc, x->k.lr };
	uint64_t yv[2 + 2 + GAPW_FR] = { y->prev, y->next, y->k.pc, y->k.lr };
	for (int i = 0; i < GAPW_FR; i++) {
		xv[4 + i] = x->k.fr[i];
		yv[4 + i] = y->k.fr[i];
	}
	for (int i = 0; i < 2 + 2 + GAPW_FR; i++) {
		if (xv[i] != yv[i]) {
			return xv[i] < yv[i] ? -1 : 1;
		}
	}
	return 0;
}
static int
gapw_cmp_n(const void *a, const void *b)
{
	const struct gapw_grp *x = a, *y = b;
	if (x->n != y->n) {
		return x->n > y->n ? -1 : 1;
	}
	return x->k.pc < y->k.pc ? -1 : x->k.pc > y->k.pc;
}

static void
gapw_where(uint64_t pc, unsigned long long *off, const char **img)
{
	*off = 0;
	*img = "-";
#ifdef __APPLE__
	Dl_info di;
	if (dladdr((const void *)(uintptr_t)pc, &di) && di.dli_fbase && di.dli_fname) {
		const char *b = strrchr(di.dli_fname, '/');
		*off = pc - (uint64_t)(uintptr_t)di.dli_fbase;
		*img = b ? b + 1 : di.dli_fname;
	}
#else
	(void)pc;
#endif
}

/* The owner thread's flush: drain the ring, tag, aggregate, print, take the counters. Nothing is printed while any thread is stopped. */
static void
gapw_flush(struct gapt_tls *g, int pid, unsigned long long tid)
{
	struct gapw_slot *s = g->slot;
	if (!s) {
		return;
	}
	uint32_t tail = s->tail;
	uint32_t head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
	uint32_t n = head - tail;
	struct gapw_grp grp[GAPW_RING];     /* one array: the ring entries become single-sample groups and are merged in place */
	if (n > GAPW_RING) {
		n = GAPW_RING;   /* cannot happen: the producer never writes past tail + RING */
	}
	uint64_t notag = 0;
	for (uint32_t i = 0; i < n; i++) {
		struct gapw_grp *q = &grp[i];
		q->k = s->ring[(tail + i) & (GAPW_RING - 1)];
		const struct gapw_nx *e = &s->nx[q->k.gap & (GAPW_NX - 1)];
		if (e->gap == q->k.gap) {
			q->prev = e->prev;
			q->next = e->next;
		} else {
			q->prev = q->next = GK_N;   /* the closing call was not booked (table slot reused, or the gap is still open): loud, never guessed */
			notag++;
		}
		q->n = 1;
		q->dt = q->k.dt;
	}
	__atomic_store_n(&s->tail, tail + n, __ATOMIC_RELEASE);
	uint64_t ticks = __atomic_exchange_n(&s->ticks, 0, __ATOMIC_RELAXED), kept = __atomic_exchange_n(&s->kept, 0, __ATOMIC_RELAXED),
	    drop = __atomic_exchange_n(&s->drop, 0, __ATOMIC_RELAXED), full = __atomic_exchange_n(&s->full, 0, __ATOMIC_RELAXED),
	    notwin = __atomic_exchange_n(&s->notwin, 0, __ATOMIC_RELAXED), badsp = __atomic_exchange_n(&s->badsp, 0, __ATOMIC_RELAXED),
	    err = __atomic_exchange_n(&s->err, 0, __ATOMIC_RELAXED), rerr = __atomic_exchange_n(&s->rerr, 0, __ATOMIC_RELAXED),
	    stop = __atomic_exchange_n(&s->stop_ns, 0, __ATOMIC_RELAXED);
	uint32_t dead = GAPW_LOAD(s->dead);
	uint32_t noslot = GAPW_LOAD(gapw_noslot);
	if (!n && !ticks && !kept && !drop && !full && !notwin && !badsp && !err && !rerr && !dead && !noslot) {
		return;
	}
	if (dead && !s->dead_said) {
		s->dead_said = 1;
		fprintf(stderr, "SWEETGL: GAPW unavailable: tid 0x%llx disabled after %u consecutive suspend/get_state errors, the thread is no longer sampled\n", tid, GAPW_DEAD_ERRS);
	}
	if (n) {
		unsigned ng = 0;
		qsort(grp, n, sizeof grp[0], gapw_cmp_key);
		for (uint32_t i = 0; i < n; i++) {
			if (ng && gapw_cmp_key(&grp[ng - 1], &grp[i]) == 0) {
				grp[ng - 1].n++;
				grp[ng - 1].dt += grp[i].dt;
			} else {
				if (ng != i) {
					grp[ng] = grp[i];
				}
				ng++;
			}
		}
		qsort(grp, ng, sizeof grp[0], gapw_cmp_n);
		uint64_t rest_n = 0;
		unsigned rest_g = 0;
		for (unsigned i = 0; i < ng; i++) {
			if (i < GAPW_TOP) {
				unsigned long long off;
				const char *img;
				gapw_where(grp[i].k.pc, &off, &img);
				/* ONE fprintf per row: stderr is unbuffered, every stdio call is its own write(2) and the USB log turns every write into its own line
				 * (run 20261103T161002Z-gputest-bdc1-gapw2: a row printed as `fr`, then one line per frame, 12310 rows). The row is built in a buffer first. */
				char row[640];
				int o = snprintf(row, sizeof row, "SWEETGL: GAPW pid %d tid 0x%llx sec %u prev %s next %s n %llu dt_us %llu pc 0x%llx off 0x%llx img %s lr 0x%llx fr", pid, tid, g->win_id,
				    gapw_kname(grp[i].prev), gapw_kname(grp[i].next), (unsigned long long)grp[i].n, (unsigned long long)(grp[i].dt / grp[i].n / 1000),
				    (unsigned long long)grp[i].k.pc, off, img, (unsigned long long)grp[i].k.lr);
				for (int f = 0; f < GAPW_FR && o > 0 && (size_t)o < sizeof row; f++)
					o += snprintf(row + o, sizeof row - (size_t)o, "%s0x%llx", f ? "," : " ", (unsigned long long)grp[i].k.fr[f]);
				if (o < 0 || (size_t)o >= sizeof row)
					fprintf(stderr, "SWEETGL: GAPW GAP: row buffer too small (%d bytes), row dropped\n", o);
				else
					fprintf(stderr, "%s\n", row);
			} else {
				rest_g++;
				rest_n += grp[i].n;
			}
		}
		fprintf(stderr, "SWEETGL: GAPWREST pid %d tid 0x%llx sec %u groups %u n %llu\n", pid, tid, g->win_id, rest_g, (unsigned long long)rest_n);
	}
	fprintf(stderr, "SWEETGL: GAPWSUM pid %d tid 0x%llx sec %u ticks %llu kept %llu drop %llu full %llu notwin %llu badsp %llu err %llu rerr %llu notag %llu stop_us %llu noslot %u dead %u\n",
	    pid, tid, g->win_id, (unsigned long long)ticks, (unsigned long long)kept, (unsigned long long)drop, (unsigned long long)full, (unsigned long long)notwin,
	    (unsigned long long)badsp, (unsigned long long)err, (unsigned long long)rerr, (unsigned long long)notag, (unsigned long long)(stop / 1000), noslot, dead);
}

/* Mesa time mark (P-G1 slice 12b, T-6717): stores one clock reading in this thread's state. Called by the patched Mesa (0005) from the hot path; it reads the clock and
 * writes one word, nothing else: no lock, no syscall, no change to Mesa state. Off (SWEETGL_GAPT unset) it returns at once; a thread that has made no kernel call yet has no gap
 * to split and is ignored. A bad id is counted and printed once, never stored. */
void
sweet_gapt_mark(int id)
{
	if (!cfg_gapt) {
		return;
	}
	struct gapt_tls *g = &gt;
	if (id < 1 || id > GM_MARKS) {
		static int said;
		g->gm_badid++;
		if (!said) {
			said = 1;
			fprintf(stderr, "SWEETGL: GAPM unavailable: mark id %d (only 1..%d exist), the mark is ignored and counted as badid\n", id, GM_MARKS);
		}
		return;
	}
	if (!g->init) {
		return;
	}
	uint64_t t = gapt_now();
	g->mk[id] = t ? t : 1;
	if (g->slot && (id == 1 || id == 2)) {
		gapw_window(g->slot, id == 1, t);
	}
}

/* A gap just booked as submit -> wait: [t0, t1] = submit return .. wait entry, d = t1 - t0. All three marks present and ordered: four segments; else the whole gap is nomark. */
static void
gapt_mark_book(struct gapt_tls *g, uint64_t t0, uint64_t t1, uint64_t d)
{
	bool ok = true;
	for (int i = 1; i <= GM_MARKS; i++) {
		if (!g->mk[i]) {
			g->gm_miss[i]++;
			ok = false;
		}
	}
	if (ok) {
		uint64_t b[GM_MARKS + 2] = { t0, g->mk[1], g->mk[2], g->mk[3], t1 };
		for (int i = 1; i < GM_MARKS + 2; i++) {
			if (b[i] < b[i - 1]) {
				g->gm_order++;
				g->gm_order_pair[i]++;
				ok = false;
				break;
			}
		}
		if (ok) {
			for (int i = 0; i < GM_NOMARK; i++) {
				g->gm_n[i]++;
				g->gm_ns[i] += b[i + 1] - b[i];
			}
			return;
		}
	}
	g->gm_n[GM_NOMARK]++;
	g->gm_ns[GM_NOMARK] += d;
}

/* Print and clear this thread's window. Not on the measured path: the caller reads the clock again afterwards. */
static void
gapt_flush(struct gapt_tls *g, uint64_t now)
{
	uint64_t calls = 0, gap = 0, in = 0;
	for (int k = 0; k < GK_N; k++) {
		calls += g->in_n[k];
		in += g->in_ns[k];
		for (int j = 0; j < GK_N; j++) {
			gap += g->cell[k][j].ns;
		}
	}
	int pid = (int)getpid();
	unsigned long long tid = (unsigned long long)g->tid;
	if (calls >= GAPT_DETAIL_MIN_CALLS) {
		for (int k = 0; k < GK_N; k++) {
			for (int j = 0; j < GK_N; j++) {
				const struct gapt_cell *c = &g->cell[k][j];
				if (c->n) {
					fprintf(stderr, "SWEETGL: GAPT pid %d tid 0x%llx sec %u prev %s next %s n %llu gap_us %llu lt1ms_n %llu lt1ms_us %llu max_us %llu\n",
					    pid, tid, g->win_id, gk_name[k], gk_name[j], (unsigned long long)c->n, (unsigned long long)(c->ns / 1000),
					    (unsigned long long)c->sn, (unsigned long long)(c->sns / 1000), (unsigned long long)(c->max / 1000));
				}
			}
		}
		for (int k = 0; k < GK_N; k++) {
			if (g->in_n[k]) {
				fprintf(stderr, "SWEETGL: GAPTIN pid %d tid 0x%llx sec %u kind %s n %llu in_us %llu max_us %llu\n",
				    pid, tid, g->win_id, gk_name[k], (unsigned long long)g->in_n[k], (unsigned long long)(g->in_ns[k] / 1000),
				    (unsigned long long)(g->in_max[k] / 1000));
			}
		}
	}
	gapt_site_flush(g, calls >= GAPT_DETAIL_MIN_CALLS);
	uint64_t gm_total = 0;
	for (int k = 0; k < GM_N; k++) {
		gm_total += g->gm_n[k];
	}
	if (calls >= GAPT_DETAIL_MIN_CALLS && (gm_total || g->gm_badid)) {
		for (int k = 0; k < GM_NOMARK; k++) {
			fprintf(stderr, "SWEETGL: GAPM pid %d tid 0x%llx sec %u seg %s n %llu us %llu\n", pid, tid, g->win_id, gm_name[k],
			    (unsigned long long)g->gm_n[k], (unsigned long long)(g->gm_ns[k] / 1000));
		}
		fprintf(stderr, "SWEETGL: GAPM pid %d tid 0x%llx sec %u seg nomark n %llu us %llu nomark_n %llu miss1 %llu miss2 %llu miss3 %llu order %llu badid %llu order_pair %s %llu %s %llu %s %llu %s %llu\n",
		    pid, tid, g->win_id, (unsigned long long)g->gm_n[GM_NOMARK], (unsigned long long)(g->gm_ns[GM_NOMARK] / 1000),
		    (unsigned long long)g->gm_n[GM_NOMARK], (unsigned long long)g->gm_miss[1], (unsigned long long)g->gm_miss[2],
		    (unsigned long long)g->gm_miss[3], (unsigned long long)g->gm_order, (unsigned long long)g->gm_badid,
		    gm_pair_name[1], (unsigned long long)g->gm_order_pair[1], gm_pair_name[2], (unsigned long long)g->gm_order_pair[2],
		    gm_pair_name[3], (unsigned long long)g->gm_order_pair[3], gm_pair_name[4], (unsigned long long)g->gm_order_pair[4]);
	}
	gapw_flush(g, pid, tid);
	if (calls || g->first) {
		fprintf(stderr, "SWEETGL: GAPTSUM pid %d tid 0x%llx sec %u win_ms %llu calls %llu gap_us %llu in_us %llu first %llu\n",
		    pid, tid, g->win_id, (unsigned long long)((now - g->win0) / 1000000), (unsigned long long)calls, (unsigned long long)(gap / 1000),
		    (unsigned long long)(in / 1000), (unsigned long long)g->first);
	}
	memset(g->cell, 0, sizeof g->cell);
	memset(g->in_n, 0, sizeof g->in_n);
	memset(g->in_ns, 0, sizeof g->in_ns);
	memset(g->in_max, 0, sizeof g->in_max);
	memset(g->gm_n, 0, sizeof g->gm_n);
	memset(g->gm_ns, 0, sizeof g->gm_ns);
	memset(g->gm_miss, 0, sizeof g->gm_miss);
	g->gm_order = g->gm_badid = 0;
	memset(g->gm_order_pair, 0, sizeof g->gm_order_pair);
	g->first = 0;
	g->win0 = now;
	g->win_id++;
}

/* Right before a kernel call of this kind: books the gap since the previous call returned. Returns the entry stamp (0 = timing off). */
static uint64_t
gapt_before(unsigned kind)
{
	if (!cfg_gapt) {
		return 0;
	}
	struct gapt_tls *g = &gt;
	uint64_t t = gapt_now();
	if (!g->init) {
		gapt_tls_init(g, t);
	}
	if (!g->win0) {
		g->win0 = t;
	}
	if (g->slot) {
		gapw_window(g->slot, 0, 0);   /* every kernel entry closes the GAPW window; before the call, so a stopped thread is never sampled inside it */
		gapw_gap_close(g->slot, g->prev, (uint8_t)kind);   /* the gap that ends here is tagged (prev, kind) for the samples it holds; before gapt_flush below */
	}
	if (g->prev == GK_NONE) {
		g->first++;
	} else {
		struct gapt_cell *c = &g->cell[g->prev][kind];
		uint64_t d = t >= g->prev_t ? t - g->prev_t : 0;
		c->n++;
		c->ns += d;
		if (d > c->max) {
			c->max = d;
		}
		if (d < GAPT_SHORT_NS) {
			c->sn++;
			c->sns += d;
		}
		gapt_site_book(g, g->prev, (uint8_t)kind, d);
		if (g->prev == GK_SUBMIT && kind == GK_WAIT) {
			gapt_mark_book(g, g->prev_t, t, d);
		}
	}
	if (t - g->win0 >= GAPT_WINDOW_NS) {
		gapt_flush(g, t);
		t = gapt_now();
	}
	return t ? t : 1;
}

/* Right after the call returned. t0 = what gapt_before returned. */
static void
gapt_after(unsigned kind, uint64_t t0)
{
	if (!t0) {
		return;
	}
	struct gapt_tls *g = &gt;
	uint64_t t1 = gapt_now();
	uint64_t d = t1 >= t0 ? t1 - t0 : 0;
	g->in_n[kind]++;
	g->in_ns[kind] += d;
	if (d > g->in_max[kind]) {
		g->in_max[kind] = d;
	}
	g->prev = (uint8_t)kind;
	g->psite = g->depth ? g->cur[0] : 0;
	g->prev_t = t1;
	memset(g->mk, 0, sizeof g->mk);   /* marks belong to the gap that just ended */
}

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
	static const uint8_t sel_kind[] = { GK_NONE, GK_NEW, GK_CLOSE, GK_SUBMIT, GK_POLL, GK_IMPORT };   /* by selector; kcall(SEL_WAIT) is the BO pool's poll */
	unsigned gk = sel < sizeof sel_kind ? sel_kind[sel] : GK_NONE;
	uint64_t gt0 = gapt_before(gk);
	kern_return_t kr = IOConnectCallMethod(C.conn, sel, in, nin, NULL, 0, out, out ? &n : NULL, NULL, NULL);
	gapt_after(gk, gt0);
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
	/* sprint GPU 2026-11-10: the PC samples of gputest-prof1 show Mesa decoding ETC2 on the CPU (etc2_rgb8_parse_block) in backboardd's render
	 * thread; panfrost takes compressed-format support from TEXTURE_FEATURES_0 (bit 1 = ETC2 RGB8, pan_props.c:84): log what the GPU reported */
	fprintf(stderr, "SWEETGL: GPU props gpu_id 0x%08x texture_features 0x%08x 0x%08x 0x%08x 0x%08x afbc 0x%x thread_features 0x%x shader_present 0x%x\n",
	        info.gpu_id, info.texture_features[0], info.texture_features[1], info.texture_features[2], info.texture_features[3], info.afbc_features,
	        info.thread_features, info.shader_present);
	return 0;
}

/* Returns 0 with the lock held, or -errno with it released. */
static int
enter(void)
{
	pthread_mutex_lock(&C.lock);
	if (!cfg_done) {
		cfg_init();
	}
	int e = ensure_ctx();
	if (e) {
		pthread_mutex_unlock(&C.lock);
	} else {
		cnt_tick();
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
	return h >= 1 && h <= C.nbos && C.bos[h - 1].used && !C.bos[h - 1].pooled ? &C.bos[h - 1] : NULL;
}

static struct sobj *
so_get(uint32_t h)
{
	return h >= 1 && h <= C.nsobjs && C.sobjs[h - 1].used ? &C.sobjs[h - 1] : NULL;
}

/* Lock held: record a kernel handle (new BO or imported surface). */
static int
bo_add(uint32_t h, uint64_t va, uint64_t size, bool heap, bool exec)
{
	if (h == 0 || (h <= C.nbos && C.bos[h - 1].used)) {
		return -EIO;                /* the kernel handed out a handle we think is live (pooled ones count) */
	}
	int e = grow((void **)&C.bos, &C.nbos, sizeof(*C.bos), h - 1, MAX_HANDLES);
	if (e) {
		return e;
	}
	C.bos[h - 1] = (struct bo){ .used = true, .heap = heap, .exec = exec, .va = va, .size = size };
	return 0;
}

/* Lock held. Unmap the CPU mapping if any; 0 or a positive errno. MUST run before SEL_BO_CLOSE of the same BO: the kernel resolves the mapping through the BO
 * (IOUserClient unmap -> clientMemoryForType(handle)), so after the close the unmap fails and the mapping stays in the task with the BO's pages (defect L-01:
 * pool_evict had the order reversed and backboardd grew by every evicted BO).
 * A failure is never silent (hard rule 7): `SWEET GAP: mali BO CPU unmap failed, the mapping stays in the task (leak) xN (prog)` plus one UNMAPFAIL detail line, on the
 * first 5 failures and then every 1000th (N = failures since process start); every failure is counted in the BOCNT line (unmap_fail, unmap_fail_bytes per second,
 * unmap_fail_total since start). The format is the one sweet_gap() prints (gli_table.c); it is written here because sweetgl-clear-test links this file without
 * gli_table.c, and because the leak wants its own rate (sweet_gap repeats at 10/100/1000/10000 only).
 * On failure b->map is KEPT: the mapping is still in the task as far as we know. If the BO record survives (Mesa's unmap without the cut, or a kernel close that then
 * fails too) the next map returns that still valid address instead of stacking a second mapping, and the next close tries the unmap again. */
static int bo_unmap_failed(uint32_t h, uint64_t bytes, kern_return_t kr, const char *where);

static int
bo_unmap_cpu(uint32_t h, struct bo *b, const char *where)
{
	if (!b->map) {
		return 0;
	}
	kern_return_t kr = IOConnectUnmapMemory64(C.conn, h, mach_task_self(), b->map);
	if (kr == kIOReturnSuccess) {
		b->map = 0;
		return 0;
	}
	return bo_unmap_failed(h, b->size, kr, where);
}

/* Lock held. One failed kernel unmap of `bytes` of BO `h`: count it and say it (the rate and the two lines are described above bo_unmap_cpu). Returns the errno.
 * Shared by bo_unmap_cpu and by darwin_drm_bo_map's too-short-mapping path, which has no b->map to go through. */
static int
bo_unmap_failed(uint32_t h, uint64_t bytes, kern_return_t kr, const char *where)
{
	unsigned long long n = __atomic_add_fetch(&unmap_fail_n, 1, __ATOMIC_RELAXED);
	unsigned long long tb = __atomic_add_fetch(&unmap_fail_b, bytes, __ATOMIC_RELAXED);
	CNT(N_UNMAP_FAIL, 1);
	CNT(N_UNMAP_FAIL_B, bytes);
	if (n <= 5 || n % 1000 == 0) {
#ifdef SWEET_DRM_HOST_TEST
		const char *prog = "?";
#else
		const char *prog = getprogname();
#endif
		fprintf(stderr, "SWEET GAP: mali BO CPU unmap failed, the mapping stays in the task (leak) x%llu (%s)\n", n, prog);
		fprintf(stderr, "SWEETGL: UNMAPFAIL pid %d n %llu where %s handle %u kr 0x%x bytes %llu total_bytes %llu\n", (int)getpid(), n, where, h, (unsigned)kr,
		    (unsigned long long)bytes, tb);
	}
	return kr_errno(kr);
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
		uint64_t gt0 = gapt_before(GK_WAIT);
		kern_return_t kr = IOConnectCallMethod(C.conn, SEL_WAIT, in, 2, NULL, 0, NULL, NULL, NULL, NULL);
		gapt_after(GK_WAIT, gt0);
		CNT(N_WAIT_KCALL, 1);
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

/* Size class of a create request (SWEETGL_BOPOOL): the next multiple of 1/8 of the power of two below it (16 KB granularity of the kernel boNew, patches
 * 0104/0105, so the class is also the size the kernel returns); sizes above 64 MB (and 0) stay exact. >= s, <= 25 % above it (12.5 % above 128 KB). */
static uint64_t
size_class(uint64_t s)
{
	if (s == 0 || s > (64ull << 20)) {
		return s;
	}
	uint64_t c = (s + 16383) & ~16383ull;
	if (c > 65536) {
		uint64_t step = (1ull << (63 - __builtin_clzll(s - 1))) / 8;
		c = ((s + step - 1) / step * step + 16383) & ~16383ull;
	}
	return c;
}

/* Lock held: really close pool entry i (kernel BO and CPU map). 0 or -errno, the entry is removed only on success. */
static int
pool_evict(uint32_t i)
{
	uint32_t h = pool[i];
	struct bo *b = &C.bos[h - 1];
	uint64_t hh = h;
	/* L-01: unmap BEFORE the kernel close, as close_bo does (after it the kernel cannot find the mapping and it stays in the task). A failed unmap is logged and counted
	 * by bo_unmap_cpu and the BO is closed anyway: keeping the kernel BO would add its GPU VA and wired pages to the loss and block the pool for good. If the close
	 * fails the BO stays pooled without a mapping; darwin_drm_bo_map maps it again after a pool hit. */
	(void)bo_unmap_cpu(h, b, "pool_evict");
	int e = kcall(SEL_BO_CLOSE, &hh, 1, NULL, 0);
	if (e != 0 && e != -ENOENT) {
		CNT(N_POOL_EVICT_FAIL, 1);  /* R8: the kernel BO lives on (e.g. a job did not finish); it stays pooled and is retried */
		return e;
	}
	pool_bytes -= b->size;
	b->used = false;
	b->pooled = false;
	memmove(&pool[i], &pool[i + 1], (npool - i - 1) * sizeof(pool[0]));
	npool--;
	CNT(N_POOL_EVICT, 1);
	CNT(N_CLOSE, 1);
	return 0;
}

/* Lock held: a pooled BO of this class whose last submit finished, or -ENOENT. Oldest first (the likeliest to be idle); a busy one is skipped, never waited for. */
static int
pool_take(uint64_t size, bool exec, struct drm_panfrost_create_bo *r)
{
	for (uint32_t i = 0; i < npool; i++) {
		uint32_t h = pool[i];
		struct bo *b = &C.bos[h - 1];
		if (b->size != size || b->exec != exec) {
			continue;
		}
		if (b->seq > C.done_seq) {
			uint64_t in[2] = { b->seq, 0 };
			int e = kcall(SEL_WAIT, in, 2, NULL, 0);
			if (e == 0) {
				C.done_seq = b->seq;
			} else {
				CNT(e == -ETIMEDOUT ? N_POOL_BUSY : N_POOL_ERR, 1);
				continue;
			}
		}
		memmove(&pool[i], &pool[i + 1], (npool - i - 1) * sizeof(pool[0]));
		npool--;
		pool_bytes -= b->size;
		b->pooled = false;
		b->seq = 0;
		r->handle = h;
		r->size = (uint32_t)b->size;
		r->offset = b->va;
		CNT(N_POOL_HIT, 1);
		CNT(N_POOL_HIT_B, b->size);
		return 0;
	}
	return -ENOENT;
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
	const bool heap = r->flags & PANFROST_BO_HEAP, exec = !(r->flags & PANFROST_BO_NOEXEC);
	uint64_t want = r->size;
	if (cfg_cnt) {
		uint64_t pg = (want + 4095) >> 12;
		unsigned bk = pg ? 63 - __builtin_clzll(pg) : 0;
		__atomic_fetch_add(&szh[bk < SZ_BUCKETS ? bk : SZ_BUCKETS - 1], 1, __ATOMIC_RELAXED);
	}
	if (cfg_pool && !heap) {
		want = size_class(want);
		if (pool_take(want, exec, r) == 0) {
			return 0;
		}
		CNT(N_POOL_MISS, 1);
	}
	uint64_t in[2] = { want, exec ? SWEET_MALI_EXEC : 0 }, out[3];
	int e = kcall(SEL_BO_NEW, in, 2, out, 3);
	if (e) {
		CNT(N_CREATE_FAIL, 1);
		return e;
	}
	if (out[0] > UINT32_MAX) {
		return -EIO;
	}
	e = bo_add((uint32_t)out[0], out[1], out[2], heap, exec);
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
	CNT(N_CREATE, 1);
	CNT(N_CREATE_B, out[2]);
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
	if (cfg_pool && !b->heap && b->surf == 0 && !b->taken && b->size <= POOL_MAX_BYTES / 2) {
		/* keep it: evict the oldest entries until it fits; a failed eviction leaves the pool over its cap (counted), never loses the BO */
		while (npool >= POOL_MAX_BOS || pool_bytes + b->size > POOL_MAX_BYTES) {
			if (npool == 0 || pool_evict(0) != 0) {
				break;
			}
		}
		if (npool < POOL_MAX_BOS) {
			pool[npool++] = handle;
			pool_bytes += b->size;
			b->pooled = true;
			CNT(N_POOL_PUT, 1);
			return 0;
		}
	}
	(void)bo_unmap_cpu(handle, b, "close_bo");   /* before the kernel close (L-01); a failure is logged and counted there, the BO is closed anyway (see pool_evict) */
	uint64_t h = handle;
	int e = kcall(SEL_BO_CLOSE, &h, 1, NULL, 0);
	if (e == 0) {
		CNT(N_CLOSE, 1);
	}
	if (e == 0 || e == -ENOENT) {
		b->used = false;            /* any other error: the kernel BO lives on (e.g. the job did not finish), keep our record */
	}
	return e;
}

/* Lock held. */
static int
submit(struct drm_panfrost_submit *r)
{
	const uint32_t known = PANFROST_JD_REQ_FS | PANFROST_JD_REQ_CYCLE_COUNT | PANFROST_JD_REQ_SWEET_OVL;
	if (r->jc == 0 || (r->requirements & ~known) || r->pad) {
		return -EINVAL;
	}
	if (r->out_sync && !so_get(r->out_sync)) {
		return -ENOENT;
	}
	lagph_submit();   /* T-9577 LAGPH (log-only, SWEETGL_LAGPH=1) */
	/* in_syncs are ignored; bo_handles only feed the per-BO seqno (WAIT_BO, SWEETGL_BOCUT): one in-order queue, and the kernel orders fragment after tiler (research 32 s.2.2 row 9). */
	const bool fs = (r->requirements & PANFROST_JD_REQ_FS) != 0;
	uint64_t in[2] = { r->jc, fs ? SWEET_MALI_REQ_FS : 0 }, seq[1];
	bool ovl_fwd = false;
	if (r->requirements & PANFROST_JD_REQ_SWEET_OVL) {
		/* T-34eb: Mesa judged this vertex/tiler submit safe beside the previous FS job (R0-R4, pan_sweet_ovl.h). Forward it only if this is not an FS submit and OUR
		 * previous successful submit was an FS submit (the redundant check of H1); anything else is dropped and counted, R8: never silent. */
		if (!fs && C.prev_fs) {
			in[1] |= SWEET_MALI_REQ_OVL;
			ovl_fwd = true;
		} else {
			static bool told;
			CNT(N_SUBMIT_OVL_DROP, 1);
			if (!told) {
				told = true;
				fprintf(stderr, "SWEETGL: OVL dropped (first one, further ones are only counted): request %s, previous submit %s an FS submit\n",
				    fs ? "is an FS submit" : "is not an FS submit", C.prev_fs ? "was" : "was not");
			}
		}
	}
	C.prev_fs = false;      /* only a successful FS submit below makes it true again */
	int e = kcall(SEL_SUBMIT, in, 2, seq, 1);
	if (e) {
		return e;
	}
	if (seq[0] == 0) {
		return -EIO;
	}
	C.prev_fs = fs;
	if (ovl_fwd) {
		CNT(N_SUBMIT_OVL, 1);       /* counted once the kernel accepted it */
	}
	C.last_seq = seq[0];
	CNT(N_SUBMIT, 1);
	if (r->bo_handles) {
		const uint32_t *hs = (const uint32_t *)(uintptr_t)r->bo_handles;
		uint32_t n = r->bo_handle_count > MAX_HANDLES ? MAX_HANDLES : r->bo_handle_count;
		for (uint32_t i = 0; i < n; i++) {
			struct bo *b = bo_get(hs[i]);
			if (b) {
				b->seq = seq[0];
			} else {
				CNT(N_SUBMIT_UNK, 1);       /* R8: a listed handle we do not know is counted, never dropped silently */
			}
		}
		CNT(N_SUBMIT_BOS, n);
	}
	if (r->out_sync) {
		so_get(r->out_sync)->seq = seq[0];
	}
	return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	GAPT_SCOPE();
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
		 * device (a superset of the BO's own); with SWEETGL_BOCUT=1 for the last submit that listed this BO (bo_handles of submit(), T-2275). */
		struct drm_panfrost_wait_bo *r = arg;
		struct bo *wb = bo_get(r->handle);
		uint64_t seq = cfg_cut && wb ? wb->seq : C.last_seq;     /* BOCUT: the last submit that listed this BO (0 = none: ready) */
		e = r->pad ? -EINVAL : wb ? 0 : -ENOENT;
		leave();
		if (!e) {
			CNT(N_WAIT, 1);
			e = wait_seq(seq, r->timeout_ns);
			CNT(e == 0 ? N_WAIT_READY : e == -ETIMEDOUT ? N_WAIT_BUSY : N_WAIT_ERR, 1);
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
			CNT(N_MADV, 1);
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
	GAPT_SCOPE();
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
		CNT(N_MAP_HIT, 1);
		ret = (void *)(uintptr_t)b->map;
	} else {
		mach_vm_address_t addr = 0;
		mach_vm_size_t sz = 0;
		kern_return_t kr = IOConnectMapMemory64(C.conn, handle, mach_task_self(), &addr, &sz, kIOMapAnywhere | kIOMapWriteCombineCache);
		if (kr != kIOReturnSuccess) {
			errno = kr_errno(kr);
		} else if (sz < size) {
			/* The kernel mapped less than Mesa asked for: refuse it and drop the mapping. b->map stays 0 either way (a too-short mapping must never be handed out
			 * by the map_hit path). If the unmap fails the sz bytes stay in the task: loud and counted like every other failed unmap (hard rule 7, L-01 class). */
			kern_return_t ukr = IOConnectUnmapMemory64(C.conn, handle, mach_task_self(), addr);
			if (ukr != kIOReturnSuccess) {
				(void)bo_unmap_failed(handle, sz, ukr, "bo_map_short");
			}
			errno = EINVAL;
		} else {
			b->map = addr;
			CNT(N_MAP, 1);
			CNT(N_MAP_B, sz);
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
		if (C.bos[i].used && !C.bos[i].pooled && C.bos[i].map == (uint64_t)(uintptr_t)addr) {
			CNT(N_UNMAP, 1);
			if (cfg_cut) {
				CNT(N_UNMAP_KEPT, 1);       /* SWEETGL_BOCUT: Mesa unmaps before it caches the BO; the next map would remap the same BO. close_bo drops it. */
			} else {
				int ue = bo_unmap_cpu(i + 1, &C.bos[i], "bo_unmap");
				if (ue) {
					ret = fail(ue);         /* logged and counted; the mapping and b->map stay, Mesa is told the truth */
					break;
				}
			}
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
	/* Any real fd; Mesa exports, imports and closes it back to back (panfrost_fence_create, once per flush with a fence).
	 * A dup of the device fd (itself an open of /dev/null) skips the path lookup and the vnode open of a fresh open() and the
	 * vnode close of the matching close() (T-e76d; open+close were 9.6 % of the submit -> wait samples in T-bdc1). The open() stays
	 * as the fallback when the dup fails (e.g. EBADF if something closed the device fd). */
	int fd = C.dev_fd >= 0 ? fcntl(C.dev_fd, F_DUPFD_CLOEXEC, 0) : -1;
	if (fd < 0) {
		fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	}
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
	GAPT_SCOPE();
	(void)fd;
	return syncobj_wait(handles, NULL, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	GAPT_SCOPE();
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

/* Lock held. Import IOSurface `id` (selector 5) or return the live import of the same id (dedupe: the kernel would map it twice).
 * 0 or -errno; *bo is the BO record. */
static int
import_surface(uint32_t id, struct bo **bo)
{
	if (id == 0) {
		return -EINVAL;
	}
	for (uint32_t i = 0; i < C.nbos; i++) {
		if (C.bos[i].used && C.bos[i].surf == id) {
			*bo = &C.bos[i];
			return 0;
		}
	}
	uint64_t in = id, out[7];
	int e = kcall(SEL_IMPORT, &in, 1, out, 7);
	if (e) {
		return e;
	}
	uint32_t h = (uint32_t)out[0];
	if (out[0] > UINT32_MAX || out[1] == 0 || out[1] >= (1ull << 32) || out[2] == 0) {   /* contract: VA < 4 GiB, non-empty */
		if (out[0] != 0 && out[0] <= UINT32_MAX && !bo_get(h)) {
			(void)kcall(SEL_BO_CLOSE, &out[0], 1, NULL, 0);
		}
		return -EIO;
	}
	e = bo_add(h, out[1], out[2], false, false);
	if (e) {
		if (e != -EIO) {            /* -EIO = duplicate handle, that BO is live: keep it */
			(void)kcall(SEL_BO_CLOSE, &out[0], 1, NULL, 0);
		}
		return e;
	}
	struct bo *b = bo_get(h);
	b->surf = id;
	b->info = (struct sweet_drm_surface){ h, out[1], out[2], (uint32_t)out[3], (uint32_t)out[4], (uint32_t)out[5], (uint32_t)out[6] };
	*bo = b;
	return 0;
}

int
sweet_drm_mali_import_surface(uint32_t id, struct sweet_drm_surface *out)
{
	GAPT_SCOPE();
	int e = enter();
	if (e) {
		return e;
	}
	struct bo *b = NULL;
	e = out ? import_surface(id, &b) : -EINVAL;
	if (!e) {
		*out = b->info;
	}
	leave();
	return e;
}

int
sweet_drm_mali_release_surface(uint32_t id)
{
	GAPT_SCOPE();
	int e = enter();
	if (e) {
		return e;
	}
	e = -ENOENT;
	for (uint32_t i = 0; i < C.nbos; i++) {
		if (C.bos[i].used && C.bos[i].surf == id && id != 0) {
			e = C.bos[i].taken ? 0 : close_bo(i + 1);    /* taken: Mesa closes it via GEM_CLOSE when its resource dies */
			break;
		}
	}
	leave();
	return e;
}

/* The EAGL shim's weak hook (eagl_objc.m): import the surface and report the GPU VA. */
int
sweet_gl_kernel_import(uint32_t surface_id, uint64_t *va)
{
	struct sweet_drm_surface s;
	int e = va ? sweet_drm_mali_import_surface(surface_id, &s) : -EINVAL;
	if (!e) {
		*va = s.va;
	}
	return e;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	GAPT_SCOPE();
	(void)fd;
	int e = enter();
	if (e) {
		errno = -e;
		return e;
	}
	struct fdrec *r = fd_get(prime_fd, FD_SURFACE);
	struct bo *b = NULL;
	e = !handle || !r ? -EINVAL : import_surface(r->surface, &b);
	if (!e && r->size > b->size) {
		e = -EINVAL;                /* Mesa would size the buffer from lseek() beyond what the kernel mapped */
	}
	if (!e) {
		b->taken = true;
		*handle = b->info.handle;
	}
	leave();
	errno = -e;
	return e;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	GAPT_SCOPE();
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
