# Roadmap

What works is in [docs/STATUS.md](https://github.com/PixOS-Project/pixelios-site/blob/main/docs/STATUS.md) (every row says whether it was *observed on hardware*, *read from source* or is a *guess*).
The port-wide roadmap (kernel, Wi-Fi, GPU) is in [pixelios-kernel ROADMAP.md](https://github.com/PixOS-Project/pixelios-kernel/blob/main/ROADMAP.md).
This page lists what concerns the iOS root, as of 2026-10-29. Order and estimates are guesses, not promises.

## Milestones

1. **iPhone GUI on the Pixel 6a: reached** (observed 2026-10-22). Lock screen, home screen, Settings, touch.
2. **Wi-Fi: in progress** in the kernel (firmware boots under XNU, observed 2026-10-26; no scan or join yet).

## Done (observed on the phone or the build host)

- Root build without sudo (`tools/build-ios-root-rootless.sh`, observed on the build host 2026-10-22, see `docs/ROOTLESS_BUILD.md`).
- Host check before a flash (`tools/check-ios-root.py`), including the HFS+ B-tree straddle guard added after later boots failed with
  `BTOpenPath returned (5)` (observed 2026-10-25, see the comment in `tools/check-ios-root.py`).
- Boot times: lock screen about 108 s after the boot command from a migrated root, about 346 s on the first boot after a fresh flash
  (runs bs-q2 / bs-q1, 2026-10-25).

## In progress (on work branches, not on `main`)

- **GPU client library** `libsweetgl` (branch wip/gpu-mesa): the lock screen and the Settings app draw through it (observed,
  2026-10-26 / 2026-10-27); home-screen icons are still black tiles on that path.
- **`bluetoothd` job dropped** from the root until Bluetooth is ported (branch boot/ba5-drop-bluetoothd; observed: no more
  restarts every ~10 s, runs ba5-inj8-boot1/boot2, 2026-10-28).

## Next

- **Boot selector / power-on boot without a PC** (kernel side: iOS image in `boot_a`, stock `boot_b` as the way back): planned, not done.
- Fresh-flash boot time: the first boot spends most of its time in data migration on the CPU (observed 2026-10-25); skipping the
  migration is not an option. With 8 CPUs instead of 1 the lock screen came at +344 s instead of +1249 s (runs bs-m6 / bs-m3, 2026-10-25).

## Not started

- Audio, camera, sensors, modem.

## Where help is most useful

- Hardware captures from other Tensor devices (see [CONTRIBUTING.md](CONTRIBUTING.md) for how to submit them).
- Knowledge of which iOS 16 daemons a phone without Apple hardware can drop or must keep, with sources.
- Host-side tests for the root build and the root check.
