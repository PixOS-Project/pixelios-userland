/* eagl_core.c: see eagl_core.h. Plain C11, POSIX threads only. */
#include "eagl_core.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct attach { uint32_t tex; int inverted; struct sweet_gl_surface s; };
struct param { uint32_t pname; int32_t value; };

struct sgl_ctx {
	int live, api;
	void *user;
	void *bctx;
	int bound;            /* current on some thread */
	pthread_t owner;
	struct param params[SGL_MAX_PARAMS]; int nparams;
	struct attach att[SGL_MAX_ATTACH]; int natt;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct sweet_gl_backend be;
static int have_be;
static sgl_ctx table[SGL_MAX_CTX];
static _Thread_local sgl_ctx *tl_cur;

static int is_live(const sgl_ctx *c) { return c && c >= table && c < table + SGL_MAX_CTX && c->live; }

int sgl_set_backend(const struct sweet_gl_backend *b)
{
	if (!b || !b->create_context) return -EINVAL;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < SGL_MAX_CTX; i++) if (table[i].live) { pthread_mutex_unlock(&mu); return -EBUSY; }
	be = *b; have_be = 1;
	pthread_mutex_unlock(&mu);
	return 0;
}

void sgl_reset(void)
{
	pthread_mutex_lock(&mu);
	for (int i = 0; i < SGL_MAX_CTX; i++) {
		if (table[i].live && be.destroy_context) be.destroy_context(be.user, table[i].bctx);
		memset(&table[i], 0, sizeof table[i]);
	}
	memset(&be, 0, sizeof be); have_be = 0;
	tl_cur = NULL;
	pthread_mutex_unlock(&mu);
}

sgl_ctx *sgl_context_create(int api)
{
	if (api != SGL_API_ES2 && api != SGL_API_ES3) { errno = EINVAL; return NULL; }
	pthread_mutex_lock(&mu);
	if (!have_be) { pthread_mutex_unlock(&mu); errno = ENODEV; return NULL; }
	sgl_ctx *c = NULL;
	for (int i = 0; i < SGL_MAX_CTX && !c; i++) if (!table[i].live) c = &table[i];
	if (!c) { pthread_mutex_unlock(&mu); errno = ENOMEM; return NULL; }
	int got = 0;
	void *b = be.create_context(be.user, api, &got);
	if (!b) { pthread_mutex_unlock(&mu); errno = ENOMEM; return NULL; }
	if (got != SGL_API_ES3) {
		if (be.destroy_context) be.destroy_context(be.user, b);
		pthread_mutex_unlock(&mu); errno = EPROTO; return NULL;
	}
	memset(c, 0, sizeof *c);
	c->live = 1; c->api = got; c->bctx = b;
	pthread_mutex_unlock(&mu);
	return c;
}

