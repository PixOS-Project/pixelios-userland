# PixOS: iOS userland

The recipe that builds the **iOS 16.7 root filesystem** for the PixOS port (Apple XNU and iOS on a Google Pixel 6a,
Tensor G1). The root is flashed to the phone's `userdata` partition and mounted by the patched kernel.

> **AI-driven development.** This project is written mostly by AI coding agents under the direction of a human maintainer, who owns the phone
> and decides what is flashed or published. Read the code before you trust it. Details: [docs/AI_DEVELOPMENT.md](docs/AI_DEVELOPMENT.md).

This repository holds scripts only. **No Apple binaries are distributed**: the IPSW restore image is downloaded on your
own machine and the root is assembled from it there.

Status 2026-11-15 (*observed on hardware* in the maintainers' runs, run labels on [STATUS.md on pixelios-site](https://github.com/PixOS-Project/pixelios-site/blob/main/docs/STATUS.md)):
the root built with this recipe, together with the kernel repository's shim and XNU patches, boots to launchd, the lock screen and the
home screen on a Pixel 6a. Now working: GPU acceleration, Wi-Fi with real internet, Bluetooth, and touch. GPU: smooth UI at about 50 fps
with the GPU at its lowest clock (150 MHz), rendered 50.6 / 50.6 pooled on two boots (runs 155b, 8177); drawn presents reaching the panel
49.0/s (run 18e1), so the 50/s bar is not met yet. Wi-Fi: joins WPA2 networks and gets real internet (DNS, HTTPS; pixnet 9/9), about
190-285 Mbit/s download (155b, 8177). Bluetooth: pairs and connects devices, tested with a headset (demo boot, job 1308). Touch: a real
finger works (same boot). **No audio playback yet.** Known issues: UDP download loses packets under heavy load; the iOS daemon `lskdd`
exits and restarts about once a second. This configuration runs 5 of the 8 CPU cores; a boot needs the PixOS shim and the `userdata` root
built here. Release note: [2026-11-15](https://github.com/PixOS-Project/pixelios-site/blob/main/docs/RELEASE-2026-11-15.md); kernel side:
[pixelios-kernel README](https://github.com/PixOS-Project/pixelios-kernel).
Disclaimer: AI was used heavily to make this run on Pixel 6a; it is based on ios-sweet.

- `tools/build-ios-root.sh`, `tools/build-ios-root-rootless.sh`: build the root (the rootless variant needs no sudo,
  see `docs/ROOTLESS_BUILD.md`)
- `tools/mkiosroot.sh`, `tools/mkinit.py`, `tools/sweet-init.S`, `tools/patch-launchd-plist.py`:
  image assembly, init and launchd tweaks
- `tools/sweet-root-tweaks.py`: root tweaks (our own, clean-room, written from the spec
  `docs/cleanroom/sweet-root-tweaks-spec.md` in the super repo)
- `tools/ios-root-plan.py`, `tools/check-ios-root.py`: plan and verify a root before flashing

Four files in `tools/` (`mkiosroot.sh`, `mkinit.py`, `sweet-init.S`, `patch-launchd-plist.py`) derive from
[ios-sweet](https://github.com/dipa-studio/ios-sweet) (MIT, Copyright (c) 2026 dipa-studio), taken at commit `11da58b` with our
changes applied; the licence text is in [LICENSE-ios-sweet.txt](LICENSE-ios-sweet.txt).

Fix-forward (user decision, 2026-10-25): `origin/main` history holds `upstream-patches/tools__sweet-root-tweaks.py.patch` (blob `4a855b6827`),
which shares 30 of 177 lines with a third-party-derived source [observed, line comparison]. `tools/sweet-root-tweaks.py` was replaced by a
clean-room version (commit `9e4e38d`) [src]. The old patch cannot be unpublished and stays in history; no history rewrite was done.
This is a maintainer's record, not a legal opinion.

Other repositories: [pixelios-kernel](https://github.com/PixOS-Project/pixelios-kernel) (shim, XNU patches, boot tools),
[pixelios-site](https://github.com/PixOS-Project/pixelios-site) (docs and status). Our code is MIT; read
`SECURITY.md` and `CONTRIBUTING.md`.
