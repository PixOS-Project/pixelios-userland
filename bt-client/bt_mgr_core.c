/* sprint-bt: host-testable part of the BluetoothManager path (see bt_mgr.h). */
#include "bt_mgr.h"

#include <stdio.h>
#include <string.h>

int
btm_attach_pass(const struct btm_attach *a)
{
	return a->shared && a->available && a->power_req && a->powered;
}

void
btm_attach_result(const struct btm_attach *a, char *buf, unsigned long n)
{
	snprintf(buf, n, "BT MGR: RESULT: %s bt-mgr mg_bluetooth=%d shared=%d init_err=%ld available=%d avail_ms=%u powered=%d power_ms=%u "
	    "scan=%d discovered=%u unique=%u gaps=%u", btm_attach_pass(a) ? "PASS" : "FAIL", a->mg_bluetooth, a->shared, a->shared ? 0L : a->init_err,
	    a->available, a->avail_ms, a->powered, a->power_ms, a->scan_req, a->discovered, a->unique, a->gaps);
}

void
btm_pair_init(struct btm_pair_sm *s, unsigned now_ms)
{
	memset(s, 0, sizeof *s);
	s->r.outcome = BTC_PAIR_NOPEER;
	s->r.stage = BTC_PAIR_ST_NONE;
	s->r.bonded_start = s->r.bonded_end = -1;
	s->t0_ms = now_ms;
}

static void
stage(struct btm_pair_sm *s, int st)
{
	if (st > s->r.stage)
		s->r.stage = st;
}

static void
finish(struct btm_pair_sm *s, int outcome, unsigned now_ms)
{
	if (s->done)
		return;
	s->r.outcome = outcome;
	s->r.ms = now_ms - s->t0_ms;
	s->done = 1;
}

int
btm_pair_feed(struct btm_pair_sm *s, enum btm_ev ev, long a, unsigned now_ms)
{
	switch (ev) {
	case BTM_EV_START:
		s->r.bonded_start = (int)a;
		break;
	case BTM_EV_FOUND:
		stage(s, BTC_PAIR_ST_FOUND);
		if (!s->done && s->r.outcome == BTC_PAIR_NOPEER)
			s->r.outcome = BTC_PAIR_TIMEOUT;	/* found: from now on a missing result is a timeout, not "no peer" */
		break;
	case BTM_EV_PEER_BONDED:
		stage(s, BTC_PAIR_ST_BONDED);
		s->r.peer_bonded = 1;
		if (!s->done && s->r.outcome == BTC_PAIR_NOPEER)
			s->r.outcome = BTC_PAIR_TIMEOUT;
		break;
	case BTM_EV_CONNECT_SENT:
		stage(s, BTC_PAIR_ST_STARTED);
		break;
	case BTM_EV_CONFIRM:
		stage(s, BTC_PAIR_ST_CONFIRM);
		s->r.prompt = 1;
		break;
	case BTM_EV_ANSWERED:
		stage(s, BTC_PAIR_ST_ANSWERED);
		break;
	case BTM_EV_CONNECTED:
		s->r.connected = 1;
		if (s->r.peer_bonded && !s->r.prompt)
			finish(s, BTC_PAIR_OK, now_ms);	/* bonded peer reconnected: nothing more to wait for */
		break;
	case BTM_EV_PIN_OK:
	case BTM_EV_PAIRED:
		stage(s, BTC_PAIR_ST_PAIRED);
		finish(s, BTC_PAIR_OK, now_ms);
		break;
	case BTM_EV_PIN_FAIL:
	case BTM_EV_CONNECT_FAIL:
		if (s->r.peer_bonded && ev == BTM_EV_CONNECT_FAIL) {
			/* the bond exists; only the reconnect failed (the app prints a GAP for it) */
			s->r.has_err = 1;
			s->r.err = a;
			finish(s, BTC_PAIR_OK, now_ms);
			break;
		}
		if (!s->done) {
			s->r.has_err = 1;
			s->r.err = a;
		}
		finish(s, BTC_PAIR_FAIL, now_ms);
		break;
	case BTM_EV_END:
		s->r.bonded_end = (int)a;
		if (!s->done && s->r.peer_bonded) {
			finish(s, BTC_PAIR_OK, now_ms);	/* bonded peer, no connect result in time */
		} else if (!s->done) {
			/* a reconnect on a surviving bond: no prompt, bonded before and after, connected */
			if (s->r.bonded_start >= 1 && s->r.bonded_end >= s->r.bonded_start && s->r.connected && !s->r.prompt && s->r.stage >= BTC_PAIR_ST_STARTED) {
				stage(s, BTC_PAIR_ST_PAIRED);
				finish(s, BTC_PAIR_OK, now_ms);
			} else {
				finish(s, s->r.outcome, now_ms);	/* NOPEER or TIMEOUT */
			}
		}
		return 1;
	}
	return s->done;
}
