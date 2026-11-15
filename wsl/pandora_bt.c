/*
 * PANDORA Bluetooth: Nezha combo-controller UART on QUPv3 wrapper 3 SE5 (0x01994000).
 *
 * The combo chip's WLAN side is PCIe (pandora_wifi.c); Bluetooth is a plain
 * 4-wire HS-UART on this serial engine (live.dts qup_uart@1994000, pins
 * gpio24..27 function qup3_se5_l0..l3, wakeup byte 0xfd). Android's
 * android.hardware.bluetooth@aidl-service-qti opens /dev/ttyHS0. Its observed
 * cold-start trace uses FD/FC wake and PERI frames before normal HCI; finding
 * Hamilton firmware in /vendor does not identify the selected image. This
 * Level 8 probes wake and PERI version only; firmware download and later
 * Ganges/PERI initialization are not implemented here.
 *
 * Boot-arg pandora_bt=1, read only: the wrapper votes and branch halts, the SE5
 * RCG, the SE identity and its pins. =2 also votes the BT rails from the DT's
 * bt-vdd-* supplies, drives the four UART pins, enables the wrapper and SE5
 * clocks and sends HCI Reset at 115200, printing whatever comes back.
 * BT_EN is PMH0104 SPMI GPIO5. Levels 2 and 6 pulse it using the encoding
 * selected by the peripheral's TYPE/SUBTYPE; level 1 only reads its state.
 * Level 6 retries cold initialization at 115200. Only level 5 uses 4 Mbps.
 * Level 7 runs the GENI internal loopback without BT power/reset or TLMM
 * programming. A loopback pass establishes AP FIFO/sequence operation only.
 * Level 8 cold-starts at 115200, requires FD/FC wake, and asks PERI version
 * once. It never sends HCI Reset, changes the baud, or downloads firmware.
 * Level 9 repeats level 8 with RX_TRANS_CFG bit1 cleared. That bit was
 * mistakenly copied from the TX-only CTS mask; keep level 8 as the measured
 * comparison. RX command parameters and all power settings stay the same.
 * Level 10 keeps level 9's RX configuration and sets only RFR_OPEN in the
 * RX command. Android's module also sets error-character filters; leave
 * those separate so the effect of RFR_OPEN can be identified on device.
 * Level 11 keeps level 10's UART/PERI settings and requests S8F_E0 at
 * 1876 mV, the live DT BT consumer minimum also used by pandora_wifi.c.
 * The earlier levels retain 1800 mV for comparison; votes are not a voltage
 * measurement and a failed vote stops initialization without dropping shared rails.
 *
 * Register map: Linux drivers/soc/qcom/qcom-geni-se.c, drivers/tty/serial/
 * msm_geni_serial.c and u-boot's drivers/serial/serial_msm_geni.c (GPL-2.0+,
 * same register facts), plus the captured DT.
 * See docs/BLUETOOTH_NO_ANALYZER_2026-11-15.md for evidence and trial criteria.
 * See docs/BLUETOOTH_ANDROID_ROM_ROUTE_2026-11-15.md for Android's actual wire trace.
 * All mappings stay for the life of the kernel, including on failure.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

extern kern_return_t thread_terminate(thread_t);
extern int pandora_rpmh_resource(const char *, uint32_t, uint32_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern bool pandora_log_sync(void);
void pandora_bt_init(void);

/* Physical windows. One page each: the vote register, the RCG and branch
 * halt, and the wrapper branch halt registers. */
#define BT_GCC_VOTE_PA   0x00152000u   /* GCC 0x52018, QUPV3 wrapper 3 enable */
#define BT_GCC_RCG_PA    0x001a8000u   /* SE5 RCG 0xa8640, branch halt 0xa8630 */
#define BT_GCC_HALT_PA   0x00123000u   /* m_ahb 0x233f0, s_ahb 0x233f4, core 0x233f8, core_2x 0x2340c */
#define BT_SE_PA         0x01994000u   /* QUPV3_3 SE5: the BT UART */
#define BT_WRAPPER_PA    0x019c0000u   /* QUPV3_3: version at +4, read only with clocks on */
#define BT_TLMM_SW_PA    0x0f112000u   /* TLMM gpio18 (wcn_sw_ctrl) */
#define BT_TLMM_UART_PA  0x0f118000u   /* TLMM gpio24..27 (CTS, RTS, TX, RX) */

/* GCC */
#define BT_VOTE          0x18u         /* 0x52018 - BT_GCC_VOTE_PA */
#define BT_VOTE_BITS     ((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 17))
#define BT_HALT_M_AHB    0x3f0u         /* 0x233f0 - BT_GCC_HALT_PA */
#define BT_HALT_S_AHB    0x3f4u
#define BT_HALT_CORE     0x3f8u
#define BT_HALT_CORE2X   0x40cu
#define BT_RCG           0x640u        /* 0xa8640 - BT_GCC_RCG_PA */
#define BT_SE5_HALT      0x630u        /* 0xa8630 */
#define BT_RCG_CMD       0x00u         /* +0: bit 0 update */
#define BT_RCG_CFG       0x04u         /* +4: [10:8] src, [13:12] mode, [4:0] pre-divider */
#define BT_RCG_M         0x08u
#define BT_RCG_N         0x0cu
#define BT_RCG_D         0x10u
#define BT_CBCR_OFF      (1u << 31)

/* GENI serial engine (0x01994000). Offsets shared with the SPI/I2C protocol
 * block are named as geni-se.h does. */
#define SE_STATUS        0x040u
#define SE_SER_M_CLK     0x048u
#define SE_SER_S_CLK     0x04cu
#define SE_IF_DISABLE    0x064u
#define SE_FW_REV        0x068u
#define SE_CLK_SEL       0x07cu
#define SE_UART_LOOPBACK 0x22cu        /* bit0 RX/TX short, bit1 CTS/RTS short */
#define SE_UART_IO_MACRO 0x240u        /* external signal routing; diagnostic read only */
#define SE_TX_TRANS_CFG  0x25cu
#define SE_TX_PACK0      0x260u
#define SE_TX_PACK1      0x264u
#define SE_TX_WORD_LEN   0x268u
#define SE_TX_STOP_LEN   0x26cu
#define SE_TX_TRANS_LEN  0x270u
#define SE_RX_TRANS_CFG  0x280u
#define SE_RX_PACK0      0x284u
#define SE_RX_PACK1      0x288u
#define SE_RX_WORD_LEN   0x28cu
#define SE_RX_STALE      0x294u
#define SE_TX_PARITY     0x2a4u
#define SE_RX_PARITY     0x2a8u
#define SE_UART_RFR      0x2acu        /* manual RTS/RFR override; diagnostic read only */
#define SE_DMA_MODE      0x258u
#define SE_M_CMD0        0x600u
#define SE_M_CMD_CTRL    0x604u
#define SE_M_IRQ         0x610u
#define SE_M_IRQ_EN      0x614u
#define SE_M_IRQ_CLR     0x618u
#define SE_S_CMD0        0x630u
#define SE_S_CMD_CTRL    0x634u
#define SE_S_IRQ         0x640u
#define SE_S_IRQ_EN      0x644u
#define SE_S_IRQ_CLR     0x648u
#define SE_TX_FIFO       0x700u
#define SE_RX_FIFO       0x780u
#define SE_TX_FIFO_ST    0x800u
#define SE_RX_FIFO_ST    0x804u
#define SE_IOS           0x908u        /* GENI input samples: RX bit0, IO2/CTS bit1 */
#define SE_TX_WM         0x80cu
#define SE_RX_WM         0x810u
#define SE_RX_RFR_WM     0x814u
#define SE_GSI_EVENT_EN  0xe18u
#define SE_IRQ_EN        0xe1cu
#define SE_HW_PARAM0     0xe24u
#define SE_DMA_GENERAL   0xe30u

#define GENI_FORCE_DEFAULT 0x020u
#define GENI_OUTPUT_CTRL   0x024u
#define GENI_CGC_CTRL      0x028u

/* GENI status and interrupt bits (geni-se.h, msm_geni_serial.c). */
#define GENI_STATUS_M_ACTIVE (1u << 0)
#define GENI_STATUS_S_ACTIVE (1u << 12)
#define GENI_CMD_ABORT        (1u << 1) /* CMD_CTRL command, distinct from IRQ completion bit */
#define M_CMD_DONE           (1u << 0)
#define M_TX_WM              (1u << 30)
#define M_RX_WM              (1u << 26)
#define M_RX_LAST            (1u << 27)
#define M_SEC_IRQ            (1u << 31)
#define S_RX_WM              (1u << 26)
#define S_RX_LAST            (1u << 27)
#define SER_CLK_EN           (1u << 0)
#define UART_PARAM_RFR_OPEN  (1u << 7) /* keep receiver-ready asserted; Android start_rx_sequencer */

