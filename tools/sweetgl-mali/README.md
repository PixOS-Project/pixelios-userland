# sweetgl-mali: libdrm/panfrost emulation over the SweetMali user client

User-space half that lets Mesa panfrost (patched by `../mesa-patches/0001-0004`) talk to our iOS kernel GPU driver (xnu patches 0104/0105, IOKit
service `SweetMali`, boot-arg `sweetmali=1`). Mali counterpart of sailfish `tools/sweetgl/sweet_drm.c`. Adapted in structure (one mutex, lazy device
context, `request & 0xff` ioctl decode, `/dev/null` device fd), nothing copied that is Adreno specific.

Tags: **[src file:line]** read from a file, **[obs]** a command I ran on this host, **[guess]** not verified.
**Nothing here has been built for iOS or run on the phone** (no Mac, no Apple SDK on this host). The only run is the Linux host test below.

## Files

* `sweet_drm_mali.c/.h`: the implementation. Compiles against Mesa's headers (`xf86drm.h` and `darwin_drm.h` from patches 0002/0004, `drm-uapi/panfrost_drm.h`)
  and either `<IOKit/IOKitLib.h>` or, with `-DSWEET_DRM_HOST_TEST`, `fake_iokit.h`.
* `fake_iokit.h/.c`: in-memory SweetMali "kernel" for the host test (selectors 0-4, BO map/unmap, one-shot error injection, call log).
* `sweet_drm_mali_host_test.c`, `host-test.sh MESA_DIR`, `mutation-check.sh MESA_DIR`.

## Host test (observed)

Prepare MESA_DIR once: unpack `mesa-26.2.5.tar.xz` into an empty dir, `git init`, apply the four patches in order (see `../mesa-patches/README.md`). It only supplies headers.
```
./host-test.sh MESA_DIR              # cc -O1 -Wall -Wextra -Werror; runs; prints PASS/FAIL
./mutation-check.sh MESA_DIR         # 4 deliberate breakages of a copy of sweet_drm_mali.c; each must make the test fail
```
[obs 2026-10-23] gcc: `PASS: 172 checks, 0 failed`; clang: same; gcc `-fsanitize=address,undefined`: same.
Mutation check: wrong param id, wrong req bit, dropped seq, off-by-one handle: all 4 killed (3, 3, 12, 8 failed checks).
What the test does not cover: real concurrency (the fake never blocks), real IOKit semantics, the real kernel.

## What it implements

