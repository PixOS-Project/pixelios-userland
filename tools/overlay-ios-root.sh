#!/bin/bash
# Put the GPU test binaries and their launchd jobs into a COPY of an existing iOS root image, without root (qemu guest, tools/rootless/vm.sh).
#
#   tools/overlay-ios-root.sh BASE.img OUT.img MALI_TEST GL_TEST
#
# BASE.img is left alone. OUT.img = BASE + /usr/local/bin/{sweetmali-test,sweetgl-test} + two jobs in System/Library/xpc/launchd.plist
# (keys are plist paths like the Apple jobs: a bare label is ignored by launchd, observed gputest1; the cache launchd reads with launchd_unsecure_cache=1; the plist is read out of BASE, so every other edit stays):
#   org.pixelios.sweetmali-{nomap,map,wc}  one SweetMali client run each, at load / +60 s / +120 s (the client is exclusive; mode = argv[1], see sweetmali_test.c)
#   org.pixelios.sweetgl-test              StartInterval 240
# Their stdout is /dev/null, which patch 0048 copies into the kernel/USB log as "|name| text" for the names in boot-arg
# sweetnull (the process name is cut at 16 chars, hence "sweetgl-test"): boot with  --args "sweetmali=1 sweetnull=sweetmali-test,sweetgl-test,backboardd,SpringBoard".
set -euo pipefail
cd "$(dirname "$0")/.."
PATH=$PATH:/usr/sbin:/sbin
[[ $# -eq 4 ]] || { sed -n '2,10p' "$0"; exit 2; }
base=$(realpath "$1") out=$2 mali=$(realpath "$3") gl=$(realpath "$4")
[[ -s $base && -x $mali && -x $gl ]] || { echo "missing input" >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
vm=tools/rootless/vm.sh
$vm fetch
w=$(mktemp -d -p "$(dirname "$out")" .overlay.XXXXXX)
trap 'rm -rf "$w"' EXIT
PLIST=System/Library/xpc/launchd.plist

truncate -s 16M "$w/get.bin"
IOS_APPEND="ios.path=$PLIST" $vm run get -m 512 "ro:$base" "$w/get.bin"

python3 -I - "$w" "$mali" "$gl" <<'EOF'
import os, plistlib, sys, tarfile
w, mali, gl = sys.argv[1:]
raw = open(w + '/get.bin', 'rb').read()
n = int(raw[:16])
plist = plistlib.loads(raw[16:16 + n])
jobs = plist['LaunchDaemons']
for k in [k for k, j in jobs.items() if j.get('Label', '').startswith('org.pixelios.')]:
    del jobs[k]                                   # rerun on an already overlaid image
T = '/usr/local/bin/sweetmali-test'
for label, args, extra in (('sweetmali-nomap', [T, 'nomap'], {'RunAtLoad': True}),
                           ('sweetmali-map', [T, 'map'], {'StartInterval': 60}),
                           ('sweetmali-wc', [T, 'wc'], {'StartInterval': 120}),
                           ('sweetgl-test', ['/usr/local/bin/sweetgl-test'], {'StartInterval': 240})):
    jobs['/System/Library/LaunchDaemons/org.pixelios.' + label + '.plist'] = dict(Label='org.pixelios.' + label, Program=args[0], ProgramArguments=args, UserName='root', **extra)
data = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)
with tarfile.open(w + '/overlay.tar', 'w', format=tarfile.GNU_FORMAT) as t:
    def add(name, payload, mode, isdir=False):
        ti = tarfile.TarInfo(name)
        ti.uid = ti.gid = 0
        ti.mode = mode
        if isdir:
            ti.type = tarfile.DIRTYPE
            t.addfile(ti)
        else:
            ti.size = len(payload)
            import io
            t.addfile(ti, io.BytesIO(payload))
    for d in ('usr', 'usr/local', 'usr/local/bin'):
        add(d, b'', 0o755, True)
    add('usr/local/bin/sweetmali-test', open(mali, 'rb').read(), 0o755)
    add('usr/local/bin/sweetgl-test', open(gl, 'rb').read(), 0o755)
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
