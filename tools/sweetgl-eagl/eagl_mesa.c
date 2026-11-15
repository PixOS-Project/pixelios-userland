/*
 * eagl_mesa: Mesa backend of the EAGL shim (DESIGN.md s.2-3). Tags: [src mesa/...] = Mesa 26.2.5 tree,
 * [guess] = unverified, nothing here has run on the phone. Compiled only in the mesa-panfrost-ios CI workflow.
 *
 * All callbacks run under the eagl_core mutex (eagl_core.h), so the singleton state below needs no lock of its own.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "frontend/api.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/winsys_handle.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_cb_flush.h"
#include "main/context.h"
#include "main/mtypes.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"

#include "eagl_core.h"
#include "eagl_mesa.h"
#include "gli_table.h"
#include "sweet_drm_mali.h"

struct pipe_screen *panfrost_drm_screen_create(int fd, const struct pipe_screen_config *config);

/* GL entry points of the static Mesa; no header declares them with shared-glapi off. [src mesa/src/mesa/main/teximage.c:3594,
 * texobj.c:1781, get.c:2051, getstring.c:120]. GLenum=unsigned, GLuint=unsigned, GLint=int, GLeglImageOES=void*. */
extern void _mesa_EGLImageTargetTexture2DOES(unsigned target, void *image);
extern void _mesa_BindTexture(unsigned target, unsigned name);
extern void _mesa_TexParameteri(unsigned target, unsigned pname, int param);   /* ALL-ON T-28 ABGR8 swizzle */
extern void _mesa_TexImage2D(unsigned target, int level, int internalformat, int width, int height, int border, unsigned format, unsigned type, const void *pixels);
extern void _mesa_PixelStorei(unsigned pname, int param);
extern unsigned _mesa_GetError(void);
extern void _mesa_GetIntegerv(unsigned pname, int *params);
extern const unsigned char *_mesa_GetString(unsigned name);

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RGB10_A2 0x8059
#define GL_RGBA8 0x8058            /* sized internal format, what the DRI frontend passes [src dri_screen.c:545] */
#define GL_BGRA_EXT 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define FOURCC_BGRA 0x42475241u    /* 'BGRA' [guess: the ObjC layer must pass kCVPixelFormatType_32BGRA as this id] */
#define FOURCC_B3A8 0x62336138u    /* 'b3a8': the pixel format of CoreAnimation's display surfaces (2 planes, 1125x2436, bpr 4544; gputest25-38 `SWEET SURF create`); plane 0 is read as linear BGRA8 by the CPU copy that shows the software-rendered lockscreen [obs g24], so the GPU writes the same bytes [guess: R/B order and the metadata plane are unverified] */
#define FOURCC_W30R 0x77333072u    /* 'w30r' (kCVPixelFormatType_30RGBLEPackedWideGamut: x2 R10 G10 B10, little endian): CoreAnimation's intermediate 1125x2436 / 2436x1125 surfaces (gputest40: ids 4-7, REJECTED before); bit layout B 9:0, G 19:10, R 29:20 matches PIPE_FORMAT_B10G10R10A2_UNORM [guess: wide-gamut value range is not remapped] */
#define MOD_LINEAR 0ull            /* DRM_FORMAT_MOD_LINEAR = fourcc_mod_code(NONE,0) [src include/drm-uapi/drm_fourcc.h:544] */

static const driOptionDescription driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "panfrost/driinfo_panfrost.h"
};

/* ---- screen ---- */

static struct {
   struct pipe_frontend_screen fscreen;
   int up;
   driOptionCache info, cache;
} S;

/* The EGLImage cookie CA-side code never sees: a stack struct built by import_bind() and read back by get_egl_image. */
struct img_cookie { struct pipe_resource *res; };

/* EGLImage internal format for a pipe format (shared by fs_get_egl_image and the format-0 diagnostics, research 86 sections E/F/H, DRAFT-format0-batch) */
static unsigned egl_image_ifmt(enum pipe_format f)
{
   return (f == PIPE_FORMAT_B10G10R10A2_UNORM || f == PIPE_FORMAT_R10G10B10A2_UNORM) ? GL_RGB10_A2 : GL_RGBA8;
}

static bool fs_get_egl_image(struct pipe_frontend_screen *fs, void *image, struct st_egl_image *out)
{
   (void)fs;
   struct img_cookie *c = image;
   if (!c || !c->res)
      return false;
   out->texture = NULL;
   pipe_resource_reference(&out->texture, c->res);   /* owned by the caller, who unreferences it [src api.h:131] */
   out->format = c->res->format;
   out->level = 0;
   out->layer = 0;
   /* GLES refuses a 10-bit Mesa format (B10G10R10A2, 'w30r' surfaces) as a colour attachment unless the texture's internal format is GL_RGB10_A2
    * [src mesa fbobject.c is_format_color_renderable: internalFormat != GL_RGB10_A2 && format == MESA_FORMAT_B10G10R10A2_UNORM -> not renderable]; with RGBA8 every
    * FBO on a w30r surface was INCOMPLETE_ATTACHMENT ('texture attachment incomplete', g86): CA's offscreen layer buffers were never drawn (research 61, review
    * gli-surface-format-hint F2) */
   out->internalformat = egl_image_ifmt(c->res->format);
   return true;
}

static bool fs_validate_egl_image(struct pipe_frontend_screen *fs, void *image)
{
   (void)fs;
   return image != NULL;
}

/* called unconditionally by st_api_create_context [src st_manager.c:1075] */
static int fs_get_param(struct pipe_frontend_screen *fs, enum st_manager_param p)
{
   (void)fs; (void)p;
   return 0;
}

/* frame capture (fcap_impl.c, included at the end of this file) */
static void fcap_install(struct pipe_screen *scr);
static void fcap_note_surface(struct pipe_resource *r, const struct sweet_gl_surface *s, int attach);
static void fcap_on_flush(void);