/* HCI: packet type 0x01 command, opcode HCI_OP_RESET 0x0c03 (little endian). */
#define HCI_RESET_LEN 4u
static const uint8_t bt_hci_reset[HCI_RESET_LEN] = {0x01, 0x03, 0x0c, 0x00};

/* TLMM: pull 0 none, 1 down, 3 up; drive in mA even (2, 4, ...); bit 9 output. */
#define BT_PIN_DRIVE(ma) ((((ma) / 2) - 1) << 6)
#define BT_PIN_OUT       (1u << 9)
#define BT_PIN_MUX(f)    ((f) << 2)
#define BT_PIN_INPUT     1u            /* canoe PINGROUP: GPIO_IO in_bit=0, out_bit=1 */

struct bt_win { uint32_t pa, bytes; vm_offset_t va; };
static struct bt_win bt_wins[] = {
	{BT_GCC_VOTE_PA, 0x1000, 0}, {BT_GCC_RCG_PA, 0x1000, 0}, {BT_GCC_HALT_PA, 0x1000, 0},
	{BT_SE_PA, 0x4000, 0}, {BT_TLMM_SW_PA, 0x1000, 0}, {BT_TLMM_UART_PA, 0x4000, 0},
	{BT_WRAPPER_PA, 0x1000, 0},
};
enum { BT_W_VOTE, BT_W_RCG, BT_W_HALT, BT_W_SE, BT_W_SWCTRL, BT_W_UART, BT_W_WRAPPER, BT_W_COUNT };

static int bt_level, bt_wait;
static unsigned bt_rate;

