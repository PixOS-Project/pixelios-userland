#!/bin/sh
# T-86aa/T-f9ab: build and run the bt-client core host test (needs only a C compiler). Prints RESULT: PASS|FAIL bt_client_host_test.
# BTC_CORE=path runs the test against another copy of bt_client_core.c (mutant-test.sh).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core=${BTC_CORE:-$here/bt_client_core.c}
out=${TMPDIR:-/tmp}/bt-client-host-test.$$
trap 'rm -f "$out"' EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -Wno-override-init -Wno-missing-field-initializers -g -O1 \
	-I"$here" -o "$out" "$core" "$here/bt_client_host_test.c" -lm
"$out"
