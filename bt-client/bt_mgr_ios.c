/* sprint-bt (2026-11-09): BluetoothManager glue of BTApp (iOS only; the logic is bt_mgr_core.c, host-tested).
 * Path = the one stock Settings uses (research T-a52f, private super repo docs/research/T-a52f-btmanager-pairing-path.md):
 *   +[BluetoothManager sharedInstance] on the MAIN thread (Q1.1.3d: another thread raises without +setSharedInstanceQueue:),
 *   wait for BluetoothAvailabilityChangedNotification object YES (Q1.1.7-9: calls before it are silent no-ops),
 *   -setPowered:YES (Q2.1 step 3), -setDeviceScanningEnabled:YES = classic inquiry (Q2.1 step 5, Q2.2.4), count
 *   BluetoothDeviceDiscoveredNotification (object = BluetoothDevice, Q2.2.1).
 * The daemon admits the session only with entitlement com.apple.bluetooth.system (Q1.2.5, btapp.entitlements carries it).
 * Pair step (btc_ios_pair): setDevicePairingEnabled:YES, scan, connectDevice: on the peer, acceptSSP:0 forDevice: on the
 * confirmation (Q2.3.1: 0 = accept), PINResult / PairedStatusChanged (Q2.3, Q2.4.3). The peer is named only in the gitignored
 * overlay file /usr/local/etc/btapp-peer (first line = its device name, written from .env by the run setup); the name is
 * compared in memory and never printed. Every notification arrives on the main queue (Q2.2.5, Q3.2); the state below is
 * touched on the main thread only, the worker threads read it through dispatch_sync_f. */
#include "bt_mgr.h"

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *id;
typedef void *SEL;
typedef void (*IMP)(void);

#define BTM_PEER_FILE "/usr/local/etc/btapp-peer"

static SEL (*sel_)(const char *);
static bool (*class_addMethod_)(id, SEL, IMP, const char *);
static id (*allocClass_)(id, const char *, size_t);
static void (*registerClass_)(id);
static id (*getClass_)(const char *);
static void *msg_;
#define M0(o, s) ((id (*)(id, SEL))msg_)((o), sel_(s))
#define MB(o, s) ((bool (*)(id, SEL))msg_)((o), sel_(s))
#define MI(o, s) ((int (*)(id, SEL))msg_)((o), sel_(s))
#define MUL(o, s) ((unsigned long (*)(id, SEL))msg_)((o), sel_(s))
#define MSETB(o, s, b) ((void (*)(id, SEL, bool))msg_)((o), sel_(s), (b))
#define M1(o, s, a) ((id (*)(id, SEL, id))msg_)((o), sel_(s), (a))

static void (*out_)(const char *);
static struct btm_attach st;		/* main thread */
static id mgr, observer;		/* main thread */
static id seen[64];			/* main thread: distinct BluetoothDevice objects (pointers, never printed) */
static unsigned nseen;
static char peer[128];			/* the peer's name (from BTM_PEER_FILE), empty = no pair step */
static int pair_on;			/* main thread: the pair step is listening */
static struct btm_pair_sm psm;		/* main thread */
static id peer_dev;			/* main thread */

static void
say(const char *fmt, ...)
{
	char b[300];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (strncmp(b, "BT MGR GAP:", 11) == 0)
		st.gaps++;
	out_(b);
}

static unsigned
ms_now(void)
{
	struct timespec ts;

	return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000) : 0;
}

static const char *
cstr(id s)
{
	return s ? ((const char *(*)(id, SEL))msg_)(s, sel_("UTF8String")) : 0;
}

static id
nsstr(const char *c)
{
	return ((id (*)(id, SEL, const char *))msg_)(getClass_("NSString"), sel_("stringWithUTF8String:"), c);
}

/* 1 = this BluetoothDevice is the configured peer (name compared, never printed) */
static int
is_peer(id dev)
{
	const char *n;

	if (!peer[0] || dev == 0)
		return 0;
	n = cstr(M0(dev, "name"));
	return n && strcmp(n, peer) == 0;
}

