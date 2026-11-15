#!/usr/bin/env python3
"""PC side of the PixOS A2DP test, v2: wait (max WAIT s) for the PipeWire/Pulse source of the phone (bluez_input/bluez_source with
the phone's address), record up to SECS s (raw s16le 44100 stereo), then score every 1 s window: RMS dBFS + zero-crossing frequency.
PASS = >= 5 consecutive windows with RMS > -40 dBFS and 400-480 Hz. Also prints the source-gone time if the link drops.
Usage: bt_pc_record.py ADDR_UNDERSCORED WAIT_S SECS. Prints 'BT REC: ...'. Writes only /tmp/pixos-a2dp.raw."""
import sys as _s
if len(_s.argv) > 1 and _s.argv[1] in ("-h", "--help"):
    print(__doc__)
    _s.exit(0)

import subprocess, sys, time, math, struct

ADDR, WAIT, SECS = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
OUT = "/tmp/pixos-a2dp.raw"
RATE = 44100


def find_source():
    r = subprocess.run(["pactl", "list", "sources", "short"], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        f = line.split("\t")
        if len(f) > 1 and ADDR in f[1] and "monitor" not in f[1]:
            return f[1]
    return None


t0 = time.time()
src = None
while time.time() - t0 < WAIT:
    src = find_source()
    if src:
        break
    time.sleep(2)
if not src:
    print("BT REC: RESULT: FAIL no bluez source for the phone within %d s" % WAIT, flush=True)
    sys.exit(1)
print("BT REC: source appeared after %d s: %s" % (time.time() - t0, src), flush=True)
p = subprocess.Popen(["parecord", "--raw", "--format=s16le", "--rate=%d" % RATE, "--channels=2", "-d", src, OUT])
t1 = time.time()
while time.time() - t1 < SECS and p.poll() is None:
    time.sleep(5)
    if not find_source():
        print("BT REC: source gone after %d s of recording" % (time.time() - t1), flush=True)
        break
p.terminate()
p.wait()
data = open(OUT, "rb").read()
n = len(data) // 4
left = struct.unpack("<%dh" % (n * 2), data[: n * 4])[0::2]
best, run, runmax, firstok = (-200.0, 0.0, 0), 0, 0, None
for w in range(n // RATE):
    seg = left[w * RATE:(w + 1) * RATE]
    rms = math.sqrt(sum(x * x for x in seg) / RATE) / 32768.0
    db = 20 * math.log10(rms) if rms > 0 else -200.0
    freq = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0)) / 2.0
    ok = db > -40 and 400 <= freq <= 480
    if db > best[0]:
        best = (db, freq, w)
    run = run + 1 if ok else 0
    runmax = max(runmax, run)
    if ok and firstok is None:
        firstok = w
res = "PASS" if runmax >= 5 else "FAIL"
print("BT REC: RESULT: %s frames=%d windows=%d tone_run_s=%d first_tone_s=%s best_rms_dbfs=%.1f best_freq_hz=%.0f best_at_s=%d"
      % (res, n, n // RATE, runmax, firstok, best[0], best[1], best[2]), flush=True)
