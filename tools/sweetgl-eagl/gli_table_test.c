#define _POSIX_C_SOURCE 200809L
/* Host test of gli_table: slot wiring (right _mesa_ function, rctx dropped, arguments not shifted), traps, specials. */
#include "gli_fake.h"
#include "gli_table.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

static int fails, checks;
/* eagl_mesa.c helper (Mesa context lookup): the host test has no Mesa context, so the size comes only from the note table */
int sweet_mesa_unit_tex2d_size(unsigned unit, unsigned *w, unsigned *h) { (void)unit; (void)w; (void)h; return 0; }
/* eagl_mesa.c helper (ABGR render-target swizzle undo, hook_framebuffertexture2d_abgr): nothing to undo on the host */
void sgl_abgr_on_fbo(unsigned tex) { (void)tex; }
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *hit; static unsigned long args[5]; unsigned long fake_ret;
static unsigned hits; static const char *hit_names[16];
/* seq: every fake call in order with its first integer arg (census tests assert the exact GL call sequence and that glGetError is never called);
 * inj: when inj_on, the fake glGetIntegerv / glGetTexLevelParameteriv write chosen values through the pointer argument (the generated fakes write nothing) */
static unsigned seq_n; static const char *seq_name[64]; static unsigned long seq_a0[64];
static int inj_pbo, inj_rbuf, inj_sb, inj_crf, inj_crt, inj_al, inj_rl, inj_sp, inj_sr;   /* census-extension readback preconditions (default 0 = what a failing query would not write) */
static int inj_blend, inj_bf[4], inj_eq[2], inj_mask[4], inj_scis, inj_sbox[4], inj_depth;   /* draw-state line values (distinct per pname) */
static int inj_on, inj_prog, inj_readfb, inj_tex, inj_w, inj_h, inj_ifmt, inj_vp[4], inj_fbo, inj_atype, inj_aname, inj_gen_on;
static unsigned inj_gen_base;   /* the fake glGenTextures writes names base, base+1, ... through its pointer (only inside the original call) */
/* capv7/8 fakes (inj_cv = 1): vertex attribute state, copy-read binding, buffer size / mapped flag, a VBO image, uniforms, and a log of every fake call */
static int inj_eb;   /* ELEMENT_ARRAY_BUFFER_BINDING (0x8895) */
static int inj_cv, inj_crb, inj_bsz, inj_bmapped, inj_nu, inj_ul, inj_link;
static int inj_va[4][5];   /* per attribute: enabled, buffer binding, size, type, stride */
static unsigned long inj_vaoff[4];
static float inj_vb[64];   /* the VBO bytes */
static struct { const char *name; unsigned type; float f[16]; int i, noloc; } inj_un[8];
static unsigned long bb_log[16][2], bs_log[8][2]; static unsigned bb_n, bs_n; static int cv_ml;   /* cv_ml: maxLength the hook passed to the last glGetActiveUniform */
static const char *cv_log[256]; static unsigned cv_n;
void fake_hit(const char *n) { if (inj_cv && cv_n < 256) cv_log[cv_n++] = n; hit = n; if (hits < 16) hit_names[hits] = n; hits++; if (seq_n < 64) { seq_name[seq_n] = n; seq_a0[seq_n] = 0; } seq_n++; }
static unsigned long first_args[5];
void fake_arg(int i, unsigned long v)
{
	args[i] = v; if (hits == 1) first_args[i] = v;
	if (i == 0 && seq_n && seq_n <= 64) seq_a0[seq_n - 1] = v;
	if (inj_on && i == 1 && hit && !strcmp(hit, "_mesa_GetIntegerv")) {
		int *d = (int *)v;
		switch (args[0]) {
		case 0x8B8D: d[0] = inj_prog; break;
		case 0x8CAA: d[0] = inj_readfb; break;
		case 0x8CA6: d[0] = inj_fbo; break;
		case 0x8069: d[0] = inj_tex; break;
		case 0x0BA2: memcpy(d, inj_vp, sizeof inj_vp); break;
		case 0x88ED: d[0] = inj_pbo; break;
		case 0x0C02: d[0] = inj_rbuf; break;
		case 0x80A8: d[0] = inj_sb; break;
		case 0x8B9B: d[0] = inj_crf; break;
		case 0x8B9A: d[0] = inj_crt; break;
		case 0x0D05: d[0] = inj_al; break;
		case 0x0D02: d[0] = inj_rl; break;
		case 0x0D04: d[0] = inj_sp; break;
		case 0x0D03: d[0] = inj_sr; break;
		case 0x0BE2: d[0] = inj_blend; break;
		case 0x80C9: d[0] = inj_bf[0]; break;
		case 0x80C8: d[0] = inj_bf[1]; break;
		case 0x80CB: d[0] = inj_bf[2]; break;
		case 0x80CA: d[0] = inj_bf[3]; break;
		case 0x8009: d[0] = inj_eq[0]; break;
		case 0x883D: d[0] = inj_eq[1]; break;
		case 0x0C23: memcpy(d, inj_mask, sizeof inj_mask); break;
		case 0x0C11: d[0] = inj_scis; break;
		case 0x0C10: memcpy(d, inj_sbox, sizeof inj_sbox); break;
		case 0x0B71: d[0] = inj_depth; break;
		case 0x8895: d[0] = inj_eb; break;   /* ELEMENT_ARRAY_BUFFER_BINDING (capv8) */
		case 0x8F36: d[0] = inj_crb; break;   /* COPY_READ_BUFFER binding (capv7/8) */
		}
	}
	if (inj_cv && hit) {
		if (i == 2 && !strcmp(hit, "_mesa_GetVertexAttribiv") && args[0] < 4) {
			int *d = (int *)v; const int *a = inj_va[args[0]];
			switch (args[1]) { case 0x8622: d[0] = a[0]; break; case 0x889F: d[0] = a[1]; break; case 0x8623: d[0] = a[2]; break; case 0x8625: d[0] = a[3]; break; case 0x8624: d[0] = a[4]; break; }
		}
		if (i == 2 && !strcmp(hit, "_mesa_GetVertexAttribPointerv") && args[0] < 4 && args[1] == 0x8645) *(void **)v = (void *)(uintptr_t)inj_vaoff[args[0]];
		if (i == 2 && !strcmp(hit, "_mesa_GetProgramiv")) { int *d = (int *)v; switch (args[1]) { case 0x8B82: d[0] = inj_link; break; case 0x8B86: d[0] = inj_nu; break; case 0x8B87: d[0] = inj_ul; break; } }
		if (i == 2 && !strcmp(hit, "_mesa_GetBufferParameteriv") && args[0] == 0x8F36) { int *d = (int *)v; if (args[1] == 0x8764) d[0] = inj_bsz; else if (args[1] == 0x88BC) d[0] = inj_bmapped; }
		if (i == 2 && !strcmp(hit, "_mesa_GetUniformfv") && args[1] >= 100 && args[1] < 108) memcpy((float *)v, inj_un[args[1] - 100].f, 16 * sizeof(float));
		if (i == 2 && !strcmp(hit, "_mesa_GetUniformiv") && args[1] >= 100 && args[1] < 108) ((int *)v)[0] = inj_un[args[1] - 100].i;
		if (i == 1 && !strcmp(hit, "_mesa_BindBuffer") && bb_n < 16) { bb_log[bb_n][0] = args[0]; bb_log[bb_n][1] = v; bb_n++; }
	}
	if (inj_gen_on && i == 1 && hit && !strcmp(hit, "_mesa_GenTextures")) { unsigned *d = (unsigned *)v; for (unsigned long k = 0; k < args[0]; k++) d[k] = inj_gen_base + (unsigned)k; }
	if (inj_on && i == 3 && hit && !strcmp(hit, "_mesa_GetFramebufferAttachmentParameteriv") && (args[0] == 0x8D40 || args[0] == 0x8CA8) && args[1] == 0x8CE0) {   /* colour attachment 0 only: depth/stencil stay NONE */
		int *d = (int *)v;
		if (args[2] == 0x8CD0) d[0] = inj_atype; else if (args[2] == 0x8CD1) d[0] = inj_aname;
	}
	if (inj_on && i == 3 && hit && !strcmp(hit, "_mesa_GetTexLevelParameteriv")) {
		int *d = (int *)v;
		switch (args[2]) { case 0x1000: d[0] = inj_w; break; case 0x1001: d[0] = inj_h; break; case 0x1003: d[0] = inj_ifmt; break; }
	}
}
/* all 8 args of glCopyTexSubImage2D (linked with --wrap, see gli-host-test.sh) */
static unsigned long cts_args[8];
void __real__mesa_CopyTexSubImage2D(unsigned, int, int, int, int, int, int, int);
void __wrap__mesa_CopyTexSubImage2D(unsigned a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
	cts_args[0] = a0; cts_args[1] = (unsigned long)(long)a1; cts_args[2] = (unsigned long)(long)a2; cts_args[3] = (unsigned long)(long)a3;
	cts_args[4] = (unsigned long)(long)a4; cts_args[5] = (unsigned long)(long)a5; cts_args[6] = (unsigned long)(long)a6; cts_args[7] = (unsigned long)(long)a7;
	__real__mesa_CopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
}   /* first_args: the ORIGINAL call of a hook (a later read-only query overwrites args) */
/* all arguments of glTexImage2D (9) and glCopyTexImage2D (8) (linked with --wrap, see gli-host-test.sh; the generated fakes record 5) */
static unsigned long ti_args[9], ci_args[8];
void __real__mesa_TexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, void *);
void __wrap__mesa_TexImage2D(unsigned a0, int a1, int a2, int a3, int a4, int a5, unsigned a6, unsigned a7, void *a8)
{
	ti_args[0] = a0; ti_args[1] = (unsigned long)(long)a1; ti_args[2] = (unsigned long)(long)a2; ti_args[3] = (unsigned long)(long)a3; ti_args[4] = (unsigned long)(long)a4;
	ti_args[5] = (unsigned long)(long)a5; ti_args[6] = a6; ti_args[7] = a7; ti_args[8] = (unsigned long)(uintptr_t)a8;
	__real__mesa_TexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}
void __real__mesa_CopyTexImage2D(unsigned, int, unsigned, int, int, int, int, int);
void __wrap__mesa_CopyTexImage2D(unsigned a0, int a1, unsigned a2, int a3, int a4, int a5, int a6, int a7)
{
	ci_args[0] = a0; ci_args[1] = (unsigned long)(long)a1; ci_args[2] = a2; ci_args[3] = (unsigned long)(long)a3; ci_args[4] = (unsigned long)(long)a4;
	ci_args[5] = (unsigned long)(long)a5; ci_args[6] = (unsigned long)(long)a6; ci_args[7] = (unsigned long)(long)a7;
	__real__mesa_CopyTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
}
/* glReadPixels (7 args) and glBlitFramebuffer (10 args): linked with --wrap. The ReadPixels wrapper fills the 64-byte patch from rp_pat and writes 0xEE into bytes 64..79 (a destination smaller than 80 bytes is an ASan error) */
static unsigned long rp_args[7], bl_args[10]; static unsigned rp_calls, rp_fill, rp_nowrite; static unsigned char rp_pat[64];
static int rp_log[32][4]; static unsigned rp_multi, rp_k; static unsigned char rp_pats[8][64]; static const unsigned char *rp_grid_src; static size_t rp_grid_len;   /* rp_k: index of the 4x4 call inside a sample; rp_grid_src: image a non-4x4 (grid) read copies from */   /* rp_log: x y w h of the first 32 calls; rp_multi: call k (0-based) copies rp_pats[k % 8] instead of rp_pat */   /* rp_fill: only the readback tests hand the wrapper a real buffer (the generic slot test passes junk pointers) */
void __real__mesa_ReadPixels(int, int, int, int, unsigned, unsigned, void *);
void __wrap__mesa_ReadPixels(int a0, int a1, int a2, int a3, unsigned a4, unsigned a5, void *a6)
{
	rp_args[0] = (unsigned long)(long)a0; rp_args[1] = (unsigned long)(long)a1; rp_args[2] = (unsigned long)(long)a2; rp_args[3] = (unsigned long)(long)a3;
	rp_args[4] = a4; rp_args[5] = a5; rp_args[6] = (unsigned long)(uintptr_t)a6; if (rp_calls < 32) { rp_log[rp_calls][0] = a0; rp_log[rp_calls][1] = a1; rp_log[rp_calls][2] = a2; rp_log[rp_calls][3] = a3; } rp_calls++; if (rp_calls == 1) rp_k = 0;
	if (rp_fill && a6 && !(a2 == 4 && a3 == 4)) { if (rp_grid_src) memcpy(a6, rp_grid_src, rp_grid_len); /* else: a grid read that writes nothing */ }
	else if (rp_fill && a6 && rp_nowrite) { /* simulates an error / a clipped-away patch: nothing is written */ }
	else if (rp_fill && a6) { memcpy(a6, rp_multi ? rp_pats[rp_k++ % 5] : rp_pat, 64); memset((unsigned char *)a6 + 64, 0xEE, 16); }
	__real__mesa_ReadPixels(a0, a1, a2, a3, a4, a5, a6);
}
void __real__mesa_BlitFramebuffer(int, int, int, int, int, int, int, int, unsigned, unsigned);
void __wrap__mesa_BlitFramebuffer(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, unsigned a8, unsigned a9)
{
	bl_args[0] = (unsigned long)(long)a0; bl_args[1] = (unsigned long)(long)a1; bl_args[2] = (unsigned long)(long)a2; bl_args[3] = (unsigned long)(long)a3; bl_args[4] = (unsigned long)(long)a4;
	bl_args[5] = (unsigned long)(long)a5; bl_args[6] = (unsigned long)(long)a6; bl_args[7] = (unsigned long)(long)a7; bl_args[8] = a8; bl_args[9] = a9;
	__real__mesa_BlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}
/* capv7: glGetActiveUniform (7 args, the generated fake records 5), glGetUniformLocation (name -> location 100 + index) . Without inj_cv they only forward. */
void __real__mesa_GetActiveUniform(unsigned, unsigned, int, int *, int *, unsigned *, char *);
void __wrap__mesa_GetActiveUniform(unsigned p, unsigned idx, int ml, int *len, int *size, unsigned *type, char *name)
{
	__real__mesa_GetActiveUniform(p, idx, ml, len, size, type, name);
	cv_ml = ml;
	if (inj_cv && idx < 8 && inj_un[idx].name && ml > 0) { snprintf(name, (size_t)ml, "%s", inj_un[idx].name); *len = (int)strlen(name); *size = 1; *type = inj_un[idx].type; }
}
int __real__mesa_GetUniformLocation(unsigned, const char *);
int __wrap__mesa_GetUniformLocation(unsigned p, const char *name)
{
	int r = __real__mesa_GetUniformLocation(p, name), i;
	if (!inj_cv) return r;
	for (i = 0; i < 8; i++) if (inj_un[i].name && !strcmp(name, inj_un[i].name)) return inj_un[i].noloc ? -1 : 100 + i;
	return -1;
}
static long long mock_t; static long long mock_clk(void) { return mock_t; }
static unsigned long unattr_n(void) { char q[256]; const char *u; sweet_th_stats_line(q, sizeof q); u = strstr(q, "unattributed "); return u ? strtoul(u + 13, NULL, 10) : 999999; }
static unsigned count_seq(const char *name) { unsigned c = 0; for (unsigned q = 0; q < seq_n && q < 64; q++) if (!strcmp(seq_name[q], name)) c++; return c; }
/* stderr capture into a file (the census tests above do the same inline) */
static int cap_saved, cap_fd; static char cap_path[] = "/tmp/gli-th-XXXXXX";
static void cap_begin(void) { strcpy(cap_path, "/tmp/gli-th-XXXXXX"); cap_saved = dup(2); cap_fd = mkstemp(cap_path); fflush(stderr); dup2(cap_fd, 2); }
static void cap_end(char *out, size_t n) { ssize_t r; fflush(stderr); dup2(cap_saved, 2); close(cap_saved); lseek(cap_fd, 0, SEEK_SET); r = read(cap_fd, out, n - 1); out[r < 0 ? 0 : r] = 0; close(cap_fd); unlink(cap_path); }
/* _mesa_ functions the specials call that no slot is wired to directly (weak: the generated fakes win when they exist) */
#define WEAK __attribute__((weak))
static const char *gs_ver, *gs_rend, *gs_ext; static unsigned gs_calls;   /* what the fake glGetString returns (NULL = nothing); gs_calls counts 0x1f03 calls */
WEAK const unsigned char *_mesa_GetString(unsigned n) { hit = "_mesa_GetString"; if (n == 0x1f03) gs_calls++; return (const unsigned char *)(n == 0x1f02 ? gs_ver : n == 0x1f01 ? gs_rend : n == 0x1f03 ? gs_ext : NULL); }
static unsigned dbgcb_n;   /* installs of the KHR_debug callback (must be once per context) */
WEAK void _mesa_DebugMessageCallback(void (*cb)(unsigned, unsigned, unsigned, unsigned, int, const char *, const void *), const void *u) { (void)cb; (void)u; hit = "_mesa_DebugMessageCallback"; dbgcb_n++; }
WEAK void _mesa_DeleteProgram(unsigned n) { (void)n; hit = "_mesa_DeleteProgram"; }
WEAK void _mesa_DeleteShader(unsigned n) { (void)n; hit = "_mesa_DeleteShader"; }
WEAK unsigned char _mesa_IsProgram(unsigned n) { (void)n; return (unsigned char)fake_ret; }
WEAK void _mesa_Finish(void) { hit = "_mesa_Finish"; }
WEAK void _mesa_GetIntegerv(unsigned p, int *d) { (void)p; hit = "_mesa_GetIntegerv"; *d = 0; }