| Mesa call | Behaviour |
|---|---|
| `drmGetVersion` / `drmFreeVersion` | malloc'd `drmVersion`, name `"panfrost"`, 1.3.0 (Mesa checks the name at `pan_kmod.c:72`, needs >= 1.1 [src research 32 s.2.2 row 1]). NULL + `errno=ENODEV` if SweetMali is not there. |
| `drmGetCap` | `-EINVAL` (errno too), `*value = 0` |
| `drmIoctl` | `DRM_IOCTL_GEM_CLOSE` by exact request, else `((req>>8)&0xff)=='d'` and `(req&0xff)-DRM_COMMAND_BASE` as the panfrost command number (the low 16 bits are the same in Linux `asm/ioctl.h` and Darwin `sys/ioccom.h` encodings [guess: Darwin layout from memory]). Failure: -1 + errno. |
| `GET_PARAM` | from the cached `struct sweet_mali_info`: PROD_ID = `gpu_id>>16`, REVISION = `gpu_id&0xffff`, SHADER_PRESENT, AS_PRESENT, JS_PRESENT, L2/TILER/MEM/MMU/THREAD_FEATURES, MAX_THREADS, THREAD_MAX_WORKGROUP_SZ, THREAD_MAX_BARRIER_SZ, TEXTURE_FEATURES0-3, AFBC_FEATURES; constants: ALLOWED_JM_CTX_PRIORITIES = MEDIUM only, SELECTED_COHERENCY = NONE (31). Every other id and `pad != 0` is `EINVAL`, as Linux does for unknown ids. |
| `CREATE_BO` | selector 1 `(size, exec)`; exec = 1 unless `NOEXEC`. `HEAP` is backed eagerly with the requested size (the kernel has no alloc-on-fault, research 32 s.2.6 item 1). `WB_MMAP` accepted and ignored. `HEAP|WB_MMAP`, unknown flags, size 0, `pad` -> `EINVAL`. Returns the kernel handle as the DRM handle, `offset` = GPU VA, `size` = the kernel's 16 KiB-rounded size. |
| `GET_BO_OFFSET` | the VA from the table; unknown handle `ENOENT` |
| `MMAP_BO` | returns cookie `handle << 32`; no mapping yet. Heap BOs: `EINVAL` (Linux does the same; Mesa never maps them, `pan_bo.c:400-410` [src research 32]). |
| `darwin_drm_bo_map` | cookie -> `IOConnectMapMemory64(conn, memoryType=handle, ..., kIOMapAnywhere \| kIOMapWriteCombineCache)`; maps once per BO, later calls return the same address. `size` larger than the BO -> `MAP_FAILED`/`EINVAL`. |
| `darwin_drm_bo_unmap` | finds the BO by address, `IOConnectUnmapMemory64`; unknown address -1/`EINVAL`. `drmCloseBufferHandle` also unmaps a still-mapped BO. |
| `SUBMIT` | selector 3 `(jc, req)`; `req = 1` iff `PANFROST_JD_REQ_FS`; `CYCLE_COUNT` accepted, ignored; other bits, `jc == 0`, `pad` -> `EINVAL`. `in_syncs`, `bo_handles`, `jm_ctx_handle` are ignored. `out_sync` (checked before the call, `ENOENT` if unknown) gets the returned seqno. |
| `WAIT_BO` | **timeout_ns is RELATIVE** (patch 0003 converts in `panfrost_kmod.c` under `__APPLE__`; `INT64_MAX` = forever, `<= 0` = poll). Waits for the device's last submitted seqno (a superset of the BO's own, because the bo list is ignored). Timeout -> -1/`ETIMEDOUT`. Unknown handle `ENOENT`. |
| `MADVISE` | `retained = 1`; `madv > DONTNEED` `EINVAL`; unknown handle `ENOENT` |
| `SYNC_BO`, `QUERY_BO_INFO`, `SET_LABEL_BO`, `JM_CTX_*`, `PERFCNT_*` | `ENOSYS` (not reached at version 1.3 on JM [src research 32 s.2.2 rows 12-16]) |
| `drmCloseBufferHandle` | the table is the authority: unknown/closed handle -> `EINVAL` without calling the kernel. If the kernel refuses (e.g. its 5 s wait for the last job timed out) the record is kept and the error returned. |
| syncobj | table of `{seqno}` (0 = signalled, `NOFENCE` = created unsignalled/reset). Create/Destroy/Signal/Reset/TimelineSignal/Transfer/Wait/TimelineWait as in research 32 s.2.5. `Wait*` take an **absolute** `os_time_get_nano()` deadline, converted with `darwin_drm_abs_timeout_to_rel_ns()` (patch 0003); past deadline = poll; timeout -> -1/`ETIME`. `WAIT_ALL` waits for the largest seqno, otherwise the smallest (in-order queue), `first_signaled` set accordingly. `WAIT_AVAILABLE` returns without waiting. The wait loops over 1 s kernel slices and does **not** hold the mutex. Timeline points must be 0. |
| `ExportSyncFile`, `HandleToFD` | a fresh `open("/dev/null")` fd recorded as `fd -> seqno` (`ExportSyncFile` of a fenceless syncobj: `EINVAL`) |
| `ImportSyncFile`, `FDToHandle` | copy the recorded seqno; unknown fd `EINVAL` |
| `drmSyncobjQuery2` | `ENOSYS`: research 32 does not design it, it is not reachable from the JM path, and the value Linux reports for a binary syncobj is unchecked (rule 7) |
| `drmPrimeFDToHandle` | fd must have been registered with `sweet_drm_mali_register_surface(fd, iosurface_id, size)` (checks `lseek(SEEK_END) == size`); then calls the **kernel selector 5, which does not exist: stub returning `-ENOSYS`** (`kernel_import_surface`, marked TODO). Unregistered fd `-EINVAL`. |
| `drmPrimeHandleToFD` | `-ENOSYS` |
| `sweet_drm_mali_open()` | connects on first use and returns a `dup` of a `/dev/null` fd: pass it as the `fd` of `panfrost_create_screen`; Mesa dups and closes it [src research 32 s.2.1]. |

Return conventions follow libdrm as I remember it (`drmIoctl`-based calls: -1 + errno; `drmGetCap`/`drmPrime*`: -errno): [guess, libdrm source not at hand]. Mesa only tests `!= 0` / `>= 0` (`pan_fence.c:48-50`, `pan_kmod.c:159`) [src].

## Selector contract (kernel side, IOConnectCallMethod)

