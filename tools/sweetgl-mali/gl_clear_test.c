/*
 * gl_clear_test: Mesa gallium panfrost over sweet_drm_mali. Clears a 64x64 RGBA8 target and reads it back.
 * Prints "SWEETGL: ..." lines, exit 0 only if every pixel is the clear colour. Not run on hardware yet.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "sweet_drm_mali.h"

struct pipe_screen *panfrost_drm_screen_create(int fd, const struct pipe_screen_config *config);

static const driOptionDescription driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "panfrost/driinfo_panfrost.h"
};

#define W 64
#define H 64
#define FAIL(...) do { say("SWEETGL: FAIL " __VA_ARGS__); say("\n"); return 1; } while (0)

/* gputest5: sweetgl-test printed nothing at all. sweetmali-test (works) opens /dev/null itself, so do the same before anything else, from a constructor too, to tell a dyld/exec failure from a main() problem. */
static void
say(const char *fmt, ...)
{
   char b[256];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(b, sizeof(b), fmt, ap);
   va_end(ap);
   if (n > 0)
      (void)!write(1, b, (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1);
}

static void
sweetgl_exit(void)
{
   say("SWEETGL: exit\n");
}

static void
sweetgl_out(void)
{
   int n = open("/dev/null", O_WRONLY);
   if (n >= 0) {
      dup2(n, 1);
      dup2(n, 2);
   }
   setvbuf(stdout, NULL, _IONBF, 0);
}

__attribute__((constructor)) static void
sweetgl_ctor(void)
{
   sweetgl_out();
   say("SWEETGL: ctor\n");
   atexit(sweetgl_exit);
}

int main(void)
{
   sweetgl_out();
   /* launchd job: stdout is /dev/null (copied into the kernel log by sweetnull=), fully buffered by default, so a hang or crash would lose everything: unbuffered + a line before each step */
   setvbuf(stdout, NULL, _IONBF, 0);
   say("SWEETGL: step start\n");
   int fd = sweet_drm_mali_open();
   say("SWEETGL: step open fd=%d\n", fd);
   if (fd < 0)
      FAIL("sweet_drm_mali_open errno=%d", errno);

   driOptionCache info, cache;
   memset(&cache, 0, sizeof(cache));
   driParseOptionInfo(&info, driconf, sizeof(driconf) / sizeof(driconf[0]));
   struct pipe_screen_config cfg = { .options = &cache, .options_info = &info };

   say("SWEETGL: step driconf parsed, creating screen\n");
   struct pipe_screen *scr = panfrost_drm_screen_create(fd, &cfg);
   if (!scr)
      FAIL("panfrost_drm_screen_create returned NULL");
   say("SWEETGL: screen name=%s vendor=%s\n", scr->get_name(scr), scr->get_device_vendor(scr));

   say("SWEETGL: step creating context\n");
   struct pipe_context *pipe = scr->context_create(scr, NULL, 0);
   if (!pipe)
      FAIL("context_create returned NULL");

   struct pipe_resource tmpl = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .width0 = W, .height0 = H, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   say("SWEETGL: step creating render target\n");
   struct pipe_resource *res = scr->resource_create(scr, &tmpl);
   if (!res)
      FAIL("resource_create returned NULL");

   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width = W;
   fb.height = H;
   fb.nr_cbufs = 1;
   fb.cbufs[0].format = PIPE_FORMAT_R8G8B8A8_UNORM;
   fb.cbufs[0].texture = res;
   pipe->set_framebuffer_state(pipe, &fb);

   say("SWEETGL: step clearing\n");
   union pipe_color_union clr = { .f = { 1.0f, 0.0f, 0.0f, 1.0f } }; /* bytes FF 00 00 FF */
   pipe->clear(pipe, PIPE_CLEAR_COLOR0, 0, 0, NULL, &clr, 0.0, 0);

   say("SWEETGL: step flushing\n");
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (fence) {
      if (!scr->fence_finish(scr, pipe, fence, 5000000000ull))
         FAIL("fence_finish timed out");
      scr->fence_reference(scr, &fence, NULL);
   }
   say("SWEETGL: clear flushed\n");

   struct pipe_box box = { .width = W, .height = H, .depth = 1 };
   struct pipe_transfer *xfer = NULL;
   const uint8_t *p = pipe->texture_map(pipe, res, 0, PIPE_MAP_READ, &box, &xfer);
   if (!p)
      FAIL("texture_map returned NULL");

   unsigned bad = 0;
   for (unsigned y = 0; y < H; y++)
      for (unsigned x = 0; x < W; x++) {
         const uint8_t *px = p + y * xfer->stride + x * 4;
         if (px[0] != 0xff || px[1] != 0 || px[2] != 0 || px[3] != 0xff) {
            if (!bad)
               say("SWEETGL: first bad pixel (%u,%u) = %02x %02x %02x %02x\n", x, y, px[0], px[1], px[2], px[3]);
            bad++;
         }
      }
   pipe->texture_unmap(pipe, xfer);

   if (bad)
      FAIL("%u of %u pixels wrong", bad, W * H);
   say("SWEETGL: PASS %ux%u cleared to ff0000ff\n", W, H);
   return 0;
}
