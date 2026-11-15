/* bt-logread (sprint-bt 2026-11-09): read bluetoothd's own unified-log lines on the phone, so the BluetoothManager attach can be
 * judged from the daemon side ("insufficient entitlements" fault vs the "accessLevel" line of our session). Userland os_log does
 * not reach the host log on PixOS; this one-shot launchd job (dev overlay, user root) opens the system log store through the
 * public-in-macOS / private-on-iOS OSLog API +[OSLogStore storeWithScope:error:] (scope 0 = system; class-dumped from the iOS 16.7
 * 20H392 dyld cache), enumerates the entries since the latest boot whose process starts with "bluetoothd", and prints:
 *   - counts: entries read, bluetoothd entries, fault-level entries;
 *   - only the bluetoothd lines that contain one of the KEYS below (session/entitlement lines; they carry session names like
 *     "com.pixos.btapp-<pid>" and an access level, never a BT address).
 * Every failure prints a "BT LOGREAD GAP:" line (rule 8). stdout = the job's /dev/null, mirrored by xnu patch 0048 for the process
 * name in boot-arg sweetnull=...,bt-logread.
 * Usage: bt-logread [--procs a,b] [--keys k1,k2] [--max N] [--debug] [delay_s ...]  one pass after each delay (cumulative), default 600 1000. */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/resource.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef BTC_BUILD
#define BTC_BUILD "dev"
#endif

typedef void *id;
typedef void *SEL;

static SEL (*sel_)(const char *);
static id (*getClass_)(const char *);
static void *msg_;
static void *(*pool_push)(void);
static void (*pool_pop)(void *);
#define M0(o, s) ((id (*)(id, SEL))msg_)((o), sel_(s))
#define ML(o, s) ((long (*)(id, SEL))msg_)((o), sel_(s))

static const char *KEYS[] = { "entitlement", "accessLevel", "MBFramework", "XpcMBFrameworkConnection", "BTSession", "asking to",
	/* bt-bw1 (2026-11-10): bluetoothd's platform answer for multi-advertising (strings of the iOS 16.7 bluetoothd: "Using a single advertising
	 * instance", "Overriding Multiple Advertising support - %s", "failed to start advertising instance %d ...", "Advertising instance %d changed:") */
	"advertising instance", "Advertising instance", "Multiple Advertising", "MultipleAdvertising" };

static const char *
cstr(id s)
{
	return s ? ((const char *(*)(id, SEL))msg_)(s, sel_("UTF8String")) : 0;
}

/* LoggingSupport path (what `log show` uses): +[OSLogEventStore localStore] -> -prepareWithCompletionHandler:(source, error) ->
 * -[OSLogEventStream initWithSource:] + setFilterPredicate: + setEventHandler:(proxy) + setInvalidationHandler: -> activateStreamFromLastBoot.
 * Selectors class-dumped from the 20H392 cache; +[OSLogStore storeWithScope:] accepts only scope 1 on iOS (cmp x2,#1 at 0x1c8656a28),
 * which is why bt-mgr4's first build got nil. The completion/invalidation block argument lists are [guess]: the first argument is
 * type-checked (NSError vs source) and printed. */
static unsigned long total, bt, faults, shown, insufficient, acclines;
static unsigned cur_pass;
/* 2026-11-11 (BT-PROPER A2DP): optional --procs a,b (process prefixes; default bluetoothd), --keys k1,k2 (default KEYS), --max N lines */
static char procs_buf[256] = "bluetoothd", keys_buf[512];
static const char *xprocs[8] = { procs_buf }, *xkeys[32];
static unsigned nprocs = 1, nkeys, maxshow = 60;
/* 2026-11-11 --debug: include info + debug entries. -[OSLogEventStream setFlags:] (property `flags`, ivar +0x20, class-dump of the 20H392
 * cache) is read by -[OSLogEventStream _activateStreamInRange:] (0x19ed21dbc [bin]): 0x19ed21ee8-0x19ed21f30 copy flag bits 0x3|0x4|0x8|0x10|
 * 0x20|0x40|0x80|0x100|0x200 into the filter mask passed to 0x19ed7ef80, default flags = 0 (-initWithSource: passes 0, 0x19ed467f0). Bits
 * 0x100 (info) and 0x200 (debug) are the OS_ACTIVITY_STREAM_INFO/DEBUG values of libtrace [guess: from memory of the private header, the
 * naming is not read here]. Both are set, so either naming gives info+debug; the first boot with --debug must show type=0x2 (debug) rows. */
static unsigned split(char *b, const char **out, unsigned cap) { unsigned n = 0; for (char *t = strtok(b, ","); t && n < cap; t = strtok(0, ",")) out[n++] = t; return n; }

