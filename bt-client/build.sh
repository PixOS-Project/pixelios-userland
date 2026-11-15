#!/bin/sh
# T-86aa/T-f9ab: build and ad-hoc sign the bt-probe binary for the phone.
# Toolchain: $PIXOS_LOCALBUILD (default ~/pixelios-artifacts/localbuild/theos) holds clang-11 + ld64-609 + iPhoneOS16.5.sdk;
# the signer is a native rcodesign (cargo build -p rcodesign) in $PIXOS_RCODESIGN (default ~/pixelios-artifacts/localbuild/rcodesign).
# The output is the gitignored artifact $OUT (default ~/pixelios-artifacts/bt-client/bt-probe), never committed.
# Prints the binary path, its size, the arm64 Mach-O header and the sealed entitlements, or the exact failing step.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
LB=${PIXOS_LOCALBUILD:-$HOME/pixelios-artifacts/localbuild/theos}
TC=$LB/toolchain/linux/iphone/bin
SDK=$LB/sdks/iPhoneOS16.5.sdk
SIGNER=${PIXOS_RCODESIGN:-$HOME/pixelios-artifacts/localbuild/rcodesign/target/release/rcodesign}
OUT=${PIXOS_BT_CLIENT_OUT:-$HOME/pixelios-artifacts/bt-client/bt-probe}
BUILD_ID=${PIXOS_BT_CLIENT_BUILD:-t86aa-probe-$(date -u +%Y%m%dT%H%M%SZ)}

say() { printf 'bt-probe-build: %s\n' "$*"; }
die() { printf 'bt-probe-build: FAIL %s\n' "$*"; exit 1; }

[ -x "$TC/clang" ] || die "no clang at $TC/clang (set PIXOS_LOCALBUILD)"
[ -x "$TC/ld" ] || die "no ld64 at $TC/ld (set PIXOS_LOCALBUILD)"
[ -d "$SDK" ] || die "no iPhoneOS SDK at $SDK (set PIXOS_LOCALBUILD)"
[ -x "$SIGNER" ] || die "no signer at $SIGNER (set PIXOS_RCODESIGN)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in bt_client_core bt_client_ios bt_adv_ios bt_tone_ios; do
	"$TC/clang" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -fno-stack-protector -I"$here" \
		-DBTC_BUILD="\"$BUILD_ID\"" -c "$here/$f.c" -o "$tmp/$f.o" || die "compile $f.c"
done
"$TC/ld" -arch arm64 -platform_version ios 16.0 16.5 -syslibroot "$SDK" -e _main \
	-o "$tmp/bt-probe" "$tmp/bt_client_core.o" "$tmp/bt_client_ios.o" "$tmp/bt_adv_ios.o" "$tmp/bt_tone_ios.o" \
	-lSystem -lobjc -framework CoreBluetooth -framework AudioToolbox -framework AVFoundation -framework Foundation -framework CoreFoundation || die "ld"
magic=$(od -An -tx1 -N8 "$tmp/bt-probe" | tr -d ' \n')
[ "$magic" = "cffaedfe0c000001" ] || die "output is not a thin arm64 Mach-O (magic $magic)"
mkdir -p "$(dirname "$OUT")"
cp "$tmp/bt-probe" "$OUT"
"$SIGNER" sign --entitlements-xml-file "$here/bt-client.entitlements" "$OUT" >"$tmp/sign.log" 2>&1 \
	|| { tail -5 "$tmp/sign.log"; die "rcodesign sign (see log above)"; }
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
for i in range(struct.unpack_from('>I', b, 8)[0]):
    t, o = struct.unpack_from('>II', b, 12 + 8 * i)
    if t == 5:
        ln = struct.unpack_from('>I', b, o + 4)[0]
        ents = plistlib.loads(b[o + 8:o + ln])
        print('bt-probe-build: entitlements', ' '.join(sorted(ents)))
        sys.exit(0)
sys.exit(1)
PY
# every multi-argument objc_msgSend must pass its arguments in registers (a61d41c spilled them: bt-up16 boot 2 crash)
python3 "$here/callsite-check.py" "$OUT" || { rm -f "$OUT"; die "callsite-check (see lines above); $OUT removed"; }
say "built $OUT build=$BUILD_ID size=$(stat -c %s "$OUT")"
say "RESULT: PASS bt-probe-build"