static uint32_t
bt_rd(unsigned w, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(bt_wins[w].va + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
bt_wr(unsigned w, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(bt_wins[w].va + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
bt_sleep(unsigned us)
{
	uint64_t deadline;
	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	(void)assert_wait_deadline(&bt_wait, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Map once, never unmap: a duplicate window that is unmapped can take the
 * shared mappings of the GPU/touch drivers with it (pandora_camera.c notes). */
static int
bt_map(unsigned w)
{
	struct bt_win *b = &bt_wins[w];
	if (b->va) {
		return 0;
	}
	b->va = ml_io_map_unmappable(b->pa, b->bytes, VM_WIMG_IO);
	return b->va ? 0 : ENOMEM;
}

static int
bt_map_all(void)
{
	for (unsigned i = 0; i < BT_W_COUNT; i++) {
		int err = bt_map(i);
		if (err) {
			printf("PANDORA BT: window %u (%#x) not mapped\n", i, bt_wins[i].pa);
			return err;
		}
	}
	return 0;
}

static uint32_t
bt_pin_ctl(unsigned pin)
{
	/* GPIO_24 is the first page of the UART window */
	return bt_rd(BT_W_UART, (pin - 24) * 0x1000u);
}

/* OUT >= 0 drives the pad and sets OE; OUT < 0 leaves the pad an input (no IO
 * write, no OE), which is what CTS and RX need. */
static void
bt_pin(unsigned pin, unsigned func, unsigned pull, unsigned ma, int out)
{
	uint32_t off = (pin - 24) * 0x1000u;
	if (out >= 0) {
		bt_wr(BT_W_UART, off + 4, out ? 2u : 0u);
	}
	uint32_t ctl = bt_rd(BT_W_UART, off) & ~0x3ffu;
	ctl |= pull | BT_PIN_MUX(func) | BT_PIN_DRIVE(ma) | (out >= 0 ? BT_PIN_OUT : 0);
	if (ctl & (1u << 11)) {
		ctl |= 1u << 12;
	}
	bt_wr(BT_W_UART, off, ctl);
}

/* GPLL0 out even is 300 MHz; the QUP RCGs take it (or TCXO) as parent. */
static unsigned
bt_rcg_rate(void)
{
	printf("PANDORA BT: rcg cfg read\n");
	uint32_t cfg = bt_rd(BT_W_RCG, BT_RCG + BT_RCG_CFG);
	uint32_t m = bt_rd(BT_W_RCG, BT_RCG + BT_RCG_M);
	uint32_t n = bt_rd(BT_W_RCG, BT_RCG + BT_RCG_N);
	uint32_t d = bt_rd(BT_W_RCG, BT_RCG + BT_RCG_D);
	unsigned pre = (cfg & 0x1fu) + 1u;
	unsigned src = (cfg >> 8) & 7u, mode = (cfg >> 12) & 3u;
	uint64_t rate = 0;

	if (mode == 0 && n && m) {
		rate = 300000000ull * m / n / pre;
	} else if (mode == 0 && m == 0) {
		rate = 19200000ull / pre;
	}
	printf("PANDORA BT: SE5 RCG cfg %#x m %u n %u d %u (src %u mode %u pre %u) -> %llu Hz\n",
	    cfg, m, n, d, src, mode, pre, rate);
	pandora_log_sync();
	return (unsigned)rate;
}

/* Wrapper clocks: on, or an error with the halt register's value in the log. */
static int
bt_wait_on(unsigned w, uint32_t off, const char *what)
{
	for (unsigned i = 0; i < 10000; i++) {
		uint32_t v = bt_rd(w, off);
		if (!(v & BT_CBCR_OFF)) {
			return 0;
		}
		delay(1);
	}
	printf("PANDORA BT: %s clock stays off (%#x)\n", what, bt_rd(w, off));
	return ETIMEDOUT;
}

static bool
bt_se_clocked(void)
{
	return !(bt_rd(BT_W_HALT, BT_HALT_M_AHB) & BT_CBCR_OFF) &&
	    !(bt_rd(BT_W_HALT, BT_HALT_S_AHB) & BT_CBCR_OFF) &&
	    !(bt_rd(BT_W_HALT, BT_HALT_CORE) & BT_CBCR_OFF) &&
	    !(bt_rd(BT_W_HALT, BT_HALT_CORE2X) & BT_CBCR_OFF) &&
	    !(bt_rd(BT_W_RCG, BT_SE5_HALT) & BT_CBCR_OFF);
}

static void
bt_report(void)
{
	/* vote (GCC), RCG cmd (GCC) and the TLMM pins are always-on space: safe to
	 * read even with wrapper3 powered off. The branch-halt and SE5 registers
	 * are not: reading them with the domain off raises a synchronous external
	 * abort. Gate those behind the vote actually being asserted. */
	uint32_t vote = bt_rd(BT_W_VOTE, BT_VOTE);
	printf("PANDORA BT: wrapper3 vote %#x (want %#x)\n", vote, BT_VOTE_BITS);
	pandora_log_sync();
	printf("PANDORA BT: RCG cmd %#x; SE5 rate %u Hz\n", bt_rd(BT_W_RCG, BT_RCG + BT_RCG_CMD), bt_rate);
	pandora_log_sync();
	printf("PANDORA BT: pins 18 %#x, cts %#x rts %#x tx %#x rx %#x; io 18 %#x 24 %#x 25 %#x 26 %#x 27 %#x\n",
	    bt_rd(BT_W_SWCTRL, 0), bt_pin_ctl(24), bt_pin_ctl(25), bt_pin_ctl(26), bt_pin_ctl(27),
	    bt_rd(BT_W_SWCTRL, 0x4), bt_rd(BT_W_UART, 0x4), bt_rd(BT_W_UART, 0x1004u),
	    bt_rd(BT_W_UART, 0x2004u), bt_rd(BT_W_UART, 0x3004u));
	printf("PANDORA BT: pad samples sw_ctrl %u cts %u rx %u (GPIO_IO bit0)\n",
	    bt_rd(BT_W_SWCTRL, 0x4) & BT_PIN_INPUT, bt_rd(BT_W_UART, 0x4) & BT_PIN_INPUT,
	    bt_rd(BT_W_UART, 0x3004u) & BT_PIN_INPUT);
	pandora_log_sync();
	if ((vote & BT_VOTE_BITS) != BT_VOTE_BITS) {
		printf("PANDORA BT: wrapper3 not voted on; halt/SE registers not read (domain off)\n");
		pandora_log_sync();
		return;
	}
	printf("PANDORA BT: halts m_ahb %#x s_ahb %#x core %#x core2x %#x se5 %#x\n",
	    bt_rd(BT_W_HALT, BT_HALT_M_AHB), bt_rd(BT_W_HALT, BT_HALT_S_AHB),
	    bt_rd(BT_W_HALT, BT_HALT_CORE), bt_rd(BT_W_HALT, BT_HALT_CORE2X), bt_rd(BT_W_RCG, BT_SE5_HALT));
	pandora_log_sync();
	if (!bt_se_clocked()) {
		printf("PANDORA BT: SE5 unclocked at this point; its registers are not read\n");
		pandora_log_sync();
		return;
	}
	printf("PANDORA BT: SE status %#x fw %#x if_disable %#x clk_sel %#x hw_param0 %#x dma %#x\n",
	    bt_rd(BT_W_SE, SE_STATUS), bt_rd(BT_W_SE, SE_FW_REV), bt_rd(BT_W_SE, SE_IF_DISABLE),
	    bt_rd(BT_W_SE, SE_CLK_SEL), bt_rd(BT_W_SE, SE_HW_PARAM0), bt_rd(BT_W_SE, SE_DMA_MODE));
	pandora_log_sync();
	printf("PANDORA BT: SE M_CMD0 %#x M_IRQ %#x S_CMD0 %#x S_IRQ %#x tx_fifo %#x rx_fifo %#x\n",
	    bt_rd(BT_W_SE, SE_M_CMD0), bt_rd(BT_W_SE, SE_M_IRQ), bt_rd(BT_W_SE, SE_S_CMD0),
	    bt_rd(BT_W_SE, SE_S_IRQ), bt_rd(BT_W_SE, SE_TX_FIFO_ST), bt_rd(BT_W_SE, SE_RX_FIFO_ST));
	pandora_log_sync();
}

/* The DT's bt-vdd-* supplies, in the vendor btpower table's order. */
static int
bt_power(void)
{
	static const struct { const char *name; uint16_t mv; } rails[] = {
		{"L2G_E0", 1800}, {"L3G_E0", 1200}, {"S2J_E1", 892}, {"S7F_E0", 1328},
		{"S8F_E0", 1800}, {"S1J_E1", 892}, {"L6K_E1", 1800},
	};
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		bool s8f_trial = bt_level == 11 && !strcmp(rails[i].name, "S8F_E0");
		unsigned mv = s8f_trial ? 1876u : rails[i].mv;
		int err = pandora_rpmh_resource(rails[i].name, 0, mv);
		if (s8f_trial) {
			printf("PANDORA BT: level 11 S8F_E0 voltage request %u mV -> %d\n", mv, err);
		}
		if (!err) {
			err = pandora_rpmh_resource(rails[i].name, 4, 1);
			if (s8f_trial) {
				printf("PANDORA BT: level 11 S8F_E0 enable request -> %d\n", err);
			}
		}
		if (err) {
			printf("PANDORA BT: rail %s at %u mV failed (%d)\n", rails[i].name, mv, err);
			return err;
		}
	}
	printf("PANDORA BT: BT rails voted\n");
	return 0;
}

/* ftbl_gcc_qupv3_wrap3_s5_clk_src[] in clk_gcc-kaanapali.c:
 * F(64000000, P_GCC_GPLL0_OUT_EVEN, 1, 16, 75) = 300 MHz * 16 / 75, src index 6
 * (gcc_parent_map_0), MN mode (0) and pre-divider 1. iBoot leaves the RCG parked
 * on the TCXO with the branch gated; Android programs this entry before it opens
 * the UART. */
static void
bt_rcg_set(unsigned m, unsigned n)
{
	/* Read-modify-write the same way pandora_amp.c does: 0x371f covers
	 * src/mode/pre-divider, bit 20 is the parked-state bit iBoot leaves
	 * set. Source 6, MN mode, pre-divider 1 (encoded 0). */
	uint32_t cfg = bt_rd(BT_W_RCG, BT_RCG + BT_RCG_CFG);
	cfg &= ~(0x371fu | (1u << 20));
	cfg |= 6u << 8;
	bt_wr(BT_W_RCG, BT_RCG + BT_RCG_CFG, cfg);
	bt_wr(BT_W_RCG, BT_RCG + BT_RCG_M, m);
	bt_wr(BT_W_RCG, BT_RCG + BT_RCG_N, n);
	bt_wr(BT_W_RCG, BT_RCG + BT_RCG_D, 0);
	bt_wr(BT_W_RCG, BT_RCG + BT_RCG_CMD, bt_rd(BT_W_RCG, BT_RCG + BT_RCG_CMD) | 1u);
	for (unsigned i = 0; i < 5000 && (bt_rd(BT_W_RCG, BT_RCG + BT_RCG_CMD) & 1u); i++) {
		delay(1);
	}
	bt_wr(BT_W_RCG, BT_SE5_HALT, 1u);		/* the s5 branch enable */
}

/* PMIC SPMI arbiter v8 (DT arbiter@c400000, "qcom,canoe-spmi-pmic-arb").
 * PMH0104 is SID 9 on bus 1 (spmi@c437000), its GPIO block is 0x8800 and
 * GPIO5 lives at 0x8c00, so the PPID is (9 << 8) | (0x8c00 >> 8) = 0x98c.
 * v8 (unlike v7):
 *   - APID->PPID table is the separate "chnl_map" window, indexed 4*apid,
 *     PPID in bits[12:0];
 *   - per-EE/channel owner is "chnl_owner" of each bus, 4*apid (bus-local);
 *   - RW channel stride is 0x200 (v7 used 0x1000);
 *   - per-bus APID count is core+0x4, one word per bus, mask 0x1fff.
 * btpower normally gives this pin a low->high edge when it opens BT; iBoot
 * hands over register values whose meaning depends on the GPIO subtype. */
#define BT_SPMI_CORE_PA    0x0c400000u
#define BT_SPMI_MAP_PA     0x0c403000u  /* chnl_map */
#define BT_SPMI_CHNLS_PA   0x0c900000u
#define BT_SPMI_OWNER1_PA  0x0c43b000u  /* bus 1 (spmi@c437000) chnl_owner */
#define BT_SPMI_FEATURES   0x0004u
#define BT_SPMI_PERIPH_MASK 0x1fffu
#define BT_SPMI_PPID_MASK  0x1fffu
#define BT_SPMI_GPIO_PPID  0x98cu        /* PMH0104 gpio5: BT_EN */
#define BT_SPMI_FMD_PPID   0x98du        /* PMH0104 gpio6: FMD, DT input-only; diagnostics only */
#define BT_SPMI_RW_STRIDE  0x200u
#define BT_SPMI_MAX_PERIPHS 8192u
#define BT_SPMI_REG_TYPE   0x04u
#define BT_SPMI_REG_SUBTYPE 0x05u
#define BT_SPMI_REG_STATUS 0x10u        /* RT_STS bit0: pad sample when input buffer is enabled */
#define BT_SPMI_REG_MODE   0x40u        /* Legacy: direction 6:4, function 3:1, level bit0; LV/MV: direction 1:0 */
#define BT_SPMI_REG_VIN    0x41u        /* DIG_VIN_CTL: power source 2:0 */
#define BT_SPMI_REG_PULL   0x42u        /* DIG_PULL_CTL: 5 = pull disabled */
#define BT_SPMI_REG_IN     0x43u        /* DIG_IN_CTL */
#define BT_SPMI_REG_SOURCE 0x44u       /* LV/MV: normal source 0, output value bit7 */
#define BT_SPMI_REG_OUT    0x45u        /* DIG_OUT_CTL: strength 1:0, buffer type 5:4 */
#define BT_SPMI_REG_EN     0x46u        /* EN_CTL, bit 7 = master enable */

static vm_offset_t bt_spmi_core, bt_spmi_map, bt_spmi_chnls, bt_spmi_owner1;
static bool bt_spmi_ready;
static uint32_t bt_bus0, bt_bus1;

static uint32_t
bt_spmi_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static bool
bt_spmi_write(vm_offset_t chan, uint8_t reg, uint8_t val)
{
	*(volatile uint32_t *)(chan + 0x10u) = val;              /* WDATA0 */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(chan + 0x00u) = ((uint32_t)reg << 4); /* EXT_WRITEL, bc = 0 -> 1 byte */
	__asm__ volatile("dsb osh" ::: "memory");
	delay(10);	/* let the arbiter clear the stale DONE before polling */
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t st = bt_spmi_rd(chan, 0x08u);
		if (st & 1u) {
			return (st & 0x0eu) == 0;
		}
		delay(1);
	}
	return false;
}

/* SPMI v8 EXT_READL: opcode 1, byte count 0, result in RDATA0. */
static bool
bt_spmi_read8(vm_offset_t chan, uint8_t reg, uint8_t *out)
{
	/* EXT_READL is opcode 1 in bits 31:27 (mainline pmic_arb_fmt_cmd_v2:
	 * (opc << 27) | (addr << 4) | bc); one byte lands in RDATA0 at 0x18. */
	*(volatile uint32_t *)(chan + 0x00u) = (1u << 27) | ((uint32_t)reg << 4);
	__asm__ volatile("dsb osh" ::: "memory");
	delay(10);
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t st = bt_spmi_rd(chan, 0x08u);
		if (st & 1u) {
			*out = (uint8_t)(bt_spmi_rd(chan, 0x18u) & 0xffu);
			return (st & 0x0eu) == 0;
		}
		delay(1);
	}
	return false;
}

/* RPMh RF_CLK1 (DT clocks = <&rpmhcc 6>, clock-names "bt_rf_clk1"): btpower
 * enables it before opening the UART. Without the RF clock the WCN BT
 * subsystem stays quiet even with BT_EN high. The CMD-DB name is tried
 * in the few spellings an 8-byte RPMh resource name allows. */
static int
bt_rf_clk(void)
{
	static const char *names[] = { "rfclka1", "rfclka2", "rfclk1" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (pandora_rpmh_resource(names[i], 0, 1) == 0) {
			printf("PANDORA BT: RF clock voted via %s\n", names[i]);
			return 0;
		}
	}
	printf("PANDORA BT: RF clock not found in CMD-DB\n");
	return ENOENT;
}

/* Resolve only AP-owned bus1 channels. Reads and writes must use the same
 * channel; a matching PPID owned by another EE is not an alternate route. */
static int
bt_spmi_channel(uint32_t bus0, uint32_t bus1, uint16_t ppid, vm_offset_t *chan)
{
	uint16_t apid = 0xffffu;
	unsigned owner = 7;
	for (uint32_t i = bus0; i < bus0 + bus1; i++) {
		uint32_t e = bt_spmi_rd(bt_spmi_map, 4u * i);
		if ((e & BT_SPMI_PPID_MASK) != ppid) {
			continue;
		}
		unsigned o = bt_spmi_rd(bt_spmi_owner1, 4u * (i - bus0)) & 7u;
		if (apid == 0xffffu || o == 0) {
			apid = (uint16_t)i;
			owner = o;
		}
		if (o == 0) {
			break;
		}
	}
	if (apid == 0xffffu || owner != 0) {
		printf("PANDORA BT: ppid %#x apid %u/ee%u: skipped\n", ppid, apid, owner);
		return ENOTSUP;
	}
	*chan = bt_spmi_chnls + BT_SPMI_RW_STRIDE * apid;
	return 0;
}

struct bt_gpio_state {
	uint8_t type, subtype, status, mode, vin, pull, in, source, out, en;
	bool lv_mv;
};

/* pinctrl-spmi-gpio.c pmic_gpio_populate(): TYPE/SUBTYPE, not the PMIC's marketing
 * name or the observed MODE value, determines which layout is valid. */
static int
bt_gpio_read(vm_offset_t chan, struct bt_gpio_state *s)
{
	*s = (struct bt_gpio_state){0};
	if (!bt_spmi_read8(chan, BT_SPMI_REG_TYPE, &s->type) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_SUBTYPE, &s->subtype)) {
		return EIO;
	}
	if (s->type != 0x10) {
		return ENOTSUP;
	}
	switch (s->subtype) {
	case 0x01: case 0x05: case 0x09: case 0x0d:
		break;
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
		s->lv_mv = true;
		break;
	default:
		return ENOTSUP;
	}
	if (!bt_spmi_read8(chan, BT_SPMI_REG_STATUS, &s->status) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_MODE, &s->mode) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_VIN, &s->vin) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_PULL, &s->pull) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_IN, &s->in) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_OUT, &s->out) ||
	    !bt_spmi_read8(chan, BT_SPMI_REG_EN, &s->en) ||
	    (s->lv_mv && !bt_spmi_read8(chan, BT_SPMI_REG_SOURCE, &s->source))) {
		return EIO;
	}
	return 0;
}

