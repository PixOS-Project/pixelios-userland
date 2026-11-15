/* Host test for the bt-client core (T-86aa, hardened T-f9ab): the scanning sequence, the counters and EVERY log line, with
 * stubbed CoreBluetooth ops. Lines are compared whole (a substring such as "discovered=1" also matches the t=2s line).
 * Privacy: every output must match neither a MAC-shaped nor a UUID-shaped regex; the stubs hand out only counters.
 * Mutation check: bt-client/mutant-test.sh applies the 5 core mutants of docs/reviews/T-86aa-review.md finding 6 and
 * expects this test to FAIL on each. */
#include "bt_client.h"

#include <regex.h>
#include <stdio.h>
#include <string.h>

#define NW 12

struct st {
	long state;
	int scanning;
	unsigned discovered, unique, gaps;
	unsigned powered_at_t;   /* the sample index at which the central becomes poweredOn (0: at start() if reach_power_on) */
	unsigned discovers_at_t; /* didDiscover calls per sample once poweredOn */
	unsigned unique_at_t;    /* new unique peripherals per sample once poweredOn (< discovers_at_t: duplicate advertisements) */
	unsigned backend_gaps;   /* what the backend reports in btc_sample.gaps */
	int start_rc;
	const char *central_options; /* T-18e5/T-2ae7: what the backend reports in btc_ops.central_options ("key=value" or 0) */
	unsigned stop_scan_at;   /* T-2ae7: from this sample on the scan is lost (scanning=0, no discoveries) until rescan() */
	int rescan_mode;         /* T-2ae7: 0 = rescan works, 1 = rescan fails (-1), 2 = queue re-check skips it (0) */
	int scan_lost, rescans;
	unsigned armb_at;        /* T-2ae7: btc_run armb_at_s (0 = no arm B) */
	int armb_mode;           /* T-2ae7: 0 = arm B scan issued (n = rescans+1), 1 = dictionary not built (-1), 2 = not poweredOn (0) */
	int armb_only;           /* T-2ae7: discoveries only once arm B is in force (the hypothesis the arm tests) */
	int armb_on, armb_calls;
	unsigned armc_at;        /* T-27d4: btc_run armc_at_s (0 = no arm C) */
	int armc_mode;           /* T-27d4: as armb_mode, for arm C */
	int armc_only;           /* T-27d4: discoveries only while arm C is in force (arm B replaces it) */
	int armc_on, armc_calls;
	int reach_power_on;
	unsigned samples;
	char out[8192];
	size_t outlen;
};

static int s_start(void *c)
{
	struct st *s = c;

	if (s->start_rc == 0 && s->powered_at_t == 0 && s->reach_power_on)
		s->state = BTC_STATE_POWERED_ON;
	return s->start_rc;
}

static const char *s_central_options(void *c)
{
	struct st *s = c;

	return s->central_options;
}
static int s_rescan(void *c)
{
	struct st *s = c;

	if (s->rescan_mode == 1)
		return -1;
	if (s->rescan_mode == 2)
		return 0;
	s->scan_lost = 0;
	return ++s->rescans;
}

static int s_arm_b(void *c, char *desc, unsigned long len)
{
	struct st *s = c;

	s->armb_calls++;
	if (s->armb_mode == 1)
		return -1;
	snprintf(desc, len, "kCBTestDaemon=1");
	s->armb_on = 1;
	s->armc_on = 0;	/* arm B's scan replaces arm C's services filter */
	if (s->armb_mode == 2)
		return 0;
	return ++s->rescans;
}

static int s_arm_c(void *c, char *desc, unsigned long len)
{
	struct st *s = c;

	s->armc_calls++;
	if (s->armc_mode == 1)
		return -1;
	snprintf(desc, len, "FD50");
	s->armc_on = 1;
	if (s->armc_mode == 2)
		return 0;
	return ++s->rescans;
}

static void s_sample(void *c, struct btc_sample *o)
{
	struct st *s = c;

	s->samples++;
	if (s->samples >= s->powered_at_t && s->powered_at_t > 0)
		s->state = BTC_STATE_POWERED_ON;
	if (s->stop_scan_at > 0 && s->samples == s->stop_scan_at)
		s->scan_lost = 1;
	if (s->state == BTC_STATE_POWERED_ON)
		s->scanning = !s->scan_lost;
	if (s->state == BTC_STATE_POWERED_ON && s->scanning && (!s->armb_only || s->armb_on) && (!s->armc_only || s->armc_on)) {
		s->discovered += s->discovers_at_t;
		s->unique += s->unique_at_t;
	}
	o->state = s->state;
	o->scanning = s->scanning;
	o->discovered = s->discovered;
	o->unique = s->unique;
	o->gaps = s->backend_gaps;
}