static int screen_up(void)
{
   if (S.up)
      return 0;
   int fd = sweet_drm_mali_open();
   if (fd < 0)
      return -errno;
   memset(&S.cache, 0, sizeof(S.cache));   /* same as gl_clear_test.c (ran green in CI link only, not on hardware) */
   driParseOptionInfo(&S.info, driconf, sizeof(driconf) / sizeof(driconf[0]));
   struct pipe_screen_config cfg = { .options = &S.cache, .options_info = &S.info };
   struct pipe_screen *scr = panfrost_drm_screen_create(fd, &cfg);
   if (!scr) {
      close(fd);
      return -ENODEV;
   }
   S.fscreen.screen = scr;
   S.fscreen.get_egl_image = fs_get_egl_image;
   S.fscreen.validate_egl_image = fs_validate_egl_image;
   S.fscreen.get_param = fs_get_param;
   fcap_install(scr);   /* no-op unless /private/var/dev/fcap-on (or SWEETGL_FCAP=1); must run before the first context */
   S.up = 1;
   return 0;
}

/* G-11 (research 61, gputest71): CoreAnimation's icon / mask surfaces arrive with IOSurface pixel format 0 (600x600 pitch 2400 = 4 bytes/px) or 'LA08'
 * (230x186 pitch 480), never BGRA, so format_ok rejected them and the sampler read nothing (black icons). The pixel format 0 surfaces are described only by
 * the GL `format` argument of texImageIOSurface (gl_hint, 0 = no hint, set by be_tex_image_surface), so it decides, checked against the pitch. */
#define FOURCC_A008 0x41303038u   /* 'A008': 1 byte alpha masks (text labels) [obs gputest93: 566x79 bpr 576 bpe 1, created through IOSurfaceCreate with IOSurfaceAddress since patch 0161] */
#define FOURCC_L008 0x4c303038u   /* 'L008' [obs earlier runs: WeatherPoster] */
#define FOURCC_RGHA 0x52476841u   /* 'RGhA': RGBA half float, 8 bytes/px (256x1 gradient LUTs, bpe 8) [obs gputest93] */
#define FOURCC_LA08 0x4c413038u   /* 'LA08' [obs gputest71: 230x186 pitch 480 = 2 bytes/px + padding] */
static unsigned gl_hint;

static enum pipe_format surface_pipe_format(const struct sweet_gl_surface *s)
{
   enum pipe_format pf = PIPE_FORMAT_NONE;
   unsigned bpp = 0;
   if (s->format == FOURCC_W30R) {
      pf = PIPE_FORMAT_B10G10R10A2_UNORM; bpp = 4;
   } else if (s->format == FOURCC_BGRA || s->format == GL_BGRA_EXT || s->format == FOURCC_B3A8) {
      pf = PIPE_FORMAT_B8G8R8A8_UNORM; bpp = 4;
   } else if (s->format == FOURCC_A008) {
      pf = PIPE_FORMAT_A8_UNORM; bpp = 1;
   } else if (s->format == FOURCC_L008) {
      pf = PIPE_FORMAT_L8_UNORM; bpp = 1;
   } else if (s->format == FOURCC_RGHA) {
      pf = PIPE_FORMAT_R16G16B16A16_FLOAT; bpp = 8;
   } else if (s->format == FOURCC_LA08) {
      pf = PIPE_FORMAT_L8A8_UNORM; bpp = 2;
   } else if (s->format == 0) {
      switch (gl_hint) {
      case 0x80E1: pf = PIPE_FORMAT_B8G8R8A8_UNORM; bpp = 4; break;   /* GL_BGRA_EXT */
      case 0x1908: pf = PIPE_FORMAT_R8G8B8A8_UNORM; bpp = 4; break;   /* GL_RGBA */
      case 0x190A: pf = PIPE_FORMAT_L8A8_UNORM; bpp = 2; break;       /* GL_LUMINANCE_ALPHA */
      case 0x1909: pf = PIPE_FORMAT_L8_UNORM; bpp = 1; break;         /* GL_LUMINANCE */
      case 0x1906: pf = PIPE_FORMAT_A8_UNORM; bpp = 1; break;         /* GL_ALPHA */
      case 0x1903: pf = PIPE_FORMAT_R8_UNORM; bpp = 1; break;         /* GL_RED */
      default: break;
      }
      if (pf == PIPE_FORMAT_NONE && gl_hint == 0 && s->bytes_per_row >= (uint64_t)s->w * 4 && s->bytes_per_row <= (uint64_t)s->w * 4 + 127 &&
          s->len >= (uint64_t)s->bytes_per_row * s->h) {   /* 4 bytes/px with at most alignment padding (review gli-attach-format0): a 1-2 byte format with a wide pitch is not guessed */
         /* attach path (attachImage:toCoreSurface: has no GL format): CA's intermediate / blur render targets (97x76, 103x103, 101x101, format 0,
          * gputest71-74) are 4 bytes/px; CA samples format-0 surfaces as GL RGBA (texImageIOSurface passes 0x1908 for them), so render them as
          * R8G8B8A8 too: one pipe format for render and sample, no red/blue swap (research 61, G-11c) */
         pf = PIPE_FORMAT_R8G8B8A8_UNORM; bpp = 4;
         {
            static unsigned chosen;
            if (chosen < 8) { chosen++; fprintf(stderr, "sweetgl: attach surface id %u fmt 0 %ux%u pitch %u -> R8G8B8A8 (guess by 4 bytes/px)\n", s->id, s->w, s->h, s->bytes_per_row); }
         }
      }
   }
   if (pf != PIPE_FORMAT_NONE && s->bytes_per_row < (uint64_t)s->w * bpp)
      return PIPE_FORMAT_NONE;   /* the pitch cannot hold a row of that format */
   return pf;
}

