/* gli_table: the GLI dispatch table CoreAnimation calls GL through (research 33 s.2.2).
 * Layout (what -[EAGLContext getMacroContextPrivate] returns): { void *rctx; void *disp[1036]; }; CA calls
 * `disp[slot](rctx, gl args...)` with slot numbers of Apple's table (gli_slots.txt, generated into gli_table_gen.c).
 * Every slot is non-NULL: wired ones call Mesa's `_mesa_*` entry point of the thread's current context, the rest log once and
 * return 0. CA's server path never sends +setCurrentContext: [obs-RE research 34 s.1.2], so every wired wrapper and special first
 * calls sweet_gli_ensure_current(rctx) (eagl_core's sgl_ensure_current; rctx = the sgl_ctx), which makes that context current on the
 * calling thread (stealing it from another thread if needed) unless it already is, and bails out with 0 if that fails. */
#ifndef GLI_TABLE_H
#define GLI_TABLE_H
#define SWEET_GLI_NSLOTS 1036   /* {rctx, disp[1036]}: research 33 s.2.1; the highest slot an OpenGLES stub uses is 1032 (RM-00936..00942) */
#include <stddef.h>
#include <stdio.h>

struct sweet_gli {
	void *rctx;
	const void *disp[SWEET_GLI_NSLOTS];
};

/* Hook called with rctx at the top of every wired wrapper and special slot (NULL = none). Returns 0 = this thread has a usable current
 * context; nonzero = none: the wrapper then returns 0 (or nothing) WITHOUT calling Mesa (a NULL-context call crashes backboardd,
 * research 36). Set by eagl_objc.m to sgl_ensure_current, which logs the failure. */
extern int (*sweet_gli_ensure_current)(void *rctx);
/* `bail` is `return` (void function) or `return 0`. */
#define SWEET_GLI_ENSURE(rctx, bail) do { if (sweet_gli_ensure_current && sweet_gli_ensure_current(rctx) != 0) bail; } while (0)

