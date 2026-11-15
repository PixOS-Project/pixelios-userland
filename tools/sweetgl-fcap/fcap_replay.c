/* fcap_replay: host software-rasterizer replay of libsweetgl frame captures (format: ../sweetgl-eagl/fcap_format.h). Sprint GPU 2026-11-09.
 *
 *   fcap_replay [--driver llvmpipe|softpipe] --out DIR FILE.fcap...
 *
 * Every FC_BLOB of every file is loaded first (a run's files share blobs: a blob is written once per boot). Then each file is replayed on a
 * fresh Mesa 26.2.5 context of the chosen software driver, frame by frame in file order, with the exact Gallium state the device driver got
 * (shaders = the st's NIR, deserialized here with the software driver's compiler options). At each FC_PRESENT the replayed scanout is read
 * back and compared with the device's scanout snapshot (FC_CONTENT role RESULT).
 *
 * Input content policy (stated in the report): a resource's content at its first touch in a frame comes from the replay itself if an earlier
 * replayed op wrote it; else the CPU view (app's CPU-cached IOSurface mapping) if the frame has one; else the device GPU view, which is then
 * listed as INHERITED (device-rendered input not produced inside the file).
 *
 * Outputs per frame: DIR/<file>-f<seq>-{ref,dev,diff}.ppm and one JSON line per frame in DIR/report.jsonl:
 *   metrics on RGB 0..255: max_abs, mean_abs, px_gt8_pct, px_gt32_pct, worst_tile32_mean (32x32 tiles), plus the tile map of tiles > 4.
 * Tolerance (fixed, user goal 2026-11-09): MATCH iff px_gt8_pct <= 0.5 and worst_tile32_mean <= 4.0.
 * Last line: RESULT: PASS fcap-replay frames=N match=N | RESULT: FAIL ... (a frame MISMATCH or a replay error) | RESULT: ERROR ...
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/box.h"
#include "util/format/u_format.h"
#include "util/hash_table.h"
#include "util/blob.h"
#include "util/u_math.h"
#include "util/u_gen_mipmap.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "compiler/glsl_types.h"
#include "llvmpipe/lp_public.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"
#include "fcap_format.h"

#define TOL_PX_GT8_PCT 0.5
#define TOL_TILE_MEAN 4.0

static int verbose, trace, watch_x = -1, watch_y = -1;
static long bbox_frame = -1;   /* --bbox SEQ: after every draw of frame SEQ, the box of colour-buffer-0 pixels the draw changed */
static uint32_t watch_last = 0xdeadbeef;
static unsigned op_no;
static uint32_t st_ids[6];
static char st_views[256];
static void die(const char *fmt, ...)
{
   va_list ap; va_start(ap, fmt);
   printf("RESULT: ERROR fcap-replay ");
   vprintf(fmt, ap); printf("\n");
   va_end(ap);
   exit(2);
}

/* ---------------------------------------------------------------- blobs */
struct hblob { uint32_t len; uint8_t *data; };
static struct hash_table_u64 *BLOBS;

static uint8_t *rle32_decode(const uint8_t *in, size_t n, uint32_t raw_len)
{
   if (raw_len % 4) return NULL;
   uint32_t *out = malloc(raw_len ? raw_len : 4);
   const uint32_t *w = (const uint32_t *)in;
   size_t nw = n / 4, i = 0, o = 0, ow = raw_len / 4;
   while (i < nw) {
      uint32_t h = w[i++];
      if (h & 0x80000000u) {
         uint32_t r = h & 0x7fffffffu;
         if (i >= nw || o + r > ow) { free(out); return NULL; }
         uint32_t v = w[i++];
         for (uint32_t k = 0; k < r; k++) out[o++] = v;
      } else {
         if (i + h > nw || o + h > ow) { free(out); return NULL; }
         memcpy(&out[o], &w[i], (size_t)h * 4); o += h; i += h;
      }
   }
   if (o != ow) { free(out); return NULL; }
   return (uint8_t *)out;
}

static const struct hblob *blob_get(uint64_t h)
{
   struct hblob *b = _mesa_hash_table_u64_search(BLOBS, h);
   return b;
}

/* ---------------------------------------------------------------- file reading */
struct file { char *path; uint8_t *p; size_t n; };
static struct file read_file(const char *path)
{
   struct file f = { strdup(path), NULL, 0 };
   FILE *fp = fopen(path, "rb");
   if (!fp) die("cannot open %s: %s", path, strerror(errno));
   fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
   f.p = malloc((size_t)len + 1); f.n = (size_t)len;
   if (fread(f.p, 1, f.n, fp) != f.n) die("short read %s", path);
   fclose(fp);
   if (f.n < 8 || memcmp(f.p, FCAP_MAGIC, 8)) die("%s: not an FCAP file", path);
   return f;
}

struct rd { const uint8_t *p, *e; };
static uint32_t r32(struct rd *r) { if (r->p + 4 > r->e) die("record underrun"); uint32_t v; memcpy(&v, r->p, 4); r->p += 4; return v; }
static int32_t ri32(struct rd *r) { return (int32_t)r32(r); }
static uint64_t r64(struct rd *r) { if (r->p + 8 > r->e) die("record underrun"); uint64_t v; memcpy(&v, r->p, 8); r->p += 8; return v; }
static float rf(struct rd *r) { uint32_t v = r32(r); float f; memcpy(&f, &v, 4); return f; }
static double rf64(struct rd *r) { uint64_t v = r64(r); double d; memcpy(&d, &v, 8); return d; }
static void rstr(struct rd *r, char *out, size_t n) { if (r->p + n > r->e) die("record underrun"); memcpy(out, r->p, n); out[n - 1] = 0; r->p += n; }

static void load_blobs(struct file *f)
{
   const uint8_t *p = f->p + 8, *e = f->p + f->n;
   while (p + 8 <= e) {
      uint32_t type, len; memcpy(&type, p, 4); memcpy(&len, p + 4, 4);
      const uint8_t *q = p + 8;
      if (q + len > e) die("%s: truncated record type %u", f->path, type);
      if (type == FC_BLOB) {
         uint64_t h; uint32_t raw, codec; memcpy(&h, q, 8); memcpy(&raw, q + 8, 4); memcpy(&codec, q + 12, 4);
         if (!_mesa_hash_table_u64_search(BLOBS, h)) {
            struct hblob *b = calloc(1, sizeof *b);
            b->len = raw;
            if (codec == FCAP_CODEC_RAW) { b->data = malloc(raw ? raw : 1); memcpy(b->data, q + 16, raw); }
            else if (codec == FCAP_CODEC_RLE32) { b->data = rle32_decode(q + 16, len - 16, raw); if (!b->data) die("%s: bad RLE32 blob", f->path); }
            else die("%s: unknown codec %u", f->path, codec);
            _mesa_hash_table_u64_insert(BLOBS, h, b);
         }
      }
      p = q + len;
   }
}