static unsigned pipe_bpp(enum pipe_format pf)
{
   switch (pf) {
   case PIPE_FORMAT_B8G8R8A8_UNORM: case PIPE_FORMAT_R8G8B8A8_UNORM: case PIPE_FORMAT_B10G10R10A2_UNORM: return 4;
   case PIPE_FORMAT_L8A8_UNORM: return 2;
   case PIPE_FORMAT_R16G16B16A16_FLOAT: return 8;
   default: return 1;
   }
}

/* Pitch of an imported linear resource must be a multiple of 64 on Valhall (arch >= 7) [src pan_layout.h:228 pan_linear_or_tiled_row_align_req, review
 * gli-surface-format-hint F1]; CA's small surfaces (pitch 2400, 480, 672) are not, and the sampler-only formats' EGLImage internalformat is not
 * modelled (fs_get_egl_image says RGBA8). Both are uploaded with a plain GL TexImage2D from the CPU mapping instead (copy; icons are static). */
static int needs_upload(const struct sweet_gl_surface *s, enum pipe_format pf)
{
   if (!s->cpu) return 0;
   return pf == PIPE_FORMAT_L8A8_UNORM || pf == PIPE_FORMAT_L8_UNORM || pf == PIPE_FORMAT_A8_UNORM || pf == PIPE_FORMAT_R8_UNORM || pf == PIPE_FORMAT_R16G16B16A16_FLOAT ||
          (s->bytes_per_row % 64) != 0;
}

static int upload_surface(const struct sweet_gl_surface *s, enum pipe_format pf)
{
   unsigned bpp = pipe_bpp(pf), x, y;
   unsigned ifmt, fmt, gltype = GL_UNSIGNED_BYTE;
   if (pf == PIPE_FORMAT_B10G10R10A2_UNORM) { sweet_gap("surface", "copy-path upload of a 10-bit surface not implemented"); return -ENOTSUP; }   /* display surfaces are 64-aligned and take the import path */
   if ((uint64_t)s->bytes_per_row * s->h > s->len) { sweet_gap("surface", "copy-path upload: pitch*height exceeds the surface length"); return -EINVAL; }
   switch (pf) {
   case PIPE_FORMAT_L8A8_UNORM: ifmt = fmt = 0x190A; break;      /* GL_LUMINANCE_ALPHA */
   case PIPE_FORMAT_L8_UNORM: ifmt = fmt = 0x1909; break;        /* GL_LUMINANCE */
   case PIPE_FORMAT_A8_UNORM: ifmt = fmt = 0x1906; break;        /* GL_ALPHA */
   case PIPE_FORMAT_R8_UNORM: ifmt = 0x8229; fmt = 0x1903; break;   /* GL_R8, GL_RED */
   case PIPE_FORMAT_R16G16B16A16_FLOAT: ifmt = 0x881A; fmt = 0x1908; gltype = 0x140B; break;   /* GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT */
   default: ifmt = 0x8058; fmt = 0x1908; break;                  /* GL_RGBA8, GL_RGBA */
   }
   unsigned char *tmp = malloc((size_t)s->w * s->h * bpp);
   if (!tmp) return -ENOMEM;
   for (y = 0; y < s->h; y++) {
      const unsigned char *src = (const unsigned char *)s->cpu + (size_t)y * s->bytes_per_row;
      unsigned char *dst = tmp + (size_t)y * s->w * bpp;
      if (pf == PIPE_FORMAT_B8G8R8A8_UNORM) {
         for (x = 0; x < s->w; x++) { dst[4 * x] = src[4 * x + 2]; dst[4 * x + 1] = src[4 * x + 1]; dst[4 * x + 2] = src[4 * x]; dst[4 * x + 3] = src[4 * x + 3]; }
      } else {
         memcpy(dst, src, (size_t)s->w * bpp);
      }
   }
   {  /* diagnostic (research 61): how much of the surface carries data when it is copied; the first uploads only */
      static unsigned nz_logged, nz_small;
      /* glyph masks (sprint GPU 2026-11-10: the App Library magnifier / keyboard / tab-bar symbols are small LA08 copy-path surfaces): every
       * upload of a surface up to 64x64 is logged (cap 400), to see whether the bytes are there when CA attaches it (copy = one-shot snapshot) */
      int small = s->w <= 64 && s->h <= 64 && nz_small < 400;
      if (nz_logged < 16 || small) {
         if (small) nz_small++;
         size_t k, total = (size_t)s->w * s->h * bpp, nz = 0;
         for (k = 0; k < total; k++) nz += tmp[k] != 0;
         nz_logged++;
         fprintf(stderr, "sweetgl: upload surface id %u %ux%u fmt %d nonzero bytes %zu of %zu\n", s->id, s->w, s->h, (int)pf, nz, total);
      }
   }
   int al = 4;
   _mesa_GetIntegerv(GL_UNPACK_ALIGNMENT, &al);
   _mesa_PixelStorei(GL_UNPACK_ALIGNMENT, 1);
   while (_mesa_GetError() != 0) {}   /* drain CA's pending errors so ours is attributable */
   _mesa_TexImage2D(GL_TEXTURE_2D, 0, (int)ifmt, (int)s->w, (int)s->h, 0, fmt, gltype, tmp);
   unsigned gerr = _mesa_GetError();
   _mesa_PixelStorei(GL_UNPACK_ALIGNMENT, al);
   free(tmp);
   {
      int bound = 0;
      _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
      sweet_gli_note_tex_size((unsigned)bound, s->w, s->h);   /* the draw hook divides rectangle coordinates by this size */
   }
   if (gerr) {
      static unsigned failed;
      if (failed < 8) { failed++; fprintf(stderr, "sweetgl: upload TexImage2D ifmt 0x%x fmt 0x%x %ux%u GL error 0x%x\n", ifmt, fmt, s->w, s->h, gerr); }
      return -EINVAL;
   }
   return 0;
}

