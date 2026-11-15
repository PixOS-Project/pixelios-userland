/*
 * gl_compute_test (ticket T-44c1, G-7, research 118): Mesa gallium panfrost over sweet_drm_mali. Builds a NIR compute kernel
 * out[gid] = gid*gid + 7 (u32) with nir_builder, binds a 4 KiB SSBO, launch_grid (block 64, grid N/64), flush, fence_finish,
 * maps the buffer and compares with the host-computed array. Prints "SWEETGL: ..." lines, exit 0 only if every element matches.
 * Same boilerplate and link line as gl_clear_test.c. Not built, not run (see docs/runplans/g7-compute.md for the status).
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
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "sweet_drm_mali.h"
#ifdef SWEET_DRM_HOST_TEST
#include "fake_iokit.h"
#endif

struct pipe_screen *panfrost_drm_screen_create(int fd, const struct pipe_screen_config *config);

static const driOptionDescription driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "panfrost/driinfo_panfrost.h"
};


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
#ifdef SWEET_DRM_HOST_TEST
   return;   /* keep stdout: the host test reads it */
#endif
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


#define N 1024u            /* elements: 4 KiB of u32 */
#define BLOCK 64u
#define SENTINEL 0xdeadbeefu
#define FAIL(...) do { say("SWEETGL: FAIL " __VA_ARGS__); say("\n"); return 1; } while (0)

static uint32_t
expect(uint32_t gid)
{
   return gid * gid + 7u;
}

/* out[gid.x] = gid.x * gid.x + 7. Integer ops only, so the host value is exact. */
static nir_shader *
build_kernel(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opts, "sweetgl_g7_compute");
   b.shader->info.workgroup_size[0] = BLOCK;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 1;
   /* [guess] the GL frontend sets this and pan_shader.c asserts it for every shader (panfrost_shader_compile); unverified for a kernel without UBOs */
   b.shader->info.first_ubo_is_default_ubo = true;

   nir_def *gid = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *val = nir_iadd_imm(&b, nir_imul(&b, gid, gid), 7);
   nir_def *off = nir_imul_imm(&b, gid, 4);
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off, .write_mask = 0x1, .access = 0, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

#ifdef SWEET_DRM_HOST_TEST
/* Host oracle (T0): the fake SweetMali kernel cannot run a shader, so only the PACKET is checked: exactly one SEL_SUBMIT (selector 3) with
 * req == 0 (no PANFROST_JD_REQ_FS) and a GPU VA jc, 8-byte aligned, inside a live BO, whose first Job Header has Type == Compute (4).
 * Job Header layout [src genxml/v9.xml:1425-1439]: word 4 bits 1..7 = Type; Job Type Compute = 4 [src v9.xml:37-47].
 * HOST_EXPECT_TYPE / HOST_EXPECT_SUBMITS can be overridden with -D for the mutation check (each override must make this fail). */
#ifndef HOST_EXPECT_TYPE
#define HOST_EXPECT_TYPE 4u
#endif
#ifndef HOST_EXPECT_SUBMITS
#define HOST_EXPECT_SUBMITS 1u
#endif
static int
host_packet_check(void)
{
   unsigned n = fake_count(3);
   say("SWEETHOST: submits=%u\n", n);
   if (n != HOST_EXPECT_SUBMITS)
      FAIL("host: %u SEL_SUBMIT calls, expected %u", n, HOST_EXPECT_SUBMITS);
   const struct fake_call *fc = fake_last(3);
   say("SWEETHOST: submit jc=0x%llx req=%llu\n", (unsigned long long)fc->in[0], (unsigned long long)fc->in[1]);
   if (fc->in[1] != 0)
      FAIL("host: submit req=%llu, expected 0 (no FS requirement)", (unsigned long long)fc->in[1]);
   if (fc->in[0] == 0 || (fc->in[0] & 7))
      FAIL("host: jc=0x%llx is zero or not 8-byte aligned", (unsigned long long)fc->in[0]);
   const uint32_t *jh = fake_va_ptr(fc->in[0], 64);
   if (!jh)
      FAIL("host: jc=0x%llx is not inside a live BO", (unsigned long long)fc->in[0]);
   unsigned type = (jh[4] >> 1) & 0x7f;
   say("SWEETHOST: first job header words: %08x %08x %08x %08x %08x %08x %08x %08x type=%u\n", jh[0], jh[1], jh[2], jh[3], jh[4], jh[5], jh[6], jh[7], type);
   if (type != HOST_EXPECT_TYPE)
      FAIL("host: first job type %u, expected %u (Compute)", type, HOST_EXPECT_TYPE);
   say("SWEETHOST: PASS one SEL_SUBMIT req=0 jc aligned first job type=%u (Compute)\n", type);
   return 0;
}
#endif

