/*
 * PANDORA VIDC: minimal clean-room Qualcomm Iris4 / VPU4 transport for UniXNU.
 *
 * Target: Xiaomi 17 Ultra (nezha, SM8850-AC / canoe / kaanapali), video IP at
 * /soc/qcom,vidc@2000000 (compatible qcom,canoe-vidc-v2), GIC SPI 174, 8 MiB
 * carveout video_region@9a600000.  The only goal of this revision is an HFI
 * version handshake: power the block, load the firmware into the carveout, set
 * up the shared command/message queues, send HFI_CMD_INIT and read back the
 * HFI_PROP_IMAGE_VERSION response.  It deliberately does NOT create a session,
 * encode a frame, or expose an IOKit user client.
 *
 * Design discipline (from WORK_ORDER_2026-11-19 item A1):
 *  - Clean room.  Register offsets, HFI packet layouts and the queue protocol
 *    below are protocol FACTS transcribed from public mainline Linux
 *    drivers/media/platform/qcom/iris (GPL-2.0).  That source is physically
 *    isolated in work/references/iris-hfi-spec-20261119/ and NO GPL code is
 *    copied here: only numeric constants and wire layouts, each tagged with
 *    its origin.  See docs/research/PANDORA-VIDC-DRAFT-SPEC.md section 6.1.
 *  - Evidence tags: [V] verified on this device, [K] verified against
 *    msm_video.ko disassembly, [M] mainline protocol fact, [I] inference that
 *    is logged but not trusted.  Anything still unknown is logged, never
 *    silently guessed.
 *  - Default off.  pandora_vidc=0 (default): do nothing.  =1: read-only MMIO
 *    identity probe.  =2: power + firmware + queue setup + HFI handshake.
 *
 * Revision history (2026-11-20, level-2 rewrite):
 *  - Queue indices are u32-WORD indices, not bytes [M iris_hfi_queue.c:14-16].
 *  - Queue header header_type = 0x0101 (IFACEQ_DFLT_QHDR), q_size in words,
 *    rings 1 MiB apart, rx/tx_irq_status start at 0 [M iris_hfi_queue.c:202-217].
 *  - Published qtable size, queue layout and the boot sequence now follow the
 *    vendor boot path exactly: five publish registers, then the CP-span
 *    registers 0xA0024/0xA0028, then WRAPPER SPARE_0 0xB078=1 and boot control
 *    0xA0048=1, then poll 0xA004C, then H2XSOFTINTEN 0xA0148=1 and
 *    X2RPMH 0xA0168=0 [K __boot_firmware_iris4 @ .text+0x15a44].
 *  - The queue table and rings live in contiguous wired memory that the CPU
 *    touches through its normal kernel mapping; every hand-off to or from the
 *    VPU is bracketed by an explicit clean/invalidate of the exact range on
 *    the physical address, so neither side reads stale lines.
 *  - bringup() now runs every stage best-effort and prints one PASS/FAIL line
 *    per stage, so a single 120 s boot yields the whole ladder instead of only
 *    the first failure.
 *
 * Firmware note (2026-11-19 device evidence; VPU_FIRMWARE_RECON.md §0.1): the image
 * actually loaded on this unit is /odm/firmware/vpu40_2v_v1.mbn (vfw-4.0:rel0060,
 * IMAGE_VARIANT PROD, built 2026-04-13) -- the Android dmesg prints that exact version
 * string.  The older /vendor/firmware venus_v7.mbn (video-firmware.2.6.2) is a
 * legacy/alternate family and NOT the current image.  The image name comes from
 * boot-arg pandora_vidc_fw (default "vpu40_2v_v1.mbn").
 *
 * PAS viability (2026-11-20 offline check of vpu40_2v_v1.mbn): ELF32, 22 phdrs,
 * HASH at index 21, loadable p_paddr span 0x0f500000-0x0fd00000 == exactly
 * 8 MiB, so pandora_pas_load_auth() with mem_pa=0x9a600000 mem_size=8 MiB has
 * zero slack and must succeed or fail loudly.
 */

#ifdef PANDORA

#include <sys/param.h>
#include <sys/systm.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>   /* thread_block */
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <arm/caches_internal.h>
#include "adreno/ArmSmmu4K.h"
#include "adreno/ArmSmmu4KContext.h"
#include "adreno/ArmSmmu4KProgram.h"
#include "adreno/ArmSmmu4KMmio.h"
#include "adreno/ArmSmmu4KStreams.h"

/* The apps-SMMU is shared with the display, the camera TPG and WiFi, so the
 * context is claimed under the same lock they use (pandora_adsp.c). */
extern void pandora_apps_smmu_lock(void);
extern void pandora_apps_smmu_unlock(void);

extern vm_map_t kernel_map;

/* Measure the kernel_map budget instead of guessing.  kernel_map's
 * `struct _vm_map` is opaque from bsd/, so ask the allocator itself: try the
 * size that actually failed at 5.17 s (864 KiB) plus the sizes around it, and
 * hand each block straight back.  This turns "our 5 MiB is too much" from an
 * inference into a number in the log. */
static void
vidc_map_budget(const char *when)
{
    static const vm_size_t want[] = { 64u << 10, 256u << 10, 864u << 10, 1u << 20 };
    char line[160];
    unsigned off = (unsigned)snprintf(line, sizeof(line), "PANDORA VIDC: kva budget %-14s", when);
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        vm_offset_t va = 0;
        /* Same primitive the driver uses for its own windows (kmem_alloc's
         * 3-arg form is KERNEL_PRIVATE in this tree; contig is what we call). */
        const kern_return_t kr = kmem_alloc_contig(kernel_map, &va, want[i], PAGE_MASK,
            0, 0, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT);
        if (kr == KERN_SUCCESS) {
            kmem_free(kernel_map, va, want[i]);
        }
        off += (unsigned)snprintf(line + off, sizeof(line) - off, " %u%c", (unsigned)(want[i] >> 10),
            kr == KERN_SUCCESS ? 'k' : 'X');
    }
    printf("%s\n", line);
}
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern void clean_dcache64(addr64_t pa, unsigned len, boolean_t to_mem);
extern void flush_dcache64(addr64_t pa, unsigned len, int phys);
extern void delay(int);

/* MMIO mapping and firmware access helpers already present in this kernel. */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern int pandora_firmware_pread(const char *name, uint64_t off, void *dst, size_t len, uint64_t *size);
extern kern_return_t thread_terminate(thread_t);

/* -Wmissing-prototypes is on in the kernel build (-Weverything -Werror), and
 * sweet_ufs.c calls this via its own extern, so declare it here as well. */
void pandora_vidc_init(void);

/* ------------------------------------------------------------------ */
/* Hardware addresses (facts: live.dtb / Android base.dtb, draft spec 2) */
/* ------------------------------------------------------------------ */
#define VIDC_PA                 0x02000000u   /* /soc/qcom,vidc@2000000 reg[0] */
#define VIDC_SIZE               0x000F0000u   /* reg size */
#define VIDEO_REGION_PA         0x9A600000u   /* video_region@9a600000 (8 MiB) */
#define VIDEO_REGION_SIZE       (8u * 1024u * 1024u)
#define VIDC_IRQ                174u           /* GIC SPI, level (unused so far) */

/* vpu4 register offsets, relative to VIDC_PA.
 * Source: mainline iris_vpu_register_defines.h (generic vpu4). */
#define VCODEC_BASE_OFFS        0x00000000u
#define AON_MVP_NOC_RESET       0x0001F000u
#define CPU_BASE_OFFS           0x000A0000u
#define WRAPPER_BASE_OFFS       0x000B0000u
#define AON_BASE_OFFS           0x000E0000u

#define R_VCODEC_SS_IDLE        (VCODEC_BASE_OFFS + 0x070u)

/* WRAPPER_EFUSE_MONITOR is WRAPPER+0x08 on vpu4 (mainline iris_vpu4x.c) and the
 * vendor reads 0xB0008 here [K __power_on_iris4_hardware @0x17754: mov w1,#8;
 * movk w1,#0xb,lsl#16 -> __read_register(0xB0008, ...)].  The earlier revision
 * used 0xB0004, which is a different register. */
#define R_WRAPPER_EFUSE         (WRAPPER_BASE_OFFS + 0x008u)
#define R_WRAPPER_INTR_STATUS   (WRAPPER_BASE_OFFS + 0x00Cu)
#define R_WRAPPER_INTR_MASK     (WRAPPER_BASE_OFFS + 0x010u)
#define R_WRAPPER_SPARE_0       (WRAPPER_BASE_OFFS + 0x078u)
#define R_WRAPPER_CORE_PWR      (WRAPPER_BASE_OFFS + 0x080u)
#define R_WRAPPER_CORE_CLK      (WRAPPER_BASE_OFFS + 0x088u)

#define R_CPU_A2HSOFTINTCLR     (CPU_BASE_OFFS + 0x01Cu)
#define R_CPU_H2XSOFTINTEN      (CPU_BASE_OFFS + 0x148u)
#define R_CPU_SOFTINT           (CPU_BASE_OFFS + 0x150u)
#define R_CPU_AHB_BRIDGE_RESET  (CPU_BASE_OFFS + 0x160u)
#define R_CPU_APV_BRIDGE_RESET  (CPU_BASE_OFFS + 0x174u)
#define R_CPU_X2RPMH            (CPU_BASE_OFFS + 0x168u)
#define MSK_CORE_POWER_ON       0x00000002u  /* [M] defines:39-41 */
#define MSK_SIGNAL_FROM_TENSILICA 0x00000001u

#define R_AON_NOC_LPI_CTRL      (AON_BASE_OFFS + 0x000u)
#define R_AON_NOC_LPI_STATUS    (AON_BASE_OFFS + 0x004u)
#define R_AON_NOC_RESET_REQ     (AON_MVP_NOC_RESET + 0x000u)
#define R_AON_NOC_RESET_SYNCRST (AON_MVP_NOC_RESET + 0x008u)
/* [M] iris_vpu4x.c:319-330 -- the hardware power-on path asserts this mask,
 * waits for it on the ACK register, then drops SYNCRST and REQ and waits for
 * ACK to fall to zero.  Without this handshake the video NoC stays in reset
 * and the Xtensa cannot fetch a single beat. */
#define MVP_NOC_RESET_REQ_MASK  0x00070103u
#define R_AON_NOC_RESET_ACK     (AON_MVP_NOC_RESET + 0x004u)

/* Host->firmware qtable publish.  Every offset below is what
 * __boot_firmware_iris4 writes [K .text+0x15a44, 776 bytes; base register
 * w21 = 0xA0024 and the +N offsets are literal immediates in the disassembly]. */
/* Publish registers, decoded from __boot_firmware_iris4 where the function
 * prologue sets the base to **w21 = 0xA0024** (`mov w21,#0x24` +
 * `movk w21,#0xa,lsl#16`), so every `add w1,w21,#imm` below is 0xA0024+imm:
 *   0xA0064 <- qtable DA   (orr w21,#0x40)   0xA0068 <- 0x300000 (add #0x44)
 *   0xA0054 <- qtable DA   (add #0x30)       0xA0050 <- 1        (add #0x2c)
 *   0xA0060 <- SFR DA      (add #0x3c)                       [K 0x15a60-0x15b0c]
 * `0xA0048` (boot) and `0xA004C` (status) are likewise w21+0x24 and w21+0x28.
 * A round where these were "corrected" to 0x40/0x44/0x30/0x2c/0x3c read back
 * self-consistent but changed nothing, which is what a wrong-but-writable
 * register set looks like: the fix was reverted. */
#define R_CPU_QUEUE_TABLE_DA    (CPU_BASE_OFFS + 0x064u)  /* qtable device addr */
#define R_CPU_QUEUE_TABLE_SIZE  (CPU_BASE_OFFS + 0x068u)  /* write 0x300000 */
#define R_CPU_QUEUE_TABLE_DA2   (CPU_BASE_OFFS + 0x054u)  /* qtable device addr */
#define R_CPU_QUEUE_TABLE_EN    (CPU_BASE_OFFS + 0x050u)  /* write 1 */
#define R_CPU_QUEUE_TABLE_ADDR2 (CPU_BASE_OFFS + 0x060u)  /* vendor writes sfr here too */
/* [M] iris_vpu_common.c iris_vpu_setup_ucregion_memory_map():
 *     #define SFR_ADDR (CPU_CS_BASE_OFFS + 0x5C)
 *     if (core->sfr_daddr) writel(sfr_daddr + core_arch, SFR_ADDR);
 * The vendor does exactly this at 0xA005C with `core[0x12c8] + 1`, which pins
 * core_arch = 1 for this platform.  We never wrote this register at all: the
 * firmware looks up its SFR here, so without it HFI can never initialise and
 * CTRL_STATUS (0xA004C) stays zero forever -- which is the observed state. */
#define R_CPU_SFR_ADDR          (CPU_BASE_OFFS + 0x05Cu)
#define VIDC_CORE_ARCH          1u
#define R_CPU_QUEUE_TABLE_STAT  (CPU_BASE_OFFS + 0x04Cu)  /* bit0 ready, bits1-3 err */
#define R_CPU_CP_SPAN_A         (CPU_BASE_OFFS + 0x024u)  /* CP span size */
#define R_CPU_CP_SPAN_B         (CPU_BASE_OFFS + 0x028u)  /* CP span size */
#define R_CPU_CP_NONPIXEL       (CPU_BASE_OFFS + 0x05Cu)  /* nonpixel size + 1 */
#define R_CPU_BOOT_CTRL         (CPU_BASE_OFFS + 0x048u)  /* write 1, release VPU */

