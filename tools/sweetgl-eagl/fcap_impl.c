/* fcap_impl.c - Gallium-level frame capture for the host software-rasterizer replay (format: fcap_format.h). Sprint GPU 2026-11-09.
 * #included at the end of eagl_mesa.c (same Mesa compile flags; no build-script change). Off unless /private/var/dev/fcap-on holds "1" at
 * screen creation (or env SWEETGL_FCAP=1): then the panfrost pipe_screen / pipe_context function pointers are replaced IN PLACE by hooks that
 * call the saved originals with the same object, so the driver sees exactly the calls it saw before.
 *
 * Two levels:
 *  - always (hooks installed): object tracking only = ids for resources, the st's NIR of every shader (nir_serialize before panfrost
 *    lowers it), CSO templates, sampler views, bound state. No copies, no driver calls.
 *  - recording (eager, while /private/var/dev/fcap-eager holds "1" or the kernel nav capture rep says so via sysctl kern.sweetnavcap bit 0):
 *    every op (draw, clear, blit, copy, texture write, mipmap generation) is recorded with its full state; each resource is snapshotted at its
 *    FIRST touch in the frame (GPU view = pipe texture_map READ, what the GPU and display read; for IOSurfaces bound by texImageIOSurface also
 *    the CPU view = the app's CPU-cached mapping, taken during the call while CA holds the lock). At the frame's flush the scanout content
 *    (GPU view) is stored as the RESULT. Frames live in a ring of FCAP_NFR; blobs are deduplicated by XXH64.
 *  - a capture request (file /private/var/dev/fcap-req holding "<n> <label>", or a new seq in sysctl kern.sweetnavcap with label kern.sweetnavcaplbl)
 *    writes the last FCAP_K completed frames to /private/var/dev/fcap/NNNN-label.fcap (blobs already written in this boot are not repeated:
 *    the host loads every file of the run). The writer is a separate thread; the render thread only waits for the mutex.
 * Every case the recorder cannot express is an FC_GAP record + a SWEET GAP line (rule 7), never a silent skip.
 */
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#endif
#include <time.h>
#include <unistd.h>
#include "util/hash_table.h"
#include "util/u_dynarray.h"
#include "util/blob.h"
#include "util/u_math.h"
#include "util/box.h"
#include "util/format/u_format.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "nir/tgsi_to_nir.h"
#include "fcap_format.h"

#ifndef FCAP_ROOT
#define FCAP_ROOT "/private/var/dev"
#endif
#define FCAP_DIR FCAP_ROOT "/fcap"
#define FCAP_NFR 4          /* frames kept */
#define FCAP_K 2            /* frames written per request (the requested frame and the one before it) */
#define FCAP_STAGES 2       /* 0 = VS, 1 = FS */
#define FCAP_MAXVIEWS 32
#define FCAP_MAXSAMP 32
#define FCAP_MAXCB 16
#define FCAP_MAXCTX 4

/* ---------------------------------------------------------------- byte buffer */
struct fbuf { uint8_t *p; size_t n, cap; };
static void fb_need(struct fbuf *b, size_t add)
{
   if (b->n + add <= b->cap) return;
   size_t c = b->cap ? b->cap : 4096;
   while (c < b->n + add) c *= 2;
   uint8_t *q = realloc(b->p, c);
   if (!q) abort();   /* the capture build only: out of memory is not survivable silently */
   b->p = q; b->cap = c;
}
static void fb_put(struct fbuf *b, const void *d, size_t n) { fb_need(b, n); memcpy(b->p + b->n, d, n); b->n += n; }
static void fb_u32(struct fbuf *b, uint32_t v) { fb_put(b, &v, 4); }
static void fb_i32(struct fbuf *b, int32_t v) { fb_put(b, &v, 4); }
static void fb_u64(struct fbuf *b, uint64_t v) { fb_put(b, &v, 8); }
static void fb_f32(struct fbuf *b, float v) { fb_put(b, &v, 4); }
static void fb_f64(struct fbuf *b, double v) { fb_put(b, &v, 8); }
static void fb_str(struct fbuf *b, const char *s, size_t n) { char t[256] = { 0 }; if (n > sizeof t) n = sizeof t; strncpy(t, s ? s : "", n - 1); fb_put(b, t, n); }
static size_t fb_rec(struct fbuf *b, uint32_t type) { fb_u32(b, type); fb_u32(b, 0); return b->n; }
static void fb_end(struct fbuf *b, size_t at) { uint32_t len = (uint32_t)(b->n - at); memcpy(b->p + at - 4, &len, 4); }
static void fb_free(struct fbuf *b) { free(b->p); memset(b, 0, sizeof *b); }

/* ---------------------------------------------------------------- objects */
struct fobj {
   uint32_t id, kind;          /* kind: FC_RES, FC_SHADER, FC_CSO, FC_SVIEW */
   struct fbuf def;            /* the complete definition record */
   uint32_t def_frame;         /* frame seq in which def was last emitted (+1, 0 = never) */
   /* resources */
   struct pipe_resource *res;
   uint32_t touch_frame;       /* +1 */
   uint32_t cpu_frame;         /* +1: CPU view taken in this frame */
   uint64_t cpu_hash;          /* its blob */
   int imported, scanout;
   uint32_t surf_id, fourcc, pitch;
   /* vertex elements */
   unsigned nve, cpe, stipple;  /* velems count; rasterizer clip_plane_enable, poly_stipple_enable */
   struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
   /* sampler view */
   struct fobj *view_res;
};

struct fblob { uint64_t h; uint32_t len, refs; uint8_t *data; };

struct fframe {
   uint32_t seq, ops, draws, refs, orphan;
   uint64_t t_ns;
   struct fbuf b;
   struct util_dynarray blobs;   /* uint64_t hashes this frame references (with repeats) */
   struct pipe_resource *scanout;
   uint32_t coh_diff;            /* CPU view != GPU view snapshots */
   struct fctx *ctx;             /* context of the last recorded op */
};

struct fcb { int set; struct pipe_resource *buf; unsigned off, size; uint8_t *user; };

struct fctx {
   struct pipe_context *pctx;
   struct pipe_context orig;     /* the driver's function table before the hooks */
   void *blend, *rast, *dsa, *vs, *fs, *velems;
   void *samp[FCAP_STAGES][FCAP_MAXSAMP]; unsigned nsamp[FCAP_STAGES];
   struct pipe_sampler_view *view[FCAP_STAGES][FCAP_MAXVIEWS]; unsigned nview[FCAP_STAGES];
   struct fcb cb[FCAP_STAGES][FCAP_MAXCB];
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS]; unsigned nvb;
   struct pipe_framebuffer_state fb;
   struct pipe_viewport_state vp; struct pipe_scissor_state sc;
   struct pipe_blend_color bc; struct pipe_stencil_ref sr; unsigned sample_mask, min_samples;
   struct pipe_clip_state clip;
   uint32_t fb_frame;            /* +1: FC_FB emitted for the current fb in this frame */
   struct pipe_resource *fb_scanout;   /* cbuf of the bound framebuffer that is a display surface (tracked always: the scanout tap) */
};

static struct {
   int installed, eager_file, eager_sysctl, have_sysctl, tap_sysctl;
   struct pipe_resource *tap_scanout;  /* display surface drawn since the last flush */
   pthread_mutex_t mu;
   struct pipe_screen *scr;
   struct pipe_resource *(*resource_create)(struct pipe_screen *, const struct pipe_resource *);
   struct pipe_resource *(*resource_from_handle)(struct pipe_screen *, const struct pipe_resource *, struct winsys_handle *, unsigned);
   void (*resource_destroy)(struct pipe_screen *, struct pipe_resource *);
   struct pipe_context *(*context_create)(struct pipe_screen *, void *, unsigned);
   struct hash_table *objs;            /* pointer -> struct fobj (resources, shaders, CSOs, views share one table: pointers are unique) */
   struct hash_table_u64 *blobs;       /* hash -> struct fblob */
   struct hash_table_u64 *written;     /* hash -> (void*)1: blob already in a file of this boot */
   uint32_t next_id, seq, files, cap_seen, gaps, req_seen, req_armed;
   struct fframe *cur, *ring[FCAP_NFR];
   struct fframe *first[8]; unsigned nfirst, want_first, win_reset;   /* SWEETGL_FCAP_FIRST=N (<= 8): also keep the first N frames of each recording window */
   struct fctx *ctx[FCAP_MAXCTX];
   pthread_t thr;
   uint64_t bytes_written;
   uint32_t n_flush, n_frames, n_tapf;   /* stats line (writer thread, every 2 s while recording or tapping) */
} FC;   /* FC.mu is RECURSIVE (fcap_install): a snapshot calls the driver with it held, and the driver creates staging resources through the hooked resource_create */

static void fc_tap_note(struct pipe_resource *dst);
static __thread int fc_depth;
/* SWEETGL_FCAP_DEPFLUSH=1 (sprint GPU 2026-11-10, App Library backdrop black at real timing but correct while recording): flush the context before a
 * draw / blit / copy that READS a resource drawn as a render target since the last flush (what the recorder's snapshots do as a side effect).
 * A diagnostic knob, not a fix: if it cures the backdrop, a dependency flush is missing below. Counted in the STAT/DEPFLUSH lines. */
static int fc_depflush = -1;
#define FCAP_DIRTY 32
static struct pipe_resource *fc_dirty_rt[FCAP_DIRTY];
static unsigned fc_ndirty, fc_depflushes;
static void fc_dirty_add(struct pipe_resource *r)
{
   if (!r) return;
   for (unsigned i = 0; i < fc_ndirty; i++) if (fc_dirty_rt[i] == r) return;
   if (fc_ndirty < FCAP_DIRTY) fc_dirty_rt[fc_ndirty++] = r;
}
static int fc_is_dirty(const struct pipe_resource *r)
{
   for (unsigned i = 0; i < fc_ndirty; i++) if (fc_dirty_rt[i] == r) return 1;
   return 0;
}   /* > 0: inside a hook's driver call or a snapshot: nested calls are the driver's own, not recorded */

static uint64_t fc_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static int fc_recording(void) { return FC.installed && (FC.have_sysctl ? FC.eager_sysctl : FC.eager_file) && FC.cur; }   /* the kernel sysctl, when present, is the only switch (its real-timing pass turns recording off) */