/* ---- IOSurface -> pipe_resource cache (DESIGN.md s.3 step 4) ---- */

#define CACHE_N 32
static struct cent {
   struct pipe_resource *res;
   uint32_t id, w, h, pitch, pf;
   uint64_t va;
   unsigned long stamp;
   uint32_t fmt;   /* IOSurface pixel format (LOWCLK real-flush: 'b3a8' = a display surface) */
} cache[CACHE_N];
static unsigned long tick;

static void cache_drop(struct cent *e)
{
   if (e->res)
      pipe_resource_reference(&e->res, NULL);
   memset(e, 0, sizeof(*e));
}

/* Fake dma-buf fds (sprint GPU 2026-11-10, settings-open launch hitch): Mesa only reads the size from the fd (lseek SEEK_END, pan_kmod.c:172), and
 * drmPrimeFDToHandle maps it to the surface through the registration. The old path made a tmpfile() + ftruncate(len) + fclose() on the HFS root for
 * EVERY new surface; the pool keeps one unlinked file per distinct size (CA reuses few sizes). /private/var/dev/sweetgl-imppool = "0" restores the
 * old path (A/B in a hot-reload session). Witness: 'SWEETGL: IMPORT' lines (time per import, pooled or not). */
#define IMP_POOL 64
static struct { uint64_t size; FILE *f; unsigned long stamp; } imp_pool[IMP_POOL];
static unsigned long imp_tick;
static int imp_pool_on = -1;
static FILE *imp_fd_for(uint64_t len, int *pooled)
{
   if (imp_pool_on < 0) {
      FILE *c = fopen("/private/var/dev/sweetgl-imppool", "r");
      int ch = c ? fgetc(c) : '1';
      if (c) fclose(c);
      imp_pool_on = ch != '0';
      fprintf(stderr, "sweetgl: IMPORT fd pool %s\n", imp_pool_on ? "ON" : "OFF (/private/var/dev/sweetgl-imppool = 0)");
   }
   *pooled = 0;
   if (!imp_pool_on) return tmpfile();
   int lru = 0;
   for (int i = 0; i < IMP_POOL; i++) {
      if (imp_pool[i].f && imp_pool[i].size == len) { imp_pool[i].stamp = ++imp_tick; *pooled = 1; return imp_pool[i].f; }
      if (!imp_pool[i].f || imp_pool[i].stamp < imp_pool[lru].stamp) lru = i;
   }
   FILE *f = tmpfile();
   if (!f) return NULL;
   if (ftruncate(fileno(f), (off_t)len) != 0) { fclose(f); return NULL; }
   if (imp_pool[lru].f) fclose(imp_pool[lru].f);
   imp_pool[lru].f = f; imp_pool[lru].size = len; imp_pool[lru].stamp = ++imp_tick;
   return f;
}

