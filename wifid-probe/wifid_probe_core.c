/* wifid-probe core: sequence, timeouts and log lines. See wifid_probe.h. */
#include "wifid_probe.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct run {
	FILE *out;
	int gaps;
};

static void say(struct run *r, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(r->out, fmt, ap);
	va_end(ap);
	fputc('\n', r->out);
	fflush(r->out);
}

#define GAP(r, ...) do { (r)->gaps++; fputs("WIFI GAP: ", (r)->out); say((r), __VA_ARGS__); } while (0)

/* FNV-1a 32-bit: not a secret-grade hash, only lets two lines say "same" or "different" without the bytes. */
static unsigned hash32(const char *s)
{
	unsigned h = 2166136261u;

	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

static const char *osstatus_name(int s)
{
	switch (s) {
	case -34018: return " (errSecMissingEntitlement)";
	case -25291: return " (errSecNotAvailable)";
	case -25300: return " (errSecItemNotFound)";
	default: return "";
	}
}

/* ---- wait-configd ---- */

static void wait_configd(const struct wp_ops *o, const struct wp_cfg *cfg, struct run *r)
{
	double t0 = o->now(o->ctx), t;
	unsigned idx = 0;
	int idlen = 0, rc = 0, store_gap = 0, found = 0;

	for (;;) {
		idx = o->en0_index(o->ctx);
		t = o->now(o->ctx) - t0;
		if (idx != 0 || t >= cfg->en0_timeout)
			break;
		o->sleep_ms(o->ctx, 500);
	}
	if (idx == 0) {
		GAP(r, "no en0 after %ds (the radio path has not come up; service wait skipped)", (int)cfg->en0_timeout);
		return;
	}
	say(r, "WIFI CONFIGD: en0 ifindex=%u after %ds", idx, (int)t);

	t0 = o->now(o->ctx);
	for (;;) {
		rc = o->store_en0_service(o->ctx, &idlen);
		t = o->now(o->ctx) - t0;
		if (rc == 1) {
			found = 1;
			break;
		}
		if (rc == WP_MISSING) {
			GAP(r, "SystemConfiguration symbol missing, cannot read the Setup service list");
			break;
		}
		if (rc < 0 && !store_gap) {
			store_gap = 1;
			GAP(r, "configd store read failed rc=%d (polling on)", rc);
		}
		if (t >= cfg->svc_timeout)
			break;
		o->sleep_ms(o->ctx, 500);
	}
	if (found)
		say(r, "WIFI CONFIGD: Setup service for en0 present after %ds id=%d", (int)t, idlen);
	else if (rc != WP_MISSING)
		GAP(r, "configd has no Setup service for en0 after %ds", (int)cfg->svc_timeout);

	{
		int keys = 0, v4 = 0;

		rc = o->store_state(o->ctx, &keys, &v4);
		if (rc == 0)
			say(r, "WIFI CONFIGD: State Interface/en0 keys=%d Global/IPv4=%s", keys, v4 ? "yes" : "no");
		else if (rc == WP_MISSING)
			GAP(r, "SystemConfiguration symbol missing, cannot read the State keys");
		else
			GAP(r, "configd State read failed rc=%d", rc);
	}
}

/* ---- keychain ---- */

static void describe(struct run *r, const char *what, int len, const char *pw_buf)
{
	if (len < 0)
		say(r, "WIFI KEYCHAIN: %s -> nothing", what);
	else
		say(r, "WIFI KEYCHAIN: %s -> string len=%d h=%08x", what, len, hash32(pw_buf));
}

static void keychain(const struct wp_ops *o, const struct wp_cfg *cfg, struct run *r)
{
	char ssid[48], pw[48], got[128];
	int lock = 0, unl = 0, lrc, urc, rc, len;
	int copy_missing = 0;
	struct wp_item it;

	snprintf(ssid, sizeof ssid, "pixos-wifid-probe-%d", cfg->seed);
	snprintf(pw, sizeof pw, "probe-pw-%d-not-a-secret", cfg->seed);

	lrc = o->mkb_lockstate(o->ctx, &lock);
	urc = o->mkb_unlocked_since_boot(o->ctx, &unl);
	if (lrc == 0 && urc == 0)
		say(r, "WIFI KEYCHAIN: MKB lockstate=%d unlockedSinceBoot=%d", lock, unl);
	else
		say(r, "WIFI KEYCHAIN: MKB lockstate=%s unlockedSinceBoot=%s", lrc == 0 ? "ok" : "n/a", urc == 0 ? "ok" : "n/a");
	if (lrc != 0)
		GAP(r, "MKBGetDeviceLockState symbol missing");
	if (urc != 0)
		GAP(r, "MKBUserUnlockedSinceBoot symbol missing");
	if (lrc == 0 && urc == 0 && lock == 1 && unl == 0)
		GAP(r, "MKB locked (lockstate=1) and not unlocked since boot: CopyPassword returns NULL without querying (XW5-7)");

	/* the made-up item, hashed so a leftover from an earlier run is visible without printing it */
	say(r, "WIFI KEYCHAIN: test item ssid len=%d h=%08x password len=%d h=%08x",
	    (int)strlen(ssid), hash32(ssid), (int)strlen(pw), hash32(pw));

	got[0] = 0;
	len = o->wifi_copy(o->ctx, ssid, got, sizeof got);
	if (len == WP_MISSING) {
		copy_missing = 1;
		GAP(r, "WiFiSecurityCopyPassword symbol missing");
	} else {
		describe(r, "CopyPassword(before)", len, got);
	}

	rc = o->wifi_set(o->ctx, ssid, pw);
	if (rc == WP_MISSING) {
		GAP(r, "WiFiSecuritySetPassword symbol missing");
	} else {
		say(r, "WIFI KEYCHAIN: SetPassword rc=%d", rc);
		if (rc != 1)
			GAP(r, "SetPassword rc=%d (expected 1; MobileWiFi's os_log line carries the securityd status)", rc);
	}

	if (!copy_missing) {
		got[0] = 0;
		len = o->wifi_copy(o->ctx, ssid, got, sizeof got);
		if (len < 0) {
			say(r, "WIFI KEYCHAIN: CopyPassword(after) -> nothing");
			GAP(r, "CopyPassword(after) returned nothing, expected the same password");
		} else if (strcmp(got, pw) == 0) {
			say(r, "WIFI KEYCHAIN: CopyPassword(after) -> same len=%d h=%08x", len, hash32(got));
		} else {
			say(r, "WIFI KEYCHAIN: CopyPassword(after) -> different len=%d h=%08x", len, hash32(got));
			GAP(r, "CopyPassword(after) returned a different password");
		}
	}

	memset(&it, 0, sizeof it);
	if (o->sec_query(o->ctx, ssid, &it) == WP_MISSING) {
		GAP(r, "SecItemCopyMatching symbol or constant missing");
	} else {
		say(r, "WIFI KEYCHAIN: SecItemCopyMatching(AirPort) status=%d group=%s class=%s sync=%s", it.status,
		    it.status == 0 ? it.group : "-", it.status == 0 ? it.cls : "-",
		    it.sync > 0 ? "y" : it.sync == 0 ? "n" : "?");
		if (it.status != 0)
			GAP(r, "SecItemCopyMatching(AirPort) OSStatus %d%s", it.status, osstatus_name(it.status));
	}

	rc = o->wifi_remove(o->ctx, ssid);
	if (rc == WP_MISSING) {
		GAP(r, "WiFiSecurityRemovePassword symbol missing");
	} else {
		say(r, "WIFI KEYCHAIN: RemovePassword rc=%d", rc);
		if (rc != 1)
			GAP(r, "RemovePassword rc=%d (expected 1)", rc);
	}

	if (!copy_missing) {
		got[0] = 0;
		len = o->wifi_copy(o->ctx, ssid, got, sizeof got);
		if (len < 0) {
			say(r, "WIFI KEYCHAIN: CopyPassword(removed) -> nothing");
		} else {
			say(r, "WIFI KEYCHAIN: CopyPassword(removed) -> string len=%d h=%08x", len, hash32(got));
			GAP(r, "CopyPassword(removed) still returns a password after RemovePassword");
		}
	}
}

/* ---- entry points ---- */

static double read_timeout(const char *(*getenv_fn)(const char *), const char *name, double def, struct wp_cfg *cfg,
			   FILE *out)
{
	const char *v = getenv_fn ? getenv_fn(name) : NULL;
	char *end;
	double d;

	if (v == NULL || *v == 0)
		return def;
	d = strtod(v, &end);
	if (*end != 0 || d <= 0 || d > 3600) {
		cfg->gaps++;
		fprintf(out, "WIFI GAP: %s is not a number of seconds in (0, 3600], using %d\n", name, (int)def);
		fflush(out);
		return def;
	}
	return d;
}

int wp_parse(int argc, char **argv, const char *(*getenv_fn)(const char *), struct wp_cfg *cfg, FILE *out)
{
	int i, bad = 0;

	cfg->do_wait = cfg->do_keychain = 0;
	cfg->gaps = 0;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "wait-configd") == 0) {
			cfg->do_wait = 1;
		} else if (strcmp(argv[i], "keychain") == 0) {
			cfg->do_keychain = 1;
		} else {
			cfg->gaps++;
			bad = 1;
			fprintf(out, "WIFI GAP: unknown step '%s' (steps: wait-configd keychain)\n", argv[i]);
			fflush(out);
		}
	}
	if (!bad && argc == 1)
		cfg->do_wait = cfg->do_keychain = 1;
	cfg->en0_timeout = read_timeout(getenv_fn, "WIFID_PROBE_EN0_TIMEOUT", 120, cfg, out);
	cfg->svc_timeout = read_timeout(getenv_fn, "WIFID_PROBE_SVC_TIMEOUT", 30, cfg, out);
	return bad;
}

int wp_run(const struct wp_ops *ops, struct wp_cfg *cfg, FILE *out)
{
	struct run r = { out, cfg->gaps };

	say(&r, "WIFI PROBE: start wifid-probe build=%s", cfg->build ? cfg->build : "unknown");
	if (cfg->do_wait)
		wait_configd(ops, cfg, &r);
	if (cfg->do_keychain)
		keychain(ops, cfg, &r);
	say(&r, "WIFI PROBE: RESULT %s gaps=%d", r.gaps ? "fail" : "pass", r.gaps);
	return r.gaps;
}

int wp_main(int argc, char **argv, const struct wp_ops *ops, const char *(*getenv_fn)(const char *),
	    const char *build, int seed, FILE *out)
{
	struct wp_cfg cfg;
	int bad;

	memset(&cfg, 0, sizeof cfg);
	cfg.build = build;
	cfg.seed = seed;
	bad = wp_parse(argc, argv, getenv_fn, &cfg, out);
	if (bad) {
		fprintf(out, "WIFI PROBE: RESULT fail gaps=%d\n", cfg.gaps);
		fflush(out);
		return 2;
	}
	return wp_run(ops, &cfg, out) ? 1 : 0;
}