#define BIT_A2H                 (1u << 2)
#define BIT_A2HWD               (1u << 3)
#define CORE_BRIDGE_SW_RESET    (1u << 0)
#define CORE_BRIDGE_HW_RST_DIS  (1u << 1)

/* ------------------------------------------------------------------ */
/* HFI gen2 wire protocol constants.
 * Source: mainline iris_hfi_gen2_defines.h / iris_hfi_queue.h (facts only). */
/* ------------------------------------------------------------------ */
#define HFI_CMD_INIT            0x01000001u
#define HFI_CMD_POWER_COLLAPSE  0x01000002u
#define HFI_CMD_OPEN            0x01000003u   /* session create */
#define HFI_CMD_START           0x01000005u
#define HFI_CMD_BUFFER          0x01000009u
#define HFI_PROP_IMAGE_VERSION  0x03000001u
#define HFI_PROP_CODEC          0x03000100u

#define IFACEQ_NUMQ             3u            /* cmd / msg / debug */
#define IFACEQ_CORE_PKT_SIZE    (1024u * 4u)  /* 4096 */
#define IFACEQ_DFLT_QHDR        0x0101u       /* [M iris_hfi_queue.h:22] */

/* Published qtable size: exactly what the vendor driver writes to CPU+0x68
 * [K], i.e. 3 MiB total for the header plus the three rings.  We therefore
 * place the rings on 1 MiB boundaries INSIDE those 3 MiB so nothing is
 * described outside the published window:
 *
 *   0x000000 +--------------------------+
 *            | table header (456 B)     |
 *   0x001000 +--------------------------+
 *            | cmd ring, 0x000FE000 B   |
 *   0x100000 |                          |  <- ring stride 0x100000, size 0xFF000
 *   0x1FF000 +--------------------------+  (1 KiB gap keeps rings disjoint)
 *   0x200000 | msg ring / dbg ring      |
 *   0x2FF000 +--------------------------+
 *   0x300000   end of published window
 */
/* Queue window is runtime-sized (boot-arg pandora_vidc_ring, KiB per ring,
 * default 1024).  The 3 MiB window plus the other MMA windows made an unrelated
 * 864 KiB kmem_alloc fail at 5.17 s (`failed with 3 @vm_kern.c:178`) and panic
 * the boot.  ml_io_map entries are permanent, so the window cannot be released
 * afterwards -- the only legitimate lever is to map less. */
#define VIDC_RING_KB_DEFAULT    1024u
#define VIDC_RING_KB_MIN        64u
#define VIDC_RING_KB_MAX        4096u
/* Default to 256 KiB per ring (0xC1000 window) so the boot survives the 5.17 s
 * 864 KiB kmem_alloc that the 3 MiB window used to trigger.  pandora_vidc_ring
 * can restore 1024 for the vendor-identical 0x300000 window when A/B testing.
 * The initialisers below keep the geometry valid even if a probe touches it
 * before pandora_vidc_init runs. */
#define VIDC_RING_KB_ACTIVE     256u
static uint32_t vidc_ring_bytes = (VIDC_RING_KB_ACTIVE << 10);
static uint32_t vidc_qtable_bytes = 0x1000u + 3u * (VIDC_RING_KB_ACTIVE << 10);
#define VIDC_QTABLE_BYTES       vidc_qtable_bytes
/* Exactly the published window: header 456 B + three 1 MiB rings ends at
 * 0x300000, so the old +64 KiB slack was pure permanent kernel_map waste. */
#define VIDC_QTABLE_ALLOC       vidc_qtable_bytes
#define IFACEQ_RING_OFFS(i)     (0x001000u + (i) * vidc_ring_bytes)
#define IFACEQ_QUEUE_SIZE       (vidc_ring_bytes - 0x001000u)   /* rings stay disjoint */
#define IFACEQ_QUEUE_WORDS      (IFACEQ_QUEUE_SIZE / 4u)

#define VIDC_SFR_BYTES          0x00004000u   /* [M] SFR_SIZE = 4 KiB */

/* Generic packet header: u32 size, u32 pkt_type (little endian). */
struct vidc_pkt_hdr {
    uint32_t size;
    uint32_t pkt_type;
};

/* Queue header, byte-exact layout from struct iris_hfi_queue_header. */
/* Byte-exact layout of struct iris_hfi_queue_header [M iris_hfi_queue.h]: every
 * field is a u32 and header_type is at offset 8, BEFORE queue_type.
 *
 * Getting this wrong is not cosmetic: the boot snapshot printed
 * `q0 00000001 ff001000 01010000`, i.e. the firmware read word 8 as 0x01010000
 * instead of the required 0x0101, because an earlier revision had uint16_t
 * queue_type at 8 and uint16_t header_type at 10 (plus a pkt_drop_cnt field the
 * upstream header does not have).  The core then re-tried a single bogus
 * address every poll (SMMU FAR 0xfdffdd7e, 16 times identical) instead of
 * reporting ready. */
struct vidc_queue_header {
    uint32_t status;        /* bit0 = active */
    uint32_t start_addr;
    uint32_t header_type;   /* [M] IFACEQ_DFLT_QHDR = 0x0101, offset 8 */
    uint32_t queue_type;    /* [M] offset 12 */
    uint32_t q_size;        /* in u32 WORDS [M] */
    uint32_t pkt_size;      /* 0 = variable */
    uint32_t rx_wm;
    uint32_t tx_wm;
    uint32_t rx_req;
    uint32_t tx_req;
    uint32_t rx_irq_status;
    uint32_t tx_irq_status;
    uint32_t read_idx;      /* in u32 WORDS [M] */
    uint32_t write_idx;     /* in u32 WORDS [M] */
};

_Static_assert(sizeof(struct vidc_queue_header) == 56,
    "iris_hfi_queue_header is 14 u32 = 56 bytes");
_Static_assert(offsetof(struct vidc_queue_header, header_type) == 8,
    "header_type must be word 2 of the queue header");
_Static_assert(offsetof(struct vidc_queue_header, queue_type) == 12,
    "queue_type must be word 3 of the queue header");
_Static_assert(offsetof(struct vidc_queue_header, q_size) == 16,
    "q_size must be word 4 of the queue header");
_Static_assert(offsetof(struct vidc_queue_header, read_idx) == 48,
    "read_idx must be word 12 of the queue header");
_Static_assert(offsetof(struct vidc_queue_header, write_idx) == 52,
    "write_idx must be word 13 of the queue header");

/* Queue table header, byte-exact layout from struct iris_hfi_queue_table_header. */
struct vidc_queue_table {
    uint32_t version;
    uint32_t size;
    uint32_t qhdr0_offset;
    uint32_t qhdr_size;
    uint32_t num_q;
    uint32_t num_active_q;
    uint64_t device_addr;
    char name[256];
    struct vidc_queue_header qhdr[IFACEQ_NUMQ];
};

/* ------------------------------------------------------------------ */
/* State */
/* ------------------------------------------------------------------ */
static int      vidc_level;                    /* pandora_vidc boot-arg */
static char     vidc_fw_name[64] = "vpu40_2v_v1.mbn";
static vm_offset_t vidc_mmio;                  /* mapped VIDC registers */
static bool     vidc_mmio_on;

/* qtable + SFR: physically contiguous, zeroed, wired pages.  The CPU touches
 * them through the normal kernel mapping (vidc_*_va); every hand-off to or from
 * the VPU is bracketed by a flush_dcache64() on the PHYSICAL address, which is
 * clean+invalidate in xnu.  That is the pattern already proven on this board in
 * pandora_gpu.c:1095 / pandora_adsp.c:814: it never leaves a dirty line that
 * could later be written back over firmware-owned data, and it never lets the
 * CPU read a stale line after the VPU wrote. */
/* Pin the queue geometry before every user of it (called from pandora_vidc_init). */
static void
vidc_set_ring_geometry(void)
{
    unsigned kb = VIDC_RING_KB_ACTIVE;
    (void)PE_parse_boot_argn("pandora_vidc_ring", &kb, sizeof(kb));
    if (kb < VIDC_RING_KB_MIN || kb > VIDC_RING_KB_MAX || (kb & (kb - 1u)) != 0) {
        printf("PANDORA VIDC: pandora_vidc_ring=%u invalid (power of two, %u..%u KiB); using %u\n",
            kb, VIDC_RING_KB_MIN, VIDC_RING_KB_MAX, VIDC_RING_KB_DEFAULT);
        kb = VIDC_RING_KB_DEFAULT;
    }
    vidc_ring_bytes = kb << 10;
    vidc_qtable_bytes = 0x1000u + IFACEQ_NUMQ * vidc_ring_bytes;
    printf("PANDORA VIDC: queue geometry: %u KiB per ring, window %#x, q_size %#x words\n",
        kb, vidc_qtable_bytes, (vidc_ring_bytes - 0x1000u) / 4u);
}

static vm_offset_t vidc_qtable_va;
static uint64_t vidc_qtable_pa;
static vm_offset_t vidc_sfr_va;
static uint64_t vidc_sfr_pa;

/* The VPU is an Xtensa core whose queue-table and start-address registers are
 * 32-bit, but every wired page xnu hands out sits above 4 GiB (level-2b run:
 * qtable PA 0x8cc482000).  Writing (uint32_t)pa published 0xcc482000, an
 * address outside the carveout, so the firmware could never find its queues.
 * Mainline does the same thing differently: iris_hfi_queue.c allocates with
 * dma_alloc_attrs() and publishes iface_q_table_daddr, an IOVA, never a PA
 * [M iris_hfi_queue.c:231,254 / iris_core.h:106].  We therefore put the queue
 * table and the SFR behind the apps-SMMU (non_secure_cb, SIDs 0x1940/0x1a20/
 * 0x1944) and give them 32-bit IOVAs below the 0xffc00000 DMA limit that the
 * DT's iommu_region_partition enforces [V live.dts iommu-addresses].
 *
 * Layout: the table header plus its three 1 MiB rings are one contiguous
 * allocation; the SFR is a separate 4 KiB allocation.  Both get a fixed 32-bit
 * IOVA from the same window, and the arm-smmu 4K tables map range by range, so
 * the IOVA offset of every ring inside the table pool equals its PA offset --
 * which is exactly the arithmetic vidc_fill_queues() relies on. */
#define VIDC_SMMU_PA            0x15000000u   /* apps-smmu@15000000 */
#define VIDC_SMMU_SIZE          0x00100000u
#define VIDC_IOVA_BASE          0xFF000000ull /* < 0xffc00000 DMA limit */
#define VIDC_IOVA_SFR           0xFF400000ull /* its own 4 KiB page */
/* ArmSmmu4KBuild reported `pages 10` for our three ranges on the real
 * hardware, so 16 pages leaves 60% headroom while keeping the permanent
 * wired footprint small: at 64 pages the extra 192 KiB of wired memory
 * contributed to a kernel-map exhaustion (kmem_alloc 128 KiB failing with
 * KERN_NO_SPACE at 5.17 s on the first level-4 run). */
#define VIDC_SMMU_TABLE_PAGES   12u
/* Leaves actually mapped: carveout 8 MiB (2048) + qtable 0x310000 (784) +
 * SFR 4 KiB (1) = 2836, so the budget must exceed that or ArmSmmu4KBuild
 * refuses with ArmSmmu4KBudget. */
#define VIDC_SMMU_LEAF_BUDGET   4096u
#define VIDC_SMMU_ACTLR         0x103u        /* DT qcom,actlr for these SIDs */
/* All four NON-SECURE video streams, from the DT contexts [V vidc-dt-audit]:
 *   non_secure_cb       (qcom,vidc,cb-ns,     phandle 0x5bd) -> 0x1940 / 0x1a20 / 0x1944
 *   non_secure_pixel_cb (qcom,vidc,cb-ns-pxl, phandle 0x5bb) -> 0x1943
 * The pixel one was missing, and that matters: if the Xtensa's fetch or its
 * buffer traffic uses 0x1943, that stream keeps whatever handoff SMR state the
 * bootloader left, which can abort the very first access while the core is
 * still silent.  All four now share one context bank, so every access the core
 * can make resolves through our tables. */
#define VIDC_SID_NONSEC         0x1940u
#define VIDC_SID_NONSEC_ALT     0x1A20u
#define VIDC_SID_NONSEC_ALT2    0x1944u
#define VIDC_SID_NONSEC_PIXEL   0x1943u
#define VIDC_NSIDS               3u

static uint32_t vidc_qtable_iova;
static uint32_t vidc_sfr_iova;
static bool     vidc_smmu_on;
static unsigned vidc_smmu_drop_calls;   /* see vidc_smmu_drop() */
static uint32_t vidc_smmu_cb;      /* CB base offset inside apps-smmu */
static uint32_t vidc_smmu_bank;
/* Fault-capture state for the post-boot-release window. */
/* Default 0 = bring-up at init.  Deferring to 9 s dodged the TOUCH 128 KiB
 * failure at 5.171 s but then our 3 MiB queue table made a later 1 MiB
 * kmem_alloc fail at 9.180 s, so the problem is what we KEEP, not when.
 * The teardown below frees the SMMU window as soon as the ladder ends. */
static int      vidc_defer_ms = 0;
static bool     vc_smmu_faults_done;
static unsigned vc_smmu_faults;

