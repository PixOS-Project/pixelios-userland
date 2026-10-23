# EAGL shim over Mesa panfrost: design

Tags: **[src file:line]** read in a file (Mesa = `~/pixelios-artifacts/scratch/mesa-sdm/mesa-26.2.5`, written `mesa/...`), **[obs]** a command run on this host,
**[guess]** not verified. Nothing here ran on the phone. Requirements come from `kernel/docs/research/33-ca-renderer-and-gles-requirements.md` (R33), plan from research 32 (R32).

## 1. Layers

```
backboardd / QuartzCore (CA::OGL::GLESContext)
  | ObjC messages            (eagl_objc.m: swizzles EAGLContext, UNBUILT)
eagl_core.c                  (context table, thread-local current, params, surface bookkeeping; host-tested)
  | struct sweet_gl_backend  (eagl_mesa.c: NOT WRITTEN)
Mesa gallium frontend ("st") + panfrost + pan_kmod
  | drmIoctl & co            (../sweetgl-mali/sweet_drm_mali.c: DRM emulation, host-tested)
SweetMali IOKit user client  (xnu patch 0105; selector 5 importSurface still missing)
```
Mesa 26.2.5 has no `osmesa` (option gone, [src R32 s.4.2]) and the dri/EGL/GLX frontends need a window system we do not have. The usable
frontend is the one below the DRI glue: `st_api_*` of the state tracker, driven with **no drawable** (surfaceless), exactly what sailfish does structurally (`sweetgl.c:141-174`,
reference only). Every GL call CA makes goes through a GLI table (R33 s.2.2) filled from Mesa's `_mesa_*` entry points; that table is not designed here.

## 2. EAGLContext -> gallium context

| EAGL / backend callback | Mesa entry point | Source |
|---|---|---|
| process start | `sweet_drm_mali_open()` -> fd; `panfrost_drm_screen_create(fd, &cfg)` (dups the fd, caches screens) giving a `struct pipe_screen *`. `panfrost_create_screen(fd,&cfg,ro)` is the direct form | [src mesa/src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c:122-126; mesa/src/gallium/drivers/panfrost/pan_public.h:18] |
| once | allocate `struct pipe_frontend_screen`, set `.screen`, `.get_egl_image`, `.validate_egl_image` (and `.get_param` if the st asks) **before** any context, the st reads them at creation | [src mesa/src/gallium/include/frontend/api.h:312-346]; "before any context" [src sweetgl.c:118 reference] |
| `create_context` | `st_api_create_context(fscreen, &attribs, &err, NULL)` with `attribs.profile = API_OPENGLES2`, `major = 3`, `minor = 0` (3.1 is available on v9, R33 s.2.3, so 3.0 is the floor CA needs), `visual` zeroed (no drawable), `options` = hand-built defaults (no xmlconfig, R32 s.4.2). Returns `struct st_context *` = our `bctx`; `*out_api = 3` on success. | [src mesa/src/mesa/state_tracker/st_context.h:376-401 (attribs), :468-472; st_manager.c:967; mesa/src/mesa/main/menums.h:48 `API_OPENGLES2`] |
| `make_current` | `st_api_make_current(st, NULL, NULL)` (NULL st = unbind). Per thread; Mesa keeps its own TLS current context, our core adds the "current on one thread only" rule | [src st_context.h:474-477; st_manager.c:1149] |
| `destroy_context` | `st_destroy_context(st)`; unbind first (the core refuses to destroy a current context) | [src st_context.h:422-423] |
| `flush` | `st_context_flush(st, 0, NULL, NULL, NULL)` then wait; simplest: call `_mesa_Finish()` with the context current (`st_finish` + frontbuffer flush). glFlush **is** glFinish for us | [src st_context.h:425-428; mesa/src/mesa/main/context.c:1634-1642 `_mesa_Finish`, :1652 `_mesa_Flush`; st_cb_flush.c:105-112 `st_glFinish`] |
| `setParameter:to:` (0x260) | nothing: stored by the core, returns YES (what CA needs, R33 s.2.1) | [src R33 s.2.1] |
| `+setCurrentContext:` / `+currentContext` | core `sgl_set_current` / `sgl_current` (+ `sgl_context_user` returning the EAGLContext object) | eagl_core.h |
| `-API` | core returns 3 for every live context; `initWithAPI:2` is served by an ES3 context, never an ES2 one | R33 s.1.1: ES2 is rejected, API must be 3 |

Rejection rule (R33 s.1.1: `renderer()` returns NULL -> CPU renderer unless `[eagl API] == 3`): `sgl_context_create` fails with `EPROTO` (and frees the backend context)
if the backend reports an API other than 3, so `initWithAPI:` returns nil and CA keeps its software path instead of running on a context CA would reject anyway.

## 3. IOSurface -> pipe_resource (render target and sampler)

`attachImage:toCoreSurface:invertedRender:` and `texImageIOSurface:...` both need "GL texture whose storage is the IOSurface's memory". Path (the EGLImage route; the
st already implements it, we only supply the two hooks):

