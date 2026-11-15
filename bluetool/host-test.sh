#!/bin/sh
# pixos-bluetool host test: the portable core against a fake controller and a fake XPC dictionary layer.
# BTT_CORE=path runs it against another copy of the core (mutants). Prints RESULT: PASS|FAIL pixos-bluetool-host cases=N.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core=${BTT_CORE:-$here/bt_tool_core.c}
out=${TMPDIR:-/tmp}/pixos-bluetool-host-test.$$
trap 'rm -f "$out"' EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -g -O1 -I"$here" -o "$out" "$core" "$here/bt_tool_host_test.c"
rc=0
timeout -k 1 20 "$out" || rc=$?
exit "$rc"
