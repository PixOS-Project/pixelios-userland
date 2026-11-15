#!/bin/sh
# T-2d63: build and run the BTApp core host test (needs only a C compiler). Prints RESULT: PASS|FAIL bt_app_host_test.
# BTC_APP_CORE=path runs the test against another copy of bt_app_core.c (app-mutant-test.sh). bt_client_core.c gives btc_rescan_due.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core=${BTC_APP_CORE:-$here/bt_app_core.c}
out=${TMPDIR:-/tmp}/bt-app-host-test.$$
trap 'rm -f "$out"' EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -Wno-override-init -Wno-missing-field-initializers -g -O1 \
	-I"$here" -o "$out" "$core" "$here/bt_client_core.c" "$here/bt_app_host_test.c" -lm
# BTC_TEST_TIMEOUT=S: a mutant that loops forever counts as FAIL (app-mutant-test.sh sets it)
rc=0
${BTC_TEST_TIMEOUT:+timeout -k 1 "$BTC_TEST_TIMEOUT"} "$out" || rc=$?
[ "$rc" != 124 ] && [ "$rc" != 137 ] || { echo "RESULT: FAIL bt_app_host_test timeout"; exit 1; }
exit "$rc"
