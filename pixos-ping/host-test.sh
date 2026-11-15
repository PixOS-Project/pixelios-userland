#!/bin/sh
# Build and run the pixos-ping host test (needs only a C compiler). Prints RESULT: PASS|FAIL ping_host_test.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/pixos-ping-host-test.$$
trap 'rm -f "$out"' EXIT
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1 -I"$here" -o "$out" \
	"$here/pixos_ping_core.c" "$here/pixos_ping_sock.c" "$here/pixos_ping_host_test.c"
"$out"