static void fc_gap(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fc_gap(const char *fmt, ...)
{
   char t[200];
   va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
   sweet_gap("fcap", "%s", t);
   if (FC.cur) { size_t at = fb_rec(&FC.cur->b, FC_GAP); fb_put(&FC.cur->b, t, strlen(t)); fb_end(&FC.cur->b, at); }
   FC.gaps++;
}

static struct fobj *fc_obj(const void *p)
{
   struct hash_entry *e = p ? _mesa_hash_table_search(FC.objs, p) : NULL;
   return e ? e->data : NULL;
}
static struct fobj *fc_new(const void *p, uint32_t kind)
{
   struct fobj *o = calloc(1, sizeof *o);
   if (!o) abort();
   o->id = ++FC.next_id; o->kind = kind;
   struct hash_entry *e = _mesa_hash_table_search(FC.objs, p);
   if (e) { struct fobj *old = e->data; fb_free(&old->def); free(old); e->data = o; }
   else _mesa_hash_table_insert(FC.objs, p, o);
   return o;
}
static void fc_forget(const void *p)
{
   struct hash_entry *e = p ? _mesa_hash_table_search(FC.objs, p) : NULL;
   if (!e) return;
   struct fobj *o = e->data;
   fb_free(&o->def); free(o);
   _mesa_hash_table_remove(FC.objs, e);
}

/* ---------------------------------------------------------------- blobs */
static uint64_t fc_blob(const void *data, uint32_t len)
{
   uint64_t h = XXH64(data, len, 0);
   struct fblob *b = _mesa_hash_table_u64_search(FC.blobs, h);
   if (!b) {
      b = calloc(1, sizeof *b);
      if (!b) abort();
      b->h = h; b->len = len; b->data = malloc(len ? len : 1);
      if (!b->data) abort();
      memcpy(b->data, data, len);
      _mesa_hash_table_u64_insert(FC.blobs, h, b);
   }
   b->refs++;
   util_dynarray_append_typed(&FC.cur->blobs, uint64_t, h);
   return h;
}
static void fc_frame_free(struct fframe *f)
{
   if (!f) return;
   util_dynarray_foreach(&f->blobs, uint64_t, hp) {
      struct fblob *b = _mesa_hash_table_u64_search(FC.blobs, *hp);
      if (b && --b->refs == 0) { _mesa_hash_table_u64_remove(FC.blobs, b->h); free(b->data); free(b); }
   }
   util_dynarray_fini(&f->blobs);
   fb_free(&f->b);
   free(f);
}
static void fc_frame_start(void)
{
   struct fframe *f = calloc(1, sizeof *f);
   if (!f) abort();
   f->seq = ++FC.seq; f->t_ns = fc_now();
   util_dynarray_init(&f->blobs, NULL);
   size_t at = fb_rec(&f->b, FC_FRAME);
   fb_u32(&f->b, f->seq); fb_u64(&f->b, f->t_ns); fb_u32(&f->b, 0); fb_str(&f->b, "", 64);
   fb_end(&f->b, at);
   FC.cur = f;
}

/* ---------------------------------------------------------------- definitions */
static void fc_emit_def(struct fobj *o)
{
   if (!o || o->def_frame == FC.cur->seq) return;
   if (o->kind == FC_SVIEW && o->view_res) fc_emit_def(o->view_res);
   fb_put(&FC.cur->b, o->def.p, o->def.n);
   o->def_frame = FC.cur->seq;
}

/* Compressed textures (CA uploads ASTC, e.g. a 1125x2436 ASTC_4x4_FLOAT in gputest-fcap7 app-launch) are recorded DECODED: the software
 * rasterizer has no ASTC path. The decode is a GPU blit on the device (an input the replay cannot check; listed in the RES record). */
static enum pipe_format fc_decoded_format(enum pipe_format f)
{
   if (!util_format_is_compressed(f)) return f;
   if (util_format_is_float(f)) return PIPE_FORMAT_R16G16B16A16_FLOAT;
   return util_format_is_srgb(f) ? PIPE_FORMAT_R8G8B8A8_SRGB : PIPE_FORMAT_R8G8B8A8_UNORM;
}

static void fc_res_def(struct fobj *o)
{
   const struct pipe_resource *r = o->res;
   o->def.n = 0;
   size_t at = fb_rec(&o->def, FC_RES);
   fb_u32(&o->def, o->id); fb_u32(&o->def, r->target); fb_u32(&o->def, fc_decoded_format(r->format)); fb_u32(&o->def, r->width0); fb_u32(&o->def, r->height0);
   fb_u32(&o->def, r->depth0); fb_u32(&o->def, r->array_size); fb_u32(&o->def, r->last_level); fb_u32(&o->def, r->nr_samples);
   fb_u32(&o->def, r->bind); fb_u32(&o->def, r->flags); fb_u32(&o->def, o->imported); fb_u32(&o->def, o->surf_id); fb_u32(&o->def, o->fourcc);
   fb_u32(&o->def, o->pitch); fb_u32(&o->def, o->scanout);
   fb_u32(&o->def, r->format);   /* 17th field: the device format (differs from field 3 when the content is recorded decoded) */
   fb_end(&o->def, at);
   o->def_frame = 0;
}

static struct fobj *fc_res(struct pipe_resource *r)
{
   if (!r) return NULL;
   struct fobj *o = fc_obj(r);
   if (!o) {   /* created before the hooks (none expected) or by a path we do not hook */
      o = fc_new(r, FC_RES); o->res = r; fc_res_def(o);
   }
   return o;
}

/* ---------------------------------------------------------------- snapshots */
/* GPU view of one level/layer through the driver's map (the bytes the GPU and the display read). */
static void fc_snap_gpu(struct fctx *c, struct fobj *o, unsigned level, unsigned layer, uint32_t role)
{
   struct pipe_resource *r = o->res;
   if (r->target == PIPE_BUFFER) { fc_gap("snapshot of a buffer resource %u not implemented", o->id); return; }
   if (r->nr_samples > 1) { fc_gap("snapshot of multisampled resource %u (%u samples) not implemented", o->id, r->nr_samples); return; }
   unsigned w = u_minify(r->width0, level), h = u_minify(r->height0, level);
   struct pipe_box box; u_box_3d(0, 0, (int)layer, (int)w, (int)h, 1, &box);
   struct pipe_transfer *tr = NULL;
   struct pipe_resource *src = r, *dec = NULL;
   unsigned slevel = level;
   enum pipe_format df = fc_decoded_format(r->format);
   if (df != r->format) {
      static int nodecode = -1;   /* diagnostic SWEETGL_FCAP_NODECODE=1 (defect 1 A/B): no GPU decode blit of compressed textures (content missing = GAP) */
      if (nodecode < 0) { const char *e = getenv("SWEETGL_FCAP_NODECODE"); nodecode = e && e[0] == '1'; if (nodecode) fprintf(stderr, "sweetgl: FCAP diagnostic nodecode (GAP: compressed textures not captured)\n"); }
      if (nodecode) { fc_gap("compressed resource %u %ux%u %s not decoded (SWEETGL_FCAP_NODECODE)", o->id, w, h, util_format_short_name(r->format)); return; }
   }
   if (df != r->format) {   /* decode on the GPU into a linear staging texture of the decoded format */
      struct pipe_resource t; memset(&t, 0, sizeof t);
      t.target = PIPE_TEXTURE_2D; t.format = df; t.width0 = w; t.height0 = h; t.depth0 = 1; t.array_size = 1;
      t.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
      dec = FC.resource_create(FC.scr, &t);
      if (!dec) { fc_gap("decode staging %ux%u fmt %d for resource %u not created", w, h, (int)df, o->id); return; }
      struct pipe_blit_info bi; memset(&bi, 0, sizeof bi);
      bi.dst.resource = dec; bi.dst.format = df; u_box_2d(0, 0, (int)w, (int)h, &bi.dst.box);
      bi.src.resource = r; bi.src.format = r->format; bi.src.level = level; u_box_3d(0, 0, (int)layer, (int)w, (int)h, 1, &bi.src.box);
      bi.mask = PIPE_MASK_RGBA; bi.filter = PIPE_TEX_FILTER_NEAREST;
      fc_depth++; c->orig.blit(c->pctx, &bi); fc_depth--;
      src = dec; slevel = 0; u_box_2d(0, 0, (int)w, (int)h, &box);
      static unsigned dl;
      if (dl < 16) { dl++; fprintf(stderr, "sweetgl: FCAP decoded resource %u %ux%u %s -> %s for the snapshot (GPU blit)\n", o->id, w, h, util_format_short_name(r->format), util_format_short_name(df)); }
   }
   fc_depth++;
   uint8_t *p = c->orig.texture_map(c->pctx, src, slevel, PIPE_MAP_READ, &box, &tr);
   fc_depth--;
   if (!p) { fc_gap("texture_map READ failed: resource %u level %u layer %u fmt %d", o->id, level, layer, (int)r->format); pipe_resource_reference(&dec, NULL); return; }
   unsigned rowb = util_format_get_stride(df, w), rows = util_format_get_nblocksy(df, h);
   uint8_t *tmp = malloc((size_t)rowb * rows);
   if (!tmp) abort();
   for (unsigned y = 0; y < rows; y++) memcpy(tmp + (size_t)y * rowb, p + (size_t)y * tr->stride, rowb);
   fc_depth++; c->orig.texture_unmap(c->pctx, tr); fc_depth--;
   pipe_resource_reference(&dec, NULL);
   uint64_t hsh = fc_blob(tmp, rowb * rows);
   free(tmp);
   size_t at = fb_rec(&FC.cur->b, FC_CONTENT);
   fb_u32(&FC.cur->b, o->id); fb_u32(&FC.cur->b, level); fb_u32(&FC.cur->b, layer); fb_u32(&FC.cur->b, w); fb_u32(&FC.cur->b, h);
   fb_u32(&FC.cur->b, rowb); fb_u32(&FC.cur->b, FCAP_VIEW_GPU); fb_u32(&FC.cur->b, role); fb_u64(&FC.cur->b, hsh); fb_u32(&FC.cur->b, FC.cur->seq);
   fb_end(&FC.cur->b, at);
   if (role == FCAP_ROLE_INITIAL && level == 0 && layer == 0 && o->cpu_frame == FC.cur->seq + 1 && o->cpu_hash != hsh) {
      /* coherency witness: the app's CPU view and the GPU's view of the same IOSurface differ at the first GPU use in this frame */
      struct fblob *a = _mesa_hash_table_u64_search(FC.blobs, o->cpu_hash), *g = _mesa_hash_table_u64_search(FC.blobs, hsh);
      uint32_t nd = 0, r0 = UINT32_MAX, r1 = 0;
      if (a && g && a->len == g->len) {
         for (uint32_t i = 0; i < a->len; i++) if (a->data[i] != g->data[i]) { nd++; uint32_t r = i / rowb; if (r < r0) r0 = r; r1 = r; }
      }
      FC.cur->coh_diff++;
      static unsigned logged;
      if (logged < 400) {
         logged++;
         fprintf(stderr, "sweetgl: FCAP COH frame %u res %u surf %u fourcc 0x%08x %ux%u differ %u of %u bytes rows %u..%u\n", FC.cur->seq, o->id, o->surf_id,
                 o->fourcc, w, h, nd, rowb * rows, r0 == UINT32_MAX ? 0 : r0, r1);
      }
   }
}

/* CPU view of an imported app surface at its first touch in the frame: the app's bytes through this process's CPU-cached mapping
 * (IOSurfaceLookup by id + read-only lock; a surface that is gone is skipped). CA binds most surfaces once and samples the cached GL texture
 * afterwards, so the bind-time CPU view (fcap_note_surface) alone missed them (gputest-fcap7: 0 CPU views). Caller holds FC.mu. */
static void fc_cpu_view(struct fobj *o)
{
#ifdef __APPLE__
   struct pipe_resource *r = o->res;
   if (!o->imported || o->scanout || !o->surf_id || o->fourcc == 0 || o->fourcc == 0x77333072u /* 'w30r' CA render surface */ ||
       o->cpu_frame == FC.cur->seq + 1 || r->target == PIPE_BUFFER || util_format_is_compressed(r->format)) return;
   /* resolved at run time: backboardd has IOSurface.framework loaded; the link-test stage of the build does not link it */
   static IOSurfaceRef (*lookup)(IOSurfaceID); static size_t (*gw)(IOSurfaceRef), (*gh)(IOSurfaceRef), (*gbpr)(IOSurfaceRef), (*glen)(IOSurfaceRef);
   static kern_return_t (*lock)(IOSurfaceRef, IOSurfaceLockOptions, uint32_t *), (*unlock)(IOSurfaceRef, IOSurfaceLockOptions, uint32_t *);
   static void *(*gbase)(IOSurfaceRef); static void (*rel)(const void *); static int resolved;
   if (!resolved) {
      resolved = 1;
      lookup = dlsym(RTLD_DEFAULT, "IOSurfaceLookup"); gw = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidth"); gh = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeight");
      gbpr = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow"); glen = dlsym(RTLD_DEFAULT, "IOSurfaceGetAllocSize"); lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
      unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock"); gbase = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress"); rel = dlsym(RTLD_DEFAULT, "CFRelease");
      if (!lookup || !gw || !gh || !gbpr || !glen || !lock || !unlock || !gbase || !rel) { lookup = NULL; sweet_gap("fcap", "IOSurface symbols not found: no CPU views"); }
   }
   if (!lookup) return;
   IOSurfaceRef s = lookup(o->surf_id);
   if (!s) return;
   if ((uint32_t)gw(s) != r->width0 || (uint32_t)gh(s) != r->height0) { rel(s); return; }   /* id reused by another surface */
   if (lock(s, kIOSurfaceLockReadOnly, NULL) == 0) {
      const uint8_t *base = gbase(s);
      size_t bpr = gbpr(s), len = glen(s);
      unsigned rowb = util_format_get_stride(r->format, r->width0), rows = util_format_get_nblocksy(r->format, r->height0);
      if (base && rowb <= bpr && (uint64_t)bpr * (rows - 1) + rowb <= len) {
         uint8_t *tmp = malloc((size_t)rowb * rows);
         if (!tmp) abort();
         for (unsigned y = 0; y < rows; y++) memcpy(tmp + (size_t)y * rowb, base + (size_t)y * bpr, rowb);
         uint64_t hsh = fc_blob(tmp, rowb * rows);
         free(tmp);
         size_t at = fb_rec(&FC.cur->b, FC_CONTENT);
         fb_u32(&FC.cur->b, o->id); fb_u32(&FC.cur->b, 0); fb_u32(&FC.cur->b, 0); fb_u32(&FC.cur->b, r->width0); fb_u32(&FC.cur->b, r->height0);
         fb_u32(&FC.cur->b, rowb); fb_u32(&FC.cur->b, FCAP_VIEW_CPU); fb_u32(&FC.cur->b, FCAP_ROLE_INITIAL); fb_u64(&FC.cur->b, hsh); fb_u32(&FC.cur->b, FC.cur->seq);
         fb_end(&FC.cur->b, at);
         o->cpu_frame = FC.cur->seq + 1;
         o->cpu_hash = hsh;
      }
      unlock(s, kIOSurfaceLockReadOnly, NULL);
   }
   rel(s);