/* Verdict accumulator: one line per stage so a single boot returns the ladder. */
static uint32_t vidc_pass_mask;
#define VIDC_STAGE_PROBE    (1u << 0)
#define VIDC_STAGE_POWER    (1u << 1)
#define VIDC_STAGE_ALLOC    (1u << 2)
#define VIDC_STAGE_SMMU     (1u << 3)
#define VIDC_STAGE_QUEUES   (1u << 4)
#define VIDC_STAGE_PAS      (1u << 5)
#define VIDC_STAGE_PUBLISH  (1u << 6)
#define VIDC_STAGE_HFI      (1u << 7)
#define VIDC_STAGE_FWCOPY   (1u << 8)
#define VIDC_STAGE_ALL      0x1FFu

static void
vidc_verdict(const char *stage, int ok)
{
    printf("PANDORA VIDC: STAGE %-8s = %s\n", stage, ok == 0 ? "PASS" : "FAIL");
}

/* Physically contiguous, zeroed, wired pages (same pattern as pandora_gpu.c
 * gpu_contig).  Returns the cached VA and the physical base. */
static vm_offset_t
vidc_contig(uint64_t bytes, uint64_t *pa)
{
    vm_offset_t va = 0;
    if (kmem_alloc_contig(kernel_map, &va, (vm_size_t)bytes, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
        return 0;
    }
    *pa = (uint64_t)ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)va));
    /* Push the KMA_ZERO contents out while the pages are still clean. */
    clean_dcache64((addr64_t)*pa, (unsigned)bytes, TRUE);
    return va;
}

/* Accessors over the CPU mapping.  Callers bracket VPU hand-off with
 * vidc_sync_out() / vidc_sync_in() on the physical range.
 *
 * The bases are vm_offset_t (an integer) rather than byte pointers, so
 * `base + off` is integer arithmetic and the pointer cast below cannot trip
 * -Wcast-align.  This is the same shape as pandora_gpu.c's gpu_rd()/gpu_wr(),
 * which CI accepts under -Weverything -Werror. */
static inline uint32_t
vidc_ld32(vm_offset_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void
vidc_st32(vm_offset_t base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint64_t
vidc_ld64(vm_offset_t base, uint32_t off)
{
    return *(volatile uint64_t *)(base + off);
}

/* Little-endian field reads out of an unaligned byte buffer.  The firmware
 * blob is only byte-aligned, so an ELF header field must not be read through
 * a wider pointer. */
static inline uint16_t
vidc_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t
vidc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t
vidc_qt_rd(uint32_t off)
{
    return vidc_ld32(vidc_qtable_va, off);
}

/* Dword read that wraps inside one ring.  The HFI rings are circular, so a
 * packet near ring_end continues at ring_off; reading it with plain
 * vidc_qt_rd(q_off + n) would silently return the next ring's bytes.  Latent
 * while the core is silent, but wrong-by-construction once HFI works. */
static inline uint32_t
vidc_ring_rd(uint32_t ring_off, uint32_t ring_end, uint32_t off)
{
    if (off >= ring_end) {
        off = ring_off + (off - ring_end);
    }
    return vidc_qt_rd(off);
}

static inline void
vidc_qt_wr(uint32_t off, uint32_t v)
{
    vidc_st32(vidc_qtable_va, off, v);
    __asm__ volatile ("dmb oshst" ::: "memory");
}

/* Push host-written bytes to memory before the VPU reads them. */
static inline void
vidc_sync_out(uint64_t off, unsigned len)
{
    __asm__ volatile ("dmb oshst" ::: "memory");
    clean_dcache64((addr64_t)(vidc_qtable_pa + off), len, TRUE);
    __asm__ volatile ("dmb oshst" ::: "memory");
}

/* Drop stale CPU lines so the next read sees what the VPU wrote. */
static inline void
vidc_sync_in(uint64_t off, unsigned len)
{
    __asm__ volatile ("dmb oshld" ::: "memory");
    flush_dcache64((addr64_t)(vidc_qtable_pa + off), len, TRUE);
    __asm__ volatile ("dmb oshld" ::: "memory");
}

/* Allocate the HFI queue table and the SFR as physically contiguous wired
 * memory.  Filling them in needs their 32-bit device addresses, which come
 * from vidc_smmu_up(), so allocation and publication are separate stages. */
static int
vidc_alloc_buffers(void)
{
    if (!vidc_qtable_va) {
        vidc_map_budget("before qtable");
        vidc_qtable_va = vidc_contig(VIDC_QTABLE_ALLOC, &vidc_qtable_pa);
        if (!vidc_qtable_va) {
            printf("PANDORA VIDC: no %#x contiguous bytes for qtable\n", VIDC_QTABLE_ALLOC);
            return -1;
        }
    }
    /* Now that our window is held, probe again: the pair of lines says exactly
     * how much the allocation cost, which is what the 5.17 s failure was about. */
    vidc_map_budget("after qtable");
    if (!vidc_sfr_va) {
        vidc_sfr_va = vidc_contig(VIDC_SFR_BYTES, &vidc_sfr_pa);
        if (!vidc_sfr_va) {
            printf("PANDORA VIDC: no %#x contiguous bytes for SFR\n", VIDC_SFR_BYTES);
            return -1;
        }
    }
    printf("PANDORA VIDC: buffers qtable PA=%08llx (%#x) SFR PA=%08llx (%#x)\n",
        (unsigned long long)vidc_qtable_pa, VIDC_QTABLE_ALLOC,
        (unsigned long long)vidc_sfr_pa, VIDC_SFR_BYTES);
    if (vidc_qtable_pa >> 32)
        printf("PANDORA VIDC: qtable is above 4 GiB (%#llx); a 32-bit VPU cannot "
               "reach it, the apps-SMMU IOVA below is mandatory\n",
            (unsigned long long)vidc_qtable_pa);
    return 0;
}

/* ------------------------------------------------------------------ */
/* apps-SMMU: 32-bit IOVAs for the queue table and the SFR             */
/* ------------------------------------------------------------------ */

static vm_offset_t vidc_smmu_va;

/* Release the apps-smmu window if it is still mapped.  Idempotent: the failure
 * paths of vidc_smmu_up() and its success path both call it, and so does the
 * ladder teardown.  This exists because the original code unmapped ONLY on
 * success, so every early return leaked 1 MiB of kernel_map -- the same kind of
 * permanent footprint that produced the 5.177 s kmem_alloc failure. */
static void
vidc_smmu_drop(void)
{
    /* NOTE: this used to call ml_io_unmap().  That is illegal for a window made
     * by ml_io_map(): it is a PERMANENT VM map entry and removing it panics
     * (`vm_map_delete: Attempting to remove permanent VM map entry`,
     * restart type 4 at 0.207 s on 2026-11-21 23:36).  The window therefore
     * stays mapped for the lifetime of the driver, exactly like every other
     * pandora_* driver's register window. */
    vidc_smmu_drop_calls++;
}

static inline uint32_t
vidc_smmu_rd(uint32_t off)
{
    if (!vidc_smmu_va) return 0;      /* window released after the ladder */
    return vidc_ld32(vidc_smmu_va, off);
}

static inline void
vidc_smmu_wr(uint32_t off, uint32_t v)
{
    if (!vidc_smmu_va) return;
    vidc_st32(vidc_smmu_va, off, v);
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* Device address the VPU must publish for the queue table / SFR.  With the
 * SMMU up this is the IOVA; without it the PA, which only works when the
 * allocation happens to land below 4 GiB (it does not on this board). */
static inline uint32_t
vidc_qtable_da(void)
{
    return vidc_smmu_on ? vidc_qtable_iova : (uint32_t)vidc_qtable_pa;
}

static inline uint32_t
vidc_sfr_da(void)
{
    return vidc_smmu_on ? vidc_sfr_iova : (uint32_t)vidc_sfr_pa;
}

static inline uint32_t
vidc_ring_da(unsigned i)
{
    return vidc_qtable_da() + IFACEQ_RING_OFFS(i);
}

/* Claim a free apps-SMMU bank, build 4K stage-1 tables that map the queue
 * table and the SFR at fixed 32-bit IOVAs, program the context and route the
 * three non-secure video SIDs to it.  The bank/ASID/stream lease follows the
 * same discipline as pandora_wifi.c: only a disabled, unreferenced bank is
 * taken, and nothing is touched while another endpoint can still reach it. */
static int
vidc_smmu_up(void)
{
    if (vidc_smmu_on) return 0;
    if (!vidc_qtable_va || !vidc_sfr_va) {
        printf("PANDORA VIDC: SMMU before buffers\n");
        return -1;
    }
    if (!vidc_smmu_va) {
        vidc_smmu_va = ml_io_map(VIDC_SMMU_PA, VIDC_SMMU_SIZE);
        if (!vidc_smmu_va) {
            printf("PANDORA VIDC: cannot map apps-smmu at %#x\n", VIDC_SMMU_PA);
            return -1;
        }
    }

    const uint32_t id0 = vidc_smmu_rd(0x20);
    const uint32_t id1 = vidc_smmu_rd(0x24);
    const uint32_t id2 = vidc_smmu_rd(0x28);
    const unsigned slots = id0 & 0xffu, banks = id1 & 0xffu, s2banks = (id1 >> 16) & 0xffu;
    const uint64_t page = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
    const uint64_t cbbase = page << (((id1 >> 28) & 7) + 1);
    printf("PANDORA VIDC: apps-smmu id0=%08x id1=%08x id2=%08x slots=%u banks=%u "
           "s2=%u page=%#llx cbbase=%#llx\n", id0, id1, id2, slots, banks, s2banks,
        page, cbbase);
    if (!(id0 & (1u << 27)) || !slots || !banks || banks > 128 || s2banks >= banks ||
        cbbase + banks * page > VIDC_SMMU_SIZE) {
        printf("PANDORA VIDC: apps-smmu layout unsupported\n");
        return -1;
    }

    const uint16_t sids[VIDC_NSIDS] = { VIDC_SID_NONSEC, VIDC_SID_NONSEC_ALT,
        VIDC_SID_NONSEC_ALT2 };
    ArmSmmu4KStream route[VIDC_NSIDS];
    ArmSmmu4KStreamState sstate = { 0 };
    ArmSmmu4KStreamResult sres;
    /* Read-only: report what the bootloader left in the SMR/S2CR table for all
     * five video contexts (DT vidc-dt-audit: cb-ns 0x1940/0x1a20/0x1944,
     * cb-ns-pxl 0x1943, cb-sec-* 0x1941/0x1a21/0x1946).  The 4th non-secure
     * stream could not be attached because only three SMR slots were idle, and
     * knowing WHICH bank the pre-assigned one points at is the difference
     * between "the core's traffic is unrouted" and "it is routed to a bank with
     * no tables". */
    {
        static const uint16_t probe_sids[] = { 0x1940u, 0x1941u, 0x1943u, 0x1944u,
            0x1946u, 0x1a20u, 0x1a21u };
        for (unsigned sl = 0; sl < slots; sl++) {
            const uint32_t smr = vidc_smmu_rd(0x800u + 4u * sl);
            const uint32_t s2cr = vidc_smmu_rd(0xC00u + 4u * sl);
            const uint32_t sidv = smr & 0xffffu;
            for (unsigned k = 0; k < sizeof(probe_sids) / sizeof(probe_sids[0]); k++) {
                if (sidv != probe_sids[k]) continue;
                printf("PANDORA VIDC: handoff slot %u sid=%#x smr=%08x s2cr=%08x "
                       "bank=%u type=%u\n", sl, sidv, smr, s2cr, s2cr & 0xffu,
                    (s2cr >> 16) & 3u);
            }
        }
    }
    unsigned nroute = 0;
    bool used[128] = {false}, asid_used[256] = {false};
    bool slot_taken[3] = {false, false, false};

    pandora_apps_smmu_lock();
    /* A stream slot is claimable when its S2CR still reports the "fault"
     * encoding (2 << 16) that the bootloader leaves on unowned SIDs, or when
     * its SMR already names one of our SIDs and S2CR is likewise idle.  That is
     * exactly what ArmSmmu4KAttachStreams()'s scan() accepts, so do not rewrite
     * S2CR here -- the kit installs the bank itself. [M ArmSmmu4KStreams.c:45] */
    /* Mirror scan() exactly: it rejects an owned slot unless
     * (s2cr & ~valid_s2cr) == FAULT, and when the stream already validates it
     * also demands smr == (valid_smr | sid). valid_smr is 0 in EXIDS mode and
     * 1<<31 otherwise; valid_s2cr is 1<<10 in EXIDS mode and 0 otherwise.
     * [M ArmSmmu4KStreams.c:12-14,44-47] */
    const bool exids = (id0 & (1u << 8)) != 0;
    const uint32_t valid_smr = exids ? 0u : (1u << 31);
    const uint32_t valid_s2cr = exids ? (1u << 10) : 0u;
    const uint32_t smr_sid_mask = exids ? 0xffffu : ((1u << ((id0 >> 9) & 15)) - 1u);
    for (unsigned s = 0; s < slots; s++) {
        const uint32_t smr = vidc_smmu_rd(0x800 + s * 4);
        const uint32_t s2cr = vidc_smmu_rd(0xc00 + s * 4);
        const bool live = exids ? ((s2cr & valid_s2cr) != 0) : ((smr & valid_smr) != 0);
        if ((s2cr & ~valid_s2cr) != (2u << 16)) {
            if (live && ((s2cr >> 16) & 3) == 0 && (s2cr & 0xffu) < banks)
                used[s2cr & 0xffu] = true;
            continue;
        }
        for (unsigned k = 0; k < 3; k++) {
            if (slot_taken[k]) continue;
            if ((sids[k] & ~smr_sid_mask) != 0) continue;
            if (live && smr != (valid_smr | sids[k])) continue;
            route[nroute].slot = s;
            route[nroute].sid = sids[k];
            slot_taken[k] = true;
            nroute++;
            break;
        }
        if (nroute == VIDC_NSIDS) break;
    }
    if (nroute != VIDC_NSIDS) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: only %u idle SMR slots for %u video SIDs\n", nroute,
            VIDC_NSIDS);
        return -1;
    }
    printf("PANDORA VIDC: stream slots %u/%u/%u for SIDs %#x/%#x/%#x "
           "(pixel 0x1943 not attachable: no idle SMR slot)\n",
        route[0].slot, route[1].slot, route[2].slot, sids[0], sids[1], sids[2]);

    unsigned bank = banks;
    for (unsigned b = s2banks; b < banks; b++) {
        const uint32_t ctl = vidc_smmu_rd((uint32_t)(cbbase + b * page));
        if (!(ctl & 1u) && !used[b]) { bank = b; break; }
        if (ctl & 1u) {
            const uint64_t ttbr = vidc_ld64(vidc_smmu_va,
                (uint32_t)(cbbase + b * page + 0x20));
            if ((ttbr >> 48) < 256) asid_used[ttbr >> 48] = true;
        }
    }
    if (bank >= banks) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: no free apps-smmu bank (%u banks, s2banks %u)\n",
            banks, s2banks);
        return -1;
    }
    unsigned asid = 1;
    while (asid < 256 && asid_used[asid]) asid++;
    if (asid == 256) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: no free ASID\n");
        return -1;
    }

    uint64_t tpa = 0;
    vm_offset_t tva = vidc_contig((uint64_t)VIDC_SMMU_TABLE_PAGES * 0x1000, &tpa);
    if (!tva) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: no table pool\n");
        return -1;
    }
    static const uint8_t widths[6] = { 32, 36, 40, 42, 44, 48 };
    const unsigned encoding = (id2 >> 4) & 15;
    if (encoding >= 6) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: unencodable OAS field %u\n", encoding);
        return -1;
    }
    const unsigned oas = widths[encoding] < 40 ? widths[encoding] : 40;
    if ((vidc_qtable_pa + VIDC_QTABLE_ALLOC) >> oas ||
        (vidc_sfr_pa + VIDC_SFR_BYTES) >> oas) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: buffers exceed the %u-bit output address space\n", oas);
        return -1;
    }

    ArmSmmu4KConfig tcfg = { 32, oas, tpa, VIDC_SMMU_LEAF_BUDGET };
    ArmSmmu4KResult tres;
    /* ArmSmmu4KBuild requires IOVA-sorted, disjoint ranges, so the ascending
     * order here is 0x9a600000 (identity-mapped carveout), 0xff000000 (queue
     * table) and 0xff400000 (SFR).
     *
     * Why the carveout must be mapped: once these SIDs are routed into the
     * bank, EVERY VPU access is translated, including the ones the firmware
     * makes to its own image and data at video_region@9a600000.  An
     * IOVA == PA identity mapping keeps whatever physical addresses the
     * firmware computed (and PAS programmed) valid, the same way
     * pandora_dpu.c identity-maps the ABL's panel memory.  The carveout is
     * inside the non_secure_cb window and below the 0xffc00000 DMA limit. */
    ArmSmmu4KRange ranges[3];
    ranges[0] = (ArmSmmu4KRange){ VIDEO_REGION_PA, VIDEO_REGION_PA, VIDEO_REGION_SIZE,
        ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2 };
    ranges[1] = (ArmSmmu4KRange){ VIDC_IOVA_BASE, vidc_qtable_pa, VIDC_QTABLE_ALLOC,
        ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2 };
    ranges[2] = (ArmSmmu4KRange){ VIDC_IOVA_SFR, vidc_sfr_pa, VIDC_SFR_BYTES,
        ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2 };
    ArmSmmu4KStatus st = ArmSmmu4KBuild(&tcfg, ranges, 3, (uint64_t *)tva,
        VIDC_SMMU_TABLE_PAGES, &tres);
    if (st != ArmSmmu4KSuccess) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: SMMU tables status %d\n", (int)st);
        return -1;
    }
    clean_dcache64((addr64_t)tpa, VIDC_SMMU_TABLE_PAGES * 0x1000, TRUE);

    ArmSmmu4KMmio mmio = { (volatile unsigned char *)vidc_smmu_va, VIDC_SMMU_SIZE };
    ArmSmmu4KIo io;
    if (ArmSmmu4KBindMmio(&mmio, &io) != ArmSmmu4KSuccess) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: SMMU mmio bind failed\n");
        return -1;
    }
    if (ArmSmmu4KCheckFaultedStreams(&io, route, VIDC_NSIDS, bank, &sres) != ArmSmmu4KSuccess) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: SMMU reports faulted streams before attach\n");
        return -1;
    }
    const uint32_t cb_off = (uint32_t)(cbbase + bank * page);
    vidc_smmu_wr(cb_off + 4, VIDC_SMMU_ACTLR);
    ArmSmmu4KContextConfig ctx = { &tcfg, NULL, &tres, NULL, id0, id2, asid, 0, 0, 1 };
    ArmSmmu4KProgramResult prog;
    st = ArmSmmu4KProgramDisabledContext(&io, bank, &ctx, 1000, &prog);
    if (st != ArmSmmu4KSuccess) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: SMMU context status %d (sctlr %08x)\n", (int)st,
            prog.readback.sctlr);
        return -1;
    }
    if (ArmSmmu4KAttachStreams(&io, &sstate, route, VIDC_NSIDS, bank, &ctx, &sres) !=
        ArmSmmu4KSuccess) {
        pandora_apps_smmu_unlock();
        vidc_smmu_drop();
        printf("PANDORA VIDC: SMMU stream attach failed\n");
        return -1;
    }
    /* No fault dispatcher exists for these streams; keep CFIE clear so a fault
     * is observed through FSR polling instead of an unhandled IRQ. */
    uint32_t sctlr = vidc_smmu_rd(cb_off);
    vidc_smmu_wr(cb_off, sctlr & ~(1u << 6));
    pandora_apps_smmu_unlock();
    vidc_smmu_drop();

    vidc_qtable_iova = (uint32_t)VIDC_IOVA_BASE;
    vidc_sfr_iova = (uint32_t)VIDC_IOVA_SFR;
    vidc_smmu_cb = cb_off;
    vidc_smmu_bank = bank;
    vidc_smmu_on = true;
    /* Give the 1 MiB of apps-smmu VA back NOW, not at the end of the ladder.
     * Measured: with the teardown only at the end, the kernel died at 5.177 s
     * inside HFI polling on someone else's 864 KiB kmem_alloc
     * (`failed with 3 @vm_kern.c:178`), i.e. we were still holding ~5.5 MiB.
     * The bank/ASID/CB programming lives in the SMMU hardware, so the mapping
     * is dead weight the moment attach succeeds. */
    printf("PANDORA VIDC: released %#x bytes of apps-smmu VA after attach\n",
        VIDC_SMMU_SIZE);
    printf("PANDORA VIDC: apps-smmu bank %u asid %u cb %#x sctlr %08x/%08x "
           "tcr %08x ttbr0 %#llx pages %zu\n", bank, asid, cb_off, sctlr,
        prog.readback.sctlr, prog.readback.tcr, prog.readback.ttbr0,
        tres.table_pages);
    printf("PANDORA VIDC: IOVA qtable %08x (PA %08llx) SFR %08x (PA %08llx) "
           "sids %#x/%#x/%#x\n", vidc_qtable_iova,
        (unsigned long long)vidc_qtable_pa, vidc_sfr_iova,
        (unsigned long long)vidc_sfr_pa, sids[0], sids[1], sids[2]);
    return 0;
}