static void
bt_gpio_dump(const char *what, uint16_t ppid, const struct bt_gpio_state *s)
{
	/* DIG_IN_CTL is input routing, not a sample. RT_STS is only meaningful
	 * with the input buffer enabled; an output-only pad can report zero. */
	printf("PANDORA BT: %s ppid %#x type %#x subtype %#x %s mode %#x source %#x vin %#x pull %#x in_cfg %#x out %#x en %#x rt_status %#x\n",
	    what, ppid, s->type, s->subtype, s->lv_mv ? "LV/MV" : "legacy",
	    s->mode, s->source, s->vin, s->pull, s->in, s->out, s->en, s->status);
	pandora_log_sync();
}

static int
bt_spmi_gpio(uint32_t bus0, uint32_t bus1, uint16_t ppid, uint8_t val, unsigned vin, const char *what)
{
	vm_offset_t chan;
	struct bt_gpio_state before, after;
	int err = bt_spmi_channel(bus0, bus1, ppid, &chan);
	if (err) {
		return err;
	}
	err = bt_gpio_read(chan, &before);
	if (err) {
		printf("PANDORA BT: %s GPIO identity/config read failed (%d), type %#x subtype %#x\n",
		    what, err, before.type, before.subtype);
		return err;
	}
	bt_gpio_dump("before", ppid, &before);
	unsigned sources;
	if (!before.lv_mv) {
		sources = (before.subtype == 0x01 || before.subtype == 0x05) ? 4 : 8;
	} else {
		sources = before.subtype == 0x10 ? 1 :
		    (before.subtype == 0x13 || before.subtype == 0x15) ? 3 : 2;
	}
	if (vin >= sources) {
		printf("PANDORA BT: %s VIN %u unsupported by subtype %#x\n", what, vin, before.subtype);
		return EINVAL;
	}
	/* Legacy normal GPIO output uses MODE=0x10/0x11. LV/MV uses MODE=1
	 * and SOURCE=0/0x80. MODE=0x12 means paired-low on legacy, and does
	 * not set the output latch on LV/MV. Never infer this from readback. */
	uint8_t mode = before.lv_mv ? 1u : (uint8_t)(0x10u | (val & 1u));
	uint8_t source = val & 1u ? 0x80u : 0;
	bool ok = bt_spmi_write(chan, BT_SPMI_REG_VIN, (uint8_t)vin) &&
	    bt_spmi_write(chan, BT_SPMI_REG_PULL, 0x05u) &&
	    bt_spmi_write(chan, BT_SPMI_REG_OUT, 0x03u) &&
	    (!before.lv_mv || bt_spmi_write(chan, BT_SPMI_REG_SOURCE, source)) &&
	    bt_spmi_write(chan, BT_SPMI_REG_MODE, mode) &&
	    bt_spmi_write(chan, BT_SPMI_REG_EN, 0x80u);
	if (!ok) {
		printf("PANDORA BT: %s SPMI write failed; power sequence stopped\n", what);
		return EIO;
	}
	err = bt_gpio_read(chan, &after);
	if (err) {
		printf("PANDORA BT: %s readback failed (%d)\n", what, err);
		return err;
	}
	bt_gpio_dump(what, ppid, &after);
	if (after.mode != mode || after.vin != vin || after.pull != 0x05u ||
	    after.out != 0x03u || after.en != 0x80u ||
	    (before.lv_mv && after.source != source)) {
		printf("PANDORA BT: %s config mismatch; power sequence stopped\n", what);
		return EIO;
	}
	return 0;
}

