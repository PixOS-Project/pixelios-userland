#!/bin/sh
# sprint-bt: build + ad-hoc sign bt-romget (no entitlements); output ~/pixelios-artifacts/sprintbt/bt-romget, never in git.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
LB=${PIXOS_LOCALBUILD:-$HOME/pixelios-artifacts/localbuild/theos}
TC=$LB/toolchain/linux/iphone/bin; SDK=$LB/sdks/iPhoneOS16.5.sdk
SIGNER=${PIXOS_RCODESIGN:-$HOME/pixelios-artifacts/localbuild/rcodesign/target/release/rcodesign}
OUT=${PIXOS_BT_ROMGET_OUT:-$HOME/pixelios-artifacts/sprintbt/bt-romget}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
"$TC/clang" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -c "$here/bt_romget_ios.c" -o "$tmp/r.o"
"$TC/ld" -arch arm64 -platform_version ios 16.0 16.5 -syslibroot "$SDK" -e _main -o "$tmp/bt-romget" "$tmp/r.o" -lSystem
cp "$tmp/bt-romget" "$OUT"; "$SIGNER" sign "$OUT" >"$tmp/s.log" 2>&1 || { tail -3 "$tmp/s.log"; exit 1; }
echo "bt-romget-build: RESULT: PASS $OUT"
