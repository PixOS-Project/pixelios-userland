/*
 * eagl_mesa: Mesa backend of the EAGL shim (DESIGN.md s.2-3). Tags: [src mesa/...] = Mesa 26.2.5 tree,
 * [guess] = unverified, nothing here has run on the phone. Compiled only in the mesa-panfrost-ios CI workflow.
 *
 * All callbacks run under the eagl_core mutex (eagl_core.h), so the singleton state below needs no lock of its own.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "frontend/api.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/winsys_handle.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_cb_flush.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"

#include "eagl_mesa.h"
#include "sweet_drm_mali.h"

struct pipe_screen *panfrost_drm_screen_create(int fd, const struct pipe_screen_config *config);

/* GL entry points of the static Mesa; no header declares them with shared-glapi off. [src mesa/src/mesa/main/teximage.c:3594,
 * texobj.c:1781, get.c:2051, getstring.c:120]. GLenum=unsigned, GLuint=unsigned, GLint=int, GLeglImageOES=void*. */
extern void _mesa_EGLImageTargetTexture2DOES(unsigned target, void *image);
extern void _mesa_BindTexture(unsigned target, unsigned name);
extern void _mesa_GetIntegerv(unsigned pname, int *params);
extern const unsigned char *_mesa_GetString(unsigned name);

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RGBA8 0x8058            /* sized internal format, what the DRI frontend passes [src dri_screen.c:545] */
#define GL_BGRA_EXT 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define FOURCC_BGRA 0x42475241u    /* 'BGRA' [guess: the ObjC layer must pass kCVPixelFormatType_32BGRA as this id] */
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
   out->internalformat = GL_RGBA8;
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
   S.up = 1;
   return 0;
}

/* ---- IOSurface -> pipe_resource cache (DESIGN.md s.3 step 4) ---- */

#define CACHE_N 32
static struct cent {
   struct pipe_resource *res;
   uint32_t id, w, h, pitch;
   uint64_t va;
   unsigned long stamp;
} cache[CACHE_N];
static unsigned long tick;

static void cache_drop(struct cent *e)
{
   if (e->res)
      pipe_resource_reference(&e->res, NULL);
   memset(e, 0, sizeof(*e));
}

/* New reference-counted resource for the surface (caller unreferences). NULL + errno on failure. */
static struct pipe_resource *import_resource(const struct sweet_gl_surface *s)
{
   struct pipe_screen *scr = S.fscreen.screen;
   FILE *f = tmpfile();   /* unlinked temp file = the fake dma-buf, size read back by lseek(SEEK_END) [src pan_kmod.c:172] */
   if (!f)
      return NULL;
   int fd = fileno(f), err = 0;
   struct pipe_resource *res = NULL;
   if (ftruncate(fd, (off_t)s->len) != 0) {
      err = errno;
   } else if ((err = -sweet_drm_mali_register_surface(fd, s->id, s->len)) == 0) {
      struct pipe_resource t;
      memset(&t, 0, sizeof(t));
      t.target = PIPE_TEXTURE_2D;
      t.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      t.width0 = s->w;
      t.height0 = s->h;
      t.depth0 = 1;
      t.array_size = 1;
      t.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
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
   fclose(f);   /* the caller keeps ownership of the fd during import, and the import does not keep it [src pan_kmod.c:151-192] */
   errno = err;
   return res;
}

static int format_ok(const struct sweet_gl_surface *s)
{
   return s->format == FOURCC_BGRA || s->format == GL_BGRA_EXT;   /* BGRA8 only for now (DESIGN.md s.6) */
}

/* Bind the surface as storage of the texture currently bound to GL_TEXTURE_2D in the current context. */
static int bind_to_current_texture(const struct sweet_gl_surface *s)
{
   if (!S.up || !format_ok(s)) {
      sweet_drm_mali_release_surface(s->id);   /* the ObjC layer imported it (selector 5) but Mesa will never take it: boClose */
      return -EINVAL;
   }
   struct cent *e = NULL, *old = &cache[0];
   for (int i = 0; i < CACHE_N; i++) {
      struct cent *c = &cache[i];
      if (c->res && c->id == s->id && c->w == s->w && c->h == s->h && c->pitch == s->bytes_per_row && c->va == s->va) {
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
         sweet_drm_mali_release_surface(s->id);   /* no-op if Mesa took the handle (its resource free closes it) */
         return -err;
      }
      e = old;
      cache_drop(e);
      *e = (struct cent){ res, s->id, s->w, s->h, s->bytes_per_row, s->va, 0 };
   }
   e->stamp = ++tick;
   struct img_cookie ck = { e->res };
   _mesa_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, &ck);   /* GL errors surface in glGetError of CA's context, not here */
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
   return st_api_make_current(bctx, NULL, NULL) ? 0 : -EIO;
}

static int be_attach_surface(void *user, void *bctx, uint32_t tex, const struct sweet_gl_surface *surf, int inverted)
{
   (void)user; (void)bctx;
   if (!surf)
      return 0;   /* the GL texture stays CA's; cache entries live until evicted (DESIGN.md s.3 step 7) */
   if (inverted)
      return -ENOTSUP;   /* no flip designed yet (DESIGN.md s.3 step 5) */
   int prev = 0;
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
   _mesa_BindTexture(GL_TEXTURE_2D, tex);
   int r = bind_to_current_texture(surf);
   _mesa_BindTexture(GL_TEXTURE_2D, (unsigned)prev);
   return r;
}

static int be_tex_image_surface(void *user, void *bctx, const struct sweet_gl_surface *surf, uint32_t target,
                                uint32_t internalformat, uint32_t w, uint32_t h, uint32_t format, uint32_t type,
                                uint32_t plane, int invert)
{
   (void)user; (void)bctx; (void)internalformat; (void)format;   /* the surface's own format decides [guess] */
   if (invert)
      return -ENOTSUP;
   if (target != GL_TEXTURE_2D || plane != 0 || type != GL_UNSIGNED_BYTE || w != surf->w || h != surf->h)
      return -EINVAL;
   return bind_to_current_texture(surf);
}

static int be_flush(void *user, void *bctx)
{
   (void)user;
   st_finish(bctx);   /* st_flush + fence_finish(INFINITE) = glFinish [src st_cb_flush.c:71-83] */
   return 0;
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

const char *sweet_gl_mesa_get_string(unsigned name)
{
   return (const char *)_mesa_GetString(name);
}