#else
   (void)o;
#endif
}

/* First touch of a resource in the current frame: definition + GPU-view snapshot of every level and layer. */
static void fc_touch(struct fctx *c, struct pipe_resource *r)
{
   struct fobj *o = fc_res(r);
   if (!o) return;
   FC.cur->ctx = c;
   fc_emit_def(o);
   if (o->touch_frame == FC.cur->seq + 1) return;
   o->touch_frame = FC.cur->seq + 1;
   if (r->target == PIPE_BUFFER) return;   /* buffers are inlined at the draw (vertex / index / constant data) */
   {  /* diagnostic switches (sprint GPU 2026-11-10: recording leaves the App Library backdrop black for the rest of the session):
       * SWEETGL_FCAP_NOSNAP=1 no snapshots at all (ops only; such captures cannot be replayed), SWEETGL_FCAP_NOCPU=1 no CPU views */
      static int nosnap = -1, nocpu = -1;
      if (nosnap < 0) { const char *e = getenv("SWEETGL_FCAP_NOSNAP"); nosnap = e && e[0] == '1'; e = getenv("SWEETGL_FCAP_NOCPU"); nocpu = e && e[0] == '1';
                        if (nosnap || nocpu) fprintf(stderr, "sweetgl: FCAP diagnostic nosnap %d nocpu %d (GAP: captures incomplete)\n", nosnap, nocpu); }
      if (nosnap) return;
      if (!nocpu) fc_cpu_view(o);   /* before the GPU view, so fc_snap_gpu compares the two (FCAP COH) */
   }
   unsigned layers = r->target == PIPE_TEXTURE_3D ? r->depth0 : r->array_size;
   for (unsigned l = 0; l <= r->last_level; l++)
      for (unsigned z = 0; z < (r->target == PIPE_TEXTURE_3D ? u_minify(layers, l) : layers); z++)
         fc_snap_gpu(c, o, l, z, FCAP_ROLE_INITIAL);
}

/* Called by eagl_mesa.c whenever an IOSurface resource is bound (attach or texImageIOSurface). cpu = the app's CPU mapping while CA holds
 * the surface lock (texImageIOSurface only), else NULL. */
static void fcap_note_surface(struct pipe_resource *r, const struct sweet_gl_surface *s, int attach)
{
   if (!FC.installed || !r || !s) return;
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_res(r);
   if (!o->imported || o->surf_id != s->id) {
      o->imported = 1; o->surf_id = s->id; o->fourcc = s->format; o->pitch = s->bytes_per_row;
      o->scanout = s->format == FOURCC_B3A8 && s->w >= 1000 && s->h >= 1000;   /* the display surfaces (1125x2436); small 'b3a8' layer surfaces exist too */
      fc_res_def(o);
      static unsigned sl;
      if (o->scanout && sl < 8) { sl++; fprintf(stderr, "sweetgl: FCAP scanout surface %u res %u %ux%u (attach %d)\n", s->id, o->id, r->width0, r->height0, attach); }
   }
   (void)attach;
   if (fc_recording() && s->cpu && o->cpu_frame != FC.cur->seq + 1 && r->target != PIPE_BUFFER) {
      unsigned rowb = util_format_get_stride(r->format, r->width0), rows = util_format_get_nblocksy(r->format, r->height0);
      if ((uint64_t)s->bytes_per_row * (rows - 1) + rowb <= s->len && rowb <= s->bytes_per_row) {
         uint8_t *tmp = malloc((size_t)rowb * rows);
         if (!tmp) abort();
         for (unsigned y = 0; y < rows; y++) memcpy(tmp + (size_t)y * rowb, (const uint8_t *)s->cpu + (size_t)y * s->bytes_per_row, rowb);
         fc_emit_def(o);
         uint64_t hsh = fc_blob(tmp, rowb * rows);
         free(tmp);
         size_t at = fb_rec(&FC.cur->b, FC_CONTENT);
         fb_u32(&FC.cur->b, o->id); fb_u32(&FC.cur->b, 0); fb_u32(&FC.cur->b, 0); fb_u32(&FC.cur->b, r->width0); fb_u32(&FC.cur->b, r->height0);
         fb_u32(&FC.cur->b, rowb); fb_u32(&FC.cur->b, FCAP_VIEW_CPU); fb_u32(&FC.cur->b, FCAP_ROLE_INITIAL); fb_u64(&FC.cur->b, hsh); fb_u32(&FC.cur->b, FC.cur->seq);
         fb_end(&FC.cur->b, at);
         o->cpu_frame = FC.cur->seq + 1;
         o->cpu_hash = hsh;
      } else {
         fc_gap("CPU view of surface %u skipped: pitch %u x rows %u exceeds len %llu", s->id, s->bytes_per_row, rows, (unsigned long long)s->len);
      }
   }
   pthread_mutex_unlock(&FC.mu);
}

/* ---------------------------------------------------------------- screen hooks */
static struct pipe_resource *h_resource_create(struct pipe_screen *s, const struct pipe_resource *t)
{
   struct pipe_resource *r = FC.resource_create(s, t);
   if (r) { pthread_mutex_lock(&FC.mu); struct fobj *o = fc_new(r, FC_RES); o->res = r; fc_res_def(o); pthread_mutex_unlock(&FC.mu); }
   return r;
}
static struct pipe_resource *h_resource_from_handle(struct pipe_screen *s, const struct pipe_resource *t, struct winsys_handle *wh, unsigned u)
{
   struct pipe_resource *r = FC.resource_from_handle(s, t, wh, u);
   if (r) { pthread_mutex_lock(&FC.mu); struct fobj *o = fc_new(r, FC_RES); o->res = r; o->imported = 1; o->pitch = wh->stride; fc_res_def(o); pthread_mutex_unlock(&FC.mu); }
   return r;
}
static void h_resource_destroy(struct pipe_screen *s, struct pipe_resource *r)
{
   pthread_mutex_lock(&FC.mu); fc_forget(r); pthread_mutex_unlock(&FC.mu);
   FC.resource_destroy(s, r);
}

/* ---------------------------------------------------------------- context lookup */
static struct fctx *fc_ctx(struct pipe_context *p)
{
   for (int i = 0; i < FCAP_MAXCTX; i++) if (FC.ctx[i] && FC.ctx[i]->pctx == p) return FC.ctx[i];
   abort();   /* a hook ran on a context we never hooked: impossible by construction */
}
static int fc_stage(mesa_shader_stage s) { return s == MESA_SHADER_VERTEX ? 0 : s == MESA_SHADER_FRAGMENT ? 1 : -1; }

/* ---------------------------------------------------------------- CSO / shader hooks */
#define FCAP_W_U(n) fb_u32(&o->def, (uint32_t)(t->n));
#define FCAP_W_F(n) fb_f32(&o->def, (float)(t->n));

static void *h_create_blend(struct pipe_context *p, const struct pipe_blend_state *t)
{
   struct fctx *c = fc_ctx(p);
   void *cso = c->orig.create_blend_state(p, t);
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_new(cso, FC_CSO);
   size_t at = fb_rec(&o->def, FC_CSO); fb_u32(&o->def, FCAP_CSO_BLEND); fb_u32(&o->def, o->id);
   FCAP_BLEND_FIELDS(FCAP_W_U, FCAP_W_F)
   for (int i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
      const struct pipe_rt_blend_state *rt = &t->rt[i];
#define FCAP_W_RU(n) fb_u32(&o->def, (uint32_t)(rt->n));
      FCAP_RT_BLEND_FIELDS(FCAP_W_RU, FCAP_W_F)
#undef FCAP_W_RU
   }
   fb_end(&o->def, at);
   pthread_mutex_unlock(&FC.mu);
   return cso;
}
static void *h_create_rast(struct pipe_context *p, const struct pipe_rasterizer_state *t)
{
   struct fctx *c = fc_ctx(p);
   void *cso = c->orig.create_rasterizer_state(p, t);
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_new(cso, FC_CSO);
   o->cpe = t->clip_plane_enable; o->stipple = t->poly_stipple_enable;
   size_t at = fb_rec(&o->def, FC_CSO); fb_u32(&o->def, FCAP_CSO_RAST); fb_u32(&o->def, o->id);
   FCAP_RAST_FIELDS(FCAP_W_U, FCAP_W_F)
   fb_end(&o->def, at);
   pthread_mutex_unlock(&FC.mu);
   return cso;
}
static void *h_create_dsa(struct pipe_context *p, const struct pipe_depth_stencil_alpha_state *t)
{
   struct fctx *c = fc_ctx(p);
   void *cso = c->orig.create_depth_stencil_alpha_state(p, t);
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_new(cso, FC_CSO);
   size_t at = fb_rec(&o->def, FC_CSO); fb_u32(&o->def, FCAP_CSO_DSA); fb_u32(&o->def, o->id);
   for (int i = 0; i < 2; i++) {
      const struct pipe_stencil_state *st = &t->stencil[i];
#define FCAP_W_SU(n) fb_u32(&o->def, (uint32_t)(st->n));
      FCAP_STENCIL_FIELDS(FCAP_W_SU, FCAP_W_F)
#undef FCAP_W_SU
   }
   FCAP_DSA_FIELDS(FCAP_W_U, FCAP_W_F)
   fb_end(&o->def, at);
   pthread_mutex_unlock(&FC.mu);
   return cso;
}
static void *h_create_sampler(struct pipe_context *p, const struct pipe_sampler_state *t)
{
   struct fctx *c = fc_ctx(p);
   void *cso = c->orig.create_sampler_state(p, t);
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_new(cso, FC_CSO);
   size_t at = fb_rec(&o->def, FC_CSO); fb_u32(&o->def, FCAP_CSO_SAMPLER); fb_u32(&o->def, o->id);
   FCAP_SAMPLER_FIELDS(FCAP_W_U, FCAP_W_F)
   fb_end(&o->def, at);
   pthread_mutex_unlock(&FC.mu);
   return cso;
}
static void *h_create_velems(struct pipe_context *p, unsigned n, const struct pipe_vertex_element *e)
{
   struct fctx *c = fc_ctx(p);
   void *cso = c->orig.create_vertex_elements_state(p, n, e);
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_new(cso, FC_CSO);
   o->nve = n > PIPE_MAX_ATTRIBS ? PIPE_MAX_ATTRIBS : n;
   memcpy(o->ve, e, o->nve * sizeof *e);
   size_t at = fb_rec(&o->def, FC_CSO); fb_u32(&o->def, FCAP_CSO_VELEMS); fb_u32(&o->def, o->id); fb_u32(&o->def, o->nve);
   for (unsigned i = 0; i < o->nve; i++) {
      const struct pipe_vertex_element *t = &e[i];
      FCAP_VELEM_FIELDS(FCAP_W_U, FCAP_W_F)
   }
   fb_end(&o->def, at);
   pthread_mutex_unlock(&FC.mu);
   return cso;
}
static void fc_delete(struct pipe_context *p, void *cso) { (void)p; pthread_mutex_lock(&FC.mu); fc_forget(cso); pthread_mutex_unlock(&FC.mu); }
static void h_delete_blend(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_blend_state(p, s); }
static void h_delete_rast(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_rasterizer_state(p, s); }
static void h_delete_dsa(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_depth_stencil_alpha_state(p, s); }
static void h_delete_sampler(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_sampler_state(p, s); }
static void h_delete_velems(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_vertex_elements_state(p, s); }