1. ObjC layer: `IOSurfaceGetID/GetBaseAddress/GetBytesPerRow/GetWidth/GetHeight/GetPixelFormat` -> `struct sweet_gl_surface`; `sweet_gl_import_surface` (TODO) asks the kernel
   for the GPU VA (selector 5 `importSurface`: id in; handle, VA < 4 GiB, size out; **not in the kernel yet** [src ../sweetgl-mali/sweet_drm_mali.h:15]).
2. Fake dma-buf: an unlinked temp file `ftruncate`d to the surface size, registered with `sweet_drm_mali_register_surface(fd, id, size)`; Mesa takes the buffer size from `lseek(SEEK_END)`
   [src ../sweetgl-mali/sweet_drm_mali.h:30-36; R32 s.2.4].
3. Build `struct winsys_handle { type = WINSYS_HANDLE_TYPE_FD, handle = fd, stride = bytes_per_row, modifier = DRM_FORMAT_MOD_LINEAR }` and a `struct pipe_resource` template (TEXTURE_2D, format from the IOSurface
   pixel format: BGRA8 -> `PIPE_FORMAT_B8G8R8A8_UNORM` first, bind = `PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW`), then
   `screen->resource_from_handle(screen, &templ, &whandle, usage)` = `panfrost_resource_from_handle` -> `panfrost_bo_import` -> `drmPrimeFDToHandle` (our emulation).
   [src mesa/src/gallium/include/frontend/winsys_handle.h:15,26-88; mesa/src/gallium/include/pipe/p_screen.h:268-271; mesa/src/gallium/drivers/panfrost/pan_resource.c:350,2589; R32 s.2.4]
4. Cache the resource per (surface id, format, pitch) in a small LRU so writer and reader share one (R33 s.3 item 2). The cache lives in `eagl_mesa.c`, not in the core.
5. Bind it: the frontend's `get_egl_image(fscreen, image, out)` fills `struct st_egl_image { texture = res, format, level 0, layer 0, internalformat }` (ref-counted: `pipe_resource_reference`);
   the `image` cookie is a pointer to a small struct the backend builds on the stack, then call `_mesa_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)&img)` with the target texture bound
   (`attach_surface`: the texture name `tex` CA generated; `tex_image_surface`: the currently bound one).
   [src mesa/src/gallium/include/frontend/api.h:137-155,330-333; mesa/src/mesa/main/teximage.c:3594-3613; mesa/src/mesa/state_tracker/st_cb_eglimage.c:344-356]
   The inverted flag (`invertedRender`, `invert:`) has no Mesa counterpart; CA passes NO in the paths seen (R33 s.2.1). The core stores it; the backend must reject YES until a flip is designed. [guess]
6. CA then does `glFramebufferTexture2D(GL_COLOR_ATTACHMENT0, tex)` itself (R33 s.2.1); the resource therefore must be a render target. Whether Mesa's FBO completeness accepts an imported linear BGRA texture on
   panfrost: [guess]. Alignment of CA's pitch against panfrost's linear rule: [guess], fallback = copy (R32 s.2.4).
7. `attach_surface(surf = NULL)` = `glDeleteTextures`-side forgetting: drop the cache reference; the GL texture itself stays CA's.

## 4. Per-frame flow

```
CA: make_buffer_current(surface)  -> glGenTextures/glBindTexture/glTexParameteri x4
                                  -> [eagl attachImage:tex toCoreSurface:iosurface invertedRender:NO]   -> sgl_attach_surface (import + EGLImageTarget)
                                  -> glGenFramebuffers/glFramebufferTexture2D
CA: draws layers (texImageIOSurface for sampled layer surfaces -> sgl_tex_image_surface)
CA: glFlush / glFinish            -> sgl_flush -> _mesa_Finish: GPU fragment job done (WAIT via sweet_drm_mali syncobj/seqno)
CA: IOMobileFramebufferSwapEnd    -> kernel SweetFramebuffer reads the linear BGRA surface (unchanged display path, R33 s.3)
```
CA's fences are not seen on the swap path (R33 s.3), so the shim makes flush synchronous (`glFlush == glFinish`), as sailfish had to. Costs a CPU stall per frame; upgrade is a kernel-waitable GPU fence. [guess]

## 5. What the core owns and why

Pure bookkeeping so it can be tested without Mesa or ObjC: contexts (16), per-thread current (thread-local; a context is current on one thread at a time, destroy refuses a current one), parameter store,
attachment table (texture name -> surface record; replace on re-attach, forget on detach, backend called first so a failed import leaves state unchanged), argument validation (the product `bytes_per_row*h` is
64-bit so a hostile surface cannot overflow it), and the "must be current" rule for attach/tex/flush (`EPERM`). Backend callbacks run under the one mutex (`ponytail:` ceiling documented in `eagl_core.h`).

## 6. Not designed here

The GLI dispatch table (`getMacroContextPrivate`, slot list R33 s.2.2), texture-rect emulation, shader text patching, the colour mode call `0x8a79`, and non-BGRA8 formats (R33 s.4 P1/P2).
