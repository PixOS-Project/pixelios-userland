/* gli_table.c: table fill and trap logging. The slot tables are generated (gli_table_gen.c). */
#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c11 (host test); Apple libc has it by default and _POSIX_C_SOURCE would hide BSD names there */
#endif
#include "gli_table.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <dlfcn.h>
#endif
#include <errno.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#define GAP_PROG() getprogname()   /* stdlib.h */
#else
#define GAP_PROG() "?"          /* host test */
#endif

static void default_log(int slot, const char *name) { fprintf(stderr, "sweetgl: GLI slot %d (%s) is not implemented\n", slot, name); }
static void (*logger)(int, const char *) = default_log;
static unsigned char seen[SWEET_GLI_NSLOTS];
static unsigned long trap_cnt[SWEET_GLI_NSLOTS];   /* ponytail: racy bytes, a duplicate log line is the worst case */

int (*sweet_gli_ensure_current)(void *rctx);

void sweet_gli_set_log(void (*fn)(int, const char *)) { logger = fn; }


/* AP-102 / KNOWN-GAPS: every stub, refusal, unknown format, unimplemented slot or fake success prints ONE loud line
 *   SWEET GAP: <area> <what> xCOUNT (process)
 * on its first occurrence and again at counts 10 / 100 / 1000 / 10000 (counts in the log are lower bounds). The run summary (ui_script.py report) collects them.
 * <what> must not contain per-object numbers (ids, sizes) or the table fills with duplicates; 96 entries, then "(table full)". */
#define GLI_GAP_MAX 96
static struct { char key[120]; unsigned long n; } gap_tab[GLI_GAP_MAX + 1];   /* [GLI_GAP_MAX] = overflow */
static unsigned gap_n;
static pthread_mutex_t gap_mu = PTHREAD_MUTEX_INITIALIZER;

void sweet_gap(const char *area, const char *fmt, ...)
{
	char what[96], key[120];
	unsigned i;
	unsigned long n;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(what, sizeof what, fmt, ap);
	va_end(ap);
	snprintf(key, sizeof key, "%s %s", area, what);
	pthread_mutex_lock(&gap_mu);
	for (i = 0; i < gap_n && strcmp(gap_tab[i].key, key) != 0; i++) {}
	if (i == gap_n) {
		if (gap_n < GLI_GAP_MAX) { snprintf(gap_tab[i].key, sizeof gap_tab[i].key, "%s", key); gap_tab[i].n = 0; gap_n++; }
		else { i = GLI_GAP_MAX; snprintf(gap_tab[i].key, sizeof gap_tab[i].key, "%s", "(table full)"); }   /* review: the last real entry is no longer overwritten */
	}
	n = ++gap_tab[i].n;
	pthread_mutex_unlock(&gap_mu);
	if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000)
		fprintf(stderr, "SWEET GAP: %s x%lu (%s)\n", key, n, GAP_PROG());
}

long sweet_gli_trap(int slot)
{
	if (slot >= 0 && slot < SWEET_GLI_NSLOTS) {
		if (!seen[slot]) {
			seen[slot] = 1;
			if (logger) logger(slot, sweet_gli_names[slot]);
		}
		{   /* review gli-gap-rect-default #2/#3: per-slot counter on the hot path, a line only at 1/10/100/1000/10000, no shared table */
			unsigned long c = ++trap_cnt[slot];
			if (c == 1 || c == 10 || c == 100 || c == 1000 || c == 10000)
				fprintf(stderr, "SWEET GAP: gli slot %d %s unimplemented (call returns 0) x%lu (%s)\n", slot, sweet_gli_names[slot], c, GAP_PROG());
		}
	}
	return 0;
}

/* Diagnostic hooks (gputest30/31: backboardd SIGSEGV in memmove under glTexImage2D, Mesa assumed a 4424-byte unpack stride for 4420-byte
 * rows). They log the first calls of glPixelStorei(UNPACK_*) / glTexImage2D / glTexSubImage2D with Mesa's current unpack state, and
 * clamp GL_UNPACK_ALIGNMENT 8 to 4 (CoreAnimation sets alignment = bytes per pixel and never resets it; rows of every 8-byte pixel
 * format are multiples of 8 anyway, so 4 is the same stride for them and the right one for 4-byte pixels). */
extern void _mesa_GetIntegerv(unsigned, int *);
#define GLI_SLOT_PIXELSTOREI 195
#define GLI_SLOT_TEXIMAGE2D 301
#define GLI_SLOT_TEXSUBIMAGE2D 307
#define GL_UNPACK_ROW_LENGTH 0x0cf2
#define GL_UNPACK_ALIGNMENT 0x0cf5
#define GLI_DBG_MAX 120
static const void *orig_ps, *orig_ti, *orig_tsi;
static unsigned dbg_n;
static int trace;
static int trace_lvl;   /* SWEETGL_TRACE=1: uploads + draws into 180x180 targets; =2 also the PixelStorei lines */

static void unpack_state(int *al, int *rl)
{
	*al = *rl = -1;
	_mesa_GetIntegerv(GL_UNPACK_ALIGNMENT, al);
	_mesa_GetIntegerv(GL_UNPACK_ROW_LENGTH, rl);
}

static void hook_pixelstorei(void *rctx, unsigned pname, int param)
{
	if (pname == GL_UNPACK_ALIGNMENT || pname == GL_UNPACK_ROW_LENGTH) {
		if (trace_lvl >= 2 && dbg_n < GLI_DBG_MAX) { dbg_n++; fprintf(stderr, "sweetgl: PixelStorei pname 0x%x param %d\n", pname, param); }
		if (pname == GL_UNPACK_ALIGNMENT && param > 4) param = 4;
	}
	((void (*)(void *, unsigned, int))orig_ps)(rctx, pname, param);
}


/* G-11 (research 61, g89): the 19 CA 180x180 icon render targets stay all-zero in the kernel scan, so what CA draws INTO them is empty. SWEETGL_TRACE=1 now also
 * logs, with its own cap (GLI_UP_MAX lines, not the 120 shared with the unpack hooks), every texture upload of at least 24x24 pixels (glTexImage2D /
 * glTexSubImage2D with data, glCompressedTexImage2D / glCompressedTexSubImage2D) with a sampled nonzero count of the pixel data, to see whether the icon source
 * images arrive and are empty. Reads only the client's pixels, row by row within the stride GL itself will read. */
#define GLI_UP_MAX 400
#define GLI_SLOT_COMPRESSEDTEXIMAGE2D 380
#define GLI_SLOT_COMPRESSEDTEXSUBIMAGE2D 383
static unsigned up_n;
static struct { unsigned tex, w, h; unsigned long nz, n; } upt[256];   /* texture name -> last sampled upload (draw trace) */
static const void *orig_cti, *orig_ctsi;

static unsigned up_bpp(unsigned fmt, unsigned type)
{
	if (type != 0x1401 /* UNSIGNED_BYTE */) return 0;
	switch (fmt) {
	case 0x1908: case 0x80E1: return 4;   /* RGBA, BGRA */
	case 0x190A: return 2;                /* LUMINANCE_ALPHA */
	case 0x1909: case 0x1906: case 0x1903: return 1;   /* LUMINANCE, ALPHA, RED */
	default: return 0;
	}
}

static void up_log(const char *what, int w, int h, unsigned fmt, unsigned type, const void *px)
{
	unsigned bpp = up_bpp(fmt, type);
	unsigned long nz = 0, n = 0;
	if (!trace || up_n >= GLI_UP_MAX || w < 24 || h < 24) return;
	up_n++;
	{  /* a bound GL_PIXEL_UNPACK_BUFFER makes px an OFFSET, not a pointer (review gli-upload-trace): never dereference it */
		int pbo = 0;
		_mesa_GetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
		if (pbo != 0) px = NULL;
	}
	if (px && bpp) {
		int al = 4, rl = 0, r, rstep = h > 32 ? h / 32 : 1;
		size_t stride, rowb = (size_t)w * bpp, k;
		_mesa_GetIntegerv(GL_UNPACK_ALIGNMENT, &al);
		_mesa_GetIntegerv(GL_UNPACK_ROW_LENGTH, &rl);
		stride = rl > 0 ? (size_t)rl * bpp : rowb;
		if (al > 1) stride = (stride + (size_t)al - 1) / (size_t)al * (size_t)al;
		for (r = 0; r < h; r += rstep)
			for (k = 0; k < rowb; k += 5) { n++; if (((const unsigned char *)px)[(size_t)r * stride + k]) nz++; }
	}
	{
		int bound = 0;
		_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
		if (bound > 0) { upt[(unsigned)bound % 256].tex = (unsigned)bound; upt[(unsigned)bound % 256].w = (unsigned)w; upt[(unsigned)bound % 256].h = (unsigned)h; upt[(unsigned)bound % 256].nz = nz; upt[(unsigned)bound % 256].n = n; }
		fprintf(stderr, "sweetgl: upload %s tex %d %dx%d fmt 0x%x type 0x%x px %s nonzero %lu of %lu sampled\n", what, bound, w, h, fmt, type, px ? "data" : "NULL", nz, n);
	}
}

static void hook_compressedteximage2d(void *rctx, unsigned target, int level, unsigned ifmt, int w, int h, int border, int size, const void *data)
{
	if (trace && up_n < GLI_UP_MAX && w >= 24 && h >= 24) { up_n++; fprintf(stderr, "sweetgl: upload CompressedTexImage2D %dx%d ifmt 0x%x bytes %d\n", w, h, ifmt, size); }
	((void (*)(void *, unsigned, int, unsigned, int, int, int, int, const void *))orig_cti)(rctx, target, level, ifmt, w, h, border, size, data);
}
static void hook_compressedtexsubimage2d(void *rctx, unsigned target, int level, int x, int y, int w, int h, unsigned fmt, int size, const void *data)
{
	if (trace && up_n < GLI_UP_MAX && w >= 24 && h >= 24) { up_n++; fprintf(stderr, "sweetgl: upload CompressedTexSubImage2D %dx%d fmt 0x%x bytes %d\n", w, h, fmt, size); }
	((void (*)(void *, unsigned, int, int, int, int, int, unsigned, int, const void *))orig_ctsi)(rctx, target, level, x, y, w, h, fmt, size, data);
}

static void hook_teximage2d(void *rctx, unsigned target, int level, int ifmt, int w, int h, int border, unsigned fmt, unsigned type, const void *px)
{
	if (trace && level == 0 && (w <= 0 || h <= 0)) sweet_gap("tex", "TexImage2D level 0 with a zero or negative size");   /* g100: an empty mask backing would leave unit 1 unusable */
	if (trace && dbg_n < GLI_DBG_MAX && sweet_gli_ensure_current && sweet_gli_ensure_current(rctx) == 0) {
		int al, rl;
		unpack_state(&al, &rl);
		dbg_n++;
		fprintf(stderr, "sweetgl: TexImage2D tgt 0x%x lvl %d ifmt 0x%x %dx%d fmt 0x%x type 0x%x px %p unpack align %d rowlen %d\n",
		    target, level, (unsigned)ifmt, w, h, fmt, type, px, al, rl);
	}
	if (trace && up_n < GLI_UP_MAX && sweet_gli_ensure_current && sweet_gli_ensure_current(rctx) == 0) up_log("TexImage2D", w, h, fmt, type, px);
	((void (*)(void *, unsigned, int, int, int, int, int, unsigned, unsigned, const void *))orig_ti)(rctx, target, level, ifmt, w, h, border, fmt, type, px);
}

static void hook_texsubimage2d(void *rctx, unsigned target, int level, int x, int y, int w, int h, unsigned fmt, unsigned type, const void *px)
{
	if (trace && dbg_n < GLI_DBG_MAX && sweet_gli_ensure_current && sweet_gli_ensure_current(rctx) == 0) {
		int al, rl;
		unpack_state(&al, &rl);
		dbg_n++;
		fprintf(stderr, "sweetgl: TexSubImage2D tgt 0x%x lvl %d %d,%d %dx%d fmt 0x%x type 0x%x px %p unpack align %d rowlen %d\n",
		    target, level, x, y, w, h, fmt, type, px, al, rl);
	}
	if (trace && up_n < GLI_UP_MAX && sweet_gli_ensure_current && sweet_gli_ensure_current(rctx) == 0) up_log("TexSubImage2D", w, h, fmt, type, px);
	((void (*)(void *, unsigned, int, int, int, int, int, unsigned, unsigned, const void *))orig_tsi)(rctx, target, level, x, y, w, h, fmt, type, px);
}

/* Shader diagnostics (G-11, OPUS DECISION 2026-10-26 #8): env SWEETGL_SHADERS=1 (no comma in the value: the kernel's sweetenv splits at commas) installs hooks
 * on glShaderSource / glCompileShader / glLinkProgram / glGetProgramInfoLog / glProgramBinary. DIAGNOSTIC ONLY: each hook calls the saved original exactly once
 * with the arguments it received and returns its result; the only extra GL calls are read-only status / info-log queries AFTER the original and only when the
 * context could be made current. Own counters and caps (the 120-line cap of the unpack hooks is untouched). Shader text and info logs are Apple-derived: they
 * only go to stderr (the run's gitignored usbnet log), never into git or docs. Program binaries are NEVER logged (format, length and status only). */
extern void _mesa_GetShaderiv(unsigned, unsigned, int *);
extern void _mesa_GetProgramiv(unsigned, unsigned, int *);
extern void _mesa_GetShaderInfoLog(unsigned, int, int *, char *);
#define GLI_SLOT_SHADERSOURCE 595
#define GLI_SLOT_COMPILESHADER 596
#define GLI_SLOT_LINKPROGRAM 599
#define GLI_SLOT_GETPROGRAMINFOLOG 660
#define GLI_SLOT_PROGRAMBINARY 888
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GLI_SH_SRC_MAX 64        /* glShaderSource events */
#define GLI_SH_RES_MAX 256       /* compile / link / info-log results */
#define GLI_SH_BIN_MAX 64        /* glProgramBinary events */
#define GLI_SH_IL_MAX 64         /* glGetProgramInfoLog events (own counter, review F3) */
#define GLI_SH_SRC_BYTES 1024    /* source bytes logged per shader */
#define GLI_SH_LOG_BYTES 4096    /* info-log bytes logged per result */
static const void *orig_ss, *orig_cs, *orig_lp, *orig_gpil, *orig_pb;
static unsigned sh_src_n, sh_res_n, sh_bin_n, sh_il_n;
static int shaders;   /* env SWEETGL_SHADERS=1, read in sweet_gli_init */

/* Escape up to `max` bytes of `p` (n bytes available) into one line: \n \r \t, backslash, quotes as escapes, other non-printables as \xNN. */
static void log_escaped(const char *tag, unsigned id, const char *p, size_t n, size_t max)
{
	char line[6 * 1024 + 8];
	size_t o = 0, i;
	if (n > max) n = max;
	for (i = 0; i < n && o + 5 < sizeof line; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c == '\n') { line[o++] = '\\'; line[o++] = 'n'; }
		else if (c == '\r') { line[o++] = '\\'; line[o++] = 'r'; }
		else if (c == '\t') { line[o++] = '\\'; line[o++] = 't'; }
		else if (c == '\\') { line[o++] = '\\'; line[o++] = '\\'; }
		else if (c >= 32 && c < 127) line[o++] = (char)c;
		else o += (size_t)snprintf(line + o, sizeof line - o, "\\x%02x", c);
	}
	line[o] = 0;
	fprintf(stderr, "sweetgl: %s %u: %s\n", tag, id, line);
}

static int sh_ctx_ok(void *rctx)
{
	return sweet_gli_ensure_current && sweet_gli_ensure_current(rctx) == 0;
}

static void hook_shadersource(void *rctx, unsigned shader, int count, const char *const *str, const int *length)
{
	if (sh_src_n < GLI_SH_SRC_MAX && count > 0 && count <= 256 && str) {   /* review F1: Mesa validates the shader id before reading `string`; bound count so junk cannot crash the hook first */
		size_t total = 0, k, got = 0;
		char first[GLI_SH_SRC_BYTES];
		for (k = 0; k < (size_t)count; k++) {
			size_t n = 0;
			if (str[k]) n = (length && length[k] >= 0) ? (size_t)length[k] : strlen(str[k]);
			if (str[k] && got < sizeof first) {
				size_t take = n < sizeof first - got ? n : sizeof first - got;
				memcpy(first + got, str[k], take);
				got += take;
			}
			total += n;
		}
		sh_src_n++;
		fprintf(stderr, "sweetgl: ShaderSource shader %u count %d total %zu bytes\n", shader, count, total);
		log_escaped("ShaderSource text", shader, first, got, GLI_SH_SRC_BYTES);
	}
	((void (*)(void *, unsigned, int, const char *const *, const int *))orig_ss)(rctx, shader, count, str, length);
}

static void hook_compileshader(void *rctx, unsigned shader)
{
	((void (*)(void *, unsigned))orig_cs)(rctx, shader);
	if (sh_res_n < GLI_SH_RES_MAX && sh_ctx_ok(rctx)) {
		int st = 0, len = 0;
		char buf[GLI_SH_LOG_BYTES];
		_mesa_GetShaderiv(shader, GL_COMPILE_STATUS, &st);
		sh_res_n++;
		fprintf(stderr, "sweetgl: CompileShader shader %u status %d\n", shader, st);
		if (!st) {
			buf[0] = 0;
			_mesa_GetShaderInfoLog(shader, (int)sizeof buf, &len, buf);
			buf[sizeof buf - 1] = 0;
			log_escaped("CompileShader info log", shader, buf, strlen(buf), GLI_SH_LOG_BYTES);
		}
	}
}

static void hook_linkprogram(void *rctx, unsigned program)
{
	((void (*)(void *, unsigned))orig_lp)(rctx, program);
	if (sh_res_n < GLI_SH_RES_MAX && sh_ctx_ok(rctx)) {
		int st = 0, len = 0;
		char buf[GLI_SH_LOG_BYTES];
		_mesa_GetProgramiv(program, GL_LINK_STATUS, &st);
		sh_res_n++;
		fprintf(stderr, "sweetgl: LinkProgram program %u status %d\n", program, st);
		if (!st) {
			buf[0] = 0;
			((void (*)(void *, unsigned, int, int *, char *))orig_gpil)(rctx, program, (int)sizeof buf, &len, buf);   /* the original, read-only */
			buf[sizeof buf - 1] = 0;
			log_escaped("LinkProgram info log", program, buf, strlen(buf), GLI_SH_LOG_BYTES);
		}
	}
}

static void hook_getprograminfolog(void *rctx, unsigned program, int bufsize, int *length, char *infolog)
{
	((void (*)(void *, unsigned, int, int *, char *))orig_gpil)(rctx, program, bufsize, length, infolog);
	/* only what the original reports it wrote (*length), never uninitialised app memory (review F3) */
	if (sh_il_n < GLI_SH_IL_MAX && infolog && length && bufsize > 1 && *length > 0) {
		size_t n = (size_t)*length < (size_t)bufsize - 1 ? (size_t)*length : (size_t)bufsize - 1;
		sh_il_n++;
		log_escaped("GetProgramInfoLog", program, infolog, n, GLI_SH_LOG_BYTES);
	}
}

static void hook_programbinary(void *rctx, unsigned program, unsigned format, const void *binary, int length)
{
	((void (*)(void *, unsigned, unsigned, const void *, int))orig_pb)(rctx, program, format, binary, length);
	if (sh_bin_n < GLI_SH_BIN_MAX && sh_ctx_ok(rctx)) {
		int st = 0;
		_mesa_GetProgramiv(program, GL_LINK_STATUS, &st);
		sh_bin_n++;
		fprintf(stderr, "sweetgl: ProgramBinary program %u format 0x%x length %d link status %d (blob not logged)\n", program, format, length, st);
	}
}

/* G-11 (research 61): Mesa's GLES2 front end rejects sampler2DRect / texture2DRect[Proj], which CoreAnimation's icon-mask shader uses. SWEETGL_RECT=1
 * rewrites them to sampler2D / texture2D[Proj] (type and function names only: the coordinates stay in pixels, so the mask is wrong until they are
 * normalised; the first experiment is only to see every program link). Returns the new length; `out` has room for n bytes (the rewrite only shrinks). */
/* Appends len bytes; 0 on overflow. */
static int rw_app(char *buf, size_t cap, size_t *o, const char *p, size_t len)
{
	if (*o + len > cap) return 0;
	memcpy(buf + *o, p, len);
	*o += len;
	return 1;
}
static int rw_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int rw_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* G-11 (research 61, g72): CoreAnimation's rectangle sampler takes coordinates in TEXELS; after sampler2DRect -> sampler2D they must be divided by the
 * texture size or every fragment reads the clamped edge texel (the icon mask's transparent corner: icons vanished). The rewrite is
 *   sampler2DRect NAME;           ->  sampler2D NAME;  uniform highp vec2 sweet_rect_inv_NAME;
 *   texture2DRect(NAME, EXPR)     ->  texture2D(NAME, (EXPR) * sweet_rect_inv_NAME)
 *   texture2DRectProj             ->  texture2DProj          (no normalisation, not seen in CA's shaders)
 * and the draw hook below sets sweet_rect_inv_NAME from the size of the texture bound for NAME. Identifiers are matched on word boundaries.
 * Returns the new length with *out malloc'ed (caller frees); 0 and *out NULL on allocation/overflow failure (caller then uses the original). */
size_t sweet_rect_rewrite(const char *src, size_t n, char **out)
{
	size_t i = 0, o = 0, cap, occ = 0, k;
	char *buf;
	*out = NULL;
	for (k = 0; k + 4 <= n; k++) if (memcmp(src + k, "Rect", 4) == 0) occ++;
	cap = n + 200 * (occ + 1) + 256;
	buf = malloc(cap + 1);
	if (!buf) return 0;
	while (i < n) {
		int hit = 0;
		if ((i == 0 || !rw_idc((unsigned char)src[i - 1])) && src[i] != 0) {
			if (n - i >= 13 && memcmp(src + i, "sampler2DRect", 13) == 0 && (i + 13 == n || !rw_idc((unsigned char)src[i + 13]))) {
				size_t j = i + 13, names[8][2], nn = 0, m;
				if (!rw_app(buf, cap, &o, "sampler2D", 9)) goto fail;
				/* declarator list: NAME[, NAME]... up to ';' (one inverse-size uniform per name, up to 8) */
				for (;;) {
					size_t ws = j, ns, ne;
					while (j < n && rw_ws((unsigned char)src[j])) j++;
					ns = j;
					while (j < n && rw_idc((unsigned char)src[j])) j++;
					ne = j;
					if (ne == ns) { j = ws; break; }   /* not a declaration (e.g. a parameter): rename only */
					if (nn < 8) { names[nn][0] = ns; names[nn][1] = ne; nn++; }
					if (!rw_app(buf, cap, &o, src + ws, ne - ws)) goto fail;
					while (j < n && rw_ws((unsigned char)src[j])) { if (!rw_app(buf, cap, &o, src + j, 1)) goto fail; j++; }
					if (j < n && src[j] == ',') { if (!rw_app(buf, cap, &o, ",", 1)) goto fail; j++; continue; }
					break;
				}
				if (nn && j < n && src[j] == ';') {
					if (!rw_app(buf, cap, &o, ";", 1)) goto fail;
					j++;
					for (m = 0; m < nn; m++)
						if (!rw_app(buf, cap, &o, "\nuniform highp vec2 sweet_rect_inv_", 35) || !rw_app(buf, cap, &o, src + names[m][0], names[m][1] - names[m][0]) || !rw_app(buf, cap, &o, ";\n", 2)) goto fail;
				}
				i = j; hit = 1;
			} else if (n - i >= 13 && memcmp(src + i, "texture2DRect", 13) == 0) {
				size_t j = i + 13;
				if (j + 4 <= n && memcmp(src + j, "Proj", 4) == 0 && (j + 4 == n || !rw_idc((unsigned char)src[j + 4]))) {
					if (!rw_app(buf, cap, &o, "texture2DProj", 13)) goto fail;
					i = j + 4; hit = 1;
				} else if (j == n || !rw_idc((unsigned char)src[j])) {
					size_t p = j, as, ae, bs, depth = 1, q;
					while (p < n && rw_ws((unsigned char)src[p])) p++;
					int parsed = 0;
					if (p < n && src[p] == '(') {
						p++;
						while (p < n && rw_ws((unsigned char)src[p])) p++;
						as = p;
						while (p < n && rw_idc((unsigned char)src[p])) p++;
						ae = p;
						while (p < n && rw_ws((unsigned char)src[p])) p++;
						if (ae > as && p < n && src[p] == ',') {
							p++; while (p < n && rw_ws((unsigned char)src[p])) p++;
							bs = p; q = p;
							while (q < n && depth) { if (src[q] == '(') depth++; else if (src[q] == ')') depth--; if (depth) q++; }
							if (depth == 0) {
								if (!rw_app(buf, cap, &o, "texture2D(", 10) || !rw_app(buf, cap, &o, src + as, ae - as) || !rw_app(buf, cap, &o, ", (", 3) ||
								    !rw_app(buf, cap, &o, src + bs, q - bs) || !rw_app(buf, cap, &o, ") * sweet_rect_inv_", 19) ||
								    !rw_app(buf, cap, &o, src + as, ae - as) || !rw_app(buf, cap, &o, ")", 1)) goto fail;
								i = q + 1; parsed = 1; hit = 1;
							}
						}
					}
					if (!parsed) { if (!rw_app(buf, cap, &o, "texture2D", 9)) goto fail; i = j; hit = 1; }   /* not the NAME, EXPR shape: only the rename */
				}
			}
		}
		if (!hit) { if (!rw_app(buf, cap, &o, src + i, 1)) goto fail; i++; }
	}
	buf[o] = 0;
	*out = buf;
	return o;
fail:
	free(buf);
	return 0;
}

/* Texture sizes (GL texture name -> w,h) for the surfaces libsweetgl uploads or imports, direct mapped (a collision overwrites; the name is compared). */
#define GLI_TSZ 4096
static struct { unsigned tex, w, h; } tsz[GLI_TSZ];
void sweet_gli_note_tex_size(unsigned tex, unsigned w, unsigned h)
{
	if (!tex) return;
	tsz[tex % GLI_TSZ].tex = tex; tsz[tex % GLI_TSZ].w = w; tsz[tex % GLI_TSZ].h = h;
}
static int tex_size(unsigned tex, unsigned *w, unsigned *h)
{
	if (!tex || tsz[tex % GLI_TSZ].tex != tex || !tsz[tex % GLI_TSZ].w || !tsz[tex % GLI_TSZ].h) return 0;
	*w = tsz[tex % GLI_TSZ].w; *h = tsz[tex % GLI_TSZ].h;
	return 1;
}

static const void *rect_next;   /* the ShaderSource entry that was installed before the rect hook (original or the diagnostic hook) */
static int rect;                /* env SWEETGL_RECT: on by default, =0 disables */

static void hook_shadersource_rect(void *rctx, unsigned shader, int count, const char *const *str, const int *length)
{
	typedef void (*ss_fn)(void *, unsigned, int, const char *const *, const int *);
	if (count > 0 && count <= 256 && str) {
		size_t total = 0, k, o = 0;
		char *cat, *rew = NULL;
		for (k = 0; k < (size_t)count; k++)
			if (str[k]) total += (length && length[k] >= 0) ? (size_t)length[k] : strlen(str[k]);
		cat = malloc(total + 1);
		if (cat) {
			size_t nl;
			for (k = 0; k < (size_t)count; k++) {
				size_t n = str[k] ? ((length && length[k] >= 0) ? (size_t)length[k] : strlen(str[k])) : 0;
				if (n) { memcpy(cat + o, str[k], n); o += n; }
			}
			nl = sweet_rect_rewrite(cat, o, &rew);
			if (rew && (nl != o || memcmp(rew, cat, o) != 0)) {
				const char *one[1];
				int len1 = (int)nl;
				one[0] = rew;
				((ss_fn)rect_next)(rctx, shader, 1, one, &len1);
				free(cat); free(rew);
				return;
			}
			free(rew);
		}
		free(cat);
	}
	((ss_fn)rect_next)(rctx, shader, count, str, length);
}

/* Per-program record of the sweet_rect_inv_textureN uniforms the rewrite added, filled after a successful link; the draw hooks keep them
 * equal to 1 / size of the texture bound at the unit the sampler textureN is set to. */
#define GLI_RPROG_MAX 128
#define GLI_RECT_SAMPLERS 4
#define GLI_SLOT_DRAWARRAYS 65
#define GLI_SLOT_DRAWELEMENTS 67
#define GLI_SLOT_ACTIVETEXTURE 342
extern int _mesa_GetUniformLocation(unsigned, const char *);
extern void _mesa_GetActiveUniform(unsigned, unsigned, int, int *, int *, unsigned *, char *);
extern void _mesa_GetUniformiv(unsigned, int, int *);
extern void _mesa_Uniform2f(int, float, float);
extern void _mesa_ActiveTexture(unsigned);
static struct rprog { void *rctx; unsigned prog; int n; int samp[GLI_RECT_SAMPLERS], inv[GLI_RECT_SAMPLERS]; } rprogs[GLI_RPROG_MAX];   /* rctx: program ids are per context (review gli-gap-rect-default #5) */
static pthread_mutex_t rprog_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned nrprogs;
static const void *rect_next_lp, *orig_da, *orig_de;

static int u1w;   /* env SWEETGL_U1W=1: rect/unit-1 witnesses (diagnostic sessions only) */
static void hook_linkprogram_rect(void *rctx, unsigned program)
{
	((void (*)(void *, unsigned))rect_next_lp)(rctx, program);
	if (sh_ctx_ok(rctx)) {
		int st = 0, i, n = 0;
		struct rprog r;
		memset(&r, 0, sizeof r);
		_mesa_GetProgramiv(program, 0x8B82 /* GL_LINK_STATUS */, &st);
		if (!st) return;
		pthread_mutex_lock(&rprog_mu);
		{  /* a relink replaces the record; a program without rect uniforms (re-linked or a reused id) must not keep a stale one (review gli-rect-normalise F2) */
			unsigned q;
			for (q = 0; q < nrprogs; q++)
				if (rprogs[q].prog == program && rprogs[q].rctx == rctx) { rprogs[q] = rprogs[--nrprogs]; break; }
		}
		for (i = 0; i < GLI_RECT_SAMPLERS; i++) {
			char nm[40];
			int inv, sp;
			snprintf(nm, sizeof nm, "sweet_rect_inv_texture%d", i);
			inv = _mesa_GetUniformLocation(program, nm);
			snprintf(nm, sizeof nm, "texture%d", i);
			sp = _mesa_GetUniformLocation(program, nm);
			if (inv >= 0 && sp >= 0) { r.samp[n] = sp; r.inv[n] = inv; n++; }
		}
		if (n) {
			r.rctx = rctx; r.prog = program; r.n = n;
			if (nrprogs < GLI_RPROG_MAX) rprogs[nrprogs++] = r;
		}
		pthread_mutex_unlock(&rprog_mu);
		if (n && u1w) {   /* rect witness: the linked program's active uniforms and their locations */
			int na = 0; unsigned ui;
			char line[900]; int o = 0;
			_mesa_GetProgramiv(program, 0x8B86 /* GL_ACTIVE_UNIFORMS */, &na);
			for (ui = 0; ui < (unsigned)na && ui < 40 && o < (int)sizeof line - 80; ui++) {
				char nm[64]; int len = 0, sz = 0; unsigned ty = 0;
				nm[0] = 0;
				_mesa_GetActiveUniform(program, ui, (int)sizeof nm, &len, &sz, &ty, nm);
				o += snprintf(line + o, sizeof line - o, " %s@%d", nm, _mesa_GetUniformLocation(program, nm));
			}
			fprintf(stderr, "sweetgl: rect witness link program %u uniforms %d:%s\n", program, na, line);
		}
	}
}

/* Rect uniform witness (sprint GPU 2026-11-10, alrect3: the rect mask sampler texture1 reads unit 1 while nothing is bound there; does CA set
 * texture1 to another unit through a location the rewrite shifted?). For rect programs only: the active uniforms with locations after link,
 * every GetUniformLocation answer, and every distinct Uniform1i(v) (program, location, value). Log only; values pass through unchanged. */
static const void *rw_next_u1i, *rw_next_u1iv, *rw_next_gul;
static int rect_prog_known(void *rctx, unsigned prog)
{
	unsigned k; int f = 0;
	pthread_mutex_lock(&rprog_mu);
	for (k = 0; k < nrprogs; k++) if (rprogs[k].prog == prog && rprogs[k].rctx == rctx) { f = 1; break; }
	pthread_mutex_unlock(&rprog_mu);
	return f;
}
static int rw_seen(unsigned a, unsigned b, unsigned c)
{
	static unsigned keys[256][3]; static unsigned nk;
	unsigned k; int nw = 0;
	pthread_mutex_lock(&rprog_mu);
	for (k = 0; k < nk; k++) if (keys[k][0] == a && keys[k][1] == b && keys[k][2] == c) break;
	if (k == nk && nk < 256) { keys[nk][0] = a; keys[nk][1] = b; keys[nk][2] = c; nk++; nw = 1; }
	pthread_mutex_unlock(&rprog_mu);
	return nw;
}
static void hook_u1i_rw(void *rctx, int loc, int v)
{
	int cur = 0;
	if (sh_ctx_ok(rctx)) {
		_mesa_GetIntegerv(0x8B8D, &cur);
		if (cur > 0 && rect_prog_known(rctx, (unsigned)cur) && rw_seen((unsigned)cur, (unsigned)loc, (unsigned)v))
			fprintf(stderr, "sweetgl: rect witness Uniform1i program %d loc %d value %d\n", cur, loc, v);
	}
	((void (*)(void *, int, int))rw_next_u1i)(rctx, loc, v);
}
static void hook_u1iv_rw(void *rctx, int loc, int n, void *vp)
{
	int cur = 0;
	if (sh_ctx_ok(rctx) && vp && n > 0) {
		_mesa_GetIntegerv(0x8B8D, &cur);
		if (cur > 0 && rect_prog_known(rctx, (unsigned)cur) && rw_seen((unsigned)cur, (unsigned)loc | 0x80000000u, (unsigned)((const int *)vp)[0]))
			fprintf(stderr, "sweetgl: rect witness Uniform1iv program %d loc %d n %d value0 %d\n", cur, loc, n, ((const int *)vp)[0]);
	}
	((void (*)(void *, int, int, void *))rw_next_u1iv)(rctx, loc, n, vp);
}
static int hook_gul_rw(void *rctx, unsigned prog, void *name)
{
	int r = ((int (*)(void *, unsigned, void *))rw_next_gul)(rctx, prog, name);
	if (sh_ctx_ok(rctx) && name && rect_prog_known(rctx, prog) && rw_seen(prog, 0x40000000u | (unsigned)r, (unsigned)strlen((const char *)name)))
		fprintf(stderr, "sweetgl: rect witness GetUniformLocation program %u '%.40s' -> %d\n", prog, (const char *)name, r);
	return r;
}

/* log each distinct (program, sampler, unit, outcome) once, up to 128 (a first-N cap only showed the startup draws, alrect2) */
static int rect_seen(unsigned prog, int samp, int unit, int ok)
{
	static unsigned keys[128]; static unsigned nk;
	unsigned key = (prog << 9) ^ ((unsigned)samp << 6) ^ ((unsigned)unit << 1) ^ (unsigned)ok, k;
	int nw = 0;
	pthread_mutex_lock(&rprog_mu);
	for (k = 0; k < nk; k++) if (keys[k] == key) break;
	if (k == nk && nk < 128) { keys[nk++] = key; nw = 1; }
	pthread_mutex_unlock(&rprog_mu);
	return nw;
}

/* Rect draw caller chain (env SWEETGL_U1W=1, sprint GPU 2026-11-10 defect 2, research 132 H4): at a rect-program draw, the frame-pointer chain of
 * return addresses as image+offset (dladdr), once per distinct chain (max 24). Log only. */
static void sweet_caller_chain(const char *tag, int a, int b)
{
#ifdef __APPLE__
	static uint64_t seen[24]; static unsigned nseen;
	uintptr_t ra[12]; unsigned n = 0;
	uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
	while (fp && n < 12) {
		uintptr_t next = fp[0], ret = fp[1];
		if (!ret) break;
		ra[n++] = ret;
		if (next <= (uintptr_t)fp || next - (uintptr_t)fp > (1u << 20)) break;
		fp = (uintptr_t *)next;
	}
	uint64_t h = 1469598103934665603ull;
	for (unsigned i = 0; i < n; i++) { h ^= ra[i]; h *= 1099511628211ull; }
	pthread_mutex_lock(&rprog_mu);
	unsigned k; for (k = 0; k < nseen; k++) if (seen[k] == h) break;
	int nw = k == nseen && nseen < 24;
	if (nw) seen[nseen++] = h;
	pthread_mutex_unlock(&rprog_mu);
	if (!nw) return;
	char line[1400]; size_t o = 0;
	line[0] = 0;
	for (unsigned i = 0; i < n && o < sizeof line - 80; i++) {
		Dl_info di; memset(&di, 0, sizeof di);
		const char *nm = "?"; uintptr_t off = ra[i];
		if (dladdr((void *)ra[i], &di) && di.dli_fname) { nm = strrchr(di.dli_fname, '/'); nm = nm ? nm + 1 : di.dli_fname; off = ra[i] - (uintptr_t)di.dli_fbase; }
		o += (size_t)snprintf(line + o, sizeof line - o, " %s+0x%lx", nm, (unsigned long)off);
	}
	fprintf(stderr, "sweetgl: %s %d %d chain:%s\n", tag, a, b, line);
#else
	(void)tag; (void)a; (void)b;
#endif
}
static void rect_draw_chain(int prog, int tex1) { sweet_caller_chain("U1W rect draw program/unit1-tex", prog, tex1); }
static void rect_pre_draw(void *rctx)
{
	int cur = 0, act = 0, i;
	unsigned k;
	struct rprog rp;
	int found = 0;
	memset(&rp, 0, sizeof rp);
	if (!nrprogs || !sh_ctx_ok(rctx)) return;
	_mesa_GetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &cur);
	pthread_mutex_lock(&rprog_mu);   /* copy the record of THIS context's program under the lock, GL calls outside it (review gli-gap-rect-default #5) */
	for (k = 0; k < nrprogs; k++)
		if (rprogs[k].prog == (unsigned)cur && rprogs[k].rctx == rctx) { rp = rprogs[k]; found = 1; break; }
	pthread_mutex_unlock(&rprog_mu);
	if (!found) return;
	{
		_mesa_GetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &act);
		for (i = 0; i < rp.n; i++) {
			int unit = 0, tex = 0;
			unsigned w, h;
			_mesa_GetUniformiv((unsigned)cur, rp.samp[i], &unit);
			if (unit < 0 || unit > 31) continue;
			_mesa_ActiveTexture(0x84C0 + (unsigned)unit);
			_mesa_GetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &tex);
			if (tex_size((unsigned)tex, &w, &h) || sweet_mesa_unit_tex2d_size((unsigned)unit, &w, &h)) {   /* table (surfaces) or Mesa's own texture object */
				_mesa_Uniform2f(rp.inv[i], 1.0f / (float)w, 1.0f / (float)h);
				if (rect_seen((unsigned)cur, i, unit, 1)) fprintf(stderr, "sweetgl: rect inv program %d texture%d unit %d tex %d %ux%u\n", cur, i, unit, tex, w, h);
			} else if (rect_seen((unsigned)cur, i, unit, 0)) fprintf(stderr, "sweetgl: GAP rect inv program %d texture%d unit %d tex %d: size unknown (scale left as is)\n", cur, i, unit, tex);
		}
		if (u1w) {
			int t1 = 0;
			_mesa_ActiveTexture(0x84C1);
			_mesa_GetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &t1);
			rect_draw_chain(cur, t1);
		}
		_mesa_ActiveTexture((unsigned)act);
		return;
	}
}

