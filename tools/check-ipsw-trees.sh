#!/bin/bash
# Read-only IPSW read check (keep: the root build dies in the tree staging without naming a cause; this names the tree/file). Host check of the IPSW trees before a ~13 min root build: the last build died in the staging tar with
# "Read error ... Input/output error" on files under four framework bundles, so read every file of the
# OS / SystemOS / AppOS trees once through apfs-fuse and list what fails. Read-only: nothing is written
# outside OUT (the artifact dir, gitignored).
set -uo pipefail
cd "$(dirname "$0")"
IPSW=${IPSW:-$HOME/pixelios-artifacts/ipsw}
OUT=${OUT:-$HOME/pixelios-artifacts/sprintbt/ipsw-check}
APFS=${APFS_FUSE:-$HOME/pixelios-artifacts/apfs-src/build/apfs-fuse}
mkdir -p "$OUT"
work=$(mktemp -d -p "$OUT" .check.XXXXXX)
pids=()
cleanup() { for p in "${pids[@]}"; do fusermount3 -u "$p" 2>/dev/null || fusermount -u "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
n=0
for d in "$IPSW"/*.dmg; do
	mkdir -p "$work/v$n"
	ln -s "$d" "$work/v$n.dmg"
	"$APFS" "$work/v$n.dmg" "$work/v$n" >"$work/v$n.log" 2>&1 &
	pids+=("$work/v$n")
	n=$((n + 1))
done
sleep 20
i=0
for d in "$IPSW"/*.dmg; do
	vol="$work/v$i"
	i=$((i + 1))
	if [ ! -d "$vol/root" ]; then
		echo "MOUNT-FAILED $(basename "$d"): $(head -3 "$work/v$i.log" 2>/dev/null | tr '\n' ' ')"
		continue
	fi
	echo "tree $(basename "$d"): $(find "$vol/root" 2>/dev/null | wc -l) entries"
	# the four bundles named by the failed build, then a full pass
	for sub in System/Library/Accounts System/Library/PrivateFrameworks/DiagnosticExtensions.framework \
	           System/Library/PrivateFrameworks/ReminderKit.framework System/Library/PrivateFrameworks/NotesUI.framework; do
		if [ -d "$vol/root/$sub" ]; then
			find "$vol/root/$sub" -type f -print0 2>/dev/null | xargs -0 -r -n 64 cat >/dev/null 2>"$work/err" ||
				echo "  READ FAIL under $sub: $(head -1 "$work/err")"
		fi
	done
	echo "  named bundles: done"
	if [ "${FULL:-0}" = 1 ]; then
		find "$vol/root" -type f -print0 2>/dev/null | xargs -0 -r -n 256 cat >/dev/null 2>"$work/full.err" || echo "  FULL PASS FAILED: $(head -3 "$work/full.err")"
		echo "  full pass: done"
	fi
done
echo "work dir: $work"
