#!/bin/sh
# Mutation check: apply each deliberate breakage to a COPY of sweet_drm_mali.c and require the host test to FAIL. Usage: mutation-check.sh MESA_DIR
set -u
M=${1:?usage: mutation-check.sh MESA_DIR}
H=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0
try() { # name, sed expression
	sed "$2" "$H/sweet_drm_mali.c" > "$T/m.c"
	if cmp -s "$H/sweet_drm_mali.c" "$T/m.c"; then echo "MUTANT $1: sed changed nothing"; bad=1; return; fi
	if SDM_SRC="$T/m.c" "$H/host-test.sh" "$M" "$T/out" > "$T/log" 2>&1; then
		echo "MUTANT $1: SURVIVED (test passed)"; bad=1
	else
		echo "MUTANT $1: killed ($(grep -c '^FAIL' "$T/log") failed checks, e.g. $(grep -m1 '^FAIL' "$T/log" | sed 's|.*host_test.c:||'))"
	fi
}
try wrong-param-id 's/case DRM_PANFROST_PARAM_GPU_PROD_ID: v = i->gpu_id >> 16/case DRM_PANFROST_PARAM_GPU_PROD_ID: v = i->gpu_id \& 0xffff/'
try wrong-req-bit  's/(r->requirements \& PANFROST_JD_REQ_FS) ? SWEET_MALI_REQ_FS : 0/(r->requirements \& PANFROST_JD_REQ_CYCLE_COUNT) ? SWEET_MALI_REQ_FS : 0/'
try dropped-seq    's/so_get(r->out_sync)->seq = seq\[0\];/so_get(r->out_sync)->seq = 0;/'
try off-by-one-handle 's/return h >= 1 \&\& h <= C.nbos \&\& C.bos\[h - 1\].used ? \&C.bos\[h - 1\] : NULL;/return h >= 1 \&\& h <= C.nbos \&\& C.bos[h].used ? \&C.bos[h] : NULL;/'
[ $bad = 0 ] && echo "all mutants killed" || { echo "MUTATION CHECK FAILED"; exit 1; }
