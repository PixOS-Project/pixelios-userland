/* dylib_main.m: entry point of libsweetgl.dylib, injected into backboardd (INJECT.md). One constructor: probe the SweetMali user client,
 * install the EAGLContext swizzles, log every step. If anything is missing it logs and returns: backboardd then keeps CoreAnimation's
 * CPU renderer, exactly as without the dylib. MRC (-fno-objc-arc), same flags as eagl_objc.m.
 *
 * Log channels (all three on purpose, the kernel is not reachable from userland except through /dev/null, INJECT.md s.5):
 *   1. write to /dev/null: patch 0048 copies it to the kernel/USB log as "|backboardd| sweetgl: ..." (boot-arg sweetnull includes backboardd by default);
 *   2. append to /var/tmp/sweetgl.log;
 *   3. os_log (subsystem org.pixelios.sweetgl), only readable on a running device.
 * Each line is < 200 bytes: the /dev/null tee takes 256 bytes per write. */
#import <Foundation/Foundation.h>
#import <os/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "eagl_private.h"
#include "sweet_drm_mali.h"

/* 0 = not run / other process, 1 = installed, -errno = refused (read with dlsym in a debugger / lldb-less: also the log) */
__attribute__((visibility("default"))) volatile int sweetgl_state;

static void slog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void slog(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if ((size_t)n > sizeof buf - 2) n = sizeof buf - 2;
	buf[n++] = '\n';
	static os_log_t L;
	if (!L) L = os_log_create("org.pixelios.sweetgl", "inject");
	os_log_error(L, "%{public}.*s", n - 1, buf);
	int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) { (void)!write(fd, buf, (size_t)n); close(fd); }
	fd = open("/var/tmp/sweetgl.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) { (void)!write(fd, buf, (size_t)n); close(fd); }
}

__attribute__((constructor)) static void sweetgl_main(void)
{
	const char *prog = getprogname();
	/* DYLD_INSERT_LIBRARIES is inherited by backboardd's children: act only in backboardd (SWEETGL_FORCE=1: any process, for tests) */
	if (strcmp(prog, "backboardd") != 0 && !getenv("SWEETGL_FORCE")) return;
	if (getenv("SWEETGL_DISABLE")) { slog("sweetgl: SWEETGL_DISABLE set, not installing"); return; }
	slog("sweetgl: loaded in %s pid %d", prog, getpid());

	/* The user client is exclusive (second client = EBUSY) and may appear late (ENODEV): wait up to 2 s, then give up [guess: enough] */
	int fd = -1;
	for (int i = 0; i < 10; i++) {
		fd = sweet_drm_mali_open();
		if (fd >= 0 || errno != ENODEV) break;
		usleep(200000);
	}
	if (fd < 0) {
		int e = errno;
		sweetgl_state = -e;
		slog("sweetgl: SweetMali user client unavailable (errno %d %s): NOT installing, CoreAnimation keeps the CPU renderer", e, strerror(e));
		return;
	}
	close(fd);   /* only a probe: the connection stays open inside sweet_drm_mali, Mesa asks for its own dup later */
	slog("sweetgl: SweetMali user client open");

	int r = sweet_eagl_install();
	sweetgl_state = r == 0 ? 1 : r;
	if (r) slog("sweetgl: EAGLContext swizzles NOT installed (%d %s)", r, strerror(-r));
	else slog("sweetgl: EAGLContext swizzles installed (initWithAPI: now returns Mesa/panfrost contexts)");
}