/* ---------------------------------------------------------------- replay state */
struct rres {
   struct pipe_resource *res;
   uint32_t imported, scanout, fourcc, surf_id;
   uint32_t undecodable;     /* compressed device format recorded raw (captures before the decode change): content skipped, gap noted */
   uint32_t written_frame;   /* +1 of the last frame in which a replayed op wrote it; any earlier write counts (written_any) */
   int written_any;
   uint32_t cpu_frame;       /* +1: CPU view applied in this frame */
   uint32_t touched_frame;   /* +1: an initial content was applied in this frame */
};
struct robj { uint32_t kind; void *cso; struct pipe_sampler_view *view; uint32_t cso_kind; struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS]; unsigned nve; uint32_t cpe; };

static struct pipe_screen *SCR;
static struct pipe_context *CTX;
static struct hash_table_u64 *RES, *OBJ;
static uint32_t cur_frame;
static char cur_label[64];
static char gaps[4096];
static size_t gaps_n;
static unsigned n_inherited, n_cpu_used, n_errors, n_appgpu, n_decoded;
static char inherited[2048];
static size_t inh_n;
static FILE *REPORT;
static const char *OUTDIR, *FILEBASE;
static unsigned frames_total, frames_match;

static struct rres *res_get(uint32_t id)
{
   if (!id) return NULL;
   struct rres *r = _mesa_hash_table_u64_search(RES, id);
   if (!r) { n_errors++; if (verbose) fprintf(stderr, "unknown resource id %u\n", id); }
   return r;
}
static struct robj *obj_get(uint32_t id) { return id ? _mesa_hash_table_u64_search(OBJ, id) : NULL; }

static void note_gap(const char *t, size_t n)
{
   if (gaps_n + n + 3 < sizeof gaps) { memcpy(gaps + gaps_n, t, n); gaps_n += n; gaps[gaps_n++] = ';'; gaps[gaps_n] = 0; }
}

static void def_res(struct rd *r)
{
   uint32_t id = r32(r);
   struct pipe_resource t; memset(&t, 0, sizeof t);
   t.target = r32(r); t.format = r32(r); t.width0 = r32(r); t.height0 = r32(r); t.depth0 = r32(r); t.array_size = r32(r);
   t.last_level = r32(r); t.nr_samples = r32(r);
   uint32_t bind = r32(r); (void)r32(r);   /* flags */
   uint32_t imported = r32(r), surf = r32(r), fourcc = r32(r); (void)r32(r); uint32_t scanout = r32(r);
   uint32_t devfmt = r->p + 4 <= r->e ? r32(r) : (uint32_t)t.format;   /* 17th field: the device format (compressed ones are recorded decoded) */
   if (devfmt != (uint32_t)t.format) n_decoded++;
   if (trace && t.width0 >= 1000) printf("RES id %u %ux%u fmt %u %s target %u imported %u fourcc 0x%x scanout %u\n", id, t.width0, t.height0, (unsigned)t.format,
                                       util_format_short_name(t.format), (unsigned)t.target, imported, fourcc, scanout);
   if (_mesa_hash_table_u64_search(RES, id)) {   /* redefinition (each frame defines what it uses; imported info may be filled in later) */
      struct rres *x = _mesa_hash_table_u64_search(RES, id);
      x->imported = imported; x->scanout = scanout; x->fourcc = fourcc; x->surf_id = surf;
      return;
   }
   t.bind = bind & (PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_VERTEX_BUFFER |
                    PIPE_BIND_INDEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER);
   if (t.target != PIPE_BUFFER && !util_format_is_depth_or_stencil(t.format) &&
       SCR->is_format_supported(SCR, t.format, t.target, 0, 0, PIPE_BIND_RENDER_TARGET))
      t.bind |= PIPE_BIND_RENDER_TARGET;
   if (t.target != PIPE_BUFFER) t.bind |= PIPE_BIND_SAMPLER_VIEW;
   if (t.nr_samples <= 1) t.nr_samples = 0;
   int undec = 0;
   if (util_format_is_compressed(t.format) && !SCR->is_format_supported(SCR, t.format, t.target, 0, 0, PIPE_BIND_SAMPLER_VIEW)) {
      t.format = util_format_is_float(t.format) ? PIPE_FORMAT_R16G16B16A16_FLOAT : PIPE_FORMAT_R8G8B8A8_UNORM;
      t.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
      undec = 1;
   }
   if (!t.depth0) t.depth0 = 1;
   if (!t.array_size) t.array_size = 1;
   struct rres *x = calloc(1, sizeof *x);
   x->res = SCR->resource_create(SCR, &t);
   if (!x->res) die("resource_create failed id %u fmt %s %ux%u target %u", id, util_format_name(t.format), t.width0, t.height0, t.target);
   x->imported = imported; x->scanout = scanout; x->fourcc = fourcc; x->surf_id = surf; x->undecodable = undec;
   _mesa_hash_table_u64_insert(RES, id, x);
}

static void def_shader(struct rd *r)
{
   uint32_t id = r32(r), stage = r32(r), len = r32(r);
   if (_mesa_hash_table_u64_search(OBJ, id)) { r->p += len; return; }
   if (r->p + len > r->e) die("shader blob truncated");
   struct blob_reader br; blob_reader_init(&br, r->p, len);
   nir_shader *nir = nir_deserialize(NULL, SCR->nir_options[stage], &br);
   r->p += len;
   if (!nir) die("nir_deserialize failed shader %u", id);
   {  /* env FCAP_PRINT_SHADERS=",3,87,": print those shaders' NIR (diagnostic; ids are recorder ids) */
      const char *ps = getenv("FCAP_PRINT_SHADERS");
      char key[24]; snprintf(key, sizeof key, ",%u,", id);
      if (ps && strstr(ps, key)) { printf("SHADER %u stage %u\n", id, stage); nir_print_shader(nir, stdout); fflush(stdout); }
   }
   struct pipe_shader_state ss; memset(&ss, 0, sizeof ss);
   ss.type = PIPE_SHADER_IR_NIR; ss.ir.nir = nir;
   struct robj *o = calloc(1, sizeof *o);
   o->kind = FC_SHADER;
   if (stage == MESA_SHADER_VERTEX) o->cso = CTX->create_vs_state(CTX, &ss);
   else if (stage == MESA_SHADER_FRAGMENT) o->cso = CTX->create_fs_state(CTX, &ss);
   else die("shader %u stage %u not supported", id, stage);
   if (!o->cso) die("create shader %u failed", id);
   _mesa_hash_table_u64_insert(OBJ, id, o);
}

