/* bt-client core: sequence and log lines (bt_client.h). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bt_client.h"

static void line(const struct btc_ops *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
line(const struct btc_ops *o, const char *fmt, ...)
{
	char b[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	o->out(o->ctx, b);
}

int
btc_scan_due(long prev_state, long state)
{
	return state == BTC_STATE_POWERED_ON && prev_state != BTC_STATE_POWERED_ON;
}

int
btc_rescan_due(long state, int scanning)
{
	return state == BTC_STATE_POWERED_ON && scanning == 0;
}

int
btc_run(const struct btc_ops *o, unsigned period_s, unsigned total_s, unsigned armb_at_s, unsigned armc_at_s, const char *build)
{
	struct btc_sample s = { -1, -1, 0, 0, 0 };
	unsigned t = 0, core_gaps = 0, a_disc = 0, a_uniq = 0, c_disc = 0, c_uniq = 0, b_disc = 0, b_uniq = 0;
	int rc, k, armb = 0, armc = 0;	/* 0 = arm not reached, 1 = its scan in force, -1 = it could not start */
	int a_open = 1;	/* T-27d4: arm A counts until the first switch; arm C until arm B starts (or the end); arm B until the end */
	const char *opts;
	char bdesc[64] = "none", cdesc[64] = "none", armb_tag[32] = "", armc_tag[32] = "";

	if (period_s == 0)
		period_s = 10;
	rc = o->start(o->ctx);
	/* T-18e5/T-2ae7: printed after start() so the boot shows the init option key string and value that reached CoreBluetooth */
	opts = rc == 0 ? o->central_options(o->ctx) : 0;
	if (armb_at_s)
		snprintf(armb_tag, sizeof armb_tag, " armB_at=%us", armb_at_s);
	if (armc_at_s)
		snprintf(armc_tag, sizeof armc_tag, " armC_at=%us", armc_at_s);
	line(o, "BT CLIENT: start build=%s period=%us total=%us central_options=%s scan=services:nil,options:nil%s%s", build, period_s,
	    total_s, opts ? opts : rc == 0 ? "none" : "none(no-manager)", armc_tag, armb_tag);
	if (rc != 0) {
		line(o, "BT CLIENT GAP: start failed rc=%d (no central; counts stay 0)", rc);
		core_gaps++;	/* gap:start */
	}
	if (armc_at_s && armb_at_s && armc_at_s >= armb_at_s) {
		line(o, "BT CLIENT GAP: arm C at %us not before arm B at %us (refused: arm B fills the duplicate map first)", armc_at_s,
		    armb_at_s);
		core_gaps++;	/* gap:armc-order */
		armc_at_s = 0;
	}
	while (t < total_s) {
		o->sleep_s(o->ctx, period_s);
		t += period_s;
		o->sample(o->ctx, &s);
		line(o, "BT CLIENT: t=%us state=%ld scanning=%d discovered=%u unique~%u gaps=%u", t, s.state, s.scanning, s.discovered,
		    s.unique, s.gaps + core_gaps);
		/* T-2ae7: keep the scan running: poweredOn without a scan -> issue it again (state -1 = no central, never due) */
		if (btc_rescan_due(s.state, s.scanning)) {
			k = o->rescan(o->ctx);
			if (k > 0) {
				line(o, "BT CLIENT: scan resumed reason=poweredOn-not-scanning n=%d", k);
			} else if (k == 0) {
				line(o, "BT CLIENT: scan resume skipped reason=queue-recheck-scanning-or-not-poweredOn");
			} else {
				line(o, "BT CLIENT GAP: scan resume failed rc=%d (poweredOn, not scanning, no scan issued)", k);
				core_gaps++;	/* gap:rescan */
			}
		}
		/* T-27d4 arm C: same central, scan services:@[FD50] options:nil (the documented background-scan input, gate 0x10058ed80) */
		if (armc_at_s && t == armc_at_s && rc == 0) {
			if (a_open) {
				a_disc = s.discovered;
				a_uniq = s.unique;
				a_open = 0;
			}
			c_disc = s.discovered;
			c_uniq = s.unique;
			k = o->arm_c(o->ctx, cdesc, sizeof cdesc);
			if (k >= 0) {
				armc = 1;
				line(o, "BT CLIENT: arm C start t=%us scan=services:%s,options:nil n=%d%s armA_discovered=%u armA_unique=%u", t,
				    cdesc, k, k == 0 ? "(on-next-poweredOn)" : "", a_disc, a_uniq);
			} else {
				armc = -1;
				line(o, "BT CLIENT GAP: arm C services not built rc=%d (scan stays services:nil,options:nil)", k);
				core_gaps++;	/* gap:armc */
			}
		}
		/* T-2ae7 arm B: same central, same services:nil scan, options {kCBMsgArgIsPrivilegedDaemon: true}; counts split here */
		if (armb_at_s && t == armb_at_s && rc == 0) {
			if (a_open) {
				a_disc = s.discovered;
				a_uniq = s.unique;
				a_open = 0;
			}
			b_disc = s.discovered;
			b_uniq = s.unique;
			k = o->arm_b(o->ctx, bdesc, sizeof bdesc);
			if (k >= 0) {
				armb = 1;
				line(o, "BT CLIENT: arm B start t=%us scan=services:nil,options:%s n=%d%s armA_discovered=%u armA_unique=%u", t,
				    bdesc, k, k == 0 ? "(on-next-poweredOn)" : "", a_disc, a_uniq);
			} else {
				armb = -1;
				/* T-27d4 review finding 2: after arm C the running scan keeps arm C's services filter */
				line(o, "BT CLIENT GAP: arm B options not built rc=%d (scan stays services:%s,options:nil)", k, armc == 1 ? cdesc : "nil");
				core_gaps++;	/* gap:armb */
			}
		}
	}
	o->sample(o->ctx, &s);
	if (armb == 0) {	/* no arm B: arm C (if any) runs to the end */
		b_disc = s.discovered;
		b_uniq = s.unique;
	}
	if (rc == 0 && s.state != BTC_STATE_POWERED_ON) {
		line(o, "BT CLIENT GAP: central never reached poweredOn (state=%ld)", s.state);
		core_gaps++;	/* gap:poweredon */
	}
	if (armb != 0 || armc != 0) {
		line(o, "BT CLIENT: ARM A options:nil discovered=%u unique=%u", a_disc, a_uniq);
		if (armc == 1)
			line(o, "BT CLIENT: ARM C services:%s discovered=%u unique_new=%u", cdesc, b_disc - c_disc, b_uniq - c_uniq);
		else if (armc == -1)
			line(o, "BT CLIENT: ARM C services:none(not-started) discovered=%u unique_new=%u", b_disc - c_disc, b_uniq - c_uniq);
		if (armb == 1)
			line(o, "BT CLIENT: ARM B options:%s discovered=%u unique_new=%u", bdesc, s.discovered - b_disc, s.unique - b_uniq);
		else if (armb == -1)
			line(o, "BT CLIENT: ARM B options:none(not-started) discovered=%u unique_new=%u", s.discovered - b_disc, s.unique - b_uniq);
	}
	line(o, "BT CLIENT: RESULT %s discovered=%u unique=%u state=%ld gaps=%u", s.unique >= 1 ? "pass" : "fail", s.discovered, s.unique,
	    s.state, s.gaps + core_gaps);
	return s.unique >= 1 ? 0 : 1;
}

