/* gli_special.c: GLI slots that are not a plain `_mesa_X` call (named with `=` in gli_slots.txt). Each takes rctx + GL args. */
#include "gli_table.h"

extern void _mesa_Finish(void);
extern void _mesa_DeleteProgram(unsigned);
extern void _mesa_DeleteShader(unsigned);
extern unsigned char _mesa_IsProgram(unsigned);
extern const unsigned char *_mesa_GetString(unsigned);
extern void _mesa_GetIntegerv(unsigned, int *);

#define GL_TEXTURE_BINDING_2D 0x8069

/* glGetString(0x8a1e): QuartzCore asks for a second extension list with an Apple-private enum; Mesa would raise
 * GL_INVALID_ENUM, and CA treats NULL as "none" [src R33 s.2.3: `search_extension_list` is NULL-safe]. */
const unsigned char *sgli_GetString(void *rctx, unsigned name)
{
	(void)rctx;
	return name == 0x8a1e ? 0 : _mesa_GetString(name);
}

/* glFinishObjectAPPLE(object, name): no APPLE object fences here; finish everything [guess, R33 s.4 item 10]. */
void sgli_FinishObject(void *rctx, unsigned object, int name) { (void)rctx; (void)object; (void)name; _mesa_Finish(); }
/* glTestObjectAPPLE(object, name): after a finish the object is complete: GL_TRUE [guess, R33 s.4 item 10]. */
unsigned char sgli_TestObject(void *rctx, unsigned object, unsigned name) { (void)rctx; (void)object; (void)name; _mesa_Finish(); return 1; }

/* glDeleteObjectARB(obj): obj is a shader or a program; GL 2.0 names are one namespace in Mesa [src sailfish eagl.c:sgl_DeleteObjectARB does the same split]. */
void sgli_DeleteObjectARB(void *rctx, unsigned obj)
{
	(void)rctx;
	if (_mesa_IsProgram(obj)) _mesa_DeleteProgram(obj); else _mesa_DeleteShader(obj);
}

/* debug markers and glFramebufferParameteri APPLE colour mode (R33 s.4 item 9: accept as no-op). */
long sgli_nop(void) { return 0; }

unsigned sweet_gli_bound_texture_2d(void)
{
	int t = 0;
	_mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &t);
	return (unsigned)t;
}
