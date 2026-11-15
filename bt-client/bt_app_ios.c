/* BTApp (T-2d63): a minimal UIKit app, com.pixos.btapp, installed at /Applications/BTApp.app so LaunchServices registers it as
 * a System app and SpringBoard can launch it into the foreground (docs/research/T-9524 "Minimal recipe"). It runs the bt-client
 * backend (bt_client_ios.c) as a services:nil, options:nil central for BTAPP_TOTAL_S seconds on a thread of its own, and the
 * app delegate records every UIKit state change with a counter snapshot, so bt_app_core.c splits the counts into fg/bg windows.
 * Plain C: the delegate class is built with class_addMethod; objc_msgSend is called through exact non-variadic prototypes
 * (callsite-check.py). Log channel: stdout and stderr are pointed at /dev/null here, whatever FrontBoard gave the process, and
 * xnu patch 0048 mirrors the /dev/null writes of a process named in boot-arg sweetnull=...,BTApp into the host log as
 * "|BTApp| text". Counts only, no identifier (rule 4). */
#include "bt_client.h"
#include "bt_mgr.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef BTC_BUILD
#define BTC_BUILD "dev"
#endif
#ifndef BTAPP_TOTAL_S
#define BTAPP_TOTAL_S 300	/* launcher: fg1 120 s, Settings in front (bg1) 60 s, then fg2 for the rest */
#endif
#ifndef BTAPP_CONN_AT_S
#define BTAPP_CONN_AT_S 30
#endif
#ifndef BTAPP_CONN_TRIES
#define BTAPP_CONN_TRIES 3
#endif
#ifndef BTAPP_CONN_TIMEOUT_S
#define BTAPP_CONN_TIMEOUT_S 10
#endif
#define NREC 32
#ifndef BTAPP_PAIR_AT_S
#define BTAPP_PAIR_AT_S 185	/* sprint-bt: the pair step in fg2 (after the Settings > Bluetooth window), only with a configured peer */
#endif
#ifndef BTAPP_PAIR_TIMEOUT_S
#define BTAPP_PAIR_TIMEOUT_S 90
#endif

int btm_ios_run(void (*out)(const char *), unsigned avail_s, unsigned power_s, unsigned scan_s);
int btm_ios_peer_configured(void);

typedef void *id;
typedef void *SEL;
typedef void (*IMP)(void);

extern int UIApplicationMain(int argc, char *argv[], id principal_class_name, id delegate_class_name);

static SEL (*sel_registerName_)(const char *);
static bool (*class_addMethod_)(id, SEL, IMP, const char *);
static id (*objc_allocateClassPair_)(id, const char *, size_t);
static void (*objc_registerClassPair_)(id);
static id (*objc_getClass_)(const char *);
static void *objc_msgSend_;
#define MSG0(o, s) ((id (*)(id, SEL))objc_msgSend_)((o), sel_registerName_(s))
#define MSG1(o, s, a) ((void (*)(id, SEL, id))objc_msgSend_)((o), sel_registerName_(s), (a))

struct rect {
	double x, y, w, h;	/* CGRect: an HFA of 4 doubles, returned in d0-d3 on arm64 (no _stret) */
};

static struct btc_ops bt;
static struct btc_trans rec[NREC];
static unsigned nrec;		/* written on the main thread (release), read by the scan thread (acquire) */
static unsigned rec_lost;	/* main thread only: callbacks after the table was full */
static id window_obj;		/* kept for the app's life */
static unsigned side_gaps_n;	/* app-side GAP lines (any thread), added to the RESULT gaps= (review T-2d63 LOW 6) */

