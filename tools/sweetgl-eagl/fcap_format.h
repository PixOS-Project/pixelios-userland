/* fcap_format.h - frame capture format shared by the device recorder (fcap_impl.c, inside libsweetgl) and the host replayer
 * (tools/sweetgl-fcap/fcap_replay.c, Mesa 26.2.5 llvmpipe/softpipe). Sprint GPU 2026-11-09 (user goal: judge every screen by the frame
 * itself against a software-rasterizer replay of the captured command stream).
 *
 * The recorder sits at the Gallium level: it hooks the panfrost pipe_context in place (function pointers replaced, same ctx object), so
 * every draw is recorded with the full bound state the driver saw. Shaders are the st's NIR before panfrost touches it (nir_serialize at
 * create_vs/fs_state entry). Gallium state structs are written FIELD BY FIELD from the X-macro lists below (never raw struct bytes), so
 * the arm64 Apple and x86-64 Linux layouts of the bitfields never matter.
 *
 * File = "FCAP0001" then records { u32 type; u32 len; u8 payload[len]; } (little endian). Integers u32 unless stated; f = IEEE float32.
 * Object ids are recorder-assigned (u32, 0 = none). A file is self-contained: every object a frame references is defined before use.
 */
#ifndef FCAP_FORMAT_H
#define FCAP_FORMAT_H
#include <stdint.h>

#define FCAP_MAGIC "FCAP0001"
#define FCAP_VERSION 1

enum fcap_rec {
	FC_HDR = 1,       /* u32 version, u32 frames_in_file, char label[64], char mesa[32] */
	FC_BLOB = 3,      /* u64 hash (XXH64 of the raw bytes, seed 0), u32 raw_len, u32 codec (FCAP_CODEC_*), u8 data[] */
	FC_RES = 4,       /* id, target, format, width0, height0, depth0, array_size, last_level, nr_samples, bind, flags, imported, surf_id, surf_fourcc, surf_pitch, scanout */
	FC_SHADER = 5,    /* id, stage (MESA_SHADER_*), u32 nir_len, u8 nir[] (nir_serialize, not stripped) */
	FC_CSO = 6,       /* kind (FCAP_CSO_*), id, then the fields of that kind's X-macro list (u32 each; f fields as float32 bits) */
	FC_SVIEW = 7,     /* id, res, format, target, swz_r, swz_g, swz_b, swz_a, first_level, last_level, first_layer, last_layer, is_buffer, buf_offset, buf_size */
	FC_CONTENT = 8,   /* res, level, layer, width, height, stride (bytes/row of the blob), view (FCAP_VIEW_*), role (FCAP_ROLE_*), u64 hash (FC_BLOB), u32 frame */
	FC_FRAME = 9,     /* u32 seq, u64 t_ns, u32 ops, char label[64] */
	FC_FB = 10,       /* width, height, layers, samples, nr_cbufs, then 8 x {res, format, level, first_layer, last_layer}, then zs {same} */
	FC_STATE = 11,    /* see fcap_state below (fixed part) + variable lists */
	FC_DRAW = 12,     /* mode, index_size, primitive_restart, restart_index, start_instance, instance_count, drawid_offset, u64 index_hash, num_draws, num_draws x {start, count, i32 index_bias} */
	FC_CLEAR = 13,    /* buffers, color_mask, stencil_mask, has_scissor, minx, miny, maxx, maxy, color.ui[4], f64 depth (2 x u32), stencil */
	FC_CLEAR_RT = 14, /* surface {res, format, level, first_layer, last_layer}, color.ui[4], x, y, w, h, render_cond */
	FC_CLEAR_DS = 15, /* surface {..5}, clear_flags, f64 depth (2 x u32), stencil, x, y, w, h, render_cond */
	FC_BLIT = 16,     /* dst {res, level, x, y, z, w, h, d, format}, src {same}, mask, filter, scissor_enable, scissor{4}, alpha_blend, swizzle_enable, swizzle[4] (u32 each), render_cond, dst_sample, sample0_only */
	FC_COPY = 17,     /* dst, dst_level, dstx, dsty, dstz, src, src_level, box {x, y, z, w, h, d} */
	FC_TEXWRITE = 18, /* res, level, box {x, y, z, w, h, d}, stride, layer_stride, u64 hash */
	FC_BUFWRITE = 19, /* res, offset, size, u64 hash */
	FC_BARRIER = 20,  /* kind (1 texture_barrier, 2 memory_barrier), flags */
	FC_FLUSH = 21,    /* flags */
	FC_PRESENT = 22,  /* scanout res, frame seq */
	FC_GAP = 23,      /* char text[] (not NUL terminated) - something the recorder could not express; the replay reports it */
	FC_END = 24,      /* u32 records */
	FC_MIPGEN = 25,   /* res, format, base_level, last_level, first_layer, last_layer */
};

enum { FCAP_CODEC_RAW = 0, FCAP_CODEC_RLE32 = 1 };   /* RLE32: see fcap_rle32 below */
enum { FCAP_VIEW_GPU = 1, FCAP_VIEW_CPU = 2 };          /* GPU = pipe texture_map (what the GPU/display reads), CPU = IOSurface CPU-cached mapping (what the app wrote) */
enum { FCAP_ROLE_INITIAL = 1, FCAP_ROLE_RESULT = 2 };   /* initial = content at first touch in the frame; result = scanout after the frame's flush */
enum { FCAP_CSO_BLEND = 1, FCAP_CSO_RAST = 2, FCAP_CSO_DSA = 3, FCAP_CSO_SAMPLER = 4, FCAP_CSO_VELEMS = 5 };