static id
device_of(id obj)
{
	/* the numeric-comparison notification carries {device, value}; the others the device itself (Q2.3) */
	if (obj && ((bool (*)(id, SEL, id))msg_)(obj, sel_("isKindOfClass:"), getClass_("NSDictionary")))
		return M1(obj, "objectForKey:", nsstr("device"));
	return obj;
}

static long
err_of(id note)
{
	id ui = M0(note, "userInfo"), e = ui ? M1(ui, "objectForKey:", nsstr("BluetoothErrorKey")) : 0;

	return e ? (long)MI(e, "intValue") : -1;
}

/* the observer's only method: every notification of the default center (name nil), filtered by name */
static void
m_note(id self, SEL _cmd, id note)
{
	const char *n = cstr(M0(note, "name"));
	id obj = M0(note, "object");
	unsigned t = ms_now();

	(void)self;
	(void)_cmd;
	if (n == 0 || strncmp(n, "Bluetooth", 9) != 0)
		return;
	if (strcmp(n, "BluetoothAvailabilityChangedNotification") == 0) {
		int yes = obj ? MB(obj, "boolValue") : 0;

		say("BT MGR: notification BluetoothAvailabilityChangedNotification object=%s", obj ? (yes ? "YES" : "NO") : "nil");
		if (yes && !st.available) {
			st.available = 1;
			st.avail_ms = t;
		}
	} else if (strcmp(n, "BluetoothPowerChangedNotification") == 0) {
		say("BT MGR: notification BluetoothPowerChangedNotification powered=%d", mgr ? (int)MB(mgr, "powered") : -1);
	} else if (strcmp(n, "BluetoothDeviceDiscoveredNotification") == 0) {
		unsigned i;

		st.discovered++;
		for (i = 0; i < nseen && seen[i] != obj; i++)
			;
		if (i == nseen && nseen < 64 && obj)
			seen[nseen++] = obj;
		st.unique = nseen;
		if (pair_on && !peer_dev && is_peer(obj)) {
			peer_dev = obj;
			btm_pair_feed(&psm, BTM_EV_FOUND, 0, t);
			((void (*)(id, SEL, id))msg_)(mgr, sel_("connectDevice:"), obj);
			say("BT MGR: pair connectDevice: sent");
			btm_pair_feed(&psm, BTM_EV_CONNECT_SENT, 0, t);
		}
	} else if (pair_on && (strcmp(n, "BluetoothPairingUserConfirmationNotification") == 0 || strcmp(n, "BluetoothPairingUserNumericComparisionNotification") == 0)) {
		id dev = device_of(obj);

		say("BT MGR: pair %s peer=%d", n + 16, dev == peer_dev);
		if (dev && dev == peer_dev) {
			btm_pair_feed(&psm, BTM_EV_CONFIRM, 0, t);
			((void (*)(id, SEL, long, id))msg_)(mgr, sel_("acceptSSP:forDevice:"), 0L, dev);
			say("BT MGR: pair acceptSSP:0 sent");
			btm_pair_feed(&psm, BTM_EV_ANSWERED, 0, t);
		}
	} else if (pair_on && strcmp(n, "BluetoothPairingPINRequestNotification") == 0) {
		say("BT MGR GAP: pair PIN request (legacy pairing) not answered: the peer is expected to use SSP");
	} else if (pair_on && strcmp(n, "BluetoothPairingPINResultSuccessNotification") == 0) {
		say("BT MGR: pair PINResultSuccess peer=%d", obj == peer_dev);
		if (obj == peer_dev)
			btm_pair_feed(&psm, BTM_EV_PIN_OK, 0, t);
	} else if (pair_on && strcmp(n, "BluetoothPairingPINResultFailedNotification") == 0) {
		say("BT MGR: pair PINResultFailed peer=%d err=%ld", obj == peer_dev, err_of(note));
		if (obj == peer_dev || obj == 0)
			btm_pair_feed(&psm, BTM_EV_PIN_FAIL, err_of(note), t);
	} else if (pair_on && strcmp(n, "BluetoothDeviceConnectSuccessNotification") == 0) {
		if (obj == peer_dev)
			btm_pair_feed(&psm, BTM_EV_CONNECTED, 0, t);
	} else if (pair_on && strcmp(n, "BluetoothDeviceConnectFailedNotification") == 0) {
		say("BT MGR: pair ConnectFailed peer=%d err=%ld", obj == peer_dev, err_of(note));
		if (obj == peer_dev)
			btm_pair_feed(&psm, BTM_EV_CONNECT_FAIL, err_of(note), t);
	} else if (pair_on && strcmp(n, "BluetoothPairedStatusChangedNotification") == 0) {
		int p = peer_dev ? (int)MB(peer_dev, "paired") : -1;

		say("BT MGR: pair PairedStatusChanged peer_paired=%d", p);
		if (p == 1)
			btm_pair_feed(&psm, BTM_EV_PAIRED, 0, t);
	}
}

