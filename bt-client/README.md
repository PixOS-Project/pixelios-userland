# bt-client (bt-probe)

PixOS own-code CoreBluetooth central, shipped as the first-party diagnostics probe `bt-probe` (tickets T-86aa, T-f9ab, goal
W-BTSCAN / BT-S3). It starts one `CBCentralManager` on its own serial dispatch queue (`com.pixos.btprobe`; a nil queue is the
main queue, which never drains while `main()` sleeps), calls `scanForPeripheralsWithServices:nil options:nil` once the
manager is powered on, and counts `centralManager:didDiscoverPeripheral:` in `discovered` (every call) and `unique` (Bloom bit).
This counting is written and host-tested only; no phone run has observed it yet (T-f9ab built it, T-9947 boots it).
Every `gap()` in the backend is counted into `gaps`. Every 10 s it prints one `BT CLIENT:` line,
with a `BT CLIENT GAP:` line per failure, and ends with `BT CLIENT: RESULT pass|fail discovered=N unique=N state=N gaps=N`.
`unique` is a Bloom-filter count, so no address, identifier or service UUID is kept or printed (rule 4).

| file | role |
|---|---|
| `bt_client.h`, `bt_client_core.c` | sequence, timing, log lines and counters; talks to the system only through `struct btc_ops` |
| `bt_client_ios.c` | backend: ObjC runtime + CoreBluetooth through `dlsym`, delegate class via `class_addMethod`, plain C |
| `bt_client_host_test.c`, `host-test.sh` | host test with stubs (7 cases): whole-line comparison incl. `gaps=N`, MAC/UUID-shaped output refused |
| `mutant-test.sh` | applies the 5 core mutants of `docs/reviews/T-86aa-review.md` finding 6 with sed; the host test must FAIL on each |
| `callsite-check.py` | capstone check on the built binary: every multi-argument `objc_msgSend` passes its arguments in registers |
| `bt-client.entitlements` | `application-identifier` only (below) |
| `build.sh` | compiles and links for arm64-apple-ios with the theos toolchain, signs with rcodesign, runs `callsite-check.py` (dies on FAIL) |
| `run-plan-overlay.sh` | stages the overlay: start tree + `usr/local/bin/bt-probe` + one-shot launchd job (`UserName mobile`); refuses a binary whose sealed entitlements differ from `{application-identifier}`, before creating anything |

## Log channel

A daemon's `/dev/console` text never reached the host log (T-99f8, observed). The lines go to stdout, which the launchd job
leaves at its default `/dev/null`; xnu patch 0048 mirrors what a process named in boot-arg `sweetnull=...,bt-probe` writes
to `/dev/null` into the host log as `|bt-probe| <text>`. No NSLog: it takes an NSString format, and a C string format
crashed the probe in bt-up16 (SIGSEGV in objc_msgSend, x0 = the "%s" literal) [obs 2026-11-07 20261107T150829Z-bt-up16].

## Entitlements

The probe carries `application-identifier` and nothing else, like an ordinary CoreBluetooth app (which holds no Bluetooth
entitlement; its access check is a TCC grant, `kTCCServiceBluetoothAlways` is among bluetoothd's strings) [bin, T-86aa review
finding 4]. The earlier set was dropped: `com.apple.developer.bluetooth-central-screen-off-scanning` is a real bluetoothd
capability flag (session+0xa9, more than an app holds); `bluetooth-usage`, `bluetooth-background-modes` and
`bluetooth-advertising-extended-filters` occur in no string of bluetoothd or of the dyld shared cache, so nothing reads them
(background modes are Info.plist `UIBackgroundModes`). `run-plan-overlay.sh` enforces equality with the allowlist.

For the record, the read from the stock `bluetoothd` naming its client gates [bin]: the scan message `0xad` only sends a
ScanFilterUUID command when `[[msg +0x20], #0x3d0] != 0` and a capability call (`vtable +0x748`) returns non-zero, else it
returns status `0x72` (`0x10025bdf0-0x10025bf8c`); the manager state is decided at `0x10046bd9c-0x10046be20`. Whether a TCC grant
is also needed for a binary without a bundle id is what the bt-up16 boot measures.

## Build and test

```
bt-client/host-test.sh                                   # RESULT: PASS bt_client_host_test
bt-client/mutant-test.sh                                 # RESULT: PASS bt-client-mutants killed=5/5
bt-client/build.sh                                       # RESULT: PASS bt-probe-callsites, then PASS bt-probe-build (PIXOS_BT_CLIENT_OUT/BUILD override)
bt-client/callsite-check.py BINARY                       # RESULT: PASS|FAIL bt-probe-callsites
bt-client/run-plan-overlay.sh T-2420TREE OUT bt-probe    # RESULT: PASS bt-probe-overlay
```

