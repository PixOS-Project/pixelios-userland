/* eagl_objc.m: ObjC layer of the EAGL shim for backboardd (QuartzCore's compositor). It replaces the IMPs of the private EAGLContext
 * methods CoreAnimation sends (research 33 s.2.1) with functions that forward to eagl_core (context table, current context, parameter
 * store, surface bookkeeping) and hand out the GLI table of gli_table.c from -getMacroContextPrivate.
 *
 * Build: MRC (-fno-objc-arc): `xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -fno-objc-arc -Wall -Wextra -Werror -c`.
 * Reason: the replaced IMPs are C functions on raw `id` and must keep Apple's init/dealloc contract by hand (no ivar of ours exists,
 * Apple's -dealloc must never run on these objects, so dealloc calls NSObject's through objc_msgSendSuper). ARC would add retain/release
 * around those calls and forbids the explicit super-dealloc call; MRC keeps the code a plain list of C functions. 
 *
 * Every EAGLContext in the process is ours: -initWithAPI:properties: never calls Apple's init (Apple's loader needs a GL driver bundle that
 * does not exist, R33 s.1.5), so no object has Apple's ivars and none of Apple's other methods may run on it. A failed init releases self.
 *
 * Signatures are [guess] (eagl_private.h); research 33 left them as selectors only. Still to reverse with tools/rea: see report/README. */
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "eagl_private.h"
#include "eagl_core.h"
#include "eagl_mesa.h"
#include "gli_table.h"

/* Kernel side of the IOSurface import (SweetMali selector 5, xnu patch 0105): fills *va (GPU VA) for an IOSurface id. Defined in
 * ../sweetgl-mali/sweet_drm_mali.c; -ENOSYS/-ENOENT/-ENODEV when the kernel refuses (CA then keeps drawing with the CPU). */
extern int sweet_gl_kernel_import(uint32_t surface_id, uint64_t *va);

struct ctxbox {
	sgl_ctx *c;
	struct sweet_gli gli;   /* what -getMacroContextPrivate returns */
};

static const void *kBox = &kBox;

static struct ctxbox *box_of(id self)
{
	NSValue *v = objc_getAssociatedObject(self, kBox);
	return v ? (struct ctxbox *)[v pointerValue] : NULL;
}
static sgl_ctx *C(id self) { struct ctxbox *b = box_of(self); return b ? b->c : NULL; }

static int import_surface(IOSurfaceRef s, struct sweet_gl_surface *out)
{
	uint64_t va = 0;
	if (!s) return -EINVAL;
	out->id = IOSurfaceGetID(s);
	int r = sweet_gl_kernel_import(out->id, &va);
	if (r) return r;   /* the caller logs it (sf_fail SGL_SF_IMPORT): before 2026-11-11 this return was silent in both callers */
	out->va = va;
	out->len = IOSurfaceGetAllocSize(s);
	out->bytes_per_row = (uint32_t)IOSurfaceGetBytesPerRow(s);
	out->w = (uint32_t)IOSurfaceGetWidth(s);
	out->h = (uint32_t)IOSurfaceGetHeight(s);
	out->format = IOSurfaceGetPixelFormat(s);
	out->cpu = NULL;   /* set by m_tex after IOSurfaceLock */
	return 0;
}

/* ---- failure accounting for the two IOSurface texture paths (sprint GPU 2026-11-11 defect 1, hard rule 7) ----
 * Every return of m_tex / m_attach that leaves the texture without the surface logs: a counted GAP (key without numbers) and a detail line with
 * the surface id, fourcc, size, pitch, errno and the bound 2D name (first 2000 per path, then every 1000th; the count is in the line).
 * SWEETGL_TEXFB (default on, =0 off): after a failure, a bound texture still without level-0 storage gets w x h RGBA8 storage, so the FBO CA
 * attaches it to is complete (CA never reads the result of texImageIOSurface, research 135 A.2); the surface itself is not attached then. */
