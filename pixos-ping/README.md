# pixos-ping

PixOS own-code ICMP echo tool for the iOS root (P-W3: "ping from iOS userland"). The stock iOS root has no ping, ifconfig, arp, netstat or nc.
Libc only; it is not Apple's `ping.c`. It uses the unprivileged ICMP datagram socket (`socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)`; XNU allows echo
requests on it without root and does not rewrite the id, `bsd/netinet/ip_icmp.c` `icmp_dgram_send`).

`pixos-ping [IPv4]`; env `PIXOS_PING_TARGET` (the argument wins), `PIXOS_PING_COUNT` (5), `PIXOS_PING_INTERVAL_MS` (1000), `PIXOS_PING_TIMEOUT_MS` (2000,
the wait after the last probe), `PIXOS_PING_WAIT_S` (90: until the first send works, a missing route is retried every 0.5 s). A bad value is a `WIFI GAP:` line
and the default is used. The address is never printed.

Log lines (the same format as the P-W2 shim line, SPEC-WIFI DP13, so one regex serves P-W2 and P-W3):

    SWEET WLAN: ping start build=<id>
    SWEET WLAN: ping reply seq=N rtt_us=N
    SWEET WLAN: ping summary sent=N got=M lost=L rtt_us min/avg/max=a/b/c bad=B ignored=I
    WIFI PING: RESULT pass|fail gaps=N          (pass = at least one reply; exit 0 pass, 1 fail, 2 bad usage)

| file | role |
|---|---|
| `pixos_ping_core.c`, `pixos_ping.h` | sequence, timing, parsing, log lines; talks to the system only through `struct pp_ops` |
| `pixos_ping_sock.c`, `pixos_ping_sock.h`, `pixos_ping_main.c` | the real backend (ICMP datagram socket) and `main` |
| `pixos_ping_host_test.c`, `host-test.sh` | fake-network scenarios + a loopback echo through a real socket (printed as `loopback=skipped(...)` if the host forbids it); `RESULT: PASS ping_host_test` |
| `build-ios.sh` | arm64 iOS 16 build with the theos `ld` + SDK 16.5 and ad-hoc `ldid -Cadhoc -S` (toolchain from `tools/xnu-local-build`, not in git) |

Root: `tools/sweet-root-tweaks.py` `tweak_ping` installs `/usr/local/bin/pixos-ping` and a one-shot launchd job (RunAtLoad, no KeepAlive) only when
`PIXOS_PING_BIN=<signed binary>` and `PIXOS_PING_TARGET=<IPv4>` are set at root build time (optional knobs above are passed through); unset = default
root unchanged; unsigned binary, non-Mach-O or a bad target/knob = refused with the reason. Test: `python3 tools/test_sweet_root_tweaks.py`
(`RESULT: PASS root-tweak-ping`; with `PIXOS_PING_REAL_BIN=<build-ios.sh output>` it also checks the real ldid output).

Status: host tested and built for iOS; not run on a phone.
