# Injecting libsweetgl.dylib into backboardd

Tags: **[src file:line]** read in a file, **[obs]** a command/CI run, **[guess]** not verified, **[UNKNOWN]** needs a hardware boot or a reverse.
Nothing here ran on the phone. Built by CI (private repo `TheSmuks/pixelios-gpu-ci`, workflow `mesa-panfrost-ios`, stage 5).

## 1. Mechanism the reference ports use

Both put `DYLD_INSERT_LIBRARIES=<dylib>` into the environment of backboardd; neither patches backboardd's binary.
* **ios-sailfish** (same iOS 16.7 build): boot-arg `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib`; the kernel's exec path appends the variables
  to every exec whose process name matches [src ~/pixelios-artifacts/refs/ios-sailfish/README.md:63,85; kernel/docs/research/18-gpu-userland.md:134-141]. Dylib path
  `/usr/lib/sweetgl.dylib` (mode 755, ad-hoc signed, `-install_name /usr/lib/sweetgl.dylib`, `-exported_symbols_list` of 5 symbols) [src ios-sailfish/tools/sweetgl/build.sh:46-51].
* **hackios-xnu-userland**: `init` writes a "LIBRARY NAME..." list to sysctl `kern.exec_inject`; the kernel adds `DYLD_INSERT_LIBRARIES` to matching execs; libredwood is
  injected into backboardd [src research 22 s.1.3 (lines 77-80), U:userland/src/init.c:1348-1380, K:xnu/redwood.patch:609-690].
* Both depend on dyld honouring the variable, which needs a kernel answer (below, s.3). Their constructor picks the process by `getprogname()` (backboardd = EAGL shim,
  SpringBoard = other hooks) [src sailfish tools/sweetgl/sweetgl.c:297-315]. Ours does the same (`dylib_main.m`).

