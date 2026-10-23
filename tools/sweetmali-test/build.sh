#!/bin/sh
# Build sweetmali-test for iOS 16 arm64 with the local Xcode (macOS only; the Apple SDK never enters git). Output: tools/sweetmali-test/out/sweetmali-test
set -e
H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$H/out"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Werror -framework IOKit -o "$H/out/sweetmali-test" "$H/sweetmali_test.c"
codesign -f -s - "$H/out/sweetmali-test"
echo "built $H/out/sweetmali-test"