static void hook_drawarrays_rect(void *rctx, unsigned mode, int first, int count)
{
	rect_pre_draw(rctx);
	((void (*)(void *, unsigned, int, int))orig_da)(rctx, mode, first, count);
}

/* sprint GPU 2026-11-10: CA also draws through glDrawRangeElements (405) and the instanced entry points (762/763); without the pre-draw there the
 * sweet_rect_inv_* scale kept a stale or zero value and the App Library search-field magnifier (an A8 rect mask) was sampled at one texel = a
 * filled square (gputest-count1 rt-applib, replay op 6134 fs 3 t0=128x54:a8). */
static const void *orig_dre, *orig_dai, *orig_dei;
static void hook_drawrangeelements_rect(void *rctx, unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void *indices)
{
	rect_pre_draw(rctx);
	((void (*)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *))orig_dre)(rctx, mode, start, end, count, type, indices);
}
static void hook_drawarraysinstanced_rect(void *rctx, unsigned mode, int first, int count, int inst)
{
	rect_pre_draw(rctx);
	((void (*)(void *, unsigned, int, int, int))orig_dai)(rctx, mode, first, count, inst);
}
static void hook_drawelementsinstanced_rect(void *rctx, unsigned mode, int count, unsigned type, const void *indices, int inst)
{
	rect_pre_draw(rctx);
	((void (*)(void *, unsigned, int, unsigned, const void *, int))orig_dei)(rctx, mode, count, type, indices, inst);
}


/*
 * GL name quarantine (sprint GPU 2026-11-11, defect 1, research 135 cause 1; default ON, SWEETGL_NAMEQ=0 turns it off).
 * Mesa 26 hands out the LOWEST free name (_mesa_HashFindFreeKeys -> util_idalloc_sparse_alloc), so a texture / framebuffer name CA just deleted comes
 * straight back from the next glGen*. CA::OGL keeps a per-unit binding cache that finalize_surface does not scrub (research 135): with the reused name
 * still cached, update_texture skips glBindTexture, CA's glTexImage2D(NULL) lands on texture 0 and the new texture is attached storage-less to an FBO
 * (gputest-fbo1: color0 0x0, INCOMPLETE_ATTACHMENT, GL_INVALID_FRAMEBUFFER_OPERATION in Clear/Draw, App Library backdrop black). Apple's GL does not
 * reuse names at once. Here a deleted name is held back for SWEET_NAMEQ_NS: a glGen* that gets a quarantined name keeps it (an empty object, held by us)
 * and asks again; held names are really deleted when their quarantine ends. Counters in a 'SWEETGL: NAMEQ' line every 4096 skips.
 */
#define SWEET_NAMEQ_NS 10000000000ull   /* 10 s: CA rebinds every unit many times per second, a stale cache entry is long gone by then */
#define SWEET_NAMEQ_MAX 8192
struct sweet_nameq {
	const char *what;
	const void *next_gen, *next_del;
	unsigned dname[SWEET_NAMEQ_MAX]; unsigned long long dtime[SWEET_NAMEQ_MAX]; unsigned dn, dhead;   /* recently deleted (ring) */
	unsigned hname[SWEET_NAMEQ_MAX]; unsigned long long htime[SWEET_NAMEQ_MAX]; unsigned hn;           /* held back by us */
	unsigned long long skipped, released, overflow;
};
static struct sweet_nameq nq_tex = { .what = "texture" }, nq_fbo = { .what = "framebuffer" };
static pthread_mutex_t nq_mu = PTHREAD_MUTEX_INITIALIZER;
static int nameq_on = 1;
static unsigned long long nq_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec; }
/* caller holds nq_mu */
static int nq_recent(struct sweet_nameq *q, unsigned name, unsigned long long now, unsigned long long *when)
{
	unsigned i, k;
	for (i = 0; i < q->dn; i++) {
		k = (q->dhead + SWEET_NAMEQ_MAX - 1 - i) % SWEET_NAMEQ_MAX;
		if (q->dname[k] == name) { if (now - q->dtime[k] < SWEET_NAMEQ_NS) { *when = q->dtime[k]; return 1; } return 0; }
	}
	return 0;
}
static void nq_gen_core(struct sweet_nameq *q, void *rctx, int n, unsigned *names, unsigned long long now)
{
	unsigned rel[64], nrel = 0, i;
	int k;
	pthread_mutex_lock(&nq_mu);
	for (i = 0; i < q->hn && nrel < 64; ) {   /* quarantine over: give held names back to Mesa (outside the lock) */
		if (now - q->htime[i] >= SWEET_NAMEQ_NS) { rel[nrel++] = q->hname[i]; q->hname[i] = q->hname[--q->hn]; q->htime[i] = q->htime[q->hn]; }
		else i++;
	}
	q->released += nrel;
	pthread_mutex_unlock(&nq_mu);
	if (nrel) ((void (*)(void *, int, const unsigned *))q->next_del)(rctx, (int)nrel, rel);
	for (k = 0; k < n; k++) {
		int tries;
		for (tries = 0; tries < 4096; tries++) {
			unsigned nm = 0; unsigned long long when = 0; int hold = 0;
			((void (*)(void *, int, unsigned *))q->next_gen)(rctx, 1, &nm);
			if (!nm) { names[k] = 0; break; }   /* no current context: the wrapper wrote nothing; pass the 0 through as before */
			pthread_mutex_lock(&nq_mu);
			if (nq_recent(q, nm, now, &when)) {
				if (q->hn < SWEET_NAMEQ_MAX) { q->hname[q->hn] = nm; q->htime[q->hn] = when; q->hn++; hold = 1; q->skipped++; }
				else q->overflow++;
			}
			pthread_mutex_unlock(&nq_mu);
			if (!hold) { names[k] = nm; break; }
			if ((q->skipped & 4095) == 1)
				fprintf(stderr, "SWEETGL: NAMEQ %s skipped %llu released %llu held %u overflow %llu (a just-deleted name came back from glGen; held 10 s)\n",
				        q->what, q->skipped, q->released, q->hn, q->overflow);
		}
		if (tries == 4096) fprintf(stderr, "SWEET GAP: NAMEQ %s: 4096 quarantined names in a row, the last one is handed out\n", q->what);
	}
}
static void nq_del_core(struct sweet_nameq *q, void *rctx, int n, const unsigned *names, unsigned long long now)
{
	int k;
	pthread_mutex_lock(&nq_mu);
	for (k = 0; k < n; k++) {
		if (!names[k]) continue;
		q->dname[q->dhead] = names[k]; q->dtime[q->dhead] = now;
		q->dhead = (q->dhead + 1) % SWEET_NAMEQ_MAX;
		if (q->dn < SWEET_NAMEQ_MAX) q->dn++;
	}
	pthread_mutex_unlock(&nq_mu);
	((void (*)(void *, int, const unsigned *))q->next_del)(rctx, n, names);
}
static void nq_gen(struct sweet_nameq *q, void *rctx, int n, unsigned *names)
{
	if (!sh_ctx_ok(rctx) || n <= 0 || !names) { ((void (*)(void *, int, unsigned *))q->next_gen)(rctx, n, names); return; }   /* no context: Mesa's wrapper writes nothing */
	nq_gen_core(q, rctx, n, names, nq_now());
}
static void nq_del(struct sweet_nameq *q, void *rctx, int n, const unsigned *names)
{
	if (!sh_ctx_ok(rctx) || n <= 0 || !names) { ((void (*)(void *, int, const unsigned *))q->next_del)(rctx, n, names); return; }
	nq_del_core(q, rctx, n, names, nq_now());
}
/* host self-test of the quarantine against a Mesa-like lowest-free allocator (gli_table_test.c): 0 = pass */
static unsigned char st_used[64];
static void st_gen(void *r, int n, unsigned *o) { (void)r; for (int k = 0; k < n; k++) { unsigned i = 1; while (i < 64 && st_used[i]) i++; st_used[i] = 1; o[k] = i; } }
static void st_del(void *r, int n, const unsigned *o) { (void)r; for (int k = 0; k < n; k++) if (o[k] < 64) st_used[o[k]] = 0; }
int sweet_nameq_selftest(void)
{
	static struct sweet_nameq q;
	unsigned a[3], b = 0, c = 0, d = 0;
	unsigned long long t0 = 1000000000ull;
	memset(&q, 0, sizeof q); memset(st_used, 0, sizeof st_used); q.what = "selftest"; q.next_gen = (const void *)st_gen; q.next_del = (const void *)st_del;
	nq_gen_core(&q, NULL, 3, a, t0);                       /* 1 2 3 */
	if (a[0] != 1 || a[1] != 2 || a[2] != 3) return 1;
	nq_del_core(&q, NULL, 1, &a[1], t0 + 1);               /* delete 2 */
	nq_gen_core(&q, NULL, 1, &b, t0 + 2);                  /* Mesa would hand 2 back: must get 4, with 2 held */
	if (b != 4 || q.hn != 1 || q.hname[0] != 2 || !st_used[2]) return 2;
	nq_gen_core(&q, NULL, 1, &c, t0 + 3);                  /* 2 still held: 5 */
	if (c != 5) return 3;
	nq_gen_core(&q, NULL, 1, &d, t0 + 1 + SWEET_NAMEQ_NS); /* quarantine over: 2 released and handed out again */
	if (d != 2 || q.hn != 0 || q.released != 1) return 4;
	return 0;
}
static void hook_gentextures_nq(void *rctx, int n, unsigned *names) { nq_gen(&nq_tex, rctx, n, names); }
static void hook_deletetextures_nq(void *rctx, int n, const unsigned *names) { nq_del(&nq_tex, rctx, n, names); }
static void hook_genframebuffers_nq(void *rctx, int n, unsigned *names) { nq_gen(&nq_fbo, rctx, n, names); }
static void hook_deleteframebuffers_nq(void *rctx, int n, const unsigned *names) { nq_del(&nq_fbo, rctx, n, names); }

/* G-7 (docs/PLAN-GPU-WIFI.md): SWEETGL_COMPUTE_TEST=1 runs ONE compute dispatch from inside the first draw of the process (a current context is needed, CA's own is the only one):
 * a 4-invocation shader writes 0x50495845+i into an SSBO, read back and compared. Prints "SWEET COMPUTE: PASS|FAIL ..." and a SWEET GAP line for every failing step, never a silent pass.
 * Restores CURRENT_PROGRAM and the SSBO binding; creates its own objects and deletes them. Diagnostic only, off by default. */
extern unsigned _mesa_GetError(void);
extern const unsigned char *_mesa_GetString(unsigned);
extern unsigned _mesa_CreateShader(unsigned);
extern unsigned _mesa_CreateProgram(void);
extern void _mesa_ShaderSource(unsigned, int, const char *const *, const int *);
extern void _mesa_CompileShader(unsigned);
extern void _mesa_AttachShader(unsigned, unsigned);
extern void _mesa_LinkProgram(unsigned);
extern void _mesa_UseProgram(unsigned);
extern void _mesa_DeleteProgram(unsigned);
extern void _mesa_DeleteShader(unsigned);
extern void _mesa_GenBuffers(int, unsigned *);
extern void _mesa_DeleteBuffers(int, const unsigned *);
extern void _mesa_BindBuffer(unsigned, unsigned);
extern void _mesa_BindBufferBase(unsigned, unsigned, unsigned);
extern void _mesa_BufferData(unsigned, long, const void *, unsigned);
extern void _mesa_GetBufferSubData(unsigned, long, long, void *);
extern void _mesa_DispatchCompute(unsigned, unsigned, unsigned);
extern void _mesa_MemoryBarrier(unsigned);
extern void _mesa_GetIntegeri_v(unsigned, unsigned, int *);
static int compute_done;
static void compute_selftest(void *rctx)
{
	static const char *src = "#version 310 es\nlayout(local_size_x = 1) in;\nlayout(std430, binding = 0) buffer B { uint v[4]; };\nvoid main() { v[gl_GlobalInvocationID.x] = 0x50495845u + gl_GlobalInvocationID.x; }\n";
	unsigned sh, prog, buf = 0, out[4] = { 0, 0, 0, 0 }, i;
	int st = 0, prev_prog = 0, prev_buf = 0, prev_base0 = 0, bad = 0;
	const unsigned char *ver;
	if (compute_done) return;
	if (!getenv("SWEETGL_COMPUTE_TEST")) { compute_done = 1; return; }
	if (!sh_ctx_ok(rctx)) return;   /* retried on the next draw */
	compute_done = 1;
	ver = _mesa_GetString(0x1F02 /* VERSION */);
	fprintf(stderr, "sweetgl: SWEET COMPUTE: start, GL_VERSION %s\n", ver ? (const char *)ver : "(null)");
	for (i = 0; i < 16 && _mesa_GetError() != 0; i++) {}
	_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prev_prog);
	_mesa_GetIntegerv(0x90D3 /* SHADER_STORAGE_BUFFER_BINDING */, &prev_buf);
	_mesa_GetIntegeri_v(0x90D3, 0, &prev_base0);   /* indexed binding 0 is overwritten by BindBufferBase below (review gli-compute-selftest #2) */
	sh = _mesa_CreateShader(0x91B9 /* COMPUTE_SHADER */);
	_mesa_ShaderSource(sh, 1, &src, NULL);
	_mesa_CompileShader(sh);
	_mesa_GetShaderiv(sh, 0x8B81 /* COMPILE_STATUS */, &st);
	if (!st) {
		char log[256]; int len = 0;
		log[0] = 0; _mesa_GetShaderInfoLog(sh, (int)sizeof log, &len, log);
		fprintf(stderr, "sweetgl: SWEET COMPUTE: FAIL compile status %d: %s\n", st, log);
		sweet_gap("compute", "compute shader did not compile");
		_mesa_DeleteShader(sh);
		for (i = 0; i < 16 && _mesa_GetError() != 0; i++) {}   /* review #1: no GL error left for CA on a context without compute */
		return;
	}
	prog = _mesa_CreateProgram();
	_mesa_AttachShader(prog, sh);
	_mesa_LinkProgram(prog);
	st = 0; _mesa_GetProgramiv(prog, 0x8B82 /* LINK_STATUS */, &st);
	if (!st) {
		fprintf(stderr, "sweetgl: SWEET COMPUTE: FAIL link status %d\n", st);
		sweet_gap("compute", "compute program did not link");
		_mesa_DeleteProgram(prog); _mesa_DeleteShader(sh);
		for (i = 0; i < 16 && _mesa_GetError() != 0; i++) {}
		return;
	}
	_mesa_GenBuffers(1, &buf);
	_mesa_BindBuffer(0x90D2 /* SHADER_STORAGE_BUFFER */, buf);
	_mesa_BufferData(0x90D2, (long)sizeof out, out, 0x88E9 /* DYNAMIC_READ */);
	_mesa_BindBufferBase(0x90D2, 0, buf);
	_mesa_UseProgram(prog);
	_mesa_DispatchCompute(4, 1, 1);
	_mesa_MemoryBarrier(0x2000 /* SHADER_STORAGE_BARRIER_BIT */);
	_mesa_GetBufferSubData(0x90D2, 0, (long)sizeof out, out);
	for (i = 0; i < 4; i++) if (out[i] != 0x50495845u + i) bad++;
	{
		unsigned err = _mesa_GetError();
		if (err) { fprintf(stderr, "sweetgl: SWEET COMPUTE: GL error 0x%x after dispatch\n", err); sweet_gap("compute", "GL error after dispatch"); bad++; }
	}
	fprintf(stderr, "sweetgl: SWEET COMPUTE: %s result %08x %08x %08x %08x (expected 50495845 50495846 50495847 50495848)\n", bad ? "FAIL" : "PASS", out[0], out[1], out[2], out[3]);
	if (bad) sweet_gap("compute", "dispatch result does not match");
	_mesa_UseProgram((unsigned)prev_prog);
	_mesa_BindBufferBase(0x90D2, 0, (unsigned)prev_base0);
	_mesa_BindBuffer(0x90D2, (unsigned)prev_buf);
	_mesa_DeleteBuffers(1, &buf);
	_mesa_DeleteProgram(prog);
	_mesa_DeleteShader(sh);
	for (i = 0; i < 16 && _mesa_GetError() != 0; i++) {}
}

static void hook_drawelements_rect(void *rctx, unsigned mode, int count, unsigned type, const void *indices)
{
	compute_selftest(rctx);
	rect_pre_draw(rctx);
	((void (*)(void *, unsigned, int, unsigned, const void *))orig_de)(rctx, mode, count, type, indices);
}

/* G-11 step 2 (research 61): CoreAnimation binds its IOSurface-backed icon-mask texture as GL_TEXTURE_RECTANGLE_ARB (0x84F5), which Mesa's GLES2 context
 * rejects (INVALID_ENUM), so the sampler reads an unbound texture. With SWEETGL_RECT=1 the target of glBindTexture / glTexParameter{i,f,iv,fv} is mapped to
 * GL_TEXTURE_2D (the surface is imported as a 2D texture by eagl_mesa.c be_tex_image_surface, which also accepts 0x84F5). The first GLI_RECT_LOG mappings are logged. */
#define GLI_SLOT_BINDTEXTURE 5
#define GLI_SLOT_TEXPARAMETERF 302
#define GLI_SLOT_TEXPARAMETERFV 303
#define GLI_SLOT_TEXPARAMETERI 304
#define GLI_SLOT_TEXPARAMETERIV 305
#define GLI_RECT_LOG 16
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#define GLI_TEXTURE_2D 0x0DE1
static const void *orig_bt, *orig_tpf, *orig_tpfv, *orig_tpi, *orig_tpiv;
static unsigned rect_map_n[5];   /* per hook, so Bind logs cannot hide the TexParameter ones (review gli-rect-texture-target F4) */

static unsigned rect_target(unsigned *n, const char *fn, unsigned target)
{
	if (target != GL_TEXTURE_RECTANGLE_ARB) return target;
	if (*n < GLI_RECT_LOG) { (*n)++; fprintf(stderr, "sweetgl: rect target 0x84f5 -> 0xde1 in %s\n", fn); }
	return GLI_TEXTURE_2D;
}

/* Unit-1 witness (env SWEETGL_U1W=1, sprint GPU 2026-11-10, alrect4: CA sets the rect mask sampler texture1 to unit 1 yet nothing is bound there at
 * the draw): every BindTexture issued while unit 1 is active (first 80 lines), and every DeleteTextures of the name last bound there. Log only. */
/* u1w: declared with the rect records above */
static unsigned u1w_last, u1w_n;
static const void *orig_dt;
static void hook_bindtexture(void *rctx, unsigned target, unsigned tex)
{
	if (u1w && sh_ctx_ok(rctx)) {
		int act = 0;
		_mesa_GetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &act);
		if (act == 0x84C1) {
			u1w_last = tex;
			if (u1w_n < 80) { u1w_n++; fprintf(stderr, "sweetgl: U1W bind unit 1 target 0x%x tex %u ctx %p\n", target, tex, rctx); }
		}
	}
	((void (*)(void *, unsigned, unsigned))orig_bt)(rctx, rect_target(&rect_map_n[0], "BindTexture", target), tex);
}
static void hook_deletetextures_u1w(void *rctx, int n, void *names)
{
	if (u1w && names && n > 0) {
		int i;
		for (i = 0; i < n; i++)
			if (((const unsigned *)names)[i] == u1w_last && u1w_last && u1w_n < 120) { u1w_n++; fprintf(stderr, "sweetgl: U1W delete of unit-1 tex %u ctx %p\n", u1w_last, rctx); }
	}
	((void (*)(void *, int, void *))orig_dt)(rctx, n, names);
}
static void hook_texparameterf(void *rctx, unsigned target, unsigned pname, float param)
{
	((void (*)(void *, unsigned, unsigned, float))orig_tpf)(rctx, rect_target(&rect_map_n[1], "TexParameterf", target), pname, param);
}
static void hook_texparameterfv(void *rctx, unsigned target, unsigned pname, const float *params)
{
	((void (*)(void *, unsigned, unsigned, const float *))orig_tpfv)(rctx, rect_target(&rect_map_n[2], "TexParameterfv", target), pname, params);
}
static void hook_texparameteri(void *rctx, unsigned target, unsigned pname, int param)
{
	((void (*)(void *, unsigned, unsigned, int))orig_tpi)(rctx, rect_target(&rect_map_n[3], "TexParameteri", target), pname, param);
}
static void hook_texparameteriv(void *rctx, unsigned target, unsigned pname, const int *params)
{
	((void (*)(void *, unsigned, unsigned, const int *))orig_tpiv)(rctx, rect_target(&rect_map_n[4], "TexParameteriv", target), pname, params);
}

/* G-11 (research 61, g77): hundreds of GL_INVALID_FRAMEBUFFER_OPERATION (glClear "incomplete framebuffer", glDrawElements) remain with the surfaces attached and
 * the icons are still not drawn. SWEETGL_FBO=1 checks glCheckFramebufferStatus before every Clear/Draw and logs the first distinct failures with the status
 * enum, the FBO name and what is attached to COLOR_ATTACHMENT0 (type, name, and for a texture its level-0 size and internal format). Read-only except a
 * temporary BindTexture(TEXTURE_2D), restored. */
extern unsigned _mesa_CheckFramebufferStatus(unsigned);
extern void _mesa_GetFramebufferAttachmentParameteriv(unsigned, unsigned, unsigned, int *);
extern void _mesa_GetTexLevelParameteriv(unsigned, int, unsigned, int *);
extern void _mesa_BindTexture(unsigned, unsigned);
extern void _mesa_BindRenderbuffer(unsigned, unsigned);
extern void _mesa_GetRenderbufferParameteriv(unsigned, unsigned, int *);
#define GLI_SLOT_CLEAR 10
static int fbo_diag_on;
static const void *fbo_next_da, *fbo_next_de, *fbo_next_cl;
static unsigned fbo_seen[32][2], fbo_nseen, fbo_logged;

extern void _mesa_DebugMessageCallback(void (*cb)(unsigned, unsigned, unsigned, unsigned, int, const char *, const void *), const void *user);
extern void _mesa_Enable(unsigned);
static void *dbg_ctx[16];   /* contexts that already got the callback: debug state is per context and CA makes several (review gli-khr-debug-diag F1) */
static unsigned dbg_nctx;
static char dbg_last[160];
/* research 90 L4: the 32-message cap was ONE counter for everything, so early FBO/GL-error noise could hide the App Library errors. Now one cap per class
 * (1 = FBO/incomplete, 2 = other "rror" / "GL_INVALID" / "GL_OUT_OF" messages) and a loud line when a class overflows (silent caps are forbidden, AP-102). Pure of GL: host-testable. */
#define GLI_DBG_CLASS_MAX 32
static unsigned dbg_cls_n[3];
static unsigned long dbg_cls_drop[3];
static pthread_mutex_t dbg_mu = PTHREAD_MUTEX_INITIALIZER;

/* 0 = not interesting (panfrost perf chatter, shader stats, flushes filled the 32-line cap in g85), 1 = FBO / incomplete, 2 = other error (also Mesa's GL_INVALID_* / GL_OUT_OF_* text) */
int sweet_gldebug_class(const char *msg)
{
	if (!msg) return 0;
	if (strstr(msg, "ncomplete") || strstr(msg, "FBO")) return 1;
	if (strstr(msg, "rror")) return 2;
	/* review runplan-gputest113 F2: Mesa's own text for a GL error is "GL_INVALID_OPERATION in glReadPixels(...)" (errors.c:272), which has no "rror" */
	if (strstr(msg, "GL_INVALID") || strstr(msg, "GL_OUT_OF")) return 2;
	return 0;
}

/* Decides whether `msg` is printed (return 1). `note` (size nn) is filled with a "SWEETGL: GLDEBUG dropped" line when this call is the 1st / 10th / 100th /
 * 1000th / 10000th drop of its class, else note[0] = 0; the caller prints it whatever the return value. */
int sweet_gldebug_accept(const char *msg, char *note, size_t nn)
{
	int c = sweet_gldebug_class(msg), ret = 0;
	if (nn) note[0] = 0;
	if (!c) return 0;
	pthread_mutex_lock(&dbg_mu);
	if (!strncmp(msg, dbg_last, sizeof dbg_last - 1)) { pthread_mutex_unlock(&dbg_mu); return 0; }   /* the same message repeats on every re-test of the FBO: count it once (F2) */
	strncpy(dbg_last, msg, sizeof dbg_last - 1); dbg_last[sizeof dbg_last - 1] = 0;
	if (dbg_cls_n[c] < GLI_DBG_CLASS_MAX) { dbg_cls_n[c]++; ret = 1; }
	else {
		unsigned long d = ++dbg_cls_drop[c];
		if (nn && (d == 1 || d == 10 || d == 100 || d == 1000 || d == 10000))
			snprintf(note, nn, "SWEETGL: GLDEBUG dropped %lu (class %s, cap %d per class)", d, c == 1 ? "fbo" : "error", GLI_DBG_CLASS_MAX);
	}
	pthread_mutex_unlock(&dbg_mu);
	return ret;
}

/* KHR_debug sink: Mesa's fbo_incomplete() sends "FBO incomplete: <reason> [<attachment>]" here even in a release build (the MESA_DEBUG=incomplete_fbo flag only works in debug builds) */
static void fbo_dbg_cb(unsigned src, unsigned type, unsigned id, unsigned sev, int len, const char *msg, const void *user)
{
	char note[128];
	int pr;
	(void)src; (void)type; (void)id; (void)sev; (void)len; (void)user;
	pr = sweet_gldebug_accept(msg, note, sizeof note);
	if (pr) { fprintf(stderr, "sweetgl: GLDEBUG %s", msg); if (!strchr(msg, '\n')) fputc('\n', stderr); }
	if (note[0]) fprintf(stderr, "%s\n", note);
}

