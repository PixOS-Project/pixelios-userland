/* Host test for the wifid-probe core: every outcome of X-W5 s.5 against stubbed MobileWiFi / SystemConfiguration /
 * MobileKeyBag. Prints each case's GAP lines and RESULT line, then RESULT: PASS|FAIL wifid_probe_host_test. */
#include "wifid_probe.h"

#include <stdlib.h>
#include <string.h>

struct sc {                  /* one scenario */
	const char *name;
	const char *steps;       /* argv steps, space separated, "" = none */
	const char *env_en0, *env_svc;
	double en0_at, svc_at;   /* fake-clock second the interface / service appears, < 0 never */
	int store_err_polls;     /* first N service polls fail with -1 */
	int miss_set, miss_copy, miss_remove, miss_mkb, miss_sec, miss_sc;
	int lock, unl;           /* MKB values */
	int sec_override;        /* OSStatus forced in sec_query, 0 = honest */
	int expect_rc, expect_gaps;
	const char *want[6];     /* substrings that must be in the output */
	double min_clock;        /* the fake clock must have run at least this far */
};

struct st {
	const struct sc *sc;
	double clock;
	int polls;
	int has_item;
	char pw[64];
};

static double s_now(void *c) { return ((struct st *)c)->clock; }
static void s_sleep(void *c, int ms) { ((struct st *)c)->clock += ms / 1000.0; }
static unsigned s_en0(void *c)
{
	struct st *s = c;

	return s->sc->en0_at >= 0 && s->clock >= s->sc->en0_at ? 4 : 0;
}
static int s_svc(void *c, int *idlen)
{
	struct st *s = c;

	if (s->sc->miss_sc)
		return WP_MISSING;
	if (s->polls++ < s->sc->store_err_polls)
		return -1;
	if (s->sc->svc_at >= 0 && s->clock >= s->sc->svc_at) {
		*idlen = 36;
		return 1;
	}
	return 0;
}
static int s_state(void *c, int *keys, int *v4)
{
	struct st *s = c;

	if (s->sc->miss_sc)
		return WP_MISSING;
	*keys = 3;
	*v4 = 0;
	return 0;
}
static int s_lock(void *c, int *v)
{
	struct st *s = c;

	if (s->sc->miss_mkb)
		return WP_MISSING;
	*v = s->sc->lock;
	return 0;
}
static int s_unl(void *c, int *v)
{
	struct st *s = c;

	if (s->sc->miss_mkb)
		return WP_MISSING;
	*v = s->sc->unl;
	return 0;
}
static int locked(struct st *s) { return s->sc->lock == 1 && s->sc->unl == 0; }
static int s_set(void *c, const char *ssid, const char *pw)
{
	struct st *s = c;

	(void)ssid;
	if (s->sc->miss_set)
		return WP_MISSING;
	snprintf(s->pw, sizeof s->pw, "%s", pw);
	s->has_item = 1;
	return 1;
}
static int s_copy(void *c, const char *ssid, char *out, size_t cap)
{
	struct st *s = c;

	(void)ssid;
	if (s->sc->miss_copy)
		return WP_MISSING;
	if (!s->has_item || locked(s))      /* XW5-7: NULL without querying while locked */
		return -1;
	snprintf(out, cap, "%s", s->pw);
	return (int)strlen(out);
}
static int s_remove(void *c, const char *ssid)
{
	struct st *s = c;
	int had = s->has_item;

	(void)ssid;
	if (s->sc->miss_remove)
		return WP_MISSING;
	s->has_item = 0;
	return had;
}
static int s_sec(void *c, const char *ssid, struct wp_item *it)
{
	struct st *s = c;

	(void)ssid;
	if (s->sc->miss_sec)
		return WP_MISSING;
	it->status = s->sc->sec_override ? s->sc->sec_override : s->has_item ? 0 : -25300;
	snprintf(it->group, sizeof it->group, "apple");
	snprintf(it->cls, sizeof it->cls, "ck");
	it->sync = 1;
	return 0;
}

static const char *cur_en0, *cur_svc;
static const char *test_getenv(const char *n)
{
	if (strcmp(n, "WIFID_PROBE_EN0_TIMEOUT") == 0)
		return cur_en0;
	if (strcmp(n, "WIFID_PROBE_SVC_TIMEOUT") == 0)
		return cur_svc;
	return NULL;
}

static int failed, checks;