/* Fill the queue table now that the device addresses are known. */
static int
vidc_fill_queues(void)
{
    if (!vidc_qtable_va || !vidc_sfr_va) {
        printf("PANDORA VIDC: fill before alloc\n");
        return -1;
    }
    if (!vidc_smmu_on && ((vidc_qtable_pa | vidc_sfr_pa) >> 32)) {
        printf("PANDORA VIDC: refusing to publish truncated 32-bit addresses "
               "(qtable %#llx SFR %#llx)\n",
            (unsigned long long)vidc_qtable_pa, (unsigned long long)vidc_sfr_pa);
        return -1;
    }

    struct vidc_queue_table *t = (struct vidc_queue_table *)vidc_qtable_va;
    memset(t, 0, sizeof(*t));
    t->version = 0;
    t->size = (uint32_t)sizeof(*t);            /* [M] 456, not the 3 MiB window */
    t->qhdr_size = (uint32_t)sizeof(struct vidc_queue_header);
    t->qhdr0_offset = (uint32_t)((char *)&t->qhdr[0] - (char *)t);   /* [M] 288 */
    t->num_q = IFACEQ_NUMQ;
    t->num_active_q = IFACEQ_NUMQ;
    t->device_addr = vidc_qtable_da();          /* device address, not the PA */
    const char *nm = "iris-hfi-queues";
    for (unsigned i = 0; i < (unsigned)sizeof(t->name) - 1 && nm[i]; i++) t->name[i] = nm[i];

    for (unsigned i = 0; i < IFACEQ_NUMQ; i++) {
        struct vidc_queue_header *q = &t->qhdr[i];
        q->status = 1;                         /* [M] active */
        q->start_addr = vidc_ring_da(i);        /* device address of the ring */
        q->header_type = IFACEQ_DFLT_QHDR;     /* [M] 0x0101, word 2 */
        q->queue_type = i;                      /* [M] word 3 */
        q->q_size = IFACEQ_QUEUE_WORDS;        /* [M] words, not bytes */
        q->pkt_size = 0;
        q->rx_wm = 1;
        q->tx_wm = 1;
        q->rx_req = (i == 2) ? 0 : 1;          /* [M] debug queue differs */
        q->tx_req = 0;
        q->rx_irq_status = 0;                  /* [M] starts at 0 */
        q->tx_irq_status = 0;
        q->read_idx = 0;
        q->write_idx = 0;
    }
    *(volatile uint32_t *)vidc_sfr_va = VIDC_SFR_BYTES;   /* [M] first word = SFR size */

    /* One clean for the header and every ring header the VPU will read. */
    vidc_sync_out(0, (unsigned)sizeof(*t));
    __asm__ volatile ("dmb oshst" ::: "memory");
    clean_dcache64((addr64_t)vidc_sfr_pa, (unsigned)VIDC_SFR_BYTES, TRUE);

    printf("PANDORA VIDC: qtable DA=%08x hdr=%u qhdr0=%u q_size=%u ring0=%08x\n",
        vidc_qtable_da(), t->size, t->qhdr0_offset,
        IFACEQ_QUEUE_WORDS, vidc_ring_da(0));
    printf("PANDORA VIDC: rings %08x/%08x/%08x SFR DA=%08x\n",
        vidc_ring_da(0), vidc_ring_da(1), vidc_ring_da(2), vidc_sfr_da());
    return 0;
}

static inline uint32_t
vidc_rd(uint32_t off)
{
    if (!vidc_mmio_on) return 0;      /* MMIO window released after the ladder */
    uint32_t v = *(volatile uint32_t *)(vidc_mmio + off);
    __asm__ volatile ("dmb oshld" ::: "memory");
    return v;
}

