#!/usr/bin/env python3
"""Turnip: accept imported LINEAR image pitches, stop whole-surface shadow copies.

Root cause of ~108k shadow copies / 90 s desktop session (data-a-live GPU agent,
gpu-optimization/ACTION_PLAN.md §1): the runtime imports IOSurface/dma-buf textures
as DRM_FORMAT_MOD_LINEAR with the surface's real 16-byte-aligned pitch (160/176).
fd6_layout.c forced the locally-allocated 64-byte perf alignment on those explicit
LINEAR images, rejected the pitch (ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT)
and the runtime fell back to a shadow copy of the whole surface.

Two exact edits (tiled paths keep the strict checks):

1. fd6_layout.c: for an explicit LINEAR image accept max(cpp,16)-byte pitch alignment
   and override pitchalign = ffs(pitch0)-1 so fdl_pitch() writes the real stride into
   the texture descriptor (otherwise 160/176 rounds to 192 and samples wrong bytes).

2. tu_image.cc: external LINEAR / DRM-modifier images no longer require a dedicated
   allocation (only tiled external images do); orthogonal to (1), kept together.

  pandora_linear_pitch.py MESA_DIR

Exact text edits, asserted; run once on a patched tree (CI: turnip.yml Sources step,
after the UniXNU patches and the other pandora_*.py scripts). fd6_layout.c anchor is
mesa 85fda3b upstream; tu_image.cc anchor is the UniXNU turnip-host-import-images.patch
version (& ~host_types), composed with the tiling exemption so both relaxations hold.
"""
import pathlib
import sys


def patch(rel, old, new):
    p = pathlib.Path(sys.argv[1]) / rel
    s = p.read_text()
    if old not in s:
        raise SystemExit(f'pandora_linear_pitch: anchor not found in {rel}:\n{old[:160]}')
    if s.count(old) != 1:
        raise SystemExit(f'pandora_linear_pitch: anchor not unique in {rel}')
    p.write_text(s.replace(old, new, 1))
    print(f'pandora_linear_pitch: patched {p}')


patch('src/freedreno/fdl/fd6_layout.c',
      """   if (explicit_layout) {
      offset = explicit_layout->offset;
      layout->pitch0 = explicit_layout->pitch;
      if (align(layout->pitch0, 1 << layout->pitchalign) != layout->pitch0)
         return false;
   }
""",
      """   if (explicit_layout) {
      offset = explicit_layout->offset;
      layout->pitch0 = explicit_layout->pitch;
      /* Imported (host/dma-buf) memory carries a pitch fixed by the source
       * surface. The inflated alignment used for locally-allocated images
       * (1 << (pitchalign + 6) bytes; for an explicit LINEAR image this is
       * 1 << 6 = 64 bytes) is only a perf heuristic for gmem store over-fetch;
       * it is not a hardware requirement for a linear image sampled from
       * imported memory. Requiring it rejects 16-byte-aligned IOSurface pitches
       * (e.g. 160/176) and forces a shadow copy of the whole surface.
       * For LINEAR imported images only cpp-aligned (with a small safety floor
       * of 16) is needed, so accept the pitch as authoritative; keep the strict
       * check for tiled layouts. */
      uint32_t min_align = MAX2(layout->cpp, 16u);
      if (layout->tile_mode == TILE6_LINEAR) {
         if (align(layout->pitch0, min_align) != layout->pitch0)
            return false;
         /* fdl_pitch() rounds the stride up to 1 << pitchalign before it is
          * written into the texture descriptor (fd6_view.cc). With the default
          * inflated pitchalign that would turn a 160/176-byte imported stride
          * into 192, sampling the wrong bytes. Override pitchalign so that
          * 1 << pitchalign is the largest power of two dividing the imported
          * pitch, making fdl_pitch() return pitch0 unchanged. */
         layout->pitchalign = ffs(layout->pitch0) - 1;
      } else if (align(layout->pitch0, 1 << layout->pitchalign) != layout->pitch0) {
         return false;
      }
   }
""")

patch('src/freedreno/vulkan/tu_image.cc',
      """      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *req =
            (VkMemoryDedicatedRequirements *) ext;
         /* Host pointer imports carry no per-image metadata, so an image
          * over one may be bound at any offset of a shared import.
          */
         const VkExternalMemoryHandleTypeFlags host_types =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT;
         req->requiresDedicatedAllocation =
            (image->vk.external_handle_types & ~host_types) != 0;
         req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
         break;
      }
""",
      """      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *req =
            (VkMemoryDedicatedRequirements *) ext;
         /* Host pointer imports carry no per-image metadata, so an image
          * over one may be bound at any offset of a shared import.
          */
         const VkExternalMemoryHandleTypeFlags host_types =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT;
         /* Linear and DRM-modifier images are laid out at fixed pitches and
          * bind at any offset their alignment allows inside an imported
          * allocation; only tiled external images (AHB's compressed layouts)
          * need the allocation to themselves. IOSurface-backed textures
          * (e.g. 1125-wide BGRA at 4608 bytes == fd6's align(4500, 256))
          * otherwise fall back to per-use shadow copies of the whole
          * surface (108,922 copies in one 90 s desktop session).
          */
         req->requiresDedicatedAllocation =
            (image->vk.external_handle_types & ~host_types) != 0 &&
            image->vk.tiling != VK_IMAGE_TILING_LINEAR &&
            image->vk.tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
         req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
         break;
      }
""")
