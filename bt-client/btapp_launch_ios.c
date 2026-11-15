/* btapp-launch (T-2d63): one-shot launchd job (user mobile) that brings BTApp to the foreground, then Settings in front of it
 * (BTApp's background control window), then BTApp again. Each open is
 *   [[FBSSystemService alloc] init] openApplication:BUNDLE options:@{FBSOpenApplicationOptionKeyUnlockDevice: @YES}
 *                                   clientPort:[service createClientPort] withResult:^(NSError *)]
 * the request SpringBoardServices' SBSLaunchApplicationWithIdentifierAndURLAndLaunchOptions sends (0x19151e13c: alloc/init
 * 0x19151e1a0, createClientPort 0x19151e1a8, openApplication:options:clientPort:withResult: 0x19151e2fc, a 10 s wait
 * 0x19151e310; its block 0x19151e620 reads the NSError code) [bin, dyld cache 20H392]. Calling FBS directly keeps the raw
 * NSError domain/code (SBS folds it through a 9-entry table at 0x191579f08) and passes the FBS unlock key unconverted.
 * Server side: lsd and SpringBoard trust a caller holding com.apple.springboard.launchapplications (research T-9524 Q2.2); the
 * job carries it. A locked device refuses a foreground open (error 7 path, research Q2.6, U4 open): the unlock key is the
 * documented answer, whether it works without a passcode on this root is what the log line shows.
 * Lines go to stdout = the job's /dev/null (xnu patch 0048 mirror: boot-arg sweetnull=...,btapp-launch). */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef BTC_BUILD
#define BTC_BUILD "dev"
#endif

typedef void *id;
typedef void *SEL;

static SEL (*sel_registerName_)(const char *);
static id (*objc_getClass_)(const char *);
static void *objc_msgSend_;
#define MSG0(o, s) ((id (*)(id, SEL))objc_msgSend_)((o), sel_registerName_(s))

/* one result slot and one semaphore per try (as SBS does per call): a reply that arrives after its try timed out writes and
 * signals only its own slot, never a later try's (review T-2d63 LOW 5). A timed-out slot is never freed (the late block may
 * still run); a replied slot is not freed either, since dispatch_semaphore_signal may still be returning in the block. At most
 * 48 tries in a run, so the leak is bounded and small. */
struct res {
	volatile long code;
	volatile int done;
	char domain[64];
	dispatch_semaphore_t sem;
};
static unsigned gaps;

static unsigned
now_s(void)
{
	struct timespec ts;

	return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? (unsigned)ts.tv_sec : 0;
}

static const struct step {
	const char *bundle;	/* an app to open, or a URL (contains ':') opened with SBSOpenSensitiveURLAndUnlock */
	unsigned after_s;	/* wait after the previous step succeeded */
	unsigned tries;		/* one try every 10 s */
} plan[] = {
	{ "com.pixos.btapp", 0, 36 },		/* fg1: SpringBoard may still be coming up: 36 x (<= 15 s wait + 10 s) ~= 15 min */
	{ "prefs:root=Bluetooth", 120, 6 },	/* bg1 control: Settings > Bluetooth in front of BTApp (sprint-bt: the camera frame of "Bluetooth on") */
	{ "com.pixos.btapp", 60, 6 },		/* fg2 (the pair step runs here when a peer is configured) */
	{ "prefs:root=Bluetooth", 130, 6 },	/* sprint-bt: Settings > Bluetooth again after BTApp's run, stays in front */
};

/* sprint-bt: URL open, as SpringBoardServices exports it: Boolean SBSOpenSensitiveURLAndUnlock(CFURLRef, Boolean) (iOS 16.7
 * 20H392 _SBSOpenSensitiveURLAndUnlock 0x19151ebf8 -> [[LSApplicationWorkspace defaultWorkspace] openSensitiveURL:withOptions:];
 * lsd requires com.apple.springboard.opensensitiveurl, btapp-launch.entitlements carries it) [bin, read in the dyld cache].
 * 1 = accepted, 0 = refused/not available (GAP printed). */