static void
on_event(id proxy)
{
	const char *m, *proc;
	unsigned long long lt;

	total++;
	proc = cstr(M0(proxy, "process"));
	{
		unsigned pi;

		for (pi = 0; proc && pi < nprocs; pi++)
			if (strncmp(proc, xprocs[pi], strlen(xprocs[pi])) == 0)
				break;
		if (!proc || pi == nprocs)
			return;
	}
	bt++;
	lt = ((unsigned long long (*)(id, SEL))msg_)(proxy, sel_("logType"));
	if (lt == 0x11)	/* OS_LOG_TYPE_FAULT */
		faults++;
	m = cstr(M0(proxy, "composedMessage"));
	if (!m)
		return;
	if (strstr(m, "insufficient entitlements"))
		insufficient++;
	if (strstr(m, "accessLevel"))
		acclines++;
	if (nkeys) {
		for (unsigned k = 0; k < nkeys; k++)
			if (strstr(m, xkeys[k])) {
				if (shown++ < maxshow)
					printf("BT LOGREAD: pass=%u %s type=0x%llx %.300s\n", cur_pass, proc, lt, m);
				break;
			}
		return;
	}
	for (unsigned k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++)
		if (strstr(m, KEYS[k])) {
			if (shown++ < 60)
				printf("BT LOGREAD: pass=%u type=0x%llx %.220s\n", cur_pass, lt, m);
			break;
		}
}

static void
apply_flags(id stream, unsigned long long stream_flags)
{
	unsigned long long back;

	((void (*)(id, SEL, unsigned long long))msg_)(stream, sel_("setFlags:"), stream_flags);
	back = ((unsigned long long (*)(id, SEL))msg_)(stream, sel_("flags"));
	printf("BT LOGREAD: stream flags=0x%llx readback=0x%llx\n", stream_flags, back);
}

static void
one_pass(unsigned pass, unsigned long long stream_flags)
{
	void *pool = pool_push ? pool_push() : 0;
	id cls = getClass_("OSLogEventStore"), store, stream, pred;
	__block id source = 0;
	dispatch_semaphore_t sem = dispatch_semaphore_create(0), done = dispatch_semaphore_create(0);

	cur_pass = pass;
	total = bt = faults = shown = insufficient = acclines = 0;
	if (!cls || !getClass_("OSLogEventStream")) {
		printf("BT LOGREAD GAP: LoggingSupport classes OSLogEventStore/OSLogEventStream missing\n");
		goto out;
	}
	store = M0(cls, "localStore");
	if (!store) {
		printf("BT LOGREAD GAP: +[OSLogEventStore localStore] nil\n");
		goto out;
	}
	((void (*)(id, SEL, void (^)(id, id)))msg_)(store, sel_("prepareWithCompletionHandler:"), ^(id a, id b) {
		bool a_err = a && ((bool (*)(id, SEL, id))msg_)(a, sel_("isKindOfClass:"), getClass_("NSError"));
		if (a && !a_err)
			source = ((id (*)(id))dlsym(RTLD_DEFAULT, "objc_retain"))(a);
		else
			printf("BT LOGREAD GAP: prepare: source nil, error=%s code=%ld desc=%.160s\n", a_err ? "arg1" : (b ? "arg2" : "none"),
			    a_err ? ML(a, "code") : (b ? ML(b, "code") : 0L), a_err ? cstr(M0(a, "localizedDescription")) : (b ? cstr(M0(b, "localizedDescription")) : "nil"));
		dispatch_semaphore_signal(sem);
	});
	if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 60LL * NSEC_PER_SEC)) != 0) {
		printf("BT LOGREAD GAP: prepareWithCompletionHandler: no answer in 60 s\n");
		goto out;
	}
	if (!source)
		goto out;
	stream = ((id (*)(id, SEL, id))msg_)(M0(getClass_("OSLogEventStream"), "alloc"), sel_("initWithSource:"), source);
	if (!stream) {
		printf("BT LOGREAD GAP: OSLogEventStream initWithSource: nil\n");
		goto out;
	}
	{
		char ps[512] = "";

		for (unsigned pi = 0; pi < nprocs; pi++) {
			if (pi)
				strlcat(ps, " OR ", sizeof ps);
			strlcat(ps, "process BEGINSWITH '", sizeof ps);
			strlcat(ps, xprocs[pi], sizeof ps);
			strlcat(ps, "'", sizeof ps);
		}
		pred = ((id (*)(id, SEL, id))msg_)(getClass_("NSPredicate"), sel_("predicateWithFormat:"),
		    ((id (*)(id, SEL, const char *))msg_)(getClass_("NSString"), sel_("stringWithUTF8String:"), ps));
	}
	((void (*)(id, SEL, id))msg_)(stream, sel_("setFilterPredicate:"), pred);
	if (stream_flags)
		apply_flags(stream, stream_flags);
	((void (*)(id, SEL, void (^)(id)))msg_)(stream, sel_("setEventHandler:"), ^(id proxy) {
		/* bt-mgr5/6: the reader grew to 6 MB and was jetsam-killed; without a pool per event every proxy string stays alive until the
		 * stream ends [guess: the dominant growth]. Pop after each event. */
		void *ep = pool_push ? pool_push() : 0;
		on_event(proxy);
		if (ep)
			pool_pop(ep);
	});
	((void (*)(id, SEL, void (^)(id, unsigned long, id)))msg_)(stream, sel_("setInvalidationHandler:"), ^(id st, unsigned long reason, id info) {
		(void)st; (void)info;
		printf("BT LOGREAD: pass=%u stream invalidated reason=%lu\n", cur_pass, reason);
		dispatch_semaphore_signal(done);
	});
	M0(stream, "activateStreamFromLastBoot");
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 180LL * NSEC_PER_SEC)) != 0)
		printf("BT LOGREAD GAP: stream not finished in 180 s (counts below are partial)\n");
	{
		struct rusage ru;

		if (getrusage(RUSAGE_SELF, &ru) == 0)
			printf("BT LOGREAD: pass=%u maxrss_kb=%ld\n", pass, (long)(ru.ru_maxrss / 1024));
	}
	printf("BT LOGREAD: pass=%u RESULT entries=%lu bluetoothd=%lu faults=%lu insufficient_entitlements=%lu accessLevel_lines=%lu shown=%lu\n", pass, total, bt,
	    faults, insufficient, acclines, shown);
