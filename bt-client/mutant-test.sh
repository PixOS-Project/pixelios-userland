#!/bin/sh
# T-f9ab: the 5 core mutants of docs/reviews/T-86aa-review.md finding 6 (+ T-0936 rescan-every-state, + T-18e5 central_options, + T-2ae7 scan resume and arm B, + T-27d4 arm C), each applied with sed to a temp copy of
# bt_client_core.c; the host test must print RESULT: FAIL bt_client_host_test for every one (and the unmutated core PASS).
# Prints RESULT: PASS bt-client-mutants killed=N/N or RESULT: FAIL bt-client-mutants survived=<names>.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
core=$here/bt_client_core.c
# T-2ae7: each core_gaps++ carries a tag comment (gap:start, gap:rescan, gap:poweredon); the mutants address it by tag
for tag in start rescan poweredon armb armc armc-order adv-start adv-poweredon adv-lost adv-addtimeout adv-starttimeout adv-addcall adv-advcall tone-session tone-start tone-qerr tone-stall tone-few; do
	[ "$(grep -c "core_gaps++;	/\* gap:$tag \*/" "$core")" = 1 ] || { echo "RESULT: FAIL bt-client-mutants cannot find core_gaps++ tag gap:$tag"; exit 1; }
done
"$here/host-test.sh" | tail -1
survived=
n=0
run() {	# name sed-script
	name=$1; shift
	sed "$@" "$core" >"$tmp/$name.c"
	cmp -s "$core" "$tmp/$name.c" && { echo "mutant $name: sed changed nothing"; survived="$survived $name(nochange)"; return; }
	printf 'mutant %s: ' "$name"
	res=$(BTC_CORE="$tmp/$name.c" "$here/host-test.sh" | tail -1) || true
	echo "$res"
	case "$res" in
	"RESULT: FAIL bt_client_host_test"*) n=$((n + 1)) ;;
	*) survived="$survived $name" ;;
	esac
}
run drop-start-gap -e '/gap:start/s/core_gaps++;/;/'
run drop-poweredon-gap -e '/gap:poweredon/s/core_gaps++;/;/'
run drop-rc-guard -e 's/rc == 0 && s\.state/s.state/'
run pass-on-discovered -e 's/s\.unique >= 1/s.discovered >= 1/g'
run drop-backend-gaps -e 's/s\.gaps + core_gaps/core_gaps/g'
run rescan-every-state -e "s/prev_state != BTC_STATE_POWERED_ON;/prev_state != -99;/"
# T-18e5: the central_options knob in the start line (both mutants are killed by the start_failure / no_gaps_no_options cases)
run drop-options-rc-guard -e 's/opts = rc == 0 ? o->central_options(o->ctx) : 0;/opts = o->central_options(o->ctx);/'
run options-const -e 's/opts = rc == 0 ? o->central_options(o->ctx) : 0;/opts = rc == 0 ? "x=0" : 0;/'
# T-2ae7: scan resume decisions (killed by scan_lost_is_resumed / scan_resume_fails / scan_resume_skipped / the rescan_due table)
run rescan-ignores-scanning -e 's/\&\& scanning == 0;/\&\& scanning != -99;/'
run rescan-ignores-state -e 's/return state == BTC_STATE_POWERED_ON \&\& scanning == 0;/return state != -99 \&\& scanning == 0;/'
run drop-rescan-gap -e '/gap:rescan/s/core_gaps++;/;/'
run skip-as-resumed -e 's/if (k > 0) {/if (k >= 0) {/'
run no-rescan-call -e 's/if (btc_rescan_due(s.state, s.scanning)) {/if (0) {/'
# T-2ae7 arm B: switch guard, per-arm deltas, gap, not-poweredOn answer, no call without a central
run drop-armb-gap -e '/gap:armb/s/core_gaps++;/;/'
run armb-ignores-rc -e 's/if (armb_at_s \&\& t == armb_at_s \&\& rc == 0) {/if (armb_at_s \&\& t == armb_at_s) {/'
run armb-no-delta -e 's/bdesc, s\.discovered - b_disc, s\.unique - b_uniq);/bdesc, s.discovered, s.unique);/'
run armb-never -e 's/if (armb_at_s \&\& t == armb_at_s \&\& rc == 0) {/if (0) {/'
run armb-zero-is-gap -e 's/if (k >= 0) {/if (k > 0) {/'
# T-27d4 arm C: switch guard, order guard, per-arm deltas (C ends where B starts), gap, not-poweredOn answer
run drop-armc-gap -e '/gap:armc \*/s/core_gaps++;/;/'
run drop-armc-order-gap -e '/gap:armc-order/s/core_gaps++;/;/'
run armc-order-ignored -e 's/armc_at_s >= armb_at_s) {/armc_at_s >= armb_at_s \&\& 0) {/'
run armc-ignores-rc -e 's/if (armc_at_s \&\& t == armc_at_s \&\& rc == 0) {/if (armc_at_s \&\& t == armc_at_s) {/'
run armc-never -e 's/if (armc_at_s \&\& t == armc_at_s \&\& rc == 0) {/if (0) {/'
run armc-zero-is-gap -e '/k = o->arm_c/,/armc = 1;/s/if (k >= 0) {/if (k > 0) {/'
run armc-to-end -e 's/cdesc, b_disc - c_disc, b_uniq - c_uniq);/cdesc, s.discovered - c_disc, s.unique - c_uniq);/'
run armc-no-delta -e 's/cdesc, b_disc - c_disc, b_uniq - c_uniq);/cdesc, b_disc, b_uniq);/'
run armc-order-strict -e 's/armc_at_s >= armb_at_s) {/armc_at_s > armb_at_s) {/'
run armb-gap-ignores-c -e 's/armc == 1 ? cdesc : "nil");/"nil");/'
run armb-from-a -e 's/^\t\t\tb_disc = s\.discovered;/\t\t\tb_disc = a_disc;/'
# bt-proper adv: result decision, teardown, gaps, delay
run adv-drop-start-gap -e '/gap:adv-start/s/core_gaps++;/;/'
run adv-drop-poweredon-gap -e '/gap:adv-poweredon/s/core_gaps++;/;/'
run adv-drop-lost-gap -e '/gap:adv-lost/s/core_gaps++;/;/'
run adv-drop-addtimeout-gap -e '/gap:adv-addtimeout/s/core_gaps++;/;/'
run adv-drop-starttimeout-gap -e '/gap:adv-starttimeout/s/core_gaps++;/;/'
run adv-drop-addcall-gap -e '/gap:adv-addcall/s/core_gaps++;/;/'
run adv-drop-advcall-gap -e '/gap:adv-advcall/s/core_gaps++;/;/'
run adv-lost-still-passes -e 's/reason = "advertising-stopped";/reason = 0;/'
run adv-no-stop -e 's/^\t\to->stop(o->ctx);/\t\t;/'
run adv-stop-always -e 's/if (started_pm) {/if (1 || started_pm) {/'
run adv-start-error-passes -e 's/if (s\.adv < 0) {/if (0) {/'
run adv-add-error-passes -e 's/if (s\.svc < 0) {/if (0) {/'
run adv-ignores-poweredon -e 's/if (s\.state == BTC_STATE_POWERED_ON)$/if (1)/'
run adv-delay-ignored -e 's/if (delay_s) {/if (0) {/'
run adv-seconds-zero -e 's/^\tdone = secs;/\tdone = 0;/'
run adv-reads-dropped -e 's/secs_done, s->reads, s->centrals,$/secs_done, 0u, s->centrals,/'
run adv-parse-allows-zero -e 's/\*secs >= 1/1/'
# bt-proper tone: sine, result decision, teardown, gaps, delay, parser
run tone-drop-session-gap -e '/gap:tone-session/s/core_gaps++;/;/'
run tone-drop-start-gap -e '/gap:tone-start/s/core_gaps++;/;/'
run tone-drop-qerr-gap -e '/gap:tone-qerr/s/core_gaps++;/;/'
run tone-drop-stall-gap -e '/gap:tone-stall/s/core_gaps++;/;/'
run tone-drop-few-gap -e '/gap:tone-few/s/core_gaps++;/;/'
run tone-qerr-ignored -e 's/if (s\.qerr != 0) {/if (0) {/'
run tone-stall-ignored -e 's/if (stall >= BTTONE_STALL_S) {/if (0) {/'
run tone-stall-never-resets -e 's/stall = s\.refills == last_refills ? stall + 1 : 0;/stall = s.refills == last_refills ? stall + 1 : stall;/'
run tone-few-ignored -e 's/s\.refills < bttone_min_refills(secs)) {/0) {/'
run tone-floor-doubled -e 's/BTTONE_BUF_FRAMES) \/ 2);/BTTONE_BUF_FRAMES) * 2);/'
run tone-no-stop -e '/BT TONE: route after/,/BT TONE: stopped/s/o->stop(o->ctx);/;/'
run tone-delay-ignored -e '/BT TONE: start build/,/attempted = 1/s/if (delay_s) {/if (0) {/'
run tone-seconds-zero -e 's/done = t;/done = 0;/'
run tone-no-route-after -e 's/^\t\to->route(o->ctx, route, sizeof route);/\t\t;/'
run tone-amp-wrong -e 's/(double)BTTONE_AMP \* sin(ph)/(double)BTTONE_AMP * 0.5 * sin(ph)/'
run tone-freq-wrong -e 's/2\.0 \* M_PI \* (double)BTTONE_HZ/2.0 * M_PI * 441.0/'
run tone-mono -e 's/out\[2 \* i + 1\] = v;/out[2 * i + 1] = 0;/'
run tone-no-wrap -e 's/\*pos = (\*pos + 1) % BTTONE_RATE;/*pos = *pos + 1;/'
run tone-parse-allows-zero -e 's/parse_uint(argv\[i\], 3600, secs) == 0 \&\& \*secs >= 1/parse_uint(argv[i], 3600, secs) == 0/'
# bt-proper: bonded-at-start pair step in bt_mgr_core.c (killed by mgr-host-test.sh cases bonded_at_start_*)
runm() {	# name sed-args
	name=$1; shift
	sed "$@" "$here/bt_mgr_core.c" >"$tmp/$name.c"
	cmp -s "$here/bt_mgr_core.c" "$tmp/$name.c" && { echo "mutant $name: sed changed nothing"; survived="$survived $name(nochange)"; return; }
	printf 'mutant %s: ' "$name"
	res=$(BTM_CORE="$tmp/$name.c" "$here/mgr-host-test.sh" 2>&1 | tail -1) || true
	echo "$res"
	case "$res" in
	"RESULT: FAIL bt_mgr_host_test"*) n=$((n + 1)) ;;
	*) survived="$survived $name" ;;
	esac
}
runm mgr-bonded-connect-no-finish -e 's/if (s->r.peer_bonded \&\& !s->r.prompt)/if (0)/'
runm mgr-bonded-connfail-is-fail -e 's/if (s->r.peer_bonded \&\& ev == BTM_EV_CONNECT_FAIL) {/if (0) {/'
runm mgr-bonded-end-timeout -e 's/if (!s->done \&\& s->r.peer_bonded) {/if (0) {/'
runm mgr-bonded-no-flag -e 's/s->r.peer_bonded = 1;/;/'
runm mgr-bonded-no-stage -e 's/stage(s, BTC_PAIR_ST_BONDED);/;/'
total=70
if [ "$n" = $total ] && [ -z "$survived" ]; then
	echo "RESULT: PASS bt-client-mutants killed=$n/$total"
else
	echo "RESULT: FAIL bt-client-mutants killed=$n/$total survived:$survived"
	exit 1
fi
