#!/bin/sh
# Host test for gli_table. Builds the COMMITTED gli_table_gen.c + gli_fake_gl.c. With HEADERS=<mesa>/include [SYMBOLS=file] it
# regenerates them first (gen_gli.py) and fails if the committed copies differ: that is the staleness check CI runs.
# Env overrides for gli-mutation-check.sh: GEN_PY, TABLE_C, SPECIAL_C.
set -e
cd "$(dirname "$0")"
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
GEN="${GEN_PY:-gen_gli.py}"; TC="${TABLE_C:-gli_table.c}"; SC="${SPECIAL_C:-gli_special.c}"
G=gli_table_gen.c; F=gli_fake_gl.c
if [ -n "$HEADERS" ]; then
	python3 -I "$GEN" --slots gli_slots.txt --headers "$HEADERS" ${SYMBOLS:+--symbols "$SYMBOLS"} --out "$O/$G" --fake-out "$O/$F" 2>"$O/gen.log" || { cat "$O/gen.log"; exit 1; }
	head -1 "$O/gen.log"
	if [ -z "$GEN_PY" ]; then cmp "$O/$G" $G && cmp "$O/$F" $F || { echo "STALE: committed generated files differ from gen_gli.py output"; exit 1; }; fi
	G="$O/$G"; F="$O/$F"
fi
SRC="$G $F $TC $SC gli_table_test.c"
cc -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I. $SRC -o "$O/t"
"$O/t"
gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -Wno-unused-parameter -I. $SRC -o "$O/ta"
"$O/ta"