static void fc_shader_def(void *so, const nir_shader *nir)
{
   struct blob bl; blob_init(&bl);
   nir_serialize(&bl, nir, false);
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_new(so, FC_SHADER);
   size_t at = fb_rec(&o->def, FC_SHADER); fb_u32(&o->def, o->id); fb_u32(&o->def, nir->info.stage); fb_u32(&o->def, (uint32_t)bl.size);
   fb_put(&o->def, bl.data, bl.size);
   fb_end(&o->def, at);
   pthread_mutex_unlock(&FC.mu);
   blob_finish(&bl);
}
static void *fc_create_shader(struct pipe_context *p, const struct pipe_shader_state *cso, int fs)
{
   struct fctx *c = fc_ctx(p);
   nir_shader *copy = NULL;
   if (cso->type == PIPE_SHADER_IR_NIR) {
      struct blob bl; blob_init(&bl); nir_serialize(&bl, cso->ir.nir, false);   /* serialize BEFORE the driver takes and lowers it */
      void *so = fs ? c->orig.create_fs_state(p, cso) : c->orig.create_vs_state(p, cso);
      if (so) {
         pthread_mutex_lock(&FC.mu);
         struct fobj *o = fc_new(so, FC_SHADER);
         size_t at = fb_rec(&o->def, FC_SHADER); fb_u32(&o->def, o->id); fb_u32(&o->def, fs ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX);
         fb_u32(&o->def, (uint32_t)bl.size); fb_put(&o->def, bl.data, bl.size); fb_end(&o->def, at);
         pthread_mutex_unlock(&FC.mu);
      }
      blob_finish(&bl);
      return so;
   }
   if (cso->type == PIPE_SHADER_IR_TGSI) {
      copy = tgsi_to_nir(cso->tokens, p->screen, false);   /* the same conversion panfrost does (pan_shader.c:505) */
      void *so = fs ? c->orig.create_fs_state(p, cso) : c->orig.create_vs_state(p, cso);
      if (so && copy) fc_shader_def(so, copy);
      if (copy) ralloc_free(copy);
      return so;
   }
   sweet_gap("fcap", "shader IR type %d not recorded", (int)cso->type);
   return fs ? c->orig.create_fs_state(p, cso) : c->orig.create_vs_state(p, cso);
}
static void *h_create_vs(struct pipe_context *p, const struct pipe_shader_state *s) { return fc_create_shader(p, s, 0); }
static void *h_create_fs(struct pipe_context *p, const struct pipe_shader_state *s) { return fc_create_shader(p, s, 1); }
static void h_delete_vs(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_vs_state(p, s); }
static void h_delete_fs(struct pipe_context *p, void *s) { fc_delete(p, s); fc_ctx(p)->orig.delete_fs_state(p, s); }

/* ---------------------------------------------------------------- bind / set hooks (state copies only) */
static void h_bind_blend(struct pipe_context *p, void *s) { fc_ctx(p)->blend = s; fc_ctx(p)->orig.bind_blend_state(p, s); }
static void h_bind_rast(struct pipe_context *p, void *s) { fc_ctx(p)->rast = s; fc_ctx(p)->orig.bind_rasterizer_state(p, s); }
static void h_bind_dsa(struct pipe_context *p, void *s) { fc_ctx(p)->dsa = s; fc_ctx(p)->orig.bind_depth_stencil_alpha_state(p, s); }
static void h_bind_vs(struct pipe_context *p, void *s) { fc_ctx(p)->vs = s; fc_ctx(p)->orig.bind_vs_state(p, s); }
static void h_bind_fs(struct pipe_context *p, void *s) { fc_ctx(p)->fs = s; fc_ctx(p)->orig.bind_fs_state(p, s); }
static void h_bind_velems(struct pipe_context *p, void *s) { fc_ctx(p)->velems = s; fc_ctx(p)->orig.bind_vertex_elements_state(p, s); }
static void h_bind_samplers(struct pipe_context *p, mesa_shader_stage sh, unsigned start, unsigned n, void **s)
{
   struct fctx *c = fc_ctx(p);
   int st = fc_stage(sh);
   if (st >= 0) {
      for (unsigned i = 0; i < n && start + i < FCAP_MAXSAMP; i++) c->samp[st][start + i] = s ? s[i] : NULL;
      unsigned top = 0;
      for (unsigned i = 0; i < FCAP_MAXSAMP; i++) if (c->samp[st][i]) top = i + 1;
      c->nsamp[st] = top;
   }
   c->orig.bind_sampler_states(p, sh, start, n, s);
}
static void h_set_views(struct pipe_context *p, mesa_shader_stage sh, unsigned start, unsigned n, unsigned unbind, struct pipe_sampler_view **v)
{
   struct fctx *c = fc_ctx(p);
   int st = fc_stage(sh);
   if (st >= 0) {
      for (unsigned i = 0; i < n && start + i < FCAP_MAXVIEWS; i++) c->view[st][start + i] = v ? v[i] : NULL;
      for (unsigned i = 0; i < unbind && start + n + i < FCAP_MAXVIEWS; i++) c->view[st][start + n + i] = NULL;
      unsigned top = 0;
      for (unsigned i = 0; i < FCAP_MAXVIEWS; i++) if (c->view[st][i]) top = i + 1;
      c->nview[st] = top;
   }
   c->orig.set_sampler_views(p, sh, start, n, unbind, v);
}
static void h_set_cb(struct pipe_context *p, mesa_shader_stage sh, uint index, const struct pipe_constant_buffer *cb)
{
   struct fctx *c = fc_ctx(p);
   int st = fc_stage(sh);
   if (st >= 0 && index < FCAP_MAXCB) {
      struct fcb *d = &c->cb[st][index];
      free(d->user); memset(d, 0, sizeof *d);
      if (cb && (cb->buffer || cb->user_buffer)) {
         d->set = 1; d->buf = cb->buffer; d->off = cb->buffer_offset; d->size = cb->buffer_size;
         if (!cb->buffer && cb->user_buffer && FC.installed) {   /* the user pointer is valid during this call only */
            d->user = malloc(cb->buffer_size ? cb->buffer_size : 1);
            if (!d->user) abort();
            memcpy(d->user, (const uint8_t *)cb->user_buffer + cb->buffer_offset, cb->buffer_size);
         }
      }
   }
   c->orig.set_constant_buffer(p, sh, index, cb);
}
static void h_set_vbs(struct pipe_context *p, unsigned n, const struct pipe_vertex_buffer *vb)
{
   struct fctx *c = fc_ctx(p);
   c->nvb = n > PIPE_MAX_ATTRIBS ? PIPE_MAX_ATTRIBS : n;
   if (vb) memcpy(c->vb, vb, c->nvb * sizeof *vb);
   else memset(c->vb, 0, sizeof c->vb);
   c->orig.set_vertex_buffers(p, n, vb);
}
static void h_set_fb(struct pipe_context *p, const struct pipe_framebuffer_state *fb)
{
   struct fctx *c = fc_ctx(p);
   c->fb = *fb; c->fb_frame = 0;
   c->fb_scanout = NULL;
   pthread_mutex_lock(&FC.mu);
   for (unsigned i = 0; i < fb->nr_cbufs && i < PIPE_MAX_COLOR_BUFS; i++) {
      struct fobj *o = fb->cbufs[i].texture ? fc_obj(fb->cbufs[i].texture) : NULL;
      if (o && o->scanout) c->fb_scanout = fb->cbufs[i].texture;
   }
   pthread_mutex_unlock(&FC.mu);
   c->orig.set_framebuffer_state(p, fb);
}
static void h_set_vp(struct pipe_context *p, unsigned s, unsigned n, const struct pipe_viewport_state *v)
{
   struct fctx *c = fc_ctx(p);
   if (s == 0 && n) c->vp = v[0];
   c->orig.set_viewport_states(p, s, n, v);
}
static void h_set_sc(struct pipe_context *p, unsigned s, unsigned n, const struct pipe_scissor_state *v)
{
   struct fctx *c = fc_ctx(p);
   if (s == 0 && n) c->sc = v[0];
   c->orig.set_scissor_states(p, s, n, v);
}
static void h_set_bc(struct pipe_context *p, const struct pipe_blend_color *v) { fc_ctx(p)->bc = *v; fc_ctx(p)->orig.set_blend_color(p, v); }
static void h_set_sr(struct pipe_context *p, const struct pipe_stencil_ref v) { fc_ctx(p)->sr = v; fc_ctx(p)->orig.set_stencil_ref(p, v); }
static void h_set_sm(struct pipe_context *p, unsigned v) { fc_ctx(p)->sample_mask = v; fc_ctx(p)->orig.set_sample_mask(p, v); }
static void h_set_ms(struct pipe_context *p, unsigned v) { fc_ctx(p)->min_samples = v; fc_ctx(p)->orig.set_min_samples(p, v); }
static void h_set_clip(struct pipe_context *p, const struct pipe_clip_state *v) { fc_ctx(p)->clip = *v; fc_ctx(p)->orig.set_clip_state(p, v); }

static struct pipe_sampler_view *h_create_view(struct pipe_context *p, struct pipe_resource *r, const struct pipe_sampler_view *t)
{
   struct fctx *c = fc_ctx(p);
   struct pipe_sampler_view *v = c->orig.create_sampler_view(p, r, t);
   if (v) {
      pthread_mutex_lock(&FC.mu);
      struct fobj *ro = fc_res(r);
      struct fobj *o = fc_new(v, FC_SVIEW);
      o->view_res = ro;
      size_t at = fb_rec(&o->def, FC_SVIEW);
      fb_u32(&o->def, o->id); fb_u32(&o->def, ro ? ro->id : 0); fb_u32(&o->def, t->format); fb_u32(&o->def, t->target);
      fb_u32(&o->def, t->swizzle_r); fb_u32(&o->def, t->swizzle_g); fb_u32(&o->def, t->swizzle_b); fb_u32(&o->def, t->swizzle_a);
      int isbuf = r && r->target == PIPE_BUFFER;
      fb_u32(&o->def, isbuf ? 0 : t->u.tex.first_level); fb_u32(&o->def, isbuf ? 0 : t->u.tex.last_level);
      fb_u32(&o->def, isbuf ? 0 : t->u.tex.first_layer); fb_u32(&o->def, isbuf ? 0 : t->u.tex.last_layer);
      fb_u32(&o->def, isbuf); fb_u32(&o->def, isbuf ? t->u.buf.offset : 0); fb_u32(&o->def, isbuf ? t->u.buf.size : 0);
      fb_end(&o->def, at);
      pthread_mutex_unlock(&FC.mu);
   }
   return v;
}
static void h_view_destroy(struct pipe_context *p, struct pipe_sampler_view *v)
{
   pthread_mutex_lock(&FC.mu); fc_forget(v); pthread_mutex_unlock(&FC.mu);
   fc_ctx(p)->orig.sampler_view_destroy(p, v);
}

/* ---------------------------------------------------------------- op recording (caller holds FC.mu, recording) */
static void fc_surf(struct fbuf *b, const struct pipe_surface *s)
{
   struct fobj *o = s->texture ? fc_res(s->texture) : NULL;
   fb_u32(b, o ? o->id : 0); fb_u32(b, s->texture ? s->format : 0); fb_u32(b, s->level); fb_u32(b, s->first_layer); fb_u32(b, s->last_layer);
}

static void fc_rec_fb(struct fctx *c)
{
   FC.cur->ctx = c;
   for (unsigned i = 0; i < c->fb.nr_cbufs && i < PIPE_MAX_COLOR_BUFS; i++) if (c->fb.cbufs[i].texture) {
      fc_touch(c, c->fb.cbufs[i].texture);
      struct fobj *o = fc_res(c->fb.cbufs[i].texture);
      if (o && o->scanout) FC.cur->scanout = c->fb.cbufs[i].texture;
   }
   if (c->fb.zsbuf.texture) fc_touch(c, c->fb.zsbuf.texture);
   if (c->fb_frame == FC.cur->seq + 1) return;
   struct fbuf *b = &FC.cur->b;
   size_t at = fb_rec(b, FC_FB);
   fb_u32(b, c->fb.width); fb_u32(b, c->fb.height); fb_u32(b, c->fb.layers); fb_u32(b, c->fb.samples); fb_u32(b, c->fb.nr_cbufs);
   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
      static const struct pipe_surface none;
      fc_surf(b, i < c->fb.nr_cbufs ? &c->fb.cbufs[i] : &none);
   }
   fc_surf(b, &c->fb.zsbuf);
   fb_end(b, at);
   c->fb_frame = FC.cur->seq + 1;
   if (c->fb.resolve) fc_gap("framebuffer resolve attachment not recorded");
}

/* Bytes [off, off+size) of a buffer resource (map READ, unsynchronized: the CPU wrote it, nothing GPU-side is pending on it). */
static int fc_read_buf(struct fctx *c, struct pipe_resource *r, unsigned off, unsigned size, uint8_t *dst)
{
   if (!r || (uint64_t)off + size > r->width0) return -1;
   if (!size) return 0;
   struct pipe_box box; u_box_1d((int)off, (int)size, &box);
   struct pipe_transfer *tr = NULL;
   fc_depth++;
   uint8_t *p = c->orig.buffer_map(c->pctx, r, 0, PIPE_MAP_READ | PIPE_MAP_UNSYNCHRONIZED, &box, &tr);
   if (p) { memcpy(dst, p, size); c->orig.buffer_unmap(c->pctx, tr); }
   fc_depth--;
   return p ? 0 : -1;
}

