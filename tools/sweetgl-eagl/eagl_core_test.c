/* Host test for eagl_core with a fake backend. Prints PASS/FAIL. */
#include "eagl_core.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static struct { int created, destroyed, cur_calls, att_calls, det_calls, tex_calls, flushes, fake_api, fail_attach, fail_cur, null_create;
                void *last_cur; uint32_t last_tex; uint64_t last_va; int last_inv; } F;
static char tok[SGL_MAX_CTX + 2];

static void *f_create(void *u, int req, int *api) { (void)u; (void)req;
	if (F.null_create) return NULL;
	*api = F.fake_api;
	return &tok[F.created++ % (SGL_MAX_CTX + 2)]; }
static void f_destroy(void *u, void *b) { (void)u; (void)b; F.destroyed++; }
static int f_cur(void *u, void *b) { (void)u; F.cur_calls++; if (F.fail_cur) return -1; F.last_cur = b; return 0; }
static int f_att(void *u, void *b, uint32_t tex, const struct sweet_gl_surface *s, int inv) { (void)u; (void)b;
	if (F.fail_attach) return -5;
	if (s) { F.att_calls++; F.last_va = s->va; F.last_inv = inv; } else F.det_calls++;
	F.last_tex = tex; return 0; }
static int f_tex(void *u, void *b, const struct sweet_gl_surface *s, uint32_t t, uint32_t i, uint32_t w, uint32_t h,
                 uint32_t f, uint32_t ty, uint32_t p, int inv) { (void)u; (void)b; (void)s; (void)t; (void)i; (void)w; (void)h; (void)f; (void)ty; (void)p; (void)inv; F.tex_calls++; return 0; }
static int f_flush(void *u, void *b) { (void)u; (void)b; F.flushes++; return 0; }

static const struct sweet_gl_backend FB = { f_create, f_destroy, f_cur, f_att, f_tex, f_flush, NULL };

static void fresh(void) { sgl_reset(); memset(&F, 0, sizeof F); F.fake_api = 3; CHECK(sgl_set_backend(&FB) == 0); }

static struct sweet_gl_surface surf(uint32_t id, uint64_t va)
{ struct sweet_gl_surface s = { id, va, 0x10000, 256 * 4, 64, 64, 0x42475241 }; return s; }

static void *thr_cur(void *arg)
{
	sgl_ctx **p = arg;
	CHECK(sgl_current() == NULL);                 /* thread-local: main's current is invisible here */
	CHECK(sgl_set_current(p[0]) == -EBUSY);       /* main holds it */
	CHECK(sgl_set_current(p[1]) == 0);
	CHECK(sgl_current() == p[1]);
	CHECK(sgl_context_destroy(p[1]) == -EBUSY);
	CHECK(sgl_set_current(NULL) == 0);
	return NULL;
}

