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
#include <errno.h>
#include "eagl_private.h"
#include "eagl_core.h"
#include "eagl_mesa.h"
#include "gli_table.h"

/* Kernel side of the IOSurface import (SweetMali selector 5, NOT in the kernel yet): fills *va (GPU VA) for an IOSurface id. Weak so this
 * file links without it; then attach/tex fail with ENOSYS and CA keeps drawing with the CPU into the surface. */
extern int sweet_gl_kernel_import(uint32_t surface_id, uint64_t *va) __attribute__((weak_import));

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
	if (!sweet_gl_kernel_import) return -ENOSYS;
	out->id = IOSurfaceGetID(s);
	int r = sweet_gl_kernel_import(out->id, &va);
	if (r) return r;
	out->va = va;
	out->len = IOSurfaceGetAllocSize(s);
	out->bytes_per_row = (uint32_t)IOSurfaceGetBytesPerRow(s);
	out->w = (uint32_t)IOSurfaceGetWidth(s);
	out->h = (uint32_t)IOSurfaceGetHeight(s);
	out->format = IOSurfaceGetPixelFormat(s);
	return 0;
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
	sweet_gli_init(&b->gli, b);       /* rctx = the box: unused by the wrappers, handy in a debugger */
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

/* CA sends setParameter:0x260 to:&0 once and ignores the result (R33 s.2.1). */
static BOOL m_set_param(id self, SEL _cmd, unsigned pname, const int *v)
{
	(void)_cmd;
	return v && C(self) && sgl_param_set(C(self), pname, *v) == 0;
}

static void *m_macro(id self, SEL _cmd) { (void)_cmd; struct ctxbox *b = box_of(self); return b ? &b->gli : NULL; }

/* The first argument is the GL target; the texture is the one CA just bound (make_buffer_current: glGenTextures, glBindTexture, then
 * attachImage, R33 s.2.1). Only GL_TEXTURE_2D (0xde1) is accepted. */
static BOOL m_attach(id self, SEL _cmd, NSUInteger target, IOSurfaceRef s, BOOL inverted)
{
	(void)_cmd;
	struct sweet_gl_surface ss;
	sgl_ctx *c = C(self);
	unsigned tex = c ? sweet_gli_bound_texture_2d() : 0;
	return c && target == 0xde1 && tex != 0 && import_surface(s, &ss) == 0 && sgl_attach_surface(c, tex, &ss, inverted) == 0;
}

static BOOL m_tex(id self, SEL _cmd, IOSurfaceRef s, NSUInteger target, NSUInteger ifmt, uint32_t w, uint32_t h, NSUInteger fmt,
                  NSUInteger type, uint32_t plane, BOOL invert)
{
	(void)_cmd;
	struct sweet_gl_surface ss;
	sgl_ctx *c = C(self);
	return c && import_surface(s, &ss) == 0 &&
	       sgl_tex_image_surface(c, &ss, (uint32_t)target, (uint32_t)ifmt, w, h, (uint32_t)fmt, (uint32_t)type, plane, invert) == 0;
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

/* Runs at dylib load (sweetenv-injected into backboardd). */
__attribute__((constructor)) static void sweet_eagl_install(void)
{
	Class cls = objc_getClass("EAGLContext");
	if (!cls) return;
	Class meta = object_getClass((id)cls);
	if (sgl_set_backend(sweet_gl_mesa_backend()) != 0) return;
	swap(cls, @selector(initWithAPI:properties:), m_init);
	swap(cls, @selector(dealloc), m_dealloc);
	swap(cls, @selector(API), m_api);
	swap(cls, @selector(setParameter:to:), m_set_param);
	swap(cls, @selector(getMacroContextPrivate), m_macro);
	swap(cls, @selector(attachImage:toCoreSurface:invertedRender:), m_attach);
	swap(cls, @selector(texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:), m_tex);
	swap(meta, @selector(setCurrentContext:), m_set_current);
	swap(meta, @selector(currentContext), m_current);
}
