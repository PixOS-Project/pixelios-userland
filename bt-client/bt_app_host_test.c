/* Host test for the BTApp core (T-2d63, bt_app_core.c): app-state windows, per-window counts, the fg-only pass rule and EVERY
 * gap, with stubbed ops. A scripted list of (time, state) transitions stands in for the UIKit delegate; each one snapshots the
 * counters as the iOS backend does. Lines are compared whole. Output must hold no MAC- or UUID-shaped string (rule 4).
 * bt-client/app-mutant-test.sh applies the core mutants and expects this test to FAIL on each. */
#include "bt_client.h"

#include <regex.h>
#include <stdio.h>
#include <string.h>

#define NW 16
#define NT 24

struct ev {
	unsigned at;	/* now value at which the delegate callback runs */
	int state;
};

struct st {
	unsigned now, step;	/* step: seconds now_s advances per sleep (0 = stalled clock) */
	long state;
	int scanning, start_rc, reach_power_on, stop_scan_at, rescans, scan_lost;
	unsigned powered_at;	/* sample index at which the central is poweredOn (0 = at start if reach_power_on) */
	unsigned fg_rate, bg_rate, other_rate;	/* didDiscover calls per sample by app state */
	unsigned discovered, unique, samples, backend_gaps;
	unsigned side_gaps;	/* GAP lines the app side reports (side_gaps callback) */
	int rescan_rc;	/* nonzero: s_rescan returns this (a failed rescan when < 0) */
	unsigned snap_extra;	/* added to the transition snapshot's discovered/unique (counters going back at the close) */
	int app;	/* current app state as the stub UIKit sees it */
	struct btc_conn cs[4];	/* T-3615: scripted outcome of connect try i */
	unsigned conn_calls, conn_first_now;	/* conn_first_now: the stub clock at the first connect call */
	struct btc_pair pr;	/* T-a52f: scripted outcome of the pair step */
	unsigned pair_calls, pair_first_now, pair_timeout_seen;
	struct ev ev[NT];
	unsigned nev, nrec;
	struct btc_trans rec[NT];
	char out[16384];
	size_t outlen;
};

static int s_start(void *c)
{
	struct st *s = c;

	if (s->start_rc == 0 && s->powered_at == 0 && s->reach_power_on)
		s->state = BTC_STATE_POWERED_ON;
	return s->start_rc;
}

static const char *s_opts(void *c)
{
	(void)c;
	return "kCBTestKey=0";
}

static int s_rescan(void *c)
{
	struct st *s = c;

	if (s->rescan_rc)
		return s->rescan_rc;
	s->scan_lost = 0;
	return ++s->rescans;
}

static int s_arm(void *c, char *d, unsigned long n)
{
	(void)c;
	(void)d;
	(void)n;
	return -1;
}

/* the delegate callbacks whose time has come run now, each with a snapshot of the counters */
static void fire(struct st *s)
{
	for (unsigned i = 0; i < s->nev; i++) {
		if (s->ev[i].at <= s->now && i >= s->nrec) {
			s->app = s->ev[i].state;
			s->rec[s->nrec] = (struct btc_trans){ s->ev[i].state, s->ev[i].at, s->discovered + s->snap_extra, s->unique + s->snap_extra };
			s->nrec++;
		}
	}
}

static void s_sample(void *c, struct btc_sample *o)
{
	struct st *s = c;
	unsigned r;

	s->samples++;
	if (s->powered_at > 0 && s->samples >= s->powered_at)
		s->state = BTC_STATE_POWERED_ON;
	if (s->stop_scan_at > 0 && s->samples == (unsigned)s->stop_scan_at)
		s->scan_lost = 1;
	if (s->state == BTC_STATE_POWERED_ON)
		s->scanning = !s->scan_lost;
	if (s->state == BTC_STATE_POWERED_ON && s->scanning) {
		r = s->app == BTC_APP_ACTIVE ? s->fg_rate : s->app == BTC_APP_BACKGROUND ? s->bg_rate : s->other_rate;
		s->discovered += r;
		s->unique += r ? 1 : 0;
	}
	o->state = s->state;
	o->scanning = s->scanning;
	o->discovered = s->discovered;
	o->unique = s->unique;
	o->gaps = s->backend_gaps;
}