static void check(int ok, const char *case_name, const char *what)
{
	checks++;
	if (!ok) {
		failed++;
		printf("  CHECK FAILED [%s]: %s\n", case_name, what);
	}
}

static void run_case(const struct sc *sc)
{
	struct st st = { sc, 0, 0, 0, "" };
	struct wp_ops ops = { &st, s_now, s_sleep, s_en0, s_svc, s_state, s_lock, s_unl, s_set, s_copy, s_remove, s_sec };
	char *argv[8] = { "wifid-probe" };
	char steps[64], buf[8192], *p, *line, *save = NULL;
	int argc = 1, rc;
	FILE *out = tmpfile();
	size_t n;
	int i, gaps_seen = 0;

	snprintf(steps, sizeof steps, "%s", sc->steps);
	for (p = strtok_r(steps, " ", &save); p && argc < 7; p = strtok_r(NULL, " ", &save))
		argv[argc++] = p;
	cur_en0 = sc->env_en0;
	cur_svc = sc->env_svc;
	rc = wp_main(argc, argv, &ops, test_getenv, "host-test", 4242, out);
	rewind(out);
	n = fread(buf, 1, sizeof buf - 1, out);
	buf[n] = 0;
	fclose(out);

	printf("case %s: rc=%d\n", sc->name, rc);
	save = NULL;
	{
		char copy[8192];

		strcpy(copy, buf);
		for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (strncmp(line, "WIFI GAP:", 9) == 0 || strstr(line, "RESULT"))
				printf("  %s\n", line);
			if (strncmp(line, "WIFI GAP:", 9) == 0)
				gaps_seen++;
		}
	}
	check(rc == (sc->expect_rc ? sc->expect_rc : sc->expect_gaps ? 1 : 0), sc->name, "exit code");
	{
		char want[64];

		snprintf(want, sizeof want, "RESULT %s gaps=%d", sc->expect_gaps ? "fail" : "pass", sc->expect_gaps);
		check(strstr(buf, want) != NULL, sc->name, want);
	}
	check(gaps_seen == sc->expect_gaps, sc->name, "number of WIFI GAP lines equals gaps=N");
	for (i = 0; i < 6 && sc->want[i]; i++)
		check(strstr(buf, sc->want[i]) != NULL, sc->name, sc->want[i]);
	check(rc == 2 || strstr(buf, "WIFI PROBE: start wifid-probe build=host-test") != NULL, sc->name, "start line");
	check(strstr(buf, "pixos-wifid-probe-") == NULL && strstr(buf, "probe-pw-") == NULL, sc->name,
	      "no SSID or password text in the output");
	check(st.clock >= sc->min_clock, sc->name, "fake clock ran at least the timeout");
}

#define NONE (-1)
/* Defaults: interface and service appear at once, MKB unlocked since boot; a designator after it overrides. */
#define C(...) { .en0_at = 0, .svc_at = 0, .unl = 1, __VA_ARGS__ }

