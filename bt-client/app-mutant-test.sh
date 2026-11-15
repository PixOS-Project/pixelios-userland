#!/bin/sh
# T-2d63: core mutants of bt_app_core.c, each applied with sed to a temp copy; app-host-test.sh must print
# RESULT: FAIL bt_app_host_test for every one (and the unmutated core PASS). Prints RESULT: PASS bt-app-mutants killed=N/N.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
core=$here/bt_app_core.c
for tag in start clock windows rescan poweredon back connect connect-never; do
	[ "$(grep -c "core_gaps++;	/\* gap:$tag \*/" "$core")" = 1 ] || { echo "RESULT: FAIL bt-app-mutants cannot find core_gaps++ tag gap:$tag"; exit 1; }
done
base=$("$here/app-host-test.sh" | tail -1)
[ "$base" = "${base#RESULT: PASS}" ] && { echo "$base"; echo "RESULT: FAIL bt-app-mutants unmutated core does not pass"; exit 1; }
survived=
n=0
total=0
run() {	# name sed-args
	name=$1; shift
	total=$((total + 1))
	sed "$@" "$core" >"$tmp/$name.c"
	cmp -s "$core" "$tmp/$name.c" && { echo "mutant $name: sed changed nothing"; survived="$survived $name(nochange)"; return; }
	printf 'mutant %s: ' "$name"
	res=$(BTC_TEST_TIMEOUT=20 BTC_APP_CORE="$tmp/$name.c" "$here/app-host-test.sh" 2>&1 | tail -1) || true
	echo "$res"
	case "$res" in
	"RESULT: FAIL bt_app_host_test"*) n=$((n + 1)) ;;
	*) survived="$survived $name" ;;
	esac
}
run drop-start-gap -e '/gap:start/s/core_gaps++;/;/'
run drop-clock-gap -e '/gap:clock/s/core_gaps++;/;/'
run drop-windows-gap -e '/gap:windows/s/core_gaps++;/;/'
run drop-rescan-gap -e '/gap:rescan/s/core_gaps++;/;/'
run drop-back-gap -e '/gap:back/s/core_gaps++;/;/'
run wrap-before-start -e 's/unsigned rt = tr.t_s >= t_start ? tr.t_s - t_start : 0;/unsigned rt = tr.t_s - t_start;/'
run drop-poweredon-gap -e '/gap:poweredon/s/core_gaps++;/;/'
run drop-backend-gaps -e 's/s\.gaps + core_gaps/core_gaps/g'
run drop-side-gaps -e 's/ + (ao->side_gaps ? ao->side_gaps(o->ctx) : 0)//'
run pass-on-any -e 's/fg >= 1 ? "pass" : "fail"/s.discovered >= 1 ? "pass" : "fail"/' -e 's/return fg >= 1 ? 0 : 1;/return s.discovered >= 1 ? 0 : 1;/'
run bg-counts-as-fg -e 's/if (w\[k\]\.state == BTC_APP_ACTIVE)/if (w[k].state == BTC_APP_ACTIVE || w[k].state == BTC_APP_BACKGROUND)/'
run repeat-is-window -e 's/if (tr\.state == w\[nw - 1\]\.state)/if (0)/'
run overflow-keeps-label -e 's/w\[nw - 1\]\.state = BTC_APP_MIXED;/;/'
run no-delta -e 's/w\[k\]\.d1 - w\[k\]\.d0 : 0/w[k].d1 : 0/'
run last-window-not-closed -e 's/^\tw\[nw - 1\]\.d1 = s\.discovered;/\tw[nw - 1].d1 = w[nw - 1].d0;/'
run abs-time -e 's/t = ao->now_s(o->ctx) - t_start;/t = ao->now_s(o->ctx);/'
run no-rescan -e 's/if (btc_rescan_due(s\.state, s\.scanning)) {/if (0) {/'
run opts-ignore-rc -e 's/opts = rc == 0 ? o->central_options(o->ctx) : 0;/opts = o->central_options(o->ctx);/'
run no-clock-cap -e 's/if (++iter > max_iter) {/if (++iter > max_iter * 1000) {/'
run poweredon-ignores-rc -e 's/if (rc == 0 \&\& s\.state != BTC_STATE_POWERED_ON) {/if (s.state != BTC_STATE_POWERED_ON) {/'
# T-3615 connect step
run drop-connect-gap -e '/gap:connect \*\//s/core_gaps++;/;/'
run drop-connect-never-gap -e '/gap:connect-never/s/core_gaps++;/;/'
run connect-in-bg -e 's/while (ao->connect \&\& !conn_done \&\& w\[nw - 1\]\.state == BTC_APP_ACTIVE \&\& t >= ao->conn_at_s)/while (ao->connect \&\& !conn_done \&\& t >= ao->conn_at_s)/'
run connect-too-early -e 's/t >= ao->conn_at_s) {/t + 5 >= ao->conn_at_s) {/'
run no-stop-after-ok -e 's/conn_done = 1;	\/\* one connection/;	\/* one connection/'
run no-try-cap -e 's/if (conn_att >= ao->conn_tries)/if (0)/'
run count-nocand-attempt -e 's/^\t\t\tconn_att++;/\t\t\tconn_att += 0;/'
run pass-without-ok -e 's/conn_ok ? "PASS" : "FAIL"/conn_att ? "PASS" : "FAIL"/'
run first-fail-lost -e 's/} else if (conn_first < 0) {/} else if (0) {/'
# bonded-at-start pair step (bt-proper): the tag exists once, and the bonded branch / reconnect rule are killed
[ "$(grep -c "core_gaps++;	/\* gap:pair-noreconnect \*/" "$core")" = 1 ] || { echo "RESULT: FAIL bt-app-mutants cannot find core_gaps++ tag gap:pair-noreconnect"; exit 1; }
run drop-pair-noreconnect-gap -e '/gap:pair-noreconnect/s/core_gaps++;/;/'
run bonded-branch-off -e 's/if (p.peer_bonded \&\& p.bonded_start >= 1) {/if (0) {/'
run bonded-reconnect-ignores-connected -e 's/if (p.connected \&\& !p.prompt) {/if (!p.prompt) {/'
run bonded-no-bonded-flag -e '/the bond survived/s/pair_bonded = 1;/;/'
if [ "$n" = "$total" ] && [ -z "$survived" ]; then
	echo "RESULT: PASS bt-app-mutants killed=$n/$total"
else
	echo "RESULT: FAIL bt-app-mutants killed=$n/$total survived:$survived"
	exit 1
fi