static void s_sleep(void *c, unsigned secs)
{
	struct st *s = c;

	(void)secs;
	s->now += s->step;
	fire(s);
}

static void s_out(void *c, const char *line)
{
	struct st *s = c;
	size_t n = strlen(line);

	if (s->outlen + n + 2 < sizeof s->out) {
		memcpy(s->out + s->outlen, line, n);
		s->outlen += n;
		s->out[s->outlen++] = '\n';
		s->out[s->outlen] = 0;
	}
}

static int s_trans(void *c, unsigned i, struct btc_trans *t)
{
	struct st *s = c;

	if (i >= s->nrec)
		return 0;
	*t = s->rec[i];
	return 1;
}

static unsigned s_now(void *c)
{
	return ((struct st *)c)->now;
}

static unsigned s_side(void *c)
{
	return ((struct st *)c)->side_gaps;
}

static int s_connect(void *c, unsigned try_idx, unsigned timeout_s, struct btc_conn *r)
{
	struct st *s = c;

	(void)timeout_s;
	if (s->conn_calls++ == 0)
		s->conn_first_now = s->now;
	*r = s->cs[try_idx < 4 ? try_idx : 3];
	return 0;
}

static int s_pair(void *c, unsigned timeout_s, struct btc_pair *r)
{
	struct st *s = c;

	if (s->pair_calls++ == 0)
		s->pair_first_now = s->now;
	s->pair_timeout_seen = timeout_s;
	*r = s->pr;
	return 0;
}

static int has_line(const char *out, const char *line)
{
	size_t n = strlen(line);
	const char *p = out;

	while (*p) {
		const char *e = strchr(p, '\n');

		if (!e)
			e = p + strlen(p);
		if ((size_t)(e - p) == n && memcmp(p, line, n) == 0)
			return 1;
		p = *e ? e + 1 : e;
	}
	return 0;
}

static int count_sub(const char *out, const char *sub)
{
	int n = 0;

	for (const char *p = out; (p = strstr(p, sub)); p++)
		n++;
	return n;
}

static int rx_match(const char *pat, const char *text)
{
	regex_t re;
	int hit;

	if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB))
		return -1;
	hit = regexec(&re, text, 0, NULL, 0) == 0;
	regfree(&re);
	return hit;
}

struct tc {
	const char *name;
	struct st st;
	unsigned total;
	int want_rc;
	const char *want[NW];
	const char *absent[4];
	const char *once;	/* substring that must occur exactly once (0 = none) */
	unsigned conn_tries;	/* T-3615: connect step on (0 = off), conn_at_s 3, timeout 10 s */
	unsigned want_calls, want_first_now;	/* want_first_now: the stub clock (relative to the start 0) at the first connect call, 0 = unchecked */
	int pair_on;	/* T-a52f: pair step on (pair_at_s 3, timeout 60 s); want_pair_calls = expected calls */
	unsigned want_pair_calls;
};

