#!/bin/sh
# Install pixos-bluetool into an overlay tree and point ONLY the com.apple.BlueTool job at it.
#   overlay.sh TREE [BINARY]   (BINARY default ~/pixelios-artifacts/bluetool/pixos-bluetool, built by bluetool/build.sh)
# TREE/usr/local/libexec/pixos-bluetool (0755); TREE/System/Library/xpc/launchd.plist: job Label com.apple.BlueTool gets
# ProgramArguments ["/usr/local/libexec/pixos-bluetool"]. Refuses if the job is missing; prints that one key before/after and verifies
# nothing else in the plist changed (binary format kept). Writes nothing unless every check passed.
set -eu
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 TREE [BINARY]"; exit 2; }
tree=$1
bin=${2:-$HOME/pixelios-artifacts/bluetool/pixos-bluetool}
plist=$tree/System/Library/xpc/launchd.plist
die() { printf 'bluetool-overlay: FAIL %s\n' "$*"; exit 1; }
[ -d "$tree" ] || die "no tree $tree"
[ -f "$bin" ] || die "no binary $bin (run bluetool/build.sh)"
[ -f "$plist" ] || die "no $plist"
[ ! -L "$tree/usr/local/libexec" ] && [ ! -L "$plist" ] || die "symlink in the target path (writes would leave $tree)"
python3 -I - "$plist" "$bin" "$tree/usr/local/libexec/pixos-bluetool" <<'PY' || exit 1
import copy, os, plistlib, shutil, sys
plist, binp, dest = sys.argv[1:4]
want = ['/usr/local/libexec/pixos-bluetool']
raw = open(plist, 'rb').read()
cache = plistlib.loads(raw)
jobs = cache.get('LaunchDaemons', {})
hits = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.BlueTool']
if len(hits) != 1:
    print('bluetool-overlay: FAIL expected exactly one com.apple.BlueTool job, found %d' % len(hits)); sys.exit(1)
k = hits[0]
before = copy.deepcopy(cache)
old = jobs[k].get('ProgramArguments')
print('bluetool-overlay: %s ProgramArguments before: %r' % (k, old))
jobs[k]['ProgramArguments'] = want
# only that one key may differ
exp = copy.deepcopy(before)
exp['LaunchDaemons'][k]['ProgramArguments'] = want
if cache != exp or {x: y for x, y in cache['LaunchDaemons'][k].items() if x != 'ProgramArguments'} != \
        {x: y for x, y in before['LaunchDaemons'][k].items() if x != 'ProgramArguments'}:
    print('bluetool-overlay: FAIL another key changed'); sys.exit(1)
data = plistlib.dumps(cache, fmt=plistlib.FMT_BINARY)
back = plistlib.loads(data)
diffs = []
def walk(a, b, p):
    if isinstance(a, dict) and isinstance(b, dict):
        for x in set(a) | set(b):
            if x not in a or x not in b: diffs.append(p + '/' + x)
            else: walk(a[x], b[x], p + '/' + x)
    elif a != b: diffs.append(p)
walk(before, back, '')
assert diffs == ['/LaunchDaemons/%s/ProgramArguments' % k], diffs
os.makedirs(os.path.dirname(dest), exist_ok=True)
shutil.copyfile(binp, dest); os.chmod(dest, 0o755)
open(plist, 'wb').write(data)
print('bluetool-overlay: %s ProgramArguments after:  %r' % (k, back['LaunchDaemons'][k]['ProgramArguments']))
print('bluetool-overlay: verified: only /LaunchDaemons/%s/ProgramArguments differs; installed %s (0755)' % (k, dest))
PY
echo "bluetool-overlay: RESULT: PASS"
