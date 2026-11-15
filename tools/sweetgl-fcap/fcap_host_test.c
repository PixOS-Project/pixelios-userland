/* fcap_host_test: the device recorder (../sweetgl-eagl/fcap_impl.c) on the host, with llvmpipe standing in for panfrost.
 * Draws a clear + a textured quad (TGSI shaders, sampled texture uploaded by texture_subdata, plus an "app IOSurface" with a CPU view that
 * differs from its GPU content in one row) into a fake 'b3a8' scanout, presents 3 frames with recording on, writes one capture, and checks it.
 * Run: fcap_host_test DIR  (DIR gets fcap/ capture parts); then fcap_replay --out DIR/out DIR/fcap/<file>.fcap.000 must MATCH.
 * Last line: RESULT: PASS fcap-host-test ... | RESULT: FAIL ... */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_text.h"
#include "llvmpipe/lp_public.h"
#include "sw/null/null_sw_winsys.h"
#include "compiler/glsl_types.h"

/* host stand-ins for what libsweetgl / Darwin provide */
#define FCAP_ROOT "/work/out/hosttest/root"   /* compile-time: fcap_impl.c joins its paths as string literals */
static char ROOTDIR[512] = FCAP_ROOT;
static unsigned gaps_seen;
void sweet_gap(const char *area, const char *fmt, ...);
void sweet_gap(const char *area, const char *fmt, ...)
{
   va_list ap; va_start(ap, fmt); fprintf(stderr, "SWEET GAP: %s ", area); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
   gaps_seen++;
}
struct sweet_gl_surface { uint32_t id; uint64_t va, len; uint32_t bytes_per_row, w, h, format; const void *cpu; };
#define FOURCC_B3A8 0x62336138u
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static uint64_t clock_gettime_nsec_np(int c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static int sysctlbyname(const char *n, void *o, size_t *l, void *ni, size_t nl) { (void)n; (void)o; (void)l; (void)ni; (void)nl; errno = ENOENT; return -1; }
#include "fcap_impl.c"

static void *tgsi(struct pipe_context *ctx, const char *text, int fs)
{
   struct tgsi_token toks[1024];
   if (!tgsi_text_translate(text, toks, 1024)) { printf("RESULT: FAIL fcap-host-test tgsi parse\n"); exit(1); }
   struct pipe_shader_state ss; memset(&ss, 0, sizeof ss);
   ss.type = PIPE_SHADER_IR_TGSI; ss.tokens = toks;
   return fs ? ctx->create_fs_state(ctx, &ss) : ctx->create_vs_state(ctx, &ss);
}

int main(int argc, char **argv)
{
   if (argc < 2) { printf("usage: fcap_host_test DIR\n"); return 2; }
   (void)argv;
   mkdir(ROOTDIR, 0755);
   setenv("SWEETGL_FCAP", "1", 1);
   glsl_type_singleton_init_or_ref();
   struct pipe_screen *scr = llvmpipe_create_screen(null_sw_create());
   fcap_install(scr);
   if (!FC.installed) { printf("RESULT: FAIL fcap-host-test not installed\n"); return 1; }
   struct pipe_context *ctx = scr->context_create(scr, NULL, 0);
   /* scanout 256x256 BGRA, flagged like a CA display surface */
   struct pipe_resource t; memset(&t, 0, sizeof t);
   t.target = PIPE_TEXTURE_2D; t.format = PIPE_FORMAT_B8G8R8A8_UNORM; t.width0 = 1024; t.height0 = 1024; t.depth0 = 1; t.array_size = 1;
   t.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *scan = scr->resource_create(scr, &t);
   struct sweet_gl_surface ss = { .id = 2, .len = 1024 * 1024 * 4, .bytes_per_row = 4096, .w = 1024, .h = 1024, .format = FOURCC_B3A8 };
   fcap_note_surface(scan, &ss, 1);
   /* texture 64x64 RGBA with a gradient (texture_subdata path) */
   t.format = PIPE_FORMAT_R8G8B8A8_UNORM; t.width0 = 64; t.height0 = 64; t.bind = PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *tex = scr->resource_create(scr, &t);
   static uint8_t px[64 * 64 * 4], cpu[64 * 64 * 4];
   for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t *p = &px[(y * 64 + x) * 4]; p[0] = x * 4; p[1] = y * 4; p[2] = 128; p[3] = 255; }
   /* recording on (file flag) before the frames */
   char fl[600]; snprintf(fl, sizeof fl, "%s/fcap-eager", ROOTDIR);
   FILE *f = fopen(fl, "w"); fputs("1\n", f); fclose(f);
   FC.eager_file = 1;
   struct pipe_box box; u_box_2d(0, 0, 64, 64, &box);
   ctx->texture_subdata(ctx, tex, 0, 0, &box, px, 256, 256 * 64);
   /* "app IOSurface": GPU content = px, CPU view = px with row 10 changed (stale-cache case): the COH line must appear and the replay uses the CPU view */
   memcpy(cpu, px, sizeof cpu);
   memset(cpu + 10 * 256, 0xff, 256);
   struct sweet_gl_surface as = { .id = 7, .len = sizeof cpu, .bytes_per_row = 256, .w = 64, .h = 64, .format = 0x42475241u, .cpu = cpu };
   fcap_note_surface(tex, &as, 0);
   void *vs = tgsi(ctx, "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n  0: MOV OUT[0], IN[0]\n  1: MOV OUT[1], IN[1]\n  2: END\n", 0);
   void *fs = tgsi(ctx, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[0], 2D\n  1: END\n", 1);
   struct pipe_vertex_element ve[2]; memset(ve, 0, sizeof ve);
   ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[0].src_stride = 32; ve[1].src_offset = 16; ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[1].src_stride = 32;
   void *vel = ctx->create_vertex_elements_state(ctx, 2, ve);
   struct pipe_blend_state bl; memset(&bl, 0, sizeof bl); bl.rt[0].colormask = 0xf;
   bl.rt[0].blend_enable = 1; bl.rt[0].rgb_func = PIPE_BLEND_ADD; bl.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA; bl.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   bl.rt[0].alpha_func = PIPE_BLEND_ADD; bl.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE; bl.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
   void *blend = ctx->create_blend_state(ctx, &bl);
   struct pipe_rasterizer_state rs; memset(&rs, 0, sizeof rs); rs.half_pixel_center = 1; rs.bottom_edge_rule = 1; rs.depth_clip_near = rs.depth_clip_far = 1;
   void *rast = ctx->create_rasterizer_state(ctx, &rs);
   struct pipe_depth_stencil_alpha_state dsa; memset(&dsa, 0, sizeof dsa);
   void *zsa = ctx->create_depth_stencil_alpha_state(ctx, &dsa);
   struct pipe_sampler_state sm; memset(&sm, 0, sizeof sm); sm.min_img_filter = sm.mag_img_filter = PIPE_TEX_FILTER_LINEAR; sm.max_lod = 0;
   void *samp = ctx->create_sampler_state(ctx, &sm);
   struct pipe_sampler_view svt; memset(&svt, 0, sizeof svt);
   svt.format = PIPE_FORMAT_R8G8B8A8_UNORM; svt.target = PIPE_TEXTURE_2D; svt.swizzle_r = 0; svt.swizzle_g = 1; svt.swizzle_b = 2; svt.swizzle_a = 3;
   struct pipe_sampler_view *view = ctx->create_sampler_view(ctx, tex, &svt);
   unsigned ok_frames = 0;
   for (int frame = 0; frame < 3; frame++) {
      struct pipe_framebuffer_state fb; memset(&fb, 0, sizeof fb);
      fb.width = fb.height = 1024; fb.nr_cbufs = 1; fb.cbufs[0].texture = scan; fb.cbufs[0].format = PIPE_FORMAT_B8G8R8A8_UNORM;
      ctx->set_framebuffer_state(ctx, &fb);
      union pipe_color_union col = { .f = { 0.1f * frame, 0.2f, 0.3f, 1.0f } };
      ctx->clear(ctx, PIPE_CLEAR_COLOR0, 0xf, 0, NULL, &col, 0, 0);
      struct pipe_viewport_state vp = { .scale = { 512, 512, 1 }, .translate = { 512, 512, 0 } };
      ctx->set_viewport_states(ctx, 0, 1, &vp);
      ctx->bind_blend_state(ctx, blend); ctx->bind_rasterizer_state(ctx, rast); ctx->bind_depth_stencil_alpha_state(ctx, zsa);
      ctx->bind_vs_state(ctx, vs); ctx->bind_fs_state(ctx, fs); ctx->bind_vertex_elements_state(ctx, vel);
      ctx->bind_sampler_states(ctx, MESA_SHADER_FRAGMENT, 0, 1, &samp);
      ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
      float x0 = -0.8f + 0.1f * frame;
      float v[4][8] = { { x0, -0.8f, 0, 1, 0, 0, 0, 1 }, { 0.8f, -0.8f, 0, 1, 1, 0, 0, 1 }, { x0, 0.8f, 0, 1, 0, 1, 0, 1 }, { 0.8f, 0.8f, 0, 1, 1, 1, 0, 1 } };
      struct pipe_vertex_buffer vb; memset(&vb, 0, sizeof vb);
      vb.buffer.resource = pipe_buffer_create_with_data(ctx, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_STREAM, sizeof v, v);
      ctx->set_vertex_buffers(ctx, 1, &vb);
      struct pipe_draw_info info; memset(&info, 0, sizeof info);
      info.mode = MESA_PRIM_TRIANGLE_STRIP; info.instance_count = 1;
      struct pipe_draw_start_count_bias d = { .start = 0, .count = 4 };
      ctx->draw_vbo(ctx, &info, 0, NULL, &d, 1);
      ctx->flush(ctx, NULL, 0);   /* the hooked flush ends the frame (h_flush) */
      if (FC.ring[0] && FC.ring[0]->seq == (uint32_t)frame + 1) ok_frames++;
   }
   fc_write_capture("hosttest");
   printf("frames recorded %u gaps %u files %u bytes %llu\n", ok_frames, gaps_seen, FC.files, (unsigned long long)FC.bytes_written);
   int ok = ok_frames == 3 && FC.files == 1 && FC.bytes_written > 0;
   printf("RESULT: %s fcap-host-test frames=%u files=%u\n", ok ? "PASS" : "FAIL", ok_frames, FC.files);
   return ok ? 0 : 1;
}