static unsigned long sf_calls, sf_ok;
static int texfb = -1;
static int texfb_on(void)
{
	if (texfb < 0) { const char *e = getenv("SWEETGL_TEXFB"); texfb = !(e && e[0] == '0'); fprintf(stderr, "sweetgl: TEXFB %s (SWEETGL_TEXFB)\n", texfb ? "on" : "off"); }
	return texfb;
}
static void sf_stat(int force)
{
	unsigned long n = __atomic_load_n(&sf_calls, __ATOMIC_RELAXED);
	if (force || (n & 1023) == 0) { char l[320]; sgl_sf_stat_line(l, sizeof l, n, __atomic_load_n(&sf_ok, __ATOMIC_RELAXED)); fprintf(stderr, "%s attach_evict %lu\n", l, sgl_attach_evictions()); }
}
static void sf_fail(const char *api, int path, IOSurfaceRef s, int err, unsigned bound)
{
	unsigned long n = sgl_sf_note(path);
	char l[256];
	sweet_gap("surface", "%s %s", api, sgl_sf_path_name(path));
	if (!sgl_sf_should_print(n)) return;
	sgl_sf_line(l, sizeof l, api, path, s ? IOSurfaceGetID(s) : 0, s ? (uint32_t)IOSurfaceGetPixelFormat(s) : 0, s ? (uint32_t)IOSurfaceGetWidth(s) : 0,
	            s ? (uint32_t)IOSurfaceGetHeight(s) : 0, s ? (uint32_t)IOSurfaceGetBytesPerRow(s) : 0, err, bound, n);
	fprintf(stderr, "%s\n", l);
	if (n == 1) sf_stat(1);
}
/* After a failure: the bound texture keeps its old storage if it has one; a storage-less one gets the fallback (TEXFB). */
static void sf_fallback(const char *api, IOSurfaceRef s, uint32_t w, uint32_t h)
{
	int w0 = 0, h0 = 0;
	unsigned bound = sweet_gli_bound_level0(&w0, &h0);
	if (!texfb_on() || !bound || w0 > 0) return;
	unsigned e = sweet_gli_fallback_storage(w, h);
	sf_fail(api, e ? SGL_SF_FBFAIL : SGL_SF_FALLBACK, s, (int)e, bound);
}
/* After a success: the bound texture must have level-0 storage now (GL errors inside the EGLImage bind are raised in CA's context, not returned). */
static BOOL sf_check(const char *api, IOSurfaceRef s, uint32_t w, uint32_t h)
{
	int w0 = 0, h0 = 0;
	unsigned bound = sweet_gli_bound_level0(&w0, &h0);
	if (bound && w0 > 0 && h0 > 0) { __atomic_add_fetch(&sf_ok, 1, __ATOMIC_RELAXED); return YES; }
	sf_fail(api, bound ? SGL_SF_NOSTORAGE : SGL_SF_TEX0, s, 0, bound);
	if (bound) sf_fallback(api, s, w, h);
	return NO;
}

/* -initWithAPI:properties: (properties = {Accelerated, ClientRetainRelease}, ignored). nil unless the core yields an API 3 context:
 * CA then falls back to its CPU renderer instead of limping on ES2 (R33 s.1.1). */
static id m_init(id self, SEL _cmd, NSUInteger api, id props)
{
	(void)_cmd; (void)props;
	sgl_ctx *c = sgl_context_create((int)api);
	struct ctxbox *b = c ? calloc(1, sizeof *b) : NULL;
	if (!b) {
		if (c) sgl_context_destroy(c);
		[self release];
		return nil;
	}
	b->c = c;
	sgl_context_set_user(c, self);   /* unretained: CA holds the EAGLContext */
	sweet_gli_init(&b->gli, c);       /* rctx = the sgl_ctx: the wrappers hand it to sgl_ensure_current */
	NSValue *v = [NSValue valueWithPointer:b];
	objc_setAssociatedObject(self, kBox, v, OBJC_ASSOCIATION_RETAIN);
	return self;
}

static void m_dealloc(id self, SEL _cmd)
{
	struct ctxbox *b = box_of(self);
	if (b) {
		sgl_ctx *c = b->c;
		if (sgl_current() == c) sgl_set_current(NULL);
		if (sgl_context_destroy(c) == 0)   /* -EBUSY (current on another thread): leak the box rather than free live state */
			free(b);
	}
	struct objc_super sup = { self, class_getSuperclass(objc_getClass("EAGLContext")) };
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);   /* NSObject's dealloc, never Apple's */
}

static NSUInteger m_api(id self, SEL _cmd) { (void)_cmd; sgl_ctx *c = C(self); return c ? (NSUInteger)sgl_context_api(c) : 0; }

/* CA sends setParameter:0x260 to:&0 once and ignores the result (R33 s.2.1). Apple returns a uint32 status, 0 = success
 * [obs-RE research 34]; the failure value is 0x2710 as Apple's "not initialised" case (CA never looks). */
static uint32_t m_set_param(id self, SEL _cmd, unsigned pname, const int *v)
{
	(void)_cmd;
	return v && C(self) && sgl_param_set(C(self), pname, *v) == 0 ? 0 : 0x2710;
}

static void *m_macro(id self, SEL _cmd) { (void)_cmd; struct ctxbox *b = box_of(self); return b ? &b->gli : NULL; }

/* The first argument is the GL target; the texture is the one CA just bound (make_buffer_current: glGenTextures, glBindTexture, then
 * attachImage, R33 s.2.1). Only GL_TEXTURE_2D (0xde1) is accepted. Apple also takes 0x8d41 (GL_RENDERBUFFER) [obs-RE research 34]; CA's
 * server path never sends it and it is NOT a one-line change here (the backend attaches textures, a renderbuffer needs
 * GL_RENDERBUFFER_BINDING and renderbuffer storage from the image), so it is refused. CA does not send +setCurrentContext:, so make
 * the context current first (the core requires it, and the binding query needs it). */
