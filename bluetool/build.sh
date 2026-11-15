#!/bin/sh
# Build and ad-hoc sign pixos-bluetool for the phone, modelled on bt-client/build.sh.
# Toolchain: $PIXOS_LOCALBUILD (default ~/pixelios-artifacts/localbuild/theos) holds clang-11 + ld64-609 + iPhoneOS16.5.sdk;
# the signer is a native rcodesign in $PIXOS_RCODESIGN. Output: gitignored artifact $PIXOS_BLUETOOL_OUT
# (default ~/pixelios-artifacts/bluetool/pixos-bluetool), never committed. No entitlements are sealed (see bt_tool_ios.c).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
LB=${PIXOS_LOCALBUILD:-$HOME/pixelios-artifacts/localbuild/theos}
TC=$LB/toolchain/linux/iphone/bin
SDK=$LB/sdks/iPhoneOS16.5.sdk
SIGNER=${PIXOS_RCODESIGN:-$HOME/pixelios-artifacts/localbuild/rcodesign/target/release/rcodesign}
OUT=${PIXOS_BLUETOOL_OUT:-$HOME/pixelios-artifacts/bluetool/pixos-bluetool}
BUILD_ID=${PIXOS_BLUETOOL_BUILD:-bluetool-$(date -u +%Y%m%dT%H%M%SZ)}

say() { printf 'pixos-bluetool-build: %s\n' "$*"; }
die() { printf 'pixos-bluetool-build: FAIL %s\n' "$*"; exit 1; }

[ -x "$TC/clang" ] || die "no clang at $TC/clang (set PIXOS_LOCALBUILD)"
[ -x "$TC/ld" ] || die "no ld64 at $TC/ld (set PIXOS_LOCALBUILD)"
[ -d "$SDK" ] || die "no iPhoneOS SDK at $SDK (set PIXOS_LOCALBUILD)"
[ -x "$SIGNER" ] || die "no signer at $SIGNER (set PIXOS_RCODESIGN)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in bt_tool_core bt_tool_ios; do
	"$TC/clang" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -fno-stack-protector -fblocks -Wall -I"$here" \
		-DBTT_BUILD="\"$BUILD_ID\"" -c "$here/$f.c" -o "$tmp/$f.o" || die "compile $f.c"
done
"$TC/ld" -arch arm64 -platform_version ios 16.0 16.5 -syslibroot "$SDK" -e _main \
	-o "$tmp/pixos-bluetool" "$tmp/bt_tool_core.o" "$tmp/bt_tool_ios.o" -lSystem || die "ld"
magic=$(od -An -tx1 -N8 "$tmp/pixos-bluetool" | tr -d ' \n')
[ "$magic" = "cffaedfe0c000001" ] || die "output is not a thin arm64 Mach-O (magic $magic)"
mkdir -p "$(dirname "$OUT")"
cp "$tmp/pixos-bluetool" "$OUT"
"$SIGNER" sign "$OUT" >"$tmp/sign.log" 2>&1 || { tail -5 "$tmp/sign.log"; die "rcodesign sign (see log above)"; }
python3 - "$OUT" <<'PY' || die "signature check"
import plistlib, struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', d, 16)[0]
off, sig = 32, None
for _ in range(n):
    c, s = struct.unpack_from('<II', d, off)
    if c == 0x1d:
        sig = struct.unpack_from('<II', d, off + 8)
    off += s
assert sig, 'no LC_CODE_SIGNATURE'
b = d[sig[0]:sig[0] + sig[1]]
assert b[:4] == b'\xfa\xde\x0c\xc0', 'not a SuperBlob'
slots = [struct.unpack_from('>II', b, 12 + 8 * i)[0] for i in range(struct.unpack_from('>I', b, 8)[0])]
assert 0 in slots, 'no CodeDirectory'
assert 5 not in slots, 'unexpected sealed entitlements'
print('pixos-bluetool-build: signed, no entitlements sealed')
PY
# no objc_msgSend in this binary, so bt-client/callsite-check.py does not apply
say "built $OUT build=$BUILD_ID size=$(stat -c %s "$OUT")"
say "RESULT: PASS pixos-bluetool-build"
