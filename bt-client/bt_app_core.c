/* bt-client app core (T-2d63): the BTApp sequence and its log lines (bt_client.h, struct btc_app_ops). One services:nil,
 * options:nil scan runs for the whole app life; the counts are split into windows at the app-state transitions the delegate
 * recorded (each with a counter snapshot taken on the delegate queue), so "fg" and "bg" windows are two states of ONE scan.
 * bluetoothd's report gate decides by the session's app state (0x10 foreground vs 0x20 daemon, T-54b1 / T-9524) [bin]; the
 * app cannot read that byte, so the window label is the UIKit state the app itself was told (a proxy, named as such). */
#include <stdarg.h>
#include <stdio.h>
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

const char *
btc_app_label(int state)
{
	switch (state) {
	case BTC_APP_ACTIVE:
		return "fg";
	case BTC_APP_INACTIVE:
		return "in";
	case BTC_APP_BACKGROUND:
		return "bg";
	case BTC_APP_LAUNCHING:
		return "pre";
	case BTC_APP_MIXED:
		return "mix";
	default:
		return "unknown";
	}
}

static const char *
state_name(int state)
{
	switch (state) {
	case BTC_APP_ACTIVE:
		return "active";
	case BTC_APP_INACTIVE:
		return "inactive";
	case BTC_APP_BACKGROUND:
		return "background";
	case BTC_APP_LAUNCHING:
		return "launching";
	case BTC_APP_MIXED:
		return "mixed";
	default:
		return "unknown";
	}
}

static const char *
conn_name(int outcome)
{
	switch (outcome) {
	case BTC_CONN_NOCAND:
		return "no-candidate";
	case BTC_CONN_OK:
		return "connected";
	case BTC_CONN_FAIL:
		return "failed";
	case BTC_CONN_TIMEOUT:
		return "timeout";
	case BTC_CONN_NOCENTRAL:
		return "no-central";
	default:
		return "unknown";
	}
}

static const char *
pair_name(int outcome)
{
	switch (outcome) {
	case BTC_PAIR_NOMGR:
		return "no-manager";
	case BTC_PAIR_NOPEER:
		return "no-peer";
	case BTC_PAIR_OK:
		return "paired";
	case BTC_PAIR_FAIL:
		return "failed";
	case BTC_PAIR_TIMEOUT:
		return "timeout";
	default:
		return "unknown";
	}
}

static const char *
pair_stage_name(int stage)
{
	switch (stage) {
	case BTC_PAIR_ST_NONE:
		return "none";
	case BTC_PAIR_ST_FOUND:
		return "found";
	case BTC_PAIR_ST_STARTED:
		return "started";
	case BTC_PAIR_ST_CONFIRM:
		return "confirm";
	case BTC_PAIR_ST_ANSWERED:
		return "answered";
	case BTC_PAIR_ST_PAIRED:
		return "paired";
	case BTC_PAIR_ST_BONDED:
		return "bonded";
	default:
		return "unknown";
	}
}

struct win {
	int state;
	unsigned t0, t1, d0, d1, u0, u1;
};