int main(void)
{
	struct sweet_gl_surface s, o;
	int32_t v = -1; int inv = 9;

	/* no backend */
	sgl_reset(); errno = 0; CHECK(sgl_context_create(3) == NULL && errno == ENODEV);
	CHECK(sgl_set_backend(NULL) == -EINVAL);

	/* create / api rules */
	fresh();
	errno = 0; CHECK(sgl_context_create(1) == NULL && errno == EINVAL);
	errno = 0; CHECK(sgl_context_create(4) == NULL && errno == EINVAL);
	F.fake_api = 2; errno = 0;
	CHECK(sgl_context_create(3) == NULL && errno == EPROTO);       /* backend only gives ES2: rejected */
	CHECK(F.created == 1 && F.destroyed == 1);                     /* and the backend context is not leaked */
	errno = 0; CHECK(sgl_context_create(2) == NULL && errno == EPROTO);
	F.fake_api = 3; F.null_create = 1; errno = 0; CHECK(sgl_context_create(3) == NULL && errno == ENOMEM);
	F.null_create = 0;
	sgl_ctx *a = sgl_context_create(3), *b = sgl_context_create(2);   /* ES2 request served by an ES3 context */
	CHECK(a && b && a != b);
	CHECK(sgl_context_api(a) == 3 && sgl_context_api(b) == 3);
	CHECK(sgl_context_backend(a) != NULL && sgl_context_backend(a) != sgl_context_backend(b));
	CHECK(sgl_set_backend(&FB) == -EBUSY);
	CHECK(sgl_context_user(a) == NULL && sgl_context_set_user(a, &F) == 0 && sgl_context_user(a) == &F && sgl_context_user(b) == NULL);
	CHECK(sgl_context_set_user(NULL, &F) == -EINVAL);

	/* table full */
	sgl_ctx *many[SGL_MAX_CTX]; int n = 2;
	many[0] = a; many[1] = b;
	for (; n < SGL_MAX_CTX; n++) { many[n] = sgl_context_create(3); CHECK(many[n] != NULL); }
	errno = 0; CHECK(sgl_context_create(3) == NULL && errno == ENOMEM);
	for (int i = 2; i < SGL_MAX_CTX; i++) CHECK(sgl_context_destroy(many[i]) == 0);

	/* params */
	CHECK(sgl_param_get(a, 0x260, &v) == -ENOENT);
	CHECK(sgl_param_set(a, SGL_PARAM_CA_0x260, 0) == 0);
	CHECK(sgl_param_get(a, 0x260, &v) == 0 && v == 0);
	CHECK(sgl_param_set(a, 0x260, 7) == 0 && sgl_param_get(a, 0x260, &v) == 0 && v == 7);   /* last write wins */
	CHECK(sgl_param_get(b, 0x260, &v) == -ENOENT);                                           /* per context */
	CHECK(sgl_param_set(a, 0x300, 1) == 0 && sgl_param_set(a, 0x260, 9) == 0 && sgl_param_get(a, 0x260, &v) == 0 && v == 9);   /* non-last key */
	CHECK(sgl_param_get(a, 0x300, &v) == 0 && v == 1);
	CHECK(sgl_param_get(a, 0x261, &v) == -ENOENT);
	CHECK(sgl_param_get(a, 0x260, NULL) == -EINVAL);
	CHECK(sgl_param_set(NULL, 1, 1) == -EINVAL);
	for (uint32_t p = 0; p < SGL_MAX_PARAMS - 1; p++) CHECK(sgl_param_set(b, 0x1000 + p, (int32_t)p) == 0);
	CHECK(sgl_param_set(b, 0x2000, 1) == 0);
	CHECK(sgl_param_set(b, 0x2001, 1) == -ENOSPC);
	CHECK(sgl_param_set(b, 0x1003, 33) == 0);                      /* existing key still updatable when full */

	/* current context */
	CHECK(sgl_current() == NULL);
	CHECK(sgl_set_current((sgl_ctx *)&F) == -EINVAL);
	CHECK(sgl_set_current(a) == 0 && sgl_current() == a && F.last_cur == sgl_context_backend(a));
	int calls = F.cur_calls; CHECK(sgl_set_current(a) == 0 && F.cur_calls == calls);        /* no redundant backend call */
	CHECK(sgl_context_destroy(a) == -EBUSY);
	{ sgl_ctx *pair[2] = { a, b }; pthread_t t; pthread_create(&t, NULL, thr_cur, pair); pthread_join(t, NULL); }
	CHECK(sgl_current() == a);
	F.fail_cur = 1; CHECK(sgl_set_current(b) == -EIO && sgl_current() == a); F.fail_cur = 0;
	CHECK(sgl_set_current(b) == 0 && sgl_current() == b);
	CHECK(sgl_context_destroy(a) == 0);                              /* a was unbound by the switch */
	CHECK(sgl_context_destroy(a) == -EINVAL && sgl_context_api(a) == -EINVAL);
	CHECK(sgl_set_current(a) == -EINVAL);

	/* surfaces: b is current */
	s = surf(10, 0x100000);
	CHECK(sgl_attach_surface(b, 5, &s, 0) == 0 && sgl_attach_count(b) == 1);
	CHECK(F.att_calls == 1 && F.last_tex == 5 && F.last_va == 0x100000);
	CHECK(sgl_attachment(b, 5, &o, &inv) == 0 && o.id == 10 && o.va == 0x100000 && o.w == 64 && inv == 0);
	s = surf(11, 0x200000);                                          /* replace same tex */
	CHECK(sgl_attach_surface(b, 5, &s, 1) == 0 && sgl_attach_count(b) == 1);
	CHECK(sgl_attachment(b, 5, &o, &inv) == 0 && o.id == 11 && o.va == 0x200000 && inv == 1 && F.last_inv == 1);
	CHECK(sgl_attach_surface(b, 6, &s, 0) == 0 && sgl_attach_count(b) == 2);
	F.fail_attach = 1;                                               /* backend failure leaves the record alone */
	{ struct sweet_gl_surface z = surf(12, 0x300000); CHECK(sgl_attach_surface(b, 5, &z, 0) == -5); }
	F.fail_attach = 0;
	CHECK(sgl_attachment(b, 5, &o, NULL) == 0 && o.id == 11);
	CHECK(sgl_detach_surface(b, 5) == 0 && F.det_calls == 1 && sgl_attach_count(b) == 1);
	CHECK(sgl_attachment(b, 5, &o, NULL) == -ENOENT && sgl_attachment(b, 6, &o, NULL) == 0);
	CHECK(sgl_detach_surface(b, 5) == -ENOENT);
	/* invalid args */
	CHECK(sgl_attach_surface(b, 0, &s, 0) == -EINVAL);
	CHECK(sgl_attach_surface(b, 7, NULL, 0) == -EINVAL);
	{ struct sweet_gl_surface z = surf(0, 0x1000); CHECK(sgl_attach_surface(b, 7, &z, 0) == -EINVAL); }
	{ struct sweet_gl_surface z = surf(1, 0); CHECK(sgl_attach_surface(b, 7, &z, 0) == -EINVAL); }
	{ struct sweet_gl_surface z = surf(1, 1); z.h = 0; CHECK(sgl_attach_surface(b, 7, &z, 0) == -EINVAL); }
	{ struct sweet_gl_surface z = surf(1, 1); z.bytes_per_row = 0; CHECK(sgl_attach_surface(b, 7, &z, 0) == -EINVAL); }
	{ struct sweet_gl_surface z = surf(1, 1); z.len = 1024 * 63; CHECK(sgl_attach_surface(b, 7, &z, 0) == -EINVAL); }   /* pitch*h > len */
	{ struct sweet_gl_surface z = surf(1, 1); z.bytes_per_row = 0xffffffffu; z.h = 0xffffffffu; z.len = 1ull << 40;
	  CHECK(sgl_attach_surface(b, 7, &z, 0) == -EINVAL); }                                                              /* no 32-bit overflow */
	CHECK(sgl_attach_surface(NULL, 7, &s, 0) == -EINVAL);
	CHECK(sgl_attach_count(b) == 1);
	/* not current -> EPERM */
	sgl_ctx *c3 = sgl_context_create(3);
	CHECK(sgl_attach_surface(c3, 7, &s, 0) == -EPERM && sgl_flush(c3) == -EPERM);
	CHECK(sgl_tex_image_surface(c3, &s, 0xde1, 0x1908, 64, 64, 0x80e1, 0x1401, 0, 0) == -EPERM);
	CHECK(sgl_detach_surface(c3, 7) == -EPERM);
	/* attach limit */
	for (uint32_t t = 100; t < 100 + SGL_MAX_ATTACH - 1; t++) CHECK(sgl_attach_surface(b, t, &s, 0) == 0);
	CHECK(sgl_attach_surface(b, 999, &s, 0) == -ENOSPC);
	CHECK(sgl_attach_surface(b, 100, &s, 0) == 0);                   /* replace still fine when full */

	/* tex image + flush */
	CHECK(sgl_tex_image_surface(b, &s, 0xde1, 0x1908, 64, 64, 0x80e1, 0x1401, 0, 0) == 0 && F.tex_calls == 1);
	{ struct sweet_gl_surface z = surf(0, 1); CHECK(sgl_tex_image_surface(b, &z, 0xde1, 0x1908, 64, 64, 0x80e1, 0x1401, 0, 0) == -EINVAL); }
	CHECK(sgl_flush(b) == 0 && F.flushes == 1);

	/* unbind then destroy frees backend ctx */
	CHECK(sgl_set_current(NULL) == 0 && sgl_current() == NULL && F.last_cur == NULL);
	int d = F.destroyed; CHECK(sgl_context_destroy(b) == 0 && F.destroyed == d + 1);
	CHECK(sgl_flush(b) == -EINVAL);
	CHECK(sgl_context_destroy(NULL) == -EINVAL);

	/* missing optional callbacks */
	{ struct sweet_gl_backend nb = { f_create, NULL, NULL, NULL, NULL, NULL, NULL };
	  sgl_reset(); CHECK(sgl_set_backend(&nb) == 0);
	  sgl_ctx *e = sgl_context_create(3); CHECK(e && sgl_set_current(e) == 0);
	  s = surf(1, 1); CHECK(sgl_attach_surface(e, 1, &s, 0) == -ENOSYS && sgl_flush(e) == -ENOSYS);
	  CHECK(sgl_tex_image_surface(e, &s, 0, 0, 1, 1, 0, 0, 0, 0) == -ENOSYS);
	  CHECK(sgl_set_current(NULL) == 0 && sgl_context_destroy(e) == 0); }

	sgl_reset();
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
