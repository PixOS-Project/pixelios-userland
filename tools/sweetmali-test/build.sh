#!/bin/sh
# Build sweetmali-test for iOS 16 arm64 with the local Xcode (macOS only; the Apple SDK never enters git). Output: tools/sweetmali-test/out/sweetmali-test
# The iPhoneOS SDK ships IOKit.framework without headers, so the headers come from the macOS SDK and the link uses the iOS stub.
set -e
H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$H/out"
MAC=$(xcrun -sdk macosx --show-sdk-path)
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall \
  -isystem "$MAC/System/Library/Frameworks/IOKit.framework/Headers" \
  -framework IOKit -o "$H/out/sweetmali-test" "$H/sweetmali_test.c"
codesign -f -s - "$H/out/sweetmali-test"
file "$H/out/sweetmali-test"
echo "built $H/out/sweetmali-test"