static inline void
vidc_wr(uint32_t off, uint32_t v)
{
    if (!vidc_mmio_on) return;
    *(volatile uint32_t *)(vidc_mmio + off) = v;
    __asm__ volatile ("dmb oshst" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Level 1: read-only identity probe.  Maps MMIO and prints the registers
 * whose values establish the vpu4 variant.  No power or clock changes. */
/* ------------------------------------------------------------------ */
static int
vidc_probe(void)
{
    if (!vidc_mmio_on) {
        vidc_mmio = ml_io_map(VIDC_PA, VIDC_SIZE);
        if (!vidc_mmio) {
            printf("PANDORA VIDC: ml_io_map(%08x) failed\n", VIDC_PA);
            return -1;
        }
        vidc_mmio_on = true;
    }

    uint32_t efuse = vidc_rd(R_WRAPPER_EFUSE);
    printf("PANDORA VIDC: MMIO %08x -> %p\n", VIDC_PA, (void *)vidc_mmio);
    printf("PANDORA VIDC: EFUSE=%08x CORE_PWR=%08x CORE_CLK=%08x IDLE=%08x\n",
        efuse, vidc_rd(R_WRAPPER_CORE_PWR),
        vidc_rd(R_WRAPPER_CORE_CLK), vidc_rd(R_VCODEC_SS_IDLE));
    printf("PANDORA VIDC: INTR_STATUS=%08x INTR_MASK=%08x X2RPMH=%08x\n",
        vidc_rd(R_WRAPPER_INTR_STATUS), vidc_rd(R_WRAPPER_INTR_MASK),
        vidc_rd(R_CPU_X2RPMH));
    printf("PANDORA VIDC: AON_LPI_CTRL=%08x AON_LPI_STATUS=%08x NOC_REQ=%08x NOC_ACK=%08x\n",
        vidc_rd(R_AON_NOC_LPI_CTRL), vidc_rd(R_AON_NOC_LPI_STATUS),
        vidc_rd(R_AON_NOC_RESET_REQ), vidc_rd(R_AON_NOC_RESET_ACK));
    printf("PANDORA VIDC: efuse apv_disabled=%u vpp1_disabled=%u vpp0_disabled=%u\n",
        (efuse >> 27) & 1u, (efuse >> 28) & 1u, (efuse >> 29) & 1u);
    return 0;
}

/* ------------------------------------------------------------------ */
/* videocc power-up.
 *
 * GDSC offsets are [V]: parsed out of videocc-canoe.ko .data, one unique hit
 * per domain (gdscr at struct+0x610, flags/pwrsts at struct+0x640).  The
 * parent relation mvs0c -> mvs0 is [V]: Android pm_genpd_detail.txt lists
 * video_cc_mvs0_gdsc as a child of video_cc_mvs0c_gdsc, so mvs0c is powered
 * first.  Branch CB/CR offsets are [V] too: clock_regs.txt:559-574 gives
 * mvs0c 0x8164, mvs0 0x80D0, vpp0 0x8134, vpp1 0x8108, mvs0a 0x8090.
 *
 * GDSC flags 0x02A0 (mvs0c 0x00A0) mean POLL_CFG_GDSCR|RETAIN_FF_ENABLE
 * (|HW_CTRL_TRIGGER).  We deliberately leave the domain in SW mode (do not set
 * bit1) because the HW-control write sequence is not verified. */
#define VIDEOCC_PA             0x020F0000u
#define VIDEOCC_SIZE           0x00010000u

/* The video block's AXI bus clocks live in GCC, not videocc, and upstream
 * enables them FIRST (iris_vpu4x_enable_hardware_clocks: IRIS_AXI_CLK before
 * everything else).  The vidc node's DT clock-names lists them explicitly:
 *   gcc_video_axi1_clk -> /soc/clock-controller@100000 args 0xba
 *   gcc_video_axi0_clk -> /soc/clock-controller@100000 args 0xb9
 * Registers from the GCC driver source (work/references/
 * drivers_clk_qcom_gcc-canoe.c:3617-3644):
 *   gcc_video_axi0_clk enable_reg 0x32018 mask BIT(0) halt_check BRANCH_HALT_SKIP
 *   gcc_video_axi1_clk enable_reg 0x3202c mask BIT(0) halt_check BRANCH_HALT_SKIP
 * BRANCH_HALT_SKIP means there is NO halt/CLK_OFF bit to poll -- a driver that
 * waits for bit31 to clear (like vidc_branch_on does for videocc) would spin
 * forever, so these two are enabled with a plain bit0 readback.
 * Without these the Xtensa cannot move a single beat over the bus, which is
 * exactly the observed all-reset state. */
#define GCC_PA                 0x00100000u
/* Only the two video AXI branch registers are needed (0x32018 / 0x3202c), so map
 * a single 4 KiB page at 0x32000 instead of 256 KiB.  Measured reason: with the
 * 256 KiB window the kernel died at 5.166 s on someone else's 864 KiB
 * kmem_alloc (`failed with 3 @vm_kern.c:178`), and ml_io_map windows cannot be
 * released (unmapping a permanent entry panics). */
#define GCC_MAP_OFF            0x00032000u
#define GCC_SIZE               0x00001000u
#define GCC_VIDEO_AXI0_CLK     0x00032018u
#define GCC_VIDEO_AXI1_CLK     0x0003202Cu

struct vidc_gdsc_desc {
    const char *name;
    uint32_t    off;        /* relative to VIDEOCC_PA */
    uint32_t    flags;
};

static const struct vidc_gdsc_desc vidc_gdscs[] = {
    { "iris-ctl", 0x814Cu, 0x00A0u },   /* mvs0c, parent */
    { "vcodec",   0x80A8u, 0x02A0u },   /* mvs0  */
    { "vpp0",     0x8120u, 0x02A0u },
    { "vpp1",     0x80F4u, 0x02A0u },
    { "apv",      0x807Cu, 0x02A0u },   /* mvs0a */
};

static vm_offset_t vidc_cc;
static vm_offset_t vidc_gcc;      /* GCC window for the video AXI branches */

static int
vidc_gdsc_on(const struct vidc_gdsc_desc *g)
{
    volatile uint32_t *gdscr = (volatile uint32_t *)(vidc_cc + g->off);
    uint32_t v = *gdscr;

    if (v & 0x80000000u) {
        printf("PANDORA VIDC: gdsc %s already on (gdscr=%08x cfg=%08x)\n",
            g->name, v, *(volatile uint32_t *)(vidc_cc + g->off + 4));
        return 0;
    }
    *gdscr = v & ~1u;                  /* SW_COLLAPSE = 0 */
    __asm__ volatile ("dmb oshst" ::: "memory");
    /* 4000 x 10 us = 40 ms.  Android waits on the same POWER_UP_COMPLETE bit
     * with a usleep_range, so sweeping the whole budget is the honest default. */
    for (unsigned i = 0; i < 4000; i++) {
        uint32_t s = *(volatile uint32_t *)(vidc_cc + g->off + 4);  /* CFG_GDSCR */
        if (s & (1u << 16)) {          /* POWER_UP_COMPLETE */
            if (g->flags & 0x80u) {    /* RETAIN_FF_ENABLE */
                *(volatile uint32_t *)(vidc_cc + g->off + 4) = s | (1u << 11);
            }
            printf("PANDORA VIDC: gdsc %s on (gdscr %08x -> %08x cfg=%08x)\n",
                g->name, v, *gdscr, s);
            return 0;
        }
        delay(10);
    }
    printf("PANDORA VIDC: gdsc %s power-up TIMEOUT (gdscr=%08x cfg=%08x)\n",
        g->name, *gdscr, *(volatile uint32_t *)(vidc_cc + g->off + 4));
    return -1;
}

/* Snapshot of the registers that only the firmware (or a reset default)
 * would change.  Reading the same four values at probe, after power-up and
 * after boot release separates "reset default" from "firmware wrote it"
 * without another guess: 0xB0010 is never written by this driver. */
static void
vidc_snap(const char *when)
{
    if (!vidc_mmio_on) return;
    printf("PANDORA VIDC: snap %-12s int=%08x/%08x tz=%08x/%08x/%08x "
           "core=%08x/%08x idle=%08x\n", when,
        vidc_rd(R_WRAPPER_INTR_STATUS), vidc_rd(R_WRAPPER_INTR_MASK),
        vidc_rd(0xC0010u), vidc_rd(0xC0014u), vidc_rd(0xC0018u),
        vidc_rd(R_WRAPPER_CORE_PWR), vidc_rd(R_WRAPPER_CORE_CLK),
        vidc_rd(R_VCODEC_SS_IDLE));
    printf("PANDORA VIDC: x2rpmh=%08x core_power_on=%u (MSK_CORE_POWER_ON=BIT1)\n",
        vidc_rd(R_CPU_X2RPMH), (vidc_rd(R_CPU_X2RPMH) >> 1) & 1u);
}

static inline uint32_t
vidc_cc_rd(uint32_t off)
{
    if (!vidc_cc) return 0;           /* videocc window released after power-up */
    return *(volatile uint32_t *)(vidc_cc + off);
}

/* GCC branch with BRANCH_HALT_SKIP: enable = set bit0, verify bit0 only. */
static int
vidc_gcc_branch_on(uint32_t off, const char *name)
{
    volatile uint32_t *cbcr = (volatile uint32_t *)(vidc_gcc + off);
    const uint32_t before = *cbcr;
    *cbcr = before | 1u;
    __asm__ volatile ("dmb oshst" ::: "memory");
    const uint32_t after = *cbcr;
    printf("PANDORA VIDC: gcc %s (%#x) %08x -> %08x %s\n", name, off, before, after,
        (after & 1u) ? "enabled" : "DID NOT STICK");
    return (after & 1u) ? 0 : -1;
}

static void
vidc_branch_on(uint32_t off)
{
    volatile uint32_t *cbcr = (volatile uint32_t *)(vidc_cc + off);
    uint32_t v = *cbcr;

    if (!(v & 1u)) {
        *cbcr = v | 1u;                /* CLK_ENABLE */
        __asm__ volatile ("dmb oshst" ::: "memory");
    }
    for (unsigned i = 0; i < 2000; i++) {
        uint32_t s = *cbcr;
        if (!(s & 0x80000000u)) {      /* CLK_OFF cleared */
            printf("PANDORA VIDC: branch %#x on (%08x -> %08x)\n", off, v, s);
            return;
        }
        delay(1);
    }
    printf("PANDORA VIDC: branch %#x halt timeout (cbcr=%08x)\n", off, *cbcr);
}

/* Bridge sync reset, mainline iris_vpu4x_set_hwmode order: BIT0|BIT1 ->
 * BIT1 -> 0.  APV bridge exists on this variant (APV encoder advertised).
 * The vendor does this in set_hwmode, i.e. AFTER boot_firmware [K iris_core.c
 * order is power_on -> fw_load -> boot_firmware -> switch_to_hwmode]; doing it
 * before firmware release is the conservative order we can verify by register
 * read-back, so keep it here and note the difference. */
/* AON MVP NOC reset handshake, verbatim from upstream's power_on_hardware.
 * Returns 0 on a completed handshake.  Every value is logged: if the ACK never
 * mirrors the request, that is itself the finding (the NoC is held elsewhere). */
/* AON LPI handshake that upstream performs immediately before the NoC reset
 * (iris_vpu4x.c:108-118 and :308-318).  REQ_POWER_DOWN_PREP is written to the
 * control register and the status must report DONE; a DENY/ACTIVE status means
 * the NoC is busy elsewhere, which is worth seeing in the log. */
/* Upstream polls VCODEC_SS_IDLE_STATUSN before the NoC handshake
 * (iris_vpu4x.c:297) so it never resets the NoC while a video client is busy.
 * On this board the register reads 0x7fff (all idle), so this normally passes
 * on the first iteration -- but it is part of the documented order and it is
 * cheap, so it is checked rather than assumed. */
static int
vidc_wait_idle(void)
{
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < 1000; i++) {
        v = vidc_rd(R_VCODEC_SS_IDLE);
        if (v) break;                 /* any non-zero = subsystem reports idle */
        delay(1);
    }
    printf("PANDORA VIDC: idle status %08x after %u us\n", v, i);
    return v ? 0 : -1;
}

static void
vidc_bridge_reset(void)
{
    vidc_wr(R_CPU_APV_BRIDGE_RESET, CORE_BRIDGE_SW_RESET | CORE_BRIDGE_HW_RST_DIS);
    delay(10);
    vidc_wr(R_CPU_APV_BRIDGE_RESET, CORE_BRIDGE_HW_RST_DIS);
    delay(10);
    vidc_wr(R_CPU_APV_BRIDGE_RESET, 0);
    delay(10);
    vidc_wr(R_CPU_AHB_BRIDGE_RESET, CORE_BRIDGE_SW_RESET | CORE_BRIDGE_HW_RST_DIS);
    delay(10);
    vidc_wr(R_CPU_AHB_BRIDGE_RESET, CORE_BRIDGE_HW_RST_DIS);
    delay(10);
    vidc_wr(R_CPU_AHB_BRIDGE_RESET, 0);
    delay(10);
    printf("PANDORA VIDC: bridge reset done (apv=%08x ahb=%08x)\n",
        vidc_rd(R_CPU_APV_BRIDGE_RESET), vidc_rd(R_CPU_AHB_BRIDGE_RESET));
}

static int
vidc_power_up(void)
{
    if (!vidc_cc) {
        vidc_cc = ml_io_map(VIDEOCC_PA, VIDEOCC_SIZE);
        if (!vidc_cc) {
            printf("PANDORA VIDC: ml_io_map(videocc) failed\n");
            return -1;
        }
    }
    int failed = 0;
    for (unsigned i = 0; i < sizeof(vidc_gdscs) / sizeof(vidc_gdscs[0]); i++) {
        if (vidc_gdsc_on(&vidc_gdscs[i]) != 0) failed++;
    }
    /* All seven hardware clocks upstream asks for.  The freerun and BSE branches
     * were missing and that is the "core never executes" candidate worth
     * testing: with HW_FREERUN gated the Xtensa cannot fetch its firmware, which
     * shows up as no ready bit and no SMMU transactions at all -- precisely the
     * observed state, as opposed to a translation fault.
     * Base offsets are the captured videocc-canoe.ko clock objects:
     *   video_cc_mvs0_clk 0x80d0, mvs0b_clk 0x80bc, mvs0c_clk 0x8164,
     *   mvs0a_clk 0x8090, mvs0_vpp0_clk 0x8134, mvs0_vpp1_clk 0x8108,
     *   mvs0_freerun_clk 0x80e4                            [V clock_regs.txt:559-574]
     * Upstream order is AXI, HW_FREERUN, HW, BSE_HW, VPP0, VPP1, APV. */
    /* AXI first, exactly like upstream's enable_hardware_clocks order. */
    if (!vidc_gcc) {
        vidc_gcc = ml_io_map(GCC_PA + GCC_MAP_OFF, GCC_SIZE);
    }
    if (!vidc_gcc) {
        printf("PANDORA VIDC: ml_io_map(GCC) failed; AXI clocks cannot be voted\n");
        failed++;
    } else {
        if (vidc_gcc_branch_on(GCC_VIDEO_AXI0_CLK - GCC_MAP_OFF, "video_axi0") != 0) failed++;
        if (vidc_gcc_branch_on(GCC_VIDEO_AXI1_CLK - GCC_MAP_OFF, "video_axi1") != 0) failed++;
    }
    vidc_branch_on(0x8164u);           /* mvs0c (AXI)   */
    /* 0x80e4 is NOT a plain branch: its captured clock object has no +0x30/+0x34
     * fields and a write there does not stick (`20010c00 -> 20010c00`), so the
     * freerun clock cannot be enabled through it.  Left out deliberately
     * instead of pretending it was turned on. */
    vidc_branch_on(0x80D0u);           /* mvs0 (HW)     */
    vidc_branch_on(0x80BCu);           /* mvs0b (BSE)   */
    vidc_branch_on(0x8134u);           /* vpp0          */
    vidc_branch_on(0x8108u);           /* vpp1          */
    vidc_branch_on(0x8090u);           /* mvs0a (APV)   */
    (void)vidc_wait_idle();                /* informational: 0 here, 0x7fff later */
    /* [M] Vendor disassembly of THIS module: 0x1F000 appears ONLY in
     * __power_off_iris4 / __power_off_iris4_hardware (0x166a4, 0x16720, 0x184a4,
     * 0x1852c); __power_on_iris4* never touches the AON NoC reset block.  The
     * upstream iris_vpu4x handshake therefore does not belong in this device's
     * power-up: asserting it left 0x1F004 at 0 forever and turned a previously
     * PASSing power stage into FAIL (measured 01:5x and 03:5x).  It belongs to
     * power-off, and is recorded there instead of being invented here. */
    vidc_bridge_reset();

    /* WRAPPER_CORE_CLOCK_CONFIG semantics are inverted from the name:
     * CORE_CLK_RUN == 0x0 [M iris_vpu_register_defines.h], and bit1 is
     * APV_CLK_HALT, which upstream clears by writing 0x0 [M iris_vpu4x.c:98-101].
     * A halted wrapper clock would leave the Xtensa unable to fetch, which is
     * exactly "no ready bit, no interrupts, no SMMU traffic". */
    uint32_t clk_cfg = vidc_rd(R_WRAPPER_CORE_CLK);
    if (clk_cfg != 0) {
        vidc_wr(R_WRAPPER_CORE_CLK, 0);
        __asm__ volatile ("dsb sy" ::: "memory");
        printf("PANDORA VIDC: core clock config %08x -> %08x (CORE_CLK_RUN=0)\n",
            clk_cfg, vidc_rd(R_WRAPPER_CORE_CLK));
        clk_cfg = vidc_rd(R_WRAPPER_CORE_CLK);
    }
    printf("PANDORA VIDC: videocc powered (%d/5 GDSC failed), CORE_PWR=%08x CORE_CLK=%08x\n",
        failed, vidc_rd(R_WRAPPER_CORE_PWR), clk_cfg);
    vidc_snap("after power");
    /* A branch reading CLK_OFF=0 only means the *gate* is open; if its RCG has no
     * root enabled or no source selected, the core still gets no clock and never
     * executes.  RCG2 layout is CFG_RCGR @+0, CMD_RCGR @+4, M/N/D @+8/+0xC/+0x10
     * [K clock_regs.txt:202-290 __clk_rcg2_configure]; base offsets come from the
     * captured videocc-canoe.ko objects.  Read-only: this decides "gated" vs
     * "configured but core still idle" without another guess. */
    static const struct { const char *name; uint32_t off; } rcgs[] = {
        { "mvs0a", 0x8000u }, { "mvs0b", 0x8018u }, { "mvs0", 0x8030u },
        { "mvs0c", 0x8048u }, { "ahb",   0x8060u },
    };
    for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
        printf("PANDORA VIDC: rcg %-5s cfg=%08x cmd=%08x m/n/d=%08x/%08x/%08x\n",
            rcgs[i].name,
            vidc_cc_rd(rcgs[i].off), vidc_cc_rd(rcgs[i].off + 4),
            vidc_cc_rd(rcgs[i].off + 8), vidc_cc_rd(rcgs[i].off + 0xC),
            vidc_cc_rd(rcgs[i].off + 0x10));
    }
    /* Every reader of the GCC/videocc windows has now run (snapshot + RCG
     * dump), so hand them back before the long tail starts.  The ordering is
     * load-bearing: releasing first made the snapshot print zeros and hid the
     * RCG configuration -- the very evidence this driver exists to produce. */
    printf("PANDORA VIDC: gcc+videocc windows kept mapped (%#x) -- unmapping "
           "ml_io_map entries panics\n", (unsigned)(GCC_SIZE + VIDEOCC_SIZE));
    return failed ? -1 : 0;
}

