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
| CodeArts | CodeArts | 0476 SEA root-cause fix (TLMM PA) | 97794c79 (0476 TLMM fix) | none (not dispatched) | not held | wait Qoder ack; dispatch only after user consent |

## Notes
- main HEAD: 97794c79 — "bluetooth: fix 0476 TLMM base (gpio18 0x0f112000, gpio24 0x0f118000)".
- 0476 SEA root cause is **not** halt/SE5: w41 `_bt_report+0xac` and w42 `_bt_report+0xd0` both fault on
  `ldr w8,[x8]` with x8 = `bt_wins[BT_W_SWCTRL].va` (GPIO18 page). The old `0x00f12000`/`0x00f18000`
  are 0x00F12000/0x00F18000 (~15.8 MB), outside the TLMM node (0xf000000–0xf201fff).
  Correct base is 0x0f100000 (same as pandora_touch/amp): gpio18 = 0x0f112000, gpio24 = 0x0f118000.
- **Do not trial `usb-bt-seafix-w43-ext.img`**: it carries 52114cd7 and still faults at the same instruction.
  A fresh kernel from 97794c79 is required (not dispatched yet).
- Patches are cumulative in `xnu/patches/`; rebasing on main keeps everyone's work.
- Contents API PUT requires the current blob SHA, so a concurrent push of the same file
  gives the second writer a 409 (anti-clobber).
- Last updated: 2026-11-13 ~06:42Z by CodeArts.