/* New reference-counted resource for the surface (caller unreferences). NULL + errno on failure. */
static struct pipe_resource *import_resource(const struct sweet_gl_surface *s)
{
   struct pipe_screen *scr = S.fscreen.screen;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   int pooled = 0;
   FILE *f = imp_fd_for(s->len, &pooled);   /* the fake dma-buf, size read back by lseek(SEEK_END) [src pan_kmod.c:172] */
   if (!f)
      return NULL;
   int fd = fileno(f), err = 0;
   struct pipe_resource *res = NULL;
   if (!pooled && !imp_pool_on && ftruncate(fd, (off_t)s->len) != 0) {
      err = errno;
   } else if ((err = -sweet_drm_mali_register_surface(fd, s->id, s->len)) == 0) {
      struct pipe_resource t;
      memset(&t, 0, sizeof(t));
      t.target = PIPE_TEXTURE_2D;
      t.format = surface_pipe_format(s);
      t.width0 = s->w;
      t.height0 = s->h;
      t.depth0 = 1;
      t.array_size = 1;
      t.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
      if (t.format == PIPE_FORMAT_B8G8R8A8_UNORM || t.format == PIPE_FORMAT_B10G10R10A2_UNORM || t.format == PIPE_FORMAT_R8G8B8A8_UNORM)
         t.bind |= PIPE_BIND_RENDER_TARGET;   /* L8A8 / L8 / A8 / R8 are sampler-only */
      struct winsys_handle wh;
      memset(&wh, 0, sizeof(wh));
      wh.type = WINSYS_HANDLE_TYPE_FD;
      wh.handle = (unsigned)fd;
      wh.stride = s->bytes_per_row;
      wh.modifier = MOD_LINEAR;
      res = scr->resource_from_handle(scr, &t, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (!res)
         err = EINVAL;   /* [guess] pitch/alignment rejected by panfrost's linear rule, or importSurface -ENOSYS */
      sweet_drm_mali_unregister_surface(fd);
   }
   if (!imp_pool_on) fclose(f);   /* the caller keeps ownership of the fd during import, and the import does not keep it [src pan_kmod.c:151-192] */
   clock_gettime(CLOCK_MONOTONIC, &t1);
   {
      static unsigned n, slow; static uint64_t sum_us, max_us;
      uint64_t us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000u + (uint64_t)(t1.tv_nsec - t0.tv_nsec) / 1000u;
      n++; sum_us += us; if (us > max_us) max_us = us; if (us > 2000) slow++;
      if (n <= 48 || us > 5000)
         fprintf(stderr, "sweetgl: IMPORT surface %u %ux%u len %llu us %llu pooled %d pool %s\n", s->id, s->w, s->h, (unsigned long long)s->len,
                 (unsigned long long)us, pooled, imp_pool_on ? "on" : "off");
      if (n % 200 == 0)
         fprintf(stderr, "sweetgl: IMPORT summary n %u mean_us %llu max_us %llu over2ms %u pool %s\n", n, (unsigned long long)(sum_us / n),
                 (unsigned long long)max_us, slow, imp_pool_on ? "on" : "off");
   }
   errno = err;
   return res;
}

static int format_ok(const struct sweet_gl_surface *s)
{
   int ok = surface_pipe_format(s) != PIPE_FORMAT_NONE;
   static unsigned logged;
   if (logged < 16) {   /* gputest25-38: no attach ever logged; the display surfaces ('b3a8') were rejected here before 'b3a8' was accepted */
      logged++;
      sgl_log("attach surface id %u fmt 0x%x %ux%u pitch %u len %lu: %s", s->id, (unsigned)s->format, (unsigned)s->w, (unsigned)s->h,
              (unsigned)s->bytes_per_row, (unsigned long)s->len, ok ? "accepted" : "REJECTED (format)");
   }
   return ok;
}

/* Bind the surface as storage of the texture currently bound to GL_TEXTURE_2D in the current context. */
static int bind_to_current_texture(const struct sweet_gl_surface *s)
{
   if (!S.up || !format_ok(s)) {
      if (!S.up) sweet_gap("surface", "screen not up: surface rejected");
      else sweet_gap("surface", "format 0x%x not supported (gl hint 0x%x): surface rejected, its texture stays empty", (unsigned)s->format, gl_hint);
      {  /* the first rejections are logged (research 61): a mask surface may use a format format_ok does not know yet */
         static unsigned rej;
         if (rej < 16) { rej++; fprintf(stderr, "sweetgl: surface id %u fmt 0x%x %ux%u pitch %u rejected (up=%d hint 0x%x)\n", s->id, s->format, s->w, s->h, s->bytes_per_row, (int)S.up, gl_hint); }
      }
      sweet_drm_mali_release_surface(s->id);   /* the ObjC layer imported it (selector 5) but Mesa will never take it: boClose */
      return -EINVAL;
   }
   struct cent *e = NULL, *old = &cache[0];
   for (int i = 0; i < CACHE_N; i++) {
      struct cent *c = &cache[i];
      if (c->res && c->id == s->id && c->w == s->w && c->h == s->h && c->pitch == s->bytes_per_row && c->pf == (uint32_t)surface_pipe_format(s) && c->va == s->va) {
         e = c;
         break;
      }
      if (!c->res || c->stamp < old->stamp)
         old = c;
   }
   if (!e) {
      struct pipe_resource *res = import_resource(s);
      if (!res) {
         int err = errno ? errno : EIO;
         sweet_gap("surface", "dma-buf import failed errno %d (pitch alignment / format?)", err);
         {
            static unsigned failed;
            if (failed < 8) { failed++; fprintf(stderr, "sweetgl: import of surface id %u fmt 0x%x pipe fmt %d %ux%u pitch %u failed: errno %d\n", s->id, (unsigned)s->format, (int)surface_pipe_format(s), s->w, s->h, s->bytes_per_row, err); }
         }
         sweet_drm_mali_release_surface(s->id);   /* no-op if Mesa took the handle (its resource free closes it) */
         return -err;
      }
      e = old;
      cache_drop(e);
      *e = (struct cent){ res, s->id, s->w, s->h, s->bytes_per_row, (uint32_t)surface_pipe_format(s), s->va, 0, (uint32_t)s->format };
   }
   e->stamp = ++tick;
   fcap_note_surface(e->res, s, 0);
   struct img_cookie ck = { e->res };
   _mesa_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, &ck);   /* GL errors surface in glGetError of CA's context, not here */
   {
      int bound = 0;
      _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
      sweet_gli_note_tex_size((unsigned)bound, s->w, s->h);
   }
   return 0;
}

/* ---- callbacks ---- */

static void *be_create_context(void *user, int requested_api, int *out_api)
{
   (void)user; (void)requested_api;   /* ES2 requests are served by an ES3 context (DESIGN.md s.2) */
   if (screen_up() != 0)
      return NULL;
   struct st_context_attribs a;
   memset(&a, 0, sizeof(a));              /* visual zeroed: color_format NONE = surfaceless [src st_manager.c:1018] */
   a.profile = API_OPENGLES2;
   a.major = 3;
   a.minor = 0;
   enum st_context_error err = ST_CONTEXT_SUCCESS;
   struct st_context *st = st_api_create_context(&S.fscreen, &a, &err, NULL);
   sgl_log("create ctx %s st=%p err=%d tid=%lu", st ? "ok" : "FAILED", (void *)st, (int)err, (unsigned long)(uintptr_t)pthread_self());
   if (!st)
      return NULL;
   *out_api = st->ctx->Version / 10;      /* ctx->Version is major*10+minor, ES 3.0 = 30 [src st_manager.c:1065] */
   return st;
}

static void be_destroy_context(void *user, void *bctx)
{
   (void)user;
   st_destroy_context(bctx);
   /* Evict the import cache: each resource unreference ends in GEM_CLOSE -> boClose once no texture still holds it. */
   for (int i = 0; i < CACHE_N; i++)
      cache_drop(&cache[i]);
}

static int be_make_current(void *user, void *bctx)
{
   (void)user;
   int r = st_api_make_current(bctx, NULL, NULL) ? 0 : -EIO;
   sgl_log("make_current bctx=%p ret=%d tid=%lu", bctx, r, (unsigned long)(uintptr_t)pthread_self());
   return r;
}

/* ALL-ON T-28 (research: RM-01029..01036 [bin], replay A/B 2026-11-12 [obs]): CA's CPU images in CA format 15 'ABGR8' (CGBitmap
 * PremultipliedLast | ByteOrder32Little = memory bytes A,B,G,R) reach us as format-0 IOSurfaces through attachImage:toCoreSurface: (no GL
 * format). iOS draws them through Metal with ImageFunction 6 (t.wzyx); on the GLES path CA's attach fallback samples raw .w, which our RGBA8
 * guess turns into byte 3 = 0 (Settings row chevrons, back chevron: invisible). CA's own offscreen render targets also arrive as format-0
 * attach surfaces and must keep RGBA8 semantics. Rule: a format-0 surface attached as the guessed R8G8B8A8 samples with the swizzle
 * (R<-A, G<-B, B<-G, A<-R) = A8B8G8R8 until any texture holding it is attached to a framebuffer (GL renders into it = a render target);
 * then the swizzle goes back to identity and the surface is remembered as a render target. /private/var/dev/sweetgl-abgr8 = "0" turns it off. */
