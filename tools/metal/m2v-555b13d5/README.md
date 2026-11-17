# Resource-sized allocation retry on the pinned runtime

Apply `0001-resource-sized-oom-fallback.patch` only after the pinned AdrenoMetal
bridge patch series. `build_plugin.sh` does this explicitly. The old 720ef1c
patch stack remains disabled. This branch starts from the accepted Stage-2
baseline so this experiment changes only memory allocation fallback behavior;
the separate DirtyPages contract branch remains available and is not discarded.

The 2026-11-17 App Library trial refused a 24 MiB request with about 34.6 MiB
free but only about 20.8 MiB in the largest run of the kernel's 1 GiB pool.
The existing runtime retries after freeing kept blocks/draining posted frees,
but repeats the same pool growth size. A single resource may still fit a
smaller run. The older maintained patch 0013 used this same fallback mechanism;
this patch ports it to the current `BlockMaker`, parking and placement paths.

Only a non-dedicated growth allocation larger than the real Vulkan memory
requirement, failing with device-memory OOM after the existing reclaim attempt,
retries once at the requirement size. A successful retry is accounted using
its actual size and treated as a block apart, so it is released rather than
kept as an empty spare. It is never smaller than required. Dedicated, host OOM,
device-lost and other errors retain their original paths. Failure stays failure.
The original growth target is marked failed in stage traces; the smaller retry
gets its own trace. Darwin reports the first eight attempts per process to
distinguish this path on the phone without an unbounded diagnostic stream.

This does not fix the separately observed oversized/stale IB submission or
process-global Darwin connection teardown. Do not mask those with skipped
commands or claim complete OOM recovery from a successful allocation retry.
Host validation must replay the exact pinned bridge series before checking this
patch; actual macOS bridge compilation and single-component device trials remain
required. Device trials preserve the verified orderly-shutdown kernel and current
dual-container root. Do not use the old forced-reset seed/kernel defaults.
