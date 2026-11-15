/* pixos-ping: a small ICMP echo tool for the PixOS iOS root (PixOS own code, libc only, no Apple ping source).
 * The stock iOS root ships no ping/ifconfig/arp/netstat (T-4505 Log step 1), so P-W3 (ping from iOS userland) needs one.
 *
 * Design: unprivileged ICMP datagram socket (socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP); echo requests are allowed without root,
 * the checksum is ours, the id is not rewritten: XNU bsd/netinet/ip_icmp.c icmp_dgram_send [read from source]). The core
 * (pixos_ping_core.c) holds the sequence, timing and every log line and talks to the system only through struct pp_ops, so
 * the host test runs it against a fake network. Log line format = the P-W2 shim line (SPEC-WIFI DP13), one regex serves both:
 *   SWEET WLAN: ping reply seq=N rtt_us=N
 *   SWEET WLAN: ping summary sent=N got=M lost=L rtt_us min/avg/max=a/b/c bad=B ignored=I
 * Every failure is a "WIFI GAP:" line that counts into gaps=N; the last line is "WIFI PING: RESULT pass|fail gaps=N".
 * The target address is never printed and never in git: it comes from PIXOS_PING_TARGET (or argv[1]).
 */
#ifndef PIXOS_PING_H
#define PIXOS_PING_H

#include <stddef.h>
#include <stdio.h>

#define PP_PAYLOAD 56            /* echo payload bytes after the 8 byte ICMP header */
#define PP_MAX_COUNT 100

struct pp_ops {
	void *ctx;
	long long (*now_us)(void *ctx);                          /* monotonic microseconds */
	void (*sleep_ms)(void *ctx, int ms);
	int (*set_target)(void *ctx, const char *ipv4);          /* 0 ok, < 0 = -errno */
	int (*send)(void *ctx, const unsigned char *pkt, size_t len);   /* ICMP message (no IP header); 0 ok, < 0 = -errno */
	/* one datagram into buf (may start with an IP header or not): > 0 length, 0 timeout after timeout_ms, < 0 = -errno */
	int (*recv)(void *ctx, unsigned char *buf, size_t cap, int timeout_ms);
};

struct pp_cfg {
	int count, interval_ms, timeout_ms, wait_s;
};

#define PP_DEFAULT_CFG { 5, 1000, 2000, 90 }

unsigned pp_cksum(const unsigned char *p, size_t n);          /* RFC 1071 internet checksum, host order result */

/* Run one ping session against target; prints the log lines to out. Returns 0 got>=1, 1 no reply / network gap, 2 bad usage. */
int pp_run(const struct pp_ops *ops, const struct pp_cfg *cfg, const char *target, unsigned nonce, FILE *out);

/* Read the configuration from the environment (getenv_fn), argv[1] overrides PIXOS_PING_TARGET, then pp_run. */
int pp_main(int argc, char **argv, const struct pp_ops *ops, const char *(*getenv_fn)(const char *), const char *build,
	    unsigned nonce, FILE *out);

#endif