#define ABGR_N 256
static struct { unsigned tex, sid; } abgr_tex[ABGR_N];
static unsigned abgr_rt[ABGR_N], abgr_rt_n;
static int abgr_on = -1;
static int abgr_enabled(void)
{
   if (abgr_on < 0) {
      FILE *c = fopen("/private/var/dev/sweetgl-abgr8", "r");
      int ch = c ? fgetc(c) : '1';
      if (c) fclose(c);
      abgr_on = ch != '0';
      fprintf(stderr, "sweetgl: ABGR8 format-0 attach images %s\n", abgr_on ? "ON (swizzle wzyx until FBO-bound)" : "OFF (/private/var/dev/sweetgl-abgr8 = 0)");
   }
   return abgr_on;
}
static int abgr_is_rt(unsigned sid) { for (unsigned i = 0; i < abgr_rt_n && i < ABGR_N; i++) if (abgr_rt[i] == sid) return 1; return 0; }
/* swizzle on the CURRENTLY BOUND 2D texture; returns the first GL error (0 = ok) */
static unsigned abgr_set_swizzle(int abgr)
{
   static const int sw_abgr[4] = { 0x1906 /* ALPHA */, 0x1905 /* BLUE */, 0x1904 /* GREEN */, 0x1903 /* RED */ };
   static const int sw_id[4] = { 0x1903, 0x1904, 0x1905, 0x1906 };
   while (_mesa_GetError() != 0) {}
   for (int k = 0; k < 4; k++) _mesa_TexParameteri(GL_TEXTURE_2D, 0x8E42 + k /* TEXTURE_SWIZZLE_R..A */, abgr ? sw_abgr[k] : sw_id[k]);
   return _mesa_GetError();
}
static void abgr_after_attach(uint32_t tex, const struct sweet_gl_surface *surf, enum pipe_format pf)
{
   unsigned i, slot = ABGR_N;
   for (i = 0; i < ABGR_N; i++) { if (abgr_tex[i].tex == tex) { slot = i; break; } if (slot == ABGR_N && !abgr_tex[i].tex) slot = i; }
   int want = surf->format == 0 && pf == PIPE_FORMAT_R8G8B8A8_UNORM && abgr_enabled() && !abgr_is_rt(surf->id);
   if (!want && (slot == ABGR_N || abgr_tex[slot].tex != tex)) return;   /* nothing set on this name before: leave CA's texture alone */
   unsigned e = abgr_set_swizzle(want);
   static unsigned logged;
   if (e) sweet_gap("surface", "ABGR8 swizzle on texture %u (surface %u) refused: GL error 0x%x", (unsigned)tex, surf->id, e);
   else if (logged < 64) { logged++; fprintf(stderr, "sweetgl: ABGR8 texture %u <- format-0 surface %u %ux%u: swizzle %s\n", (unsigned)tex, surf->id, surf->w, surf->h, want ? "wzyx (CPU image)" : "identity (render target)"); }
   if (slot == ABGR_N) { static unsigned full; if (!full++) sweet_gap("surface", "ABGR8 texture table full (%u): texture %u not tracked", ABGR_N, (unsigned)tex); return; }
   if (want && !e) { abgr_tex[slot].tex = tex; abgr_tex[slot].sid = surf->id; } else if (abgr_tex[slot].tex == tex) abgr_tex[slot].tex = 0;
}
void sgl_abgr_on_fbo(unsigned tex);   /* used by gli_table.c */
/* called by the glFramebufferTexture2D hook (gli_table.c) BEFORE the attach: a tracked texture becomes a render target */
void sgl_abgr_on_fbo(unsigned tex)
{
   for (unsigned i = 0; i < ABGR_N; i++) if (abgr_tex[i].tex == tex) {
      unsigned sid = abgr_tex[i].sid;
      int prev = 0;
      _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
      _mesa_BindTexture(GL_TEXTURE_2D, tex);
      unsigned e = abgr_set_swizzle(0);
      _mesa_BindTexture(GL_TEXTURE_2D, (unsigned)prev);
      if (!abgr_is_rt(sid)) abgr_rt[abgr_rt_n++ % ABGR_N] = sid;
      for (unsigned j = 0; j < ABGR_N; j++) if (abgr_tex[j].sid == sid) abgr_tex[j].tex = 0;   /* every name of this surface: render target now */
      static unsigned logged;
      if (e) sweet_gap("surface", "ABGR8 swizzle reset on FBO texture %u (surface %u) failed: GL error 0x%x", tex, sid, e);
      else if (logged < 64) { logged++; fprintf(stderr, "sweetgl: ABGR8 texture %u (surface %u) attached to a framebuffer: render target, swizzle identity\n", tex, sid); }
      return;
   }
}

