# Mesa panfrost for Darwin/iOS: patch series (not built)

Makes the gallium panfrost driver configurable for `host_machine.system() == 'darwin'` (iOS arm64) with a fake libdrm and
user-client hook points, for a Valhall Mali-G78 (`GPU_ID 0x92020010` = `PAN_PROD_ID(9, 2, 2)`, Pixel 6a).
Plan and file:line facts behind it: `pixelios-kernel/docs/research/32-mesa-panfrost-ios-plan.md` (private super repo).

Tags: **[src file:line]** read from the pinned Mesa tree (line numbers are in the *pristine* 26.2.5 tree, before the patches),
**[obs]** a command I ran, **[guess]** not verified. **Nothing here has been built or run: there is no Mac, Apple SDK or
LLVM on this host.**

## Pin

* Mesa **26.2.5** release tarball `mesa-26.2.5.tar.xz`, sha256 `794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5`
  [obs: `sha256sum /tmp/gpu18/mesa.tar.xz`; same value as research 32; **not** compared with an upstream checksum, archive.mesa3d.org
  gave no usable `.sha256` on 2026-10-22].
* The tarball has no `.git`; the patches were made against a `git init` + commit of the unpacked tree (author of the
  patches: `TheSmuks`). Not usable against Mesa main (`f1fbde1c`): `pan_model.c` differs there [src research 32 s.3].

## Apply order and check

```
tar -xJf mesa-26.2.5.tar.xz && cd mesa-26.2.5
for p in /path/to/tools/mesa-patches/000*.patch; do git apply --check "$p" && git apply "$p" || break; done   # needs a git repo: git init + commit first, or use plain `patch -p1 < $p`
```
`git apply --check` of each patch, in order, on a pristine tree: all four OK [obs, 2026-10-23, in a second fresh unpack].
`patch -p1 --no-backup-if-mismatch < $p` (what `mesa-panfrost-ios.yml.draft` does) also works, the mail header is ignored [guess, not run].
Apply 0001 to 0008 in this order (0006 does not exist): 0003 adds `darwin_drm.h` which 0004 extends; 0003 needs the include dir from 0002.

## What each patch does

1. **0001 panfrost/model: add Mali-G78.** One `VALHALL_MODEL(PAN_PROD_ID(9, 2, 2), 0, "G78", "G78", ...)` row after the `(9,0,3)` row of
   `src/panfrost/model/pan_model.c`. Decode: `PAN_ARCH_MAJOR/MINOR/PRODUCT_MAJOR` (`pan_model.h:25-28`) of 0x92020010 = 9, 2, 2;
   `PAN_PROD_ID` is `(major<<16)|(minor<<8)|prod` (`pan_model.h:38-39`); `pan_get_model(gpu_id, variant)` compares `gpu_prod_id` and
   `gpu_variant` (`pan_model.c`, end of file); its only gallium user is `pan_device.c:43-48`, which bails if it returns NULL. `gpu_variant` is 0:
   `panfrost_kmod.c` never sets it [src grep]. `"G78"` is already a counter set name (`perf/mali_perf.h:187`).
   **The tilebuffer sizes (16384/8192) and rates (2,4,8,32,32,8) are a copy of the G57 row [guess]**, marked `[guess]` in the code comment and the commit message.