static int
open_url(const char *url, unsigned try_n, unsigned t0)
{
	static unsigned char (*sbs_open)(void *, unsigned char);
	static void *(*url_create)(void *, const unsigned char *, long, unsigned, void *);	/* CFURLCreateWithBytes(alloc, bytes, len, CFStringEncoding, base) */
	static void (*cf_release)(void *);
	void *u;
	unsigned char rc;

	if (!sbs_open) {
		void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
		void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);

		sbs_open = sbs ? dlsym(sbs, "SBSOpenSensitiveURLAndUnlock") : 0;
		url_create = cf ? dlsym(cf, "CFURLCreateWithBytes") : 0;
		cf_release = cf ? dlsym(cf, "CFRelease") : 0;
		if (!sbs_open || !url_create || !cf_release) {
			printf("BT LAUNCH GAP: SBSOpenSensitiveURLAndUnlock/CFURLCreateWithBytes/CFRelease missing (sbs=%s)\n", sbs ? "loaded" : "not-loaded");
			gaps++;
			sbs_open = 0;
			return 0;
		}
	}
	u = url_create(0, (const unsigned char *)url, (long)strlen(url), 0x08000100 /* kCFStringEncodingUTF8 */, 0);
	if (!u) {
		printf("BT LAUNCH GAP: CFURL nil for %s\n", url);
		gaps++;
		return 0;
	}
	rc = sbs_open(u, 1);
	cf_release(u);
	printf("BT LAUNCH: open url=%s try=%u t=%us result=%s\n", url, try_n, now_s() - t0, rc ? "ok" : "refused");
	return rc ? 1 : 0;
}

/* 1 = opened, 0 = error (code/domain logged), -1 = no reply within 15 s. noinline: inlined into main's new URL branch the
 * selector loads were no longer resolvable by callsite-check.py (sprint-bt build, 2 sites) */
static int __attribute__((noinline))
open_app(id service, const char *bundle, id opts, unsigned try_n, unsigned t0)
{
	id nsstr = objc_getClass_("NSString");
	id bid = ((id (*)(id, SEL, const char *))objc_msgSend_)(nsstr, sel_registerName_("stringWithUTF8String:"), bundle);
	unsigned port;
	struct res *r;
	int ok;

	if (bid == 0) {
		printf("BT LAUNCH GAP: bundle id string nil for %s\n", bundle);
		gaps++;
		return 0;
	}
	r = calloc(1, sizeof *r);
	if (r == 0 || (r->sem = dispatch_semaphore_create(0)) == 0) {
		printf("BT LAUNCH GAP: no memory for the result slot of try %u (%s)\n", try_n, bundle);
		gaps++;
		return -1;
	}
	r->code = -1;
	port = ((unsigned (*)(id, SEL))objc_msgSend_)(service, sel_registerName_("createClientPort"));
	void (^done)(id) = ^(id err) {
		if (err) {
			id dom = MSG0(err, "domain");
			const char *d = dom ? ((const char *(*)(id, SEL))objc_msgSend_)(dom, sel_registerName_("UTF8String")) : 0;
			r->code = ((long (*)(id, SEL))objc_msgSend_)(err, sel_registerName_("code"));
			snprintf(r->domain, sizeof r->domain, "%s", d ? d : "unreadable");
		} else {
			r->code = 0;
		}
		r->done = 1;
		dispatch_semaphore_signal(r->sem);
	};
	((void (*)(id, SEL, id, id, unsigned, void (^)(id)))objc_msgSend_)(service,
	    sel_registerName_("openApplication:options:clientPort:withResult:"), bid, opts, port, done);
	if (dispatch_semaphore_wait(r->sem, dispatch_time(DISPATCH_TIME_NOW, 15LL * NSEC_PER_SEC)) != 0 || !r->done) {
		/* a late reply lands in this try's slot only */
		printf("BT LAUNCH: open id=%s try=%u t=%us result=timeout(15s)\n", bundle, try_n, now_s() - t0);
		ok = -1;
	} else if (r->code == 0) {
		printf("BT LAUNCH: open id=%s try=%u t=%us result=ok\n", bundle, try_n, now_s() - t0);
		ok = 1;
	} else {
		printf("BT LAUNCH: open id=%s try=%u t=%us result=error domain=%s code=%ld\n", bundle, try_n, now_s() - t0, r->domain,
		    r->code);
		ok = 0;
	}
	((void (*)(id, SEL, unsigned))objc_msgSend_)(service, sel_registerName_("cleanupClientPort:"), port);
	return ok;
}

