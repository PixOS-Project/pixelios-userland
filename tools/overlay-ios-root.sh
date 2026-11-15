#!/bin/bash
# Put the GPU test binaries and their launchd jobs into a COPY of an existing iOS root image, without root (qemu guest, tools/rootless/vm.sh).
#
#   tools/overlay-ios-root.sh BASE.img OUT.img MALI_TEST GL_TEST [SWEETGL_DYLIB]
#
# With SWEETGL_DYLIB (tools/sweetgl-eagl/INJECT.md s.6) the image gets /usr/lib/libsweetgl.dylib (755) and DYLD_INSERT_LIBRARIES in the
# com.apple.backboardd job; the test binaries and the org.pixelios.* jobs are NOT installed (the SweetMali client is exclusive, backboardd's
# libsweetgl opens it), but MALI_TEST and GL_TEST stay required positional arguments. Boot with  --args "sweetgpu=... sweetmali=1 sweetnull=backboardd,SpringBoard".
#
# BASE.img is left alone. OUT.img = BASE + /usr/local/bin/{sweetmali-test,sweetgl-test} + two jobs in System/Library/xpc/launchd.plist
# (keys are plist paths like the Apple jobs: a bare label is ignored by launchd, observed gputest1; the cache launchd reads with launchd_unsecure_cache=1; the plist is read out of BASE, so every other edit stays):
#   org.pixelios.sweetmali-{nomap,map,wc}  one SweetMali client run each, at load / +60 s / +120 s (the client is exclusive; mode = argv[1], see sweetmali_test.c)
#   org.pixelios.sweetmali-nonroot         `sweetmali-test nonroot` as UserName=mobile (uid 501), StartInterval 90: must be REFUSED by the root-only gate of patch 0105
#   org.pixelios.sweetgl-test              StartInterval 240
# Timeline (s after load, launchd StartInterval repeats): nomap 0 | map 60,120.. | nonroot 90,180.. | wc 120,240.. | sweetgl-test 240.. (nonroot never holds the client, so overlaps do not matter for it).
# Their stdout is /dev/null, which patch 0048 copies into the kernel/USB log as "|name| text" for the names in boot-arg
# sweetnull (the process name is cut at 16 chars, hence "sweetgl-test"): boot with  --args "sweetmali=1 sweetnull=sweetmali-test,sweetgl-test,backboardd,SpringBoard".
set -euo pipefail
cd "$(dirname "$0")/.."
PATH=$PATH:/usr/sbin:/sbin
[[ $# -eq 4 || $# -eq 5 ]] || { sed -n '2,18p' "$0"; exit 2; }
base=$(realpath "$1") out=$2 mali=$(realpath "$3") gl=$(realpath "$4")
dylib=${5:+$(realpath "$5")}
[[ -s $base && -x $mali && -x $gl && ( -z $dylib || -s $dylib ) ]] || { echo "missing input" >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
vm=tools/rootless/vm.sh
$vm fetch
w=$(mktemp -d -p "$(dirname "$out")" .overlay.XXXXXX)
trap 'rm -rf "$w"' EXIT
PLIST=System/Library/xpc/launchd.plist

truncate -s 16M "$w/get.bin"
IOS_APPEND="ios.path=$PLIST" $vm run get -m 512 "ro:$base" "$w/get.bin"

python3 -I - "$w" "$mali" "$gl" "$dylib" <<'EOF'
import os, plistlib, sys, tarfile
w, mali, gl, dylib = sys.argv[1:]
raw = open(w + '/get.bin', 'rb').read()
n = int(raw[:16])
plist = plistlib.loads(raw[16:16 + n])
jobs = plist['LaunchDaemons']
for k in [k for k, j in jobs.items() if j.get('Label', '').startswith('org.pixelios.')]:
    del jobs[k]                                   # rerun on an already overlaid image
T = '/usr/local/bin/sweetmali-test'
for label, args, extra in () if dylib else (('sweetmali-nomap', [T, 'nomap'], {'RunAtLoad': True}),
                           ('sweetmali-map', [T, 'map'], {'StartInterval': 60}),
                           ('sweetmali-nonroot', [T, 'nonroot'], {'StartInterval': 90, 'UserName': 'mobile', 'GroupName': 'mobile'}),
                           ('sweetmali-wc', [T, 'wc'], {'StartInterval': 120}),
                           ('sweetgl-test', ['/usr/local/bin/sweetgl-test'], {'StartInterval': 240})):
    jobs['/System/Library/LaunchDaemons/org.pixelios.' + label + '.plist'] = dict(Label='org.pixelios.' + label, Program=args[0], ProgramArguments=args, **{'UserName': 'root', **extra})
if dylib:                                         # INJECT.md s.6: backboardd env, idempotent
    frag = plistlib.loads(open('tools/sweetgl-eagl/com.pixelios.backboardd-sweetgl.plist.in', 'rb').read().replace(b'@DYLIB@', b'/usr/lib/libsweetgl.dylib'))
    hit = [j for j in jobs.values() if j.get('Label') == 'com.apple.backboardd']
    assert len(hit) == 1, 'backboardd job count %d' % len(hit)
    hit[0].setdefault('EnvironmentVariables', {}).update(frag)
data = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)
with tarfile.open(w + '/overlay.tar', 'w', format=tarfile.GNU_FORMAT) as t:
    def add(name, payload, mode, isdir=False, owner=0):
        ti = tarfile.TarInfo(name)
        ti.uid = ti.gid = owner
        ti.mode = mode
        if isdir:
            ti.type = tarfile.DIRTYPE
            t.addfile(ti)
        else:
            ti.size = len(payload)
            import io
            t.addfile(ti, io.BytesIO(payload))
    if dylib:                                     # no dir entries: usr/lib keeps its mode
        add('usr/lib/libsweetgl.dylib', open(dylib, 'rb').read(), 0o755)
    else:
        for d in ('usr', 'usr/local', 'usr/local/bin'):
            add(d, b'', 0o755, True)
        add('usr/local/bin/sweetmali-test', open(mali, 'rb').read(), 0o755)
        add('usr/local/bin/sweetgl-test', open(gl, 'rb').read(), 0o755)
    add('private/var/mobile/tmp', b'', 0o755, True, 501)   # posterkit mkdtemp errno=2 (gputest20); parents exist, only the leaf is listed
    add(os.environ.get('PLIST', 'System/Library/xpc/launchd.plist'), data, 0o644)
print('overlay: %d jobs, plist %d bytes' % (len(jobs), len(data)))
EOF

cp --sparse=always "$base" "$out.partial"
$vm run overlay -m 512 "$out.partial" "ro:$w/overlay.tar"
fsck.hfsplus -f -y "$out.partial" | grep -v 'Illegal name' | tail -3 || true
fsck.hfsplus -f -n "$out.partial" >/dev/null || { rm -f "$out.partial"; echo "error: $out is not clean: do NOT flash it" >&2; exit 1; }
python3 -I tools/check-ios-root.py "$out.partial" || { rm -f "$out.partial"; echo "error: failed tools/check-ios-root.py: do NOT flash it" >&2; exit 1; }
mv "$out.partial" "$out"
sha256sum "$out" | tee "$out.sha256"
