#!/bin/sh
# sprint-bt: build and run the BluetoothManager core host test (bt_mgr_core.c: attach accounting + pair state machine).
# BTM_CORE=path runs it against another copy of bt_mgr_core.c (mutants). Prints RESULT: PASS|FAIL bt_mgr_host_test cases=N.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core=${BTM_CORE:-$here/bt_mgr_core.c}
out=${TMPDIR:-/tmp}/bt-mgr-host-test.$$
trap 'rm -f "$out"' EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -g -O1 -I"$here" -o "$out" "$core" "$here/bt_mgr_host_test.c"
rc=0
timeout -k 1 20 "$out" || rc=$?
exit "$rc"