static BOOL m_attach(id self, SEL _cmd, NSUInteger target, IOSurfaceRef s, BOOL inverted)
{
	(void)_cmd;
	struct sweet_gl_surface ss;
	sgl_ctx *c = C(self);
	__atomic_add_fetch(&sf_calls, 1, __ATOMIC_RELAXED);
	sf_stat(0);
	if (!c) { sf_fail("attach", SGL_SF_NOCTX, s, -EINVAL, 0); return NO; }
	int r = sgl_ensure_current(c);
	if (r) { sf_fail("attach", SGL_SF_CURRENT, s, r, 0); return NO; }
	unsigned tex = sweet_gli_bound_texture_2d();
	if (target != 0xde1) { sf_fail("attach", SGL_SF_TARGET, s, (int)target, tex); return NO; }
	if (!s) { sf_fail("attach", SGL_SF_NOSURF, s, -EINVAL, tex); return NO; }
	if (tex == 0) { sf_fail("attach", SGL_SF_TEX0, s, 0, 0); return NO; }
	r = import_surface(s, &ss);
	if (r) { sf_fail("attach", SGL_SF_IMPORT, s, r, tex); return NO; }
	r = sgl_attach_surface(c, tex, &ss, inverted);
	if (r) { sf_fail("attach", SGL_SF_CORE, s, r, tex); return NO; }
	return sf_check("attach", s, ss.w, ss.h);
}

static BOOL m_tex(id self, SEL _cmd, IOSurfaceRef s, NSUInteger target, NSUInteger ifmt, uint32_t w, uint32_t h, NSUInteger fmt,
                  NSUInteger type, uint32_t plane, BOOL invert)
{
	(void)_cmd;
	struct sweet_gl_surface ss;
	sgl_ctx *c = C(self);
	__atomic_add_fetch(&sf_calls, 1, __ATOMIC_RELAXED);
	sf_stat(0);
	if (!c) { sf_fail("teximage", SGL_SF_NOCTX, s, -EINVAL, 0); return NO; }
	int r = sgl_ensure_current(c);
	if (r) { sf_fail("teximage", SGL_SF_CURRENT, s, r, 0); return NO; }
	if (!s) { sf_fail("teximage", SGL_SF_NOSURF, s, -EINVAL, sweet_gli_bound_texture_2d()); sf_fallback("teximage", s, w, h); return NO; }
	r = import_surface(s, &ss);
	if (r) { sf_fail("teximage", SGL_SF_IMPORT, s, r, sweet_gli_bound_texture_2d()); sf_fallback("teximage", s, w, h); return NO; }
	BOOL locked = IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL) == 0;   /* the copy path reads the CPU mapping during the call (cpu set after the lock, review F3) */
	if (!locked) sf_fail("teximage", SGL_SF_LOCK, s, 0, sweet_gli_bound_texture_2d());
	ss.cpu = locked ? IOSurfaceGetBaseAddress(s) : NULL;
	r = sgl_tex_image_surface(c, &ss, (uint32_t)target, (uint32_t)ifmt, w, h, (uint32_t)fmt, (uint32_t)type, plane, invert);
	if (locked) IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	if (r) { sf_fail("teximage", SGL_SF_CORE, s, r, sweet_gli_bound_texture_2d()); sf_fallback("teximage", s, w, h); return NO; }
	return sf_check("teximage", s, w, h);
}

static BOOL m_set_current(id cls, SEL _cmd, id ctx)
{
	(void)cls; (void)_cmd;
	if (!ctx) return sgl_set_current(NULL) == 0;
	return C(ctx) && sgl_set_current(C(ctx)) == 0;
}
static id m_current(id cls, SEL _cmd) { (void)cls; (void)_cmd; return (id)sgl_context_user(sgl_current()); }

static void swap(Class c, SEL sel, void *imp)
{
	Method m = class_getInstanceMethod(c, sel);
	if (m) method_setImplementation(m, (IMP)imp);
	else NSLog(@"sweetgl: EAGLContext has no %s", sel_getName(sel));
}

/* Install the swizzles. Called by dylib_main.m's constructor (not a constructor itself, so the loader can log and refuse first).
 * 0 = installed; -ENOENT: no EAGLContext class (OpenGLES not loaded); -EBUSY/-EINVAL: backend refused. Nothing is half-installed on failure. */
int sweet_eagl_install(void)
{
	Class cls = objc_getClass("EAGLContext");
	if (!cls) return -ENOENT;
	Class meta = object_getClass((id)cls);
	int r = sgl_set_backend(sweet_gl_mesa_backend());
	if (r != 0) return r < 0 ? r : -EINVAL;
	sweet_gli_ensure_current = sgl_ensure_current;   /* CA's server path never sets the current context itself */
	swap(cls, @selector(initWithAPI:properties:), m_init);
	swap(cls, @selector(dealloc), m_dealloc);
	swap(cls, @selector(API), m_api);
	swap(cls, @selector(setParameter:to:), m_set_param);
	swap(cls, @selector(getMacroContextPrivate), m_macro);
	swap(cls, @selector(attachImage:toCoreSurface:invertedRender:), m_attach);
	swap(cls, @selector(texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:), m_tex);
	swap(meta, @selector(setCurrentContext:), m_set_current);
	swap(meta, @selector(currentContext), m_current);
	return 0;
}