/* Installs fbo_dbg_cb once per context (fbo_check and, research 90 L1/L2, the census hooks share it: one callback, never two). Caller made rctx current.
 * Review census-copytex-b2 F5: dbg_mu is held only to CLAIM the slot; the GL calls run outside it (fbo_dbg_cb takes the same mutex, a message delivered from inside those calls must not deadlock). */
static void dbg_install_ctx(void *rctx)
{
	unsigned q, n = __atomic_load_n(&dbg_nctx, __ATOMIC_ACQUIRE);
	int claimed = 0;
	for (q = 0; q < n && dbg_ctx[q] != rctx; q++) {}
	if (q < n) return;
	pthread_mutex_lock(&dbg_mu);
	for (q = 0; q < dbg_nctx && dbg_ctx[q] != rctx; q++) {}
	if (q == dbg_nctx && dbg_nctx < 16) {
		dbg_ctx[dbg_nctx] = rctx;
		__atomic_store_n(&dbg_nctx, dbg_nctx + 1, __ATOMIC_RELEASE);
		claimed = 1;
	}
	pthread_mutex_unlock(&dbg_mu);
	if (claimed) {
		_mesa_DebugMessageCallback(fbo_dbg_cb, NULL);
		_mesa_Enable(0x92E0 /* GL_DEBUG_OUTPUT */);
		_mesa_Enable(0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */);
	}
}

/* GL_FRAMEBUFFER status names for the SWEETGL_FBO line (research 86 sections E/F/H, DRAFT-format0-batch) [src GLES3 gl2ext values] */
const char *sweet_fbo_status_name(unsigned status)
{
	switch (status) {
	case 0x8CD6: return "INCOMPLETE_ATTACHMENT";
	case 0x8CD7: return "MISSING_ATTACHMENT";
	case 0x8CD9: return "INCOMPLETE_DIMENSIONS";
	case 0x8CDD: return "UNSUPPORTED";
	case 0x8D56: return "INCOMPLETE_MULTISAMPLE";
	default: return "other";
	}
}

int sweet_fbo_status_line(char *buf, size_t n, int fbo, const char *where, unsigned st, int type, int name, int w, int h, unsigned ifmt)
{
	return snprintf(buf, n, "SWEETGL: FBO incomplete fbo %d in %s status 0x%x (%s) attachment color0 type 0x%x name %d level0 %dx%d ifmt 0x%x", fbo, where, st, sweet_fbo_status_name(st), (unsigned)type, name, w, h, ifmt);
}

int sweet_fbo_sizes_line(char *buf, size_t n, int fbo, int r, int g, int b, int a)
{
	return snprintf(buf, n, "SWEETGL: FBO incomplete fbo %d color0 sizes r %d g %d b %d a %d", fbo, r, g, b, a);
}

/* ---- per-texture event history (SWEETGL_CENSUS=1, log-only; g111d: FBO 6 color0 texture 25 had no storage, what happened to it?) ----
 * GLI_TH_TAB (1024) textures, a ring of the last 12 events each; names are NOT qualified by context (guess: CA's contexts share one name space, a name reused by
 * another share group would mix two histories). Full table: the entry with the oldest LAST event is replaced and counted (th_evict, printed in the
 * stats line). Events of a deleted texture stay: a later glGenTextures of the same name appends to the same ring. Pure data (struct sweet_th_ev) and
 * pure formatters; the hooks (census section) only fill them. */
#define GLI_TH_TAB 1024   /* g111d: texture names reach ~194 by +960 s; 256 would evict texture 25 (review runplan-gputest112 F2). Static memory: sizeof(struct th_ent) = 520 bytes (8 + 4 + 4 + 12 * 40 + padding, 12 events of 40 bytes) * 1024 = ~520 KB bss, census only, no malloc */
#define GLI_TH_RING 12
#define GLI_TH_BINDS 3     /* only the first 3 binds of a texture are events, the rest is a counter */
#define GLI_TH_LINE 400
static int census_on;
struct th_ent { unsigned name; int used; unsigned long last_seq, total, binds; unsigned head, count; struct sweet_th_ev ev[GLI_TH_RING]; };
static struct th_ent th_tab[GLI_TH_TAB];
static unsigned long th_seq, th_events, th_evict, th_binds_unlogged, th_unattr;
static void *th_ctx[16];
static unsigned th_nctx;
#define GLI_TH_REPORT_MAX 8
static unsigned th_rep[GLI_TH_REPORT_MAX], th_nrep;   /* names already reported by the fbo-no-storage dump */
static pthread_mutex_t th_mu = PTHREAD_MUTEX_INITIALIZER;

int sweet_th_event_str(char *buf, size_t n, const struct sweet_th_ev *e)
{
	char hd[48], rest[112];
	if (e->ctx > 0) snprintf(hd, sizeof hd, "#%lu ctx %d", e->seq, e->ctx); else snprintf(hd, sizeof hd, "#%lu", e->seq);
	switch (e->kind) {
	case SWEET_TH_GEN: snprintf(rest, sizeof rest, "genned"); break;
	case SWEET_TH_BIND: snprintf(rest, sizeof rest, "bind 0x%x", e->a); break;
	case SWEET_TH_TEXIMAGE: snprintf(rest, sizeof rest, "teximage2d 0x%x level %d %dx%d ifmt 0x%x data %s", e->a, e->b, e->c, e->d, e->e, e->f ? "ptr" : "null"); break;
	case SWEET_TH_TEXSTORAGE: snprintf(rest, sizeof rest, "texstorage2d 0x%x levels %d %dx%d ifmt 0x%x", e->a, e->b, e->c, e->d, e->e); break;
	case SWEET_TH_COPYIMAGE: snprintf(rest, sizeof rest, "copyteximage2d 0x%x level %d %dx%d ifmt 0x%x", e->a, e->b, e->c, e->d, e->e); break;
	case SWEET_TH_FBOTEX:
		if (e->c < 0) snprintf(rest, sizeof rest, "fbotex fbo ? attach 0x%x textarget 0x%x level %d", e->a, e->e, e->b);
		else snprintf(rest, sizeof rest, "fbotex fbo %d attach 0x%x textarget 0x%x level %d", e->c, e->a, e->e, e->b);
		break;
	case SWEET_TH_DELETED: snprintf(rest, sizeof rest, "deleted"); break;
	case SWEET_TH_ATTACH: snprintf(rest, sizeof rest, "attachimage %u %dx%d fmt 0x%x", e->a, e->c, e->d, e->e); break;
	default: snprintf(rest, sizeof rest, "kind%d?", e->kind); break;
	}
	return snprintf(buf, n, "%s %s", hd, rest);
}

void sweet_th_reset(void)
{
	pthread_mutex_lock(&th_mu);
	memset(th_tab, 0, sizeof th_tab);
	th_seq = th_events = th_evict = th_binds_unlogged = 0;
	memset(th_ctx, 0, sizeof th_ctx); th_nctx = 0;
	memset(th_rep, 0, sizeof th_rep); th_nrep = 0;
	pthread_mutex_unlock(&th_mu);
	__atomic_store_n(&th_unattr, 0, __ATOMIC_RELAXED);
}

int sweet_th_ctx_index(void *rctx)
{
	unsigned i;
	int r = 0;
	pthread_mutex_lock(&th_mu);
	for (i = 0; i < th_nctx; i++) if (th_ctx[i] == rctx) { r = (int)i + 1; break; }
	if (!r && th_nctx < 16) { th_ctx[th_nctx++] = rctx; r = (int)th_nctx; }
	pthread_mutex_unlock(&th_mu);
	return r;
}

void sweet_th_unattributed(void) { __atomic_add_fetch(&th_unattr, 1, __ATOMIC_RELAXED); }

void sweet_th_record(unsigned name, struct sweet_th_ev ev)
{
	struct th_ent *e = NULL, *old = NULL, *fr = NULL;
	unsigned i;
	pthread_mutex_lock(&th_mu);
	for (i = 0; i < GLI_TH_TAB; i++) {
		if (th_tab[i].used && th_tab[i].name == name) { e = &th_tab[i]; break; }
		if (!th_tab[i].used) { if (!fr) fr = &th_tab[i]; }
		else if (!old || th_tab[i].last_seq < old->last_seq) old = &th_tab[i];
	}
	if (!e) {
		if (fr) e = fr; else { e = old; th_evict++; }
		memset(e, 0, sizeof *e);
		e->used = 1; e->name = name;
	}
	if (ev.kind == SWEET_TH_GEN) e->binds = 0;   /* a new incarnation of the name: its first binds are logged again */
	if (ev.kind == SWEET_TH_BIND) {
		e->binds++;
		if (e->binds > GLI_TH_BINDS) { th_binds_unlogged++; e->last_seq = th_seq; pthread_mutex_unlock(&th_mu); return; }   /* an actively bound texture is not the eviction victim */
		ev.c = (int)e->binds;
	}
	ev.seq = ++th_seq;
	if (e->count < GLI_TH_RING) e->ev[(e->head + e->count++) % GLI_TH_RING] = ev;
	else { e->ev[e->head] = ev; e->head = (e->head + 1) % GLI_TH_RING; }
	e->total++; th_events++; e->last_seq = ev.seq;
	pthread_mutex_unlock(&th_mu);
}

/* Line `idx` (0-based) of the history of texture `name`; 1 = produced, 0 = no such line. Pieces are joined with " | " and packed up to GLI_TH_LINE chars per line. */
int sweet_th_line_label(const char *label, unsigned name, unsigned idx, char *buf, size_t n)
{
	struct th_ent c;
	char piece[GLI_TH_RING + 2][128];
	unsigned np = 0, i, pi = 0, ln;
	int found = 0;
	pthread_mutex_lock(&th_mu);
	for (i = 0; i < GLI_TH_TAB; i++) if (th_tab[i].used && th_tab[i].name == name) { c = th_tab[i]; found = 1; break; }
	pthread_mutex_unlock(&th_mu);
	if (!found) {
		if (idx) return 0;
		snprintf(buf, n, "SWEETGL: TEXHIST %stex %u no events recorded (table cap or created before the hook)", label, name);
		return 1;
	}
	if (c.total > c.count) snprintf(piece[np++], sizeof piece[0], "(%lu older events dropped by the ring of %d)", c.total - c.count, GLI_TH_RING);
	for (i = 0; i < c.count; i++) sweet_th_event_str(piece[np++], sizeof piece[0], &c.ev[(c.head + i) % GLI_TH_RING]);
	if (c.binds > GLI_TH_BINDS) snprintf(piece[np++], sizeof piece[0], "binds %lu (only the first %d logged)", c.binds, GLI_TH_BINDS);
	for (ln = 0; pi < np; ln++) {
		char l[GLI_TH_LINE + 256];
		size_t o = (size_t)snprintf(l, sizeof l, "SWEETGL: TEXHIST %s%stex %u events %lu:", ln ? "cont " : "", label, name, c.total);
		int first = 1;
		while (pi < np) {
			size_t pl = strlen(piece[pi]);
			if (!first && o + 3 + pl > GLI_TH_LINE) break;
			o += (size_t)snprintf(l + o, sizeof l - o, "%s%s", first ? " " : " | ", piece[pi]);
			first = 0; pi++;
		}
		if (ln == idx) { snprintf(buf, n, "%s", l); return 1; }
	}
	return 0;
}

int sweet_th_line(unsigned name, unsigned idx, char *buf, size_t n) { return sweet_th_line_label("", name, idx, buf, n); }

/* F4 (review runplan-gputest112): textures that were attached to an FBO (fbotex event in the ring) but show no storage event (teximage2d, texstorage2d,
 * copyteximage2d, attachimage). At most GLI_TH_REPORT_MAX reports per process, once per name. If the ring dropped events the absence cannot be claimed:
 * *dropped = 1 and the caller prints `fbo-ring-dropped`. Reads the table only (no GL call). Returns 1 and sets *name, 0 = nothing (more) to report. */
int sweet_th_fbo_pick(unsigned *name, int *dropped)
{
	unsigned i, k, j;
	int r = 0;
	pthread_mutex_lock(&th_mu);
	for (i = 0; i < GLI_TH_TAB && !r && th_nrep < GLI_TH_REPORT_MAX; i++) {
		const struct th_ent *e = &th_tab[i];
		int fbo = 0, storage = 0, seen = 0;
		if (!e->used) continue;
		for (j = 0; j < th_nrep; j++) if (th_rep[j] == e->name) seen = 1;
		if (seen) continue;
		for (k = 0; k < e->count; k++) {
			int kd = e->ev[(e->head + k) % GLI_TH_RING].kind;
			if (kd == SWEET_TH_FBOTEX) fbo = 1;
			if (kd == SWEET_TH_TEXIMAGE || kd == SWEET_TH_TEXSTORAGE || kd == SWEET_TH_COPYIMAGE || kd == SWEET_TH_ATTACH) storage = 1;
		}
		if (fbo && !storage) { th_rep[th_nrep++] = e->name; *name = e->name; *dropped = e->total > e->count; r = 1; }
	}
	pthread_mutex_unlock(&th_mu);
	return r;
}

/* every 20 s from the census thread, after the stats line */
void sweet_th_fbo_report(void)
{
	unsigned n, k;
	int dropped;
	char l[GLI_TH_LINE + 256];
	while (sweet_th_fbo_pick(&n, &dropped))
		for (k = 0; sweet_th_line_label(dropped ? "fbo-ring-dropped " : "fbo-no-storage ", n, k, l, sizeof l); k++) fprintf(stderr, "%s\n", l);
}

void sweet_th_print(unsigned name)
{
	char l[GLI_TH_LINE + 256];
	unsigned k;
	for (k = 0; sweet_th_line(name, k, l, sizeof l); k++) fprintf(stderr, "%s\n", l);
}

int sweet_th_stats_line(char *buf, size_t n)
{
	unsigned i, used = 0;
	unsigned long ev, evict, bu, un;
	pthread_mutex_lock(&th_mu);
	for (i = 0; i < GLI_TH_TAB; i++) used += th_tab[i].used ? 1u : 0u;
	ev = th_events; evict = th_evict; bu = th_binds_unlogged;
	pthread_mutex_unlock(&th_mu);
	un = __atomic_load_n(&th_unattr, __ATOMIC_RELAXED);
	return snprintf(buf, n, "SWEETGL: TEXHIST stats events %lu textures %u of %d evictions %lu binds_unlogged %lu unattributed %lu", ev, used, GLI_TH_TAB, evict, bu, un);
}

/* ---- input provenance (part A, census armed mode): the texture bound to GL_TEXTURE_2D on units 0..7, tracked from the BindTexture / ActiveTexture calls alone (no GL query).
 * Per context (index of sweet_th_ctx_index, 17 slots, the 17th+ contexts share slot 0 and are counted as `ctxover`). Units >= 8 and
 * targets other than 2D / rectangle are ignored; a deleted texture stays listed (the history says `del`). Default active unit = 0 (GL_TEXTURE0 0x84C0). */
#define GLI_TU_N 8
#define GLI_CTX_N 17   /* slot 0 = overflow (sweet_th_ctx_index returns 0 for the 17th and later contexts: they SHARE this slot; every hook call attributed to it is counted, stats line `ctxover N`) */
struct tu_s { unsigned tex[GLI_TU_N], act; };   /* act 0..7, 8 = a unit we do not track */
static struct tu_s tu_c[GLI_CTX_N];
static pthread_mutex_t tu_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long cen_ctx_over;
/* context slot of a hook call (review prov-unbind-b5 F2): the same small index the texture history uses, 1..16; 0 = overflow, counted */
static int cen_ctx(void *rctx)
{
	int c = sweet_th_ctx_index(rctx);
	if (c < 0 || c >= GLI_CTX_N) c = 0;
	if (!c) __atomic_add_fetch(&cen_ctx_over, 1, __ATOMIC_RELAXED);
	return c;
}
void sweet_tu_reset(void)
{
	pthread_mutex_lock(&tu_mu);
	memset(tu_c, 0, sizeof tu_c);
	pthread_mutex_unlock(&tu_mu);
}
void sweet_tu_active(int ctx, unsigned unit)
{
	if (ctx < 0 || ctx >= GLI_CTX_N) return;
	pthread_mutex_lock(&tu_mu);
	tu_c[ctx].act = (unit >= 0x84C0 && unit < 0x84C0 + GLI_TU_N) ? unit - 0x84C0 : GLI_TU_N;
	pthread_mutex_unlock(&tu_mu);
}
/* GL_TEXTURE_2D and GL_TEXTURE_RECTANGLE_ARB 0x84F5 (F1: census is the OUTERMOST hook, so it sees CA's raw rect target; the rect -> 0xde1 mapping is applied below it) */
void sweet_tu_bind(int ctx, unsigned target, unsigned tex)
{
	if (target != 0x0DE1 /* GL_TEXTURE_2D */ && target != 0x84F5 /* GL_TEXTURE_RECTANGLE_ARB */) return;
	if (ctx < 0 || ctx >= GLI_CTX_N) return;
	pthread_mutex_lock(&tu_mu);
	if (tu_c[ctx].act < GLI_TU_N) tu_c[ctx].tex[tu_c[ctx].act] = tex;
	pthread_mutex_unlock(&tu_mu);
}
void sweet_tu_snapshot(int ctx, unsigned out[8])
{
	memset(out, 0, 8 * sizeof out[0]);
	if (ctx < 0 || ctx >= GLI_CTX_N) return;
	pthread_mutex_lock(&tu_mu);
	memcpy(out, tu_c[ctx].tex, sizeof tu_c[ctx].tex);
	pthread_mutex_unlock(&tu_mu);
}

/* From the history of `name`: size of the latest level-0 teximage2d / copyteximage2d or any texstorage2d / attachimage event (*w = *h = 0 when none), FBO of the latest
 * fbotex event (*att = -1 none, -2 recorded with an unknown FBO), *del = the newest event is `deleted`. Returns 0 when the texture has no history at all. */
int sweet_th_info(unsigned name, int *w, int *h, int *att, int *del)
{
	unsigned i, k;
	int found = 0;
	*w = *h = 0; *att = -1; *del = 0;
	pthread_mutex_lock(&th_mu);
	for (i = 0; i < GLI_TH_TAB && !found; i++) {
		const struct th_ent *e = &th_tab[i];
		if (!e->used || e->name != name) continue;
		found = 1;
		for (k = 0; k < e->count; k++) {
			const struct sweet_th_ev *v = &e->ev[(e->head + k) % GLI_TH_RING];
			if (((v->kind == SWEET_TH_TEXIMAGE || v->kind == SWEET_TH_COPYIMAGE) && v->b == 0) || v->kind == SWEET_TH_TEXSTORAGE || v->kind == SWEET_TH_ATTACH) { *w = v->c; *h = v->d; }
			if (v->kind == SWEET_TH_FBOTEX) *att = v->c >= 0 ? v->c : -2;
			*del = v->kind == SWEET_TH_DELETED;
		}
	}
	pthread_mutex_unlock(&th_mu);
	return found;
}

/* "SWEETGL: PIXELS prov r<round> fbo <n> units: u0 tex 25 1082x1226 att 3 | u1 tex 31 ? att - | ... | +N more": bound units only (tex != 0), at most 4 shown, `-` when none;
 * size `?` without a size event, `del` when the history ends in `deleted`, att `-` none / `?` unknown FBO */
int sweet_prov_line(char *buf, size_t n, int round, int fbo, const unsigned tex[8])
{
	int o = snprintf(buf, n, "SWEETGL: PIXELS prov r%d fbo %d units:", round, fbo), shown = 0, bound = 0, u;
	for (u = 0; u < GLI_TU_N; u++) {
		int w, h, att, del;
		char sz[32], at[16];
		if (!tex[u]) continue;
		bound++;
		if (shown >= 4 || o < 0 || (size_t)o >= n) continue;
		sweet_th_info(tex[u], &w, &h, &att, &del);
		if (del) snprintf(sz, sizeof sz, "del"); else if (w > 0 && h > 0) snprintf(sz, sizeof sz, "%dx%d", w, h); else snprintf(sz, sizeof sz, "?");
		if (att >= 0) snprintf(at, sizeof at, "%d", att); else snprintf(at, sizeof at, att == -2 ? "?" : "-");
		o += snprintf(buf + o, n - (size_t)o, "%s u%d tex %u %s att %s", shown ? " |" : "", u, tex[u], sz, at);
		shown++;
	}
	if (!bound && (size_t)o < n) o += snprintf(buf + o, n - (size_t)o, " -");
	if (bound > shown && (size_t)o < n) o += snprintf(buf + o, n - (size_t)o, " | +%d more", bound - shown);
	return o;
}

static void fbo_check(void *rctx, const char *where)
{
	unsigned st;
	if (sh_ctx_ok(rctx)) dbg_install_ctx(rctx);   /* once per context, on its first Clear/Draw (a current context exists) */
	if (fbo_logged >= 32 || !sh_ctx_ok(rctx)) return;
	st = _mesa_CheckFramebufferStatus(0x8D40 /* GL_FRAMEBUFFER */);
	if (st == 0x8CD5 /* COMPLETE */ || st == 0) return;
	{
		int fbo = 0, type = 0, name = 0, w = -1, h = -1, ifmt = -1, prev = 0, ch[4] = { -1, -1, -1, -1 }, tex_sizes = 0;
		unsigned k;
		_mesa_GetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING */, &fbo);
		for (k = 0; k < fbo_nseen; k++) if (fbo_seen[k][0] == (unsigned)fbo && fbo_seen[k][1] == st) return;
		if (fbo_nseen < 32) { fbo_seen[fbo_nseen][0] = (unsigned)fbo; fbo_seen[fbo_nseen][1] = st; fbo_nseen++; }
		if (fbo == 0) {   /* default framebuffer: COLOR_ATTACHMENT0 queries would raise INVALID_ENUM in CA's error flag (review gli-fbo-diag F3) */
			fbo_logged++;
			fprintf(stderr, "sweetgl: FBO 0 incomplete in %s: status 0x%x\n", where, st);
			return;
		}
		_mesa_GetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x8CD0 /* OBJECT_TYPE */, &type);
		_mesa_GetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &name);
		if (type == 0x1702 /* GL_TEXTURE */ && name > 0) /* [guess] a RECTANGLE attachment would make this BindTexture raise INVALID_OPERATION (review F1); CA binds no rectangle textures (g72-g77) */ {
			_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &prev);
			_mesa_BindTexture(GLI_TEXTURE_2D, (unsigned)name);
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1000 /* WIDTH */, &w);
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1001 /* HEIGHT */, &h);
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1003 /* INTERNAL_FORMAT */, &ifmt);
			/* channel sizes (research 86 sections E/F/H, DRAFT-format0-batch) */
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x805C /* RED_SIZE */, &ch[0]);
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x805D /* GREEN_SIZE */, &ch[1]);
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x805E /* BLUE_SIZE */, &ch[2]);
			_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x805F /* ALPHA_SIZE */, &ch[3]);
			tex_sizes = 1;
			_mesa_BindTexture(GLI_TEXTURE_2D, (unsigned)prev);
		} else if (type == 0x8D41 /* RENDERBUFFER */ && name > 0) {   /* colour renderbuffer: same size / format query as the depth branch below (research 86 sections E/F/H, DRAFT-format0-batch) */
			int prb = 0;
			_mesa_GetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &prb);
			_mesa_BindRenderbuffer(0x8D41, (unsigned)name);
			_mesa_GetRenderbufferParameteriv(0x8D41, 0x8D42 /* WIDTH */, &w);
			_mesa_GetRenderbufferParameteriv(0x8D41, 0x8D43 /* HEIGHT */, &h);
			_mesa_GetRenderbufferParameteriv(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &ifmt);
			_mesa_BindRenderbuffer(0x8D41, (unsigned)prb);
		}
		fbo_logged++;
		fprintf(stderr, "sweetgl: FBO %d incomplete in %s: status 0x%x color0 type 0x%x name %d level0 %dx%d ifmt 0x%x\n", fbo, where, st, type, name, w, h, (unsigned)ifmt);
		sweet_caller_chain("FBO incomplete fbo/color0-name", fbo, name);   /* sprint GPU 2026-11-11 defect 1: which CA path renders into a texture without storage */
		/* research 86 sections E/F/H, DRAFT-format0-batch: the same facts with the status named, and the channel sizes of a texture attachment */
		{
			char l[256];
			sweet_fbo_status_line(l, sizeof l, fbo, where, st, type, name, w, h, (unsigned)ifmt);
			fprintf(stderr, "%s\n", l);
			if (tex_sizes) { sweet_fbo_sizes_line(l, sizeof l, fbo, ch[0], ch[1], ch[2], ch[3]); fprintf(stderr, "%s\n", l); }
		}
		if (census_on && type == 0x1702 /* GL_TEXTURE */ && name > 0) sweet_th_print((unsigned)name);   /* history of the colour texture (census only) */
		{  /* depth and stencil attachments too: Mesa reports 'depth/stencil attachment incomplete' with the same status 0x8cd6 */
			static const unsigned ap[2] = { 0x8D00 /* DEPTH_ATTACHMENT */, 0x8D20 /* STENCIL_ATTACHMENT */ };
			int a;
			for (a = 0; a < 2; a++) {
				int t2 = 0, n2 = 0, rw = -1, rh = -1, rf = -1, prb = 0;
				_mesa_GetFramebufferAttachmentParameteriv(0x8D40, ap[a], 0x8CD0, &t2);
				if (t2 == 0 /* NONE */) { fprintf(stderr, "sweetgl: FBO %d %s attachment: none\n", fbo, a ? "stencil" : "depth"); continue; }
				_mesa_GetFramebufferAttachmentParameteriv(0x8D40, ap[a], 0x8CD1, &n2);
				if (t2 == 0x8D41 /* RENDERBUFFER */ && n2 > 0) {
					_mesa_GetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &prb);
					_mesa_BindRenderbuffer(0x8D41, (unsigned)n2);
					_mesa_GetRenderbufferParameteriv(0x8D41, 0x8D42 /* WIDTH */, &rw);
					_mesa_GetRenderbufferParameteriv(0x8D41, 0x8D43 /* HEIGHT */, &rh);
					_mesa_GetRenderbufferParameteriv(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &rf);
					_mesa_BindRenderbuffer(0x8D41, (unsigned)prb);
				}
				fprintf(stderr, "sweetgl: FBO %d %s attachment: type 0x%x name %d %dx%d ifmt 0x%x\n", fbo, a ? "stencil" : "depth", t2, n2, rw, rh, (unsigned)rf);
			}
		}
	}
}

static void hook_drawarrays_fbo(void *rctx, unsigned mode, int first, int count)
{
	fbo_check(rctx, "DrawArrays");
	((void (*)(void *, unsigned, int, int))fbo_next_da)(rctx, mode, first, count);
}
static void hook_drawelements_fbo(void *rctx, unsigned mode, int count, unsigned type, const void *indices)
{
	fbo_check(rctx, "DrawElements");
	((void (*)(void *, unsigned, int, unsigned, const void *))fbo_next_de)(rctx, mode, count, type, indices);
}
static void hook_clear_fbo(void *rctx, unsigned mask)
{
	fbo_check(rctx, "Clear");
	((void (*)(void *, unsigned))fbo_next_cl)(rctx, mask);
}

/* SWEETGL_TRACE=1, draw/clear/blit side (research 61, g89/g90: 19 CA 180x180 icon targets stay all-zero and no draw ever targeted one through
 * DrawArrays/DrawElements): for every draw-like call (DrawArrays, DrawElements, DrawRangeElements, DrawArraysInstanced, DrawElementsInstanced, Clear,
 * BlitFramebuffer) log, with two caps: (a) GLI_DRAW_MAX lines when the bound framebuffer's colour attachment is a small texture (180x180 or <= 256 px both
 * ways): program, mode/count, viewport and, for texture units 0..3, the bound 2D texture with its size and last upload nonzero count; (b) GLI_SRC_MAX lines
 * when any of units 0..3 holds a texture that came from a <= 256 px upload or a 180x180 surface attach (whatever the target): program, viewport and the size
 * of the target framebuffer. */
#define GLI_DRAW_MAX 150
#define GLI_SRC_MAX 100
static unsigned draw_trace_n, src_trace_n, src_checks;   /* src_checks: the per-draw texture-unit queries stop after GLI_SRC_CHECKS draws (review: cost) */
#define GLI_SRC_CHECKS 30000
static const void *trc_next_da, *trc_next_de, *trc_next_dre, *trc_next_dai, *trc_next_dei, *trc_next_cl, *trc_next_bl;
static void draw_trace(void *rctx, const char *what, unsigned mode, int count)
{
	int fbo = 0, type = 0, name = 0, cur = 0, act = 0, vp[4] = { 0, 0, 0, 0 }, u, tex[4] = { 0, 0, 0, 0 }, small_src = 0, small_tgt = 0;
	unsigned w = 0, h = 0, tw[4] = { 0, 0, 0, 0 }, th[4] = { 0, 0, 0, 0 };
	int is_draw = strcmp(what, "Clear") && strcmp(what, "Blit");
	if (!trace || (draw_trace_n >= GLI_DRAW_MAX && src_trace_n >= GLI_SRC_MAX) || !sh_ctx_ok(rctx)) return;
	_mesa_GetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING */, &fbo);
	if (fbo != 0) {
		_mesa_GetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD0, &type);
		if (type == 0x1702) {
			_mesa_GetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD1, &name);
			if (tex_size((unsigned)name, &w, &h) && ((w == 180 && h == 180) || (w <= 256 && h <= 256))) small_tgt = 1;
		}
	}
	if (is_draw && src_trace_n < GLI_SRC_MAX && src_checks++ < GLI_SRC_CHECKS) {
		_mesa_GetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &act);
		for (u = 0; u < 4; u++) {
			_mesa_ActiveTexture(0x84C0 + (unsigned)u);
			_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex[u]);
			if (tex[u] > 0) {
				unsigned t = (unsigned)tex[u];
				int have_size = tex_size(t, &tw[u], &th[u]);
				if (!have_size && upt[t % 256].tex == t) { tw[u] = upt[t % 256].w; th[u] = upt[t % 256].h; have_size = 1; }
				if (have_size && ((tw[u] == 180 && th[u] == 180) || (upt[t % 256].tex == t && tw[u] <= 256 && th[u] <= 256))) small_src = 1;
			}
		}
		_mesa_ActiveTexture((unsigned)act);
	}
	if (small_tgt && draw_trace_n < GLI_DRAW_MAX) {
		draw_trace_n++;
		_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
		_mesa_GetIntegerv(0x0BA2 /* VIEWPORT */, vp);
		fprintf(stderr, "sweetgl: %s into fbo %d tex %d %ux%u: program %d mode 0x%x count %d viewport %d,%d %dx%d\n", what, fbo, name, w, h, cur, mode, count, vp[0], vp[1], vp[2], vp[3]);
		for (u = 0; u < 4; u++)
			if (tex[u] > 0) fprintf(stderr, "sweetgl:   unit %d tex %d size %ux%u %s\n", u, tex[u], tw[u], th[u], upt[(unsigned)tex[u] % 256].tex == (unsigned)tex[u] ? "(uploaded)" : "(no upload sampled)");
	}
	if (small_src && src_trace_n < GLI_SRC_MAX) {
		src_trace_n++;
		_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
		_mesa_GetIntegerv(0x0BA2 /* VIEWPORT */, vp);
		fprintf(stderr, "sweetgl: %s samples a small source: program %d viewport %d,%d %dx%d target fbo %d (%ux%u)", what, cur, vp[0], vp[1], vp[2], vp[3], fbo, w, h);
		for (u = 0; u < 4; u++)
			if (tex[u] > 0) fprintf(stderr, " u%d=tex%d(%ux%u%s)", u, tex[u], tw[u], th[u], upt[(unsigned)tex[u] % 256].tex == (unsigned)tex[u] ? " up" : "");
		fputc('\n', stderr);
	}
}

/* Count-gated home-page draw trace (research 61, g91: the lockscreen burned the earlier caps). Under SWEETGL_TRACE=1 every DrawElements/DrawArrays increments a counter;
 *  (A) the first GLI_HOME_NONFULL draws whose viewport OR scissor box is not the full 1125x2436 are logged (the per-icon passes are expected to use a scissor/viewport),
 *  (B) after GLI_HOME_SKIP draws, the next GLI_HOME_WIN draws are logged.
 * Each line: program, bound fbo and its colour texture size, viewport, scissor (+enabled), and for units 0..3 the bound 2D texture with its size and the nonzero
 * sample of its last upload ("up a/b") or "(surface)" if it is an IOSurface attach. Own queries only; temporary BindTexture restored. */
