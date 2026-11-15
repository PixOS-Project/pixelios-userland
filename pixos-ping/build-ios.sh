#!/bin/sh
# Build and ad-hoc sign pixos-ping for the phone (arm64 iOS 16). Usage: build-ios.sh OUTDIR [BUILD_ID]
# Toolchain (not in git): THEOS_BIN holds ld (ld64-609) + ldid, IOS_SDK is iPhoneOS16.5.sdk; the defaults are where
# tools/xnu-local-build puts them (research 111 s.2). The linker output has NO signature; the ldid step is mandatory and the
# identifier is taken from the file name, so sign the final name.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-ios.sh OUTDIR [BUILD_ID]}
id=${2:-dev}
art=${PIXELIOS_ARTIFACTS:-$HOME/pixelios-artifacts}
T=${THEOS_BIN:-$art/localbuild/theos/toolchain/linux/iphone/bin}
SDK=${IOS_SDK:-$art/localbuild/theos/sdks/iPhoneOS16.5.sdk}
for f in "$T/ld" "$T/ldid" "$SDK/usr/include/netinet/ip_icmp.h"; do
	[ -e "$f" ] || { echo "RESULT: FAIL build-ios-ping missing $f (iOS SDK / theos toolchain, see research 111 s.2)"; exit 1; }
done
mkdir -p "$out"
${CC:-clang} -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -Wextra -Werror -DPP_BUILD="\"$id\"" -I"$here" \
	--ld-path="$T/ld" -o "$out/pixos-ping" "$here/pixos_ping_core.c" "$here/pixos_ping_sock.c" "$here/pixos_ping_main.c"
"$T/ldid" -Cadhoc -S "$out/pixos-ping"
echo "RESULT: PASS build-ios-ping $out/pixos-ping"
