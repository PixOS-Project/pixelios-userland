/* bt-client iOS backend (T-86aa): starts one CBCentralManager and reports what its scan hears, through the ObjC
 * runtime and CoreBluetooth symbols found with dlsym, so a missing piece becomes a "BT CLIENT GAP:" line instead of a
 * launch failure. Protocol methods are installed with class_addMethod, so the file needs no Objective-C compiler.
 * stdout goes to the launchd job's /dev/null, which xnu patch 0048 mirrors into the host log for the process names in
 * boot-arg sweetnull=...,bt-probe. Counts only: identifiers are reduced to one Bloom-filter bit (never kept or printed). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define __STDC_WANT_LIB_EXT1__ 1	/* memset_s: the iPhoneOS16.5 SDK has no explicit_bzero (checked in string.h) */
#include "bt_client.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef BTC_BUILD
#define BTC_BUILD "dev"
#endif

typedef void *id;
typedef void *SEL;
typedef void (*IMP)(void);

static SEL (*sel_registerName_)(const char *);
static bool (*class_addMethod_)(id, SEL, IMP, const char *);	/* objc/runtime.h: BOOL class_addMethod(Class, SEL, IMP, const char *); BOOL = bool on arm64 */
static id (*objc_allocateClassPair_)(id, const char *, size_t);
static void (*objc_registerClassPair_)(id);
static id (*objc_getClass_)(const char *);
/* objc_msgSend is called through exact, NON-variadic prototypes: on Apple arm64 a variadic call puts the arguments on
 * the stack, the callee reads x2.. (bt-up16 boot 2: initWithDelegate:queue:options: got x2 = 0x103, SIGBUS). */
static void *objc_msgSend_;
/* The delegate queue: a nil queue = the main queue, which never drains while main() sleeps between samples (T-86aa
 * review finding 2). A serial queue of our own runs the callbacks; samples read the counters on that queue. */
static id (*dispatch_queue_create_)(const char *, void *);
static void (*dispatch_sync_f_)(id, void *, void (*)(void *));
#define MSG0(o, s) ((id (*)(id, SEL))objc_msgSend_)((o), sel_registerName_(s))
#define MSGL(o, s) ((long (*)(id, SEL))objc_msgSend_)((o), sel_registerName_(s))
#define MSGB(o, s) ((bool (*)(id, SEL))objc_msgSend_)((o), sel_registerName_(s))
#define MSGP(o, s, p) ((void (*)(id, SEL, void *))objc_msgSend_)((o), sel_registerName_(s), (p))
#define MSG2(o, s, a, b) ((void (*)(id, SEL, id, id))objc_msgSend_)((o), sel_registerName_(s), (a), (b))
#define MSG3(o, s, a, b, c) ((id (*)(id, SEL, id, id, id))objc_msgSend_)((o), sel_registerName_(s), (a), (b), (c))
static uint64_t bloom[1024];
static unsigned unique_bits;		/* queue-only */
static unsigned discovered_n;		/* queue-only: didDiscoverPeripheral calls */
static unsigned gap_n;			/* any thread: every gap() line (rule 8: reported in out->gaps) */
static int central_created, scans_sent;	/* scans_sent: queue-only, one per transition into poweredOn (T-0936) */
static int central_options_n = -1;	/* T-18e5: 1 = CBCentralManagerOptionShowPowerAlertKey was in the init options dictionary */
static id scan_opts;	/* T-2ae7: 0 in arm A (services:nil, options:nil); the arm-B dictionary after o_arm_b. Delegate queue only. */
static id scan_services;	/* T-27d4: 0 (services:nil) except in arm C: @[CBUUID FD50]. Delegate queue only. */
static char central_options_desc[64];	/* T-2ae7: "<key string>=<NSNumber value>" of that dictionary, for the start row */
static long central_state;		/* queue-only */
static id central_obj, delegate_q;
static void *cb_handle;	/* the loaded CoreBluetooth framework (the init option key is its exported constant) */
static const struct btc_ops *g_ops;