extern unsigned char _mesa_IsEnabled(unsigned);
#define GLI_HOME_SKIP 5000
/* g100: ring of the last GLI_BRING glBindTexture / glActiveTexture calls (trace only), dumped for the unit-1 and name-0 entries at the first GLI_BDUMP SWEETGL_HOME_PROG
 * draws: what does CoreAnimation do with unit 1 before the masked program-45 draw? unit is tracked from our own ActiveTexture hook (process-wide, initial 0). */
#define GLI_BRING 256
#define GLI_BDUMP 6
static struct { void *rctx; unsigned unit, tex, target; int act; } bring[GLI_BRING];
static unsigned bring_n, bring_dumped, bdump_n, bind_unit;
static const void *trc_next_bt, *trc_next_at;
static pthread_mutex_t bring_mu = PTHREAD_MUTEX_INITIALIZER;
static void hook_bindtexture_trc(void *rctx, unsigned target, unsigned tex)
{
	pthread_mutex_lock(&bring_mu);
	bring[bring_n % GLI_BRING].rctx = rctx; bring[bring_n % GLI_BRING].unit = bind_unit; bring[bring_n % GLI_BRING].tex = tex; bring[bring_n % GLI_BRING].target = target; bring[bring_n % GLI_BRING].act = 0;
	bring_n++;
	pthread_mutex_unlock(&bring_mu);
	((void (*)(void *, unsigned, unsigned))trc_next_bt)(rctx, target, tex);
}
static void hook_activetexture_trc(void *rctx, unsigned unit)
{
	pthread_mutex_lock(&bring_mu);
	bind_unit = unit >= 0x84C0 ? unit - 0x84C0 : 99;
	bring[bring_n % GLI_BRING].rctx = rctx; bring[bring_n % GLI_BRING].unit = bind_unit; bring[bring_n % GLI_BRING].tex = 0; bring[bring_n % GLI_BRING].target = 0; bring[bring_n % GLI_BRING].act = 1;
	bring_n++;
	pthread_mutex_unlock(&bring_mu);
	((void (*)(void *, unsigned))trc_next_at)(rctx, unit);
}
static void bring_dump(void *rctx, int prog)
{
	unsigned i, from, shown = 0;
	pthread_mutex_lock(&bring_mu);
	if (bdump_n >= GLI_BDUMP) { pthread_mutex_unlock(&bring_mu); return; }
	bdump_n++;
	from = bring_n > GLI_BRING && bring_n - GLI_BRING > bring_dumped ? bring_n - GLI_BRING : bring_dumped;
	if (from < bring_dumped) from = bring_dumped;
	fprintf(stderr, "sweetgl:   HOME bindlog prog %d: %u calls since the last dump (ring %d), showing unit 1 and name-0 binds, ctx %p\n", prog, bring_n - from, GLI_BRING, rctx);
	for (i = from; i < bring_n; i++) {
		unsigned k = i % GLI_BRING;
		if (bring[k].rctx != rctx) continue;
		if (bring[k].unit != 1 && !(bring[k].tex == 0 && !bring[k].act)) continue;
		if (shown++ >= 40) break;
		if (bring[k].act) fprintf(stderr, "sweetgl:     #%u ActiveTexture unit %u\n", i, bring[k].unit);
		else fprintf(stderr, "sweetgl:     #%u BindTexture unit %u tgt 0x%x tex %u\n", i, bring[k].unit, bring[k].target, bring[k].tex);
	}
	bring_dumped = bring_n;
	pthread_mutex_unlock(&bring_mu);
}
#define GLI_HOME_WIN 150
#define GLI_HOME_NONFULL 60
static unsigned home_draws, home_win_n, home_nonfull_n, home_prog_n;
extern void _mesa_GetActiveUniform(unsigned, unsigned, int, int *, int *, unsigned *, char *);
#define GLI_HOME_PROG_MAX 64

/* Size of a texture whatever its origin: IOSurface attach table, sampled upload, else a level-0 query through a temporary bind (restored). */
static void tex_dims(unsigned t, unsigned *w, unsigned *h)
{
	int qw = 0, qh = 0, prev = 0;
	*w = *h = 0;
	if (!t) return;
	if (tex_size(t, w, h)) return;
	if (upt[t % 256].tex == t) { *w = upt[t % 256].w; *h = upt[t % 256].h; return; }
	_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &prev);
	_mesa_BindTexture(GLI_TEXTURE_2D, t);
	_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1000 /* WIDTH */, &qw);
	_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1001 /* HEIGHT */, &qh);
	_mesa_BindTexture(GLI_TEXTURE_2D, (unsigned)prev);
	while (_mesa_GetError() != 0) {}   /* a rectangle / cube / deleted name makes the bind raise INVALID_OPERATION: do not leave it for CA's glGetError (review gli-home-trace-0160d) */
	*w = qw > 0 ? (unsigned)qw : 0; *h = qh > 0 ? (unsigned)qh : 0;
}

static void home_trace(void *rctx, const char *what, unsigned mode, int count)
{
	unsigned n, window, nonfull_cand;
	int by_prog = 0;
	int vp[4] = { 0, 0, 0, 0 }, sc[4] = { 0, 0, 0, 0 }, fbo = 0, cur = 0, act = 0, u, type = 0, name = 0, scen;
	unsigned tw = 0, th = 0;
	if (!trace) return;
	n = ++home_draws;
	window = n > GLI_HOME_SKIP && home_win_n < GLI_HOME_WIN;
	nonfull_cand = home_nonfull_n < GLI_HOME_NONFULL;
	/* SWEETGL_HOME_PROG=N: also log the first GLI_HOME_PROG_MAX draws of program N, whatever the window (program 45 = CA masked element, g98) */
	if (!window && !nonfull_cand && !(getenv("SWEETGL_HOME_PROG") && home_prog_n < GLI_HOME_PROG_MAX)) return;
	if (!sh_ctx_ok(rctx)) return;
	if (!window && !nonfull_cand) {
		int pc = 0;
		_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &pc);
		if (pc <= 0 || pc != atoi(getenv("SWEETGL_HOME_PROG"))) return;
		home_prog_n++;
		by_prog = 1;
		bring_dump(rctx, pc);
	}
	_mesa_GetIntegerv(0x0BA2 /* VIEWPORT */, vp);
	_mesa_GetIntegerv(0x0C10 /* SCISSOR_BOX */, sc);
	scen = _mesa_IsEnabled(0x0C11 /* SCISSOR_TEST */) ? 1 : 0;
	{
		int full = vp[0] == 0 && vp[1] == 0 && vp[2] == 1125 && vp[3] == 2436 && (!scen || (sc[0] == 0 && sc[1] == 0 && sc[2] == 1125 && sc[3] == 2436));
		int log_nonfull = nonfull_cand && !full;
		if (!window && !log_nonfull && !by_prog) return;
		if (window) home_win_n++;
		if (log_nonfull) home_nonfull_n++;
		_mesa_GetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING */, &fbo);
		_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
		if (fbo) {
			_mesa_GetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD0, &type);
			if (type == 0x1702) { _mesa_GetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD1, &name); tex_dims((unsigned)name, &tw, &th); }
		}
		fprintf(stderr, "sweetgl: HOME #%u %s %s mode 0x%x count %d prog %d fbo %d(tex %d %ux%u) vp %d,%d %dx%d sc%s %d,%d %dx%d\n", n, window ? "win" : by_prog ? "prog" : "nonfull", what, mode, count, cur, fbo, name, tw, th,
		    vp[0], vp[1], vp[2], vp[3], scen ? "" : "(off)", sc[0], sc[1], sc[2], sc[3]);
		_mesa_GetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &act);
		for (u = 0; u < 4; u++) {
			int t = 0;
			unsigned w2, h2;
			_mesa_ActiveTexture(0x84C0 + (unsigned)u);
			_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &t);
			if (t > 0) {
				tex_dims((unsigned)t, &w2, &h2);
				if (upt[(unsigned)t % 256].tex == (unsigned)t) fprintf(stderr, "sweetgl:   HOME u%d tex %d %ux%u up %lu/%lu\n", u, t, w2, h2, upt[(unsigned)t % 256].nz, upt[(unsigned)t % 256].n);
				else fprintf(stderr, "sweetgl:   HOME u%d tex %d %ux%u %s\n", u, t, w2, h2, tex_size((unsigned)t, &w2, &h2) ? "(surface)" : "(no upload sampled)");
			}
		}
		/* every sampler uniform of the program: which unit it points at and what is bound there (an unbound unit samples as 0,0,0,1 = silent black, g98 program 45) */
		{
			int nu = 0, ui;
			if (cur > 0) _mesa_GetProgramiv((unsigned)cur, 0x8B86 /* ACTIVE_UNIFORMS */, &nu);
			for (ui = 0; ui < nu && ui < 64; ui++) {
				char nm[64];
				int len = 0, sz = 0, loc, unit = -1, t = 0;
				unsigned ty = 0;
				nm[0] = 0;
				_mesa_GetActiveUniform((unsigned)cur, (unsigned)ui, (int)sizeof nm, &len, &sz, &ty, nm);
				if (ty != 0x8B5E /* SAMPLER_2D */ && ty != 0x8B60 /* SAMPLER_CUBE */) continue;
				loc = _mesa_GetUniformLocation((unsigned)cur, nm);
				if (loc >= 0) _mesa_GetUniformiv((unsigned)cur, loc, &unit);
				if (unit >= 0 && unit < 16) {
					_mesa_ActiveTexture(0x84C0 + (unsigned)unit);
					_mesa_GetIntegerv(ty == 0x8B60 ? 0x8514 : 0x8069, &t);
				}
				fprintf(stderr, "sweetgl:   HOME sampler %s -> unit %d tex %d%s\n", nm, unit, t, t ? "" : "  UNBOUND");
				if (!t && strncmp(nm, "sweet_", 6)) sweet_gap("draw", "sampler %s samples an unbound texture unit", nm);
			}
		}
		while (_mesa_GetError() != 0) {}   /* ActiveTexture on a missing unit must not leave an error for CA's glGetError (review gli-sampler-trace) */
		_mesa_ActiveTexture((unsigned)act);
	}
}