The build needs `$PIXOS_LOCALBUILD` (theos toolchain + iPhoneOS16.5.sdk) and `$PIXOS_RCODESIGN` (a native rcodesign); no root.
The binary is a gitignored artifact under `~/pixelios-artifacts`, never committed.

Reproducibility: with the same `PIXOS_BT_CLIENT_BUILD` and the same output file NAME two builds are byte-identical (`cmp`, observed
T-f9ab). The CodeDirectory identifier is the output file name (rcodesign), so builds to `b1` and `b2` differ in that one byte.

## BTApp: the foreground-app client (T-2d63)

bluetoothd delivers a `services:nil` scan only to a session whose app state is foreground (0x10); a launchd daemon is state
0x20 (T-54b1, T-9524 [bin]). BTApp is a real UIKit app, `com.pixos.btapp`, so SpringBoard can put it in front.

| file | role |
|---|---|
| `bt_app_core.c` | app sequence: one `services:nil, options:nil` scan for the app's life; counts split into windows at the app-state transitions (`ARM fg1/bg1/fg2... services:nil discovered=N`); RESULT pass iff the fg windows discovered >= 1 |
| `bt_app_ios.c` | UIKit main (`UIApplicationMain`, delegate via `class_addMethod`), blue full-screen window, state callbacks with counter snapshots; stdout/stderr dup2'd to `/dev/null` (patch 0048 mirror `|BTApp|`) |
| `btapp_launch_ios.c` | launcher job: `FBSSystemService openApplication:options:clientPort:withResult:` with `__UnlockDevice`, BTApp -> Settings (+120 s, bg control) -> BTApp (+60 s); raw NSError domain/code per try (`|btapp-launch| BT LAUNCH: ...`) |
| `bt_app_host_test.c`, `app-host-test.sh`, `app-mutant-test.sh` | host test (10 cases) and core mutants (16) |
| `build-app.sh`, `sigcheck.py` | build + sign `BTApp.app` (bundle-signed, CodeDirectory identifier = bundle id) and `btapp-launch`; sealed entitlements must equal `btapp.entitlements` / `btapp-launch.entitlements` |
| `app-overlay.sh` | dev overlay: START minus bt-probe, plus `usr/local/bin/btapp-launch` + job `com.pixos.btapp-launch` |

The bundle goes into the root image (`tools/sweet-root-tweaks.py` `tweak_btapp`, env `PIXOS_BTAPP_BUNDLE`): the overlay refuses
`/Applications` (patch 0117), and LaunchServices registers a new `/Applications` app only when its database is seeded, i.e.
on the first boot of a fresh userdata (T-9524 Q1.2). Boot-arg: `sweetnull=...,BTApp,btapp-launch`.

## adv mode (bt-proper)

`bt-client adv [seconds] [--delay N]` (default 60 s, delay 0): CBPeripheralManager publishes one connectable GATT service with one
readable characteristic (value `pixos`, served from `didReceiveReadRequest`) and advertises local name `PixOS-adv` plus the service UUID
(fixed random constants in `bt_adv_ios.c`), so bluetoothd runs its multi-advertising path (VSC 0xfd4a..0xfd4d). The sequence and the
result line are in `bt_client_core.c` (`btadv_run`, host-tested): `BT ADV: RESULT: PASS adv started=1 err=0 seconds=N reads=R centrals=C gaps=G`
or `FAIL adv reason=... started= err= state= ...`. `adv-overlay.sh TREE [BINARY]` installs `bt-client` and the job `com.pixos.bt-adv`
(`adv 90 --delay 420`) into a copy of an overlay tree; the boot-arg `sweetnull` must list `bt-client` for the lines to reach the host log.
Entitlements are unchanged (`application-identifier` only): that this is enough for the peripheral role is NOT proven (see the commit message).

## tone mode (bt-proper)

`bt-client tone [seconds] [--delay N]` (default 30 s): AVAudioSession category Playback + active, a 440 Hz sine at -12 dBFS through an
AudioQueue (44.1 kHz 16-bit stereo, 3 buffers of 4096 frames refilled in the callback), route portTypes logged before and after, result
`BT TONE: RESULT: PASS tone played_s=N route=<portType> gaps=G` or `FAIL reason=...`. The route is reported, not required. Core
(`bt_client_core.c`: sine, parser, state machine) is host-tested and mutant-tested; the iOS side is `bt_tone_ios.c`; `tone-overlay.sh TREE [BINARY]`
adds job com.pixos.bt-tone (`tone 60 --delay 540`). Entitlements unchanged; not run on hardware.