#define RD_U(n) t.n = r32(r);
#define RD_F(n) t.n = rf(r);
static void def_cso(struct rd *r)
{
   uint32_t kind = r32(r), id = r32(r);
   if (_mesa_hash_table_u64_search(OBJ, id)) { r->p = r->e; return; }
   struct robj *o = calloc(1, sizeof *o);
   o->kind = FC_CSO; o->cso_kind = kind;
   switch (kind) {
   case FCAP_CSO_BLEND: {
      struct pipe_blend_state t; memset(&t, 0, sizeof t);
      FCAP_BLEND_FIELDS(RD_U, RD_F)
      for (int i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
#define RD_RU(n) t.rt[i].n = r32(r);
         FCAP_RT_BLEND_FIELDS(RD_RU, RD_F)
#undef RD_RU
      }
      o->cso = CTX->create_blend_state(CTX, &t);
      break;
   }
   case FCAP_CSO_RAST: {
      struct pipe_rasterizer_state t; memset(&t, 0, sizeof t);
      FCAP_RAST_FIELDS(RD_U, RD_F)
      o->cpe = t.clip_plane_enable;
      o->cso = CTX->create_rasterizer_state(CTX, &t);
      break;
   }
   case FCAP_CSO_DSA: {
      struct pipe_depth_stencil_alpha_state t; memset(&t, 0, sizeof t);
      for (int i = 0; i < 2; i++) {
#define RD_SU(n) t.stencil[i].n = r32(r);
         FCAP_STENCIL_FIELDS(RD_SU, RD_F)
#undef RD_SU
      }
      FCAP_DSA_FIELDS(RD_U, RD_F)
      o->cso = CTX->create_depth_stencil_alpha_state(CTX, &t);
      break;
   }
   case FCAP_CSO_SAMPLER: {
      struct pipe_sampler_state t; memset(&t, 0, sizeof t);
      FCAP_SAMPLER_FIELDS(RD_U, RD_F)
      o->cso = CTX->create_sampler_state(CTX, &t);
      break;
   }
   case FCAP_CSO_VELEMS: {
      o->nve = r32(r);
      if (o->nve > PIPE_MAX_ATTRIBS) die("velems count %u", o->nve);
      for (unsigned i = 0; i < o->nve; i++) {
         struct pipe_vertex_element t; memset(&t, 0, sizeof t);
         FCAP_VELEM_FIELDS(RD_U, RD_F)
         o->ve[i] = t;
      }
      o->cso = CTX->create_vertex_elements_state(CTX, o->nve, o->ve);
      break;
   }
   default: die("cso kind %u", kind);
   }
   if (!o->cso) die("create cso kind %u id %u failed", kind, id);
   _mesa_hash_table_u64_insert(OBJ, id, o);
}

static void def_view(struct rd *r)
{
   uint32_t id = r32(r), resid = r32(r);
   struct pipe_sampler_view t; memset(&t, 0, sizeof t);
   t.format = r32(r); t.target = r32(r);
   t.swizzle_r = r32(r); t.swizzle_g = r32(r); t.swizzle_b = r32(r); t.swizzle_a = r32(r);
   unsigned fl = r32(r), ll = r32(r), fla = r32(r), lla = r32(r), isbuf = r32(r), bo = r32(r), bs = r32(r);
   if (_mesa_hash_table_u64_search(OBJ, id)) return;
   if (isbuf) { t.u.buf.offset = bo; t.u.buf.size = bs; }
   else { t.u.tex.first_level = fl; t.u.tex.last_level = ll; t.u.tex.first_layer = fla; t.u.tex.last_layer = lla; }
   struct rres *x = res_get(resid);
   if (!x) die("view %u on unknown resource %u", id, resid);
   if (util_format_is_compressed(t.format)) t.format = x->res->format;   /* the resource was recorded decoded (RES field 17) */
   struct robj *o = calloc(1, sizeof *o);
   o->kind = FC_SVIEW;
   o->view = CTX->create_sampler_view(CTX, x->res, &t);
   if (!o->view) die("create_sampler_view %u failed", id);
   _mesa_hash_table_u64_insert(OBJ, id, o);
}

/* ---------------------------------------------------------------- content */
static void upload(struct rres *x, unsigned level, unsigned layer, unsigned w, unsigned h, unsigned stride, const struct hblob *b)
{
   struct pipe_box box; u_box_3d(0, 0, (int)layer, (int)w, (int)h, 1, &box);
   unsigned rows = util_format_get_nblocksy(x->res->format, h);
   if ((size_t)stride * rows > b->len) die("content blob too small (%u x %u > %u)", stride, rows, b->len);
   CTX->texture_subdata(CTX, x->res, level, 0, &box, b->data, stride, (uintptr_t)stride * rows);
}

static const struct hblob *frame_result;
static uint32_t result_res;
static unsigned result_w, result_h, result_stride;