/* ---- main-thread steps (dispatch_sync_f from the worker) ---- */
static void
main_attach(void *ctx)
{
	void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	bool (*mgb)(id) = mg ? (bool (*)(id))dlsym(mg, "MGGetBoolAnswer") : 0;
	void *bm = dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_NOW);
	id cls, center;

	(void)ctx;
	st.mg_bluetooth = mgb ? (int)mgb(nsstr("bluetooth")) : -1;
	say("BT MGR: MGGetBoolAnswer(bluetooth)=%d main_thread=%d", st.mg_bluetooth, (int)MB(getClass_("NSThread"), "isMainThread"));
	if (st.mg_bluetooth != 1)
		say("BT MGR GAP: MGGetBoolAnswer(bluetooth) is %d: BluetoothManager attaches only when it is 1 (research Q1.1.3b)", st.mg_bluetooth);
	if (!bm || !(cls = getClass_("BluetoothManager"))) {
		say("BT MGR GAP: BluetoothManager.framework %s", bm ? "loaded but no class BluetoothManager" : "not loaded");
		return;
	}
	observer = M0(M0(getClass_("BTMgrObserver"), "alloc"), "init");
	center = M0(getClass_("NSNotificationCenter"), "defaultCenter");
	if (!observer || !center) {
		say("BT MGR GAP: observer or NSNotificationCenter nil: no notification is counted");
	} else {
		((void (*)(id, SEL, id, SEL, id, id))msg_)(center, sel_("addObserver:selector:name:object:"), observer, sel_("n:"), 0, 0);
	}
	st.avail_ms = ms_now();	/* start of the wait; replaced by the delta below */
	mgr = M0(cls, "sharedInstance");
	st.shared = mgr != 0;
	if (!mgr) {
		st.init_err = (long)MI(cls, "lastInitError");
		say("BT MGR GAP: +sharedInstance returned nil, +lastInitError=%ld", st.init_err);
		return;
	}
	say("BT MGR: sharedInstance non-nil available=%d", (int)MB(mgr, "available"));
}

static void
main_poll(void *ctx)
{
	struct btm_attach *c = ctx;

	if (mgr && !st.available && MB(mgr, "available")) {
		say("BT MGR: -available YES (polled)");
		st.available = 1;
		st.avail_ms = ms_now();
	}
	if (mgr && st.power_req && !st.powered && MB(mgr, "powered")) {
		st.powered = 1;
		st.power_ms = ms_now();
	}
	*c = st;
}

static void
main_power(void *ctx)
{
	(void)ctx;
	say("BT MGR: setPowered:YES (powered before=%d enabled=%d)", (int)MB(mgr, "powered"), (int)MB(mgr, "enabled"));
	MSETB(mgr, "setPowered:", true);
	st.power_req = 1;
	st.power_ms = ms_now();
}

