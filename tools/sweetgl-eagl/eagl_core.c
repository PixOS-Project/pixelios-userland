/* eagl_core.c: see eagl_core.h. Plain C11, POSIX threads only. */
#include "eagl_core.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct attach { uint32_t tex; int inverted; struct sweet_gl_surface s; };
struct param { uint32_t pname; int32_t value; };

struct sgl_ctx {
	int live, api;
	void *user;
	void *bctx;
	int bound;            /* current on some thread */
	pthread_t owner;
	struct param params[SGL_MAX_PARAMS]; int nparams;
	struct attach att[SGL_MAX_ATTACH]; int natt; unsigned att_next;   /* att_next: the record a full table replaces next (oldest first) */
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long att_evictions;
unsigned long sgl_attach_evictions(void) { return __atomic_load_n(&att_evictions, __ATOMIC_RELAXED); }
static struct sweet_gl_backend be;
static int have_be;
static sgl_ctx table[SGL_MAX_CTX];
static _Thread_local sgl_ctx *tl_cur;
/* tl_cur can go stale when another thread steals the context (sgl_ensure_current) or it is destroyed. Every steal/destroy/reset bumps
 * g_epoch; a thread whose tl_epoch differs revalidates tl_cur (live, bound, owned by itself) before trusting it. */
static unsigned g_epoch;
static _Thread_local unsigned tl_epoch;
#define EPOCH() __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE)

/* One line per write() to a private /dev/null fd (patch 0048 tees it into the kernel log as "|backboardd| sweetgl: ..."), only the
 * first SGL_LOG_MAX lines of the process, so a hot path cannot flood the log. */
#define SGL_LOG_MAX 30
void sgl_log(const char *fmt, ...)
{
	static int n, fd = -2;
	if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) >= SGL_LOG_MAX) return;
	if (fd == -2) fd = open("/dev/null", O_WRONLY | O_CLOEXEC);   /* ponytail: racy init may leak one fd */
	if (fd < 0) return;
	char b[200];
	int k = snprintf(b, sizeof b, "sweetgl: ");
	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(b + k, sizeof b - (size_t)k - 2, fmt, ap);
	va_end(ap);
	if (m < 0) return;
	k += m < (int)sizeof b - k - 2 ? m : (int)sizeof b - k - 2;
	b[k++] = '\n';
	(void)!write(fd, b, (size_t)k);
}

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
	__atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
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
		__atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
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
/* mu held. This thread's current context, revalidated if the epoch moved. */
static sgl_ctx *cur_locked(void)
{
	unsigned e = EPOCH();
	if (tl_epoch != e) {
		if (tl_cur && !(is_live(tl_cur) && tl_cur->bound && pthread_equal(tl_cur->owner, pthread_self()))) tl_cur = NULL;
		tl_epoch = e;
	}
	return tl_cur;
}

sgl_ctx *sgl_current(void)
{
	pthread_mutex_lock(&mu);
	sgl_ctx *c = cur_locked();
	pthread_mutex_unlock(&mu);
	return c;
}

/* mu held. steal != 0: take a context that is current on another thread (that thread's tl_cur goes stale, see g_epoch). */
static int set_cur_locked(sgl_ctx *c, int steal)
{
	sgl_ctx *cur = cur_locked();
	int stolen = 0;
	if (c && !is_live(c)) return -EINVAL;
	if (c && c->bound && !pthread_equal(c->owner, pthread_self())) {
		if (!steal) return -EBUSY;
		stolen = 1;
	}
	if (c == cur) return 0;
	if (be.make_current && be.make_current(be.user, c ? c->bctx : NULL) != 0) return -EIO;
	if (cur) cur->bound = 0;
	tl_cur = c;
	if (c) { c->bound = 1; c->owner = pthread_self(); }
	if (stolen) __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
	tl_epoch = EPOCH();
	return 0;
}

int sgl_set_current(sgl_ctx *c)
{
	pthread_mutex_lock(&mu);
	int r = set_cur_locked(c, 0);
	pthread_mutex_unlock(&mu);
	return r;
}

/* GLI path: CA's render thread is not the thread that created the context and never sends +setCurrentContext: [obs research 36],
 * so take the context for the calling thread even if another thread holds it. ponytail: only one thread may use GL at a time (the
 * old owner's Mesa TLS still points at the context; nothing stops it from calling in). Ceiling = a real second GL thread would need
 * Mesa-level context migration. */