static void hook_drawarrays_trc(void *rctx, unsigned mode, int first, int count)
{
	draw_trace(rctx, "DrawArrays", mode, count);
	home_trace(rctx, "DrawArrays", mode, count);
	((void (*)(void *, unsigned, int, int))trc_next_da)(rctx, mode, first, count);
}
static void hook_drawelements_trc(void *rctx, unsigned mode, int count, unsigned type, const void *indices)
{
	draw_trace(rctx, "DrawElements", mode, count);
	home_trace(rctx, "DrawElements", mode, count);
	((void (*)(void *, unsigned, int, unsigned, const void *))trc_next_de)(rctx, mode, count, type, indices);
}
static void hook_drawrangeelements_trc(void *rctx, unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void *indices)
{
	draw_trace(rctx, "DrawRangeElements", mode, count);
	((void (*)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *))trc_next_dre)(rctx, mode, start, end, count, type, indices);
}
static void hook_drawarraysinstanced_trc(void *rctx, unsigned mode, int first, int count, int inst)
{
	draw_trace(rctx, "DrawArraysInstanced", mode, count);
	((void (*)(void *, unsigned, int, int, int))trc_next_dai)(rctx, mode, first, count, inst);
}
static void hook_drawelementsinstanced_trc(void *rctx, unsigned mode, int count, unsigned type, const void *indices, int inst)
{
	draw_trace(rctx, "DrawElementsInstanced", mode, count);
	((void (*)(void *, unsigned, int, unsigned, const void *, int))trc_next_dei)(rctx, mode, count, type, indices, inst);
}
static void hook_clear_trc(void *rctx, unsigned mask)
{
	draw_trace(rctx, "Clear", mask, 0);
	((void (*)(void *, unsigned))trc_next_cl)(rctx, mask);
}
static void hook_blit_trc(void *rctx, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter)
{
	draw_trace(rctx, "Blit", mask, 0);
	((void (*)(void *, int, int, int, int, int, int, int, int, unsigned, unsigned))trc_next_bl)(rctx, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

/* SWEETGL_HIST=1 (research 61, "settle which GL paths CA uses"): every GLI slot is replaced by a per-slot arm64 trampoline (gli_tramp_gen.h, generated by
 * gen_tramp.py) that counts the call and branches to the saved original with all argument registers and the stack untouched. A detached thread prints the
 * counts by name every 20 s (top 60 names per dump, 10 per line, 200 lines in total). Slots that are traps (unimplemented) are counted too and show up
 * by name. Not available off arm64/Apple (the host test only checks that the table is untouched without the env var). */
unsigned long sgl_cnt[SWEET_GLI_NSLOTS] __attribute__((visibility("hidden")));
const void *sgl_orig[SWEET_GLI_NSLOTS] __attribute__((visibility("hidden")));
#include "gli_tramp_gen.h"
#ifdef SGL_HAVE_TRAMP
extern const void *const sgl_tramps[SWEET_GLI_NSLOTS];
#endif
static int hist_on;
#ifdef SGL_HAVE_TRAMP
static unsigned hist_lines;

static unsigned long hist_snap[SWEET_GLI_NSLOTS];   /* counts copied once per dump: qsort must not see values another thread is still incrementing (review gli-hist-tramp-0160b) */
static int hist_cmp(const void *a, const void *b)
{
	unsigned long x = hist_snap[*(const unsigned *)a], y = hist_snap[*(const unsigned *)b];
	return x < y ? 1 : (x > y ? -1 : 0);
}

static void hist_dump(unsigned round)
{
	static unsigned idx[SWEET_GLI_NSLOTS];
	unsigned i, n = 0, k, shown;
	char line[1024];
	size_t o;
	for (i = 0; i < SWEET_GLI_NSLOTS; i++) hist_snap[i] = sgl_cnt[i];
	for (i = 0; i < SWEET_GLI_NSLOTS; i++) if (hist_snap[i]) idx[n++] = i;
	qsort(idx, n, sizeof idx[0], hist_cmp);
	shown = n > 60 ? 60 : n;
	for (k = 0; k < shown && hist_lines < 200; k += 10) {
		unsigned j;
		o = (size_t)snprintf(line, sizeof line, "sweetgl: HIST t+%us (%u of %u active)", round * 20, shown, n);
		for (j = k; j < k + 10 && j < shown; j++)
			o += (size_t)snprintf(line + o, sizeof line - o, " %s=%lu", sweet_gli_names[idx[j]], hist_snap[idx[j]]);
		fprintf(stderr, "%s\n", line);
		hist_lines++;
	}
}

static void *hist_thread(void *arg)
{
	unsigned round = 0;
	(void)arg;
	for (;;) {
		sleep(20);
		hist_dump(++round);
		if (hist_lines >= 200) return NULL;
	}
}

#endif

/* research 90 L1/L2 (UNVERIFIED hypotheses H1/H2 there, this only measures): SWEETGL_CENSUS=1 counts draws per shader kind and per target class and logs the
 * first calls of glCopyTexSubImage2D. LOG-ONLY: every hook calls the saved next entry exactly once with the arguments it received and returns its result; the only
 * extra GL calls are glGetIntegerv / glCheckFramebufferStatus / glGetTexLevelParameteriv (no state change); glGetError is NEVER called (CA's own glGetError must see
 * what it saw before), errors come from the KHR_debug sink (fbo_dbg_cb, installed once per context through dbg_install_ctx). Default off: no hook is installed.
 * Shader kinds are tagged on the ORIGINAL text: the census ShaderSource hook is installed after the rect hook so it runs first (the rect rewrite would otherwise
 * remove "sampler2DRect"). shader -> program: glAttachShader (slot 598) ORs the shader's kinds into the program's; glCreateProgram (597) resets the record of a reused
 * id. NOT handled: glDetachShader and a relink with different shaders (guess: CA does neither).
 * S/T/O is classified by the VIEWPORT (glViewport state at draw time, width >= 1000 / both <= 256), NOT by the framebuffer or texture being drawn into (review
 * census-copytex-b2 F4): a pass into a small FBO with a stale full-size viewport counts as S. No per-draw attachment queries are made on purpose (cost). */
#define GLI_SLOT_CREATEPROGRAM 597
#define GLI_SLOT_ATTACHSHADER 598
#define GLI_SLOT_COPYTEXSUBIMAGE2D 56
#define GLI_CEN_TAB 1024
#define GLI_CEN_COPY_LOG 32
#define CEN_BLUR 1u
#define CEN_FBFETCH 2u
#define CEN_CMATRIX 4u
#define CEN_LUM 8u
#define CEN_RECT 16u
extern unsigned _mesa_CheckFramebufferStatus(unsigned);
static const void *cen_next_da, *cen_next_de, *cen_next_dre, *cen_next_dai, *cen_next_dei, *cen_next_ss, *cen_next_cp, *cen_next_as, *cen_next_cts;
static unsigned long cen_cnt[6][3];       /* [kind: blur fbfetch cmatrix lum rect other][class: S T O] */
static unsigned long cen_copytex;
static struct cen_ent { void *rctx; unsigned id, flags; int used; } cen_sh[GLI_CEN_TAB], cen_pr[GLI_CEN_TAB];   /* shader id / program id -> kind flags (ids are per context) */
static pthread_mutex_t cen_mu = PTHREAD_MUTEX_INITIALIZER;

static int cen_has(const char *s, size_t n, const char *needle)
{
	size_t m = strlen(needle), i;
	if (m > n) return 0;
	for (i = 0; i + m <= n; i++) if (s[i] == needle[0] && memcmp(s + i, needle, m) == 0) return 1;
	return 0;
}

/* kind flags of a shader text (research 90 L1): BLUR "weight", FBFETCH "gl_LastFragData", CMATRIX "color_matrix", LUM "tlum" or "luminanceCurve", RECT "sampler2DRect" */
unsigned sweet_census_kinds(const char *s, size_t n)
{
	unsigned f = 0;
	if (!s) return 0;
	if (cen_has(s, n, "weight")) f |= CEN_BLUR;
	if (cen_has(s, n, "gl_LastFragData")) f |= CEN_FBFETCH;
	if (cen_has(s, n, "color_matrix")) f |= CEN_CMATRIX;
	if (cen_has(s, n, "tlum") || cen_has(s, n, "luminanceCurve")) f |= CEN_LUM;
	if (cen_has(s, n, "sampler2DRect")) f |= CEN_RECT;
	return f;
}

/* target class of a viewport: 0 = S (w >= 1000), 1 = T (w <= 256 and h <= 256), 2 = O (other) */
int sweet_census_class(int w, int h)
{
	if (w >= 1000) return 0;
	if (w <= 256 && h <= 256) return 1;
	return 2;
}

/* counts one draw: once in every kind of `flags`, in `other` when none (kinds are not exclusive) */
void sweet_census_count(unsigned flags, int w, int h)
{
	int c = sweet_census_class(w, h), k, any = 0;
	for (k = 0; k < 5; k++)
		if (flags & (1u << k)) { __atomic_fetch_add(&cen_cnt[k][c], 1, __ATOMIC_RELAXED); any = 1; }
	if (!any) __atomic_fetch_add(&cen_cnt[5][c], 1, __ATOMIC_RELAXED);
}

void sweet_census_snapshot(unsigned long c[6][3], unsigned long *copytex)
{
	int k, j;
	for (k = 0; k < 6; k++) for (j = 0; j < 3; j++) c[k][j] = __atomic_load_n(&cen_cnt[k][j], __ATOMIC_RELAXED);
	*copytex = __atomic_load_n(&cen_copytex, __ATOMIC_RELAXED);
}

int sweet_census_line(char *buf, size_t n, unsigned secs, const unsigned long c[6][3], unsigned long copytex)
{
	static const char *const nm[6] = { "blur", "fbfetch", "cmatrix", "lum", "rect", "other" };
	size_t o = (size_t)snprintf(buf, n, "SWEETGL: BDROP census t+%us", secs);
	int k;
	for (k = 0; k < 6 && o < n; k++)
		o += (size_t)snprintf(buf + o, n - o, " %s S/T/O %lu/%lu/%lu", nm[k], c[k][0], c[k][1], c[k][2]);
	if (o < n) o += (size_t)snprintf(buf + o, n - o, " copytex %lu", copytex);
	return (int)o;
}

int sweet_copytex_line(char *buf, size_t n, unsigned idx, unsigned target, int level, int xoff, int yoff, int x, int y, int w, int h,
    int readfb, int has_status, unsigned status, int has_dst, int dst, int dw, int dh, unsigned ifmt)
{
	size_t o = (size_t)snprintf(buf, n, "SWEETGL: copytex #%u target 0x%x level %d xoff %d yoff %d x %d y %d w %d h %d readfb %d readstatus ", idx, target, level, xoff, yoff, x, y, w, h, readfb);
	if (o < n) o += has_status ? (size_t)snprintf(buf + o, n - o, "0x%x", status) : (size_t)snprintf(buf + o, n - o, "n/a");
	if (o < n) o += has_dst ? (size_t)snprintf(buf + o, n - o, " dst %d %dx%d ifmt 0x%x", dst, dw, dh, ifmt) : (size_t)snprintf(buf + o, n - o, " dst n/a");
	return (int)o;
}

/* ---- census extension (log-only, SWEETGL_CENSUS=1): counters of other calls per target class, pixel readback stats, draw sequences ----
 * The pure parts below are exported for the host test; the hooks are further down (census section). */
static unsigned long cen_cnt2[5][3];   /* [blit readpix genmip fbotex copyteximg][class S T O], class by the viewport at call time */

void sweet_census_count2(int which, int w, int h)
{
	if (which < 0 || which > 4) return;
	__atomic_fetch_add(&cen_cnt2[which][sweet_census_class(w, h)], 1, __ATOMIC_RELAXED);
}

static unsigned ds_started;   /* sequences armed so far, 0..3 (the census line prints it) */

void sweet_census_snapshot2(unsigned long c2[5][3], unsigned *drawseq)
{
	int k, j;
	for (k = 0; k < 5; k++) for (j = 0; j < 3; j++) c2[k][j] = __atomic_load_n(&cen_cnt2[k][j], __ATOMIC_RELAXED);
	*drawseq = __atomic_load_n(&ds_started, __ATOMIC_RELAXED);
}

/* " blit S/T/O a/b/c readpix .. genmip .. fbotex .. drawseq N copyteximg .." (copyteximg is appended last so the requested order stays a prefix) */
int sweet_census_extra(char *buf, size_t n, const unsigned long c2[5][3], unsigned drawseq)
{
	static const char *const nm[5] = { "blit", "readpix", "genmip", "fbotex", "copyteximg" };
	size_t o = 0;
	int k;
	buf[0] = 0;
	for (k = 0; k < 4 && o < n; k++)
		o += (size_t)snprintf(buf + o, n - o, " %s S/T/O %lu/%lu/%lu", nm[k], c2[k][0], c2[k][1], c2[k][2]);
	if (o < n) o += (size_t)snprintf(buf + o, n - o, " drawseq %u", drawseq);
	if (o < n) o += (size_t)snprintf(buf + o, n - o, " %s S/T/O %lu/%lu/%lu", nm[4], c2[4][0], c2[4][1], c2[4][2]);
	return (int)o;
}

int sweet_census_line_full(char *buf, size_t n, unsigned secs, const unsigned long c[6][3], unsigned long copytex, const unsigned long c2[5][3], unsigned drawseq)
{
	int o = sweet_census_line(buf, n, secs, c, copytex);
	if (o < 0 || (size_t)o >= n) return o;
	return o + sweet_census_extra(buf + o, n - (size_t)o, c2, drawseq);
}

/* 4x4 RGBA8 patch: a pixel is nonzero when any of its 4 bytes is; maxa = largest alpha byte; sum = sum of all 64 bytes */
void sweet_px_stats(const unsigned char *b64, unsigned *nonzero, unsigned *maxa, unsigned *sum)
{
	unsigned i, nz = 0, ma = 0, sm = 0;
	for (i = 0; i < 16; i++) {
		const unsigned char *p = b64 + 4 * i;
		if (p[0] | p[1] | p[2] | p[3]) nz++;
		if (p[3] > ma) ma = p[3];
		sm += (unsigned)p[0] + p[1] + p[2] + p[3];
	}
	*nonzero = nz; *maxa = ma; *sum = sm;
}

/* 1 when the 64-byte patch is not entirely the 0xA5 sentinel the buffer was pre-filled with (a read that wrote nothing leaves all 64). A partly clipped patch keeps sentinel bytes inside the
 * counts; a genuine all-0xA5 patch is reported as not written (accepted: guess, vanishingly rare). */
int sweet_px_written(const unsigned char *b64)
{
	int i;
	for (i = 0; i < 64; i++) if (b64[i] != 0xA5) return 1;
	return 0;
}

int sweet_px_line(char *buf, size_t n, const char *kind, int cls, int fbo, int vw, int vh, const unsigned char *b64)
{
	unsigned nz, ma, sm;
	char head[160];
	snprintf(head, sizeof head, "SWEETGL: PIXELS %s %c fbo %d vp %dx%d", kind, "STO"[(cls >= 0 && cls <= 2) ? cls : 2], fbo, vw, vh);
	if (!sweet_px_written(b64)) return snprintf(buf, n, "%s written 0", head);
	sweet_px_stats(b64, &nz, &ma, &sm);
	return snprintf(buf, n, "%s nonzero %u of 16 maxa 0x%x sum 0x%x written 1", head, nz, ma, sm);
}

/* sampling budget: blur (T class) first 4 per distinct bound draw FBO name, 16 total; fbfetch (S class) 4 total, >= 5 s apart. Attempts are counted, not successes.
 * These are the EARLY samples (round 0), kept as they were (cheap: one claim under a mutex). Time-armed rounds (SWEETGL_CENSUS=<N>, N >= 2) come on top, see below. */
#define GLI_PX_BLUR_PER_FBO 4
#define GLI_PX_BLUR_TOTAL 16
#define GLI_PX_FBF_TOTAL 4
#define GLI_PX_FBF_GAP_S 5
/* rounds (finding runplan gputest113-pixels-seq: all 18 early samples fell on the lock screen). A round = the next 4 blur-T draws (distinct FBO; a repeated FBO only after
 * GLI_PXR_REPEAT_S into the round, so a one-FBO scene still gets its samples) and the next 2 fbfetch-S draws after the round start. Rounds start >= N s after the census init,
 * then GLI_PXR_GAP_S after the previous round completed or timed out (timeout GLI_PXR_TIMEOUT_S), at most GLI_PXR_ROUNDS rounds / GLI_PXR_READS readbacks per process.
 * Clock: CLOCK_MONOTONIC seconds (px_clock; the host test injects one). The round start / timeout is evaluated at draw time and by the 20 s census thread tick. */
#define GLI_PXR_BLUR 4
#define GLI_PXR_FBF 2
#define GLI_PXR_TIMEOUT_S 20
#define GLI_PXR_GAP_S 60
#define GLI_PXR_REPEAT_S 10
#define GLI_PXR_ROUNDS 16
#define GLI_PXR_READS 96
static pthread_mutex_t px_mu = PTHREAD_MUTEX_INITIALIZER;
static int px_fbo[GLI_PX_BLUR_TOTAL];
static unsigned px_fbo_n[GLI_PX_BLUR_TOTAL], px_nfbo, px_blur_total, px_fbf_total;
static long long px_fbf_last;
static unsigned long px_samples, px_skip_r[5], px_quadsmall;   /* reasons: pbo fbo rb align small; quadsmall = samples whose four quadrant patches were all skipped (viewport < 8x8) */
static long long (*px_clock_fn)(void);
static int px_sched, px_configured, px_r_active, px_cap_done;
static long long px_next_start, px_r_start;
static unsigned px_r_k, px_r_blur, px_r_fbf, px_r_nfbo, px_r_total;
static int px_r_fbo[GLI_PXR_BLUR];
#define GLI_PXU_PER_ROUND 2
#define GLI_PXU_TOTAL 24
static unsigned px_unb_round, px_unb_n, px_unb_total;   /* unbind samples (part B): attributed round, taken in it, total */
static int px_unb_cap_done;
static unsigned long px_unb_skip;   /* unbind samples given back by the pre-read gates */

static long long px_now(void)
{
	struct timespec ts;
	if (px_clock_fn) return px_clock_fn();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec;
}
void sweet_px_set_clock(long long (*fn)(void)) { px_clock_fn = fn; }
long long sweet_px_now(void) { return px_now(); }

/* SWEETGL_CENSUS value (strtol: leading white space / '+' are accepted, so " 12" arms at 12 s; "1 " and "1x" keep the old rule e[0]=='1' = on, no rounds; pinned in the test): returns census on/off; *arm_s = 0 (today's behaviour: early samples only) or N >= 2 (rounds armed N s after init). "1" and any other value starting
 * with '1' that is not a number >= 2 keep the old meaning (on, no rounds); "0", "", unset, "x" are off. N is capped at 1000000. */
int sweet_census_parse(const char *e, long *arm_s)
{
	char *end;
	long v;
	*arm_s = 0;
	if (!e || !e[0]) return 0;
	v = strtol(e, &end, 10);
	if (end != e && *end == 0 && v >= 2) { *arm_s = v > 1000000 ? 1000000 : v; return 1; }
	return e[0] == '1';
}

/* first call with arm_s >= 2 arms the schedule for the process (sweet_gli_init runs once per context: later calls must not re-start the clock) */
void sweet_px_configure(long arm_s)
{
	int first = 0;
	pthread_mutex_lock(&px_mu);
	if (arm_s >= 2 && !px_configured) { px_configured = 1; px_next_start = px_now() + arm_s; __atomic_store_n(&px_sched, 1, __ATOMIC_RELAXED); first = 1; }
	pthread_mutex_unlock(&px_mu);
	if (first) fprintf(stderr, "SWEETGL: PIXELS rounds armed at %ld s (clock monotonic)\n", arm_s);   /* review F2: the first hardware run proves the schedule started */
}
int sweet_px_sched_on(void) { return __atomic_load_n(&px_sched, __ATOMIC_RELAXED); }

int sweet_px_claim(int fbfetch, int fbo, long long now_s)
{
	int ok = 0;
	unsigned i;
	pthread_mutex_lock(&px_mu);
	if (fbfetch) {
		if (px_fbf_total < GLI_PX_FBF_TOTAL && (px_fbf_total == 0 || now_s - px_fbf_last >= GLI_PX_FBF_GAP_S)) { px_fbf_total++; px_fbf_last = now_s; ok = 1; }
	} else if (px_blur_total < GLI_PX_BLUR_TOTAL) {
		for (i = 0; i < px_nfbo && px_fbo[i] != fbo; i++) ;
		if (i == px_nfbo && px_nfbo < GLI_PX_BLUR_TOTAL) { px_fbo[i] = fbo; px_fbo_n[i] = 0; px_nfbo++; }
		if (i < px_nfbo && px_fbo_n[i] < GLI_PX_BLUR_PER_FBO) { px_fbo_n[i]++; px_blur_total++; ok = 1; }
	}
	pthread_mutex_unlock(&px_mu);
	return ok;
}

static int px_exhausted_locked(void) { return px_r_k >= GLI_PXR_ROUNDS || px_r_total >= GLI_PXR_READS; }
/* ends the active round at end_s; returns 1 once, when this was the last round the caps allow (the caller prints the cap line) */
static int px_finish_locked(long long end_s)
{
	px_r_active = 0;
	px_next_start = end_s + GLI_PXR_GAP_S;
	if (px_exhausted_locked() && !px_cap_done) { px_cap_done = 1; return 1; }
	return 0;
}
static void px_expire_locked(long long now_s, struct sweet_px_rinfo *r)
{
	if (px_r_active && now_s - px_r_start >= GLI_PXR_TIMEOUT_S) {
		r->exp_round = (int)px_r_k; r->exp_samples = px_r_blur + px_r_fbf;
		r->cap_e = px_finish_locked(px_r_start + GLI_PXR_TIMEOUT_S);
	}
}
int sweet_px_round_claim(int fbfetch, int fbo, long long now_s, struct sweet_px_rinfo *r)
{
	unsigned i;
	int ok = 0;
	memset(r, 0, sizeof *r);
	if (!__atomic_load_n(&px_sched, __ATOMIC_RELAXED)) return 0;
	pthread_mutex_lock(&px_mu);
	px_expire_locked(now_s, r);
	if (!px_r_active && !px_exhausted_locked() && now_s >= px_next_start) {
		px_r_active = 1; px_r_k++; px_r_start = now_s; px_r_blur = px_r_fbf = px_r_nfbo = 0;
	}
	if (px_r_active) {
		if (fbfetch) {
			if (px_r_fbf < GLI_PXR_FBF) { px_r_fbf++; ok = 1; }
		} else if (px_r_blur < GLI_PXR_BLUR) {
			for (i = 0; i < px_r_nfbo && px_r_fbo[i] != fbo; i++) ;
			if (i == px_r_nfbo || now_s - px_r_start >= GLI_PXR_REPEAT_S) {
				if (i == px_r_nfbo && px_r_nfbo < GLI_PXR_BLUR) px_r_fbo[px_r_nfbo++] = fbo;
				px_r_blur++; ok = 1;
			}
		}
		if (ok) {
			px_r_total++;
			r->round = (int)px_r_k;
			if (px_r_blur >= GLI_PXR_BLUR && px_r_fbf >= GLI_PXR_FBF) { r->complete = 1; r->csamples = px_r_blur + px_r_fbf; r->cap_c = px_finish_locked(now_s); }
		}
	}
	pthread_mutex_unlock(&px_mu);
	return ok;
}
void sweet_px_round_tick(long long now_s, struct sweet_px_rinfo *r)
{
	memset(r, 0, sizeof *r);
	if (!__atomic_load_n(&px_sched, __ATOMIC_RELAXED)) return;
	pthread_mutex_lock(&px_mu);
	px_expire_locked(now_s, r);
	pthread_mutex_unlock(&px_mu);
}
/* ---- draw state line (part A of the g113d task; armed mode only) ----
 * Every pname below was checked against Mesa 26.2.5 [read from source, Mesa 26.2.5 src/mesa/main]:
 * glGetIntegerv -> find_value (get.c:1643-1700) raises GL_INVALID_ENUM only when the hash walk for the pname ends at index 0, and the GLES3 hash table gets every GLES2 entry
 * (get_hash_generator.py:170-174). Entries, all in the descriptor group apis [GL, GLES, GLES2, GL_CORE] (get_hash_params.py:2) with NO_EXTRA (no extension check, so no error path):
 * BLEND (:4, GL_BLEND), COLOR_WRITEMASK (:7, TYPE_INT_4, get.c:791), DEPTH_TEST (:13), SCISSOR_BOX (:28, get.c:889), SCISSOR_TEST (:29, get.c:896), VIEWPORT (:42, get.c:913),
 * BLEND_SRC_RGB / DST_RGB / SRC_ALPHA / DST_ALPHA (:59-62); BLEND_EQUATION_RGB is the same enum value 0x8009 as GL_BLEND_EQUATION (:66; include/GLES3/gl3.h:87-88) and
 * BLEND_EQUATION_ALPHA is 0x883D = BLEND_EQUATION_ALPHA_EXT (:67; gl3.h:89). CURRENT_PROGRAM is in the group [GL, GL_CORE, GLES2] (get_hash_params.py:277, entry :323, get.c:1130).
 * None was dropped. The values read are state AFTER the draw call (the sample runs after the original draw); the draw state is not modified by the queries. */
int sweet_census_kind_str(unsigned flags, char *buf, size_t n)
{
	int o = 0;
	if (n < 7) return 0;
	if (flags & CEN_BLUR) buf[o++] = 'b';
	if (flags & CEN_FBFETCH) buf[o++] = 'f';
	if (flags & CEN_CMATRIX) buf[o++] = 'c';
	if (flags & CEN_LUM) buf[o++] = 'l';
	if (flags & CEN_RECT) buf[o++] = 'r';
	if (!o) buf[o++] = 'o';
	buf[o] = 0;
	return o;
}
int sweet_state_line(char *buf, size_t n, int round, int fbo, const struct sweet_px_state *s)
{
	char kind[8];
	sweet_census_kind_str(s->flags, kind, sizeof kind);
	return snprintf(buf, n, "SWEETGL: PIXELS state r%d fbo %d prog %d kind %s mode 0x%x count %d blend %d 0x%x/0x%x/0x%x/0x%x eq 0x%x/0x%x mask %d%d%d%d scissor %d %d,%d,%d,%d vp %d,%d,%d,%d depth %d",
	    round, fbo, s->prog, kind, s->mode, s->count, s->blend ? 1 : 0, (unsigned)s->bf[0], (unsigned)s->bf[1], (unsigned)s->bf[2], (unsigned)s->bf[3], (unsigned)s->eq[0], (unsigned)s->eq[1],
	    s->mask[0] ? 1 : 0, s->mask[1] ? 1 : 0, s->mask[2] ? 1 : 0, s->mask[3] ? 1 : 0, s->scissor ? 1 : 0, s->sbox[0], s->sbox[1], s->sbox[2], s->sbox[3],
	    s->vp[0], s->vp[1], s->vp[2], s->vp[3], s->depth ? 1 : 0);
}

/* ---- grid map (unbind samples): the viewport rect divided into 8x8 cells; cell (cx,cy) = x in [cx*w/8, (cx+1)*w/8), y in [cy*h/8, (cy+1)*h/8) (integer floor division, the last cell ends at w / h;
 * needs w, h >= 8 so every cell has >= 1 pixel). Row 0 = the first row glReadPixels returns = y at the viewport origin (GL bottom-left origin, NOT flipped: same orientation as the
 * quad a/b patches). '.' every pixel zero, '#' every pixel non-zero, '+' mixed; a pixel is non-zero when any of its 4 bytes is. `stride` = bytes per row (PACK_ALIGNMENT padding).
 * Returns the number of cells that are not '.', or -1 when w < 8 or h < 8. */
int sweet_grid_classify(const unsigned char *b, size_t stride, int w, int h, char map[8][9])
{
	int cx, cy, nz = 0;
	if (w < 8 || h < 8) return -1;
	for (cy = 0; cy < 8; cy++) {
		int y0 = cy * h / 8, y1 = (cy + 1) * h / 8;
		for (cx = 0; cx < 8; cx++) {
			int x0 = cx * w / 8, x1 = (cx + 1) * w / 8, x, y;
			unsigned zero = 0, non = 0, unw = 0, tot = 0;
			for (y = y0; y < y1; y++)
				for (x = x0; x < x1; x++) {
					const unsigned char *p = b + (size_t)y * stride + (size_t)x * 4;
					tot++;
					if (p[0] == 0xA5 && p[1] == 0xA5 && p[2] == 0xA5 && p[3] == 0xA5) unw++;   /* still the pre-fill: the read did not write it (clipped) */
					else if (p[0] | p[1] | p[2] | p[3]) non++; else zero++;
				}
			/* review state-grid-capdraws-b6 F1: pre-fill pixels are neither zero nor non-zero; a cell of ONLY pre-fill pixels is '?'; in a mixed cell they are ignored. A genuine 0xA5A5A5A5 pixel
			 * is indistinguishable from the pre-fill (documented limit; the line's `a5 N` count shows how many pixels were affected). */
			map[cy][cx] = unw == tot ? '?' : non == 0 ? '.' : zero == 0 ? '#' : '+';
			if (non) nz++;
		}
		map[cy][8] = 0;
	}
	return nz;
}
/* number of pixels that are exactly 0xA5A5A5A5 (the pre-fill: clipped away by the read, or a genuine such pixel); padding ignored */
unsigned sweet_grid_sentinel(const unsigned char *b, size_t stride, int w, int h)
{
	unsigned n = 0;
	int x, y;
	for (y = 0; y < h; y++) for (x = 0; x < w; x++) { const unsigned char *p = b + (size_t)y * stride + (size_t)x * 4; if (p[0] == 0xA5 && p[1] == 0xA5 && p[2] == 0xA5 && p[3] == 0xA5) n++; }
	return n;
}
/* 1 when not every pixel byte still is the 0xA5 pre-fill (padding bytes ignored) */
int sweet_grid_written(const unsigned char *b, size_t stride, int w, int h)
{
	int x, y;
	for (y = 0; y < h; y++) for (x = 0; x < w * 4; x++) if (b[(size_t)y * stride + (size_t)x] != 0xA5) return 1;
	return 0;
}
int sweet_grid_line(char *buf, size_t n, int round, int fbo, int w, int h, int written, int nz, char map[8][9], unsigned a5)
{
	if (!written) return snprintf(buf, n, "SWEETGL: PIXELS grid r%d fbo %d vp %dx%d written 0", round, fbo, w, h);
	return snprintf(buf, n, "SWEETGL: PIXELS grid r%d fbo %d vp %dx%d cells %d/64 map %s|%s|%s|%s|%s|%s|%s|%s a5 %u", round, fbo, w, h, nz, map[0], map[1], map[2], map[3], map[4], map[5], map[6], map[7], a5);
}
int sweet_capdraws_line(char *buf, size_t n, int round, int fbo, const struct sweet_capdraws *c)
{
	return snprintf(buf, n, "SWEETGL: PIXELS capdraws r%d fbo %d draws %u kinds b:%u f:%u c:%u l:%u r:%u o:%u verts %lld progs %d..%d blendlast %d since-bind", round, fbo, c->draws,
	    c->kinds[0], c->kinds[1], c->kinds[2], c->kinds[3], c->kinds[4], c->kinds[5], c->verts, c->pfirst, c->plast, c->blendlast ? 1 : 0);
}
int sweet_capvp_line(char *buf, size_t n, int round, int fbo, const struct sweet_capdraws *c)
{
	return snprintf(buf, n, "SWEETGL: PIXELS capvp r%d fbo %d first vp %d,%d,%d,%d mode 0x%x count %d last vp %d,%d,%d,%d mode 0x%x count %d", round, fbo,
	                c->fvp[0], c->fvp[1], c->fvp[2], c->fvp[3], c->fmode, c->fcount, c->lvp[0], c->lvp[1], c->lvp[2], c->lvp[3], c->lmode, c->lcount);
}
/* capv8 lines. All-or-nothing appender: a fragment that does not fit below `room` is dropped and the line is marked ` ~cut` (5 bytes reserved). Every line is < SWEET_CAPV_MAX bytes: the log tee splits
 * longer ones (~256 B, observed g113g). */
struct capv_ap { char *b; size_t o; int cut; };
static void capv_add(struct capv_ap *a, size_t room, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void capv_add(struct capv_ap *a, size_t room, const char *fmt, ...)
{
	char t[320];
	va_list ap;
	int l;
	if (a->cut) return;
	va_start(ap, fmt); l = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
	if (l < 0 || (size_t)l >= sizeof t || a->o + (size_t)l > room) { a->cut = 1; return; }
	memcpy(a->b + a->o, t, (size_t)l + 1); a->o += (size_t)l;
}
static void capv_floats(char *f, size_t n, const float *x, int nf)
{
	int k, o = 0;
	f[0] = 0;
	for (k = 0; k < nf && (size_t)o < n; k++) o += snprintf(f + o, n - (size_t)o, "%s%g", k ? "," : "", (double)x[k]);
}
int sweet_capv_line(char *buf, size_t n, int fbo, int idx, const struct sweet_capv *v, char tag, int k)
{
	struct capv_ap a;
	size_t lim = n < SWEET_CAPV_MAX ? n : SWEET_CAPV_MAX, top, body;
	int i;
	if (!n) return 0;
	buf[0] = 0; a.b = buf; a.o = 0; a.cut = 0;
	if (lim < 8) return 0;
	top = lim - 1;                                  /* room for the NUL */
	body = top - 5;                                 /* reserve ` ~cut` */
	if ((tag == 'a' && (k < 0 || k > 1)) || ((tag == 'h' || tag == 'v') && k != 0 && !(tag == 'v' && k == 1 && v->vstate == SWEET_CAPV_V_VALUES && v->nv > 2)) || tag == 0) return 0;
	if (tag != 'h' && tag != 'a' && tag != 'v' && tag != 'u') return 0;
	if (tag == 'u' && k < 0) return 0;
#define CAPV_PRE() capv_add(&a, body, "SWEETGL: PIXELS capv8 fbo %d vp %dx%d draw %d", fbo, v->vw, v->vh, idx)
	if (tag == 'h') {
		char first[16], ix[16];
		int any = 0;
		if (v->first >= 0) snprintf(first, sizeof first, "%d", v->first); else snprintf(first, sizeof first, "-");
		if (v->idxtype) snprintf(ix, sizeof ix, "0x%x", v->idxtype); else snprintf(ix, sizeof ix, "-");
		CAPV_PRE();
		capv_add(&a, body, " prog %d mode 0x%x count %d first %s idx %s", v->prog, v->mode, v->count, first, ix);
		capv_add(&a, body, " | tex");
		for (i = 0; i < 8; i++) if (v->tex[i]) { capv_add(&a, body, " u%d %u", i, v->tex[i]); any = 1; }
		if (!any) capv_add(&a, body, " -");
	} else if (tag == 'a') {
		CAPV_PRE();
		capv_add(&a, body, " a");
		for (i = 2 * k; i < 2 * k + 2; i++) {
			char off[24];
			if (v->at[i].buf > 0) snprintf(off, sizeof off, "%lu", v->at[i].off); else snprintf(off, sizeof off, "cli");   /* never a client pointer */
			capv_add(&a, body, "%s a%d en %d buf %d sz %d ty 0x%x st %d off %s", i & 1 ? "," : "", i, v->at[i].en, v->at[i].buf, v->at[i].sz, v->at[i].ty, v->at[i].st, off);
		}
	} else if (tag == 'v') {
		CAPV_PRE();
		capv_add(&a, body, " v");
		if (k == 0 && v->hasidx) {   /* the computed index header is printed before the values or the reason word (idxrange, oob, ...) */
			capv_add(&a, body, " idx");
			for (i = 0; i < v->nidx && i < 4; i++) capv_add(&a, body, "%s%u", i ? "," : " ", v->idx[i]);
			capv_add(&a, body, " min %u max %u", v->imin, v->imax);
		}
		if (v->vstate == SWEET_CAPV_V_VALUES && v->nv > 0) {
			for (i = 2 * k; i < v->nv && i < 2 * k + 2 && i < 4; i++) {
				char f[96];
				capv_floats(f, sizeof f, v->v[i], v->vsz < 4 ? v->vsz : 4);
				capv_add(&a, body, " v%d %s", i, f);
			}
		} else {
			static const char *const w[] = { "-", "-", "oob", "mapped", "idxbuf", "idxrange", "cli0", "type", "dis", "nofirst", "idxq", "inst" };
			unsigned st = (unsigned)v->vstate;
			capv_add(&a, body, " %s", st < sizeof w / sizeof w[0] ? w[st] : "-");
		}
	} else {   /* 'u': tokens (a float uniform is chunked by 4 floats so a mat4 never makes a long token), packed greedily; k-th line */
		char tk[2 * SWEET_CAPV_U * 4 + 2][96];
		int nt = 0, ln = 0, started = 0;
		size_t used, plen;
		char pre[128];
		if (v->nu <= 0) { if (k != 0) return 0; snprintf(tk[nt++], sizeof tk[0], "-"); }
		for (i = 0; i < v->nu && i < SWEET_CAPV_U; i++) {
			if (v->u[i].kind == SWEET_CAPV_U_FLOATS) {
				int c, nf = v->u[i].nf < 16 ? v->u[i].nf : 16;
				for (c = 0; c < (nf > 0 ? nf : 1); c += 4) {
					char f[64];
					capv_floats(f, sizeof f, v->u[i].f + c, nf - c < 4 ? (nf - c > 0 ? nf - c : 0) : 4);
					if (c == 0) snprintf(tk[nt++], sizeof tk[0], "%.15s=%s", v->u[i].name, f);
					else snprintf(tk[nt++], sizeof tk[0], "%.15s.%d=%s", v->u[i].name, c, f);
				}
			} else if (v->u[i].kind == SWEET_CAPV_U_UNIT) snprintf(tk[nt++], sizeof tk[0], "%.15s=unit%d", v->u[i].name, v->u[i].unit);
			else if (v->u[i].kind == SWEET_CAPV_U_TYPE) snprintf(tk[nt++], sizeof tk[0], "%.15s=t0x%x", v->u[i].name, v->u[i].type);
			else snprintf(tk[nt++], sizeof tk[0], "%.15s=-", v->u[i].name);
		}
		if (v->umore > 0) snprintf(tk[nt++], sizeof tk[0], "+%d", v->umore);
		plen = (size_t)snprintf(pre, sizeof pre, "SWEETGL: PIXELS capv8 fbo %d vp %dx%d draw %d u", fbo, v->vw, v->vh, idx);
		used = plen;
		for (i = 0; i < nt; i++) {
			size_t l = strlen(tk[i]);
			if (used + 1 + l > body && used > plen) { ln++; used = plen; }
			if (ln > k) break;
			if (ln == k) {
				if (!started) { capv_add(&a, body, "%s", pre); started = 1; }
				capv_add(&a, body, " %s", tk[i]);
			}
			used += 1 + l;
		}
		if (!started) return 0;
	}
#undef CAPV_PRE
	if (a.cut) { a.cut = 0; capv_add(&a, top, " ~cut"); }
	return (int)a.o;
}
int sweet_capv_cap_line(char *buf, size_t n)
{
	return snprintf(buf, n, "SWEETGL: PIXELS capv8 cap");
}
/* grid budget (review state-grid-capdraws-b6 F3): 1 per unbind sample, GLI_PXG_TOTAL per process and at most GLI_PXG_PER_FBO per FBO name (so later unbinds of the same capture FBO still get a map
 * while other FBOs cannot starve it); the claim that reaches the total sets *cap (caller prints the loud line once). F2: size skips print `grid skip` once per FBO name, max GLI_PXG_SKIPLINES. */
#define GLI_PXG_TOTAL 16
#define GLI_PXG_PER_FBO 3
#define GLI_PXG_SKIPLINES 8
#define GLI_PXG_MAX_BYTES (16u * 1024u * 1024u)
static unsigned px_grid_n; static unsigned long px_grid_skip; static int px_grid_cap_done;
static int px_gf_name[GLI_PXG_TOTAL]; static unsigned px_gf_cnt[GLI_PXG_TOTAL], px_gf_n;   /* grids claimed per FBO name */
static int px_gs_name[GLI_PXG_SKIPLINES]; static unsigned px_gs_n;                          /* FBO names that have had their skip line */
static unsigned px_gf_count_locked(int fbo) { unsigned i; for (i = 0; i < px_gf_n; i++) if (px_gf_name[i] == fbo) return px_gf_cnt[i]; return 0; }
/* 1 when a grid may be taken now; 0 when the process total is spent (the cap line was printed) or this FBO name has its GLI_PXG_PER_FBO grids (counted in gridskip) */
int sweet_px_grid_avail(int fbo)
{
	int r = 0;
	pthread_mutex_lock(&px_mu);
	if (px_grid_n < GLI_PXG_TOTAL) { r = px_gf_count_locked(fbo) < GLI_PXG_PER_FBO; if (!r) px_grid_skip++; }
	pthread_mutex_unlock(&px_mu);
	return r;
}
int sweet_px_grid_claim(int fbo, int *cap)
{
	int ok = 0;
	unsigned i;
	*cap = 0;
	pthread_mutex_lock(&px_mu);
	if (px_grid_n < GLI_PXG_TOTAL && px_gf_count_locked(fbo) < GLI_PXG_PER_FBO) {
		for (i = 0; i < px_gf_n && px_gf_name[i] != fbo; i++) {}
		if (i == px_gf_n) { px_gf_name[i] = fbo; px_gf_cnt[i] = 0; px_gf_n++; }   /* px_gf_n <= px_grid_n < GLI_PXG_TOTAL here */
		px_gf_cnt[i]++; px_grid_n++; ok = 1;
		if (px_grid_n >= GLI_PXG_TOTAL && !px_grid_cap_done) { px_grid_cap_done = 1; *cap = 1; }
	}
	pthread_mutex_unlock(&px_mu);
	return ok;
}
void sweet_px_grid_skip(void)
{
	pthread_mutex_lock(&px_mu);
	px_grid_skip++;
	pthread_mutex_unlock(&px_mu);
}
/* counts a skip and returns 1 when its loud line is to be printed (first skip of this FBO name, at most GLI_PXG_SKIPLINES lines) */
int sweet_px_grid_skip_once(int fbo)
{
	int r = 0;
	unsigned i;
	pthread_mutex_lock(&px_mu);
	px_grid_skip++;
	for (i = 0; i < px_gs_n && px_gs_name[i] != fbo; i++) {}
	if (i == px_gs_n && px_gs_n < GLI_PXG_SKIPLINES) { px_gs_name[px_gs_n++] = fbo; r = 1; }
	pthread_mutex_unlock(&px_mu);
	return r;
}
int sweet_px_grid_skip_line(char *buf, size_t n, int fbo, int w, int h, unsigned long long bytes) { return snprintf(buf, n, "SWEETGL: PIXELS grid skip fbo %d vp %dx%d bytes %llu", fbo, w, h, bytes); }
int sweet_px_grid_cap_line(char *buf, size_t n) { return snprintf(buf, n, "SWEETGL: PIXELS grid cap total %d per-fbo %d", GLI_PXG_TOTAL, GLI_PXG_PER_FBO); }

/* part B: an unbind sample of a large offscreen FBO (armed mode only). It does not consume the round's 6 claims. The window is the active round, or, between rounds, the
 * upcoming one once its start time has passed (the capture FBO is unbound BEFORE the blur draws that start a round): attributed round = active ? k : (now >= next start ? k+1 : none).
 * Max GLI_PXU_PER_ROUND per attributed round, GLI_PXU_TOTAL per process; the claim that reaches the total sets r->cap_u once (the caller prints the cap line). */
int sweet_px_unbind_claim(long long now_s, struct sweet_px_rinfo *r)
{
	int ok = 0;
	unsigned attr;
	memset(r, 0, sizeof *r);
	if (!__atomic_load_n(&px_sched, __ATOMIC_RELAXED)) return 0;
	pthread_mutex_lock(&px_mu);
	px_expire_locked(now_s, r);
	if (!px_exhausted_locked() && px_unb_total < GLI_PXU_TOTAL) {
		attr = px_r_active ? px_r_k : (now_s >= px_next_start ? px_r_k + 1 : 0);
		if (attr) {
			if (px_unb_round != attr) { px_unb_round = attr; px_unb_n = 0; }
			if (px_unb_n < GLI_PXU_PER_ROUND) {
				px_unb_n++; px_unb_total++; ok = 1; r->round = (int)attr;
				if (px_unb_total >= GLI_PXU_TOTAL && !px_unb_cap_done) { px_unb_cap_done = 1; r->cap_u = 1; }
			}
		}
	}
	pthread_mutex_unlock(&px_mu);
	return ok;
}
/* review prov-unbind-b5 F3: the pre-read gates skipped an unbind sample: give the claim back (2 per round / 24 total are budgets of READS), count it apart from the blur/fbfetch counters,
 * and withdraw the cap flag if this claim was the one that reached the total (the cap line is printed only for a real 24th read) */
void sweet_px_unbind_rollback(int round, int *cap_u)
{
	pthread_mutex_lock(&px_mu);
	if (px_unb_round == (unsigned)round && px_unb_n > 0 && px_unb_total > 0) { px_unb_n--; px_unb_total--; }
	if (px_unb_cap_done && px_unb_total < GLI_PXU_TOTAL) { px_unb_cap_done = 0; if (cap_u) *cap_u = 0; }
	px_unb_skip++;
	pthread_mutex_unlock(&px_mu);
}
int sweet_px_unbind_cap_line(char *buf, size_t n) { return snprintf(buf, n, "SWEETGL: PIXELS unbind cap"); }
/* unbind sample eligibility: a non-default FBO that was drawn into, last viewport >= 1000 x 100 and not the scanout size */
int sweet_ub_eligible(int fbo, unsigned draws, int vw, int vh) { return fbo != 0 && draws >= 1 && vw >= 1000 && vh >= 100 && !(vw == 1125 && vh == 2436); }
int sweet_px_round_done_line(char *buf, size_t n, int round, unsigned samples, int timeout)
{
	return snprintf(buf, n, "SWEETGL: PIXELS round %d done samples %u timeout %d", round, samples, timeout ? 1 : 0);
}
int sweet_px_rounds_cap_line(char *buf, size_t n) { return snprintf(buf, n, "SWEETGL: PIXELS rounds cap"); }

void sweet_px_reset(void)
{
	pthread_mutex_lock(&px_mu);
	px_nfbo = px_blur_total = px_fbf_total = 0; px_fbf_last = 0; px_samples = 0; px_quadsmall = 0; memset(px_skip_r, 0, sizeof px_skip_r);
	__atomic_store_n(&px_sched, 0, __ATOMIC_RELAXED); px_configured = px_r_active = px_cap_done = 0; px_next_start = px_r_start = 0; px_r_k = px_r_blur = px_r_fbf = px_r_nfbo = px_r_total = 0;
	px_unb_round = px_unb_n = px_unb_total = 0; px_unb_cap_done = 0; px_unb_skip = 0; __atomic_store_n(&cen_ctx_over, 0, __ATOMIC_RELAXED); px_grid_n = 0; px_grid_skip = 0; px_grid_cap_done = 0; px_gf_n = 0; px_gs_n = 0;
	pthread_mutex_unlock(&px_mu);
}
static void px_note(int reason)   /* -1 = a sample was read, 0..4 = skipped for pbo / fbo / rb / align / small */
{
	pthread_mutex_lock(&px_mu);
	if (reason < 0) px_samples++; else px_skip_r[reason]++;
	pthread_mutex_unlock(&px_mu);
}
static void px_note_quadsmall(void)
{
	pthread_mutex_lock(&px_mu);
	px_quadsmall++;
	pthread_mutex_unlock(&px_mu);
}
int sweet_px_stats_line(char *buf, size_t n)
{
	unsigned long s, r[5], qs;
	unsigned rk, rt, ub;
	unsigned long us, co, gs;
	unsigned gn;
	int sched;
	int o;
	pthread_mutex_lock(&px_mu);
	s = px_samples; memcpy(r, px_skip_r, sizeof r); qs = px_quadsmall; sched = px_sched; rk = px_r_k; rt = px_r_total; ub = px_unb_total; us = px_unb_skip; gn = px_grid_n; gs = px_grid_skip; co = __atomic_load_n(&cen_ctx_over, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&px_mu);
	o = snprintf(buf, n, "SWEETGL: PIXELS stats samples %lu skipped %lu reasons pbo=%lu fbo=%lu rb=%lu align=%lu small=%lu", s, r[0] + r[1] + r[2] + r[3] + r[4], r[0], r[1], r[2], r[3], r[4]);
	if (sched && o > 0 && (size_t)o < n) o += snprintf(buf + o, n - (size_t)o, " rounds %u claims %u quadsmall %lu unbind %u skipped %lu ctxover %lu grid %u gridskip %lu", rk, rt, qs, ub, us, co, gn, gs);
	return o;
}

/* the armed-mode sample line: the centre line as always, then " round k quad a/b/c/d" (k = 0 for the early samples; per quadrant patch q[i] = nonzero pixels of 16,
 * -1 = skipped (outside the viewport / viewport < 8x8) printed '-', -2 = the read wrote nothing (0xA5 sentinel untouched) printed '?') */
int sweet_px_line5(char *buf, size_t n, const char *kind, int cls, int fbo, int vw, int vh, const unsigned char *b64, int round, const int q[4])
{
	int o = sweet_px_line(buf, n, kind, cls, fbo, vw, vh, b64), i;
	if (o < 0 || (size_t)o >= n) return o;
	o += snprintf(buf + o, n - (size_t)o, " round %d quad", round);
	for (i = 0; i < 4 && (size_t)o < n; i++) {
		if (q[i] >= 0) o += snprintf(buf + o, n - (size_t)o, "%c%d", i ? '/' : ' ', q[i]);
		else o += snprintf(buf + o, n - (size_t)o, "%c%c", i ? '/' : ' ', q[i] == -2 ? '?' : '-');
	}
	return o;
}

/* draw sequences: when the cumulative blur T-class draw count first reaches 100, 1000, 3000 the NEXT 120 draws are recorded (kind/class/FBO) and printed in lines of <= 400 chars */
static pthread_mutex_t ds_mu = PTHREAD_MUTEX_INITIALIZER;
static int ds_active;
static unsigned ds_rec;
static struct sweet_dseq_run ds_run;

char sweet_census_kind_char(unsigned flags)
{
	if (flags & CEN_BLUR) return 'b';
	if (flags & CEN_FBFETCH) return 'f';
	if (flags & CEN_CMATRIX) return 'c';
	if (flags & CEN_LUM) return 'l';
	if (flags & CEN_RECT) return 'r';
	return 'o';
}
int sweet_dseq_arm(unsigned long blur_t_total)
{
	static const unsigned long thr[3] = { 100, 1000, 3000 };
	int armed = 0;
	pthread_mutex_lock(&ds_mu);
	if (!ds_active && ds_started < 3 && blur_t_total >= thr[ds_started]) {
		ds_active = 1; ds_rec = 0;
		memset(&ds_run, 0, sizeof ds_run);
		__atomic_add_fetch(&ds_started, 1, __ATOMIC_RELAXED);
		ds_run.seq = ds_started;
		armed = 1;
	}
	pthread_mutex_unlock(&ds_mu);
	return armed;
}
int sweet_dseq_active(void) { return __atomic_load_n(&ds_active, __ATOMIC_RELAXED); }
int sweet_dseq_record(char kind, char cls, int fbo, struct sweet_dseq_run *done)
{
	int fin = 0;
	pthread_mutex_lock(&ds_mu);
	if (ds_active && ds_rec < SWEET_DSEQ_N) {
		ds_run.kind[ds_rec] = kind; ds_run.cls[ds_rec] = cls; ds_run.fbo[ds_rec] = fbo;
		ds_rec++;
		if (ds_rec == SWEET_DSEQ_N) { ds_run.n = ds_rec; *done = ds_run; ds_active = 0; fin = 1; }
	}
	pthread_mutex_unlock(&ds_mu);
	return fin;
}
int sweet_dseq_format(char *buf, size_t n, const struct sweet_dseq_run *r, unsigned *pos)
{
	char tail[512];
	size_t o = 0;
	unsigned cnt = 0, i = *pos;
	if (i >= r->n || n < 64) return 0;
	for (; i < r->n; i++) {
		char e[24];
		int len;
		if (r->fbo[i] < 0) len = snprintf(e, sizeof e, " %c/%c/?", r->kind[i], r->cls[i]);
		else len = snprintf(e, sizeof e, " %c/%c/%d", r->kind[i], r->cls[i], r->fbo[i]);
		if (40 + o + (size_t)len > 400 || o + (size_t)len >= sizeof tail) break;
		memcpy(tail + o, e, (size_t)len); o += (size_t)len; cnt++;
	}
	tail[o] = 0;
	*pos = i;
	return snprintf(buf, n, "SWEETGL: DRAWSEQ #%u n %u:%s", r->seq, cnt, tail);
}
void sweet_dseq_reset(void)
{
	pthread_mutex_lock(&ds_mu);
	ds_active = 0; ds_rec = 0; memset(&ds_run, 0, sizeof ds_run);
	__atomic_store_n(&ds_started, 0, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&ds_mu);
}

static struct cen_ent *cen_find(struct cen_ent *tab, void *rctx, unsigned id, int create)
{
	unsigned h = (id * 2654435761u ^ (unsigned)((uintptr_t)rctx >> 4)) % GLI_CEN_TAB, i;
	for (i = 0; i < GLI_CEN_TAB; i++) {
		struct cen_ent *e = &tab[(h + i) % GLI_CEN_TAB];
		if (!e->used) {
			if (!create) return NULL;
			e->used = 1; e->rctx = rctx; e->id = id; e->flags = 0;
			return e;
		}
		if (e->id == id && e->rctx == rctx) return e;
	}
	return NULL;
}
static void cen_set(struct cen_ent *tab, void *rctx, unsigned id, unsigned flags, int or_in)
{
	struct cen_ent *e;
	pthread_mutex_lock(&cen_mu);
	e = cen_find(tab, rctx, id, 1);
	if (e) e->flags = or_in ? (e->flags | flags) : flags;
	pthread_mutex_unlock(&cen_mu);
	if (!e) sweet_gap("census", "tag table full (kinds of later shaders count as other)");
}
static unsigned cen_get(struct cen_ent *tab, void *rctx, unsigned id)
{
	struct cen_ent *e;
	unsigned f = 0;
	pthread_mutex_lock(&cen_mu);
	e = cen_find(tab, rctx, id, 0);
	if (e) f = e->flags;
	pthread_mutex_unlock(&cen_mu);
	return f;
}
unsigned sweet_census_shader_kinds(void *rctx, unsigned shader) { return cen_get(cen_sh, rctx, shader); }
unsigned sweet_census_program_kinds(void *rctx, unsigned program) { return cen_get(cen_pr, rctx, program); }

static void hook_shadersource_census(void *rctx, unsigned shader, int count, const char *const *str, const int *length)
{
	unsigned flags = 0;
	if (count > 0 && count <= 256 && str) {
		size_t total = 0, k, o = 0;
		char *cat;
		for (k = 0; k < (size_t)count; k++)
			if (str[k]) total += (length && length[k] >= 0) ? (size_t)length[k] : strlen(str[k]);
		cat = malloc(total + 1);
		if (cat) {
			for (k = 0; k < (size_t)count; k++) {
				size_t n = str[k] ? ((length && length[k] >= 0) ? (size_t)length[k] : strlen(str[k])) : 0;
				if (n) { memcpy(cat + o, str[k], n); o += n; }
			}
			flags = sweet_census_kinds(cat, o);
			free(cat);
		}
	}
	cen_set(cen_sh, rctx, shader, flags, 0);
	((void (*)(void *, unsigned, int, const char *const *, const int *))cen_next_ss)(rctx, shader, count, str, length);
}
static unsigned hook_createprogram_census(void *rctx)
{
	unsigned r = ((unsigned (*)(void *))cen_next_cp)(rctx);
	if (r) cen_set(cen_pr, rctx, r, 0, 0);   /* a reused id must not keep the kinds of the program that had it */
	return r;
}
static void hook_attachshader_census(void *rctx, unsigned program, unsigned shader)
{
	unsigned f = cen_get(cen_sh, rctx, shader);
	if (f) cen_set(cen_pr, rctx, program, f, 1);
	((void (*)(void *, unsigned, unsigned))cen_next_as)(rctx, program, shader);
}

/* one draw: current program's kinds and the viewport class (glGetIntegerv only). Extension (log-only): while a DRAWSEQ is recording the bound draw FBO is queried too, and a
 * BLUR draw into a T viewport / an FBFETCH draw into an S viewport may claim a pixel readback sample, taken AFTER the original draw (cen_dr_post). */
struct cen_dr { int ok, cls, vp[4], fbo, px, round, five, complete, cap_c, prog, count, first; unsigned csamples, flags, mode, idxtype; const void *indices; int inst; };   /* inst: an instanced draw (attribute 0 may have a divisor: fetched by instance, not by index, review capv8-r1 F1; nothing is read); first: DrawArrays only (-1 otherwise); idxtype: the index type of the elements draws (0 otherwise); indices: the elements draws' `indices` argument (client pointer or VBO offset); capv8 */
static const void *cen_next_rp, *cen_next_bl, *cen_next_gm;

void sweet_px_census_tick(void);
static void px_print_exp(const struct sweet_px_rinfo *ri)   /* a round that timed out inside the call (draw or census thread tick) */
{
	char l[96];
	if (ri->exp_round <= 0) return;
	sweet_px_round_done_line(l, sizeof l, ri->exp_round, ri->exp_samples, 1);
	fprintf(stderr, "%s\n", l);
	if (ri->cap_e) { sweet_px_rounds_cap_line(l, sizeof l); fprintf(stderr, "%s\n", l); }
}
static void cen_dr_pre(void *rctx, struct cen_dr *d)
{
	int cur = 0, want = 0;
	memset(d, 0, sizeof *d);
	d->fbo = -1; d->first = -1;
	if (!sh_ctx_ok(rctx)) return;
	d->ok = 1;
	dbg_install_ctx(rctx);
	_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
	d->prog = cur;
	_mesa_GetIntegerv(0x0BA2 /* VIEWPORT */, d->vp);
	d->flags = cur > 0 ? cen_get(cen_pr, rctx, (unsigned)cur) : 0;
	d->cls = sweet_census_class(d->vp[2], d->vp[3]);
	sweet_census_count(d->flags, d->vp[2], d->vp[3]);
	if ((d->flags & CEN_BLUR) && d->cls == 1) d->px = 1;
	else if ((d->flags & CEN_FBFETCH) && d->cls == 0) d->px = 2;
	if (d->px || sweet_dseq_active()) { _mesa_GetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING */, &d->fbo); want = 1; }
	if (want && sweet_dseq_active()) {   /* this draw is one of the 120 after the arming draw */
		struct sweet_dseq_run done;
		if (sweet_dseq_record(sweet_census_kind_char(d->flags), "STO"[d->cls], d->fbo, &done)) {
			unsigned pos = 0;
			char l[512];
			while (sweet_dseq_format(l, sizeof l, &done, &pos)) fprintf(stderr, "%s\n", l);
		}
	}
	if (d->cls == 1 && (d->flags & CEN_BLUR)) {   /* armed AFTER recording: the arming draw itself is not part of the sequence */
		unsigned long bt = __atomic_load_n(&cen_cnt[0][1], __ATOMIC_RELAXED);
		if (sweet_dseq_arm(bt)) fprintf(stderr, "SWEETGL: DRAWSEQ armed at %lu draws\n", bt);
	}
	if (d->px) {
		long long now = px_now();
		struct sweet_px_rinfo ri;
		if (!sweet_px_claim(d->px == 2, d->fbo, now)) {   /* early sample (round 0) first, then the time-armed rounds */
			int got = sweet_px_round_claim(d->px == 2, d->fbo, now, &ri);
			px_print_exp(&ri);
			if (got) { d->round = ri.round; d->complete = ri.complete; d->cap_c = ri.cap_c; d->csamples = ri.csamples; }
			else d->px = 0;
		}
		d->five = sweet_px_sched_on();
	}
}

/* pixel readback (log-only, one glReadPixels through the saved original entry, no state change). Every precondition is queried first; any failing one skips the read and is
 * counted. Queries that fail to write leave the pessimistic initial value, which skips. The 80-byte buffer holds the 64-byte patch plus 16 bytes of slack. */
static void px_note_d(const struct cen_dr *d, int reason) { if (d->px != 3) px_note(reason); }
/* draw state (armed blur / fbfetch samples): 13 glGetIntegerv calls, every pname verified error-free in an ES 3.0 context (comment above sweet_census_kind_str) */
static void cen_state_print(const struct cen_dr *d)
{
	struct sweet_px_state st;
	char l[420];
	memset(&st, 0, sizeof st);
	st.prog = -1; st.blend = -1; st.scissor = -1; st.depth = -1;   /* what a query that wrote nothing leaves */
	st.bf[0] = st.bf[1] = st.bf[2] = st.bf[3] = -1; st.eq[0] = st.eq[1] = -1;
	st.mask[0] = st.mask[1] = st.mask[2] = st.mask[3] = -1; st.sbox[0] = st.sbox[1] = st.sbox[2] = st.sbox[3] = -1; st.vp[0] = st.vp[1] = st.vp[2] = st.vp[3] = -1;
	_mesa_GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &st.prog);
	_mesa_GetIntegerv(0x0BE2 /* BLEND */, &st.blend);
	_mesa_GetIntegerv(0x80C9 /* BLEND_SRC_RGB */, &st.bf[0]);
	_mesa_GetIntegerv(0x80C8 /* BLEND_DST_RGB */, &st.bf[1]);
	_mesa_GetIntegerv(0x80CB /* BLEND_SRC_ALPHA */, &st.bf[2]);
	_mesa_GetIntegerv(0x80CA /* BLEND_DST_ALPHA */, &st.bf[3]);
	_mesa_GetIntegerv(0x8009 /* BLEND_EQUATION_RGB */, &st.eq[0]);
	_mesa_GetIntegerv(0x883D /* BLEND_EQUATION_ALPHA */, &st.eq[1]);
	_mesa_GetIntegerv(0x0C23 /* COLOR_WRITEMASK */, st.mask);
	_mesa_GetIntegerv(0x0C11 /* SCISSOR_TEST */, &st.scissor);
	_mesa_GetIntegerv(0x0C10 /* SCISSOR_BOX */, st.sbox);
	_mesa_GetIntegerv(0x0BA2 /* VIEWPORT */, st.vp);
	_mesa_GetIntegerv(0x0B71 /* DEPTH_TEST */, &st.depth);
	st.flags = d->flags; st.mode = d->mode; st.count = d->count;
	sweet_state_line(l, sizeof l, d->round, d->fbo, &st);
	fprintf(stderr, "%s\n", l);
}
/* grid map of an unbind sample: ONE full-viewport glReadPixels (RGBA / UNSIGNED_BYTE, original dispatch) after the 5-point sample whose gates (pbo / read fbo / complete / read buffer /
 * read format / PACK_ALIGNMENT 1..8, ROW_LENGTH / SKIP 0) were evaluated once; the buffer stride follows PACK_ALIGNMENT. Pre-filled with 0xA5: a read that wrote nothing prints `written 0`.
 * A viewport rect extending past the framebuffer leaves its clipped part at the 0xA5 pre-fill: the line says how many pixels (`a5 N`) and all-pre-fill cells print '?' (review b6 F1). */
static void cen_grid(void *rctx, const struct cen_dr *d, int al)
{
	int w = d->vp[2], h = d->vp[3], nz, cap = 0, wr;
	unsigned a5;
	size_t stride, need;
	unsigned char *buf;
	char map[8][9], l[300];   /* the longest line (grid, 64-char map) is ~170 chars */
	if (!sweet_px_grid_avail(d->fbo)) return;   /* the process total is spent (cap line printed) or this FBO has its grids (counted) */
	if (w < 8 || h < 8 || al < 1) { sweet_px_grid_skip(); return; }
	stride = ((size_t)w * 4 + (size_t)al - 1) / (size_t)al * (size_t)al;
	need = stride * (size_t)h;
	if (need > GLI_PXG_MAX_BYTES || !(buf = (unsigned char *)malloc(need))) {   /* F2: loud, once per FBO name */
		if (sweet_px_grid_skip_once(d->fbo)) { sweet_px_grid_skip_line(l, sizeof l, d->fbo, w, h, (unsigned long long)need); fprintf(stderr, "%s\n", l); }
		return;
	}
	memset(buf, 0xA5, need);
	if (!sweet_px_grid_claim(d->fbo, &cap)) { free(buf); return; }
	((void (*)(void *, int, int, int, int, unsigned, unsigned, void *))cen_next_rp)(rctx, d->vp[0], d->vp[1], w, h, 0x1908, 0x1401, buf);
	wr = sweet_grid_written(buf, stride, w, h);
	nz = wr ? sweet_grid_classify(buf, stride, w, h, map) : 0;
	a5 = wr ? sweet_grid_sentinel(buf, stride, w, h) : 0;
	free(buf);
	sweet_grid_line(l, sizeof l, d->round, d->fbo, w, h, wr, nz, map, a5);
	fprintf(stderr, "%s\n", l);
	if (cap) { sweet_px_grid_cap_line(l, sizeof l); fprintf(stderr, "%s\n", l); }
}
/* returns 1 when the centre patch was read (the sample happened), 0 when a pre-read gate skipped it. Unbind samples (d->px == 3) keep out of the shared samples/skipped counters (F3). */
static int cen_px_read(void *rctx, const struct cen_dr *d)
{
	int ot = -1, pbo = -1, rfb = -1, sb = -1, rbuf = 0, crf = 0, crt = 0, al = 99, rl = -1, sp = -1, sr = -1, quad[4], i;
	unsigned char b[80];
	char l[320];   /* F5: the longest prov line (4 units of 10-digit names / sizes / att + `+N more`) is ~310 chars */
	_Static_assert(sizeof b >= 64 + 16, "readback buffer");
	if (d->vp[2] < 4 || d->vp[3] < 4) { px_note_d(d, 4); return 0; }
	_mesa_GetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pbo);
	if (pbo != 0) { px_note_d(d, 0); return 0; }
	_mesa_GetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
	if (rfb < 0 || rfb != d->fbo) { px_note_d(d, 1); return 0; }
	if (rfb != 0 && _mesa_CheckFramebufferStatus(0x8CA8 /* READ_FRAMEBUFFER */) != 0x8CD5 /* COMPLETE */) { px_note_d(d, 1); return 0; }
	_mesa_GetIntegerv(0x80A8 /* SAMPLE_BUFFERS */, &sb);
	if (sb != 0) { px_note_d(d, 1); return 0; }
	_mesa_GetIntegerv(0x0C02 /* READ_BUFFER */, &rbuf);
	if (rfb != 0 ? rbuf != 0x8CE0 : (rbuf != 0x0405 && rbuf != 0x0404)) { px_note_d(d, 2); return 0; }
	if (rfb != 0) {   /* review census-b4 F1: the COLOR_READ_FORMAT/TYPE queries raise INVALID_OPERATION when the read buffer has no image ([read from source] mesa 26.2.5 framebuffer.c _mesa_get_color_read_format/type,
		 * called from get.c). An empty COLOR_ATTACHMENT0 of a complete user FBO is legal in ES 3.0, so ask first. This query raises NO error here [read from source] fbobject.c:263-275
		 * get_framebuffer_target (READ_FRAMEBUFFER valid in ES3), :291-330 get_attachment (COLOR_ATTACHMENT0 of a user FBO, no error), :4966-4975 OBJECT_TYPE returns att->Type = GL_NONE without an error.
		 * Assumes an ES 3.0+ context, like the READ_BUFFER and SAMPLE_BUFFERS queries above (in an ES2 context READ_FRAMEBUFFER would already be INVALID_ENUM in CheckFramebufferStatus). */
		_mesa_GetFramebufferAttachmentParameteriv(0x8CA8 /* READ_FRAMEBUFFER */, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x8CD0 /* OBJECT_TYPE */, &ot);
		if (ot <= 0) { px_note_d(d, 2); return 0; }   /* NONE (0) or the query wrote nothing (-1) */
	}
	_mesa_GetIntegerv(0x8B9B /* IMPLEMENTATION_COLOR_READ_FORMAT */, &crf);
	_mesa_GetIntegerv(0x8B9A /* IMPLEMENTATION_COLOR_READ_TYPE */, &crt);
	if (crf != 0x1908 || crt != 0x1401) { px_note_d(d, 2); return 0; }
	_mesa_GetIntegerv(0x0D05 /* PACK_ALIGNMENT */, &al);
	_mesa_GetIntegerv(0x0D02 /* PACK_ROW_LENGTH */, &rl);
	_mesa_GetIntegerv(0x0D04 /* PACK_SKIP_PIXELS */, &sp);
	_mesa_GetIntegerv(0x0D03 /* PACK_SKIP_ROWS */, &sr);
	if (al < 1 || al > 8 || rl != 0 || sp != 0 || sr != 0) { px_note_d(d, 3); return 0; }
	/* gates evaluated once above; now the centre patch exactly as before, then (armed mode only) four quadrant patches, each its own sentinel-filled buffer and its own original glReadPixels */
	memset(b, 0xA5, sizeof b);   /* review census-b4 F3: sentinel, so a read that wrote nothing (error, patch clipped away) is not reported as black pixels */
	((void (*)(void *, int, int, int, int, unsigned, unsigned, void *))cen_next_rp)(rctx, d->vp[0] + d->vp[2] / 2 - 2, d->vp[1] + d->vp[3] / 2 - 2, 4, 4, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, b);
	px_note_d(d, -1);
	if (!d->five) {   /* SWEETGL_CENSUS=1: the line and the single read are exactly what they were */
		sweet_px_line(l, sizeof l, d->px == 1 ? "blur" : "fbfetch", d->cls, d->fbo, d->vp[2], d->vp[3], b);
		fprintf(stderr, "%s\n", l);
		return 1;
	}
	if (d->vp[2] < 8 || d->vp[3] < 8) px_note_quadsmall();
	for (i = 0; i < 4; i++) {
		int x = d->vp[0] + (i & 1 ? 3 * d->vp[2] / 4 : d->vp[2] / 4) - 2, y = d->vp[1] + (i >> 1 ? 3 * d->vp[3] / 4 : d->vp[3] / 4) - 2;
		unsigned char qb[80];
		unsigned nz, ma, sm;
		_Static_assert(sizeof qb >= 64 + 16, "readback buffer");
		quad[i] = -1;
		if (d->vp[2] < 8 || d->vp[3] < 8 || x < d->vp[0] || y < d->vp[1] || x + 4 > d->vp[0] + d->vp[2] || y + 4 > d->vp[1] + d->vp[3]) continue;
		memset(qb, 0xA5, sizeof qb);
		((void (*)(void *, int, int, int, int, unsigned, unsigned, void *))cen_next_rp)(rctx, x, y, 4, 4, 0x1908, 0x1401, qb);
		if (!sweet_px_written(qb)) { quad[i] = -2; continue; }
		sweet_px_stats(qb, &nz, &ma, &sm);
		quad[i] = (int)nz;
	}
	sweet_px_line5(l, sizeof l, d->px == 1 ? "blur" : d->px == 2 ? "fbfetch" : "unbind", d->cls, d->fbo, d->vp[2], d->vp[3], b, d->round, quad);
	fprintf(stderr, "%s\n", l);
	if (d->px != 3) {   /* part A: where the shader inputs came from (a sample line is followed by exactly one prov line in armed mode) */
		unsigned tu[8];
		sweet_tu_snapshot(sweet_th_ctx_index(rctx), tu);
		sweet_prov_line(l, sizeof l, d->round, d->fbo, tu);
		fprintf(stderr, "%s\n", l);
		cen_state_print(d);
	} else {
		cen_grid(rctx, d, al);
	}
	return 1;
}
/* part B state: the draw FBO as the BindFramebuffer hook saw it (GL_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER binds only; no GL query), the draws since it was bound and the last viewport the
 * census draw hook saw. a bound FBO that gets deleted is not noticed (the sample's own READ_FRAMEBUFFER gate still protects the read). */
