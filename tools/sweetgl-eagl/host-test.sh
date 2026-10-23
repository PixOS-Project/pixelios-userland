#!/bin/sh
# Host test for eagl_core: cc -Wall -Wextra -Werror, then gcc + ASan/UBSan. Prints PASS or FAIL.
set -e
cd "$(dirname "$0")"
SRC="${EAGL_CORE_SRC:-eagl_core.c}"
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
cc -std=c11 -O1 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -I. "$SRC" eagl_core_test.c -o "$O/t" -lpthread
"$O/t"
gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -I. "$SRC" eagl_core_test.c -o "$O/ta" -lpthread
"$O/ta"