int main(void)
{
#ifdef SWEET_DRM_HOST_TEST
   fake_reset();
   fake.completed = UINT64_MAX;   /* the fake kernel finishes every job at once */
#endif
   sweetgl_out();
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
   say("SWEETGL: compute caps: compute=%d\n", scr->caps.compute);
   if (!scr->caps.compute)
      FAIL("screen reports caps.compute = 0");

   say("SWEETGL: step creating context\n");
   struct pipe_context *pipe = scr->context_create(scr, NULL, 0);
   if (!pipe)
      FAIL("context_create returned NULL");

   struct pipe_resource tmpl = {
      .target = PIPE_BUFFER, .format = PIPE_FORMAT_R8_UNORM,
      .width0 = N * 4, .height0 = 1, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SHADER_BUFFER, .usage = PIPE_USAGE_DEFAULT,
   };
   say("SWEETGL: step creating ssbo (%u bytes)\n", N * 4);
   struct pipe_resource *res = scr->resource_create(scr, &tmpl);
   if (!res)
      FAIL("resource_create returned NULL");

   /* sentinel fill, so an element the GPU never wrote cannot equal the expected value by accident */
   static uint32_t fill[N];
   for (unsigned i = 0; i < N; i++)
      fill[i] = SENTINEL;
   pipe->buffer_subdata(pipe, res, PIPE_MAP_WRITE, 0, N * 4, fill);

   say("SWEETGL: step compiling kernel\n");
   nir_shader *nir = build_kernel(scr->nir_options[MESA_SHADER_COMPUTE]);
   struct pipe_compute_state cs = { .ir_type = PIPE_SHADER_IR_NIR, .prog = nir };
   void *kernel = pipe->create_compute_state(pipe, &cs);
   if (!kernel)
      FAIL("create_compute_state returned NULL");
   pipe->bind_compute_state(pipe, kernel);

   struct pipe_shader_buffer sb = { .buffer = res, .buffer_offset = 0, .buffer_size = N * 4 };
   pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, 1, &sb, 0x1);

   say("SWEETGL: step launch_grid block=%u grid=%u\n", BLOCK, N / BLOCK);
   struct pipe_grid_info grid;
   memset(&grid, 0, sizeof(grid));
   grid.work_dim = 1;
   grid.block[0] = BLOCK; grid.block[1] = 1; grid.block[2] = 1;
   grid.grid[0] = N / BLOCK; grid.grid[1] = 1; grid.grid[2] = 1;
   pipe->launch_grid(pipe, &grid);

   say("SWEETGL: step flushing\n");
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (!fence)
      FAIL("flush returned no fence (nothing was submitted)");
   if (!scr->fence_finish(scr, pipe, fence, 5000000000ull))
      FAIL("fence_finish timed out");
   scr->fence_reference(scr, &fence, NULL);
   say("SWEETGL: compute flushed\n");
#ifdef SWEET_DRM_HOST_TEST
   return host_packet_check();
#endif

   struct pipe_box box = { .width = N * 4, .height = 1, .depth = 1 };
   struct pipe_transfer *xfer = NULL;
   const uint32_t *p = pipe->buffer_map(pipe, res, 0, PIPE_MAP_READ, &box, &xfer);
   if (!p)
      FAIL("buffer_map returned NULL");

   unsigned bad = 0, untouched = 0;
   for (unsigned i = 0; i < N; i++) {
      if (p[i] == SENTINEL)
         untouched++;
      if (p[i] != expect(i)) {
         if (!bad)
            say("SWEETGL: first bad element [%u] = %08x expected %08x\n", i, p[i], expect(i));
         bad++;
      }
   }
   pipe->buffer_unmap(pipe, xfer);

   if (bad)
      FAIL("%u of %u elements wrong (%u still the sentinel: never written)", bad, N, untouched);
   say("SWEETGL: PASS compute %u elements match\n", N);
   return 0;
}
