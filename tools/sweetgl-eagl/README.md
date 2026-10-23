# sweetgl-eagl

EAGL shim for iOS backboardd on top of Mesa panfrost (plan: `DESIGN.md`). Tags: [obs] a command run here, [src] a file, [guess] unverified.

| File | State |
|---|---|
| `eagl_core.c/.h` | pure C, no ObjC/Mesa: context table, thread-local current, parameter store, surface attachments, `struct sweet_gl_backend` vtable. **Host-tested.** |
| `eagl_core_test.c`, `host-test.sh`, `mutation-check.sh` | fake-backend test and 8 mutants |
| `eagl_objc.m` | ObjC swizzle sketch. **Never compiled, never run** (no Apple SDK here). |
| `DESIGN.md` | Mesa entry points with file:line, IOSurface import, per-frame flow |
| `eagl_mesa.c` | **not written** (the backend: st_api_*, pipe_frontend_screen hooks, resource cache) |

```
./host-test.sh        # cc -Wall -Wextra -Werror run, then gcc ASan+UBSan run: PASS
./mutation-check.sh   # checks the baseline passes, then 8 sed mutants must each fail the test
```
[obs 2026-10-23] 190 checks pass plain and under ASan/UBSan; 8/8 mutants killed (the script first requires the unmutated source to pass).

## Unverified (do not build on these)

* ObjC method signatures (`attachImage:...` / `texImageIOSurface:...` return types, argument types, `getMacroContextPrivate` return layout, `dealloc` super call). Reverse from OpenGLES / QuartzCore with `tools/rea`. The sketch also does not call the original IMPs for contexts it did not create (sailfish does); needed if anything else in backboardd uses EAGL.
* That Mesa's `st_api_create_context` works with no drawable and a zeroed `st_visual` for ES 3.0 on panfrost; that `_mesa_EGLImageTargetTexture2DOES` is linkable (static build, hidden symbols) and yields a render-target-capable texture from an imported linear resource.
* The DRM-side contract: kernel selector 5 `importSurface` does not exist; pitch/alignment of CA surfaces vs panfrost linear rules.
* The `inverted` flag semantics; thread-safety under real CA render threads (one global mutex, callbacks inside it).
* `initWithAPI:2` returns a context (ES3 underneath) rather than nil: assumed harmless because CA tries 3 first (R33 s.1.1).
