/* gli_table: the GLI dispatch table CoreAnimation calls GL through (research 33 s.2.2).
 * Layout (what -[EAGLContext getMacroContextPrivate] returns): { void *rctx; void *disp[1036]; }; CA calls
 * `disp[slot](rctx, gl args...)` with slot numbers of Apple's table (gli_slots.txt, generated into gli_table_gen.c).
 * Every slot is non-NULL: wired ones call Mesa's `_mesa_*` entry point of the thread's current context, the rest log once and
 * return 0. CA's server path never sends +setCurrentContext: [obs-RE research 34 s.1.2], so every wired wrapper and special first
 * calls sweet_gli_ensure_current(rctx) (eagl_core's sgl_ensure_current; rctx = the sgl_ctx), which makes that context current on the
 * calling thread unless it already is. */
#ifndef GLI_TABLE_H
#define GLI_TABLE_H
#define SWEET_GLI_NSLOTS 1036   /* [src refs/ios-sailfish/tools/sweetgl/gen-dispatch.py:62, R33 s.2.1] */

struct sweet_gli {
	void *rctx;
	const void *disp[SWEET_GLI_NSLOTS];
};

/* Hook called with rctx at the top of every wired wrapper and special slot (NULL = none). Set by eagl_objc.m. */
extern void (*sweet_gli_ensure_current)(void *rctx);
#define SWEET_GLI_ENSURE(rctx) do { if (sweet_gli_ensure_current) sweet_gli_ensure_current(rctx); } while (0)

/* Fill t: rctx and every slot (wired wrapper or trap). */
void sweet_gli_init(struct sweet_gli *t, void *rctx);
/* Called once per trapped slot with (slot, Apple name or "?"). Default logger writes to stderr; NULL = silent. */
void sweet_gli_set_log(void (*fn)(int slot, const char *name));
/* Used by the generated trap stubs: logs the first call of `slot`, returns 0. */
long sweet_gli_trap(int slot);

/* Generated (gen_gli.py): defaults, Apple names, what each slot is wired to ("_mesa_X", "sgli_x", or NULL = trap),
 * and the count of integer-class arguments of the wired GL function (host test). */
extern const void *const sweet_gli_defaults[SWEET_GLI_NSLOTS];
extern const char *const sweet_gli_names[SWEET_GLI_NSLOTS];
extern const char *const sweet_gli_targets[SWEET_GLI_NSLOTS];
extern const unsigned char sweet_gli_nint[SWEET_GLI_NSLOTS];

/* glGetIntegerv(GL_TEXTURE_BINDING_2D) of the current context (for attachImage:, whose first argument is a target). */
unsigned sweet_gli_bound_texture_2d(void);
#endif
