/* pixos-bluetool iOS glue: libxpc Mach-service listener for com.apple.BlueTool + the kctl `com.apple.uart.bluetooth` transport.
 * Declarations are local because the iPhoneOS16.5 SDK ships neither xpc/ nor sys/kern_control.h; every value is taken from the XNU / XPC
 * headers: XPC_CONNECTION_MACH_SERVICE_LISTENER (1<<0) = MacOSX14.0.sdk xpc/connection.h:104; CTLIOCGINFO = _IOWR('N',3,struct ctl_info),
 * struct ctl_info {u32 ctl_id; char ctl_name[96]}, struct sockaddr_ctl, AF_SYSTEM 32, SYSPROTO_CONTROL/AF_SYS_CONTROL 2 = xnu bsd/sys/
 * kern_control.h, sys_domain.h, socket.h. Open sequence: BlueTool rows M17 (socket, CTLIOCGINFO, connect) [bin].
 * No entitlements: the kctl is unprivileged, registering a launchd MachService needs none (spec). Logs: stderr + os_log-style line. */
#include "bt_tool.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef BTT_BUILD
#define BTT_BUILD "dev"
#endif

/* ---- kctl (xnu bsd/sys/kern_control.h) ---- */
struct ctl_info_ {
	uint32_t ctl_id;
	char ctl_name[96];	/* MAX_KCTL_NAME 96 */
};
struct sockaddr_ctl_ {
	uint8_t sc_len;
	uint8_t sc_family;
	uint16_t ss_sysaddr;
	uint32_t sc_id;
	uint32_t sc_unit;
	uint32_t sc_reserved[5];
};
#define CTLIOCGINFO_ _IOWR('N', 3, struct ctl_info_)
#define AF_SYSTEM_ 32
#define SYSPROTO_CONTROL_ 2
#define AF_SYS_CONTROL_ 2

/* ---- libxpc (opaque; resolved from libSystem) ---- */
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
struct _xpc_type_s;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
#define XPC_CONNECTION_MACH_SERVICE_LISTENER_ (1 << 0)
xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
void xpc_connection_set_event_handler(xpc_connection_t, void (^)(xpc_object_t));
void xpc_connection_resume(xpc_connection_t);
void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
xpc_type_t xpc_get_type(xpc_object_t);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
xpc_connection_t xpc_dictionary_get_remote_connection(xpc_object_t);
const char *xpc_dictionary_get_string(xpc_object_t, const char *);
xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
void xpc_release(xpc_object_t);

static void
logline(void *ctx, const char *line)
{
	(void)ctx;
	fprintf(stderr, "%s\n", line);	/* the launchd job's stderr; patch 0048 mirrors it into the host log for names in sweetnull= */
}

/* ---- transport ---- */
static int ctl_fd = -1;

static int
tx_open(void *ctx)
{
	struct ctl_info_ ci;
	struct sockaddr_ctl_ sc;

	(void)ctx;
	ctl_fd = socket(AF_SYSTEM_, SOCK_STREAM, SYSPROTO_CONTROL_);
	if (ctl_fd < 0)
		return -1;
	memset(&ci, 0, sizeof(ci));
	strlcpy(ci.ctl_name, "com.apple.uart.bluetooth", sizeof(ci.ctl_name));
	if (ioctl(ctl_fd, CTLIOCGINFO_, &ci) != 0)
		goto bad;
	memset(&sc, 0, sizeof(sc));
	sc.sc_len = sizeof(sc);
	sc.sc_family = AF_SYSTEM_;
	sc.ss_sysaddr = AF_SYS_CONTROL_;
	sc.sc_id = ci.ctl_id;
	sc.sc_unit = 0;
	if (connect(ctl_fd, (struct sockaddr *)&sc, sizeof(sc)) != 0)
		goto bad;
	return 0;
bad:
	fprintf(stderr, "BT TOOL: kctl open errno %d\n", errno);
	close(ctl_fd);
	ctl_fd = -1;
	return -1;
}

static void
tx_close(void *ctx)
{
	(void)ctx;
	if (ctl_fd >= 0)
		close(ctl_fd);
	ctl_fd = -1;
}

static void
tx_log_termios(void *ctx)
{
	struct termios t;

	(void)ctx;
	/* getsockopt(fd, level 2, TIOCGETA) on the control socket (BlueTool M15/M18); logged, never changed */
	socklen_t l = sizeof(t);

	if (getsockopt(ctl_fd, SYSPROTO_CONTROL_, TIOCGETA, &t, &l) == 0)
		fprintf(stderr, "BT TOOL: termios cflag 0x%lx ispeed %lu\n", (unsigned long)t.c_cflag, (unsigned long)t.c_ispeed);
	else
		fprintf(stderr, "BT GAP: kctl TIOCGETA unsupported (errno %d)\n", errno);
}

