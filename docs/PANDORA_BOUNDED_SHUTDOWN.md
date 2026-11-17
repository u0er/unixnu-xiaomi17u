# Preserve filesystem writes at the bounded trial's exit

`pandora_reboot_after` previously flushed only the raw console ring and invoked
PSCI directly from the log thread. The restored root uses non-journaled HFS,
whose allocation bitmap updates use delayed writes (`ReleaseBitmapBlock`). An
abrupt reset can persist catalog/data changes without the corresponding bitmap
updates. Two actual captures show referenced blocks marked free; a later trial
reused them and overwrote a signed Metal executable's code with database data.
This is an identified persistence gap, not proof that all captured damage has
one cause. Existing damage needs independent ownership/data repair.

Patch 0481 makes the ordinary timed exit call the kernel's existing
`reboot_kernel(RB_AUTOBOOT, NULL)` path with no UFS lock held. That stops processes,
syncs, unmounts and drains busy buffers before the existing final reset hook.
Panic/emergency reset behavior is unchanged. The timer is the shutdown trigger;
normal shutdown may add time. If shutdown returns (for example EBUSY because a
different caller already owns shutdown), log the result and disable further timer
requests. Never add a forced reset fallback that discards the writes again.

Validation before deployment: apply the patch against the actual CI source and
build the complete PANDORA kernel. Device acceptance requires shutdown logs and a
post-return catalog/overflow/attribute/allocation capture with zero referenced
free blocks and zero overlapping file extents. Merely reaching Android or seeing
the desktop is insufficient. Do not boot the currently damaged volume to test
this patch; complete offline storage repair first and obtain the current
conversation's explicit temporary boot approval.
