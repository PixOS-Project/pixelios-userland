/* bt-client adv iOS backend (bt-proper): one CBPeripheralManager that publishes a connectable GATT service with one readable
 * characteristic and advertises it, so bluetoothd runs its multi-advertising path (Apple VSCs 0xfd4a..0xfd4d). Like
 * bt_client_ios.c it reaches CoreBluetooth through the ObjC runtime and dlsym, so a missing piece becomes a "BT ADV GAP:" line;
 * protocol methods are installed with class_addMethod. stdout goes to the launchd job's /dev/null, which xnu patch 0048 mirrors
 * into the host log for the process names in boot-arg sweetnull=...,bt-client. Counts only: the central behind a read request
 * is compared by pointer and never printed (rule 4). */
#include "bt_client.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef BTC_BUILD
#define BTC_BUILD "dev"
#endif

/* Fixed random identifiers, generated once with uuid4 and not derived from anything: they only make the advertisement and the
 * service recognisable to the external phone that verifies the run. The service UUID is the one advertised; the characteristic
 * UUID is its readable child; reading it returns ADV_VALUE. */
#define ADV_SERVICE_UUID "696FEF29-FD82-40E9-804C-55A6E1A2DA7F"
#define ADV_CHAR_UUID "29B822B3-2F8F-451A-9279-52CEED6BA087"
#define ADV_LOCAL_NAME "PixOS-adv"
#define ADV_VALUE "pixos"
/* CoreBluetooth public headers (iPhoneOS16.5 SDK): CBCharacteristicPropertyRead = 0x02, CBAttributePermissionsReadable = 0x01,
 * CBATTErrorSuccess = 0x00, CBATTErrorInvalidOffset = 0x07, CBManagerStatePoweredOn = 5. */
#define CB_PROP_READ 0x02UL
#define CB_PERM_READABLE 0x01UL
#define CB_ATT_SUCCESS 0L
#define CB_ATT_INVALID_OFFSET 7L

typedef void *id;
typedef void *SEL;
typedef void (*IMP)(void);

static SEL (*sel_)(const char *);
static bool (*add_method_)(id, SEL, IMP, const char *);
static id (*alloc_class_)(id, const char *, size_t);
static void (*register_class_)(id);
static id (*get_class_)(const char *);
static void *msg_;
static id (*queue_create_)(const char *, void *);
static void (*sync_f_)(id, void *, void (*)(void *));

#define M0(o, s) ((id (*)(id, SEL))msg_)((o), sel_(s))
#define ML(o, s) ((long (*)(id, SEL))msg_)((o), sel_(s))
#define MB(o, s) ((bool (*)(id, SEL))msg_)((o), sel_(s))
#define M1(o, s, a) ((id (*)(id, SEL, id))msg_)((o), sel_(s), (a))
#define M1V(o, s, a) ((void (*)(id, SEL, id))msg_)((o), sel_(s), (a))
#define M3(o, s, a, b, c) ((id (*)(id, SEL, id, id, id))msg_)((o), sel_(s), (a), (b), (c))

static void *cb_handle;
static id pm, delegate_q, delegate_obj, service_obj, adv_dict, value_data;
static const struct btadv_ops *g_ops;
static unsigned gap_n;
/* written on the delegate queue only (read there too, through sample_on_queue) */
static int svc_state, adv_state;
static long err_code;
static char err_domain[48];
static unsigned reads_n, centrals_n;	/* centrals_n <= 8 */
static id centrals[8];	/* pointer identity of the distinct centrals seen (not retained: used for equality only) */

