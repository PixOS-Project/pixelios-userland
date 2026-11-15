/* wifid-probe iOS backend: every system function is found with dlsym, so a missing symbol becomes a
 * "WIFI GAP:" line (WP_MISSING) instead of a launch failure, and the file needs no iOS SDK headers.
 * Prototypes are written from the public CoreFoundation / SystemConfiguration / Security headers (read from
 * memory of the SDK, [guess] until a build against the iOS SDK) and, for MobileWiFi and MobileKeyBag, from the
 * disassembly in X-W5 s.2 (Set 0x1b3037754 (ssid,pw)->BOOL, Copy 0x1b3037570 (ssid)->CFString,
 * Remove 0x1b30379c0 (ssid)->BOOL; MKBGetDeviceLockState 0x1ae3df468 passes x0 through and returns an int;
 * MKBUserUnlockedSinceBoot 0x1ae3dfb0c takes an int handle in w0, called with 0 here).
 * Not yet built for iOS and never run: syntax-checked on the host only (tools: wifid-probe/ios-syntax-check.sh).
 */
#include "wifid_probe.h"

#include <dlfcn.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef const void *CFTypeRef;
typedef long CFIndex;
typedef unsigned char Boolean;
#define UTF8 0x08000100u   /* kCFStringEncodingUTF8 */

#define FW "/System/Library/Frameworks/"
#define PFW "/System/Library/PrivateFrameworks/"

static void *h_cf, *h_sc, *h_sec, *h_mw, *h_mkb;

static void *sym(void *h, const char *name)
{
	return h ? dlsym(h, name) : NULL;
}

/* ---- CoreFoundation helpers (all through dlsym) ---- */
static CFTypeRef (*CFStringCreateWithCString_)(CFTypeRef, const char *, unsigned);
static Boolean (*CFStringGetCString_)(CFTypeRef, char *, CFIndex, unsigned);
static void (*CFRelease_)(CFTypeRef);
static CFTypeRef (*CFDictionaryCreate_)(CFTypeRef, const CFTypeRef *, const CFTypeRef *, CFIndex, const void *, const void *);
static CFTypeRef (*CFDictionaryGetValue_)(CFTypeRef, CFTypeRef);
static CFIndex (*CFArrayGetCount_)(CFTypeRef);
static CFTypeRef (*CFArrayGetValueAtIndex_)(CFTypeRef, CFIndex);
static Boolean (*CFBooleanGetValue_)(CFTypeRef);
static CFTypeRef (*CFGetTypeID_)(CFTypeRef);
static CFTypeRef (*CFStringGetTypeID_)(void);
static CFTypeRef (*CFBooleanGetTypeID_)(void);
static const void *kCFTypeDictionaryKeyCallBacks_, *kCFTypeDictionaryValueCallBacks_;
static CFTypeRef kCFBooleanTrue_;

static int cf_ready;

static int cf_init(void)
{
	if (cf_ready)
		return cf_ready > 0 ? 0 : WP_MISSING;
	h_cf = dlopen(FW "CoreFoundation.framework/CoreFoundation", RTLD_NOW);
	CFStringCreateWithCString_ = sym(h_cf, "CFStringCreateWithCString");
	CFStringGetCString_ = sym(h_cf, "CFStringGetCString");
	CFRelease_ = sym(h_cf, "CFRelease");
	CFDictionaryCreate_ = sym(h_cf, "CFDictionaryCreate");
	CFDictionaryGetValue_ = sym(h_cf, "CFDictionaryGetValue");
	CFArrayGetCount_ = sym(h_cf, "CFArrayGetCount");
	CFArrayGetValueAtIndex_ = sym(h_cf, "CFArrayGetValueAtIndex");
	CFBooleanGetValue_ = sym(h_cf, "CFBooleanGetValue");
	CFGetTypeID_ = sym(h_cf, "CFGetTypeID");
	CFStringGetTypeID_ = sym(h_cf, "CFStringGetTypeID");
	CFBooleanGetTypeID_ = sym(h_cf, "CFBooleanGetTypeID");
	kCFTypeDictionaryKeyCallBacks_ = sym(h_cf, "kCFTypeDictionaryKeyCallBacks");
	kCFTypeDictionaryValueCallBacks_ = sym(h_cf, "kCFTypeDictionaryValueCallBacks");
	{
		CFTypeRef *t = sym(h_cf, "kCFBooleanTrue");

		kCFBooleanTrue_ = t ? *t : NULL;
	}
	cf_ready = CFStringCreateWithCString_ && CFStringGetCString_ && CFRelease_ && CFDictionaryCreate_ &&
		   CFDictionaryGetValue_ && CFArrayGetCount_ && CFArrayGetValueAtIndex_ && CFBooleanGetValue_ &&
		   CFGetTypeID_ && CFStringGetTypeID_ && CFBooleanGetTypeID_ &&
		   kCFTypeDictionaryKeyCallBacks_ && kCFTypeDictionaryValueCallBacks_ && kCFBooleanTrue_ ? 1 : -1;
	return cf_ready > 0 ? 0 : WP_MISSING;
}