static void s_sleep(void *c, unsigned secs)
{
	(void)c;
	(void)secs;
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

/* is `line` (without newline) exactly one of the output lines? */
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

struct tc {
	const char *name;
	struct st st;
	int want_rc;
	const char *want[NW];    /* whole lines that must appear */
	const char *absent[4];   /* substrings that must NOT appear */
};

static const char *const DENY_MAC = "[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}";
static const char *const DENY_UUID = "[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}";

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

/* ---- bt-client adv (bt-proper): the state machine and the result decision with a scripted peripheral manager ---- */
struct ast {
	unsigned now;		/* virtual seconds, advanced by sleep_s */
	unsigned slept_total;
	int start_rc, add_rc, adv_rc;
	long state_after;	/* state once now >= power_at (else 0 = unknown) */
	unsigned power_at;
	int svc_mode;		/* 1 = didAddService ok, -1 error, 0 never answers */
	int adv_mode;		/* same for didStartAdvertising */
	long err;		/* code behind a -1 */
	unsigned lose_at;	/* isAdvertising drops to NO when now >= lose_at (0 = never) */
	unsigned reads_per_s;
	unsigned backend_gaps;
	int added, advertised, stops;
	unsigned adv_t0;
	char out[8192];
	size_t outlen;
};

static int a_start(void *c) { return ((struct ast *)c)->start_rc; }
static int a_add(void *c) { struct ast *a = c; a->added = a->add_rc == 0; return a->add_rc; }
static int a_adv(void *c) { struct ast *a = c; a->advertised = a->adv_rc == 0; a->adv_t0 = a->now; return a->adv_rc; }
static void a_sleep(void *c, unsigned s)
{
	struct ast *a = c;

	a->now += s;
	a->slept_total += s;
}
static void a_sample(void *c, struct btadv_sample *o)
{
	struct ast *a = c;

	memset(o, 0, sizeof *o);
	o->state = a->now >= a->power_at ? a->state_after : 0;
	o->svc = a->added ? a->svc_mode : 0;
	o->adv = a->advertised ? a->adv_mode : 0;
	o->is_adv = a->advertised && a->adv_mode == 1 && !a->stops && !(a->lose_at && a->now >= a->lose_at);
	o->err = (o->adv < 0 || o->svc < 0) ? a->err : 0;
	if (o->err)
		snprintf(o->err_domain, sizeof o->err_domain, "CBErrorDomain");
	/* reads: reads_per_s for every virtual second since startAdvertising was accepted */
	o->reads = a->advertised && a->adv_mode == 1 ? a->reads_per_s * (a->now - a->adv_t0) : 0;
	o->centrals = o->reads ? 1 : 0;
	o->gaps = a->backend_gaps;
}
static void a_stop(void *c) { ((struct ast *)c)->stops++; }
static void a_out(void *c, const char *line)
{
	struct ast *a = c;
	size_t n = strlen(line);

	if (a->outlen + n + 2 < sizeof a->out) {
		memcpy(a->out + a->outlen, line, n);
		a->outlen += n;
		a->out[a->outlen++] = '\n';
		a->out[a->outlen] = 0;
	}
}

struct atc {
	const char *name;
	struct ast st;
	unsigned secs, delay;
	int want_rc;
	int want_stops;
	const char *want[8];
	const char *absent[3];
};

static void adv_cases(unsigned *fails, unsigned *ncases)
{
	struct atc cases[] = {
	    { "adv_pass", { .state_after = 5, .power_at = 2, .svc_mode = 1, .adv_mode = 1, .reads_per_s = 0 },
	      3, 0, 0, 1,
	      { "BT ADV: start build=hosttest seconds=3 delay=0s", "BT ADV: poweredOn after 2s", "BT ADV: didAddService ok", "BT ADV: didStartAdvertising ok",
	        "BT ADV: stopped", "BT ADV: RESULT: PASS adv started=1 err=0 seconds=3 reads=0 centrals=0 gaps=0", 0 },
	      { "GAP", "FAIL", 0 } },
	    { "adv_pass_with_reads_and_backend_gap", { .state_after = 5, .svc_mode = 1, .adv_mode = 1, .reads_per_s = 2, .backend_gaps = 1 },
	      3, 0, 0, 1,
	      { "BT ADV: RESULT: PASS adv started=1 err=0 seconds=3 reads=6 centrals=1 gaps=1", 0 },
	      { "FAIL", 0 } },
	    { "adv_delay_first", { .state_after = 5, .svc_mode = 1, .adv_mode = 1 },
	      1, 420, 0, 1,
	      { "BT ADV: waiting 420s before the first CoreBluetooth call (after BTApp's checks)", "BT ADV: RESULT: PASS adv started=1 err=0 seconds=1 reads=0 centrals=0 gaps=0", 0 },
	      { 0 } },
	    { "adv_never_powered_on", { .state_after = 4, .power_at = 0, .svc_mode = 1, .adv_mode = 1 },
	      5, 0, 1, 1,
	      { "BT ADV GAP: peripheral manager never reached poweredOn within 30s (state=4)",
	        "BT ADV: RESULT: FAIL adv reason=not-poweredOn started=0 err=0 state=4 seconds=0 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", "didAddService", 0 } },
	    { "adv_start_failed_no_stop", { .start_rc = -1, .state_after = 5, .svc_mode = 1, .adv_mode = 1 },
	      5, 0, 1, 0,
	      { "BT ADV GAP: start failed (no peripheral manager)",
	        "BT ADV: RESULT: FAIL adv reason=start-failed started=0 err=0 state=-1 seconds=0 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", "stopped", 0 } },
	    { "adv_add_service_error", { .state_after = 5, .svc_mode = -1, .adv_mode = 1, .err = 7 },
	      5, 0, 1, 1,
	      { "BT ADV: didAddService error domain=CBErrorDomain code=7",
	        "BT ADV: RESULT: FAIL adv reason=add-service-error started=0 err=7 state=5 seconds=0 reads=0 centrals=0 gaps=0", 0 },
	      { "PASS", "didStartAdvertising", 0 } },
	    { "adv_add_service_silent", { .state_after = 5, .svc_mode = 0, .adv_mode = 1 },
	      5, 0, 1, 1,
	      { "BT ADV GAP: no didAddService within 10s",
	        "BT ADV: RESULT: FAIL adv reason=add-service-timeout started=0 err=0 state=5 seconds=0 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", 0 } },
	    { "adv_add_call_refused", { .add_rc = -1, .state_after = 5, .svc_mode = 1, .adv_mode = 1 },
	      5, 0, 1, 1,
	      { "BT ADV GAP: addService could not be issued",
	        "BT ADV: RESULT: FAIL adv reason=add-service-call started=0 err=0 state=5 seconds=0 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", 0 } },
	    { "adv_start_error", { .state_after = 5, .svc_mode = 1, .adv_mode = -1, .err = 11 },
	      5, 0, 1, 1,
	      { "BT ADV: didStartAdvertising error domain=CBErrorDomain code=11",
	        "BT ADV: RESULT: FAIL adv reason=start-error started=0 err=11 state=5 seconds=0 reads=0 centrals=0 gaps=0", 0 },
	      { "PASS", "didStartAdvertising ok", 0 } },
	    { "adv_start_silent", { .state_after = 5, .svc_mode = 1, .adv_mode = 0 },
	      5, 0, 1, 1,
	      { "BT ADV GAP: no didStartAdvertising within 10s",
	        "BT ADV: RESULT: FAIL adv reason=start-timeout started=0 err=0 state=5 seconds=0 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", 0 } },
	    { "adv_advertise_call_refused", { .adv_rc = -1, .state_after = 5, .svc_mode = 1, .adv_mode = 1 },
	      5, 0, 1, 1,
	      { "BT ADV GAP: startAdvertising could not be issued",
	        "BT ADV: RESULT: FAIL adv reason=advertise-call started=0 err=0 state=5 seconds=0 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", 0 } },
	    { "adv_dropped_mid_window", { .state_after = 5, .svc_mode = 1, .adv_mode = 1, .lose_at = 2 },
	      4, 0, 1, 1,
	      { "BT ADV GAP: isAdvertising dropped to NO at t=2s without stopAdvertising",
	        "BT ADV: RESULT: FAIL adv reason=advertising-stopped started=1 err=0 state=5 seconds=4 reads=0 centrals=0 gaps=1", 0 },
	      { "PASS", 0 } },
	};
	unsigned n = sizeof cases / sizeof cases[0];

	for (unsigned i = 0; i < n; i++) {
		struct atc *t = &cases[i];
		struct btadv_ops ops = { &t->st, a_start, a_add, a_adv, a_sample, a_stop, a_sleep, a_out };
		int rc = btadv_run(&ops, t->secs, t->delay, "hosttest"), ok = 1;

		if (rc != t->want_rc) {
			printf("case %s: rc=%d want %d\n", t->name, rc, t->want_rc);
			ok = 0;
		}
		if (t->st.stops != t->want_stops) {
			printf("case %s: stop called %d times, want %d (teardown must run exactly once after a successful start)\n", t->name, t->st.stops,
			    t->want_stops);
			ok = 0;
		}
		if (t->delay && t->st.slept_total < t->delay) {
			printf("case %s: delay not slept (%u < %u)\n", t->name, t->st.slept_total, t->delay);
			ok = 0;
		}
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
		/* exactly one RESULT line, and it is the last line */
		{
			const char *p;
			int count = 0;
			size_t len = strlen(t->st.out);
			char tmp[sizeof t->st.out];

			for (p = t->st.out; (p = strstr(p, "BT ADV: RESULT: ")) != 0; p++)
				count++;
			memcpy(tmp, t->st.out, len + 1);
			if (len)
				tmp[len - 1] = 0;	/* drop the final newline */
			p = strrchr(tmp, '\n');
			p = p ? p + 1 : tmp;
			if (count != 1 || strncmp(p, "BT ADV: RESULT: ", 16) != 0) {
				printf("case %s: want exactly one RESULT line, as the last line (count %d)\n", t->name, count);
				ok = 0;
			}
		}
		if (rx_match(DENY_MAC, t->st.out) != 0 || rx_match(DENY_UUID, t->st.out) != 0) {
			printf("case %s: output contains a MAC- or UUID-shaped string\n", t->name);
			ok = 0;
		}
		printf("case %s: %s\n", t->name, ok ? "ok" : "FAILED");
		if (!ok) {
			(*fails)++;
			printf("%s", t->st.out);
		}
	}
	*ncases += n;
	{	/* argument parser table (the job passes: adv 90 --delay 420) */
		static const struct {
			const char *argv[4];
			int argc, rc;
			unsigned secs, delay;
		} p[] = {
		    { { 0 }, 0, 0, 60, 0 },
		    { { "90", "--delay", "420" }, 3, 0, 90, 420 },
		    { { "--delay", "5", "7" }, 3, 0, 7, 5 },
		    { { "30" }, 1, 0, 30, 0 },
		    { { "0" }, 1, -1, 0, 0 },
		    { { "abc" }, 1, -1, 0, 0 },
		    { { "--delay" }, 1, -1, 0, 0 },
		    { { "--delay", "x" }, 2, -1, 0, 0 },
		    { { "--delay", "3601" }, 2, -1, 0, 0 },
		    { { "10", "20" }, 2, -1, 0, 0 },
		    { { "86401" }, 1, -1, 0, 0 },
		    { { "-5" }, 1, -1, 0, 0 },
		};
		int ok = 1;

		for (unsigned k = 0; k < sizeof p / sizeof p[0]; k++) {
			char *av[4];
			unsigned secs = 99, delay = 99;
			char why[96] = "";
			int rc;

			for (int j = 0; j < p[k].argc; j++)
				av[j] = (char *)p[k].argv[j];
			rc = btadv_parse(p[k].argc, av, &secs, &delay, why, sizeof why);
			if (rc != p[k].rc || (rc == 0 && (secs != p[k].secs || delay != p[k].delay)) || (rc != 0 && why[0] == 0)) {
				printf("case adv_parse #%u: rc=%d secs=%u delay=%u why=\"%s\"\n", k, rc, secs, delay, why);
				ok = 0;
			}
		}
		printf("case adv_parse table: %s\n", ok ? "ok" : "FAILED");
		*fails += !ok;
		(*ncases)++;
	}
}

/* ---- bt-client tone (bt-proper) ---- */
struct tst {
	int session_rc, start_rc;
	unsigned now, refills_per_s;	/* refills_per_s: 0 -> 10 */
	unsigned stall_at;	/* from this virtual second on the refill counter stops (0 = never) */
	int hiccups;	/* every even virtual second the counter pauses once (isolated stalls must not add up) */
	unsigned qerr_at;	/* from this virtual second on qerr = qerr_code (0 = never) */
	long qerr_code;
	unsigned backend_gaps;
	const char *route_before, *route_after;
	int started, stops, route_calls, session_calls, sleeps_before_session;
	unsigned started_at;
	char out[8192];
	size_t outlen;
};

static int t_session(void *c) { struct tst *t = c; t->session_calls++; return t->session_rc; }
static void t_route(void *c, char *buf, unsigned long len)
{
	struct tst *t = c;

	snprintf(buf, len, "%s", t->route_calls++ == 0 ? t->route_before : t->route_after);
}
static int t_start(void *c) { struct tst *t = c; t->started = t->start_rc == 0; t->started_at = t->now; return t->start_rc; }
static void t_sample(void *c, struct bttone_sample *o)
{
	struct tst *t = c;
	unsigned el = t->started ? t->now - t->started_at : 0, per = t->refills_per_s ? t->refills_per_s : 10;
	unsigned live = el;

	if (t->stall_at && el >= t->stall_at)
		live = t->stall_at - 1;
	if (t->hiccups)
		live = el - el / 2;
	memset(o, 0, sizeof *o);
	o->refills = live * per;
	o->qerr = (t->qerr_at && el >= t->qerr_at) ? t->qerr_code : 0;
	o->gaps = t->backend_gaps;
}
static void t_stop(void *c) { ((struct tst *)c)->stops++; }
static void t_sleep(void *c, unsigned s)
{
	struct tst *t = c;

	if (t->session_calls == 0)
		t->sleeps_before_session += (int)s;
	t->now += s;
}
static void t_out(void *c, const char *line)
{
	struct tst *t = c;
	size_t n = strlen(line);

	if (t->outlen + n + 2 < sizeof t->out) {
		memcpy(t->out + t->outlen, line, n);
		t->outlen += n;
		t->out[t->outlen++] = '\n';
		t->out[t->outlen] = 0;
	}
}

struct ttc {
	const char *name;
	struct tst st;
	unsigned secs, delay;
	int want_rc, want_stops;
	const char *want[8];
	const char *absent[3];
};

static int tone_sine_checks(void)
{
	static const struct { unsigned n; short v; } exp[] = { { 0, 0 }, { 1, 516 }, { 25, 8230 }, { 50, 59 }, { 75, -8230 } };
	short a[2 * 200], b[2 * 200], c[2 * 200];
	unsigned pos = 0, p2 = 0, k, i, ok = 1, peak = 0, wrapped;
	static short one[2 * 44100];

	bttone_fill(a, 200, &pos);
	for (k = 0; k < sizeof exp / sizeof exp[0]; k++)
		if (a[2 * exp[k].n] != exp[k].v) {
			printf("case tone_sine: frame %u L=%d want %d\n", exp[k].n, a[2 * exp[k].n], exp[k].v);
			ok = 0;
		}
	for (i = 0; i < 200; i++)
		if (a[2 * i] != a[2 * i + 1]) {
			printf("case tone_sine: frame %u L != R\n", i);
			ok = 0;
		}
	if (pos != 200) {
		printf("case tone_sine: pos %u after 200 frames\n", pos);
		ok = 0;
	}
	bttone_fill(b, 120, &p2);	/* the same 200 frames in two calls must join without a jump */
	bttone_fill(c, 80, &p2);
	if (memcmp(b, a, sizeof b[0] * 240) != 0 || memcmp(c, a + 240, sizeof c[0] * 160) != 0) {
		printf("case tone_sine: split fill differs from one fill\n");
		ok = 0;
	}
	pos = 0;
	bttone_fill(one, 44100, &pos);
	for (i = 0; i < 2 * 44100; i++) {
		unsigned m = one[i] < 0 ? (unsigned)-one[i] : (unsigned)one[i];

		if (m > peak)
			peak = m;
	}
	if (peak != BTTONE_AMP) {
		printf("case tone_sine: peak %u want %d\n", peak, BTTONE_AMP);
		ok = 0;
	}
	if (pos != 0) {
		printf("case tone_sine: counter %u after one second (want wrap to 0)\n", pos);
		ok = 0;
	}
	pos = 44099;	/* wrap: frame 44099 is the sample just before the start, -516 */
	bttone_fill(a, 2, &pos);
	wrapped = pos;
	if (a[0] != -516 || a[2] != 0 || wrapped != 1) {
		printf("case tone_sine: wrap L=%d,%d pos=%u want -516,0 pos 1\n", a[0], a[2], wrapped);
		ok = 0;
	}
	return ok;
}

static void tone_cases(unsigned *fails, unsigned *ncases)
{
	struct ttc cases[] = {
	    { "tone_pass", { .route_before = "Speaker", .route_after = "BluetoothA2DPOutput" },
	      3, 0, 0, 1,
	      { "BT TONE: start build=hosttest seconds=3 delay=0s", "BT TONE: route before=Speaker", "BT TONE: queue running (3 buffers of 4096 frames, 44100 Hz, 440 Hz tone)",
	        "BT TONE: route after=BluetoothA2DPOutput", "BT TONE: stopped", "BT TONE: RESULT: PASS tone played_s=3 route=BluetoothA2DPOutput gaps=0", 0 },
	      { "GAP", "FAIL", 0 } },
	    { "tone_isolated_stalls_pass", { .hiccups = 1, .refills_per_s = 20, .route_before = "none", .route_after = "none" },
	      8, 0, 0, 1,
	      { "BT TONE: RESULT: PASS tone played_s=8 route=none gaps=0", 0 },
	      { "GAP", 0 } },
	    { "tone_pass_speaker_route_reported", { .route_before = "none", .route_after = "Speaker", .backend_gaps = 2 },
	      2, 0, 0, 1,
	      { "BT TONE: route before=none", "BT TONE: RESULT: PASS tone played_s=2 route=Speaker gaps=2", 0 },
	      { "FAIL", 0 } },
	    { "tone_delay_first", { .route_before = "none", .route_after = "none" },
	      1, 540, 0, 1,
	      { "BT TONE: waiting 540s before the first audio call (after BTApp's checks)", "BT TONE: RESULT: PASS tone played_s=1 route=none gaps=0", 0 },
	      { 0 } },
	    { "tone_session_failed", { .session_rc = -1, .route_before = "none", .route_after = "none" },
	      5, 0, 1, 1,
	      { "BT TONE GAP: audio session could not be set to Playback and activated",
	        "BT TONE: RESULT: FAIL reason=session-failed route=none played_s=0 qerr=0 refills=0 gaps=1", 0 },
	      { "PASS", "queue running", 0 } },
	    { "tone_queue_start_failed", { .start_rc = -1, .route_before = "none", .route_after = "none" },
	      5, 0, 1, 1,
	      { "BT TONE GAP: AudioQueue could not be created or started",
	        "BT TONE: RESULT: FAIL reason=queue-start route=none played_s=0 qerr=0 refills=0 gaps=1", 0 },
	      { "PASS", "queue running", 0 } },
	    { "tone_queue_error_mid_run", { .qerr_at = 3, .qerr_code = -50, .route_before = "none", .route_after = "Speaker" },
	      6, 0, 1, 1,
	      { "BT TONE GAP: AudioQueue error OSStatus=-50 at t=3s",
	        "BT TONE: RESULT: FAIL reason=queue-error route=Speaker played_s=2 qerr=-50 refills=30 gaps=1", 0 },
	      { "PASS", 0 } },
	    { "tone_queue_stalled", { .stall_at = 3, .route_before = "none", .route_after = "none" },
	      10, 0, 1, 1,
	      { "BT TONE GAP: no buffer refill for 3s at t=5s (the queue stopped consuming)",
	        "BT TONE: RESULT: FAIL reason=queue-stalled route=none played_s=4 qerr=0 refills=20 gaps=1", 0 },
	      { "PASS", 0 } },
	    { "tone_too_few_refills", { .refills_per_s = 3, .route_before = "none", .route_after = "none" },
	      4, 0, 1, 1,
	      { "BT TONE GAP: only 12 buffer refills in 4s (expected at least 21)",
	        "BT TONE: RESULT: FAIL reason=too-few-refills route=none played_s=4 qerr=0 refills=12 gaps=1", 0 },
	      { "PASS", 0 } },
	};
	unsigned n = sizeof cases / sizeof cases[0], i;
	unsigned bad;

	for (i = 0; i < n; i++) {
		struct ttc *t = &cases[i];
		struct bttone_ops ops = { &t->st, t_session, t_route, t_start, t_sample, t_stop, t_sleep, t_out };
		int rc = bttone_run(&ops, t->secs, t->delay, "hosttest"), ok = 1;

		if (rc != t->want_rc) {
			printf("case %s: rc=%d want %d\n", t->name, rc, t->want_rc);
			ok = 0;
		}
		if (t->st.stops != t->want_stops) {
			printf("case %s: stop() called %d times, want %d\n", t->name, t->st.stops, t->want_stops);
			ok = 0;
		}
		if (t->delay && t->st.sleeps_before_session != (int)t->delay) {
			printf("case %s: slept %d s before the first audio call, want %u\n", t->name, t->st.sleeps_before_session, t->delay);
			ok = 0;
		}
		for (int k = 0; t->want[k]; k++) {
			char line[300], nl_line[301];

			snprintf(line, sizeof line, "%s\n", t->want[k]);
			snprintf(nl_line, sizeof nl_line, "\n%s", line);
			if (strncmp(t->st.out, line, strlen(line)) != 0 && strstr(t->st.out, nl_line) == 0) {	/* whole line only */
				printf("case %s: missing line \"%s\"\n", t->name, t->want[k]);
				ok = 0;
			}
		}
		for (int k = 0; t->absent[k]; k++)
			if (strstr(t->st.out, t->absent[k])) {
				printf("case %s: forbidden text \"%s\"\n", t->name, t->absent[k]);
				ok = 0;
			}
		if (rx_match(DENY_MAC, t->st.out) != 0 || rx_match(DENY_UUID, t->st.out) != 0) {
			printf("case %s: output contains a MAC- or UUID-shaped string\n", t->name);
			ok = 0;
		}
		printf("case %s: %s\n", t->name, ok ? "ok" : "FAILED");
		if (!ok) {
			(*fails)++;
			printf("%s", t->st.out);
		}
		(*ncases)++;
	}
	bad = !tone_sine_checks();
	printf("case tone sine samples, split fill, peak, wrap: %s\n", bad ? "FAILED" : "ok");
	*fails += bad;
	(*ncases)++;
	{	/* refill floor and the parser */
		char *a1[] = { "60", "--delay", "540" }, *a2[] = { "--delay", "5" }, *a3[] = { "0" }, *a4[] = { "10", "20" }, *a5[] = { "--delay" }, *a6[] = { "3601" };
		unsigned secs, d, ok = 1;
		char why[96];

		ok &= bttone_min_refills(30) == 161 && bttone_min_refills(1) == 5;
		ok &= bttone_parse(3, a1, &secs, &d, why, sizeof why) == 0 && secs == 60 && d == 540;
		ok &= bttone_parse(2, a2, &secs, &d, why, sizeof why) == 0 && secs == BTTONE_DEFAULT_S && d == 5 && secs == 30;
		ok &= bttone_parse(0, a1, &secs, &d, why, sizeof why) == 0 && secs == 30 && d == 0;
		ok &= bttone_parse(1, a3, &secs, &d, why, sizeof why) != 0;
		ok &= bttone_parse(2, a4, &secs, &d, why, sizeof why) != 0;
		ok &= bttone_parse(1, a5, &secs, &d, why, sizeof why) != 0;
		ok &= bttone_parse(1, a6, &secs, &d, why, sizeof why) != 0;
		printf("case tone parser and refill floor: %s\n", ok ? "ok" : "FAILED");
		*fails += !ok;
		(*ncases)++;
	}
}

int main(void)
{
	struct tc cases[] = {
	    { "powered_on_and_discovers",   /* 3 advertisements per sample from 1 peripheral: discovered != unique */
	      { .state = -1, .scanning = -1, .discovers_at_t = 3, .unique_at_t = 1, .reach_power_on = 1, .central_options = "kCBTestKey=0" },
	      0,
	      { "BT CLIENT: start build=hosttest period=1s total=3s central_options=kCBTestKey=0 scan=services:nil,options:nil",
	        "BT CLIENT: t=1s state=5 scanning=1 discovered=3 unique~1 gaps=0",
	        "BT CLIENT: t=2s state=5 scanning=1 discovered=6 unique~2 gaps=0",
	        "BT CLIENT: t=3s state=5 scanning=1 discovered=9 unique~3 gaps=0",
	        "BT CLIENT: RESULT pass discovered=12 unique=4 state=5 gaps=0", 0 },
	      { "GAP", "scan resume", 0 } },
	    { "never_powered_on",
	      { .state = 4, .scanning = 0 },
	      1,
	      { "BT CLIENT: t=1s state=4 scanning=0 discovered=0 unique~0 gaps=0",
	        "BT CLIENT GAP: central never reached poweredOn (state=4)",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=4 gaps=1", 0 },
	      { "start failed", "RESULT pass", "scan resume", 0 } },
	    { "no_gaps_no_options",     /* T-18e5: the knob line reads 0 when the backend built no options dictionary */
	      { .state = -1, .scanning = -1, .reach_power_on = 1 },
	      1,
	      { "BT CLIENT: start build=hosttest period=1s total=3s central_options=none scan=services:nil,options:nil",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=0", 0 },
	      { "GAP", 0 } },
	    { "start_failure",   /* no central: state=-1, exactly ONE gap, and no poweredOn gap on top */
	      { .state = -1, .scanning = -1, .start_rc = -1, .central_options = "kCBTestKey=0" }, /* options never asked without a central */
	      1,
	      { "BT CLIENT: start build=hosttest period=1s total=3s central_options=none(no-manager) scan=services:nil,options:nil",
	        "BT CLIENT GAP: start failed rc=-1 (no central; counts stay 0)",
	        "BT CLIENT: t=1s state=-1 scanning=-1 discovered=0 unique~0 gaps=1",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=-1 gaps=1", 0 },
	      { "never reached poweredOn", "RESULT pass", 0 } },
	    { "late_powered_on_still_counts",
	      { .state = 1, .scanning = 0, .powered_at_t = 2, .discovers_at_t = 1, .unique_at_t = 1 },
	      0,
	      { "BT CLIENT: t=1s state=1 scanning=0 discovered=0 unique~0 gaps=0",
	        "BT CLIENT: t=2s state=5 scanning=1 discovered=1 unique~1 gaps=0",
	        "BT CLIENT: RESULT pass discovered=3 unique=3 state=5 gaps=0", 0 },
	      { "GAP", 0 } },
	    { "backend_gaps_are_totalled",   /* a backend gap does not fail the run but must reach the summary */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .backend_gaps = 2, .reach_power_on = 1 },
	      0,
	      { "BT CLIENT: t=1s state=5 scanning=1 discovered=1 unique~1 gaps=2",
	        "BT CLIENT: RESULT pass discovered=4 unique=4 state=5 gaps=2", 0 },
	      { "never reached poweredOn", 0 } },
	    { "start_failure_plus_backend_gaps",
	      { .state = -1, .scanning = -1, .start_rc = -1, .backend_gaps = 2 },
	      1,
	      { "BT CLIENT: t=1s state=-1 scanning=-1 discovered=0 unique~0 gaps=3",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=-1 gaps=3", 0 },
	      { "never reached poweredOn", 0 } },
	    { "discovered_without_unique_fails",   /* advertisements heard but no identifier reduced: the run must NOT pass */
	      { .state = -1, .scanning = -1, .discovers_at_t = 2, .unique_at_t = 0, .reach_power_on = 1 },
	      1,
	      { "BT CLIENT: RESULT fail discovered=8 unique=0 state=5 gaps=0", 0 },
	      { "RESULT pass", 0 } },
	    { "scan_lost_is_resumed",   /* T-2ae7: poweredOn but isScanning == NO at t=2s: the scan is issued again once */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .reach_power_on = 1, .stop_scan_at = 2 },
	      0,
	      { "BT CLIENT: t=1s state=5 scanning=1 discovered=1 unique~1 gaps=0",
	        "BT CLIENT: t=2s state=5 scanning=0 discovered=1 unique~1 gaps=0",
	        "BT CLIENT: scan resumed reason=poweredOn-not-scanning n=1",
	        "BT CLIENT: t=3s state=5 scanning=1 discovered=2 unique~2 gaps=0",
	        "BT CLIENT: RESULT pass discovered=3 unique=3 state=5 gaps=0", 0 },
	      { "GAP", "skipped", 0 } },
	    { "scan_resume_fails",      /* T-2ae7: each failed resume is one GAP and reaches the summary */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .reach_power_on = 1, .stop_scan_at = 2, .rescan_mode = 1 },
	      0,
	      { "BT CLIENT GAP: scan resume failed rc=-1 (poweredOn, not scanning, no scan issued)",
	        "BT CLIENT: t=3s state=5 scanning=0 discovered=1 unique~1 gaps=1",
	        "BT CLIENT: RESULT pass discovered=1 unique=1 state=5 gaps=2", 0 },
	      { "scan resumed", 0 } },
	    { "scan_resume_skipped",    /* T-2ae7: the queue re-check issued nothing: said so, not called a resume, not a gap */
	      { .state = -1, .scanning = -1, .reach_power_on = 1, .stop_scan_at = 1, .rescan_mode = 2 },
	      1,
	      { "BT CLIENT: scan resume skipped reason=queue-recheck-scanning-or-not-poweredOn",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=0", 0 },
	      { "scan resumed", "GAP", 0 } },
	    { "arm_b_switch_splits_counts",   /* T-2ae7: arm B after the t=2s sample; ARM rows split the totals there */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .reach_power_on = 1, .armb_at = 2 },
	      0,
	      { "BT CLIENT: start build=hosttest period=1s total=3s central_options=none scan=services:nil,options:nil armB_at=2s",
	        "BT CLIENT: arm B start t=2s scan=services:nil,options:kCBTestDaemon=1 n=1 armA_discovered=2 armA_unique=2",
	        "BT CLIENT: ARM A options:nil discovered=2 unique=2",
	        "BT CLIENT: ARM B options:kCBTestDaemon=1 discovered=2 unique_new=2",
	        "BT CLIENT: RESULT pass discovered=4 unique=4 state=5 gaps=0", 0 },
	      { "GAP", "scan resume", 0 } },
	    { "arm_b_only_discovers",         /* T-2ae7: the arm's hypothesis: arm A 0, arm B > 0 */
	      { .state = -1, .scanning = -1, .discovers_at_t = 2, .unique_at_t = 1, .reach_power_on = 1, .armb_at = 1, .armb_only = 1 },
	      0,
	      { "BT CLIENT: t=1s state=5 scanning=1 discovered=0 unique~0 gaps=0",
	        "BT CLIENT: arm B start t=1s scan=services:nil,options:kCBTestDaemon=1 n=1 armA_discovered=0 armA_unique=0",
	        "BT CLIENT: ARM A options:nil discovered=0 unique=0",
	        "BT CLIENT: ARM B options:kCBTestDaemon=1 discovered=6 unique_new=3",
	        "BT CLIENT: RESULT pass discovered=6 unique=3 state=5 gaps=0", 0 },
	      { "GAP", 0 } },
	    { "arm_b_not_built",              /* T-2ae7: no dictionary: one GAP, counted, arm B named not-started */
	      { .state = -1, .scanning = -1, .reach_power_on = 1, .armb_at = 2, .armb_mode = 1 },
	      1,
	      { "BT CLIENT GAP: arm B options not built rc=-1 (scan stays services:nil,options:nil)",
	        "BT CLIENT: t=3s state=5 scanning=1 discovered=0 unique~0 gaps=1",
	        "BT CLIENT: ARM A options:nil discovered=0 unique=0",
	        "BT CLIENT: ARM B options:none(not-started) discovered=0 unique_new=0",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=1", 0 },
	      { "arm B start", 0 } },
	    { "arm_b_not_powered_on",         /* T-2ae7: options kept for the next poweredOn scan; said so, not a gap */
	      { .state = 1, .scanning = 0, .powered_at_t = 3, .armb_at = 2, .armb_mode = 2 },
	      1,
	      { "BT CLIENT: arm B start t=2s scan=services:nil,options:kCBTestDaemon=1 n=0(on-next-poweredOn) armA_discovered=0 armA_unique=0",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=0", 0 },
	      { "GAP", 0 } },
	    { "arm_b_needs_a_central",        /* T-2ae7: start failed: arm B is never asked, no ARM rows */
	      { .state = -1, .scanning = -1, .start_rc = -1, .armb_at = 2 },
	      1,
	      { "BT CLIENT: RESULT fail discovered=0 unique=0 state=-1 gaps=1", 0 },
	      { "arm B", "ARM ", 0 } },
	    { "arm_c_then_b_splits_counts",   /* T-27d4: A t=1, C t=2, B t=3+final; arm C ends where arm B starts */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .reach_power_on = 1, .armc_at = 1, .armb_at = 2 },
	      0,
	      { "BT CLIENT: start build=hosttest period=1s total=3s central_options=none scan=services:nil,options:nil armC_at=1s armB_at=2s",
	        "BT CLIENT: arm C start t=1s scan=services:FD50,options:nil n=1 armA_discovered=1 armA_unique=1",
	        "BT CLIENT: arm B start t=2s scan=services:nil,options:kCBTestDaemon=1 n=2 armA_discovered=1 armA_unique=1",
	        "BT CLIENT: ARM A options:nil discovered=1 unique=1",
	        "BT CLIENT: ARM C services:FD50 discovered=1 unique_new=1",
	        "BT CLIENT: ARM B options:kCBTestDaemon=1 discovered=2 unique_new=2",
	        "BT CLIENT: RESULT pass discovered=4 unique=4 state=5 gaps=0", 0 },
	      { "GAP", "scan resume", 0 } },
	    { "arm_c_only_discovers",         /* T-27d4: the arm's hypothesis: arm A 0, arm C > 0; no arm B rows without arm B */
	      { .state = -1, .scanning = -1, .discovers_at_t = 2, .unique_at_t = 1, .reach_power_on = 1, .armc_at = 1, .armc_only = 1 },
	      0,
	      { "BT CLIENT: t=1s state=5 scanning=1 discovered=0 unique~0 gaps=0",
	        "BT CLIENT: arm C start t=1s scan=services:FD50,options:nil n=1 armA_discovered=0 armA_unique=0",
	        "BT CLIENT: ARM A options:nil discovered=0 unique=0",
	        "BT CLIENT: ARM C services:FD50 discovered=6 unique_new=3",
	        "BT CLIENT: RESULT pass discovered=6 unique=3 state=5 gaps=0", 0 },
	      { "GAP", "ARM B", "arm B", 0 } },
	    { "arm_c_not_built",              /* T-27d4: no services array: one GAP, counted, arm C named not-started */
	      { .state = -1, .scanning = -1, .reach_power_on = 1, .armc_at = 2, .armc_mode = 1 },
	      1,
	      { "BT CLIENT GAP: arm C services not built rc=-1 (scan stays services:nil,options:nil)",
	        "BT CLIENT: t=3s state=5 scanning=1 discovered=0 unique~0 gaps=1",
	        "BT CLIENT: ARM A options:nil discovered=0 unique=0",
	        "BT CLIENT: ARM C services:none(not-started) discovered=0 unique_new=0",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=1", 0 },
	      { "arm C start", "ARM B", 0 } },
	    { "arm_c_not_powered_on",         /* T-27d4: services kept for the next poweredOn scan; said so, not a gap */
	      { .state = 1, .scanning = 0, .powered_at_t = 3, .armc_at = 2, .armc_mode = 2 },
	      1,
	      { "BT CLIENT: arm C start t=2s scan=services:FD50,options:nil n=0(on-next-poweredOn) armA_discovered=0 armA_unique=0",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=0", 0 },
	      { "GAP", 0 } },
	    { "arm_c_after_b_refused",        /* T-27d4: arm C at/after arm B is refused with one gap, never called */
	      { .state = -1, .scanning = -1, .reach_power_on = 1, .armc_at = 2, .armb_at = 1 },
	      1,
	      { "BT CLIENT GAP: arm C at 2s not before arm B at 1s (refused: arm B fills the duplicate map first)",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=1", 0 },
	      { "arm C start", "ARM C", 0 } },
	    { "arm_c_same_time_as_b_refused", /* T-27d4 review finding 3: equal switch times are refused too */
	      { .state = -1, .scanning = -1, .reach_power_on = 1, .armc_at = 2, .armb_at = 2 },
	      1,
	      { "BT CLIENT GAP: arm C at 2s not before arm B at 2s (refused: arm B fills the duplicate map first)",
	        "BT CLIENT: RESULT fail discovered=0 unique=0 state=5 gaps=1", 0 },
	      { "arm C start", "ARM C", 0 } },
	    { "arm_c_ok_then_b_not_built",    /* T-27d4 review finding 2: the gap names arm C's filter, which stays in force */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .reach_power_on = 1, .armc_at = 1, .armb_at = 2, .armb_mode = 1 },
	      0,
	      { "BT CLIENT GAP: arm B options not built rc=-1 (scan stays services:FD50,options:nil)",
	        "BT CLIENT: ARM A options:nil discovered=1 unique=1",
	        "BT CLIENT: ARM C services:FD50 discovered=1 unique_new=1",
	        "BT CLIENT: ARM B options:none(not-started) discovered=2 unique_new=2",
	        "BT CLIENT: RESULT pass discovered=4 unique=4 state=5 gaps=1", 0 },
	      { "arm B start", 0 } },
	    { "arm_c_not_built_then_b",       /* T-27d4: arm C failed, arm B still runs and counts from its own switch */
	      { .state = -1, .scanning = -1, .discovers_at_t = 1, .unique_at_t = 1, .reach_power_on = 1, .armc_at = 1, .armb_at = 2, .armc_mode = 1 },
	      0,
	      { "BT CLIENT GAP: arm C services not built rc=-1 (scan stays services:nil,options:nil)",
	        "BT CLIENT: ARM A options:nil discovered=1 unique=1",
	        "BT CLIENT: ARM C services:none(not-started) discovered=1 unique_new=1",
	        "BT CLIENT: ARM B options:kCBTestDaemon=1 discovered=2 unique_new=2",
	        "BT CLIENT: RESULT pass discovered=4 unique=4 state=5 gaps=1", 0 },
	      { "arm C start", 0 } },
	    { "arm_c_needs_a_central",        /* T-27d4: start failed: arm C is never asked, no ARM rows */
	      { .state = -1, .scanning = -1, .start_rc = -1, .armc_at = 2 },
	      1,
	      { "BT CLIENT: RESULT fail discovered=0 unique=0 state=-1 gaps=1", 0 },
	      { "arm C", "ARM ", 0 } },
	};
	unsigned fails = 0, ncases = sizeof cases / sizeof cases[0];
	/* the privacy regexes themselves must work, or the check below proves nothing */
	int r1 = rx_match(DENY_MAC, "x aa:bb:cc:dd:ee:ff y"), r2 = rx_match(DENY_MAC, "t=1s state=5 gaps=0"),
	    r3 = rx_match(DENY_UUID, "0123ABCD-0123-4567-89ab-0123456789AB"), r4 = rx_match(DENY_UUID, "unique~3 gaps=0");

	if (r1 != 1 || r2 != 0 || r3 != 1 || r4 != 0) {
		printf("deny regex self-check failed (%d %d %d %d)\n", r1, r2, r3, r4);
		fails++;
	}
	for (unsigned i = 0; i < ncases; i++) {
		struct tc *t = &cases[i];
		struct btc_ops ops = { &t->st, s_start, s_central_options, s_rescan, s_arm_b, s_arm_c, s_sample, s_sleep, s_out };
		int rc = btc_run(&ops, 1, 3, t->st.armb_at, t->st.armc_at, "hosttest");
		int ok = rc == t->want_rc;

		if (t->st.start_rc != 0 && t->st.armb_calls != 0) {
			printf("case %s: arm_b called without a central\n", t->name);
			ok = 0;
		}
		if ((t->st.start_rc != 0 || (t->st.armb_at && t->st.armc_at >= t->st.armb_at)) && t->st.armc_calls != 0) {
			printf("case %s: arm_c called without a central or after arm B\n", t->name);
			ok = 0;
		}

		if (rc != t->want_rc)
			printf("case %s: rc=%d want %d\n", t->name, rc, t->want_rc);
		for (int k = 0; t->want[k]; k++) {
			if (!has_line(t->st.out, t->want[k])) {
				printf("case %s: missing line \"%s\"\n", t->name, t->want[k]);
				ok = 0;
			}
		}
		for (int k = 0; t->absent[k]; k++) {
			if (strstr(t->st.out, t->absent[k])) {
				printf("case %s: forbidden text \"%s\"\n", t->name, t->absent[k]);
				ok = 0;
			}
		}
		if (rx_match(DENY_MAC, t->st.out) != 0 || rx_match(DENY_UUID, t->st.out) != 0) {
			printf("case %s: output contains a MAC- or UUID-shaped string\n", t->name);
			ok = 0;
		}
		printf("case %s: %s\n", t->name, ok ? "ok" : "FAILED");
		if (!ok) {
			fails++;
			printf("%s", t->st.out);
		}
	}
	{	/* case 8 (T-0936): scan on every transition into poweredOn, never twice in a row (bt-up17 states 0 -> 5 -> 1 -> 5) */
		static const long seq[] = { 0, 5, 5, 1, 5, 4, 4, 5, 3, 5 };
		static const int want[] = { 0, 1, 0, 0, 1, 0, 0, 1, 0, 1 };
		int ok = 1, n = 0;
		long prev = 0;

		for (unsigned k = 0; k < sizeof seq / sizeof seq[0]; k++) {
			int due = btc_scan_due(prev, seq[k]);

			if (due != want[k]) {
				printf("case rescan: step %u state %ld -> %ld due=%d want %d\n", k, prev, seq[k], due, want[k]);
				ok = 0;
			}
			n += due;
			prev = seq[k];
		}
		if (n != 4)
			ok = 0;
		printf("case rescan on each poweredOn: %s\n", ok ? "ok" : "FAILED");
		fails += !ok;
		ncases++;
	}
	{	/* case 12 (T-2ae7): rescan only for poweredOn + not scanning; -1 (no central) and every other state never */
		static const long st[] = { 5, 5, 4, -1, 5, 0, 1 };
		static const int sc[] = { 0, 1, 0, -1, -1, 0, 0 };
		static const int want[] = { 1, 0, 0, 0, 0, 0, 0 };
		int ok = 1;

		for (unsigned k = 0; k < sizeof st / sizeof st[0]; k++) {
			if (btc_rescan_due(st[k], sc[k]) != want[k]) {
				printf("case rescan_due: state %ld scanning %d due=%d want %d\n", st[k], sc[k], btc_rescan_due(st[k], sc[k]), want[k]);
				ok = 0;
			}
		}
		printf("case rescan_due truth table: %s\n", ok ? "ok" : "FAILED");
		fails += !ok;
		ncases++;
	}
	adv_cases(&fails, &ncases);
	tone_cases(&fails, &ncases);
	printf("RESULT: %s bt_client_host_test cases=%u\n", fails ? "FAIL" : "PASS", ncases);
	return fails ? 1 : 0;
}