static void
emit(const char *fmt, ...)
{
	char b[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (strncmp(b, "BT CLIENT GAP:", 14) == 0)
		__atomic_add_fetch(&side_gaps_n, 1, __ATOMIC_RELAXED);
	printf("%s\n", b);
	fflush(stdout);
}

static unsigned
a_side_gaps(void *ctx)
{
	(void)ctx;
	return __atomic_load_n(&side_gaps_n, __ATOMIC_RELAXED);
}

static unsigned
a_now_s(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (unsigned)ts.tv_sec;
}

static int
a_transition(void *ctx, unsigned i, struct btc_trans *t)
{
	(void)ctx;
	if (i >= __atomic_load_n(&nrec, __ATOMIC_ACQUIRE))
		return 0;
	*t = rec[i];
	return 1;
}

/* main thread (UIKit delegate callbacks) */
static void
note(int state, const char *event)
{
	unsigned d, u, n = __atomic_load_n(&nrec, __ATOMIC_RELAXED);

	btc_ios_counters(&d, &u);
	emit("BT APP: event=%s state=%s t=%u discovered=%u unique~%u", event, btc_app_label(state), a_now_s(0), d, u);
	if (n >= NREC) {
		if (rec_lost++ == 0)
			emit("BT CLIENT GAP: more than %u app-state callbacks (later ones not recorded)", NREC);
		return;
	}
	rec[n] = (struct btc_trans){ state, a_now_s(0), d, u };
	__atomic_store_n(&nrec, n + 1, __ATOMIC_RELEASE);
}

static int
a_connect(void *ctx, unsigned try_idx, unsigned timeout_s, struct btc_conn *r)
{
	(void)ctx;
	return btc_ios_connect(try_idx, timeout_s, r);
}

static int
a_pair(void *ctx, unsigned timeout_s, struct btc_pair *r)
{
	(void)ctx;
	return btc_ios_pair(timeout_s, r);
}

static void
mgr_out(const char *line)
{
	emit("%s", line);
}

/* sprint-bt: the BluetoothManager attach step (Settings' path) on a thread of its own: attach, power on, 40 s classic scan */
static void *
mgr_main(void *arg)
{
	(void)arg;
	(void)btm_ios_run(mgr_out, 60, 20, 40);
	return 0;
}

static void *
scan_main(void *arg)
{
	/* T-3615: connect step in the first fg stretch at t >= BTAPP_CONN_AT_S, up to BTAPP_CONN_TRIES attempts of BTAPP_CONN_TIMEOUT_S;
	 * sprint-bt: pair step (BluetoothManager) only when /usr/local/etc/btapp-peer names a peer */
	static struct btc_app_ops ao = { &bt, a_transition, a_now_s, a_side_gaps, a_connect, BTAPP_CONN_AT_S, BTAPP_CONN_TRIES,
	    BTAPP_CONN_TIMEOUT_S, 0, 0, 0 };
	int rc;

	(void)arg;
	if (btm_ios_peer_configured()) {
		ao.pair = a_pair;
		ao.pair_at_s = BTAPP_PAIR_AT_S;
		ao.pair_timeout_s = BTAPP_PAIR_TIMEOUT_S;
	}
	emit("BT APP: pair step %s", ao.pair ? "on (peer configured)" : "off (no peer configured)");
	rc = btc_app_run(&ao, 10, BTAPP_TOTAL_S, BTC_BUILD);
	emit("BT APP: scan thread done rc=%d", rc);
	return 0;
}

/* a full-screen window with a plain blue root view: the camera frame shows whether the app is in front */
static void
make_window(void)
{
	id cls_win = objc_getClass_("UIWindow"), cls_vc = objc_getClass_("UIViewController"), cls_color = objc_getClass_("UIColor");
	id vc, view, color;
	struct rect b;

	if (cls_win == 0 || cls_vc == 0 || cls_color == 0) {
		emit("BT CLIENT GAP: UIWindow, UIViewController or UIColor missing (no window; the scan still runs)");
		return;
	}
	window_obj = MSG0(MSG0(cls_win, "alloc"), "init");	/* -init: the main screen's bounds (checked below and logged) */
	vc = MSG0(MSG0(cls_vc, "alloc"), "init");
	if (window_obj == 0 || vc == 0) {
		emit("BT CLIENT GAP: UIWindow or UIViewController init returned nil (no window; the scan still runs)");
		return;
	}
	view = MSG0(vc, "view");
	color = MSG0(cls_color, "systemBlueColor");
	if (view && color)
		MSG1(view, "setBackgroundColor:", color);
	else
		emit("BT CLIENT GAP: root view or systemBlueColor nil (window stays default)");
	MSG1(window_obj, "setRootViewController:", vc);
	MSG0(window_obj, "makeKeyAndVisible");
	b = ((struct rect (*)(id, SEL))objc_msgSend_)(window_obj, sel_registerName_("bounds"));
	emit("BT APP: window %dx%d key", (int)b.w, (int)b.h);
	if (b.w < 1 || b.h < 1)
		emit("BT CLIENT GAP: window bounds %dx%d (UIWindow -init gave no frame; the scan still runs)", (int)b.w, (int)b.h);
}

static bool
m_finish(id self, SEL _cmd, id app, id opts)
{
	pthread_t th;
	int e;

	(void)self;
	(void)_cmd;
	(void)app;
	emit("BT APP: didFinishLaunching options=%s", opts ? "set" : "nil");
	make_window();
	e = pthread_create(&th, 0, scan_main, 0);
	if (e != 0)
		emit("BT CLIENT GAP: scan thread not started (pthread_create %d): no central, no counts", e);
	else
		pthread_detach(th);
	e = pthread_create(&th, 0, mgr_main, 0);
	if (e != 0)
		emit("BT CLIENT GAP: BluetoothManager thread not started (pthread_create %d)", e);
	else
		pthread_detach(th);
	return true;
}

static void
m_active(id self, SEL _cmd, id app)
{
	(void)self;
	(void)_cmd;
	(void)app;
	note(BTC_APP_ACTIVE, "didBecomeActive");
}

static void
m_resign(id self, SEL _cmd, id app)
{
	(void)self;
	(void)_cmd;
	(void)app;
	note(BTC_APP_INACTIVE, "willResignActive");
}

static void
m_background(id self, SEL _cmd, id app)
{
	(void)self;
	(void)_cmd;
	(void)app;
	note(BTC_APP_BACKGROUND, "didEnterBackground");
}

static void
m_foreground(id self, SEL _cmd, id app)
{
	(void)self;
	(void)_cmd;
	(void)app;
	note(BTC_APP_INACTIVE, "willEnterForeground");
}

static void
m_terminate(id self, SEL _cmd, id app)
{
	(void)self;
	(void)_cmd;
	(void)app;
	emit("BT APP: willTerminate t=%u", a_now_s(0));
}

int
main(int argc, char **argv)
{
	static const struct {
		const char *sel, *types;
		IMP imp;
	} m[] = {
		{ "application:didFinishLaunchingWithOptions:", "B@:@@", (IMP)m_finish },
		{ "applicationDidBecomeActive:", "v@:@", (IMP)m_active },
		{ "applicationWillResignActive:", "v@:@", (IMP)m_resign },
		{ "applicationDidEnterBackground:", "v@:@", (IMP)m_background },
		{ "applicationWillEnterForeground:", "v@:@", (IMP)m_foreground },
		{ "applicationWillTerminate:", "v@:@", (IMP)m_terminate },
	};
	void *rt;
	id cls, name;
	int fd = open("/dev/null", O_WRONLY);

	/* the log channel does not depend on where FrontBoard / RunningBoard pointed stdout */
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	setvbuf(stdout, 0, _IONBF, 0);
	emit("BT APP: main build=%s pid=%d stdout=%s", BTC_BUILD, (int)getpid(), fd >= 0 ? "devnull" : "inherited(open-failed)");
	if (fd < 0)
		emit("BT CLIENT GAP: open /dev/null failed: the lines may not reach the host log");
	if (btc_ios_setup(&bt) != 0) {
		emit("BT CLIENT GAP: libobjc symbols missing");
		emit("BT CLIENT: RESULT fail discovered=0 unique=0 fg_discovered=0 bg_discovered=0 windows=0 state=-1 gaps=1");
		return 2;
	}
	rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	sel_registerName_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	objc_msgSend_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	class_addMethod_ = dlsym(rt ? rt : RTLD_DEFAULT, "class_addMethod");
	objc_allocateClassPair_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_allocateClassPair");
	objc_registerClassPair_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_registerClassPair");
	objc_getClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	if (!sel_registerName_ || !objc_msgSend_ || !class_addMethod_ || !objc_allocateClassPair_ || !objc_registerClassPair_ || !objc_getClass_) {
		emit("BT CLIENT GAP: libobjc symbols missing (app side)");
		return 2;
	}
	cls = objc_allocateClassPair_(objc_getClass_("NSObject"), "BTAppDelegate", 0);
	if (cls == 0) {
		emit("BT CLIENT GAP: cannot create the app delegate class");
		return 2;
	}
	for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
		if (!class_addMethod_(cls, sel_registerName_(m[i].sel), m[i].imp, m[i].types)) {
			emit("BT CLIENT GAP: class_addMethod %s failed", m[i].sel);
			return 2;
		}
	objc_registerClassPair_(cls);
	name = ((id (*)(id, SEL, const char *))objc_msgSend_)(objc_getClass_("NSString"), sel_registerName_("stringWithUTF8String:"),
	    "BTAppDelegate");
	if (name == 0) {
		emit("BT CLIENT GAP: delegate class name string nil");
		return 2;
	}
	emit("BT APP: UIApplicationMain delegate=BTAppDelegate total=%us", (unsigned)BTAPP_TOTAL_S);
	return UIApplicationMain(argc, argv, 0, name);
}