static int
bt_spmi_init(void)
{
	/* Map once: the retry loop below re-enters this function after a failed
	 * attempt and the arbiter window must not be mapped twice. */
	if (!bt_spmi_ready) {
		if (!bt_spmi_core) {
			bt_spmi_core = ml_io_map_unmappable(BT_SPMI_CORE_PA, 0x3000, VM_WIMG_IO);
		}
		if (!bt_spmi_map) {
			bt_spmi_map = ml_io_map_unmappable(BT_SPMI_MAP_PA, 0x8000, VM_WIMG_IO);
		}
		if (!bt_spmi_chnls) {
			bt_spmi_chnls = ml_io_map_unmappable(BT_SPMI_CHNLS_PA, 0x400000, VM_WIMG_IO);
		}
		if (!bt_spmi_owner1) {
			bt_spmi_owner1 = ml_io_map_unmappable(BT_SPMI_OWNER1_PA, 0x8000, VM_WIMG_IO);
		}
		if (!bt_spmi_core || !bt_spmi_map || !bt_spmi_chnls || !bt_spmi_owner1) {
			printf("PANDORA BT: SPMI map failed: no BT_EN pulse\n");
			return ENOMEM;
		}
		uint32_t version = bt_spmi_rd(bt_spmi_core, 0);
		if (version < 0x80000000u) {
			printf("PANDORA BT: SPMI arbiter %#x is not v8: no BT_EN pulse\n", version);
			return ENOTSUP;
		}
		bt_bus0 = bt_spmi_rd(bt_spmi_core, BT_SPMI_FEATURES) & BT_SPMI_PERIPH_MASK;
		bt_bus1 = bt_spmi_rd(bt_spmi_core, BT_SPMI_FEATURES + 4u) & BT_SPMI_PERIPH_MASK;
		if (bt_bus1 == 0 || bt_bus0 + bt_bus1 > BT_SPMI_MAX_PERIPHS) {
			printf("PANDORA BT: SPMI bus1 apids %u (bus0 %u): no BT_EN pulse\n", bt_bus1, bt_bus0);
			return ENOTSUP;
		}
		bt_spmi_ready = true;
	}
	return 0;
}

static void
bt_pmic_report(void)
{
	int err = bt_spmi_init();
	if (err) {
		return;
	}
	static const uint16_t ppids[] = { BT_SPMI_GPIO_PPID, BT_SPMI_FMD_PPID };
	for (unsigned i = 0; i < sizeof(ppids) / sizeof(ppids[0]); i++) {
		vm_offset_t chan;
		struct bt_gpio_state s;
		err = bt_spmi_channel(bt_bus0, bt_bus1, ppids[i], &chan);
		if (!err) {
			err = bt_gpio_read(chan, &s);
		}
		if (err) {
			printf("PANDORA BT: read-only PMIC ppid %#x failed (%d)\n", ppids[i], err);
			continue;
		}
		bt_gpio_dump("read-only PMIC", ppids[i], &s);
	}
}

static int
bt_en_pulse(void)
{
	int err = bt_spmi_init();
	if (err) {
		return err;
	}
	uint32_t bus0 = bt_bus0, bus1 = bt_bus1;
	/* PMH0104 GPIO6 is bt_fmd_clk_en, configured as an input at VIN1 in
	 * this board's live DT. SM8850 btpower only records that GPIO; its XO
	 * helper uses the separate qcom,xo-clk-gpio. Preserve GPIO6 instead of
	 * driving the FMD input high on VIN0 as earlier experiments did. */
	err = bt_spmi_gpio(bus0, bus1, BT_SPMI_GPIO_PPID, 0x00, 1, "BT_EN low");
	if (err) {
		return err;
	}
	bt_sleep(100 * 1000);
	err = bt_spmi_gpio(bus0, bus1, BT_SPMI_GPIO_PPID, 0x01, 1, "BT_EN high");
	if (err) {
		return err;
	}
	bt_sleep(150 * 1000);	/* Linux qca_power_on: 150 ms after BT_EN */
	/* There is no qcom,btwake_gpio binding on this board. In particular,
	 * mpm_wake_set_gpios[1] does not establish GPIO9 as a dev-wake output.
	 * Leave that unrelated pad in its inherited configuration. */
	return 0;
}

static int
bt_clocks(void)
{
	uint32_t vote = bt_rd(BT_W_VOTE, BT_VOTE);
	if ((vote & BT_VOTE_BITS) != BT_VOTE_BITS) {
		bt_wr(BT_W_VOTE, BT_VOTE, vote | BT_VOTE_BITS);
	}
	/* level 5 takes over a chip that Android already trained to 4 Mbps:
	 * 128 MHz RCG (M 32 / N 75) gives divider 1. Everything else stays at
	 * the 64 MHz/115200 pairing. */
	if (bt_level == 5) {
		bt_rcg_set(32, 75);
	} else {
		bt_rcg_set(16, 75);
	}
	if (bt_wait_on(BT_W_HALT, BT_HALT_M_AHB, "m_ahb") || bt_wait_on(BT_W_HALT, BT_HALT_S_AHB, "s_ahb") ||
	    bt_wait_on(BT_W_HALT, BT_HALT_CORE, "core") || bt_wait_on(BT_W_HALT, BT_HALT_CORE2X, "core_2x") ||
	    bt_wait_on(BT_W_RCG, BT_SE5_HALT, "se5")) {
		return ETIMEDOUT;
	}
	bt_rate = bt_rcg_rate();
	return 0;
}

/* Stop persistent commands before reconfiguration or loopback cleanup. The
 * returned error keeps a failed abort visible instead of assuming quiescence. */
static int
bt_uart_stop(void)
{
	uint32_t active = bt_rd(BT_W_SE, SE_STATUS);
	if (active & GENI_STATUS_M_ACTIVE) {
		bt_wr(BT_W_SE, SE_M_CMD_CTRL, GENI_CMD_ABORT);
	}
	if (active & GENI_STATUS_S_ACTIVE) {
		bt_wr(BT_W_SE, SE_S_CMD_CTRL, GENI_CMD_ABORT);
	}
	for (unsigned waited = 0; waited < 5000; waited++) {
		if (!(bt_rd(BT_W_SE, SE_STATUS) & (GENI_STATUS_M_ACTIVE | GENI_STATUS_S_ACTIVE))) {
			return 0;
		}
		delay(1);
	}
	printf("PANDORA BT: UART sequencers did not stop (status %#x)\n", bt_rd(BT_W_SE, SE_STATUS));
	return ETIMEDOUT;
}

/* FIFO-mode UART, as Linux msm_geni_serial.c / u-boot serial_msm_geni.c.
 * Loopback must be selected before starting the persistent RX sequencer. */
