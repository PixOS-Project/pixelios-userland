# Run plan: bt-up16 (T-86aa) — W-BTSCAN: bt-probe (first-party CoreBluetooth peripheral counter) on the bt-up15 image (one change)

Goal W-BTSCAN (BT-S3). Kernel: unchanged from bt-up15 (T-b58f: `~/pixelios-artifacts/localbuild/out/tb58f/kernel.development.sweet`, tools/T-b58f 4703094f, patch 0231). Overlay: bt-up15's tree `~/pixelios-artifacts/t-2420/tree` copied by `bt-client/run-plan-overlay.sh` + two additions (`usr/local/bin/bt-probe`, `System/Library/LaunchDaemons/com.pixos.bt-probe.plist`) + one boot-arg token. ONE change vs bt-up15: the probe runs and scans.

Baseline: bt-up15 `20261107T132335Z-bt-up15` (T-b58f): 35 `sweetbtnopcf=1 withheld` rows, 0 `BT HCI: rx .. 04 3e` rows, no observable client [obs].
Prediction [guess, docs/research/BT-S3-pcf-filter-callers-and-client.md s.3-4]: the probe prints `state event=5`, `scan started isScanning=1`, then `discovered` grows; withheld rows appear right after the scan start (sub-op 0x05/0x07 action 0x02 = bluetoothd drops or rewrites the filter for an unfiltered rule; new `0x07 action 0x00` rows only = it keeps it).
Metric (rule 10): the probe's `BT CLIENT: RESULT pass|fail discovered=N unique=N state=N gaps=N` line from the host log, plus the `04 3e` row count and the withheld rows after the scan start.

## Host tier (done, docs/reviews/T-86aa-host-proof.md)

`bt-client/host-test.sh` `RESULT: PASS bt_client_host_test cases=4`; `bt-client/build.sh` `RESULT: PASS bt-probe-build` (arm64 Mach-O, ADHOC CodeDirectory, entitlements slots 5+7, platform 0); `bt-client/run-plan-overlay.sh` `RESULT: PASS bt-probe-overlay` and refuses an unsigned probe. Source: userland `bt/T-f86aa` 1c4a199.

## Overlay

```
bt-client/run-plan-overlay.sh ~/pixelios-artifacts/t-2420/tree ~/pixelios-artifacts/t86aa/tree ~/pixelios-artifacts/bt-client/bt-probe
```

It writes a `RunAtLoad` job with no `StandardOutPath` (launchd's default `/dev/null`, mirrored to the host log by xnu patch 0048 for the names in `sweetnull`) and `OS_ACTIVITY_DT_MODE=YES`.

## Safety

Ticket risk HW-WRITE (the boot drives the controller through bluetoothd, as bt-up15 does). The probe writes no controller state itself: it is a CoreBluetooth client of bluetoothd. No new kernel code, RAM boot only, no flash. Nothing touches userdata.

## Questions
| # | Question | Prediction | Line that answers it |
|---|---|---|---|
| 1 | does bluetoothd authorize the probe (CBManagerState 5)? | yes [guess] | `bt-probe\| BT CLIENT: state event=5` |
| 2 | does the scan start? | yes | `BT CLIENT: scan started isScanning=1`, no `BT CLIENT GAP` |
| 3 | does the probe hear an advertiser? | >= 1 [guess] | `BT CLIENT: RESULT pass discovered=` |
| 4 | does bluetoothd change its filter for an unfiltered rule? | drops or rewrites it | withheld rows `sub 0x05`/`sub 0x07 action 0x0[02]` after the scan start |
| 5 | no new failure path? | none | `--fail-on 'Kernel trap at\|panic\(cpu\|panic:\|Assertion failed'`, `--expect-absent 'reset-after-fail'` |

## Command

bt-up15's command (below) with `--overlay ~/pixelios-artifacts/t86aa/tree`, `--label bt-up16`, the bt-up15 `--args` unchanged except `sweetnull=bluetoothd-m49,BlueTool,bt-probe`, plus:

```
--expect 'BT GAP: sweetbtnopcf=1 active' --expect 'exec bluetoothd-m49' \
--expect '\|bt-probe\| BT CLIENT: start build=' \
--expect '\|bt-probe\| BT CLIENT: RESULT (pass|fail) discovered=[0-9]+ unique=[0-9]+ state=[0-9]+ gaps=[0-9]+' \
--expect-absent 'reset-after-fail'
```

```
env PREFLIGHT_GATE_WAIVER="user <date> approval: bt-up16 probe run" tools/phone-lock run --owner T-86aa --wait 3000 --ttl 1800 -- \
  tools/xrun --intent none --kernel ~/pixelios-artifacts/localbuild/out/tb58f/kernel.development.sweet \
  --cpus 8 --restart 650,bootloader --duration 520 --hard-timeout 1000 --overlay ~/pixelios-artifacts/t86aa/tree --label bt-up16 \
  --args "sweetusb3=1 sweetbt=1 sweetcpuwatch=launchd,mediaserverd,bluetoothd-m49 sweetnull=bluetoothd-m49,BlueTool,bt-probe \
  sweetbtkeepbaud=1 sweetbtleprobe=4 sweetbtnopadv=1 sweetbtpubscan=1 sweetbtpatchram=1 sweetbtrfwit=1 sweetbtnopcf=1" \
  --fail-on 'Kernel trap at|panic\(cpu|panic:|Assertion failed' <all bt-up15 --expect lines> <the four lines above>
```

The phone run and the gate waiver are the user's step.

## Proof

`docs/reviews/T-86aa-proof.md`: run label, the probe's lines verbatim (counts only), the `04 3e` count after the scan start, the withheld rows after the scan start (sub-op/action only), and `RESULT: PASS bt-probe client_adv=N` or `RESULT: FAIL bt-probe client_adv=0`.