/* ---- bt-client adv (bt-proper): see bt_client.h ---- */

static void aline(const struct btadv_ops *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
aline(const struct btadv_ops *o, const char *fmt, ...)
{
	char b[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	o->out(o->ctx, b);
}

static int
parse_uint(const char *s, unsigned long max, unsigned *out)
{
	char *end;
	unsigned long v;

	if (s == 0 || *s < '0' || *s > '9')
		return -1;
	v = strtoul(s, &end, 10);
	if (*end != 0 || v > max)
		return -1;
	*out = (unsigned)v;
	return 0;
}

int
btadv_parse(int argc, char **argv, unsigned *secs, unsigned *delay_s, char *why, unsigned long why_len)
{
	int i, have_secs = 0;

	*secs = BTADV_DEFAULT_S;
	*delay_s = 0;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--delay") == 0) {
			if (i + 1 >= argc || parse_uint(argv[i + 1], 3600, delay_s) != 0) {
				snprintf(why, why_len, "--delay needs 0..3600 seconds");
				return -1;
			}
			i++;
		} else if (!have_secs && parse_uint(argv[i], 86400, secs) == 0 && *secs >= 1) {
			have_secs = 1;
		} else {
			snprintf(why, why_len, "unknown or bad argument #%d (usage: adv [seconds 1..86400] [--delay 0..3600])", i + 1);
			return -1;
		}
	}
	return 0;
}