static int
bt_uart_init(unsigned baud, bool loopback)
{
	uint32_t fw = bt_rd(BT_W_SE, SE_FW_REV);
	if (bt_rate < 1000000u) {
		printf("PANDORA BT: SE5 clock is %u Hz; the UART is not configured\n", bt_rate);
		return EIO;
	}
	if (bt_rd(BT_W_SE, SE_IF_DISABLE) & 1u) {
		printf("PANDORA BT: SE5 FIFO interface is disabled (fw %#x); no UART\n", fw);
		return ENOTSUP;
	}
	if (((fw >> 8) & 0xffu) != 2u) {
		printf("PANDORA BT: SE5 firmware protocol is not UART (%#x)\n", fw);
		return ENOTSUP;
	}
	printf("PANDORA BT: SE5 protocol %u (fw %#x), FIFO interface\n", (fw >> 8) & 0xffu, fw);
	/* Android's msm_geni_serial_config_baud_rate and upstream both switch
	 * from 32x to 16x at QUP v2.5. SE_FW_REV identifies protocol firmware,
	 * not that hardware revision. A correct RCG readback alone cannot
	 * establish the wire baud rate. Use the driver's rounded-up divider. */
	uint32_t ver = bt_rd(BT_W_WRAPPER, 4);
	if (ver == 0 || ver == 0xffffffffu) {
		printf("PANDORA BT: invalid QUP wrapper version %#x\n", ver);
		return EIO;
	}
	unsigned sampling = ver >= 0x20050000u ? 16 : 32;
	unsigned div = (bt_rate + baud * sampling - 1u) / (baud * sampling);
	if (div < 1 || div > 0xfff) {
		printf("PANDORA BT: UART divider %u out of range\n", div);
		return EINVAL;
	}
	printf("PANDORA BT: QUP version %#x sampling %u; UART %u baud from %u Hz: divider %u (actual %u)\n",
	    ver, sampling, baud, bt_rate, div, bt_rate / (div * sampling));
	/* The receiver is a persistent secondary command. Stop inherited or
	 * previous-attempt commands before changing their clock and format. */
	int err = bt_uart_stop();
	if (err) {
		return err;
	}
	bt_wr(BT_W_SE, SE_UART_LOOPBACK, loopback ? 3u : 0);
	if (bt_rd(BT_W_SE, SE_UART_LOOPBACK) != (loopback ? 3u : 0)) {
		printf("PANDORA BT: UART loopback configuration did not latch\n");
		return EIO;
	}

	bt_wr(BT_W_SE, SE_GSI_EVENT_EN, 0);
	bt_wr(BT_W_SE, SE_M_IRQ_CLR, 0xffffffffu);
	bt_wr(BT_W_SE, SE_S_IRQ_CLR, 0xffffffffu);
	bt_wr(BT_W_SE, GENI_FORCE_DEFAULT, 1);
	bt_wr(BT_W_SE, GENI_OUTPUT_CTRL, 0x7fu);
	bt_wr(BT_W_SE, GENI_CGC_CTRL, bt_rd(BT_W_SE, GENI_CGC_CTRL) | 0x7fu);
	bt_wr(BT_W_SE, SE_DMA_MODE, bt_rd(BT_W_SE, SE_DMA_MODE) & ~1u);

	/* 8 data bits, one byte per FIFO word (u-boot serial_msm_geni.c's proven
	 * FIFO layout), no parity, one stop bit. */
	bt_wr(BT_W_SE, SE_RX_PACK0, 0xfu);
	bt_wr(BT_W_SE, SE_RX_PACK1, 0);
	/* One byte per FIFO word on TX as well: the TX path writes single bytes. */
	bt_wr(BT_W_SE, SE_TX_PACK0, 0xfu);
	bt_wr(BT_W_SE, SE_TX_PACK1, 0);
	bt_wr(BT_W_SE, SE_TX_WORD_LEN, 8);
	bt_wr(BT_W_SE, SE_RX_WORD_LEN, 8);
	bt_wr(BT_W_SE, SE_TX_STOP_LEN, 0);
	bt_wr(BT_W_SE, SE_TX_PARITY, 0);
	bt_wr(BT_W_SE, SE_RX_PARITY, 0);
	bt_wr(BT_W_SE, SE_RX_STALE, 0xf);
	bt_wr(BT_W_SE, SE_TX_TRANS_CFG, bt_rd(BT_W_SE, SE_TX_TRANS_CFG) | (1u << 1)); /* mask external CTS */
	/* CTS_MASK belongs to TX_TRANS_CFG only. Android's observed RX value
	 * is zero and its module never sets RX bit1. Isolate removal of the
	 * historical write in level 9; preserve the measured older levels. */
	uint32_t rx_cfg = bt_rd(BT_W_SE, SE_RX_TRANS_CFG);
	rx_cfg = (bt_level >= 9) ? rx_cfg & ~(1u << 1) : rx_cfg | (1u << 1);
	bt_wr(BT_W_SE, SE_RX_TRANS_CFG, rx_cfg);
	if (bt_level >= 9) {
		uint32_t actual = bt_rd(BT_W_SE, SE_RX_TRANS_CFG);
		printf("PANDORA BT: level %d RX bit1 comparison requested %#x readback %#x\n", bt_level, rx_cfg, actual);
		if (actual != rx_cfg) {
			return EIO;
		}
	}

	bt_wr(BT_W_SE, SE_TX_WM, 2);
	bt_wr(BT_W_SE, SE_RX_WM, 2);
	bt_wr(BT_W_SE, SE_RX_RFR_WM, 1);
	bt_wr(BT_W_SE, SE_IRQ_EN, bt_rd(BT_W_SE, SE_IRQ_EN) | 0xfu);
	bt_wr(BT_W_SE, SE_M_IRQ_EN, M_CMD_DONE | M_TX_WM | M_RX_WM | M_RX_LAST | M_SEC_IRQ);
	bt_wr(BT_W_SE, SE_S_IRQ_EN, S_RX_WM | S_RX_LAST);

	/* The serial engine clock divider sets the baud rate on both sequencers. */
	bt_wr(BT_W_SE, SE_SER_M_CLK, (div << 4) | SER_CLK_EN);
	bt_wr(BT_W_SE, SE_SER_S_CLK, (div << 4) | SER_CLK_EN);

	/* The secondary sequencer listens for the first byte. */
	uint32_t rx_cmd = 1u << 27;
	if (bt_level >= 10) {
		/* The local Android module writes 0x080000e0: RFR_OPEN plus
		 * skip-frame/break flags. Isolate bit7 from the error filters. */
		rx_cmd |= UART_PARAM_RFR_OPEN;
	}
	bt_wr(BT_W_SE, SE_S_CMD0, rx_cmd);
	if (bt_level >= 10) {
		uint32_t actual = bt_rd(BT_W_SE, SE_S_CMD0);
		printf("PANDORA BT: level %d RFR_OPEN command requested %#x readback %#x\n", bt_level, rx_cmd, actual);
		if (actual != rx_cmd) {
			return EIO;
		}
	}
	return 0;
}

/* Read back what bt_uart_init() programmed, plus CTS/RX pad input levels.
 * GPIO_IO bit0 is the sample; bit1 is the output latch. */
static void
bt_uart_dump(void)
{
	printf("PANDORA BT: UART tx_cfg %#x rx_cfg %#x pack0 %#x pack1 %#x wl %u/%u stop %#x par %#x/%#x\n",
	    bt_rd(BT_W_SE, SE_TX_TRANS_CFG), bt_rd(BT_W_SE, SE_RX_TRANS_CFG),
	    bt_rd(BT_W_SE, SE_RX_PACK0), bt_rd(BT_W_SE, SE_RX_PACK1),
	    bt_rd(BT_W_SE, SE_TX_WORD_LEN), bt_rd(BT_W_SE, SE_RX_WORD_LEN),
	    bt_rd(BT_W_SE, SE_TX_STOP_LEN), bt_rd(BT_W_SE, SE_TX_PARITY), bt_rd(BT_W_SE, SE_RX_PARITY));
	printf("PANDORA BT: UART mclk %#x sclk %#x irq_en %#x m_irq_en %#x s_irq_en %#x stale %#x\n",
	    bt_rd(BT_W_SE, SE_SER_M_CLK), bt_rd(BT_W_SE, SE_SER_S_CLK), bt_rd(BT_W_SE, SE_IRQ_EN),
	    bt_rd(BT_W_SE, SE_M_IRQ_EN), bt_rd(BT_W_SE, SE_S_IRQ_EN), bt_rd(BT_W_SE, SE_RX_STALE));
	printf("PANDORA BT: UART status %#x m_irq %#x s_irq %#x tx_fifo %#x rx_fifo %#x cts_in %#x rx_in %#x\n",
	    bt_rd(BT_W_SE, SE_STATUS), bt_rd(BT_W_SE, SE_M_IRQ), bt_rd(BT_W_SE, SE_S_IRQ),
	    bt_rd(BT_W_SE, SE_TX_FIFO_ST), bt_rd(BT_W_SE, SE_RX_FIFO_ST),
	    bt_rd(BT_W_UART, 0x4) & BT_PIN_INPUT, bt_rd(BT_W_UART, 3 * 0x1000u + 0x4) & BT_PIN_INPUT);
	printf("PANDORA BT: GENI IOS %#x (RX bit0, IO2/CTS bit1)\n", bt_rd(BT_W_SE, SE_IOS));
	printf("PANDORA BT: UART routing loopback %#x io_macro %#x manual_rfr %#x\n",
	    bt_rd(BT_W_SE, SE_UART_LOOPBACK), bt_rd(BT_W_SE, SE_UART_IO_MACRO), bt_rd(BT_W_SE, SE_UART_RFR));
}

/* Wait for up to MS, collecting RX FIFO words; returns bytes in OUT. */
static unsigned
bt_rx_poll(uint8_t *out, unsigned cap, unsigned ms)
{
	uint64_t deadline;
	unsigned got = 0;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint32_t st = bt_rd(BT_W_SE, SE_RX_FIFO_ST);
		unsigned words = st & 0x1ffffffu;
		while (words-- && got < cap) {
			out[got++] = (uint8_t)bt_rd(BT_W_SE, SE_RX_FIFO);
		}
		/* One HCI event: type, event code, length, then that many bytes. */
		if (got == cap || (got >= 3 && out[0] == 0x04 && got >= 3u + out[2])) {
			break;
		}
		bt_sleep(1000);
	}
	return got;
}