2. **0002 fake libdrm + meson gating.** New option `darwin-drm` (bool, default false, `meson.options`). In `meson.build`, a new `elif get_option('darwin-drm')` branch
   next to the `system_has_kms_drm` / `dep_libdrm = null_dep` branches (pristine `meson.build:1920-1935`) makes `dep_libdrm` a
   `declare_dependency` with include dir `src/panfrost/darwin_drm` (error if the host is not darwin). New `src/panfrost/darwin_drm/xf86drm.h`: declarations only, for exactly
   the libdrm symbols the panfrost build references [obs: grep of `src/panfrost/lib`, `src/gallium/drivers/panfrost`, `src/gallium/winsys/panfrost`, `util/u_sync_provider.c`,
   `renderonly.c`, `u_screen.c`]: `drmIoctl drmGetVersion drmFreeVersion drmGetCap drmCloseBufferHandle drmPrimeHandleToFD drmPrimeFDToHandle drmSyncobj{Create,Destroy,HandleToFD,FDToHandle,ImportSyncFile,ExportSyncFile,Wait,TimelineWait,Reset,Signal,TimelineSignal,Query2,Transfer}`,
   `drmVersion`, `DRM_CLOEXEC`, `DRM_RDWR`. Signatures follow `util/u_sync_provider.c:55-105` and the call sites; the struct layout of `drmVersion` is libdrm's
   (checked field by field against libdrm 2.4.124 `xf86drm.h:142-152`, and `DRM_CLOEXEC`/`DRM_RDWR` against `libdrm/drm.h:878-879`; every prototype above was compared with `xf86drm.h:133,609-612,834-837,936-961`, no difference) [obs: Debian `libdrm-dev` 2.4.124-2 header read 2026-11-15]. Declarations after libdrm (MIT, (c) the libdrm authors), reduced to the symbols listed.
   `src/panfrost/darwin_drm/darwin_drm_weak.c`: weak ENOSYS definitions, added to `libpankmod_lib` (`src/panfrost/lib/kmod/meson.build`) only with the option, so that stage-1 host
   tools (`panfrost_compile` links `libpankmod_lib`, research 32 s.4.3) link; the dylib's strong definitions win [guess: ld64 weak/strong behaviour, not run].
   **Deliberate deviation from the brief:** no fake `drm.h` / `panfrost_drm.h`. Mesa's own `include/drm-uapi/drm.h` has a non-Linux branch using `<sys/ioccom.h>`
   (`drm.h:36-65`) and `panfrost_drm.h` only includes `drm.h`; duplicating them would risk wrong ioctl numbers. No ioctl number is defined by these patches.
   Side effects of `dep_libdrm.found()`: `-DHAVE_LIBDRM`, and `u_sync_provider.c`, `renderonly.c`, `winsys/panfrost/drm` get compiled [src `util/meson.build:317`,
   `gallium/auxiliary/meson.build:287`, `gallium/meson.build:116`].
   **Use:** `-Ddarwin-drm=true` on the meson command line of **both** stages (stage 1 is also a darwin host build), or `[project options] darwin-drm = true` in the cross/native file [guess: [project options] support depends on the meson version, >= 0.56 per meson docs, not run].
3. **0003 panfrost_kmod.c on Darwin.** `<sys/eventfd.h>`/`<sys/timerfd.h>` (`panfrost_kmod.c:12-13`) are included only `#ifndef __APPLE__`. The perf-session code that uses
   eventfd/timerfd/`sched_setscheduler(SCHED_FIFO)` (`panfrost_kmod.c:657-967`) is compiled out under `__APPLE__`; `perf_create` returns NULL, the other perf ops are left NULL
   (`pan_kmod_perf_*` are called only from `src/panfrost/perf/pan_perf.c:76,123`, a tool/ds library, not gallium). The brief said "pipe-based or stubbed": stubbed.
   Wait translation: `DRM_IOCTL_PANFROST_WAIT_BO` takes an *absolute* deadline in Linux (`panfrost_kmod.c:429-445`, callers `pan_bo.c:158` with 0 or `INT64_MAX`
   from `pan_context.c:592` etc.). Under `__APPLE__` the kmod sends a **relative** ns timeout instead: new `src/panfrost/darwin_drm/darwin_drm.h`,
   `darwin_drm_abs_timeout_to_rel_ns()` (`INT64_MAX` stays, a past deadline becomes 0, else `abs - os_time_get_nano()`). This makes the user client independent of the clock
   `os_time_get_nano()` reads (`util/os_time.c:57-61`; whether Darwin's `CLOCK_MONOTONIC` matches the kernel's timebase is research 32 s.5 row J, [guess]).
   **The emulated `WAIT_BO` therefore sees a relative timeout.** `drmSyncobjWait`/`TimelineWait` still get Mesa's absolute `os_time_get_nano()` value (`pan_fence.c:44-48`, `pan_jm.c:166`);
   the dylib's implementation should call the same helper.
