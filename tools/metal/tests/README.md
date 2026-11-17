# IOSurface dirty-page contract checks

The iOS 16 adapter injects `SurfaceDirtyPages` into the pinned AdrenoMetal bridge.
Run these host checks before building the complete iOS bundle:

```sh
python3 tools/metal/tests/check_surface_dirty_pages.py /path/to/metal2vulkan
```

Python extracts the actual injected Rust implementation from `adapt_ios16.py` and the
actual `DirtyPages` trait from that runtime checkout. Rust tests replace only the
CF/IOSurface calls and the diagnostic device snapshot. They cover equal publication stamps, range clipping, watch
lifetime, bounded compaction, an incomplete external write source, alias lengths,
concurrent publication and both counter ABI sizes. Temporary sources and binaries are removed automatically.
`--adapt /path/to/old-adapt.py` runs the same checks against an older adapter.

The tests run with distro rustc 1.75 or later; the runner changes only syntax
forms that require a newer compiler (unsafe extern blocks/attributes and inline const arrays).
The full bridge still uses the workflow's Rust toolchain and macOS/iPhoneOS SDK.
Host success proves neither the complete bridge build nor rendering on the phone.

## Publication rules

The log's `runs` mutex serializes seed observation, record insertion/compaction,
publication stamps, watch transitions and reads. A `request` must not return a stamp
before the corresponding record is visible. A hold at the whole-write watermark
already includes that write; only an older hold needs it copied again. Unwatched
ranges and unknown future stamps answer whole. Record allocation failure compacts
to a whole-range publication, and registry allocation failure leaves the import
without a dirty hook. Different lengths of one surface cannot share truncated logs.

SweetSurface's CPU write-unlock seed and this process's runtime write reports do
not establish that every external GPU writer has published. `wait` therefore
returns false, and the runtime performs a full copy. The removed periodic full
answer could not guarantee correct frames between refreshes. Enabling selective
copies requires a shared write-publication mechanism that covers CPU and GPU
writers, their completion ordering, and writes occurring between request and use;
changing the constant alone does not establish those guarantees.

## Telemetry

The original `nmb_device_dirty_counts` retains its 12-word ABI for existing callers.
The frontend uses a new `nmb_device_dirty_counts_v2` export with 16 output words;
indices 0..11 retain their meanings. New words are:

| Index | WorkCounts field |
|---|---|
| 12 | `dirty_copy_in_bytes_whole` |
| 13 | `dirty_copies_in_skipped` |
| 14 | `dirty_wait_nanos` |
| 15 | `dirty_holds_withheld` |

The old single `DP` row exceeded telemetry's 90-character limit, losing its trailing
record counters. New rows fit that limit even with maximum-width u64 values:

| Row | Meaning |
|---|---|
| DP | CPU copied-buffer bytes / bytes skipped |
| DG | Gated GPU shadow copies / whole copies / skipped copies |
| DB | GPU shadow bytes if whole / actual bytes copied |
| DW | Publication requests / waits / unknown-source answers |
| DH | Wait nanoseconds / holds withheld for waiting writers |
| DL | Logs created / bridge whole-range publications |
| DR | Runtime write records / compactions |

`DP cpu=0` does not imply zero GPU shadow traffic. GPU bytes saved are `DB whole`
minus `DB copied`; compare their interval deltas under the same workload. A source
that refuses skipping will increase `DW unknown`, which is expected.
