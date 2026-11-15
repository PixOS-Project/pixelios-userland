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
# P-G1 slice 6 (T-2275): BO churn counters + cut; the knobs are read once per process, so one process per mode
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_bocut_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_bocut_test"
for m in cut off bad; do timeout 60 "$O/sweet_drm_mali_bocut_test" $m; done
# P-G1 slice 7 (T-bf4f): BO recycle pool + size classes
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_bopool_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_bopool_test"
for m in pool nocut off default; do timeout 60 "$O/sweet_drm_mali_bopool_test" $m; done
# P-G1 slice 10 (T-46d7): user-mode gap timing between kernel calls (SWEETGL_GAPT), fake clock; one process per mode
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_gapt_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_gapt_test"
for m in on off bad tbfail top nest walk; do timeout 60 "$O/sweet_drm_mali_gapt_test" $m; done
# P-G1 slice 11 (T-d815): call-site tags per gap; frame-pointer walk (the test functions are noinline, the build keeps frame pointers); a 16-row table for the "lost" mode
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer -DSWEET_DRM_HOST_TEST -DGAPT_TAB_N=16 -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_gapt_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_gapt_small"
timeout 60 "$O/sweet_drm_mali_gapt_small" lost
# P-G1 slice 12b (T-6717): Mesa time marks + GAPM rows (the test plays Mesa: sweet_gapt_mark between its own kernel calls)
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_gapm_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_gapm_test"
for m in on off bad; do timeout 60 "$O/sweet_drm_mali_gapm_test" $m; done
# P-G1 slice 12d (T-baf5): window pc sampler (SWEETGL_GAPW) with a fake kernel (suspend/get_state/resume hooks) and a fake clock; one process per mode
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_gapw_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_gapw_test"
for m in on off bad nogapt; do timeout 60 "$O/sweet_drm_mali_gapw_test" $m; done
# P-G1 slice 12e (T-bdc1): GAPW v2 with a 32-group print bound (reaches GAPWREST; the default build prints every group)
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer -DSWEET_DRM_HOST_TEST -DGAPW_TOP=32 -DGAPW_TOP_SMALL -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_gapw_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_gapw_small"
timeout 60 "$O/sweet_drm_mali_gapw_small" on
# T-9577: LAGPH lag-phase witness (flush exit -> first submit; fake wall/CPU clocks and cpu number); one process per mode
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -DSWEET_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_lagph_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_lagph_test"
for m in on off bad cap; do timeout 60 "$O/sweet_drm_mali_lagph_test" $m; done
# T-34eb: OVL pass-through of the shim (SWEET_MALI_REQ_OVL) and the overlap rule of Mesa patch 0008 (pan_sweet_ovl.h, R0-R4). OVL_DIR holds the header (default: the patched Mesa tree;
# mutation-check.sh points it at a broken copy)
OVLD=${OVL_DIR:-$M/src/gallium/drivers/panfrost}
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -DSWEET_DRM_HOST_TEST -I"$H" -I"$OVLD" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$SRC" "$H/sweet_drm_mali_ovl_test.c" "$H/fake_iokit.c" -o "$O/sweet_drm_mali_ovl_test"
for m in shim rule; do timeout 60 "$O/sweet_drm_mali_ovl_test" $m; done