static const struct sc cases[] = {
	C(.name = "pass", .steps = "", .en0_at = 3, .svc_at = 5,
	  .want = { "WIFI CONFIGD: en0 ifindex=4 after 3s", "WIFI CONFIGD: Setup service for en0 present after 2s id=36",
	            "WIFI CONFIGD: State Interface/en0 keys=3 Global/IPv4=no", "WIFI KEYCHAIN: MKB lockstate=0 unlockedSinceBoot=1",
	            "SecItemCopyMatching(AirPort) status=0 group=apple class=ck sync=y", "CopyPassword(removed) -> nothing" }),
	C(.name = "pass-keychain-lines", .steps = "keychain",
	  .want = { "SetPassword rc=1", "CopyPassword(before) -> nothing", "CopyPassword(after) -> same len=", "RemovePassword rc=1" }),
	C(.name = "set-symbol-missing", .steps = "keychain", .miss_set = 1, .expect_gaps = 4,
	  .want = { "WIFI GAP: WiFiSecuritySetPassword symbol missing", "WIFI GAP: CopyPassword(after) returned nothing",
	            "WIFI GAP: SecItemCopyMatching(AirPort) OSStatus -25300 (errSecItemNotFound)", "WIFI GAP: RemovePassword rc=0" }),
	C(.name = "copy-symbol-missing", .steps = "keychain", .miss_copy = 1, .expect_gaps = 1,
	  .want = { "WIFI GAP: WiFiSecurityCopyPassword symbol missing", "SetPassword rc=1" }),
	C(.name = "remove-symbol-missing", .steps = "keychain", .miss_remove = 1, .expect_gaps = 2,
	  .want = { "WIFI GAP: WiFiSecurityRemovePassword symbol missing", "WIFI GAP: CopyPassword(removed) still returns a password" }),
	C(.name = "mkb-symbols-missing", .steps = "keychain", .miss_mkb = 1, .expect_gaps = 2,
	  .want = { "WIFI GAP: MKBGetDeviceLockState symbol missing", "WIFI GAP: MKBUserUnlockedSinceBoot symbol missing" }),
	C(.name = "mkb-locked", .steps = "keychain", .lock = 1, .unl = 0, .expect_gaps = 2,
	  .want = { "WIFI KEYCHAIN: MKB lockstate=1 unlockedSinceBoot=0", "WIFI GAP: MKB locked (lockstate=1)",
	            "WIFI GAP: CopyPassword(after) returned nothing" }),
	C(.name = "secitem--34018", .steps = "keychain", .sec_override = -34018, .expect_gaps = 1,
	  .want = { "status=-34018 group=- class=- sync=y", "WIFI GAP: SecItemCopyMatching(AirPort) OSStatus -34018 (errSecMissingEntitlement)" }),
	C(.name = "secitem--25291", .steps = "keychain", .sec_override = -25291, .expect_gaps = 1,
	  .want = { "WIFI GAP: SecItemCopyMatching(AirPort) OSStatus -25291 (errSecNotAvailable)" }),
	C(.name = "secitem--25300", .steps = "keychain", .sec_override = -25300, .expect_gaps = 1,
	  .want = { "WIFI GAP: SecItemCopyMatching(AirPort) OSStatus -25300 (errSecItemNotFound)" }),
	C(.name = "sec-symbols-missing", .steps = "keychain", .miss_sec = 1, .expect_gaps = 1,
	  .want = { "WIFI GAP: SecItemCopyMatching symbol or constant missing" }),
	C(.name = "en0-timeout-120s", .steps = "wait-configd", .en0_at = NONE, .svc_at = NONE, .expect_gaps = 1, .min_clock = 120,
	  .want = { "WIFI GAP: no en0 after 120s", "service wait skipped" }),
	C(.name = "en0-timeout-short-env", .steps = "wait-configd", .env_en0 = "10", .en0_at = NONE, .svc_at = NONE,
	  .expect_gaps = 1, .min_clock = 10, .want = { "WIFI GAP: no en0 after 10s" }),
	C(.name = "service-timeout-30s", .steps = "wait-configd", .en0_at = 2, .svc_at = NONE, .expect_gaps = 1, .min_clock = 32,
	  .want = { "WIFI CONFIGD: en0 ifindex=4 after 2s", "WIFI GAP: configd has no Setup service for en0 after 30s",
	            "WIFI CONFIGD: State Interface/en0 keys=3" }),
	C(.name = "service-timeout-short-env", .steps = "wait-configd", .env_svc = "5", .svc_at = NONE, .expect_gaps = 1,
	  .min_clock = 5, .want = { "WIFI GAP: configd has no Setup service for en0 after 5s" }),
	C(.name = "store-error-then-ok", .steps = "wait-configd", .svc_at = 2, .store_err_polls = 2, .expect_gaps = 1,
	  .want = { "WIFI GAP: configd store read failed rc=-1", "Setup service for en0 present" }),
	C(.name = "sc-symbols-missing", .steps = "wait-configd", .miss_sc = 1, .expect_gaps = 2,
	  .want = { "WIFI GAP: SystemConfiguration symbol missing, cannot read the Setup service list",
	            "WIFI GAP: SystemConfiguration symbol missing, cannot read the State keys" }),
	C(.name = "bad-env-value", .steps = "keychain", .env_en0 = "abc", .expect_gaps = 1,
	  .want = { "WIFI GAP: WIFID_PROBE_EN0_TIMEOUT is not a number of seconds in (0, 3600], using 120" }),
	C(.name = "unknown-step", .steps = "bogus", .expect_gaps = 1, .expect_rc = 2,
	  .want = { "WIFI GAP: unknown step 'bogus'" }),
};

int main(void)
{
	size_t i;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		run_case(&cases[i]);
	}
	printf("%d checks, %d failed\n", checks, failed);
	if (failed) {
		printf("RESULT: FAIL wifid_probe_host_test\n");
		return 1;
	}
	printf("RESULT: PASS wifid_probe_host_test\n");
	return 0;
}