int
main(void)
{
	void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	void *fbs = dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);
	void *fo = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
	id *unlock_key, *cftrue, cls, service, opts = 0;
	unsigned t0 = now_s(), ok_steps = 0, i, k;
	int r = 0;

	setvbuf(stdout, 0, _IONBF, 0);
	(void)fo;
	printf("BT LAUNCH: start build=%s steps=%u\n", BTC_BUILD, (unsigned)(sizeof plan / sizeof plan[0]));
	sel_registerName_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	objc_getClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	objc_msgSend_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	if (!sel_registerName_ || !objc_getClass_ || !objc_msgSend_ || !fbs) {
		printf("BT LAUNCH GAP: libobjc symbols or FrontBoardServices missing (fbs=%s)\n", fbs ? "loaded" : "not-loaded");
		printf("BT LAUNCH: RESULT fail steps_ok=0/%u gaps=1\n", (unsigned)(sizeof plan / sizeof plan[0]));
		return 2;
	}
	cls = objc_getClass_("FBSSystemService");
	service = cls ? MSG0(MSG0(cls, "alloc"), "init") : 0;
	unlock_key = dlsym(fbs, "FBSOpenApplicationOptionKeyUnlockDevice");
	cftrue = dlsym(RTLD_DEFAULT, "kCFBooleanTrue");
	if (unlock_key && *unlock_key && cftrue && *cftrue) {
		const char *ks = ((const char *(*)(id, SEL))objc_msgSend_)(*unlock_key, sel_registerName_("UTF8String"));

		opts = ((id (*)(id, SEL, const id *, const id *, unsigned long))objc_msgSend_)(objc_getClass_("NSDictionary"),
		    sel_registerName_("dictionaryWithObjects:forKeys:count:"), cftrue, unlock_key, 1);
		printf("BT LAUNCH: options %s=1%s\n", ks ? ks : "unreadable-key", opts ? "" : " (dictionary nil)");
	}
	if (opts == 0) {
		printf("BT LAUNCH GAP: FBSOpenApplicationOptionKeyUnlockDevice options not built: opening with nil options\n");
		gaps++;
	}
	if (service == 0) {
		printf("BT LAUNCH GAP: FBSSystemService missing\n");
		printf("BT LAUNCH: RESULT fail steps_ok=0/%u gaps=%u\n", (unsigned)(sizeof plan / sizeof plan[0]), gaps + 1);
		return 2;
	}
	for (i = 0; i < sizeof plan / sizeof plan[0]; i++) {
		if (plan[i].after_s)
			sleep(plan[i].after_s);
		for (k = 1; k <= plan[i].tries; k++) {
			r = strchr(plan[i].bundle, ':') ? open_url(plan[i].bundle, k, t0) : open_app(service, plan[i].bundle, opts, k, t0);
			if (r == 1)
				break;
			sleep(10);
		}
		if (r != 1) {
			printf("BT LAUNCH GAP: step %u (%s) not opened after %u tries\n", i + 1, plan[i].bundle, plan[i].tries);
			gaps++;
			if (i == 0)
				break;	/* without BTApp in front the rest measures nothing */
			continue;
		}
		ok_steps++;
	}
	printf("BT LAUNCH: RESULT %s steps_ok=%u/%u gaps=%u t=%us\n", ok_steps == sizeof plan / sizeof plan[0] ? "pass" : "fail", ok_steps,
	    (unsigned)(sizeof plan / sizeof plan[0]), gaps, now_s() - t0);
	return ok_steps == sizeof plan / sizeof plan[0] ? 0 : 1;
}