static pthread_mutex_t ub_mu = PTHREAD_MUTEX_INITIALIZER;
struct ub_s { int fbo, vp[4]; unsigned draws, capv_n, capv_e; struct sweet_capdraws cd; };   /* capv_n: capv8 draws logged since the last bind of this FBO; capv_e: eligible-class draws since the bind (the window); skipped 1125x2436 draws and other classes do not count */   /* cd: since the last bind of this FBO, reset at the bind */
/* GL_BLEND as tracked from the Enable / Disable hooks (starts disabled = the GL default), per context, under ub_mu. Review b6 F4: its OWN small context table (16 entries, keyed by the context
 * pointer), so the hooks neither register contexts in the texture-history table nor touch ctxover, and take no th_mu. A 17th context is not tracked (reads 0). */
static void *blend_k[16]; static unsigned char blend_v[16]; static unsigned blend_n;
static int blend_find_locked(void *rctx, int create)
{
	unsigned i;
	for (i = 0; i < blend_n; i++) if (blend_k[i] == rctx) return (int)i;
	if (!create || blend_n >= 16) return -1;
	blend_k[blend_n] = rctx; blend_v[blend_n] = 0;
	return (int)blend_n++;
}
static struct ub_s ub_c[GLI_CTX_N];   /* per context (F2); overflow contexts share slot 0 */
#define GLI_CAPV_TOTAL 32      /* capv8 draws per process (skip draws count; each draw prints several lines) */
#define GLI_CAPV_PER_BIND 4    /* and per FBO bind */
#define GLI_CAPV_WINDOW 4      /* eligible-class draws since the bind that can be logged */
#define GLI_CAPV_MIN_H 1000    /* viewport height gate (w >= 1000 is class S): the 1127x149 FBOs of g113f must not spend the 32-line budget */
/* g113g [obs]: the 32-draw budget went to the screen-size compositor FBOs (viewport exactly 1125x2436) and no count-48 capture draw was logged: those draws are not eligible */
#define GLI_CAPV_SKIP_W 1125
#define GLI_CAPV_SKIP_H 2436
static unsigned capv_total; static int capv_capped;   /* under ub_mu */
/* F7: the viewport is the last draw's viewport of that context whatever FBO the draw went to (d->fbo is only queried for sampled draws); a final small draw into the FBO makes it ineligible */
void sweet_ub_reset(void)
{
	pthread_mutex_lock(&ub_mu);
	memset(ub_c, 0, sizeof ub_c); memset(blend_k, 0, sizeof blend_k); memset(blend_v, 0, sizeof blend_v); blend_n = 0;
	capv_total = 0; capv_capped = 0;
	pthread_mutex_unlock(&ub_mu);
}
/* ---- capv8 (batch 8, log-only; capv7 rework): per early draw into a capture FBO, the sampled textures, vertex attribute state, first vertices, uniforms (the app's GL error state is never read) ----
 * Every call below is verified error-free in an ES 3.0 context against Mesa 26.2.5 [read from source, Mesa 26.2.5 src/mesa/main], given the
 * preconditions the code checks first. Each _mesa_ function is a global (non-static) in the Mesa objects the build links; those that are gli slots (GetVertexAttribiv 516,
 * GetVertexAttribPointerv 517, GetUniformLocation 625, GetActiveUniform 626, GetUniformfv 627, GetUniformiv 628, GetBufferParameteriv 651, GetProgramiv 658, BindBuffer, GetError, GetIntegerv)
 * are symbol-checked by gen_gli.py --symbols; _mesa_GetBufferSubData is not a slot but lives in bufferobj.c next to _mesa_BindBuffer / _mesa_GetBufferParameteriv (same object file).
 *  - glGetError is NEVER called here: getstring.c:383-401 returns ctx->ErrorValue and CLEARS it (:400), so any read would consume the app's pending error (the hook runs after the app's draw).
 *    The app's GL error state is never touched; the per-call citations below prove our queries raise no error, so no error tail is printed (not measured).
 *  - GetVertexAttribiv (varray.c:2519): pnames ENABLED 0x8622, BUFFER_BINDING 0x889F, SIZE 0x8623, TYPE 0x8625, STRIDE 0x8624 go to get_vertex_array_attrib (varray.c:2364), cases
 *    :2382 ENABLED, :2384 SIZE, :2386 STRIDE, :2388 TYPE, :2392 BUFFER_BINDING; the only error on those paths is index >= MaxAttribs (:2372; ES3 minimum 16, we use 0..3).
 *    Values: include/GLES3/gl3.h:310-316. Stride is the user stride, 0 = tightly packed (varray.c:1121; mtypes.h:1509-1510); the effective stride is the element size (varray.c:1137-1140).
 *    The pointer / offset query covers the ES 3.0 attribute API only (glVertexAttribPointer); VertexAttribFormat / BindVertexBuffer (ES 3.1) keep the offset in BufferBinding, so vertices could then come from the wrong offset (bounds are still checked).
 *  - GetVertexAttribPointerv (varray.c:2607) with pname 0x8645 (gl3.h:315): errors only for index >= MaxAttribs (:2611) and another pname (:2616); returns VertexAttrib[].Ptr (:2624-2625),
 *    with a VBO the byte offset (varray.c:1139-1140: the same ptr is the buffer binding offset).
 *  - GetIntegerv(0x8F36) = COPY_READ_BUFFER binding (GL_COPY_READ_BUFFER_BINDING has the same value, gl3.h:893): get_hash_params.py:101 (group apis GLES2, :2) + get_hash_generator.py:170-174
 *    (GLES2 entries also go into the GLES3 table), NO_EXTRA; value get.c:1104-1105.
 *  - BindBuffer(0x8F36, name) (bufferobj.c:1548): get_buffer_target (:698) allows the target in an ES3 context (gate :701, case :722); an existing name is only looked up (:1370; handle_bind_buffer_gen
 *    errors only when the name does not exist in a core profile or on out of memory, bufferobj.c:handle_bind_buffer_gen); name 0 unbinds (:1360). The restored `old` came from GetIntegerv.
 *  - GetBufferParameteriv (bufferobj.c:3167) pnames SIZE 0x8764 and MAPPED 0x88BC (gl3.h:108, :691): get_buffer_parameter (:3111) cases :3116 / :3126, no extension check; get_buffer (:794) errors
 *    only with no buffer bound (we just bound one) or a bad target.
 *  - GetBufferSubData (bufferobj.c:2681): errors only for no buffer bound (get_buffer :2687), size < 0 / offset < 0 / offset + size > Size (:904-914) and a user-mapped buffer (:935); we check SIZE and
 *    MAPPED first. No API gate in the function (the dispatch entry is not in ES3, the _mesa_ function is called directly, as compute_selftest does).
 *  - GetProgramiv (shaderapi.c:1858 -> get_programiv :753): LINK_STATUS 0x8B82 (:791), ACTIVE_UNIFORMS 0x8B86 (:810), ACTIVE_UNIFORM_MAX_LENGTH 0x8B87 (:815) (gl2.h:211, :214-215), for an existing program
 *    (CURRENT_PROGRAM, queried in cen_dr_pre); _mesa_get_program_interfaceiv(GL_UNIFORM, ACTIVE_RESOURCES / MAX_NAME_LENGTH) raises no error for GL_UNIFORM (shader_query.cpp:1792-1815).
 *  - GetActiveUniform (uniform_query.cpp:96 -> _mesa_GetActiveUniform_impl :52-94): errors only for maxLength < 0, a name that is not a program, index >= resource count; we pass 0 <= index < ACTIVE_UNIFORMS and maxLength 1..64.
 *  - GetUniformLocation (uniforms.c:1028 -> :1001-1025): -1 without an error for an unknown name or a block member; an error only for a program that is not linked (LINK_STATUS is checked first).
 *  - GetUniformfv / GetUniformiv (uniforms.c:919 / :934 -> _mesa_get_uniform uniform_query.cpp:305): error only for a location that is not valid for the program (we pass >= 0 locations returned for this program)
 *    and a buffer smaller than the uniform (:390; bufSize is INT_MAX, our float buffer holds 16 = mat4, the largest type we accept; one int for a sampler, whose read is a 4-byte memcpy).
 * Types accepted for float reads: FLOAT 0x1406, VEC2/3/4 0x8B50-0x8B52, MAT3/4 0x8B5B/0x8B5C (gl3.h:189, :295-297, :306-307); samplers (read with GetUniformiv): 2D 0x8B5E, 3D 0x8B5F, CUBE 0x8B60,
 * 2D_SHADOW 0x8B62, 2D_ARRAY 0x8DC1 (gl3.h:308-309, :718-719, :782), RECT 0x8B63 (GL/glext.h:1276), EXTERNAL_OES 0x8D66 (GLES2/gl2ext.h:253). Everything else prints `t0x<type>`. */
extern void _mesa_GetVertexAttribiv(unsigned, unsigned, int *);
extern void _mesa_GetVertexAttribPointerv(unsigned, unsigned, void **);
extern void _mesa_GetBufferParameteriv(unsigned, unsigned, int *);
extern void _mesa_GetUniformfv(unsigned, int, float *);
static int capv_sampler(unsigned t)
{
	return t == 0x8B5E || t == 0x8B5F || t == 0x8B60 || t == 0x8B62 || t == 0x8B63 || t == 0x8D66 || t == 0x8DC1;
}
static int capv_nfloats(unsigned t)
{
	switch (t) { case 0x1406: return 1; case 0x8B50: return 2; case 0x8B51: return 3; case 0x8B52: return 4; case 0x8B5B: return 9; case 0x8B5C: return 16; }
	return 0;
}
/* the vertices vi[0..nv-1] of attribute 0 (VBO, FLOAT, size 2..4): bind the VBO to COPY_READ_BUFFER, check size and mapped state, read, ALWAYS restore the old binding */
static void capv_verts(struct sweet_capv *v, const unsigned *vi, int nv)
{
	int old = -1, size = -1, mapped = -1, i, k, sz = v->at[0].sz;
	long long elem = (long long)sz * 4, stride = v->at[0].st ? v->at[0].st : elem, end, mx = 0;
	unsigned long off = v->at[0].off;
	for (i = 0; i < nv; i++) if ((long long)vi[i] > mx) mx = vi[i];
	_mesa_GetIntegerv(0x8F36 /* COPY_READ_BUFFER binding */, &old);
	if (old < 0) return;   /* the query wrote nothing: leave the binding alone */
	_mesa_BindBuffer(0x8F36, (unsigned)v->at[0].buf);
	_mesa_GetBufferParameteriv(0x8F36, 0x8764 /* BUFFER_SIZE */, &size);
	_mesa_GetBufferParameteriv(0x8F36, 0x88BC /* BUFFER_MAPPED */, &mapped);
	if (mapped == 1) v->vstate = SWEET_CAPV_V_MAPPED;
	else if (mapped == 0 && size >= 0) {
		end = off > (1UL << 40) ? -1 : (long long)off + mx * stride + elem;
		if (end < 0 || end > size) v->vstate = SWEET_CAPV_V_OOB;
		else {
			for (i = 0; i < nv; i++) {
				float t[4] = { 0, 0, 0, 0 };
				_mesa_GetBufferSubData(0x8F36, (long)((long long)off + (long long)vi[i] * stride), (long)elem, t);
				for (k = 0; k < 4; k++) v->v[i][k] = t[k];
			}
			v->vstate = SWEET_CAPV_V_VALUES; v->vsz = sz; v->nv = nv;
		}
	}
	_mesa_BindBuffer(0x8F36, (unsigned)old);   /* teardown: always runs once the VBO was bound */
}
/* the vertices vi[0..nv-1] of a client array: plain loads from the app's own pointer in this process, no GL call. For a draw that SUCCEEDED (glGetError is never read, so a failed draw that read nothing
 * cannot be told apart: the pointer is then only what the app set, review capv8-r1 F2, accepted) the draw already read these vertices (every index of
 * the first min(count,64) is <= imax <= 65535 and the draw consumed at least those entries), so the loads stay inside what the app handed to GL. memcpy: no alignment assumption. */
static void capv_cli_verts(struct sweet_capv *v, const unsigned *vi, int nv, const void *ptr)
{
	int i, sz = v->at[0].sz;
	long long stride = v->at[0].st ? v->at[0].st : (long long)sz * 4;
	for (i = 0; i < nv; i++) {
		float t[4] = { 0, 0, 0, 0 };
		memcpy(t, (const char *)ptr + (long long)vi[i] * stride, (size_t)sz * 4);
		memcpy(v->v[i], t, sizeof t);
	}
	v->vstate = SWEET_CAPV_V_VALUES; v->vsz = sz; v->nv = nv;
}
/* elements draw: the first min(count,64) client indices (u8 / u16 / u32), the first 4 for the vertex fetch. GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895 (gl3.h:104) is the VAO's index buffer name:
 * get_hash_params.py:2 (apis incl. GLES2), :93 -> get.c:1088-1090 (VAO->IndexBufferObj->Name); a name != 0 makes `indices` a byte offset, not a pointer: nothing is read. Returns 0 with vstate set on failure. */
