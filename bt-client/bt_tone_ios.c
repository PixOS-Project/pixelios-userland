/* bt-client tone iOS backend (bt-proper): AVAudioSession (ObjC runtime through dlsym, like bt_adv_ios.c) sets category Playback and
 * activates; an AudioToolbox AudioQueue plays the sine of bttone_fill() (44.1 kHz, 16-bit stereo LPCM, BTTONE_NBUF buffers refilled in
 * the callback). iOS then encodes for the Bluetooth sink in mediaserverd (docs/research/bt-audio-path.md). Every failure is a
 * "BT TONE GAP:" line. Logs carry portType strings (framework constants, charset-filtered) and counts only: no device name or
 * address (rule 4). stdout goes to the launchd job's /dev/null; xnu patch 0048 mirrors it for process names in sweetnull=... */
#include "bt_client.h"

#include <AudioToolbox/AudioQueue.h>
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

typedef void *id;
typedef void *SEL;

static SEL (*sel_)(const char *);
static id (*get_class_)(const char *);
static void *msg_;

#define M0(o, s) ((id (*)(id, SEL))msg_)((o), sel_(s))
#define ML(o, s) ((unsigned long (*)(id, SEL))msg_)((o), sel_(s))
#define M1U(o, s, a) ((id (*)(id, SEL, unsigned long))msg_)((o), sel_(s), (a))
#define MBE(o, s, a, e) ((bool (*)(id, SEL, id, id *))msg_)((o), sel_(s), (a), (e))
#define MBBE(o, s, a, e) ((bool (*)(id, SEL, bool, id *))msg_)((o), sel_(s), (a), (e))

static const struct bttone_ops *g_ops;
static unsigned gap_n;
static AudioQueueRef queue;
static AudioQueueBufferRef bufs[BTTONE_NBUF];
static unsigned pos;	/* sine frame counter; touched by the priming code, then by the callback thread only */
static volatile int stopping;
static unsigned refills;
static long first_qerr;
static id session;
static int session_active;