static int
bt_uart_tx(const uint8_t *pkt, unsigned len, const char *what)
{
	unsigned i;

	for (i = 0; i < 5000 && (bt_rd(BT_W_SE, SE_STATUS) & GENI_STATUS_M_ACTIVE); i++) {
		delay(1);
	}
	if (bt_rd(BT_W_SE, SE_STATUS) & GENI_STATUS_M_ACTIVE) {
		printf("PANDORA BT: %s: previous TX still active\n", what);
		return ETIMEDOUT;
	}
	bt_wr(BT_W_SE, SE_M_IRQ_CLR, 0xffffffffu);
	bt_wr(BT_W_SE, SE_TX_WM, 2);
	bt_wr(BT_W_SE, SE_TX_TRANS_LEN, len);
	bt_wr(BT_W_SE, SE_M_CMD0, 1u << 27);   /* UART_START_TX */

	for (i = 0; i < 1000 && !(bt_rd(BT_W_SE, SE_M_IRQ) & M_TX_WM); i++) {
		delay(1);
	}
	if (!(bt_rd(BT_W_SE, SE_M_IRQ) & M_TX_WM)) {
		printf("PANDORA BT: %s: TX FIFO never opened (irq %#x)\n", what, bt_rd(BT_W_SE, SE_M_IRQ));
		bt_wr(BT_W_SE, SE_M_CMD_CTRL, GENI_CMD_ABORT);
		return ETIMEDOUT;
	}
	for (i = 0; i < len; i++) {
		bt_wr(BT_W_SE, SE_TX_FIFO, pkt[i]);   /* one byte per word */
	}
	bt_wr(BT_W_SE, SE_M_IRQ_CLR, M_TX_WM);
	for (i = 0; i < 2000 && !(bt_rd(BT_W_SE, SE_M_IRQ) & M_CMD_DONE); i++) {
		delay(1);
	}
	uint32_t irq = bt_rd(BT_W_SE, SE_M_IRQ);
	if (!(irq & M_CMD_DONE)) {
		printf("PANDORA BT: %s: command not done (irq %#x status %#x)\n", what, irq, bt_rd(BT_W_SE, SE_STATUS));
		bt_wr(BT_W_SE, SE_M_CMD_CTRL, GENI_CMD_ABORT);
		bt_wr(BT_W_SE, SE_M_IRQ_CLR, 0xffffffffu);
		return ETIMEDOUT;
	}
	bt_wr(BT_W_SE, SE_M_IRQ_CLR, M_CMD_DONE);
	printf("PANDORA BT: %s sent (%u bytes), tx io %#x\n", what, len, bt_rd(BT_W_UART, 0x2004u));
	return 0;
}

static void
bt_dump(const char *what, const uint8_t *buf, unsigned n)
{
	printf("PANDORA BT: %s: %u bytes", what, n);
	for (unsigned i = 0; i < n && i < 32; i++) {
		printf(" %02x", buf[i]);
	}
	printf("\n");
}

/* Isolate FIFO packing and both sequencers from the external controller.
 * The fixed eight-byte input fits the SE5 TX FIFO and exercises low/high and
 * alternating bits. Wait a bounded interval and reject extra/stale bytes.
 * Restore the inherited loopback value after stopping commands on every path;
 * this does not restore the entire UART to a previous OS's working session. */
static void
bt_loopback(void)
{
	static const uint8_t pattern[] = { 0x55, 0xaa, 0x00, 0xff, 0x01, 0x80, 0x7e, 0x81 };
	uint8_t rx[16];
	uint32_t saved_loopback = bt_rd(BT_W_SE, SE_UART_LOOPBACK);
	int err = bt_uart_init(115200, true);
	unsigned got = 0;
	if (!err) {
		/* Ignore inherited FIFO bytes only after the receiver is routed
		 * internally. A stuck/non-empty FIFO is a setup failure, not data
		 * to count as loopback response. No unbounded FIFO draining. */
		unsigned discarded = 0;
		while (discarded < 64 && (bt_rd(BT_W_SE, SE_RX_FIFO_ST) & 0x1ffffffu)) {
			(void)bt_rd(BT_W_SE, SE_RX_FIFO);
			discarded++;
		}
		printf("PANDORA BT: internal loopback config %#x; discarded %u inherited bytes\n",
		    bt_rd(BT_W_SE, SE_UART_LOOPBACK), discarded);
		if (bt_rd(BT_W_SE, SE_RX_FIFO_ST) & 0x1ffffffu) {
			err = EIO;
		} else {
			bt_wr(BT_W_SE, SE_S_IRQ_CLR, 0xffffffffu);
			err = bt_uart_tx(pattern, sizeof(pattern), "internal loopback pattern");
			if (!err) {
				got = bt_rx_poll(rx, sizeof(rx), 200);
				bt_dump("internal loopback RX", rx, got);
				if (got != sizeof(pattern) || memcmp(rx, pattern, sizeof(pattern))) {
					err = EIO;
				}
			}
		}
		bt_uart_dump();
	}
	int stop_err = bt_uart_stop();
	bt_wr(BT_W_SE, SE_UART_LOOPBACK, saved_loopback);
	if (!err && stop_err) {
		err = stop_err;
	}
	if (bt_rd(BT_W_SE, SE_UART_LOOPBACK) != saved_loopback) {
		printf("PANDORA BT: loopback restore failed\n");
		err = EIO;
	}
	printf("PANDORA BT: internal loopback %s (error %d, received %u/%u); no controller-ready verdict\n",
	    err ? "FAIL" : "PASS", err, got, (unsigned)sizeof(pattern));
}

static bool
bt_try_hci_reset(void)
{
	uint8_t rx[96];

	if (bt_uart_tx(bt_hci_reset, HCI_RESET_LEN, "HCI Reset")) {
		return false;
	}
	unsigned got = bt_rx_poll(rx, sizeof(rx), 200);
	if (!got) {
		printf("PANDORA BT: no HCI event within 200 ms\n");
		return false;
	}
	bt_dump("HCI event", rx, got);
	if (got >= 7 && rx[0] == 0x04 && rx[1] == 0x0e && rx[2] == 4 &&
	    rx[4] == 0x03 && rx[5] == 0x0c) {
		printf("PANDORA BT: Command Complete opcode %#x status %u\n",
		    (unsigned)rx[4] | (unsigned)rx[5] << 8, rx[6]);
		return rx[6] == 0;
	}
	printf("PANDORA BT: response is not a successful HCI Reset completion\n");
	return false;
}

/* Read a precise number of bytes against one absolute deadline. Unlike the
 * legacy HCI helper, this never guesses frame type from partial payloads or
 * drains bytes beyond the caller's current field. The one-byte FIFO packing
 * is shared with the device-proven internal loopback. */
static int
bt_rx_exact(uint8_t *out, unsigned want, unsigned *got, uint64_t deadline)
{
	*got = 0;
	while (*got < want && mach_absolute_time() < deadline) {
		if (bt_rd(BT_W_SE, SE_RX_FIFO_ST) & 0x1ffffffu) {
			out[(*got)++] = (uint8_t)bt_rd(BT_W_SE, SE_RX_FIFO);
		} else {
			bt_sleep(1000);
		}
	}
	return *got == want ? 0 : ETIMEDOUT;
}

/* Android PeriPatchDLManager::WakeUpSoC / ReadHciPeriEvent and the captured
 * UART trace both require FD -> FC before the version request. Peer FD is
 * acknowledged with FC; FE is sleep indication. Bound retries and control
 * traffic, reject any other byte, and never count peer wake as our ACK. */
static int
bt_peri_wake(void)
{
	static const uint8_t wake = 0xfd, ack = 0xfc;
	unsigned controls = 0;
	for (unsigned attempt = 1; attempt <= 3; attempt++) {
		int err = bt_uart_tx(&wake, 1, "PERI wake FD");
		if (err) {
			return err;
		}
		uint64_t deadline;
		clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
		while (mach_absolute_time() < deadline) {
			uint8_t byte;
			unsigned got;
			err = bt_rx_exact(&byte, 1, &got, deadline);
			if (err) {
				break;
			}
			printf("PANDORA BT: PERI wake attempt %u RX %#x\n", attempt, byte);
			if (++controls > 32) {
				return EOVERFLOW;
			}
			if (byte == ack) {
				printf("PANDORA BT: PERI wake ACK FC received\n");
				return 0;
			}
			if (byte == wake) {
				err = bt_uart_tx(&ack, 1, "PERI peer wake ACK FC");
				if (err) {
					return err;
				}
			} else if (byte != 0xfe) {
				printf("PANDORA BT: unexpected byte during PERI wake\n");
				return EPROTO;
			}
		}
	}
	printf("PANDORA BT: PERI wake timed out; version request not sent\n");
	return ETIMEDOUT;
}