typedef unsigned long (*callfn)(void *, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
/* ponytail: calls every wrapper as if it took 5 integer args (true for the integer-class registers of both AAPCS64 and SysV);
 * the extra registers are ignored by shorter wrappers. Not a conforming call, fine for a host test. */
static unsigned long call(const struct sweet_gli *t, int slot)
{
	return ((callfn)t->disp[slot])(t->rctx, 1, 2, 3, 4, 5);
}

static int ens_n, ens_ret; static void *ens_last;
static int ens(void *r) { ens_n++; ens_last = r; return ens_ret; }

static int logged_slot, logged_n; static char logged_name[64];
static void logfn(int slot, const char *name) { logged_slot = slot; logged_n++; snprintf(logged_name, sizeof logged_name, "%s", name); }

static int slot_of(const char *name)
{
	for (int i = 0; i < SWEET_GLI_NSLOTS; i++)
		if (!strcmp(sweet_gli_names[i], name)) return i;
	return -1;
}

/* P-G0c flush timing test helpers */
static int rf_gate_v, rf_disp_v, rf_disp_n; static int rf_gate_t(void) { return rf_gate_v; } static int rf_disp_t(void) { rf_disp_n++; return rf_disp_v; }   /* LOWCLK real-flush */
static unsigned long long ft_t, ft_step; static unsigned long long ft_clk(void) { unsigned long long v = ft_t; ft_t += ft_step; return v; }
static unsigned long long ft_mt = 5000000000ull, ft_total;
static void *ft_worker(void *a)
{
	(void)a;
	for (int i = 0; i < 20000; i++) {
		unsigned long long t0 = __atomic_fetch_add(&ft_mt, 100000, __ATOMIC_RELAXED), t1 = __atomic_fetch_add(&ft_mt, 100000, __ATOMIC_RELAXED);
		struct sweet_ft_win d; unsigned s;
		if (sweet_ft_note(i & 1, t0, t1, &d, &s)) __atomic_fetch_add(&ft_total, (unsigned long long)d.flush + d.finish, __ATOMIC_RELAXED);
	}
	return NULL;
}

/* P-G1 slice 2: BASE line test helpers */
static struct sweet_base_info bs_info; static int bs_ret; static const char *bs_why; static int bs_calls;
static int bs_resolve(struct sweet_base_info *bi, const char **why) { bs_calls++; *bi = bs_info; *why = bs_why; return bs_ret; }

int main(void)
{
	static struct sweet_gli t;
	int dummy;
	bs_ret = 0; bs_why = "test: first init"; sweet_base_set_resolver(bs_resolve);   /* the first init consumes the once-per-process BASE line */
	setenv("SWEETGL_NAMEQ", "0", 1);   /* name quarantine (default on) wraps gen/delete with real arrays: the junk-arg slot loop runs without it; sweet_nameq_selftest covers it */
	setenv("SWEETGL_RECT", "0", 1);   /* the rewrite is ON by default since research 76: the default-off checks below run with it explicitly off */
	sweet_gli_set_log(logfn);
	sweet_gli_init(&t, &dummy);
	sweet_gli_ensure_current = ens;
	CHECK(t.rctx == &dummy);
	CHECK(sizeof t.disp / sizeof t.disp[0] == 1036);

	int wired = 0, special = 0, traps = 0, named = 0;
	for (int i = 0; i < SWEET_GLI_NSLOTS; i++) {
		CHECK(t.disp[i] != NULL);
		if (strcmp(sweet_gli_names[i], "?")) named++;
		const char *tg = sweet_gli_targets[i];
		if (!tg) {   /* trap: logs once, returns 0, count matches */
			traps++;
			int before = logged_n; ens_n = 0;
			CHECK(call(&t, i) == 0 && ens_n == 0);   /* traps touch no GL: no context switch */
			CHECK(logged_n == before + 1 && logged_slot == i && !strcmp(logged_name, sweet_gli_names[i]));
			CHECK(call(&t, i) == 0 && logged_n == before + 1);   /* once only */
		} else if (!strncmp(tg, "_mesa_", 6)) {   /* wired: the right fake, args after rctx in order */
			wired++;
			hit = NULL; memset(args, 0, sizeof args);
			fake_ret = 0; ens_n = 0; ens_last = NULL;
			call(&t, i);
			CHECK(ens_n == (i == 677 ? 2 : 1) && ens_last == &dummy);   /* (677: the always-on ALL-ON T-28 ABGR hook checks the context once more) made current exactly once, with this table's rctx */
			CHECK(hit && !strcmp(hit, tg));
			int n = sweet_gli_nint[i] < 5 ? sweet_gli_nint[i] : 5;
			for (int k = 0; k < n; k++) CHECK(args[k] == (unsigned long)(k + 1));
		} else {
			special++;
			CHECK(!strncmp(tg, "sgli_", 5));
			ens_n = 0;
			call(&t, i);   /* must not crash */
			CHECK(ens_n == (strcmp(tg, "sgli_nop") ? 1 : 0));
		}
	}
	printf("slots: %d named, %d wired, %d special, %d trap\n", named, wired, special, traps);
	CHECK(wired > 300 && special >= 7 && named > 400);
	CHECK(sweet_gli_names[1035] != NULL);

	/* out-of-range trap is safe */
	CHECK(sweet_gli_trap(-1) == 0 && sweet_gli_trap(SWEET_GLI_NSLOTS) == 0);

	/* what CoreAnimation calls (R33 s.2.2, core spelling; the table's first name of a slot) must be wired or special */
	static const char *const need[] = { "glBindTexture", "glBlendEquation", "glBlendFunc", "glClear", "glClearColor", "glClearStencil",
		"glCompressedTexImage2D", "glCopyTexSubImage2D", "glDeleteTextures", "glDisable", "glDrawArrays", "glDrawElements", "glEnable",
		"glEnableClientState", "glFinish", "glFlush", "glGenTextures", "glGetError", "glGetIntegerv", "glGetString", "glPixelStorei",
		"glReadBuffer", "glScissor", "glTexImage2D", "glTexParameterf", "glTexParameteri", "glTexSubImage2D", "glViewport",
		"glActiveTexture", "glVertexAttribPointer", "glEnableVertexAttribArray", "glDisableVertexAttribArray", "glCreateShader",
		"glShaderSource", "glCompileShader", "glCreateProgram", "glAttachShader", "glLinkProgram", "glUseProgram", "glDeleteShader",
		"glBindAttribLocation", "glGetUniformLocation", "glUniform1f", "glUniform1i", "glUniform4f", "glUniform3fv", "glUniform4fv",
		"glUniformMatrix3fv", "glUniformMatrix4fv", "glBindFramebuffer", "glGenFramebuffers", "glDeleteFramebuffers",
		"glFramebufferTexture2D", "glGenerateMipmap", "glFinishObjectAPPLE", "glTestObjectAPPLE", "glLabelObjectEXT",
		"glClientWaitSync", "glFramebufferParameteriAPPLE", "glFenceSync", "glDeleteSync", "glGetShaderiv", "glGetProgramiv",
		"glGetShaderInfoLog", "glGetProgramInfoLog", "glGetShaderPrecisionFormat", "glReleaseShaderCompiler", "glShaderBinary",
		"glInvalidateFramebuffer", "glGetIntegeri_v" };
	for (unsigned k = 0; k < sizeof need / sizeof need[0]; k++) {
		int s = slot_of(need[k]);
		if (s < 0 || !sweet_gli_targets[s]) printf("not wired: %s (slot %d)\n", need[k], s);
		CHECK(s >= 0 && sweet_gli_targets[s]);
	}
	CHECK(!strcmp(sweet_gli_targets[slot_of("glFlush")], "_mesa_Finish"));   /* glFlush == glFinish [src R33 s.3] */
	CHECK(slot_of("glGetString") == 117 && slot_of("glGetIntegerv") == 104);   /* offsets +0x3b0 / +0x348 [src R33 s.2.2] */

	/* specials */
	int gs = slot_of("glGetString");
	hit = NULL;
	CHECK(((const unsigned char *(*)(void *, unsigned))t.disp[gs])(t.rctx, 0x8a1e) == NULL && hit == NULL);   /* Apple-private enum: NULL, no Mesa call */
	((const unsigned char *(*)(void *, unsigned))t.disp[gs])(t.rctx, 0x1f03);
	CHECK(hit && !strcmp(hit, "_mesa_GetString"));
	hit = NULL; call(&t, slot_of("glFinishObjectAPPLE")); CHECK(hit && !strcmp(hit, "_mesa_Finish"));
	CHECK((call(&t, slot_of("glTestObjectAPPLE")) & 0xff) == 1);
	int del = slot_of("glDeleteShader");   /* slot 591, shared by glDeleteShader and glDeleteProgram (RM-00936..00942) */
	CHECK(del == 591);
	fake_ret = 1; call(&t, del); CHECK(hit && !strcmp(hit, "_mesa_DeleteProgram"));
	fake_ret = 0; call(&t, del); CHECK(hit && !strcmp(hit, "_mesa_DeleteShader"));
	hit = NULL; call(&t, slot_of("glInsertEventMarkerEXT")); CHECK(hit == NULL);
	CHECK(sweet_gli_bound_texture_2d() == 0 && hit && !strcmp(hit, "_mesa_GetIntegerv"));

	/* glPixelStorei hook (gli_table.c): UNPACK_ALIGNMENT above 4 is clamped to 4, every other pname/param passes through unchanged */
	{ typedef void (*psfn)(void *, unsigned, int);
	  psfn ps = (psfn)t.disp[slot_of("glPixelStorei")];
	  static const unsigned pn[] = { 0xcf5, 0xcf5, 0xcf5, 0xcf5, 0xcf2, 0xcf6 };
	  static const int in[]      = { 8,     4,     1,     2,     8,     8 };
	  static const int out[]     = { 4,     4,     1,     2,     8,     8 };
	  for (unsigned k = 0; k < sizeof pn / sizeof pn[0]; k++) {
		hit = NULL; memset(args, 0, sizeof args);
		ps(t.rctx, pn[k], in[k]);
		CHECK(hit && !strcmp(hit, "_mesa_PixelStorei") && args[0] == pn[k] && args[1] == (unsigned long)out[k]);
	  } }

	/* hook fails: no wrapper/special except the no-op reaches Mesa; non-void ones return 0 */
	sweet_gli_ensure_current = ens; ens_ret = -5; fake_ret = 7;
	for (int i = 0; i < SWEET_GLI_NSLOTS; i++) {
		const char *tg = sweet_gli_targets[i];
		if (!tg || !strcmp(tg, "sgli_nop")) continue;
		hit = NULL; ens_n = 0;
		call(&t, i);
		CHECK(ens_n == (i == 677 ? 2 : 1) && hit == NULL);   /* 677: ABGR hook (T-28) + the wrapper each check the context */
	}
	{ int s = slot_of("glGetError"), g = slot_of("glGetString"), tobj = slot_of("glTestObjectAPPLE"), cs = slot_of("glCreateShader");
	  CHECK((call(&t, s) & 0xffffffffu) == 0 && (call(&t, g) == 0) && (call(&t, tobj) & 0xff) == 0 && (call(&t, cs) & 0xffffffffu) == 0);
	  ens_ret = 0;   /* sanity: with a working hook the same calls do reach Mesa and return the fake value */
	  CHECK((call(&t, s) & 0xffffffffu) == 7 && (call(&t, cs) & 0xffffffffu) == 7); }
	ens_ret = 0;

	/* without a hook the wrappers still work */
	sweet_gli_ensure_current = NULL; ens_n = 0; hit = NULL;
	call(&t, slot_of("glBindTexture")); CHECK(hit && ens_n == 0);


	{   /* G-11: the SWEETGL_RECT hook and its install (review gli-rect-rewrite F1) */
		static struct sweet_gli tr;
		typedef void (*f_ss)(void *, unsigned, int, const char *const *, const int *);
		const char *a[2] = { "uniform sampler2DRect t; vec4 c = texture2D", "Rect(t, p);" };   /* token split across two strings */
		const char *plain[1] = { "void main(){}" };
		const int lp[1] = { -1 };
		/* default off: ShaderSource is the generated entry */
		CHECK(t.disp[595] == sweet_gli_defaults[595] && t.disp[5] == sweet_gli_defaults[5] && t.disp[304] == sweet_gli_defaults[304]);
		setenv("SWEETGL_RECT", "1", 1);
		sweet_gli_init(&tr, &dummy);
		CHECK(tr.disp[595] != sweet_gli_defaults[595] && tr.disp[595] != NULL);
		sweet_gli_ensure_current = ens; ens_ret = 0;
		hits = 0; memset(first_args, 0, sizeof first_args);
		((f_ss)tr.disp[595])(tr.rctx, 9, 1, plain, lp);   /* no token: original pointers pass through */
		CHECK(hits == 1 && first_args[0] == 9 && first_args[1] == 1 && first_args[2] == (unsigned long)(uintptr_t)plain && first_args[3] == (unsigned long)(uintptr_t)lp);
		hits = 0; memset(first_args, 0, sizeof first_args);
		((f_ss)tr.disp[595])(tr.rctx, 9, 2, a, NULL);      /* rewritten: one string, other pointers */
		CHECK(hits == 1 && first_args[0] == 9 && first_args[1] == 1 && first_args[2] != (unsigned long)(uintptr_t)a);
		hits = 0;
		((f_ss)tr.disp[595])(tr.rctx, 9, 0, a, NULL); ((f_ss)tr.disp[595])(tr.rctx, 9, -1, a, NULL); ((f_ss)tr.disp[595])(tr.rctx, 9, 257, a, NULL);
		CHECK(hits == 3);   /* out-of-range counts pass through to the original */
		/* G-7 compute self-test: runs once, inside the first draw that reaches the rect hook, only with SWEETGL_COMPUTE_TEST (fake GL: the compile status stays 0, so it takes the loud FAIL path and returns before the dispatch) */
		{
			typedef void (*f_de)(void *, unsigned, int, unsigned, const void *);
			setenv("SWEETGL_COMPUTE_TEST", "1", 1);
			hits = 0; ((f_de)tr.disp[67])(tr.rctx, 4, 6, 0x1403, NULL);
			CHECK(hits > 8);   /* the self-test made its GL calls (hit_names holds only the first 16, drained GetError calls fill it) */
			hits = 0; ((f_de)tr.disp[67])(tr.rctx, 4, 6, 0x1403, NULL);
			CHECK(hits == 1);   /* second draw: once only */
			unsetenv("SWEETGL_COMPUTE_TEST");
		}
		/* texture target mapping (step 2): 0x84f5 -> 0xde1 for Bind/TexParameter{i,iv}, other targets untouched */
		{
			typedef void (*f_bt)(void *, unsigned, unsigned);
			typedef void (*f_tpi)(void *, unsigned, unsigned, int);
			hits = 0; memset(first_args, 0, sizeof first_args);
			((f_bt)tr.disp[5])(tr.rctx, 0x84f5, 7);
			CHECK(hits == 1 && first_args[0] == 0xde1 && first_args[1] == 7);
			hits = 0; memset(first_args, 0, sizeof first_args);
			((f_bt)tr.disp[5])(tr.rctx, 0x8513, 9);   /* cube map: untouched */
			CHECK(hits == 1 && first_args[0] == 0x8513 && first_args[1] == 9);
			hits = 0; memset(first_args, 0, sizeof first_args);
			((f_tpi)tr.disp[304])(tr.rctx, 0x84f5, 0x2801, 0x2601);
			CHECK(hits == 1 && first_args[0] == 0xde1 && first_args[1] == 0x2801 && first_args[2] == 0x2601);
			hits = 0; memset(first_args, 0, sizeof first_args);
			((f_tpi)tr.disp[304])(tr.rctx, 0xde1, 0x2801, 0x2601);
			CHECK(hits == 1 && first_args[0] == 0xde1);
			{
				typedef void (*f_tpf)(void *, unsigned, unsigned, float);
				typedef void (*f_tpv)(void *, unsigned, unsigned, const void *);
				static const int iv[1] = { 0x2601 }; static const float fvv[1] = { 1.0f };
				hits = 0; memset(first_args, 0, sizeof first_args);
				((f_tpf)tr.disp[302])(tr.rctx, 0x84f5, 0x2801, 9.0f);
				CHECK(hits == 1 && first_args[0] == 0xde1 && first_args[1] == 0x2801);   /* the fake does not record the float */
				hits = 0; memset(first_args, 0, sizeof first_args);
				((f_tpv)tr.disp[303])(tr.rctx, 0x84f5, 0x2801, fvv);
				CHECK(hits == 1 && first_args[0] == 0xde1 && first_args[2] == (unsigned long)(uintptr_t)fvv);
				hits = 0; memset(first_args, 0, sizeof first_args);
				((f_tpv)tr.disp[305])(tr.rctx, 0x84f5, 0x2801, iv);
				CHECK(hits == 1 && first_args[0] == 0xde1 && first_args[2] == (unsigned long)(uintptr_t)iv);
				hits = 0; memset(first_args, 0, sizeof first_args);
				((f_tpv)tr.disp[305])(tr.rctx, 0xde1, 0x2801, iv);
				CHECK(hits == 1 && first_args[0] == 0xde1);
				for (int k = 302; k <= 305; k++) CHECK(t.disp[k] == sweet_gli_defaults[k]);
			}
		}
		/* chain with the diagnostics: the logged text is the rewritten one */
		setenv("SWEETGL_SHADERS", "1", 1);
		sweet_gli_init(&tr, &dummy);
		sweet_gli_ensure_current = ens;
		{
			char o3[8192]; size_t n3;
			fflush(stderr);
			int sv3 = dup(2); FILE *tf3 = tmpfile(); dup2(fileno(tf3), 2);
			((f_ss)tr.disp[595])(tr.rctx, 12, 2, a, NULL);
			fflush(stderr); dup2(sv3, 2); close(sv3); rewind(tf3);
			n3 = fread(o3, 1, sizeof o3 - 1, tf3); o3[n3] = 0; fclose(tf3);
			CHECK(strstr(o3, "uniform sampler2D t;\\nuniform highp vec2 sweet_rect_inv_t;\\n vec4 c = texture2D(t, (p) * sweet_rect_inv_t);") != NULL && !strstr(o3, "Rect"));
		}
		unsetenv("SWEETGL_SHADERS"); setenv("SWEETGL_RECT", "0", 1);
	}

	{   /* SWEETGL_FBO diagnostic hooks: off = generated entries, on = Clear/Draw hooked and still call through */
		static struct sweet_gli tf;
		typedef void (*f_dr)(void *, unsigned, int, int);
		typedef void (*f_cl)(void *, unsigned);
		CHECK(t.disp[10] == sweet_gli_defaults[10] && t.disp[65] == sweet_gli_defaults[65] && t.disp[67] == sweet_gli_defaults[67]);
		setenv("SWEETGL_FBO", "1", 1);
		sweet_gli_init(&tf, &dummy);
		CHECK(tf.disp[10] != sweet_gli_defaults[10] && tf.disp[65] != sweet_gli_defaults[65] && tf.disp[67] != sweet_gli_defaults[67]);
		sweet_gli_ensure_current = ens; ens_ret = 0;
		hits = 0; memset(first_args, 0, sizeof first_args);
		((f_dr)tf.disp[65])(tf.rctx, 4, 0, 3);
		CHECK(hits >= 1 && hit && !strcmp(hit, "_mesa_DrawArrays"));   /* the original is called last, after the status check */
		hits = 0;
		((f_cl)tf.disp[10])(tf.rctx, 0x4000);
		CHECK(hits >= 1 && hit && !strcmp(hit, "_mesa_Clear"));
		unsetenv("SWEETGL_FBO");
	}

	/* ---- shader diagnostics (SWEETGL_SHADERS=1, OPUS DECISION 2026-10-26 #8) ---- */
	{
		static const int sl[5] = { 595, 596, 599, 660, 888 };
		static const char *const orig_name[5] = { "_mesa_ShaderSource", "_mesa_CompileShader", "_mesa_LinkProgram", "_mesa_GetProgramInfoLog", "_mesa_ProgramBinary" };
		static struct sweet_gli ts;
		/* default off: the five slots are exactly the generated entries */
		for (int k = 0; k < 5; k++) CHECK(t.disp[sl[k]] == sweet_gli_defaults[sl[k]]);
		setenv("SWEETGL_SHADERS", "1", 1);
		sweet_gli_init(&ts, &dummy);
		for (int k = 0; k < 5; k++) CHECK(ts.disp[sl[k]] != sweet_gli_defaults[sl[k]] && ts.disp[sl[k]] != NULL);
		sweet_gli_ensure_current = ens; ens_ret = 0;
		typedef void (*f_ss)(void *, unsigned, int, const char *const *, const int *);
		typedef void (*f_u)(void *, unsigned);
		typedef void (*f_il)(void *, unsigned, int, int *, char *);
		typedef void (*f_pb)(void *, unsigned, unsigned, const void *, int);
		/* every hook: the original once with the same arguments FIRST; anything else is one of the read-only status / info-log queries */
		static const char *const src[2] = { "void main() {\n\tgl_FragColor = vec4(1.0);\n}\n", "// second\n" };
		static const int lens[2] = { -1, 11 };
		hits = 0; memset(first_args, 0, sizeof first_args);
		((f_ss)ts.disp[595])(ts.rctx, 7, 2, src, lens);
		CHECK(hits == 1 && !strcmp(hit_names[0], orig_name[0]) && first_args[0] == 7 && first_args[1] == 2 && first_args[2] == (unsigned long)(uintptr_t)src && first_args[3] == (unsigned long)(uintptr_t)lens);
		hits = 0; memset(first_args, 0, sizeof first_args);
		((f_u)ts.disp[596])(ts.rctx, 8);
		CHECK(hits >= 1 && !strcmp(hit_names[0], orig_name[1]) && first_args[0] == 8);
		for (unsigned h = 1; h < hits && h < 16; h++) CHECK(!strcmp(hit_names[h], "_mesa_GetShaderiv") || !strcmp(hit_names[h], "_mesa_GetShaderInfoLog"));
		hits = 0;
		memset(first_args, 0, sizeof first_args);
		((f_u)ts.disp[599])(ts.rctx, 9);
		CHECK(!strcmp(hit_names[0], orig_name[2]) && first_args[0] == 9);
		for (unsigned h = 1; h < hits && h < 16; h++) CHECK(!strcmp(hit_names[h], "_mesa_GetProgramiv") || !strcmp(hit_names[h], "_mesa_GetProgramInfoLog"));
		{ char buf[16] = "link failed: x"; int len = 14;   /* the (fake) original writes nothing: *length is what the app/Mesa reported */
		  hits = 0; memset(first_args, 0, sizeof first_args);
		  ((f_il)ts.disp[660])(ts.rctx, 9, 16, &len, buf);
		  CHECK(hits == 1 && !strcmp(hit_names[0], orig_name[3]) && first_args[0] == 9 && first_args[1] == 16 && first_args[2] == (unsigned long)(uintptr_t)&len && first_args[3] == (unsigned long)(uintptr_t)buf); }
		/* ProgramBinary: the blob is passed through unchanged and its bytes are never logged */
		static const char blob[] = "SECRETBLOBBYTES-0123456789";
		fflush(stderr);
		int saved = dup(2); FILE *tf = tmpfile(); dup2(fileno(tf), 2);
		hits = 0; memset(first_args, 0, sizeof first_args);
		((f_pb)ts.disp[888])(ts.rctx, 10, 0x8e21, blob, (int)sizeof blob);
		CHECK(!strcmp(hit_names[0], orig_name[4]) && first_args[0] == 10 && first_args[1] == 0x8e21 && first_args[2] == (unsigned long)(uintptr_t)blob && first_args[3] == sizeof blob);
		{ static char big[3000]; memset(big, 'A', sizeof big - 1);
		  const char *bigsrc[1] = { big };
		  ((f_ss)ts.disp[595])(ts.rctx, 6, 1, bigsrc, NULL); }   /* a 3000 byte source: at most 1024 bytes are logged */
		/* caps: 100 sources / 300 compiles / 300 links keep forwarding every call but log at most 64 / 256 results */
		for (int i = 0; i < 100; i++) ((f_ss)ts.disp[595])(ts.rctx, 20 + i, 2, src, lens);
		unsigned fw = 0; hits = 0;
		for (int i = 0; i < 300; i++) { ((f_u)ts.disp[596])(ts.rctx, 100 + i); }
		for (int i = 0; i < 300; i++) { ((f_u)ts.disp[599])(ts.rctx, 400 + i); }
		fw = hits;
		fflush(stderr); dup2(saved, 2); close(saved);
		rewind(tf);
		static char out[1 << 20]; size_t n = fread(out, 1, sizeof out - 1, tf); out[n] = 0; fclose(tf);
		unsigned n_src = 0, n_res = 0; const char *q;
		for (q = out; (q = strstr(q, "sweetgl: ShaderSource shader ")); q++) n_src++;
		for (q = out; (q = strstr(q, "sweetgl: CompileShader shader ")); q++) n_res++;
		for (q = out; (q = strstr(q, "sweetgl: LinkProgram program ")); q++) n_res++;
		CHECK(n_src <= 64 && n_src >= 60);                 /* 1 earlier + 100 more calls, capped */
		CHECK(n_res <= 256 && n_res >= 250);               /* 300 + 300 calls, capped (a few counted earlier) */
		CHECK(fw >= 600);                                  /* every call still reached the original */
		CHECK(strstr(out, "ProgramBinary program 10 format 0x8e21 length 27") != NULL);
		{ unsigned maxa = 0, runa = 0; for (size_t z = 0; z < n; z++) { runa = out[z] == 'A' ? runa + 1 : 0; if (runa > maxa) maxa = runa; } CHECK(maxa <= 1024 && maxa >= 1000); }
		CHECK(strstr(out, "SECRETBLOB") == NULL);          /* blob bytes never logged */
		CHECK(strstr(out, "SECRETBL") == NULL);            /* AP-214: not even the first 8 blob bytes (mutant shader-logs-blob prints %.8s) */
		CHECK(strstr(out, "void main() {\\n\\tgl_FragColor") != NULL);   /* source text escaped to one line */
				/* review F4: byte caps and the other caps, on a fresh capture */
		{
			static char ilbuf[8192]; memset(ilbuf, 'L', sizeof ilbuf - 1);
			int ilen = 5000;
			fflush(stderr);
			int sv2 = dup(2); FILE *tf2 = tmpfile(); dup2(fileno(tf2), 2);
			for (int i = 0; i < 100; i++) ((f_pb)ts.disp[888])(ts.rctx, 30 + i, 0x8e21, blob, (int)sizeof blob);
			for (int i = 0; i < 100; i++) ((f_il)ts.disp[660])(ts.rctx, 500 + i, (int)sizeof ilbuf, &ilen, ilbuf);
			fflush(stderr); dup2(sv2, 2); close(sv2); rewind(tf2);
			static char o2[1 << 21]; size_t n2 = fread(o2, 1, sizeof o2 - 1, tf2); o2[n2] = 0; fclose(tf2);
			unsigned nb = 0, ni = 0; const char *q2;
			for (q2 = o2; (q2 = strstr(q2, "sweetgl: ProgramBinary program ")); q2++) nb++;
			for (q2 = o2; (q2 = strstr(q2, "sweetgl: GetProgramInfoLog ")); q2++) ni++;
			CHECK(nb <= 64 && nb >= 60);
			CHECK(ni <= 64 && ni >= 60);
			/* an info log line carries at most 4096 'L' */
			unsigned maxl = 0, run = 0;
			for (size_t z = 0; z < n2; z++) { run = o2[z] == 'L' ? run + 1 : 0; if (run > maxl) maxl = run; }
			CHECK(maxl <= 4096 && maxl >= 4000);
		}
		/* context not current: the wrapper (the original) bails without calling Mesa and the hooks add NO query */
		ens_ret = -5; hits = 0;
		((f_u)ts.disp[596])(ts.rctx, 1); CHECK(hits == 0);
		((f_u)ts.disp[599])(ts.rctx, 1); CHECK(hits == 0);
		((f_pb)ts.disp[888])(ts.rctx, 1, 2, blob, 3); CHECK(hits == 0);
		ens_ret = 0;
		unsetenv("SWEETGL_SHADERS");
	}


	{   /* G-11 rect rewrite: sampler2DRect -> sampler2D + inverse-size uniform, texture2DRect(NAME, EXPR) -> normalised texture2D */
		char *o = NULL; size_t n;
		const char *in = "uniform lowp sampler2DRect texture1; x = texture2DRect(texture1, texcoord1_0) + texture2DRectProj(texture1, d) + texture2D(t0, e);";
		n = sweet_rect_rewrite(in, strlen(in), &o);
		CHECK(o && n == strlen(o));
		CHECK(o && strstr(o, "uniform lowp sampler2D texture1;\nuniform highp vec2 sweet_rect_inv_texture1;\n") != NULL);
		CHECK(o && strstr(o, "texture2D(texture1, (texcoord1_0) * sweet_rect_inv_texture1)") != NULL);
		CHECK(o && strstr(o, "texture2DProj(texture1, d)") != NULL && !strstr(o, "Rect"));
		free(o);
		/* nested parentheses and spaces in the argument */
		in = "y = texture2DRect ( texture0 , vec2(a.x, (b + c) * 2.0) );";
		n = sweet_rect_rewrite(in, strlen(in), &o);
		CHECK(o && strstr(o, "texture2D(texture0, (vec2(a.x, (b + c) * 2.0) ) * sweet_rect_inv_texture0)") != NULL);
		free(o);
		/* no token: identical text; truncated and glued tokens untouched or consistent */
		n = sweet_rect_rewrite("void main(){}", 13, &o); CHECK(o && n == 13 && !strcmp(o, "void main(){}")); free(o);
		n = sweet_rect_rewrite("sampler2DRec", 12, &o); CHECK(o && n == 12); free(o);
		n = sweet_rect_rewrite("mysampler2DRect x;", 18, &o); CHECK(o && n == 18 && !strcmp(o, "mysampler2DRect x;")); free(o);   /* word boundary: untouched */
		n = sweet_rect_rewrite("texture2DRectX(a)", 17, &o); CHECK(o && n == 17); free(o);
		/* unparsable call shape: only the rename, still terminated */
		n = sweet_rect_rewrite("texture2DRect(", 14, &o); CHECK(o && n == 9 + 1 && !strncmp(o, "texture2D(", 10)); free(o);
		/* unbalanced parenthesis in the argument: rename only, no overrun */
		n = sweet_rect_rewrite("texture2DRect(t, (a", 19, &o); CHECK(o && !strncmp(o, "texture2D(t, (a", 15)); free(o);
		/* many occurrences: the output buffer bound holds */
		{
			char big[8192]; size_t bl = 0; int r;
			for (r = 0; r < 100; r++) bl += (size_t)snprintf(big + bl, sizeof big - bl, "uniform sampler2DRect s%d; v = texture2DRect(s%d, c);\n", r, r);
			n = sweet_rect_rewrite(big, bl, &o); CHECK(o && n > bl && !strstr(o, "Rect")); free(o);
		}
		/* declarator list: one inverse-size uniform per name; a parameter-like use is only renamed */
		n = sweet_rect_rewrite("uniform sampler2DRect a, b; v = texture2DRect(b, c);", 52, &o);
		CHECK(o && strstr(o, "uniform sampler2D a, b;\nuniform highp vec2 sweet_rect_inv_a;\n\nuniform highp vec2 sweet_rect_inv_b;\n") != NULL && strstr(o, "texture2D(b, (c) * sweet_rect_inv_b)") != NULL);
		free(o);
		n = sweet_rect_rewrite("vec4 f(sampler2DRect s) { return vec4(1.0); }", 44, &o);
		CHECK(o && strstr(o, "vec4 f(sampler2D s)") != NULL && !strstr(o, "Rect")); free(o);
		/* texture size table */
		sweet_gli_note_tex_size(7, 230, 186); sweet_gli_note_tex_size(7 + 1024, 10, 10);
		sweet_gli_note_tex_size(0, 5, 5);   /* name 0 is ignored */
	}

	{   /* extension description (research 86 sections E/F/H, DRAFT-format0-batch): token-exact matcher, line formats, once per context, NULL safety */
		char buf[8192]; FILE *f; size_t n;
		static struct sweet_gli tg;
		static const char *const probe[16] = { "GL_APPLE_fence", "GL_APPLE_texture_filter_cubic", "GL_APPLE_texture_ycbcr_basic_formats", "GL_APPLE_texture_ycbcr_extended_formats",
			"GL_APPLE_texture_xr", "GL_APPLE_tile_order_rot", "GL_EXT_shader_framebuffer_fetch", "GL_EXT_texture_filter_anisotropic", "GL_EXT_texture_lod_bias",
			"GL_OES_standard_derivatives", "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear", "GL_OES_texture_float", "GL_OES_texture_float_linear",
			"GL_IMG_texture_compression_pvrtc", "GL_KHR_texture_compression_astc_ldr" };
		CHECK(sweet_ext_has("GL_OES_texture_float_linear GL_APPLE_fence", "GL_APPLE_fence") && sweet_ext_has("GL_APPLE_fence", "GL_APPLE_fence"));
		CHECK(sweet_ext_has("A GL_OES_texture_float ", "GL_OES_texture_float"));
		CHECK(!sweet_ext_has("GL_OES_texture_float_linear", "GL_OES_texture_float"));       /* prefix of a longer name: no match */
		CHECK(!sweet_ext_has("GL_OES_texture_float", "GL_OES_texture_float_linear"));       /* longer than the token: no match */
		CHECK(!sweet_ext_has("XGL_APPLE_fence", "GL_APPLE_fence") && !sweet_ext_has("GL_APPLE_fence_x GL_APPLE_fenc", "GL_APPLE_fence"));
		CHECK(!sweet_ext_has(NULL, "x") && !sweet_ext_has("x", NULL) && !sweet_ext_has("", "x") && !sweet_ext_has("x", ""));
		/* printer: NULL strings */
		f = tmpfile(); sweet_gl_describe(f, NULL, NULL, NULL); n = (size_t)ftell(f); rewind(f); n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
		CHECK(strstr(buf, "SWEETGL: GL_VERSION (null)\nSWEETGL: GL_RENDERER (null)\nSWEETGL: GL_EXTENSIONS (null)\n") == buf);
		CHECK(strstr(buf, "SWEETGL: ext-check GL_APPLE_fence 0\n") && strstr(buf, "SWEETGL: ext-check GL_KHR_texture_compression_astc_ldr 0\n"));
		{ unsigned c = 0; for (const char *p = buf; (p = strstr(p, "SWEETGL: ext-check ")); p++) c++; CHECK(c == 16); }
		/* printer: chunks of <= 400 on word boundaries, trailing blank dropped, ext-checks by token */
		{
			char ext[3000]; size_t el = 0; int i; unsigned lines = 0, ok = 1; const char *p;
			el += (size_t)snprintf(ext + el, sizeof ext - el, "GL_APPLE_fence GL_OES_texture_float_linear ");
			for (i = 0; i < 60; i++) el += (size_t)snprintf(ext + el, sizeof ext - el, "GL_FAKE_extension_number_%02d ", i);
			f = tmpfile(); sweet_gl_describe(f, "OpenGL ES 3.0 Mesa", "Mali-G78", ext); rewind(f); n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
			CHECK(strstr(buf, "SWEETGL: GL_VERSION OpenGL ES 3.0 Mesa\nSWEETGL: GL_RENDERER Mali-G78\nSWEETGL: GL_EXTENSIONS GL_APPLE_fence GL_OES_texture_float_linear GL_FAKE") == buf);
			for (p = buf; (p = strstr(p, "SWEETGL: GL_EXTENSIONS ")); p++) {
				const char *e = strchr(p, '\n'); size_t cl = (size_t)(e - (p + 23));
				lines++; if (cl > 400 || cl == 0 || p[23 + cl - 1] == ' ') ok = 0;
			}
			CHECK(lines >= 5 && ok);
			CHECK(strstr(buf, "SWEETGL: ext-check GL_APPLE_fence 1\n") && strstr(buf, "SWEETGL: ext-check GL_OES_texture_float_linear 1\n") && strstr(buf, "SWEETGL: ext-check GL_OES_texture_float 0\n"));
			for (i = 0; i < 16; i++) { char l[96]; snprintf(l, sizeof l, "SWEETGL: ext-check %s ", probe[i]); CHECK(strstr(buf, l) != NULL); }
			/* every extension word reappears intact in exactly the chunk stream (no word cut) */
			{ char one[40]; int bad = 0; for (i = 0; i < 60; i++) { snprintf(one, sizeof one, "GL_FAKE_extension_number_%02d", i); if (!strstr(buf, one)) bad++; } CHECK(!bad); }
		}
		/* sgli_GetString: once per context, only for 0x1f03, return value unchanged. stderr is redirected to a file. */
		{
			static int mA, mB; int saved = dup(2); char path[] = "/tmp/gli-gs-XXXXXX"; int fd = mkstemp(path);
			typedef const unsigned char *(*f_gs)(void *, unsigned);
			const unsigned char *r1, *r2;
			sweet_gli_init(&tg, &mA);
			sweet_gli_ensure_current = ens; ens_ret = 0;
			gs_ver = "V1"; gs_rend = "R1"; gs_ext = "GL_APPLE_fence GL_OES_texture_float_linear";
			fflush(stderr); dup2(fd, 2);
			((f_gs)tg.disp[slot_of("glGetString")])(&mA, 0x1f02);   /* other names: no description */
			((f_gs)tg.disp[slot_of("glGetString")])(&mA, 0x1f01);
			r1 = ((f_gs)tg.disp[slot_of("glGetString")])(&mA, 0x1f03);
			r2 = ((f_gs)tg.disp[slot_of("glGetString")])(&mA, 0x1f03);   /* same context again: silent */
			((f_gs)tg.disp[slot_of("glGetString")])(&mB, 0x1f03);        /* a second context prints again */
			gs_ext = NULL;
			CHECK(((f_gs)tg.disp[slot_of("glGetString")])(&mA, 0x1f03) == NULL);   /* NULL string is returned unchanged */
			fflush(stderr); dup2(saved, 2); close(saved);
			CHECK(r1 == (const unsigned char *)gs_ver || (r1 && !strcmp((const char *)r1, "GL_APPLE_fence GL_OES_texture_float_linear")));
			CHECK(r2 == r1);
			lseek(fd, 0, SEEK_SET); n = (size_t)read(fd, buf, sizeof buf - 1); buf[n] = 0; close(fd); unlink(path);
			{ unsigned c = 0; for (const char *p = buf; (p = strstr(p, "SWEETGL: GL_VERSION V1\n")); p++) c++; CHECK(c == 2); }
			CHECK(strstr(buf, "SWEETGL: GL_RENDERER R1\nSWEETGL: GL_EXTENSIONS GL_APPLE_fence GL_OES_texture_float_linear\n") != NULL);
			CHECK(strstr(buf, "SWEETGL: ext-check GL_APPLE_fence 1\n") != NULL && strstr(buf, "SWEETGL: ext-check GL_OES_texture_float 0\n") != NULL);
		}
		gs_ver = gs_rend = gs_ext = NULL;
	}

	{   /* A2 (research 86 sections E/F/H, DRAFT-format0-batch): status names and exact line formats. The hooked Clear path itself is not host-testable:
	     * the generated fake glGetIntegerv writes nothing, so fbo_check always sees FBO 0 and returns early. */
		char l[256];
		CHECK(!strcmp(sweet_fbo_status_name(0x8CD6), "INCOMPLETE_ATTACHMENT") && !strcmp(sweet_fbo_status_name(0x8CD7), "MISSING_ATTACHMENT") &&
		      !strcmp(sweet_fbo_status_name(0x8CD9), "INCOMPLETE_DIMENSIONS") && !strcmp(sweet_fbo_status_name(0x8CDD), "UNSUPPORTED") &&
		      !strcmp(sweet_fbo_status_name(0x8D56), "INCOMPLETE_MULTISAMPLE") && !strcmp(sweet_fbo_status_name(0x1234), "other") && !strcmp(sweet_fbo_status_name(0), "other"));
		sweet_fbo_status_line(l, sizeof l, 7, "Clear", 0x8CD6, 0x8D41, 12, 180, 90, 0x8058);
		CHECK(!strcmp(l, "SWEETGL: FBO incomplete fbo 7 in Clear status 0x8cd6 (INCOMPLETE_ATTACHMENT) attachment color0 type 0x8d41 name 12 level0 180x90 ifmt 0x8058"));
		sweet_fbo_sizes_line(l, sizeof l, 7, 8, 8, 8, -1);
		CHECK(!strcmp(l, "SWEETGL: FBO incomplete fbo 7 color0 sizes r 8 g 8 b 8 a -1"));
	}

	{   /* research 90 L1/L2/L4: SWEETGL_CENSUS + GLDEBUG per-class caps */
		static const int cs[14] = { 56, 65, 67, 405, 762, 763, 595, 597, 598, 54, 59, 98, 677, 1031 };   /* last five: per-texture history slots that nothing else hooks */
		static struct sweet_gli tc;
		typedef void (*f_dr)(void *, unsigned, int, int);
		typedef void (*f_ss)(void *, unsigned, int, const char *const *, const int *);
		typedef void (*f_as)(void *, unsigned, unsigned);
		typedef void (*f_cp)(void *, unsigned, int, int, int, int, int, int, int);
		typedef unsigned (*f_cprog)(void *);
		char l[512];
		unsigned long c0[6][3], c1[6][3], cp0, cp1;
		/* census OFF (t was initialised without SWEETGL_CENSUS): every touched slot is exactly the generated entry */
		for (int k = 0; k < 14; k++) CHECK(cs[k] == 677 || t.disp[cs[k]] == sweet_gli_defaults[cs[k]]);   /* 677 is always hooked since ALL-ON T-28 */
		/* exact line formats, zero counts printed */
		{
			unsigned long z[6][3]; memset(z, 0, sizeof z);
			sweet_census_line(l, sizeof l, 20, z, 0);
			CHECK(!strcmp(l, "SWEETGL: BDROP census t+20s blur S/T/O 0/0/0 fbfetch S/T/O 0/0/0 cmatrix S/T/O 0/0/0 lum S/T/O 0/0/0 rect S/T/O 0/0/0 other S/T/O 0/0/0 copytex 0"));
			z[0][0] = 1; z[1][1] = 22; z[2][2] = 333; z[3][0] = 4; z[4][1] = 5; z[5][2] = 66; 
			sweet_census_line(l, sizeof l, 100, z, 7);
			CHECK(!strcmp(l, "SWEETGL: BDROP census t+100s blur S/T/O 1/0/0 fbfetch S/T/O 0/22/0 cmatrix S/T/O 0/0/333 lum S/T/O 4/0/0 rect S/T/O 0/5/0 other S/T/O 0/0/66 copytex 7"));
		}
		sweet_copytex_line(l, sizeof l, 1, 0xde1, 0, 1, 2, 3, 4, 97, 76, 5, 1, 0x8cd5, 1, 25, 97, 76, 0x8058);
		CHECK(!strcmp(l, "SWEETGL: copytex #1 target 0xde1 level 0 xoff 1 yoff 2 x 3 y 4 w 97 h 76 readfb 5 readstatus 0x8cd5 dst 25 97x76 ifmt 0x8058"));
		sweet_copytex_line(l, sizeof l, 32, 0x8513, 1, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0);
		CHECK(!strcmp(l, "SWEETGL: copytex #32 target 0x8513 level 1 xoff 0 yoff 0 x 0 y 0 w 8 h 8 readfb 0 readstatus n/a dst n/a"));
		/* class boundaries */
		CHECK(sweet_census_class(1000, 5) == 0 && sweet_census_class(999, 5) == 2 && sweet_census_class(999, 999) == 2 && sweet_census_class(1125, 2436) == 0);
		CHECK(sweet_census_class(256, 256) == 1 && sweet_census_class(257, 256) == 2 && sweet_census_class(256, 257) == 2 && sweet_census_class(0, 0) == 1 && sweet_census_class(97, 76) == 1);
		/* kind tagging from sample texts */
		CHECK(sweet_census_kinds("uniform lowp vec4 weight[2]; texture2D(texture0, texcoord0)", 56) == 1);
		CHECK(sweet_census_kinds("#extension GL_EXT_shader_framebuffer_fetch : require\nlowp vec4 d0 = gl_LastFragData[0];", strlen("#extension GL_EXT_shader_framebuffer_fetch : require\nlowp vec4 d0 = gl_LastFragData[0];")) == 2);
		{
			static const char multi[] = "uniform sampler2DRect texture0; uniform mat4 color_matrix; float tlum; weight; gl_LastFragData";
			CHECK(sweet_census_kinds(multi, sizeof multi - 1) == (1u | 2u | 4u | 8u | 16u));
			CHECK(sweet_census_kinds("luminanceCurve", 14) == 8 && sweet_census_kinds("tlum", 4) == 8);
			CHECK(sweet_census_kinds("void main() {}", 14) == 0 && sweet_census_kinds(NULL, 0) == 0);
			CHECK(sweet_census_kinds("weigh", 5) == 0 && sweet_census_kinds("weight and more", 6) == 1);   /* the length bounds the search */
		}
		/* counting: several kinds, other, classes */
		sweet_census_snapshot(c0, &cp0);
		sweet_census_count(1u | 2u, 1125, 2436); sweet_census_count(0, 100, 100); sweet_census_count(16u, 999, 999);
		sweet_census_snapshot(c1, &cp1);
		CHECK(c1[0][0] - c0[0][0] == 1 && c1[1][0] - c0[1][0] == 1 && c1[5][1] - c0[5][1] == 1 && c1[4][2] - c0[4][2] == 1 && c1[2][0] == c0[2][0] && cp1 == cp0);
		/* census ON: hooks installed, tag flow through ShaderSource -> AttachShader, call-through exactly once */
		setenv("SWEETGL_CENSUS", "1", 1);
		sweet_gli_init(&tc, &dummy);
		sweet_gli_ensure_current = ens; ens_ret = 0;
		for (int k = 0; k < 14; k++) CHECK(tc.disp[cs[k]] != sweet_gli_defaults[cs[k]]);
		{
			const char *src[2] = { "uniform lowp vec4 weight[2];", " gl_LastFragData[0]; " };
			fake_ret = 9;
			hits = 0;
			CHECK(((f_cprog)tc.disp[597])(tc.rctx) == 9 && hits == 1 && !strcmp(hit_names[0], "_mesa_CreateProgram"));
			hits = 0; memset(first_args, 0, sizeof first_args);
			((f_ss)tc.disp[595])(tc.rctx, 77, 2, src, NULL);
			CHECK(hits == 1 && !strcmp(hit_names[0], "_mesa_ShaderSource") && first_args[0] == 77 && first_args[1] == 2);
			CHECK(sweet_census_shader_kinds(tc.rctx, 77) == 3 && sweet_census_shader_kinds(tc.rctx, 78) == 0);
			hits = 0; memset(first_args, 0, sizeof first_args);
			((f_as)tc.disp[598])(tc.rctx, 9, 77);
			CHECK(hits == 1 && !strcmp(hit_names[0], "_mesa_AttachShader") && first_args[0] == 9 && first_args[1] == 77);
			CHECK(sweet_census_program_kinds(tc.rctx, 9) == 3);
			((f_as)tc.disp[598])(tc.rctx, 9, 78);   /* an untagged shader adds nothing */
			CHECK(sweet_census_program_kinds(tc.rctx, 9) == 3);
			fake_ret = 9; ((f_cprog)tc.disp[597])(tc.rctx);   /* the id is reused: record reset */
			CHECK(sweet_census_program_kinds(tc.rctx, 9) == 0);
			fake_ret = 0;
		}
		/* review census-copytex-b2 F1/F2: every installed draw slot, exact GL call sequence (viewport/program queries only, never glGetError), counter delta 1, original last */
		{
			static const struct { int slot; const char *orig; } dr[5] = { { 65, "_mesa_DrawArrays" }, { 67, "_mesa_DrawElements" }, { 405, "_mesa_DrawRangeElements" },
			    { 762, "_mesa_DrawArraysInstanced" }, { 763, "_mesa_DrawElementsInstanced" } };
			typedef void (*f_dre)(void *, unsigned, int, unsigned, const void *);
			typedef void (*f_drr)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *);
			typedef void (*f_dai)(void *, unsigned, int, int, int);
			typedef void (*f_dei)(void *, unsigned, int, unsigned, const void *, int);
			static int mX, mY;
			inj_on = 1; inj_prog = 0; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 1125; inj_vp[3] = 2436;
			((f_dr)tc.disp[65])(tc.rctx, 4, 0, 3);   /* prime: the first draw of a context installs the debug sink */
			for (int k = 0; k < 5; k++) {
				int ng = 0;
				sweet_census_snapshot(c0, &cp0);
				seq_n = 0; hits = 0;
				switch (dr[k].slot) {
				case 65: ((f_dr)tc.disp[65])(tc.rctx, 4, 0, 3); break;
				case 67: ((f_dre)tc.disp[67])(tc.rctx, 4, 3, 0x1403, NULL); break;
				case 405: ((f_drr)tc.disp[405])(tc.rctx, 4, 0, 2, 3, 0x1403, NULL); break;
				case 762: ((f_dai)tc.disp[762])(tc.rctx, 4, 0, 3, 2); break;
				default: ((f_dei)tc.disp[763])(tc.rctx, 4, 3, 0x1403, NULL, 2); break;
				}
				sweet_census_snapshot(c1, &cp1);
				for (unsigned q = 0; q < seq_n && q < 64; q++) if (!strcmp(seq_name[q], "_mesa_GetError")) ng++;
				CHECK(seq_n == 3 && !strcmp(seq_name[0], "_mesa_GetIntegerv") && seq_a0[0] == 0x8B8D && !strcmp(seq_name[1], "_mesa_GetIntegerv") && seq_a0[1] == 0x0BA2 && !strcmp(seq_name[2], dr[k].orig));
				CHECK(ng == 0);
				CHECK(c1[5][0] - c0[5][0] == 1);   /* program 0, viewport 1125x2436: other / S */
			}
			/* viewport 97x76 with a tagged program: counted under blur / T */
			{
				const char *bs[1] = { "weight" };
				fake_ret = 4; ((f_cprog)tc.disp[597])(tc.rctx); fake_ret = 0;
				((f_ss)tc.disp[595])(tc.rctx, 90, 1, bs, NULL);
				((f_as)tc.disp[598])(tc.rctx, 4, 90);
				inj_prog = 4; inj_vp[2] = 97; inj_vp[3] = 76;
				sweet_census_snapshot(c0, &cp0);
				((f_dr)tc.disp[65])(tc.rctx, 4, 0, 3);
				sweet_census_snapshot(c1, &cp1);
				CHECK(c1[0][1] - c0[0][1] == 1 && c1[5][1] == c0[5][1]);
				inj_prog = 0;
			}
			/* F3: the debug callback is installed once per context (3 draws on mX, 1 on mY = 2 installs, each with both Enable calls) */
			{
				unsigned d0 = dbgcb_n, en = 0;
				seq_n = 0;
				for (int i = 0; i < 3; i++) ((f_dr)tc.disp[65])(&mX, 4, 0, 3);
				((f_dr)tc.disp[65])(&mY, 4, 0, 3);
				for (unsigned q = 0; q < seq_n && q < 64; q++) if (!strcmp(seq_name[q], "_mesa_Enable") && seq_a0[q] == 0x92E0) en++;
				CHECK(dbgcb_n - d0 == 2 && en == 2);
			}
		}
		{
			/* F3 (this copytex call takes index #1 of the 32 without logging; the cap test below accounts for it): no current context (ensure fails): draw and copytex hooks make no GL query and log nothing; the draw is not counted, the copytex call is */
			{
				int saved = dup(2); char path[] = "/tmp/gli-cen2-XXXXXX"; int fd = mkstemp(path); char big[256]; ssize_t n;
				ens_ret = -1;
				sweet_census_snapshot(c0, &cp0);
				seq_n = 0;
				fflush(stderr); dup2(fd, 2);
				((f_dr)tc.disp[65])(tc.rctx, 4, 0, 3);
				((f_cp)tc.disp[56])(tc.rctx, 0xde1, 0, 1, 2, 3, 4, 5, 6);
				fflush(stderr); dup2(saved, 2); close(saved);
				ens_ret = 0;
				sweet_census_snapshot(c1, &cp1);
				lseek(fd, 0, SEEK_SET); n = read(fd, big, sizeof big - 1); close(fd); unlink(path);
				CHECK(n == 0 && seq_n == 0);
				CHECK(cp1 - cp0 == 1);
				{ int same = 1; for (int k = 0; k < 6; k++) for (int j = 0; j < 3; j++) if (c0[k][j] != c1[k][j]) same = 0; CHECK(same); }
			}
		}
		/* copytex: first 32 logged (stderr to a file), all counted; the original runs once, last, with ALL 8 original arguments; exact query sequence */
		sweet_census_snapshot(c0, &cp0);
		{
			int saved = dup(2); char path[] = "/tmp/gli-cen-XXXXXX"; int fd = mkstemp(path); char big[16384]; size_t n; unsigned lines = 0;
			inj_on = 1; inj_readfb = 5; inj_tex = 25; inj_w = 97; inj_h = 76; inj_ifmt = 0x8058; fake_ret = 0x8cd5;
			fflush(stderr); dup2(fd, 2);
			for (int i = 0; i < 40; i++) {
				int ng = 0;
				memset(cts_args, 0, sizeof cts_args);
				seq_n = 0; hits = 0;
				((f_cp)tc.disp[56])(tc.rctx, 0xde1, 12, 13, 14, 15, 16, 17, 18);
				for (unsigned q = 0; q < seq_n && q < 64; q++) if (!strcmp(seq_name[q], "_mesa_GetError")) ng++;
				CHECK(ng == 0 && seq_n == (i < 31 ? 7u : 1u));   /* past the 32-line cap only the original runs */
				if (i < 31) CHECK(!strcmp(seq_name[0], "_mesa_GetIntegerv") && seq_a0[0] == 0x8CAA && !strcmp(seq_name[1], "_mesa_CheckFramebufferStatus") && seq_a0[1] == 0x8CA8 &&
				      !strcmp(seq_name[2], "_mesa_GetIntegerv") && seq_a0[2] == 0x8069 && !strcmp(seq_name[3], "_mesa_GetTexLevelParameteriv") && seq_a0[3] == 0xde1 &&
				      !strcmp(seq_name[4], "_mesa_GetTexLevelParameteriv") && !strcmp(seq_name[5], "_mesa_GetTexLevelParameteriv") && !strcmp(seq_name[6], "_mesa_CopyTexSubImage2D") && hits == 7);
				if (i >= 31) CHECK(!strcmp(seq_name[0], "_mesa_CopyTexSubImage2D"));
				CHECK(cts_args[0] == 0xde1 && cts_args[1] == 12 && cts_args[2] == 13 && cts_args[3] == 14 && cts_args[4] == 15 && cts_args[5] == 16 && cts_args[6] == 17 && cts_args[7] == 18);
			}
			fflush(stderr); dup2(saved, 2); close(saved);
			inj_on = 0; fake_ret = 0;
			lseek(fd, 0, SEEK_SET); n = (size_t)read(fd, big, sizeof big - 1); big[n] = 0; close(fd); unlink(path);
			for (const char *p = big; (p = strstr(p, "SWEETGL: copytex #")); p++) lines++;
			CHECK(lines == 31);   /* #1 was the no-context call: counted, not logged */
			CHECK(strstr(big, "SWEETGL: copytex #1 ") == NULL);
			CHECK(strstr(big, "SWEETGL: copytex #2 target 0xde1 level 12 xoff 13 yoff 14 x 15 y 16 w 17 h 18 readfb 5 readstatus 0x8cd5 dst 25 97x76 ifmt 0x8058\n") != NULL);
			CHECK(strstr(big, "copytex #32 ") != NULL && strstr(big, "copytex #33 ") == NULL);
			sweet_census_snapshot(c1, &cp1);
			CHECK(cp1 - cp0 == 40);   /* cp0 is taken after the no-context call */
		}
		unsetenv("SWEETGL_CENSUS");
		/* GLDEBUG: classes and per-class caps; the dropped note appears at the 1st drop (and 10th, ...), the other class keeps its own budget */
		{
			char note[160], msg[64];
			int printed1 = 0, printed2 = 0, notes = 0, first_note_ok = 0;
			CHECK(sweet_gldebug_class("FBO error") == 1 && sweet_gldebug_class("FBO incomplete: x") == 1 && sweet_gldebug_class("Framebuffer is incomplete") == 1 && sweet_gldebug_class("GL_INVALID_ENUM error 5") == 2 &&
			      sweet_gldebug_class("shader stats: 1 inst") == 0 && sweet_gldebug_class(NULL) == 0);
			/* review runplan-gputest113 F2: Mesa's real GL-error text has no "rror" in it */
			CHECK(sweet_gldebug_class("GL_INVALID_OPERATION in glReadPixels(format/type)") == 2 && sweet_gldebug_class("GL_INVALID_ENUM in glGetIntegerv(pname=0x8b9b)") == 2 && sweet_gldebug_class("GL_INVALID_FRAMEBUFFER_OPERATION in glClear(incomplete)") == 1 &&
			      sweet_gldebug_class("GL_OUT_OF_MEMORY in glTexImage2D") == 2 && sweet_gldebug_class("GL_INVALID_VALUE in glViewport(w<0)") == 2);
			CHECK(sweet_gldebug_class("shader compiler: flush 3 draws") == 0 && sweet_gldebug_class("perf: 12 instructions") == 0 && sweet_gldebug_class("panfrost: GL_ARB chatter") == 0 && sweet_gldebug_class("") == 0);
			for (int i = 0; i < 40; i++) {
				snprintf(msg, sizeof msg, "FBO incomplete: reason %d", i);
				printed1 += sweet_gldebug_accept(msg, note, sizeof note);
				if (note[0]) { notes++; if (i == 32) first_note_ok = !strcmp(note, "SWEETGL: GLDEBUG dropped 1 (class fbo, cap 32 per class)"); }
			}
			CHECK(printed1 == 32 && notes == 1 && first_note_ok);   /* drops 1..8 of class fbo: only the 1st prints a note */
			for (int i = 0; i < 40; i++) {
				snprintf(msg, sizeof msg, "GL_INVALID_OPERATION in glReadPixels(format/type) %d", i);   /* the real Mesa text: only the new filter lets it through */
				printed2 += sweet_gldebug_accept(msg, note, sizeof note);
			}
			CHECK(printed2 == 32);   /* FBO noise did not use the error budget */
			CHECK(sweet_gldebug_accept("GL_INVALID_OPERATION in glReadPixels(format/type) 39", note, sizeof note) == 0 && note[0] == 0);   /* identical to the last message: not a new drop */
			CHECK(sweet_gldebug_accept("perf chatter", note, sizeof note) == 0 && note[0] == 0);
			{   /* error-class drops so far: 8 (i = 32..39); the 10th drop prints a note naming the class */
				int got = 0;
				for (int i = 100; i < 104; i++) { snprintf(msg, sizeof msg, "boom rror %d", i); sweet_gldebug_accept(msg, note, sizeof note); if (!strcmp(note, "SWEETGL: GLDEBUG dropped 10 (class error, cap 32 per class)")) got = 1; }
				CHECK(got);
			}
		}
	}

	{   /* per-texture event history (SWEETGL_CENSUS=1): formats, ring, eviction, trigger, hooks, call-through, census off */
		static struct sweet_gli th, thoff;
		typedef void (*f_gen)(void *, int, unsigned *);
		typedef void (*f_bt)(void *, unsigned, unsigned);
		typedef void (*f_ti)(void *, unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
		typedef void (*f_ts)(void *, unsigned, int, unsigned, int, int);
		typedef void (*f_ci)(void *, unsigned, int, unsigned, int, int, int, int, int);
		typedef void (*f_fbt)(void *, unsigned, unsigned, unsigned, unsigned, int);
		typedef void (*f_cl)(void *, unsigned);
		char l[1024], big[16384];
		struct sweet_th_ev e;
		/* the slot numbers are the ones of Apple's table */
		CHECK(slot_of("glBindTexture") == 5 && slot_of("glCopyTexImage2D") == 54 && slot_of("glDeleteTextures") == 59 && slot_of("glGenTextures") == 98);
		CHECK(slot_of("glTexImage2D") == 301 && slot_of("glFramebufferTexture2D") == 677 && slot_of("glTexStorage2D") == 1031);
		/* exact event formats */
		memset(&e, 0, sizeof e); e.seq = 1; e.kind = SWEET_TH_GEN; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#1 genned"));
		e.seq = 2; e.kind = SWEET_TH_BIND; e.a = 0xde1; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#2 bind 0xde1"));
		e.seq = 3; e.kind = SWEET_TH_TEXIMAGE; e.a = 0xde1; e.b = 0; e.c = 97; e.d = 76; e.e = 0x1908; e.f = 0; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#3 teximage2d 0xde1 level 0 97x76 ifmt 0x1908 data null"));
		e.f = 1; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#3 teximage2d 0xde1 level 0 97x76 ifmt 0x1908 data ptr"));
		e.seq = 4; e.kind = SWEET_TH_TEXSTORAGE; e.b = 3; e.e = 0x8058; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#4 texstorage2d 0xde1 levels 3 97x76 ifmt 0x8058"));
		e.seq = 5; e.kind = SWEET_TH_COPYIMAGE; e.b = 1; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#5 copyteximage2d 0xde1 level 1 97x76 ifmt 0x8058"));
		e.seq = 6; e.kind = SWEET_TH_FBOTEX; e.a = 0x8ce0; e.b = 0; e.c = 6; e.e = 0xde1; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#6 fbotex fbo 6 attach 0x8ce0 textarget 0xde1 level 0"));
		e.c = -1; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#6 fbotex fbo ? attach 0x8ce0 textarget 0xde1 level 0"));
		e.seq = 7; e.kind = SWEET_TH_DELETED; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#7 deleted"));
		e.seq = 8; e.kind = SWEET_TH_ATTACH; e.a = 77; e.c = 1125; e.d = 2436; e.e = 0x42475241; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#8 attachimage 77 1125x2436 fmt 0x42475241"));
		e.seq = 9; e.kind = SWEET_TH_BIND; e.a = 0xde1; e.ctx = 2; sweet_th_event_str(l, sizeof l, &e); CHECK(!strcmp(l, "#9 ctx 2 bind 0xde1"));
		/* ring wraps at 12: 14 events keep #3..#14 and say that 2 were dropped; lines stay within 400 chars and continue with the same prefix + cont */
		sweet_th_reset();
		for (int i = 1; i <= 14; i++) { memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXIMAGE; e.a = 0xde1; e.c = i; e.d = i; e.e = 0x1908; sweet_th_record(7, e); }
		{
			unsigned k, pieces = 0; int ok = 1;
			big[0] = 0;
			for (k = 0; sweet_th_line(7, k, l, sizeof l); k++) {
				ok &= strlen(l) <= 400;
				ok &= k ? !strncmp(l, "SWEETGL: TEXHIST cont tex 7 events 14: #", sizeof "SWEETGL: TEXHIST cont tex 7 events 14: #" - 1) : !strncmp(l, "SWEETGL: TEXHIST tex 7 events 14: (", sizeof "SWEETGL: TEXHIST tex 7 events 14: (" - 1);
				strcat(big, l); strcat(big, "\n");
			}
			CHECK(ok && k >= 2 && k <= 4);
			CHECK(strstr(big, "SWEETGL: TEXHIST tex 7 events 14: (2 older events dropped by the ring of 12) | #3 teximage2d 0xde1 level 0 3x3 ifmt 0x1908 data null | #4 ") != NULL);
			CHECK(strstr(big, "#2 ") == NULL && strstr(big, "#14 teximage2d 0xde1 level 0 14x14 ifmt 0x1908 data null") != NULL);
			for (const char *q = big; (q = strstr(q, " teximage2d ")); q++) pieces++;
			CHECK(pieces == 12);
		}
		/* binds: the first 3 are events, the rest a counter; unknown texture: the exact "no events" line */
		sweet_th_reset();
		for (int i = 0; i < 5; i++) { memset(&e, 0, sizeof e); e.kind = SWEET_TH_BIND; e.a = 0xde1; sweet_th_record(8, e); }
		CHECK(sweet_th_line(8, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 8 events 3: #1 bind 0xde1 | #2 bind 0xde1 | #3 bind 0xde1 | binds 5 (only the first 3 logged)"));
		CHECK(!sweet_th_line(8, 1, l, sizeof l));
		CHECK(sweet_th_line(99, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 99 no events recorded (table cap or created before the hook)") && !sweet_th_line(99, 1, l, sizeof l));
		sweet_th_stats_line(l, sizeof l);
		CHECK(!strcmp(l, "SWEETGL: TEXHIST stats events 3 textures 1 of 1024 evictions 0 binds_unlogged 2 unattributed 0"));
		/* table full: the entry with the oldest last event is replaced and the replacement is counted */
		sweet_th_reset();
		for (unsigned n = 1; n <= 1024; n++) { memset(&e, 0, sizeof e); e.kind = SWEET_TH_GEN; sweet_th_record(n, e); }   /* GLI_TH_TAB */
		memset(&e, 0, sizeof e); e.kind = SWEET_TH_DELETED; sweet_th_record(1, e);   /* texture 1 is now the newest, texture 2 the oldest */
		sweet_th_stats_line(l, sizeof l);
		CHECK(!strcmp(l, "SWEETGL: TEXHIST stats events 1025 textures 1024 of 1024 evictions 0 binds_unlogged 0 unattributed 0"));
		memset(&e, 0, sizeof e); e.kind = SWEET_TH_GEN; sweet_th_record(2000, e);
		sweet_th_stats_line(l, sizeof l);
		CHECK(!strcmp(l, "SWEETGL: TEXHIST stats events 1026 textures 1024 of 1024 evictions 1 binds_unlogged 0 unattributed 0"));
		CHECK(sweet_th_line(2, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL);
		CHECK(sweet_th_line(1, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 1 events 2: #1 genned | #1025 deleted"));
		CHECK(sweet_th_line(2000, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 2000 events 1: #1026 genned"));

		/* review runplan-gputest112 F4: FBO-attached textures without storage event (census thread dump) */
		{
			unsigned pn = 0; int pd = 0;
			sweet_th_reset();
			memset(&e, 0, sizeof e);
			e.kind = SWEET_TH_GEN; sweet_th_record(10, e); e.kind = SWEET_TH_FBOTEX; e.a = 0x8ce0; e.c = 6; e.e = 0xde1; sweet_th_record(10, e);          /* fbotex only: selected */
			e.kind = SWEET_TH_GEN; sweet_th_record(11, e); e.kind = SWEET_TH_FBOTEX; sweet_th_record(11, e);
			e.kind = SWEET_TH_TEXIMAGE; e.a = 0xde1; e.c = 4; e.d = 4; e.e = 0x1908; sweet_th_record(11, e);                                         /* with teximage2d: not */
			e.kind = SWEET_TH_GEN; e.a = 0; sweet_th_record(12, e);                                                                                    /* never attached: not */
			e.kind = SWEET_TH_FBOTEX; e.a = 0x8ce0; sweet_th_record(13, e); e.kind = SWEET_TH_ATTACH; sweet_th_record(13, e);                          /* attachimage: not */
			e.kind = SWEET_TH_FBOTEX; sweet_th_record(14, e); e.kind = SWEET_TH_TEXSTORAGE; sweet_th_record(14, e);                                    /* texstorage2d: not */
			e.kind = SWEET_TH_FBOTEX; sweet_th_record(15, e); e.kind = SWEET_TH_COPYIMAGE; sweet_th_record(15, e);                                     /* copyteximage2d: not */
			CHECK(sweet_th_fbo_pick(&pn, &pd) && pn == 10 && pd == 0);
			CHECK(!sweet_th_fbo_pick(&pn, &pd));   /* once per name: 10 is not reported again, nothing else qualifies */
			/* the ring dropped events: absence cannot be claimed -> ring-dropped */
			sweet_th_reset();
			e.kind = SWEET_TH_FBOTEX; for (int i = 0; i < 14; i++) sweet_th_record(20, e);
			CHECK(sweet_th_fbo_pick(&pn, &pd) && pn == 20 && pd == 1);
			/* cap of 8 reports per process, each name once */
			sweet_th_reset();
			e.kind = SWEET_TH_FBOTEX; for (unsigned n = 100; n < 112; n++) sweet_th_record(n, e);
			{ unsigned got = 0, seen[16]; int dup = 0; while (sweet_th_fbo_pick(&pn, &pd)) { for (unsigned z = 0; z < got; z++) if (seen[z] == pn) dup = 1; if (got < 16) seen[got] = pn; got++; if (got > 12) break; }
			  CHECK(got == 8 && !dup); }
			/* the report lines: exact text, split like the incomplete-FBO dump */
			sweet_th_reset();
			memset(&e, 0, sizeof e); e.kind = SWEET_TH_GEN; sweet_th_record(30, e);
			e.kind = SWEET_TH_FBOTEX; e.a = 0x8ce0; e.c = 6; e.e = 0xde1; sweet_th_record(30, e);
			e.kind = SWEET_TH_FBOTEX; for (int i = 0; i < 14; i++) sweet_th_record(31, e);
			cap_begin(); sweet_th_fbo_report(); sweet_th_fbo_report(); cap_end(big, sizeof big);   /* the second call: everything already reported */
			{ unsigned nl = 0; for (const char *q = big; *q; q++) nl += *q == '\n'; CHECK(nl == 4); }
			CHECK(strstr(big, "SWEETGL: TEXHIST fbo-no-storage tex 30 events 2: #1 genned | #2 fbotex fbo 6 attach 0x8ce0 textarget 0xde1 level 0\nSWEETGL: TEXHIST fbo-ring-dropped tex 31 events 14: (2 older events dropped by the ring of 12) | #5 fbotex fbo 6 ") == big);
			CHECK(strstr(big, "\nSWEETGL: TEXHIST cont fbo-ring-dropped tex 31 events 14: #10 fbotex fbo 6 ") != NULL);
			CHECK(strstr(big, "\nSWEETGL: TEXHIST cont fbo-ring-dropped tex 31 events 14: #16 fbotex fbo 6 attach 0x8ce0 textarget 0xde1 level 0\n") != NULL);
			CHECK(strstr(big, "fbo-no-storage tex 31") == NULL);   /* a texture with dropped events is never claimed to have no storage event */
		}

		/* review texhist-b3 F3: a texture bound after its 3rd bind keeps its slot (the unlogged binds refresh its age); F4: GEN starts a new bind count */
		sweet_th_reset();
		memset(&e, 0, sizeof e); e.kind = SWEET_TH_GEN; sweet_th_record(1, e);
		e.kind = SWEET_TH_BIND; e.a = 0xde1; for (int i = 0; i < 3; i++) sweet_th_record(1, e);
		e.kind = SWEET_TH_GEN; e.a = 0; for (unsigned n = 2; n <= 1024; n++) sweet_th_record(n, e);
		e.kind = SWEET_TH_BIND; e.a = 0xde1; sweet_th_record(1, e);   /* 4th bind: counter only */
		e.kind = SWEET_TH_GEN; e.a = 0; sweet_th_record(2000, e);     /* full table: the victim must be texture 2, not the bound texture 1 */
		CHECK(sweet_th_line(1, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 1 events 4: #1 genned | #2 bind 0xde1 | #3 bind 0xde1 | #4 bind 0xde1 | binds 4 (only the first 3 logged)"));
		CHECK(sweet_th_line(2, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL);
		sweet_th_stats_line(l, sizeof l); CHECK(strstr(l, "evictions 1 ") != NULL);
		sweet_th_reset();
		memset(&e, 0, sizeof e); e.kind = SWEET_TH_BIND; e.a = 0xde1; for (int i = 0; i < 5; i++) sweet_th_record(8, e);
		e.kind = SWEET_TH_GEN; e.a = 0; sweet_th_record(8, e);
		e.kind = SWEET_TH_BIND; e.a = 0xde1; sweet_th_record(8, e);
		CHECK(sweet_th_line(8, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 8 events 5: #1 bind 0xde1 | #2 bind 0xde1 | #3 bind 0xde1 | #4 genned | #5 bind 0xde1"));

		/* hooks: SWEETGL_CENSUS=1 with the rect hook on (BindTexture must be wrapped, not replaced) */
		setenv("SWEETGL_RECT", "1", 1); setenv("SWEETGL_CENSUS", "1", 1);
		sweet_gli_init(&th, &dummy);
		unsetenv("SWEETGL_CENSUS");
		sweet_gli_init(&thoff, &dummy);   /* census OFF again: sets census_on = 0, the table must be exactly what the same environment gave before */
		{
			static struct sweet_gli tref;
			sweet_gli_init(&tref, &dummy);
			CHECK(memcmp(thoff.disp, tref.disp, sizeof thoff.disp) == 0);
			CHECK(th.disp[5] != tref.disp[5] && th.disp[301] != tref.disp[301]);
			CHECK(th.disp[5] != sweet_gli_defaults[5] && th.disp[301] != sweet_gli_defaults[301]);
		}
		{   /* census off: the attach note is a no-op */
			sweet_th_reset();
			sweet_th_note_attach(25, 77, 97, 76, 1);
			CHECK(sweet_th_line(25, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL);
		}
		setenv("SWEETGL_CENSUS", "1", 1);
		sweet_gli_init(&th, &dummy);   /* census_on = 1 again */
		unsetenv("SWEETGL_CENSUS");
		sweet_gli_ensure_current = ens; ens_ret = 0;
		sweet_th_reset();
		inj_on = 1; inj_tex = 25; inj_fbo = 6;
		{
			unsigned names[2] = { 0xdead, 0xbeef };
			inj_gen_on = 1; inj_gen_base = 25;   /* the fake writes 25, 26 inside the original only */
			/* GenTextures: original once with the same arguments, no query, names read from the pointer */
			seq_n = 0; hits = 0;
			((f_gen)th.disp[98])(th.rctx, 2, names);
			inj_gen_on = 0;
			CHECK(seq_n == 1 && count_seq("_mesa_GenTextures") == 1 && args[0] == 2 && args[1] == (unsigned long)(uintptr_t)names && names[0] == 25 && names[1] == 26);
			CHECK(sweet_th_line(25, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 25 events 1: #1 ctx 1 genned"));
			CHECK(sweet_th_line(26, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 26 events 1: #2 ctx 1 genned"));
			/* BindTexture: the rect hook still maps 0x84f5 -> 0xde1 for the original; the history keeps what CA passed */
			seq_n = 0;
			((f_bt)th.disp[5])(th.rctx, 0xde1, 25);
			CHECK(seq_n == 1 && count_seq("_mesa_BindTexture") == 1 && args[0] == 0xde1 && args[1] == 25);
			seq_n = 0;
			((f_bt)th.disp[5])(th.rctx, 0x84f5, 26);
			CHECK(seq_n == 1 && count_seq("_mesa_BindTexture") == 1 && args[0] == 0xde1 && args[1] == 26);
			CHECK(sweet_th_line(26, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 26 events 2: #2 ctx 1 genned | #4 ctx 1 bind 0x84f5"));
			seq_n = 0;
			((f_bt)th.disp[5])(th.rctx, 0xde1, 0);   /* unbind: passed through, no event */
			CHECK(seq_n == 1 && args[1] == 0);
			/* TexImage2D: queries TEXTURE_BINDING_2D (no glGetError), the original gets all 9 arguments once */
			memset(ti_args, 0, sizeof ti_args); seq_n = 0;
			((f_ti)th.disp[301])(th.rctx, 0xde1, 2, 0x1908, 97, 76, 0, 0x1908, 0x1401, names);
			CHECK(count_seq("_mesa_TexImage2D") == 1 && count_seq("_mesa_GetError") == 0 && seq_n == 2 && !strcmp(seq_name[0], "_mesa_GetIntegerv") && seq_a0[0] == 0x8069 && !strcmp(seq_name[1], "_mesa_TexImage2D"));
			CHECK(ti_args[0] == 0xde1 && ti_args[1] == 2 && ti_args[2] == 0x1908 && ti_args[3] == 97 && ti_args[4] == 76 && ti_args[5] == 0 && ti_args[6] == 0x1908 && ti_args[7] == 0x1401 && ti_args[8] == (unsigned long)(uintptr_t)names);
			((f_ti)th.disp[301])(th.rctx, 0xde1, 0, 0x1908, 97, 76, 0, 0x1908, 0x1401, NULL);
			/* cube-map face: cannot be named without another binding query: pass through, counted as unattributed */
			seq_n = 0;
			((f_ti)th.disp[301])(th.rctx, 0x8515, 0, 0x1908, 8, 8, 0, 0x1908, 0x1401, NULL);
			CHECK(seq_n == 1 && count_seq("_mesa_TexImage2D") == 1 && ti_args[0] == 0x8515);
			/* TexStorage2D */
			seq_n = 0;
			((f_ts)th.disp[1031])(th.rctx, 0xde1, 1, 0x8058, 97, 76);
			CHECK(count_seq("_mesa_TexStorage2D") == 1 && count_seq("_mesa_GetError") == 0 && seq_n == 2 && args[0] == 0xde1 && args[1] == 1 && args[2] == 0x8058 && args[3] == 97 && args[4] == 76);
			/* CopyTexImage2D (8 arguments) */
			memset(ci_args, 0, sizeof ci_args); seq_n = 0;
			((f_ci)th.disp[54])(th.rctx, 0xde1, 0, 0x1908, 11, 12, 97, 76, 0);
			CHECK(count_seq("_mesa_CopyTexImage2D") == 1 && count_seq("_mesa_GetError") == 0 && seq_n == 3 && seq_a0[0] == 0x8069 && seq_a0[1] == 0x0BA2);   /* + the census-extension VIEWPORT query */
			CHECK(ci_args[0] == 0xde1 && ci_args[1] == 0 && ci_args[2] == 0x1908 && ci_args[3] == 11 && ci_args[4] == 12 && ci_args[5] == 97 && ci_args[6] == 76 && ci_args[7] == 0);
			/* FramebufferTexture2D: FRAMEBUFFER_BINDING queried, the original gets the 5 arguments once */
			seq_n = 0;
			((f_fbt)th.disp[677])(th.rctx, 0x8D40, 0x8CE0, 0xde1, 25, 0);
			CHECK(count_seq("_mesa_FramebufferTexture2D") == 1 && count_seq("_mesa_GetError") == 0 && seq_n == 3 && !strcmp(seq_name[0], "_mesa_GetIntegerv") && seq_a0[0] == 0x8CA6 && seq_a0[1] == 0x0BA2);
			CHECK(args[0] == 0x8D40 && args[1] == 0x8CE0 && args[2] == 0xde1 && args[3] == 25 && args[4] == 0);
			seq_n = 0;
			((f_fbt)th.disp[677])(th.rctx, 0x8D40, 0x8CE0, 0xde1, 0, 0);   /* detach: passed through, no event */
			CHECK(seq_n == 2 && seq_a0[0] == 0x0BA2 && args[3] == 0);   /* counted by the census extension: one VIEWPORT query */
			/* attach note from eagl_mesa.c */
			sweet_th_note_attach(25, 77, 97, 76, 0x42475241);
			/* DeleteTextures: names read before the call, original once */
			seq_n = 0;
			((f_gen)th.disp[59])(th.rctx, 1, names);
			CHECK(seq_n == 1 && count_seq("_mesa_DeleteTextures") == 1 && args[0] == 1 && args[1] == (unsigned long)(uintptr_t)names);
			names[0] = 25; ((f_gen)th.disp[98])(th.rctx, 1, names);   /* no injection: the array already holds 25 */
			inj_gen_on = 0;   /* the name is generated again */
			CHECK(sweet_th_line(25, 0, l, sizeof l) && !strcmp(l,
			    "SWEETGL: TEXHIST tex 25 events 10: #1 ctx 1 genned | #3 ctx 1 bind 0xde1 | #5 ctx 1 teximage2d 0xde1 level 2 97x76 ifmt 0x1908 data ptr | #6 ctx 1 teximage2d 0xde1 level 0 97x76 ifmt 0x1908 data null | #7 ctx 1 texstorage2d 0xde1 levels 1 97x76 ifmt 0x8058 | #8 ctx 1 copyteximage2d 0xde1 level 0 97x76 ifmt 0x1908 | #9 ctx 1 fbotex fbo 6 attach 0x8ce0 textarget 0xde1 level 0"));
			CHECK(sweet_th_line(25, 1, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST cont tex 25 events 10: #10 attachimage 77 97x76 fmt 0x42475241 | #11 ctx 1 deleted | #12 ctx 1 genned"));
			CHECK(!sweet_th_line(25, 2, l, sizeof l));
			sweet_th_stats_line(l, sizeof l);
			CHECK(strstr(l, "unattributed 1") != NULL && strstr(l, "evictions 0") != NULL);
		}
		/* review texhist-b3 F1/F2/F5 and the hook edge cases */
		{
			static int cA, cB;
			unsigned nm[3] = { 999, 998, 997 }, big[257];
			unsigned long u0;
			sweet_gli_ensure_current = ens; ens_ret = 0;
			sweet_th_reset();
			inj_on = 1; inj_tex = 25; inj_fbo = 6; inj_readfb = 4;
			/* F2: two contexts, same name: each event carries its context index (first seen = 1) */
			inj_gen_on = 1; inj_gen_base = 50;
			((f_gen)th.disp[98])(&cA, 1, nm);
			inj_gen_on = 0;
			((f_bt)th.disp[5])(&cB, 0xde1, 50);
			CHECK(sweet_th_line(50, 0, l, sizeof l) && !strcmp(l, "SWEETGL: TEXHIST tex 50 events 2: #1 ctx 1 genned | #2 ctx 2 bind 0xde1"));
			CHECK(sweet_th_ctx_index(&cA) == 1 && sweet_th_ctx_index(&cB) == 2);
			/* F1: names[] are read only after the original wrote them (999 was in the array before the call) */
			nm[0] = 999; nm[1] = 998; inj_gen_on = 1; inj_gen_base = 40;
			((f_gen)th.disp[98])(&cA, 2, nm);
			inj_gen_on = 0;
			CHECK(nm[0] == 40 && nm[1] == 41);
			CHECK(sweet_th_line(40, 0, l, sizeof l) && strstr(l, "events 1: #3 ctx 1 genned") != NULL && sweet_th_line(41, 0, l, sizeof l) && strstr(l, "events 1: #4 ctx 1 genned") != NULL);
			CHECK(sweet_th_line(999, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL && sweet_th_line(998, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL);
			/* F1: no current context: the original (the wrapper) is still called, it writes nothing, nothing is recorded from the untouched array */
			nm[0] = 777; ens_ret = -1; ens_n = 0; seq_n = 0; inj_gen_on = 1; inj_gen_base = 500;
			((f_gen)th.disp[98])(&cA, 1, nm);
			CHECK(ens_n == 2 && seq_n == 0 && nm[0] == 777);   /* hook's own check + the wrapper's: the original was reached and bailed out */
			inj_gen_on = 0;
			CHECK(sweet_th_line(777, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL);
			/* no current context: TexImage2D / FramebufferTexture2D make no query, pass through (original wrapper reached: ens_n 2 / 2), TexImage2D is counted unattributed, fbotex says fbo ? */
			u0 = unattr_n(); ens_n = 0; seq_n = 0;
			((f_ti)th.disp[301])(&cA, 0xde1, 0, 0x1908, 5, 5, 0, 0x1908, 0x1401, NULL);
			CHECK(ens_n == 2 && seq_n == 0 && unattr_n() == u0 + 1);
			ens_n = 0; seq_n = 0;
			((f_fbt)th.disp[677])(&cA, 0x8D40, 0x8CE0, 0xde1, 61, 0);
			CHECK(ens_n == 3 && seq_n == 0);   /* history hook + ABGR hook (T-28) + wrapper */
			ens_ret = 0;
			CHECK(sweet_th_line(61, 0, l, sizeof l) && strstr(l, "ctx 1 fbotex fbo ? attach 0x8ce0 textarget 0xde1 level 0") != NULL);
			/* a bound texture of 0 is unattributed: the original runs, no event */
			inj_tex = 0; u0 = unattr_n(); seq_n = 0;
			((f_ti)th.disp[301])(&cA, 0xde1, 0, 0x1908, 5, 5, 0, 0x1908, 0x1401, NULL);
			CHECK(seq_n == 2 && count_seq("_mesa_TexImage2D") == 1 && unattr_n() == u0 + 1);
			inj_tex = 25;
			/* FramebufferTexture2D: READ_FRAMEBUFFER asks READ_FRAMEBUFFER_BINDING (inj_readfb 4, not inj_fbo 6); any other target: no query, fbo ? */
			seq_n = 0;
			((f_fbt)th.disp[677])(&cA, 0x8CA8, 0x8CE0, 0xde1, 60, 0);
			CHECK(seq_n == 3 && seq_a0[0] == 0x8CAA && seq_a0[1] == 0x0BA2 && count_seq("_mesa_FramebufferTexture2D") == 1);
			CHECK(sweet_th_line(60, 0, l, sizeof l) && strstr(l, "ctx 1 fbotex fbo 4 attach 0x8ce0 textarget 0xde1 level 0") != NULL);
			seq_n = 0;
			((f_fbt)th.disp[677])(&cA, 0x8CA9, 0x8CE0, 0xde1, 62, 0);
			CHECK(seq_n == 2 && seq_a0[0] == 0x0BA2 && count_seq("_mesa_FramebufferTexture2D") == 1);
			CHECK(sweet_th_line(62, 0, l, sizeof l) && strstr(l, "fbotex fbo ? attach") != NULL);
			/* F5: bounds. n = 257 is not read (and is counted), NULL names and name 0 are ignored, the original runs once each time */
			for (unsigned i = 0; i < 257; i++) big[i] = 5000 + i;
			inj_gen_on = 1; inj_gen_base = 600; u0 = unattr_n(); seq_n = 0;
			((f_gen)th.disp[98])(&cA, 257, big);
			inj_gen_on = 0;
			CHECK(count_seq("_mesa_GenTextures") == 1 && args[0] == 257 && unattr_n() == u0 + 1);
			CHECK(sweet_th_line(600, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL && sweet_th_line(5000, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL);
			seq_n = 0; ((f_gen)th.disp[98])(&cA, 3, NULL);
			CHECK(count_seq("_mesa_GenTextures") == 1);
			seq_n = 0; ((f_gen)th.disp[59])(&cA, 2, NULL);
			CHECK(count_seq("_mesa_DeleteTextures") == 1);
			seq_n = 0; ((f_gen)th.disp[59])(&cA, 0, nm); ((f_gen)th.disp[59])(&cA, -5, nm);
			CHECK(count_seq("_mesa_DeleteTextures") == 2 && sweet_th_line(40, 0, l, sizeof l) && strstr(l, "events 1:") != NULL);   /* nm[0] = 40 was not deleted */
			nm[0] = 0; nm[1] = 31;
			seq_n = 0; ((f_gen)th.disp[59])(&cA, 2, nm);
			CHECK(count_seq("_mesa_DeleteTextures") == 1 && args[0] == 2);
			CHECK(sweet_th_line(0, 0, l, sizeof l) && strstr(l, "no events recorded") != NULL && sweet_th_line(31, 0, l, sizeof l) && strstr(l, "events 1: #") != NULL && strstr(l, "ctx 1 deleted") != NULL);
			inj_on = 0;
		}
		inj_on = 0;

		/* trigger: the incomplete-FBO log prints the history of the colour texture (only with census on), once per distinct (fbo,status) */
		{
			static struct sweet_gli tg;
			setenv("SWEETGL_FBO", "1", 1); setenv("SWEETGL_CENSUS", "1", 1);
			sweet_gli_init(&tg, &dummy);
			unsetenv("SWEETGL_CENSUS");
			sweet_gli_ensure_current = ens; ens_ret = 0;
			sweet_th_reset();
			memset(&e, 0, sizeof e); e.kind = SWEET_TH_GEN; sweet_th_record(25, e);
			memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.a = 0x8ce0; e.b = 0; e.c = 6; e.e = 0xde1; sweet_th_record(25, e);
			inj_on = 1; inj_fbo = 6; inj_atype = 0x1702; inj_aname = 25; inj_w = 0; inj_h = 0; inj_ifmt = 0x1908; fake_ret = 0x8cd6;
			cap_begin();
			((f_cl)tg.disp[10])(tg.rctx, 0x4000);
			((f_cl)tg.disp[10])(tg.rctx, 0x4000);   /* same (fbo,status): nothing new */
			cap_end(big, sizeof big);
			CHECK(strstr(big, "sweetgl: FBO 6 incomplete in Clear: status 0x8cd6 color0 type 0x1702 name 25 level0 0x0 ifmt 0x1908\n") != NULL);
			CHECK(strstr(big, "SWEETGL: TEXHIST tex 25 events 2: #1 genned | #2 fbotex fbo 6 attach 0x8ce0 textarget 0xde1 level 0\n") != NULL);
			{ unsigned c = 0; for (const char *q = big; (q = strstr(q, "SWEETGL: TEXHIST tex 25 ")); q++) c++; CHECK(c == 1); }
			inj_fbo = 8; inj_aname = 99;   /* a texture with no record */
			cap_begin(); ((f_cl)tg.disp[10])(tg.rctx, 0x4000); cap_end(big, sizeof big);
			CHECK(strstr(big, "SWEETGL: TEXHIST tex 99 no events recorded (table cap or created before the hook)\n") != NULL);
			inj_fbo = 9; inj_atype = 0x8D41; inj_aname = 25;   /* a renderbuffer: no texture history */
			cap_begin(); ((f_cl)tg.disp[10])(tg.rctx, 0x4000); cap_end(big, sizeof big);
			CHECK(strstr(big, "sweetgl: FBO 9 incomplete in Clear") != NULL && strstr(big, "TEXHIST") == NULL);
			/* census off: the FBO log is unchanged and has no history line */
			sweet_gli_init(&tg, &dummy);
			sweet_gli_ensure_current = ens; ens_ret = 0;
			inj_fbo = 10; inj_atype = 0x1702; inj_aname = 25;
			cap_begin(); ((f_cl)tg.disp[10])(tg.rctx, 0x4000); cap_end(big, sizeof big);
			CHECK(strstr(big, "sweetgl: FBO 10 incomplete in Clear") != NULL && strstr(big, "TEXHIST") == NULL);
			inj_on = 0; fake_ret = 0;
			unsetenv("SWEETGL_FBO");
		}
		setenv("SWEETGL_RECT", "0", 1);
	}

	{   /* census extension (log-only): per-class counters of other calls, pixel readback stats, draw sequences */
		static struct sweet_gli cx, cxoff;
		static char big[65536];
		typedef void (*f_dr)(void *, unsigned, int, int);
		typedef void (*f_ss)(void *, unsigned, int, const char *const *, const int *);
		typedef void (*f_as)(void *, unsigned, unsigned);
		typedef unsigned (*f_cprog)(void *);
		typedef void (*f_blit)(void *, int, int, int, int, int, int, int, int, unsigned, unsigned);
		typedef void (*f_rp)(void *, int, int, int, int, unsigned, unsigned, void *);
		typedef void (*f_gm)(void *, unsigned);
		typedef void (*f_fbt2)(void *, unsigned, unsigned, unsigned, unsigned, int);
		typedef void (*f_ci2)(void *, unsigned, int, unsigned, int, int, int, int, int);
		char l[1024];
		unsigned long z[5][3], y[5][3];
		unsigned dq, k;

		/* ---- pure formatters ---- */
		memset(z, 0, sizeof z);
		CHECK(sweet_census_extra(l, sizeof l, z, 0) > 0 && !strcmp(l, " blit S/T/O 0/0/0 readpix S/T/O 0/0/0 genmip S/T/O 0/0/0 fbotex S/T/O 0/0/0 drawseq 0 copyteximg S/T/O 0/0/0"));
		z[0][0] = 1; z[1][1] = 22; z[2][2] = 333; z[3][0] = 4; z[3][2] = 5; z[4][1] = 6;
		sweet_census_extra(l, sizeof l, z, 3);
		CHECK(!strcmp(l, " blit S/T/O 1/0/0 readpix S/T/O 0/22/0 genmip S/T/O 0/0/333 fbotex S/T/O 4/0/5 drawseq 3 copyteximg S/T/O 0/6/0"));
		{   /* the full line starts with the old line byte for byte (old regexes still match) and ends with the extra */
			unsigned long c[6][3]; char old[512], full[768];
			memset(c, 0, sizeof c); c[0][1] = 9;
			sweet_census_line(old, sizeof old, 40, c, 3);
			sweet_census_line_full(full, sizeof full, 40, c, 3, z, 2);
			CHECK(strncmp(full, old, strlen(old)) == 0 && !strcmp(full + strlen(old), " blit S/T/O 1/0/0 readpix S/T/O 0/22/0 genmip S/T/O 0/0/333 fbotex S/T/O 4/0/5 drawseq 2 copyteximg S/T/O 0/6/0"));
			CHECK(strstr(old, "copytex 3") && old[strlen(old) - 1] == '3');
		}
		sweet_census_count2(0, 1000, 5); sweet_census_count2(0, 256, 256); sweet_census_count2(0, 257, 256); sweet_census_count2(9, 1, 1); sweet_census_count2(-1, 1, 1);
		sweet_census_snapshot2(y, &dq);
		sweet_census_snapshot2(z, &dq);
		CHECK(z[0][0] >= 1 && z[0][1] >= 1 && z[0][2] >= 1);
		{   /* boundaries use sweet_census_class: 1000 -> S, 256x256 -> T, 257x256 -> O; out-of-range `which` counts nothing */
			unsigned long t0[5][3], t1[5][3]; unsigned d0;
			sweet_census_snapshot2(t0, &d0);
			sweet_census_count2(1, 999, 5); sweet_census_count2(2, 1000, 5); sweet_census_count2(3, 0, 0); sweet_census_count2(4, 97, 76); sweet_census_count2(5, 1, 1);
			sweet_census_snapshot2(t1, &d0);
			CHECK(t1[1][2] - t0[1][2] == 1 && t1[2][0] - t0[2][0] == 1 && t1[3][1] - t0[3][1] == 1 && t1[4][1] - t0[4][1] == 1);
			{ int same = 1; for (int a = 0; a < 5; a++) for (int b = 0; b < 3; b++) if (t1[a][b] - t0[a][b] > 1) same = 0; CHECK(same); }
		}
		/* pixel stats */
		{
			unsigned char b[64]; unsigned nz, ma, sm;
			memset(b, 0, sizeof b);
			sweet_px_stats(b, &nz, &ma, &sm); CHECK(nz == 0 && ma == 0 && sm == 0);
			b[0] = 1; b[1] = 2; b[2] = 3; b[3] = 4;     /* pixel 0 */
			b[11] = 0xff;                                /* pixel 2, alpha only */
			b[60] = 7;                                   /* pixel 15, red only: counts as nonzero, not as alpha */
			sweet_px_stats(b, &nz, &ma, &sm); CHECK(nz == 3 && ma == 0xff && sm == 1 + 2 + 3 + 4 + 255 + 7);
			sweet_px_line(l, sizeof l, "blur", 1, 5, 97, 76, b);
			CHECK(!strcmp(l, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 nonzero 3 of 16 maxa 0xff sum 0x110 written 1"));
			memset(b, 0xff, sizeof b);
			sweet_px_line(l, sizeof l, "fbfetch", 0, 0, 1125, 2436, b);
			CHECK(!strcmp(l, "SWEETGL: PIXELS fbfetch S fbo 0 vp 1125x2436 nonzero 16 of 16 maxa 0xff sum 0x3fc0 written 1"));
			sweet_px_line(l, sizeof l, "blur", 2, -1, 300, 300, b);
			CHECK(strstr(l, "PIXELS blur O fbo -1 vp 300x300") != NULL);
			/* review census-b4 F3: the 0xA5 sentinel: all 64 bytes untouched = the read wrote nothing */
			memset(b, 0, sizeof b); sweet_px_line(l, sizeof l, "blur", 1, 5, 97, 76, b);
			CHECK(!strcmp(l, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 nonzero 0 of 16 maxa 0x0 sum 0x0 written 1"));   /* real black */
			memset(b, 0xA5, sizeof b); sweet_px_line(l, sizeof l, "blur", 1, 5, 97, 76, b);
			CHECK(!strcmp(l, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 written 0") && !sweet_px_written(b));
			b[63] = 0xA4; CHECK(sweet_px_written(b)); b[63] = 0xA5; b[0] = 0; CHECK(sweet_px_written(b));
		}
		/* sampling budget */
		sweet_px_reset();
		sweet_px_stats_line(l, sizeof l);
		CHECK(!strcmp(l, "SWEETGL: PIXELS stats samples 0 skipped 0 reasons pbo=0 fbo=0 rb=0 align=0 small=0"));
		for (k = 0; k < 4; k++) CHECK(sweet_px_claim(0, 5, 0) == 1);
		CHECK(sweet_px_claim(0, 5, 0) == 0 && sweet_px_claim(0, 6, 0) == 1);   /* 4 per distinct FBO name */
		for (k = 0; k < 3; k++) sweet_px_claim(0, 6, 0);
		for (int f = 7; f <= 8; f++) for (k = 0; k < 4; k++) CHECK(sweet_px_claim(0, f, 0) == 1);   /* 16 so far */
		CHECK(sweet_px_claim(0, 9, 0) == 0 && sweet_px_claim(0, 5, 0) == 0);                      /* 16 total */
		CHECK(sweet_px_claim(1, 0, 100) == 1 && sweet_px_claim(1, 0, 104) == 0 && sweet_px_claim(1, 0, 105) == 1 && sweet_px_claim(1, 0, 109) == 0 && sweet_px_claim(1, 0, 110) == 1 && sweet_px_claim(1, 0, 115) == 1);
		CHECK(sweet_px_claim(1, 0, 500) == 0);                                                       /* fbfetch cap 4, blur budget untouched by it */
		/* kind letters */
		CHECK(sweet_census_kind_char(1) == 'b' && sweet_census_kind_char(2) == 'f' && sweet_census_kind_char(4) == 'c' && sweet_census_kind_char(8) == 'l' && sweet_census_kind_char(16) == 'r' && sweet_census_kind_char(0) == 'o' && sweet_census_kind_char(3) == 'b' && sweet_census_kind_char(24) == 'l');
		/* draw sequence: thresholds, once each, format */
		sweet_dseq_reset();
		{
			struct sweet_dseq_run done, run2;
			unsigned pos = 0, tot = 0;
			memset(&done, 0, sizeof done);
			CHECK(!sweet_dseq_arm(99) && !sweet_dseq_active() && sweet_dseq_arm(100) && sweet_dseq_active() && !sweet_dseq_arm(100000));   /* active: no second arm */
			for (k = 0; k < 119; k++) CHECK(!sweet_dseq_record(k % 2 ? 'f' : 'b', k % 3 ? 'S' : 'T', k % 5 ? 7 : -1, &done));
			CHECK(sweet_dseq_record('r', 'O', 123456, &done) && !sweet_dseq_active() && done.n == 120 && done.seq == 1);
			CHECK(!sweet_dseq_record('b', 'T', 1, &run2));   /* not recording any more */
			CHECK(!sweet_dseq_arm(999) && sweet_dseq_arm(1000));   /* second threshold */
			for (k = 0; k < 120; k++) sweet_dseq_record('b', 'T', 3, &run2);
			CHECK(!sweet_dseq_arm(2999) && sweet_dseq_arm(3000) && sweet_dseq_active());
			for (k = 0; k < 120; k++) sweet_dseq_record('b', 'T', 3, &run2);
			CHECK(!sweet_dseq_arm(1000000) && !sweet_dseq_active());   /* 3 sequences max */
			sweet_census_snapshot2(y, &dq); CHECK(dq == 3);
			/* format: lines <= 400 chars, counts add up to 120, first/last entries exact */
			CHECK(sweet_dseq_format(l, sizeof l, &done, &pos) > 0 && strncmp(l, "SWEETGL: DRAWSEQ #1 n ", 22) == 0 && strlen(l) <= 400 && strstr(l, ": b/T/? ") != NULL);
			{
				unsigned lines = 1, nn = (unsigned)atoi(l + 22), last_n = 0;
				char last[1024] = "";
				tot = nn;
				strcpy(last, l);
				while (sweet_dseq_format(l, sizeof l, &done, &pos)) { CHECK(strlen(l) <= 400); nn = (unsigned)atoi(l + 22); tot += nn; last_n = nn; strcpy(last, l); lines++; }
				CHECK(tot == 120 && lines >= 2 && pos == 120 && last_n > 0);
				CHECK(strlen(last) > 10 && !strcmp(last + strlen(last) - 11, " r/O/123456"));
			}
			CHECK(!sweet_dseq_format(l, sizeof l, &done, &pos));
		}
		sweet_dseq_reset();
		sweet_census_snapshot2(y, &dq); CHECK(dq == 0);

		/* ---- census OFF: the new slots are exactly the generated entries ---- */
		sweet_gli_init(&cxoff, &dummy);
		CHECK(cxoff.disp[237] == sweet_gli_defaults[237] && cxoff.disp[755] == sweet_gli_defaults[755] && cxoff.disp[681] == sweet_gli_defaults[681]);

		/* ---- census ON: hooks ---- */
		setenv("SWEETGL_CENSUS", "1", 1);
		sweet_gli_init(&cx, &dummy);
		unsetenv("SWEETGL_CENSUS");
		sweet_gli_ensure_current = ens; ens_ret = 0;
		CHECK(cx.disp[237] != sweet_gli_defaults[237] && cx.disp[755] != sweet_gli_defaults[755] && cx.disp[681] != sweet_gli_defaults[681]);
		inj_on = 1; inj_prog = 0; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 97; inj_vp[3] = 76; inj_fbo = 5; inj_readfb = 5;
		{   /* per-class counters, exact call-through (arguments, once), query sequence */
			static const int vps[3][2] = { { 1125, 2436 }, { 97, 76 }, { 400, 400 } };
			for (int cls = 0; cls < 3; cls++) {
				inj_vp[2] = vps[cls][0]; inj_vp[3] = vps[cls][1];
				memset(bl_args, 0, sizeof bl_args); sweet_census_snapshot2(z, &dq); seq_n = 0;
				((f_blit)cx.disp[755])(cx.rctx, 1, 2, 3, 4, 5, 6, 7, 8, 0x4100, 0x2601);
				sweet_census_snapshot2(y, &dq);
				CHECK(y[0][cls] - z[0][cls] == 1 && y[1][cls] == z[1][cls] && y[2][cls] == z[2][cls] && y[3][cls] == z[3][cls] && y[4][cls] == z[4][cls]);
				CHECK(seq_n == 2 && seq_a0[0] == 0x0BA2 && !strcmp(seq_name[1], "_mesa_BlitFramebuffer") && count_seq("_mesa_BlitFramebuffer") == 1 && count_seq("_mesa_GetError") == 0);
				CHECK(bl_args[0] == 1 && bl_args[1] == 2 && bl_args[2] == 3 && bl_args[3] == 4 && bl_args[4] == 5 && bl_args[5] == 6 && bl_args[6] == 7 && bl_args[7] == 8 && bl_args[8] == 0x4100 && bl_args[9] == 0x2601);
				memset(rp_args, 0, sizeof rp_args); rp_calls = 0; seq_n = 0; sweet_census_snapshot2(z, &dq);
				((f_rp)cx.disp[237])(cx.rctx, 11, 12, 13, 14, 0x1908, 0x1401, big);
				sweet_census_snapshot2(y, &dq);
				CHECK(y[1][cls] - z[1][cls] == 1 && y[0][cls] == z[0][cls] && y[2][cls] == z[2][cls] && y[3][cls] == z[3][cls] && y[4][cls] == z[4][cls]);
				CHECK(seq_n == 2 && seq_a0[0] == 0x0BA2 && rp_calls == 1 && count_seq("_mesa_ReadPixels") == 1 && count_seq("_mesa_GetError") == 0);
				CHECK(rp_args[0] == 11 && rp_args[1] == 12 && rp_args[2] == 13 && rp_args[3] == 14 && rp_args[4] == 0x1908 && rp_args[5] == 0x1401 && rp_args[6] == (unsigned long)(uintptr_t)big);
				seq_n = 0; sweet_census_snapshot2(z, &dq);
				((f_gm)cx.disp[681])(cx.rctx, 0xde1);
				sweet_census_snapshot2(y, &dq);
				CHECK(y[2][cls] - z[2][cls] == 1 && y[0][cls] == z[0][cls] && y[1][cls] == z[1][cls] && y[3][cls] == z[3][cls] && y[4][cls] == z[4][cls]);
				CHECK(seq_n == 2 && seq_a0[0] == 0x0BA2 && count_seq("_mesa_GenerateMipmap") == 1 && args[0] == 0xde1 && count_seq("_mesa_GetError") == 0);
				seq_n = 0; sweet_census_snapshot2(z, &dq);   /* the texhist hooks carry the fbotex / copyteximg counters (no second hook) */
				((f_fbt2)cx.disp[677])(cx.rctx, 0x8D40, 0x8CE0, 0xde1, 0, 0);
				sweet_census_snapshot2(y, &dq);
				CHECK(y[3][cls] - z[3][cls] == 1 && y[0][cls] == z[0][cls] && y[1][cls] == z[1][cls] && y[2][cls] == z[2][cls] && y[4][cls] == z[4][cls]);
				CHECK(seq_n == 2 && count_seq("_mesa_FramebufferTexture2D") == 1);
				seq_n = 0; sweet_census_snapshot2(z, &dq);
				inj_tex = 25;
				((f_ci2)cx.disp[54])(cx.rctx, 0xde1, 0, 0x1908, 1, 2, 3, 4, 0);
				sweet_census_snapshot2(y, &dq);
				CHECK(y[4][cls] - z[4][cls] == 1 && y[3][cls] == z[3][cls] && y[0][cls] == z[0][cls]);
				CHECK(count_seq("_mesa_CopyTexImage2D") == 1 && count_seq("_mesa_GetError") == 0);
			}
		}
		{   /* no current context: no query, no count, the original (the wrapper) is still reached once */
			sweet_census_snapshot2(z, &dq); ens_ret = -1; seq_n = 0;
			((f_blit)cx.disp[755])(cx.rctx, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
			((f_rp)cx.disp[237])(cx.rctx, 0, 0, 1, 1, 0x1908, 0x1401, big);
			((f_gm)cx.disp[681])(cx.rctx, 0xde1);
			ens_ret = 0;
			sweet_census_snapshot2(y, &dq);
			CHECK(seq_n == 0 && !memcmp(y, z, sizeof y));
		}

		/* ---- pixel readback ---- */
		{
			static const char *const bsrc[1] = { "uniform lowp vec4 weight[2];" };
			static const char *const fsrc[1] = { "lowp vec4 d0 = gl_LastFragData[0];" };
			static const char *const osrc[1] = { "void main() {}" };
			int ok_seq;
			unsigned exp_q[] = { 0x8B8D, 0x0BA2, 0x8CA6 };
			(void)osrc;
			fake_ret = 4; ((f_cprog)cx.disp[597])(cx.rctx); fake_ret = 0;
			((f_ss)cx.disp[595])(cx.rctx, 90, 1, bsrc, NULL); ((f_as)cx.disp[598])(cx.rctx, 4, 90);
			fake_ret = 6; ((f_cprog)cx.disp[597])(cx.rctx); fake_ret = 0;
			((f_ss)cx.disp[595])(cx.rctx, 91, 1, fsrc, NULL); ((f_as)cx.disp[598])(cx.rctx, 6, 91);
			rp_fill = 1;
			for (k = 0; k < 64; k++) rp_pat[k] = 0;
			rp_pat[0] = 1; rp_pat[1] = 2; rp_pat[2] = 3; rp_pat[3] = 4; rp_pat[11] = 0xff; rp_pat[60] = 7;
			inj_prog = 4; inj_vp[2] = 97; inj_vp[3] = 76;
			inj_fbo = 5; inj_readfb = 5; inj_pbo = 0; inj_sb = 0; inj_rbuf = 0x8CE0; inj_crf = 0x1908; inj_crt = 0x1401; inj_al = 4; inj_rl = 0; inj_sp = 0; inj_sr = 0; inj_atype = 0x1702;
			inj_vp[0] = 10; inj_vp[1] = 20;   /* review census-b4 F2: a non-zero origin must be added to the centre */
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);   /* prime: the debug sink install of this context */
			sweet_px_reset(); sweet_dseq_reset();
			fake_ret = 0x8cd5;
			/* success: exact order of queries, ReadPixels once after the draw at the viewport centre */
			rp_calls = 0; seq_n = 0; hits = 0;
			cap_begin(); ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); cap_end(big, sizeof big);
			{
				static const unsigned post_q[] = { 0x88ED, 0x8CAA, 0x8CA8, 0x80A8, 0x0C02, 0x8CA8, 0x8B9B, 0x8B9A, 0x0D05, 0x0D02, 0x0D04, 0x0D03 };   /* 6th: GetFramebufferAttachmentParameteriv(READ_FRAMEBUFFER, ...) */
				ok_seq = seq_n == 3 + 1 + 12 + 1;
				for (int q = 0; q < 3 && ok_seq; q++) ok_seq = !strcmp(seq_name[q], "_mesa_GetIntegerv") && seq_a0[q] == exp_q[q];
				ok_seq = ok_seq && !strcmp(seq_name[3], "_mesa_DrawArrays");
				for (int q = 0; q < 12 && ok_seq; q++) ok_seq = seq_a0[4 + q] == post_q[q] && !strcmp(seq_name[4 + q], q == 2 ? "_mesa_CheckFramebufferStatus" : q == 5 ? "_mesa_GetFramebufferAttachmentParameteriv" : "_mesa_GetIntegerv");
				ok_seq = ok_seq && !strcmp(seq_name[16], "_mesa_ReadPixels") && args[2] != 0;
				CHECK(ok_seq);
			}
			CHECK(rp_calls == 1 && count_seq("_mesa_ReadPixels") == 1 && count_seq("_mesa_GetError") == 0);
			CHECK(rp_args[0] == 56 && rp_args[1] == 56 && rp_args[2] == 4 && rp_args[3] == 4 && rp_args[4] == 0x1908 && rp_args[5] == 0x1401 && rp_args[6] != 0);
			CHECK(!strcmp(big, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 nonzero 3 of 16 maxa 0xff sum 0x110 written 1\n"));
			sweet_px_stats_line(l, sizeof l);
			CHECK(!strcmp(l, "SWEETGL: PIXELS stats samples 1 skipped 0 reasons pbo=0 fbo=0 rb=0 align=0 small=0"));
			/* the ORIGINAL draw ran first: nothing the readback queried comes before it */
			/* per-FBO cap 4 and total cap 16 through the hook (the 5th draw on FBO 5 only queries the FBO: 3 + draw) */
			for (k = 0; k < 3; k++) ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			rp_calls = 0; seq_n = 0;
			cap_begin(); ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); cap_end(big, sizeof big);
			CHECK(rp_calls == 0 && seq_n == 4 && big[0] == 0);
			for (int f = 6; f <= 8; f++) { inj_fbo = f; inj_readfb = f; for (k = 0; k < 5; k++) ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); }
			sweet_px_stats_line(l, sizeof l);
			CHECK(!strcmp(l, "SWEETGL: PIXELS stats samples 16 skipped 0 reasons pbo=0 fbo=0 rb=0 align=0 small=0"));
			inj_fbo = 9; inj_readfb = 9; rp_calls = 0;
			for (k = 0; k < 5; k++) ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			CHECK(rp_calls == 0);   /* 16 total */

			/* every safety condition skips the read (ReadPixels never called), is counted under its reason and logs no PIXELS line */
			{
				static const struct { const char *what; int reason; } cases[] = {
				    { "pbo bound", 0 }, { "read fbo differs", 1 }, { "fbo incomplete", 1 }, { "multisampled", 1 }, { "read fbo unknown", 1 },
				    { "read buffer wrong for fbo", 2 }, { "color read format", 2 }, { "color read type", 2 },
				    { "align 16", 3 }, { "align 0", 3 }, { "row length", 3 }, { "skip pixels", 3 }, { "skip rows", 3 }, { "small viewport", 4 }, { "attachment 0 empty", 2 }, { "viewport 4x3", 4 }, { "viewport 3x4 (height ok)", 4 }, { "attachment type -1", 2 } };
				for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
					unsigned long want[5] = { 0, 0, 0, 0, 0 }, got[5];
					char exp[200];
					sweet_px_reset();
					inj_prog = 4; inj_vp[2] = 97; inj_vp[3] = 76; inj_fbo = 5; inj_readfb = 5; inj_pbo = 0; inj_sb = 0; inj_rbuf = 0x8CE0; inj_crf = 0x1908; inj_crt = 0x1401; inj_al = 4; inj_rl = 0; inj_sp = 0; inj_sr = 0; inj_atype = 0x1702; fake_ret = 0x8cd5;
					inj_vp[0] = 0; inj_vp[1] = 0;
					switch (c) {
					case 0: inj_pbo = 3; break;
					case 1: inj_readfb = 4; break;
					case 2: fake_ret = 0x8cd6; break;
					case 3: inj_sb = 1; break;
					case 4: inj_fbo = -1; inj_readfb = -1; break;
					case 5: inj_rbuf = 0x0405; break;
					case 6: inj_crf = 0x1907; break;
					case 7: inj_crt = 0x8363; break;
					case 8: inj_al = 16; break;
					case 9: inj_al = 0; break;
					case 10: inj_rl = 8; break;
					case 11: inj_sp = 1; break;
					case 12: inj_sr = 1; break;
					case 14: inj_atype = 0; break;
					case 15: inj_vp[2] = 4; inj_vp[3] = 3; break;
					case 16: inj_vp[2] = 3; inj_vp[3] = 4; break;
					case 17: inj_atype = -1; break;
					default: inj_vp[2] = 3; break;
					}
					rp_calls = 0; seq_n = 0;
					cap_begin(); ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); cap_end(big, sizeof big);
					CHECK(rp_calls == 0 && count_seq("_mesa_ReadPixels") == 0 && big[0] == 0);
					if (c == 14) { unsigned f8 = 0; for (unsigned q = 0; q < seq_n && q < 64; q++) if (seq_a0[q] == 0x8B9B || seq_a0[q] == 0x8B9A) f8++; CHECK(f8 == 0 && count_seq("_mesa_GetFramebufferAttachmentParameteriv") == 1); }   /* F1: no read-format query on an empty attachment */
					want[cases[c].reason] = 1;
					snprintf(exp, sizeof exp, "SWEETGL: PIXELS stats samples 0 skipped 1 reasons pbo=%lu fbo=%lu rb=%lu align=%lu small=%lu", want[0], want[1], want[2], want[3], want[4]);
					sweet_px_stats_line(l, sizeof l);
					if (strcmp(l, exp)) printf("case %u (%s): got '%s'\n", c, cases[c].what, l);
					CHECK(!strcmp(l, exp));
					(void)got;
				}
			}
			/* review census-b4 F2/F1: exact boundaries that must READ: viewport 4x4 at origin (3,5) reads at (3,5); PACK_ALIGNMENT 8 reads, 16 skips (cases above); default FB makes no attachment query */
			inj_atype = 0x1702; inj_prog = 4; inj_fbo = 5; inj_readfb = 5; inj_rbuf = 0x8CE0; inj_pbo = 0; fake_ret = 0x8cd5;
			sweet_px_reset(); rp_calls = 0; inj_vp[0] = 3; inj_vp[1] = 5; inj_vp[2] = 4; inj_vp[3] = 4;
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			CHECK(rp_calls == 1 && rp_args[0] == 3 && rp_args[1] == 5);
			sweet_px_reset(); rp_calls = 0; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 97; inj_vp[3] = 76; inj_al = 8;
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			CHECK(rp_calls == 1);
			inj_al = 4;
			/* F3: the read happened but wrote nothing (error / clipped patch): logged as `written 0`, still counted as a sample */
			sweet_px_reset(); rp_calls = 0; rp_nowrite = 1;
			cap_begin(); ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); cap_end(big, sizeof big);
			rp_nowrite = 0;
			CHECK(rp_calls == 1 && !strcmp(big, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 written 0\n"));
			sweet_px_stats_line(l, sizeof l);
			CHECK(strstr(l, "samples 1 skipped 0") != NULL);
			/* the query after a failing one is not made (no wasted GL calls): pbo bound = pre 3 + draw + 1 query */
			sweet_px_reset(); inj_vp[2] = 97; inj_pbo = 3; seq_n = 0; fake_ret = 0x8cd5;
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			CHECK(seq_n == 5 && seq_a0[4] == 0x88ED);
			inj_pbo = 0;
			/* default framebuffer: no status query, READ_BUFFER BACK or FRONT accepted, an FBO-style 0x8CE0 is not */
			inj_pbo = 0; inj_fbo = 0; inj_readfb = 0; inj_rbuf = 0x0405; inj_atype = 0; sweet_px_reset(); rp_calls = 0; seq_n = 0;
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			CHECK(rp_calls == 1 && count_seq("_mesa_CheckFramebufferStatus") == 0 && count_seq("_mesa_GetFramebufferAttachmentParameteriv") == 0);   /* default FB: attachment type 0 is irrelevant */
			inj_rbuf = 0x0404; rp_calls = 0; ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); CHECK(rp_calls == 1);
			inj_rbuf = 0x8CE0; rp_calls = 0; ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); CHECK(rp_calls == 0);
			/* FBFETCH program in an S viewport: one read at the centre, then >= 5 s spacing (real clock: the 2nd..3rd draws are inside the gap), cap 4 */
			inj_prog = 6; inj_vp[0] = 3; inj_vp[1] = 7; inj_vp[2] = 1125; inj_vp[3] = 2436; inj_atype = 0x1702; inj_fbo = 5; inj_readfb = 5; inj_rbuf = 0x8CE0; fake_ret = 0x8cd5;
			sweet_px_reset(); rp_calls = 0;
			cap_begin(); for (k = 0; k < 3; k++) ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3); cap_end(big, sizeof big);
			CHECK(rp_calls == 1 && rp_args[0] == 563 && rp_args[1] == 1223);
			CHECK(!strcmp(big, "SWEETGL: PIXELS fbfetch S fbo 5 vp 1125x2436 nonzero 3 of 16 maxa 0xff sum 0x110 written 1\n"));
			/* a fbfetch program in a T viewport and a blur program in an S viewport sample nothing */
			sweet_px_reset(); rp_calls = 0; inj_vp[2] = 97; inj_vp[3] = 76;
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			inj_prog = 4; inj_vp[2] = 1125; inj_vp[3] = 2436;
			((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			CHECK(rp_calls == 0);
			inj_prog = 0; fake_ret = 0;
		}

		/* ---- draw sequences through the hooks ---- */
		{
			unsigned long c[6][3]; unsigned long cp;
			static char exp[4096], got[4096];
			static const unsigned long marks[3] = { 100, 1000, 3000 };
			inj_pbo = 3;   /* readbacks skip fast: not under test here */
			sweet_px_reset(); sweet_dseq_reset();
			for (int m = 0; m < 3; m++) {
				char note[80];
				sweet_census_snapshot(c, &cp);
				CHECK(c[0][1] < marks[m]);
				while (c[0][1] < marks[m] - 1) { sweet_census_count(1u, 97, 76); c[0][1]++; }   /* advance the counter only */
				exp[0] = 0;
				cap_begin();
				inj_prog = 4; inj_vp[2] = 97; inj_vp[3] = 76; inj_fbo = 3;
				((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);   /* this draw reaches the mark: arms, is not recorded */
				for (k = 0; k < 120; k++) {
					int pr = (k % 3) == 0 ? 4 : 0, bigvp = (k % 3) == 2;
					char e[24];
					inj_prog = pr; inj_vp[2] = bigvp ? 1125 : 97; inj_vp[3] = bigvp ? 2436 : 76; inj_fbo = (int)(k % 7);
					seq_n = 0;
					((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
					if (k == 4) CHECK(seq_n == 4 && seq_a0[2] == 0x8CA6 && !strcmp(seq_name[3], "_mesa_DrawArrays"));   /* recorded, not sampled: one extra FBO query */
					snprintf(e, sizeof e, " %c/%c/%d", pr ? 'b' : 'o', bigvp ? 'S' : 'T', (int)(k % 7)); strcat(exp, e);
				}
				cap_end(got, sizeof got);
				snprintf(note, sizeof note, "SWEETGL: DRAWSEQ armed at %lu draws\n", marks[m]);
				CHECK(strncmp(got, note, strlen(note)) == 0);
				{   /* the lines after the note: DRAWSEQ #m+1, each <= 400 chars, entries concatenated == expected, total n == 120 */
					char pfx[40], cat[4096] = "";
					unsigned tot = 0, lines = 0;
					const char *q = got + strlen(note);
					snprintf(pfx, sizeof pfx, "SWEETGL: DRAWSEQ #%d n ", m + 1);
					while (*q) {
						const char *nl = strchr(q, '\n'), *colon;
						size_t len = nl ? (size_t)(nl - q) : strlen(q);
						CHECK(len <= 400 && strncmp(q, pfx, strlen(pfx)) == 0);
						tot += (unsigned)atoi(q + strlen(pfx));
						colon = memchr(q + strlen(pfx), ':', len - strlen(pfx));
						if (colon && strlen(cat) + len < sizeof cat) strncat(cat, colon + 1, len - (size_t)(colon + 1 - q));
						lines++;
						q += len + (nl ? 1 : 0);
					}
					CHECK(tot == 120 && lines >= 2 && !strcmp(cat, exp));
				}
				CHECK(!sweet_dseq_active());
			}
			/* once each: further blur T draws arm nothing and print nothing; the census line says 3 */
			cap_begin(); inj_prog = 4; inj_vp[2] = 97; inj_vp[3] = 76; inj_fbo = 3;
			for (k = 0; k < 200; k++) ((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
			cap_end(got, sizeof got);
			CHECK(got[0] == 0 && !sweet_dseq_active());
			sweet_census_snapshot2(y, &dq); CHECK(dq == 3);
		}
		/* ---- time-armed rounds + five-point patches (SWEETGL_CENSUS=<N>, N >= 2) ---- */
		{
			static struct sweet_gli cy;
			static char ob[65536];
			char lines[8][400];
			long arm = -1;
			unsigned long on;
			struct sweet_px_rinfo ri;
			const char *pp;
			int nl;
#define OB_COUNT(s_) ({ int c_ = 0; const char *p_ = ob; while ((p_ = strstr(p_, s_)) != NULL) { c_++; p_++; } c_; })
#define DRAW_B(g_, fbo_, t_) do { mock_t = (t_); inj_prog = 4; inj_vp[0] = 10; inj_vp[1] = 20; inj_vp[2] = 97; inj_vp[3] = 76; inj_fbo = (fbo_); inj_readfb = (fbo_); ((f_dr)(g_).disp[65])((g_).rctx, 4, 0, 3); } while (0)
#define DRAW_F(g_, fbo_, t_) do { mock_t = (t_); inj_prog = 6; inj_vp[0] = 10; inj_vp[1] = 20; inj_vp[2] = 1125; inj_vp[3] = 2436; inj_fbo = (fbo_); inj_readfb = (fbo_); ((f_dr)(g_).disp[65])((g_).rctx, 4, 0, 3); } while (0)
			(void)lines; (void)on;
			sweet_tu_reset(); sweet_ub_reset(); sweet_th_reset();
			inj_blend = 1; inj_bf[0] = 0x302; inj_bf[1] = 0x303; inj_bf[2] = 1; inj_bf[3] = 0x304; inj_eq[0] = 0x8006; inj_eq[1] = 0x800A; inj_mask[0] = 1; inj_mask[1] = 0; inj_mask[2] = 1; inj_mask[3] = 1;
			inj_scis = 1; inj_sbox[0] = 5; inj_sbox[1] = 6; inj_sbox[2] = 7; inj_sbox[3] = 8; inj_depth = 1;
			/* env parsing */
			CHECK(sweet_census_parse("1", &arm) == 1 && arm == 0);
			CHECK(sweet_census_parse("600", &arm) == 1 && arm == 600);
			CHECK(sweet_census_parse("2", &arm) == 1 && arm == 2);
			CHECK(sweet_census_parse("1abc", &arm) == 1 && arm == 0);
			CHECK(sweet_census_parse("0", &arm) == 0 && arm == 0 && sweet_census_parse("", &arm) == 0 && sweet_census_parse(NULL, &arm) == 0 && sweet_census_parse("x", &arm) == 0 && sweet_census_parse("-5", &arm) == 0);
			CHECK(sweet_census_parse(" 12", &arm) == 1 && arm == 12 && sweet_census_parse("+5", &arm) == 1 && arm == 5);   /* F5: strtol accepts leading space / plus: armed */
			CHECK(sweet_census_parse("1 ", &arm) == 1 && arm == 0 && sweet_census_parse("1x", &arm) == 1 && arm == 0 && sweet_census_parse(" 1", &arm) == 0 && arm == 0 && sweet_census_parse("12", &arm) == 1 && arm == 12);   /* " 1": old rule (e[0] != 1) = off, same as before */
			CHECK(sweet_census_parse("99999999999", &arm) == 1 && arm == 1000000);
			/* census=1 pin: rounds never fire however late it is; 16 early samples = 16 reads, one read each, the old line text (no round / quad) */
			sweet_px_reset(); sweet_px_set_clock(mock_clk);
			CHECK(!sweet_px_sched_on());
			inj_on = 1; rp_fill = 1; rp_multi = 0; inj_pbo = 0; inj_sb = 0; inj_rbuf = 0x8CE0; inj_crf = 0x1908; inj_crt = 0x1401; inj_al = 4; inj_rl = 0; inj_sp = 0; inj_sr = 0; inj_atype = 0x1702; fake_ret = 0x8cd5;
			rp_calls = 0;
			cap_begin(); for (k = 0; k < 40; k++) DRAW_B(cx, 1 + k / 4, 100000 + k * 100); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 16 && OB_COUNT("SWEETGL: PIXELS blur T fbo") == 16 && strstr(ob, " round") == NULL && strstr(ob, " quad") == NULL);
			CHECK(strstr(ob, "SWEETGL: PIXELS blur T fbo 1 vp 97x76 nonzero 3 of 16 maxa 0xff sum 0x110 written 1\n") != NULL);
			/* armed at 600: the schedule starts 600 s after the init (mock clock 1000) */
			sweet_px_reset(); mock_t = 1000;
			setenv("SWEETGL_CENSUS", "600", 1); cap_begin(); sweet_gli_init(&cy, &dummy); cap_end(ob, sizeof ob); unsetenv("SWEETGL_CENSUS");
			CHECK(sweet_px_sched_on() && !strcmp(ob, "SWEETGL: PIXELS rounds armed at 600 s (clock monotonic)\n"));   /* review F2: exact text, once */
			mock_t = 1200; setenv("SWEETGL_CENSUS", "600", 1); cap_begin(); sweet_gli_init(&cy, &dummy); cap_end(ob, sizeof ob); unsetenv("SWEETGL_CENSUS"); CHECK(ob[0] == 0);   /* a second init (next context) must NOT restart the clock: arming stays at 1000 + 600 (a restart would arm at 1800) */
			for (k = 0; k < 16; k++) CHECK(sweet_px_claim(0, 100 + k / 4, 0));   /* exhaust the early budget: only rounds are left */
			for (k = 0; k < 4; k++) CHECK(sweet_px_claim(1, 0, k * 5));
			for (k = 0; k < 8; k++) { rp_pats[0][k] = (unsigned char)(k < 4 ? k + 1 : 0); }
			memcpy(rp_pats[0], rp_pat, 64);                                  /* centre: nonzero 3 maxa ff sum 0x110 */
			memset(rp_pats[1], 0, 64); rp_pats[1][0] = 9;                    /* quad a: 1 */
			memset(rp_pats[2], 0xff, 64);                                    /* quad b: 16 */
			memset(rp_pats[3], 0, 64);                                       /* quad c: 0 */
			memset(rp_pats[4], 0, 64); rp_pats[4][0] = 1; rp_pats[4][20] = 1;   /* quad d: 2 */
			rp_multi = 1;
			rp_calls = 0; seq_n = 0;
			cap_begin(); DRAW_B(cy, 21, 1599); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 0 && ob[0] == 0 && seq_n == 4);   /* 599 s after init: not armed yet (pre queries + draw) */
			mock_t = 1599; sweet_px_round_tick(1599, &ri); CHECK(ri.round == 0 && ri.exp_round == 0);
			rp_calls = 0; seq_n = 0;
			cap_begin(); DRAW_B(cy, 21, 1600); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 5 && count_seq("_mesa_ReadPixels") == 5 && count_seq("_mesa_GetError") == 0);
			{   /* gates once per sample: pre 3 + draw + 12 gate queries + 5 reads */
				unsigned c88 = 0; for (unsigned q = 0; q < seq_n && q < 64; q++) if (seq_a0[q] == 0x88ED) c88++;
				CHECK(c88 == 1 && seq_n == 3 + 1 + 12 + 5 + 13);
				{   /* the 13 state queries, in order, after the prov line (no GL call between them) */
					static const unsigned sq[13] = { 0x8B8D, 0x0BE2, 0x80C9, 0x80C8, 0x80CB, 0x80CA, 0x8009, 0x883D, 0x0C23, 0x0C11, 0x0C10, 0x0BA2, 0x0B71 };
					int okq = 1;
					for (int q = 0; q < 13; q++) okq = okq && seq_a0[21 + q] == sq[q] && !strcmp(seq_name[21 + q], "_mesa_GetIntegerv");
					CHECK(okq);
				}
			}
			/* coordinates: vp origin (10,20) size 97x76: centre, then a b c d = (w/4,h/4) (3w/4,h/4) (w/4,3h/4) (3w/4,3h/4) */
			CHECK(rp_log[0][0] == 56 && rp_log[0][1] == 56 && rp_log[1][0] == 32 && rp_log[1][1] == 37 && rp_log[2][0] == 80 && rp_log[2][1] == 37 && rp_log[3][0] == 32 && rp_log[3][1] == 75 && rp_log[4][0] == 80 && rp_log[4][1] == 75);
			for (k = 0; k < 5; k++) CHECK(rp_log[k][2] == 4 && rp_log[k][3] == 4);
			CHECK(!strcmp(ob, "SWEETGL: PIXELS blur T fbo 21 vp 97x76 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 1 quad 1/16/0/2\nSWEETGL: PIXELS prov r1 fbo 21 units: -\nSWEETGL: PIXELS state r1 fbo 21 prog 4 kind b mode 0x4 count 3 blend 1 0x302/0x303/0x1/0x304 eq 0x8006/0x800a mask 1011 scissor 1 5,6,7,8 vp 10,20,97,76 depth 1\n"));   /* part A: one prov line after each armed sample */
			rp_calls = 0; DRAW_B(cy, 21, 1601); CHECK(rp_calls == 0);   /* same FBO again within 10 s: distinct FBOs preferred */
			cap_begin(); DRAW_B(cy, 22, 1601); DRAW_B(cy, 23, 1601); DRAW_B(cy, 24, 1601); cap_end(ob, sizeof ob);
			CHECK(OB_COUNT("round 1 quad") == 3 && strstr(ob, "done") == NULL);
			rp_calls = 0; DRAW_B(cy, 25, 1601); CHECK(rp_calls == 0);   /* 4 blur per round */
			cap_begin(); DRAW_F(cy, 7, 1601); cap_end(ob, sizeof ob);
			CHECK(strstr(ob, "SWEETGL: PIXELS fbfetch S fbo 7 vp 1125x2436 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 1 quad ") != NULL && strstr(ob, "done") == NULL);
			cap_begin(); DRAW_F(cy, 7, 1602); cap_end(ob, sizeof ob);
			pp = strstr(ob, "round 1 quad "); CHECK(pp != NULL);
			CHECK(strstr(ob, "\nSWEETGL: PIXELS round 1 done samples 6 timeout 0\n") != NULL && strstr(ob, "cap") == NULL);
			rp_calls = 0; DRAW_F(cy, 7, 1603); DRAW_B(cy, 30, 1603); CHECK(rp_calls == 0);
			/* 60 s after the completion (1602): 1661 nothing, 1662 round 2 */
			DRAW_B(cy, 31, 1661); CHECK(rp_calls == 0);
			cap_begin(); DRAW_B(cy, 31, 1662); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 5 && strstr(ob, "written 1 round 2 quad ") != NULL);
			/* timeout 20 s: at 1681 still active (a new FBO is sampled), at 1682 the round ends with 2 samples, timeout 1 */
			cap_begin(); DRAW_B(cy, 32, 1681); cap_end(ob, sizeof ob);
			CHECK(strstr(ob, "round 2 quad") != NULL && strstr(ob, "done") == NULL);
			rp_calls = 0; cap_begin(); DRAW_B(cy, 33, 1682); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 0 && !strcmp(ob, "SWEETGL: PIXELS round 2 done samples 2 timeout 1\n"));
			DRAW_B(cy, 34, 1741); CHECK(rp_calls == 0);   /* next round: timeout instant (1662 + 20) + 60 = 1742 */
			cap_begin(); DRAW_B(cy, 35, 1742); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 5 && strstr(ob, "round 3 quad") != NULL);
			rp_calls = 0; DRAW_B(cy, 35, 1743); CHECK(rp_calls == 0);   /* repeat within 10 s: no */
			DRAW_B(cy, 35, 1751); CHECK(rp_calls == 0);   /* F5: 9 s into the round (start 1742): still no; 10 s accepted below */
			cap_begin(); DRAW_B(cy, 35, 1752); cap_end(ob, sizeof ob);  /* repeat after 10 s into the round: accepted (one-FBO scenes) */
			CHECK(rp_calls == 5 && strstr(ob, "round 3 quad") != NULL);
			/* the 20 s census thread tick ends a timed-out round without a draw */
			sweet_px_round_tick(1761, &ri); CHECK(ri.exp_round == 0);
			sweet_px_round_tick(1762, &ri); CHECK(ri.exp_round == 3 && ri.exp_samples == 2 && ri.cap_e == 0);
			sweet_px_round_tick(1763, &ri); CHECK(ri.exp_round == 0);   /* once */
			{   /* rounds 4..16: complete rounds 60 s apart (next start = 1762 + 60), then the cap line exactly once and nothing more */
				long long t = 1822;
				int rd;
				cap_begin();
				for (rd = 4; rd <= 16; rd++) {
					for (k = 0; k < 4; k++) DRAW_B(cy, 40 + k, t);
					DRAW_F(cy, 7, t); DRAW_F(cy, 7, t);
					t += 60;
				}
				for (k = 0; k < 40; k++) { DRAW_B(cy, 50 + k, t); DRAW_F(cy, 7, t); t += 100; }
				cap_end(ob, sizeof ob);
				CHECK(OB_COUNT("SWEETGL: PIXELS rounds cap") == 1 && OB_COUNT("done samples 6 timeout 0") == 13 && OB_COUNT("round 16 done samples 6 timeout 0\nSWEETGL: PIXELS rounds cap\n") == 1);
				CHECK(strstr(ob, "round 17") == NULL && OB_COUNT("round 15 done") == 1);
				sweet_px_round_tick(t + 1000, &ri); CHECK(ri.exp_round == 0 && ri.cap_e == 0);
				sweet_px_stats_line(l, sizeof l);
				CHECK(strstr(l, " rounds 16 claims 88 quadsmall 0") != NULL);
			}
			/* armed stats line: legacy prefix intact + the round fields */
			CHECK(strstr(l, "SWEETGL: PIXELS stats samples ") == l);

			/* early samples in armed mode: round 0, five points, regex-visible text intact */
			sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000;
			setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
			rp_calls = 0; cap_begin(); DRAW_B(cy, 5, 1000); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 5 && !strcmp(ob, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 0 quad 1/16/0/2\nSWEETGL: PIXELS prov r0 fbo 5 units: -\nSWEETGL: PIXELS state r0 fbo 5 prog 4 kind b mode 0x4 count 3 blend 1 0x302/0x303/0x1/0x304 eq 0x8006/0x800a mask 1011 scissor 1 5,6,7,8 vp 10,20,97,76 depth 1\n"));
			/* quadrants: viewport 8x8 at the origin reads all five (2,2) (0,0) (4,0) (0,4) (4,4); 7x7 / 4x4 / 5x9 read the centre only and print - */
			sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
			mock_t = 1000; inj_prog = 4; inj_fbo = 5; inj_readfb = 5; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 8; inj_vp[3] = 8;
			rp_calls = 0; cap_begin(); ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3); cap_end(ob, sizeof ob);
			CHECK(rp_calls == 5 && rp_log[0][0] == 2 && rp_log[0][1] == 2 && rp_log[1][0] == 0 && rp_log[1][1] == 0 && rp_log[2][0] == 4 && rp_log[2][1] == 0 && rp_log[3][0] == 0 && rp_log[3][1] == 4 && rp_log[4][0] == 4 && rp_log[4][1] == 4);
			sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
			{
				static const int sz[3][2] = { { 7, 7 }, { 4, 4 }, { 5, 9 } };
				for (k = 0; k < 3; k++) {
					inj_vp[2] = sz[k][0]; inj_vp[3] = sz[k][1]; inj_fbo = 5 + k; inj_readfb = 5 + k;
					rp_calls = 0; cap_begin(); ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 1 && strstr(ob, " written 1 round 0 quad -/-/-/-\n") != NULL);
				}
				sweet_px_stats_line(l, sizeof l);
				CHECK(!strcmp(l, "SWEETGL: PIXELS stats samples 3 skipped 0 reasons pbo=0 fbo=0 rb=0 align=0 small=0 rounds 0 claims 0 quadsmall 3 unbind 0 skipped 0 ctxover 0 grid 0 gridskip 0"));
			}
			/* a quadrant read that writes nothing prints ? */
			sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
			inj_vp[2] = 97; inj_vp[3] = 76; inj_vp[0] = 10; inj_vp[1] = 20; memset(rp_pats[2], 0xA5, 64); rp_calls = 0;
			cap_begin(); ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3); cap_end(ob, sizeof ob);
			CHECK(strstr(ob, " round 0 quad 1/?/0/2\n") != NULL);
			/* the safety gates still skip the whole sample (no read at all, no line), one gate evaluation, no glGetError */
			{
				static const char *const nm[] = { "pbo", "fbo", "rb", "align" };
				for (k = 0; k < 4; k++) {
					sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
					inj_pbo = k == 0 ? 3 : 0; inj_readfb = k == 1 ? 4 : inj_fbo; inj_rbuf = k == 2 ? 0x0405 : 0x8CE0; inj_al = k == 3 ? 16 : 4;
					rp_calls = 0; seq_n = 0; cap_begin(); ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 0 && ob[0] == 0 && count_seq("_mesa_GetError") == 0);
					sweet_px_stats_line(l, sizeof l);
					CHECK(strstr(l, "samples 0 skipped 1") != NULL && strstr(l, nm[k]) != NULL);
				}
				inj_pbo = 0; inj_readfb = inj_fbo; inj_rbuf = 0x8CE0; inj_al = 4;
			}
			/* F1: the completing claim FAILS a gate / the viewport is < 4: the done line is still printed, exactly once, and no sample line for that draw */
			for (int variant = 0; variant < 3; variant++) {
				sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; rp_multi = 0;
				setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
				for (k = 0; k < 16; k++) sweet_px_claim(0, 100 + k / 4, 0);
				for (k = 0; k < 4; k++) sweet_px_claim(1, 0, k * 5);
				inj_pbo = 0; inj_al = 4; inj_rbuf = 0x8CE0; fake_ret = 0x8cd5; inj_readfb = inj_fbo;
				cap_begin();
				for (int rd = 1; rd <= (variant == 2 ? 16 : 1); rd++) {
					long long t = 1002 + 60 * (rd - 1);
					for (k = 0; k < 3; k++) DRAW_B(cy, 60 + k, t);
					DRAW_F(cy, 7, t); 
					if (variant == 2 && rd < 16) { DRAW_F(cy, 7, t); DRAW_B(cy, 63, t); continue; }
					DRAW_F(cy, 7, t);
					rp_calls = 0;
					if (variant == 1) { mock_t = t; inj_prog = 4; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 3; inj_vp[3] = 3; inj_fbo = 63; inj_readfb = 63; ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3); }
					else { inj_pbo = 3; DRAW_B(cy, 63, t); inj_pbo = 0; }
				}
				cap_end(ob, sizeof ob);
				CHECK(rp_calls == 0);
				if (variant == 2) CHECK(OB_COUNT("round 16 done samples 6 timeout 0\nSWEETGL: PIXELS rounds cap\n") == 1 && OB_COUNT("rounds cap") == 1 && OB_COUNT("done") == 16);
				else CHECK(OB_COUNT("round 1 done samples 6 timeout 0\n") == 1 && OB_COUNT("done") == 1 && strstr(ob, "rounds cap") == NULL && OB_COUNT("fbo 63") == 0);
				sweet_px_stats_line(l, sizeof l);
				CHECK(strstr(l, variant == 1 ? "small=1" : "pbo=1") != NULL);
			}
			/* F5: the census thread's per-tick function (the thread's own call of it is one line and cannot run from the host: static, 20 s sleep) */
			sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000;
			setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
			for (k = 0; k < 16; k++) sweet_px_claim(0, 100 + k / 4, 0);
			DRAW_B(cy, 70, 1002);
			mock_t = 1021; cap_begin(); sweet_px_census_tick(); cap_end(ob, sizeof ob); CHECK(ob[0] == 0);
			mock_t = 1022; cap_begin(); sweet_px_census_tick(); cap_end(ob, sizeof ob); CHECK(!strcmp(ob, "SWEETGL: PIXELS round 1 done samples 1 timeout 1\n"));
			cap_begin(); sweet_px_census_tick(); cap_end(ob, sizeof ob); CHECK(ob[0] == 0);
			/* ---- part A: input provenance ---- */
			{
				unsigned tu[8];
				sweet_tu_reset(); sweet_th_reset();
				sweet_tu_snapshot(1, tu); sweet_prov_line(l, sizeof l, 3, 5, tu);
				CHECK(!strcmp(l, "SWEETGL: PIXELS prov r3 fbo 5 units: -"));
				{   /* unit switch, rebind, ignored targets / units */
					sweet_tu_bind(1, 0xDE1, 25);                       /* default unit 0 */
					sweet_tu_active(1, 0x84C1); sweet_tu_bind(1, 0xDE1, 31);
					sweet_tu_bind(1, 0x8513, 99);                      /* cube map: ignored */
					sweet_tu_bind(1, 0x8D65, 98);                      /* external: ignored */
					sweet_tu_active(1, 0x84C8); sweet_tu_bind(1, 0xDE1, 7);   /* unit 8: ignored (and does not leak into unit 0) */
					sweet_tu_active(1, 0x84BF); sweet_tu_bind(1, 0xDE1, 8);   /* below TEXTURE0: ignored */
					sweet_tu_snapshot(1, tu);
					CHECK(tu[0] == 25 && tu[1] == 31 && tu[2] == 0 && tu[7] == 0);
					sweet_tu_active(1, 0x84C0); sweet_tu_bind(1, 0xDE1, 26); sweet_tu_snapshot(1, tu); CHECK(tu[0] == 26 && tu[1] == 31);   /* rebind */
					sweet_tu_bind(1, 0xDE1, 0); sweet_tu_snapshot(1, tu); CHECK(tu[0] == 0 && tu[1] == 31);
					sweet_tu_active(1, 0x84C7); sweet_tu_bind(1, 0xDE1, 44); sweet_tu_snapshot(1, tu); CHECK(tu[7] == 44 && tu[0] == 0);   /* last unit of the table */
				}
				{   /* history: sizes, att, unknown att, deleted, no history, mip level, 8-unit bound with +N more */
					struct sweet_th_ev e;
					sweet_tu_reset();
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXSTORAGE; e.b = 1; e.c = 1082; e.d = 1226; sweet_th_record(25, e);
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = 3; sweet_th_record(25, e);
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXIMAGE; e.b = 0; e.c = 97; e.d = 76; sweet_th_record(40, e);
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_DELETED; sweet_th_record(40, e);
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXIMAGE; e.b = 1; e.c = 10; e.d = 10; sweet_th_record(41, e);   /* level 1 only: no size */
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = -1; sweet_th_record(41, e);                      /* fbo unknown */
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = 3; sweet_th_record(25, e);
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = 9; sweet_th_record(25, e);                      /* the LATEST fbotex wins */
					memset(tu, 0, sizeof tu); tu[0] = 25; tu[1] = 31; tu[2] = 40; tu[3] = 41;
					sweet_prov_line(l, sizeof l, 2, 5, tu);
					CHECK(!strcmp(l, "SWEETGL: PIXELS prov r2 fbo 5 units: u0 tex 25 1082x1226 att 9 | u1 tex 31 ? att - | u2 tex 40 del att - | u3 tex 41 ? att ?"));
					for (k = 0; k < 8; k++) tu[k] = 100 + (unsigned)k;
					tu[0] = 25;
					sweet_prov_line(l, sizeof l, 0, -1, tu);
					CHECK(!strcmp(l, "SWEETGL: PIXELS prov r0 fbo -1 units: u0 tex 25 1082x1226 att 9 | u1 tex 101 ? att - | u2 tex 102 ? att - | u3 tex 103 ? att - | +4 more"));
					memset(tu, 0, sizeof tu); tu[7] = 25; tu[5] = 31;
					sweet_prov_line(l, sizeof l, 1, 1, tu);
					CHECK(!strcmp(l, "SWEETGL: PIXELS prov r1 fbo 1 units: u5 tex 31 ? att - | u7 tex 25 1082x1226 att 9"));
					sweet_prov_line(l, 40, 1, 1, tu); CHECK(strlen(l) < 40);   /* truncation stays inside the buffer */
				}
				{   /* through the hooks: ActiveTexture then BindTexture, each forwarded exactly once with the same arguments; the history still gets its bind event */
					sweet_tu_reset(); sweet_th_reset();
					seq_n = 0; hits = 0;
					((void (*)(void *, unsigned))cy.disp[342])(cy.rctx, 0x84C2);
					CHECK(seq_n == 1 && count_seq("_mesa_ActiveTexture") == 1 && args[0] == 0x84C2);
					seq_n = 0; hits = 0;
					((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0xde1, 77);
					CHECK(seq_n == 1 && count_seq("_mesa_BindTexture") == 1 && args[0] == 0xde1 && args[1] == 77);
					sweet_tu_snapshot(1, tu); CHECK(tu[2] == 77 && tu[0] == 0);
					CHECK(sweet_th_line(77, 0, l, sizeof l) && strstr(l, "bind 0xde1") != NULL);
					((void (*)(void *, unsigned))cy.disp[342])(cy.rctx, 0x84C0);
				}
				{   /* armed sample followed by its prov line (units tracked through the hooks) */
					struct sweet_th_ev e;
					sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
					sweet_tu_reset(); sweet_th_reset();
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXSTORAGE; e.b = 1; e.c = 1082; e.d = 1226; sweet_th_record(25, e);
					memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = 3; sweet_th_record(25, e);
					((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0xde1, 25);
					memset(rp_pats[1], 0, 64); rp_pats[1][0] = 9; memset(rp_pats[2], 0xff, 64); memset(rp_pats[3], 0, 64); memset(rp_pats[4], 0, 64); rp_pats[4][0] = 1; rp_pats[4][20] = 1;
					rp_multi = 1; memcpy(rp_pats[0], rp_pat, 64); rp_calls = 0; inj_pbo = 0; inj_al = 4; inj_rbuf = 0x8CE0; fake_ret = 0x8cd5;
					cap_begin(); DRAW_B(cy, 5, 1000); cap_end(ob, sizeof ob);
					CHECK(!strcmp(ob, "SWEETGL: PIXELS blur T fbo 5 vp 97x76 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 0 quad 1/16/0/2\nSWEETGL: PIXELS prov r0 fbo 5 units: u0 tex 25 1082x1226 att 3\nSWEETGL: PIXELS state r0 fbo 5 prog 4 kind b mode 0x4 count 3 blend 1 0x302/0x303/0x1/0x304 eq 0x8006/0x800a mask 1011 scissor 1 5,6,7,8 vp 10,20,97,76 depth 1\n"));
					sweet_tu_reset();
				}
			}
			/* ---- part B: unbind readback of large offscreen FBOs ---- */
#define UB_ARM() do { sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS"); \
	{ for (k = 0; k < 16; k++) { sweet_px_claim(0, 100 + k / 4, 0); } for (k = 0; k < 4; k++) { sweet_px_claim(1, 0, k * 5); } } sweet_ub_reset(); sweet_tu_reset(); \
	memset(rp_pats[1], 0, 64); rp_pats[1][0] = 9; memset(rp_pats[2], 0xff, 64); memset(rp_pats[3], 0, 64); memset(rp_pats[4], 0, 64); rp_pats[4][0] = 1; rp_pats[4][20] = 1; rp_multi = 1; memcpy(rp_pats[0], rp_pat, 64); rp_calls = 0; \
	inj_pbo = 0; inj_al = 4; inj_rbuf = 0x8CE0; inj_atype = 0x1702; fake_ret = 0x8cd5; } while (0)
#define BIND_FB(fbo_, t_) do { mock_t = (t_); ((void (*)(void *, unsigned, unsigned))cy.disp[672])(cy.rctx, 0x8D40, (unsigned)(fbo_)); inj_fbo = (fbo_); inj_readfb = (fbo_); } while (0)   /* GL state after the bind: the old FBO is still bound while the hook samples */
#define BIND_T(tg_, fbo_, t_) do { mock_t = (t_); ((void (*)(void *, unsigned, unsigned))cy.disp[672])(cy.rctx, (tg_), (unsigned)(fbo_)); inj_fbo = (fbo_); inj_readfb = (fbo_); } while (0)
#define DRAW_G(fbo_, w_, h_, t_) do { mock_t = (t_); inj_prog = 0; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = (w_); inj_vp[3] = (h_); inj_fbo = (fbo_); inj_readfb = (fbo_); ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3); } while (0)
			{
				static const char UBGRID0[] = "SWEETGL: PIXELS grid r1 fbo 3 vp 1082x1226 written 0\n";
				static const char UBCAPD[] = "SWEETGL: PIXELS capdraws r1 fbo 3 draws 1 kinds b:0 f:0 c:0 l:0 r:0 o:1 verts 3 progs 0..0 blendlast 0 since-bind\n";
				static const char UBCAPV[] = "SWEETGL: PIXELS capvp r1 fbo 3 first vp 0,0,1082,1226 mode 0x4 count 3 last vp 0,0,1082,1226 mode 0x4 count 3\n";
				static char UBFULL[768];
				static const char UBLINE[] = "SWEETGL: PIXELS unbind S fbo 3 vp 1082x1226 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 1 quad 1/16/0/2\n";
				int bpos, lastrp;
				snprintf(UBFULL, sizeof UBFULL, "%s%s%s%s", UBLINE, UBGRID0, UBCAPD, UBCAPV);
				UB_ARM();
				BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002);
				/* sample BEFORE the forwarded bind, FBO still bound, one forward with exact args, 5 reads with exact coordinates, no glGetError */
				rp_calls = 0; seq_n = 0; hits = 0;
				cap_begin(); mock_t = 1002; inj_fbo = 3; inj_readfb = 3; ((void (*)(void *, unsigned, unsigned))cy.disp[672])(cy.rctx, 0x8D40, 5); cap_end(ob, sizeof ob);
				CHECK(!strcmp(ob, UBFULL));   /* sample line, grid line (the fake writes nothing: written 0), capdraws line */
				CHECK(rp_calls == 6 && count_seq("_mesa_ReadPixels") == 6 && count_seq("_mesa_BindFramebuffer") == 1 && count_seq("_mesa_GetError") == 0);
				bpos = -1; lastrp = -1;
				for (unsigned q = 0; q < seq_n && q < 64; q++) { if (!strcmp(seq_name[q], "_mesa_BindFramebuffer")) bpos = (int)q; if (!strcmp(seq_name[q], "_mesa_ReadPixels")) lastrp = (int)q; }
				CHECK(bpos == (int)seq_n - 1 && lastrp < bpos && lastrp >= 0 && args[0] == 0x8D40 && args[1] == 5);
				CHECK(rp_log[5][0] == 0 && rp_log[5][1] == 0 && rp_log[5][2] == 1082 && rp_log[5][3] == 1226);   /* the grid read: whole viewport, after the five patches, before the forwarded bind */
				CHECK(rp_log[0][0] == 539 && rp_log[0][1] == 611 && rp_log[1][0] == 268 && rp_log[1][1] == 304 && rp_log[2][0] == 809 && rp_log[2][1] == 304 && rp_log[3][0] == 268 && rp_log[3][1] == 917 && rp_log[4][0] == 809 && rp_log[4][1] == 917);
				sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, "samples 0 skipped 0") != NULL && strstr(l, " unbind 1 skipped 0 ctxover 0") != NULL);   /* F3: unbind reads stay out of the shared samples counter */
				/* not armed yet: a bind before the start time (1002) takes nothing but still forwards once */
				UB_ARM();
				BIND_FB(3, 1001); DRAW_G(3, 1082, 1226, 1001);
				rp_calls = 0; seq_n = 0; cap_begin(); BIND_FB(5, 1001); cap_end(ob, sizeof ob);
				CHECK(rp_calls == 0 && ob[0] == 0 && count_seq("_mesa_BindFramebuffer") == 1 && seq_n == 1);
				/* FBO 0 is never sampled; no draw, small viewports, the scanout size, an unchanged bind */
				UB_ARM();
				BIND_FB(0, 1002); DRAW_G(0, 1082, 1226, 1002);
				inj_rbuf = 0x0405; inj_atype = 0;   /* default-framebuffer gates satisfied: only the eligibility rule can stop the read */
				rp_calls = 0; cap_begin(); BIND_FB(3, 1002); cap_end(ob, sizeof ob); CHECK(rp_calls == 0 && ob[0] == 0);       /* leaving FBO 0 */
				inj_rbuf = 0x8CE0; inj_atype = 0x1702;
				rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob); CHECK(rp_calls == 0 && ob[0] == 0);       /* FBO 3 had no draw */
				{
					static const int cases[][2] = { { 999, 1226 }, { 1000, 99 }, { 1125, 2436 }, { 4, 4 } };
					for (unsigned c = 0; c < 4; c++) {
						BIND_FB(3, 1002); DRAW_G(3, cases[c][0], cases[c][1], 1002);
						rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob); CHECK(rp_calls == 0 && ob[0] == 0);
					}
					BIND_FB(3, 1002); DRAW_G(3, 1000, 100, 1002);   /* the exact thresholds read */
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob); CHECK(rp_calls == 6 && strstr(ob, "unbind S fbo 3 vp 1000x100 nonzero") != NULL);
				}
				UB_ARM();
				BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002);
				rp_calls = 0; seq_n = 0; cap_begin(); BIND_FB(3, 1002); cap_end(ob, sizeof ob);   /* same FBO again: no change, no sample, forwarded once */
				CHECK(rp_calls == 0 && ob[0] == 0 && count_seq("_mesa_BindFramebuffer") == 1);
				rp_calls = 0; seq_n = 0; ((void (*)(void *, unsigned, unsigned))cy.disp[672])(cy.rctx, 0x8CA8 /* READ_FRAMEBUFFER */, 7);   /* read binds do not change the draw FBO */
				CHECK(rp_calls == 0 && seq_n == 1);
				rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob); CHECK(rp_calls == 6);   /* the earlier draws were kept */
				/* a gate (pbo) skips the read but the bind is still forwarded once */
				UB_ARM();
				BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002); inj_pbo = 3;
				rp_calls = 0; seq_n = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
				CHECK(rp_calls == 0 && ob[0] == 0 && count_seq("_mesa_BindFramebuffer") == 1 && count_seq("_mesa_GetError") == 0);
				sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, "samples 0 skipped 0 reasons pbo=0") != NULL && strstr(l, " unbind 0 skipped 1 ctxover 0") != NULL); inj_pbo = 0;   /* F3: counted apart, claim given back */
				/* per round 2 (an active round keeps the same budget), the next round gets 2 more */
				UB_ARM();
				for (k = 0; k < 3; k++) { BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002); rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob); CHECK((rp_calls == 6) == (k < 2)); }
				DRAW_B(cy, 21, 1003);   /* starts round 1 (blur claim) */
				BIND_FB(3, 1004); DRAW_G(3, 1082, 1226, 1004); rp_calls = 0; BIND_FB(5, 1004); CHECK(rp_calls == 0);   /* still round 1: 2 used */
				{
					struct sweet_px_rinfo ri;
					sweet_px_round_tick(1023, &ri); CHECK(ri.exp_round == 1);   /* timeout; next start 1023 + ... = 1003 + 20 + 60 = 1083 */
					BIND_FB(3, 1082); DRAW_G(3, 1082, 1226, 1082); rp_calls = 0; BIND_FB(5, 1082); CHECK(rp_calls == 0);
					BIND_FB(3, 1083); DRAW_G(3, 1082, 1226, 1083); rp_calls = 0; cap_begin(); BIND_FB(5, 1083); cap_end(ob, sizeof ob); CHECK(rp_calls == 6 && strstr(ob, "written 1 round 2 quad") != NULL);
				}
				/* total 24 with the cap line once, right after the 24th sample line; nothing after */
				UB_ARM();
				cap_begin();
				for (int rd = 1; rd <= 13; rd++) {
					long long t = 2000 + 100LL * rd;
					DRAW_B(cy, 21, t);   /* starts round rd */
					for (k = 0; k < 2; k++) {
						if (rd == 12 && k == 1) { BIND_FB(3, t); DRAW_G(3, 1082, 1226, t); inj_pbo = 3; BIND_FB(5, t); inj_pbo = 0; }   /* F3: a skipped 24th attempt gives the claim back: no cap line yet */
						BIND_FB(3, t); DRAW_G(3, 1082, 1226, t); BIND_FB(5, t);
					}
					sweet_px_round_tick(t + 20, &ri);
				}
				cap_end(ob, sizeof ob);
				CHECK(OB_COUNT("SWEETGL: PIXELS unbind S fbo 3") == 24 && OB_COUNT("unbind cap") == 1 && OB_COUNT("unbind S fbo 3 vp 1082x1226 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 13") == 0 && OB_COUNT("unbind S fbo 3 vp 1082x1226 nonzero 3 of 16 maxa 0xff sum 0x110 written 1 round 12") == 2);
				pp = strstr(ob, "SWEETGL: PIXELS unbind cap\n"); CHECK(pp != NULL && strstr(pp, "unbind S") == NULL);
				{ const char *q = ob; int last = -1; while ((q = strstr(q, "unbind S fbo 3")) != NULL) { last = (int)(q - ob); q++; } CHECK(last >= 0 && pp != NULL && (int)(pp - ob) > last); }
				sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " unbind 24 skipped 1 ctxover 0 grid 3 gridskip 21") != NULL);
				{   /* review b6 F3: at most 3 grids per FBO name (24 unbind samples of fbo 3 -> 3 grids, 21 counted skips, no cap line: the process total is not reached) */
					CHECK(OB_COUNT("SWEETGL: PIXELS grid r") == 3 && OB_COUNT("SWEETGL: PIXELS grid cap") == 0);
				}
				{   /* grid budget: 16 per process over several FBO names (6 names x 3), the loud cap line once, right after the 16th grid line, no grid line after it */
					struct sweet_px_rinfo ri2;
					UB_ARM();
					cap_begin();
					for (int rd = 1; rd <= 9; rd++) {
						long long t = 2000 + 100LL * rd;
						DRAW_B(cy, 21, t);
						for (k = 0; k < 2; k++) { int fb = 10 + ((rd - 1) * 2 + k) % 6; BIND_FB(fb, t); DRAW_G(fb, 1082, 1226, t); BIND_FB(5, t); }
						sweet_px_round_tick(t + 20, &ri2);
					}
					cap_end(ob, sizeof ob);
					{
						int ng = OB_COUNT("SWEETGL: PIXELS grid r"); const char *gc = strstr(ob, "SWEETGL: PIXELS grid cap total 16 per-fbo 3\n"), *q = ob, *lastg = NULL;
						while ((q = strstr(q, "SWEETGL: PIXELS grid r")) != NULL) { lastg = q; q++; }
						CHECK(ng == 16 && OB_COUNT("SWEETGL: PIXELS grid cap") == 1 && gc != NULL && lastg != NULL && gc > lastg && strstr(gc, "grid r") == NULL && OB_COUNT("SWEETGL: PIXELS unbind S") == 18);
						sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " grid 16 gridskip 0") != NULL);
					}
				}
				/* DRAW_FRAMEBUFFER (0x8CA9) binds change the draw FBO too; a READ_FRAMEBUFFER bind of the same name does not sample */
				UB_ARM();
				BIND_T(0x8CA9, 3, 1002); DRAW_G(3, 1082, 1226, 1002);
				rp_calls = 0; seq_n = 0; cap_begin(); BIND_T(0x8CA9, 5, 1002); cap_end(ob, sizeof ob);
				CHECK(rp_calls == 6 && !strcmp(ob, UBFULL) && count_seq("_mesa_BindFramebuffer") == 1 && args[0] == 0x8CA9);
				/* the READ binding differs from f (gate): no read, forwarded once, no GL error, budget NOT burned (2 samples still follow in the same round), shared counters untouched */
				UB_ARM();
				BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002); inj_readfb = 9;
				rp_calls = 0; seq_n = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
				CHECK(rp_calls == 0 && ob[0] == 0 && count_seq("_mesa_BindFramebuffer") == 1 && count_seq("_mesa_GetError") == 0);
				sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, "samples 0 skipped 0 reasons pbo=0 fbo=0") != NULL && strstr(l, " unbind 0 skipped 1 ") != NULL);
				for (k = 0; k < 3; k++) { BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002); rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob); CHECK((rp_calls == 6) == (k < 2)); }
				/* F1: the rect target 0x84F5 arrives raw at census (the mapping to 0xde1 is below it): tracked as the unit's binding; other targets still ignored */
				sweet_tu_reset(); sweet_th_reset();
				{
					unsigned tu[8]; int ci;
					seq_n = 0; ((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0x84F5, 50);
					ci = sweet_th_ctx_index(&dummy);
					sweet_tu_snapshot(ci, tu); CHECK(tu[0] == 50 && count_seq("_mesa_BindTexture") == 1 && args[0] == 0x84F5 && args[1] == 50);
					((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0x8513, 51); sweet_tu_snapshot(ci, tu); CHECK(tu[0] == 50);
					((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0xde1, 52); sweet_tu_snapshot(ci, tu); CHECK(tu[0] == 52);
				}
				/* F2: two contexts. Interleaved ActiveTexture / BindTexture land on each context's own unit */
				{
					static int dummyB, ovf[16];
					unsigned ta[8], tb[8]; int ca, cb;
					typedef void (*f_at)(void *, unsigned); typedef void (*f_bt)(void *, unsigned, unsigned);
					UB_ARM(); sweet_th_reset();
					((f_at)cy.disp[342])(&dummy, 0x84C1); ((f_at)cy.disp[342])(&dummyB, 0x84C0);
					((f_bt)cy.disp[5])(&dummy, 0xde1, 31); ((f_bt)cy.disp[5])(&dummyB, 0xde1, 55);
					ca = sweet_th_ctx_index(&dummy); cb = sweet_th_ctx_index(&dummyB);
					CHECK(ca != cb && ca > 0 && cb > 0);
					sweet_tu_snapshot(ca, ta); sweet_tu_snapshot(cb, tb);
					CHECK(ta[1] == 31 && ta[0] == 0 && tb[0] == 55 && tb[1] == 0);
					/* a bind in ctx B must not sample ctx A's FBO; B has its own record */
					UB_ARM(); sweet_th_reset();
					BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002);                                   /* ctx A (dummy) */
					rp_calls = 0; cap_begin(); ((void (*)(void *, unsigned, unsigned))cy.disp[672])(&dummyB, 0x8D40, 7); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 0 && ob[0] == 0);
					mock_t = 1002; inj_prog = 0; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 1082; inj_vp[3] = 1226; inj_fbo = 7; inj_readfb = 7;
					((f_dr)cy.disp[65])(&dummyB, 4, 0, 3);                                          /* ctx B draws into its FBO 7 */
					rp_calls = 0; cap_begin(); ((void (*)(void *, unsigned, unsigned))cy.disp[672])(&dummyB, 0x8D40, 8); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 6 && strstr(ob, "unbind S fbo 7 vp 1082x1226 nonzero") != NULL);
					inj_fbo = 3; inj_readfb = 3;
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);              /* ctx A still has its own FBO 3 */
					CHECK(rp_calls == 6 && strstr(ob, "unbind S fbo 3 vp 1082x1226 nonzero") != NULL);
					/* overflow: the 17th and later contexts share slot 0 and every hook call attributed to it is counted */
					UB_ARM(); sweet_th_reset();
					for (k = 0; k < 16; k++) CHECK(sweet_th_ctx_index(&ovf[k]) == (int)k + 1);
					((f_at)cy.disp[342])(&dummyB, 0x84C1); ((f_bt)cy.disp[5])(&dummyB, 0xde1, 9);   /* overflow ctx 1 */
					((f_bt)cy.disp[5])(&dummy, 0xde1, 12);                                           /* overflow ctx 2: same shared slot 0, active unit 1 */
					sweet_tu_snapshot(0, ta); CHECK(ta[1] == 12 && ta[0] == 0);
					sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " unbind 0 skipped 0 ctxover 3") != NULL);
					sweet_th_reset();
				}
				/* F5: the longest prov line (4 units, 10-digit names / sizes / att, `+4 more`) is not truncated in the real sample path */
				{
					struct sweet_th_ev e; unsigned long mx; const char *pl;
					typedef void (*f_at)(void *, unsigned); typedef void (*f_bt)(void *, unsigned, unsigned);
					sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
					sweet_tu_reset(); sweet_th_reset(); sweet_ub_reset();
					for (k = 0; k < 8; k++) {
						unsigned nm = 4000000000u + (unsigned)k;
						((f_at)cy.disp[342])(cy.rctx, 0x84C0 + (unsigned)k); ((f_bt)cy.disp[5])(cy.rctx, 0xde1, nm);
						memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXSTORAGE; e.b = 1; e.c = 2147483647; e.d = 2147483647; sweet_th_record(nm, e);
						memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = 2147483647; sweet_th_record(nm, e);
					}
					rp_multi = 1; memcpy(rp_pats[0], rp_pat, 64); rp_calls = 0; inj_pbo = 0; inj_al = 4; inj_rbuf = 0x8CE0; fake_ret = 0x8cd5;
					cap_begin(); DRAW_B(cy, 2147483647, 1000); cap_end(ob, sizeof ob);
					pl = strstr(ob, "SWEETGL: PIXELS prov r0 fbo 2147483647 units: u0 tex 4000000000 2147483647x2147483647 att 2147483647 | u1 tex 4000000001");
					mx = pl ? (size_t)(strchr(pl, '\n') - pl) : 0;
					CHECK(pl != NULL && mx > 260 && mx < 320 && strstr(pl, " | u3 tex 4000000003 2147483647x2147483647 att 2147483647 | +4 more\n") != NULL);
					sweet_tu_reset(); sweet_th_reset();
				}
				/* ---- g113d: draw state line formatter, grid map, capdraws ---- */
				{
					struct sweet_px_state st;
					char kb[8];
					memset(&st, 0, sizeof st);
					st.prog = 7; st.flags = 1u | 2u; st.mode = 5; st.count = 100; st.blend = 2; st.bf[0] = 0x302; st.bf[1] = 0x303; st.bf[2] = 1; st.bf[3] = 0x304; st.eq[0] = 0x8006; st.eq[1] = 0x800A;
					st.mask[0] = 1; st.mask[3] = 5; st.scissor = 0; st.sbox[2] = 1125; st.sbox[3] = 2436; st.vp[2] = 1125; st.vp[3] = 2436; st.depth = 0;
					sweet_state_line(l, sizeof l, 3, 9, &st);
					CHECK(!strcmp(l, "SWEETGL: PIXELS state r3 fbo 9 prog 7 kind bf mode 0x5 count 100 blend 1 0x302/0x303/0x1/0x304 eq 0x8006/0x800a mask 1001 scissor 0 0,0,1125,2436 vp 0,0,1125,2436 depth 0"));
					sweet_census_kind_str(0, kb, sizeof kb); CHECK(!strcmp(kb, "o"));
					sweet_census_kind_str(31u, kb, sizeof kb); CHECK(!strcmp(kb, "bfclr"));
					sweet_census_kind_str(16u, kb, sizeof kb); CHECK(!strcmp(kb, "r"));
					sweet_census_kind_str(4u | 8u, kb, sizeof kb); CHECK(!strcmp(kb, "cl"));
				}
				{   /* grid classification: non-square, not multiples of 8, padded rows */
					static unsigned char gi[88 * 13];
					char map[8][9]; int nz, x, y;
					memset(gi, 0, sizeof gi);
					for (y = 0; y < 13; y++) memset(gi + y * 88 + 80, 0xff, 8);           /* row padding is never looked at */
					gi[0 * 88 + 4 * 4 + 3] = 1;                                              /* pixel (4,0), alpha only: x in [2,5) -> cell 1 */
					for (y = 3; y < 4; y++) for (x = 7; x < 10; x++) memset(gi + y * 88 + x * 4, 0xff, 4);   /* cell (3,2) = x[7,10) y[3,4) fully non-zero */
					for (y = 11; y < 13; y++) for (x = 17; x < 20; x++) memset(gi + y * 88 + x * 4, 1, 4);    /* cell (7,7) = x[17,20) y[11,13) ... */
					memset(gi + 12 * 88 + 19 * 4, 0, 4);                                                      /* ... with one zero pixel */
					nz = sweet_grid_classify(gi, 88, 20, 13, map);
					CHECK(nz == 3 && !strcmp(map[0], ".+......") && !strcmp(map[1], "........") && !strcmp(map[2], "...#....") && !strcmp(map[3], "........") && !strcmp(map[7], ".......+"));
					sweet_grid_line(l, sizeof l, 2, 3, 20, 13, 1, nz, map, 0);
					CHECK(!strcmp(l, "SWEETGL: PIXELS grid r2 fbo 3 vp 20x13 cells 3/64 map .+......|........|...#....|........|........|........|........|.......+ a5 0"));
					gi[0 * 88 + 5 * 4] = 7;   /* pixel (5,0) is in cell 2 (x[5,7)), one past the boundary */
					nz = sweet_grid_classify(gi, 88, 20, 13, map); CHECK(nz == 4 && !strcmp(map[0], ".++....."));
					memset(gi, 0xA5, sizeof gi); for (y = 0; y < 13; y++) memset(gi + y * 88 + 80, 0, 8);
					CHECK(!sweet_grid_written(gi, 88, 20, 13));                 /* only the pre-fill (padding differs but is ignored) */
					gi[12 * 88 + 79] = 0xA4; CHECK(sweet_grid_written(gi, 88, 20, 13));
					sweet_grid_line(l, sizeof l, 4, 3, 20, 13, 0, 0, map, 0); CHECK(!strcmp(l, "SWEETGL: PIXELS grid r4 fbo 3 vp 20x13 written 0"));
				}
				{   /* smallest viewports: 8x8 (one pixel per cell), 9x8 (last column cell 2 pixels wide), < 8 */
					static unsigned char g8[32 * 8], g9[36 * 8];
					char map[8][9]; int x, y;
					memset(g8, 0, sizeof g8);
					for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) if (((x + y) & 1) == 0) g8[y * 32 + x * 4 + 1] = 9;
					CHECK(sweet_grid_classify(g8, 32, 8, 8, map) == 32 && !strcmp(map[0], "#.#.#.#.") && !strcmp(map[1], ".#.#.#.#") && !strcmp(map[7], ".#.#.#.#"));
					memset(g9, 0, sizeof g9); g9[0 * 36 + 8 * 4] = 1;   /* pixel (8,0); cell 7 = x[7,9) (pixel 7 is zero) */
					CHECK(sweet_grid_classify(g9, 36, 9, 8, map) == 1 && !strcmp(map[0], ".......+") && !strcmp(map[1], "........"));
					CHECK(sweet_grid_classify(g8, 28, 7, 8, map) == -1 && sweet_grid_classify(g8, 32, 8, 7, map) == -1);
					/* review b6 F1: pre-fill pixels (0xA5A5A5A5) are neither zero nor data: a cell of only pre-fill is '?', in a mixed cell they are ignored */
					memset(g8, 0, sizeof g8); memset(g8 + 0 * 32 + 0 * 4, 0xA5, 4);                      /* pixel (0,0) = cell (0,0): all pre-fill */
					CHECK(sweet_grid_classify(g8, 32, 8, 8, map) == 0 && !strcmp(map[0], "?.......") && sweet_grid_sentinel(g8, 32, 8, 8) == 1);
					memset(g9, 0, sizeof g9); memset(g9 + 7 * 4, 0xA5, 4); g9[8 * 4] = 1;                  /* cell 7 = pixels 7 (pre-fill) and 8 (data) */
					CHECK(sweet_grid_classify(g9, 36, 9, 8, map) == 1 && !strcmp(map[0], ".......#"));
					memset(g9, 0, sizeof g9); memset(g9 + 8 * 4, 0xA5, 4);                                 /* cell 7 = pixel 7 (zero) and 8 (pre-fill): '.' */
					CHECK(sweet_grid_classify(g9, 36, 9, 8, map) == 0 && !strcmp(map[0], "........"));
					{ unsigned char one[4] = { 0xA5, 0xA5, 0xA5, 0xA4 }; CHECK(sweet_grid_sentinel(one, 4, 1, 1) == 0); }
				}
				{   /* capdraws formatter */
					struct sweet_capdraws cd; memset(&cd, 0, sizeof cd);
					cd.draws = 10; cd.kinds[0] = 2; cd.kinds[2] = 1; cd.kinds[3] = 1; cd.kinds[4] = 1; cd.kinds[5] = 6; cd.verts = 1056; cd.pfirst = 4; cd.plast = 0; cd.blendlast = 3;
					sweet_capdraws_line(l, sizeof l, 2, 3, &cd);
					CHECK(!strcmp(l, "SWEETGL: PIXELS capdraws r2 fbo 3 draws 10 kinds b:2 f:0 c:1 l:1 r:1 o:6 verts 1056 progs 4..0 blendlast 1 since-bind"));
				}
				{   /* capvp formatter */
					struct sweet_capdraws cd; memset(&cd, 0, sizeof cd);
					cd.fvp[0] = 1; cd.fvp[1] = 2; cd.fvp[2] = 3; cd.fvp[3] = 4; cd.fmode = 5; cd.fcount = 6; cd.lvp[0] = 7; cd.lvp[1] = 8; cd.lvp[2] = 9; cd.lvp[3] = 10; cd.lmode = 0xb; cd.lcount = 12;
					sweet_capvp_line(l, sizeof l, 2, 3, &cd);
					CHECK(!strcmp(l, "SWEETGL: PIXELS capvp r2 fbo 3 first vp 1,2,3,4 mode 0x5 count 6 last vp 7,8,9,10 mode 0xb count 12"));
				}
				{   /* capv8 formatter: lines of tags h / a / v / u, each < SWEET_CAPV_MAX */
					struct sweet_capv cv;
					char lc[800];
					int n1, i2, k2, t2;
					memset(&cv, 0, sizeof cv);
					cv.vw = 1082; cv.vh = 1226; cv.prog = 9; cv.mode = 4; cv.count = 48; cv.first = 1; cv.tex[0] = 25; cv.tex[3] = 31;
					cv.at[0].en = 1; cv.at[0].buf = 7; cv.at[0].sz = 2; cv.at[0].st = 16; cv.at[0].ty = 0x1406; cv.at[0].off = 8;
					cv.at[1].en = 1; cv.at[1].buf = 7; cv.at[1].sz = 2; cv.at[1].st = 16; cv.at[1].ty = 0x1406;
					cv.at[2].sz = 4; cv.at[2].ty = 0x1406; cv.at[2].off = 0x1234;   /* a client array: the pointer must not be printed */
					cv.vstate = SWEET_CAPV_V_VALUES; cv.vsz = 2; cv.nv = 4;
					for (i2 = 0; i2 < 4; i2++) { cv.v[i2][0] = 3.0f + 2.0f * (float)i2; cv.v[i2][1] = cv.v[i2][0] + 0.5f; }
					cv.nu = 3; cv.umore = 2;
					snprintf(cv.u[0].name, sizeof cv.u[0].name, "u_mvp"); cv.u[0].kind = SWEET_CAPV_U_FLOATS; cv.u[0].nf = 4; cv.u[0].f[0] = 1; cv.u[0].f[3] = 1;
					snprintf(cv.u[1].name, sizeof cv.u[1].name, "tex0"); cv.u[1].kind = SWEET_CAPV_U_UNIT; cv.u[1].unit = 2;
					snprintf(cv.u[2].name, sizeof cv.u[2].name, "a_very_long_uni"); cv.u[2].kind = SWEET_CAPV_U_NOLOC;
					n1 = sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'h', 0);
					CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 prog 9 mode 0x4 count 48 first 1 idx - | tex u0 25 u3 31") && n1 == (int)strlen(lc));
					sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'a', 0);
					CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 a a0 en 1 buf 7 sz 2 ty 0x1406 st 16 off 8, a1 en 1 buf 7 sz 2 ty 0x1406 st 16 off 0"));
					sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'a', 1);
					CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 a a2 en 0 buf 0 sz 4 ty 0x1406 st 0 off cli, a3 en 0 buf 0 sz 0 ty 0x0 st 0 off cli") && strstr(lc, "4660") == NULL && strstr(lc, "0x1234") == NULL);
					CHECK(sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'a', 2) == 0 && lc[0] == 0);
					sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'v', 0); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 v v0 3,3.5 v1 5,5.5"));
					sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'v', 1); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 v v2 7,7.5 v3 9,9.5"));
					CHECK(sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'v', 2) == 0);
					sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'u', 0); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 u u_mvp=1,0,0,1 tex0=unit2 a_very_long_uni=- +2"));
					CHECK(sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'u', 1) == 0 && sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'h', 1) == 0 && sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'x', 0) == 0);
					/* elements draw with the index header; only two vertices (nv 2) means no second v line */
					cv.hasidx = 1; cv.nidx = 4; cv.idx[0] = 0; cv.idx[1] = 1; cv.idx[2] = 2; cv.idx[3] = 3; cv.imin = 0; cv.imax = 3;
					sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'v', 0); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 2 v idx 0,1,2,3 min 0 max 3 v0 3,3.5 v1 5,5.5"));
					cv.nv = 2; CHECK(sweet_capv_line(lc, sizeof lc, 3, 2, &cv, 'v', 1) == 0); cv.nv = 4; cv.hasidx = 0;
					/* `-` fields, elements draw, v states (one short word each), type-only uniform, a long name cut to 15 chars */
					memset(&cv, 0, sizeof cv);
					cv.mode = 5; cv.count = 6; cv.first = -1; cv.idxtype = 0x1403;
					sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'h', 0);
					CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 5 vp 0x0 draw 1 prog 0 mode 0x5 count 6 first - idx 0x1403 | tex -"));
					sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'a', 0);
					CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 5 vp 0x0 draw 1 a a0 en 0 buf 0 sz 0 ty 0x0 st 0 off cli, a1 en 0 buf 0 sz 0 ty 0x0 st 0 off cli"));
					sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'v', 0); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 5 vp 0x0 draw 1 v -"));
					sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'u', 0); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 fbo 5 vp 0x0 draw 1 u -"));
					cv.first = 0; sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'h', 0); CHECK(strstr(lc, " count 6 first 0 idx 0x1403 | tex -") != NULL); cv.first = -1;   /* first 0 is a real DrawArrays first, not `-` */
					{
						static const struct { int st; const char *w; } vs[] = { { SWEET_CAPV_V_OOB, "oob" }, { SWEET_CAPV_V_MAPPED, "mapped" }, { SWEET_CAPV_V_IDXBUF, "idxbuf" }, { SWEET_CAPV_V_IDXRANGE, "idxrange" },
						    { SWEET_CAPV_V_CLI0, "cli0" }, { SWEET_CAPV_V_TYPE, "type" }, { SWEET_CAPV_V_DIS, "dis" }, { SWEET_CAPV_V_NOFIRST, "nofirst" }, { SWEET_CAPV_V_IDXQ, "idxq" }, { SWEET_CAPV_V_INST, "inst" }, { SWEET_CAPV_V_NONE, "-" }, { 99, "-" } };
						for (t2 = 0; t2 < (int)(sizeof vs / sizeof vs[0]); t2++) {
							char want[96];
							cv.vstate = vs[t2].st; sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'v', 0);
							snprintf(want, sizeof want, "SWEETGL: PIXELS capv8 fbo 5 vp 0x0 draw 1 v %s", vs[t2].w);
							CHECK(!strcmp(lc, want));
						}
						cv.vstate = SWEET_CAPV_V_NONE;
					}
					cv.nu = 2; memcpy(cv.u[0].name, "0123456789abcdefX", sizeof cv.u[0].name);   /* 16 bytes, no NUL: the formatter still prints at most 15 */
					cv.u[0].kind = SWEET_CAPV_U_TYPE; cv.u[0].type = 0x8b56;
					snprintf(cv.u[1].name, sizeof cv.u[1].name, "f"); cv.u[1].kind = SWEET_CAPV_U_FLOATS; cv.u[1].nf = 1; cv.u[1].f[0] = -0.5f;
					sweet_capv_line(lc, sizeof lc, 5, 1, &cv, 'u', 0); CHECK(strstr(lc, " u 0123456789abcde=t0x8b56 f=-0.5") != NULL && lc[strlen(lc) - 1] == '5');
					/* six 16-float uniforms of ugly values: split over several u lines (mat4 chunked by 4 floats), each line short, no value lost */
					memset(&cv, 0, sizeof cv);
					cv.first = 0; cv.nu = SWEET_CAPV_U; cv.umore = 1;
					for (i2 = 0; i2 < SWEET_CAPV_U; i2++) {
						snprintf(cv.u[i2].name, sizeof cv.u[i2].name, "uniform_%c", 'a' + i2); cv.u[i2].kind = SWEET_CAPV_U_FLOATS; cv.u[i2].nf = 16;
						for (k2 = 0; k2 < 16; k2++) cv.u[i2].f[k2] = -1.23456789e-5f * (float)(1 + k2 + i2);
					}
					{
						char all[4096];
						int nl = 0;
						all[0] = 0;
						for (k2 = 0; k2 < 32; k2++) { n1 = sweet_capv_line(lc, sizeof lc, 3, 1, &cv, 'u', k2); if (n1 <= 0) break; nl++; CHECK(strlen(lc) < SWEET_CAPV_MAX && strstr(lc, "~cut") == NULL && strncmp(lc, "SWEETGL: PIXELS capv8 fbo 3 vp 0x0 draw 1 u ", 44) == 0); strcat(all, lc); }
						CHECK(nl >= 3 && nl <= 14 && strstr(all, " uniform_a=-1.23457e-05,") != NULL && strstr(all, " uniform_a.12=") != NULL && strstr(all, " uniform_f.12=-") != NULL && strstr(all, " +1") != NULL);
					}
					/* worst case for every tag at once, huge ints and ugly floats: every line < SWEET_CAPV_MAX (a cut is marked) and the NUL is inside */
					cv.vstate = SWEET_CAPV_V_VALUES; cv.vsz = 4; cv.nv = 4; cv.hasidx = 1; cv.nidx = 4; cv.imin = 4000000000u; cv.imax = 4000000000u;
					for (i2 = 0; i2 < 4; i2++) cv.idx[i2] = 4000000000u;
					for (i2 = 0; i2 < 4; i2++) for (k2 = 0; k2 < 4; k2++) cv.v[i2][k2] = -1.23456789e-5f * (float)(1 + i2 * 4 + k2);
					for (i2 = 0; i2 < 8; i2++) cv.tex[i2] = 4000000000u + (unsigned)i2;
					for (i2 = 0; i2 < 4; i2++) { cv.at[i2].en = -2000000000; cv.at[i2].buf = 2000000000; cv.at[i2].sz = -2000000000; cv.at[i2].st = -2000000000; cv.at[i2].ty = 0xffffffffu; cv.at[i2].off = 18446744073709551615ul; }
					cv.prog = -2000000000; cv.count = -2000000000; cv.mode = 0xffffffffu; cv.first = 2000000000; cv.idxtype = 0xffffffffu;
					cv.vw = -2000000000; cv.vh = -2000000000;
					{
						static const char tg[] = "hvau";
						int nlines = 0, cuts = 0;
						for (t2 = 0; t2 < 4; t2++) for (k2 = 0; k2 < 12; k2++) {
							n1 = sweet_capv_line(lc, sizeof lc, -2000000000, -2000000000, &cv, tg[t2], k2);
							if (n1 <= 0) { CHECK(lc[0] == 0); continue; }
							nlines++; if (strstr(lc, " ~cut") != NULL) cuts++;
							CHECK(n1 == (int)strlen(lc) && strlen(lc) < SWEET_CAPV_MAX && strncmp(lc, "SWEETGL: PIXELS capv8 fbo -2000000000 vp -2000000000x-2000000000 draw -2000000000 ", 82) == 0);
						}
						CHECK(nlines >= 6 && cuts >= 1);   /* the worst-case header does not fit: it is cut and marked, never overlong */
					}
					/* a short buffer: always NUL terminated inside the buffer, never an overflow (heap copy for ASan) */
					for (i2 = 0; i2 < 120; i2 += 7) for (t2 = 0; t2 < 4; t2++) { char *sb = (char *)malloc((size_t)i2 + 1); sweet_capv_line(sb, (size_t)i2 + 1, 3, 1, &cv, "hvau"[t2], 0); CHECK(strlen(sb) <= (size_t)i2); free(sb); }
					CHECK(sweet_capv_line(lc, 0, 3, 1, &cv, 'h', 0) == 0);
					/* skip and cap lines */
					sweet_capv_cap_line(lc, sizeof lc); CHECK(!strcmp(lc, "SWEETGL: PIXELS capv8 cap"));
				}
				{   /* capdraws through the hooks: kinds from the program tags, counts of all five draw entries, blend tracked from Enable / Disable, reset at the bind */
					static const char *const csrc[1] = { "uniform mat4 color_matrix;" }, *const lsrc[1] = { "float tlum;" }, *const rsrc[1] = { "uniform sampler2DRect t;" };
					typedef void (*f_de2)(void *, unsigned, int, unsigned, const void *);
					typedef void (*f_drr2)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *);
					typedef void (*f_dai2)(void *, unsigned, int, int, int);
					typedef void (*f_dei2)(void *, unsigned, int, unsigned, const void *, int);
					typedef void (*f_en)(void *, unsigned);
