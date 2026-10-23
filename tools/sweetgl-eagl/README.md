# sweetgl-eagl

EAGL shim for iOS backboardd on top of Mesa panfrost (plan: `DESIGN.md`). Tags: [obs] a command run here, [src] a file, [guess] unverified.

| File | State |
|---|---|
| `eagl_core.c/.h` | pure C, no ObjC/Mesa: context table, thread-local current, parameter store, surface attachments, `struct sweet_gl_backend` vtable. **Host-tested.** |
| `eagl_core_test.c`, `host-test.sh`, `mutation-check.sh` | fake-backend test and 9 mutants |
| `eagl_objc.m`, `eagl_private.h` | ObjC layer: swizzles the private EAGLContext selectors (R33 s.2.1) onto eagl_core + the GLI table. MRC (`-fno-objc-arc`, reason in the file header). **Compiles for iOS arm64 with `-Wall -Wextra -Werror`** (private CI repo, workflow `eagl-objc`, run 39983933553, [obs 2026-10-23]); never linked into a dylib, never run. All signatures [guess]. |
| `gli_table.c/.h`, `gli_special.c`, `gli_slots.txt`, `gen_gli.py` -> `gli_table_gen.c`, `gli_fake_gl.c` | the `{rctx, disp[1036]}` table CA calls GL through: 389 slots wired to Mesa `_mesa_*` (wrapper drops rctx), 8 hand-written, the rest log-once traps. **Host-tested** |
| `gli_table_test.c`, `gli-host-test.sh`, `gli-mutation-check.sh` | fake `_mesa_*` generated from the same list; checks every slot non-NULL, right target called with args not shifted, traps log once, specials; 7 mutants |
| `machosyms.py` | lists the defined global symbols of Mach-O static libs (`python3 -I machosyms.py $(find LIBS -name '*.a') > syms.txt`): input of `gen_gli.py --symbols` |
| `DESIGN.md` | Mesa entry points with file:line, IOSurface import, per-frame flow |
| `eagl_mesa.c/.h`, `eagl_mesa_link_test.c` | the Mesa backend (st_api_*, pipe_frontend_screen hooks, IOSurface resource cache) and a test main. **Compiles and links for iOS arm64 in CI only** (workflow Stage 4, private CI repo run 39981978917, [obs 2026-10-23]); never run, no host test (needs Mesa + the phone). |

```
./host-test.sh        # cc -Wall -Wextra -Werror run, then gcc ASan+UBSan run: PASS
./mutation-check.sh   # checks the baseline passes, then 8 sed mutants must each fail the test
HEADERS=<mesa-26.2.5>/include SYMBOLS=syms.txt ./gli-host-test.sh      # regenerates, fails if gli_table_gen.c/gli_fake_gl.c are stale, then tests (plain + ASan/UBSan)
HEADERS=... SYMBOLS=... ./gli-mutation-check.sh                         # baseline first, then 9 mutants (3 of them mutate the generator)
# regenerate: python3 -I gen_gli.py --slots gli_slots.txt --headers <mesa>/include --symbols syms.txt --out gli_table_gen.c --fake-out gli_fake_gl.c
```
[obs 2026-10-23] gli: 4874 checks pass plain and under ASan/UBSan; 9/9 mutants killed. Generated with Mesa 26.2.5 headers and the symbols of the CI-built libs (run 39981978917).
[obs 2026-10-23] 201 checks pass plain and under ASan/UBSan; 9/9 mutants killed (the script first requires the unmutated source to pass).

## Unverified (do not build on these)

* GLI table: slot numbers/names come from two reference ports (iOS 16.5 table), not from the 16.7 OpenGLES binary; every wired wrapper and special calls `sweet_gli_ensure_current(rctx)` (= `sgl_ensure_current`, rctx = the `sgl_ctx`) because CA's server path never sends `+setCurrentContext:` [obs-RE research 34 s.1.2]; a context bound on another thread stays refused (-EBUSY, call ignored); 13 slots trap (APPLE fences 463-468, ES1 palette OES 1011-1014, `glGetFramebufferParameteriAPPLE`, `glResolveMultisampleFramebufferAPPLE`, `glCopyTextureLevelsAPPLE`); `glFinishObjectAPPLE`/`glTestObjectAPPLE` = finish/true [guess]; slot count 1036 and the layout `{rctx, disp[]}` are from refs/R33, not re-derived. `glGetAttachedObjectsARB`/`glGetHandleARB` (sailfish had them) have no slot in either list: if CA reaches them they are unknown slots (they trap, logged).
* `attachImage:toCoreSurface:invertedRender:` takes a GL **target**, not a texture name as R33 s.2.1 says (both reference ports read it so and use the bound texture); the shim asks Mesa for `GL_TEXTURE_BINDING_2D`. Apple also accepts 0x8d41 (GL_RENDERBUFFER) [research 34]; not added: the backend only attaches textures, so it is not a one-line change (CA never sends it).
* ObjC method signatures (`attachImage:...` / `texImageIOSurface:...` return types, argument types, `getMacroContextPrivate` return layout, `dealloc` super call). Reverse from OpenGLES / QuartzCore with `tools/rea`. The sketch also does not call the original IMPs for contexts it did not create (sailfish does); needed if anything else in backboardd uses EAGL.
* That Mesa's `st_api_create_context` works with no drawable and a zeroed `st_visual` for ES 3.0 on panfrost; that `_mesa_EGLImageTargetTexture2DOES` is linkable (static build, hidden symbols) and yields a render-target-capable texture from an imported linear resource.
* The DRM-side contract: kernel selector 5 `importSurface` does not exist; pitch/alignment of CA surfaces vs panfrost linear rules.
* The `inverted` flag semantics; thread-safety under real CA render threads (one global mutex, callbacks inside it).
* `initWithAPI:2` returns a context (ES3 underneath) rather than nil: assumed harmless because CA tries 3 first (R33 s.1.1).