**Ours: the launchd job's `EnvironmentVariables`** (no kernel exec-inject, no boot-arg). The same mechanism already sets `CA_BACKDROP_SCALE` on backboardd in the sailfish root
[src ios-sailfish/tools/sweet-root-tweaks.py:93-96]. launchd on this port reads the combined cache `/System/Library/xpc/launchd.plist` (boot-arg `launchd_unsecure_cache=1`);
its jobs are keyed by plist PATH, the backboardd job has `Label` `com.apple.backboardd` [src tools/overlay-ios-root.sh:6-7,39; sailfish tools/patch-launchd-plist.py:28-33].
Template: `com.pixelios.backboardd-sweetgl.plist.in` (a fragment to merge, not a job).
**[UNKNOWN]** that launchd passes a `DYLD_*` key of EnvironmentVariables through unchanged [guess: yes, it does not filter; not read in launchd's binary]. Fallback if it does
not: sailfish's `sweetenv` kernel patches (0104/0163) in the kernel thread's hands, or fallback B in s.3.

## 2. Install path

`/usr/lib/libsweetgl.dylib`, root:wheel, mode 0755 (CI links with `-install_name /usr/lib/libsweetgl.dylib`; `otool -D` checked). Reason: sailfish uses `/usr/lib` and
its backboardd read it [src sailfish README:63]; a path outside the system trees may be refused by the sandbox profile [guess; our sandbox stand-in is permissive, kernel patch 0050].
CI artifact: `out/libsweetgl.dylib` of workflow artifact `mesa-panfrost-ios` (21.8 MB). Local copy of run 39988248534: `~/pixelios-artifacts/libsweetgl/libsweetgl.dylib`.
Sailfish zero-padded its dylib to 64 MiB because `push.sh` overwrites the file's extents in place from TWRP [src sailfish build.sh:54-56, push.sh:17]; the overlay builds a new image, no padding.

## 3. Code signing, AMFI, dyld policy on this port

* The dylib is **ad-hoc signed** in CI (`codesign -f -s -`, `Signature=adhoc`, valid, 5278 page hashes) [obs run 39988248534]. No Apple SDK or Apple binary is in git.
* **No trust cache** exists on this port (Image4/AMFI service stubbed, loads rejected) [src kernel/xnu/patches/0015]. The kernel's built-in AMFI policy marks every validly signed
  vnode on the root filesystem `CS_SIGNED` and `CS_PLATFORM_BINARY`, ad-hoc signatures included [src kernel/xnu/patches/0035 (sweet_amfi.c hunk "Every signed binary on the root filesystem"), 0036].
  The system binaries are themselves ad-hoc signed, so an ad-hoc dylib in `/usr/lib` on the root fs is the same class as them. The stub implements only `mpo_vnode_check_signature`:
  no library validation, no team-id check [src 0035/0036; no other hook in patches 0001-0105: grep]. **[UNKNOWN]** on hardware: that the dylib's signature is accepted when dyld
  maps it (`F_ADDFILESIGS_RETURN`), that the file must be on the MNT_ROOTFS mount (the overlay writes the root image, so yes) and not on the data volume.
* **BLOCKER (kernel thread): dyld policy.** dyld asks `__mac_syscall("AMFI", 0x5a, ...)` which `DYLD_*` variables to honour and honours none if it fails [src sailfish
  xnu/patches/0139-sailfish-amfi-dyld-policy.patch:8-12; research 18 s.1.5, 22 item 3]. Ours names the policy `"SWEET AMFI"` and sets no `mpo_policy_syscall`
  [src patches/0035 ctx: `.mpc_name = "SWEET AMFI"`; research 22 line 111] and no patch 0001-0105 contains a 0x5a handler [obs grep]. So today `DYLD_INSERT_LIBRARIES` is ignored
  [guess, high: sailfish needed 0139 for exactly this]. Needed: rename the policy `"AMFI"`, handle op 0x5a, return flags `0x7b` (copyin `{u64 in; u64 *out}`, copyout flags) = sailfish 0139.
  Our own implementation, not a copy. Also a boot with `sweetnull=...backboardd...` shows `sweetgl: loaded in backboardd` iff this works.
* **Fallback B (no kernel dyld policy):** add `LC_LOAD_DYLIB /usr/lib/libsweetgl.dylib` to `/usr/libexec/backboardd` and re-sign it ad hoc. Needs header room, a rootless
  signing tool for Linux (ldid) and gives up the "binaries stay untouched" property. **[UNKNOWN]** whether it works; not built.

## 4. When the constructor runs vs QuartzCore's renderer choice

* The constructor (`sweetgl_main`, `__TEXT,__init_offsets` entry [obs run 39988248534]) runs in dyld's initialiser phase, **before `main()`** of backboardd. Inserted libraries come first
  in the load order, and the dylib links OpenGLES, Foundation, IOSurface, so those are initialised and registered with libobjc before our initialiser [guess, dyld ordering not read].
  `objc_getClass("EAGLContext")` must therefore succeed; if not, `sweet_eagl_install` returns -ENOENT and it is logged **[UNKNOWN until a boot]**.
* QuartzCore chooses lazily: `CA::WindowServer::EAGLServer::renderer()` (0x1882eaf3c in the 20H392 cache) runs at the first `render_update` of the display server, after
  backboardd's main run loop is up; it sends `[[EAGLContext alloc] initWithAPI:3 properties:]` only when no Metal device exists and `CA_ENABLE_OGL` is not `0`, then
  `setParameter:0x260 to:` and `API`; `API != 3` means CPU renderer [src kernel/docs/research/33 s.1.1, obs-RE]. The swizzles are installed long before the first call, so the first
  `initWithAPI:` already lands in our `m_init`. The cached renderer (this+0x320) is never created earlier, so there is no "too late" window unless something sends `initWithAPI:` before main [guess].
* Metal is tried first: `CAMetalContextCreate` must return nil (empty `IOAcceleratorES` stub, patch 0084) [src research 33 s.1.2; guess for this phone]. hackios additionally empties the Metal
  device array in backboardd [src research 22 line 90-91]; not done here. Keep `CA_ENABLE_OGL` unset.
* The constructor probes the SweetMali user client (exclusive: 2nd client = EBUSY, may appear late = ENODEV: it retries 10x200 ms on ENODEV only). Absent/busy: it logs and returns, the
  swizzles are not installed, CA keeps the CPU renderer. Mesa itself is initialised lazily at the first `initWithAPI:` (`eagl_mesa.c screen_up`), not in the constructor.
  **Conflict:** the overlay's `org.pixelios.sweetmali-test` / `sweetgl-test` jobs also open the client; while injection is on, drop them or backboardd sees EBUSY.

## 5. Logging (userland cannot reach the kernel log directly)

Every line goes to three places (`slog` in `dylib_main.m`):
1. a write to **`/dev/null`**: kernel patch 0048 copies writes of processes named in boot-arg `sweetnull` (default `backboardd,SpringBoard`) to the console / persistent / USB-UDP log as
   `|backboardd| sweetgl: ...` (256 bytes per write, so lines are < 200) [src kernel/xnu/patches/0048-sweet-devnull-tee.patch]. This is the only channel from userland to `tools/xrun`'s
   listener and it is the one the existing test jobs use [src tools/overlay-ios-root.sh:9-11; tools/sweetmali-test/sweetmali_test.c:4]. launchd `StandardOutPath=/dev/console` stops jobs launching [src patch 0048 comment].
2. appended to **`/var/tmp/sweetgl.log`** (readable after the run from the data volume; the file is not truncated, delete between runs).
3. `os_log` subsystem `org.pixelios.sweetgl` (only useful on a running device).
Exported `int sweetgl_state` (1 installed, 0 not run, -errno refused). Env: `SWEETGL_DISABLE=1` skips everything, `SWEETGL_FORCE=1` runs in any process (default: backboardd only).
Expected lines: `sweetgl: loaded in backboardd pid N` / `SweetMali user client open` / `EAGLContext swizzles installed ...` (or the `NOT installing` variants).

## 6. Steps for tools/overlay-ios-root.sh (owned by another thread; not edited here)

1. New input `SWEETGL_DYLIB` = CI artifact `out/libsweetgl.dylib`. In the tar builder add `add('usr/lib/libsweetgl.dylib', open(dylib,'rb').read(), 0o755)`.
   Do NOT add a directory entry for `usr/lib` (the existing one must keep its mode; the script's `for d in ('usr', ...)` adds dir entries only for the paths it creates).
2. Merge the fragment into the cached plist, next to the loop that adds the `org.pixelios.*` jobs (idempotent, safe on an already overlaid image):
   ```python
   frag = plistlib.loads(open('tools/sweetgl-eagl/com.pixelios.backboardd-sweetgl.plist.in','rb').read().replace(b'@DYLIB@', b'/usr/lib/libsweetgl.dylib'))
   hit = [j for j in jobs.values() if j.get('Label') == 'com.apple.backboardd']
   assert len(hit) == 1, 'backboardd job not found'
   hit[0].setdefault('EnvironmentVariables', {}).update(frag)
   ```
3. When injecting, do not add the `org.pixelios.sweetmali-test`/`sweetgl-test` jobs (client is exclusive), or run them only in a separate image.
4. After building, `tools/check-ios-root.py` should assert: file `usr/lib/libsweetgl.dylib` exists, mode 755, is a Mach-O with platform iOS; backboardd's env contains the key.
5. Boot with `sweetnull=backboardd,...` (default already has it). The kernel needs the s.3 dyld-policy patch first, otherwise nothing is loaded and no line appears.
   Then read the run with `tools/xrun --dry-run --from-dir DIR` and grep `sweetgl:`.

## 7. CI checks (stage 5, run 39988248534, all [obs])

`vtool`: platform IOS, minos 16.0, sdk 17.0 (Xcode 15.0 runner SDK); arm64 only; `otool -L`: only `/usr/lib` and `/System/Library` dependencies, install name `/usr/lib/libsweetgl.dylib`;
`nm -um`: 250 undefined symbols, all `(from <system dylib>)` except the weak import `_sweet_gl_kernel_import` (kernel selector 5 wrapper, not written; linked with `-Wl,-U`, null at run time -> attach/tex fail ENOSYS);
defined inside (hidden): `_drmIoctl _drmGetVersion _drmGetCap _drmPrimeFDToHandle _sweet_drm_mali_open _sweet_eagl_install _sgl_context_create _st_api_create_context
_panfrost_drm_screen_create __mesa_EGLImageTargetTexture2DOES _sweet_gli_defaults`; exactly one export, `_sweetgl_state`; `__TEXT,__init_offsets` with 3 entries (`_sweetgl_main` + 2 static initialisers of libglsl.a);
`codesign --verify --strict` ok, ad hoc. Hidden-visibility note: the DRM emulation symbols are hidden (`-fvisibility=hidden`, Mesa's own flag) and `-exported_symbols_list` hides the rest; hidden symbols still resolve inside one image,
so nothing needed exporting; the real link failure was `libglsl_standalone.a` duplicating `_mesa_problem` & co of `libmesa.a` (run 39987422556), now excluded.

## 8. Unknowns (summary)

launchd keeps `DYLD_INSERT_LIBRARIES` from EnvironmentVariables; kernel 0x5a dyld policy (blocker, s.3); the ad-hoc dylib passes dyld's signature check in a platform process; `EAGLContext` class
registered before our constructor; sandbox allows `/var/tmp` writes and the SweetMali IOKit connection from backboardd (M5: the user client has no entitlement check yet); Mesa's `st_api_create_context`
surfaceless on panfrost (see README); whether any other backboardd code uses `EAGLContext` (all instances become ours, Apple's IMPs are not forwarded); static initialisers of libglsl running at load cost (not measured).
