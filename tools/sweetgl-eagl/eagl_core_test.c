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
{ struct sweet_gl_surface s = { id, va, 0x10000, 256 * 4, 64, 64, 0x42475241, 0 }; return s; }

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

static void *thr_ensure(void *arg)
{
	int c = F.cur_calls;
	CHECK(sgl_current() == NULL);                  /* held by main */
	CHECK(sgl_set_current(arg) == -EBUSY);          /* plain +setCurrentContext: path never steals */
	CHECK(sgl_ensure_current(arg) == 0);            /* GLI path steals: make_current runs on this thread */
	CHECK(sgl_current() == arg && F.cur_calls == c + 1 && F.last_cur == sgl_context_backend(arg));
	CHECK(sgl_ensure_current(arg) == 0 && F.cur_calls == c + 1);   /* fast path again */
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
	{ int calls = F.att_calls; unsigned long ev = sgl_attach_evictions();   /* full table: the attach still reaches the backend, the oldest record goes */
	  CHECK(sgl_attach_surface(b, 999, &s, 0) == 0 && F.att_calls == calls + 1 && F.last_tex == 999 && sgl_attach_count(b) == SGL_MAX_ATTACH);
	  CHECK(sgl_attach_evictions() == ev + 1 && sgl_attachment(b, 999, &o, NULL) == 0 && sgl_attachment(b, 6, &o, NULL) == -ENOENT);
	  CHECK(sgl_attach_surface(b, 1000, &s, 0) == 0 && sgl_attach_evictions() == ev + 2 && sgl_attachment(b, 100, &o, NULL) == -ENOENT);
	  CHECK(sgl_attach_surface(b, 999, &s, 0) == 0 && sgl_attach_evictions() == ev + 2);   /* a recorded name re-attaches in place */
	  for (uint32_t t = 2000; t < 2000 + 3 * SGL_MAX_ATTACH; t++) CHECK(sgl_attach_surface(b, t, &s, 0) == 0);   /* never refused, any number of names */
	  CHECK(sgl_attach_count(b) == SGL_MAX_ATTACH && sgl_attachment(b, 2000 + 3 * SGL_MAX_ATTACH - 1, &o, NULL) == 0); }
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

	/* sgl_ensure_current: fast path, switch, steal between threads, dead ctx, backend failure */
	fresh();
	{ sgl_ctx *a = sgl_context_create(3), *b = sgl_context_create(3);
	  CHECK(a && b && F.cur_calls == 0);
	  CHECK(sgl_ensure_current(a) == 0 && sgl_current() == a && F.cur_calls == 1 && F.last_cur == sgl_context_backend(a));
	  CHECK(sgl_ensure_current(a) == 0 && F.cur_calls == 1);             /* matching: make_current not called */
	  CHECK(sgl_ensure_current(b) == 0 && sgl_current() == b && F.cur_calls == 2 && F.last_cur == sgl_context_backend(b));
	  CHECK(sgl_context_destroy(a) == 0);                                 /* a was unbound by the switch */
	  { pthread_t t; pthread_create(&t, NULL, thr_ensure, b); pthread_join(t, NULL); }   /* thread B steals b from main */
	  int c = F.cur_calls;
	  CHECK(sgl_ensure_current(b) == 0 && F.cur_calls == c + 1 && sgl_current() == b);   /* steal back: main's tl_cur was stale, no fast path */
	  { pthread_t t; pthread_create(&t, NULL, thr_ensure, b); pthread_join(t, NULL); }   /* stolen again */
	  CHECK(sgl_current() == NULL);                                       /* main's current went stale and is revalidated away */
	  c = F.cur_calls;
	  CHECK(sgl_ensure_current(b) == 0 && F.cur_calls == c + 1 && sgl_current() == b);
	  CHECK(sgl_ensure_current(b) == 0 && F.cur_calls == c + 1);
	  CHECK(sgl_ensure_current(a) == -EINVAL && sgl_current() == b);      /* dead ctx */
	  F.fail_cur = 1; CHECK(sgl_ensure_current(NULL) == -EIO && sgl_current() == b); F.fail_cur = 0;
	  CHECK(sgl_ensure_current(NULL) == 0 && sgl_current() == NULL);      /* NULL rctx = unbind */
	  CHECK(sgl_context_destroy(b) == 0); }

	{ /* format-0 diagnostics (research 86 sections E/F/H, DRAFT-format0-batch): exact line formats and the attach/sample pairing */
	  char b[160];
	  sgl_fmt0_reset();
	  sgl_fmt0_attach_line(b, sizeof b, 12, 97, 76, 400, 3, 0x8058);
	  CHECK(!strcmp(b, "SWEETGL: fmt0 attach id 12 97x76 pitch 400 pipe_fmt 3 ifmt 0x8058"));
	  sgl_fmt0_sample_line(b, sizeof b, 12, 97, 76, 0x8058, 0x1908, 0x1401, 1);
	  CHECK(!strcmp(b, "SWEETGL: fmt0 sample id 12 97x76 ifmt 0x8058 fmt 0x1908 type 0x1401 attached 1"));
	  CHECK(!sgl_fmt0_was_attached(12) && sgl_fmt0_note_attach(12) == 1 && sgl_fmt0_was_attached(12) && sgl_fmt0_note_attach(12) == 0);
	  CHECK(sgl_fmt0_sample_count(12) == 0 && sgl_fmt0_sample_count(12) == 1 && sgl_fmt0_sample_count(12) >= 2 && sgl_fmt0_sample_count(13) == 0);
	  for (uint32_t i = 100; i < 164; i++) sgl_fmt0_note_attach(i);
	  CHECK(!sgl_fmt0_was_attached(163));   /* the table holds 64 ids (12 + 63 more); the 65th is dropped */
	  CHECK(sgl_fmt0_was_attached(162));
	  sgl_fmt0_reset(); CHECK(!sgl_fmt0_was_attached(12));
	  {  /* review format0-a1a3 F4: sample table full (128 ids) reports "already logged twice"; the per-id counter never wraps back below 2 */
	     int ok = 1;
	     for (uint32_t i = 0; i < 128; i++) ok &= sgl_fmt0_sample_count(1000 + i) == 0;
	     CHECK(ok);
	     CHECK(sgl_fmt0_sample_count(5000) >= 2 && sgl_fmt0_sample_count(5000) >= 2);
	     sgl_fmt0_reset();
	     ok = 1;
	     for (uint32_t i = 0; i < 300; i++) { int c = sgl_fmt0_sample_count(500); ok &= (i < 2) ? c == (int)i : c >= 2; }
	     CHECK(ok);
	  }
	  sgl_fmt0_reset(); }

	/* IOSurface texture-path failure lines (defect 1, 2026-11-11): the formats/sizes CA uses ('w30r' landscape, 'b3a8' display, 'BGRA'), every path named */
	{ char l[256];
	  sgl_sf_reset();
	  CHECK(sgl_sf_note(SGL_SF_IMPORT) == 1 && sgl_sf_note(SGL_SF_IMPORT) == 2 && sgl_sf_count(SGL_SF_IMPORT) == 2);
	  CHECK(sgl_sf_note(-1) == 0 && sgl_sf_note(SGL_SF_N) == 0 && sgl_sf_count(SGL_SF_N) == 0);
	  sgl_sf_line(l, sizeof l, "teximage", SGL_SF_IMPORT, 412, 0x77333072u, 2436, 1125, 9744, -2, 31, 2);
	  CHECK(!strcmp(l, "SWEETGL: SURFFAIL teximage import id 412 fourcc 'w30r' (0x77333072) 2436x1125 bpr 9744 err -2 bound 31 n 2"));
	  sgl_sf_line(l, sizeof l, "teximage", SGL_SF_NOSTORAGE, 9, 0x62336138u, 1125, 2436, 4544, 0x506, 174, 1);
	  CHECK(!strcmp(l, "SWEETGL: SURFFAIL teximage nostorage id 9 fourcc 'b3a8' (0x62336138) 1125x2436 bpr 4544 err 1286 bound 174 n 1"));
	  sgl_sf_line(l, sizeof l, "attach", SGL_SF_TEX0, 7, 0x42475241u, 1125, 2436, 4544, 0, 0, 1);
	  CHECK(!strcmp(l, "SWEETGL: SURFFAIL attach tex0 id 7 fourcc 'BGRA' (0x42475241) 1125x2436 bpr 4544 err 0 bound 0 n 1"));
	  sgl_sf_line(l, sizeof l, NULL, 99, 0, 0x00002701u, 0, 0, 0, -22, 0, 0);   /* format 0 / odd bytes: dots, unknown path '?' */
	  CHECK(!strcmp(l, "SWEETGL: SURFFAIL ? ? id 0 fourcc '....' (0x00002701) 0x0 bpr 0 err -22 bound 0 n 0"));
	  CHECK(sgl_sf_should_print(1) && sgl_sf_should_print(2000) && !sgl_sf_should_print(2001) && sgl_sf_should_print(3000) && !sgl_sf_should_print(0));
	  for (int p = 0; p < SGL_SF_N; p++) CHECK(strcmp(sgl_sf_path_name(p), "?") != 0);
	  sgl_sf_stat_line(l, sizeof l, 10, 7);
	  CHECK(!strcmp(l, "SWEETGL: SURFSTAT calls 10 ok 7 noctx 0 current 0 nosurf 0 import 2 lock 0 core 0 nostorage 0 tex0 0 target 0 fallback 0 fbfail 0"));
	  { char tiny[8]; sgl_sf_stat_line(tiny, sizeof tiny, 1, 1); CHECK(strlen(tiny) == 7); }   /* truncation stays inside the buffer */
	  sgl_sf_reset(); CHECK(sgl_sf_count(SGL_SF_IMPORT) == 0); }

	sgl_reset();
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