/* Publish qtable/SFR to the firmware and release the VPU core.
 *
 * Sequence is byte-for-byte what __boot_firmware_iris4 does [K]:
 *   0xA0064 <- qtable PA            (32-bit)
 *   0xA0068 <- 0x300000             (published window)
 *   0xA0054 <- qtable PA            (second copy)
 *   0xA0050 <- 1                    (enable)
 *   0xA0060 <- SFR PA               (vendor: core->+0x13E8, the SFR carveout)
 *   0xA0024/0xA0028/0xA005C         (TZ content-protection span: the vendor
 *                                    only writes these when it has a CP table
 *                                    with a non-zero start, which we do not)
 *   0xB078  <- 1                    (WRAPPER SPARE_0, canoe-vidc-v2 match path)
 *   0xA0048 <- 1                    (boot control: release the VPU)
 *   poll 0xA004C up to 1000x, 50-100us apart: bits1-3 = error, bit0 = ready
 *   0xA0148 <- 1                    (CPU_CS_H2XSOFTINTEN, host->Xtensa IRQ)
 *   0xA0168 <- 0                    (CPU_CS_X2RPMH)
 *
 * The earlier revision stopped after the five publish registers and immediately
 * polled 0xA004C without 0xB078/0xA0048, so the core was never actually
 * released from reset -- that alone explains "queue table publish TIMEOUT". */
static int
vidc_publish_queues(void)
{
    if (!vidc_qtable_va || !vidc_sfr_va) {
        printf("PANDORA VIDC: publish before alloc\n");
        return -1;
    }
    uint32_t da = vidc_qtable_da();

    /* Vendor order, kept exactly: DA, window size, second DA, commit, SFR.
     * The commit (0xA002C <- 1) is what latches the addresses, so writing it
     * before the DAs would publish stale values. */
    vidc_wr(R_CPU_QUEUE_TABLE_DA, da);
    vidc_wr(R_CPU_QUEUE_TABLE_SIZE, VIDC_QTABLE_BYTES);
    vidc_wr(R_CPU_QUEUE_TABLE_DA2, da);
    vidc_wr(R_CPU_QUEUE_TABLE_EN, 1);
    if (vidc_sfr_da()) {
        vidc_wr(R_CPU_QUEUE_TABLE_ADDR2, vidc_sfr_da());
        /* SFR_ADDR must carry sfr + core_arch (upstream), not the bare DA. */
        vidc_wr(R_CPU_SFR_ADDR, vidc_sfr_da() + VIDC_CORE_ARCH);
    }
    printf("PANDORA VIDC: published DA=%08x SFR=%08x (smmu %s) "
           "regs A0064/A0068/A0054/A0050/A0060\n", da, vidc_sfr_da(),
        vidc_smmu_on ? "on" : "OFF, truncated physical address");
    printf("PANDORA VIDC: readback DA=%08x size=%08x DA2=%08x en=%08x sfr=%08x\n",
        vidc_rd(R_CPU_QUEUE_TABLE_DA), vidc_rd(R_CPU_QUEUE_TABLE_SIZE),
        vidc_rd(R_CPU_QUEUE_TABLE_DA2), vidc_rd(R_CPU_QUEUE_TABLE_EN),
        vidc_rd(R_CPU_QUEUE_TABLE_ADDR2));

    /* 0xA0024/0xA0028 carry the TrustZone content-protection span: the vendor
     * derives it from its CP resource table and only writes it when that table
     * reports a non-zero start (start != 0 is purely a guard; both registers
     * receive the SPAN) [K __boot_firmware_iris4 step 6-7].  We do not use
     * protected playback and own no CP table, so the vendor-equivalent action
     * is to write nothing -- not to invent a span for video_region.  Both
     * registers and 0xA005C are therefore left at their reset values. */
    printf("PANDORA VIDC: SFR published at 0xA005C=%08x (sfr+core_arch) and "
           "0xA0060=%08x; CP span 0xA0024/0xA0028 skipped (no TZ CP table)\n",
        vidc_rd(R_CPU_SFR_ADDR), vidc_rd(R_CPU_QUEUE_TABLE_ADDR2));

    /* Assert MSK_CORE_POWER_ON (BIT1) before releasing the core.  The register
     * definition is explicit: CPU_CS_X2RPMH bit1 is the core power-on request
     * (iris_vpu_register_defines.h:39-41) and the vendor clears it to 0 only
     * after the handshake succeeds.  Every earlier ladder left it at 0, and the
     * snapshot proved the consequence: bit1 read 0, the CPU block and the TZ
     * wrapper stayed all-zero, and 0xA004C never reported ready -- TZ's
     * auth_and_reset had released the reset, but the wrapper had never been
     * asked to power the core. */
    vidc_wr(R_CPU_X2RPMH, MSK_CORE_POWER_ON);
    __asm__ volatile ("dsb sy" ::: "memory");
    printf("PANDORA VIDC: core power-on request 0x%08x = %08x\n",
        R_CPU_X2RPMH, vidc_rd(R_CPU_X2RPMH));
    vc_smmu_faults = 0;
    vc_smmu_faults_done = false;
    /* Drop any fault latched during table build/attach so the capture below
     * only ever shows faults this boot produced. */
    if (vidc_smmu_on) {
        const uint32_t stale = vidc_smmu_rd((uint32_t)(vidc_smmu_cb + 0x58));
        if (stale) vidc_smmu_wr((uint32_t)(vidc_smmu_cb + 0x58), stale);
        printf("PANDORA VIDC: what the firmware will read: hdr %08x %08x %08x "
               "q0 %08x %08x %08x\n", vidc_qt_rd(0), vidc_qt_rd(4), vidc_qt_rd(8),
            vidc_qt_rd(0x120), vidc_qt_rd(0x124), vidc_qt_rd(0x128));
    }
    vidc_wr(R_WRAPPER_SPARE_0, 1);
    vidc_wr(R_CPU_BOOT_CTRL, 1);
    printf("PANDORA VIDC: boot released (spare0=%08x boot=%08x stat=%08x)\n",
        vidc_rd(R_WRAPPER_SPARE_0), vidc_rd(R_CPU_BOOT_CTRL),
        vidc_rd(R_CPU_QUEUE_TABLE_STAT));
    vidc_snap("after boot");

    /* Vendor gate [K __prepare_pc_iris4 0x15e64-0x1602c]: proceed only when
     *   (0xA004C & BIT(30)) && (0xC0010 & BIT(0))
     * and then poll 0xC0010 bit0 with `read_register_with_poll_timeout(..., 250, 2500)`
     * -- a 2.5 s budget.  Our previous 100 us x 1000 = 100 ms window was an
     * order of magnitude too short, and it watched the wrong register pair.
     * 0xC0010 is WRAPPER_TZ_CPU_STATUS; bit0 says the Xtensa is up. */
    for (unsigned i = 0; i < 25000; i++) {
        uint32_t s = vidc_rd(R_CPU_QUEUE_TABLE_STAT);
        const uint32_t tz = vidc_rd(0xC0010u);
        if ((s & (1u << 30)) && (tz & 1u)) {
            printf("PANDORA VIDC: pc ready after %u us (0xA004C=%08x 0xC0010=%08x)\n",
                i * 100u, s, tz);
            if (s & 1u) {
                vidc_wr(R_CPU_H2XSOFTINTEN, 1);
                vidc_wr(R_CPU_X2RPMH, 0);
            }
            return 0;
        }
        /* While the core boots it starts touching memory through the SMMU.
         * A single latched FSR only tells us the FIRST address; the pattern is
         * what identifies the window the firmware expects.  So sample often,
         * and clear FSR (write-1-to-clear) to catch the next one.
         *
         * Note `fsr != 0`, not `fsr & 0xF`: a translation fault reads back as
         * 0x400 here, whose low nibble is zero, and the first version of this
         * capture silently printed nothing for exactly that reason. */
        if (vidc_smmu_on && !vc_smmu_faults_done && (i & 1u) == 0) {
            const uint32_t fsr = vidc_smmu_rd((uint32_t)(vidc_smmu_cb + 0x58));
            if (fsr != 0) {
                const uint32_t far = vidc_smmu_rd((uint32_t)(vidc_smmu_cb + 0x60));
                if (vc_smmu_faults < 16) {
                    printf("PANDORA VIDC: fault[%u] t=%u fsr=%08x far=%08x\n",
                        vc_smmu_faults, i, fsr, far);
                    vc_smmu_faults++;
                }
                vidc_smmu_wr((uint32_t)(vidc_smmu_cb + 0x58), fsr);
                if (vc_smmu_faults >= 16) {
                    vc_smmu_faults_done = true;
                }
            }
        }
        if (s & 0xEu) {
            printf("PANDORA VIDC: queue table rejected, status %08x (iter %u)\n", s, i);
            return -1;
        }
        if (s & 1u) {
            vidc_wr(R_CPU_H2XSOFTINTEN, 1);
            vidc_wr(R_CPU_X2RPMH, 0);
            printf("PANDORA VIDC: queue table accepted after %u polls (status %08x)\n", i, s);
            printf("PANDORA VIDC: h2x=%08x x2rpmh=%08x core_clk=%08x intr=%08x/%08x\n",
                vidc_rd(R_CPU_H2XSOFTINTEN), vidc_rd(R_CPU_X2RPMH),
                vidc_rd(R_WRAPPER_CORE_CLK),
                vidc_rd(R_WRAPPER_INTR_STATUS), vidc_rd(R_WRAPPER_INTR_MASK));
            return 0;
        }
        delay(100);                    /* 100 us, vendor uses 50-100 us */
    }
    printf("PANDORA VIDC: queue table publish TIMEOUT (stat=%08x boot=%08x spare0=%08x)\n",
        vidc_rd(R_CPU_QUEUE_TABLE_STAT), vidc_rd(R_CPU_BOOT_CTRL),
        vidc_rd(R_WRAPPER_SPARE_0));
    /* If the core never reports ready, the only way to tell "not executing"
     * from "executing and stuck" is to read the blocks the core itself would
     * write.  Dump the CPU block (0xA0000-0xA0070), the TZ wrapper (0xC0000-
     * 0xC0020) and the outer wrapper (0xB0080-0xB0090) in full: any non-reset
     * word is a breadcrumb the firmware left. */
    for (unsigned base = 0; base < 0x70u; base += 0x10u) {
        printf("PANDORA VIDC: A00%02x: %08x %08x %08x %08x\n", base,
            vidc_rd(CPU_BASE_OFFS + base), vidc_rd(CPU_BASE_OFFS + base + 4),
            vidc_rd(CPU_BASE_OFFS + base + 8), vidc_rd(CPU_BASE_OFFS + base + 0xC));
    }
    for (unsigned base = 0; base < 0x20u; base += 0x10u) {
        printf("PANDORA VIDC: C00%02x: %08x %08x %08x %08x\n", base,
            vidc_rd(0xC0000u + base), vidc_rd(0xC0000u + base + 4),
            vidc_rd(0xC0000u + base + 8), vidc_rd(0xC0000u + base + 0xC));
    }
    printf("PANDORA VIDC: B00[80..9c]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
        vidc_rd(0xB0080u), vidc_rd(0xB0084u), vidc_rd(0xB0088u), vidc_rd(0xB008Cu),
        vidc_rd(0xB0090u), vidc_rd(0xB0094u), vidc_rd(0xB0098u), vidc_rd(0xB009Cu));
    /* Why the Xtensa never reports ready, measured rather than guessed.
     *
     * Vendor fact (msm_video.ko): __power_on_iris4 / _controller / _hardware
     * contain NO __write_register at all -- power-on is genpd+clk only -- and
     * __power_off_iris4_hardware does write WRAPPER_CORE_CLK (0xB0088)=0.  So if
     * the wrapper core clock is left gated, the core never fetches its firmware
     * and 0xA004C stays 0 forever, which is exactly what we see.  The same
     * snapshot also tells us whether the VPU actually touched memory at all:
     * an SMMU context fault (CB_FSR != 0) would mean the firmware started but
     * its accesses were mistranslated, a completely different fix. */
    printf("PANDORA VIDC: core clk=%08x pwr=%08x boot=%08x a2h=%08x x2rpmh=%08x\n",
        vidc_rd(R_WRAPPER_CORE_CLK), vidc_rd(R_WRAPPER_CORE_PWR),
        vidc_rd(R_CPU_BOOT_CTRL), vidc_rd(R_CPU_A2HSOFTINTCLR),
        vidc_rd(R_CPU_X2RPMH));
    printf("PANDORA VIDC: bridge ahb=%08x apv=%08x lpi=%08x/%08x noc=%08x/%08x\n",
        vidc_rd(R_CPU_AHB_BRIDGE_RESET), vidc_rd(R_CPU_APV_BRIDGE_RESET),
        vidc_rd(R_AON_NOC_LPI_CTRL), vidc_rd(R_AON_NOC_LPI_STATUS),
        vidc_rd(R_AON_NOC_RESET_REQ), vidc_rd(R_AON_NOC_RESET_ACK));
    if (vidc_smmu_on) {
        const uint32_t gfsr = vidc_smmu_rd(0x48);
        const uint32_t fsr = vidc_smmu_rd((uint32_t)(vidc_smmu_cb + 0x58));
        const uint32_t far = vidc_smmu_rd((uint32_t)(vidc_smmu_cb + 0x60));
        const uint32_t sctlr = vidc_smmu_rd(vidc_smmu_cb);
        /* ArmSmmu4KProgram.c defines FSR_FAULT as 0xc00001fe.  0x400 has none
         * of those bits, so it is NOT a context fault -- an earlier revision of
         * this diagnostic printed "the VPU did run and was mistranslated" for
         * it, which was a false positive worth remembering. */
        printf("PANDORA VIDC: smmu gfsr=%08x fsr=%08x far=%08x sctlr=%08x fault=%s\n",
            gfsr, fsr, far, sctlr, (fsr & 0xc00001feu) ? "YES" : "no(0x400 is not a fault)");
    }
    return -1;
}