#define DRAWP(p_, cnt_) do { mock_t = 1002; inj_prog = (p_); inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 1082; inj_vp[3] = 1226; inj_fbo = 3; inj_readfb = 3; ((f_dr)cy.disp[65])(cy.rctx, 4, 0, (cnt_)); } while (0)
					fake_ret = 9; ((f_cprog)cx.disp[597])(cx.rctx); fake_ret = 10; ((f_cprog)cx.disp[597])(cx.rctx); fake_ret = 11; ((f_cprog)cx.disp[597])(cx.rctx); fake_ret = 0x8cd5;
					((f_ss)cx.disp[595])(cx.rctx, 95, 1, csrc, NULL); ((f_as)cx.disp[598])(cx.rctx, 9, 95);
					((f_ss)cx.disp[595])(cx.rctx, 96, 1, lsrc, NULL); ((f_as)cx.disp[598])(cx.rctx, 10, 96);
					((f_ss)cx.disp[595])(cx.rctx, 97, 1, rsrc, NULL); ((f_as)cx.disp[598])(cx.rctx, 11, 97);
					UB_ARM();
					BIND_FB(3, 1002);
					DRAWP(4, 3); DRAWP(4, 5);
					((f_en)cy.disp[72])(cy.rctx, 0x0BE2);
					DRAWP(9, 7); DRAWP(10, 11); DRAWP(11, 13);
					((f_en)cy.disp[63])(cy.rctx, 0x0BE2);
					DRAWP(0, 17);
					mock_t = 1002; inj_prog = 0;
					((f_de2)cy.disp[67])(cy.rctx, 4, 100, 0x1403, NULL);
					((f_drr2)cy.disp[405])(cy.rctx, 4, 0, 9, 200, 0x1403, NULL);
					((f_dai2)cy.disp[762])(cy.rctx, 4, 0, 300, 2);
					((f_dei2)cy.disp[763])(cy.rctx, 4, 400, 0x1403, NULL, 2);
					cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					pp = strstr(ob, "SWEETGL: PIXELS grid r1 fbo 3 vp 1082x1226 written 0\n"); CHECK(pp != NULL);
					CHECK(pp != NULL && !strcmp(pp, "SWEETGL: PIXELS grid r1 fbo 3 vp 1082x1226 written 0\nSWEETGL: PIXELS capdraws r1 fbo 3 draws 10 kinds b:2 f:0 c:1 l:1 r:1 o:5 verts 1056 progs 4..0 blendlast 0 since-bind\nSWEETGL: PIXELS capvp r1 fbo 3 first vp 0,0,1082,1226 mode 0x4 count 3 last vp 0,0,1082,1226 mode 0x4 count 400\n"));
					/* reset at the bind, blend enabled for the last draw */
					BIND_FB(3, 1002);
					((f_en)cy.disp[72])(cy.rctx, 0x0BE2); ((f_en)cy.disp[72])(cy.rctx, 0x0B71 /* depth test: not blend */);
					DRAWP(4, 6);
					cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(strstr(ob, "SWEETGL: PIXELS capdraws r1 fbo 3 draws 1 kinds b:1 f:0 c:0 l:0 r:0 o:0 verts 6 progs 4..4 blendlast 1 since-bind\nSWEETGL: PIXELS capvp r1 fbo 3 first vp 0,0,1082,1226 mode 0x4 count 6 last vp 0,0,1082,1226 mode 0x4 count 6\n") != NULL);
					/* two draws with different viewports and modes: first and last differ */
					UB_ARM(); fake_ret = 0x8cd5;
					BIND_FB(3, 1002);
					mock_t = 1002; inj_prog = 0; inj_fbo = 3; inj_readfb = 3;
					inj_vp[0] = 1; inj_vp[1] = 2; inj_vp[2] = 3; inj_vp[3] = 4; ((f_dr)cy.disp[65])(cy.rctx, 5, 0, 7);
					inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 1082; inj_vp[3] = 1226; ((f_dr)cy.disp[65])(cy.rctx, 6, 0, 9);
					cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(strstr(ob, "SWEETGL: PIXELS capvp r1 fbo 3 first vp 1,2,3,4 mode 0x5 count 7 last vp 0,0,1082,1226 mode 0x6 count 9\n") != NULL);
					fake_ret = 0;
				}
				{   /* capv8 through the draw hooks: eligibility, budget, the exact GL calls, the always-run restore of the COPY_READ_BUFFER binding, the errors */
					typedef void (*cv_da)(void *, unsigned, int, int);
					typedef void (*cv_de)(void *, unsigned, int, unsigned, const void *);
					typedef void (*cv_ss)(void *, unsigned, int, const char *const *, const int *);
					typedef void (*cv_as)(void *, unsigned, unsigned);
					typedef unsigned (*cv_cp)(void *);
					static const char *const allow[] = { "_mesa_GetIntegerv", "_mesa_DrawArrays", "_mesa_DrawElements", "_mesa_GetVertexAttribiv", "_mesa_GetVertexAttribPointerv",
					    "_mesa_BindBuffer", "_mesa_GetBufferParameteriv", "_mesa_GetBufferSubData", "_mesa_GetProgramiv", "_mesa_GetActiveUniform", "_mesa_GetUniformLocation", "_mesa_GetUniformfv", "_mesa_GetUniformiv" };
					static char cb[16384];
					static const char CV_A[] =
					    "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 1 prog 90 mode 0x4 count 48 first 1 idx - | tex u0 25\n"
					    "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 1 a a0 en 1 buf 7 sz 2 ty 0x1406 st 16 off 8, a1 en 1 buf 7 sz 2 ty 0x1406 st 16 off 0\n"
					    "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 1 a a2 en 0 buf 0 sz 4 ty 0x1406 st 0 off cli, a3 en 0 buf 0 sz 0 ty 0x0 st 0 off cli\n"
					    "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 1 v v0 3,3.5 v1 5,5.5\n"
					    "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 1 v v2 7,7.5 v3 9,9.5\n"
					    "SWEETGL: PIXELS capv8 fbo 3 vp 1082x1226 draw 1 u u_mvp=1,0,0,0 u_mvp.4=0,1,0,0 u_mvp.8=0,0,1,0 u_mvp.12=0,0,0,1 tex0=unit2 tint=0.5,0.25,1,1 flag=t0x8b56 blk.x=- a_very_long_uni=2.5 +1\n"
					    "SWEETGL: PIXELS prov r0 fbo 3 units: u0 tex 25 1082x1226 att 3\n";
					struct sweet_th_ev e;
					unsigned q, w, nbad, dq, lastsub, lastbind, nsub, nbind;
					int b, dd;
#define CV_SETUP() do { UB_ARM(); fake_ret = 0; inj_cv = 1; inj_crb = 5; inj_link = 1; inj_bsz = 160; inj_bmapped = 0; inj_nu = 7; inj_ul = 25; \
	memset(inj_va, 0, sizeof inj_va); memset(inj_vaoff, 0, sizeof inj_vaoff); memset(inj_un, 0, sizeof inj_un); \
	inj_va[0][0] = 1; inj_va[0][1] = 7; inj_va[0][2] = 2; inj_va[0][3] = 0x1406; inj_va[0][4] = 16; inj_vaoff[0] = 8; \
	inj_va[1][0] = 1; inj_va[1][1] = 7; inj_va[1][2] = 2; inj_va[1][3] = 0x1406; inj_va[1][4] = 16; \
	inj_va[2][2] = 4; inj_va[2][3] = 0x1406; inj_vaoff[2] = 0x1234; inj_eb = 0; \
	for (q = 0; q < 40; q++) inj_vb[q] = 0.5f * (float)q; \
	inj_un[0].name = "u_mvp"; inj_un[0].type = 0x8B5C; inj_un[0].f[0] = inj_un[0].f[5] = inj_un[0].f[10] = inj_un[0].f[15] = 1; \
	inj_un[1].name = "tex0"; inj_un[1].type = 0x8B5E; inj_un[1].i = 2; \
	inj_un[2].name = "tint"; inj_un[2].type = 0x8B52; inj_un[2].f[0] = 0.5f; inj_un[2].f[1] = 0.25f; inj_un[2].f[2] = 1; inj_un[2].f[3] = 1; \
	inj_un[3].name = "flag"; inj_un[3].type = 0x8B56; inj_un[4].name = "blk.x"; inj_un[4].type = 0x1406; inj_un[4].noloc = 1; \
	inj_un[5].name = "a_very_long_uniform_name"; inj_un[5].type = 0x1406; inj_un[5].f[0] = 2.5f; inj_un[6].name = "extra"; inj_un[6].type = 0x1406; \
	sweet_th_reset(); memset(&e, 0, sizeof e); e.kind = SWEET_TH_TEXSTORAGE; e.b = 1; e.c = 1082; e.d = 1226; sweet_th_record(25, e); \
	memset(&e, 0, sizeof e); e.kind = SWEET_TH_FBOTEX; e.c = 3; sweet_th_record(25, e); ((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0xde1, 25); \
	BIND_FB(3, 1002); mock_t = 1002; inj_prog = 90; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 1082; inj_vp[3] = 1226; inj_fbo = 3; inj_readfb = 3; } while (0)
#define CV_RESET() do { cv_n = 0; bb_n = 0; bs_n = 0; } while (0)
					/* A: DrawArrays first 1 count 48, VBO 7 holds the vertices */
					CV_SETUP(); CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(!strcmp(cb, CV_A));
					nbad = 0; dq = 0; nsub = nbind = 0; lastsub = lastbind = 0;
					for (q = 0; q < cv_n; q++) {
						int okn = 0;
						for (w = 0; w < sizeof allow / sizeof allow[0]; w++) if (!strcmp(cv_log[q], allow[w])) okn = 1;
						if (!okn) nbad++;
						if (!strcmp(cv_log[q], "_mesa_DrawArrays")) dq = q;
						if (!strcmp(cv_log[q], "_mesa_GetBufferSubData")) { nsub++; lastsub = q; }
						if (!strcmp(cv_log[q], "_mesa_BindBuffer")) { nbind++; lastbind = q; }
						if (!strcmp(cv_log[q], "_mesa_GetError")) nbad++;   /* the hook never calls glGetError: the app's error state is untouched */
					}
					CHECK(nbad == 0 && cv_n < 256 && dq > 0 && !strcmp(cv_log[dq + 1], "_mesa_GetVertexAttribiv"));   /* queries start right after the draw; nothing outside the allowlist, no glGetError */
					CHECK(nbind == 2 && bb_n == 2 && bb_log[0][0] == 0x8F36 && bb_log[0][1] == 7 && bb_log[1][0] == 0x8F36 && bb_log[1][1] == 5 && lastbind > lastsub);   /* bind the VBO, read, restore the old binding LAST */
					CHECK(nsub == 4 && bs_n == 4 && bs_log[0][0] == 24 && bs_log[1][0] == 40 && bs_log[2][0] == 56 && bs_log[3][0] == 72 && bs_log[0][1] == 8 && bs_log[3][1] == 8);   /* off 8 + (first 1 + i) * stride 16, 8 bytes each */
					/* B: draw 2: the VBO is too small for the 4 vertices -> `v oob`, no read, the binding is still restored */
					inj_bsz = 79; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 2 ") != NULL && strstr(cb, " draw 2 v oob\n") != NULL && strstr(cb, " draw 2 u u_mvp=") != NULL && bs_n == 0 && bb_n == 2 && bb_log[1][1] == 5);
					inj_bsz = 80; CV_RESET();   /* 24 + 3*16 + 8 = 80 fits exactly (draw 3) */
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 3 ") != NULL && strstr(cb, " draw 3 v v0 ") != NULL && strstr(cb, " v oob") == NULL && bs_n == 4 && bb_n == 2);
					/* C: draw 4 elements (first `-`, idx type, `v -`, no VBO bind) */
					CV_RESET();
					cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 100, 0x1403, NULL); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 4 prog 90 mode 0x4 count 100 first - idx 0x1403 | tex u0 25\n") != NULL && strstr(cb, " draw 4 a a0 en 1 ") != NULL && strstr(cb, " draw 4 v -\n") != NULL && strstr(cb, " draw 4 u u_mvp=") != NULL && bb_n == 0 && bs_n == 0);   /* NULL indices: nothing to read */
					/* D: the 5th draw since the bind is not eligible: silence (the per-bind budget is the draw window) */
					CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);   /* only the census draw hook's own queries and the draw */
					/* E: mapped buffer -> `v mapped`; an unwritten size query (-1) leaves `v -`; both restore */
					CV_SETUP(); inj_bmapped = 1; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " v mapped\n") != NULL && strstr(cb, " u u_mvp=") != NULL && bs_n == 0 && bb_n == 2 && bb_log[1][1] == 5);
					/* H: a program that is not linked / has no uniforms prints `u -`; no VBO attribute means no reads */
					CV_SETUP(); inj_link = 0; memset(inj_va, 0, sizeof inj_va); CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " v dis\n") != NULL && strstr(cb, " u -\n") != NULL && strstr(cb, "err") == NULL && bb_n == 0 && bs_n == 0 && count_seq("_mesa_GetActiveUniform") == 0);
					/* I: not eligible: small viewport, FBO 0 (never bound), census without a schedule, a non-`o` program */
					CV_SETUP(); inj_vp[2] = 999; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					UB_ARM(); fake_ret = 0; inj_cv = 1; CV_RESET(); mock_t = 1002; inj_prog = 90; inj_vp[2] = 1082; inj_vp[3] = 1226;
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					sweet_px_reset(); sweet_px_set_clock(mock_clk); setenv("SWEETGL_CENSUS", "1", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS"); sweet_ub_reset(); fake_ret = 0;
					BIND_FB(3, 1002); inj_prog = 90; inj_vp[2] = 1082; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					CV_SETUP(); fake_ret = 91; ((cv_cp)cy.disp[597])(cy.rctx); fake_ret = 0;
					{ static const char *const rs[1] = { "uniform sampler2DRect t;" }; ((cv_ss)cy.disp[595])(cy.rctx, 97, 1, rs, NULL); ((cv_as)cy.disp[598])(cy.rctx, 91, 97); }
					inj_prog = 91; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					/* K: the other elements / instanced entries record their index type (`first -`), DrawArrays first 0 reads vertices */
					CV_SETUP(); CV_RESET();
					{
						typedef void (*cv_dr)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *);
						typedef void (*cv_dai)(void *, unsigned, int, int, int);
						typedef void (*cv_dei)(void *, unsigned, int, unsigned, const void *, int);
						cap_begin();
						((cv_dr)cy.disp[405])(cy.rctx, 5, 0, 9, 200, 0x1405, NULL);
						((cv_dei)cy.disp[763])(cy.rctx, 6, 300, 0x1401, NULL, 2);
						((cv_dai)cy.disp[762])(cy.rctx, 7, 0, 400, 2);
						cap_end(cb, sizeof cb);
						CHECK(strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 1 prog 90 mode 0x5 count 200 first - idx 0x1405 | tex u0 25\n") != NULL && strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 2 prog 90 mode 0x6 count 300 first - idx 0x1401 | ") != NULL &&
						      strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 3 prog 90 mode 0x7 count 400 first - idx - | ") != NULL && strstr(cb, " draw 3 v inst\n") != NULL && strstr(cb, " draw 2 v inst\n") != NULL && bb_n == 0 && bs_n == 0);   /* instanced draws: `v inst`, nothing read (F1); DrawArraysInstanced does not pass first on (brief: DrawArrays only) */
					}
					/* L: vertex-read gates, one setup per variant: attribute 0 state / count / first -> expected VBO binds (2 = bind + restore) and read offsets */
					{
						static const struct { int en, buf, sz, ty, st, count, first, binds, nsub; unsigned long off0; const char *w; } var[] = {
							{ 1, 7, 2, 0x1406, 16, 48, 1, 2, 4, 24, NULL },   /* baseline: off 8 + 1 * 16 */
							{ 1, 7, 2, 0x1406, 16, 48, 0, 2, 4, 8, NULL },    /* first 0 is a DrawArrays too */
							{ 1, 7, 2, 0x1406, 0, 48, 1, 2, 4, 16, NULL },    /* stride 0 = tightly packed: element size 8 -> 8 + 1 * 8 */
							{ 1, 7, 4, 0x1406, 0, 48, 0, 2, 4, 8, NULL },     /* size 4, tight: element 16 bytes */
							{ 1, 7, 3, 0x1406, 16, 2, 1, 2, 2, 24, NULL },    /* count 2: two vertices only */
							{ 0, 7, 2, 0x1406, 16, 48, 1, 0, 0, 0, "dis" },   /* disabled array */
							{ 1, 7, 2, 0x1401, 16, 48, 1, 0, 0, 0, "type" },  /* not FLOAT */
							{ 1, 7, 1, 0x1406, 16, 48, 1, 0, 0, 0, "type" },  /* size 1 */
							{ 1, 7, 5, 0x1406, 16, 48, 1, 0, 0, 0, "type" },  /* size 5 (does not exist) */
							{ 1, 7, 2, 0x1406, -1, 48, 1, 0, 0, 0, "type" },  /* a stride query that wrote nothing */
							{ 1, 7, 2, 0x1406, 16, 0, 1, 0, 0, 0, "-" },      /* count 0 */
						};
						for (w = 0; w < sizeof var / sizeof var[0]; w++) {
							CV_SETUP();
							inj_va[0][0] = var[w].en; inj_va[0][1] = var[w].buf; inj_va[0][2] = var[w].sz; inj_va[0][3] = var[w].ty; inj_va[0][4] = var[w].st; inj_bsz = 160; CV_RESET();
							cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, var[w].first, var[w].count); cap_end(cb, sizeof cb);
							CHECK(bb_n == (unsigned)var[w].binds && bs_n == (unsigned)var[w].nsub && (!var[w].nsub || bs_log[0][0] == var[w].off0) && (!var[w].binds || (bb_log[1][1] == 5 && bb_log[0][1] == 7)));
							{ char want[32]; snprintf(want, sizeof want, " v %s\n", var[w].w ? var[w].w : ""); CHECK(var[w].nsub ? strstr(cb, " v v0 ") != NULL : strstr(cb, want) != NULL); }
							CHECK(var[w].nsub != 2 || (strstr(cb, " v1 ") != NULL && strstr(cb, " v2 ") == NULL));
							CHECK(strstr(cb, "4660") == NULL);
						}
					}
					/* M: glGetActiveUniform's maxLength is the program's longest name, capped at 64 (the name buffer is 64 bytes) */
					CV_SETUP(); inj_ul = 100; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cv_ml == 64);
					CV_SETUP(); inj_ul = 25; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cv_ml == 25);
					CV_SETUP(); inj_ul = 0; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " u -\n") != NULL);
					/* N: queries that wrote nothing (-1) never turn into a read; the binding is still restored when the VBO was bound */
					CV_SETUP(); inj_crb = -1; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(bb_n == 0 && bs_n == 0 && strstr(cb, " v -\n") != NULL && strstr(cb, " u u_mvp=") != NULL);   /* the old binding is unknown: the binding is not touched */
					CV_SETUP(); inj_bsz = -1; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(bb_n == 2 && bs_n == 0 && bb_log[1][1] == 5 && strstr(cb, " v -\n") != NULL && strstr(cb, " u u_mvp=") != NULL);
					CV_SETUP(); inj_bmapped = -1; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(bb_n == 2 && bs_n == 0 && bb_log[1][1] == 5 && strstr(cb, " v -\n") != NULL && strstr(cb, " u u_mvp=") != NULL);
					CV_SETUP(); inj_crb = 0; CV_RESET();   /* nothing was bound before: the restore unbinds */
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(bb_n == 2 && bb_log[0][1] == 7 && bb_log[1][1] == 0 && bs_n == 4);
					/* O: viewports below 1000 px wide (class 1: 200x200, class 2: 800x600) into a non-zero FBO are not logged */
					CV_SETUP(); inj_vp[2] = 200; inj_vp[3] = 200; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					CV_SETUP(); inj_vp[2] = 800; inj_vp[3] = 600; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					/* O2: tall but narrow viewports (class 1 / 2 by width, height passes the gate) are not logged either */
					CV_SETUP(); inj_vp[2] = 200; inj_vp[3] = 1200; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					CV_SETUP(); inj_vp[2] = 800; inj_vp[3] = 1200; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					/* Q: wide but short viewports (the 1127x149 / 1082x149 FBOs) are below GLI_CAPV_MIN_H: no line; 1000 high is the first logged height */
					CV_SETUP(); inj_vp[2] = 1082; inj_vp[3] = 149; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					CV_SETUP(); inj_vp[2] = 1127; inj_vp[3] = 999; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					CV_SETUP(); inj_vp[2] = 1000; inj_vp[3] = 1000; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "capv8 fbo 3 vp 1000x1000 draw 1 ") != NULL);
					/* P: an attribute offset beyond 1 << 40 (app-controlled) is `v oob` with no read; no signed overflow in the range arithmetic (UBSan build) */
					CV_SETUP(); inj_vaoff[0] = ~0UL >> 1; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " v oob\n") != NULL && strstr(cb, " u u_mvp=") != NULL && bs_n == 0 && bb_n == 2 && bb_log[1][1] == 5);
					/* R: g113g [obs]: the screen-size compositor FBO (viewport exactly 1125x2436) is not eligible; a neighbour size still is */
					CV_SETUP(); inj_vp[2] = 1125; inj_vp[3] = 2436; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(cb[0] == 0 && cv_n == 3);
					CV_SETUP(); inj_vp[2] = 1125; inj_vp[3] = 2437; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "capv8 fbo 3 vp 1125x2437 draw 1 ") != NULL);
					CV_SETUP(); inj_vp[2] = 1125; inj_vp[3] = 2436; CV_RESET();   /* skipped draws do not spend the per-bind or process budget: the same bind still logs a capture draw next */
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); inj_vp[2] = 1082; inj_vp[3] = 1226; ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 2 prog 90 mode 0x4 count 48 first 1 ") != NULL && strstr(cb, "vp 1125x2436") == NULL);
					/* R2 (review capv8-r1 F4): the window counts eligible draws only: 6 skipped-size draws first, then 5 capture-size draws of the same bind: draws 7..10 are logged, the 5th eligible (draw 11) is not */
					CV_SETUP(); inj_vp[2] = 1125; inj_vp[3] = 2436; CV_RESET();
					cap_begin();
					for (q = 0; q < 6; q++) ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48);
					inj_vp[2] = 1082; inj_vp[3] = 1226;
					for (q = 0; q < 5; q++) ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48);
					cap_end(cb, sizeof cb);
					CHECK(strstr(cb, "vp 1125x2436") == NULL && strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 6 prog") == NULL && strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 7 prog 90") != NULL &&
					      strstr(cb, "capv8 fbo 3 vp 1082x1226 draw 10 prog 90") != NULL && strstr(cb, " draw 11 ") == NULL && strstr(cb, "capv8 cap") == NULL);
					/* S: client arrays: real heap memory sized exactly to what may be read (ASan catches an over-read); no GL call reads it and the pointer is never printed */
					{
						typedef void (*cv_dr)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *);
						typedef void (*cv_dei)(void *, unsigned, int, unsigned, const void *, int);
						float *cva = (float *)malloc(72);                 /* DrawArrays first 1, 4 vertices of 2 floats, stride 16: bytes 16..71 */
						float *cve = (float *)malloc(88);                 /* indices up to 5: 5 * 16 + 8 */
						unsigned short *i16 = (unsigned short *)malloc(12), *i64 = (unsigned short *)malloc(128);
						unsigned char *i8 = (unsigned char *)malloc(3);
						unsigned *i32 = (unsigned *)malloc(12);
						char pbuf[40];
						int t;
						for (t = 0; t < 18; t++) cva[t] = ((t / 4) + 1) * 10.0f + ((t % 4) == 1 ? 0.5f : 0.0f);
						for (t = 0; t < 22; t++) cve[t] = ((t / 4) + 1) * 10.0f + ((t % 4) == 1 ? 0.5f : 0.0f);
						i16[0] = 5; i16[1] = 1; i16[2] = 2; i16[3] = 5; i16[4] = 1; i16[5] = 2;
						for (t = 0; t < 64; t++) i64[t] = (unsigned short)(t % 7);
						i8[0] = 0; i8[1] = 1; i8[2] = 2;
						i32[0] = 70000; i32[1] = 1; i32[2] = 2;
						/* S1: DrawArrays first 1 count 48 on a client array: vertices 1..4, no buffer bind, no GetBufferSubData, pointer not printed */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cva; CV_RESET();
						cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
						snprintf(pbuf, sizeof pbuf, "%lu", (unsigned long)(uintptr_t)cva);
						CHECK(strstr(cb, " draw 1 v v0 20,20.5 v1 30,30.5\n") != NULL && strstr(cb, " draw 1 v v2 40,40.5 v3 50,50.5\n") != NULL && strstr(cb, " a0 en 1 buf 0 sz 2 ty 0x1406 st 16 off cli,") != NULL && strstr(cb, pbuf) == NULL && bb_n == 0 && bs_n == 0);
						/* S1b: tight stride 0 = size * 4 = 8 bytes (heap sized for it); a client DrawArrays with count 2 reads two vertices only */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 0; inj_vaoff[0] = (unsigned long)(uintptr_t)cva; CV_RESET();
						cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 2); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v v0 10,10 v1 20,20.5\n") != NULL);
						CHECK(strstr(cb, " v v2 ") == NULL && bb_n == 0);
						/* S2: DrawElements u16, count 6, client indices: header with the first indices, min / max, then the vertices of the first 4 indices */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1403, i16); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 5,1,2,5 min 1 max 5 v0 60,60.5 v1 20,20.5\n") != NULL && strstr(cb, " draw 1 v v2 30,30.5 v3 60,60.5\n") != NULL && bb_n == 0 && bs_n == 0);
						/* S2b: count 100 reads only the first 64 indices (the heap holds exactly 64) */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 100, 0x1403, i64); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 0,1,2,3 min 0 max 6 v0 10,10.5 v1 20,20.5\n") != NULL);
						/* S2c: u8 with count 3 (three vertices) and the instanced element hook (`v inst`) */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 3, 0x1401, i8); ((cv_dei)cy.disp[763])(cy.rctx, 4, 3, 0x1401, i8, 2); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 0,1,2 min 0 max 2 v0 10,10.5 v1 20,20.5\n") != NULL && strstr(cb, " draw 1 v v2 30,30.5\n") != NULL &&
						      strstr(cb, " draw 2 v inst\n") != NULL);   /* the instanced element hook reads nothing (F1) */
						/* S2d: DrawRangeElements, u32 index above 65535 -> `v idxrange`, no read */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
						cap_begin(); ((cv_dr)cy.disp[405])(cy.rctx, 4, 0, 70000, 3, 0x1405, i32); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 70000,1,2 min 1 max 70000 idxrange\n") != NULL && bb_n == 0 && bs_n == 0);   /* F3: the computed index header precedes the reason word */
						/* S2e: an element buffer is bound (indices is an offset): `v idxbuf`, the indices pointer is not dereferenced (offset 4 would fault) */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; inj_eb = 9; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1403, (const void *)(uintptr_t)4); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idxbuf\n") != NULL && bb_n == 0 && bs_n == 0);
						/* S2f: NULL indices -> `v -`; the element-buffer query that wrote nothing -> `v idxq` */
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1403, NULL); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v -\n") != NULL);
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; inj_eb = -1; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1403, i16); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idxq\n") != NULL);
						/* S3: client array with a NULL pointer -> `v cli0` (nothing dereferenced) */
						CV_SETUP(); inj_va[0][1] = 0; inj_vaoff[0] = 0; CV_RESET();
						cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v cli0\n") != NULL && bb_n == 0);
						/* S4: a VBO attribute with client indices: the index list drives the VBO reads (index 5 -> 8 + 5 * 16 = 88; the VBO size must hold 96) */
						CV_SETUP(); inj_bsz = 96; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1403, i16); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 5,1,2,5 min 1 max 5 v0 ") != NULL && bs_n == 4 && bs_log[0][0] == 88 && bs_log[1][0] == 24 && bb_n == 2 && bb_log[1][1] == 5);
						/* S5 (review capv8-r1 F1): instanced draws never read the client array (divisor fetch is by instance): exact-size blocks, a 3-entry array and index 60000 (ASan would fault) */
					{
						typedef void (*cv_dai)(void *, unsigned, int, int, int);
						float *c3 = (float *)malloc(24);                  /* 3 vertices of 2 floats, stride 8 */
						unsigned short *ix = (unsigned short *)malloc(6);
						for (t = 0; t < 6; t++) c3[t] = (float)t;
						ix[0] = 60000; ix[1] = 1; ix[2] = 2;
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 8; inj_vaoff[0] = (unsigned long)(uintptr_t)c3; CV_RESET();
						cap_begin(); ((cv_dei)cy.disp[763])(cy.rctx, 4, 3, 0x1403, ix, 3); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v inst\n") != NULL && strstr(cb, " v idx ") == NULL && strstr(cb, " v v0 ") == NULL && bb_n == 0 && bs_n == 0);
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 8; inj_vaoff[0] = (unsigned long)(uintptr_t)c3; CV_RESET();
						cap_begin(); ((cv_dai)cy.disp[762])(cy.rctx, 4, 0, 3, 3); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v inst\n") != NULL && strstr(cb, " v v0 ") == NULL);
						free(c3); free(ix);
					}
					/* S6: attribute 0 buffer query that wrote nothing (-1) with a non-NULL pointer: no client read (heap block holds real data, so a read would print values) */
					CV_SETUP(); inj_va[0][1] = -1; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cva; CV_RESET();
					cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " draw 1 v -\n") != NULL && strstr(cb, " v v0 ") == NULL && bb_n == 0 && bs_n == 0);
					/* S7: ELEMENT_ARRAY_BUFFER_BINDING 1 (the smallest real name) is a buffer: `idxbuf`, the (valid) heap indices are not read as indices */
					CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; inj_eb = 1; CV_RESET();
					cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1403, i16); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " draw 1 v idxbuf\n") != NULL);
					/* S8: a non-NULL indices pointer with a type that is not u8/u16/u32 (0x1406 FLOAT): nothing read (3-byte block, count 6; a read would fault) */
					CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
					cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 6, 0x1406, i8); cap_end(cb, sizeof cb);
					CHECK(strstr(cb, " draw 1 v -\n") != NULL && strstr(cb, " v idx ") == NULL);
					/* S9: u32 indices with distinct values at index >= 1 (4-byte stride); max 65535 is accepted, 65536 is `idxrange` (heap holds exactly what the accepted case reads) */
					{
						unsigned *u3 = (unsigned *)malloc(12);
						float *big = (float *)calloc(1, 65535 * 16 + 8);   /* index 65535 at stride 16, 8 bytes */
						u3[0] = 1; u3[1] = 2; u3[2] = 3;
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)cve; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 3, 0x1405, u3); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 1,2,3 min 1 max 3 v0 20,20.5 v1 30,30.5\n") != NULL && strstr(cb, " draw 1 v v2 40,40.5\n") != NULL);
						big[0] = 100; big[1] = 100.5; big[4] = 1; big[5] = 1.5; big[8] = 2; big[9] = 2.5; big[65535 * 4] = 7; big[65535 * 4 + 1] = 7.5;
						u3[0] = 65535; u3[1] = 1; u3[2] = 2;
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)big; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 3, 0x1405, u3); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 65535,1,2 min 1 max 65535 v0 7,7.5 v1 1,1.5\n") != NULL && strstr(cb, " draw 1 v v2 2,2.5\n") != NULL);
						u3[0] = 65536;
						CV_SETUP(); inj_va[0][1] = 0; inj_va[0][4] = 16; inj_vaoff[0] = (unsigned long)(uintptr_t)big; CV_RESET();
						cap_begin(); ((cv_de)cy.disp[67])(cy.rctx, 4, 3, 0x1405, u3); cap_end(cb, sizeof cb);
						CHECK(strstr(cb, " draw 1 v idx 65536,1,2 min 1 max 65536 idxrange\n") != NULL && strstr(cb, " v v0 ") == NULL);
						free(u3); free(big);
					}
					/* S10: the prov line stays whole at 229 bytes + ` ~cut` placement: four bound units with 10-digit names and sizes make the line over 230 bytes */
					{
						struct sweet_th_ev e2;
						unsigned tu2[8];
						char full[512], want[256], *lp;
						int u2;
						CV_SETUP();
						for (u2 = 1; u2 < 4; u2++) {
							memset(&e2, 0, sizeof e2); e2.kind = SWEET_TH_TEXSTORAGE; e2.b = 1; e2.c = 2000000000; e2.d = 2000000000; sweet_th_record(4000000000u + (unsigned)u2, e2);
							memset(&e2, 0, sizeof e2); e2.kind = SWEET_TH_FBOTEX; e2.c = 2000000000; sweet_th_record(4000000000u + (unsigned)u2, e2);
							((void (*)(void *, unsigned))cy.disp[342])(cy.rctx, 0x84C0 + (unsigned)u2);
							((void (*)(void *, unsigned, unsigned))cy.disp[5])(cy.rctx, 0xde1, 4000000000u + (unsigned)u2);
						}
						((void (*)(void *, unsigned))cy.disp[342])(cy.rctx, 0x84C0);
						CV_RESET();
						sweet_tu_snapshot(sweet_th_ctx_index(cy.rctx), tu2);
						sweet_prov_line(full, sizeof full, 0, 3, tu2);
						CHECK(strlen(full) >= 230 && strlen(full) < 300);   /* the precondition: the full line is over the limit */
						memcpy(want, full, 224); memcpy(want + 224, " ~cut", 6);
						cap_begin(); ((cv_da)cy.disp[65])(cy.rctx, 4, 1, 48); cap_end(cb, sizeof cb);
						lp = strstr(cb, "SWEETGL: PIXELS prov ");
						CHECK(lp != NULL && lp[strlen(lp) - 1] == '\n');
						if (lp) { lp[strlen(lp) - 1] = 0; CHECK(strlen(lp) == 229 && !strcmp(lp, want)); }
					}
					/* S11: line packing vs an independent model: every fragment is all-or-nothing against 224 bytes, the ` ~cut` mark always fits (strlen <= 229); sweep the digit counts of 4 units so some fragment ends in the last 5 bytes before the cap */
					{
						struct sweet_capv cv2;
						char lc2[256], md[512], fr[128];
						unsigned pw[11] = { 0, 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
						int l0, l1, l2, l3, i3, o, cutm, straddle = 0, nchk = 0;
						memset(&cv2, 0, sizeof cv2);
						cv2.vw = -2000000000; cv2.vh = -2000000000; cv2.prog = -2000000000; cv2.mode = 0xffffffffu; cv2.count = -2000000000; cv2.first = 2000000000; cv2.idxtype = 0xffffffffu;
						for (i3 = 4; i3 < 8; i3++) cv2.tex[i3] = 4000000000u;
						for (l0 = 1; l0 <= 10; l0++) for (l1 = 1; l1 <= 10; l1++) for (l2 = 1; l2 <= 10; l2++) for (l3 = 1; l3 <= 10; l3++) {
							cv2.tex[0] = pw[l0]; cv2.tex[1] = pw[l1]; cv2.tex[2] = pw[l2]; cv2.tex[3] = pw[l3];
							sweet_capv_line(lc2, sizeof lc2, -2000000000, -2000000000, &cv2, 'h', 0);
							o = snprintf(md, sizeof md, "SWEETGL: PIXELS capv8 fbo %d vp %dx%d draw %d", -2000000000, cv2.vw, cv2.vh, -2000000000); cutm = 0;
							CHECK(o <= 224);
							snprintf(fr, sizeof fr, " prog %d mode 0x%x count %d first %s idx 0x%x", cv2.prog, cv2.mode, cv2.count, "2000000000", cv2.idxtype);
							if (!cutm && o + (int)strlen(fr) <= 224) { strcat(md, fr); o += (int)strlen(fr); } else cutm = 1;
							if (!cutm && o + 6 <= 224) { strcat(md, " | tex"); o += 6; } else cutm = 1;
							for (i3 = 0; i3 < 8; i3++) {
								snprintf(fr, sizeof fr, " u%d %u", i3, cv2.tex[i3]);
								if (!cutm && o + (int)strlen(fr) <= 224) { strcat(md, fr); o += (int)strlen(fr); }
								else { if (!cutm && o + (int)strlen(fr) <= 229) straddle++; cutm = 1; }
							}
							if (cutm) strcat(md, " ~cut");
							nchk++;
							CHECK(strlen(lc2) <= 229 && !strcmp(lc2, md));
						}
						CHECK(nchk == 10000 && straddle >= 10);   /* the sweep does hit the case: a fragment ending in the last 5 bytes before the cap while a cut follows */
					}
					free(cva); free(cve); free(i16); free(i64); free(i8); free(i32);
					}
					/* J: the process budget is 32 lines; the 33rd eligible draw prints ONE `capv8 cap` line, then silence; skip lines are counted too */
					for (dd = 0; dd < 1; dd++) {
						char *pc;
						unsigned nl = 0, nc = 0;
						CV_SETUP(); inj_prog = 0; inj_nu = 0;
						cap_begin();
						for (b = 0; b < 9; b++) {
							BIND_FB(10 + b, 1002); inj_vp[2] = 1082; inj_vp[3] = 1226; inj_prog = 0;
							for (q = 0; q < 6; q++) ((cv_da)cy.disp[65])(cy.rctx, 4, 0, 3);
						}
						cap_end(ob, sizeof ob);
						for (pc = ob; (pc = strstr(pc, "SWEETGL: PIXELS capv8 ")) != NULL; pc++) {   /* nl counts the header (h) lines: one per draw */
							char *e = strchr(pc, '\n'), *pr = strstr(pc, " prog ");
							if (!strncmp(pc, "SWEETGL: PIXELS capv8 cap\n", 26)) nc++; else if (e && pr && pr < e) nl++;
						}
						CHECK(nl == 32 && nc == 1);
						pc = strstr(ob, "SWEETGL: PIXELS capv8 cap\n");
						CHECK(pc != NULL && strstr(pc + 1, "SWEETGL: PIXELS capv8 ") == NULL);   /* nothing after the cap line */
						CHECK(strstr(ob, "capv8 fbo 10 vp 1082x1226 draw 4 ") != NULL);
						for (pc = ob; *pc; ) { char *e = strchr(pc, '\n'); size_t ll = e ? (size_t)(e - pc) : strlen(pc); CHECK(ll < SWEET_CAPV_MAX); pc += ll + (e ? 1 : 0); }   /* every line of the whole output stays short */
					}
					inj_cv = 0; sweet_ub_reset(); sweet_px_reset(); sweet_px_set_clock(NULL); fake_ret = 0; rp_multi = 0;
