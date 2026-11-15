#!/bin/sh
# T-2d63: build and ad-hoc sign BTApp.app (UIKit app com.pixos.btapp, baked into the root at /Applications/BTApp.app by
# tools/sweet-root-tweaks.py, env PIXOS_BTAPP_BUNDLE) and btapp-launch (its launcher, shipped in the dev overlay by
# app-overlay.sh). Same toolchain and signer as build.sh. Outputs are gitignored artifacts, never committed:
#   $PIXOS_BTAPP_OUT (default ~/pixelios-artifacts/bt-client/app)/BTApp.app and .../btapp-launch
# Checks: thin arm64, sealed entitlements == the .entitlements files, the app's CodeDirectory identifier == com.pixos.btapp
# (sigcheck.py), every multi-argument objc_msgSend passes its arguments in registers (callsite-check.py), Info.plist keys.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
LB=${PIXOS_LOCALBUILD:-$HOME/pixelios-artifacts/localbuild/theos}
TC=$LB/toolchain/linux/iphone/bin
SDK=$LB/sdks/iPhoneOS16.5.sdk
SIGNER=${PIXOS_RCODESIGN:-$HOME/pixelios-artifacts/localbuild/rcodesign/target/release/rcodesign}
OUT=${PIXOS_BTAPP_OUT:-$HOME/pixelios-artifacts/bt-client/app}
BUILD_ID=${PIXOS_BT_CLIENT_BUILD:-t2d63-app-$(date -u +%Y%m%dT%H%M%SZ)}
BID=com.pixos.btapp

say() { printf 'btapp-build: %s\n' "$*"; }
die() { printf 'btapp-build: FAIL %s\n' "$*"; exit 1; }

[ -x "$TC/clang" ] || die "no clang at $TC/clang (set PIXOS_LOCALBUILD)"
[ -x "$TC/ld" ] || die "no ld64 at $TC/ld (set PIXOS_LOCALBUILD)"
[ -d "$SDK" ] || die "no iPhoneOS SDK at $SDK (set PIXOS_LOCALBUILD)"
[ -x "$SIGNER" ] || die "no signer at $SIGNER (set PIXOS_RCODESIGN)"
[ ! -e "$OUT/BTApp.app" ] || die "$OUT/BTApp.app exists (remove it or set PIXOS_BTAPP_OUT)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc() { "$TC/clang" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -fno-stack-protector -fblocks -I"$here" \
	-DBTC_BUILD="\"$BUILD_ID\"" "$@"; }
cc -DBTC_NO_MAIN -c "$here/bt_client_ios.c" -o "$tmp/bt_client_ios.o" || die "compile bt_client_ios.c"
for f in bt_client_core bt_app_core bt_app_ios btapp_launch_ios bt_mgr_core bt_mgr_ios; do
	cc -c "$here/$f.c" -o "$tmp/$f.o" || die "compile $f.c"
done
ld() { "$TC/ld" -arch arm64 -platform_version ios 16.0 16.5 -syslibroot "$SDK" -e _main "$@"; }
ld -o "$tmp/BTApp" "$tmp/bt_app_ios.o" "$tmp/bt_app_core.o" "$tmp/bt_client_core.o" "$tmp/bt_client_ios.o" "$tmp/bt_mgr_core.o" "$tmp/bt_mgr_ios.o" \
	-lSystem -lobjc -framework UIKit -framework CoreBluetooth -framework Foundation -framework CoreFoundation || die "ld BTApp"
ld -o "$tmp/btapp-launch" "$tmp/btapp_launch_ios.o" -lSystem -lobjc -framework Foundation -framework CoreFoundation || die "ld btapp-launch"

app=$tmp/BTApp.app
mkdir -p "$app"
cp "$tmp/BTApp" "$app/BTApp"
chmod 755 "$app/BTApp"
# Info.plist: CFBundleIdentifier is the one key the LS scan requires (T-9524 Q1.2.5); the rest mirror stock Settings (Q1.4.6);
# NSBluetoothAlwaysUsageDescription avoids tccd's missing-usage crash path (Q3.3). No UIApplicationSceneManifest: a legacy
# app-delegate app (UIKit still runs those on 16.x) [read: Apple "Managing your app's life cycle"; not observed here].
python3 - "$app/Info.plist" "$BID" "$BUILD_ID" <<'PY' || die "Info.plist"
import plistlib, sys
p, bid, build = sys.argv[1:4]
pl = {'CFBundleIdentifier': bid, 'CFBundleExecutable': 'BTApp', 'CFBundlePackageType': 'APPL', 'CFBundleName': 'BTApp',
      'CFBundleDisplayName': 'BT Scan', 'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0',
      'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleSupportedPlatforms': ['iPhoneOS'], 'MinimumOSVersion': '16.0',
      'LSRequiresIPhoneOS': True, 'UIDeviceFamily': [1], 'UIRequiredDeviceCapabilities': ['arm64'],
      'UISupportedInterfaceOrientations': ['UIInterfaceOrientationPortrait'], 'UILaunchStoryboardName': '',
      'DTPlatformName': 'iphoneos', 'DTPlatformVersion': '16.5', 'DTSDKName': 'iphoneos16.5',
      'NSBluetoothAlwaysUsageDescription': 'PixOS Bluetooth scan test (' + build + ')'}
plistlib.dump(pl, open(p, 'wb'), fmt=plistlib.FMT_BINARY)
print('btapp-build: Info.plist', len(pl), 'keys, CFBundleIdentifier', bid)
PY
# bundle signing: rcodesign takes the main executable's identifier from CFBundleIdentifier and seals Info.plist (CodeResources)
"$SIGNER" sign --entitlements-xml-file "$here/btapp.entitlements" "$app" >"$tmp/sign-app.log" 2>&1 \
	|| { tail -5 "$tmp/sign-app.log"; die "rcodesign sign BTApp.app"; }
cp "$tmp/btapp-launch" "$tmp/btapp-launch.s"
"$SIGNER" sign --binary-identifier btapp-launch --entitlements-xml-file "$here/btapp-launch.entitlements" "$tmp/btapp-launch.s" \
	>"$tmp/sign-l.log" 2>&1 || { tail -5 "$tmp/sign-l.log"; die "rcodesign sign btapp-launch"; }
python3 "$here/sigcheck.py" "$app/BTApp" "$here/btapp.entitlements" "$BID" || die "sigcheck BTApp"
python3 "$here/sigcheck.py" "$tmp/btapp-launch.s" "$here/btapp-launch.entitlements" btapp-launch || die "sigcheck btapp-launch"
python3 "$here/callsite-check.py" "$app/BTApp" || die "callsite-check BTApp"
python3 "$here/callsite-check.py" "$tmp/btapp-launch.s" || die "callsite-check btapp-launch"
mkdir -p "$OUT"
rm -f "$OUT/btapp-launch"
cp -a "$app" "$OUT/BTApp.app"
cp "$tmp/btapp-launch.s" "$OUT/btapp-launch"
chmod 755 "$OUT/btapp-launch" "$OUT/BTApp.app/BTApp"
say "built $OUT/BTApp.app ($(stat -c %s "$OUT/BTApp.app/BTApp") bytes) + $OUT/btapp-launch ($(stat -c %s "$OUT/btapp-launch") bytes) build=$BUILD_ID"
say "RESULT: PASS btapp-build"
