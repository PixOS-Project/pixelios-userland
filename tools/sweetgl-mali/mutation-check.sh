#!/bin/sh
# Mutation check (the unmutated source must pass first, AP-20): apply each deliberate breakage to a COPY of sweet_drm_mali.c and require the host test to FAIL. Usage: mutation-check.sh MESA_DIR
set -u
M=${1:?usage: mutation-check.sh MESA_DIR}
H=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0
"$H/host-test.sh" "$M" "$T/base" > "$T/log" 2>&1 || { echo "BASELINE FAILS"; tail -3 "$T/log"; exit 1; }
ONLY=${2:-}   # "ovl": run only the ovl-* mutants (T-34eb), after the baseline
try() { # name, sed expression
	case "$1" in ovl-*) ;; *) [ -z "$ONLY" ] || return 0 ;; esac
	sed "$2" "$H/sweet_drm_mali.c" > "$T/m.c"
	if cmp -s "$H/sweet_drm_mali.c" "$T/m.c"; then echo "MUTANT $1: sed changed nothing"; bad=1; return; fi
	if SDM_SRC="$T/m.c" "$H/host-test.sh" "$M" "$T/out" > "$T/log" 2>&1; then
		echo "MUTANT $1: SURVIVED (test passed)"; bad=1
	else
		case "$1" in ovl-*) grep -q '^FAIL' "$T/log" || { echo "MUTANT $1: died WITHOUT a failed check (build error?), not a kill (AP-214)"; bad=1; return; } ;; esac
		echo "MUTANT $1: killed ($(grep -c '^FAIL' "$T/log") failed checks, e.g. $(grep -m1 '^FAIL' "$T/log" | sed 's|.*host_test.c:||'))"
	fi
}
tryh() { # name, sed expression: breaks a COPY of the Mesa overlap rule header (pan_sweet_ovl.h) and points host-test.sh at it with OVL_DIR (AP-86: the test source stays here)
	mkdir -p "$T/ovl"
	sed "$2" "$M/src/gallium/drivers/panfrost/pan_sweet_ovl.h" > "$T/ovl/pan_sweet_ovl.h"
	if cmp -s "$M/src/gallium/drivers/panfrost/pan_sweet_ovl.h" "$T/ovl/pan_sweet_ovl.h"; then echo "MUTANT $1: sed changed nothing"; bad=1; return; fi
	if OVL_DIR="$T/ovl" "$H/host-test.sh" "$M" "$T/out" > "$T/log" 2>&1; then
		echo "MUTANT $1: SURVIVED (test passed)"; bad=1
	elif grep -q '^FAIL' "$T/log"; then
		echo "MUTANT $1: killed ($(grep -c '^FAIL' "$T/log") failed checks, e.g. $(grep -m1 '^FAIL' "$T/log" | sed 's|.*host_test.c:||'))"
	else
		echo "MUTANT $1: died WITHOUT a failed check (build error?), not a kill (AP-214)"; bad=1
	fi
}
try wrong-param-id 's/case DRM_PANFROST_PARAM_GPU_PROD_ID: v = i->gpu_id >> 16/case DRM_PANFROST_PARAM_GPU_PROD_ID: v = i->gpu_id \& 0xffff/'
try wrong-req-bit  's/(r->requirements \& PANFROST_JD_REQ_FS) ? SWEET_MALI_REQ_FS : 0/(r->requirements \& PANFROST_JD_REQ_CYCLE_COUNT) ? SWEET_MALI_REQ_FS : 0/'
try dropped-seq    's/so_get(r->out_sync)->seq = seq\[0\];/so_get(r->out_sync)->seq = 0;/'
try off-by-one-handle 's/return h >= 1 \&\& h <= C.nbos \&\& C.bos\[h - 1\].used \&\& !C.bos\[h - 1\].pooled ? \&C.bos\[h - 1\] : NULL;/return h >= 1 \&\& h <= C.nbos \&\& C.bos[h].used \&\& !C.bos[h].pooled ? \&C.bos[h] : NULL;/'
try import-no-dedupe 's/if (C.bos\[i\].used \&\& C.bos\[i\].surf == id) {/if (0) {/'
try import-swapped-pitch-width 's/(uint32_t)out\[3\], (uint32_t)out\[4\]/(uint32_t)out[4], (uint32_t)out[3]/'
try import-not-taken 's/b->taken = true;/b->taken = false;/'
try import-no-size-check 's/if (!e \&\& r->size > b->size) {/if (!e \&\& 0) {/'
try import-wrong-selector 's/SEL_WAIT, SEL_IMPORT }/SEL_WAIT, SEL_IMPORT = 6 }/'
try import-va-not-from-out 's/e = bo_add(h, out\[1\], out\[2\], false, false);/e = bo_add(h, out[1] + 0x4000, out[2], false, false);/'
# P-G1 slice 6 (T-2275): BO churn cut + counters
try bocut-wait-device-wide 's/uint64_t seq = cfg_cut \&\& wb ? wb->seq : C.last_seq;/uint64_t seq = C.last_seq;/'
try bocut-seq-not-recorded 's/\t\t\t\tb->seq = seq\[0\];/\t\t\t\tb->seq = 0;/'
try bocut-unmap-not-kept 's/\t\t\tif (cfg_cut) {/\t\t\tif (0) {/'
try bocut-close-keeps-map 's/\t(void)bo_unmap_cpu(handle, b, "close_bo");/\t(void)b;/'
try bocut-bad-knob-accepted 's/return e\[0\] == .1. \&\& !e\[1\] ? 1 : -1;/return 1;/'
try bocut-unknown-bo-silent 's/CNT(N_SUBMIT_UNK, 1);       \/\* R8/(void)0;       \/* R8/'
try bocut-wait-ready-miscounted 's/CNT(e == 0 ? N_WAIT_READY : e == -ETIMEDOUT ? N_WAIT_BUSY : N_WAIT_ERR, 1);/CNT(N_WAIT_READY, 1);/'
# P-G1 slice 7 (T-bf4f): BO recycle pool + size classes
try pool-handle-visible 's/ \&\& !C.bos\[h - 1\].pooled ? \&C.bos\[h - 1\] : NULL;/ ? \&C.bos[h - 1] : NULL;/'
try pool-no-busy-check 's/\t\tif (b->seq > C.done_seq) {/\t\tif (0) {/'
try pool-ignore-exec-key 's/if (b->size != size || b->exec != exec) {/if (b->size != size || (b->exec != exec \&\& 0)) {/'
try pool-no-class 's/\t\twant = size_class(want);/\t\t(void)size_class;/'
try pool-heap-pooled 's/if (cfg_pool \&\& !b->heap \&\& b->surf == 0/if (cfg_pool \&\& b->surf == 0/'
try pool-import-pooled 's/\&\& !b->heap \&\& b->surf == 0 \&\& !b->taken/\&\& !b->heap \&\& !b->taken/'
try pool-no-byte-cap 's/ || pool_bytes + b->size > POOL_MAX_BYTES) {/) {/'
try pool-evict-fail-silent 's/\t\tCNT(N_POOL_EVICT_FAIL, 1);  \/\* R8/\t\t(void)0;  \/* R8/'
# L-01: a pooled BO's CPU mapping is dropped before the kernel close, and a failed unmap is loud and counted
try pool-evict-keeps-map 's/\t(void)bo_unmap_cpu(h, b, "pool_evict");/\t(void)b;/'
try unmap-fail-silent 's/\tif (n <= 5 || n % 1000 == 0) {/\tif (0) {/'
try unmap-fail-not-counted 's/\tCNT(N_UNMAP_FAIL, 1);/\t(void)0;/'
try unmap-fail-clears-map 's/\tif (kr == kIOReturnSuccess) {/\tif (1) {/'
# L-01 class: darwin_drm_bo_map's too-short mapping is refused, dropped, and a failed drop is loud and counted
try map-short-accepted 's/\t\t} else if (sz < size) {/\t\t} else if (0) {/'
try map-short-not-unmapped 's/kern_return_t ukr = IOConnectUnmapMemory64(C.conn, handle, mach_task_self(), addr);/kern_return_t ukr = kIOReturnSuccess;/'
try map-short-unmap-fail-silent 's/\t\t\t\t(void)bo_unmap_failed(handle, sz, ukr, "bo_map_short");/\t\t\t\t(void)0;/'
try map-short-fail-wrong-bytes 's/bo_unmap_failed(handle, sz, ukr, "bo_map_short")/bo_unmap_failed(handle, b->size, ukr, "bo_map_short")/'
try pool-poll-error-as-busy 's/CNT(e == -ETIMEDOUT ? N_POOL_BUSY : N_POOL_ERR, 1);/CNT(N_POOL_BUSY, 1);/'
try pool-hit-keeps-request-size 's/\t\tr->size = (uint32_t)b->size;\n//; s/r->size = (uint32_t)b->size;/r->size = r->size;/'
try pool-without-cut 's/if (cfg_pool \&\& !cfg_cut) {/if (0) {/'
try pool-evicts-newest 's/pool_evict(0) != 0/pool_evict(npool - 1) != 0/'
# default ON (T-bf4f)
try pool-default-off 's/cfg_knob("SWEETGL_BOPOOL", 1)/cfg_knob("SWEETGL_BOPOOL", 0)/'
try cut-default-off 's/cfg_knob("SWEETGL_BOCUT", 1)/cfg_knob("SWEETGL_BOCUT", 0)/'
try zero-ignored 's/if (e\[0\] == .0. \&\& !e\[1\]) {/if (0) {/'
# P-G1 slice 10 (T-46d7): user-mode gap timing
try gapt-default-on 's/int v = cfg_parse(e, 0);/int v = cfg_parse(e, 1);/'
try gapt-not-thread-local 's/static __thread struct gapt_tls gt;/static struct gapt_tls gt;/'
try gapt-gap-from-entry 's/\tg->prev_t = t1;/\tg->prev_t = t0;/'
try gapt-never-flushes 's/if (t - g->win0 >= GAPT_WINDOW_NS) {/if (0) {/'
try gapt-in-call-not-summed 's/\tg->in_ns\[kind\] += d;/\tg->in_ns[kind] += 0;/'
try gapt-wait-kind-wrong 's/gapt_before(GK_WAIT)/gapt_before(GK_POLL)/'
try gapt-detail-always 's/if (calls >= GAPT_DETAIL_MIN_CALLS) {/if (1) {/'
try gapt-bad-knob-silent 's/"SWEETGL: GAPT unavailable: SWEETGL_GAPT=/"SWEETGL: GAPTX unavailable: SWEETGL_GAPT=/'
try gapt-timebase-fail-silent 's/"SWEETGL: GAPT unavailable: mach_timebase_info failed/"SWEETGL: GAPTX unavailable: mach_timebase_info failed/'
try gapt-short-bound-wrong 's/if (d < GAPT_SHORT_NS) {/if (d <= 1) {/'
try gapt-first-booked-as-gap 's/\tif (g->prev == GK_NONE) {/\tif (0) {/'
try gapt-print-time-in-call 's/\t\tgapt_flush(g, t);\n\t\tt = gapt_now();/\t\tgapt_flush(g, t);/; /^\t\tt = gapt_now();$/d'
try gapt-max-not-kept 's/\t\tif (d > c->max) {/\t\tif (0) {/'
# P-G1 slice 11 (T-d815): call-site tags
try gapt-site-walk-one-frame 's/\t\tif (f\[0\] <= fp) {/\t\tif (1) {/'
try gapt-site-walk-unbounded-stack 's/fp + 16 <= hi) {/fp + 16 <= hi + 4096) {/'
try gapt-site-walk-no-growth-check 's/\t\tif (f\[0\] <= fp) {/\t\tif (0) {/'
try gapt-site-nested-overwrites 's/if (g->depth++ == 0) {/if (g->depth++, 1) {/'
try gapt-site-depth-leak 's/\t\tgt.depth--;/\t\tgt.depth -= 0;/'
try gapt-site-psite-not-saved 's/\tg->psite = g->depth ? g->cur\[0\] : 0;/\tg->psite = 0;/'
try gapt-site-rows-merge 's/ || memcmp(e->site, site, sizeof e->site)) {/) {/'
try gapt-site-prev-not-in-key 's/e->psite != g->psite || //; s/{ prev, next, psite }/{ prev, next, psite \& 0 }/'
try gapt-site-top-off-by-one 's/i < GAPT_SITE_TOP) {/i <= GAPT_SITE_TOP) {/'
try gapt-site-rest-not-summed 's/\t\t\t\trest_n += e->n;/\t\t\t\trest_n += 0;/'
try gapt-site-rest-gap-not-summed 's/\t\t\t\trest_ns += e->ns;/\t\t\t\trest_ns += 0;/'
try gapt-site-lost-silent 's/^\tg->lost_n++;$/\tg->lost_n += 0;/'
try gapt-site-lost-gap-silent 's/^\tg->lost_ns += d;$/\tg->lost_ns += 0;/'
try gapt-site-not-sorted 's/return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;/return x->ns > y->ns ? 1 : x->ns < y->ns ? -1 : 0;/'
try gapt-site-short-not-kept 's/\t\t\te->sns += d;/\t\t\te->sns += 0;/'
try gapt-site-table-not-cleared 's/\t\tmemset(g->tab, 0, GAPT_TAB_N \* sizeof \*g->tab);//'
try gapt-site-ioctl-scope-missing '/^drmIoctl(int fd/,/^}/ s/^\tGAPT_SCOPE();$//'
try gapt-site-close-scope-missing '/^drmCloseBufferHandle(int fd/,/^}/ s/^\tGAPT_SCOPE();$//'
try gapt-site-wait-scope-missing '/^drmSyncobjWait(int fd/,/^}/ s/^\tGAPT_SCOPE();$//'
try gapt-site-base-silent 's/"SWEETGL: GAPS on pid %d base 0x%llx depth %d\\n"/"SWEETGL: GAPSX on pid %d base 0x%llx depth %d\\n"/'
try gapt-site-detail-always 's/gapt_site_flush(g, calls >= GAPT_DETAIL_MIN_CALLS);/gapt_site_flush(g, 1);/'
# P-G1 slice 12b (T-6717): Mesa time marks + GAPM rows
try gapm-marks-not-cleared 's/\tmemset(g->mk, 0, sizeof g->mk);   \/\* marks belong to the gap that just ended \*\///'
try gapm-first-reading-wins 's/\tg->mk\[id\] = t ? t : 1;/\tif (!g->mk[id]) g->mk[id] = t ? t : 1;/'
try gapm-order-unchecked 's/\t\t\tif (b\[i\] < b\[i - 1\]) {/\t\t\tif (0) {/'
try gapm-nomark-count-dropped 's/\tg->gm_n\[GM_NOMARK\]++;/\t(void)0;/'
try gapm-nomark-gap-dropped 's/\tg->gm_ns\[GM_NOMARK\] += d;/\t(void)0;/'
try gapm-miss-silent 's/\t\t\tg->gm_miss\[i\]++;/\t\t\t(void)0;/'
try gapm-order-silent 's/\t\t\t\tg->gm_order++;/\t\t\t\t(void)0;/'
try gapm-wrong-pair 's/if (g->prev == GK_SUBMIT \&\& kind == GK_WAIT) {/if (g->prev == GK_SUBMIT \&\& kind == GK_POLL) {/'
try gapm-any-pair 's/if (g->prev == GK_SUBMIT \&\& kind == GK_WAIT) {/if (1) {/'
try gapm-segment-from-start 's/\t\t\t\tg->gm_ns\[i\] += b\[i + 1\] - b\[i\];/\t\t\t\tg->gm_ns[i] += b[i + 1] - b[0];/'
try gapm-segment-end-is-prev 's/uint64_t b\[GM_MARKS + 2\] = { t0, g->mk\[1\], g->mk\[2\], g->mk\[3\], t1 };/uint64_t b[GM_MARKS + 2] = { t0, g->mk[1], g->mk[2], g->mk[3], t0 + d - 1000 };/'
try gapm-mark-ids-swapped 's/\tg->mk\[id\] = t ? t : 1;/\tg->mk[GM_MARKS + 1 - id] = t ? t : 1;/'
try gapm-badid-silent 's/"SWEETGL: GAPM unavailable: mark id/"SWEETGL: GAPMX unavailable: mark id/'
try gapm-badid-stored 's/if (id < 1 || id > GM_MARKS) {/if (id < 0 || id > GM_MARKS) {/'
try gapm-badid-not-counted 's/\t\tg->gm_badid++;/\t\t(void)0;/'
try gapm-badid-said-every-time 's/\t\tstatic int said;/\t\tint said = 0;/'
try gapm-window-not-reset 's/\tmemset(g->gm_n, 0, sizeof g->gm_n);//'
try gapm-window-ns-not-reset 's/\tmemset(g->gm_ns, 0, sizeof g->gm_ns);//'
try gapm-print-always 's/if (calls >= GAPT_DETAIL_MIN_CALLS \&\& (gm_total || g->gm_badid)) {/if (1) {/'
try gapm-nomark-row-missing 's/seg nomark n %llu us %llu nomark_n/seg nomarkX n %llu us %llu nomark_n/'
# P-G1 slice 12c (T-a479): out-of-order gaps split by the first violating boundary pair
try gapm-pair-always-first 's/\t\t\t\tg->gm_order_pair\[i\]++;/\t\t\t\tg->gm_order_pair[1]++;/'
try gapm-pair-not-counted 's/\t\t\t\tg->gm_order_pair\[i\]++;/\t\t\t\t(void)0;/'
try gapm-pair-off-by-one 's/\t\t\t\tg->gm_order_pair\[i\]++;/\t\t\t\tg->gm_order_pair[i - 1]++;/'
try gapm-pair-last-violation '/gm_order_pair\[i\]++/,/break;/ s/break;/continue;/'
try gapm-pair-not-reset 's/\tmemset(g->gm_order_pair, 0, sizeof g->gm_order_pair);//'
try gapm-pair-names-swapped 's/{ "-", "t0_m1", "m1_m2", "m2_m3", "m3_t1" }/{ "-", "t0_m1", "m2_m3", "m1_m2", "m3_t1" }/'
try gapm-pair-not-printed 's/ order_pair %s %llu/ order_pairX %s %llu/'
try gapm-pair-last-printed-wrong 's/gm_pair_name\[4\], (unsigned long long)g->gm_order_pair\[4\]/gm_pair_name[4], (unsigned long long)g->gm_order_pair[3]/'
# P-G1 slice 12d (T-baf5): window pc sampler
try gapw-default-on '/^gapw_cfg_init/,/^}/ s/int v = cfg_parse(e, 0);/int v = cfg_parse(e, 1);/'
try gapw-bad-knob-silent 's/"SWEETGL: GAPW unavailable: SWEETGL_GAPW=\\"%.16s\\" is not 0 or 1/"SWEETGL: GAPWX unavailable: SWEETGL_GAPW=\\"%.16s\\" is not 0 or 1/'
try gapw-needs-gapt-silent 's/ needs SWEETGL_GAPT=1 (gap timing is off)/ needsX SWEETGL_GAPT=1 (gap timing is off)/'
try gapw-on-line-silent 's/"SWEETGL: GAPW on pid %d/"SWEETGL: GAPWX on pid %d/'
try gapw-period-changed 's/#define GAPW_PERIOD_US   2500u/#define GAPW_PERIOD_US   250u/'
try gapw-kernel-entry-keeps-window 's/^\t\tgapw_window(g->slot, 0, 0);/\t\t(void)0;/'
try gapw-mark2-keeps-window 's/(id == 1 || id == 2)) {/(id == 1)) {/'
try gapw-mark1-no-open 's/gapw_window(g->slot, id == 1, t);/gapw_window(g->slot, 0, t);/'
try gapw-mark1-time-not-kept 's/\t\t__atomic_store_n(&s->mk1, t ? t : 1, __ATOMIC_RELAXED);/\t\t(void)0;/'
try gapw-slot-shared-between-threads 's/^\tg->slot = s;/\tg->slot = gapw_slots[0] ? gapw_slots[0] : s;/'
try gapw-no-pre-check 's/if (GAPW_LOAD(s->dead) || !__atomic_load_n(&s->win, __ATOMIC_ACQUIRE)) {/if (GAPW_LOAD(s->dead)) {/'
try gapw-no-post-stop-recheck 's/^\tif (!__atomic_load_n(&s->win, __ATOMIC_ACQUIRE)) {/\tif (0) {/'
try gapw-dead-ignored 's/if (GAPW_LOAD(s->dead) || /if (/'
try gapw-dead-never-set 's/^\t\t__atomic_store_n(&s->dead, 1u, __ATOMIC_RELAXED);/\t\t(void)0;/'
try gapw-dead-threshold 's/++s->errrun >= GAPW_DEAD_ERRS/++s->errrun > GAPW_DEAD_ERRS/'
try gapw-errrun-not-reset 's/^\ts->errrun = 0;/\t(void)0;/'
try gapw-sp-unchecked 's/^\t} else if (r.sp < s->stack_lo || r.sp >= s->stack_hi) {/\t} else if (0) {/'
try gapw-sp-upper-unchecked 's/ || r.sp >= s->stack_hi) {/) {/'
try gapw-sp-lower-unchecked 's/r.sp < s->stack_lo || r.sp >= s->stack_hi/r.sp >= s->stack_hi/'
try gapw-getstate-error-silent '/^\tif (why == 2) {/,/^\t}/ s/gapw_err(s);/(void)0;/'
try gapw-suspend-error-silent '/if (ops->suspend(s) != 0) {/,/}/ s/gapw_err(s);/(void)0;/'
try gapw-no-resume-after-getstate-error 's/^\tint rr = ops->resume(s);/\tint rr = why == 2 ? 0 : ops->resume(s);/'
try gapw-resume-error-silent 's/^\t\tGAPW_ADD(s->rerr, 1);/\t\t(void)0;/'
try gapw-stop-time-lost 's/^\tGAPW_ADD(s->stop_ns, gapt_now() - t0);/\t(void)0;/'
try gapw-ring-one-too-many 's/if (h - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE) >= GAPW_RING) {/if (h - __atomic_load_n(\&s->tail, __ATOMIC_ACQUIRE) > GAPW_RING) {/'
try gapw-drop-silent 's/^\t\t\tGAPW_ADD(s->drop, 1);/\t\t\t(void)0;/'
try gapw-notwin-silent 's/^\t\tGAPW_ADD(s->notwin, 1);/\t\t(void)0;/'
try gapw-badsp-silent 's/^\t\tGAPW_ADD(s->badsp, 1);/\t\t(void)0;/'
try gapw-ticks-not-counted 's/^\tGAPW_ADD(s->ticks, 1);/\t(void)0;/'
try gapw-kept-not-counted 's/^\t\t\tGAPW_ADD(s->kept, 1);/\t\t\t(void)0;/'
try gapw-dt-from-epoch 's/t0 >= m1 ? t0 - m1 : 0;/t0 >= m1 ? t0 : 0;/'
try gapw-pc-is-lr 's/^\t\tsmp.pc = r.pc;/\t\tsmp.pc = r.lr;/'
try gapw-walk-skipped 's/^\t\tgapt_walk((uintptr_t)r.fp, s->stack_lo, s->stack_hi, smp.fr, GAPW_FR);/\t\t(void)0;/'
try gapw-walk-from-sp 's/gapt_walk((uintptr_t)r.fp, s->stack_lo/gapt_walk((uintptr_t)r.sp, s->stack_lo/'
try gapw-sampler-prints-while-stopped 's/^\tint why = 0;  /\tfprintf(stderr, "x\\n"); int why = 0;  /'
try gapw-flush-keeps-ring 's/^\t__atomic_store_n(&s->tail, tail + n, __ATOMIC_RELEASE);/\t(void)0;/'
try gapw-counters-not-reset 's/__atomic_exchange_n(&s->ticks, 0, __ATOMIC_RELAXED)/__atomic_load_n(\&s->ticks, __ATOMIC_RELAXED)/'
try gapw-stop-not-reset 's/__atomic_exchange_n(&s->stop_ns, 0, __ATOMIC_RELAXED)/__atomic_load_n(\&s->stop_ns, __ATOMIC_RELAXED)/'
try gapw-groups-not-merged 's/if (ng \&\& gapw_cmp_key(&grp\[ng - 1\], &grp\[i\]) == 0) {/if (0) {/'
try gapw-key-ignores-lr 's/x->k.pc, x->k.lr }/x->k.pc, 0 }/; s/y->k.pc, y->k.lr }/y->k.pc, 0 }/'
try gapw-group-dt-not-summed 's/^\t\t\t\tgrp\[ng - 1\].dt += grp\[i\].dt;/\t\t\t\tgrp[ng - 1].dt += 0;/'
try gapw-sorted-ascending 's/return x->n > y->n ? -1 : 1;/return x->n > y->n ? 1 : -1;/'
try gapw-top-off-by-one 's/^\t\t\tif (i < GAPW_TOP) {/\t\t\tif (i <= GAPW_TOP) {/'
try gapw-rest-not-summed 's/^\t\t\t\trest_n += grp\[i\].n;/\t\t\t\trest_n += 0;/'
try gapw-rest-groups-not-counted 's/^\t\t\t\trest_g++;/\t\t\t\t(void)0;/'
try gapw-sum-row-missing 's/"SWEETGL: GAPWSUM pid/"SWEETGL: GAPWSUMX pid/'
try gapw-dead-line-silent 's/GAPW unavailable: tid 0x%llx disabled after/GAPWX unavailable: tid 0x%llx disabled after/'
try gapw-dead-line-every-window 's/if (dead \&\& !s->dead_said) {/if (dead) {/'
try gapw-noslot-silent 's/GAPW unavailable: more than %u threads/GAPWX unavailable: more than %u threads/'
try gapw-noslot-not-counted 's/uint32_t noslot = GAPW_LOAD(gapw_noslot);/uint32_t noslot = 0;/'
try gapw-slot-limit-off-by-one 's/if (i >= GAPW_MAX_SLOTS) {/if (i > GAPW_MAX_SLOTS) {/'
try gapw-no-bounds-silent 's/GAPW unavailable: tid 0x%llx has no stack bounds/GAPWX unavailable: tid 0x%llx has no stack bounds/'
# P-G1 slice 12e (T-bdc1): GAPW v2. (a) no stop on a full ring
try gapw2-full-still-stops '/GAPW_ADD(s->full, 1);/{n;s/return;/(void)0;/}'
try gapw2-full-not-counted 's/GAPW_ADD(s->full, 1);   \/\* v2/(void)0;   \/* v2/'
try gapw2-full-counted-as-drop 's/GAPW_ADD(s->full, 1);   \/\* v2/GAPW_ADD(s->drop, 1);   \/* v2/'
try gapw2-full-check-off-by-one 's/\t\tGAPW_ADD(s->full, 1);   \/\* v2/\t\tGAPW_ADD(s->full, 1);   \/* v2/; 0,/>= GAPW_RING) {/s//> GAPW_RING) {/'
try gapw2-drop-overwrites 's/^\t\th = s->head;   \/\* re-read.*$/\t\t(void)0;/; s/^\t\tif (h - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE) >= GAPW_RING) {\n\t\t\tGAPW_ADD(s->drop, 1);/XX/'
try gapw2-no-reread 's/^\t\th = s->head;   \/\* re-read.*$/\t\t(void)0;/'
try gapw2-sum-no-full 's/ drop %llu full %llu notwin/ drop %llu full_x %llu notwin/'
try gapw2-flush-no-full-reset 's/full = __atomic_exchange_n(&s->full, 0, __ATOMIC_RELAXED)/full = __atomic_load_n(\&s->full, __ATOMIC_RELAXED)/'
# (b) prev/next tags
try gapw2-tag-prev-is-next 's/^\t\te->prev = prev;/\t\te->prev = next;/'
try gapw2-tag-next-is-prev 's/^\t\te->next = next;/\t\te->next = prev;/'
try gapw2-tag-next-from-prev-call 's/gapw_gap_close(g->slot, g->prev, (uint8_t)kind);/gapw_gap_close(g->slot, g->prev, g->prev);/'
try gapw2-tag-prev-from-kind 's/gapw_gap_close(g->slot, g->prev, (uint8_t)kind);/gapw_gap_close(g->slot, (uint8_t)kind, (uint8_t)kind);/'
try gapw2-gap-serial-stuck 's/__atomic_store_n(&s->gap, gap + 1, __ATOMIC_RELAXED);/__atomic_store_n(\&s->gap, gap, __ATOMIC_RELAXED);/'
try gapw2-sample-gap-zero 's/smp.gap = __atomic_load_n(&s->gap, __ATOMIC_RELAXED);/smp.gap = 0;/'
try gapw2-sampled-not-recorded 's/__atomic_store_n(&s->sampled, smp.gap, __ATOMIC_RELAXED);/(void)0;/'
try gapw2-key-ignores-tags 's/xv\[2 + 2 + GAPW_FR\] = { x->prev, x->next,/xv[2 + 2 + GAPW_FR] = { 0, 0,/; s/yv\[2 + 2 + GAPW_FR\] = { y->prev, y->next,/yv[2 + 2 + GAPW_FR] = { 0, 0,/'
try gapw2-key-ignores-next 's/xv\[2 + 2 + GAPW_FR\] = { x->prev, x->next,/xv[2 + 2 + GAPW_FR] = { x->prev, 0,/; s/yv\[2 + 2 + GAPW_FR\] = { y->prev, y->next,/yv[2 + 2 + GAPW_FR] = { y->prev, 0,/'
try gapw2-row-no-tags 's/ sec %u prev %s next %s n %llu dt_us/ sec %u prevX %s nextX %s n %llu dt_us/'
try gapw2-unknown-tag-guessed 's/q->prev = q->next = GK_N;/q->prev = q->next = GK_SUBMIT;/'
try gapw2-unknown-tag-looks-known 's/return k < GK_N ? gk_name\[k\] : "?";/return gk_name[k % GK_N];/'
try gapw2-notag-not-counted 's/^\t\t\tnotag++;/\t\t\t(void)0;/'
try gapw2-tag-gap-unchecked 's/if (e->gap == q->k.gap) {/if (1) {/'
try gapw2-close-after-flush 's/^\t\tgapw_gap_close(g->slot, g->prev, (uint8_t)kind);.*$/\t\t(void)0;/; s/^\t\tgapt_flush(g, t);/\t\tgapt_flush(g, t);\n\t\tif (g->slot) gapw_gap_close(g->slot, g->prev, (uint8_t)kind);/'
try gapw2-table-too-small 's/#define GAPW_NX          4096u/#define GAPW_NX          2048u/'
try gapw2-sum-no-notag 's/ rerr %llu notag %llu stop_us/ rerr %llu notag_x %llu stop_us/'
# (c) every group printed
try gapw2-top-small 's/#define GAPW_TOP         GAPW_RING /#define GAPW_TOP         32u /'
try gapw2-top-off-by-one-small 's/^\t\t\tif (i < GAPW_TOP) {/\t\t\tif (i < GAPW_TOP - 1) {/'
# (d) frames
try gapw2-frames-three 's/#define GAPW_FR          8/#define GAPW_FR          3/'
try gapw2-frames-not-printed 's/for (int f = 0; f < GAPW_FR; f++) {/for (int f = 0; f < 3; f++) {/'
try gapw2-frames-not-in-key 's/for (int i = 0; i < GAPW_FR; i++) {\n\t\txv/XX/; s/xv\[4 + i\] = x->k.fr\[i\];/xv[4 + i] = 0;/'
# T-34eb: OVL pass-through and the overlap rule (host-test.sh runs sweet_drm_mali_ovl_test.c)
try ovl-forward-after-non-fs 's/if (!fs \&\& C.prev_fs) {/if (!fs) {/'
try ovl-forward-on-fs-request 's/if (!fs \&\& C.prev_fs) {/if (C.prev_fs) {/'
try ovl-prev-not-reset 's/^\tC.prev_fs = false;.*$/\t(void)0;/'
try ovl-drop-silent 's/^\t\t\tif (!told) {/\t\t\tif (!told \&\& 0) {/'
try ovl-bit-not-known 's/ | PANFROST_JD_REQ_SWEET_OVL;/;/'
tryh ovl-rule-drop-r0 's/if (!p->valid || p->dev != c->dev)/if (0)/'
tryh ovl-rule-drop-r1 's/^   if (c->has_tiler) {/   if (0) {/'
tryh ovl-rule-drop-r2 's/if (sweet_ovl_intersects(c->vt, c->n_vt, p->wr, p->n_wr))/if (0)/'
tryh ovl-rule-drop-r3 's/if (sweet_ovl_intersects(c->vtw, c->n_vtw, p->all, p->n_all))/if (0)/'
tryh ovl-rule-drop-r4 's/^   if (c->in_sync)/   if (0)/'
tryh ovl-rule-r1-ignores-same 's/if (c->half == p->half)/if (0)/'
tryh ovl-rule-r2-uses-all 's/sweet_ovl_intersects(c->vt, c->n_vt, p->wr, p->n_wr)/sweet_ovl_intersects(c->vt, c->n_vt, p->all, p->n_all)/'
[ $bad = 0 ] && echo "all mutants killed" || { echo "MUTATION CHECK FAILED"; exit 1; }
