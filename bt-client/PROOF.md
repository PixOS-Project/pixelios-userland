# bt-probe (T-86aa, fixed in T-f9ab): build, sign, host test

Host-only evidence. Nothing here was observed on the phone except where marked [obs phone] (the two bt-up16 crashes are in
`docs/reviews/T-86aa-review.md`). The T-f9ab results are in `docs/reviews/T-f9ab-proof.md` (super repo).

| check | command | result |
|---|---|---|
| core + log lines | `bt-client/host-test.sh` | `RESULT: PASS bt_client_host_test cases=7` |
| host test mutants | `bt-client/mutant-test.sh` | `RESULT: PASS bt-client-mutants killed=5/5` |
| backend syntax | `cc -std=gnu99 -Wall -Wextra -Werror -fsyntax-only bt_client_ios.c` | clean |
| call sites | `bt-client/callsite-check.py bt-probe` | `RESULT: PASS bt-probe-callsites n=11 multi=3` (the a61d41c binary: `FAIL spilled=3`) |
| iOS build (theos clang-11 + ld64-609 + iPhoneOS16.5.sdk) | `bt-client/build.sh` | `RESULT: PASS bt-probe-build` |
| overlay | `bt-client/run-plan-overlay.sh START OUT bt-probe` | `RESULT: PASS bt-probe-overlay`; dummy file, unsigned, no-entitlement and extra-entitlement binaries are refused and leave no OUT |

The binary is a gitignored artifact (`~/pixelios-artifacts/bt-client/bt-probe`); the source is `pixos-project/pixelios-userland`
branch `bt/T-f86aa` (local, not pushed).

Reproducible: with a fixed `PIXOS_BT_CLIENT_BUILD` and the same output file name, two builds are byte-identical (`cmp`). The
CodeDirectory identifier is the output file NAME, so different names differ in that one byte. (The earlier claim here, "not
reproducible byte for byte", was wrong; T-86aa review finding 7.)

The signer: the theos `ldid` on this host cannot open any Mach-O. `rcodesign` was built from source (`cargo build --release -p
apple-codesign`); its own `verify` prints a bogus CMS error on every ad-hoc file (the tool says so itself).

Open before the boot: the probe's launchd job starts at the same time as bluetoothd. CBManagerState 5 = poweredOn (3 =
unauthorized); the run's `state event=` line and `central never reached poweredOn` gap answer whether it gets there on its own.