static void fc_rec_state(struct fctx *c, unsigned minv, unsigned maxv, unsigned mini, unsigned maxi)
{
   struct fbuf *b = &FC.cur->b;
   struct fobj *ob[6] = { fc_obj(c->blend), fc_obj(c->rast), fc_obj(c->dsa), fc_obj(c->vs), fc_obj(c->fs), fc_obj(c->velems) };
   void *raw[6] = { c->blend, c->rast, c->dsa, c->vs, c->fs, c->velems };
   for (int i = 0; i < 6; i++) { if (raw[i] && !ob[i]) fc_gap("bound object %d has no record (created before the hooks?)", i); fc_emit_def(ob[i]); }
   for (int st = 0; st < FCAP_STAGES; st++) {
      for (unsigned i = 0; i < c->nview[st]; i++) if (c->view[st][i]) {
         struct fobj *vo = fc_obj(c->view[st][i]);
         if (!vo) { fc_gap("sampler view without record (stage %d slot %u)", st, i); continue; }
         if (c->view[st][i]->texture) fc_touch(c, c->view[st][i]->texture);
         fc_emit_def(vo);
      }
      for (unsigned i = 0; i < c->nsamp[st]; i++) fc_emit_def(fc_obj(c->samp[st][i]));
   }
   size_t at = fb_rec(b, FC_STATE);
   for (int i = 0; i < 6; i++) fb_u32(b, ob[i] ? ob[i]->id : 0);
   for (int i = 0; i < 4; i++) fb_f32(b, c->bc.color[i]);
   fb_u32(b, c->sr.ref_value[0]); fb_u32(b, c->sr.ref_value[1]); fb_u32(b, c->sample_mask); fb_u32(b, c->min_samples);
   {
      const struct pipe_viewport_state *t = &c->vp;
#define FCAP_W_BU(n) fb_u32(b, (uint32_t)(t->n));
#define FCAP_W_BF(n) fb_f32(b, (float)(t->n));
      FCAP_VIEWPORT_FIELDS(FCAP_W_BU, FCAP_W_BF)
   }
   fb_u32(b, c->sc.minx); fb_u32(b, c->sc.miny); fb_u32(b, c->sc.maxx); fb_u32(b, c->sc.maxy);
   for (int st = 0; st < FCAP_STAGES; st++) {
      fb_u32(b, c->nview[st]);
      for (unsigned i = 0; i < c->nview[st]; i++) { struct fobj *vo = fc_obj(c->view[st][i]); fb_u32(b, vo ? vo->id : 0); }
      fb_u32(b, c->nsamp[st]);
      for (unsigned i = 0; i < c->nsamp[st]; i++) { struct fobj *so = fc_obj(c->samp[st][i]); fb_u32(b, so ? so->id : 0); }
      unsigned ncb = 0;
      for (unsigned i = 0; i < FCAP_MAXCB; i++) ncb += c->cb[st][i].set;
      fb_u32(b, ncb);
      for (unsigned i = 0; i < FCAP_MAXCB; i++) if (c->cb[st][i].set) {
         struct fcb *d = &c->cb[st][i];
         uint8_t *tmp = d->user;
         int own = 0;
         if (!tmp) {
            tmp = malloc(d->size ? d->size : 1); own = 1;
            if (!tmp) abort();
            if (fc_read_buf(c, d->buf, d->off, d->size, tmp)) { memset(tmp, 0, d->size); fc_gap("constant buffer stage %d slot %u unreadable", st, i); }
         }
         fb_u32(b, i); fb_u32(b, d->size); fb_u64(b, fc_blob(tmp, d->size));
         if (own) free(tmp);
      }
   }
   /* vertex buffers: the byte range the elements reach for vertex indices minv..maxv / instances mini..maxi */
   struct fobj *ve = fc_obj(c->velems);
   fb_u32(b, c->nvb);
   for (unsigned v = 0; v < c->nvb; v++) {
      uint64_t lo = UINT64_MAX, hi = 0;
      for (unsigned e = 0; ve && e < ve->nve; e++) {
         const struct pipe_vertex_element *el = &ve->ve[e];
         if (el->vertex_buffer_index != v) continue;
         unsigned a = el->instance_divisor ? mini / el->instance_divisor : minv, z = el->instance_divisor ? maxi / el->instance_divisor : maxv;
         uint64_t l = (uint64_t)el->src_offset + (uint64_t)el->src_stride * a;
         uint64_t h = (uint64_t)el->src_offset + (uint64_t)el->src_stride * z + util_format_get_blocksize((enum pipe_format)el->src_format);
         if (l < lo) lo = l;
         if (h > hi) hi = h;
      }
      if (lo == UINT64_MAX || hi > (64u << 20)) {
         if (lo != UINT64_MAX) fc_gap("vertex buffer %u range %llu..%llu too large", v, (unsigned long long)lo, (unsigned long long)hi);
         fb_u64(b, 0); fb_u32(b, 0); fb_u32(b, 0);
         continue;
      }
      uint32_t size = (uint32_t)(hi - lo);
      uint8_t *tmp = malloc(size ? size : 1);
      if (!tmp) abort();
      if (c->vb[v].is_user_buffer) memcpy(tmp, (const uint8_t *)c->vb[v].buffer.user + c->vb[v].buffer_offset + lo, size);
      else if (fc_read_buf(c, c->vb[v].buffer.resource, (unsigned)(c->vb[v].buffer_offset + lo), size, tmp)) { memset(tmp, 0, size); fc_gap("vertex buffer %u unreadable", v); }
      fb_u64(b, fc_blob(tmp, size)); fb_u32(b, size); fb_u32(b, (uint32_t)lo);
      free(tmp);
   }
   struct fobj *ro = fc_obj(c->rast);
   int clip = ro && ro->cpe;
   if (ro && ro->stipple) fc_gap("polygon stipple enabled: stipple pattern not recorded");
   fb_u32(b, clip ? 8 : 0);
   if (clip) for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) fb_f32(b, c->clip.ucp[i][j]);
   fb_end(b, at);
}

static void fc_depflush_check(struct fctx *c, struct pipe_resource *reads0, int check_views)
{
   if (fc_depflush < 0) { const char *e = getenv("SWEETGL_FCAP_DEPFLUSH"); fc_depflush = e && e[0] == '1'; if (fc_depflush) fprintf(stderr, "sweetgl: FCAP DEPFLUSH on\n"); }
   if (!fc_depflush || fc_depth || !fc_ndirty) return;
   int hit = reads0 && fc_is_dirty(reads0);
   for (int st = 0; check_views && !hit && st < FCAP_STAGES; st++)
      for (unsigned i = 0; i < c->nview[st] && !hit; i++) {
         struct pipe_sampler_view *v = c->view[st][i];
         if (!v || !v->texture || !fc_is_dirty(v->texture)) continue;
         int self = 0;
         for (unsigned k = 0; k < c->fb.nr_cbufs; k++) if (c->fb.cbufs[k].texture == v->texture) self = 1;   /* render-to-self is the st's business */
         if (!self) hit = 1;
      }
   if (!hit) return;
   fc_depth++; c->orig.flush(c->pctx, NULL, 0); fc_depth--;
   fc_ndirty = 0;
   if ((++fc_depflushes % 500) == 1) fprintf(stderr, "sweetgl: FCAP DEPFLUSH flushes %u\n", fc_depflushes);
}

static void h_draw(struct pipe_context *p, const struct pipe_draw_info *info, unsigned drawid, const struct pipe_draw_indirect_info *ind,
                   const struct pipe_draw_start_count_bias *draws, unsigned nd)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && c->fb_scanout) FC.tap_scanout = c->fb_scanout;
   if (!fc_depth) {
      fc_depflush_check(c, NULL, 1);
      for (unsigned k = 0; k < c->fb.nr_cbufs && k < PIPE_MAX_COLOR_BUFS; k++) if (fc_depflush == 1) fc_dirty_add(c->fb.cbufs[k].texture);
   }
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      if (ind) fc_gap("indirect draw not recorded");
      else {
         uint8_t *idx = NULL; size_t isz = 0;
         unsigned minv = UINT32_MAX, maxv = 0;
         if (info->index_size) {
            unsigned lo = UINT32_MAX, hi = 0;
            for (unsigned d = 0; d < nd; d++) { if (draws[d].start < lo) lo = draws[d].start; if (draws[d].start + draws[d].count > hi) hi = draws[d].start + draws[d].count; }
            isz = (size_t)(hi - lo) * info->index_size;
            idx = malloc(isz ? isz : 1);
            if (!idx) abort();
            if (info->has_user_indices) memcpy(idx, (const uint8_t *)info->index.user + (size_t)lo * info->index_size, isz);
            else if (fc_read_buf(c, info->index.resource, lo * info->index_size, (unsigned)isz, idx)) { memset(idx, 0, isz); fc_gap("index buffer unreadable"); }
            for (unsigned d = 0; d < nd; d++) for (unsigned k = 0; k < draws[d].count; k++) {
               size_t at = (size_t)(draws[d].start - lo + k);
               unsigned v = info->index_size == 1 ? idx[at] : info->index_size == 2 ? ((uint16_t *)idx)[at] : ((uint32_t *)idx)[at];
               if (info->primitive_restart && v == info->restart_index) continue;
               int64_t vv = (int64_t)v + draws[d].index_bias;
               if (vv < 0) vv = 0;
               if (vv < minv) minv = (unsigned)vv;
               if (vv > maxv) maxv = (unsigned)vv;
            }
            /* rebase: the recorded index data starts at index `lo`, so the replay draws start - lo */
            if (minv == UINT32_MAX) minv = maxv = 0;
            fc_rec_fb(c);
            fc_rec_state(c, minv, maxv, info->start_instance, info->start_instance + (info->instance_count ? info->instance_count - 1 : 0));
            struct fbuf *b = &FC.cur->b;
            size_t at = fb_rec(b, FC_DRAW);
            fb_u32(b, info->mode); fb_u32(b, info->index_size); fb_u32(b, info->primitive_restart); fb_u32(b, info->restart_index);
            fb_u32(b, info->start_instance); fb_u32(b, info->instance_count); fb_u32(b, drawid); fb_u64(b, fc_blob(idx, (uint32_t)isz)); fb_u32(b, nd);
            for (unsigned d = 0; d < nd; d++) { fb_u32(b, draws[d].start - lo); fb_u32(b, draws[d].count); fb_i32(b, draws[d].index_bias); }
            fb_end(b, at);
            free(idx);
         } else {
            for (unsigned d = 0; d < nd; d++) {
               if (draws[d].start < minv) minv = draws[d].start;
               if (draws[d].count && draws[d].start + draws[d].count - 1 > maxv) maxv = draws[d].start + draws[d].count - 1;
            }
            if (minv == UINT32_MAX) minv = maxv = 0;
            fc_rec_fb(c);
            fc_rec_state(c, minv, maxv, info->start_instance, info->start_instance + (info->instance_count ? info->instance_count - 1 : 0));
            struct fbuf *b = &FC.cur->b;
            size_t at = fb_rec(b, FC_DRAW);
            fb_u32(b, info->mode); fb_u32(b, 0); fb_u32(b, 0); fb_u32(b, 0);
            fb_u32(b, info->start_instance); fb_u32(b, info->instance_count); fb_u32(b, drawid); fb_u64(b, 0); fb_u32(b, nd);
            for (unsigned d = 0; d < nd; d++) { fb_u32(b, draws[d].start); fb_u32(b, draws[d].count); fb_i32(b, 0); }
            fb_end(b, at);
         }
         FC.cur->ops++; FC.cur->draws++;
      }
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.draw_vbo(p, info, drawid, ind, draws, nd); fc_depth--;
}

static void h_clear(struct pipe_context *p, unsigned buffers, uint32_t cmask, uint8_t smask, const struct pipe_scissor_state *sc,
                    const union pipe_color_union *col, double depth, unsigned stencil)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      fc_rec_fb(c);
      struct fbuf *b = &FC.cur->b;
      size_t at = fb_rec(b, FC_CLEAR);
      fb_u32(b, buffers); fb_u32(b, cmask); fb_u32(b, smask); fb_u32(b, sc != NULL);
      fb_u32(b, sc ? sc->minx : 0); fb_u32(b, sc ? sc->miny : 0); fb_u32(b, sc ? sc->maxx : 0); fb_u32(b, sc ? sc->maxy : 0);
      for (int i = 0; i < 4; i++) fb_u32(b, col ? col->ui[i] : 0);
      fb_f64(b, depth); fb_u32(b, stencil);
      fb_end(b, at);
      FC.cur->ops++;
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.clear(p, buffers, cmask, smask, sc, col, depth, stencil); fc_depth--;
}

