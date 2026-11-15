#!/bin/sh
# bt-proper: add the LE advertising run to an overlay tree IN PLACE (make a copy first: cp -a TREE TREE2). Installs the signed bt-client
# at usr/local/bin/bt-client and the one-shot launchd job com.pixos.bt-adv (user mobile, RunAtLoad, no StartInterval, no KeepAlive):
#   /usr/local/bin/bt-client adv 90 --delay 420
# The 420 s delay is inside the client (it sleeps before its first CoreBluetooth call, so the job runs after BTApp's checks).
# Like the other jobs it has no StandardOutPath (launchd default /dev/null): xnu patch 0048 mirrors that stdout into the host log
# for the process names in boot-arg sweetnull=..., so the boot-arg must list bt-client.
# The binary is validated BEFORE anything changes: its sealed entitlements must EQUAL bt-client.entitlements (sigcheck.py). Any later
# failure restores the tree (plist backup, binary removed).
# adv-overlay.sh TREE [BINARY]   (BINARY default $PIXOS_BT_CLIENT_OUT or ~/pixelios-artifacts/bt-client/bt-probe, from bt-client/build.sh)
set -eu
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 TREE [BINARY]"; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
tree=$1
bin=${2:-${PIXOS_BT_CLIENT_OUT:-$HOME/pixelios-artifacts/bt-client/bt-probe}}
say() { printf 'bt-adv-overlay: %s\n' "$*"; }
die() { printf 'bt-adv-overlay: FAIL %s\n' "$*"; exit 1; }
plist=$tree/System/Library/xpc/launchd.plist
[ -d "$tree" ] || die "no overlay tree $tree"
[ -f "$bin" ] || die "no binary $bin (run bt-client/build.sh)"
[ -f "$plist" ] || die "no System/Library/xpc/launchd.plist in $tree (xrun allows jobs only there)"
python3 "$here/sigcheck.py" "$bin" "$here/bt-client.entitlements" bt-client || die "binary check (nothing was changed)"
# the signing identifier must be the name the binary is installed under
lnk=$(find "$tree" -type l -print -quit)
[ -z "$lnk" ] || die "symlink in the tree: $lnk (writes through it could leave $tree; nothing was changed)"

bak=$(mktemp -d)
ok=0
had_bin=0
[ -e "$tree/usr/local/bin/bt-client" ] && { had_bin=1; cp -p "$tree/usr/local/bin/bt-client" "$bak/bt-client"; }
cp -p "$plist" "$bak/launchd.plist"
restore() {
	if [ "$ok" != 1 ]; then
		cp -p "$bak/launchd.plist" "$plist"
		if [ "$had_bin" = 1 ]; then cp -p "$bak/bt-client" "$tree/usr/local/bin/bt-client"; else rm -f "$tree/usr/local/bin/bt-client"; fi
		printf 'bt-adv-overlay: restored %s after the failure\n' "$tree"
	fi
	rm -rf "$bak"
}
trap restore EXIT
mkdir -p "$tree/usr/local/bin"
cp "$bin" "$tree/usr/local/bin/bt-client"
chmod 755 "$tree/usr/local/bin/bt-client"
python3 - "$plist" <<'PY' || die "job install"
import plistlib, sys
p = sys.argv[1]
cache = plistlib.load(open(p, 'rb'))
jobs = cache.setdefault('LaunchDaemons', {})
key = '/System/Library/LaunchDaemons/com.pixos.bt-adv.plist'
for k in [k for k, j in jobs.items() if j.get('Label') == 'com.pixos.bt-adv']:
    del jobs[k]
jobs[key] = {'Label': 'com.pixos.bt-adv', 'UserName': 'mobile',
             'ProgramArguments': ['/usr/local/bin/bt-client', 'adv', '90', '--delay', '420'],
             'RunAtLoad': True, 'EnvironmentVariables': {'OS_ACTIVITY_DT_MODE': 'YES'}}
open(p, 'wb').write(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
print('bt-adv-overlay: job', key, 'daemons', len(jobs))
PY
say "installed usr/local/bin/bt-client + job com.pixos.bt-adv in $tree"
say "boot-arg: add bt-client to sweetnull=..."
ok=1
say "RESULT: PASS bt-adv-overlay"