static int be_attach_surface(void *user, void *bctx, uint32_t tex, const struct sweet_gl_surface *surf, int inverted)
{
   (void)user; (void)bctx;
   if (!surf)
      return 0;   /* the GL texture stays CA's; cache entries live until evicted (DESIGN.md s.3 step 7) */
   if (inverted) {
      sweet_gap("surface", "attachImage with invertedRender (flip) not supported");
      return -ENOTSUP;   /* no flip designed yet (DESIGN.md s.3 step 5) */
   }
   {  /* which GL texture got which surface (G-11: FBO 5 / texture 25 is incomplete, research 61) */
      static unsigned an;
      if (an < 200 && !(surf->w == 1125 && surf->h == 2436)) { an++; fprintf(stderr, "sweetgl: attach texture %u <- surface id %u fmt 0x%x %ux%u pitch %u\n", (unsigned)tex, surf->id, (unsigned)surf->format, surf->w, surf->h, surf->bytes_per_row); }
   }
   sweet_th_note_attach((unsigned)tex, surf->id, surf->w, surf->h, (unsigned)surf->format);   /* census texture history (gli_table.c); no-op unless SWEETGL_CENSUS=1 */
   if (surf->format == 0 && !sgl_fmt0_was_attached(surf->id)) {  /* research 86 sections E/F/H, DRAFT-format0-batch: what the attach path makes of a format-0 surface (no GL format hint here); once per new surface id (review format0-a1a3 F1: CA re-attaches the same surfaces every frame) */
      char line[160];
      unsigned saved = gl_hint;
      gl_hint = 0;
      enum pipe_format pf = surface_pipe_format(surf);
      gl_hint = saved;
      if (sgl_fmt0_note_attach(surf->id)) {
         sgl_fmt0_attach_line(line, sizeof line, surf->id, surf->w, surf->h, surf->bytes_per_row, (int)pf, egl_image_ifmt(pf));
         fprintf(stderr, "%s\n", line);
      }
   }
   int prev = 0;
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
   _mesa_BindTexture(GL_TEXTURE_2D, tex);
   int r = bind_to_current_texture(surf);
   if (!r && surf->format == 0) {   /* ALL-ON T-28: ABGR8 CPU images (see abgr_after_attach) */
      unsigned saved = gl_hint;
      gl_hint = 0;
      enum pipe_format pf = surface_pipe_format(surf);
      gl_hint = saved;
      abgr_after_attach(tex, surf, pf);
   }
   _mesa_BindTexture(GL_TEXTURE_2D, (unsigned)prev);
   return r;
}

static int be_tex_image_surface(void *user, void *bctx, const struct sweet_gl_surface *surf, uint32_t target,
                                uint32_t internalformat, uint32_t w, uint32_t h, uint32_t format, uint32_t type,
                                uint32_t plane, int invert)
{
   (void)user; (void)bctx;
   if (invert) {
      sweet_gap("surface", "texImageIOSurface with invert not supported");
      return -ENOTSUP;
   }
   {  /* G-11 (research 61): log the first calls so the target / plane / type CoreAnimation really uses are observed, not guessed */
      static unsigned logged;
      if (logged < 16) {
         logged++;
         fprintf(stderr, "sweetgl: texImageIOSurface tgt 0x%x ifmt 0x%x fmt 0x%x plane %u type 0x%x %ux%u surface %ux%u spf 0x%x\n", target, internalformat, format, plane, type, w, h, surf->w, surf->h, (unsigned)surf->format);
      }
   }
   {  /* unit-1 witness (env SWEETGL_U1W=1): every texImageIOSurface with the active unit and the 2D name it lands on */
      static int u1w = -1;
      if (u1w < 0) { const char *e = getenv("SWEETGL_U1W"); u1w = e && e[0] == '1'; }
      static unsigned n;
      if (u1w && n < 200) {
         int act = 0, b2 = 0;
         n++;
         _mesa_GetIntegerv(0x84E0, &act);
         _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &b2);
         fprintf(stderr, "sweetgl: U1W teximage unit %d tgt 0x%x bound2d %d surf %u %ux%u fmt 0x%x ifmt 0x%x\n", act - 0x84C0, target, b2, surf->id, surf->w, surf->h, (unsigned)surf->format, internalformat);
      }
   }
   /* GL_TEXTURE_RECTANGLE_ARB (0x84F5) is what CA uses for its icon-mask surfaces; the surface is imported as a plain 2D texture
    * (libsweetgl's glBindTexture hook maps the bind target the same way under SWEETGL_RECT=1) */
   if ((target != GL_TEXTURE_2D && target != 0x84F5) || plane != 0 || (type != GL_UNSIGNED_BYTE && type != 0x140B /* GL_HALF_FLOAT: only for 'RGhA', checked below */) || w != surf->w || h != surf->h) {
      sweet_gap("surface", "texImageIOSurface refused (target 0x%x plane %u type 0x%x or size mismatch)", target, plane, type);
      return -EINVAL;
   }
   gl_hint = format;
   enum pipe_format upf = surface_pipe_format(surf);
   int r;
   if (surf->format == 0) {  /* research 86 sections E/F/H, DRAFT-format0-batch: sample-side pair of the fmt0 attach line, first 2 per surface id, 128 lines */
      static unsigned sn;
      if (sn < 128 && sgl_fmt0_sample_count(surf->id) < 2) {
         char line[160];
         sn++;
         sgl_fmt0_sample_line(line, sizeof line, surf->id, surf->w, surf->h, egl_image_ifmt(upf), format, type, sgl_fmt0_was_attached(surf->id));
         fprintf(stderr, "%s\n", line);
      }
   }
   if (type != GL_UNSIGNED_BYTE && upf != PIPE_FORMAT_R16G16B16A16_FLOAT) {
      sweet_gap("surface", "texImageIOSurface half-float type on a non-RGhA surface refused");   /* a half-float type is only meaningful for 'RGhA' surfaces (review gli-surface-fourcc-a008 F1) */
      gl_hint = 0;
      return -EINVAL;
   }
   if (type == GL_UNSIGNED_BYTE && upf == PIPE_FORMAT_R16G16B16A16_FLOAT) {
      sweet_gap("surface", "texImageIOSurface byte type on an RGhA surface refused");
      gl_hint = 0;
      return -EINVAL;
   }
   if (upf != PIPE_FORMAT_NONE && needs_upload(surf, upf)) {
      static unsigned uploads;
      if (uploads < 16) { uploads++; fprintf(stderr, "sweetgl: upload surface id %u pipe fmt %d %ux%u pitch %u (copy path)\n", surf->id, (int)upf, surf->w, surf->h, surf->bytes_per_row); }
      r = upload_surface(surf, upf);
      sweet_drm_mali_release_surface(surf->id);   /* the ObjC layer imported it (selector 5), the copy path never hands it to Mesa: boClose (review F1) */
   } else {
      r = bind_to_current_texture(surf);
   }
   gl_hint = 0;
   return r;
}