static void h_clear_rt(struct pipe_context *p, struct pipe_surface *s, const union pipe_color_union *col, unsigned x, unsigned y, unsigned w,
                       unsigned h, bool rc)
{
   struct fctx *c = fc_ctx(p);
   fc_tap_note(s->texture);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      if (s->texture) fc_touch(c, s->texture);
      struct fobj *o = s->texture ? fc_res(s->texture) : NULL;
      if (o && o->scanout) FC.cur->scanout = s->texture;
      struct fbuf *b = &FC.cur->b;
      size_t at = fb_rec(b, FC_CLEAR_RT);
      fc_surf(b, s);
      for (int i = 0; i < 4; i++) fb_u32(b, col->ui[i]);
      fb_u32(b, x); fb_u32(b, y); fb_u32(b, w); fb_u32(b, h); fb_u32(b, rc);
      fb_end(b, at);
      FC.cur->ops++;
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.clear_render_target(p, s, col, x, y, w, h, rc); fc_depth--;
}

static void h_clear_ds(struct pipe_context *p, struct pipe_surface *s, unsigned flags, double depth, unsigned stencil, unsigned x, unsigned y,
                       unsigned w, unsigned h, bool rc)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      if (s->texture) fc_touch(c, s->texture);
      struct fbuf *b = &FC.cur->b;
      size_t at = fb_rec(b, FC_CLEAR_DS);
      fc_surf(b, s);
      fb_u32(b, flags); fb_f64(b, depth); fb_u32(b, stencil); fb_u32(b, x); fb_u32(b, y); fb_u32(b, w); fb_u32(b, h); fb_u32(b, rc);
      fb_end(b, at);
      FC.cur->ops++;
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.clear_depth_stencil(p, s, flags, depth, stencil, x, y, w, h, rc); fc_depth--;
}

static void fc_box(struct fbuf *b, const struct pipe_box *x) { fb_i32(b, x->x); fb_i32(b, x->y); fb_i32(b, x->z); fb_i32(b, x->width); fb_i32(b, x->height); fb_i32(b, x->depth); }

static void fc_tap_note(struct pipe_resource *dst)
{
   if (fc_depth || !dst) return;
   pthread_mutex_lock(&FC.mu);
   struct fobj *o = fc_obj(dst);
   if (o && o->scanout) FC.tap_scanout = dst;
   pthread_mutex_unlock(&FC.mu);
}
static void h_blit(struct pipe_context *p, const struct pipe_blit_info *bi)
{
   struct fctx *c = fc_ctx(p);
   fc_tap_note(bi->dst.resource);
   fc_depflush_check(c, bi->src.resource, 0);
   if (fc_depflush == 1 && !fc_depth) fc_dirty_add(bi->dst.resource);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      FC.cur->ctx = c;
      fc_touch(c, bi->src.resource); fc_touch(c, bi->dst.resource);
      struct fobj *d = fc_res(bi->dst.resource), *s = fc_res(bi->src.resource);
      if (d && d->scanout) FC.cur->scanout = bi->dst.resource;
      if (bi->num_window_rectangles) fc_gap("blit window rectangles not recorded");
      struct fbuf *b = &FC.cur->b;
      size_t at = fb_rec(b, FC_BLIT);
      fb_u32(b, d ? d->id : 0); fb_u32(b, bi->dst.level); fc_box(b, &bi->dst.box); fb_u32(b, bi->dst.format);
      fb_u32(b, s ? s->id : 0); fb_u32(b, bi->src.level); fc_box(b, &bi->src.box); fb_u32(b, bi->src.format);
      fb_u32(b, bi->mask); fb_u32(b, bi->filter); fb_u32(b, bi->scissor_enable);
      fb_u32(b, bi->scissor.minx); fb_u32(b, bi->scissor.miny); fb_u32(b, bi->scissor.maxx); fb_u32(b, bi->scissor.maxy);
      fb_u32(b, bi->alpha_blend); fb_u32(b, bi->swizzle_enable);
      for (int i = 0; i < 4; i++) fb_u32(b, bi->swizzle[i]);
      fb_u32(b, bi->render_condition_enable); fb_u32(b, bi->dst_sample); fb_u32(b, bi->sample0_only);
      fb_end(b, at);
      FC.cur->ops++;
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.blit(p, bi); fc_depth--;
}

static void h_copy(struct pipe_context *p, struct pipe_resource *dst, unsigned dl, unsigned dx, unsigned dy, unsigned dz, struct pipe_resource *src,
                   unsigned sl, const struct pipe_box *box)
{
   struct fctx *c = fc_ctx(p);
   fc_tap_note(dst);
   fc_depflush_check(c, src, 0);
   if (fc_depflush == 1 && !fc_depth) fc_dirty_add(dst);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      if (dst->target == PIPE_BUFFER || src->target == PIPE_BUFFER) fc_gap("buffer resource_copy_region not recorded");
      else {
         fc_touch(c, src); fc_touch(c, dst);
         struct fobj *d = fc_res(dst), *s = fc_res(src);
         if (d && d->scanout) FC.cur->scanout = dst;
         struct fbuf *b = &FC.cur->b;
         size_t at = fb_rec(b, FC_COPY);
         fb_u32(b, d->id); fb_u32(b, dl); fb_u32(b, dx); fb_u32(b, dy); fb_u32(b, dz); fb_u32(b, s->id); fb_u32(b, sl); fc_box(b, box);
         fb_end(b, at);
         FC.cur->ops++;
      }
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.resource_copy_region(p, dst, dl, dx, dy, dz, src, sl, box); fc_depth--;
}

static void fc_rec_texwrite(struct fctx *c, struct pipe_resource *r, unsigned level, const struct pipe_box *box, const void *data, unsigned stride,
                            uintptr_t lstride)
{
   fc_touch(c, r);
   if (util_format_is_compressed(r->format)) return;   /* recorded as a decoded snapshot after the write (fc_after_texwrite) */
   struct fobj *o = fc_res(r);
   unsigned rowb = util_format_get_stride(r->format, (unsigned)box->width), rows = util_format_get_nblocksy(r->format, (unsigned)box->height);
   size_t n = (size_t)rowb * rows * (size_t)box->depth;
   uint8_t *tmp = malloc(n ? n : 1);
   if (!tmp) abort();
   for (int z = 0; z < box->depth; z++)
      for (unsigned y = 0; y < rows; y++)
         memcpy(tmp + ((size_t)z * rows + y) * rowb, (const uint8_t *)data + (size_t)z * lstride + (size_t)y * stride, rowb);
   struct fbuf *b = &FC.cur->b;
   size_t at = fb_rec(b, FC_TEXWRITE);
   fb_u32(b, o->id); fb_u32(b, level); fc_box(b, box); fb_u32(b, rowb); fb_u32(b, rowb * rows); fb_u64(b, fc_blob(tmp, (uint32_t)n));
   fb_end(b, at);
   FC.cur->ops++;
   free(tmp);
}

static void fc_after_texwrite(struct fctx *c, struct pipe_resource *r, unsigned level, const struct pipe_box *box)
{
   if (!util_format_is_compressed(r->format)) return;
   pthread_mutex_lock(&FC.mu);
   if (fc_recording()) {
      struct fobj *o = fc_res(r);
      for (int z = box->z; z < box->z + box->depth; z++) fc_snap_gpu(c, o, level, (unsigned)z, FCAP_ROLE_INITIAL);
   }
   pthread_mutex_unlock(&FC.mu);
}

static void h_texture_subdata(struct pipe_context *p, struct pipe_resource *r, unsigned level, unsigned usage, const struct pipe_box *box,
                              const void *data, unsigned stride, uintptr_t lstride)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      fc_rec_texwrite(c, r, level, box, data, stride, lstride);
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.texture_subdata(p, r, level, usage, box, data, stride, lstride); fc_depth--;
   if (!fc_depth) fc_after_texwrite(c, r, level, box);
}

/* Direct texture maps for writing (st paths that do not go through texture_subdata): the written box is recorded at unmap. */
#define FCAP_MAXMAP 16
static struct { struct pipe_transfer *tr; void *ptr; } fc_maps[FCAP_MAXMAP];
static void *h_texture_map(struct pipe_context *p, struct pipe_resource *r, unsigned level, unsigned usage, const struct pipe_box *box,
                           struct pipe_transfer **out)
{
   struct fctx *c = fc_ctx(p);
   int rec = !fc_depth && fc_recording() && (usage & PIPE_MAP_WRITE);
   if (rec) {
      pthread_mutex_lock(&FC.mu);
      fc_touch(c, r);   /* initial content BEFORE the CPU writes into the mapping */
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++;
   void *ptr = c->orig.texture_map(p, r, level, usage, box, out);
   fc_depth--;
   if (rec && ptr) {
      pthread_mutex_lock(&FC.mu);
      int i = 0;
      while (i < FCAP_MAXMAP && fc_maps[i].tr) i++;
      if (i < FCAP_MAXMAP) { fc_maps[i].tr = *out; fc_maps[i].ptr = ptr; }
      else fc_gap("more than %d concurrent texture write maps", FCAP_MAXMAP);
      pthread_mutex_unlock(&FC.mu);
   }
   return ptr;
}
static void h_texture_unmap(struct pipe_context *p, struct pipe_transfer *tr)
{
   struct fctx *c = fc_ctx(p);
   struct pipe_resource *wres = NULL; unsigned wlevel = 0; struct pipe_box wbox;
   if (FC.installed) {
      pthread_mutex_lock(&FC.mu);
      for (int i = 0; i < FCAP_MAXMAP; i++) if (fc_maps[i].tr == tr) {
         if (fc_recording()) fc_rec_texwrite(c, tr->resource, tr->level, &tr->box, fc_maps[i].ptr, tr->stride, tr->layer_stride);
         pipe_resource_reference(&wres, tr->resource); wlevel = tr->level; wbox = tr->box;
         fc_maps[i].tr = NULL; fc_maps[i].ptr = NULL;
      }
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.texture_unmap(p, tr); fc_depth--;
   if (wres) { if (!fc_depth) fc_after_texwrite(c, wres, wlevel, &wbox); pipe_resource_reference(&wres, NULL); }
}

static bool h_genmip(struct pipe_context *p, struct pipe_resource *r, enum pipe_format f, unsigned bl, unsigned ll, unsigned fl, unsigned la)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      fc_touch(c, r);
      struct fbuf *b = &FC.cur->b;
      size_t at = fb_rec(b, FC_MIPGEN);
      fb_u32(b, fc_res(r)->id); fb_u32(b, f); fb_u32(b, bl); fb_u32(b, ll); fb_u32(b, fl); fb_u32(b, la);
      fb_end(b, at);
      FC.cur->ops++;
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; bool ok = c->orig.generate_mipmap(p, r, f, bl, ll, fl, la); fc_depth--;
   return ok;
}

static void h_tex_barrier(struct pipe_context *p, unsigned flags)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) {
      pthread_mutex_lock(&FC.mu);
      size_t at = fb_rec(&FC.cur->b, FC_BARRIER); fb_u32(&FC.cur->b, 1); fb_u32(&FC.cur->b, flags); fb_end(&FC.cur->b, at);
      pthread_mutex_unlock(&FC.mu);
   }
   fc_depth++; c->orig.texture_barrier(p, flags); fc_depth--;
}

static void h_launch_grid(struct pipe_context *p, const struct pipe_grid_info *g)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) { pthread_mutex_lock(&FC.mu); fc_gap("compute launch_grid not recorded"); pthread_mutex_unlock(&FC.mu); }
   fc_depth++; c->orig.launch_grid(p, g); fc_depth--;
}

static void h_clear_texture(struct pipe_context *p, struct pipe_resource *r, unsigned level, const struct pipe_box *box, const void *data)
{
   struct fctx *c = fc_ctx(p);
   if (!fc_depth && fc_recording()) { pthread_mutex_lock(&FC.mu); fc_gap("clear_texture not recorded"); pthread_mutex_unlock(&FC.mu); }
   fc_depth++; c->orig.clear_texture(p, r, level, box, data); fc_depth--;
}

/* ---------------------------------------------------------------- scanout tap (defect 3: what the display really showed, at real timing)
 * While kern.sweetnavcap bit 1 is set (the kernel sets it around chosen actions of a measured repetition), every flush that drew a display
 * surface blits it at half resolution into a ring of staging textures (GPU copy, no CPU stall) with the kernel's clock (mach absolute time in
 * ns, the clock of SWEET SWAPT / SWEET NAV). When the bit clears, the ring is read back and written as /private/var/dev/fcap/pPID-tapN.ftap. */