/* Fill t: rctx and every slot (wired wrapper or trap). */
void sweet_gli_init(struct sweet_gli *t, void *rctx);
size_t sweet_rect_rewrite(const char *src, size_t n, char **out);   /* G-11: sampler2DRect/texture2DRect -> sampler2D/texture2D with texel->normalised coordinates (see gli_table.c); *out malloc'ed */
void sweet_gli_note_tex_size(unsigned tex, unsigned w, unsigned h);   /* size of the texture libsweetgl bound a surface to (draw hook input) */
int sweet_mesa_unit_tex2d_size(unsigned unit, unsigned *w, unsigned *h);   /* eagl_mesa.c: level-0 size of the 2D texture bound to `unit` in the current Mesa context; 1 = found */
/* Called once per trapped slot with (slot, Apple name or "?"). Default logger writes to stderr; NULL = silent. */
void sweet_gli_set_log(void (*fn)(int slot, const char *name));
/* Used by the generated trap stubs: logs the first call of `slot`, returns 0. */
void sweet_gap(const char *area, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* SWEET GAP: <area> <what> xCOUNT (process), see gli_table.c */
long sweet_gli_trap(int slot);

/* Generated (gen_gli.py): defaults, Apple names, what each slot is wired to ("_mesa_X", "sgli_x", or NULL = trap),
 * and the count of integer-class arguments of the wired GL function (host test). */
extern const void *const sweet_gli_defaults[SWEET_GLI_NSLOTS];
extern const char *const sweet_gli_names[SWEET_GLI_NSLOTS];
extern const char *const sweet_gli_targets[SWEET_GLI_NSLOTS];
extern const unsigned char sweet_gli_nint[SWEET_GLI_NSLOTS];

/* Diagnostics (research 86 sections E/F/H, DRAFT-format0-batch): token-exact extension match, the GL_EXTENSIONS description printer (gli_special.c)
 * and the GL_FRAMEBUFFER status name (gli_table.c). All pure. */
int sweet_ext_has(const char *exts, const char *name);
void sweet_gl_describe(FILE *file, const char *ver, const char *rend, const char *ext);
const char *sweet_fbo_status_name(unsigned status);
int sweet_fbo_status_line(char *buf, size_t n, int fbo, const char *where, unsigned st, int type, int name, int w, int h, unsigned ifmt);
int sweet_fbo_sizes_line(char *buf, size_t n, int fbo, int r, int g, int b, int a);

/* research 90 L1/L2/L4 log-only diagnostics (SWEETGL_CENSUS=1, gli_table.c). Pure formatters and counters, exported for the host test. Kind flags: bit0 blur, 1 fbfetch, 2 cmatrix, 3 lum, 4 rect. */
unsigned sweet_census_kinds(const char *s, size_t n);
int sweet_census_class(int w, int h);                       /* 0 S (w >= 1000), 1 T (w <= 256 && h <= 256), 2 O */
void sweet_census_count(unsigned flags, int w, int h);      /* one draw; no flag = other */
void sweet_census_snapshot(unsigned long c[6][3], unsigned long *copytex);
int sweet_census_line(char *buf, size_t n, unsigned secs, const unsigned long c[6][3], unsigned long copytex);
int sweet_copytex_line(char *buf, size_t n, unsigned idx, unsigned target, int level, int xoff, int yoff, int x, int y, int w, int h,
    int readfb, int has_status, unsigned status, int has_dst, int dst, int dw, int dh, unsigned ifmt);
/* census extension (log-only): counters of other calls per target class, pixel readback stats, draw sequences. Exported for the host test. */
void sweet_census_count2(int which, int w, int h);          /* which: 0 blit 1 readpixels 2 generatemipmap 3 framebuffertexture2d 4 copyteximage2d; class by viewport */
void sweet_census_snapshot2(unsigned long c2[5][3], unsigned *drawseq);
int sweet_census_extra(char *buf, size_t n, const unsigned long c2[5][3], unsigned drawseq);   /* " blit S/T/O a/b/c readpix ... genmip ... fbotex ... drawseq N copyteximg ..." */
int sweet_census_line_full(char *buf, size_t n, unsigned secs, const unsigned long c[6][3], unsigned long copytex, const unsigned long c2[5][3], unsigned drawseq);
void sweet_px_stats(const unsigned char *b64, unsigned *nonzero, unsigned *maxa, unsigned *sum);   /* 4x4 RGBA8 patch: pixels with any nonzero byte, max alpha byte, sum of all 64 bytes */
int sweet_px_written(const unsigned char *b64);                 /* 0 = all 64 bytes still the 0xA5 sentinel (the read wrote nothing) */
int sweet_px_line(char *buf, size_t n, const char *kind, int cls, int fbo, int vw, int vh, const unsigned char *b64);
int sweet_px_stats_line(char *buf, size_t n);
int sweet_px_claim(int fbfetch, int fbo, long long now_s);   /* 1 = take a readback sample now (consumes the budget: blur 4 per fbo / 16 total, fbfetch 4 total >= 5 s apart) */
void sweet_px_reset(void);                                   /* host test */
/* time-armed rounds (SWEETGL_CENSUS=<N>, N >= 2); see gli_table.c. Clock = CLOCK_MONOTONIC seconds unless sweet_px_set_clock installs one (host test) */
struct sweet_px_rinfo {
	int round;                  /* round this claim belongs to (>= 1), 0 = none */
	int complete;               /* this claim completed the round (print the done line after the sample line) */
	unsigned csamples;
	int cap_c;                  /* print the cap line after the completion line */
	int exp_round;              /* a previous round timed out in this call (> 0): print its done line now */
	unsigned exp_samples;
	int cap_e;                  /* print the cap line after the timeout line */
	int cap_u;                  /* unbind claim reached the total: print `SWEETGL: PIXELS unbind cap` after the sample */
};
int sweet_census_parse(const char *env, long *arm_s);       /* on/off; *arm_s = 0 (early samples only, = value "1") or N >= 2 */
void sweet_px_configure(long arm_s);                        /* arm the schedule N s from now; only the first call with N >= 2 per process counts (until sweet_px_reset) */
int sweet_px_sched_on(void);
long long sweet_px_now(void);                               /* the clock the schedule uses (CLOCK_MONOTONIC seconds unless injected) */
void sweet_px_census_tick(void);                            /* what the 20 s census thread calls: expire a timed-out round, print its done line */
void sweet_px_set_clock(long long (*fn)(void));             /* host test; NULL = CLOCK_MONOTONIC */
int sweet_px_round_claim(int fbfetch, int fbo, long long now_s, struct sweet_px_rinfo *r);
void sweet_px_round_tick(long long now_s, struct sweet_px_rinfo *r);   /* expire a timed-out round without a draw */
int sweet_px_unbind_claim(long long now_s, struct sweet_px_rinfo *r);   /* part B: 1 = take an unbind sample now (r->round = attributed round) */
int sweet_px_unbind_cap_line(char *buf, size_t n);
int sweet_ub_eligible(int fbo, unsigned draws, int vw, int vh);         /* fbo != 0, >= 1 draw, vp w >= 1000, h >= 100, not 1125x2436 */
void sweet_ub_reset(void);                                  /* host test */
void sweet_tu_reset(void);                                  /* part A (texture units 0..7), host test */
void sweet_tu_active(int ctx, unsigned unit);               /* ctx = sweet_th_ctx_index(rctx), 0 = overflow slot */
void sweet_tu_bind(int ctx, unsigned target, unsigned tex);   /* GL_TEXTURE_2D and GL_TEXTURE_RECTANGLE_ARB 0x84F5 */
void sweet_tu_snapshot(int ctx, unsigned out[8]);
void sweet_px_unbind_rollback(int round, int *cap_u);       /* give an unbind claim back (gate skip) */
int sweet_th_info(unsigned name, int *w, int *h, int *att, int *del);
int sweet_prov_line(char *buf, size_t n, int round, int fbo, const unsigned tex[8]);
struct sweet_px_state { int prog, blend, bf[4], eq[2], mask[4], scissor, sbox[4], vp[4], depth, count; unsigned flags, mode; };   /* the values of the draw state line */
int sweet_census_kind_str(unsigned flags, char *buf, size_t n);   /* program kind letters in b f c l r order, `o` when none (needs n >= 7) */
int sweet_state_line(char *buf, size_t n, int round, int fbo, const struct sweet_px_state *s);
struct sweet_capdraws { unsigned draws, kinds[6]; long long verts; int pfirst, plast, blendlast; int fvp[4], lvp[4]; unsigned fmode, lmode; int fcount, lcount; };   /* kinds: b f c l r o */
int sweet_capdraws_line(char *buf, size_t n, int round, int fbo, const struct sweet_capdraws *c);
int sweet_capvp_line(char *buf, size_t n, int round, int fbo, const struct sweet_capdraws *c);   /* first / last draw since the bind: viewport, mode, count */
/* capv8 (batch 8, log-only; rework of capv7): what one early draw into a capture FBO samples and where its vertices go. Pure data for the formatter; gli_table.c fills it from GL queries and
 * in-process client-array reads. One draw prints several lines of at most SWEET_CAPV_MAX bytes each (the log tee splits lines at ~256 B, observed g113g): tag h (header), a (attribs 0..1, then 2..3),
 * v (vertices), u (uniforms, as many lines as needed). */
#define SWEET_CAPV_U 6                                      /* uniforms shown */
#define SWEET_CAPV_MAX 230                                  /* every line is at most 230 bytes including the prefix (a cut is marked ` ~cut`) */
enum { SWEET_CAPV_V_NONE = 0, SWEET_CAPV_V_VALUES, SWEET_CAPV_V_OOB, SWEET_CAPV_V_MAPPED, SWEET_CAPV_V_IDXBUF, SWEET_CAPV_V_IDXRANGE, SWEET_CAPV_V_CLI0, SWEET_CAPV_V_TYPE, SWEET_CAPV_V_DIS, SWEET_CAPV_V_NOFIRST, SWEET_CAPV_V_IDXQ, SWEET_CAPV_V_INST };   /* v: `-`, floats, `oob`, `mapped`, `idxbuf`, `idxrange`, `cli0`, `type`, `dis`, `nofirst`, `idxq`, `inst` (instanced draw: nothing read) */
enum { SWEET_CAPV_U_FLOATS = 0, SWEET_CAPV_U_UNIT, SWEET_CAPV_U_TYPE, SWEET_CAPV_U_NOLOC };   /* u: f,f,.. / unitN / t0x<type> / `-` (no location) */
struct sweet_capv {
	int vw, vh;                 /* the draw's viewport (printed right after the fbo) */
	int prog, count, first;     /* first < 0: not a DrawArrays (printed `-`) */
	unsigned mode, idxtype;     /* idxtype 0: not an elements draw (printed `-`) */
	unsigned tex[8];            /* bound GL_TEXTURE_2D names per unit, 0 = none */
	struct { int en, buf, sz, st; unsigned ty; unsigned long off; } at[4];   /* off is printed only when buf > 0; buf == 0 prints `cli` (a client pointer is never printed) */
	int vstate, vsz, nv; float v[4][4];   /* nv vertices of vsz floats */
	int hasidx, nidx; unsigned idx[4], imin, imax;   /* elements draws: the first indices, min / max over the first min(count,64) */
	int nu, umore;              /* uniforms in u[]; further active uniforms not shown */
	struct { char name[16]; int kind, nf, unit; unsigned type; float f[16]; } u[SWEET_CAPV_U];
};
/* One line of tag h / a / v / u (k: line number for a (0..1) and u (0..), 0 otherwise). Returns the length, or 0 when there is no such line (nothing written but a NUL). Every line is < SWEET_CAPV_MAX bytes. */
int sweet_capv_line(char *buf, size_t n, int fbo, int idx, const struct sweet_capv *v, char tag, int k);
int sweet_capv_cap_line(char *buf, size_t n);                                         /* `SWEETGL: PIXELS capv8 cap` */
int sweet_grid_classify(const unsigned char *b, size_t stride, int w, int h, char map[8][9]);   /* cells that are not '.', -1 when w or h < 8 */
int sweet_grid_written(const unsigned char *b, size_t stride, int w, int h);
unsigned sweet_grid_sentinel(const unsigned char *b, size_t stride, int w, int h);   /* pixels exactly 0xA5A5A5A5 */
int sweet_grid_line(char *buf, size_t n, int round, int fbo, int w, int h, int written, int nz, char map[8][9], unsigned a5);
int sweet_px_grid_avail(int fbo);
int sweet_px_grid_claim(int fbo, int *cap);
int sweet_px_grid_skip_once(int fbo);
int sweet_px_grid_skip_line(char *buf, size_t n, int fbo, int w, int h, unsigned long long bytes);
void sweet_px_grid_skip(void);
int sweet_px_grid_cap_line(char *buf, size_t n);
int sweet_px_round_done_line(char *buf, size_t n, int round, unsigned samples, int timeout);
int sweet_px_rounds_cap_line(char *buf, size_t n);
int sweet_px_line5(char *buf, size_t n, const char *kind, int cls, int fbo, int vw, int vh, const unsigned char *b64, int round, const int q[4]);
#define SWEET_DSEQ_N 120
struct sweet_dseq_run { unsigned seq, n; char kind[SWEET_DSEQ_N], cls[SWEET_DSEQ_N]; int fbo[SWEET_DSEQ_N]; };
char sweet_census_kind_char(unsigned flags);                 /* b blur, f fbfetch, c cmatrix, l lum, r rect, o other */
int sweet_dseq_arm(unsigned long blur_t_total);              /* 1 = a sequence was armed by this call (thresholds 100, 1000, 3000, once each) */
int sweet_dseq_active(void);
int sweet_dseq_record(char kind, char cls, int fbo, struct sweet_dseq_run *done);   /* 1 = the sequence is complete, *done filled */
int sweet_dseq_format(char *buf, size_t n, const struct sweet_dseq_run *r, unsigned *pos);   /* next line (<= 400 chars), 0 = nothing left */
void sweet_dseq_reset(void);                                 /* host test */
unsigned sweet_census_shader_kinds(void *rctx, unsigned shader);
unsigned sweet_census_program_kinds(void *rctx, unsigned program);
int sweet_gldebug_class(const char *msg);                   /* 0 ignore, 1 FBO/incomplete, 2 other error */
int sweet_gldebug_accept(const char *msg, char *note, size_t nn);   /* 1 = print msg; note = "SWEETGL: GLDEBUG dropped ..." line or "" */

/* Per-texture event history (SWEETGL_CENSUS=1, log-only, gli_table.c). Field use by kind: BIND a=target c=nth bind; TEXIMAGE a=target b=level c,d=w,h e=ifmt f=data!=NULL;
 * TEXSTORAGE a=target b=levels c,d=w,h e=ifmt; COPYIMAGE a=target b=level c,d=w,h e=ifmt; FBOTEX a=attachment b=level c=fbo (<0 unknown) e=textarget;
 * ATTACH a=surface id c,d=w,h e=format. Pure formatters, exported for the host test. */
enum { SWEET_TH_GEN = 1, SWEET_TH_BIND, SWEET_TH_TEXIMAGE, SWEET_TH_TEXSTORAGE, SWEET_TH_COPYIMAGE, SWEET_TH_FBOTEX, SWEET_TH_DELETED, SWEET_TH_ATTACH };
struct sweet_th_ev { unsigned long seq; int kind; unsigned a; int b, c, d; unsigned e; int f; int ctx; };   /* ctx: 1..16 = index of the rctx the hook ran in (sweet_th_ctx_index), 0 = unknown (printed without a tag) */
int sweet_th_event_str(char *buf, size_t n, const struct sweet_th_ev *e);
void sweet_th_record(unsigned name, struct sweet_th_ev ev);   /* assigns the sequence number; binds beyond the 3rd are only counted */
void sweet_th_reset(void);                                    /* host test */
void sweet_th_unattributed(void);
int sweet_th_ctx_index(void *rctx);                           /* small stable index per context pointer, 1..16, 0 when more than 16 */
int sweet_th_line(unsigned name, unsigned idx, char *buf, size_t n);   /* line idx of the history (<= 400 chars of events), 0 = no such line */
void sweet_th_print(unsigned name);                                    /* all lines to stderr */
int sweet_th_line_label(const char *label, unsigned name, unsigned idx, char *buf, size_t n);   /* label "" or e.g. "fbo-no-storage " (with the trailing space) */
int sweet_th_fbo_pick(unsigned *name, int *dropped);          /* next FBO-attached texture without storage event (max 8 per process, once per name); 0 = none */
void sweet_th_fbo_report(void);                               /* prints the picks: SWEETGL: TEXHIST fbo-no-storage tex N events M: ... / fbo-ring-dropped */
int sweet_th_stats_line(char *buf, size_t n);
/* eagl_mesa.c be_attach_surface: surface `surf_id` (w x h, format fmt) was attached to GL texture `tex`. No-op unless SWEETGL_CENSUS=1. */
void sweet_th_note_attach(unsigned tex, unsigned surf_id, unsigned w, unsigned h, unsigned fmt);

/* P-G0 slice c (research 113 s.7.2): glFlush / glFinish timing, env SWEETGL_FLUSHT=1, log-only, default off. See gli_table.c "flush timing". */
struct sweet_ft_win { unsigned flush, finish; unsigned long long flush_in_ns, finish_in_ns, flush_max_ns, finish_max_ns, last_exit_ns; };
int sweet_ft_parse(const char *e, int *bad);
int sweet_ft_init(const char *e);
int sweet_ft_enabled(void);
unsigned long long sweet_ft_ticks_to_ns(unsigned long long t, unsigned numer, unsigned denom);
int sweet_ft_line(char *buf, size_t n, int pid, unsigned sec, const struct sweet_ft_win *w);
int sweet_ft_note(int which, unsigned long long t0, unsigned long long t1, struct sweet_ft_win *done, unsigned *done_sec);
void sweet_ft_set_clock(unsigned long long (*fn)(void));   /* host test: returns ns */
/* LOWCLK lever real-flush: glFlush as a non-blocking flush behind a runtime switch, default off. See gli_table.c "real flush". */
void sgli_RealFlush(void *rctx);                                  /* gli_special.c: _mesa_Flush (submit, no wait) */
int sweet_rf_parse(const char *e, int *armed, int *def_mode);     /* env SWEETGL_REALFLUSH: unset / "" / "0" = not armed, "arm" = armed + mode 0, "1" / "2" = armed + that mode; else returns 1 (bad) */
int sweet_rf_init(const char *e);                                 /* 1 = install the hook (armed by the env, or the switch file exists) */
int sweet_rf_mode(void);                                          /* effective mode after the last poll: 0 glFinish, 1 real flush on display targets, 2 every glFlush */
void sweet_rf_counts(unsigned long long *real, unsigned long long *finish, unsigned long long *nodisp);
void sweet_rf_reset(void);                                        /* host test */
void sweet_rf_set_file(const char *path);                         /* host test: NULL = the default path */
void sweet_rf_set_gate(int (*fn)(void));                          /* host test: 1 = the kernel present gate is on */
void sweet_rf_set_disp(int (*fn)(void));                          /* host test: 1 = the draw framebuffer's colour target is a display surface */
void sweet_rf_set_clock(unsigned long long (*fn)(void));          /* host test: returns ns */
struct sweet_base_info { unsigned long long image, slide; const char *path; };
int sweet_base_line(char *buf, size_t n, int pid, const struct sweet_base_info *bi, unsigned long long size);   /* `SWEETGL: BASE pid P image 0x.. slide 0x.. size N path P` (path cut to 80 B: line < 200 B) */
int sweet_base_unavailable_line(char *buf, size_t n, const char *why);
int sweet_base_report(void);                                                     /* once per process, called by sweet_gli_init */
void sweet_base_set_resolver(int (*fn)(struct sweet_base_info *, const char **));   /* host test; NULL = default */
void sweet_base_reset(void);                                                     /* host test */
void sweet_ft_reset(void);                                 /* host test */
extern int sweet_ft_tb_fail;                               /* host test */

/* glGetIntegerv(GL_TEXTURE_BINDING_2D) of the current context (for attachImage:, whose first argument is a target). */
unsigned sweet_gli_bound_texture_2d(void);
unsigned sweet_gli_bound_level0(int *w, int *h);          /* bound 2D name; *w,*h = its level-0 size (0 = no storage) */
unsigned sweet_gli_fallback_storage(unsigned w, unsigned h);   /* RGBA8 level-0 storage on the bound 2D texture; returns the GL error (0 = ok) */
#endif