int sgl_context_destroy(sgl_ctx *c)
{
	pthread_mutex_lock(&mu);
	int r = 0;
	if (!is_live(c)) r = -EINVAL;
	else if (c->bound) r = -EBUSY;
	else {
		if (be.destroy_context) be.destroy_context(be.user, c->bctx);
		memset(c, 0, sizeof *c);
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_context_api(const sgl_ctx *c) { return is_live(c) ? c->api : -EINVAL; }
void *sgl_context_backend(const sgl_ctx *c) { return is_live(c) ? c->bctx : NULL; }
int sgl_context_set_user(sgl_ctx *c, void *u)
{
	pthread_mutex_lock(&mu);
	int r = is_live(c) ? (c->user = u, 0) : -EINVAL;
	pthread_mutex_unlock(&mu);
	return r;
}
void *sgl_context_user(const sgl_ctx *c) { return is_live(c) ? c->user : NULL; }
sgl_ctx *sgl_current(void) { return tl_cur; }

int sgl_set_current(sgl_ctx *c)
{
	pthread_mutex_lock(&mu);
	int r = 0;
	if (c && !is_live(c)) r = -EINVAL;
	else if (c && c->bound && !pthread_equal(c->owner, pthread_self())) r = -EBUSY;
	else if (c == tl_cur) r = 0;
	else if (be.make_current && be.make_current(be.user, c ? c->bctx : NULL) != 0) r = -EIO;
	else {
		if (tl_cur) tl_cur->bound = 0;
		tl_cur = c;
		if (c) { c->bound = 1; c->owner = pthread_self(); }
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_param_set(sgl_ctx *c, uint32_t pname, int32_t value)
{
	pthread_mutex_lock(&mu);
	int r = 0;
	if (!is_live(c)) r = -EINVAL;
	else {
		int i;
		for (i = 0; i < c->nparams && c->params[i].pname != pname; i++) ;
		if (i == c->nparams) { if (i == SGL_MAX_PARAMS) r = -ENOSPC; else { c->nparams++; c->params[i].pname = pname; } }
		if (!r) c->params[i].value = value;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_param_get(sgl_ctx *c, uint32_t pname, int32_t *value)
{
	pthread_mutex_lock(&mu);
	int r = -ENOENT;
	if (!is_live(c) || !value) r = -EINVAL;
	else for (int i = 0; i < c->nparams; i++) if (c->params[i].pname == pname) { *value = c->params[i].value; r = 0; }
	pthread_mutex_unlock(&mu);
	return r;
}

static int surf_ok(const struct sweet_gl_surface *s)
{
	return s && s->id && s->va && s->w && s->h && s->bytes_per_row &&
	       (uint64_t)s->bytes_per_row * s->h <= s->len;
}

/* mu held. */
static int need_current(const sgl_ctx *c)
{
	if (!is_live(c)) return -EINVAL;
	if (c != tl_cur) return -EPERM;
	return 0;
}

int sgl_attach_surface(sgl_ctx *c, uint32_t tex, const struct sweet_gl_surface *surf, int inverted)
{
	pthread_mutex_lock(&mu);
	int r = need_current(c);
	if (!r && (!tex || !surf_ok(surf))) r = -EINVAL;
	if (!r) {
		int i;
		for (i = 0; i < c->natt && c->att[i].tex != tex; i++) ;
		if (i == c->natt && i == SGL_MAX_ATTACH) r = -ENOSPC;
		else if (!be.attach_surface) r = -ENOSYS;
		else {
			r = be.attach_surface(be.user, c->bctx, tex, surf, inverted);
			if (!r) {
				if (i == c->natt) c->natt++;
				c->att[i].tex = tex; c->att[i].inverted = inverted; c->att[i].s = *surf;
			}
		}
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_detach_surface(sgl_ctx *c, uint32_t tex)
{
	pthread_mutex_lock(&mu);
	int r = need_current(c);
	if (!r) {
		int i;
		for (i = 0; i < c->natt && c->att[i].tex != tex; i++) ;
		if (i == c->natt) r = -ENOENT;
		else {
			if (be.attach_surface) r = be.attach_surface(be.user, c->bctx, tex, NULL, 0);
			if (!r) { c->att[i] = c->att[--c->natt]; }
		}
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_attachment(sgl_ctx *c, uint32_t tex, struct sweet_gl_surface *out, int *inverted)
{
	pthread_mutex_lock(&mu);
	int r = is_live(c) ? -ENOENT : -EINVAL;
	if (r == -ENOENT) for (int i = 0; i < c->natt; i++) if (c->att[i].tex == tex) {
		if (out) *out = c->att[i].s;
		if (inverted) *inverted = c->att[i].inverted;
		r = 0;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_attach_count(sgl_ctx *c)
{
	pthread_mutex_lock(&mu);
	int r = is_live(c) ? c->natt : -EINVAL;
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_tex_image_surface(sgl_ctx *c, const struct sweet_gl_surface *surf, uint32_t target, uint32_t internalformat,
                          uint32_t w, uint32_t h, uint32_t format, uint32_t type, uint32_t plane, int invert)
{
	pthread_mutex_lock(&mu);
	int r = need_current(c);
	if (!r && !surf_ok(surf)) r = -EINVAL;
	if (!r) r = be.tex_image_surface ? be.tex_image_surface(be.user, c->bctx, surf, target, internalformat, w, h,
	                                                        format, type, plane, invert) : -ENOSYS;
	pthread_mutex_unlock(&mu);
	return r;
}

int sgl_flush(sgl_ctx *c)
{
	pthread_mutex_lock(&mu);
	int r = need_current(c);
	if (!r) r = be.flush ? be.flush(be.user, c->bctx) : -ENOSYS;
	pthread_mutex_unlock(&mu);
	return r;
}