#define FCAP_TAPN 90
struct ftap { struct pipe_resource *st[FCAP_TAPN]; uint64_t t[FCAP_TAPN]; unsigned n, on, seq, dropped, head; } TAP;   /* ring: the LAST FCAP_TAPN frames survive */
static uint64_t fc_kns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void fc_tap_frame(struct fctx *c, struct pipe_resource *src)
{
   unsigned slot = TAP.head;
   if (TAP.n == FCAP_TAPN) TAP.dropped++;   /* overwrite the oldest */
   unsigned w = src->width0 / 4, h = src->height0 / 4;   /* quarter resolution: 90 frames ~ 62 MB */
   if (!TAP.st[slot]) {
      struct pipe_resource t; memset(&t, 0, sizeof t);
      t.target = PIPE_TEXTURE_2D; t.format = PIPE_FORMAT_B8G8R8A8_UNORM; t.width0 = w; t.height0 = h; t.depth0 = 1; t.array_size = 1;
      t.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
      TAP.st[slot] = FC.resource_create(FC.scr, &t);   /* the original: staging textures stay out of the capture tables */
      if (!TAP.st[slot]) { TAP.dropped++; sweet_gap("fcap", "tap staging texture %ux%u not created", w, h); return; }
   }
   struct pipe_blit_info bi; memset(&bi, 0, sizeof bi);
   bi.dst.resource = TAP.st[slot]; bi.dst.format = PIPE_FORMAT_B8G8R8A8_UNORM; u_box_2d(0, 0, (int)w, (int)h, &bi.dst.box);
   bi.src.resource = src; bi.src.format = src->format; u_box_2d(0, 0, (int)src->width0, (int)src->height0, &bi.src.box);
   bi.mask = PIPE_MASK_RGBA; bi.filter = PIPE_TEX_FILTER_LINEAR;
   fc_depth++; c->orig.blit(c->pctx, &bi); c->orig.flush(c->pctx, NULL, 0); fc_depth--;
   TAP.t[slot] = fc_kns();
   TAP.head = (slot + 1) % FCAP_TAPN;
   if (TAP.n < FCAP_TAPN) TAP.n++;
   FC.n_tapf++;
}
static void fc_tap_write(struct fctx *c);   /* defined after the part writer */
/* ---------------------------------------------------------------- frame end (be_flush) */
static void fcap_on_flush(void)
{
   if (!FC.installed) return;
   if (!fc_depth && FC.tap_scanout && FC.ctx[0]) {
      struct fctx *c = FC.ctx[0];
      if (FC.tap_sysctl) { if (!TAP.on) fprintf(stderr, "sweetgl: FCAP TAP start\n"); TAP.on = 1; fc_tap_frame(c, FC.tap_scanout); }
      else if (TAP.on) { TAP.on = 0; fc_tap_write(c); }
   } else if (TAP.on && !FC.tap_sysctl && FC.ctx[0]) {
      TAP.on = 0; fc_tap_write(FC.ctx[0]);
   }
   FC.tap_scanout = NULL;
   pthread_mutex_lock(&FC.mu);
   if (!FC.cur) fc_frame_start();
   if (fc_recording() && FC.cur->scanout) {
      struct fctx *c = FC.cur->ctx ? FC.cur->ctx : FC.ctx[0];
      struct fobj *o = fc_res(FC.cur->scanout);
      fc_emit_def(o);
      fc_snap_gpu(c, o, 0, 0, FCAP_ROLE_RESULT);
      size_t at = fb_rec(&FC.cur->b, FC_PRESENT); fb_u32(&FC.cur->b, o->id); fb_u32(&FC.cur->b, FC.cur->seq); fb_end(&FC.cur->b, at);
      if (FC.ring[FCAP_NFR - 1]) {
         if (FC.ring[FCAP_NFR - 1]->refs == 0) fc_frame_free(FC.ring[FCAP_NFR - 1]);
         else FC.ring[FCAP_NFR - 1]->orphan = 1;   /* the writer frees it when done */
      }
      memmove(&FC.ring[1], &FC.ring[0], (FCAP_NFR - 1) * sizeof FC.ring[0]);
      FC.n_frames++;
      FC.ring[0] = FC.cur;
      if (FC.win_reset) {   /* a new recording window: drop the first frames of the previous one */
         for (unsigned i = 0; i < FC.nfirst; i++) if (--FC.first[i]->refs == 0 && FC.first[i]->orphan) fc_frame_free(FC.first[i]);
         FC.nfirst = 0; FC.win_reset = 0;
      }
      if (FC.nfirst < FC.want_first) { FC.first[FC.nfirst++] = FC.cur; FC.cur->refs++; }
      fc_frame_start();
      {  /* diagnostic SWEETGL_FCAP_SLOWMS=N (sprint GPU 2026-11-10 defect 1): stall N ms per recorded frame, to tell "slow frames" apart from
          * the snapshot side effects when combined with SWEETGL_FCAP_NOSNAP=1 */
         static int slowms = -1;
         if (slowms < 0) { const char *e = getenv("SWEETGL_FCAP_SLOWMS"); slowms = e ? atoi(e) : 0; if (slowms) fprintf(stderr, "sweetgl: FCAP diagnostic slowms %d (GAP: recorded frames stalled on purpose)\n", slowms); }
         if (slowms > 0) usleep((useconds_t)slowms * 1000u);
      }
   } else if (!fc_recording() && FC.cur->ops) {
      fc_frame_free(FC.cur);   /* recording was switched off mid-frame */
      fc_frame_start();
   }
   pthread_mutex_unlock(&FC.mu);
}

/* Frame end: CA's glFlush reaches Mesa through the GLI table directly (slot 90 = _mesa_Flush, not sgl_flush), so the frame boundary is the
 * Gallium flush the st makes for it. A flush ends a frame only if the display surface was drawn since the last frame end. */
static void h_flush(struct pipe_context *p, struct pipe_fence_handle **fence, unsigned flags)
{
   struct fctx *c = fc_ctx(p);
   fc_depth++; c->orig.flush(p, fence, flags); fc_depth--;
   if (!fc_depth) { fc_ndirty = 0; FC.n_flush++; fcap_on_flush(); }
}

/* ---------------------------------------------------------------- writer thread */
/* Output in parts of at most FCAP_PART bytes (devd pulls at most 40 MB per file): <name>.fcap.000, .001, ... concatenated = the capture. */
#define FCAP_PART (30u << 20)
struct pw { FILE *f; char base[256]; unsigned part; size_t n; int err; };
static int pw_open(struct pw *w)
{
   char p[300];
   snprintf(p, sizeof p, "%s.%03u", w->base, w->part);
   w->f = fopen(p, "wb"); w->n = 0;
   if (!w->f) { w->err = errno; return -1; }
   return 0;
}
static void pw_write(struct pw *w, const void *d, size_t len)
{
   const uint8_t *q = d;
   while (len && w->f) {
      size_t room = FCAP_PART - w->n, k = len < room ? len : room;
      if (fwrite(q, 1, k, w->f) != k) w->err = errno ? errno : EIO;
      w->n += k; q += k; len -= k;
      if (w->n >= FCAP_PART) { if (fclose(w->f) != 0) w->err = errno; w->part++; if (pw_open(w)) return; }
   }
}
static void pw_close(struct pw *w) { if (w->f && fclose(w->f) != 0) w->err = errno; w->f = NULL; }

static void fc_write_blob(struct pw *f, const struct fblob *bl)
{
   /* RLE32 when it pays, else raw */
   uint32_t codec = FCAP_CODEC_RAW;
   uint8_t *enc = NULL; size_t en = 0;
   if (bl->len % 4 == 0 && bl->len >= 64) {
      const uint32_t *w = (const uint32_t *)bl->data; size_t nw = bl->len / 4, i = 0, o = 0;
      uint32_t *out = malloc(bl->len + 16);
      if (!out) abort();
      while (i < nw && o + 2 < nw) {
         size_t r = 1;
         while (i + r < nw && w[i + r] == w[i] && r < 0x7fffffff) r++;
         if (r >= 3) { out[o++] = 0x80000000u | (uint32_t)r; out[o++] = w[i]; i += r; continue; }
         size_t s = i, l = 0;
         while (i < nw && l < 0x7fffffff) {
            if (i + 2 < nw && w[i] == w[i + 1] && w[i] == w[i + 2]) break;
            i++; l++;
         }
         if (o + 1 + l >= nw) { o = nw; break; }
         out[o++] = (uint32_t)l; memcpy(&out[o], &w[s], l * 4); o += l;
      }
      if (i >= nw && o < nw) { codec = FCAP_CODEC_RLE32; enc = (uint8_t *)out; en = o * 4; }
      else free(out);
   }
   uint32_t hdr[2] = { FC_BLOB, 16 + (uint32_t)(codec ? en : bl->len) };
   pw_write(f, hdr, 8); pw_write(f, &bl->h, 8); pw_write(f, &bl->len, 4); pw_write(f, &codec, 4);
   pw_write(f, codec ? enc : bl->data, codec ? en : bl->len);
   FC.bytes_written += 24 + (codec ? en : bl->len);
   free(enc);
}

static void fc_write_capture(const char *label)
{
   struct fframe *fr[FCAP_K + 8] = { 0 };   /* newest first: the ring frames, then the window's first frames (older) */
   int n = 0;
   pthread_mutex_lock(&FC.mu);
   for (int i = 0; i < FCAP_K && i < FCAP_NFR; i++) if (FC.ring[i]) { fr[n++] = FC.ring[i]; FC.ring[i]->refs++; }
   for (int i = (int)FC.nfirst - 1; i >= 0; i--) {
      int dup = 0;
      for (int j = 0; j < n; j++) if (fr[j] == FC.first[i]) dup = 1;
      if (!dup && (n == 0 || FC.first[i]->seq < fr[n - 1]->seq)) { fr[n++] = FC.first[i]; FC.first[i]->refs++; }
   }
   pthread_mutex_unlock(&FC.mu);
   if (!n) { sweet_gap("fcap", "capture '%s' requested but no recorded frame (eager off?)", label); return; }
   char path[256], clean[48];
   size_t k = 0;
   for (const char *s = label; *s && k < sizeof clean - 1; s++) clean[k++] = (isalnum((unsigned char)*s) || *s == '-') ? *s : '_';
   clean[k] = 0;
   snprintf(path, sizeof path, FCAP_DIR "/p%05d-%04u-%s.fcap", (int)getpid(), ++FC.files, clean);   /* pid: files of earlier boots stay apart */
   struct pw W = { 0 }, *f = &W;
   snprintf(W.base, sizeof W.base, "%s", path);
   if (pw_open(&W)) f = NULL;
   uint64_t t0 = fc_now(), b0 = FC.bytes_written;
   uint32_t ops = 0, draws = 0, nblob = 0, coh = 0;
   if (!f) {
      sweet_gap("fcap", "cannot create %s.000 errno %d", path, W.err);
   } else {
      pw_write(f, FCAP_MAGIC, 8);
      struct fbuf h = { 0 };
      size_t at = fb_rec(&h, FC_HDR); fb_u32(&h, FCAP_VERSION); fb_u32(&h, (uint32_t)n); fb_str(&h, label, 64); fb_str(&h, "26.2.5", 32); fb_end(&h, at);
      pw_write(f, h.p, h.n); fb_free(&h);
      for (int i = n - 1; i >= 0; i--) {   /* oldest first */
         struct fframe *x = fr[i];
         pthread_mutex_lock(&FC.mu);
         util_dynarray_foreach(&x->blobs, uint64_t, hp) {
            if (_mesa_hash_table_u64_search(FC.written, *hp)) continue;
            struct fblob *bl = _mesa_hash_table_u64_search(FC.blobs, *hp);
            if (!bl) continue;
            fc_write_blob(f, bl); nblob++;
            _mesa_hash_table_u64_insert(FC.written, *hp, (void *)1);
         }
         pthread_mutex_unlock(&FC.mu);
         pw_write(f, x->b.p, x->b.n);
         ops += x->ops; draws += x->draws; coh += x->coh_diff;
      }
      uint32_t endr[3] = { FC_END, 4, (uint32_t)n };
      pw_write(f, endr, 12);
      pw_close(f);
      if (W.err) sweet_gap("fcap", "write error on %s part %u errno %d", path, W.part, W.err);
   }
   fprintf(stderr, "sweetgl: FCAP WROTE %s parts %u frames %d (seq %u..%u) ops %u draws %u blobs %u bytes %llu ms %llu gaps %u coh %u\n", path, W.part + 1, n, fr[n - 1]->seq, fr[0]->seq,
           ops, draws, nblob, (unsigned long long)(FC.bytes_written - b0), (unsigned long long)((fc_now() - t0) / 1000000), FC.gaps, coh);
   pthread_mutex_lock(&FC.mu);
   for (int i = 0; i < n; i++) if (--fr[i]->refs == 0 && fr[i]->orphan) fc_frame_free(fr[i]);
   FC.win_reset = 1;   /* SWEETGL_FCAP_FIRST: the next action's first frames start after this capture (the sysctl window spans the whole capture rep) */
   pthread_mutex_unlock(&FC.mu);
}