/* One place prints the verdict. started/err/state are what the run saw; reason is NULL for PASS. */
static int
adv_result(const struct btadv_ops *o, const char *reason, const struct btadv_sample *s, unsigned core_gaps, unsigned secs_done)
{
	if (reason == 0) {
		aline(o, "BT ADV: RESULT: PASS adv started=1 err=0 seconds=%u reads=%u centrals=%u gaps=%u", secs_done, s->reads, s->centrals,
		    s->gaps + core_gaps);
		return 0;
	}
	aline(o, "BT ADV: RESULT: FAIL adv reason=%s started=%d err=%ld state=%ld seconds=%u reads=%u centrals=%u gaps=%u", reason,
	    s->adv == 1, s->err, s->state, secs_done, s->reads, s->centrals, s->gaps + core_gaps);
	return 1;
}

int
btadv_run(const struct btadv_ops *o, unsigned secs, unsigned delay_s, const char *build)
{
	struct btadv_sample s = { -1, 0, 0, -1, 0, "", 0, 0, 0 };
	unsigned t, core_gaps = 0, done = 0;
	const char *reason = 0;
	int lost = 0, started_pm = 0;

	aline(o, "BT ADV: start build=%s seconds=%u delay=%us", build, secs, delay_s);
	if (delay_s) {
		aline(o, "BT ADV: waiting %us before the first CoreBluetooth call (after BTApp's checks)", delay_s);
		o->sleep_s(o->ctx, delay_s);
	}
	if (o->start(o->ctx) != 0) {
		aline(o, "BT ADV GAP: start failed (no peripheral manager)");
		core_gaps++;	/* gap:adv-start */
		reason = "start-failed";
		goto finish;
	}
	started_pm = 1;
	/* 1. poweredOn within BTADV_POWER_WAIT_S */
	for (t = 0;; t++) {
		o->sample(o->ctx, &s);
		if (s.state == BTC_STATE_POWERED_ON)
			break;
		if (t >= BTADV_POWER_WAIT_S) {
			aline(o, "BT ADV GAP: peripheral manager never reached poweredOn within %us (state=%ld)", BTADV_POWER_WAIT_S, s.state);
			core_gaps++;	/* gap:adv-poweredon */
			reason = "not-poweredOn";
			goto finish;
		}
		o->sleep_s(o->ctx, 1);
	}
	aline(o, "BT ADV: poweredOn after %us", t);
	/* 2. the GATT service, before advertising */
	if (o->add_service(o->ctx) != 0) {
		aline(o, "BT ADV GAP: addService could not be issued");
		core_gaps++;	/* gap:adv-addcall */
		reason = "add-service-call";
		goto finish;
	}
	for (t = 0;; t++) {
		o->sample(o->ctx, &s);
		if (s.svc != 0)
			break;
		if (t >= BTADV_STEP_WAIT_S) {
			aline(o, "BT ADV GAP: no didAddService within %us", BTADV_STEP_WAIT_S);
			core_gaps++;	/* gap:adv-addtimeout */
			reason = "add-service-timeout";
			goto finish;
		}
		o->sleep_s(o->ctx, 1);
	}
	if (s.svc < 0) {
		aline(o, "BT ADV: didAddService error domain=%s code=%ld", s.err_domain, s.err);
		reason = "add-service-error";
		goto finish;
	}
	aline(o, "BT ADV: didAddService ok");
	/* 3. advertising */
	if (o->advertise(o->ctx) != 0) {
		aline(o, "BT ADV GAP: startAdvertising could not be issued");
		core_gaps++;	/* gap:adv-advcall */
		reason = "advertise-call";
		goto finish;
	}
	for (t = 0;; t++) {
		o->sample(o->ctx, &s);
		if (s.adv != 0)
			break;
		if (t >= BTADV_STEP_WAIT_S) {
			aline(o, "BT ADV GAP: no didStartAdvertising within %us", BTADV_STEP_WAIT_S);
			core_gaps++;	/* gap:adv-starttimeout */
			reason = "start-timeout";
			goto finish;
		}
		o->sleep_s(o->ctx, 1);
	}
	if (s.adv < 0) {
		aline(o, "BT ADV: didStartAdvertising error domain=%s code=%ld", s.err_domain, s.err);
		reason = "start-error";
		goto finish;
	}
	aline(o, "BT ADV: didStartAdvertising ok");
	/* 4. keep advertising for the window; the controller may drop it on its own, which must not read as a pass */
	for (done = 1; done <= secs; done++) {
		o->sleep_s(o->ctx, 1);
		o->sample(o->ctx, &s);
		if (s.is_adv == 0 && !lost) {
			lost = 1;
			aline(o, "BT ADV GAP: isAdvertising dropped to NO at t=%us without stopAdvertising", done);
			core_gaps++;	/* gap:adv-lost */
		}
		if (done % 10 == 0 || done == secs)
			aline(o, "BT ADV: t=%us state=%ld advertising=%d reads=%u centrals=%u gaps=%u", done, s.state, s.is_adv, s.reads, s.centrals,
			    s.gaps + core_gaps);
	}
	done = secs;
	if (lost)
		reason = "advertising-stopped";
finish:
	if (started_pm) {	/* always-run teardown: also after every failure past start() */
		o->stop(o->ctx);
		aline(o, "BT ADV: stopped");
		o->sample(o->ctx, &s);	/* reads that arrived at the very end */
	}
	return adv_result(o, reason, &s, core_gaps, done);
}

