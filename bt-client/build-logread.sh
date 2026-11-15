#!/bin/sh
# sprint-bt: build + ad-hoc sign bt-logread (bt_logread_ios.c) with bt-logread.entitlements; output never in git.
#   PIXOS_BT_LOGREAD_OUT (default ~/pixelios-artifacts/sprintbt/bt-logread)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
LB=${PIXOS_LOCALBUILD:-$HOME/pixelios-artifacts/localbuild/theos}
TC=$LB/toolchain/linux/iphone/bin
SDK=$LB/sdks/iPhoneOS16.5.sdk
SIGNER=${PIXOS_RCODESIGN:-$HOME/pixelios-artifacts/localbuild/rcodesign/target/release/rcodesign}
OUT=${PIXOS_BT_LOGREAD_OUT:-$HOME/pixelios-artifacts/sprintbt/bt-logread}
BUILD_ID=${PIXOS_BT_CLIENT_BUILD:-logread-$(date -u +%Y%m%dT%H%M%SZ)}
die() { printf 'bt-logread-build: FAIL %s\n' "$*"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
"$TC/clang" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -fno-stack-protector -fblocks -Wall -DBTC_BUILD="\"$BUILD_ID\"" -c "$here/bt_logread_ios.c" -o "$tmp/l.o" || die compile
"$TC/ld" -arch arm64 -platform_version ios 16.0 16.5 -syslibroot "$SDK" -e _main -o "$tmp/bt-logread" "$tmp/l.o" -lSystem -lobjc -framework Foundation -framework CoreFoundation || die ld
mkdir -p "$(dirname "$OUT")"; cp "$tmp/bt-logread" "$OUT"
"$SIGNER" sign --entitlements-xml-file "$here/bt-logread.entitlements" "$OUT" >"$tmp/sign.log" 2>&1 || { tail -5 "$tmp/sign.log"; die sign; }
python3 "$here/sigcheck.py" "$OUT" "$here/bt-logread.entitlements" bt-logread || die sigcheck
python3 "$here/callsite-check.py" "$OUT" || die callsite-check
echo "bt-logread-build: RESULT: PASS $OUT build=$BUILD_ID"
