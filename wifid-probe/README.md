# wifid-probe

PixOS own-code probe for Wi-Fi phase P-W4 (X-W5 design, super repo `docs/research/X-W5-keychain-configd-probe.md` s.5): wait for configd's `en0`
Setup service, then try MobileWiFi's `WiFiSecuritySetPassword` / `CopyPassword` / `RemovePassword` and one `SecItemCopyMatching`, printing a loud
`WIFI ...` line per result and a `WIFI GAP:` line for every failure (`WIFI PROBE: RESULT pass|fail gaps=N`). It uses a made-up SSID and password and
prints only their length and a non-cryptographic hash prefix. Steps: `wifid-probe [wait-configd] [keychain]` (none = both). Env: `WIFID_PROBE_EN0_TIMEOUT`
(default 120 s), `WIFID_PROBE_SVC_TIMEOUT` (default 30 s).

| file | role |
|---|---|
| `wifid_probe_core.c`, `wifid_probe.h` | sequence, timeouts, log lines; talks to the system only through `struct wp_ops` |
| `wifid_probe_ios.c` | the iOS backend: every system function through `dlsym` (missing symbol = `WIFI GAP:`) |
| `wifid_probe_host_test.c`, `host-test.sh` | host test with stubs; prints `RESULT: PASS wifid_probe_host_test` |
| `ios-syntax-check.sh` | syntax/type check of the iOS backend with the host compiler (not an iOS build) |
| `wifid-probe.entitlements` | wifid's keychain entitlements (application-identifier, system-keychain, keychain-access-groups) |

Build for the phone (needs the iOS SDK and an `ldid`-style signer, neither is on the dev host; nothing here was built for iOS yet):
`clang -target arm64-apple-ios16.0 -isysroot <iPhoneOS.sdk> -DWP_BUILD='"<id>"' wifid_probe_core.c wifid_probe_ios.c -o wifid-probe`, then sign with
`wifid-probe.entitlements` as an XML entitlements blob (a DER-only signature gets no entitlements from the kernel, xnu patch 0035). The root build
installs it only when `PIXOS_WIFID_PROBE=<that binary>` is set (`tools/sweet-root-tweaks.py`, `tweak_wifid_probe`), and refuses a binary whose signature
lacks the entitlements; unset = the default root is unchanged.