/* RLE32 (both sides): a stream of u32 words over the raw bytes taken as little-endian u32 (raw_len % 4 == 0 required, else RAW).
 * Token = u32 h: if h & 0x80000000: run of (h & 0x7fffffff) copies of the next word; else literal: the next h words verbatim. */

/* ---- X-macro field lists. U(name) = unsigned/bitfield/enum read as u32; F(name) = float. ---- */
#define FCAP_BLEND_FIELDS(U, F) \
	U(independent_blend_enable) U(logicop_enable) U(logicop_func) U(dither) U(alpha_to_coverage) U(alpha_to_coverage_dither) \
	U(alpha_to_one) U(max_rt) U(advanced_blend_func) U(blend_coherent)
#define FCAP_RT_BLEND_FIELDS(U, F) \
	U(blend_enable) U(rgb_func) U(rgb_src_factor) U(rgb_dst_factor) U(alpha_func) U(alpha_src_factor) U(alpha_dst_factor) U(colormask)
#define FCAP_STENCIL_FIELDS(U, F) U(enabled) U(func) U(fail_op) U(zpass_op) U(zfail_op) U(valuemask) U(writemask)
#define FCAP_DSA_FIELDS(U, F) \
	U(alpha_enabled) U(alpha_func) U(depth_enabled) U(depth_writemask) U(depth_func) U(depth_bounds_test) F(alpha_ref_value)
#define FCAP_SAMPLER_FIELDS(U, F) \
	U(wrap_s) U(wrap_t) U(wrap_r) U(min_img_filter) U(min_mip_filter) U(mag_img_filter) U(compare_mode) U(compare_func) \
	U(unnormalized_coords) U(max_anisotropy) U(seamless_cube_map) U(border_color_is_integer) U(reduction_mode) \
	F(lod_bias) F(min_lod) F(max_lod) U(border_color.ui[0]) U(border_color.ui[1]) U(border_color.ui[2]) U(border_color.ui[3]) \
	U(border_color_format)
#define FCAP_RAST_FIELDS(U, F) \
	U(flatshade) U(light_twoside) U(clamp_vertex_color) U(clamp_fragment_color) U(front_ccw) U(cull_face) U(fill_front) U(fill_back) \
	U(offset_point) U(offset_line) U(offset_tri) U(scissor) U(poly_smooth) U(poly_stipple_enable) U(point_smooth) U(sprite_coord_mode) \
	U(point_quad_rasterization) U(point_line_tri_clip) U(point_size_per_vertex) U(multisample) U(no_ms_sample_mask_out) \
	U(force_persample_interp) U(line_smooth) U(line_stipple_enable) U(line_last_pixel) U(line_rectangular) U(conservative_raster_mode) \
	U(flatshade_first) U(half_pixel_center) U(bottom_edge_rule) U(subpixel_precision_x) U(subpixel_precision_y) U(rasterizer_discard) \
	U(rasterization_stream) U(tile_raster_order_fixed) U(tile_raster_order_increasing_x) U(tile_raster_order_increasing_y) \
	U(depth_clip_near) U(depth_clip_far) U(depth_clamp) U(clip_halfz) U(offset_units_unscaled) U(unclamped_fragment_depth_values) \
	U(clip_plane_enable) U(representative_fragment_test) U(line_stipple_factor) U(line_stipple_pattern) U(sprite_coord_enable) \
	F(line_width) F(point_size) F(offset_units) F(offset_scale) F(offset_clamp) F(conservative_raster_dilate)
#define FCAP_VELEM_FIELDS(U, F) U(src_offset) U(vertex_buffer_index) U(dual_slot) U(src_format) U(src_stride) U(instance_divisor)
#define FCAP_VIEWPORT_FIELDS(U, F) \
	F(scale[0]) F(scale[1]) F(scale[2]) F(translate[0]) F(translate[1]) F(translate[2]) U(swizzle_x) U(swizzle_y) U(swizzle_z) U(swizzle_w)

/* FC_CSO payload after {kind, id}: BLEND = BLEND fields + 8 x RT_BLEND fields; RAST = RAST fields; DSA = 2 x STENCIL fields + DSA fields;
 * SAMPLER = SAMPLER fields; VELEMS = u32 count + count x VELEM fields. */

/* FC_STATE payload:
 *   blend, rast, dsa, vs, fs, velems            (CSO / shader ids; 0 = unbound)
 *   f blend_color[4], stencil_ref[2], sample_mask, min_samples
 *   viewport: VIEWPORT fields; scissor: minx, miny, maxx, maxy
 *   for stage in {VS, FS}: n_views, n_views x sview id; n_samplers, n_samplers x sampler id; n_cb, n_cb x {index, size, u64 hash}
 *   n_vb, n_vb x {u64 hash, size, base}   (blob holds bytes [buffer_offset + base, +size) of the vertex buffer; replay puts them at offset base
 *                                          of a fresh buffer and binds it with buffer_offset 0)
 *   n_clip (0 or 8 x 4 floats follow when the rasterizer has clip_plane_enable)
 */
#endif
