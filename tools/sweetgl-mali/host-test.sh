#!/bin/sh
# Build and run the host test with gcc/clang on Linux (fake IOKit, no Apple headers). Usage: host-test.sh MESA_DIR [OUT_DIR]
# MESA_DIR: Mesa 26.2.5 with patches 0001-0004 of tools/mesa-patches applied (it only supplies headers: drm-uapi, xf86drm.h, darwin_drm.h, os_time.h).
set -eu
M=${1:?usage: host-test.sh MESA_DIR [OUT_DIR]}
H=$(cd "$(dirname "$0")" && pwd)
O=${2:-${TMPDIR:-/tmp}/sweetgl-mali-host}
SRC=${SDM_SRC:-$H/sweet_drm_mali.c}      # mutation-check.sh points this at a broken copy
mkdir -p "$O"
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_host_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_host_test"
timeout 60 "$O/sweet_drm_mali_host_test"
