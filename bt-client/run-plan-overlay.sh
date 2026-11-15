#!/bin/sh
# T-86aa/T-f9ab: cp -a START OUT, install the signed PROBE_BINARY at usr/local/bin/bt-probe plus a one-shot launchd job
# (user mobile). The binary is validated BEFORE anything is created: its sealed entitlements must EQUAL the allowlist
# {application-identifier} (a missing and an extra key are both refused). Any later failure removes OUT again.
# run-plan-overlay.sh START OUT PROBE_BINARY.
set -eu
[ $# -eq 3 ] || { echo "usage: $0 START OUT PROBE_BINARY"; exit 2; }
start=$1; out=$2; probe=$3
say() { printf 'bt-probe-overlay: %s\n' "$*"; }
die() { printf 'bt-probe-overlay: FAIL %s\n' "$*"; exit 1; }
[ -d "$start" ] || die "no overlay start $start"
[ -f "$probe" ] || die "no probe binary $probe (run bt-client/build.sh)"
[ ! -e "$out" ] || die "$out exists (remove it or pick another name)"

# 1. validate the binary; creates nothing
python3 - "$probe" <<'PY' || die "binary check (nothing was created)"
import plistlib, struct, sys
ALLOW = {'application-identifier'}
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\xcf\xfa\xed\xfe' or struct.unpack_from('<I', d, 4)[0] != 0x0100000c:
    sys.exit('not an arm64 Mach-O')
n = struct.unpack_from('<I', d, 16)[0]
off, sig = 32, None
for _ in range(n):
    c, s = struct.unpack_from('<II', d, off)
    if c == 0x1d:
        sig = struct.unpack_from('<II', d, off + 8)
    off += s
if not sig:
    sys.exit('no code signature')
b = d[sig[0]:sig[0] + sig[1]]
ents = None
for i in range(struct.unpack_from('>I', b, 8)[0]):
    t, o = struct.unpack_from('>II', b, 12 + 8 * i)
    if t == 5:
        ents = plistlib.loads(b[o + 8:o + struct.unpack_from('>I', b, o + 4)[0]])
got = set(ents or {})
if got != ALLOW:
    sys.exit('sealed entitlements %s != allowlist %s (missing: %s; extra: %s)' % (
        sorted(got), sorted(ALLOW), ','.join(sorted(ALLOW - got)) or '-', ','.join(sorted(got - ALLOW)) or '-'))
print('bt-probe-overlay: entitlements', ' '.join(sorted(got)), '== allowlist')
PY

# 2. build OUT; from here on a failure removes it
mkdir -p "$out"
ok=0
trap '[ "$ok" = 1 ] || { rm -rf "$out"; printf "bt-probe-overlay: removed %s after the failure\n" "$out"; }' EXIT
cp -a "$start/." "$out/"
mkdir -p "$out/usr/local/bin"
[ -f "$out/System/Library/xpc/launchd.plist" ] || die "no System/Library/xpc/launchd.plist in $start (xrun allows jobs only there)"
cp "$probe" "$out/usr/local/bin/bt-probe"
chmod 755 "$out/usr/local/bin/bt-probe"
python3 - "$out/System/Library/xpc/launchd.plist" <<'PY' || die "job install"
import plistlib, sys
job = sys.argv[1]
# xrun's mkdevoverlay allows no file under System/Library/LaunchDaemons: the job goes into the launchd cache plist.
cache = plistlib.load(open(job, 'rb'))
key = '/System/Library/LaunchDaemons/com.pixos.bt-probe.plist'
# UserName mobile: the stock bluetoothd job and apps run as mobile (T-86aa review finding 5). No StandardOutPath, no KeepAlive.
cache.setdefault('LaunchDaemons', {})[key] = {'Label': 'com.pixos.bt-probe', 'UserName': 'mobile',
    'ProgramArguments': ['/usr/local/bin/bt-probe'], 'RunAtLoad': True, 'EnvironmentVariables': {'OS_ACTIVITY_DT_MODE': 'YES'}}
open(job, 'wb').write(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
print('bt-probe-overlay: job', key, 'in', job, 'daemons', len(cache['LaunchDaemons']))
PY
say "overlay $out (start $start + usr/local/bin/bt-probe)"
say "boot-arg: add bt-probe to sweetnull=..."
ok=1
say "RESULT: PASS bt-probe-overlay"
