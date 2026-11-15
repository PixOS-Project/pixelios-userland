/* wifid-probe: wait for configd's en0 service, then try the MobileWiFi keychain calls (PixOS own code).
 *
 * Design: docs/research/X-W5-keychain-configd-probe.md s.5 (super repo). Prior art, ideas only, nothing copied:
 * HakiOS wifipw_test.c and net_test.m ("wifi" step).
 *
 * The core (wifid_probe_core.c) holds the sequence, the timeouts and every log line. It talks to the system only
 * through struct wp_ops, so the host test (wifid_probe_host_test.c) runs it against stubs on any machine.
 * The iOS backend is wifid_probe_ios.c. Every result is a loud line ("WIFI ..."), every failure a "WIFI GAP:"
 * line that counts into gaps=N; no SSID or password byte is ever printed (length and a hash prefix only).
 */
#ifndef WIFID_PROBE_H
#define WIFID_PROBE_H

#include <stddef.h>
#include <stdio.h>

#define WP_MISSING (-2)   /* the system symbol behind an op was not found (dlsym returned NULL) */

struct wp_item {          /* what SecItemCopyMatching(service AirPort, account ssid) reported */
	int status;           /* OSStatus */
	char group[64];       /* kSecAttrAccessGroup, "?" when absent */
	char cls[32];         /* kSecAttrAccessible (protection class), "?" when absent */
	int sync;             /* kSecAttrSynchronizable: 1 yes, 0 no, -1 absent */
};

struct wp_ops {
	void *ctx;
	double (*now)(void *ctx);                                /* monotonic seconds */
	void (*sleep_ms)(void *ctx, int ms);
	unsigned (*en0_index)(void *ctx);                        /* if_nametoindex("en0"), 0 = none */
	/* Setup:/Network/Service/<id>/Interface with DeviceName en0: 1 present (*idlen = chars of <id>),
	 * 0 absent, WP_MISSING = SystemConfiguration symbol missing, other < 0 = store error */
	int (*store_en0_service)(void *ctx, int *idlen);
	/* number of State:/Network/Interface/en0 entity keys and State:/Network/Global/IPv4 present; 0 ok, < 0 error */
	int (*store_state)(void *ctx, int *iface_keys, int *global_ipv4);
	int (*mkb_lockstate)(void *ctx, int *v);                 /* 0 ok, WP_MISSING */
	int (*mkb_unlocked_since_boot)(void *ctx, int *v);       /* 0 ok, WP_MISSING */
	int (*wifi_set)(void *ctx, const char *ssid, const char *pw);   /* 1 yes, 0 no, WP_MISSING */
	/* length of the password copied into out (NUL-terminated, cap bytes), -1 = NULL result, WP_MISSING */
	int (*wifi_copy)(void *ctx, const char *ssid, char *out, size_t cap);
	int (*wifi_remove)(void *ctx, const char *ssid);         /* 1 yes, 0 no, WP_MISSING */
	/* 0 filled *it (it->status is the OSStatus), WP_MISSING = the Sec symbols are missing */
	int (*sec_query)(void *ctx, const char *ssid, struct wp_item *it);
};

struct wp_cfg {
	int do_wait;          /* step wait-configd */
	int do_keychain;      /* step keychain */
	double en0_timeout;   /* seconds, default 120 (env WIFID_PROBE_EN0_TIMEOUT) */
	double svc_timeout;   /* seconds, default 30  (env WIFID_PROBE_SVC_TIMEOUT) */
	int seed;             /* makes the made-up SSID/password unique per run (the pid) */
	const char *build;    /* build id for the start line */
	int gaps;             /* gaps found while parsing arguments and environment */
};

/* Parse argv steps (none = both, in the order wait-configd, keychain) and the two timeout variables.
 * Prints a GAP line for every bad argument or value. Returns 0, or 1 when a step name is unknown. */
int wp_parse(int argc, char **argv, const char *(*getenv_fn)(const char *), struct wp_cfg *cfg, FILE *out);

/* Run the steps; returns the number of gaps (also printed in the RESULT line). */
int wp_run(const struct wp_ops *ops, struct wp_cfg *cfg, FILE *out);

/* wp_parse + wp_run; process exit code: 0 pass, 1 gaps, 2 bad argument. */
int wp_main(int argc, char **argv, const struct wp_ops *ops, const char *(*getenv_fn)(const char *),
	    const char *build, int seed, FILE *out);

#endif