out:
	if (pool_pop && pool)
		pool_pop(pool);
}

int
main(int argc, char **argv)
{
	void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	void *ol = dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
	unsigned delays[8] = { 600, 300 }, n = 2;
	unsigned long long stream_flags = 0;

	setvbuf(stdout, 0, _IONBF, 0);
	if (argc > 1) {
		n = 0;
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--procs") == 0 && i + 1 < argc) {
				strlcpy(procs_buf, argv[++i], sizeof procs_buf);
				nprocs = split(procs_buf, xprocs, 8);
			} else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
				strlcpy(keys_buf, argv[++i], sizeof keys_buf);
				nkeys = split(keys_buf, xkeys, 32);
			} else if (strcmp(argv[i], "--debug") == 0) {
				stream_flags = 0x300;
			} else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
				maxshow = (unsigned)strtoul(argv[++i], 0, 10);
			} else if (n < 8) {
				delays[n++] = (unsigned)strtoul(argv[i], 0, 10);
			}
		}
	}
	sel_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	getClass_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	msg_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	pool_push = dlsym(rt ? rt : RTLD_DEFAULT, "objc_autoreleasePoolPush");
	pool_pop = dlsym(rt ? rt : RTLD_DEFAULT, "objc_autoreleasePoolPop");
	printf("BT LOGREAD: start build=%s uid=%d passes=%u loggingsupport=%s\n", BTC_BUILD, (int)getuid(), n, ol ? "loaded" : "NOT-loaded");
	{	/* bt-mgr5/6: launchd gave this overlay job the 6 MB default and jetsam killed it mid-read (JetsamProperties on the job was not
		 * applied). Raise our own limit: MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT = 6, flags = limit in MB, allowed for root
		 * (xnu bsd/sys/kern_memorystatus.h:349, kern_memorystatus.c memorystatus_control: kauth_cred_issuser) [src]. */
		int (*msc)(unsigned, int, unsigned, void *, size_t) = dlsym(RTLD_DEFAULT, "memorystatus_control");
		int rc = msc ? msc(6, getpid(), 300, 0, 0) : -2;

		if (rc != 0)
			printf("BT LOGREAD GAP: memorystatus_control(SET_JETSAM_TASK_LIMIT, 300 MB) rc=%d errno=%d: jetsam may kill this job at 6 MB\n", rc, rc == -2 ? 0 : errno);
		else
			printf("BT LOGREAD: jetsam limit set to 300 MB\n");
	}
	if (!sel_ || !getClass_ || !msg_ || !ol) {
		printf("BT LOGREAD GAP: libobjc symbols or LoggingSupport.framework missing\n");
		return 2;
	}
	for (unsigned i = 0; i < n; i++) {
		sleep(delays[i]);
		one_pass(i + 1, stream_flags);
	}
	return 0;
}