static int capv_indices(struct sweet_capv *v, const struct cen_dr *d, unsigned *vi, int *nv)
{
	int eb = -1, n = d->count < 64 ? d->count : 64, i, isz = d->idxtype == 0x1401 ? 1 : d->idxtype == 0x1403 ? 2 : d->idxtype == 0x1405 ? 4 : 0;
	unsigned mn = ~0u, mx = 0;
	_mesa_GetIntegerv(0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING */, &eb);
	if (eb < 0) { v->vstate = SWEET_CAPV_V_IDXQ; return 0; }
	if (eb > 0) { v->vstate = SWEET_CAPV_V_IDXBUF; return 0; }
	if (!d->indices || !isz) { v->vstate = SWEET_CAPV_V_NONE; return 0; }
	for (i = 0; i < n; i++) {
		unsigned x = 0;
		if (isz == 1) { unsigned char c; memcpy(&c, (const char *)d->indices + i, 1); x = c; }
		else if (isz == 2) { unsigned short c; memcpy(&c, (const char *)d->indices + 2 * i, 2); x = c; }
		else memcpy(&x, (const char *)d->indices + 4 * i, 4);
		if (x < mn) mn = x;
		if (x > mx) mx = x;
		if (i < 4) vi[i] = x;
	}
	v->hasidx = 1; v->nidx = n < 4 ? n : 4; v->imin = mn; v->imax = mx;
	memcpy(v->idx, vi, sizeof v->idx);
	if (mx > 65535) { v->vstate = SWEET_CAPV_V_IDXRANGE; return 0; }
	*nv = n < 4 ? n : 4;
	return 1;
}
/* up to SWEET_CAPV_U active uniforms of the program: floats, sampler units, or the type */
static void capv_uniforms(struct sweet_capv *v, int prog)
{
	int link = 0, nact = -1, maxl = -1, i;
	if (prog <= 0) return;
	_mesa_GetProgramiv((unsigned)prog, 0x8B82 /* LINK_STATUS */, &link);
	if (link != 1) return;
	_mesa_GetProgramiv((unsigned)prog, 0x8B86 /* ACTIVE_UNIFORMS */, &nact);
	_mesa_GetProgramiv((unsigned)prog, 0x8B87 /* ACTIVE_UNIFORM_MAX_LENGTH */, &maxl);
	if (nact <= 0 || maxl <= 0) return;
	if (maxl > 64) maxl = 64;
	for (i = 0; i < nact && v->nu < SWEET_CAPV_U; i++) {
		char nm[64];
		int len = 0, size = 0, loc;
		unsigned type = 0;
		__typeof__(v->u[0]) *u = &v->u[v->nu++];
		nm[0] = 0;
		_mesa_GetActiveUniform((unsigned)prog, (unsigned)i, maxl, &len, &size, &type, nm);
		nm[sizeof nm - 1] = 0;
		snprintf(u->name, sizeof u->name, "%.15s", nm);
		u->type = type;
		loc = _mesa_GetUniformLocation((unsigned)prog, nm);
		if (loc < 0) u->kind = SWEET_CAPV_U_NOLOC;
		else if (capv_sampler(type)) { int unit = -1; _mesa_GetUniformiv((unsigned)prog, loc, &unit); u->kind = SWEET_CAPV_U_UNIT; u->unit = unit; }
		else if (capv_nfloats(type)) { _mesa_GetUniformfv((unsigned)prog, loc, u->f); u->kind = SWEET_CAPV_U_FLOATS; u->nf = capv_nfloats(type); }
		else u->kind = SWEET_CAPV_U_TYPE;
	}
	v->umore = nact > i ? nact - i : 0;
}
static void cen_capv(void *rctx, const struct cen_dr *d, int fbo, int idx)
{
	struct sweet_capv v;
	char l[SWEET_CAPV_MAX + 26], pl[320];   /* the formatter stays below SWEET_CAPV_MAX; the prov line needs 320 (cen_px_read F5) */
	unsigned a, vi[4] = { 0, 0, 0, 0 };
	void *ptr0 = NULL;
	int nv = 0, ok = 1, k;
	memset(&v, 0, sizeof v);
	v.vw = d->vp[2]; v.vh = d->vp[3];
	v.prog = d->prog; v.mode = d->mode; v.count = d->count; v.first = d->first; v.idxtype = d->idxtype;
	sweet_tu_snapshot(sweet_th_ctx_index(rctx), v.tex);
	for (a = 0; a < 4; a++) {
		int en = -1, buf = -1, sz = -1, ty = -1, st = -1;
		void *ptr = NULL;
		_mesa_GetVertexAttribiv(a, 0x8622 /* ENABLED */, &en);
		_mesa_GetVertexAttribiv(a, 0x889F /* BUFFER_BINDING */, &buf);
		_mesa_GetVertexAttribiv(a, 0x8623 /* SIZE */, &sz);
		_mesa_GetVertexAttribiv(a, 0x8625 /* TYPE */, &ty);
		_mesa_GetVertexAttribiv(a, 0x8624 /* STRIDE */, &st);
		_mesa_GetVertexAttribPointerv(a, 0x8645 /* POINTER */, &ptr);
		v.at[a].en = en; v.at[a].buf = buf; v.at[a].sz = sz; v.at[a].ty = (unsigned)ty; v.at[a].st = st; v.at[a].off = (unsigned long)(uintptr_t)ptr;
		if (a == 0) ptr0 = ptr;
	}
	/* which vertices, and why not (a short word): see sweet_capv_line */
	if (d->inst) v.vstate = SWEET_CAPV_V_INST;   /* F1: a vertex divisor makes the fetch index an instance number: the client array may hold only `instances` entries, so nothing is read */
	else if (d->first < 0 && !d->idxtype) v.vstate = SWEET_CAPV_V_NOFIRST;
	else if (v.at[0].en != 1) v.vstate = SWEET_CAPV_V_DIS;
	else if (v.at[0].ty != 0x1406 || v.at[0].sz < 2 || v.at[0].sz > 4 || v.at[0].st < 0) v.vstate = SWEET_CAPV_V_TYPE;
	else if (d->count <= 0) v.vstate = SWEET_CAPV_V_NONE;
	else if (v.at[0].buf == 0 && !ptr0) v.vstate = SWEET_CAPV_V_CLI0;
	else {
		if (d->idxtype) ok = capv_indices(&v, d, vi, &nv);
		else { nv = d->count < 4 ? d->count : 4; for (k = 0; k < nv; k++) vi[k] = (unsigned)d->first + (unsigned)k; }
		if (ok && v.at[0].buf > 0) capv_verts(&v, vi, nv);
		else if (ok && v.at[0].buf == 0) capv_cli_verts(&v, vi, nv, ptr0);
	}
	capv_uniforms(&v, d->prog);
	for (a = 0; a < 5; a++) {
		static const char tags[5] = { 'h', 'a', 'a', 'v', 'v' };
		static const int ks[5] = { 0, 0, 1, 0, 1 };
		if (sweet_capv_line(l, sizeof l, fbo, idx, &v, tags[a], ks[a]) > 0) fprintf(stderr, "%s\n", l);
	}
	for (k = 0; k < 16 && sweet_capv_line(l, sizeof l, fbo, idx, &v, 'u', k) > 0; k++) fprintf(stderr, "%s\n", l);
	sweet_prov_line(pl, sizeof pl, 0, fbo, v.tex);
	if (strlen(pl) >= SWEET_CAPV_MAX) strcpy(pl + SWEET_CAPV_MAX - 6, " ~cut");   /* the tee splits lines above ~256 B; this one stays whole (normally ~150 B) */
	fprintf(stderr, "%s\n", pl);
}
static void cen_dr_post(void *rctx, const struct cen_dr *d)
{
	int capv = 0, capv_fbo = 0, capv_idx = 0;
	if (d->ok) {
		int c = cen_ctx(rctx);
		pthread_mutex_lock(&ub_mu);
		ub_c[c].draws++; memcpy(ub_c[c].vp, d->vp, sizeof ub_c[c].vp);
		{
			struct sweet_capdraws *cd = &ub_c[c].cd;
			static const char kc[] = "bfclro";
			unsigned ki = 0;
			while (kc[ki] != sweet_census_kind_char(d->flags)) ki++;
			cd->draws++; cd->kinds[ki]++; cd->verts += d->count > 0 ? d->count : 0;
			if (cd->draws == 1) { cd->pfirst = d->prog; memcpy(cd->fvp, d->vp, sizeof cd->fvp); cd->fmode = d->mode; cd->fcount = d->count; }
			memcpy(cd->lvp, d->vp, sizeof cd->lvp); cd->lmode = d->mode; cd->lcount = d->count;
			{ int bi = blend_find_locked(rctx, 0); cd->plast = d->prog; cd->blendlast = bi >= 0 ? blend_v[bi] : 0; }
			if (kc[ki] == 'o' && d->cls == 0 && d->vp[3] >= GLI_CAPV_MIN_H && !(d->vp[2] == GLI_CAPV_SKIP_W && d->vp[3] == GLI_CAPV_SKIP_H) && ub_c[c].fbo != 0 && sweet_px_sched_on() && ++ub_c[c].capv_e <= GLI_CAPV_WINDOW) {   /* capv8: one of the first 4 ELIGIBLE plain draws into a capture FBO (review capv8-r1 F4: other classes / the skipped size do not use the window) */
				if (capv_total < GLI_CAPV_TOTAL && ub_c[c].capv_n < GLI_CAPV_PER_BIND) { capv_total++; ub_c[c].capv_n++; capv = 1; capv_fbo = ub_c[c].fbo; capv_idx = (int)cd->draws; }
				else if (!capv_capped) { capv_capped = 1; capv = 2; }   /* the budget is spent: ONE loud line, then silence */
			}
		}
		pthread_mutex_unlock(&ub_mu);
	}
	if (capv == 1) cen_capv(rctx, d, capv_fbo, capv_idx);
	else if (capv == 2) { char l[64]; sweet_capv_cap_line(l, sizeof l); fprintf(stderr, "%s\n", l); }
	if (!d->ok || !d->px) return;
	cen_px_read(rctx, d);
	if (d->complete) {   /* review pixels-rounds-g113c F1: after the read whether or not a gate skipped it (state advanced at claim time; the completion and the cap must never be silent), exactly once per draw */
		char l[96];
		sweet_px_round_done_line(l, sizeof l, d->round, d->csamples, 0);
		fprintf(stderr, "%s\n", l);
		if (d->cap_c) { sweet_px_rounds_cap_line(l, sizeof l); fprintf(stderr, "%s\n", l); }
	}
}
static void hook_drawarrays_census(void *rctx, unsigned mode, int first, int count)
{
	struct cen_dr d;
	cen_dr_pre(rctx, &d);
	d.mode = mode; d.count = count; d.first = first;
	((void (*)(void *, unsigned, int, int))cen_next_da)(rctx, mode, first, count);
	cen_dr_post(rctx, &d);
}
static void hook_drawelements_census(void *rctx, unsigned mode, int count, unsigned type, const void *indices)
{
	struct cen_dr d;
	cen_dr_pre(rctx, &d);
	d.mode = mode; d.count = count; d.idxtype = type; d.indices = indices;
	((void (*)(void *, unsigned, int, unsigned, const void *))cen_next_de)(rctx, mode, count, type, indices);
	cen_dr_post(rctx, &d);
}
static void hook_drawrangeelements_census(void *rctx, unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void *indices)
{
	struct cen_dr d;
	cen_dr_pre(rctx, &d);
	d.mode = mode; d.count = count; d.idxtype = type; d.indices = indices;
	((void (*)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *))cen_next_dre)(rctx, mode, start, end, count, type, indices);
	cen_dr_post(rctx, &d);
}
static void hook_drawarraysinstanced_census(void *rctx, unsigned mode, int first, int count, int inst)
{
	struct cen_dr d;
	cen_dr_pre(rctx, &d);
	d.mode = mode; d.count = count; d.inst = 1;
	((void (*)(void *, unsigned, int, int, int))cen_next_dai)(rctx, mode, first, count, inst);
	cen_dr_post(rctx, &d);
}
static void hook_drawelementsinstanced_census(void *rctx, unsigned mode, int count, unsigned type, const void *indices, int inst)
{
	struct cen_dr d;
	cen_dr_pre(rctx, &d);
	d.mode = mode; d.count = count; d.idxtype = type; d.inst = 1;
	((void (*)(void *, unsigned, int, unsigned, const void *, int))cen_next_dei)(rctx, mode, count, type, indices, inst);
	cen_dr_post(rctx, &d);
}

/* census extension counters for other calls (class by viewport, one glGetIntegerv(VIEWPORT); nothing when no context is current). Slots read from gli_slots.txt:
 * glReadPixels 237, glGenerateMipmap 681, glBlitFramebuffer 755. glFramebufferTexture2D (677) and glCopyTexImage2D (54) are counted inside the texhist hooks (no second hook). */
static void cen_count_other(void *rctx, int which, int ok)
{
	int vp[4] = { 0, 0, 0, 0 };
	if (!ok) return;
	_mesa_GetIntegerv(0x0BA2 /* VIEWPORT */, vp);
	sweet_census_count2(which, vp[2], vp[3]);
}
static void hook_blit_census(void *rctx, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter)
{
	cen_count_other(rctx, 0, sh_ctx_ok(rctx));
	((void (*)(void *, int, int, int, int, int, int, int, int, unsigned, unsigned))cen_next_bl)(rctx, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
static void hook_readpixels_census(void *rctx, int x, int y, int w, int h, unsigned fmt, unsigned type, void *px)
{
	cen_count_other(rctx, 1, sh_ctx_ok(rctx));
	((void (*)(void *, int, int, int, int, unsigned, unsigned, void *))cen_next_rp)(rctx, x, y, w, h, fmt, type, px);
}
static void hook_generatemipmap_census(void *rctx, unsigned target)
{
	cen_count_other(rctx, 2, sh_ctx_ok(rctx));
	((void (*)(void *, unsigned))cen_next_gm)(rctx, target);
}

/* research 90 L2: glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, w, h): log the first GLI_CEN_COPY_LOG calls with the read FBO, its status and the bound 2D texture, count all */
static void hook_copytexsubimage2d_census(void *rctx, unsigned target, int level, int xoff, int yoff, int x, int y, int w, int h)
{
	unsigned idx = __atomic_add_fetch(&cen_copytex, 1, __ATOMIC_RELAXED);
	if (idx <= GLI_CEN_COPY_LOG && sh_ctx_ok(rctx)) {
		int readfb = 0, bound = 0, dw = 0, dh = 0, dif = 0, has_dst = 0;
		unsigned st = 0;
		char l[320];
		dbg_install_ctx(rctx);
		_mesa_GetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &readfb);
		if (readfb != 0) st = _mesa_CheckFramebufferStatus(0x8CA8 /* READ_FRAMEBUFFER */);
		if (target == GLI_TEXTURE_2D) {
			_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
			if (bound > 0) {
				_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1000 /* WIDTH */, &dw);
				_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1001 /* HEIGHT */, &dh);
				_mesa_GetTexLevelParameteriv(GLI_TEXTURE_2D, 0, 0x1003 /* INTERNAL_FORMAT */, &dif);
			}
			has_dst = 1;
		}
		sweet_copytex_line(l, sizeof l, idx, target, level, xoff, yoff, x, y, w, h, readfb, readfb != 0, st, has_dst, bound, dw, dh, (unsigned)dif);
		fprintf(stderr, "%s\n", l);
	}
	((void (*)(void *, unsigned, int, int, int, int, int, int, int))cen_next_cts)(rctx, target, level, xoff, yoff, x, y, w, h);
}

/* per-texture history hooks (installed by census_install after the others, so each calls whatever was installed before: the rect BindTexture hook, hook_teximage2d).
 * Slots [read gli_slots.txt]: glBindTexture 5, glCopyTexImage2D 54, glDeleteTextures 59, glGenTextures 98, glTexImage2D 301, glFramebufferTexture2D 677, glTexStorage2D 1031.
 * Queries only: glGetIntegerv(TEXTURE_BINDING_2D 0x8069 / FRAMEBUFFER_BINDING 0x8CA6 / READ_FRAMEBUFFER_BINDING 0x8CAA), never glGetError. The target must be 0xde1 to name
 * the texture (other targets, no current context or nothing bound are counted as `unattributed` in the stats line). */
#define GLI_SLOT_GENTEXTURES 98
#define GLI_SLOT_DELETETEXTURES 59
#define GLI_SLOT_COPYTEXIMAGE2D 54
#define GLI_SLOT_FRAMEBUFFERTEXTURE2D 677
#define GLI_SLOT_BINDFRAMEBUFFER 672
#define GLI_SLOT_FINISH 89     /* gli_slots.txt:36 */
#define GLI_SLOT_FLUSH 90      /* gli_slots.txt:37 */
#define GLI_SLOT_TEXSTORAGE2D 1031
static const void *th_next_gen, *th_next_del, *th_next_bind, *th_next_ti, *th_next_ts, *th_next_ci, *th_next_fbt;

static unsigned th_bound2d(void *rctx, unsigned target)
{
	int b = 0;
	if (target != GLI_TEXTURE_2D || !sh_ctx_ok(rctx)) { sweet_th_unattributed(); return 0; }
	_mesa_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &b);
	if (b <= 0) { sweet_th_unattributed(); return 0; }
	return (unsigned)b;
}
/* names[] of n textures: n outside 1..256 or a NULL pointer is not read (n > 256 is counted as unattributed: no silent cap); name 0 is never a texture */
static void th_names(void *rctx, int kind, int n, const unsigned *names)
{
	int ctx = sweet_th_ctx_index(rctx);
	if (!names || n <= 0) return;
	if (n > 256) { sweet_th_unattributed(); return; }
	for (int i = 0; i < n; i++) {
		struct sweet_th_ev e = { 0 };
		if (!names[i]) continue;
		e.kind = kind; e.ctx = ctx;
		sweet_th_record(names[i], e);
	}
}
static void hook_gentextures_th(void *rctx, int n, unsigned *names)
{
	int ok = sh_ctx_ok(rctx);   /* the wrapper bails out without writing names[] when there is no current context: do not read it then */
	((void (*)(void *, int, unsigned *))th_next_gen)(rctx, n, names);
	if (ok) th_names(rctx, SWEET_TH_GEN, n, names);   /* after the original: names[] is written by it */
}
static void hook_deletetextures_th(void *rctx, int n, const unsigned *names)
{
	th_names(rctx, SWEET_TH_DELETED, n, names);
	((void (*)(void *, int, const unsigned *))th_next_del)(rctx, n, names);
}
static void hook_bindtexture_th(void *rctx, unsigned target, unsigned tex)
{
	if (tex) { struct sweet_th_ev e = { 0 }; e.kind = SWEET_TH_BIND; e.a = target; e.ctx = sweet_th_ctx_index(rctx); sweet_th_record(tex, e); }
	sweet_tu_bind(cen_ctx(rctx), target, tex);   /* part A: unit tracking rides on this (the only) BindTexture census hook */
	((void (*)(void *, unsigned, unsigned))th_next_bind)(rctx, target, tex);
}
static void hook_teximage2d_th(void *rctx, unsigned target, int level, int ifmt, int w, int h, int border, unsigned fmt, unsigned type, const void *px)
{
	unsigned b = th_bound2d(rctx, target);
	if (b) { struct sweet_th_ev e = { 0 }; e.kind = SWEET_TH_TEXIMAGE; e.ctx = sweet_th_ctx_index(rctx); e.a = target; e.b = level; e.c = w; e.d = h; e.e = (unsigned)ifmt; e.f = px != NULL; sweet_th_record(b, e); }
	((void (*)(void *, unsigned, int, int, int, int, int, unsigned, unsigned, const void *))th_next_ti)(rctx, target, level, ifmt, w, h, border, fmt, type, px);
}
static void hook_texstorage2d_th(void *rctx, unsigned target, int levels, unsigned ifmt, int w, int h)
{
	unsigned b = th_bound2d(rctx, target);
	if (b) { struct sweet_th_ev e = { 0 }; e.kind = SWEET_TH_TEXSTORAGE; e.ctx = sweet_th_ctx_index(rctx); e.a = target; e.b = levels; e.c = w; e.d = h; e.e = ifmt; sweet_th_record(b, e); }
	((void (*)(void *, unsigned, int, unsigned, int, int))th_next_ts)(rctx, target, levels, ifmt, w, h);
}
static void hook_copyteximage2d_th(void *rctx, unsigned target, int level, unsigned ifmt, int x, int y, int w, int h, int border)
{
	unsigned b = th_bound2d(rctx, target);
	if (b) { struct sweet_th_ev e = { 0 }; e.kind = SWEET_TH_COPYIMAGE; e.ctx = sweet_th_ctx_index(rctx); e.a = target; e.b = level; e.c = w; e.d = h; e.e = ifmt; sweet_th_record(b, e); }
	cen_count_other(rctx, 4, sh_ctx_ok(rctx));   /* census extension */
	((void (*)(void *, unsigned, int, unsigned, int, int, int, int, int))th_next_ci)(rctx, target, level, ifmt, x, y, w, h, border);
}
static void hook_framebuffertexture2d_th(void *rctx, unsigned target, unsigned att, unsigned textarget, unsigned tex, int level)
{
	int ok = sh_ctx_ok(rctx);   /* one check for the history query and the class counter */
	if (tex) {
		struct sweet_th_ev e = { 0 };
		int fbo = -1;
		if ((target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA8 /* READ_FRAMEBUFFER */) && ok)
			_mesa_GetIntegerv(target == 0x8D40 ? 0x8CA6 : 0x8CAA, &fbo);
		e.kind = SWEET_TH_FBOTEX; e.ctx = sweet_th_ctx_index(rctx); e.a = att; e.b = level; e.c = fbo; e.e = textarget;
		sweet_th_record(tex, e);
	}
	cen_count_other(rctx, 3, ok);   /* census extension: every call, detach included */
	((void (*)(void *, unsigned, unsigned, unsigned, unsigned, int))th_next_fbt)(rctx, target, att, textarget, tex, level);
}

void sweet_th_note_attach(unsigned tex, unsigned surf_id, unsigned w, unsigned h, unsigned fmt)
{
	struct sweet_th_ev e = { 0 };
	if (!census_on || !tex) return;
	e.kind = SWEET_TH_ATTACH; e.a = surf_id; e.c = (int)w; e.d = (int)h; e.e = fmt;
	sweet_th_record(tex, e);
}

/* one line every 20 s from a detached thread started once (research 90 L1) */
/* the 20 s thread tick: ends a timed-out round without a draw (exported so the host test can call it; the thread's call of it is one line and is not host-testable) */
void sweet_px_census_tick(void)
{
	struct sweet_px_rinfo ri;
	sweet_px_round_tick(px_now(), &ri);
	px_print_exp(&ri);
}

static void *census_thread(void *arg)
{
	unsigned round = 0;
	(void)arg;
	for (;;) {
		unsigned long c[6][3], cp, c2[5][3];
		unsigned dsq;
		char l[768];
		sleep(20);
		sweet_px_census_tick();
		sweet_census_snapshot(c, &cp);
		sweet_census_snapshot2(c2, &dsq);
		sweet_census_line_full(l, sizeof l, ++round * 20, c, cp, c2, dsq);
		fprintf(stderr, "%s\n", l);
		sweet_px_stats_line(l, sizeof l);
		fprintf(stderr, "%s\n", l);
		sweet_th_stats_line(l, sizeof l);   /* a silent cap must show (rule: no silent caps) */
		fprintf(stderr, "%s\n", l);
		sweet_th_fbo_report();
	}
	return NULL;
}

static const void *cen_next_at, *cen_next_bf, *cen_next_en, *cen_next_di;
static void cen_blend_note(void *rctx, unsigned cap, int on)
{
	int bi;
	if (cap != 0x0BE2 /* GL_BLEND */) return;
	pthread_mutex_lock(&ub_mu);
	bi = blend_find_locked(rctx, 1);
	if (bi >= 0) blend_v[bi] = (unsigned char)on;
	pthread_mutex_unlock(&ub_mu);
}
static void hook_enable_census(void *rctx, unsigned cap) { cen_blend_note(rctx, cap, 1); ((void (*)(void *, unsigned))cen_next_en)(rctx, cap); }
static void hook_disable_census(void *rctx, unsigned cap) { cen_blend_note(rctx, cap, 0); ((void (*)(void *, unsigned))cen_next_di)(rctx, cap); }
static void hook_activetexture_census(void *rctx, unsigned unit)
{
	sweet_tu_active(cen_ctx(rctx), unit);
	((void (*)(void *, unsigned))cen_next_at)(rctx, unit);
}
/* part B: ONE 5-point sample of FBO f (still bound: this runs BEFORE the original BindFramebuffer), through cen_px_read (same gates, sentinel, original ReadPixels dispatch) */
static void cen_ub_sample(void *rctx, int f, const int vp[4], const struct sweet_capdraws *cd)
{
	struct sweet_px_rinfo ri;
	struct cen_dr d;
	char l[96], l2[256], l3[256];
	int got = sweet_px_unbind_claim(px_now(), &ri);
	px_print_exp(&ri);
	if (!got) return;
	memset(&d, 0, sizeof d);
	d.ok = 1; d.px = 3; d.cls = sweet_census_class(vp[2], vp[3]);   /* eligibility needs w >= 1000, and class 0 = S is exactly w >= 1000 (sweet_census_class), so the letter is always S; derived, not a constant (F4) */
	 d.five = 1; d.fbo = f; d.round = ri.round; memcpy(d.vp, vp, sizeof d.vp);
	dbg_install_ctx(rctx);
	if (!cen_px_read(rctx, &d)) sweet_px_unbind_rollback(ri.round, &ri.cap_u);
	else { sweet_capdraws_line(l2, sizeof l2, ri.round, f, cd); fprintf(stderr, "%s\n", l2); sweet_capvp_line(l3, sizeof l3, ri.round, f, cd); fprintf(stderr, "%s\n", l3); }   /* after the sample line and the grid line; capvp right after capdraws */
	if (ri.cap_u) { sweet_px_unbind_cap_line(l, sizeof l); fprintf(stderr, "%s\n", l); }   /* after the read whether or not a gate skipped it */
}
static void hook_bindframebuffer_census(void *rctx, unsigned target, unsigned fbo)
{
	if (target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA9 /* DRAW_FRAMEBUFFER */) {
		int f, vp[4], changed, c = cen_ctx(rctx);
		unsigned dr;
		struct sweet_capdraws cd;
		pthread_mutex_lock(&ub_mu);
		f = ub_c[c].fbo; dr = ub_c[c].draws; memcpy(vp, ub_c[c].vp, sizeof vp); cd = ub_c[c].cd;
		changed = (unsigned)f != fbo;
		if (changed) { ub_c[c].fbo = (int)fbo; ub_c[c].draws = 0; ub_c[c].capv_n = 0; ub_c[c].capv_e = 0; memset(&ub_c[c].cd, 0, sizeof ub_c[c].cd); }   /* capdraws restarts at every bind */
		pthread_mutex_unlock(&ub_mu);
		if (changed && sweet_px_sched_on() && sweet_ub_eligible(f, dr, vp[2], vp[3]) && sh_ctx_ok(rctx)) cen_ub_sample(rctx, f, vp, &cd);
	}
	((void (*)(void *, unsigned, unsigned))cen_next_bf)(rctx, target, fbo);
}

static void census_install(struct sweet_gli *t)
{
	static int started;
	pthread_t th;
	cen_next_ss = t->disp[GLI_SLOT_SHADERSOURCE]; cen_next_cp = t->disp[GLI_SLOT_CREATEPROGRAM]; cen_next_as = t->disp[GLI_SLOT_ATTACHSHADER];
	cen_next_da = t->disp[GLI_SLOT_DRAWARRAYS]; cen_next_de = t->disp[GLI_SLOT_DRAWELEMENTS]; cen_next_dre = t->disp[405]; cen_next_dai = t->disp[762]; cen_next_dei = t->disp[763];
	cen_next_cts = t->disp[GLI_SLOT_COPYTEXSUBIMAGE2D];
	cen_next_at = t->disp[GLI_SLOT_ACTIVETEXTURE]; cen_next_bf = t->disp[GLI_SLOT_BINDFRAMEBUFFER];
	cen_next_en = t->disp[72]; cen_next_di = t->disp[63];   /* glEnable / glDisable (gli_slots.txt:34, :30): GL_BLEND tracking for the capdraws line */
	t->disp[72] = (const void *)hook_enable_census;
	t->disp[63] = (const void *)hook_disable_census;
	t->disp[GLI_SLOT_ACTIVETEXTURE] = (const void *)hook_activetexture_census;
	t->disp[GLI_SLOT_BINDFRAMEBUFFER] = (const void *)hook_bindframebuffer_census;
	cen_next_rp = t->disp[237]; cen_next_bl = t->disp[755]; cen_next_gm = t->disp[681];
	t->disp[237] = (const void *)hook_readpixels_census;
	t->disp[755] = (const void *)hook_blit_census;
	t->disp[681] = (const void *)hook_generatemipmap_census;
	t->disp[GLI_SLOT_SHADERSOURCE] = (const void *)hook_shadersource_census;
	t->disp[GLI_SLOT_CREATEPROGRAM] = (const void *)hook_createprogram_census;
	t->disp[GLI_SLOT_ATTACHSHADER] = (const void *)hook_attachshader_census;
	t->disp[GLI_SLOT_DRAWARRAYS] = (const void *)hook_drawarrays_census;
	t->disp[GLI_SLOT_DRAWELEMENTS] = (const void *)hook_drawelements_census;
	t->disp[405] = (const void *)hook_drawrangeelements_census;
	t->disp[762] = (const void *)hook_drawarraysinstanced_census;
	t->disp[763] = (const void *)hook_drawelementsinstanced_census;
	t->disp[GLI_SLOT_COPYTEXSUBIMAGE2D] = (const void *)hook_copytexsubimage2d_census;
	th_next_gen = t->disp[GLI_SLOT_GENTEXTURES]; th_next_del = t->disp[GLI_SLOT_DELETETEXTURES]; th_next_bind = t->disp[GLI_SLOT_BINDTEXTURE];
	th_next_ti = t->disp[GLI_SLOT_TEXIMAGE2D]; th_next_ts = t->disp[GLI_SLOT_TEXSTORAGE2D]; th_next_ci = t->disp[GLI_SLOT_COPYTEXIMAGE2D]; th_next_fbt = t->disp[GLI_SLOT_FRAMEBUFFERTEXTURE2D];
	t->disp[GLI_SLOT_GENTEXTURES] = (const void *)hook_gentextures_th;
	t->disp[GLI_SLOT_DELETETEXTURES] = (const void *)hook_deletetextures_th;
	t->disp[GLI_SLOT_BINDTEXTURE] = (const void *)hook_bindtexture_th;
	t->disp[GLI_SLOT_TEXIMAGE2D] = (const void *)hook_teximage2d_th;
	t->disp[GLI_SLOT_TEXSTORAGE2D] = (const void *)hook_texstorage2d_th;
	t->disp[GLI_SLOT_COPYTEXIMAGE2D] = (const void *)hook_copyteximage2d_th;
	t->disp[GLI_SLOT_FRAMEBUFFERTEXTURE2D] = (const void *)hook_framebuffertexture2d_th;
	if (!__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL) && pthread_create(&th, NULL, census_thread, NULL) == 0) pthread_detach(th);
}

static void hist_install(struct sweet_gli *t)
{
#ifdef SGL_HAVE_TRAMP
	static int started;
	int i;
	for (i = 0; i < SWEET_GLI_NSLOTS; i++) { sgl_orig[i] = t->disp[i]; t->disp[i] = sgl_tramps[i]; }
	if (!started) {
		pthread_t th;
		started = 1;
		if (pthread_create(&th, NULL, hist_thread, NULL) == 0) pthread_detach(th);
	}
#else
	(void)t;
#endif
}

/* ---- flush timing (P-G0 slice c, research 113 s.7.2; log-only; env SWEETGL_FLUSHT=1, default off) ----
 * glFlush (slot 90) / glFinish (slot 89) are wrapped: the next function runs unchanged, only two clock reads and a counter update around it. No GL call, no
 * glGetError. One line per second per process, printed lazily by the first call of the NEXT second (no thread): the last window of a quiet process is not printed.
 * Clock = mach_absolute_time ticks * mach_timebase_info (numer/denom) = ns; the same counter the kernel uses (kernel: absolutetime_to_nanoseconds(mach_absolute_time()),
 * kernel patch 0141; userland arm64 reads CNTVCT, [read from source] xnu-pristine libsyscall/wrappers/mach_absolute_time.s:177-184). Host: CLOCK_MONOTONIC ns.
 * Gate: env var SWEETGL_FLUSHT, delivered by the sweetenv boot-arg like SWEETGL_CENSUS (kernel patch 0110): boot-arg sweetenv=backboardd:...,SWEETGL_FLUSHT=1.
 * Boot-arg sweetfps=1 is NOT this knob: it is kernel patch 0141 (frame stats), the dylib cannot read boot-args. */
static unsigned long long (*ft_clock_fn)(void);
static unsigned ft_numer = 1, ft_denom = 1;
static int ft_on, ft_loud_done;
static pthread_mutex_t ft_mu = PTHREAD_MUTEX_INITIALIZER;
static int ft_have;
static unsigned long long ft_base;
static unsigned ft_cur;
static struct sweet_ft_win ft_win;
static const void *ft_next_fl, *ft_next_fi;
int sweet_ft_tb_fail;   /* host test: mach_timebase_info "fails" */

void sweet_ft_set_clock(unsigned long long (*fn)(void)) { ft_clock_fn = fn; }
void sweet_ft_reset(void) { pthread_mutex_lock(&ft_mu); ft_have = 0; ft_base = 0; ft_cur = 0; memset(&ft_win, 0, sizeof ft_win); pthread_mutex_unlock(&ft_mu); ft_on = 0; ft_loud_done = 0; ft_numer = ft_denom = 1; sweet_ft_tb_fail = 0; }
int sweet_ft_enabled(void) { return ft_on; }
/* ticks -> ns without overflowing the product for realistic uptimes */
unsigned long long sweet_ft_ticks_to_ns(unsigned long long t, unsigned numer, unsigned denom)
{
	if (denom == 0) return 0;
	return (t / denom) * numer + (t % denom) * numer / denom;
}
static unsigned long long ft_now(void)
{
	if (ft_clock_fn) return ft_clock_fn();
#ifdef __APPLE__
	return sweet_ft_ticks_to_ns(mach_absolute_time(), ft_numer, ft_denom);
#else
	{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec; }
#endif
}
/* "1" = on; unset, "" and "0" = off; anything else = bad value (off, *bad = 1) */
int sweet_ft_parse(const char *e, int *bad)
{
	*bad = 0;
	if (!e || !e[0] || (e[0] == '0' && !e[1])) return 0;
	if (e[0] == '1' && !e[1]) return 1;
	*bad = 1;
	return 0;
}
static unsigned ft_sat32(unsigned long long v) { return v > 0xffffffffull ? 0xffffffffu : (unsigned)v; }
static unsigned long long ft_sat_add(unsigned long long a, unsigned long long b) { return a + b < a ? ~0ull : a + b; }
int sweet_ft_line(char *buf, size_t n, int pid, unsigned sec, const struct sweet_ft_win *w)
{
	return snprintf(buf, n, "SWEETGL: FLUSHT pid %d sec %u flush %u flush_in_us %llu flush_max_us %u finish %u finish_in_us %llu finish_max_us %u last_exit_ns %llu",
		pid, sec, w->flush, w->flush_in_ns / 1000, ft_sat32(w->flush_max_ns / 1000), w->finish, w->finish_in_ns / 1000, ft_sat32(w->finish_max_ns / 1000), w->last_exit_ns);
}
/* Records one call (which 0 = flush, 1 = finish) of [t0, t1] ns. When the call falls in a later second than the open window, the open window is copied to
 * *done / *done_sec and 1 is returned (the caller prints it); 0 otherwise. Clock steps backwards: duration 0, a late exit is counted in the open window. */
