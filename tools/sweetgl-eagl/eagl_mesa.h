/* eagl_mesa: the Mesa (gallium state tracker + panfrost) backend of eagl_core. Built in CI only (needs the Mesa tree). */
#ifndef EAGL_MESA_H
#define EAGL_MESA_H
#include "eagl_core.h"

/* Backend vtable to hand to sgl_set_backend(). Mesa/panfrost are brought up lazily by create_context. */
const struct sweet_gl_backend *sweet_gl_mesa_backend(void);
/* glGetString(name) on the context current on this thread (NULL if none). For the link test and diagnostics. */
const char *sweet_gl_mesa_get_string(unsigned name);
#endif
