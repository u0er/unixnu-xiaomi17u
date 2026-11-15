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
  --level 2 --ci-run 43651227510 `
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

`--level 7` selects the independent GENI internal loopback mode. It sends a
fixed eight-byte pattern and requires an exact return, without BT power/reset
or TLMM programming. It does configure UART and QUP clocks. Its image requires
a kernel built with level 7 support; do not change only the argument of the
earlier level-2 kernel. A pass establishes the AP UART data path only.

`uart_trace.py TRACE --out JSON` reads an already-captured Android `hsuart/trace`
offline. It extracts baud settings and DMA Tx/Rx chunks with source line numbers,
reported lengths and captured hex prefixes. It marks truncated dumps and does
not reconstruct protocol frames or fill missing data. The 2026-11-15 capture
provides direct FD/FC and PERI startup evidence; see
`docs/BLUETOOTH_ANDROID_ROM_ROUTE_2026-11-15.md` before implementing a ROM probe.

`--level 8` selects the bounded FD/FC + PERI version probe. It follows the
validated cold-start power/UART path, but sends no ordinary HCI Reset, firmware,
NVM, UWB initialization or baud-rate change. Wake ACK, complete framing and
matching response fields are required. The result indicates only whether the
PERI ROM inquiry worked; it is not a Bluetooth-ready result. Use a kernel built
with level 8 support, then request a new device window for its first trial.

`--level 9` repeats level 8 with only RX_TRANS_CFG bit1 cleared. The earlier
code copied the TX CTS mask into this RX register; Android's trace reports
RX_TRANS_CFG=0 and its module does not set that bit. The power sequence, baud,
S_CMD0 parameters and PERI request stay as in level 8. Readback must match
before sending FD. This comparison requires a kernel built with level 9 support.
It does not test RFR_OPEN or PMIC input/output sampling; those are separate
experiments. Prior level8 device evidence remains the baseline.

`--level 10` keeps level 9's RX bit1 correction and sets only RFR_OPEN (bit7)
in UART_START_READ: S_CMD0 changes from 0x08000000 to 0x08000080. Command
readback must match before sending FD. The local Android module's
0x080000e0 also filters framing/break errors; these bits are deliberately
excluded from this comparison. Power, firmware, baud, FIFO packing and PERI
handling follow level 9. Use a complete kernel with level 10 support and a
new explicitly authorized trial window. No device operation is part of preparation.

`--level 11` keeps level 10's UART/PERI path and changes only the BT S8F_E0
voltage request from 1800 to 1876 mV. This is the live DT BT consumer minimum
and the value already used by the existing Wi-Fi driver for that resource.
Voltage and enable request return codes are logged separately; either failure
stops BT initialization. No shared rail is disabled on failure. The request
result is not an analog measurement. No BT_EN sampling, error filters, firmware,
baud change or extra power retry is added. A complete level 11 kernel and
a newly authorized device window are required.

`--level 12` keeps level 11's power and UART/PERI settings. At the end of
the existing BT_EN low/high waits it temporarily selects normal pad input
and LV/MV INPUT_OUTPUT MODE2, obtains three RT_STS bit0 samples, and restores
the saved mode/input configuration with readback. It touches no source,
VIN or drive setting during sampling. Only the confirmed GPIO5 subtype0x12
and expected output configuration are accepted. Every attempted change is
restored even if its write failed. Sample/config/restore failure stops the
power sequence before UART; no FC is required for this diagnostic to provide
new evidence. Passing samples establish PMIC digital input levels only.

## Incremental local kernel preparation

For one driver change, reuse buildtree, objects and linklibs from the **same**
device-tested CI build; do not mix the workspace's older caches. Example in WSL:

```sh
python3 tools/bluetooth/compile_driver.py \
  --buildtree /opt/nezha-bt-incremental/ci-43668443548/buildtree \
  --source "$PWD/wsl/pandora_bt.c" --clang /usr/bin/clang \
  --sdk-include /mnt/c/Users/1/Documents/Codex/2026-10-31/wo-x/work/toolchain/iPhoneOS16.5.sdk/usr/include \
  --out /opt/nezha-bt-incremental/level12/pandora_bt.o
python3 tools/relink_nezha_kernel.py \
  --buildtree /opt/nezha-bt-incremental/ci-43668443548/buildtree \
  --ci /opt/nezha-bt-incremental/ci-43668443548/ci \
  --replace pandora_bt=/opt/nezha-bt-incremental/level12/pandora_bt.o \
  --ld /opt/unixnu-lto-build/cctools/ld64/src/ld/ld \
  --out /opt/nezha-bt-incremental/level12/kernel.development.pandora.unstripped
```

Run from the project root after converting the workspace path to WSL form.
The local tools record compiler/linker arguments, return codes and diagnostics.
Use cctools strip `-S` for this static Mach-O; the installed LLVM14 strip does
not support it. Retain unstripped output and link records for debugging.
Local ld64 may change layout and produce static absolute pointers instead of
CI's chained fixups. Validate entry/segments, unresolved symbols, relocation
format and packed image; report that linker change alongside the driver change.
This path has passed host linking/packing for level12 but has not yet booted.
Never label it a new successful CI build or a device-accepted kernel.