#undef CV_SETUP
#undef CV_RESET
				}
				{   /* mode / count recorded by all five census draw hooks: seen in the state line of sampled blur draws */
					typedef void (*f_de2)(void *, unsigned, int, unsigned, const void *);
					typedef void (*f_drr2)(void *, unsigned, unsigned, unsigned, int, unsigned, const void *);
					typedef void (*f_dai2)(void *, unsigned, int, int, int);
					typedef void (*f_dei2)(void *, unsigned, int, unsigned, const void *, int);
					sweet_px_reset(); sweet_px_set_clock(mock_clk); mock_t = 1000; setenv("SWEETGL_CENSUS", "2", 1); sweet_gli_init(&cy, &dummy); unsetenv("SWEETGL_CENSUS");
					sweet_ub_reset(); sweet_tu_reset(); rp_multi = 1; memcpy(rp_pats[0], rp_pat, 64); inj_prog = 4; inj_vp[0] = 10; inj_vp[1] = 20; inj_vp[2] = 97; inj_vp[3] = 76;
					fake_ret = 0x8cd5; inj_pbo = 0; inj_al = 4; inj_rbuf = 0x8CE0;
					cap_begin();
					inj_fbo = inj_readfb = 31; rp_calls = 0; ((f_de2)cy.disp[67])(cy.rctx, 5, 100, 0x1403, NULL);
					inj_fbo = inj_readfb = 32; rp_calls = 0; ((f_drr2)cy.disp[405])(cy.rctx, 6, 0, 9, 200, 0x1403, NULL);
					inj_fbo = inj_readfb = 33; rp_calls = 0; ((f_dai2)cy.disp[762])(cy.rctx, 7, 0, 300, 2);
					inj_fbo = inj_readfb = 34; rp_calls = 0; ((f_dei2)cy.disp[763])(cy.rctx, 8, 400, 0x1403, NULL, 2);
					cap_end(ob, sizeof ob);
					CHECK(strstr(ob, "state r0 fbo 31 prog 4 kind b mode 0x5 count 100 ") != NULL && strstr(ob, "state r0 fbo 32 prog 4 kind b mode 0x6 count 200 ") != NULL &&
					      strstr(ob, "state r0 fbo 33 prog 4 kind b mode 0x7 count 300 ") != NULL && strstr(ob, "state r0 fbo 34 prog 4 kind b mode 0x8 count 400 ") != NULL);
				}
				/* grid through the unbind hook: whole-viewport read after the five patches, exact map; padding (PACK_ALIGNMENT 8, odd width); size cap; written 0 */
				{
					static unsigned char gimg[4328 * 1226], gimg2[4008 * 100];
					int x, y;
					memset(gimg, 0, sizeof gimg);
					for (y = 0; y < 613; y++) memset(gimg + (size_t)y * 4328, 0xff, 541 * 4);   /* x < 541, y < 613 = cells 0..3 of rows 0..3 (boundaries 541 and 613 are cell starts) */
					rp_grid_src = gimg; rp_grid_len = sizeof gimg;
					UB_ARM();
					BIND_FB(3, 1002); DRAW_G(3, 1082, 1226, 1002);
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 6 && strstr(ob, "SWEETGL: PIXELS grid r1 fbo 3 vp 1082x1226 cells 16/64 map ####....|####....|####....|####....|........|........|........|........ a5 0\nSWEETGL: PIXELS capdraws r1 fbo 3 ") != NULL);
					sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " grid 1 gridskip 0") != NULL);
					/* odd width with PACK_ALIGNMENT 8: stride 4008, the 4 pad bytes per row are not pixels */
					memset(gimg2, 0, sizeof gimg2); for (y = 0; y < 100; y++) for (x = 0; x < 1001; x++) memset(gimg2 + (size_t)y * 4008 + (size_t)x * 4, 0xff, 4);
					rp_grid_src = gimg2; rp_grid_len = sizeof gimg2;
					UB_ARM(); inj_al = 8;
					BIND_FB(3, 1002); DRAW_G(3, 1001, 100, 1002);
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 6 && strstr(ob, "grid r1 fbo 3 vp 1001x100 cells 64/64 map ########|########|########|########|########|########|########|######## a5 0\n") != NULL);
					inj_al = 4;
					/* size cap 16 MiB (review b6 F2): 1127x2438 (11 MB, g113d fbo 15) is read; 4096x1025x4 = 16793600 > 16 MiB -> no grid read, a loud skip line once per FBO name, gridskip counted,
					 * the 5-point sample and capdraws still printed; 4096x1024 is exactly 16 MiB -> read */
					rp_grid_src = gimg; rp_grid_len = sizeof gimg;
					UB_ARM();
					BIND_FB(3, 1002); DRAW_G(3, 1127, 2438, 1002);
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 6 && strstr(ob, "PIXELS grid r1 fbo 3 vp 1127x2438 cells ") != NULL && strstr(ob, "grid skip") == NULL);
					UB_ARM();
					BIND_FB(3, 1002); DRAW_G(3, 4096, 1025, 1002);
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 5 && strstr(ob, "unbind S fbo 3 vp 4096x1025 nonzero") != NULL && strstr(ob, "PIXELS grid r") == NULL && strstr(ob, "capdraws r1 fbo 3 draws 1") != NULL);
					CHECK(strstr(ob, "SWEETGL: PIXELS grid skip fbo 3 vp 4096x1025 bytes 16793600\n") != NULL && OB_COUNT("grid skip") == 1);
					sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " grid 0 gridskip 1") != NULL);
					BIND_FB(3, 1002); DRAW_G(3, 4096, 1025, 1002);   /* same FBO name again: counted, no second line */
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 5 && OB_COUNT("grid skip") == 0);
					sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " grid 0 gridskip 2") != NULL);
					UB_ARM(); rp_grid_src = gimg; rp_grid_len = sizeof gimg;
					BIND_FB(4, 1002); DRAW_G(4, 4096, 1025, 1002);   /* another FBO name: its own line */
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(strstr(ob, "SWEETGL: PIXELS grid skip fbo 4 vp 4096x1025 bytes 16793600\n") != NULL);
					{   /* skip lines: max 8 FBO names */
						int nl = 0, n2 = 0;
						UB_ARM();
						for (int fb = 100; fb < 110; fb++) nl += sweet_px_grid_skip_once(fb);
						n2 = sweet_px_grid_skip_once(100) + sweet_px_grid_skip_once(107);
						CHECK(nl == 8 && n2 == 0);
					}
					UB_ARM();
					BIND_FB(3, 1002); DRAW_G(3, 4096, 1024, 1002);
					rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
					CHECK(rp_calls == 6 && strstr(ob, "PIXELS grid r1 fbo 3 vp 4096x1024 cells ") != NULL && strstr(ob, "grid skip") == NULL);
					sweet_px_stats_line(l, sizeof l); CHECK(strstr(l, " grid 1 gridskip 0") != NULL);
					/* F1: viewport larger than the FBO: the read clips, the outside stays 0xA5: columns right of x=541 are pre-fill -> '?', a5 count = 541 columns x 1226 rows; origin (37,11) reaches glReadPixels unchanged */
					{
						static unsigned char gclip[4328 * 1226];
						memset(gclip, 0xA5, sizeof gclip);
						for (y = 0; y < 1226; y++) memset(gclip + (size_t)y * 4328, 0xff, 541 * 4);
						rp_grid_src = gclip; rp_grid_len = sizeof gclip;
						UB_ARM();
						BIND_FB(3, 1002); mock_t = 1002; inj_prog = 0; inj_vp[0] = 37; inj_vp[1] = 11; inj_vp[2] = 1082; inj_vp[3] = 1226; inj_fbo = 3; inj_readfb = 3; ((f_dr)cy.disp[65])(cy.rctx, 4, 0, 3);
						rp_calls = 0; cap_begin(); BIND_FB(5, 1002); cap_end(ob, sizeof ob);
						CHECK(rp_calls == 6 && rp_log[5][0] == 37 && rp_log[5][1] == 11 && rp_log[5][2] == 1082 && rp_log[5][3] == 1226);
						CHECK(strstr(ob, "SWEETGL: PIXELS grid r1 fbo 3 vp 1082x1226 cells 32/64 map ####????|####????|####????|####????|####????|####????|####????|####???? a5 663266\n") != NULL);
						rp_grid_src = NULL; rp_grid_len = 0;
					}
					rp_grid_src = NULL; rp_grid_len = 0;
				}
				{   /* review b6 F4: Enable / Disable of GL_BLEND from 20 distinct contexts neither registers them in the texture-history table nor bumps ctxover */
					static int ectx[20]; char l2[300];
					sweet_th_reset(); UB_ARM();
					for (int e = 0; e < 20; e++) { ((void (*)(void *, unsigned))cy.disp[72])(&ectx[e], 0x0BE2); ((void (*)(void *, unsigned))cy.disp[63])(&ectx[e], 0x0BE2); }
					sweet_px_stats_line(l2, sizeof l2); CHECK(strstr(l2, " ctxover 0 ") != NULL);
					CHECK(sweet_th_ctx_index(&ectx[19]) == 1 && sweet_th_ctx_index(&ectx[0]) == 2);   /* the first contexts to register get slots 1, 2: the hooks took none */
					sweet_th_reset();
				}
				/* census=1 (no schedule): the hook only tracks and forwards, no read, no output */
				sweet_px_reset(); sweet_ub_reset();
				rp_calls = 0; seq_n = 0; cap_begin();
				mock_t = 99999; inj_fbo = 3; inj_readfb = 3;
				((void (*)(void *, unsigned, unsigned))cx.disp[672])(cx.rctx, 0x8D40, 3);
				inj_prog = 0; inj_vp[0] = 0; inj_vp[1] = 0; inj_vp[2] = 1082; inj_vp[3] = 1226;
				((f_dr)cx.disp[65])(cx.rctx, 4, 0, 3);
				seq_n = 0;
				((void (*)(void *, unsigned, unsigned))cx.disp[672])(cx.rctx, 0x8D40, 5);
				cap_end(ob, sizeof ob);
				CHECK(rp_calls == 0 && ob[0] == 0 && seq_n == 1 && count_seq("_mesa_BindFramebuffer") == 1 && args[0] == 0x8D40 && args[1] == 5);
				inj_fbo = 0; inj_readfb = 0;
			}
			sweet_px_reset(); sweet_px_set_clock(NULL); rp_multi = 0; inj_prog = 0; inj_fbo = 0; inj_readfb = 0; fake_ret = 0; inj_vp[0] = inj_vp[1] = 0;
			{   /* F5: the default clock is CLOCK_MONOTONIC seconds (REALTIME would be ~1.7e9 s off the uptime) */
				struct timespec ts; long long a, b;
				sweet_px_set_clock(NULL);
				a = sweet_px_now(); clock_gettime(CLOCK_MONOTONIC, &ts); b = sweet_px_now();
				CHECK(a >= 0 && b >= a && ts.tv_sec - a <= 2 && b - ts.tv_sec <= 2);
			}
			(void)nl;
		}

		inj_on = 0; inj_pbo = 0; inj_prog = 0; fake_ret = 0; rp_fill = 0;
	}

	{   /* P-G0 slice c: flush timing (SWEETGL_FLUSHT). Fake clock: every read returns ft_t then adds ft_step. */
		static struct sweet_gli f0, f1, f2, f3; int fd_ = 0; char fb[4096]; struct sweet_ft_win w, d; unsigned dsec = 77;
		sweet_ft_reset();
		/* formatter: exact string, field order, us truncation (ns / 1000), pid/sec/last_exit as given */
		memset(&w, 0, sizeof w); w.flush = 3; w.flush_in_ns = 4567999; w.flush_max_ns = 2999999; w.finish = 1; w.finish_in_ns = 999; w.finish_max_ns = 1000; w.last_exit_ns = 12345678901234ull;
		sweet_ft_line(fb, sizeof fb, 321, 5, &w);
		CHECK(!strcmp(fb, "SWEETGL: FLUSHT pid 321 sec 5 flush 3 flush_in_us 4567 flush_max_us 2999 finish 1 finish_in_us 0 finish_max_us 1 last_exit_ns 12345678901234"));
		CHECK(strlen(fb) < 200);
		/* large values: u32 saturation of the max fields, u64 sums printed whole; the worst line is still bounded by snprintf (n) */
		memset(&w, 0, sizeof w); w.flush = 0xffffffffu; w.flush_in_ns = ~0ull; w.flush_max_ns = ~0ull; w.finish = 0xffffffffu; w.finish_in_ns = ~0ull; w.finish_max_ns = 5000000000000ull; w.last_exit_ns = ~0ull;
		sweet_ft_line(fb, sizeof fb, 2147483647, 0xffffffffu, &w);
		CHECK(strstr(fb, "flush 4294967295 flush_in_us 18446744073709551 flush_max_us 4294967295 finish 4294967295 finish_in_us 18446744073709551 finish_max_us 4294967295 last_exit_ns 18446744073709551615") != NULL);
		sweet_ft_line(fb, 40, 1, 1, &w); CHECK(strlen(fb) == 39);   /* truncates, never overruns */
		/* timebase conversion: identity, 125/3 (Apple arm), a huge tick count does not overflow, denom 0 */
		CHECK(sweet_ft_ticks_to_ns(1000, 1, 1) == 1000 && sweet_ft_ticks_to_ns(24, 125, 3) == 1000 && sweet_ft_ticks_to_ns(25, 125, 3) == 1041);
		CHECK(sweet_ft_ticks_to_ns(100000000000000ull, 125, 3) == 4166666666666666ull && sweet_ft_ticks_to_ns(5, 1, 0) == 0);
		/* parse: only "1" is on, "0" / "" / unset off, everything else bad */
		{ int bad = 7; CHECK(sweet_ft_parse(NULL, &bad) == 0 && bad == 0); CHECK(sweet_ft_parse("", &bad) == 0 && bad == 0); CHECK(sweet_ft_parse("0", &bad) == 0 && bad == 0);
		  CHECK(sweet_ft_parse("1", &bad) == 1 && bad == 0); CHECK(sweet_ft_parse("11", &bad) == 0 && bad == 1); CHECK(sweet_ft_parse("yes", &bad) == 0 && bad == 1); CHECK(sweet_ft_parse("1 ", &bad) == 0 && bad == 1); }
		/* window logic: calls in second 0 are held, the first call of second 1 yields the window of second 0 */
		sweet_ft_reset();
		CHECK(sweet_ft_note(0, 1000000000ull, 1000000500ull, &d, &dsec) == 0);        /* base = 1.0000005 s */
		CHECK(sweet_ft_note(1, 1100000000ull, 1100002000ull, &d, &dsec) == 0);
		CHECK(sweet_ft_note(0, 1200000000ull, 1200000100ull, &d, &dsec) == 0);
		CHECK(sweet_ft_note(0, 2000000000ull, 2000000300ull, &d, &dsec) == 0);        /* t1 - base = 0.9999995 s: still window 0 */
		CHECK(sweet_ft_note(0, 2100000000ull, 2100000400ull, &d, &dsec) == 1 && dsec == 0);   /* 1.0999995 s: window 1 opens, window 0 comes out */
		CHECK(d.flush == 3 && d.finish == 1 && d.flush_in_ns == 500 + 100 + 300 && d.flush_max_ns == 500 && d.finish_in_ns == 2000 && d.finish_max_ns == 2000 && d.last_exit_ns == 2000000300ull);
		CHECK(sweet_ft_note(0, 4500000000ull, 4500000050ull, &d, &dsec) == 1 && dsec == 1 && d.flush == 1 && d.flush_in_ns == 400 && d.finish == 0);   /* a gap of quiet seconds prints nothing for them */
		CHECK(sweet_ft_note(0, 3000000000ull, 3000000050ull, &d, &dsec) == 0);        /* a late thread with an older exit lands in the open window, no flip-flop */
		CHECK(sweet_ft_note(1, 4600000100ull, 4600000000ull, &d, &dsec) == 0);        /* clock backwards: duration 0, no wrap */
		CHECK(sweet_ft_note(0, 5600000000ull, 5600000000ull, &d, &dsec) == 1 && dsec == 3 && d.flush == 2 && d.finish == 1 && d.finish_in_ns == 0 && d.finish_max_ns == 0 && d.flush_in_ns == 100);
		/* the u64 sum saturates instead of wrapping: 5 calls of 4e18 ns (sum 2e19 > 2^64) in one window; the u32 max fields clamp at print */
		sweet_ft_reset();
		for (int k = 0; k < 5; k++) CHECK(sweet_ft_note(0, 0, 4000000000000000000ull, &d, &dsec) == 0);
		CHECK(sweet_ft_note(1, 4100000000000000000ull, 4200000000000000000ull, &d, &dsec) == 1 && dsec == 0 && d.flush == 5 && d.flush_in_ns == ~0ull && d.flush_max_ns == 4000000000000000000ull);
		CHECK(sweet_ft_note(0, 0, ~0ull - 5, &d, &dsec) == 1 && dsec == 200000000u);   /* an exit time beyond the u32 second range clamps the window id; nothing wraps (UBSan) */
		sweet_ft_reset();
		/* off by default: the slots are exactly the generated entries, no output */
		unsetenv("SWEETGL_FLUSHT");
		cap_begin(); sweet_gli_init(&f0, &fd_); cap_end(fb, sizeof fb);
		CHECK(f0.disp[89] == sweet_gli_defaults[89] && f0.disp[90] == sweet_gli_defaults[90] && fb[0] == 0 && !sweet_ft_enabled());
		setenv("SWEETGL_FLUSHT", "0", 1); cap_begin(); sweet_gli_init(&f0, &fd_); cap_end(fb, sizeof fb); unsetenv("SWEETGL_FLUSHT");
		CHECK(f0.disp[89] == sweet_gli_defaults[89] && f0.disp[90] == sweet_gli_defaults[90] && fb[0] == 0);
		/* unreadable knob: ONE loud line per process (two inits), timing stays off */
		setenv("SWEETGL_FLUSHT", "on", 1); cap_begin(); sweet_gli_init(&f0, &fd_); sweet_gli_init(&f0, &fd_); cap_end(fb, sizeof fb); unsetenv("SWEETGL_FLUSHT");
		CHECK(!strcmp(fb, "SWEETGL: FLUSHT unavailable: SWEETGL_FLUSHT=\"on\" is not 0 or 1, flush timing OFF\n") && f0.disp[90] == sweet_gli_defaults[90]);
		/* timebase unavailable with the knob on: one loud line, off */
		sweet_ft_reset(); sweet_ft_tb_fail = 1;
		setenv("SWEETGL_FLUSHT", "1", 1); cap_begin(); sweet_gli_init(&f0, &fd_); sweet_gli_init(&f0, &fd_); cap_end(fb, sizeof fb); unsetenv("SWEETGL_FLUSHT");
		CHECK(!strcmp(fb, "SWEETGL: FLUSHT unavailable: mach_timebase_info failed, flush timing OFF\n") && f0.disp[89] == sweet_gli_defaults[89] && !sweet_ft_enabled());
		/* on: hooks installed; the original runs exactly once with the same context, return path unchanged; times from the fake clock */
		sweet_ft_reset(); sweet_ft_set_clock(ft_clk); ft_t = 1000000000ull; ft_step = 250000;
		setenv("SWEETGL_FLUSHT", "1", 1); cap_begin(); sweet_gli_init(&f1, &fd_); cap_end(fb, sizeof fb); unsetenv("SWEETGL_FLUSHT");
		CHECK(fb[0] == 0 && sweet_ft_enabled() && f1.disp[90] != sweet_gli_defaults[90] && f1.disp[89] != sweet_gli_defaults[89] && f1.disp[90] != f1.disp[89]);
		sweet_gli_ensure_current = ens; ens_ret = 0; ens_n = 0; hit = NULL; hits = 0;
		cap_begin();
		((void (*)(void *))f1.disp[90])(&fd_);                                   /* t0=1.0 s, t1 = +0.25 ms... flush dur 250 us */
		CHECK(hit && !strcmp(hit, "_mesa_Finish") && hits == 1 && ens_n == 1 && ens_last == &fd_);
		ft_step = 750000; ft_t = 1500000000ull;
		((void (*)(void *))f1.disp[89])(&fd_);                                   /* finish dur 750 us, same second */
		CHECK(hits == 2 && ens_n == 2);
		ft_step = 250000; ft_t = 2600000000ull;
		((void (*)(void *))f1.disp[90])(&fd_);                                   /* next second: the window of second 0 is printed now */
		cap_end(fb, sizeof fb);
		{ char pp[64]; snprintf(pp, sizeof pp, "SWEETGL: FLUSHT pid %d sec 0 ", (int)getpid()); CHECK(strncmp(fb, pp, strlen(pp)) == 0); }
		CHECK(strstr(fb, " sec 0 flush 1 flush_in_us 250 flush_max_us 250 finish 1 finish_in_us 750 finish_max_us 750 last_exit_ns 1500750000\n") != NULL);
		CHECK(strchr(fb, '\n') == fb + strlen(fb) - 1 && strlen(fb) < 200);       /* exactly one line */
		CHECK(strstr(fb, "unavailable") == NULL);
		/* hooks chain: a second init (fresh table) does not nest the hook on itself */
		sweet_ft_reset(); sweet_ft_set_clock(ft_clk);
		setenv("SWEETGL_FLUSHT", "1", 1); sweet_gli_init(&f2, &fd_); sweet_gli_init(&f3, &fd_); unsetenv("SWEETGL_FLUSHT");
		ft_t = 5000000000ull; ft_step = 1; hits = 0; ((void (*)(void *))f3.disp[90])(&fd_); CHECK(hits == 1);
		/* thread safety: 4 threads x 20000 calls through the note path (ASan/UBSan run); every call lands in exactly one window */
		sweet_ft_reset();
		{
			pthread_t th[4]; int i;
			ft_total = 0;
			for (i = 0; i < 4; i++) pthread_create(&th[i], NULL, ft_worker, NULL);
			for (i = 0; i < 4; i++) pthread_join(th[i], NULL);
			CHECK(sweet_ft_note(0, 0, 4000000000000000000ull, &d, &dsec) == 1);   /* far later second: the last open window comes out */
			ft_total += d.flush + d.finish;
			CHECK(ft_total == 4 * 20000);
		}
		sweet_ft_set_clock(NULL); sweet_ft_reset();
		sweet_gli_ensure_current = NULL;
		(void)ens_n;
	}

	{   /* P-G1 slice 2: `SWEETGL: BASE` once per process */
		static struct sweet_gli b0; int bd = 0; char bb[4096], tp[] = "/tmp/gli-base-XXXXXX", lp[400]; int fd2 = mkstemp(tp); char blk[777]; struct sweet_base_info bi;
		memset(blk, 'x', sizeof blk); CHECK(fd2 >= 0 && write(fd2, blk, sizeof blk) == (ssize_t)sizeof blk); close(fd2);
		bi.image = 0x1f0a000ull; bi.slide = 0x4000ull; bi.path = "/usr/lib/libsweetgl.dylib";
		CHECK(sweet_base_line(bb, sizeof bb, 321, &bi, 98765) > 0);
		CHECK(!strcmp(bb, "SWEETGL: BASE pid 321 image 0x1f0a000 slide 0x4000 size 98765 path /usr/lib/libsweetgl.dylib"));
		bi.image = 0xfffffff012345678ull; bi.slide = 0xffffffffffffc000ull; bi.path = NULL;   /* 64-bit values; negative slide as two's complement */
		sweet_base_line(bb, sizeof bb, 2147483647, &bi, 0xffffffffffffffffull);
		CHECK(!strcmp(bb, "SWEETGL: BASE pid 2147483647 image 0xfffffff012345678 slide 0xffffffffffffc000 size 18446744073709551615 path ?"));
		memset(lp, 'p', sizeof lp - 1); lp[sizeof lp - 1] = 0; bi.path = lp;
		sweet_base_line(bb, sizeof bb, 2147483647, &bi, 0xffffffffffffffffull);
		CHECK(strlen(bb) < 200 && strlen(bb) > 150 && strstr(bb, " path pppp") != NULL && bb[strlen(bb) - 1] == 'p');   /* long path is cut, the worst-case line stays < 200 B */
		sweet_base_line(bb, 30, 1, &bi, 1); CHECK(strlen(bb) == 29);                                                    /* never overruns */
		CHECK(sweet_base_unavailable_line(bb, sizeof bb, "dladdr failed") > 0 && !strcmp(bb, "SWEETGL: BASE unavailable (dladdr failed)"));
		sweet_base_unavailable_line(bb, sizeof bb, lp); CHECK(strlen(bb) < 200);
		/* ok: resolver success, stat of the real file, ONE line over two inits */
		sweet_base_reset(); sweet_base_set_resolver(bs_resolve); bs_calls = 0; bs_ret = 1; bs_why = "x"; bs_info.image = 0xabcdef000ull; bs_info.slide = 0x1000ull; bs_info.path = tp;
		cap_begin(); sweet_gli_init(&b0, &bd); sweet_gli_init(&b0, &bd); cap_end(bb, sizeof bb);
		{ char ex[300]; snprintf(ex, sizeof ex, "SWEETGL: BASE pid %d image 0xabcdef000 slide 0x1000 size 777 path %s\n", (int)getpid(), tp); CHECK(!strcmp(bb, ex)); }
		CHECK(bs_calls == 1 && sweet_base_report() == 0);
		/* resolver fails: loud line once, nothing else */
		sweet_base_reset(); sweet_base_set_resolver(bs_resolve); bs_ret = 0; bs_why = "dladdr failed";
		cap_begin(); sweet_gli_init(&b0, &bd); sweet_gli_init(&b0, &bd); cap_end(bb, sizeof bb);
		CHECK(!strcmp(bb, "SWEETGL: BASE unavailable (dladdr failed)\n"));
		/* resolver says ok but gives no path: unavailable, not a crash */
		sweet_base_reset(); sweet_base_set_resolver(bs_resolve); bs_ret = 1; bs_info.path = NULL; bs_why = "no path";
		cap_begin(); CHECK(sweet_base_report() == -1); cap_end(bb, sizeof bb);
		CHECK(!strcmp(bb, "SWEETGL: BASE unavailable (no path)\n"));
		/* stat fails: the line with size 0 plus one loud line */
		sweet_base_reset(); sweet_base_set_resolver(bs_resolve); bs_ret = 1; bs_info.path = "/nonexistent-dir/libsweetgl.dylib";
		cap_begin(); CHECK(sweet_base_report() == -2); cap_end(bb, sizeof bb);
		{ char ex[300]; snprintf(ex, sizeof ex, "SWEETGL: BASE pid %d image 0xabcdef000 slide 0x1000 size 0 path /nonexistent-dir/libsweetgl.dylib\nSWEETGL: BASE unavailable (stat failed errno 2, size 0)\n", (int)getpid()); CHECK(!strcmp(bb, ex)); }
		/* the default (host) resolver says unavailable */
		sweet_base_reset(); cap_begin(); CHECK(sweet_base_report() == -1); cap_end(bb, sizeof bb);
		CHECK(!strcmp(bb, "SWEETGL: BASE unavailable (no dyld (host build))\n"));
		sweet_base_reset(); sweet_base_set_resolver(bs_resolve); sweet_base_set_resolver(NULL);   /* NULL restores the default resolver */
		cap_begin(); CHECK(sweet_base_report() == -1); cap_end(bb, sizeof bb); CHECK(!strcmp(bb, "SWEETGL: BASE unavailable (no dyld (host build))\n"));
		unlink(tp); sweet_base_reset(); sweet_base_set_resolver(bs_resolve); bs_ret = 0; bs_why = "consumed";   /* later inits in other tests stay quiet-ish: the once is consumed below */
		sweet_base_report(); sweet_base_set_resolver(NULL);
	}

	{   /* LOWCLK lever real-flush (SWEETGL_REALFLUSH + switch file): default off = table untouched; armed = runtime switch 0/1/2, gated by the kernel present gate */
		static struct sweet_gli r0, r1, r2; int rd = 0; char rb[4096], rp[64], ex[512]; unsigned long long nr, nf, nn; int armed = 9, def = 9; FILE *wf;
		typedef void (*f_fl)(void *);
		strcpy(rp, "/tmp/gli-rf-XXXXXX"); { int tfd = mkstemp(rp); CHECK(tfd >= 0); close(tfd); } unlink(rp);
		sweet_ft_reset(); sweet_rf_reset(); sweet_rf_set_file(rp);
		/* env parse: unset / "" / "0" not armed; arm / 1 / 2 armed with that default mode; everything else bad */
		CHECK(sweet_rf_parse(NULL, &armed, &def) == 0 && armed == 0 && def == 0); CHECK(sweet_rf_parse("", &armed, &def) == 0 && armed == 0); CHECK(sweet_rf_parse("0", &armed, &def) == 0 && armed == 0);
		CHECK(sweet_rf_parse("arm", &armed, &def) == 0 && armed == 1 && def == 0); CHECK(sweet_rf_parse("1", &armed, &def) == 0 && armed == 1 && def == 1); CHECK(sweet_rf_parse("2", &armed, &def) == 0 && armed == 1 && def == 2);
		CHECK(sweet_rf_parse("3", &armed, &def) == 1 && armed == 0); CHECK(sweet_rf_parse("12", &armed, &def) == 1 && armed == 0); CHECK(sweet_rf_parse("on", &armed, &def) == 1 && armed == 0); CHECK(sweet_rf_parse("arm ", &armed, &def) == 1);
		/* default (no env, no file): slot 90 is the generated _mesa_Finish wrapper, nothing printed */
		unsetenv("SWEETGL_REALFLUSH"); unsetenv("SWEETGL_FLUSHT");
		cap_begin(); sweet_gli_init(&r0, &rd); cap_end(rb, sizeof rb);
		CHECK(r0.disp[90] == sweet_gli_defaults[90] && r0.disp[89] == sweet_gli_defaults[89] && rb[0] == 0 && sweet_rf_mode() == 0);
		setenv("SWEETGL_REALFLUSH", "0", 1); cap_begin(); sweet_gli_init(&r0, &rd); cap_end(rb, sizeof rb); CHECK(r0.disp[90] == sweet_gli_defaults[90] && rb[0] == 0);
		/* bad env: ONE loud line for two inits, not armed */
		setenv("SWEETGL_REALFLUSH", "yes", 1); cap_begin(); sweet_gli_init(&r0, &rd); sweet_gli_init(&r0, &rd); cap_end(rb, sizeof rb); unsetenv("SWEETGL_REALFLUSH");
		CHECK(!strcmp(rb, "SWEETGL: REALFLUSH unavailable: SWEETGL_REALFLUSH=\"yes\" is not 0, arm, 1 or 2: not armed by the env\n") && r0.disp[90] == sweet_gli_defaults[90]);
		/* armed by the env "arm": hook on slot 90 only, one armed line for two inits; mode 0 until the file says otherwise */
		sweet_rf_reset(); sweet_rf_set_file(rp); sweet_rf_set_clock(ft_clk); sweet_rf_set_gate(rf_gate_t); sweet_rf_set_disp(rf_disp_t); ft_t = 5000000000ull; ft_step = 0; rf_gate_v = 1; rf_disp_v = 1; rf_disp_n = 0;
		setenv("SWEETGL_REALFLUSH", "arm", 1); cap_begin(); sweet_gli_init(&r1, &rd); sweet_gli_init(&r2, &rd); cap_end(rb, sizeof rb); unsetenv("SWEETGL_REALFLUSH");
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH armed pid %d by env, default mode 0; switch file %s (0 glFinish, 1 real flush on display targets, 2 every glFlush), needs kern.sweetfbgate=1\n", (int)getpid(), rp);
		CHECK(!strcmp(rb, ex)); CHECK(r1.disp[90] != sweet_gli_defaults[90] && r1.disp[89] == sweet_gli_defaults[89] && r2.disp[90] == r1.disp[90]);
		sweet_gli_ensure_current = ens; ens_ret = 0; ens_n = 0; hit = NULL; hits = 0;
		cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb);              /* first flush polls: no file -> env mode 0 -> finish */
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH pid %d mode 0 (glFlush = glFinish) want 0 src env real 0 finish 0 nodisp 0\n", (int)getpid());
		CHECK(!strcmp(rb, ex)); CHECK(hit && !strcmp(hit, "_mesa_Finish") && hits == 1 && ens_last == &rd);
		/* file "2": not seen before the next poll second (still finish), then every flush is the real one */
		wf = fopen(rp, "w"); fputs("2\n", wf); fclose(wf);
		ft_t += 999999999ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(rb[0] == 0 && !strcmp(hit, "_mesa_Finish") && hits == 2);
		ft_t += 1; hits = 0; cap_begin(); ((f_fl)r1.disp[90])(&rd); ((f_fl)r2.disp[90])(&rd); cap_end(rb, sizeof rb);
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH pid %d mode 2 (real flush for every glFlush) want 2 src file real 0 finish 2 nodisp 0\n", (int)getpid());
		CHECK(!strcmp(rb, ex)); CHECK(!strcmp(hit, "_mesa_Flush") && hits == 2 && rf_disp_n == 0 && sweet_rf_mode() == 2);
		((f_fl)r1.disp[89])(&rd); CHECK(!strcmp(hit, "_mesa_Finish"));               /* glFinish always waits */
		/* a refused context (ensure fails) never reaches Mesa in real mode either */
		ens_ret = -5; hits = 0; ((f_fl)r1.disp[90])(&rd); CHECK(hits == 0); ens_ret = 0;
		/* while the mode is not 0: one stats line per poll second with moving counts, none without */
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb);
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH pid %d mode 2 real 3 finish 2 nodisp 0\n", (int)getpid()); CHECK(!strcmp(rb, ex));
		/* file "1": display target = real flush, any other target = finish (counted nodisp) */
		wf = fopen(rp, "w"); fputs("1", wf); fclose(wf);
		ft_t += 1000000000ull; hits = 0; rf_disp_v = 1; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb);
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH pid %d mode 1 (real flush on display targets, glFinish elsewhere) want 1 src file real 4 finish 2 nodisp 0\n", (int)getpid());
		CHECK(!strcmp(rb, ex)); CHECK(!strcmp(hit, "_mesa_Flush") && rf_disp_n == 1);
		rf_disp_v = 0; ((f_fl)r1.disp[90])(&rd); CHECK(!strcmp(hit, "_mesa_Finish") && rf_disp_n == 2);
		rf_disp_v = -1; ((f_fl)r1.disp[90])(&rd); CHECK(!strcmp(hit, "_mesa_Finish"));
		sweet_rf_counts(&nr, &nf, &nn); CHECK(nr == 5 && nf == 2 && nn == 2);
		/* the kernel gate goes away: REFUSED line + mode 0, flushes wait again; and comes back */
		rf_gate_v = 0; rf_disp_v = 1; ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb);
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH REFUSED pid %d want 1: sysctl kern.sweetfbgate is not 1 (kernel present gate: patch 0343 + boot-arg sweetfb_gate=1): glFlush stays glFinish\n"
			"SWEETGL: REALFLUSH pid %d mode 0 (glFlush = glFinish) want 1 src file real 5 finish 2 nodisp 2\n", (int)getpid(), (int)getpid());
		CHECK(!strcmp(rb, ex)); CHECK(!strcmp(hit, "_mesa_Finish") && sweet_rf_mode() == 0);
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(rb[0] == 0);   /* unchanged state: quiet in mode 0 */
		rf_gate_v = 1; ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(strstr(rb, " mode 1 (") && !strstr(rb, "REFUSED") && !strcmp(hit, "_mesa_Flush"));
		/* bad file value: loud, mode 0; empty file too; file "0" = off; file removed = env mode */
		wf = fopen(rp, "w"); fputs("x", wf); fclose(wf);
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb);
		snprintf(ex, sizeof ex, "SWEETGL: REALFLUSH unavailable: %s starts with byte 0x78 (want 0, 1 or 2): glFlush stays glFinish\n", rp);
		CHECK(strncmp(rb, ex, strlen(ex)) == 0 && strstr(rb, " mode 0 (glFlush = glFinish) want 0 src file ") && !strcmp(hit, "_mesa_Finish"));
		wf = fopen(rp, "w"); fclose(wf);
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(rb[0] == 0 && !strcmp(hit, "_mesa_Finish"));   /* empty = same bad state: not repeated */
		wf = fopen(rp, "w"); fputs("0", wf); fclose(wf);
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(strstr(rb, " mode 0 (glFlush = glFinish) want 0 src file ") && !strstr(rb, "unavailable") && !strcmp(hit, "_mesa_Finish"));
		unlink(rp);
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(strstr(rb, " want 0 src env ") && sweet_rf_mode() == 0);
		/* env "2" = armed with default mode 2 and no file; the gate still decides */
		sweet_rf_reset(); sweet_rf_set_file(rp); sweet_rf_set_clock(ft_clk); sweet_rf_set_gate(rf_gate_t); rf_gate_v = 0;
		setenv("SWEETGL_REALFLUSH", "2", 1); cap_begin(); sweet_gli_init(&r1, &rd); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); unsetenv("SWEETGL_REALFLUSH");
		CHECK(strstr(rb, "by env, default mode 2;") && strstr(rb, "REFUSED") && strstr(rb, " mode 0 (glFlush = glFinish) want 2 src env ") && !strcmp(hit, "_mesa_Finish"));
		rf_gate_v = 1; ft_t += 1000000000ull; ((f_fl)r1.disp[90])(&rd); CHECK(!strcmp(hit, "_mesa_Flush") && sweet_rf_mode() == 2);
		/* armed by the file alone (no env): file present at init, value 0 -> hook installed, finish */
		sweet_rf_reset(); sweet_rf_set_file(rp); sweet_rf_set_clock(ft_clk); sweet_rf_set_gate(rf_gate_t);
		wf = fopen(rp, "w"); fputs("0", wf); fclose(wf);
		cap_begin(); sweet_gli_init(&r1, &rd); cap_end(rb, sizeof rb); CHECK(strstr(rb, "armed pid") && strstr(rb, " by file, default mode 0;") && r1.disp[90] != sweet_gli_defaults[90]);
		ft_t += 1000000000ull; ((f_fl)r1.disp[90])(&rd); CHECK(!strcmp(hit, "_mesa_Finish"));
		/* mode 1 with no display test linked (host build): GAP, every flush waits */
		wf = fopen(rp, "w"); fputs("1", wf); fclose(wf); sweet_rf_set_disp(NULL);
		ft_t += 1000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb);
		CHECK(strstr(rb, "SWEET GAP: realflush mode 1 without a display test in this build: every glFlush waits") && !strcmp(hit, "_mesa_Finish"));
		/* with FLUSHT on, the timing hook wraps the switch: the real flush is timed as a flush */
		sweet_rf_reset(); sweet_rf_set_file(rp); sweet_rf_set_clock(ft_clk); sweet_rf_set_gate(rf_gate_t); rf_gate_v = 1; sweet_ft_reset(); sweet_ft_set_clock(ft_clk);
		wf = fopen(rp, "w"); fputs("2", wf); fclose(wf);
		setenv("SWEETGL_FLUSHT", "1", 1); cap_begin(); sweet_gli_init(&r1, &rd); cap_end(rb, sizeof rb); unsetenv("SWEETGL_FLUSHT");
		ft_t = 9000000000ull; ft_step = 1000; hits = 0; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(hits == 1 && !strcmp(hit, "_mesa_Flush"));
		ft_t = 11000000000ull; cap_begin(); ((f_fl)r1.disp[90])(&rd); cap_end(rb, sizeof rb); CHECK(strstr(rb, "SWEETGL: FLUSHT pid") && strstr(rb, " flush 1 ") && strstr(rb, " finish 0 "));
		unlink(rp); sweet_rf_reset(); sweet_ft_reset(); ft_step = 0;
	}

	{ extern int sweet_nameq_selftest(void); int r = sweet_nameq_selftest(); if (r) printf("NAMEQ selftest failed step %d\n", r); CHECK(r == 0); }
	printf("%d checks, %d failed\n", checks, fails);
	puts(fails ? "FAIL" : "PASS");
	return fails != 0;
}

/* G-7 compute self-test externs that the generated fake GL does not cover (Apple's GLES3 table has no compute / these are not wired slots) */
void _mesa_GetBufferSubData(unsigned t, long o, long n, void *d)
{
	if (!inj_cv) { (void)t; (void)o; (void)n; (void)d; return; }
	fake_hit("_mesa_GetBufferSubData");
	if (bs_n < 8) { bs_log[bs_n][0] = (unsigned long)o; bs_log[bs_n][1] = (unsigned long)n; bs_n++; }
	if (t == 0x8F36 && d && o >= 0 && n >= 0 && (size_t)(o + n) <= sizeof inj_vb) memcpy(d, (const char *)inj_vb + o, (size_t)n);
}
void _mesa_DispatchCompute(unsigned x, unsigned y, unsigned z) { (void)x; (void)y; (void)z; }
void _mesa_MemoryBarrier(unsigned b) { (void)b; }
void _mesa_Flush(void) { fake_hit("_mesa_Flush"); }   /* LOWCLK real-flush: not a wired slot (slot 90 stays _mesa_Finish), reached through sgli_RealFlush */