static void
gap(const char *fmt, ...)
{
	char b[200], l[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	__atomic_fetch_add(&gap_n, 1, __ATOMIC_RELAXED);
	snprintf(l, sizeof l, "BT ADV GAP: %s", b);
	g_ops->out((void *)0, l);
}

static id
nsstr(const char *s)
{
	return ((id (*)(id, SEL, const char *))msg_)(get_class_("NSString"), sel_("stringWithUTF8String:"), s);
}

/* record an NSError's domain (copied with a charset filter: it is a framework constant, but never trust a string into a log) and code */
static void
note_error(id err)
{
	const char *d = 0;
	id dom = err ? M0(err, "domain") : 0;
	size_t n = 0;

	err_code = err ? ML(err, "code") : -1;
	if (dom)
		d = ((const char *(*)(id, SEL))msg_)(dom, sel_("UTF8String"));
	if (d == 0)
		d = "unknown";
	for (; *d && n < sizeof err_domain - 1; d++)
		if ((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z') || (*d >= '0' && *d <= '9') || *d == '_' || *d == '.' || *d == '-')
			err_domain[n++] = *d;
	err_domain[n] = 0;
}

static void
m_did_update_state(id self, SEL cmd, id manager)
{
	(void)self;
	(void)cmd;
	char b[80];

	snprintf(b, sizeof b, "BT ADV: state event=%ld", ML(manager, "state"));
	g_ops->out((void *)0, b);
}

static void
m_did_add_service(id self, SEL cmd, id manager, id service, id err)
{
	(void)self;
	(void)cmd;
	(void)manager;
	(void)service;
	if (err) {
		note_error(err);
		svc_state = -1;
	} else {
		svc_state = 1;
	}
}

static void
m_did_start_adv(id self, SEL cmd, id manager, id err)
{
	(void)self;
	(void)cmd;
	(void)manager;
	if (err) {
		note_error(err);
		adv_state = -1;
	} else {
		adv_state = 1;
	}
}

static void
m_read_request(id self, SEL cmd, id manager, id req)
{
	unsigned long off = ((unsigned long (*)(id, SEL))msg_)(req, sel_("offset"));
	id central = M0(req, "central"), data;
	unsigned i;
	const unsigned long len = sizeof ADV_VALUE - 1;

	(void)self;
	(void)cmd;
	reads_n++;
	/* distinct = distinct CBCentral object pointers, capped at 8 [guess: CoreBluetooth reuses one object per connected central; if it
	 * hands out a fresh object per request this over-counts up to the cap] */
	for (i = 0; i < centrals_n; i++)
		if (centrals[i] == central)
			break;
	if (i == centrals_n && centrals_n < 8)
		centrals[centrals_n++] = central;
	if (off > len) {
		((void (*)(id, SEL, id, long))msg_)(manager, sel_("respondToRequest:withResult:"), req, CB_ATT_INVALID_OFFSET);
		return;
	}
	data = ((id (*)(id, SEL, const void *, unsigned long))msg_)(get_class_("NSData"), sel_("dataWithBytes:length:"), ADV_VALUE + off, len - off);
	M1V(req, "setValue:", data);
	((void (*)(id, SEL, id, long))msg_)(manager, sel_("respondToRequest:withResult:"), req, CB_ATT_SUCCESS);
}

static int
a_start(void *ctx)
{
	id cls, dcls;
	(void)ctx;

	cls = get_class_("CBPeripheralManager");
	if (cls == 0) {
		gap("CBPeripheralManager class not found");
		return -1;
	}
	if (queue_create_ == 0 || sync_f_ == 0) {
		gap("libdispatch symbols missing (dispatch_queue_create/dispatch_sync_f)");
		return -1;
	}
	dcls = alloc_class_(get_class_("NSObject"), "BTAdvDelegate", 0);
	if (dcls == 0) {
		gap("cannot create the delegate class");
		return -1;
	}
	if (!add_method_(dcls, sel_("peripheralManagerDidUpdateState:"), (IMP)m_did_update_state, "v@:@") ||
	    !add_method_(dcls, sel_("peripheralManager:didAddService:error:"), (IMP)m_did_add_service, "v@:@@@") ||
	    !add_method_(dcls, sel_("peripheralManagerDidStartAdvertising:error:"), (IMP)m_did_start_adv, "v@:@@") ||
	    !add_method_(dcls, sel_("peripheralManager:didReceiveReadRequest:"), (IMP)m_read_request, "v@:@@")) {
		gap("class_addMethod for the CBPeripheralManagerDelegate methods failed");
		return -1;
	}
	register_class_(dcls);
	delegate_obj = M0(dcls, "new");
	delegate_q = queue_create_("com.pixos.btadv", (void *)0);
	if (delegate_obj == 0 || delegate_q == 0) {
		gap("delegate or dispatch queue could not be created");
		return -1;
	}
	pm = M3(M0(cls, "alloc"), "initWithDelegate:queue:options:", delegate_obj, delegate_q, (id)0);
	if (pm == 0) {
		gap("cannot instantiate CBPeripheralManager");
		return -1;
	}
	{
		char b[80];

		snprintf(b, sizeof b, "BT ADV: peripheral manager created state=%ld", ML(pm, "state"));
		g_ops->out((void *)0, b);
	}
	return 0;
}

/* build the GATT service: primary service ADV_SERVICE_UUID with one read-only characteristic whose value is nil in the object, so
 * every read reaches didReceiveReadRequest (CBCharacteristic.h: a value given at init is cached and answered without the delegate). */
static void
add_service_on_queue(void *p)
{
	int *rc = p;
	id uuid_cls = get_class_("CBUUID"), svc_cls = get_class_("CBMutableService"), chr_cls = get_class_("CBMutableCharacteristic");
	id suuid, cuuid, chr, arr;

	*rc = -1;
	if (!uuid_cls || !svc_cls || !chr_cls) {
		gap("CBUUID/CBMutableService/CBMutableCharacteristic class missing");
		return;
	}
	suuid = M1(uuid_cls, "UUIDWithString:", nsstr(ADV_SERVICE_UUID));
	cuuid = M1(uuid_cls, "UUIDWithString:", nsstr(ADV_CHAR_UUID));
	if (!suuid || !cuuid) {
		gap("CBUUID UUIDWithString: returned nil");
		return;
	}
	chr = ((id (*)(id, SEL, id, unsigned long, id, unsigned long))msg_)(M0(chr_cls, "alloc"), sel_("initWithType:properties:value:permissions:"),
	    cuuid, CB_PROP_READ, (id)0, CB_PERM_READABLE);
	service_obj = ((id (*)(id, SEL, id, bool))msg_)(M0(svc_cls, "alloc"), sel_("initWithType:primary:"), suuid, true);
	if (!chr || !service_obj) {
		gap("cannot instantiate the mutable characteristic or service");
		return;
	}
	arr = ((id (*)(id, SEL, const id *, unsigned long))msg_)(get_class_("NSArray"), sel_("arrayWithObjects:count:"), &chr, 1);
	if (!arr) {
		gap("NSArray arrayWithObjects:count: returned nil");
		return;
	}
	M1V(service_obj, "setCharacteristics:", arr);
	M1V(pm, "addService:", service_obj);
	*rc = 0;
}

static int
a_add_service(void *ctx)
{
	int rc = -1;
	(void)ctx;
	sync_f_(delegate_q, &rc, add_service_on_queue);
	return rc;
}

/* {CBAdvertisementDataLocalNameKey: ADV_LOCAL_NAME, CBAdvertisementDataServiceUUIDsKey: @[ADV_SERVICE_UUID]}; the keys are the
 * values of the CoreBluetooth exported constants, read at run time */
static void
advertise_on_queue(void *p)
{
	int *rc = p;
	id *k_name = dlsym(cb_handle ? cb_handle : RTLD_DEFAULT, "CBAdvertisementDataLocalNameKey");
	id *k_uuids = dlsym(cb_handle ? cb_handle : RTLD_DEFAULT, "CBAdvertisementDataServiceUUIDsKey");
	id suuid, uuids, vals[2], keys[2];

	*rc = -1;
	if (!k_name || !*k_name || !k_uuids || !*k_uuids) {
		gap("CBAdvertisementDataLocalNameKey/ServiceUUIDsKey constants missing");
		return;
	}
	suuid = M1(get_class_("CBUUID"), "UUIDWithString:", nsstr(ADV_SERVICE_UUID));
	uuids = suuid ? ((id (*)(id, SEL, const id *, unsigned long))msg_)(get_class_("NSArray"), sel_("arrayWithObjects:count:"), &suuid, 1) : 0;
	if (!uuids) {
		gap("service UUID array not built");
		return;
	}
	keys[0] = *k_name;
	vals[0] = nsstr(ADV_LOCAL_NAME);
	keys[1] = *k_uuids;
	vals[1] = uuids;
	adv_dict = ((id (*)(id, SEL, const id *, const id *, unsigned long))msg_)(get_class_("NSDictionary"),
	    sel_("dictionaryWithObjects:forKeys:count:"), vals, keys, 2);
	if (!adv_dict) {
		gap("advertisement dictionary not built");
		return;
	}
	M1V(pm, "startAdvertising:", adv_dict);
	*rc = 0;
}

static int
a_advertise(void *ctx)
{
	int rc = -1;
	(void)ctx;
	sync_f_(delegate_q, &rc, advertise_on_queue);
	return rc;
}

static void
sample_on_queue(void *p)
{
	struct btadv_sample *s = p;

	s->state = ML(pm, "state");
	s->svc = svc_state;
	s->adv = adv_state;
	s->is_adv = (int)MB(pm, "isAdvertising");
	s->err = adv_state < 0 || svc_state < 0 ? err_code : 0;
	if (s->err)
		snprintf(s->err_domain, sizeof s->err_domain, "%s", err_domain);
	s->reads = reads_n;
	s->centrals = centrals_n;
}

static void
a_sample(void *ctx, struct btadv_sample *s)
{
	(void)ctx;
	memset(s, 0, sizeof *s);
	s->state = -1;
	s->is_adv = -1;
	if (pm && delegate_q)
		sync_f_(delegate_q, s, sample_on_queue);
	s->gaps = __atomic_load_n(&gap_n, __ATOMIC_RELAXED);
}

static void
stop_on_queue(void *p)
{
	(void)p;
	M0(pm, "stopAdvertising");
	M0(pm, "removeAllServices");
}

static void
a_stop(void *ctx)
{
	(void)ctx;
	if (pm && delegate_q)
		sync_f_(delegate_q, 0, stop_on_queue);
}

static void
a_sleep_s(void *ctx, unsigned secs)
{
	(void)ctx;
	sleep(secs);
}

static void
a_out(void *ctx, const char *line)
{
	(void)ctx;
	printf("%s\n", line);	/* one write per line: patch 0048 mirrors each write as a row */
	fflush(stdout);
}

int
btadv_ios_main(unsigned secs, unsigned delay_s)
{
	static struct btadv_ops ops;
	void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	void *cb = dlopen("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth", RTLD_NOW);
	void *fo = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);	/* NSString/NSArray/NSData */

	(void)fo;
	ops = (struct btadv_ops){ 0, a_start, a_add_service, a_advertise, a_sample, a_stop, a_sleep_s, a_out };
	g_ops = &ops;
	cb_handle = cb;
	sel_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	msg_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	add_method_ = dlsym(rt ? rt : RTLD_DEFAULT, "class_addMethod");
	alloc_class_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_allocateClassPair");
	register_class_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_registerClassPair");
	get_class_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	queue_create_ = dlsym(RTLD_DEFAULT, "dispatch_queue_create");
	sync_f_ = dlsym(RTLD_DEFAULT, "dispatch_sync_f");
	if (!sel_ || !msg_ || !add_method_ || !alloc_class_ || !register_class_ || !get_class_) {
		printf("BT ADV: start build=%s seconds=%u delay=%us\n", BTC_BUILD, secs, delay_s);
		printf("BT ADV GAP: libobjc symbols missing\n");
		printf("BT ADV: RESULT: FAIL adv reason=no-objc started=0 err=0 state=-1 seconds=0 reads=0 centrals=0 gaps=1\n");
		fflush(stdout);
		return 2;
	}
	if (!cb)
		g_ops->out(0, "BT ADV GAP: CoreBluetooth framework did not load (dlopen); class lookup will fail");
	return btadv_run(&ops, secs, delay_s, BTC_BUILD);
}
