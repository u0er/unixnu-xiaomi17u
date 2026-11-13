# AGENTS.md — unixnu-xiaomi17u (XNU / SM8850 nezha bring-up)

## Cross-app coordination (READ FIRST)
Multiple agents across different apps (Qoder, CodeArts, ...) share this repo, the CI,
and the physical device. There is no shared internal chat between apps.

- Before pushing to main, dispatching CI, or trialing on the device: read
  [COORDINATION.md](COORDINATION.md), check the current main HEAD + CI concurrency, and
  rebase on latest main (`xnu/patches/*` are cumulative — keep the other agents' work).
- After acting: update your row in COORDINATION.md.
- Device trials are exclusive: confirm the device is free and get user consent first.
  Temporary `fastboot boot` only; no partition writes.
- Verify CI run status/conclusion via api.github.com yourself; do not trust verbal claims.

## Build / patch facts
- CI workflow `.github/workflows/xnu.yml` is `workflow_dispatch` only (no push trigger);
  default DEVELOPMENT / PANDORA / macOS 13.5.
- Patches in `xnu/patches/` apply to a pristine pure-LF XNU tree; keep patch context pure
  LF and new-file hunks must include `new file mode 100644`.
- Kernel cmdline budget is 608 bytes; keep trials single-variable over a verified baseline.