static int be_flush(void *user, void *bctx)
{
   (void)user;
   st_finish(bctx);   /* st_flush + fence_finish(INFINITE) = glFinish [src st_cb_flush.c:71-83] */
   return 0;   /* the capture's frame end is the Gallium flush hook (fcap_impl.c h_flush) */
}

static const struct sweet_gl_backend be = {
   .create_context = be_create_context,
   .destroy_context = be_destroy_context,
   .make_current = be_make_current,
   .attach_surface = be_attach_surface,
   .tex_image_surface = be_tex_image_surface,
   .flush = be_flush,
};

const struct sweet_gl_backend *sweet_gl_mesa_backend(void)
{
   return &be;
}

/* Size of level 0 of the 2D texture bound to texture unit `unit` in the current context (the texture-rectangle emulation of gli_table.c needs it for
 * textures that did not come through an IOSurface: CopyTexImage / TexImage / storage). 1 = found. */
int sweet_mesa_unit_tex2d_size(unsigned unit, unsigned *w, unsigned *h)
{
   GET_CURRENT_CONTEXT(ctx);
   if (!ctx || unit >= ARRAY_SIZE(ctx->Texture.Unit))
      return 0;
   /* the bound object, the DEFAULT one (name 0) included: CA attaches its rect mask IOSurface to texture 0 on unit 1 (alrect5 U1W witness: bind 0
    * right before every rect draw), so a name check here left the rect scale unset and the mask sampled at one texel (App Library magnifier) */
   struct gl_texture_object *t = ctx->Texture.Unit[unit].CurrentTex[TEXTURE_2D_INDEX];
   struct gl_texture_image *img0 = t ? t->Image[0][t->Attrib.BaseLevel] : NULL;
   if (!img0 || !img0->Width || !img0->Height) {
      /* diagnostic: a texture left on the RECTANGLE target while the rewritten shader samples 2D would read the default texture (GAP) */
      static int nr;
      struct gl_texture_object *r = ctx->Texture.Unit[unit].CurrentTex[TEXTURE_RECT_INDEX];
      if (r && r->Name && nr < 8) { nr++; fprintf(stderr, "sweetgl: GAP rect unit %u: no 2D texture, RECT target holds tex %u (shader samples 2D)\n", unit, r->Name); }
      if (nr < 40) {   /* where is the texture then: every non-default binding of units 0..7 */
         char b[512]; int o = 0;
         nr++;
         for (unsigned u = 0; u < 8 && u < ARRAY_SIZE(ctx->Texture.Unit); u++)
            for (unsigned ti = 0; ti < NUM_TEXTURE_TARGETS; ti++) {
               struct gl_texture_object *x = ctx->Texture.Unit[u].CurrentTex[ti];
               if (x && (x->Name || (x->Image[0][0] && x->Image[0][0]->Width)) && o < (int)sizeof b - 32)
                  o += snprintf(b + o, sizeof b - o, " u%u/t%u=%u", u, ti, x->Name);
            }
         {  /* what the fragment program actually samples: Mesa's sampler -> unit map (what Gallium will see at draw) */
            struct gl_program *fp = ctx->FragmentProgram._Current;
            char m[160]; int mo = 0;
            if (fp)
               for (unsigned si = 0; si < 8; si++)
                  if ((fp->SamplersUsed >> si) & 1)
                     mo += snprintf(m + mo, sizeof m - mo, " s%u->u%u", si, fp->SamplerUnits[si]);
            m[mo] = 0;
            fprintf(stderr, "sweetgl: GAP rect unit %u size unknown; bound:%s (active %u, api %d) fp %u samplers:%s\n", unit, o ? b : " none", ctx->Texture.CurrentUnit, (int)ctx->API,
                    fp ? fp->Id : 0, mo ? m : " none");
         }
      }
      return 0;
   }
   *w = img0->Width;
   *h = img0->Height;
   return 1;
}

/* LOWCLK lever real-flush (gli_table.c "real flush", mode 1): 1 = colour attachment 0 of the current context's draw framebuffer is a texture whose storage is
 * an imported display IOSurface ('b3a8', the only format SweetFramebuffer's panel copy is fed with [obs gputest25-38 `SWEET SURF create`, see FOURCC_B3A8]), so
 * the kernel present gate (patch 0343) covers its CPU reader; 0 = anything else (the caller keeps the wait). An EGLImage texture's storage is the imported
 * resource itself [src mesa 26.2.5 st_cb_eglimage.c:655 `pipe_resource_reference(&texObj->pt, stimg->texture)`]; attachments [src main/mtypes.h:2594-2612,
 * :2727, menums.h:140]. The cache is written under the eagl_core mutex and only compared here (no dereference): a race costs one misjudged flush. */
int sweet_mesa_draw_is_display(void);
int sweet_mesa_draw_is_display(void)
{
   GET_CURRENT_CONTEXT(ctx);
   if (!ctx || !ctx->DrawBuffer)
      return 0;
   const struct gl_renderbuffer_attachment *att = &ctx->DrawBuffer->Attachment[BUFFER_COLOR0];
   if (att->Type != 0x1702 /* GL_TEXTURE */ || !att->Texture || !att->Texture->pt)
      return 0;
   for (int i = 0; i < CACHE_N; i++)
      if (cache[i].res == att->Texture->pt)
         return cache[i].fmt == FOURCC_B3A8;
   return 0;
}

const char *sweet_gl_mesa_get_string(unsigned name)
{
   return (const char *)_mesa_GetString(name);
}

#include "fcap_impl.c"