static void
main_scan(void *ctx)
{
	bool on = ctx != 0;

	MSETB(mgr, "setDeviceScanningEnabled:", on);
	if (on)
		st.scan_req = 1;
	say("BT MGR: setDeviceScanningEnabled:%s (deviceScanningEnabled=%d)", on ? "YES" : "NO", (int)MB(mgr, "deviceScanningEnabled"));
}

static int
setup_runtime(void)
{
	void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	id c;

	sel_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	msg_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	class_addMethod_ = dlsym(rt ? rt : RTLD_DEFAULT, "class_addMethod");
	allocClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_allocateClassPair");
	registerClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_registerClassPair");
	getClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	if (!sel_ || !msg_ || !class_addMethod_ || !allocClass_ || !registerClass_ || !getClass_)
		return -1;
	if (getClass_("BTMgrObserver"))
		return 0;
	c = allocClass_(getClass_("NSObject"), "BTMgrObserver", 0);
	if (!c || !class_addMethod_(c, sel_("n:"), (IMP)m_note, "v@:@"))
		return -1;
	registerClass_(c);
	return 0;
}

static void
read_peer(void)
{
	FILE *f = fopen(BTM_PEER_FILE, "r");
	size_t l;

	peer[0] = 0;
	if (!f)
		return;
	if (fgets(peer, sizeof peer, f) == 0)
		peer[0] = 0;
	fclose(f);
	l = strlen(peer);
	while (l > 0 && (peer[l - 1] == '\n' || peer[l - 1] == '\r'))
		peer[--l] = 0;
}

/* 1 = a peer is configured (the app then turns the pair step on) */
int
btm_ios_peer_configured(void)
{
	read_peer();
	return peer[0] != 0;
}

/* the attach step, on a worker thread: blocking, about avail_s + power_s + scan_s at most; prints the RESULT line */
int
btm_ios_run(void (*out)(const char *), unsigned avail_s, unsigned power_s, unsigned scan_s)
{
	struct btm_attach c;
	unsigned t0, i;
	char line[400];

	out_ = out;
	memset(&st, 0, sizeof st);
	if (setup_runtime() != 0) {
		out("BT MGR GAP: libobjc symbols missing or the observer class could not be built");
		out("BT MGR: RESULT: FAIL bt-mgr shared=0 gaps=1");
		return -1;
	}
	t0 = ms_now();
	say("BT MGR: start avail_wait=%us power_wait=%us scan=%us peer=%s", avail_s, power_s, scan_s, btm_ios_peer_configured() ? "configured" : "none");
	dispatch_sync_f(dispatch_get_main_queue(), 0, main_attach);
	for (i = 0; i < avail_s * 10; i++) {
		dispatch_sync_f(dispatch_get_main_queue(), &c, main_poll);
		if (!c.shared || c.available)
			break;
		usleep(100000);
	}
	if (c.shared && !c.available)
		say("BT MGR GAP: no BluetoothAvailabilityChangedNotification YES within %us (bluetoothd log: look for \"insufficient entitlements\")", avail_s);
	if (c.available) {
		say("BT MGR: available after %u ms", c.avail_ms - t0);
		dispatch_sync_f(dispatch_get_main_queue(), 0, main_power);
		for (i = 0; i < power_s * 10; i++) {
			dispatch_sync_f(dispatch_get_main_queue(), &c, main_poll);
			if (c.powered)
				break;
			usleep(100000);
		}
		if (!c.powered)
			say("BT MGR GAP: -powered still NO %us after setPowered:YES", power_s);
		else
			say("BT MGR: powered after %u ms", c.power_ms - c.avail_ms);
		dispatch_sync_f(dispatch_get_main_queue(), (void *)1, main_scan);
		sleep(scan_s);
		dispatch_sync_f(dispatch_get_main_queue(), 0, main_scan);
	}
	dispatch_sync_f(dispatch_get_main_queue(), &c, main_poll);
	c.avail_ms = c.available ? c.avail_ms - t0 : 0;
	c.power_ms = c.powered ? c.power_ms - (c.avail_ms + t0) : 0;
	btm_attach_result(&c, line, sizeof line);
	out(line);
	return btm_attach_pass(&c) ? 0 : 1;
}

