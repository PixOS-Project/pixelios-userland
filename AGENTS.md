# AGENTS.md

This repo is the PixOS iOS userland side (root-filesystem content, daemons and tools that run in the iOS root on the phone). PixOS is the project
name; the `pixelios-*` repo, directory and remote names are legacy and stay as they are. Read `README.md` before touching anything.

Hard rules (reversible only; the human runs the phone; no secrets, Apple binaries or IPSW contents in git; tag every claim as observed on hardware /
read from source / guess; never assume, check against a cited source), the workflow (commits by path with a worker trailer, one heavy job at a time)
and the safety guards are kept once, in the super repo's `AGENTS.md` and `docs/RULES.md`; they apply here unchanged. The user pre-approves flashing
`userdata` with the iOS root through `tools/fflash-userdata.sh` with a stated reason (kernel repo); any other destructive step needs the user.

Do not publish, push to a public remote or change repo visibility without the user saying so.