static void
emit(const char *fmt, ...)
{
	char b[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	g_ops->out((void *)0, b);
}

static void
gap(const char *fmt, ...)
{
	char b[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	__atomic_fetch_add(&gap_n, 1, __ATOMIC_RELAXED);
	emit("BT CLIENT GAP: %s", b);
}

/* T-3615: the connect step. The scan keeps the best connectable advertisers (peripheral objects retained, RSSI, never printed);
 * btc_ios_connect connects to the strongest unused one. CBCentralManager hands back the same CBPeripheral object for a device it
 * already knows, so the pointer is the dedup key [guess: documented only as "identifier is stable"; a duplicate costs one slot]. */
#define CAND_N 8
#define CP_IDLE 0
#define CP_PENDING 1
#define CP_CONNECTED 2
#define CP_SERVICES 3
#define CP_FAILED 4
static struct {
	id p;
	int rssi, conn;		/* conn: 1 = kCBAdvDataIsConnectable true, -1 = the key was absent from the advertisement data */
	int used;
} cand[CAND_N];			/* delegate queue only */
static id *adv_conn_key_p;	/* &CBAdvertisementDataIsConnectable (an exported NSString constant) */
static id delegate_obj, conn_cur;	/* conn_cur: delegate queue (written) / compared in callbacks */
static int conn_cb_ok, conn_unknown_gap;
static int conn_phase;		/* CP_*, atomic: written on the delegate queue, polled by the run thread */
static int conn_disc, conn_services = -1, conn_has_err;
static long conn_err;
static unsigned conn_ms, conn_t0;

static unsigned
now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void
cand_note(id peripheral, id data, id rssi)
{
	int conn = -1, r = -127, i, weakest = -1;

	if (adv_conn_key_p && *adv_conn_key_p && data) {
		id v = ((id (*)(id, SEL, id))objc_msgSend_)(data, sel_registerName_("objectForKey:"), *adv_conn_key_p);

		if (v)
			conn = MSGB(v, "boolValue") ? 1 : 0;
	}
	if (rssi) {
		r = ((int (*)(id, SEL))objc_msgSend_)(rssi, sel_registerName_("intValue"));
		if (r == 127)
			r = -127;	/* 127 = "not available" */
	}
	if (conn == 0)
		return;		/* a non-connectable advertiser is not a candidate */
	for (i = 0; i < CAND_N; i++)
		if (cand[i].p == peripheral) {
			cand[i].rssi = r;
			cand[i].conn = conn;
			return;
		}
	for (i = 0; i < CAND_N; i++)
		if (!cand[i].p) {
			cand[i].p = MSG0(peripheral, "retain");
			cand[i].rssi = r;
			cand[i].conn = conn;
			cand[i].used = 0;
			return;
		}
	for (i = 0; i < CAND_N; i++)	/* full: replace the weakest unused entry if this one is stronger */
		if (!cand[i].used && cand[i].p != conn_cur && (weakest < 0 || cand[i].rssi < cand[weakest].rssi))
			weakest = i;
	if (weakest >= 0 && cand[weakest].rssi < r) {
		MSG0(cand[weakest].p, "release");
		cand[weakest].p = MSG0(peripheral, "retain");
		cand[weakest].rssi = r;
		cand[weakest].conn = conn;
		cand[weakest].used = 0;
	}
}

static void
m_did_discover(id self, SEL _cmd, id central, id peripheral, id data, id rssi)
{
	id ident;
	unsigned char uu[16];
	uint64_t h, bit;
	(void)self;
	(void)_cmd;
	(void)central;
	discovered_n++;
	if (peripheral)
		cand_note(peripheral, data, rssi);
	if (peripheral == 0 || (ident = MSG0(peripheral, "identifier")) == 0) {
		gap("didDiscover without a peripheral identifier (counted in discovered, not in unique)");
		return;
	}
	MSGP(ident, "getUUIDBytes:", uu);
	h = 0xcbf29ce484222325ull;
	for (int i = 0; i < 16; i++)
		h = (h ^ uu[i]) * 0x100000001b3ull;
	bit = (h ^ (h >> 33)) & 0xffff;
	if (!(bloom[bit >> 6] & (1ull << (bit & 63)))) {
		bloom[bit >> 6] |= 1ull << (bit & 63);
		unique_bits++;
	}
#ifdef __APPLE__
	memset_s(uu, sizeof uu, 0, sizeof uu);	/* C11 Annex K: the compiler may not drop it */
#else
	explicit_bzero(uu, sizeof uu);		/* host syntax check */
#endif
}

static void
m_did_update_state(id self, SEL _cmd, id central)
{
	(void)self;
	(void)_cmd;
	long prev = central_state;

	central_state = MSGL(central, "state");
	emit("BT CLIENT: state event=%ld", central_state);
	/* use the callback's own `central`: this can run before initWithDelegate:queue:options: returns in o_start;
	 * T-0936: scan again after every poweredOff/resetting -> poweredOn (a bluetoothd restart drops the client's scan) */
	if (btc_scan_due(prev, central_state) && central) {
		MSG2(central, "scanForPeripheralsWithServices:options:", scan_services, scan_opts);
		scans_sent++;
		emit("BT CLIENT: scan started isScanning=%d n=%d", (int)MSGB(central, "isScanning"), scans_sent);
	}
}

static void
m_did_connect(id self, SEL _cmd, id central, id peripheral)
{
	(void)self;
	(void)_cmd;
	(void)central;
	if (peripheral == 0 || peripheral != conn_cur)
		return;
	conn_ms = now_ms() - conn_t0;
	__atomic_store_n(&conn_phase, CP_CONNECTED, __ATOMIC_SEQ_CST);
	emit("BT CLIENT: connect callback=didConnectPeripheral ms=%u", conn_ms);
	MSGP(peripheral, "discoverServices:", (void *)0);
}

static void
m_did_fail(id self, SEL _cmd, id central, id peripheral, id err)
{
	(void)self;
	(void)_cmd;
	(void)central;
	if (peripheral == 0 || peripheral != conn_cur)
		return;
	conn_ms = now_ms() - conn_t0;
	conn_has_err = err != 0;
	conn_err = err ? MSGL(err, "code") : 0;
	__atomic_store_n(&conn_phase, CP_FAILED, __ATOMIC_SEQ_CST);
	emit("BT CLIENT: connect callback=didFailToConnectPeripheral ms=%u err=%ld", conn_ms, conn_err);
}

static void
m_did_disconnect(id self, SEL _cmd, id central, id peripheral, id err)
{
	(void)self;
	(void)_cmd;
	(void)central;
	if (peripheral == 0 || peripheral != conn_cur)
		return;
	__atomic_store_n(&conn_disc, 1, __ATOMIC_SEQ_CST);
	emit("BT CLIENT: connect callback=didDisconnectPeripheral err=%ld", err ? MSGL(err, "code") : 0L);
}

static void
m_did_services(id self, SEL _cmd, id peripheral, id err)
{
	id sv;

	(void)self;
	(void)_cmd;
	if (peripheral == 0 || peripheral != conn_cur)
		return;
	sv = MSG0(peripheral, "services");
	conn_services = sv ? (int)MSGL(sv, "count") : 0;
	emit("BT CLIENT: connect callback=didDiscoverServices count=%d err=%ld", conn_services, err ? MSGL(err, "code") : 0L);
	__atomic_store_n(&conn_phase, CP_SERVICES, __ATOMIC_SEQ_CST);
}

/* T-18e5: {CBCentralManagerOptionShowPowerAlertKey: @0} as an NSDictionary, or 0 when CoreBluetooth or a Foundation piece is
 * missing (the caller prints one gap). The key is the CoreBluetooth.tbd data symbol _CBCentralManagerOptionShowPowerAlertKey
 * (CBCentralManagerConstants.h: CB_EXTERN NSString * const), not its name: its string value is read from the loaded framework
 * (an audit finding: "CBCentralManagerOptionShowPowerAlertKey" literal + dlsym of the symbol -> the value, never the name).
 * The value is @0 (NSNumber numberWithInt:0) because the header says "An NSNumber (Boolean)"; whether bluetoothd parses
 * init options the way it parses the scan keys 0x10021da90 is not decoded [guess]. */
/* T-2ae7: the start row names the option by the constant's own string value (NSString UTF8String) and the value read back
 * from the NSNumber passed (longValue), not by words typed here. Only [A-Za-z0-9_] are copied (a public constant name). */
static void
describe_option(id key, id val)
{
	const char *k = ((const char *(*)(id, SEL))objc_msgSend_)(key, sel_registerName_("UTF8String"));
	long v = ((long (*)(id, SEL))objc_msgSend_)(val, sel_registerName_("longValue"));
	size_t n = 0;

	if (k == 0) {
		gap("CBCentralManagerOptionShowPowerAlertKey: UTF8String returned NULL (the key is still passed)");
		k = "unreadable-key";
	}
	for (; *k && n < 40; k++)
		if ((*k >= 'A' && *k <= 'Z') || (*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_' || *k == '-')
			central_options_desc[n++] = *k;
	snprintf(central_options_desc + n, sizeof central_options_desc - n, "=%ld", v);
}

static id
central_options_dict(void *corebluetooth)
{
	id nsdict, nsnum, key, val, dict;
	id *const_key = dlsym(corebluetooth ? corebluetooth : RTLD_DEFAULT, "CBCentralManagerOptionShowPowerAlertKey");

	if (objc_msgSend_ == 0 || sel_registerName_ == 0 || objc_getClass_ == 0 || const_key == 0 || *const_key == 0)
		return 0;
	nsdict = objc_getClass_("NSDictionary");
	nsnum = objc_getClass_("NSNumber");
	if (nsdict == 0 || nsnum == 0)
		return 0;
	val = ((id (*)(id, SEL, int))objc_msgSend_)(nsnum, sel_registerName_("numberWithInt:"), 0);
	if (val == 0)
		return 0;
	key = *const_key;
	dict = ((id (*)(id, SEL, const id *, const id *, unsigned long))objc_msgSend_)(nsdict,
	    sel_registerName_("dictionaryWithObjects:forKeys:count:"), &val, &key, 1);
	if (dict != 0)
		describe_option(key, val);
	return dict;
}

static int
o_start(void *ctx)
{
	id cls, delegate_cls, delegate, opts;
	(void)ctx;
	cls = objc_getClass_("CBCentralManager");
	if (cls == 0) {
		gap("CBCentralManager class not found");
		return -1;
	}
	delegate_cls = objc_allocateClassPair_(objc_getClass_("NSObject"), "BTProbeDelegate", 0);
	if (delegate_cls == 0) {
		gap("cannot create the delegate class");
		return -1;
	}
	/* T-2ae7 review finding 2: a delegate without these methods gets no state event, so no scan, and no line said so */
	if (!class_addMethod_(delegate_cls, sel_registerName_("centralManager:didDiscoverPeripheral:advertisementData:RSSI:"),
	    (IMP)m_did_discover, "v@:@@@@")) {
		gap("class_addMethod centralManager:didDiscoverPeripheral:advertisementData:RSSI: failed");
		return -1;
	}
	if (!class_addMethod_(delegate_cls, sel_registerName_("centralManagerDidUpdateState:"), (IMP)m_did_update_state, "v@:@")) {
		gap("class_addMethod centralManagerDidUpdateState: failed");
		return -1;
	}
	/* T-3615: the connect callbacks; a failed registration disables the connect step only (the scan does not need them) */
	conn_cb_ok = class_addMethod_(delegate_cls, sel_registerName_("centralManager:didConnectPeripheral:"), (IMP)m_did_connect, "v@:@@") &&
	    class_addMethod_(delegate_cls, sel_registerName_("centralManager:didFailToConnectPeripheral:error:"), (IMP)m_did_fail, "v@:@@@") &&
	    class_addMethod_(delegate_cls, sel_registerName_("centralManager:didDisconnectPeripheral:error:"), (IMP)m_did_disconnect, "v@:@@@") &&
	    class_addMethod_(delegate_cls, sel_registerName_("peripheral:didDiscoverServices:"), (IMP)m_did_services, "v@:@@");
	if (!conn_cb_ok)
		gap("class_addMethod for the connect callbacks failed: the connect step is disabled");
	adv_conn_key_p = dlsym(cb_handle ? cb_handle : RTLD_DEFAULT, "CBAdvertisementDataIsConnectable");
	if (adv_conn_key_p == 0 || *adv_conn_key_p == 0)
		gap("CBAdvertisementDataIsConnectable constant missing: every advertiser counts as an unknown-connectable candidate");
	objc_registerClassPair_(delegate_cls);
	delegate = MSG0(delegate_cls, "new");
	delegate_obj = delegate;
	if (delegate == 0) {
		gap("BTProbeDelegate new returned nil");
		return -1;
	}
	/* T-18e5: ONE documented CBCentralManager init option and nothing else. The dictionary and its NSNumber are built
	 * explicitly and the key comes from CoreBluetooth's own constant, so a missing piece is a BT CLIENT GAP line and the
	 * manager still starts with nil options. */
	opts = central_options_dict(cb_handle);
	central_options_n = opts != 0;
	if (opts == 0)
		gap("CBCentralManagerOptionShowPowerAlertKey dictionary not built (CoreBluetooth constant or a Foundation class missing); starting with nil options");
	central_obj = MSG0(cls, "alloc");
	if (dispatch_sync_f_ == 0 || dispatch_queue_create_ == 0) {
		gap("libdispatch symbols missing (dispatch_queue_create/dispatch_sync_f)");
		return -1;
	}
	delegate_q = dispatch_queue_create_("com.pixos.btprobe", (void *)0);
	if (delegate_q == 0) {
		gap("dispatch_queue_create failed");
		return -1;
	}
	central_obj = MSG3(central_obj, "initWithDelegate:queue:options:", delegate, delegate_q, opts);
	if (central_obj == 0 && opts != 0) {
		gap("initWithDelegate:queue:options: with CBCentralManagerOptionShowPowerAlertKey returned nil; retrying with nil options");
		central_options_n = 0;
		central_obj = MSG3(MSG0(cls, "alloc"), "initWithDelegate:queue:options:", delegate, delegate_q, (id)0);
	}
	if (central_obj == 0) {
		gap("cannot instantiate CBCentralManager");
		return -1;
	}
	central_created = 1;
	emit("BT CLIENT: central created state=%ld", MSGL(central_obj, "state"));
	return 0;
}

/* runs on the delegate queue: the counters are written there, so reading them there needs no lock */
static void
copy_counters(void *p)
{
	struct btc_sample *out = p;

	out->discovered = discovered_n;
	out->unique = unique_bits;
}

static void
o_sample(void *ctx, struct btc_sample *out)
{
	(void)ctx;
	out->state = central_created ? MSGL(central_obj, "state") : -1;
	out->scanning = central_created ? (int)MSGB(central_obj, "isScanning") : -1;
	if (delegate_q) {	/* o_start refuses to leave delegate_q set without dispatch_sync_f_ */
		dispatch_sync_f_(delegate_q, out, copy_counters);
	} else {		/* start failed before the queue existed: nothing ran a callback; o_start counted its gap */
		out->discovered = 0;
		out->unique = 0;
	}
	out->gaps = __atomic_load_n(&gap_n, __ATOMIC_RELAXED);
}

static const char *
o_central_options(void *ctx)
{
	(void)ctx;
	return central_options_n == 1 ? central_options_desc : 0;
}

/* T-2ae7: runs on the delegate queue (scans_sent is queue-only; m_did_update_state may have just started a scan) */
static void
rescan_on_queue(void *p)
{
	int *res = p;

	if (MSGL(central_obj, "state") != BTC_STATE_POWERED_ON || MSGB(central_obj, "isScanning")) {
		*res = 0;
		return;
	}
	MSG2(central_obj, "scanForPeripheralsWithServices:options:", scan_services, scan_opts);
	*res = ++scans_sent;
}

/* T-2ae7 arm B. bluetoothd's scan handler 0x100470ef0 reads the client's kCBMsgArgOptions dictionary by key string; for the
 * key "kCBMsgArgIsPrivilegedDaemon" (cstring 0x100733502) an XPC int64 (parser 0x10021dd44) != 0 sets request byte +1
 * (0x100471400-0x100471420), with no entitlement test in between; the report gate 0x10058ed28 passes a report when the scan
 * agent's state +0x51 == 0x20 and that byte != 0 (0x10058ed38-0x10058ed6c) [bin]. CoreBluetooth passes the caller's options
 * through unfiltered: -[CBCentralManager scanForPeripheralsWithServices:options:] 0x1a5e30ec8 copies them and adds
 * kCBMsgArgHasCompletionBlock = kCFBooleanFalse, _scanForPeripheralsWithServices:options: 0x1a5e48040 sends
 * {kCBMsgArgUUIDs, kCBMsgArgOptions} with sendMsg:args: [bin, dyld cache]. The key is a private string, not an exported
 * constant, so the literal IS the value bluetoothd looks up. The value is kCFBooleanTrue, the same CFBoolean type the framework
 * sends for its own key through the same int64 parser [bin + inferred: bt-up18 scanned, so that conversion is accepted]. */
static const char ARMB_KEY[] = "kCBMsgArgIsPrivilegedDaemon";

struct armb_req {
	id opts;
	int res;
};

static void
armb_on_queue(void *p)
{
	struct armb_req *r = p;

	scan_opts = r->opts;	/* every later scan (state event, resume) carries it */
	scan_services = 0;	/* T-27d4: arm B is services:nil, so arm C's filter ends here */
	if (MSGL(central_obj, "state") != BTC_STATE_POWERED_ON) {
		r->res = 0;
		return;
	}
	if (MSGB(central_obj, "isScanning"))
		MSG0(central_obj, "stopScan");
	MSG2(central_obj, "scanForPeripheralsWithServices:options:", scan_services, scan_opts);
	r->res = ++scans_sent;
}

static int
o_arm_b(void *ctx, char *desc, unsigned long desc_len)
{
	id nsdict, nsstr, key, val, dict, *cftrue = dlsym(RTLD_DEFAULT, "kCFBooleanTrue");
	struct armb_req r = { 0, -1 };
	const char *k;

	(void)ctx;
	if (!central_created || delegate_q == 0) {
		gap("arm B without a central");
		return -1;
	}
	nsdict = objc_getClass_("NSDictionary");
	nsstr = objc_getClass_("NSString");
	if (nsdict == 0 || nsstr == 0 || cftrue == 0 || *cftrue == 0) {
		gap("arm B: NSDictionary, NSString or kCFBooleanTrue missing");
		return -1;
	}
	val = *cftrue;
	key = ((id (*)(id, SEL, const char *))objc_msgSend_)(nsstr, sel_registerName_("stringWithUTF8String:"), ARMB_KEY);
	if (key == 0) {
		gap("arm B: stringWithUTF8String returned nil");
		return -1;
	}
	dict = ((id (*)(id, SEL, const id *, const id *, unsigned long))objc_msgSend_)(nsdict,
	    sel_registerName_("dictionaryWithObjects:forKeys:count:"), &val, &key, 1);
	if (dict == 0) {
		gap("arm B: dictionaryWithObjects:forKeys:count: returned nil");
		return -1;
	}
	r.opts = MSG0(dict, "retain");	/* kept for the rest of the run: the delegate queue's autorelease pool must not free it */
	/* the row names what was passed, read back from the objects: key UTF8String, value longValue */
	k = ((const char *(*)(id, SEL))objc_msgSend_)(key, sel_registerName_("UTF8String"));
	snprintf(desc, desc_len, "%s=%ld", k ? k : "unreadable-key", ((long (*)(id, SEL))objc_msgSend_)(val, sel_registerName_("longValue")));
	dispatch_sync_f_(delegate_q, &r, armb_on_queue);
	return r.res;
}

/* T-27d4 arm C: the documented service-UUID scan. bluetoothd's scan handler parses kCBMsgArgUUIDs (cstring ref 0x10045ffcc,
 * parser 0x10021e04c) and copies the list into the new scan request at +0x8 (0x100460048 alloc, 0x100460064-0x100460070); the
 * report gate 0x10058ed28, for an agent in state 0x20 whose request has byte +1 == 0 and empty +0x58/+0x40, branches at
 * 0x10058ed68 to 0x10058ed80 and passes iff that list is non-empty (fSupportsBackgrounding is not read on this path); the
 * match 0x10058efe4 -> 0x10058f4a8 -> 0x100590498 then needs one listed UUID in the report's service-UUID list (equality
 * 0x1001930f0). The restricted-service check 0x100471a68 refuses only 0xFD6F and one 128-bit UUID [bin, stock bluetoothd].
 * 0xFD50 is a 16-bit UUID seen in bt-up20 reports (AD type 0x02, 8 rows) [obs]; bt-up20 put this agent in state 0x20 [obs]. */
static const char ARMC_UUID[] = "FD50";

struct armc_req {
	id services;
	int res;
};

static void
armc_on_queue(void *p)
{
	struct armc_req *r = p;

	scan_services = r->services;	/* every later scan (state event, resume) carries it, until arm B */
	scan_opts = 0;			/* options:nil: the daemon-flag knob is not part of arm C */
	if (MSGL(central_obj, "state") != BTC_STATE_POWERED_ON) {
		r->res = 0;
		return;
	}
	if (MSGB(central_obj, "isScanning"))
		MSG0(central_obj, "stopScan");
	MSG2(central_obj, "scanForPeripheralsWithServices:options:", scan_services, scan_opts);
	r->res = ++scans_sent;
}

static int
o_arm_c(void *ctx, char *desc, unsigned long desc_len)
{
	id nsarr, nsstr, cbuuid, str, uuid, arr, ustr;
	struct armc_req r = { 0, -1 };
	const char *u;
	size_t n = 0;

	(void)ctx;
	if (!central_created || delegate_q == 0) {
		gap("arm C without a central");
		return -1;
	}
	nsarr = objc_getClass_("NSArray");
	nsstr = objc_getClass_("NSString");
	cbuuid = objc_getClass_("CBUUID");
	if (nsarr == 0 || nsstr == 0 || cbuuid == 0) {
		gap("arm C: NSArray, NSString or CBUUID missing");
		return -1;
	}
	str = ((id (*)(id, SEL, const char *))objc_msgSend_)(nsstr, sel_registerName_("stringWithUTF8String:"), ARMC_UUID);
	if (str == 0) {
		gap("arm C: stringWithUTF8String returned nil");
		return -1;
	}
	uuid = ((id (*)(id, SEL, id))objc_msgSend_)(cbuuid, sel_registerName_("UUIDWithString:"), str);
	if (uuid == 0) {
		gap("arm C: CBUUID UUIDWithString returned nil");
		return -1;
	}
	arr = ((id (*)(id, SEL, const id *, unsigned long))objc_msgSend_)(nsarr, sel_registerName_("arrayWithObjects:count:"), &uuid, 1);
	if (arr == 0) {
		gap("arm C: arrayWithObjects:count: returned nil");
		return -1;
	}
	r.services = MSG0(arr, "retain");	/* kept for the rest of the run, as arm B's dictionary */
	/* the row names what was passed, read back from the CBUUID (UUIDString); only [0-9A-Fa-f-] are copied, <= 36 chars */
	ustr = MSG0(uuid, "UUIDString");
	u = ustr ? ((const char *(*)(id, SEL))objc_msgSend_)(ustr, sel_registerName_("UTF8String")) : 0;
	for (; u && *u && n < 36 && n + 1 < desc_len; u++)
		if ((*u >= '0' && *u <= '9') || (*u >= 'A' && *u <= 'F') || (*u >= 'a' && *u <= 'f') || *u == '-')
			desc[n++] = *u;
	if (n == 0) {
		gap("arm C: CBUUID UUIDString unreadable (the services array is still passed)");
		snprintf(desc, desc_len, "unreadable");
	} else {
		desc[n] = 0;
	}
	dispatch_sync_f_(delegate_q, &r, armc_on_queue);
	return r.res;
}

static int
o_rescan(void *ctx)
{
	int res = -1;

	(void)ctx;
	if (!central_created || delegate_q == 0) {
		gap("rescan without a central");
		return -1;
	}
	dispatch_sync_f_(delegate_q, &res, rescan_on_queue);
	return res;
}

static void
o_sleep_s(void *ctx, unsigned secs)
{
	(void)ctx;
	sleep(secs);
}

static void
o_out(void *ctx, const char *line)
{
	(void)ctx;
	printf("%s\n", line);	/* one write per line: patch 0048 mirrors each write as a row */
	fflush(stdout);
}

/* T-3615 connect: delegate-queue part (pick + connectPeripheral:options:) */
struct conn_req {
	int res;	/* 1 = connect issued, 0 = no candidate, -1 = not poweredOn */
	unsigned cands;
	int rssi;
};

static void
conn_start_on_queue(void *p)
{
	struct conn_req *q = p;
	int i, best = -1, want, n;

	if (MSGL(central_obj, "state") != BTC_STATE_POWERED_ON) {
		q->res = -1;
		return;
	}
	for (want = 1; want >= -1 && best < 0; want -= 2) {	/* connectable first, then (key absent) unknown */
		n = 0;
		for (i = 0; i < CAND_N; i++)
			if (cand[i].p && !cand[i].used && cand[i].conn == want) {
				n++;
				if (best < 0 || cand[i].rssi > cand[best].rssi)
					best = i;
			}
		q->cands = (unsigned)n;
		if (best >= 0 && want == -1 && !conn_unknown_gap) {
			conn_unknown_gap = 1;
			gap("no advertisement carried CBAdvertisementDataIsConnectable=YES: connecting to the strongest of %d unknown-connectable", n);
		}
	}
	if (best < 0) {
		q->res = 0;
		return;
	}
	cand[best].used = 1;
	conn_cur = cand[best].p;
	q->rssi = cand[best].rssi;
	if (MSGB(central_obj, "isScanning"))
		MSG0(central_obj, "stopScan");
	MSGP(conn_cur, "setDelegate:", delegate_obj);
	conn_has_err = 0;
	conn_err = 0;
	conn_services = -1;
	conn_ms = 0;
	__atomic_store_n(&conn_disc, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&conn_phase, CP_PENDING, __ATOMIC_SEQ_CST);
	conn_t0 = now_ms();
	MSG2(central_obj, "connectPeripheral:options:", conn_cur, (id)0);
	q->res = 1;
}

static void
conn_cancel_on_queue(void *p)
{
	(void)p;
	if (conn_cur)
		MSGP(central_obj, "cancelPeripheralConnection:", conn_cur);
}

/* poll (50 ms) until *flag != until_not or ms elapsed; returns the final value */
static int
wait_flag(int *flag, int until_not, unsigned ms)
{
	unsigned t0 = now_ms();

	while (__atomic_load_n(flag, __ATOMIC_SEQ_CST) == until_not && now_ms() - t0 < ms)
		usleep(50 * 1000);
	return __atomic_load_n(flag, __ATOMIC_SEQ_CST);
}

int
btc_ios_connect(unsigned try_idx, unsigned timeout_s, struct btc_conn *r)
{
	struct conn_req q = { -1, 0, 127 };
	int ph;

	memset(r, 0, sizeof *r);
	r->rssi = 127;
	r->services = -1;
	if (!central_created || delegate_q == 0 || dispatch_sync_f_ == 0 || !conn_cb_ok) {
		gap("connect try=%u without a central, a queue or the connect callbacks", try_idx + 1);
		r->outcome = BTC_CONN_NOCENTRAL;
		return -1;
	}
	dispatch_sync_f_(delegate_q, &q, conn_start_on_queue);
	r->cands = q.cands;
	if (q.res < 0) {
		gap("connect try=%u: central not poweredOn", try_idx + 1);
		r->outcome = BTC_CONN_NOCENTRAL;
		return -1;
	}
	if (q.res == 0) {
		r->outcome = BTC_CONN_NOCAND;
		return 0;
	}
	r->rssi = q.rssi;
	emit("BT CLIENT: connect try=%u issued cands=%u rssi=%d timeout=%us", try_idx + 1, q.cands, q.rssi, timeout_s);
	ph = wait_flag(&conn_phase, CP_PENDING, timeout_s * 1000u);
	if (ph == CP_PENDING) {
		dispatch_sync_f_(delegate_q, 0, conn_cancel_on_queue);
		r->outcome = BTC_CONN_TIMEOUT;
		r->ms = timeout_s * 1000u;
	} else if (ph == CP_FAILED) {
		r->outcome = BTC_CONN_FAIL;
		r->has_err = conn_has_err;
		r->err = conn_err;
		r->ms = conn_ms;
	} else {
		r->outcome = BTC_CONN_OK;
		r->ms = conn_ms;
		ph = wait_flag(&conn_phase, CP_CONNECTED, 10000);
		r->services = ph == CP_SERVICES ? conn_services : -1;
		dispatch_sync_f_(delegate_q, 0, conn_cancel_on_queue);
	}
	r->disc = wait_flag(&conn_disc, 0, 3000);
	return 0;
}

/* T-2d63: the counters as the delegate queue sees them, for the app's state-transition snapshots (any thread but the delegate
 * queue). 0/0 before the central and its queue exist. */
void
btc_ios_counters(unsigned *discovered, unsigned *unique)
{
	struct btc_sample s = { 0, 0, 0, 0, 0 };

	if (delegate_q && dispatch_sync_f_)
		dispatch_sync_f_(delegate_q, &s, copy_counters);
	*discovered = s.discovered;
	*unique = s.unique;
}

/* T-2d63: resolve the runtime symbols and fill *ops with this backend (the bt-probe main and BTApp share it). Returns 0, or -1
 * when a libobjc symbol is missing (nothing can run; the caller prints its own start/GAP/RESULT lines). */
int
btc_ios_setup(struct btc_ops *ops)
{
	void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW), *cb = dlopen("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth", RTLD_NOW),
	     *fo = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW) /* NSObject; no NSLog: it takes an NSString, a C string crashed bt-up16 */;

	*ops = (struct btc_ops){ 0, o_start, o_central_options, o_rescan, o_arm_b, o_arm_c, o_sample, o_sleep_s, o_out };
	g_ops = ops;
	sel_registerName_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	objc_msgSend_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	class_addMethod_ = dlsym(rt ? rt : RTLD_DEFAULT, "class_addMethod");
	objc_allocateClassPair_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_allocateClassPair");
	objc_registerClassPair_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_registerClassPair");
	objc_getClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	dispatch_queue_create_ = dlsym(RTLD_DEFAULT, "dispatch_queue_create");
	dispatch_sync_f_ = dlsym(RTLD_DEFAULT, "dispatch_sync_f");
	(void)fo;
	cb_handle = cb;	/* the key constant is exported by CoreBluetooth itself */
	return (!sel_registerName_ || !objc_msgSend_ || !class_addMethod_ || !objc_allocateClassPair_ || !objc_registerClassPair_ || !objc_getClass_) ? -1 : 0;
}

#ifndef BTC_NO_MAIN
int
main(int argc, char **argv)
{
	static struct btc_ops ops;

	setvbuf(stdout, 0, _IONBF, 0);
	if (argc >= 2) {	/* bt-proper: `adv|tone [seconds] [--delay N]`; any other argument is a GAP, never ignored */
		if (strcmp(argv[1], "adv") == 0) {
			unsigned secs, delay_s;
			char why[96];

			if (btadv_parse(argc - 2, argv + 2, &secs, &delay_s, why, sizeof why) != 0) {
				printf("BT ADV GAP: %s\n", why);
				printf("BT ADV: RESULT: FAIL adv reason=usage started=0 err=0 state=-1 seconds=0 reads=0 centrals=0 gaps=1\n");
				return 2;
			}
			return btadv_ios_main(secs, delay_s);
		}
		if (strcmp(argv[1], "tone") == 0) {
			unsigned secs, delay_s;
			char why[96];

			if (bttone_parse(argc - 2, argv + 2, &secs, &delay_s, why, sizeof why) != 0) {
				printf("BT TONE GAP: %s\n", why);
				printf("BT TONE: RESULT: FAIL reason=usage route=? played_s=0 qerr=0 refills=0 gaps=1\n");
				return 2;
			}
			return bttone_ios_main(secs, delay_s);
		}
		printf("BT CLIENT GAP: unknown mode (only \"adv\" and \"tone\" take arguments); nothing was run\n");
		printf("BT CLIENT: RESULT fail discovered=0 unique=0 state=-1 gaps=1\n");
		return 2;
	}
	if (btc_ios_setup(&ops) != 0) {
		fputs("BT CLIENT: start build=" BTC_BUILD "\n", stdout);
		fputs("BT CLIENT GAP: libobjc symbols missing\n", stdout);
		fputs("BT CLIENT: RESULT fail discovered=0 unique=0 state=-1 gaps=1\n", stdout);
		return 2;
	}
	return btc_run(&ops, 10, 180, 120, 60, BTC_BUILD);	/* T-27d4: arm A t=10..60 s, arm C t=70..120 s, arm B t=130..180 s */
}
#endif