/* ---- pair step (called by bt_app_core.c on its run thread) ---- */
static void
main_pair_start(void *ctx)
{
	(void)ctx;
	btm_pair_init(&psm, ms_now());
	peer_dev = 0;
	pair_on = 1;
	{
		/* a paired device sends no BluetoothDeviceDiscoveredNotification, so a surviving bond is looked up in pairedDevices
		 * (-[BluetoothManager pairedDevices] 0x1d4d13888, research T-a52f-btmanager-pairing-path.md Q4 / pairing path 3) */
		id list = M0(mgr, "pairedDevices");
		unsigned long i, n = list ? MUL(list, "count") : 0;

		btm_pair_feed(&psm, BTM_EV_START, (long)n, ms_now());
		for (i = 0; i < n && !peer_dev; i++) {
			id d = ((id (*)(id, SEL, unsigned long))msg_)(list, sel_("objectAtIndex:"), i);

			if (is_peer(d))
				peer_dev = d;
		}
	}
	MSETB(mgr, "setDevicePairingEnabled:", true);
	if (peer_dev) {
		btm_pair_feed(&psm, BTM_EV_PEER_BONDED, 0, ms_now());
		say("BT MGR: pair start bonded=%d peer_bonded=1 pairing_agent=on scanning=off", psm.r.bonded_start);
		((void (*)(id, SEL, id))msg_)(mgr, sel_("connectDevice:"), peer_dev);
		say("BT MGR: pair connectDevice: sent (bonded peer)");
		btm_pair_feed(&psm, BTM_EV_CONNECT_SENT, 0, ms_now());
		return;
	}
	MSETB(mgr, "setDeviceScanningEnabled:", true);
	say("BT MGR: pair start bonded=%d peer_bonded=0 pairing_agent=on scanning=%d", psm.r.bonded_start, (int)MB(mgr, "deviceScanningEnabled"));
}

static void
main_pair_poll(void *ctx)
{
	*(int *)ctx = psm.done;
}

static void
main_pair_end(void *ctx)
{
	struct btc_pair *r = ctx;

	btm_pair_feed(&psm, BTM_EV_END, (long)MUL(M0(mgr, "pairedDevices"), "count"), ms_now());
	pair_on = 0;
	MSETB(mgr, "setDeviceScanningEnabled:", false);
	*r = psm.r;
}

static void
main_has_mgr(void *ctx)
{
	*(int *)ctx = mgr != 0 && st.available && st.powered;
}

int
btc_ios_pair(unsigned timeout_s, struct btc_pair *r)
{
	int ok = 0, done = 0;
	unsigned i;

	memset(r, 0, sizeof *r);
	r->outcome = BTC_PAIR_NOMGR;
	r->bonded_start = r->bonded_end = -1;
	if (!msg_ || !peer[0]) {
		say("BT MGR GAP: pair step without %s", !msg_ ? "the BluetoothManager runtime" : "a configured peer");
		return -1;
	}
	dispatch_sync_f(dispatch_get_main_queue(), &ok, main_has_mgr);
	if (!ok) {
		say("BT MGR GAP: pair step: BluetoothManager not attached/available/powered");
		return -1;
	}
	dispatch_sync_f(dispatch_get_main_queue(), 0, main_pair_start);
	for (i = 0; i < timeout_s * 10 && !done; i++) {
		usleep(100000);
		dispatch_sync_f(dispatch_get_main_queue(), &done, main_pair_poll);
	}
	sleep(2);	/* the bond count settles after the PIN result (keychain write) [guess] */
	dispatch_sync_f(dispatch_get_main_queue(), r, main_pair_end);
	return 0;
}
