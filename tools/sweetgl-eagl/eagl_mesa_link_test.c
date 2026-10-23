/* sweetgl-eagl-link-test: create an ES3 context through the EAGL core + Mesa backend, make it current, print GL_RENDERER.
 * Exit 0 only if every step worked. Built in CI; not run on hardware yet. */
#include <errno.h>
#include <stdio.h>
#include "eagl_core.h"
#include "eagl_mesa.h"

int main(void)
{
   int r = sgl_set_backend(sweet_gl_mesa_backend());
   if (r) { printf("SWEETGL: FAIL set_backend %d\n", r); return 1; }
   sgl_ctx *c = sgl_context_create(SGL_API_ES3);
   if (!c) { printf("SWEETGL: FAIL context_create errno=%d\n", errno); return 1; }
   r = sgl_set_current(c);
   if (r) { printf("SWEETGL: FAIL set_current %d\n", r); return 1; }
   const char *s = sweet_gl_mesa_get_string(0x1F01);   /* GL_RENDERER */
   printf("SWEETGL: renderer=%s version=%s\n", s ? s : "(null)", sweet_gl_mesa_get_string(0x1F02) ?: "(null)");
   r = sgl_flush(c);
   sgl_set_current(NULL);
   sgl_context_destroy(c);
   if (!s || r) { printf("SWEETGL: FAIL flush=%d\n", r); return 1; }
   printf("SWEETGL: PASS\n");
   return 0;
}
