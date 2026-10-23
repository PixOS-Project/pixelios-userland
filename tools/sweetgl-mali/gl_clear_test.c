/*
 * gl_clear_test: Mesa gallium panfrost over sweet_drm_mali. Clears a 64x64 RGBA8 target and reads it back.
 * Prints "SWEETGL: ..." lines, exit 0 only if every pixel is the clear colour. Not run on hardware yet.
 */
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
#define FAIL(...) do { printf("SWEETGL: FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

int main(void)
{
   int fd = sweet_drm_mali_open();
   if (fd < 0)
      FAIL("sweet_drm_mali_open errno=%d", errno);

   driOptionCache info, cache;
   memset(&cache, 0, sizeof(cache));
   driParseOptionInfo(&info, driconf, sizeof(driconf) / sizeof(driconf[0]));
   struct pipe_screen_config cfg = { .options = &cache, .options_info = &info };

   struct pipe_screen *scr = panfrost_drm_screen_create(fd, &cfg);
   if (!scr)
      FAIL("panfrost_drm_screen_create returned NULL");
   printf("SWEETGL: screen name=%s vendor=%s\n", scr->get_name(scr), scr->get_device_vendor(scr));

   struct pipe_context *pipe = scr->context_create(scr, NULL, 0);
   if (!pipe)
      FAIL("context_create returned NULL");

   struct pipe_resource tmpl = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .width0 = W, .height0 = H, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
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

   union pipe_color_union clr = { .f = { 1.0f, 0.0f, 0.0f, 1.0f } }; /* bytes FF 00 00 FF */
   pipe->clear(pipe, PIPE_CLEAR_COLOR0, 0, 0, NULL, &clr, 0.0, 0);

   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (fence) {
      if (!scr->fence_finish(scr, pipe, fence, 5000000000ull))
         FAIL("fence_finish timed out");
      scr->fence_reference(scr, &fence, NULL);
   }
   printf("SWEETGL: clear flushed\n");

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
               printf("SWEETGL: first bad pixel (%u,%u) = %02x %02x %02x %02x\n", x, y, px[0], px[1], px[2], px[3]);
            bad++;
         }
      }
   pipe->texture_unmap(pipe, xfer);

   if (bad)
      FAIL("%u of %u pixels wrong", bad, W * H);
   printf("SWEETGL: PASS %ux%u cleared to ff0000ff\n", W, H);
   return 0;
}