static void content(struct rd *r)
{
   uint32_t id = r32(r), level = r32(r), layer = r32(r), w = r32(r), h = r32(r), stride = r32(r), view = r32(r), role = r32(r);
   uint64_t hsh = r64(r); (void)r32(r);
   struct rres *x = res_get(id);
   const struct hblob *b = blob_get(hsh);
   if (!x) return;
   if (!b) { n_errors++; char t[96]; int n = snprintf(t, sizeof t, "missing blob %016llx (res %u)", (unsigned long long)hsh, id); note_gap(t, (size_t)n); return; }
   if (role == FCAP_ROLE_RESULT) { frame_result = b; result_res = id; result_w = w; result_h = h; result_stride = stride; return; }
   if (x->written_any) return;   /* the replay produced this content itself */
   if (x->undecodable) { char t[96]; int n = snprintf(t, sizeof t, "res %u compressed content recorded raw: not replayable", id); note_gap(t, (size_t)n); n_errors++; return; }
   if (view == FCAP_VIEW_CPU) {
      upload(x, level, layer, w, h, stride, b);
      x->cpu_frame = cur_frame + 1; n_cpu_used++;
      return;
   }
   if (x->cpu_frame == cur_frame + 1 && level == 0 && layer == 0) return;   /* the app's CPU view wins */
   upload(x, level, layer, w, h, stride, b);
   /* device-rendered = Mesa-allocated textures and CA's own render surfaces (fourcc 0 intermediates, 'b3a8' display, 'w30r' layer buffers);
    * other imported surfaces are app content seen through the GPU view (counted apart: no CPU view was taken for them in this frame) */
   int devr = !x->imported || x->fourcc == 0 || x->fourcc == 0x62336138u || x->fourcc == 0x77333072u;
   if (!devr && x->touched_frame != cur_frame + 1) n_appgpu++;
   if (devr && x->touched_frame != cur_frame + 1) {
      n_inherited++;
      if (inh_n + 48 < sizeof inherited)
         inh_n += (size_t)snprintf(inherited + inh_n, sizeof inherited - inh_n, "%s%u:%ux%u:%s%s", inh_n ? "," : "", id, w, h,
                                   util_format_short_name(x->res->format), x->scanout ? ":scanout" : "");
   }
   x->touched_frame = cur_frame + 1;
}

/* ---------------------------------------------------------------- ops */
static struct pipe_framebuffer_state FB;
static void rd_surf(struct rd *r, struct pipe_surface *s)
{
   uint32_t id = r32(r), fmt = r32(r), level = r32(r), fl = r32(r), ll = r32(r);
   memset(s, 0, sizeof *s);
   if (!id) return;
   struct rres *x = res_get(id);
   if (!x) return;
   s->texture = x->res; s->format = fmt; s->level = level; s->first_layer = fl; s->last_layer = ll;
}
static void watch_after(const char *what);
/* --bbox SEQ helpers: copy of colour buffer 0 (tightly packed, 4 bytes/px; NULL when none or not 4-byte) */
static uint8_t *bbox_grab(unsigned *w, unsigned *h)
{
   struct pipe_resource *r = FB.cbufs[0].texture;
   if (!r || util_format_get_blocksize(r->format) != 4) return NULL;
   *w = r->width0; *h = r->height0;
   struct pipe_fence_handle *fence = NULL;
   CTX->flush(CTX, &fence, 0);
   if (fence) { SCR->fence_finish(SCR, NULL, fence, OS_TIMEOUT_INFINITE); SCR->fence_reference(SCR, &fence, NULL); }
   struct pipe_box box; u_box_2d(0, 0, (int)*w, (int)*h, &box);
   struct pipe_transfer *tr = NULL;
   uint8_t *p = CTX->texture_map(CTX, r, 0, PIPE_MAP_READ, &box, &tr);
   if (!p) return NULL;
   uint8_t *c = malloc((size_t)*w * *h * 4);
   if (!c) abort();
   for (unsigned y = 0; y < *h; y++) memcpy(c + (size_t)y * *w * 4, p + (size_t)y * tr->stride, (size_t)*w * 4);
   CTX->texture_unmap(CTX, tr);
   return c;
}
static void bbox_report(const uint8_t *a, const uint8_t *b, unsigned w, unsigned h)
{
   unsigned x0 = w, y0 = h, x1 = 0, y1 = 0; size_t n = 0; uint32_t last = 0;
   for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
      size_t o = ((size_t)y * w + x) * 4;
      if (memcmp(a + o, b + o, 4)) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; memcpy(&last, b + o, 4); }
   }
   if (w > 300 && h > 60) { uint32_t pa, pb; memcpy(&pa, a + ((size_t)60 * w + 300) * 4, 4); memcpy(&pb, b + ((size_t)60 * w + 300) * 4, 4); printf("BBOXDBG op %u px300,60 0x%08x -> 0x%08x\n", op_no, pa, pb); }
   if (n) printf("BBOX op %u f%u fb %ux%u changed %zu px box %u,%u..%u,%u sample 0x%08x\n", op_no, cur_frame, w, h, n, x0, y0, x1, y1, last);
   else printf("BBOX op %u f%u fb %ux%u changed 0 px\n", op_no, cur_frame, w, h);
}

static void mark_written(struct pipe_resource *p)
{
   if (!p) return;
   hash_table_u64_foreach(RES, e) {
      struct rres *x = e.data;
      if (x->res == p) { x->written_any = 1; x->written_frame = cur_frame + 1; }
   }
}
static void op_fb(struct rd *r)
{
   memset(&FB, 0, sizeof FB);
   FB.width = r32(r); FB.height = r32(r); FB.layers = r32(r); FB.samples = r32(r); FB.nr_cbufs = r32(r);
   for (int i = 0; i < PIPE_MAX_COLOR_BUFS; i++) rd_surf(r, &FB.cbufs[i]);
   rd_surf(r, &FB.zsbuf);
   CTX->set_framebuffer_state(CTX, &FB);
}
static void fb_written(void)
{
   for (unsigned i = 0; i < FB.nr_cbufs; i++) mark_written(FB.cbufs[i].texture);
   mark_written(FB.zsbuf.texture);
}

