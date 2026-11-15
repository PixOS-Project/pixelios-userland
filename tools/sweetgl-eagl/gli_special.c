/* gli_special.c: GLI slots that are not a plain `_mesa_X` call (named with `=` in gli_slots.txt). Each takes rctx + GL args. */
#include "gli_table.h"
#include <stdio.h>
#include <string.h>

extern void _mesa_Finish(void);
extern void _mesa_DeleteProgram(unsigned);
extern void _mesa_DeleteShader(unsigned);
extern unsigned char _mesa_IsProgram(unsigned);
extern const unsigned char *_mesa_GetString(unsigned);
extern void _mesa_GetIntegerv(unsigned, int *);

#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_2D 0x0DE1

/* Token-exact test: `name` is one whole space-separated word of `exts` (a name that is a prefix of a longer extension must not match). NULL-safe.
 * research 86 sections E/F/H, DRAFT-format0-batch. */
int sweet_ext_has(const char *exts, const char *name)
{
	size_t n;
	if (!exts || !name || !*name) return 0;
	n = strlen(name);
	while (*exts) {
		size_t len;
		while (*exts == ' ') exts++;
		for (len = 0; exts[len] && exts[len] != ' '; len++) {}
		if (len == n && !strncmp(exts, name, n)) return 1;
		exts += len;
	}
	return 0;
}

/* The 16 extensions CA's GL paths may depend on (research 86 sections E/F/H, DRAFT-format0-batch); one `ext-check` line each. */
static const char *const ext_probe[16] = {
	"GL_APPLE_fence", "GL_APPLE_texture_filter_cubic", "GL_APPLE_texture_ycbcr_basic_formats", "GL_APPLE_texture_ycbcr_extended_formats",
	"GL_APPLE_texture_xr", "GL_APPLE_tile_order_rot", "GL_EXT_shader_framebuffer_fetch", "GL_EXT_texture_filter_anisotropic",
	"GL_EXT_texture_lod_bias", "GL_OES_standard_derivatives", "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear",
	"GL_OES_texture_float", "GL_OES_texture_float_linear", "GL_IMG_texture_compression_pvrtc", "GL_KHR_texture_compression_astc_ldr" };

/* Pure diagnostic printer (research 86 sections E/F/H, DRAFT-format0-batch): version, renderer, the extension string in chunks of <= 400 chars on
 * word boundaries (a single longer word is cut at 400), then the 16 ext-check lines. NULL strings print "(null)". */
void sweet_gl_describe(FILE *f, const char *ver, const char *rend, const char *ext)
{
	unsigned k;
	fprintf(f, "SWEETGL: GL_VERSION %s\n", ver ? ver : "(null)");
	fprintf(f, "SWEETGL: GL_RENDERER %s\n", rend ? rend : "(null)");
	if (!ext) {
		fprintf(f, "SWEETGL: GL_EXTENSIONS (null)\n");
	} else {
		const char *p = ext;
		while (*p) {
			size_t len = 0, cut;
			while (*p == ' ') p++;
			if (!*p) break;
			while (p[len]) len++;
			if (len > 400) {   /* back up to the last space within the first 401 chars */
				for (cut = 400; cut > 0 && p[cut] != ' '; cut--) {}
				len = cut ? cut : 400;
			}
			while (len > 1 && p[len - 1] == ' ') len--;   /* no trailing blank */
			fprintf(f, "SWEETGL: GL_EXTENSIONS %.*s\n", (int)len, p);
			p += len;
		}
	}
	for (k = 0; k < 16; k++) fprintf(f, "SWEETGL: ext-check %s %d\n", ext_probe[k], sweet_ext_has(ext, ext_probe[k]));
}

static void *gs_ctx[16];   /* contexts that already printed the description (once per context, like dbg_ctx in gli_table.c) */
static unsigned gs_nctx;

/* glGetString(0x8a1e): QuartzCore asks for a second extension list with an Apple-private enum; Mesa would raise
 * GL_INVALID_ENUM, and CA treats NULL as "none" [src R33 s.2.3: `search_extension_list` is NULL-safe]. The first GL_EXTENSIONS (0x1f03) call of each
 * context also logs version / renderer / extensions (research 86 sections E/F/H, DRAFT-format0-batch); the return value is unchanged. */