static void
gap(const char *fmt, ...)
{
	char b[200], l[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	__atomic_fetch_add(&gap_n, 1, __ATOMIC_RELAXED);
	snprintf(l, sizeof l, "BT TONE GAP: %s", b);
	g_ops->out((void *)0, l);
}

/* copy a framework string into dst keeping only [A-Za-z0-9_.-]: never trust a string into a log */
static void
copy_safe(char *dst, unsigned long n, const char *s)
{
	unsigned long k = 0;

	for (; s && *s && k + 1 < n; s++)
		if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' || *s == '-')
			dst[k++] = *s;
	dst[k] = 0;
}

static const char *
utf8(id nsstr)
{
	return nsstr ? ((const char *(*)(id, SEL))msg_)(nsstr, sel_("UTF8String")) : 0;
}

static void
describe_error(const char *what, id err)
{
	char dom[48] = "unknown";

	if (err) {
		id d = M0(err, "domain");

		copy_safe(dom, sizeof dom, utf8(d));
		gap("%s failed: domain=%s code=%ld", what, dom, (long)ML(err, "code"));
	} else {
		gap("%s returned NO without an NSError", what);
	}
}

static int
t_session(void *ctx)
{
	id *cat = dlsym(RTLD_DEFAULT, "AVAudioSessionCategoryPlayback");
	id cls = get_class_("AVAudioSession"), err = 0;

	(void)ctx;
	if (!cls) {
		gap("AVAudioSession class not found (AVFoundation did not load)");
		return -1;
	}
	if (!cat || !*cat) {
		gap("AVAudioSessionCategoryPlayback symbol not found");
		return -1;
	}
	session = M0(cls, "sharedInstance");
	if (!session) {
		gap("[AVAudioSession sharedInstance] returned nil");
		return -1;
	}
	if (!MBE(session, "setCategory:error:", *cat, &err)) {
		describe_error("setCategory:Playback", err);
		return -1;
	}
	err = 0;
	session_active = 1;	/* from here stop() deactivates, also if setActive: reports failure */
	if (!MBBE(session, "setActive:error:", true, &err)) {
		describe_error("setActive:YES", err);
		return -1;
	}
	return 0;
}

static void
t_route(void *ctx, char *buf, unsigned long len)
{
	id route, outs;
	unsigned long n, i, k = 0;

	(void)ctx;
	snprintf(buf, len, "?");
	if (!session)
		return;
	route = M0(session, "currentRoute");
	outs = route ? M0(route, "outputs") : 0;
	if (!outs) {
		gap("currentRoute.outputs unreadable");
		return;
	}
	n = ML(outs, "count");
	if (n == 0) {
		snprintf(buf, len, "none");
		return;
	}
	buf[0] = 0;
	for (i = 0; i < n && k + 2 < len; i++) {
		id port = M1U(outs, "objectAtIndex:", i);
		char one[40];

		copy_safe(one, sizeof one, port ? utf8(M0(port, "portType")) : 0);
		if (one[0] == 0)
			snprintf(one, sizeof one, "unknown");
		k += (unsigned long)snprintf(buf + k, len - k, "%s%s", i ? "+" : "", one);
	}
}

static void
note_qerr(const char *what, OSStatus st)
{
	long expected = 0;

	if (stopping)	/* enqueue during the stop/reset is not a failure of the run */
		return;
	if (__atomic_compare_exchange_n(&first_qerr, &expected, (long)st, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		gap("AudioQueue %s failed: OSStatus=%d", what, (int)st);
}

static void
fill_buffer(AudioQueueBufferRef b)
{
	bttone_fill((short *)b->mAudioData, BTTONE_BUF_FRAMES, &pos);
	b->mAudioDataByteSize = BTTONE_BUF_FRAMES * 4;
}

static void
out_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
	OSStatus st;

	(void)user;
	if (stopping)
		return;
	fill_buffer(b);
	st = AudioQueueEnqueueBuffer(q, b, 0, 0);
	if (st != 0) {
		note_qerr("EnqueueBuffer (callback)", st);
		return;
	}
	__atomic_fetch_add(&refills, 1, __ATOMIC_RELAXED);
}

static int
t_start(void *ctx)
{
	AudioStreamBasicDescription f;
	OSStatus st;
	unsigned i;

	(void)ctx;
	memset(&f, 0, sizeof f);
	f.mSampleRate = BTTONE_RATE;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	f.mBytesPerPacket = 4;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 4;
	f.mChannelsPerFrame = 2;
	f.mBitsPerChannel = 16;
	st = AudioQueueNewOutput(&f, out_cb, 0, 0, 0, 0, &queue);
	if (st != 0) {
		gap("AudioQueueNewOutput failed: OSStatus=%d", (int)st);
		queue = 0;
		return -1;
	}
	for (i = 0; i < BTTONE_NBUF; i++) {
		st = AudioQueueAllocateBuffer(queue, BTTONE_BUF_FRAMES * 4, &bufs[i]);
		if (st != 0) {
			gap("AudioQueueAllocateBuffer #%u failed: OSStatus=%d", i, (int)st);
			return -1;
		}
		fill_buffer(bufs[i]);
		st = AudioQueueEnqueueBuffer(queue, bufs[i], 0, 0);
		if (st != 0) {
			gap("AudioQueueEnqueueBuffer (prime #%u) failed: OSStatus=%d", i, (int)st);
			return -1;
		}
	}
	st = AudioQueueStart(queue, 0);
	if (st != 0) {
		gap("AudioQueueStart failed: OSStatus=%d", (int)st);
		return -1;
	}
	return 0;
}

static void
t_sample(void *ctx, struct bttone_sample *s)
{
	(void)ctx;
	s->qerr = __atomic_load_n(&first_qerr, __ATOMIC_RELAXED);
	s->refills = __atomic_load_n(&refills, __ATOMIC_RELAXED);
	s->gaps = __atomic_load_n(&gap_n, __ATOMIC_RELAXED);
}

static void
t_stop(void *ctx)
{
	id err = 0;
	OSStatus st;

	(void)ctx;
	stopping = 1;
	if (queue) {
		st = AudioQueueStop(queue, true);
		if (st != 0)
			gap("AudioQueueStop failed: OSStatus=%d", (int)st);
		st = AudioQueueDispose(queue, true);
		if (st != 0)
			gap("AudioQueueDispose failed: OSStatus=%d", (int)st);
		queue = 0;
	}
	if (session && session_active) {
		session_active = 0;
		if (!MBBE(session, "setActive:error:", false, &err))
			describe_error("setActive:NO", err);
	}
}

static void
t_sleep_s(void *ctx, unsigned secs)
{
	(void)ctx;
	sleep(secs);
}

static void
t_out(void *ctx, const char *line)
{
	(void)ctx;
	printf("%s\n", line);	/* one write per line: patch 0048 mirrors each write as a row */
	fflush(stdout);
}

int
bttone_ios_main(unsigned secs, unsigned delay_s)
{
	static struct bttone_ops ops;
	void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	void *av = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW);
	void *fo = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);

	(void)fo;
	ops = (struct bttone_ops){ 0, t_session, t_route, t_start, t_sample, t_stop, t_sleep_s, t_out };
	g_ops = &ops;
	sel_ = dlsym(rt ? rt : RTLD_DEFAULT, "sel_registerName");
	msg_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_msgSend");
	get_class_ = dlsym(rt ? rt : RTLD_DEFAULT, "objc_getClass");
	if (!sel_ || !msg_ || !get_class_) {
		printf("BT TONE: start build=%s seconds=%u delay=%us\n", BTC_BUILD, secs, delay_s);
		printf("BT TONE GAP: libobjc symbols missing\n");
		printf("BT TONE: RESULT: FAIL reason=no-objc route=? played_s=0 qerr=0 refills=0 gaps=1\n");
		fflush(stdout);
		return 2;
	}
	if (!av)
		g_ops->out(0, "BT TONE GAP: AVFoundation framework did not load (dlopen); class lookup will fail");
	return bttone_run(&ops, secs, delay_s, BTC_BUILD);
}