static void op_state(struct rd *r)
{
   uint32_t ids[6]; for (int i = 0; i < 6; i++) ids[i] = r32(r);
   memcpy(st_ids, ids, sizeof ids); st_views[0] = 0;
   struct robj *o;
   if ((o = obj_get(ids[0]))) CTX->bind_blend_state(CTX, o->cso); else if (ids[0]) n_errors++;
   uint32_t cpe = 0;
   if ((o = obj_get(ids[1]))) { CTX->bind_rasterizer_state(CTX, o->cso); cpe = o->cpe; } else if (ids[1]) n_errors++;
   if ((o = obj_get(ids[2]))) CTX->bind_depth_stencil_alpha_state(CTX, o->cso); else if (ids[2]) n_errors++;
   if ((o = obj_get(ids[3]))) CTX->bind_vs_state(CTX, o->cso); else n_errors++;
   if ((o = obj_get(ids[4]))) CTX->bind_fs_state(CTX, o->cso); else n_errors++;
   struct robj *ve = obj_get(ids[5]);
   if (ve) CTX->bind_vertex_elements_state(CTX, ve->cso); else if (ids[5]) n_errors++;
   (void)cpe;
   struct pipe_blend_color bc; for (int i = 0; i < 4; i++) bc.color[i] = rf(r);
   CTX->set_blend_color(CTX, &bc);
   struct pipe_stencil_ref sr; sr.ref_value[0] = (uint8_t)r32(r); sr.ref_value[1] = (uint8_t)r32(r);
   CTX->set_stencil_ref(CTX, sr);
   CTX->set_sample_mask(CTX, r32(r));
   unsigned ms = r32(r);
   if (CTX->set_min_samples) CTX->set_min_samples(CTX, ms ? ms : 1);
   struct pipe_viewport_state t; memset(&t, 0, sizeof t);
   FCAP_VIEWPORT_FIELDS(RD_U, RD_F)
   CTX->set_viewport_states(CTX, 0, 1, &t);
   struct pipe_scissor_state sc; sc.minx = r32(r); sc.miny = r32(r); sc.maxx = r32(r); sc.maxy = r32(r);
   CTX->set_scissor_states(CTX, 0, 1, &sc);
   for (int st = 0; st < 2; st++) {
      mesa_shader_stage stage = st ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
      unsigned nv = r32(r);
      struct pipe_sampler_view *views[64] = { 0 };
      if (nv > 64) die("views %u", nv);
      for (unsigned i = 0; i < nv; i++) {
         uint32_t vid = r32(r);
         struct robj *v = obj_get(vid); views[i] = v ? v->view : NULL;
         if (trace && st && v && v->view && strlen(st_views) < 200) {
            size_t L = strlen(st_views);
            snprintf(st_views + L, sizeof st_views - L, " t%u=%ux%u:%s", i, v->view->texture->width0, v->view->texture->height0, util_format_short_name(v->view->texture->format));
         }
      }
      CTX->set_sampler_views(CTX, stage, 0, nv, 64 - nv, views);
      unsigned ns = r32(r);
      void *samp[64] = { 0 };
      if (ns > 64) die("samplers %u", ns);
      for (unsigned i = 0; i < ns; i++) { struct robj *s = obj_get(r32(r)); samp[i] = s ? s->cso : NULL; }
      if (ns) CTX->bind_sampler_states(CTX, stage, 0, ns, samp);
      unsigned ncb = r32(r);
      for (unsigned i = 0; i < 16; i++) CTX->set_constant_buffer(CTX, stage, i, NULL);
      for (unsigned i = 0; i < ncb; i++) {
         uint32_t idx = r32(r), size = r32(r); uint64_t h = r64(r);
         const struct hblob *b = blob_get(h);
         if (!b) { n_errors++; continue; }
         struct pipe_constant_buffer cb; memset(&cb, 0, sizeof cb);   /* llvmpipe takes no user constant buffers (lp_state_fs.c assert): a real buffer */
         cb.buffer = pipe_buffer_create(SCR, PIPE_BIND_CONSTANT_BUFFER, PIPE_USAGE_STREAM, size ? size : 16);
         if (size) pipe_buffer_write(CTX, cb.buffer, 0, size, b->data);
         cb.buffer_size = size;
         CTX->set_constant_buffer(CTX, stage, idx, &cb);
         pipe_resource_reference(&cb.buffer, NULL);
      }
   }
   unsigned nvb = r32(r);
   struct pipe_vertex_buffer vbs[PIPE_MAX_ATTRIBS]; memset(vbs, 0, sizeof vbs);
   if (nvb > PIPE_MAX_ATTRIBS) die("nvb %u", nvb);
   for (unsigned v = 0; v < nvb; v++) {
      uint64_t h = r64(r); uint32_t size = r32(r), base = r32(r);
      if (!size) continue;
      const struct hblob *b = blob_get(h);
      if (!b) { n_errors++; continue; }
      struct pipe_resource *buf = pipe_buffer_create(SCR, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_STREAM, base + size);
      pipe_buffer_write(CTX, buf, base, size, b->data);
      vbs[v].buffer.resource = buf;   /* ownership passes to the driver (util_set_vertex_buffers_count) */
      vbs[v].buffer_offset = 0;
   }
   CTX->set_vertex_buffers(CTX, nvb, vbs);
   unsigned nclip = r32(r);
   if (nclip) {
      struct pipe_clip_state cs;
      for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) cs.ucp[i][j] = rf(r);
      CTX->set_clip_state(CTX, &cs);
   }
}

static void op_draw(struct rd *r)
{
   struct pipe_draw_info info; memset(&info, 0, sizeof info);
   info.mode = r32(r); info.index_size = r32(r); info.primitive_restart = r32(r); info.restart_index = r32(r);
   info.start_instance = r32(r); info.instance_count = r32(r);
   unsigned drawid = r32(r); uint64_t ih = r64(r); unsigned nd = r32(r);
   struct pipe_draw_start_count_bias d[64];
   if (nd > 64) die("num_draws %u", nd);
   for (unsigned i = 0; i < nd; i++) { d[i].start = r32(r); d[i].count = r32(r); d[i].index_bias = ri32(r); }
   struct pipe_resource *ib = NULL;
   if (info.index_size) {
      const struct hblob *b = blob_get(ih);
      if (!b) { n_errors++; return; }
      ib = pipe_buffer_create(SCR, PIPE_BIND_INDEX_BUFFER, PIPE_USAGE_STREAM, b->len ? b->len : 4);
      if (b->len) pipe_buffer_write(CTX, ib, 0, b->len, b->data);
      info.index.resource = ib;
   }
   if (!info.instance_count) info.instance_count = 1;
   if (trace) {
      printf("op %u f%u DRAW mode %u n %u count %u idx %u fb %ux%u cb0 %s%s vs %u fs %u blend %u rast %u%s\n", op_no, cur_frame, info.mode, nd, nd ? d[0].count : 0,
             info.index_size, FB.width, FB.height, FB.cbufs[0].texture ? util_format_short_name(FB.cbufs[0].texture->format) : "-",
             FB.cbufs[0].texture ? "" : "", st_ids[3], st_ids[4], st_ids[0], st_ids[1], st_views);
   }
   unsigned bw = 0, bh = 0; uint8_t *bpre = bbox_frame == (long)cur_frame ? bbox_grab(&bw, &bh) : NULL;
   CTX->draw_vbo(CTX, &info, drawid, NULL, d, nd);
   if (bpre) { unsigned w2, h2; uint8_t *bpost = bbox_grab(&w2, &h2); if (bpost) { bbox_report(bpre, bpost, bw, bh); free(bpost); } free(bpre); }
   pipe_resource_reference(&ib, NULL);
   fb_written();
   watch_after("DRAW");
}

