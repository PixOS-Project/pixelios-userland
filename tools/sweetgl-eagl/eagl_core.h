/* eagl_core: the pure C half of the EAGL shim (no ObjC, no Mesa, no IOKit). Host-testable.
 * Holds the context table, per-thread current context, setParameter:to: store and IOSurface attachment bookkeeping;
 * the GL work is delegated to a `struct sweet_gl_backend` (the Mesa side, not written yet).
 * Errors are negative errno values; pointers are NULL on failure with the errno set.
 * Thread safety: one global mutex guards the tables; the current context is thread-local. Backend callbacks run
 * WITH the mutex held (ponytail: backends must not call back into sgl_*; ceiling = a slow glFinish blocks other
 * threads' sgl calls, upgrade to a per-context lock if CA's render threads ever contend). */
#ifndef EAGL_CORE_H
#define EAGL_CORE_H
#include <stddef.h>
#include <stdint.h>

#define SGL_API_ES2 2   /* kEAGLRenderingAPIOpenGLES2 */
#define SGL_API_ES3 3   /* kEAGLRenderingAPIOpenGLES3 */
#define SGL_PARAM_CA_0x260 0x260 /* CA sends setParameter:0x260 to:&0 once [src research 33 s.1.1] */

/* One IOSurface as the shim sees it (filled by the ObjC layer from IOSurfaceGetBaseAddress etc.; `va` is the GPU VA
 * the kernel import returned, 0 is invalid). format is an opaque fourcc/pixel-format id passed through. */
struct sweet_gl_surface {
	uint32_t id;
	uint64_t va, len;
	uint32_t bytes_per_row, w, h, format;
};

/* Backend vtable. All callbacks optional-NULL = -ENOSYS from the matching sgl_ call, except create_context.
 * `user` is passed back verbatim. Return 0 / -errno unless stated. bctx is the backend's own context pointer. */
struct sweet_gl_backend {
	/* Create a GL context. Returns bctx (owned by the core until destroy_context) or NULL; *out_api = the ES major
	 * version the context really provides. Core rejects (and destroys) the context unless *out_api == 3. */
	void *(*create_context)(void *user, int requested_api, int *out_api);
	void (*destroy_context)(void *user, void *bctx);
	/* bctx NULL = unbind on this thread. */
	int (*make_current)(void *user, void *bctx);
	/* Bind `surf` as storage of GL texture `tex` (render target and sampler). surf NULL = detach/forget `tex`.
	 * The surf pointer is only valid during the call: copy what you need. */
	int (*attach_surface)(void *user, void *bctx, uint32_t tex, const struct sweet_gl_surface *surf, int inverted);
	/* Upload/bind `surf` as the image of the currently bound texture (texImageIOSurface:...). Arguments are the GL
	 * enums CA passes (target 0xde1, internalformat 0x1908, format 0x80e1, type 0x1401, plane 0). */
	int (*tex_image_surface)(void *user, void *bctx, const struct sweet_gl_surface *surf, uint32_t target,
	                         uint32_t internalformat, uint32_t w, uint32_t h, uint32_t format, uint32_t type,
	                         uint32_t plane, int invert);
	/* glFlush == glFinish: must return only when the GPU wrote the pixels (research 33 s.3). */
	int (*flush)(void *user, void *bctx);
	void *user;
};

typedef struct sgl_ctx sgl_ctx;

/* Install the backend (copied). Must be called before any other call; replacing it while contexts live is -EBUSY. */
int sgl_set_backend(const struct sweet_gl_backend *be);
/* Destroy every context and clear state (test/teardown). Not safe against concurrent users. */
void sgl_reset(void);

/* initWithAPI:. api must be 2 or 3 (else EINVAL). Returns a context, or NULL: EINVAL bad api, ENODEV no backend,
 * ENOMEM table full (SGL_MAX_CTX) or backend returned NULL, EPROTO backend gave an API other than 3 (the CA rule:
 * ES2 contexts are rejected, so CA falls back to the CPU renderer instead of limping on ES2). */
sgl_ctx *sgl_context_create(int api);
/* dealloc. -EINVAL unknown/destroyed ctx, -EBUSY while current on any thread. 0 on success (ctx pointer then dead). */
int sgl_context_destroy(sgl_ctx *c);
int sgl_context_api(const sgl_ctx *c);          /* -API: 3 for every live context, -EINVAL if not live */
void *sgl_context_backend(const sgl_ctx *c);     /* bctx, NULL if not live */

/* Opaque pointer the ObjC layer parks on a context (the EAGLContext object, for +currentContext). 0 / -EINVAL. */
int sgl_context_set_user(sgl_ctx *c, void *u);
void *sgl_context_user(const sgl_ctx *c);        /* NULL if not live */

/* +setCurrentContext: (c may be NULL = unbind). 0, -EINVAL (dead ctx), -EBUSY (current on another thread),
 * -EIO (backend refused; nothing changes). Switching unbinds the previous context of this thread. */
int sgl_set_current(sgl_ctx *c);
sgl_ctx *sgl_current(void);                      /* +currentContext, thread-local */
/* Make `rctx` (an sgl_ctx *, the rctx word of the GLI table) current on this thread unless it already is (then nothing is called).
 * Failures are ignored (see sgl_set_current). Hook for gli_table: sweet_gli_ensure_current = sgl_ensure_current. */
void sgl_ensure_current(void *rctx);

/* setParameter:to: / store. -EINVAL (bad ctx/NULL out), -ENOSPC (SGL_MAX_PARAMS distinct pnames). Last write wins. */
int sgl_param_set(sgl_ctx *c, uint32_t pname, int32_t value);
int sgl_param_get(sgl_ctx *c, uint32_t pname, int32_t *value);   /* -ENOENT if never set */

/* attachImage:toCoreSurface:invertedRender:. c must be current on the calling thread (-EPERM). surf validated:
 * id!=0, va!=0, w,h,bytes_per_row>0, bytes_per_row*h <= len (-EINVAL). Re-attaching the same `tex` replaces the
 * record (and calls the backend again); tex 0 is -EINVAL. Max SGL_MAX_ATTACH per context (-ENOSPC). The backend
 * is called first; if it fails the record is unchanged. */
int sgl_attach_surface(sgl_ctx *c, uint32_t tex, const struct sweet_gl_surface *surf, int inverted);
/* Forget `tex`'s surface. -ENOENT if none attached. Backend gets surf == NULL. */
int sgl_detach_surface(sgl_ctx *c, uint32_t tex);
/* Which surface does `tex` hold? 0 and *out filled, or -ENOENT. */
int sgl_attachment(sgl_ctx *c, uint32_t tex, struct sweet_gl_surface *out, int *inverted);
int sgl_attach_count(sgl_ctx *c);

/* texImageIOSurface:... Same surface validation and current-context rule as attach; not recorded. */
int sgl_tex_image_surface(sgl_ctx *c, const struct sweet_gl_surface *surf, uint32_t target, uint32_t internalformat,
                          uint32_t w, uint32_t h, uint32_t format, uint32_t type, uint32_t plane, int invert);
/* glFlush/glFinish equivalent; c must be current (-EPERM). */
int sgl_flush(sgl_ctx *c);

#define SGL_MAX_CTX 16
#define SGL_MAX_PARAMS 16
#define SGL_MAX_ATTACH 64
#endif