static void fc_tap_write(struct fctx *c)
{
   char path[256];
   snprintf(path, sizeof path, FCAP_DIR "/p%05d-tap%u.ftap", (int)getpid(), ++TAP.seq);
   struct pw W = { 0 }, *f = &W;   /* parts of <= 30 MB, as the captures (devd pulls <= 40 MB per file) */
   snprintf(W.base, sizeof W.base, "%s", path);
   if (pw_open(&W)) { sweet_gap("fcap", "cannot create %s.000 errno %d", path, W.err); f = NULL; }
   uint64_t t0 = fc_kns();
   unsigned ok = 0;
   if (f) pw_write(f, "FTAP0001", 8);
   unsigned first = TAP.n == FCAP_TAPN ? TAP.head : 0;   /* oldest frame first */
   for (unsigned k = 0; k < TAP.n && f; k++) {
      unsigned i = (first + k) % FCAP_TAPN;
      struct pipe_resource *r = TAP.st[i];
      struct pipe_box box; u_box_2d(0, 0, (int)r->width0, (int)r->height0, &box);
      struct pipe_transfer *tr = NULL;
      fc_depth++;
      uint8_t *p = c->orig.texture_map(c->pctx, r, 0, PIPE_MAP_READ, &box, &tr);
      if (p) {
         uint32_t hdr[2] = { r->width0, r->height0 };
         pw_write(f, &TAP.t[i], 8); pw_write(f, hdr, 8);
         for (unsigned y = 0; y < r->height0; y++) pw_write(f, p + (size_t)y * tr->stride, (size_t)4 * r->width0);
         c->orig.texture_unmap(c->pctx, tr);
         ok++;
      }
      fc_depth--;
   }
   if (f) { pw_close(f); if (W.err) sweet_gap("fcap", "write error on %s errno %d", path, W.err); }
   fprintf(stderr, "sweetgl: FCAP TAP WROTE %s parts %u frames %u of %u dropped %u t %llu..%llu readback ms %llu\n", path, W.part + 1, ok, TAP.n, TAP.dropped,
           (unsigned long long)(TAP.n ? TAP.t[first] : 0), (unsigned long long)(TAP.n ? TAP.t[(first + TAP.n - 1) % FCAP_TAPN] : 0), (unsigned long long)((fc_kns() - t0) / 1000000));
   TAP.n = 0; TAP.dropped = 0; TAP.head = 0;
}

/* a flag file is ON when it exists and starts with '1' (devd can overwrite but not delete: "0" switches it off) */
static int fc_flag(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f) return 0;
   int ch = fgetc(f);
   fclose(f);
   return ch == '1';
}

static int fc_sysctl_cap(unsigned *val, char *lbl, size_t n)
{
   size_t sz = sizeof *val;
   if (sysctlbyname("kern.sweetnavcap", val, &sz, NULL, 0) != 0) return -1;
   size_t ln = n;
   if (sysctlbyname("kern.sweetnavcaplbl", lbl, &ln, NULL, 0) != 0) snprintf(lbl, n, "nav");
   lbl[n - 1] = 0;
   return 0;
}

static void *fc_thread(void *arg)
{
   (void)arg;
   unsigned last_seq = 0;
   for (;;) {
      struct timespec ts = { 0, 50 * 1000 * 1000 };
      nanosleep(&ts, NULL);
      FC.eager_file = fc_flag(FCAP_ROOT "/fcap-eager");
      {
         static unsigned tick;
         if ((FC.eager_file || FC.eager_sysctl || FC.tap_sysctl) && ++tick % 40 == 0)
            fprintf(stderr, "sweetgl: FCAP STAT flushes %u frames %u tapframes %u cur_ops %u gaps %u\n", FC.n_flush, FC.n_frames, FC.n_tapf,
                    FC.cur ? FC.cur->ops : 0, FC.gaps);
      }
      unsigned v = 0; char lbl[64] = { 0 };
      int src = fc_sysctl_cap(&v, lbl, sizeof lbl);
      {
         static int logged_first, last_rc = 99; static unsigned last_v = ~0u;
         if (!logged_first || src != last_rc || (src == 0 && v != last_v)) {
            fprintf(stderr, "sweetgl: FCAP SYSCTL kern.sweetnavcap rc %d errno %d value 0x%x label %s\n", src, src ? errno : 0, v, src ? "-" : lbl);
            logged_first = 1; last_rc = src; last_v = v;
         }
      }
      if (src == 0) {
         FC.have_sysctl = 1;
         if ((v & 1) && !FC.eager_sysctl) {
            static int fi = -1;
            if (fi < 0) { const char *e = getenv("SWEETGL_FCAP_FIRST"); fi = e ? atoi(e) : 0; if (fi > 8) fi = 8; if (fi < 0) fi = 0;
                          if (fi) fprintf(stderr, "sweetgl: FCAP first %d frames of each window kept too\n", fi); }
            pthread_mutex_lock(&FC.mu); FC.want_first = (unsigned)fi; FC.win_reset = 1; pthread_mutex_unlock(&FC.mu);
         }
         FC.eager_sysctl = v & 1;
         FC.tap_sysctl = (v >> 1) & 1;
         unsigned seq = v >> 16;
         if (seq != last_seq) { last_seq = seq; if (seq) fc_write_capture(lbl); }
      }
      FILE *rq = fopen(FCAP_ROOT "/fcap-req", "r");
      if (rq) {   /* "<n> <label>": a new n = a new request (devd overwrites, it cannot delete) */
         char l[96] = "";
         if (!fgets(l, sizeof l, rq)) l[0] = 0;
         fclose(rq);
         l[strcspn(l, "\r\n")] = 0;
         unsigned n = 0; char lab[64] = "req";
         if (sscanf(l, "%u %63s", &n, lab) >= 1 && n != FC.req_seen && !FC.have_sysctl) {   /* the sysctl, when present, is the trigger */
            int first = FC.req_seen == 0 && !FC.req_armed;
            FC.req_seen = n;
            if (!first) fc_write_capture(lab);   /* a req file left from an earlier boot only arms */
         }
         FC.req_armed = 1;
      } else {
         FC.req_armed = 1;
      }
   }
   return NULL;
}

/* ---------------------------------------------------------------- install */
static struct pipe_context *h_context_create(struct pipe_screen *s, void *priv, unsigned flags)
{
   struct pipe_context *p = FC.context_create(s, priv, flags);
   if (!p) return p;
   pthread_mutex_lock(&FC.mu);
   int slot = -1;
   for (int i = 0; i < FCAP_MAXCTX; i++) if (!FC.ctx[i]) { slot = i; break; }
   if (slot < 0) { pthread_mutex_unlock(&FC.mu); sweet_gap("fcap", "more than %d contexts: context %p not hooked", FCAP_MAXCTX, (void *)p); return p; }
   struct fctx *c = calloc(1, sizeof *c);
   if (!c) abort();
   c->pctx = p; c->orig = *p; c->sample_mask = ~0u;
   FC.ctx[slot] = c;
   pthread_mutex_unlock(&FC.mu);
#define FCAP_HOOK(field, fn) do { if (p->field) p->field = fn; } while (0)
   FCAP_HOOK(create_blend_state, h_create_blend); FCAP_HOOK(delete_blend_state, h_delete_blend); FCAP_HOOK(bind_blend_state, h_bind_blend);
   FCAP_HOOK(create_rasterizer_state, h_create_rast); FCAP_HOOK(delete_rasterizer_state, h_delete_rast); FCAP_HOOK(bind_rasterizer_state, h_bind_rast);
   FCAP_HOOK(create_depth_stencil_alpha_state, h_create_dsa); FCAP_HOOK(delete_depth_stencil_alpha_state, h_delete_dsa);
   FCAP_HOOK(bind_depth_stencil_alpha_state, h_bind_dsa);
   FCAP_HOOK(create_sampler_state, h_create_sampler); FCAP_HOOK(delete_sampler_state, h_delete_sampler); FCAP_HOOK(bind_sampler_states, h_bind_samplers);
   FCAP_HOOK(create_vertex_elements_state, h_create_velems); FCAP_HOOK(delete_vertex_elements_state, h_delete_velems);
   FCAP_HOOK(bind_vertex_elements_state, h_bind_velems);
   FCAP_HOOK(create_vs_state, h_create_vs); FCAP_HOOK(delete_vs_state, h_delete_vs); FCAP_HOOK(bind_vs_state, h_bind_vs);
   FCAP_HOOK(create_fs_state, h_create_fs); FCAP_HOOK(delete_fs_state, h_delete_fs); FCAP_HOOK(bind_fs_state, h_bind_fs);
   FCAP_HOOK(set_sampler_views, h_set_views); FCAP_HOOK(create_sampler_view, h_create_view); FCAP_HOOK(sampler_view_destroy, h_view_destroy);
   FCAP_HOOK(set_constant_buffer, h_set_cb); FCAP_HOOK(set_vertex_buffers, h_set_vbs); FCAP_HOOK(set_framebuffer_state, h_set_fb);
   FCAP_HOOK(set_viewport_states, h_set_vp); FCAP_HOOK(set_scissor_states, h_set_sc); FCAP_HOOK(set_blend_color, h_set_bc);
   FCAP_HOOK(set_stencil_ref, h_set_sr); FCAP_HOOK(set_sample_mask, h_set_sm); FCAP_HOOK(set_min_samples, h_set_ms); FCAP_HOOK(set_clip_state, h_set_clip);
   FCAP_HOOK(draw_vbo, h_draw); FCAP_HOOK(clear, h_clear); FCAP_HOOK(clear_render_target, h_clear_rt); FCAP_HOOK(clear_depth_stencil, h_clear_ds);
   FCAP_HOOK(blit, h_blit); FCAP_HOOK(resource_copy_region, h_copy); FCAP_HOOK(texture_subdata, h_texture_subdata);
   FCAP_HOOK(texture_map, h_texture_map); FCAP_HOOK(texture_unmap, h_texture_unmap); FCAP_HOOK(generate_mipmap, h_genmip);
   FCAP_HOOK(texture_barrier, h_tex_barrier); FCAP_HOOK(flush, h_flush); FCAP_HOOK(launch_grid, h_launch_grid); FCAP_HOOK(clear_texture, h_clear_texture);
#undef FCAP_HOOK
   if (p->sampler_view_release) sweet_gap("fcap", "driver has sampler_view_release: views released that way stay in the table");
   if (p->draw_vertex_state) sweet_gap("fcap", "driver has draw_vertex_state: not hooked, not recorded");
   fprintf(stderr, "sweetgl: FCAP hooked context %p slot %d\n", (void *)p, slot);
   return p;
}

static void fcap_install(struct pipe_screen *scr)
{
   const char *env = getenv("SWEETGL_FCAP");
   if (!(env && env[0] == '1') && !fc_flag(FCAP_ROOT "/fcap-on")) return;
   mkdir(FCAP_DIR, 0755);
   {
      pthread_mutexattr_t ma;
      pthread_mutexattr_init(&ma);
      pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&FC.mu, &ma);
      pthread_mutexattr_destroy(&ma);
   }
   FC.objs = _mesa_pointer_hash_table_create(NULL);
   FC.blobs = _mesa_hash_table_u64_create(NULL);
   FC.written = _mesa_hash_table_u64_create(NULL);
   FC.scr = scr;
   FC.context_create = scr->context_create; scr->context_create = h_context_create;
   FC.resource_create = scr->resource_create; scr->resource_create = h_resource_create;
   FC.resource_from_handle = scr->resource_from_handle; scr->resource_from_handle = h_resource_from_handle;
   FC.resource_destroy = scr->resource_destroy; scr->resource_destroy = h_resource_destroy;
   if (scr->resource_create_with_modifiers) sweet_gap("fcap", "resource_create_with_modifiers not hooked: such resources get ids at first use");
   FC.installed = 1;
   fc_frame_start();
   if (pthread_create(&FC.thr, NULL, fc_thread, NULL) != 0) sweet_gap("fcap", "writer thread not started errno %d: no capture files", errno);
   else pthread_detach(FC.thr);
   fprintf(stderr, "sweetgl: FCAP installed dir " FCAP_DIR " nfr %d k %d\n", FCAP_NFR, FCAP_K);
}
