/* eagl_objc.m: ObjC shim SKETCH. UNBUILT and UNRUN: it needs the iOS SDK (objc runtime, IOSurface headers), which this host
 * does not have; it compiles only on the macOS CI. Method signatures marked [guess] are not checked against OpenGLES.framework
 * (rule 7: reverse them with tools/rea before this is built). Selectors and call order: research 33 s.2.1 [src].
 * Everything stateful lives in eagl_core.c; the Mesa-facing struct sweet_gl_backend (eagl_mesa.c) is NOT written yet. */
#import <objc/runtime.h>
#import <IOSurface/IOSurfaceRef.h>
#include "eagl_core.h"

extern const struct sweet_gl_backend *sweet_gl_mesa_backend(void);  /* TODO eagl_mesa.c */
extern int sweet_gl_import_surface(IOSurfaceRef s, struct sweet_gl_surface *out); /* TODO: id/va/len/pitch/w/h/format, kernel selector 5 */

static const void *kCtx = &kCtx;

static sgl_ctx *C(id self) { return (sgl_ctx *)[objc_getAssociatedObject(self, kCtx) pointerValue]; }

/* -initWithAPI:properties: returns nil unless the core gives an API 3 context (CA then keeps its CPU renderer). */
static id m_init(id self, SEL _cmd, unsigned api, id props)
{
	(void)_cmd; (void)props;
	sgl_ctx *c = sgl_context_create((int)api);
	if (!c) return nil;
	sgl_context_set_user(c, (__bridge void *)self);   /* unretained: CA holds the EAGLContext */
	objc_setAssociatedObject(self, kCtx, [NSValue valueWithPointer:c], OBJC_ASSOCIATION_RETAIN);
	return self;
}
static void m_dealloc(id self, SEL _cmd) { (void)_cmd; sgl_ctx *c = C(self); if (c) { sgl_set_current(NULL); sgl_context_destroy(c); } }
static unsigned m_api(id self, SEL _cmd) { (void)_cmd; return (unsigned)sgl_context_api(C(self)); }
static BOOL m_set_param(id self, SEL _cmd, unsigned pname, int *v) { (void)_cmd; return v && sgl_param_set(C(self), pname, *v) == 0; }
/* [guess] CA stores the returned pointer and calls through {rctx, disp[1036]} (research 33 s.2.1): the GLI table is not designed here. */
static void *m_macro(id self, SEL _cmd) { (void)self; (void)_cmd; return NULL; /* TODO gli table */ }
static BOOL m_attach(id self, SEL _cmd, unsigned tex, IOSurfaceRef s, BOOL inv) /* [guess] BOOL return */
{
	(void)_cmd; struct sweet_gl_surface ss;
	return s && sweet_gl_import_surface(s, &ss) == 0 && sgl_attach_surface(C(self), tex, &ss, inv) == 0;
}
static BOOL m_tex(id self, SEL _cmd, IOSurfaceRef s, unsigned target, unsigned ifmt, unsigned w, unsigned h, unsigned fmt,
                  unsigned type, unsigned plane, BOOL inv) /* [guess] BOOL return, arg types */
{
	(void)_cmd; struct sweet_gl_surface ss;
	return s && sweet_gl_import_surface(s, &ss) == 0 &&
	       sgl_tex_image_surface(C(self), &ss, target, ifmt, w, h, fmt, type, plane, inv) == 0;
}
static void m_set_current(id self, SEL _cmd, id ctx) { (void)self; (void)_cmd; sgl_set_current(ctx ? C(ctx) : NULL); }
static id m_current(id self, SEL _cmd) { (void)self; (void)_cmd; return (__bridge id)sgl_context_user(sgl_current()); }

static void swap(Class c, const char *sel, void *imp) { Method m = class_getInstanceMethod(c, sel_registerName(sel)); if (m) method_setImplementation(m, (IMP)imp); }

/* Call once at dylib load (sweetenv-injected into backboardd). */
__attribute__((constructor)) static void sweet_eagl_install(void)
{
	Class cls = objc_getClass("EAGLContext"), meta = object_getClass((id)cls);
	if (!cls || sgl_set_backend(sweet_gl_mesa_backend()) != 0) return;
	swap(cls, "initWithAPI:properties:", m_init);
	swap(cls, "dealloc", m_dealloc);
	swap(cls, "API", m_api);
	swap(cls, "setParameter:to:", m_set_param);
	swap(cls, "getMacroContextPrivate", m_macro);
	swap(cls, "attachImage:toCoreSurface:invertedRender:", m_attach);
	swap(cls, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:", m_tex);
	swap(meta, "setCurrentContext:", m_set_current);
	swap(meta, "currentContext", m_current);
}