static int
tx_write(void *ctx, const uint8_t *b, size_t n)
{
	ssize_t r;

	(void)ctx;
	r = write(ctl_fd, b, n);
	return r < 0 ? -1 : (int)r;
}

static int
tx_read(void *ctx, uint8_t *b, size_t max, unsigned timeout_ms)
{
	fd_set rf;
	struct timeval tv;
	int s;
	ssize_t r;

	(void)ctx;
	FD_ZERO(&rf);
	FD_SET(ctl_fd, &rf);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	s = select(ctl_fd + 1, &rf, NULL, NULL, &tv);
	if (s < 0)
		return errno == EINTR ? 0 : -1;
	if (s == 0)
		return 0;
	r = read(ctl_fd, b, max);
	return r <= 0 ? -1 : (int)r;
}

static unsigned
tx_now(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void tx_sleep(void *ctx, unsigned ms) { (void)ctx; usleep(ms * 1000u); }
static const struct btt_tx_ops txo = { tx_open, tx_close, tx_log_termios, tx_write, tx_read, tx_now, tx_sleep };

/* ---- dictionary layer: ctx = struct { request, reply } ---- */
struct msg {
	xpc_object_t req, rep;
};

static const char *
d_req_str(void *c, const char *k)
{
	return xpc_dictionary_get_string(((struct msg *)c)->req, k);
}

static int
d_req_has_dict(void *c, const char *k)
{
	xpc_object_t v = xpc_dictionary_get_value(((struct msg *)c)->req, k);

	return v && xpc_get_type(v) == &_xpc_type_dictionary;
}

static const char *
d_arg_str(void *c, const char *k)
{
	xpc_object_t a = xpc_dictionary_get_value(((struct msg *)c)->req, "kMsgArgs");

	return (a && xpc_get_type(a) == &_xpc_type_dictionary) ? xpc_dictionary_get_string(a, k) : NULL;
}

static void
d_rep_i64(void *c, const char *k, int64_t v)
{
	xpc_dictionary_set_int64(((struct msg *)c)->rep, k, v);
}

static void
d_rep_str(void *c, const char *k, const char *v)
{
	xpc_dictionary_set_string(((struct msg *)c)->rep, k, v);
}

static const struct btt_dict_ops dop = { d_req_str, d_req_has_dict, d_arg_str, d_rep_i64, d_rep_str };

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;	/* one request at a time: the kernel serves one UART client */
static volatile unsigned last_ms;

static void
serve(xpc_object_t m)
{
	struct msg mg;
	struct btt_env e;
	xpc_connection_t rc;

	mg.req = m;
	mg.rep = xpc_dictionary_create_reply(m);
	if (!mg.rep) {
		fprintf(stderr, "BT GAP: no reply port in the request, nothing sent\n");
		return;
	}
	e.dict = &dop;
	e.dctx = &mg;
	e.tx = &txo;
	e.tctx = NULL;
	e.log = logline;
	e.lctx = NULL;
	pthread_mutex_lock(&big);
	btt_handle(&e);
	last_ms = tx_now(NULL);
	pthread_mutex_unlock(&big);
	rc = xpc_dictionary_get_remote_connection(m);
	if (rc)
		xpc_connection_send_message(rc, mg.rep);
	xpc_release(mg.rep);
}

int
main(void)
{
	dispatch_queue_t q = dispatch_queue_create("pixos-bluetool", DISPATCH_QUEUE_SERIAL);
	xpc_connection_t l;
	dispatch_source_t tm;

	fprintf(stderr, "BT TOOL: pixos-bluetool start build=%s\n", BTT_BUILD);
	last_ms = tx_now(NULL);
	l = xpc_connection_create_mach_service("com.apple.BlueTool", q, XPC_CONNECTION_MACH_SERVICE_LISTENER_);
	if (!l) {
		fprintf(stderr, "BT GAP: xpc_connection_create_mach_service(com.apple.BlueTool) failed\n");
		return 1;
	}
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection)
			return;	/* listener errors: nothing to answer */
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) == &_xpc_type_dictionary)
				serve(m);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	/* exit after 30 s idle (spec); launchd restarts on demand */
	tm = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(tm, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(tm, ^{
		if (pthread_mutex_trylock(&big) == 0) {
			unsigned idle = tx_now(NULL) - last_ms;

			pthread_mutex_unlock(&big);
			if (idle >= 30000u) {
				fprintf(stderr, "BT TOOL: idle 30 s, exit\n");
				exit(0);
			}
		}
	});
	dispatch_resume(tm);
	dispatch_main();
}
