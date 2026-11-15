#!/bin/sh
# Build and run the wifid-probe host test (needs only a C compiler). Prints RESULT: PASS|FAIL wifid_probe_host_test.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/wifid-probe-host-test.$$
trap 'rm -f "$out"' EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -Wno-override-init -Wno-missing-field-initializers -g -O1 \
	-I"$here" -o "$out" "$here/wifid_probe_core.c" "$here/wifid_probe_host_test.c"
"$out"