int sgl_ensure_current(void *rctx)
{
	sgl_ctx *c = rctx;
	if (c && c == tl_cur && tl_epoch == EPOCH()) return 0;   /* fast path: no lock; epoch unchanged = nobody stole or destroyed since we bound it */
	pthread_mutex_lock(&mu);
	sgl_ctx *cur = cur_locked();
	int steal = c && c->bound && !pthread_equal(c->owner, pthread_self());
	int r = c == cur ? 0 : set_cur_locked(c, 1);
	if (c != cur)
		sgl_log("ensure ctx=%p steal=%d ret=%d tid=%lu", (void *)c, steal, r, (unsigned long)(uintptr_t)pthread_self());
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
	if (c != cur_locked()) return -EPERM;
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
		/* sprint GPU 2026-11-11 defect 1 [obs gputest-surffail-a: 74 'SURFFAIL attach core err -28']: the table is bookkeeping only (nothing
		 * reads it on the device), CA never detaches and deleted texture names stay recorded, so after 64 distinct names every NEW name was
		 * refused with -ENOSPC and its texture (CA's w30r 1125x2436 offscreens among them) stayed storage-less = FBO incomplete. A full table
		 * now replaces its oldest record (counted, sgl_attach_evictions) and the attach goes ahead. */
		int evict = i == c->natt && i == SGL_MAX_ATTACH;
		if (evict) i = (int)(c->att_next++ % SGL_MAX_ATTACH);
		if (!be.attach_surface) r = -ENOSYS;
		else {
			r = be.attach_surface(be.user, c->bctx, tex, surf, inverted);
			if (!r) {
				if (evict) __atomic_add_fetch(&att_evictions, 1, __ATOMIC_RELAXED);
				else if (i == c->natt) c->natt++;
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

/* ---- format-0 surface diagnostics (research 86 sections E/F/H, DRAFT-format0-batch); unsynchronised: callers hold the core mutex ---- */
#define FMT0_IDS 64
static uint32_t fmt0_att[FMT0_IDS]; static unsigned fmt0_natt;
static struct { uint32_t id; unsigned char n; } fmt0_smp[128]; static unsigned fmt0_nsmp;

int sgl_fmt0_attach_line(char *buf, size_t n, uint32_t id, uint32_t w, uint32_t h, uint32_t pitch, int pipe_fmt, unsigned ifmt)
{
	return snprintf(buf, n, "SWEETGL: fmt0 attach id %u %ux%u pitch %u pipe_fmt %d ifmt 0x%x", id, w, h, pitch, pipe_fmt, ifmt);
}

int sgl_fmt0_sample_line(char *buf, size_t n, uint32_t id, uint32_t w, uint32_t h, unsigned ifmt, unsigned fmt, unsigned type, int attached)
{
	return snprintf(buf, n, "SWEETGL: fmt0 sample id %u %ux%u ifmt 0x%x fmt 0x%x type 0x%x attached %d", id, w, h, ifmt, fmt, type, attached);
}

int sgl_fmt0_was_attached(uint32_t id)
{
	for (unsigned i = 0; i < fmt0_natt; i++) if (fmt0_att[i] == id) return 1;
	return 0;
}

int sgl_fmt0_note_attach(uint32_t id)
{
	if (sgl_fmt0_was_attached(id) || fmt0_natt >= FMT0_IDS) return 0;
	fmt0_att[fmt0_natt++] = id;
	return 1;
}

int sgl_fmt0_sample_count(uint32_t id)
{
	for (unsigned i = 0; i < fmt0_nsmp; i++)
		if (fmt0_smp[i].id == id) { int c = fmt0_smp[i].n; if (c < 255) fmt0_smp[i].n++; return c; }
	if (fmt0_nsmp >= 128) return 2;   /* table full: report "already logged twice" so nothing more is printed */
	fmt0_smp[fmt0_nsmp].id = id; fmt0_smp[fmt0_nsmp].n = 1; fmt0_nsmp++;
	return 0;
}

void sgl_fmt0_reset(void) { fmt0_natt = 0; fmt0_nsmp = 0; }

/* ---- IOSurface texture-path failures (eagl_core.h enum sgl_sf_path) ---- */
static const char *const sf_names[SGL_SF_N] = { "noctx", "current", "nosurf", "import", "lock", "core", "nostorage", "tex0", "target", "fallback", "fbfail" };
static unsigned long sf_cnt[SGL_SF_N];

const char *sgl_sf_path_name(int path) { return path >= 0 && path < SGL_SF_N ? sf_names[path] : "?"; }
unsigned long sgl_sf_note(int path) { return path >= 0 && path < SGL_SF_N ? __atomic_add_fetch(&sf_cnt[path], 1, __ATOMIC_RELAXED) : 0; }
unsigned long sgl_sf_count(int path) { return path >= 0 && path < SGL_SF_N ? __atomic_load_n(&sf_cnt[path], __ATOMIC_RELAXED) : 0; }
int sgl_sf_should_print(unsigned long count) { return count >= 1 && (count <= 2000 || count % 1000 == 0); }
void sgl_sf_reset(void) { for (int i = 0; i < SGL_SF_N; i++) __atomic_store_n(&sf_cnt[i], 0, __ATOMIC_RELAXED); }

int sgl_sf_line(char *buf, size_t n, const char *api, int path, uint32_t id, uint32_t fourcc, uint32_t w, uint32_t h, uint32_t bpr, int err,
                unsigned bound, unsigned long count)
{
	char cc[5];
	for (int i = 0; i < 4; i++) {
		unsigned char b = (unsigned char)(fourcc >> (24 - 8 * i));
		cc[i] = b >= 0x20 && b < 0x7f && b != '\'' ? (char)b : '.';
	}
	cc[4] = 0;
	return snprintf(buf, n, "SWEETGL: SURFFAIL %s %s id %u fourcc '%s' (0x%08x) %ux%u bpr %u err %d bound %u n %lu", api ? api : "?",
	                sgl_sf_path_name(path), id, cc, fourcc, w, h, bpr, err, bound, count);
}

int sgl_sf_stat_line(char *buf, size_t n, unsigned long calls, unsigned long ok)
{
	int k = snprintf(buf, n, "SWEETGL: SURFSTAT calls %lu ok %lu", calls, ok);
	for (int i = 0; i < SGL_SF_N && k >= 0 && (size_t)k < n; i++)
		k += snprintf(buf + k, n - (size_t)k, " %s %lu", sf_names[i], sgl_sf_count(i));
	return k;
}
