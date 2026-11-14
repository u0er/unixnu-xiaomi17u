# Bluetooth trial preparation

`prepare_trial.py` prepares a boot image entirely on the host. It contains no
device access, container installation or boot operation. A completed `trial.json`
records `device_booted: false`; absence of that file means preparation did not
finish. Compiler failures remain visible in `loader-build.log`.

Example for the existing Windows/Nezha lane:

```powershell
python C:/Users/1/Documents/Codex/2026-10-31/wo-x/outputs/data-a-live-nezha/tools/bluetooth/prepare_trial.py `
  --kernel D:/turnip-local/newboot/bt-no-analyzer-20261115/artifact/kernel.development.pandora `
  --base-boot D:/turnip-local/newboot/usb-bt-tlmm-level2-w67-ext.img `
  --build-root E:/IOS16/IOS16 `
  --llvm C:/Users/1/Documents/Codex/2026-10-31/wo-x/work/toolchain/ndk/bin `
  --fw-extents C:/Users/1/Documents/Codex/2026-10-31/wo-x/work/bluetooth-source-audit/android-current-fw-extents.json `
  --level 2 --ci-run 43649529960 `
  --out D:/turnip-local/newboot/bt-no-analyzer-20261115/level2
```

The output directory must not already exist. The resulting image is
`<out>/loader/nezha-loader-boot.img`.

The baseline supplies its ADT, trust cache, memory bases and boot arguments.
It must already contain exactly one `pandora_reboot_after=90` and one
`pandora_bt` argument. Only the latter argument is replaced. Root/log mapping
and service arguments are inherited from the selected baseline.

If firmware mapping has changed, supply an explicit `--fw-extents` JSON captured
with pinfile's read-only FIEMAP operation. The existing
`mkadt_pandora.fw_extent_props` producer updates only the two firmware properties
in the new host ADT. This never writes or relocates the container. The 2026-11-15
baseline needs this option: its old 32 MiB mapping no longer matches the current
64 MiB `metal-update-0013.img`. A separate preflight must also establish that
the baseline root/log maps still cover the ranges exposed by the image.

Trust-cache bytes are located through their ADT physical range and a containing
file segment, not through the segment's position. The tool delegates Mach-O
packing and chained rebases to `mkpayload.build`, then keeps the observed
virtual base and relocates the unchanged cache after the new kernel. It checks
segment bounds, address ranges and the resulting ADT mapping before invoking
the existing loader builder. No file hashing is used.

`--build-root` chooses the loader source. Use the same source lane as the
observed baseline. In this task, E:/IOS16/IOS16 is the existing w67 loader lane;
the workspace loader has additional boot-argument ABI and framebuffer changes.
Switching to that loader would introduce a separate experimental variable and
must be evaluated independently. All loader products are written under `--out`.

The first Bluetooth candidate should use level 2, with one initialization at
the corrected cold-start settings. Level 6 is reserved for a subsequent retry
experiment. Compilation and image preparation do not establish HCI behavior;
device startup still follows the repository's explicit-consent rule.