int sweet_ft_note(int which, unsigned long long t0, unsigned long long t1, struct sweet_ft_win *done, unsigned *done_sec)
{
	unsigned long long dur = t1 > t0 ? t1 - t0 : 0, w;
	unsigned wid;
	int out = 0;
	pthread_mutex_lock(&ft_mu);
	if (!ft_have) { ft_have = 1; ft_base = t1; ft_cur = 0; }
	w = t1 > ft_base ? (t1 - ft_base) / 1000000000ull : 0;
	wid = w > 0xffffffffull ? 0xffffffffu : (unsigned)w;
	if (wid < ft_cur) wid = ft_cur;
	if (wid != ft_cur) {
		if (ft_win.flush || ft_win.finish) { *done = ft_win; *done_sec = ft_cur; out = 1; }
		memset(&ft_win, 0, sizeof ft_win);
		ft_cur = wid;
	}
	if (which == 0) {
		if (ft_win.flush != 0xffffffffu) ft_win.flush++;
		ft_win.flush_in_ns = ft_sat_add(ft_win.flush_in_ns, dur);
		if (dur > ft_win.flush_max_ns) ft_win.flush_max_ns = dur;
	} else {
		if (ft_win.finish != 0xffffffffu) ft_win.finish++;
		ft_win.finish_in_ns = ft_sat_add(ft_win.finish_in_ns, dur);
		if (dur > ft_win.finish_max_ns) ft_win.finish_max_ns = dur;
	}
	if (t1 > ft_win.last_exit_ns) ft_win.last_exit_ns = t1;
	pthread_mutex_unlock(&ft_mu);
	return out;
}
static void ft_record(int which, unsigned long long t0, unsigned long long t1)
{
	struct sweet_ft_win d; unsigned sec; char l[256];
	if (sweet_ft_note(which, t0, t1, &d, &sec)) { sweet_ft_line(l, sizeof l, (int)getpid(), sec, &d); fprintf(stderr, "%s\n", l); }
}
/* T-9577 LAGPH: defined in sweet_drm_mali.c (inside libsweetgl); weak so the gli host tests, which do not link it, still build. Log-only, off unless SWEETGL_LAGPH=1. */
extern void sweet_lagph_flush_exit(void) __attribute__((weak));
static void hook_flush_ft(void *rctx)
{
	unsigned long long t0 = ft_now(), t1;
	((void (*)(void *))ft_next_fl)(rctx);
	t1 = ft_now();
	ft_record(0, t0, t1);
	if (sweet_lagph_flush_exit) sweet_lagph_flush_exit();
}
static void hook_finish_ft(void *rctx)
{
	unsigned long long t0 = ft_now(), t1;
	((void (*)(void *))ft_next_fi)(rctx);
	t1 = ft_now();
	ft_record(1, t0, t1);
	if (sweet_lagph_flush_exit) sweet_lagph_flush_exit();
}
/* Reads the knob and the timebase; returns whether the hooks are installed. A knob that is set but unusable logs ONE loud line per process and leaves timing off. */
int sweet_ft_init(const char *e)
{
	int bad, on = sweet_ft_parse(e, &bad);
	if (bad && !ft_loud_done) { ft_loud_done = 1; fprintf(stderr, "SWEETGL: FLUSHT unavailable: SWEETGL_FLUSHT=\"%.16s\" is not 0 or 1, flush timing OFF\n", e); }
	if (on) {
#ifdef __APPLE__
		mach_timebase_info_data_t tb;
		if (sweet_ft_tb_fail || mach_timebase_info(&tb) != 0 || tb.denom == 0) on = 0; else { ft_numer = tb.numer; ft_denom = tb.denom; }
#else
		if (sweet_ft_tb_fail) on = 0;
#endif
		if (!on && !ft_loud_done) { ft_loud_done = 1; fprintf(stderr, "SWEETGL: FLUSHT unavailable: mach_timebase_info failed, flush timing OFF\n"); }
	}
	ft_on = on;
	return on;
}
/* ---- real flush (LOWCLK lever real-flush; default OFF = glFlush stays glFinish, the table is untouched) ----
 * Why glFlush was a glFinish [read DESIGN.md s.4, eagl_core.h:43, kernel docs/research/33 s.3 item 4]: the kernel panel copy (SweetFramebuffer present(), a CPU
 * read of the display IOSurface) runs after IOMobileFramebufferSwapEnd with no GPU fence, so the frame had to be complete when glFlush returned. GL-internal
 * ordering never needed it: every job goes through ONE in-order kernel queue (sweet_drm_mali.c:1928 "in_syncs are ignored ... one in-order queue"), and Mesa's
 * own CPU reads wait by themselves (WAIT_BO -> wait_seq). CoreAnimation calls glFlush once per frame and never glFinish [obs boot 20261113T071232Z b5: FLUSHT
 * flush 10830 finish 0], so that wait serialised CPU and GPU: 4.0 ms mean per flush at 421 MHz memory [obs same boot].
 * With kernel patch 0343 (boot-arg sweetfb_gate=1) the panel copy itself waits for the swap's GPU jobs, and glFlush can be Mesa's real flush (submit, no wait).
 * Arming (process start): env SWEETGL_REALFLUSH = arm | 1 | 2 (boot-arg sweetenv, like SWEETGL_FLUSHT), or the switch file exists. Not armed = no hook at all.
 * Switch (runtime, A/B inside one boot): file /private/var/dev/sweetgl-realflush, first byte 0 | 1 | 2, re-read at most once per second from the flush path:
 *   0 = glFinish (as before)   1 = real flush when the draw framebuffer's colour target is a display surface ('b3a8'), glFinish otherwise
 *   2 = real flush for every glFlush.   No file = the env's mode (arm = 0).   glFinish (slot 89) and the APPLE object fences always wait.
 * Mode 1/2 take effect only while sysctl kern.sweetfbgate reads 1 (the kernel gate is on); otherwise ONE loud REFUSED line per change and glFinish.
 * Not covered by the gate [guess, no instance known]: a CPU reader of a NON-display IOSurface that the GPU rendered (snapshots handed to another process);
 * mode 1 keeps the wait for those targets, mode 2 does not.
 * Lines: `SWEETGL: REALFLUSH armed ...` once; `SWEETGL: REALFLUSH pid P mode M (...) want W src S real R finish F nodisp N` on every change of mode and, while the
 * mode is not 0, once per second with moving counts (sums since process start: real = non-blocking flushes, finish = waited in mode 0, nodisp = waited in
 * mode 1 because the target is not a display surface); `SWEETGL: REALFLUSH REFUSED ...`; `SWEETGL: REALFLUSH unavailable: ...` for a bad env / file value. */
#define RF_FILE_DEFAULT "/private/var/dev/sweetgl-realflush"
#define RF_POLL_NS 1000000000ull
extern int sweet_mesa_draw_is_display(void) __attribute__((weak));   /* eagl_mesa.c (inside libsweetgl); the gli host tests do not link it */
static const char *rf_file = RF_FILE_DEFAULT;
static int (*rf_gate_fn)(void), (*rf_disp_fn)(void);
static unsigned long long (*rf_clock_fn)(void);
static pthread_mutex_t rf_mu = PTHREAD_MUTEX_INITIALIZER;
static int rf_armed, rf_def_mode, rf_mode, rf_state = -1, rf_env_loud, rf_armed_said, rf_nodisp_said;   /* rf_state: last printed (want << 8 | refused << 4 | bad << 2 | src) */
static unsigned long long rf_next_poll, rf_n_real, rf_n_finish, rf_n_nodisp, rf_said_sum;
static const void *rf_next_fl;

void sweet_rf_set_file(const char *path) { rf_file = path ? path : RF_FILE_DEFAULT; }
void sweet_rf_set_gate(int (*fn)(void)) { rf_gate_fn = fn; }
void sweet_rf_set_disp(int (*fn)(void)) { rf_disp_fn = fn; }
void sweet_rf_set_clock(unsigned long long (*fn)(void)) { rf_clock_fn = fn; }
void sweet_rf_reset(void)
{
	pthread_mutex_lock(&rf_mu);
	rf_armed = rf_def_mode = 0; rf_state = -1; rf_env_loud = rf_armed_said = rf_nodisp_said = 0; rf_next_poll = 0; rf_said_sum = 0;
	__atomic_store_n(&rf_mode, 0, __ATOMIC_RELEASE); __atomic_store_n(&rf_n_real, 0, __ATOMIC_RELEASE); __atomic_store_n(&rf_n_finish, 0, __ATOMIC_RELEASE); __atomic_store_n(&rf_n_nodisp, 0, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&rf_mu);
	rf_file = RF_FILE_DEFAULT; rf_gate_fn = NULL; rf_disp_fn = NULL; rf_clock_fn = NULL;
}
int sweet_rf_mode(void) { return __atomic_load_n(&rf_mode, __ATOMIC_ACQUIRE); }
void sweet_rf_counts(unsigned long long *real, unsigned long long *finish, unsigned long long *nodisp)
{
	*real = __atomic_load_n(&rf_n_real, __ATOMIC_ACQUIRE); *finish = __atomic_load_n(&rf_n_finish, __ATOMIC_ACQUIRE); *nodisp = __atomic_load_n(&rf_n_nodisp, __ATOMIC_ACQUIRE);
}
int sweet_rf_parse(const char *e, int *armed, int *def_mode)
{
	*armed = 0; *def_mode = 0;
	if (!e || !e[0] || (e[0] == '0' && !e[1])) return 0;
	if (!strcmp(e, "arm")) { *armed = 1; return 0; }
	if ((e[0] == '1' || e[0] == '2') && !e[1]) { *armed = 1; *def_mode = e[0] - '0'; return 0; }
	return 1;
}
static unsigned long long rf_now(void)
{
	struct timespec ts;
	if (rf_clock_fn) return rf_clock_fn();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
/* 1 = kernel present gate on (kern.sweetfbgate, kernel patch 0343); 0 = off, absent or unreadable */
static int rf_gate(void)
{
	if (rf_gate_fn) return rf_gate_fn();
#ifdef __APPLE__
	{
		extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);   /* <sys/sysctl.h>; as fcap_impl.c reads kern.sweetnavcap */
		int v = 0; size_t sz = sizeof v;
		if (sysctlbyname("kern.sweetfbgate", &v, &sz, NULL, 0) != 0 || sz != sizeof v) return 0;
		return v == 1;
	}
#else
	return 0;
#endif
}
/* the switch file: -1 = no file, 0 / 1 / 2, -2 = unreadable value (*bad_ch = its first byte, -1 = empty) */
static int rf_read_file(int *bad_ch)
{
	FILE *f = fopen(rf_file, "r");
	int ch;
	if (!f) return -1;
	ch = fgetc(f);
	fclose(f);
	if (ch == '0' || ch == '1' || ch == '2') return ch - '0';
	*bad_ch = ch;
	return -2;
}
static const char *rf_mode_text(int m) { return m == 0 ? "glFlush = glFinish" : m == 1 ? "real flush on display targets, glFinish elsewhere" : "real flush for every glFlush"; }
/* rf_mu held: read the switch, decide the effective mode, print what changed */
static void rf_poll_locked(void)
{
	int bad_ch = 0, f = rf_read_file(&bad_ch), src = f == -1 ? 0 : 1, bad = f == -2, want = f == -1 ? rf_def_mode : bad ? 0 : f, eff = want, refused = 0, state;
	unsigned long long r, fi, nd;
	if (want != 0 && rf_gate() != 1) { eff = 0; refused = 1; }
	state = want << 8 | refused << 4 | bad << 2 | src;
	sweet_rf_counts(&r, &fi, &nd);
	if (state != rf_state) {
		if (bad) {
			if (bad_ch < 0) fprintf(stderr, "SWEETGL: REALFLUSH unavailable: %s is empty (want 0, 1 or 2): glFlush stays glFinish\n", rf_file);
			else fprintf(stderr, "SWEETGL: REALFLUSH unavailable: %s starts with byte 0x%02x (want 0, 1 or 2): glFlush stays glFinish\n", rf_file, (unsigned)bad_ch & 0xff);
		}
		if (refused) fprintf(stderr, "SWEETGL: REALFLUSH REFUSED pid %d want %d: sysctl kern.sweetfbgate is not 1 (kernel present gate: patch 0343 + boot-arg sweetfb_gate=1): glFlush stays glFinish\n", (int)getpid(), want);
		fprintf(stderr, "SWEETGL: REALFLUSH pid %d mode %d (%s) want %d src %s real %llu finish %llu nodisp %llu\n", (int)getpid(), eff, rf_mode_text(eff), want, src ? "file" : "env", r, fi, nd);
		rf_state = state; rf_said_sum = r + fi + nd;
	} else if (eff != 0 && r + fi + nd != rf_said_sum) {
		fprintf(stderr, "SWEETGL: REALFLUSH pid %d mode %d real %llu finish %llu nodisp %llu\n", (int)getpid(), eff, r, fi, nd);
		rf_said_sum = r + fi + nd;
	}
	__atomic_store_n(&rf_mode, eff, __ATOMIC_RELEASE);
}
static int rf_mode_now(void)
{
	unsigned long long now = rf_now();
	if (now >= __atomic_load_n(&rf_next_poll, __ATOMIC_ACQUIRE) && pthread_mutex_trylock(&rf_mu) == 0) {
		if (now >= rf_next_poll) { __atomic_store_n(&rf_next_poll, now + RF_POLL_NS, __ATOMIC_RELEASE); rf_poll_locked(); }
		pthread_mutex_unlock(&rf_mu);
	}
	return __atomic_load_n(&rf_mode, __ATOMIC_ACQUIRE);
}
static void hook_flush_rf(void *rctx)
{
	int m = rf_mode_now();
	if (m == 0) { __atomic_fetch_add(&rf_n_finish, 1, __ATOMIC_RELAXED); ((void (*)(void *))rf_next_fl)(rctx); return; }
	if (m == 1) {
		int (*df)(void) = rf_disp_fn ? rf_disp_fn : sweet_mesa_draw_is_display;
		int d = 0;
		if (!df) { if (!__atomic_exchange_n(&rf_nodisp_said, 1, __ATOMIC_ACQ_REL)) sweet_gap("realflush", "mode 1 without a display test in this build: every glFlush waits"); }
		else if (!sweet_gli_ensure_current || sweet_gli_ensure_current(rctx) == 0) d = df();   /* the test reads the CURRENT context's draw framebuffer */
		if (d != 1) { __atomic_fetch_add(&rf_n_nodisp, 1, __ATOMIC_RELAXED); ((void (*)(void *))rf_next_fl)(rctx); return; }
	}
	__atomic_fetch_add(&rf_n_real, 1, __ATOMIC_RELAXED);
	sgli_RealFlush(rctx);
}
/* Reads the arming knob; returns whether the hook is installed. A bad env value logs ONE loud line per process and does not arm. */
int sweet_rf_init(const char *e)
{
	int armed, def, bad = sweet_rf_parse(e, &armed, &def), by_file = 0;
	pthread_mutex_lock(&rf_mu);
	if (bad && !rf_env_loud) { rf_env_loud = 1; fprintf(stderr, "SWEETGL: REALFLUSH unavailable: SWEETGL_REALFLUSH=\"%.16s\" is not 0, arm, 1 or 2: not armed by the env\n", e); }
	if (!armed && access(rf_file, F_OK) == 0) { armed = 1; by_file = 1; }
	if (armed) {
		if (!rf_armed) rf_def_mode = def;
		rf_armed = 1;
		if (!rf_armed_said) {
			rf_armed_said = 1;
			fprintf(stderr, "SWEETGL: REALFLUSH armed pid %d by %s, default mode %d; switch file %s (0 glFinish, 1 real flush on display targets, 2 every glFlush), needs kern.sweetfbgate=1\n",
				(int)getpid(), by_file ? "file" : "env", rf_def_mode, rf_file);
		}
	}
	armed = rf_armed;
	pthread_mutex_unlock(&rf_mu);
	return armed;
}
static void rf_install(struct sweet_gli *t)
{
	rf_next_fl = sweet_gli_defaults[GLI_SLOT_FLUSH];   /* the generated entry (_mesa_Finish wrapper), whatever table this is */
	t->disp[GLI_SLOT_FLUSH] = (const void *)hook_flush_rf;
}
static void ft_install(struct sweet_gli *t)
{
	ft_next_fl = t->disp[GLI_SLOT_FLUSH]; ft_next_fi = t->disp[GLI_SLOT_FINISH];
	t->disp[GLI_SLOT_FLUSH] = (const void *)hook_flush_ft;
	t->disp[GLI_SLOT_FINISH] = (const void *)hook_finish_ft;
}

/* P-G1 slice 2: one `SWEETGL: BASE` line per process so the libsweetgl frames of an SMP backtrace can be symbolicated (image = mach header, slide, size of the file).
 * Apple resolver: dladdr(&sweet_gli_init) gives dli_fname + dli_fbase (the dylib's mach header; [guess, not read from a local man page]); the header is then looked up in
 * the dyld image list (_dyld_get_image_header / _dyld_get_image_vmaddr_slide, <mach-o/dyld.h>) so a wrong dli_fbase cannot yield a made-up slide: no match = unavailable.
 * Host: no dyld, the default resolver reports unavailable; tests inject a resolver. */
static int base_done;
static int base_resolve_default(struct sweet_base_info *bi, const char **why)
{
#ifdef __APPLE__
	Dl_info di; uint32_t i, n;
	if (!dladdr((const void *)sweet_gli_init, &di) || !di.dli_fname || !di.dli_fbase) { *why = "dladdr failed"; return 0; }
	n = _dyld_image_count();
	for (i = 0; i < n; i++) {
		if ((const void *)_dyld_get_image_header(i) == di.dli_fbase) {
			bi->image = (unsigned long long)(uintptr_t)di.dli_fbase; bi->slide = (unsigned long long)(intptr_t)_dyld_get_image_vmaddr_slide(i); bi->path = di.dli_fname;
			return 1;
		}
	}
	*why = "dladdr base not in the dyld image list"; return 0;
#else
	(void)bi; *why = "no dyld (host build)"; return 0;
#endif
}
static int (*base_resolver)(struct sweet_base_info *, const char **) = base_resolve_default;
void sweet_base_set_resolver(int (*fn)(struct sweet_base_info *, const char **)) { base_resolver = fn ? fn : base_resolve_default; }
void sweet_base_reset(void) { base_done = 0; base_resolver = base_resolve_default; }
int sweet_base_line(char *buf, size_t n, int pid, const struct sweet_base_info *bi, unsigned long long size)
{
	return snprintf(buf, n, "SWEETGL: BASE pid %d image 0x%llx slide 0x%llx size %llu path %.80s", pid, bi->image, bi->slide, size, bi->path ? bi->path : "?");
}
int sweet_base_unavailable_line(char *buf, size_t n, const char *why) { return snprintf(buf, n, "SWEETGL: BASE unavailable (%.80s)", why ? why : "?"); }
/* Returns 0 when already reported in this process, 1 = line printed, -1 = resolver failed (loud line), -2 = line printed with size 0 after a stat failure (plus a loud line). */
int sweet_base_report(void)
{
	struct sweet_base_info bi; const char *why = "?"; char l[256], w[64]; struct stat st; unsigned long long size = 0; int rc = 1;
	if (__atomic_exchange_n(&base_done, 1, __ATOMIC_ACQ_REL)) return 0;
	memset(&bi, 0, sizeof bi);
	if (!base_resolver(&bi, &why) || !bi.path) { sweet_base_unavailable_line(l, sizeof l, why); fprintf(stderr, "%s\n", l); return -1; }
	if (stat(bi.path, &st) == 0) size = (unsigned long long)st.st_size; else { rc = -2; snprintf(w, sizeof w, "stat failed errno %d, size 0", errno); }
	sweet_base_line(l, sizeof l, (int)getpid(), &bi, size); fprintf(stderr, "%s\n", l);
	if (rc == -2) { sweet_base_unavailable_line(l, sizeof l, w); fprintf(stderr, "%s\n", l); }
	return rc;
}

/* ALL-ON T-28: glFramebufferTexture2D always hooked: a format-0 ABGR8-swizzled attach texture that becomes a colour target is a render
 * target, its swizzle goes back to identity first (eagl_mesa.c sgl_abgr_on_fbo). */
extern void sgl_abgr_on_fbo(unsigned tex);
static const void *abgr_next_fbt;
static void hook_framebuffertexture2d_abgr(void *rctx, unsigned target, unsigned att, unsigned textarget, unsigned tex, int level)
{
	if (tex && sh_ctx_ok(rctx)) sgl_abgr_on_fbo(tex);
	((void (*)(void *, unsigned, unsigned, unsigned, unsigned, int))abgr_next_fbt)(rctx, target, att, textarget, tex, level);
}

void sweet_gli_init(struct sweet_gli *t, void *rctx)
{
	const char *e = getenv("SWEETGL_TRACE");
	sweet_base_report();   /* once per process, before anything else can crash */
	trace = e && e[0] >= '1';
	trace_lvl = e ? e[0] - '0' : 0;
	e = getenv("SWEETGL_SHADERS");
	shaders = e && e[0] == '1';
	e = getenv("SWEETGL_U1W");
	u1w = e && e[0] == '1';
	e = getenv("SWEETGL_RECT");
	rect = !(e && e[0] == '0');   /* default ON (research 76 s.3.3): CA's rect program does not link without the rewrite; SWEETGL_RECT=0 turns it off */
	e = getenv("SWEETGL_NAMEQ");
	nameq_on = !(e && e[0] == '0');
	{ static int said; if (!said) { said = 1; fprintf(stderr, "SWEETGL: NAMEQ %s (texture + framebuffer names held 10 s after delete; SWEETGL_NAMEQ=0 off)\n", nameq_on ? "on" : "OFF (GAP: CA may bind a reused name storage-less)"); } }   /* once per process (init runs per table) */
	e = getenv("SWEETGL_FBO");
	fbo_diag_on = e && e[0] == '1';
	e = getenv("SWEETGL_HIST");
	hist_on = e && e[0] == '1';
	e = getenv("SWEETGL_CENSUS");
	{ long arm = 0; census_on = sweet_census_parse(e, &arm); if (census_on) sweet_px_configure(arm); }   /* "1" = early samples only; N >= 2 = rounds armed N s after this init */
	t->rctx = rctx;
	memcpy(t->disp, sweet_gli_defaults, sizeof(t->disp));
	/* disp[] is indexed by slot (gli_table.h: { rctx; disp[1036] } with CA's offset 8 + 8 * slot) */
	orig_ps = t->disp[GLI_SLOT_PIXELSTOREI];
	orig_ti = t->disp[GLI_SLOT_TEXIMAGE2D];
	orig_tsi = t->disp[GLI_SLOT_TEXSUBIMAGE2D];
	t->disp[GLI_SLOT_PIXELSTOREI] = (const void *)hook_pixelstorei;
	t->disp[GLI_SLOT_TEXIMAGE2D] = (const void *)hook_teximage2d;
	t->disp[GLI_SLOT_TEXSUBIMAGE2D] = (const void *)hook_texsubimage2d;
	if (trace) {   /* only with SWEETGL_TRACE: without it the table stays exactly as generated for these slots (review gli-upload-trace) */
		orig_cti = t->disp[GLI_SLOT_COMPRESSEDTEXIMAGE2D]; orig_ctsi = t->disp[GLI_SLOT_COMPRESSEDTEXSUBIMAGE2D];
		t->disp[GLI_SLOT_COMPRESSEDTEXIMAGE2D] = (const void *)hook_compressedteximage2d;
		t->disp[GLI_SLOT_COMPRESSEDTEXSUBIMAGE2D] = (const void *)hook_compressedtexsubimage2d;
	}
	if (shaders) {   /* diagnostic hooks only with SWEETGL_SHADERS=1; without it the table is exactly the generated one */
		orig_ss = t->disp[GLI_SLOT_SHADERSOURCE];
		orig_cs = t->disp[GLI_SLOT_COMPILESHADER];
		orig_lp = t->disp[GLI_SLOT_LINKPROGRAM];
		orig_gpil = t->disp[GLI_SLOT_GETPROGRAMINFOLOG];
		orig_pb = t->disp[GLI_SLOT_PROGRAMBINARY];
		t->disp[GLI_SLOT_SHADERSOURCE] = (const void *)hook_shadersource;
		t->disp[GLI_SLOT_COMPILESHADER] = (const void *)hook_compileshader;
		t->disp[GLI_SLOT_LINKPROGRAM] = (const void *)hook_linkprogram;
		t->disp[GLI_SLOT_GETPROGRAMINFOLOG] = (const void *)hook_getprograminfolog;
		t->disp[GLI_SLOT_PROGRAMBINARY] = (const void *)hook_programbinary;
	}
	if (rect) {   /* after the diagnostic hooks so both work together */
		rect_next = t->disp[GLI_SLOT_SHADERSOURCE];
		t->disp[GLI_SLOT_SHADERSOURCE] = (const void *)hook_shadersource_rect;
		rect_next_lp = t->disp[GLI_SLOT_LINKPROGRAM]; orig_da = t->disp[GLI_SLOT_DRAWARRAYS]; orig_de = t->disp[GLI_SLOT_DRAWELEMENTS];
		t->disp[GLI_SLOT_LINKPROGRAM] = (const void *)hook_linkprogram_rect;
		t->disp[GLI_SLOT_DRAWARRAYS] = (const void *)hook_drawarrays_rect;
		t->disp[GLI_SLOT_DRAWELEMENTS] = (const void *)hook_drawelements_rect;
		orig_dre = t->disp[405]; orig_dai = t->disp[762]; orig_dei = t->disp[763];
		if (u1w && t->disp[59]) { orig_dt = t->disp[59]; t->disp[59] = (const void *)hook_deletetextures_u1w; }
		if (u1w) {   /* the rect uniform witness costs a GL query + a lock per call: diagnostic sessions only (count2 fps drop, 2026-11-10) */
			rw_next_u1i = t->disp[606]; rw_next_u1iv = t->disp[614]; rw_next_gul = t->disp[625];
			if (rw_next_u1i) t->disp[606] = (const void *)hook_u1i_rw;
			if (rw_next_u1iv) t->disp[614] = (const void *)hook_u1iv_rw;
			if (rw_next_gul) t->disp[625] = (const void *)hook_gul_rw;
		}
		if (orig_dre) t->disp[405] = (const void *)hook_drawrangeelements_rect;
		if (orig_dai) t->disp[762] = (const void *)hook_drawarraysinstanced_rect;
		if (orig_dei) t->disp[763] = (const void *)hook_drawelementsinstanced_rect;
		orig_bt = t->disp[GLI_SLOT_BINDTEXTURE]; orig_tpf = t->disp[GLI_SLOT_TEXPARAMETERF]; orig_tpfv = t->disp[GLI_SLOT_TEXPARAMETERFV];
		orig_tpi = t->disp[GLI_SLOT_TEXPARAMETERI]; orig_tpiv = t->disp[GLI_SLOT_TEXPARAMETERIV];
		t->disp[GLI_SLOT_BINDTEXTURE] = (const void *)hook_bindtexture;
		t->disp[GLI_SLOT_TEXPARAMETERF] = (const void *)hook_texparameterf;
		t->disp[GLI_SLOT_TEXPARAMETERFV] = (const void *)hook_texparameterfv;
		t->disp[GLI_SLOT_TEXPARAMETERI] = (const void *)hook_texparameteri;
		t->disp[GLI_SLOT_TEXPARAMETERIV] = (const void *)hook_texparameteriv;
	}
	if (nameq_on && t->disp[GLI_SLOT_GENTEXTURES] && t->disp[GLI_SLOT_DELETETEXTURES] && t->disp[674] && t->disp[673]) {   /* name quarantine (default on) */
		nq_tex.next_gen = t->disp[GLI_SLOT_GENTEXTURES]; nq_tex.next_del = t->disp[GLI_SLOT_DELETETEXTURES];
		t->disp[GLI_SLOT_GENTEXTURES] = (const void *)hook_gentextures_nq; t->disp[GLI_SLOT_DELETETEXTURES] = (const void *)hook_deletetextures_nq;
		nq_fbo.next_gen = t->disp[674]; nq_fbo.next_del = t->disp[673];
		t->disp[674] = (const void *)hook_genframebuffers_nq; t->disp[673] = (const void *)hook_deleteframebuffers_nq;
	} else if (nameq_on) {
		fprintf(stderr, "SWEET GAP: NAMEQ not installed: a gen/delete slot is empty in this table\n");
	}
	if (fbo_diag_on) {   /* last, so it runs first and then calls whatever was installed before */
		fbo_next_da = t->disp[GLI_SLOT_DRAWARRAYS]; fbo_next_de = t->disp[GLI_SLOT_DRAWELEMENTS]; fbo_next_cl = t->disp[GLI_SLOT_CLEAR];
		t->disp[GLI_SLOT_DRAWARRAYS] = (const void *)hook_drawarrays_fbo;
		t->disp[GLI_SLOT_DRAWELEMENTS] = (const void *)hook_drawelements_fbo;
		t->disp[GLI_SLOT_CLEAR] = (const void *)hook_clear_fbo;
	}
	if (trace) {   /* after everything else so it runs first and then calls whatever was installed */
		trc_next_bt = t->disp[GLI_SLOT_BINDTEXTURE]; trc_next_at = t->disp[GLI_SLOT_ACTIVETEXTURE];
		t->disp[GLI_SLOT_BINDTEXTURE] = (const void *)hook_bindtexture_trc;
		t->disp[GLI_SLOT_ACTIVETEXTURE] = (const void *)hook_activetexture_trc;
		trc_next_da = t->disp[GLI_SLOT_DRAWARRAYS]; trc_next_de = t->disp[GLI_SLOT_DRAWELEMENTS]; trc_next_cl = t->disp[GLI_SLOT_CLEAR];
		trc_next_dre = t->disp[405]; trc_next_dai = t->disp[762]; trc_next_dei = t->disp[763]; trc_next_bl = t->disp[755];
		t->disp[GLI_SLOT_DRAWARRAYS] = (const void *)hook_drawarrays_trc;
		t->disp[GLI_SLOT_DRAWELEMENTS] = (const void *)hook_drawelements_trc;
		t->disp[GLI_SLOT_CLEAR] = (const void *)hook_clear_trc;
		t->disp[405] = (const void *)hook_drawrangeelements_trc;      /* glDrawRangeElements */
		t->disp[762] = (const void *)hook_drawarraysinstanced_trc;    /* glDrawArraysInstanced(EXT) */
		t->disp[763] = (const void *)hook_drawelementsinstanced_trc;  /* glDrawElementsInstanced(EXT) */
		t->disp[755] = (const void *)hook_blit_trc;                   /* glBlitFramebuffer */
	}
	abgr_next_fbt = t->disp[GLI_SLOT_FRAMEBUFFERTEXTURE2D];   /* ALL-ON T-28, always on (after rect/shader/fbo/trace hooks: runs before them) */
	t->disp[GLI_SLOT_FRAMEBUFFERTEXTURE2D] = (const void *)hook_framebuffertexture2d_abgr;
	if (census_on) census_install(t);   /* research 90 L1/L2: after rect/shader/fbo/trace so its ShaderSource hook sees the original text; before HIST so HIST still counts every slot */
	if (sweet_rf_init(getenv("SWEETGL_REALFLUSH"))) rf_install(t);   /* LOWCLK real-flush: before FLUSHT, so FLUSHT times whichever flush runs */
	if (sweet_ft_init(getenv("SWEETGL_FLUSHT"))) ft_install(t);   /* P-G0c: before HIST so HIST still counts every slot */
	if (hist_on) hist_install(t);   /* outermost: wraps every slot as finally installed */
}