static void op_clear(struct rd *r)
{
   unsigned buffers = r32(r), cmask = r32(r), smask = r32(r), has_sc = r32(r);
   struct pipe_scissor_state sc; sc.minx = r32(r); sc.miny = r32(r); sc.maxx = r32(r); sc.maxy = r32(r);
   union pipe_color_union col; for (int i = 0; i < 4; i++) col.ui[i] = r32(r);
   double depth = rf64(r); unsigned stencil = r32(r);
   CTX->clear(CTX, buffers, cmask, (uint8_t)smask, has_sc ? &sc : NULL, &col, depth, stencil);
   fb_written();
   watch_after("CLEAR");
}
static void op_clear_rt(struct rd *r)
{
   struct pipe_surface s; rd_surf(r, &s);
   union pipe_color_union col; for (int i = 0; i < 4; i++) col.ui[i] = r32(r);
   unsigned x = r32(r), y = r32(r), w = r32(r), h = r32(r), rc = r32(r);
   if (s.texture) { CTX->clear_render_target(CTX, &s, &col, x, y, w, h, rc); mark_written(s.texture); }
}
static void op_clear_ds(struct rd *r)
{
   struct pipe_surface s; rd_surf(r, &s);
   unsigned flags = r32(r); double depth = rf64(r); unsigned stencil = r32(r), x = r32(r), y = r32(r), w = r32(r), h = r32(r), rc = r32(r);
   if (s.texture) { CTX->clear_depth_stencil(CTX, &s, flags, depth, stencil, x, y, w, h, rc); mark_written(s.texture); }
}
static void rd_box(struct rd *r, struct pipe_box *b) { b->x = ri32(r); b->y = ri32(r); b->z = ri32(r); b->width = ri32(r); b->height = ri32(r); b->depth = ri32(r); }
static void op_blit(struct rd *r)
{
   struct pipe_blit_info bi; memset(&bi, 0, sizeof bi);
   struct rres *d = res_get(r32(r)); bi.dst.level = r32(r); rd_box(r, &bi.dst.box); bi.dst.format = r32(r);
   struct rres *s = res_get(r32(r)); bi.src.level = r32(r); rd_box(r, &bi.src.box); bi.src.format = r32(r);
   bi.mask = r32(r); bi.filter = r32(r); bi.scissor_enable = r32(r);
   bi.scissor.minx = r32(r); bi.scissor.miny = r32(r); bi.scissor.maxx = r32(r); bi.scissor.maxy = r32(r);
   bi.alpha_blend = r32(r); bi.swizzle_enable = r32(r);
   for (int i = 0; i < 4; i++) bi.swizzle[i] = (uint8_t)r32(r);
   bi.render_condition_enable = r32(r); bi.dst_sample = (uint8_t)r32(r); bi.sample0_only = r32(r);
   if (trace) printf("op %u f%u BLIT dst %ux%u box %d,%d %dx%d <- src %ux%u box %d,%d %dx%d mask 0x%x filter %u scissor %u\n", op_no, cur_frame,
                     d ? d->res->width0 : 0, d ? d->res->height0 : 0, bi.dst.box.x, bi.dst.box.y, bi.dst.box.width, bi.dst.box.height,
                     s ? s->res->width0 : 0, s ? s->res->height0 : 0, bi.src.box.x, bi.src.box.y, bi.src.box.width, bi.src.box.height, bi.mask, bi.filter, bi.scissor_enable);
   if (!d || !s) return;
   bi.dst.resource = d->res; bi.src.resource = s->res;
   CTX->blit(CTX, &bi);
   mark_written(d->res);
}
static void op_copy(struct rd *r)
{
   struct rres *d = res_get(r32(r)); unsigned dl = r32(r), dx = r32(r), dy = r32(r), dz = r32(r);
   struct rres *s = res_get(r32(r)); unsigned sl = r32(r);
   struct pipe_box b; rd_box(r, &b);
   if (trace) printf("op %u f%u COPY dst %ux%u at %u,%u <- src %ux%u box %d,%d %dx%d\n", op_no, cur_frame, d ? d->res->width0 : 0, d ? d->res->height0 : 0, dx, dy,
                     s ? s->res->width0 : 0, s ? s->res->height0 : 0, b.x, b.y, b.width, b.height);
   if (!d || !s) return;
   CTX->resource_copy_region(CTX, d->res, dl, dx, dy, dz, s->res, sl, &b);
   mark_written(d->res);
}
static void op_texwrite(struct rd *r)
{
   struct rres *x = res_get(r32(r)); unsigned level = r32(r);
   struct pipe_box b; rd_box(r, &b);
   unsigned stride = r32(r), lstride = r32(r); uint64_t h = r64(r);
   const struct hblob *bl = blob_get(h);
   if (!x || !bl) { n_errors++; return; }
   CTX->texture_subdata(CTX, x->res, level, 0, &b, bl->data, stride, lstride);
   mark_written(x->res);
}
static void op_mipgen(struct rd *r)
{
   struct rres *x = res_get(r32(r)); unsigned fmt = r32(r), bl = r32(r), ll = r32(r), fl = r32(r), la = r32(r);
   if (!x) return;
   if (!util_gen_mipmap(CTX, x->res, fmt, bl, ll, fl, la, PIPE_TEX_FILTER_LINEAR)) n_errors++;
   mark_written(x->res);
}

/* ---------------------------------------------------------------- compare */
static void write_ppm(const char *path, const uint8_t *rgb, unsigned w, unsigned h)
{
   FILE *f = fopen(path, "wb");
   if (!f) die("cannot write %s", path);
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   fwrite(rgb, 3, (size_t)w * h, f);
   fclose(f);
}
/* BGRA8 / RGBA8 / B10G10R10A2 rows -> RGB8 */
static void to_rgb(enum pipe_format fmt, const uint8_t *src, unsigned stride, unsigned w, unsigned h, uint8_t *rgb)
{
   for (unsigned y = 0; y < h; y++) {
      const uint8_t *s = src + (size_t)y * stride;
      uint8_t *d = rgb + (size_t)y * w * 3;
      for (unsigned x = 0; x < w; x++) {
         if (fmt == PIPE_FORMAT_B8G8R8A8_UNORM || fmt == PIPE_FORMAT_B8G8R8X8_UNORM) { d[3 * x] = s[4 * x + 2]; d[3 * x + 1] = s[4 * x + 1]; d[3 * x + 2] = s[4 * x]; }
         else if (fmt == PIPE_FORMAT_R8G8B8A8_UNORM || fmt == PIPE_FORMAT_R8G8B8X8_UNORM) { d[3 * x] = s[4 * x]; d[3 * x + 1] = s[4 * x + 1]; d[3 * x + 2] = s[4 * x + 2]; }
         else if (fmt == PIPE_FORMAT_B10G10R10A2_UNORM) {
            uint32_t v; memcpy(&v, s + 4 * x, 4);
            d[3 * x] = (uint8_t)(((v >> 20) & 0x3ff) >> 2); d[3 * x + 1] = (uint8_t)(((v >> 10) & 0x3ff) >> 2); d[3 * x + 2] = (uint8_t)((v & 0x3ff) >> 2);
         } else { d[3 * x] = d[3 * x + 1] = d[3 * x + 2] = 0; }
      }
   }
}

