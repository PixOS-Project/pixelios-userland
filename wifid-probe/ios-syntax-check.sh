#!/bin/sh
# Syntax/type check of the iOS backend on a host with no iOS SDK (it only uses libc and dlfcn).
# This does NOT prove the iOS build: that needs the iOS SDK (see the T-9f3b Outcome).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
${CC:-clang} -std=gnu99 -Wall -Wextra -Werror -fsyntax-only -I"$here" "$here/wifid_probe_ios.c" "$here/wifid_probe_core.c"
echo "ios backend syntax ok (host libc only; not an iOS build)"
