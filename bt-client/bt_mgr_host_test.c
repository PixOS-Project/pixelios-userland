/* sprint-bt: host test of bt_mgr_core.c, the BluetoothManager attach accounting and the pair state machine that bt_mgr_ios.c
 * feeds with notifications. Each pair case is the notification order the T-a52f decode predicts (research Q2.1-Q2.4) or a
 * failure of it; the expected struct btc_pair is what bt_app_core.c then classifies (its own host test covers that part).
 * Last line: RESULT: PASS|FAIL bt_mgr_host_test cases=N */
#include "bt_mgr.h"

#include <stdio.h>
#include <string.h>

static int fails, cases;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s line %d: %s\n", name, __LINE__, #c); fails++; } } while (0)

struct ev { enum btm_ev e; long a; unsigned t; };

static struct btc_pair
run(const struct ev *v, int n, int *fin_at)
{
	struct btm_pair_sm s;
	int i;

	btm_pair_init(&s, 1000);
	*fin_at = -1;
	for (i = 0; i < n; i++)
		if (btm_pair_feed(&s, v[i].e, v[i].a, v[i].t) && *fin_at < 0)
			*fin_at = i;
	return s.r;
}

int
main(void)
{
	const char *name;
	int f;
	struct btc_pair r;

	{	/* 1. new bond, Just Works / numeric comparison: found, connect, confirm, answer, PIN success, then END reads 1 bond */
		static const struct ev v[] = { { BTM_EV_START, 0, 1000 }, { BTM_EV_FOUND, 0, 3000 }, { BTM_EV_CONNECT_SENT, 0, 3001 },
		    { BTM_EV_CONFIRM, 0, 5000 }, { BTM_EV_ANSWERED, 0, 5001 }, { BTM_EV_CONNECTED, 0, 6000 }, { BTM_EV_PIN_OK, 0, 7000 }, { BTM_EV_END, 1, 7100 } };
		name = "new_bond"; cases++;
		r = run(v, 8, &f);
		CHECK(f == 6);	/* the glue stops waiting at the PIN result, then reads the bond count */
		CHECK(r.outcome == BTC_PAIR_OK && r.stage == BTC_PAIR_ST_PAIRED && r.prompt == 1 && r.connected == 1);
		CHECK(r.bonded_start == 0 && r.bonded_end == 1 && r.ms == 6000 && r.has_err == 0);
	}
	{	/* 2. paired via the status notification instead of the PIN result */
		static const struct ev v[] = { { BTM_EV_START, 0, 1000 }, { BTM_EV_FOUND, 0, 2000 }, { BTM_EV_CONNECT_SENT, 0, 2000 },
		    { BTM_EV_CONFIRM, 0, 2500 }, { BTM_EV_ANSWERED, 0, 2500 }, { BTM_EV_PAIRED, 0, 4000 }, { BTM_EV_END, 1, 4000 } };
		name = "paired_status"; cases++;
		r = run(v, 7, &f);
		CHECK(f == 5 && r.outcome == BTC_PAIR_OK && r.stage == BTC_PAIR_ST_PAIRED && r.bonded_end == 1);
	}
	{	/* 3. reconnect after a reboot: bond exists, connect succeeds, no prompt, no PIN result -> OK at END */
		static const struct ev v[] = { { BTM_EV_START, 1, 1000 }, { BTM_EV_FOUND, 0, 2000 }, { BTM_EV_CONNECT_SENT, 0, 2000 },
		    { BTM_EV_CONNECTED, 0, 3000 }, { BTM_EV_END, 1, 9000 } };
		name = "reconnect"; cases++;
		r = run(v, 5, &f);
		CHECK(f == 4 && r.outcome == BTC_PAIR_OK && r.prompt == 0 && r.connected == 1 && r.bonded_start == 1 && r.bonded_end == 1);
	}
	{	/* 4. peer never found */
		static const struct ev v[] = { { BTM_EV_START, 0, 1000 }, { BTM_EV_END, 0, 61000 } };
		name = "no_peer"; cases++;
		r = run(v, 2, &f);
		CHECK(f == 1 && r.outcome == BTC_PAIR_NOPEER && r.stage == BTC_PAIR_ST_NONE && r.ms == 60000);
	}
	{	/* 5. found, prompt answered, nothing else: timeout at stage answered */
		static const struct ev v[] = { { BTM_EV_START, 0, 1000 }, { BTM_EV_FOUND, 0, 2000 }, { BTM_EV_CONNECT_SENT, 0, 2000 },
		    { BTM_EV_CONFIRM, 0, 3000 }, { BTM_EV_ANSWERED, 0, 3000 }, { BTM_EV_END, 0, 61000 } };
		name = "stuck_answered"; cases++;
		r = run(v, 6, &f);
		CHECK(f == 5 && r.outcome == BTC_PAIR_TIMEOUT && r.stage == BTC_PAIR_ST_ANSWERED);
	}
	{	/* 6. PIN result failed with an error: FAIL, err kept, a later PAIRED does not flip it */
		static const struct ev v[] = { { BTM_EV_START, 0, 1000 }, { BTM_EV_FOUND, 0, 2000 }, { BTM_EV_CONNECT_SENT, 0, 2000 },
		    { BTM_EV_CONFIRM, 0, 3000 }, { BTM_EV_ANSWERED, 0, 3000 }, { BTM_EV_PIN_FAIL, 158, 4000 }, { BTM_EV_PAIRED, 0, 4500 }, { BTM_EV_END, 0, 4600 } };
		name = "pin_fail"; cases++;
		r = run(v, 8, &f);
		CHECK(f == 5 && r.outcome == BTC_PAIR_FAIL && r.has_err == 1 && r.err == 158 && r.ms == 3000);
	}
	{	/* 7. connect failed before any prompt */
		static const struct ev v[] = { { BTM_EV_START, 0, 1000 }, { BTM_EV_FOUND, 0, 2000 }, { BTM_EV_CONNECT_SENT, 0, 2000 },
		    { BTM_EV_CONNECT_FAIL, 4, 12000 }, { BTM_EV_END, 0, 12000 } };
		name = "connect_fail"; cases++;
		r = run(v, 5, &f);
		CHECK(f == 3 && r.outcome == BTC_PAIR_FAIL && r.stage == BTC_PAIR_ST_STARTED && r.err == 4 && r.prompt == 0);
	}
	{	/* 8. existing bond but connect never completed: not a reconnect -> timeout */
		static const struct ev v[] = { { BTM_EV_START, 1, 1000 }, { BTM_EV_FOUND, 0, 2000 }, { BTM_EV_CONNECT_SENT, 0, 2000 }, { BTM_EV_END, 1, 61000 } };
		name = "bond_no_connect"; cases++;
		r = run(v, 4, &f);
		CHECK(r.outcome == BTC_PAIR_TIMEOUT && r.stage == BTC_PAIR_ST_STARTED);
	}
	{	/* 10. bonded at start, peer in pairedDevices, connect succeeds: finishes at CONNECTED, bonded + connected, no prompt */
		static const struct ev v[] = { { BTM_EV_START, 1, 1000 }, { BTM_EV_PEER_BONDED, 0, 1000 }, { BTM_EV_CONNECT_SENT, 0, 1000 },
		    { BTM_EV_CONNECTED, 0, 2500 }, { BTM_EV_END, 1, 4500 } };
		name = "bonded_at_start_connect"; cases++;
		r = run(v, 5, &f);
		CHECK(f == 3 && r.outcome == BTC_PAIR_OK && r.stage == BTC_PAIR_ST_BONDED && r.peer_bonded == 1 && r.connected == 1 && r.prompt == 0);
		CHECK(r.bonded_start == 1 && r.bonded_end == 1 && r.ms == 1500);
	}
	{	/* 11. bonded at start, connect fails: the bond still counts (OK), connected 0, error kept for the GAP line */
		static const struct ev v[] = { { BTM_EV_START, 1, 1000 }, { BTM_EV_PEER_BONDED, 0, 1000 }, { BTM_EV_CONNECT_SENT, 0, 1000 },
		    { BTM_EV_CONNECT_FAIL, 4, 6000 }, { BTM_EV_END, 1, 6100 } };
		name = "bonded_at_start_connect_fail"; cases++;
		r = run(v, 5, &f);
		CHECK(f == 3 && r.outcome == BTC_PAIR_OK && r.peer_bonded == 1 && r.connected == 0 && r.has_err == 1 && r.err == 4);
	}
	{	/* 12. bonded at start, no connect result before the timeout: OK at END with connected 0 */
		static const struct ev v[] = { { BTM_EV_START, 1, 1000 }, { BTM_EV_PEER_BONDED, 0, 1000 }, { BTM_EV_CONNECT_SENT, 0, 1000 }, { BTM_EV_END, 1, 61000 } };
		name = "bonded_at_start_silent"; cases++;
		r = run(v, 4, &f);
		CHECK(r.outcome == BTC_PAIR_OK && r.stage == BTC_PAIR_ST_BONDED && r.connected == 0 && r.peer_bonded == 1);
	}
	{	/* 13. a different device is bonded (no PEER_BONDED): the fresh path stays; a connect alone without a bond event is not "bonded peer" */
		static const struct ev v[] = { { BTM_EV_START, 1, 1000 }, { BTM_EV_END, 1, 61000 } };
		name = "other_bonded_no_peer"; cases++;
		r = run(v, 2, &f);
		CHECK(r.outcome == BTC_PAIR_NOPEER && r.peer_bonded == 0 && r.stage == BTC_PAIR_ST_NONE);
	}
	{	/* 9. attach accounting: PASS needs shared + available + powered after a request; the line carries counts only */
		struct btm_attach a = { 1, 1, 0, 1, 1200, 1, 1, 300, 1, 5, 3, 0 };
		char b[400];
		name = "attach"; cases++;
		CHECK(btm_attach_pass(&a));
		btm_attach_result(&a, b, sizeof b);
		CHECK(strcmp(b, "BT MGR: RESULT: PASS bt-mgr mg_bluetooth=1 shared=1 init_err=0 available=1 avail_ms=1200 powered=1 power_ms=300 scan=1 discovered=5 unique=3 gaps=0") == 0);
		a.powered = 0; CHECK(!btm_attach_pass(&a)); a.powered = 1;
		a.available = 0; CHECK(!btm_attach_pass(&a)); a.available = 1;
		a.power_req = 0; CHECK(!btm_attach_pass(&a)); a.power_req = 1;
		a.shared = 0; a.init_err = 9; CHECK(!btm_attach_pass(&a));
		btm_attach_result(&a, b, sizeof b);
		CHECK(strstr(b, "RESULT: FAIL bt-mgr") != NULL && strstr(b, "shared=0 init_err=9") != NULL);
	}
	printf("RESULT: %s bt_mgr_host_test cases=%d\n", fails ? "FAIL" : "PASS", cases);
	return fails != 0;
}