int main(void)
{
	/* UIKit order on launch: didFinishLaunching (no state change), willEnterForeground/inactive is skipped at cold launch,
	 * didBecomeActive; to the background: willResignActive (inactive) then didEnterBackground; back: willEnterForeground
	 * (inactive) then didBecomeActive. */
	struct tc cases[] = {
	    { "fg_bg_fg_windows",	/* fg 2/sample, bg 0: the control window counts 0, the two fg windows count their own samples */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 2, .bg_rate = 0, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE }, { 4, BTC_APP_INACTIVE }, { 4, BTC_APP_BACKGROUND }, { 7, BTC_APP_INACTIVE },
	                { 7, BTC_APP_ACTIVE } }, .nev = 5 },
	      9, 0,
	      { "BT CLIENT: start build=hosttest mode=app period=1s total=9s central_options=kCBTestKey=0 scan=services:nil,options:nil",
	        "BT CLIENT: app state=active t=1s discovered=0 unique~0",
	        "BT CLIENT: t=1s app=active state=5 scanning=1 discovered=2 unique~1 gaps=0",
	        "BT CLIENT: app state=background t=4s discovered=6 unique~3",
	        "BT CLIENT: t=5s app=background state=5 scanning=1 discovered=6 unique~3 gaps=0",
	        "BT CLIENT: ARM pre1 services:nil discovered=0 unique_new=0 t=0..1s",
	        "BT CLIENT: ARM fg1 services:nil discovered=6 unique_new=3 t=1..4s",
	        "BT CLIENT: ARM in1 services:nil discovered=0 unique_new=0 t=4..4s",
	        "BT CLIENT: ARM bg1 services:nil discovered=0 unique_new=0 t=4..7s",
	        "BT CLIENT: ARM in2 services:nil discovered=0 unique_new=0 t=7..7s",
	        "BT CLIENT: ARM fg2 services:nil discovered=8 unique_new=4 t=7..9s",
	        "BT CLIENT: RESULT pass discovered=14 unique=7 fg_discovered=14 bg_discovered=0 windows=6 state=5 gaps=0", 0 },
	      { "GAP", "scan resume", 0 }, 0 },
	    { "side_gaps_counted",	/* review T-2d63 LOW 6: GAP lines the app side printed are in RESULT gaps= */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 0, .bg_rate = 3, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE }, { 2, BTC_APP_BACKGROUND } }, .nev = 2, .side_gaps = 2 },
	      4, 1,
	      { "BT CLIENT: RESULT fail discovered=12 unique=4 fg_discovered=0 bg_discovered=12 windows=3 state=5 gaps=2", 0 },
	      { 0 }, 0 },
	    { "bg_only_is_fail",	/* discoveries only while backgrounded: fg_discovered=0 -> RESULT fail (pass is fg only) */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 0, .bg_rate = 3, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE }, { 2, BTC_APP_BACKGROUND } }, .nev = 2 },
	      4, 1,
	      { "BT CLIENT: ARM fg1 services:nil discovered=0 unique_new=0 t=1..2s",
	        "BT CLIENT: ARM bg1 services:nil discovered=12 unique_new=4 t=2..4s",
	        "BT CLIENT: RESULT fail discovered=12 unique=4 fg_discovered=0 bg_discovered=12 windows=3 state=5 gaps=0", 0 },
	      { "GAP", 0 }, 0 },
	    { "never_launched",		/* no delegate callback at all: one pre window, fail, nothing claimed as fg */
	      { .step = 2, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 5, .other_rate = 1, .app = BTC_APP_LAUNCHING },
	      4, 1,
	      { "BT CLIENT: t=2s app=launching state=5 scanning=1 discovered=1 unique~1 gaps=0",
	        "BT CLIENT: ARM pre1 services:nil discovered=3 unique_new=3 t=0..4s",
	        "BT CLIENT: RESULT fail discovered=3 unique=3 fg_discovered=0 bg_discovered=0 windows=1 state=5 gaps=0", 0 },
	      { "ARM fg", "GAP", 0 }, 0 },
	    { "start_failure",
	      { .step = 1, .state = -1, .scanning = -1, .start_rc = -1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1 },
	      2, 1,
	      { "BT CLIENT: start build=hosttest mode=app period=1s total=2s central_options=none(no-manager) scan=services:nil,options:nil",
	        "BT CLIENT GAP: start failed rc=-1 (no central; counts stay 0)",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 fg_discovered=0 bg_discovered=0 windows=2 state=-1 gaps=1", 0 },
	      { "never reached poweredOn", "scan resume", 0 }, 0 },
	    { "never_powered_on",	/* clock starts at 100 (times are relative to the start), backend reports 2 gaps of its own */
	      { .now = 100, .step = 1, .state = 4, .scanning = 0, .backend_gaps = 2, .app = BTC_APP_LAUNCHING,
	        .ev = { { 101, BTC_APP_ACTIVE } }, .nev = 1 },
	      2, 1,
	      { "BT CLIENT: app state=active t=1s discovered=0 unique~0",
	        "BT CLIENT: t=1s app=active state=4 scanning=0 discovered=0 unique~0 gaps=2",
	        "BT CLIENT GAP: central never reached poweredOn (state=4)",
	        "BT CLIENT: ARM fg1 services:nil discovered=0 unique_new=0 t=1..2s",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 fg_discovered=0 bg_discovered=0 windows=2 state=4 gaps=3", 0 },
	      { "start failed", "scan resume", 0 }, 0 },
	    { "clock_stalled",		/* now_s never advances: the loop ends after total/period+4 samples with a gap */
	      { .step = 0, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 0, BTC_APP_ACTIVE } }, .nev = 1 },
	      3, 0,
	      { "BT CLIENT GAP: clock stalled at t=0s after 7 samples (now_s does not advance; run ended early)",
	        "BT CLIENT: ARM fg1 services:nil discovered=8 unique_new=8 t=0..0s",
	        "BT CLIENT: RESULT pass discovered=8 unique=8 fg_discovered=8 bg_discovered=0 windows=2 state=5 gaps=1", 0 },
	      { 0 }, 0 },
	    { "repeat_state_no_window",	/* active twice in a row is one window */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE }, { 2, BTC_APP_ACTIVE } }, .nev = 2 },
	      3, 0,
	      { "BT CLIENT: ARM fg1 services:nil discovered=4 unique_new=4 t=1..3s",
	        "BT CLIENT: RESULT pass discovered=4 unique=4 fg_discovered=4 bg_discovered=0 windows=2 state=5 gaps=0", 0 },
	      { "ARM fg2", "GAP", 0 }, 0 },
	    { "window_overflow",	/* 20 alternating changes: windows capped at 16, ONE gap line, later counts merged into the last */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .bg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 },
	                { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 }, { 1, 0 }, { 1, 2 } }, .nev = 20 },
	      3, 1,
	      { "BT CLIENT GAP: more than 16 app-state windows (later changes merged into window 16)",
	        "BT CLIENT: ARM fg7 services:nil discovered=0 unique_new=0 t=1..1s",
	        "BT CLIENT: ARM bg7 services:nil discovered=0 unique_new=0 t=1..1s",
	        "BT CLIENT: ARM mix1 services:nil discovered=4 unique_new=4 t=1..3s",
	        "BT CLIENT: RESULT fail discovered=4 unique=4 fg_discovered=0 bg_discovered=0 windows=16 state=5 gaps=1", 0 },
	      { "ARM fg8", 0 }, "more than 16 app-state windows" },
	    { "scan_lost_is_resumed",	/* poweredOn without a scan -> rescan, logged */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .stop_scan_at = 2, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1 },
	      3, 0,
	      { "BT CLIENT: t=2s app=active state=5 scanning=0 discovered=1 unique~1 gaps=0",
	        "BT CLIENT: scan resumed reason=poweredOn-not-scanning n=1",
	        "BT CLIENT: RESULT pass discovered=3 unique=3 fg_discovered=3 bg_discovered=0 windows=2 state=5 gaps=0", 0 },
	      { "GAP", 0 }, 0 },
	    { "rescan_fails_is_gap",	/* poweredOn, scan lost, rescan returns < 0 -> GAP line, counted in gaps= (review T-2d63 LOW 3) */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .stop_scan_at = 2, .rescan_rc = -5,
	        .app = BTC_APP_LAUNCHING, .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1 },
	      3, 0,
	      { "BT CLIENT GAP: scan resume failed rc=-5 (poweredOn, not scanning, no scan issued)",
	        "BT CLIENT: RESULT pass discovered=1 unique=1 fg_discovered=1 bg_discovered=0 windows=2 state=5 gaps=2", 0 },
	      { "scan resumed", 0 }, 0 },
	    { "counters_back_is_gap",	/* transition snapshot above the closing sample -> window counted 0, GAP (review T-2d63 LOW 3) */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .snap_extra = 10, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1 },
	      3, 1,
	      { "BT CLIENT GAP: window 2 counters went back (10 -> 4), counted 0",
	        "BT CLIENT: ARM fg1 services:nil discovered=0 unique_new=0 t=1..3s",
	        "BT CLIENT: RESULT fail discovered=4 unique=4 fg_discovered=0 bg_discovered=0 windows=2 state=5 gaps=1", 0 },
	      { 0 }, "counters went back" },
	    { "transition_before_start",	/* a transition stamped before t_start counts as t=0, no unsigned wrap (review T-2d63 LOW 4) */
	      { .now = 5, .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1 },
	      3, 0,
	      { "BT CLIENT: app state=active t=0s discovered=0 unique~0",
	        "BT CLIENT: ARM pre1 services:nil discovered=0 unique_new=0 t=0..0s",
	        "BT CLIENT: ARM fg1 services:nil discovered=4 unique_new=4 t=0..3s", 0 },
	      { "4294967", "GAP", 0 }, 0 },
	    { "connect_first_try_ok",	/* T-3615: ACTIVE at t=1, connect at t=3, one attempt, services answered, stops after the first success */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1,
	        .cs = { { BTC_CONN_OK, 0, 0, -60, 250, 3, 2, 1 } } },
	      8, 0,
	      { "BT CLIENT: connect try=1 outcome=connected cands=3 rssi=-60 ms=250 err=0 services=2 disc=1",
	        "BT CLIENT: RESULT: PASS bt-connect conn_ok=1 attempts=1 services=2 first_fail=none gaps=0", 0 },
	      { "GAP", 0 }, 0, 3, 1, 3 },
	    { "connect_fail_timeout_then_ok",	/* a failure and a timeout are attempts; the third connects; first_fail names the first non-success */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1,
	        .cs = { { BTC_CONN_FAIL, 1, 62, -50, 120, 4, -1, 0 }, { BTC_CONN_TIMEOUT, 0, 0, -70, 10000, 3, -1, 1 },
	                { BTC_CONN_OK, 0, 0, -80, 900, 2, -1, 1 } } },
	      8, 0,
	      { "BT CLIENT: connect try=1 outcome=failed cands=4 rssi=-50 ms=120 err=62 services=-1 disc=0",
	        "BT CLIENT: connect try=2 outcome=timeout cands=3 rssi=-70 ms=10000 err=0 services=-1 disc=1",
	        "BT CLIENT: connect try=3 outcome=connected cands=2 rssi=-80 ms=900 err=0 services=-1 disc=1",
	        "BT CLIENT: RESULT: PASS bt-connect conn_ok=1 attempts=3 services=-1 first_fail=failed gaps=0", 0 },
	      { "GAP", 0 }, 0, 3, 3 },
	    { "connect_all_fail",	/* three attempts, none connected -> FAIL, first_fail is the first outcome */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1,
	        .cs = { { BTC_CONN_TIMEOUT, 0, 0, -50, 10000, 4, -1, 0 }, { BTC_CONN_FAIL, 1, 3, -60, 80, 3, -1, 0 },
	                { BTC_CONN_FAIL, 1, 3, -70, 90, 2, -1, 0 } } },
	      8, 0,
	      { "BT CLIENT: RESULT: FAIL bt-connect conn_ok=0 attempts=3 services=-1 first_fail=timeout gaps=0", 0 },
	      { "GAP", "conn_ok=1", 0 }, 0, 3, 3 },
	    { "connect_no_candidate",	/* nothing connectable heard: no attempt, a GAP, FAIL (no silent skip) */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .cs = { { BTC_CONN_NOCAND, 0, 0, 127, 0, 0, -1, 0 } } },
	      8, 0,
	      { "BT CLIENT GAP: connect try=1 no-candidate (cands=0): nothing was sent",
	        "BT CLIENT: RESULT: FAIL bt-connect conn_ok=0 attempts=0 services=-1 first_fail=no-candidate gaps=1", 0 },
	      { "outcome=", 0 }, 0, 3, 1 },
	    { "connect_never_active",	/* the app never reached the ACTIVE window: the step never ran -> GAP, FAIL, zero backend calls */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING },
	      8, 1,
	      { "BT CLIENT GAP: connect step never ran (no ACTIVE window at t >= 3s)",
	        "BT CLIENT: RESULT: FAIL bt-connect conn_ok=0 attempts=0 services=-1 first_fail=no-central gaps=1", 0 },
	      { "outcome=", 0 }, 0, 3, 0 },
	    { "connect_late_active",	/* ACTIVE only at t=5 > conn_at_s: the step runs at the first ACTIVE sample, not before */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 5, BTC_APP_ACTIVE } }, .nev = 1, .cs = { { BTC_CONN_OK, 0, 0, -40, 300, 1, 1, 1 } } },
	      8, 0,
	      { "BT CLIENT: connect try=1 outcome=connected cands=1 rssi=-40 ms=300 err=0 services=1 disc=1",
	        "BT CLIENT: RESULT: PASS bt-connect conn_ok=1 attempts=1 services=1 first_fail=none gaps=0", 0 },
	      { "GAP", 0 }, 0, 3, 1, 5 },
	    { "pair_new_bond",	/* T-a52f: fresh bond: bonded_start 0 -> 1 after a confirmation prompt */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_OK, BTC_PAIR_ST_PAIRED, 0, 0, 14000, 0, 1, 1, 1 } },
	      8, 0,
	      { "BT CLIENT: pair outcome=paired stage=paired ms=14000 err=0 bonded_start=0 bonded_end=1 prompt=1 connected=1 peer_bonded=0",
	        "BT CLIENT: RESULT: PASS bt-pair bonded=1 reconnect=0 first_fail=none stage=paired gaps=0", 0 },
	      { "GAP", 0 }, 0, 0, 0, 0, 1, 1 },
	    { "pair_reconnect",	/* T-a52f: bond survived a reboot: connected with no prompt */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_OK, BTC_PAIR_ST_PAIRED, 0, 0, 900, 1, 1, 0, 1 } },
	      8, 0,
	      { "BT CLIENT: RESULT: PASS bt-pair bonded=0 reconnect=1 first_fail=none stage=paired gaps=0", 0 },
	      { "GAP", 0 }, 0, 0, 0, 0, 1, 1 },
	    { "pair_reconnect_with_prompt",	/* T-a52f: existing bond but a new prompt appeared: neither bonded nor reconnect -> FAIL + shape GAP */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_OK, BTC_PAIR_ST_PAIRED, 0, 0, 900, 1, 1, 1, 1 } },
	      8, 0,
	      { "BT CLIENT GAP: pair ok but bonded_start=1 bonded_end=1 prompt=1 connected=1 fits neither bonded nor reconnect",
	        "BT CLIENT: RESULT: FAIL bt-pair bonded=0 reconnect=0 first_fail=paired stage=paired gaps=1", 0 },
	      { "zzz" }, 0, 0, 0, 0, 1, 1 },
	    { "pair_bonded_at_start",	/* peer already bonded + connected, no prompt: PASS bonded=1 reconnect=1 stage=bonded */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_OK, BTC_PAIR_ST_BONDED, 0, 0, 1500, 1, 1, 0, 1, 1 } },
	      8, 0,
	      { "BT CLIENT: pair outcome=paired stage=bonded ms=1500 err=0 bonded_start=1 bonded_end=1 prompt=0 connected=1 peer_bonded=1",
	        "BT CLIENT: RESULT: PASS bt-pair bonded=1 reconnect=1 first_fail=none stage=bonded gaps=0", 0 },
	      { "GAP", 0 }, 0, 0, 0, 0, 1, 1 },
	    { "pair_bonded_no_reconnect",	/* peer bonded, connect failed: still PASS bonded=1, reconnect=0 is a GAP line (not a FAIL) */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_OK, BTC_PAIR_ST_BONDED, 1, 4, 5000, 1, 1, 0, 0, 1 } },
	      8, 0,
	      { "BT CLIENT GAP: pair peer already bonded but no reconnect (connected=0 prompt=0 err=4)",
	        "BT CLIENT: RESULT: PASS bt-pair bonded=1 reconnect=0 first_fail=none stage=bonded gaps=1", 0 },
	      { "zzz" }, 0, 0, 0, 0, 1, 1 },
	    { "pair_no_peer",	/* T-a52f: nothing discovered: FAIL names the first missing step */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_NOPEER, BTC_PAIR_ST_NONE, 0, 0, 60000, 0, 0, 0, 0 } },
	      8, 0,
	      { "BT CLIENT: RESULT: FAIL bt-pair bonded=0 reconnect=0 first_fail=no-peer stage=none gaps=0", 0 },
	      { "GAP", 0 }, 0, 0, 0, 0, 1, 1 },
	    { "pair_stuck_at_confirm",	/* T-a52f: prompt answered, no paired status: timeout at stage answered */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_TIMEOUT, BTC_PAIR_ST_ANSWERED, 0, 0, 60000, 0, 0, 1, 0 } },
	      8, 0,
	      { "BT CLIENT: RESULT: FAIL bt-pair bonded=0 reconnect=0 first_fail=timeout stage=answered gaps=0", 0 },
	      { "GAP", 0 }, 0, 0, 0, 0, 1, 1 },
	    { "pair_failed_err",	/* T-a52f: API failure carries the error code */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_FAIL, BTC_PAIR_ST_STARTED, 1, 5, 3000, 0, 0, 0, 0 } },
	      8, 0,
	      { "BT CLIENT: pair outcome=failed stage=started ms=3000 err=5 bonded_start=0 bonded_end=0 prompt=0 connected=0 peer_bonded=0",
	        "BT CLIENT: RESULT: FAIL bt-pair bonded=0 reconnect=0 first_fail=failed stage=started gaps=0", 0 },
	      { "GAP", 0 }, 0, 0, 0, 0, 1, 1 },
	    { "pair_no_manager",	/* T-a52f: the pairing API was unusable: GAP, FAIL, no silent skip */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING,
	        .ev = { { 1, BTC_APP_ACTIVE } }, .nev = 1, .pr = { BTC_PAIR_NOMGR, BTC_PAIR_ST_NONE, 0, 0, 0, -1, -1, 0, 0 } },
	      8, 0,
	      { "BT CLIENT GAP: pair no-manager: nothing was sent",
	        "BT CLIENT: RESULT: FAIL bt-pair bonded=0 reconnect=0 first_fail=no-manager stage=none gaps=1", 0 },
	      { "zzz" }, 0, 0, 0, 0, 1, 1 },
	    { "pair_never_active",	/* T-a52f: never ACTIVE: step never ran -> GAP, FAIL, zero backend calls */
	      { .step = 1, .state = -1, .scanning = -1, .reach_power_on = 1, .fg_rate = 1, .app = BTC_APP_LAUNCHING },
	      8, 1,
	      { "BT CLIENT GAP: pair step never ran (no ACTIVE window at t >= 3s)",
	        "BT CLIENT: RESULT: FAIL bt-pair bonded=0 reconnect=0 first_fail=no-central stage=none gaps=1", 0 },
	      { "zzz", 0 }, 0, 0, 0, 0, 1, 0 },
	};
	unsigned fails = 0, ncases = sizeof cases / sizeof cases[0];
	int r1 = rx_match("[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}", "aa:bb:cc:dd:ee:ff"), r2 = rx_match("[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}", "t=1..4s");

	if (r1 != 1 || r2 != 0) {
		printf("deny regex self-check failed (%d %d)\n", r1, r2);
		fails++;
	}
	if (strcmp(btc_app_label(BTC_APP_ACTIVE), "fg") || strcmp(btc_app_label(BTC_APP_BACKGROUND), "bg") ||
	    strcmp(btc_app_label(BTC_APP_INACTIVE), "in") || strcmp(btc_app_label(BTC_APP_LAUNCHING), "pre") || strcmp(btc_app_label(BTC_APP_MIXED), "mix") || strcmp(btc_app_label(7), "unknown")) {
		printf("case labels: FAILED\n");
		fails++;
	}
	for (unsigned i = 0; i < ncases; i++) {
		struct tc *t = &cases[i];
		struct btc_ops bt = { &t->st, s_start, s_opts, s_rescan, s_arm, s_arm, s_sample, s_sleep, s_out };
		struct btc_app_ops ao = { &bt, s_trans, s_now, s_side, t->conn_tries ? s_connect : 0, 3, t->conn_tries, 10,
		    t->pair_on ? s_pair : 0, 3, 60 };
		int rc = btc_app_run(&ao, 1, t->total, "hosttest");
		int ok = rc == t->want_rc;

		if (!ok)
			printf("case %s: rc=%d want %d\n", t->name, rc, t->want_rc);
		for (int k = 0; t->want[k]; k++)
			if (!has_line(t->st.out, t->want[k])) {
				printf("case %s: missing line \"%s\"\n", t->name, t->want[k]);
				ok = 0;
			}
		for (int k = 0; t->absent[k]; k++)
			if (strstr(t->st.out, t->absent[k])) {
				printf("case %s: forbidden text \"%s\"\n", t->name, t->absent[k]);
				ok = 0;
			}
		if (t->st.conn_calls != t->want_calls) {
			printf("case %s: connect called %u times, want %u\n", t->name, t->st.conn_calls, t->want_calls);
			ok = 0;
		}
		if (t->st.pair_calls != t->want_pair_calls || (t->pair_on && t->st.pair_calls && t->st.pair_timeout_seen != 60)) {
			printf("case %s: pair called %u times (timeout %u), want %u (60)\n", t->name, t->st.pair_calls, t->st.pair_timeout_seen, t->want_pair_calls);
			ok = 0;
		}
		if (t->want_first_now && t->st.conn_first_now != t->want_first_now) {
			printf("case %s: first connect call at now=%u, want %u\n", t->name, t->st.conn_first_now, t->want_first_now);
			ok = 0;
		}
		if (t->once && count_sub(t->st.out, t->once) != 1) {
			printf("case %s: \"%s\" occurs %d times, want 1\n", t->name, t->once, count_sub(t->st.out, t->once));
			ok = 0;
		}
		if (rx_match("[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}", t->st.out) != 0 ||
		    rx_match("[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}", t->st.out) != 0) {
			printf("case %s: output contains a MAC- or UUID-shaped string\n", t->name);
			ok = 0;
		}
		printf("case %s: %s\n", t->name, ok ? "ok" : "FAILED");
		if (!ok) {
			fails++;
			printf("%s", t->st.out);
		}
	}
	printf("RESULT: %s bt_app_host_test cases=%u\n", fails ? "FAIL" : "PASS", ncases + 1);
	return fails != 0;
}
