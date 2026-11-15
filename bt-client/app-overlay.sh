#!/bin/sh
# T-2d63: cp -a START OUT, drop the bt-probe daemon client (binary + com.pixos.bt-probe job: one knob, the app is the only
# CoreBluetooth client this run adds), install the signed LAUNCHER at usr/local/bin/btapp-launch plus a one-shot launchd job
# (user mobile, RunAtLoad). The launcher is validated BEFORE anything is created (sigcheck.py: sealed entitlements EQUAL
# btapp-launch.entitlements). BTApp itself is NOT here: /Applications is outside the overlay allowlist (patch 0117); it is baked
# into the root (tools/sweet-root-tweaks.py tweak_btapp). Any later failure removes OUT again.
# app-overlay.sh START OUT LAUNCHER
set -eu
[ $# -eq 3 ] || { echo "usage: $0 START OUT LAUNCHER"; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
start=$1; out=$2; launcher=$3
say() { printf 'btapp-overlay: %s\n' "$*"; }
die() { printf 'btapp-overlay: FAIL %s\n' "$*"; exit 1; }
[ -d "$start" ] || die "no overlay start $start"
[ -f "$launcher" ] || die "no launcher $launcher (run bt-client/build-app.sh)"
[ ! -e "$out" ] || die "$out exists (remove it or pick another name)"
python3 "$here/sigcheck.py" "$launcher" "$here/btapp-launch.entitlements" btapp-launch || die "launcher check (nothing was created)"
# review T-2d63 finding 1: a symlink in START would make cp -a copy it and the writes below go through it, outside OUT
lnk=$(find "$start" -type l -print -quit)
[ -z "$lnk" ] || die "symlink in the overlay start: $lnk (writes through it would leave $out; nothing was created)"
[ -f "$start/System/Library/xpc/launchd.plist" ] || die "no System/Library/xpc/launchd.plist in $start (xrun allows jobs only there)"

mkdir -p "$out"
ok=0
trap '[ "$ok" = 1 ] || { rm -rf "$out"; printf "btapp-overlay: removed %s after the failure\n" "$out"; }' EXIT
cp -a "$start/." "$out/"
if [ -e "$out/usr/local/bin/bt-probe" ]; then rm -f "$out/usr/local/bin/bt-probe"; say "removed usr/local/bin/bt-probe (from $start)"; fi
mkdir -p "$out/usr/local/bin"
cp "$launcher" "$out/usr/local/bin/btapp-launch"
chmod 755 "$out/usr/local/bin/btapp-launch"
python3 - "$out/System/Library/xpc/launchd.plist" <<'PY' || die "job install"
import plistlib, sys
p = sys.argv[1]
cache = plistlib.load(open(p, 'rb'))
jobs = cache.setdefault('LaunchDaemons', {})
gone = [k for k, j in jobs.items() if j.get('Label') in ('com.pixos.bt-probe', 'com.pixos.btapp-launch')]
for k in gone:
    del jobs[k]
key = '/System/Library/LaunchDaemons/com.pixos.btapp-launch.plist'
# UserName mobile like SpringBoard and the probe; no StandardOutPath (launchd default /dev/null = the patch 0048 mirror); no KeepAlive
jobs[key] = {'Label': 'com.pixos.btapp-launch', 'UserName': 'mobile', 'ProgramArguments': ['/usr/local/bin/btapp-launch'],
             'RunAtLoad': True, 'EnvironmentVariables': {'OS_ACTIVITY_DT_MODE': 'YES'}}
open(p, 'wb').write(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
print('btapp-overlay: removed jobs', ','.join(sorted(gone)) or '-', '; job', key, '; daemons', len(jobs))
PY
say "overlay $out (start $start - bt-probe + usr/local/bin/btapp-launch)"
say "boot-arg: sweetnull=...,BTApp,btapp-launch (and drop bt-probe)"
ok=1
say "RESULT: PASS btapp-overlay"