int
btc_app_run(const struct btc_app_ops *ao, unsigned period_s, unsigned total_s, const char *build)
{
	const struct btc_ops *o = ao->bt;
	struct btc_sample s = { -1, -1, 0, 0, 0 };
	struct btc_trans tr;
	struct win w[BTC_APP_MAX_WINDOWS];
	unsigned nw = 1, seen = 0, core_gaps = 0, t = 0, t_start, iter = 0, max_iter, fg = 0, bg = 0, k, idx;
	int rc, n, full = 0, conn_done = 0, conn_first = -1, conn_svc = -1;
	unsigned conn_att = 0, conn_ok = 0;
	int pair_done = 0, pair_ran = 0, pair_bonded = 0, pair_reconnect = 0, pair_outcome = BTC_PAIR_NOMGR, pair_stage = BTC_PAIR_ST_NONE;
	const char *opts;

	if (period_s == 0)
		period_s = 10;
	max_iter = total_s / period_s + 4;
	t_start = ao->now_s(o->ctx);
	w[0] = (struct win){ BTC_APP_LAUNCHING, 0, 0, 0, 0, 0, 0 };
	rc = o->start(o->ctx);
	opts = rc == 0 ? o->central_options(o->ctx) : 0;
	line(o, "BT CLIENT: start build=%s mode=app period=%us total=%us central_options=%s scan=services:nil,options:nil", build, period_s,
	    total_s, opts ? opts : rc == 0 ? "none" : "none(no-manager)");
	if (rc != 0) {
		line(o, "BT CLIENT GAP: start failed rc=%d (no central; counts stay 0)", rc);
		core_gaps++;	/* gap:start */
	}
	while (t < total_s) {
		if (++iter > max_iter) {
			line(o, "BT CLIENT GAP: clock stalled at t=%us after %u samples (now_s does not advance; run ended early)", t, iter - 1);
			core_gaps++;	/* gap:clock */
			break;
		}
		o->sleep_s(o->ctx, period_s);
		t = ao->now_s(o->ctx) - t_start;
		o->sample(o->ctx, &s);
		/* close a window at every recorded state change (a repeat of the same state is not a new window) */
		while (ao->transition(o->ctx, seen, &tr)) {
			/* a transition stamped before the scan thread read t_start counts as t=0 (unsigned, no wrap) */
			unsigned rt = tr.t_s >= t_start ? tr.t_s - t_start : 0;

			seen++;
			line(o, "BT CLIENT: app state=%s t=%us discovered=%u unique~%u", state_name(tr.state), rt, tr.discovered,
			    tr.unique);
			if (tr.state == w[nw - 1].state)
				continue;
			if (nw == BTC_APP_MAX_WINDOWS) {
				if (!full) {
					line(o, "BT CLIENT GAP: more than %u app-state windows (later changes merged into window %u)",
					    BTC_APP_MAX_WINDOWS, nw);
					core_gaps++;	/* gap:windows */
					full = 1;
					w[nw - 1].state = BTC_APP_MIXED;	/* its counts now span several states: neither fg nor bg */
				}
				continue;
			}
			w[nw - 1].t1 = rt;
			w[nw - 1].d1 = tr.discovered;
			w[nw - 1].u1 = tr.unique;
			w[nw] = (struct win){ tr.state, rt, 0, tr.discovered, 0, tr.unique, 0 };
			nw++;
		}
		line(o, "BT CLIENT: t=%us app=%s state=%ld scanning=%d discovered=%u unique~%u gaps=%u", t, state_name(w[nw - 1].state), s.state,
		    s.scanning, s.discovered, s.unique, s.gaps + core_gaps);
		/* T-3615: the connect step, once, in the first ACTIVE stretch after conn_at_s (the scan is stopped by the backend and
		 * re-issued by the not-scanning resume below) */
		while (ao->connect && !conn_done && w[nw - 1].state == BTC_APP_ACTIVE && t >= ao->conn_at_s) {
			struct btc_conn c;

			memset(&c, 0, sizeof c);
			c.rssi = 127;
			c.services = -1;
			(void)ao->connect(o->ctx, conn_att, ao->conn_timeout_s, &c);
			if (c.outcome == BTC_CONN_NOCAND || c.outcome == BTC_CONN_NOCENTRAL) {
				line(o, "BT CLIENT GAP: connect try=%u %s (cands=%u): nothing was sent", conn_att + 1, conn_name(c.outcome), c.cands);
				core_gaps++;	/* gap:connect */
				if (conn_first < 0)
					conn_first = c.outcome;
				conn_done = 1;
				break;
			}
			conn_att++;
			line(o, "BT CLIENT: connect try=%u outcome=%s cands=%u rssi=%d ms=%u err=%ld services=%d disc=%d", conn_att, conn_name(c.outcome),
			    c.cands, c.rssi, c.ms, c.has_err ? c.err : 0L, c.services, c.disc);
			if (c.outcome == BTC_CONN_OK) {
				conn_ok++;
				conn_svc = c.services;
				conn_done = 1;	/* one connection is the metric; the rest of the run scans on */
			} else if (conn_first < 0) {
				conn_first = c.outcome;
			}
			if (conn_att >= ao->conn_tries)
				conn_done = 1;
		}
		/* T-a52f: the pair step, once, in the first ACTIVE stretch after pair_at_s */
		if (ao->pair && !pair_done && w[nw - 1].state == BTC_APP_ACTIVE && t >= ao->pair_at_s) {
			struct btc_pair p;

			memset(&p, 0, sizeof p);
			p.bonded_start = p.bonded_end = -1;
			pair_done = 1;
			pair_ran = 1;
			(void)ao->pair(o->ctx, ao->pair_timeout_s, &p);
			pair_outcome = p.outcome;
			pair_stage = p.stage;
			line(o, "BT CLIENT: pair outcome=%s stage=%s ms=%u err=%ld bonded_start=%d bonded_end=%d prompt=%d connected=%d peer_bonded=%d", pair_name(p.outcome),
			    pair_stage_name(p.stage), p.ms, p.has_err ? p.err : 0L, p.bonded_start, p.bonded_end, p.prompt, p.connected, p.peer_bonded);
			if (p.outcome == BTC_PAIR_NOMGR) {
				line(o, "BT CLIENT GAP: pair %s: nothing was sent", pair_name(p.outcome));
				core_gaps++;	/* gap:pair */
			} else if (p.outcome == BTC_PAIR_OK) {
				if (p.peer_bonded && p.bonded_start >= 1) {
					pair_bonded = 1;	/* the bond survived: the peer is among the paired devices */
					if (p.connected && !p.prompt) {
						pair_reconnect = 1;
					} else {
						line(o, "BT CLIENT GAP: pair peer already bonded but no reconnect (connected=%d prompt=%d err=%ld)", p.connected, p.prompt,
						    p.has_err ? p.err : 0L);
						core_gaps++;	/* gap:pair-noreconnect */
					}
				} else if (p.bonded_start == 0 && p.bonded_end >= 1)
					pair_bonded = 1;	/* a new bond this boot */
				else if (p.bonded_start >= 1 && p.connected && !p.prompt)
					pair_reconnect = 1;	/* encrypted reconnect on a surviving bond, no new prompt */
				else {
					line(o, "BT CLIENT GAP: pair ok but bonded_start=%d bonded_end=%d prompt=%d connected=%d fits neither bonded nor reconnect",
					    p.bonded_start, p.bonded_end, p.prompt, p.connected);
					core_gaps++;	/* gap:pair-shape */
				}
			}
		}
		if (btc_rescan_due(s.state, s.scanning)) {
			n = o->rescan(o->ctx);
			if (n > 0) {
				line(o, "BT CLIENT: scan resumed reason=poweredOn-not-scanning n=%d", n);
			} else if (n == 0) {
				line(o, "BT CLIENT: scan resume skipped reason=queue-recheck-scanning-or-not-poweredOn");
			} else {
				line(o, "BT CLIENT GAP: scan resume failed rc=%d (poweredOn, not scanning, no scan issued)", n);
				core_gaps++;	/* gap:rescan */
			}
		}
	}
	o->sample(o->ctx, &s);
	w[nw - 1].t1 = t;
	w[nw - 1].d1 = s.discovered;
	w[nw - 1].u1 = s.unique;
	if (rc == 0 && s.state != BTC_STATE_POWERED_ON) {
		line(o, "BT CLIENT GAP: central never reached poweredOn (state=%ld)", s.state);
		core_gaps++;	/* gap:poweredon */
	}
	for (k = 0; k < nw; k++) {
		unsigned d = w[k].d1 >= w[k].d0 ? w[k].d1 - w[k].d0 : 0, u = w[k].u1 >= w[k].u0 ? w[k].u1 - w[k].u0 : 0;

		if (w[k].d1 < w[k].d0 || w[k].u1 < w[k].u0) {
			line(o, "BT CLIENT GAP: window %u counters went back (%u -> %u), counted 0", k + 1, w[k].d0, w[k].d1);
			core_gaps++;	/* gap:back */
		}
		idx = 1;
		for (unsigned j = 0; j < k; j++)
			idx += w[j].state == w[k].state;
		line(o, "BT CLIENT: ARM %s%u services:nil discovered=%u unique_new=%u t=%u..%us", btc_app_label(w[k].state), idx, d, u, w[k].t0,
		    w[k].t1);
		if (w[k].state == BTC_APP_ACTIVE)
			fg += d;
		else if (w[k].state == BTC_APP_BACKGROUND)
			bg += d;
	}
	if (ao->connect) {
		if (!conn_done && conn_att == 0) {
			line(o, "BT CLIENT GAP: connect step never ran (no ACTIVE window at t >= %us)", ao->conn_at_s);
			core_gaps++;	/* gap:connect-never */
			conn_first = BTC_CONN_NOCENTRAL;
		}
		line(o, "BT CLIENT: RESULT: %s bt-connect conn_ok=%u attempts=%u services=%d first_fail=%s gaps=%u", conn_ok ? "PASS" : "FAIL", conn_ok,
		    conn_att, conn_svc, conn_first < 0 ? "none" : conn_name(conn_first),
		    s.gaps + core_gaps + (ao->side_gaps ? ao->side_gaps(o->ctx) : 0));
	}
	if (ao->pair) {
		if (!pair_ran) {
			line(o, "BT CLIENT GAP: pair step never ran (no ACTIVE window at t >= %us)", ao->pair_at_s);
			core_gaps++;	/* gap:pair-never */
		}
		line(o, "BT CLIENT: RESULT: %s bt-pair bonded=%d reconnect=%d first_fail=%s stage=%s gaps=%u", pair_bonded || pair_reconnect ? "PASS" : "FAIL",
		    pair_bonded, pair_reconnect, pair_bonded || pair_reconnect ? "none" : pair_ran ? pair_name(pair_outcome) : "no-central",
		    pair_stage_name(pair_stage), s.gaps + core_gaps + (ao->side_gaps ? ao->side_gaps(o->ctx) : 0));
	}
	line(o, "BT CLIENT: RESULT %s discovered=%u unique=%u fg_discovered=%u bg_discovered=%u windows=%u state=%ld gaps=%u",
	    fg >= 1 ? "pass" : "fail", s.discovered, s.unique, fg, bg, nw, s.state,
	    s.gaps + core_gaps + (ao->side_gaps ? ao->side_gaps(o->ctx) : 0));
	return fg >= 1 ? 0 : 1;
}
