#!/bin/bash
# Build fcap_replay (host, x86-64) against a native Mesa 26.2.5 with llvmpipe + softpipe, static. Runs INSIDE the pixos-mesa-host container:
#   podman run --rm --network none -v MESA:/work/mesa:ro -v USERLAND:/work/ml:ro -v OUT:/work/out -e JOBS=8 localhost/pixos-mesa-host:t44c1 \
#     bash /work/ml/tools/sweetgl-fcap/build-replay.sh
# MESA = the pristine 26.2.5 tree (no panfrost patches needed: the replay never touches panfrost). OUT caches the Mesa build (first run ~10 min).
# Last line: RESULT: PASS build-replay OUT/fcap_replay | RESULT: FAIL ...
set -eu
J=${JOBS:-8}
OUT=/work/out
M=/work/mesa
S=/work/ml/tools/sweetgl-fcap
E=/work/ml/tools/sweetgl-eagl
B=$OUT/build
say() { echo "== $*"; }
if [ ! -f "$B/build.ninja" ]; then
  say "meson setup"
  export CC=clang-19 CXX=clang++-19
  meson setup "$B" "$M" \
    --buildtype=debugoptimized -Ddefault_library=static --wrap-mode=nofallback \
    -Dgallium-drivers=llvmpipe,softpipe -Dvulkan-drivers= -Dllvm=enabled -Dmesa-clc=auto -Dprecomp-compiler=auto \
    -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dshared-glapi=disabled \
    -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dperfetto=false \
    -Dbuild-tests=false -Dgallium-rusticl=false -Dtools= -Dteflon=false \
    -Dgallium-va=disabled -Dvideo-codecs= -Dhtml-docs=disabled -Ddisplay-info=disabled 2>&1 | tail -30
fi
say "ninja (static libs)"
LIBS=$(ninja -C "$B" -t targets all | sed -n 's/^\(src\/.*\.a\): .*/\1/p')
# shellcheck disable=SC2086
ninja -k 0 -j"$J" -C "$B" $LIBS 2>&1 | tail -40 > "$OUT/ninja.tail" || true
BAD=$(grep '^FAILED:' "$OUT/ninja.tail" | grep -vE 'libloader|pipe_loader' || true)
tail -3 "$OUT/ninja.tail"
[ -z "$BAD" ] || { echo "$BAD"; echo "RESULT: FAIL mesa build"; exit 1; }
say "compile flags of lp_screen.c"
FLAGS=$(python3 - "$B" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1] + "/compile_commands.json")):
    if e["file"].endswith("src/gallium/drivers/llvmpipe/lp_screen.c"):
        a = shlex.split(e.get("command") or " ".join(e["arguments"]))[1:]
        out, skip = [], False
        for x in a:
            if skip: skip = False; continue
            if x in ("-o", "-MF", "-MQ", "-MT"): skip = True; continue
            if x in ("-c", "-MD") or x == e["file"] or x.endswith("lp_screen.c"): continue
            out.append(x)
        print(" ".join(shlex.quote(x) for x in out)); break
PY
)
[ -n "$FLAGS" ] || { echo "RESULT: FAIL no compile flags"; exit 1; }
cd "$B"
# shellcheck disable=SC2086
eval clang-19 $FLAGS -I"$E" -I"$M/src/gallium/drivers" -I"$M/src/gallium/winsys" -Wno-unused-parameter -c "$S/fcap_replay.c" -o "$OUT/fcap_replay.o"
# shellcheck disable=SC2086
clang++-19 "$OUT/fcap_replay.o" -Wl,--start-group $(find "$B/src" -name '*.a' ! -name 'libglsl_standalone.a' | sort) -Wl,--end-group \
  $(llvm-config --libs --ldflags) -ldrm -lz -lm -lpthread -ldl -o "$OUT/fcap_replay" 2>&1 | tail -30
[ -x "$OUT/fcap_replay" ] || { echo "RESULT: FAIL link"; exit 1; }
say "host test: the recorder on llvmpipe, then its capture replayed"
# shellcheck disable=SC2086
eval clang-19 $FLAGS -I"$E" -I"$S" -I"$M/src/gallium/drivers" -I"$M/src/gallium/winsys" -I"$M/src/gallium/auxiliary" -Wno-unused-parameter -Wno-unused-function \
  -c "$S/fcap_host_test.c" -o "$OUT/fcap_host_test.o"
# shellcheck disable=SC2086
clang++-19 "$OUT/fcap_host_test.o" -Wl,--start-group $(find "$B/src" -name '*.a' ! -name 'libglsl_standalone.a' | sort) -Wl,--end-group \
  $(llvm-config --libs --ldflags) -ldrm -lz -lm -lpthread -ldl -o "$OUT/fcap_host_test" 2>&1 | tail -20
T=$OUT/hosttest; rm -rf "$T"; mkdir -p "$T/root"
"$OUT/fcap_host_test" "$T/root" 2>&1 | tail -15
F=$(ls "$T"/root/fcap/*.fcap.000 2>/dev/null | head -1)
[ -n "$F" ] || { echo "RESULT: FAIL host test wrote no capture"; exit 1; }
cp "$F" "$T/cap.fcap"
"$OUT/fcap_replay" --trace --out "$T/out" "$T/cap.fcap" 2>&1 | tail -12
grep -q '"verdict":"MATCH"' "$T/out/report.jsonl" || { echo "RESULT: FAIL host test replay does not MATCH"; exit 1; }
echo "RESULT: PASS build-replay $OUT/fcap_replay (host test MATCH)"