/* ---- bt-client tone (bt-proper): see bt_client.h ---- */

static void tline(const struct bttone_ops *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
tline(const struct bttone_ops *o, const char *fmt, ...)
{
	char b[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	o->out(o->ctx, b);
}

int
bttone_parse(int argc, char **argv, unsigned *secs, unsigned *delay_s, char *why, unsigned long why_len)
{
	int i, have_secs = 0;

	*secs = BTTONE_DEFAULT_S;
	*delay_s = 0;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--delay") == 0) {
			if (i + 1 >= argc || parse_uint(argv[i + 1], 3600, delay_s) != 0) {
				snprintf(why, why_len, "--delay needs 0..3600 seconds");
				return -1;
			}
			i++;
		} else if (!have_secs && parse_uint(argv[i], 3600, secs) == 0 && *secs >= 1) {
			have_secs = 1;
		} else {
			snprintf(why, why_len, "unknown or bad argument #%d (usage: tone [seconds 1..3600] [--delay 0..3600])", i + 1);
			return -1;
		}
	}
	return 0;
}

void
bttone_fill(short *out, unsigned frames, unsigned *pos)
{
	unsigned i;

	for (i = 0; i < frames; i++) {
		/* 440 Hz at 44100 Hz repeats every 44100 frames (440 whole cycles), so the counter wraps without a phase jump */
		double ph = 2.0 * M_PI * (double)BTTONE_HZ * (double)(*pos) / (double)BTTONE_RATE;
		short v = (short)lrint((double)BTTONE_AMP * sin(ph));

		out[2 * i] = v;
		out[2 * i + 1] = v;
		*pos = (*pos + 1) % BTTONE_RATE;
	}
}