/* Firmware load + auth + reset.  pandora_adsp.c already exposes
 * pandora_pas_load_auth() (PAS init/mem_setup/segment copy/auth_and_reset over
 * SMC); it works on this platform without a TrustZone driver of our own.
 * Mainline uses IRIS_PAS_ID = 9 for video; the MBN segment span is exactly the
 * 8 MiB carveout, so a wrong PAS id shows up as a loud SCM failure. */
extern int pandora_pas_load_auth(const char *file, unsigned pas,
    uint64_t mem_pa, uint64_t mem_size);
extern int pandora_pas_load_auth_src(const char *file, unsigned pas, uint64_t mem_pa,
    uint64_t mem_size, bool part);

#define VIDC_PAS_ID 9u

static int
vidc_auth_reset(void)
{
    int err = pandora_pas_load_auth_src(vidc_fw_name, VIDC_PAS_ID,
        VIDEO_REGION_PA, VIDEO_REGION_SIZE, false);
    if (err != 0) {
        /* The rebuilt volume carries a different userdata pack (20 files vs 19)
         * whose VPU image trips the segment bounds check.  The firmware container
         * was never touched and still holds the known-good image, so retry there
         * before giving up on the stage. */
        printf("PANDORA VIDC: pack image failed (err %d); retrying from the "
               "firmware container\n", err);
        err = pandora_pas_load_auth_src(vidc_fw_name, VIDC_PAS_ID,
            VIDEO_REGION_PA, VIDEO_REGION_SIZE, true);
    }
    if (err != 0) {
        printf("PANDORA VIDC: PAS load/auth failed for %s (err %d)\n", vidc_fw_name, err);
        return -1;
    }
    printf("PANDORA VIDC: firmware %s authenticated and released from reset\n", vidc_fw_name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Firmware load into the carveout via the existing firmware reader.  Kept as
 * the manual fallback if SCM PAS is unavailable; parses the MBN ELF program
 * headers and copies loadable segments into video_region. */
/* ------------------------------------------------------------------ */
struct vidc_elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

static int
vidc_load_firmware(void)
{
    uint64_t mdt_size = 0;
    uint8_t hdr[52];
    int err;

    err = pandora_firmware_pread(vidc_fw_name, 0, hdr, sizeof(hdr), &mdt_size);
    if (err || mdt_size < sizeof(hdr) || hdr[0] != 0x7f || hdr[1] != 'E' ||
        hdr[2] != 'L' || hdr[3] != 'F') {
        printf("PANDORA VIDC: firmware %s not readable/ELF (err %d size %llu)\n",
            vidc_fw_name, err, (unsigned long long)mdt_size);
        return -1;
    }

    /* Byte-wise so the parse does not depend on the alignment of hdr/phbuf
     * (and does not trip -Wcast-align). */
    uint32_t phoff = vidc_le32(hdr + 28);
    uint16_t phentsize = vidc_le16(hdr + 42);
    uint16_t phnum = vidc_le16(hdr + 44);
    printf("PANDORA VIDC: firmware %s ELF32 phoff=%u phnum=%u size=%llu\n",
        vidc_fw_name, phoff, phnum, (unsigned long long)mdt_size);

    if (phnum == 0 || phnum > 64 || phentsize < sizeof(struct vidc_elf32_phdr)) {
        printf("PANDORA VIDC: invalid ELF program header table\n");
        return -1;
    }
    if (!vidc_mmio_on) {           /* ensure the carveout mapping is available */
        if (vidc_probe() != 0) return -1;
    }

    /* Each loadable PT_LOAD segment goes to its p_paddr, which for these MBNs
     * lies inside video_region.  Use ml_io_map_unmappable (not wcomb): the
     * GPU lane found that unmapping a wcomb window panics (pandora_gpu.c p0180),
     * while unmappable windows can be taken back with ml_io_unmap. */
    vm_offset_t carveout = ml_io_map_unmappable(VIDEO_REGION_PA, VIDEO_REGION_SIZE, 0);
    if (!carveout) {
        printf("PANDORA VIDC: cannot map video_region carveout\n");
        return -1;
    }

    uint8_t phbuf[64];
    unsigned loaded = 0;
    for (unsigned i = 0; i < phnum; i++) {
        err = pandora_firmware_pread(vidc_fw_name, phoff + i * phentsize,
                                     phbuf, sizeof(struct vidc_elf32_phdr), NULL);
        if (err) { printf("PANDORA VIDC: phdr %u read failed %d\n", i, err); break; }
        /* Field-wise (byte-aligned buffer, no widening pointer cast). */
        const uint32_t p_type = vidc_le32(phbuf + 0);
        const uint32_t p_offset = vidc_le32(phbuf + 4);
        const uint32_t p_paddr = vidc_le32(phbuf + 12);
        const uint32_t p_filesz = vidc_le32(phbuf + 16);
        if (p_type != 1 || p_filesz == 0) continue;         /* PT_LOAD only */
        if (p_paddr < VIDEO_REGION_PA ||
            (uint64_t)p_paddr + p_filesz > VIDEO_REGION_PA + VIDEO_REGION_SIZE) {
            printf("PANDORA VIDC: segment %u paddr %08x outside carveout, skip\n", i, p_paddr);
            continue;
        }
        uint64_t dst_off = (uint64_t)p_paddr - VIDEO_REGION_PA;
        err = pandora_firmware_pread(vidc_fw_name, p_offset,
                                     (void *)(carveout + dst_off), p_filesz, NULL);
        if (err) { printf("PANDORA VIDC: segment %u copy failed %d\n", i, err); break; }
        loaded++;
    }
    printf("PANDORA VIDC: loaded %u firmware segments into carveout\n", loaded);
    ml_io_unmap(carveout, VIDEO_REGION_SIZE);
    return loaded ? 0 : -1;
    /* NOTE: every error exit above that happens AFTER the carveout mapping was
     * taken leaks 8 MiB of kernel_map.  Callers only reach this function when
     * PAS already failed (so the ladder is finished anyway and the teardown at
     * the end of vidc_bringup covers it), but the leak is real and is recorded
     * here rather than silently forgotten. */
}

/* Drain one response packet from the message queue (cmd=0, msg=1, dbg=2).
 * Both indices are u32 WORD indices [M iris_hfi_queue.c:14-16], so the byte
 * offset of a packet is start_addr_rel + read_idx*4.  The queue is read through
 * the CPU mapping, so invalidate the ring region first: the VPU wrote it while
 * our lines were still resident.
 *
 * Return: 0 = queue empty, 1 = one valid packet consumed, 2 = malformed packet
 * discarded (the caller must NOT treat 2 as a handshake). */
static int
vidc_hfi_poll(void)
{
    struct vidc_queue_table *t = (struct vidc_queue_table *)vidc_qtable_va;
    uint32_t moff = (uint32_t)((char *)&t->qhdr[1] - (char *)t);

    /* Only the queue header changes while we are idle: invalidate that (56 B)
     * on every pass.  The ring body is invalidated lazily, once write_idx has
     * actually moved, so the poll loop does not flush 1 MiB per iteration. */
    vidc_sync_in(moff, (unsigned)sizeof(struct vidc_queue_header));

    uint32_t w = vidc_qt_rd(moff + offsetof(struct vidc_queue_header, write_idx));
    uint32_t r = vidc_qt_rd(moff + offsetof(struct vidc_queue_header, read_idx));
    if (w == r) return 0;

    uint32_t start = vidc_qt_rd(moff + offsetof(struct vidc_queue_header, start_addr));
    uint32_t ring_off = start - vidc_qtable_da();
    uint32_t q_off = ring_off + r * 4u;
    const uint32_t ring_end = ring_off + (uint32_t)IFACEQ_QUEUE_SIZE;
    if (q_off + 8u > VIDC_QTABLE_BYTES) {
        printf("PANDORA VIDC: msg queue index out of range (r=%u w=%u start=%08x)\n", r, w, start);
        vidc_qt_wr(moff + offsetof(struct vidc_queue_header, read_idx), w);
        vidc_sync_out(moff, (unsigned)sizeof(struct vidc_queue_header));
        return 2;
    }
    /* One packet is at most IFACEQ_CORE_PKT_SIZE, and it may wrap at the ring
     * end, so invalidate the tail and (when wrapping) the head separately. */
    uint32_t span = 20u + 128u;                  /* header + property head */
    if (q_off + span <= ring_end) {
        vidc_sync_in(q_off, span);
    } else {
        vidc_sync_in(q_off, ring_end - q_off);
        vidc_sync_in(ring_off, span - (ring_end - q_off));
    }
    uint32_t size = vidc_ring_rd(ring_off, ring_end, q_off);
    uint32_t type = vidc_ring_rd(ring_off, ring_end, q_off + 4u);
    printf("PANDORA VIDC: response size=%u type=%08x read_idx=%u write_idx=%u (words)\n",
        size, type, r, w);

    if (size < 8u || size > IFACEQ_CORE_PKT_SIZE) {
        printf("PANDORA VIDC: bad packet size %u, dropping whole queue\n", size);
        vidc_qt_wr(moff + offsetof(struct vidc_queue_header, read_idx), w);
        vidc_sync_out(moff, (unsigned)sizeof(struct vidc_queue_header));
        return 2;
    }
    /* Property response layout [M iris_hfi]: header (8) + property_id (4) +
     * flags (4) + payload_size (4) + payload. */
    if (size >= 20u) {
        uint32_t pid = vidc_ring_rd(ring_off, ring_end, q_off + 8u);
        uint32_t flags = vidc_ring_rd(ring_off, ring_end, q_off + 12u);
        uint32_t psize = vidc_ring_rd(ring_off, ring_end, q_off + 16u);
        printf("PANDORA VIDC: prop=%08x flags=%u payload=%u\n", pid, flags, psize);
        if (pid == HFI_PROP_IMAGE_VERSION && psize && psize <= 128u) {
            /* The ring was clean+invalidated above, so byte access through the
             * CPU mapping is coherent with what the VPU wrote. */
            char ver[129];
            unsigned n = psize;
            const volatile uint8_t *src =
                (const volatile uint8_t *)vidc_qtable_va + q_off + 20u;
            if (q_off + 20u + n > VIDC_QTABLE_BYTES) n = 0;
            for (unsigned i = 0; i < n; i++) ver[i] = (char)src[i];
            ver[n] = 0;
            printf("PANDORA VIDC: IMAGE_VERSION=%s\n", ver);
        }
    }
    vidc_qt_wr(moff + offsetof(struct vidc_queue_header, read_idx), w);
    vidc_sync_out(moff, (unsigned)sizeof(struct vidc_queue_header));
    return 1;
}

/* Queue HFI_CMD_INIT into the command ring and wait (bounded) for the firmware
 * to advance the message queue.  The wakeup is CPU_IC_SOFTINT bit0 after
 * clearing any stale A2H latch, which is the documented host->Xtensa doorbell. */
static int
vidc_hfi_init(void)
{
    struct vidc_queue_table *t = (struct vidc_queue_table *)vidc_qtable_va;
    uint32_t coff = (uint32_t)((char *)&t->qhdr[0] - (char *)t);
    if (!vidc_qtable_va) {
        printf("PANDORA VIDC: hfi_init before alloc\n");
        return -1;
    }
    /* Without the SMMU the published address is a truncated physical one, so
     * `start - vidc_qtable_da()` no longer yields a valid ring offset and the
     * doorbell would point at nothing.  Refuse instead of scribbling inside our
     * own mapping (this really happened on the 4-SID attempt, where smmu=FAIL). */
    if (!vidc_smmu_on) {
        printf("PANDORA VIDC: hfi_init skipped (no SMMU device address)\n");
        return -1;
    }
    uint32_t start = vidc_qt_rd(coff + offsetof(struct vidc_queue_header, start_addr));
    uint32_t ring_off = start - vidc_qtable_da();
    uint64_t ring_pa = vidc_qtable_pa + ring_off;

    /* Drop any stale host->fw interrupt latch before ringing the doorbell. */
    vidc_wr(R_CPU_A2HSOFTINTCLR, 1);

    vidc_qt_wr(ring_off + 0u, (uint32_t)sizeof(struct vidc_pkt_hdr));   /* size */
    vidc_qt_wr(ring_off + 4u, HFI_CMD_INIT);                           /* type */
    /* The packet body must be visible to the VPU before write_idx moves; push
     * the whole first cache line of the ring out now. */
    vidc_sync_out(ring_off, 64u);
    /* write_idx is in WORDS [M], so a single 8-byte packet advances it by 2. */
    vidc_qt_wr(coff + offsetof(struct vidc_queue_header, write_idx),
        (uint32_t)(sizeof(struct vidc_pkt_hdr) / 4u));
    vidc_sync_out((uint64_t)coff, (unsigned)sizeof(struct vidc_queue_header));

    vidc_wr(R_CPU_SOFTINT, 1);   /* CPU_IC_SOFTINT bit0: host -> Xtensa */
    printf("PANDORA VIDC: HFI_CMD_INIT queued at %08llx (cmd write_idx=2 words), "
           "doorbell %08x\n", (unsigned long long)ring_pa, vidc_rd(R_CPU_SOFTINT));

    for (unsigned i = 0; i < 4000; i++) {
        int got = vidc_hfi_poll();
        if (got == 1) {
            printf("PANDORA VIDC: HFI handshake response received (iter %u, intr=%08x)\n",
                i, vidc_rd(R_WRAPPER_INTR_STATUS));
            vidc_wr(R_CPU_A2HSOFTINTCLR, 1);
            return 0;
        }
        if (got == 2) {
            printf("PANDORA VIDC: malformed reply discarded (iter %u), still waiting\n", i);
        }
        if ((i % 500u) == 0)
            printf("PANDORA VIDC: waiting for HFI response, iter %u stat=%08x intr=%08x\n",
                i, vidc_rd(R_CPU_QUEUE_TABLE_STAT), vidc_rd(R_WRAPPER_INTR_STATUS));
        delay(1000);
    }
    printf("PANDORA VIDC: HFI response TIMEOUT (stat=%08x intr=%08x/%08x core_pwr=%08x)\n",
        vidc_rd(R_CPU_QUEUE_TABLE_STAT), vidc_rd(R_WRAPPER_INTR_STATUS),
        vidc_rd(R_WRAPPER_INTR_MASK), vidc_rd(R_WRAPPER_CORE_PWR));
    return -1;
}

/* ------------------------------------------------------------------ */
/* Full bring-up sequence (level 2).  Order follows the mainline iris boot
 * flow; every stage is attempted and reported so one boot returns the whole
 * ladder.  Stages that depend on a failed predecessor are still attempted
 * because the register read-back they produce is diagnostic (e.g. PAS without
 * power tells us whether SCM rejects the id or the domain). */
/* ------------------------------------------------------------------ */
static void
vidc_bringup(void)
{
    int rc;

    rc = vidc_probe();
    vidc_verdict("probe", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_PROBE;

    /* Bisection levels, so one image can answer "which stage eats memory":
     *   2 = full (default ladder), 3 = probe + alloc only, 4 = probe + alloc + smmu.
     * They exist because `=2` panics at 5.17 s with a failed 128 KiB kmem_alloc
     * while `=0` and `=1` run to the 135 s timer, and each answer otherwise costs
     * a whole CI round plus a device window. */
    if (vidc_level < 2) return;
    if (vidc_level == 3) {
        rc = vidc_alloc_buffers();
        vidc_verdict("alloc", rc);
        if (rc == 0) vidc_pass_mask |= VIDC_STAGE_ALLOC;
        goto summary;
    }
    if (vidc_level == 4) {
        rc = vidc_alloc_buffers();
        vidc_verdict("alloc", rc);
        if (rc == 0) vidc_pass_mask |= VIDC_STAGE_ALLOC;
        rc = vidc_smmu_up();
        vidc_verdict("smmu", rc);
        if (rc == 0) vidc_pass_mask |= VIDC_STAGE_SMMU;
        goto summary;
    }

    rc = vidc_power_up();               /* GDSC + branches + bridge reset */
    vidc_verdict("power", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_POWER;

    rc = vidc_alloc_buffers();          /* host memory only, always safe */
    vidc_verdict("alloc", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_ALLOC;

    rc = vidc_smmu_up();                /* 32-bit IOVAs for table + SFR */
    vidc_verdict("smmu", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_SMMU;

    rc = vidc_fill_queues();            /* needs the device addresses */
    vidc_verdict("queues", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_QUEUES;

    rc = vidc_auth_reset();             /* SCM PAS load + auth_and_reset */
    vidc_verdict("pas", rc);
    if (rc == 0) {
        vidc_pass_mask |= VIDC_STAGE_PAS;
    } else {
        /* PAS rejected the image (wrong PAS id, or the carveout span does not
         * match).  Copy the loadable segments into video_region ourselves so
         * the publish stage still has firmware to release: if the handshake
         * then succeeds the failure was in PAS bookkeeping, not in the image. */
        int fw = vidc_load_firmware();
        vidc_verdict("fwcopy", fw);
        if (fw == 0) vidc_pass_mask |= VIDC_STAGE_FWCOPY;
    }

    rc = vidc_publish_queues();         /* boot release + 0xA004C poll */
    vidc_verdict("publish", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_PUBLISH;

    rc = vidc_hfi_init();               /* HFI_CMD_INIT + version packet */
    vidc_verdict("hfi", rc);
    if (rc == 0) vidc_pass_mask |= VIDC_STAGE_HFI;

summary:
    printf("PANDORA VIDC: SUMMARY mask=%03x/%03x probe=%u power=%u alloc=%u smmu=%u "
           "queues=%u pas=%u fwcopy=%u publish=%u hfi=%u\n", vidc_pass_mask, VIDC_STAGE_ALL,
        !!(vidc_pass_mask & VIDC_STAGE_PROBE), !!(vidc_pass_mask & VIDC_STAGE_POWER),
        !!(vidc_pass_mask & VIDC_STAGE_ALLOC), !!(vidc_pass_mask & VIDC_STAGE_SMMU),
        !!(vidc_pass_mask & VIDC_STAGE_QUEUES), !!(vidc_pass_mask & VIDC_STAGE_PAS),
        !!(vidc_pass_mask & VIDC_STAGE_FWCOPY),
        !!(vidc_pass_mask & VIDC_STAGE_PUBLISH), !!(vidc_pass_mask & VIDC_STAGE_HFI));
    if (vidc_mmio_on)
        printf("PANDORA VIDC: final EFUSE=%08x CORE_PWR=%08x CORE_CLK=%08x IDLE=%08x\n",
            vidc_rd(R_WRAPPER_EFUSE), vidc_rd(R_WRAPPER_CORE_PWR),
            vidc_rd(R_WRAPPER_CORE_CLK), vidc_rd(R_VCODEC_SS_IDLE));

    /* Every register window taken by ml_io_map() is a PERMANENT VM map entry.
     * Unmapping one panics ("Attempting to remove permanent VM map entry",
     * restart type 4 at 0.207 s on 2026-11-21 23:36), so the windows stay mapped
     * for the driver's lifetime -- same as every other pandora_* driver.  The
     * earlier "release early to shrink the footprint" idea was simply illegal. */
    printf("PANDORA VIDC: ladder done; MMA windows kept mapped (permanent entries)\n");
}

/* ------------------------------------------------------------------ */
/* Worker thread and entry point */
/* ------------------------------------------------------------------ */
static void
vidc_thread(void *arg, wait_result_t wresult)
{
    (void)arg; (void)wresult;
    /* Delay the bring-up so its ~5 MiB of permanent kernel_map (3.06 MiB queue
     * table + SFR + 16-page SMMU pool + the 2 MiB of MMIO windows) is not taken
     * while the boot-time drivers are still allocating.  Measured on this board:
     * with pandora_vidc=0 or =1 the kernel reaches the 135 s timer, but with
     * =2 the TOUCH driver's 128 KiB pageable kmem_alloc fails at 5.171 s
     * (`failed with 3 @vm_kern.c:178` -> restart type 4).  The GPU lane maps a
     * 2 GiB pool, so the kernel_map margin is thin and our peak tips it over.
     * pandora_vidc_defer_ms defaults to 9000: past the 5.2 s window, and the
     * 2.5 s vendor ready-poll still ends well inside the 120 s image timer. */
    if (vidc_defer_ms > 0) delay(vidc_defer_ms * 1000);
    if (vidc_level >= 1) {
        if (vidc_level >= 2) vidc_bringup();
        else vidc_probe();
    }
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_vidc_init(void)
{
    vidc_set_ring_geometry();
    (void)PE_parse_boot_argn("pandora_vidc", &vidc_level, sizeof(vidc_level));
    if (!vidc_level) return;
    if (vidc_level < 1 || vidc_level > 4) {
        printf("PANDORA VIDC: unsupported pandora_vidc=%d (expected 0..4)\n", vidc_level);
        return;
    }
    (void)PE_parse_boot_argn("pandora_vidc_defer_ms", &vidc_defer_ms,
        sizeof(vidc_defer_ms));
    if (vidc_defer_ms < 0) vidc_defer_ms = 0;
    printf("PANDORA VIDC: bring-up deferred %d ms\n", vidc_defer_ms);

    char fw[64];
    if (PE_parse_boot_arg_str("pandora_vidc_fw", fw, sizeof(fw)) && fw[0])
        strlcpy(vidc_fw_name, fw, sizeof(vidc_fw_name));

    printf("PANDORA VIDC: level %d, firmware %s (clean-room transport, "
           "per-stage verdicts)\n", vidc_level, vidc_fw_name);

    thread_t th;
    if (kernel_thread_start(vidc_thread, NULL, &th) == KERN_SUCCESS)
        thread_deallocate(th);
}

#endif /* PANDORA */