4. **0004 BO mmap/munmap hook points.** Under `__APPLE__`, `pan_kmod_bo_mmap()` (`pan_kmod.h:765`, was `os_mmap(dev->fd, offset)`) calls `darwin_drm_bo_map(fd, size, prot, flags, offset)` and
   `panfrost_bo_munmap()` (`pan_bo.c:346`, was `os_munmap`) calls `darwin_drm_bo_unmap(addr, size)`. Both are declared in `darwin_drm.h` and have **no definition in Mesa** (strong symbols from the dylib; a
   missing one is a link error on purpose). `offset` is whatever the emulated `DRM_IOCTL_PANFROST_MMAP_BO` returned in `drm_panfrost_mmap_bo.offset`; Mesa treats it as opaque
   (`panfrost_kmod.c:414-425`), so the user client can return the mapped user address there.
5. **0005 time marks for the submit -> BO wait gap (P-G1 slice 12b, T-6717).** Under `__APPLE__`, `sweet_gapt_mark(1)` runs at the end of `panfrost_batch_cleanup()` (`pan_job.c`),
   `sweet_gapt_mark(2)` at the entry of `panfrost_batch_init()` (`pan_job.c`) and `sweet_gapt_mark(3)` at the entry of `panfrost_bo_cache_fetch()` (`pan_bo.c`). The declaration is in
   `darwin_drm.h`; `pan_bo.c` holds a WEAK no-op definition (an embedder without gap timing links unchanged), libsweetgl's strong definition replaces it (it stores one clock reading
   per mark and thread, see `sweet_drm_mali.c` "Mesa time marks"). A mark changes no Mesa state, submit order or wait result. Linux builds are untouched. [obs 2026-11-03: 0001..0005
   apply in order with `patch -p1` on a fresh unpack of the pinned tarball; the ci-local mesa stage result is in the T-6717 proof]
6. **0006/0007 and 0008 (T-9577, T-34eb).** 0007 is the split tiler heap probe (`SWEETGL_SPLITHEAP`, `pan_jm.c`); there was no README entry for it until now. **0008 overlap flag
   (T-34eb, `SWEETGL_OVERLAP`, default off, applies after 0007).** Env read once per process: unset/""/"0" off, "1" on, anything else one loud stderr line and off; "1" also needs
   `SWEETGL_SPLITHEAP=1` (else one loud line and off). `__APPLE__` and `PAN_ARCH >= 6` only. (a) `struct panfrost_batch` gets `sweet_heap_half`, set to -1 in `panfrost_batch_init()`
   (`pan_job.c`, runs for every batch) and set to the half 0007 picks in `jm_emit_tiler_desc()`; the mode part of 0007's `sweet_splitheap_half()` is split out as
   `sweet_splitheap_mode()` (same behaviour, same log lines). (b) With overlap on, one process-wide mutex covers the whole of `GENX(jm_submit_batch)`; the previous submit's state
   (device, FS-ok, heap half, sorted handle sets with WRITE / all, from `batch->bos` only) is consumed at the start of each batch and set again only by a successful FS submit.
   (c) New `pan_sweet_ovl.h`: pure `sweet_ovl_check()` = rule R0-R4 of `docs/reviews/T-34eb-proof.md` H1; the host test includes it. (d) A safe vertex/tiler submit carries
   `PANFROST_JD_REQ_SWEET_OVL` (`1u << 31`, defined in `include/drm-uapi/panfrost_drm.h` next to `PANFROST_JD_REQ_CYCLE_COUNT`, Mesa-private, consumed by the SweetMali libdrm shim,
   never sent to a Linux kernel); never the FS submit. (e) stderr: `SWEETGL: OVERLAP on pid P`, then `SWEETGL: OVERLAP checked N safe N no_prev_fs N half_same N half_unknown N r2 N r3 N insync N`
   at the first decision and every 4096th. Host test: `tools/sweetgl-mali/sweet_drm_mali_ovl_test.c` (rule + shim), mutants `ovl-*` in `mutation-check.sh` (`mutation-check.sh MESA_DIR ovl`).
   Not run on hardware; Mesa itself is not compiled by the host test (only `pan_sweet_ovl.h` and the shim), the ci-local mesa stage is the first real compile of `pan_jm.c` [guess: clean].

## Hook points the user client / dylib must provide (Mesa-side contract; no kernel code here)