static void present(struct rd *r)
{
   uint32_t id = r32(r), seq = r32(r);
   struct rres *x = res_get(id);
   frames_total++;
   CTX->flush(CTX, NULL, 0);
   if (!x || !frame_result || result_res != id) {
      fprintf(REPORT, "{\"file\":\"%s\",\"label\":\"%s\",\"seq\":%u,\"verdict\":\"ERROR\",\"why\":\"no device result for scanout %u\"}\n", FILEBASE, cur_label, seq, id);
      n_errors++;
      return;
   }
   unsigned w = result_w, h = result_h;
   struct pipe_box box; u_box_3d(0, 0, 0, (int)w, (int)h, 1, &box);
   struct pipe_transfer *tr = NULL;
   uint8_t *p = CTX->texture_map(CTX, x->res, 0, PIPE_MAP_READ, &box, &tr);
   if (!p) die("readback map failed");
   uint8_t *ref = malloc((size_t)w * h * 3), *dev = malloc((size_t)w * h * 3), *dif = malloc((size_t)w * h * 3);
   to_rgb(x->res->format, p, tr->stride, w, h, ref);
   CTX->texture_unmap(CTX, tr);
   to_rgb(x->res->format, frame_result->data, result_stride, w, h, dev);
   unsigned maxa = 0; uint64_t sum = 0, gt8 = 0, gt32 = 0;
   unsigned tw = (w + 31) / 32, th = (h + 31) / 32;
   double *tile = calloc((size_t)tw * th, sizeof(double));
   for (unsigned y = 0; y < h; y++) for (unsigned xx = 0; xx < w; xx++) {
      size_t i = ((size_t)y * w + xx) * 3;
      unsigned m = 0;
      for (int c = 0; c < 3; c++) {
         unsigned a = (unsigned)abs((int)ref[i + c] - (int)dev[i + c]);
         sum += a; if (a > m) m = a;
      }
      if (m > maxa) maxa = m;
      gt8 += m > 8; gt32 += m > 32;
      tile[(y / 32) * tw + xx / 32] += m;
      uint8_t v = m > 255 ? 255 : (uint8_t)(m * 4 > 255 ? 255 : m * 4);
      dif[i] = v; dif[i + 1] = m > 8 ? 0 : v; dif[i + 2] = m > 8 ? 0 : v;
   }
   double worst = 0; unsigned worst_t = 0, bad_tiles = 0;
   char tmap[4096]; size_t tn = 0;
   for (unsigned t = 0; t < tw * th; t++) {
      unsigned x0 = (t % tw) * 32, y0 = (t / tw) * 32;
      unsigned cw = MIN2(32, w - x0), ch = MIN2(32, h - y0);
      double mean = tile[t] / (cw * ch);
      if (mean > worst) { worst = mean; worst_t = t; }
      if (mean > TOL_TILE_MEAN) {
         bad_tiles++;
         if (tn + 24 < sizeof tmap) tn += (size_t)snprintf(tmap + tn, sizeof tmap - tn, "%s[%u,%u]", tn ? "," : "", x0, y0);
      }
   }
   tmap[tn] = 0;
   double px = (double)w * h;
   double gt8p = 100.0 * gt8 / px, gt32p = 100.0 * gt32 / px, mean = (double)sum / (px * 3);
   int match = gt8p <= TOL_PX_GT8_PCT && worst <= TOL_TILE_MEAN;
   char path[1024];
   snprintf(path, sizeof path, "%s/%s-f%u-ref.ppm", OUTDIR, FILEBASE, seq); write_ppm(path, ref, w, h);
   snprintf(path, sizeof path, "%s/%s-f%u-dev.ppm", OUTDIR, FILEBASE, seq); write_ppm(path, dev, w, h);
   snprintf(path, sizeof path, "%s/%s-f%u-diff.ppm", OUTDIR, FILEBASE, seq); write_ppm(path, dif, w, h);
   fprintf(REPORT, "{\"file\":\"%s\",\"label\":\"%s\",\"seq\":%u,\"w\":%u,\"h\":%u,\"verdict\":\"%s\",\"max_abs\":%u,\"mean_abs\":%.3f,"
                   "\"px_gt8_pct\":%.4f,\"px_gt32_pct\":%.4f,\"worst_tile32_mean\":%.2f,\"worst_tile_xy\":[%u,%u],\"bad_tiles\":%u,\"tiles\":\"%s\","
                   "\"inherited\":%u,\"inherited_list\":\"%s\",\"cpu_views\":%u,\"app_gpu_views\":%u,\"decoded_res\":%u,\"errors\":%u,\"gaps\":\"%s\"}\n",
           FILEBASE, cur_label, seq, w, h, match ? "MATCH" : "MISMATCH", maxa, mean, gt8p, gt32p, worst, (worst_t % tw) * 32, (worst_t / tw) * 32,
           bad_tiles, tmap, n_inherited, inherited, n_cpu_used, n_appgpu, n_decoded, n_errors, gaps);
   fflush(REPORT);
   printf("FRAME %s f%u %s px_gt8 %.3f%% worst_tile %.2f max %u inherited %u errors %u\n", FILEBASE, seq, match ? "MATCH" : "MISMATCH", gt8p,
          worst, maxa, n_inherited, n_errors);
   frames_match += match && !n_errors;
   free(ref); free(dev); free(dif); free(tile);
   frame_result = NULL;
}