0 query (struct out `sweet_mali_info`, 88 bytes), 1 boNew (in size, flags; out handle, va, size), 2 boClose (in handle), 3 submit (in jc, req; out seq), 4 wait (in seq, relative us),
`IOConnectMapMemory64(memoryType = handle)`: all as in `xnu/patches/0105-sweet-mali-iokit-user-client.patch` `SweetMali.cpp` header comment and `externalMethod` [src].
Kernel behaviours this code relies on, all [src 0105]: handles are slot+1 and the lowest free slot is reused; `boNew` accepts only flag bit 0 and size <= 256 MiB; `wait` clamps to 60 s and returns `kIOReturnTimeout` when not done;
`boClose` waits (<= 5 s) for the last submit; only one client may be open at a time (second `IOServiceOpen` fails: `EBUSY`/`ENODEV` here).
**Selector 5 `importSurface` (to be added to the kernel):** in `IOSurface id`; out `handle, GPU VA (< 4 GiB), size`. The handle must be in the same table as `boNew` handles (so `boClose` frees it and `IOConnectMapMemory64` can map it).
Seqno 0 must never be returned by `submit` (0 means signalled here). Errors map `kIOReturn* -> errno` in `kr_errno()`.

## Mesa integration (not done on a Mac yet)

* Build `sweet_drm_mali.c` into the dylib that the frontend links (plan: research 32 s.4.4), with `-I MESA/include -I MESA/src/panfrost/darwin_drm -I MESA/src` and `-framework IOKit`, e.g.
  `xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Werror -dynamiclib -framework IOKit ... sweet_drm_mali.c` [guess: not run].
* Symbols the dylib must export (strong definitions): `drmIoctl drmGetVersion drmFreeVersion drmGetCap drmCloseBufferHandle drmPrimeHandleToFD drmPrimeFDToHandle drmSyncobj{Create,Destroy,HandleToFD,FDToHandle,ImportSyncFile,ExportSyncFile,Wait,TimelineWait,Reset,Signal,TimelineSignal,Query2,Transfer}`
  (all are defined here; list from `xf86drm.h` of patch 0002 [src]) and `darwin_drm_bo_map`, `darwin_drm_bo_unmap` (patch 0004: no weak fallback in Mesa, a missing one is a link error on purpose).
  Plus the three `sweet_drm_mali_*` functions for the EAGL shim. Default symbol visibility is needed (no `-fvisibility=hidden` on this file).
* "Replaces libdrm": with `-Ddarwin-drm=true` (patch 0002) `dep_libdrm` is only the fake header dir, so there is no libdrm to link; the calls resolve against this dylib. The weak ENOSYS definitions of `darwin_drm_weak.c` (patch 0002) must lose to ours: ld64 weak/strong behaviour [guess, not run].
* `os_time_get_nano()` (used by `darwin_drm.h`) must resolve from Mesa's util library or the host application [guess].
* The EAGL shim must call `sweet_drm_mali_open()` for the screen fd, and `sweet_drm_mali_register_surface()` for each IOSurface it wants to import.

## Not done / known ceilings

* Kernel selector 5 (surface import) and everything IOSurface: import returns `ENOSYS`; export unsupported. Pitch/AFBC rules of imports untouched (research 32 s.2.4).
* Params the kernel struct does not carry return `EINVAL`: `THREAD_TLS_ALLOC` (Mesa falls back to max threads), `SYSTEM_TIMESTAMP`, `SYSTEM_TIMESTAMP_FREQUENCY` (the frequency is a `required` query at version >= 1.3, `panfrost_kmod.c:210-216`; in a release build it silently becomes 0, in a debug build `assert(!required)` fires: either report 1.2 or add the value to the kernel struct once the GPU timestamp clock is measured), `TILER_PRESENT`, `L2_PRESENT`, `STACK_PRESENT`, `CORE_FEATURES`, `COHERENCY_FEATURES`, `JS_FEATURES*`, `NR_CORE_GROUPS` (Mesa does not ask for them [src]).
* `WAIT_BO` waits for the whole device, not the BO's own last job (upgrade: record the seqno per BO from the `bo_handles` list of `SUBMIT`). `in_syncs` ignored (in-order queue, fragment-after-tiler ordering is the kernel's job: research 32 s.2.2 row 9).
* Syncobj fds are `/dev/null` dups recorded by fd number; if a caller closes one and the number is reused by an unrelated open, a stale record is still believed (Mesa exports, imports and closes back to back). `FDToHandle` copies the seqno (not a shared live object). A fenceless syncobj (created unsignalled, or reset) cannot be waited on (`EINVAL`), no `WAIT_FOR_SUBMIT` blocking. Timeline points other than 0: `EINVAL`. `Query2`: `ENOSYS`.
* One device context per process; SweetMali allows a single client, so a second process gets `EBUSY`.
* `HEAP` BOs are fully backed (128 MiB wired for Mesa's tiler heap, kernel cap 1 GiB total [src 0105 `SWEET_MALI_MAX_WIRED`]).
* The wait loop for `INT64_MAX` never gives up (a hung GPU hangs the caller); the kernel does not report GPU resets.