| Hook | Declared | Mesa call site [src] | Contract |
|---|---|---|---|
| all `drm*` of `xf86drm.h` | 0002 | see research 32 s.2.2 table | `drmGetVersion` must return name `"panfrost"`, version >= 1.1 (`panfrost_kmod.c` dev_create); 1.3 recommended (research 32 s.0 item 6, [guess]) |
| `drmIoctl` | 0002 | `pan_kmod.h` `pan_kmod_ioctl` | request numbers are the Darwin encodings of `DRM_IOWR(DRM_COMMAND_BASE + n, struct)` from Mesa's `drm-uapi/panfrost_drm.h`; decode with the same macros (`request & 0xff` minus `DRM_COMMAND_BASE`) |
| prime fd import (`drmPrimeFDToHandle`) | 0002 | `pan_kmod.c:159`, then `lseek(fd, 0, SEEK_END)` as size at `pan_kmod.c:172` (size 0 or -1 = error) | **Not implemented.** The dylib must hand Mesa a real fd whose `lseek(SEEK_END)` gives the IOSurface size (research 32 s.2.4: unlinked temp file, [guess]) and map `fd -> IOSurface` inside `drmPrimeFDToHandle`. `panfrost_resource_from_handle` only accepts `WINSYS_HANDLE_TYPE_FD` (`gallium/drivers/panfrost/pan_resource.c:360`), so the frontend must import by fd, not KMS handle. Export (`drmPrimeHandleToFD`) can return -ENOSYS |
| syncobj emulation | 0002 | `pan_context.c:1111,1217`, `pan_fence.c`, `pan_jm.c:93,166` | **Not implemented.** Needs working create/destroy/wait/import/export; design in research 32 s.2.5 [guess] |
| `darwin_drm_bo_map` / `_unmap` | 0004 | `pan_kmod.h:765`, `pan_bo.c:346` | see 0004 |
| `WAIT_BO` timeout | 0003 | `panfrost_kmod.c:429-445` | relative ns, `INT64_MAX` = forever; return -1 + `errno = ETIMEDOUT` on timeout |

## NOT done

* No definitions of the `drm*` functions, no ioctl decoder, no user client, no kernel side, no syncobj/prime-fd emulation, no frontend (EAGL shim, driconf; research 32 s.4.4, s.7).
* No change to `panthor_kmod.c`: its `os_mmap(fd, ...)` (`panthor_kmod.c:304`) is not redirected (never reached, the version string is `"panfrost"`) [guess].
* Not covered: `src/panfrost/vulkan`, `ds`, `perf`/`tools`, `kraid/hw_runner` (they include `<xf86drm.h>` too; build with `-Dtools= -Dvulkan-drivers=` as in the CI draft; `-Dtools=panfrost` in stage 1 builds `panfrost_texfeatures`/perf, **may fail to link or compile** [guess]).
* G78 tilebuffer sizes / rates are placeholders (0001).
* The CI draft (`tools/ci/mesa-panfrost-ios.yml.draft`) still has to be amended: patch path (`tools/mesa/` there, `tools/mesa-patches/` here), drop the `cp -R tools/mesa/darwin-drm` line (the dir is part of the patch), add `-Ddarwin-drm=true` to both meson setups.
* Whether the rest of the Darwin configure (`-Dplatforms= -Dgallium-drivers=panfrost ...`, `with_gallium_drm`-gated parts, `kmsro`) configures and links: research 32 s.4.2 reasoned from source only, never run.

## Verification done on this host

* `git apply --check` + `git apply` of 0001..0004 in order on a fresh unpack: OK [obs].
* `clang -fsyntax-only` as Darwin without the Apple SDK: `tools`-side harness (`~/pixelios-artifacts/scratch/mesa-darwin-tools/syntax.sh`, kept out of the repo) passes for `pan_model.c`,
  `darwin_drm_weak.c`, `pan_kmod.c`, `panfrost_kmod.c`, `panthor_kmod.c` [obs]. It is **not** an iOS compile: it uses the host glibc headers with `-U__linux__ -D__APPLE__ -D__MACH__`,
  an own stub `<sys/ioccom.h>`/`<machine/endian.h>`, and shadow `sys/eventfd.h`/`sys/timerfd.h` that `#error` if included (negative control with `-U__APPLE__` fails as expected).
  `pan_bo.c` (0004 hunk) could not be checked: it needs generated headers (`shader_stats.h` needs `mako`, then `nir.h` etc.). meson changes were not run (no meson/bison/flex here).