static CFTypeRef cfstr(const char *s)
{
	return CFStringCreateWithCString_(NULL, s, UTF8);
}

/* CFString -> out; returns the UTF-8 length, -1 if v is not a CFString */
static int cfstr_get(CFTypeRef v, char *out, size_t cap)
{
	out[0] = 0;
	if (v == NULL || CFGetTypeID_(v) != CFStringGetTypeID_())
		return -1;
	if (!CFStringGetCString_(v, out, (CFIndex)cap, UTF8)) {
		out[0] = 0;
		return -1;
	}
	return (int)strlen(out);
}

/* ---- clock, interface ---- */
static double o_now(void *c)
{
	struct timespec ts;

	(void)c;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void o_sleep(void *c, int ms)
{
	(void)c;
	usleep((useconds_t)ms * 1000);
}

static unsigned o_en0(void *c)
{
	(void)c;
	return if_nametoindex("en0");
}

/* ---- SystemConfiguration (read-only) ---- */
static CFTypeRef (*SCDynamicStoreCreate_)(CFTypeRef, CFTypeRef, void *, void *);
static CFTypeRef (*SCDynamicStoreCopyKeyList_)(CFTypeRef, CFTypeRef);
static CFTypeRef (*SCDynamicStoreCopyValue_)(CFTypeRef, CFTypeRef);
static CFTypeRef store;
static int sc_ready;

static int sc_init(void)
{
	CFTypeRef name;

	if (cf_init())
		return WP_MISSING;
	if (sc_ready == 0) {
		h_sc = dlopen(FW "SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
		SCDynamicStoreCreate_ = sym(h_sc, "SCDynamicStoreCreate");
		SCDynamicStoreCopyKeyList_ = sym(h_sc, "SCDynamicStoreCopyKeyList");
		SCDynamicStoreCopyValue_ = sym(h_sc, "SCDynamicStoreCopyValue");
		sc_ready = SCDynamicStoreCreate_ && SCDynamicStoreCopyKeyList_ && SCDynamicStoreCopyValue_ ? 1 : -1;
	}
	if (sc_ready < 0)
		return WP_MISSING;
	if (store == NULL) {
		name = cfstr("wifid-probe");
		store = SCDynamicStoreCreate_(NULL, name, NULL, NULL);
		CFRelease_(name);
	}
	return store ? 0 : -1;
}

/* Setup:/Network/Service/<id>/Interface whose DeviceName is "en0" (HakiOS net_test.m test, XW5-11) */
static int o_store_en0_service(void *c, int *idlen)
{
	CFTypeRef pat, keys, devname;
	CFIndex i, n;
	int rc = sc_init(), found = 0;
	char key[256], dev[32];

	(void)c;
	if (rc)
		return rc;
	pat = cfstr("Setup:/Network/Service/[^/]+/Interface");
	keys = SCDynamicStoreCopyKeyList_(store, pat);
	CFRelease_(pat);
	if (keys == NULL)
		return 0;
	devname = cfstr("DeviceName");
	n = CFArrayGetCount_(keys);
	for (i = 0; i < n && !found; i++) {
		CFTypeRef k = CFArrayGetValueAtIndex_(keys, i), val = SCDynamicStoreCopyValue_(store, k);

		if (val == NULL)
			continue;
		if (cfstr_get(CFDictionaryGetValue_(val, devname), dev, sizeof dev) >= 0 && strcmp(dev, "en0") == 0 &&
		    cfstr_get(k, key, sizeof key) > 0) {
			/* key = Setup:/Network/Service/<id>/Interface */
			const char *id = key + strlen("Setup:/Network/Service/"), *slash = strchr(id, '/');

			*idlen = slash ? (int)(slash - id) : (int)strlen(id);
			found = 1;
		}
		CFRelease_(val);
	}
	CFRelease_(devname);
	CFRelease_(keys);
	return found;
}

static int count_keys(const char *pattern)
{
	CFTypeRef pat = cfstr(pattern), keys = SCDynamicStoreCopyKeyList_(store, pat);
	int n = 0;

	CFRelease_(pat);
	if (keys) {
		n = (int)CFArrayGetCount_(keys);
		CFRelease_(keys);
	}
	return n;
}

static int o_store_state(void *c, int *iface_keys, int *global_ipv4)
{
	int rc = sc_init();

	(void)c;
	if (rc)
		return rc;
	*iface_keys = count_keys("State:/Network/Interface/en0/.*");
	*global_ipv4 = count_keys("State:/Network/Global/IPv4") > 0;
	return 0;
}

/* ---- MobileKeyBag ---- */
static int (*MKBGetDeviceLockState_)(CFTypeRef);
static int (*MKBUserUnlockedSinceBoot_)(int);
static int mkb_ready;

static void mkb_init(void)
{
	if (mkb_ready)
		return;
	h_mkb = dlopen(PFW "MobileKeyBag.framework/MobileKeyBag", RTLD_NOW);
	MKBGetDeviceLockState_ = sym(h_mkb, "MKBGetDeviceLockState");
	MKBUserUnlockedSinceBoot_ = sym(h_mkb, "MKBUserUnlockedSinceBoot");
	mkb_ready = 1;
}

static int o_mkb_lock(void *c, int *v)
{
	(void)c;
	mkb_init();
	if (!MKBGetDeviceLockState_)
		return WP_MISSING;
	*v = MKBGetDeviceLockState_(NULL);
	return 0;
}

static int o_mkb_unl(void *c, int *v)
{
	(void)c;
	mkb_init();
	if (!MKBUserUnlockedSinceBoot_)
		return WP_MISSING;
	*v = MKBUserUnlockedSinceBoot_(0);
	return 0;
}

/* ---- MobileWiFi ---- */
static Boolean (*WiFiSecuritySetPassword_)(CFTypeRef, CFTypeRef);
static CFTypeRef (*WiFiSecurityCopyPassword_)(CFTypeRef);
static Boolean (*WiFiSecurityRemovePassword_)(CFTypeRef);
static int mw_ready;

static void mw_init(void)
{
	if (mw_ready)
		return;
	h_mw = dlopen(PFW "MobileWiFi.framework/MobileWiFi", RTLD_NOW);
	WiFiSecuritySetPassword_ = sym(h_mw, "WiFiSecuritySetPassword");
	WiFiSecurityCopyPassword_ = sym(h_mw, "WiFiSecurityCopyPassword");
	WiFiSecurityRemovePassword_ = sym(h_mw, "WiFiSecurityRemovePassword");
	mw_ready = 1;
}

static int o_wifi_set(void *c, const char *ssid, const char *pw)
{
	CFTypeRef s, p;
	int r;

	(void)c;
	mw_init();
	if (!WiFiSecuritySetPassword_ || cf_init())
		return WP_MISSING;
	s = cfstr(ssid);
	p = cfstr(pw);
	r = WiFiSecuritySetPassword_(s, p) ? 1 : 0;
	CFRelease_(s);
	CFRelease_(p);
	return r;
}

static int o_wifi_copy(void *c, const char *ssid, char *out, size_t cap)
{
	CFTypeRef s, r;
	int n;

	(void)c;
	mw_init();
	if (!WiFiSecurityCopyPassword_ || cf_init())
		return WP_MISSING;
	s = cfstr(ssid);
	r = WiFiSecurityCopyPassword_(s);
	CFRelease_(s);
	if (r == NULL)
		return -1;
	n = cfstr_get(r, out, cap);
	CFRelease_(r);
	return n;      /* -1 also when the result is not a CFString: reported as "nothing" */
}

static int o_wifi_remove(void *c, const char *ssid)
{
	CFTypeRef s;
	int r;

	(void)c;
	mw_init();
	if (!WiFiSecurityRemovePassword_ || cf_init())
		return WP_MISSING;
	s = cfstr(ssid);
	r = WiFiSecurityRemovePassword_(s) ? 1 : 0;
	CFRelease_(s);
	return r;
}

/* ---- Security: the item MobileWiFi stores (XW5-5): generic password, service AirPort, account ssid,
 * Synchronizable Any, system keychain; returns the attributes only, never the data ---- */
static CFTypeRef cst(const char *name)
{
	CFTypeRef *p = sym(h_sec, name);

	return p ? *p : NULL;
}

static int o_sec_query(void *c, const char *ssid, struct wp_item *it)
{
	int32_t (*SecItemCopyMatching_)(CFTypeRef, CFTypeRef *);
	CFTypeRef kClass, kClassGP, kSvc, kAcct, kSync, kSyncAny, kRetAttrs, kLimit, kLimitOne, kSysKc, kGroup, kAcc;
	CFTypeRef keys[7], vals[7], q, res = NULL, svc, acct, v;
	int32_t st;

	(void)c;
	h_sec = dlopen(FW "Security.framework/Security", RTLD_NOW);
	SecItemCopyMatching_ = sym(h_sec, "SecItemCopyMatching");
	kClass = cst("kSecClass");
	kClassGP = cst("kSecClassGenericPassword");
	kSvc = cst("kSecAttrService");
	kAcct = cst("kSecAttrAccount");
	kSync = cst("kSecAttrSynchronizable");
	kSyncAny = cst("kSecAttrSynchronizableAny");
	kRetAttrs = cst("kSecReturnAttributes");
	kLimit = cst("kSecMatchLimit");
	kLimitOne = cst("kSecMatchLimitOne");
	kSysKc = cst("kSecUseSystemKeychain");     /* private (SecItemPriv.h): MobileWiFi sets it (XW5-5) */
	kGroup = cst("kSecAttrAccessGroup");
	kAcc = cst("kSecAttrAccessible");
	if (!SecItemCopyMatching_ || !kClass || !kClassGP || !kSvc || !kAcct || !kSync || !kSyncAny || !kRetAttrs ||
	    !kLimit || !kLimitOne || !kSysKc || !kGroup || !kAcc || cf_init())
		return WP_MISSING;

	svc = cfstr("AirPort");
	acct = cfstr(ssid);
	keys[0] = kClass;    vals[0] = kClassGP;
	keys[1] = kSvc;      vals[1] = svc;
	keys[2] = kAcct;     vals[2] = acct;
	keys[3] = kSync;     vals[3] = kSyncAny;
	keys[4] = kRetAttrs; vals[4] = kCFBooleanTrue_;
	keys[5] = kLimit;    vals[5] = kLimitOne;
	keys[6] = kSysKc;    vals[6] = kCFBooleanTrue_;
	q = CFDictionaryCreate_(NULL, keys, vals, 7, kCFTypeDictionaryKeyCallBacks_, kCFTypeDictionaryValueCallBacks_);
	st = SecItemCopyMatching_(q, &res);
	it->status = st;
	strcpy(it->group, "?");
	strcpy(it->cls, "?");
	it->sync = -1;
	if (st == 0 && res != NULL) {
		if (cfstr_get(CFDictionaryGetValue_(res, kGroup), it->group, sizeof it->group) < 0)
			strcpy(it->group, "?");
		if (cfstr_get(CFDictionaryGetValue_(res, kAcc), it->cls, sizeof it->cls) < 0)
			strcpy(it->cls, "?");
		v = CFDictionaryGetValue_(res, kSync);
		if (v != NULL && CFGetTypeID_(v) == CFBooleanGetTypeID_())
			it->sync = CFBooleanGetValue_(v) ? 1 : 0;
	}
	if (res)
		CFRelease_(res);
	CFRelease_(q);
	CFRelease_(svc);
	CFRelease_(acct);
	return 0;
}

static const char *env(const char *name)
{
	return getenv(name);
}

#ifndef WP_BUILD
#define WP_BUILD "dev"
#endif

int main(int argc, char **argv)
{
	static const struct wp_ops ops = {
		NULL, o_now, o_sleep, o_en0, o_store_en0_service, o_store_state, o_mkb_lock, o_mkb_unl,
		o_wifi_set, o_wifi_copy, o_wifi_remove, o_sec_query,
	};

	/* MobileWiFi logs the securityd status through os_log; DT mode sends it to the console (HakiOS does the
	 * same). The launchd job sets it too; this covers a manual start. */
	if (getenv("OS_ACTIVITY_DT_MODE") == NULL) {
		setenv("OS_ACTIVITY_DT_MODE", "YES", 1);
		execv(argv[0], argv);
		fputs("WIFI GAP: cannot re-exec with OS_ACTIVITY_DT_MODE=YES\n", stdout);
		fputs("WIFI PROBE: RESULT fail gaps=1\n", stdout);
		return 3;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	return wp_main(argc, argv, &ops, env, WP_BUILD, (int)getpid(), stdout);
}