unsigned
bttone_min_refills(unsigned secs)
{
	return (unsigned)(((unsigned long)secs * BTTONE_RATE / BTTONE_BUF_FRAMES) / 2);
}

static int
tone_result(const struct bttone_ops *o, const char *reason, const struct bttone_sample *s, const char *route, unsigned core_gaps,
    unsigned played)
{
	if (reason == 0) {
		tline(o, "BT TONE: RESULT: PASS tone played_s=%u route=%s gaps=%u", played, route, s->gaps + core_gaps);
		return 0;
	}
	tline(o, "BT TONE: RESULT: FAIL reason=%s route=%s played_s=%u qerr=%ld refills=%u gaps=%u", reason, route, played, s->qerr, s->refills,
	    s->gaps + core_gaps);
	return 1;
}

int
bttone_run(const struct bttone_ops *o, unsigned secs, unsigned delay_s, const char *build)
{
	struct bttone_sample s = { 0, 0, 0 };
	char route[BTROUTE_LEN] = "?", before[BTROUTE_LEN] = "?";
	unsigned t, core_gaps = 0, done = 0, stall = 0, last_refills = 0;
	const char *reason = 0;
	int attempted = 0;

	tline(o, "BT TONE: start build=%s seconds=%u delay=%us", build, secs, delay_s);
	if (delay_s) {
		tline(o, "BT TONE: waiting %us before the first audio call (after BTApp's checks)", delay_s);
		o->sleep_s(o->ctx, delay_s);
	}
	attempted = 1;	/* from here stop() always runs: it also deactivates a session that was only half set up */
	if (o->session(o->ctx) != 0) {
		tline(o, "BT TONE GAP: audio session could not be set to Playback and activated");
		core_gaps++;	/* gap:tone-session */
		reason = "session-failed";
		goto finish;
	}
	o->route(o->ctx, before, sizeof before);
	tline(o, "BT TONE: route before=%s", before);
	if (o->start(o->ctx) != 0) {
		tline(o, "BT TONE GAP: AudioQueue could not be created or started");
		core_gaps++;	/* gap:tone-start */
		reason = "queue-start";
		goto finish;
	}
	tline(o, "BT TONE: queue running (%u buffers of %u frames, %u Hz, %u Hz tone)", BTTONE_NBUF, BTTONE_BUF_FRAMES, BTTONE_RATE, BTTONE_HZ);
	for (t = 1; t <= secs; t++) {
		o->sleep_s(o->ctx, 1);
		o->sample(o->ctx, &s);
		if (s.qerr != 0) {
			tline(o, "BT TONE GAP: AudioQueue error OSStatus=%ld at t=%us", s.qerr, t);
			core_gaps++;	/* gap:tone-qerr */
			reason = "queue-error";
			break;
		}
		stall = s.refills == last_refills ? stall + 1 : 0;
		last_refills = s.refills;
		if (stall >= BTTONE_STALL_S) {
			tline(o, "BT TONE GAP: no buffer refill for %us at t=%us (the queue stopped consuming)", stall, t);
			core_gaps++;	/* gap:tone-stall */
			reason = "queue-stalled";
			break;
		}
		done = t;
		if (t % 10 == 0 || t == secs)
			tline(o, "BT TONE: t=%us refills=%u gaps=%u", t, s.refills, s.gaps + core_gaps);
	}
	if (reason == 0 && s.refills < bttone_min_refills(secs)) {
		tline(o, "BT TONE GAP: only %u buffer refills in %us (expected at least %u)", s.refills, secs, bttone_min_refills(secs));
		core_gaps++;	/* gap:tone-few */
		reason = "too-few-refills";
	}
finish:
	if (attempted) {
		o->route(o->ctx, route, sizeof route);
		tline(o, "BT TONE: route after=%s", route);
		o->stop(o->ctx);
		tline(o, "BT TONE: stopped");
		o->sample(o->ctx, &s);
	}
	return tone_result(o, reason, &s, route, core_gaps, done);
}