const unsigned char *sgli_GetString(void *rctx, unsigned name)
{
	const unsigned char *r;
	SWEET_GLI_ENSURE(rctx, return 0);
	if (name == 0x8a1e) return 0;
	r = _mesa_GetString(name);
	if (name == 0x1f03) {
		unsigned q;
		unsigned seen = __atomic_load_n(&gs_nctx, __ATOMIC_ACQUIRE);
		if (seen > 16) seen = 16;
		for (q = 0; q < seen && gs_ctx[q] != rctx; q++) {}
		if (q == seen && seen < 16 && (q = __atomic_fetch_add(&gs_nctx, 1, __ATOMIC_ACQ_REL)) < 16) {   /* slot claimed atomically (review format0-a1a3 F3: CA has several render threads) */
			gs_ctx[q] = rctx;
			sweet_gl_describe(stderr, (const char *)_mesa_GetString(0x1f02), (const char *)_mesa_GetString(0x1f01), (const char *)r);
		}
	}
	return r;
}

/* glFinishObjectAPPLE(object, name): no APPLE object fences here; finish everything [guess, R33 s.4 item 10]. */
void sgli_FinishObject(void *rctx, unsigned object, int name) { SWEET_GLI_ENSURE(rctx, return); (void)object; (void)name; _mesa_Finish(); }
/* glTestObjectAPPLE(object, name): after a finish the object is complete: GL_TRUE [guess, R33 s.4 item 10]. */
unsigned char sgli_TestObject(void *rctx, unsigned object, unsigned name) { SWEET_GLI_ENSURE(rctx, return 0); (void)object; (void)name; _mesa_Finish(); return 1; }

/* glFlush as a real flush (LOWCLK lever real-flush): submit, no wait. _mesa_Flush -> _mesa_flush -> st_glFlush -> st_flush(st, NULL, flags) ->
 * panfrost_flush -> panfrost_flush_all_batches, no fence and no fence_finish [src mesa 26.2.5 main/context.c:1617-1657, state_tracker/st_cb_flush.c:50-106,
 * gallium/drivers/panfrost/pan_context.c:94-118]. Only reached through the switch of gli_table.c "real flush"; slot 90 of the generated table stays _mesa_Finish. */
extern void _mesa_Flush(void);
void sgli_RealFlush(void *rctx) { SWEET_GLI_ENSURE(rctx, return); _mesa_Flush(); }

/* glDeleteObjectARB(obj): obj is a shader or a program; GL 2.0 names are one namespace in Mesa (one GLI slot, 591, serves both glDeleteShader and glDeleteProgram: RM-00936..00942). */
void sgli_DeleteObjectARB(void *rctx, unsigned obj)
{
	SWEET_GLI_ENSURE(rctx, return);
	if (_mesa_IsProgram(obj)) _mesa_DeleteProgram(obj); else _mesa_DeleteShader(obj);
}

/* (sgli_nop below touches no GL state, so it does not ensure a current context.) */
/* debug markers and glFramebufferParameteri APPLE colour mode (R33 s.4 item 9: accept as no-op). */
long sgli_nop(void) { return 0; }

unsigned sweet_gli_bound_texture_2d(void)
{
	int t = 0;
	_mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &t);
	return (unsigned)t;
}

/* Level-0 size of the texture bound to GL_TEXTURE_2D in the current context (sprint GPU 2026-11-11 defect 1: did texImageIOSurface / attachImage
 * leave storage?). Returns the bound name; *w / *h = 0 when the texture has no level-0 image. Reads GL state only. */
extern void _mesa_GetTexLevelParameteriv(unsigned int, int, unsigned int, void *);
extern void _mesa_TexImage2D(unsigned int, int, int, int, int, int, unsigned int, unsigned int, void *);
extern unsigned _mesa_GetError(void);
unsigned sweet_gli_bound_level0(int *w, int *h)
{
	unsigned name = sweet_gli_bound_texture_2d();
	int a = 0, b = 0;
	if (name) {
		_mesa_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1000 /* TEXTURE_WIDTH */, &a);
		_mesa_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1001 /* TEXTURE_HEIGHT */, &b);
	}
	if (w) *w = a;
	if (h) *h = b;
	return name;
}

/* Define w x h RGBA8 level-0 storage (no data) on the bound GL_TEXTURE_2D texture, the storage CA's own create_surface gives its offscreen targets
 * (research 135 A.1: glTexImage2D(0xde1, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL)), so the FBO CA attaches it to is complete.
 * Pending GL errors are drained first (they would be blamed on this call); returns the GL error of the TexImage2D (0 = storage defined). */
unsigned sweet_gli_fallback_storage(unsigned w, unsigned h)
{
	unsigned i, e;
	if (!sweet_gli_bound_texture_2d() || !w || !h) return 0x502 /* GL_INVALID_OPERATION: nothing to give storage to */;
	for (i = 0; i < 16 && _mesa_GetError() != 0; i++) {}
	_mesa_TexImage2D(GL_TEXTURE_2D, 0, 0x1908 /* GL_RGBA */, (int)w, (int)h, 0, 0x1908, 0x1401 /* UNSIGNED_BYTE */, NULL);
	e = _mesa_GetError();
	return e;
}
