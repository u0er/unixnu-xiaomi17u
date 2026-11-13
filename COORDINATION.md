# Cross-app coordination anchor

Shared by every agent working on this repo (Qoder, CodeArts, ...). Different apps have
no shared internal chat, so this file + main HEAD + the CI run list are the
synchronization points. Read it before acting; update your row after acting.

## Protocol (every agent)
1. Before pushing to main or dispatching CI: read this file, GET the current main HEAD
   and the target blob SHA, and check CI for an `in_progress` run. Rebase on latest main
   so you keep the other agents' patches (`xnu/patches/*` are cumulative).
2. Before any device trial: confirm the device is free (device column below) and get
   user consent. Trials are exclusive — one agent at a time. Temporary `fastboot boot`
   only; no partition writes.
3. After pushing / trialing: update your row below (task, main contribution, CI run,
   device, next step) and the "Last updated" line.
4. The GitHub repo main + CI runs (api.github.com) are the source of truth, not any
   agent's verbal claim. Verify run status/conclusion yourself.

## Current state (update your row)
| agent | app | task | latest main contribution | CI run | device | next |
|-------|-----|------|--------------------------|--------|--------|------|
| Qoder | Qoder | BT read-only probe SEA fix | 52114cd7 (0476 SEA guard) | 43375585870 success | not held | packaged usb-bt-seafix-w43-ext.img; trial pending user consent |
| CodeArts | CodeArts | (fill in) | (fill in) | (fill in) | (fill in) | needs to push new CI |

## Notes
- main HEAD: 52114cd7 — "bluetooth: guard 0476 read-only probe against powered-off SEA".
- Patches are cumulative in `xnu/patches/`; rebasing on main keeps everyone's work.
- Contents API PUT requires the current blob SHA, so a concurrent push of the same file
  gives the second writer a 409 (anti-clobber).
- Last updated: 2026-11-13 ~06:20Z by Qoder.