/* --watch X,Y: after every op, read the pixel (X,Y) of colour buffer 0 when it is a 1125x2436 surface and print the op when it changed */
static void watch_after(const char *what)
{
   if (watch_x < 0 || !FB.cbufs[0].texture) return;
   struct pipe_resource *r = FB.cbufs[0].texture;
   if (r->width0 < 1000 || r->height0 < 2000 || watch_x >= (int)r->width0 || watch_y >= (int)r->height0) return;
   struct pipe_box box; u_box_2d(watch_x, watch_y, 1, 1, &box);
   struct pipe_transfer *tr = NULL;
   uint8_t *p = CTX->texture_map(CTX, r, 0, PIPE_MAP_READ, &box, &tr);
   if (!p) return;
   uint32_t v; memcpy(&v, p, 4); CTX->texture_unmap(CTX, tr);
   if (v != watch_last) {
      printf("WATCH op %u f%u %s (%d,%d) 0x%08x -> 0x%08x vs %u fs %u%s\n", op_no, cur_frame, what, watch_x, watch_y, watch_last, v, st_ids[3], st_ids[4], st_views);
      watch_last = v;
   }
}

/* ---------------------------------------------------------------- driver */
static void replay_file(struct file *f)
{
   RES = _mesa_hash_table_u64_create(NULL);
   OBJ = _mesa_hash_table_u64_create(NULL);
   CTX = SCR->context_create(SCR, NULL, 0);
   if (!CTX) die("context_create failed");
   const char *slash = strrchr(f->path, '/');
   static char base[512];
   snprintf(base, sizeof base, "%s", slash ? slash + 1 : f->path);
   char *dot = strstr(base, ".fcap"); if (dot) *dot = 0;
   FILEBASE = base;
   const uint8_t *p = f->p + 8, *e = f->p + f->n;
   cur_label[0] = 0;
   while (p + 8 <= e) {
      uint32_t type, len; memcpy(&type, p, 4); memcpy(&len, p + 4, 4);
      struct rd r = { p + 8, p + 8 + len };
      if (type >= FC_FB && type != FC_CONTENT) op_no++;
      switch (type) {
      case FC_HDR: { (void)r32(&r); (void)r32(&r); rstr(&r, cur_label, 64); break; }
      case FC_BLOB: break;
      case FC_RES: def_res(&r); break;
      case FC_SHADER: def_shader(&r); break;
      case FC_CSO: def_cso(&r); break;
      case FC_SVIEW: def_view(&r); break;
      case FC_CONTENT: content(&r); break;
      case FC_FRAME: {
         cur_frame = r32(&r);
         gaps_n = 0; gaps[0] = 0; n_inherited = n_cpu_used = n_errors = n_appgpu = 0; inh_n = 0; inherited[0] = 0;
         break;
      }
      case FC_FB: op_fb(&r); break;
      case FC_STATE: op_state(&r); break;
      case FC_DRAW: op_draw(&r); break;
      case FC_CLEAR: op_clear(&r); break;
      case FC_CLEAR_RT: op_clear_rt(&r); break;
      case FC_CLEAR_DS: op_clear_ds(&r); break;
      case FC_BLIT: op_blit(&r); break;
      case FC_COPY: op_copy(&r); break;
      case FC_TEXWRITE: op_texwrite(&r); break;
      case FC_BUFWRITE: break;
      case FC_BARRIER: { unsigned k = r32(&r), fl = r32(&r); if (k == 1 && CTX->texture_barrier) CTX->texture_barrier(CTX, fl); break; }
      case FC_FLUSH: CTX->flush(CTX, NULL, 0); break;
      case FC_MIPGEN: op_mipgen(&r); break;
      case FC_PRESENT: present(&r); break;
      case FC_GAP: { char t[256]; size_t n = len < 255 ? len : 255; memcpy(t, r.p, n); t[n] = 0; for (size_t i = 0; i < n; i++) if (t[i] == '"' || t[i] == '\\') t[i] = '\''; note_gap(t, n); break; }
      case FC_END: break;
      default: die("%s: unknown record type %u", f->path, type);
      }
      p += 8 + len;
   }
   CTX->destroy(CTX);
}

int main(int argc, char **argv)
{
   const char *driver = "llvmpipe";
   int first = 1;
   for (; first < argc && argv[first][0] == '-'; first++) {
      if (!strcmp(argv[first], "--driver") && first + 1 < argc) driver = argv[++first];
      else if (!strcmp(argv[first], "--out") && first + 1 < argc) OUTDIR = argv[++first];
      else if (!strcmp(argv[first], "-v")) verbose = 1;
      else if (!strcmp(argv[first], "--trace")) trace = 1;
      else if (!strcmp(argv[first], "--bbox") && first + 1 < argc) bbox_frame = atol(argv[++first]);
      else if (!strcmp(argv[first], "--watch") && first + 1 < argc) { if (sscanf(argv[++first], "%d,%d", &watch_x, &watch_y) != 2) die("--watch X,Y"); }
      else die("unknown option %s", argv[first]);
   }
   if (!OUTDIR || first >= argc) die("usage: fcap_replay [--driver llvmpipe|softpipe] --out DIR FILE.fcap...");
   mkdir(OUTDIR, 0755);
   glsl_type_singleton_init_or_ref();
   struct sw_winsys *ws = null_sw_create();
   SCR = !strcmp(driver, "softpipe") ? softpipe_create_screen(ws) : llvmpipe_create_screen(ws);
   if (!SCR) die("cannot create %s screen", driver);
   BLOBS = _mesa_hash_table_u64_create(NULL);
   int nf = argc - first;
   struct file *files = calloc((size_t)nf, sizeof *files);
   for (int i = 0; i < nf; i++) { files[i] = read_file(argv[first + i]); load_blobs(&files[i]); }
   char rp[1024]; snprintf(rp, sizeof rp, "%s/report.jsonl", OUTDIR);
   REPORT = fopen(rp, "a");
   if (!REPORT) die("cannot open %s", rp);
   for (int i = 0; i < nf; i++) replay_file(&files[i]);
   fclose(REPORT);
   if (!frames_total) { printf("RESULT: FAIL fcap-replay no frame with a present in %d file(s)\n", nf); return 1; }
   printf("RESULT: %s fcap-replay driver=%s files=%d frames=%u match=%u tolerance px_gt8<=%.1f%% tile32<=%.1f\n", frames_match == frames_total ? "PASS" : "FAIL",
          driver, nf, frames_total, frames_match, TOL_PX_GT8_PCT, TOL_TILE_MEAN);
   return frames_match == frames_total ? 0 : 1;
}