/* Deliberately narrow ROM probe, not a general PERI transport. The exact
 * request, 0x0034 event header and 25-byte success layout are evidenced by
 * Android trace plus the owning HAL's framing/event handlers. Keep a strict
 * length/opcode/subcommand/subsystem/status check; unsupported or truncated
 * frames fail without replay, baud changes, firmware writes or a reset. */
static int
bt_peri_probe(void)
{
	static const uint8_t request[] = { 0x31, 0x00, 0xf0, 0xff, 0x02, 0x05, 0x00 };
	static const uint8_t identity[] = { 0xf0, 0x01, 0x01, 0xf0, 0xff };
	uint8_t rx[32] = {0};
	unsigned got = 0, field = 0;
	int err = bt_peri_wake();
	if (err) {
		return err;
	}
	err = bt_uart_tx(request, sizeof(request), "PERI version request");
	if (err) {
		return err;
	}
	uint64_t deadline;
	clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
	/* Control bytes may occur only between frames; FD/FE inside the event
	 * payload are data. Read them before selecting the fixed header. */
	for (unsigned controls = 0; controls < 32; controls++) {
		err = bt_rx_exact(rx, 1, &field, deadline);
		got = field;
		if (err) {
			goto done;
		}
		if (rx[0] == 0x34) {
			break;
		}
		printf("PANDORA BT: PERI between-frame byte %#x\n", rx[0]);
		if (rx[0] == 0xfd) {
			const uint8_t ack = 0xfc;
			err = bt_uart_tx(&ack, 1, "PERI between-frame wake ACK FC");
			if (err) {
				goto done;
			}
		} else if (rx[0] != 0xfc && rx[0] != 0xfe) {
			err = EPROTO;
			goto done;
		}
		got = 0;
	}
	if (!got || rx[0] != 0x34) {
		err = EOVERFLOW;
		goto done;
	}
	err = bt_rx_exact(rx + 1, 3, &field, deadline);
	got += field;
	if (err) {
		goto done;
	}
	if (rx[1] != 0 || rx[2] != 0xff) {
		err = EPROTO;
		goto done;
	}
	if ((unsigned)rx[3] > sizeof(rx) - 4) {
		err = EMSGSIZE;
		goto done;
	}
	err = bt_rx_exact(rx + 4, rx[3], &field, deadline);
	got += field;
	if (err) {
		goto done;
	}
	if (got != 25 || rx[3] != 21 || memcmp(rx + 4, identity, sizeof(identity)) ||
	    rx[10] != 0x05 || rx[11] != 0x00) {
		err = EPROTO;
		goto done;
	}
	printf("PANDORA BT: PERI version response status %u\n", rx[9]);
	if (rx[9]) {
		err = EIO;
	}
done:
	bt_dump("PERI raw response", rx, got);
	printf("PANDORA BT: PERI version probe %s (error %d); no Bluetooth-ready verdict\n",
	    err ? "FAIL" : "PASS", err);
	return err;
}

static void
bt_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	printf("PANDORA BT: thread start level=%d\n", bt_level);
	pandora_log_sync();
	if (bt_map_all()) {
		printf("PANDORA BT: map_all FAILED\n");
		pandora_log_sync();
		goto out;
	}
	printf("PANDORA BT: mapped vote=%#llx rcg=%#llx halt=%#llx se=%#llx sw=%#llx uart=%#llx\n",
	    (unsigned long long)bt_wins[BT_W_VOTE].va, (unsigned long long)bt_wins[BT_W_RCG].va,
	    (unsigned long long)bt_wins[BT_W_HALT].va, (unsigned long long)bt_wins[BT_W_SE].va,
	    (unsigned long long)bt_wins[BT_W_SWCTRL].va, (unsigned long long)bt_wins[BT_W_UART].va);
	pandora_log_sync();
	if (bt_level == 7) {
		/* No BT rail votes, reset pulse, or TLMM programming in this mode.
		 * The QUP clocks and UART configuration are still changed. */
		if (bt_clocks()) {
			goto out;
		}
		bt_loopback();
		bt_report();
		goto out;
	}
	if (bt_level >= 2) {
		if (bt_power()) {
			bt_report();
			goto out;
		}
		if (bt_rf_clk()) {
			goto out;
		}
		if (bt_level < 3 || bt_level == 6 || bt_level >= 8) {
			/* level 6 also needs the SPMI windows mapped for its retries. */
			if (bt_en_pulse()) {
				goto out;
			}
		} else {
			/* Preserve the inherited pad configuration. Its output level and
			 * controller readiness have not been established by MODE alone. */
			printf("PANDORA BT: level 3 keeps the iBoot BT_RESET level\n");
			pandora_log_sync();
		}
		/* CTS in, RTS out LOW (the asserted "ready to receive" level Android
		 * shows), TX out high, RX in; the UART mux is function 1. */
		bt_pin(24, 1, 0, 2, -1);
		bt_pin(25, 1, 1, 2, 0);
		bt_pin(26, 1, 3, 2, 1);
		bt_pin(27, 1, 0, 2, -1);
		printf("PANDORA BT: RX pin level before the UART %#x (1 = WCN TX idle high)\n",
		    bt_rd(BT_W_UART, 3 * 0x1000u + 0x4) & BT_PIN_INPUT);
		pandora_log_sync();
		if (bt_level == 4) {
			/* Isolated historical wait experiment; cold retries must not
			 * silently inherit its 60-second delay. */
			unsigned waited = 0;
			uint32_t lvl = 0;
			for (unsigned i = 0; i < 600; i++) {
				lvl = bt_rd(BT_W_UART, 3 * 0x1000u + 0x4) & BT_PIN_INPUT;
				if (lvl) {
					break;
				}
				bt_sleep(100 * 1000);
				waited += 100;
			}
			printf("PANDORA BT: level 4 waited %u ms for the chip, RX %#x\n", waited, lvl);
			pandora_log_sync();
		}
		if (bt_clocks()) {
			bt_report();
			goto out;
		}
		if (bt_level >= 8) {
			int err = bt_uart_init(115200, false);
			if (!err) {
				bt_uart_dump();
				err = bt_peri_probe();
				bt_uart_dump();
			}
			int stop_err = bt_uart_stop();
			printf("PANDORA BT: PERI probe ended (error %d, UART stop %d)\n", err, stop_err);
			bt_report();
			goto out;
		}
		for (unsigned attempt = 1; attempt <= 3; attempt++) {
			if (bt_uart_init(bt_level == 5 ? 4000000 : 115200, false) == 0) {
				bt_uart_dump();
				if (bt_try_hci_reset()) {
					printf("PANDORA BT: controller answered on attempt %u\n", attempt);
					pandora_log_sync();
					break;
				}
			}
			if (bt_level < 6 || attempt == 3) {
				break;
			}
			/* hci_qca retries qca_power_on after a failed setup: drop the
			 * enable pad, wait, then bring it back up before retrying. */
			printf("PANDORA BT: attempt %u failed (RX %#x): power cycling\n", attempt,
			    bt_rd(BT_W_UART, 3 * 0x1000u + 0x4) & BT_PIN_INPUT);
			pandora_log_sync();
			if (bt_spmi_gpio(bt_bus0, bt_bus1, BT_SPMI_GPIO_PPID, 0x00, 1, "retry BT_RESET low")) {
				goto out;
			}
			bt_sleep(100 * 1000);
			if (bt_spmi_gpio(bt_bus0, bt_bus1, BT_SPMI_GPIO_PPID, 0x01, 1, "retry BT_RESET high")) {
				goto out;
			}
			bt_sleep(150 * 1000);
		}
	} else {
		bt_pmic_report();
		printf("PANDORA BT: about to read RCG\n");
		bt_rate = bt_rcg_rate();
	}
	printf("PANDORA BT: about to report\n");
	pandora_log_sync();
	bt_report();
out:
	pandora_log_sync();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_bt_init(void)
{
	(void)PE_parse_boot_argn("pandora_bt", &bt_level, sizeof(bt_level));
	if (!bt_level) {
		return;
	}
	if (bt_level < 1 || bt_level > 11) {
		printf("PANDORA BT: unsupported pandora_bt=%d (expected 1..11)\n", bt_level);
		return;
	}
	thread_t thread;
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
