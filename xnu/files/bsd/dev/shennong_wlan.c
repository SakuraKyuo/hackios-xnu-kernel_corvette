/*
 * The Wi-Fi of the Xiaomi 14 Pro (shennong, Snapdragon 8 Gen 3), from nothing and step by step. Every
 * address below belongs to this phone (its own device tree, /sys/firmware/fdt of its Android), and
 * nothing here is touched unless boot-arg wlan=N asks for it. wifi=1 is the POCO X5 Pro's
 * (redwood_wifi.c) and must stay off here.
 *
 * The radio is a WCN7850 ("kiwi" to Qualcomm's Android driver, PCI ID 17cb:1107), a chip of its own on
 * the SoC's first PCIe link, PCIe0 (qcom,pcie@1c00000), one lane pair, the root port at bus 0 and the
 * radio at bus 1. It is not a processor of the SoC to be loaded and started as redwood's WPSS is: once
 * it has power and the link is up it is an ordinary PCIe device, and its firmware is given to it over
 * the link (MHI, a later step). What comes before is the SoC's side and the chip's power:
 *
 *   power    the chip's power management unit makes the radio's own rails from eight of the phone's,
 *            all of them RPMh's (cnss "qcom,cnss-kiwi" in the device tree): vdd 0.94 V (smpi4), aon
 *            0.976 V (smpc2), dig 0.916 V (smpc3), rfa1 1.864 V (smpc6), rfa2 1.316 V (smpc1), io 1.8 V
 *            (ldob15), io12 1.2 V (ldoc3), ant-share 1.8 V (ldok6); and the 38.4 MHz reference clock
 *            "RF_CLK1" (RPMh's "clka1"). Then GPIO 16 (wlan-en) high; GPIO 18 (sw-ctrl) says the unit is
 *            up. GPIO 17 is Bluetooth's enable: not touched.
 *   PCIe0    the controller (PARF 0x1c00000, the DesignWare core's DBI 0x60000000, its iATU 0x60001000,
 *            the window for configuration requests 0x60100000, for memory 0x60300000..0x63ffffff), its
 *            PHY (QMP gen3x2, 0x1c06000; supplies ldoi1 0.88 V and ldoi3 1.2 V; the sequence of
 *            register writes is the device tree's own "qcom,phy-sequence"), and their clocks, resets and
 *            power domains in GCC (offsets of Linux's gcc-sm8650.c), the reference clock's gate in TCSR.
 *            Pins: GPIO 94 PERST# (low = the radio held in reset), 95 CLKREQ# (pcie0_clk_req_n), 96 WAKE#.
 *
 * Boot-arg wlan=N performs steps 1 to N and stops; a step says what it is about to touch before it
 * touches it and what it found after, reads come before writes, and a step that does not find what it
 * needs ends the sequence instead of going on blind.
 *
 *   1  reads   the pins, GCC's clocks and power domains of PCIe0, TCSR's reference clock gate, RPMh's
 *              addresses of the supplies (the command database); the controller and the PHY only if
 *              their clocks run (with them stopped a read there can end the kernel)
 *   2  writes  RPMh: the radio's supplies and its reference clock on (the switched-mode ones only
 *              switched on, at whatever voltage they have: they feed more than the radio; wlan_volt=1
 *              asks for the device tree's voltages too), then PERST# low and wlan-en high; reads sw-ctrl
 *              50 ms later
 *   3  writes  GCC: PCIe0's two power domains on, its clocks, the PHY's 100 MHz, the reference clock;
 *              the controller and the PHY reset; reads the controller after
 *   4  writes  the controller to root complex mode, the PHY's sequence, the pipe clock; waits for the
 *              PHY; the PCIe pins, PERST# released 100 ms after the radio got power, link training
 *              on; waits up to a second for the link
 *   5  reads   the radio's configuration space through the iATU: who it is, its BARs, its capabilities,
 *              the link's speed and width
 *   6  writes  BAR 0 (where the radio's registers go, in the memory window), the iATU's memory window,
 *              memory and bus mastering on; reads the radio's MHI and BHI registers: what MHI version
 *              it speaks and in which execution environment it is (PBL, before any firmware)
 *   7  reads   the application processors' SMMU (0x15000000): whether anything is set up for PCIe0's
 *              streams (0x1400 the root port, 0x1401 the radio: dt "iommu-map"), and the controller's
 *              translation of requesters to streams
 *   8  writes  the SMMU: PCIe0's streams (0x1400 with mask 0x7f: whatever the controller says, in its
 *              bypass, is ours) through a context of ours with page tables of ours -- a free match
 *              register and the lowest free context, as shennong_sound.c takes them for the audio DSP;
 *              the radio's addresses from 0x98000000 (dt: qcom,iommu-dma-addr-pool of cnss_pci0). Put
 *              back as found when the radio is unpowered
 *   9  writes  the first 512 KB of fw/amss20.bin (the phone's modem partition, image/kiwi; wlan_fw=NAME
 *              for another) into memory of ours, behind one of the radio's addresses, and gives the radio
 *              that address through BHI: the boot ROM (PBL) fetches that much and runs it as its second-
 *              stage loader (SBL), as Linux's MHI does with ath12k's "fbc_download"; waits for BHI's
 *              status and for the execution environment SBL; reads the SMMU's fault registers after
 *  10  writes  the MHI host (Linux's mhi_init_dev_ctxt(), mhi_init_mmio(), mhi_ready_state_transition()):
 *              contexts of 128 channels (IPCR's 20 and 21 filled in), of two event rings (32 and 256
 *              elements, ath12k's for WCN7850) and of the command ring, in memory the radio reaches;
 *              their addresses to MHI's registers, the event rings handed over, then M0; what the radio
 *              puts on its event rings is printed (and polled: no interrupts yet)
 *  11  writes  the whole fw/amss20.bin in pieces of 512 KB and a vector table of them through BHIe
 *              (mhi_download_amss_image()); waits for BHIe's status and for the execution environment
 *              AMSS, the radio's own firmware, and prints the events that follow (mission mode)
 *  12  writes  IPCR's channels (20 to the radio, 21 from it) started with MHI's START commands, 16
 *              buffers of 8 KB queued on 21; QRTR's HELLO from our node 1 (af_qrtr.c's qrtr_say_hello())
 *              on 20; prints every packet from the radio: its HELLO, its NEW_SERVERs -- WLFW's (QMI
 *              service 0x45) is what the next step talks to
 *  13  writes  QMI with WLFW from our port 0x4000 (ath12k's qmi.c): phy cap, indications registered,
 *              host cap (wlan_nm=N adds nm_modem N: 14 is ath12k's for non-Qualcomm hosts; none by
 *              default), the radio's memory request answered with memory of ours (64 KB aligned),
 *              "memory ready", target cap: chip, board id, firmware version
 *  14  writes  fw/regdb.bin (wlan_regdb=NAME) and the board's data, fw/bdwlan.eXX by the board
 *              id (wlan_bdf=NAME), through BDF_DOWNLOAD; fw/phy_ucode20.elf (M3) in memory of ours and
 *              its address through M3_INFO (wlan_m3=NAME for another); waits for "firmware ready"
 * wlan_link=1: step 13 is instead the link -- a thread of ours carrying QRTR over IPCR for the
 * kernel's QMI helpers (sys/hakios_qrtr.h, as redwood_wifi.c does over GLINK) -- and one question
 * to WLFW through them (phy cap): what ath12k's qmi.c will stand on.
 * wlan_driver=1 (with wlan=13): step 13 is the driver -- ath12k (wifi/lx12, tools/mk-ath12k.sh), linked
 * into the kernel as one object -- on the link: its qmi.c does steps 13 and 14 itself, then the copy
 * engines, HTC and WMI; wifi_scan=1 has it scan. Its memory is shown to the radio here
 * (shennong_wlan_radio_map()), its files come from fw/ (shennong_wlan_driver_file()).
 *
 * More boot-args: wlan_pause=MS, how long a step waits after announcing writes (default 2200: the
 * loader, watching from another core, notes the end of our log every two seconds, and after a silent
 * reset that is all there is; 0 for none); wlan_keep=1, the radio is left powered and the link up when
 * the steps are over (for the next steps; it is powered off at the run's end either way); wlan_tcs=N,
 * which of RPMh's active command sets is ours (default 2: shennong_synaptics.c uses 0).
 *
 * What is switched on and stays: RPMh's votes for the supplies (taking a vote back could take a rail
 * from something else that the boot chain voted for through the same channel). What is switched off at
 * the end, and on a panic's way back to the loader: PERST# low, wlan-en low -- the radio unpowered.
 *
 * Where things come from: the phone's device tree; Linux's pcie-qcom.c and pcie-designware.h (the
 * controller), phy-qcom-qmp-pcie.c (the PHY's order), gcc-sm8650.c, tcsrcc-sm8650.c, clk-rpmh.c,
 * pwrseq-qcom-wcn.c (WCN7850: 50 ms after power), mhi/common.h (MHI's registers), for the registers and
 * the order; shennong_synaptics.c for RPMh by hand.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/hakios.lx.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern lck_mtx_t hakios_smmu_lock;             /* (redwood_camera.c): one writer of the SMMU at a time */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static uint32_t last_step, pause_ms = 2200, keep, volt, our_tcs = 2, link_wanted_option, driver_wanted;
static bool leaving, radio_powered;
static uint64_t powered_at;     /* mach_absolute_time() when wlan-en went high */

extern void hakios_lx_keep(const char *text);     /* hakios.lx.c: the driver's lines, kept for the way out */
extern void hakios_lx_replay(void);
static int say(const char *format, ...) __printflike(1, 2);
static bool keep_lines;                 /* from the driver's start on, our lines are kept with its own (hakios_lx_keep()) */

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (keep_lines && !leaving && ml_get_interrupts_enabled() && strncmp(line, "  sent on channel", 17) != 0) {
		hakios_lx_keep("wlan: ");
		hakios_lx_keep(line);
	}
	loader_log_putc('w'); loader_log_putc('l'); loader_log_putc('a'); loader_log_putc('n'); loader_log_putc(':');
	loader_log_putc(' ');
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("wlan: %s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__builtin_arm_dsb(15);
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_us(uint32_t us)
{
	if (leaving) {
		spin_us(us);
	} else {
		delay_for_interval(us, NSEC_PER_USEC);
	}
}

static void
wait_ms(uint32_t ms)
{
	wait_us(ms * 1000);
}

static uint32_t
ms_since(uint64_t then)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - then, &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/* before a group of writes: what is about to happen, and a pause for the loader to note it */
static void
announce(const char *what)
{
	say("%s\n", what);
	if (pause_ms != 0) {
		wait_ms(pause_ms);
	}
}

/* a register, polled until (value & mask) == want; false (and what it read) after `us' microseconds */
static bool
wait_for(vm_offset_t address, uint32_t mask, uint32_t want, uint32_t us, uint32_t *last)
{
	uint32_t value = 0;

	for (uint32_t waited = 0;; waited += 10) {
		value = rd(address);
		if ((value & mask) == want || waited >= us) {
			break;
		}
		wait_us(10);
	}
	if (last != NULL) {
		*last = value;
	}
	return (value & mask) == want;
}

/* ---- the address map ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* pinctrl@f000000 + 0x100000 (pinctrl-sm8650.c) */
#define GCC_PHYS                0x00100000UL    /* clock-controller@100000 */
#define TCSR_CLKREF_PHYS        0x01ff1100UL    /* tcsr@1fc0000 + 0x31100: TCSR_PCIE_0_CLKREF_EN, bit 0 */
#define PARF_PHYS               0x01c00000UL
#define PARF_SIZE               0x3000UL
#define PHY_PHYS                0x01c06000UL
#define PHY_SIZE                0x2000UL
#define DBI_PHYS                0x60000000UL    /* the root port's own configuration space and the core's registers */
#define ELBI_OFFSET             0xf20UL
#define ATU_PHYS                0x60001000UL    /* "iatu": unrolled, outbound region i at i << 9 */
#define CONF_PHYS               0x60100000UL    /* "conf": where configuration requests go out */
#define MEM_PHYS                0x60300000UL    /* the memory window ("ranges") */
#define MEM_SIZE                0x03d00000UL
#define CMD_DB_PHYS             0x81c60000UL    /* reserved-memory aop_cmd_db_region */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL    /* apps_rsc "drv-2", the application processor's */
#define RSC_SIZE                0x10000UL

static vm_offset_t gcc_10000, gcc_52000, gcc_6b000, tcsr_clkref, parf, phy, dbi, atu, conf, bar0, rsc, cmd_db;
static vm_offset_t pin_page[4];         /* 16..19 in one mapping (16 KB); then 94, 95, 96 */

/* ---- pins ---------------------------------------------------------------------------------------------- */

#define PIN_WLAN_EN             16
#define PIN_BT_EN               17
#define PIN_SW_CTRL             18
#define PIN_WLAN_SW_CTRL        19
#define PIN_PERST               94
#define PIN_CLKREQ              95
#define PIN_WAKE                96
/* [1:0] pull (0 none, 1 down, 2 keeper, 3 up), [5:2] function, [8:6] drive (mA / 2 - 1), [9] output */
#define CFG_PULL_DOWN           1
#define CFG_PULL_UP             3
#define CFG_FUNC(f)             ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)
#define CFG_EGPIO_PRESENT       (1u << 11)      /* pinctrl-msm.c on SM8650: a pin another subsystem may own ... */
#define CFG_EGPIO_ENABLE        (1u << 12)      /* ... and is APPS's only while this is set */
#define IO_IN                   1u
#define IO_OUT                  2u

static vm_offset_t
pin_cfg(unsigned pin)
{
	switch (pin) {
	case 16: case 17: case 18: case 19:
		return pin_page[0] + 0x1000UL * (pin - 16);
	case PIN_PERST:
		return pin_page[1];
	case PIN_CLKREQ:
		return pin_page[2];
	default:
		return pin_page[3];
	}
}

static void
pin_set(unsigned pin, uint32_t cfg, int out)
{
	vm_offset_t c = pin_cfg(pin);

	if (rd(c) & CFG_EGPIO_PRESENT) {
		cfg |= CFG_EGPIO_ENABLE;
	}
	if (out >= 0) {
		wr(c + 4, out ? IO_OUT : 0);    /* the level first, so that the pin does not glitch */
	}
	wr(c, cfg);
}

static void
pin_show(unsigned pin, const char *name)
{
	uint32_t cfg = rd(pin_cfg(pin)), io = rd(pin_cfg(pin) + 4);

	say("  gpio%u %-10s cfg %#06x (function %u, pull %u, %u mA, %s%s), reads %u, drives %u\n", pin, name, cfg,
	    cfg >> 2 & 0xf, cfg & 3, ((cfg >> 6 & 7) + 1) * 2, cfg & CFG_OUTPUT ? "output" : "input",
	    cfg & CFG_EGPIO_PRESENT ? (cfg & CFG_EGPIO_ENABLE ? ", eGPIO: ours" : ", eGPIO: not ours") : "", io & IO_IN,
	    io >> 1 & 1);
}

static void
pins_show(void)
{
	pin_show(PIN_WLAN_EN, "wlan-en");
	pin_show(PIN_BT_EN, "bt-en");
	pin_show(PIN_SW_CTRL, "sw-ctrl");
	pin_show(PIN_WLAN_SW_CTRL, "wl-sw-ctrl");
	pin_show(PIN_PERST, "PERST#");
	pin_show(PIN_CLKREQ, "CLKREQ#");
	pin_show(PIN_WAKE, "WAKE#");
}

/* the radio's power off: PERST# low first (the link goes), then wlan-en low */
static void
radio_off(void)
{
	pin_set(PIN_PERST, CFG_DRIVE_MA(2) | CFG_PULL_DOWN | CFG_OUTPUT, 0);
	pin_set(PIN_WLAN_EN, CFG_DRIVE_MA(2) | CFG_PULL_DOWN | CFG_OUTPUT, 0);
	radio_powered = false;
}

/* For shennong_bt.c, the same chip's other half */
bool
shennong_wlan_powered(void)
{
	return radio_powered;
}

/* ---- RPMh: the supplies and the reference clock ---------------------------------------------------- */

#define RSC_TCS_BASE            0xd00           /* qcom,tcs-offset; TCS 0..2 are "active" ones (tcs-config: ACTIVE 3) */
#define RSC_TCS_STRIDE          672             /* rpmh-rsc.c RSC_DRV_TCS_OFFSET */
#define TCS_IRQ_ENABLE          0x00            /* these three are the DRV's, one bit per command set */
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

/* the command set's own registers move between RSC versions (rpmh-rsc.c rpmh_rsc_reg_offset_ver_2_7/3_0) */
static struct {
	uint32_t wait_for_completion, control, status, cmd_enable, msgid, addr, data, cmd_stride;
} tcs_reg;
#define DRV(reg)                (rsc + RSC_TCS_BASE + (reg))
#define TCS(reg)                (rsc + RSC_TCS_BASE + RSC_TCS_STRIDE * our_tcs + (reg))

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 ||
		    sizeof(*db) + slave->header_offset + slave->count * sizeof(*entry) > CMD_DB_SIZE) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(vm_offset_t address, uint32_t mask, uint32_t value, const char *what)
{
	uint32_t last;

	if (wait_for(address, mask, value, 200000, &last)) {
		return true;
	}
	say("RPMh: timeout waiting for %s (%#x)\n", what, last);
	return false;
}

/* Up to three register writes to RPMh through our command set, and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t bit = 1u << our_tcs, irq_enable;
	bool done;

	if (!rpmh_wait(TCS(tcs_reg.status), 1, 1, "our command set to be idle")) {
		return false;
	}
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(DRV(TCS_IRQ_CLEAR), bit);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(tcs_reg.msgid + tcs_reg.cmd_stride * i), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(tcs_reg.addr + tcs_reg.cmd_stride * i), address[i]);
		wr(TCS(tcs_reg.data + tcs_reg.cmd_stride * i), data[i]);
	}
	wr(TCS(tcs_reg.wait_for_completion), (1u << count) - 1);
	wr(TCS(tcs_reg.cmd_enable), (1u << count) - 1);
	irq_enable = rd(DRV(TCS_IRQ_ENABLE));
	wr(DRV(TCS_IRQ_ENABLE), irq_enable | bit);      /* the status bit follows the enable; the interrupt stays masked */
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(DRV(TCS_IRQ_STATUS), bit, bit, "the request");
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.cmd_enable), 0);
	wr(TCS(tcs_reg.wait_for_completion), 0);
	wr(DRV(TCS_IRQ_CLEAR), bit);
	wr(DRV(TCS_IRQ_ENABLE), rd(DRV(TCS_IRQ_ENABLE)) & ~bit);
	return done;
}

/*
 * The radio's supplies, in the order of the device tree's cnss node, and the PHY's two. mv 0: a
 * switched-mode supply that more than the radio hangs on -- switched on, its voltage left alone
 * unless wlan_volt=1 (then `tree_mv', the device tree's minimum, is asked for).
 */
static const struct supply {
	const char *    name;           /* RPMh's, in the command database */
	const char *    what;
	uint32_t        mv;             /* asked for (with the high-power mode, LDOs) */
	uint32_t        tree_mv;
} supplies[] = {
	{ "ldok6",  "ant-share 1.8 V",   1800, 1800 },
	{ "smpc1",  "rfa2 1.316 V",         0, 1316 },
	{ "smpc6",  "rfa1 1.864 V",         0, 1864 },
	{ "smpc3",  "dig 0.916 V",          0,  916 },
	{ "smpc2",  "aon 0.976 V",          0,  976 },
	{ "smpi4",  "vdd 0.94 V",           0,  940 },
	{ "ldoc3",  "io12 1.2 V",        1200, 1200 },
	{ "ldob15", "io 1.8 V",          1800, 1800 },
	{ "ldoi1",  "PCIe0 PHY 0.88 V",   880,  880 },
	{ "ldoi3",  "PCIe0 PHY 1.2 V",   1200, 1200 },
};
/* Once the supplies are on nothing else here writes through our command set: one vote, for shennong_bt.c. */
bool
shennong_wlan_rpmh_bcm(const char *name, uint32_t value)
{
	uint32_t address = rpmh_address(name);

	if (address == 0) {
		say("RPMh has no %s\n", name);
		return false;
	}
	return rpmh_write(&address, &value, 1);
}

#define RF_CLOCK                "clka1"         /* RPMH_RF_CLK1 (clk-rpmh.c sm8650): the radio's "rf_clk" */

static bool
rpmh_setup(void)
{
	uint32_t id = rd(rsc), major = id >> 16 & 0xff, minor = id >> 8 & 0xff;

	if (major >= 3) {
		tcs_reg.wait_for_completion = 0x20; tcs_reg.control = 0x24; tcs_reg.status = 0x28; tcs_reg.cmd_enable = 0x2c;
		tcs_reg.msgid = 0x34; tcs_reg.addr = 0x38; tcs_reg.data = 0x3c; tcs_reg.cmd_stride = 24;
	} else {
		tcs_reg.wait_for_completion = 0x10; tcs_reg.control = 0x14; tcs_reg.status = 0x18; tcs_reg.cmd_enable = 0x1c;
		tcs_reg.msgid = 0x30; tcs_reg.addr = 0x34; tcs_reg.data = 0x38; tcs_reg.cmd_stride = 20;
	}
	say("RPMh %u.%u; our command set %u: status %#x, control %#x; the DRV's irq enable %#x status %#x\n", major, minor,
	    our_tcs, rd(TCS(tcs_reg.status)), rd(TCS(tcs_reg.control)), rd(DRV(TCS_IRQ_ENABLE)), rd(DRV(TCS_IRQ_STATUS)));
	return rpmh_address("ldob15") != 0;
}

/* step 1's part: where RPMh has them */
static bool
supplies_show(void)
{
	bool all = true;

	if (rpmh_address("ldob15") == 0) {
		const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;

		say("no command database at %#lx (magic %02x %02x %02x %02x)\n", CMD_DB_PHYS, db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return false;
	}
	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		uint32_t a = rpmh_address(supplies[i].name);

		say("  %-6s %-17s RPMh %#07x%s\n", supplies[i].name, supplies[i].what, a,
		    a == 0 ? ": NOT in the command database" : (a >> 16 & 0xf) != 4 ? ": not a regulator's address" : "");
		all = all && a != 0 && (a >> 16 & 0xf) == 4;
	}
	say("  %-6s %-17s RPMh %#07x%s\n", RF_CLOCK, "RF_CLK1 (rf_clk)", rpmh_address(RF_CLOCK),
	    rpmh_address(RF_CLOCK) == 0 ? ": NOT in the command database" : "");
	return all && rpmh_address(RF_CLOCK) != 0;
}

static bool
supplies_on(void)
{
	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		const struct supply *s = &supplies[i];
		uint32_t base = rpmh_address(s->name), address[3], data[3], n = 0, mv = s->mv != 0 ? s->mv : volt ? s->tree_mv : 0;
		bool ldo = s->name[0] == 'l';

		if (mv != 0) {
			address[n] = base + VRM_VOLTAGE; data[n++] = mv;
		}
		address[n] = base + VRM_ENABLE; data[n++] = 1;
		if (ldo) {
			address[n] = base + VRM_MODE; data[n++] = VRM_MODE_LDO_HPM;
		}
		if (!rpmh_write(address, data, n)) {
			say("RPMh did not take %s (%s)\n", s->name, s->what);
			return false;
		}
		if (mv != 0) {
			say("  %-6s on, %u mV%s\n", s->name, mv, ldo ? ", high-power mode" : "");
		} else {
			say("  %-6s on (its voltage as it was)\n", s->name);
		}
	}
	{
		uint32_t address[1] = { rpmh_address(RF_CLOCK) + VRM_ENABLE }, data[1] = { 1 };

		if (!rpmh_write(address, data, 1)) {
			say("RPMh did not take %s\n", RF_CLOCK);
			return false;
		}
		say("  %-6s on (the radio's 38.4 MHz)\n", RF_CLOCK);
	}
	return true;
}

/* ---- GCC: PCIe0's power domains, clocks and resets ------------------------------------------------- */

/* an offset of GCC's to the address of one of the three pieces we map */
static vm_offset_t
gccr(uint32_t offset)
{
	if (offset >= 0x6b000 && offset < 0x6d000) {
		return gcc_6b000 + (offset - 0x6b000);
	}
	if (offset >= 0x52000 && offset < 0x53000) {
		return gcc_52000 + (offset - 0x52000);
	}
	return gcc_10000 + (offset - 0x10000);
}

#define GCC_VOTE_0              0x52000         /* APPS's votes for branch clocks */
#define GCC_VOTE_2              0x52008
#define GCC_COLLAPSE_VOTE       0x5214c         /* bit 0 PCIe0's domain collapsed, bit 3 its PHY's */
#define GCC_PCIE_0_BCR          0x6b000
#define GCC_PCIE_0_GDSCR        0x6b004
#define GCC_PCIE_0_PHY_GDSCR    0x6c000
#define GCC_PCIE_0_PHY_BCR      0x6c01c
#define GCC_PCIE_0_PIPE_MUX     0x6b070         /* [1:0]: 0 the PHY's pipe clock, 2 the crystal */
#define GCC_PCIE_0_AUX_RCG      0x6b074         /* 19.2 MHz from the crystal */
#define GCC_PCIE_0_RCHNG_RCG    0x6b058         /* 100 MHz: GPLL0's even output (300 MHz) / 3 */
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_RETAIN_FF          (1u << 11)
#define GDSC_PWR_ON             (1u << 31)
#define GDSC_CFG_UP_COMPLETE    (1u << 16)      /* in the register after (CFG_GDSCR) */
#define CBCR_ENABLE             (1u << 0)
#define CBCR_HW_CTL             (1u << 1)       /* hardware clock gating on */
#define CBCR_OFF                (1u << 31)
#define RCG_UPDATE              (1u << 0)
#define RCG_ROOT_OFF            (1u << 31)

static const struct branch {
	const char *    name;
	uint32_t        cbcr;
	uint32_t        vote;           /* the vote register, and the bit in it */
	uint32_t        bit;
	bool            halt_check;     /* false: its CBCR does not say (Linux's BRANCH_HALT_SKIP) */
} branches[] = {
	{ "cnoc_pcie_sf_axi",    0x10058, GCC_VOTE_2,  6, true  },
	{ "aggre_noc_pcie_axi",  0x10064, GCC_VOTE_0, 12, false },
	{ "ddrss_pcie_sf_qtb",   0x10074, GCC_VOTE_0, 19, false },
	{ "pcie_0_cfg_ahb",      0x6b038, GCC_VOTE_2,  2, true  },
	{ "pcie_0_aux",          0x6b03c, GCC_VOTE_2,  3, true  },
	{ "pcie_0_mstr_axi",     0x6b02c, GCC_VOTE_2,  1, false },
	{ "pcie_0_slv_axi",      0x6b020, GCC_VOTE_2,  0, true  },
	{ "pcie_0_slv_q2a_axi",  0x6b01c, GCC_VOTE_2,  5, true  },
	{ "pcie_0_phy_rchng",    0x6b054, GCC_VOTE_0, 22, true  },
};
#define PIPE_CBCR               0x6b048         /* gcc_pcie_0_pipe_clk: GCC_VOTE_2 bit 4, runs only once the PHY does */
#define PIPE_BIT                4

static bool
gcc_show(void)
{
	uint32_t collapse = rd(gccr(GCC_COLLAPSE_VOTE)), core = rd(gccr(GCC_PCIE_0_GDSCR)), phy_d = rd(gccr(GCC_PCIE_0_PHY_GDSCR));

	say("  power domains: votes to collapse %#x (PCIe0 %s, its PHY %s); PCIe0 GDSCR %#x cfg %#x (%s), PHY GDSCR %#x cfg %#x (%s)\n",
	    collapse, collapse & 1 ? "collapsed" : "not", collapse & 8 ? "collapsed" : "not", core,
	    rd(gccr(GCC_PCIE_0_GDSCR) + 4), core & GDSC_PWR_ON ? "on" : "off", phy_d, rd(gccr(GCC_PCIE_0_PHY_GDSCR) + 4),
	    phy_d & GDSC_PWR_ON ? "on" : "off");
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		const struct branch *b = &branches[i];
		uint32_t cbcr = rd(gccr(b->cbcr));

		say("  %-20s CBCR %#010x (%s%s), voted %s\n", b->name, cbcr, cbcr & CBCR_OFF ? "off" : "running",
		    cbcr & CBCR_HW_CTL ? ", gated by the hardware" : "",
		    rd(gccr(b->vote)) >> b->bit & 1 ? "yes" : "no");
	}
	say("  %-20s CBCR %#010x, voted %s; source (mux) %u\n", "pcie_0_pipe", rd(gccr(PIPE_CBCR)),
	    rd(gccr(GCC_VOTE_2)) >> PIPE_BIT & 1 ? "yes" : "no", rd(gccr(GCC_PCIE_0_PIPE_MUX)) & 3);
	say("  aux RCG cmd %#x cfg %#x; rchng RCG cmd %#x cfg %#x; resets: PCIe0 %#x, its PHY %#x; TCSR clkref %#x\n",
	    rd(gccr(GCC_PCIE_0_AUX_RCG)), rd(gccr(GCC_PCIE_0_AUX_RCG) + 4), rd(gccr(GCC_PCIE_0_RCHNG_RCG)),
	    rd(gccr(GCC_PCIE_0_RCHNG_RCG) + 4), rd(gccr(GCC_PCIE_0_BCR)), rd(gccr(GCC_PCIE_0_PHY_BCR)), rd(tcsr_clkref));
	/* the controller's and the PHY's registers answer only with their domains on and the configuration clocks voted for */
	return (core & GDSC_PWR_ON) && (phy_d & GDSC_PWR_ON) && (rd(gccr(GCC_VOTE_2)) & (1u << 2 | 1u << 6)) == (1u << 2 | 1u << 6);
}

static bool
gdsc_on(uint32_t gdscr, uint32_t collapse_bit, const char *name)
{
	uint32_t last;

	wr(gccr(GCC_COLLAPSE_VOTE), rd(gccr(GCC_COLLAPSE_VOTE)) & ~collapse_bit);
	if (!wait_for(gccr(gdscr) + 4, GDSC_CFG_UP_COMPLETE, GDSC_CFG_UP_COMPLETE, 5000, &last) ||
	    !wait_for(gccr(gdscr), GDSC_PWR_ON, GDSC_PWR_ON, 5000, NULL)) {
		say("%s's power domain did not come on: GDSCR %#x, cfg %#x\n", name, rd(gccr(gdscr)), last);
		return false;
	}
	wr(gccr(gdscr), rd(gccr(gdscr)) | GDSC_RETAIN_FF);
	say("  %s's power domain on: GDSCR %#x\n", name, rd(gccr(gdscr)));
	return true;
}

static bool
branch_on(const struct branch *b)
{
	uint32_t last;

	wr(gccr(b->vote), rd(gccr(b->vote)) | 1u << b->bit);
	/* with hardware gating on, CLK_OFF says only that nobody uses the clock right now (clk-branch.c) */
	if (b->halt_check && !(rd(gccr(b->cbcr)) & CBCR_HW_CTL) && !wait_for(gccr(b->cbcr), CBCR_OFF, 0, 2000, &last)) {
		say("the clock %s does not run: CBCR %#x\n", b->name, last);
		return false;
	}
	return true;
}

/* a root clock generator to (source, divider field), and the update taken */
static bool
rcg_set(uint32_t rcg, uint32_t cfg, const char *name)
{
	uint32_t last;

	if ((rd(gccr(rcg) + 4) & 0x71f) == cfg) {
		return true;
	}
	wr(gccr(rcg) + 4, (rd(gccr(rcg) + 4) & ~0x371fu) | cfg);
	wr(gccr(rcg), rd(gccr(rcg)) | RCG_UPDATE);
	if (!wait_for(gccr(rcg), RCG_UPDATE, 0, 2000, &last)) {
		say("the clock source %s did not take its new setting: cmd %#x\n", name, last);
		return false;
	}
	return true;
}

static void
bcr_pulse(uint32_t bcr)
{
	wr(gccr(bcr), 1);
	wait_us(1000);
	wr(gccr(bcr), 0);
	wait_us(1000);
}

/* step 3 */
static bool
pcie_power_on(void)
{
	if (!gdsc_on(GCC_PCIE_0_GDSCR, 1u << 0, "PCIe0") || !gdsc_on(GCC_PCIE_0_PHY_GDSCR, 1u << 3, "PCIe0 PHY")) {
		return false;
	}
	/* the pipe clock from the crystal until the PHY makes its own */
	wr(gccr(GCC_PCIE_0_PIPE_MUX), (rd(gccr(GCC_PCIE_0_PIPE_MUX)) & ~3u) | 2);
	if (!rcg_set(GCC_PCIE_0_AUX_RCG, 0x001, "aux (19.2 MHz)") || !rcg_set(GCC_PCIE_0_RCHNG_RCG, 0x605, "rchng (100 MHz)")) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (!branch_on(&branches[i])) {
			return false;
		}
	}
	say("  the clocks run\n");
	wr(tcsr_clkref, rd(tcsr_clkref) | 1);
	wait_us(100);
	say("  the reference clock's gate open: %#x\n", rd(tcsr_clkref));
	bcr_pulse(GCC_PCIE_0_BCR);
	bcr_pulse(GCC_PCIE_0_PHY_BCR);
	say("  the controller and the PHY reset\n");
	return true;
}

/* ---- the controller and the PHY -------------------------------------------------------------------- */

#define PARF_SYS_CTRL           0x00
#define PARF_PM_CTRL            0x20
#define PARF_PHY_CTRL           0x40
#define PARF_MHI_CLOCK_RESET    0x174
#define PARF_AXI_MSTR_WR_HALT   0x1a8
#define PARF_LTSSM              0x1b0
#define PARF_INT_ALL_STATUS     0x224
#define PARF_DBI_BASE           0x350           /* _V2, and its high word after */
#define PARF_SLV_ADDR_SIZE      0x358           /* _V2, and its high word after */
#define PARF_ATU_BASE           0x634
#define PARF_DEVICE_TYPE        0x1000
#define DEVICE_TYPE_RC          4
#define PHY_TEST_PWR_DOWN       (1u << 0)
#define MAC_PHY_POWERDOWN_MUX   (1u << 29)
#define MHI_CLOCK_BYPASS        (1u << 4)
#define REQ_NOT_ENTR_L1         (1u << 5)
#define AXI_HALT_EN             (1u << 31)
#define LTSSM_EN                (1u << 8)
#define INT_LINK_UP             (1u << 13)
#define ELBI_SYS_STTS           0x08            /* bit 10: the link is up (XMLH_LINK_UP) */

#define DBI_PORT_DEBUG0         0x728           /* [5:0] the LTSSM's state: 0x11 = L0 */
#define DBI_PORT_DEBUG1         0x72c           /* bit 4 link up, bit 29 in training */
#define ATU_CTRL1               0x00
#define ATU_CTRL2               0x04
#define ATU_LOWER_BASE          0x08
#define ATU_UPPER_BASE          0x0c
#define ATU_LIMIT               0x10
#define ATU_LOWER_TARGET        0x14
#define ATU_UPPER_TARGET        0x18
#define ATU_TYPE_MEM            0
#define ATU_TYPE_CFG0           4
#define ATU_ENABLE              (1u << 31)

#define PHY_SW_RESET            0x200           /* the PCS's registers (QMP v6) */
#define PHY_PCS_STATUS1         0x214           /* bit 6: PHYSTATUS, 0 = the PHY is ready ("qcom,phy-status-offset/-bit") */
#define PHY_POWER_DOWN          0x240
#define PHY_START               0x244

/* "qcom,phy-sequence" of the device tree's pcie@1c00000, in order: (offset, value); every delay is 0 */
static const struct {
	uint16_t offset;
	uint8_t value;
} phy_sequence[] = {
	{ 0x240, 0x3 }, { 0xc0, 0x1 }, { 0xcc, 0x62 }, { 0xd0, 0x2 }, { 0x60, 0xf8 }, { 0x64, 0x1 }, { 0x0, 0x93 },
	{ 0x4, 0x1 }, { 0xe0, 0x90 }, { 0xe4, 0x82 }, { 0xf4, 0x7 }, { 0x70, 0x2 }, { 0x10, 0x2 }, { 0x74, 0x16 },
	{ 0x14, 0x16 }, { 0x78, 0x36 }, { 0x18, 0x36 }, { 0x110, 0x8 }, { 0xbc, 0xa }, { 0x120, 0x42 }, { 0x80, 0x4 },
	{ 0x84, 0xd }, { 0x20, 0xa }, { 0x24, 0x1a }, { 0x88, 0x41 }, { 0x28, 0x34 }, { 0x90, 0xab }, { 0x94, 0xaa },
	{ 0x98, 0x1 }, { 0x30, 0x55 }, { 0x34, 0x55 }, { 0x38, 0x1 }, { 0x140, 0x14 }, { 0x164, 0x34 }, { 0x3c, 0x1 },
	{ 0x1c, 0x4 }, { 0x174, 0x16 }, { 0x1bc, 0xf }, { 0x170, 0xa0 }, { 0x11a4, 0x38 }, { 0x10dc, 0x11 }, { 0x1160, 0xbf },
	{ 0x1164, 0xbf }, { 0x1168, 0xb7 }, { 0x116c, 0xea }, { 0x115c, 0x3f }, { 0x1174, 0x5c }, { 0x1178, 0x9c },
	{ 0x117c, 0x1a }, { 0x1180, 0x89 }, { 0x1170, 0xdc }, { 0x1188, 0x94 }, { 0x118c, 0x5b }, { 0x1190, 0x1a },
	{ 0x1194, 0x89 }, { 0x10cc, 0x0 }, { 0x1008, 0x9 }, { 0x1014, 0x5 }, { 0x104c, 0x8 }, { 0x1050, 0x8 },
	{ 0x10d8, 0xf }, { 0x1118, 0x1c }, { 0x10f8, 0x7 }, { 0x11f8, 0x8 }, { 0x1600, 0x0 }, { 0xe84, 0x15 },
	{ 0xe90, 0x3f }, { 0xee4, 0x2 }, { 0xe40, 0x9 }, { 0xe3c, 0x15 }, { 0x19a4, 0x38 }, { 0x18dc, 0x11 },
	{ 0x1960, 0xbf }, { 0x1964, 0xbf }, { 0x1968, 0xb7 }, { 0x196c, 0xea }, { 0x195c, 0x3f }, { 0x1974, 0x5c },
	{ 0x1978, 0x9c }, { 0x197c, 0x1a }, { 0x1980, 0x89 }, { 0x1970, 0xdc }, { 0x1988, 0x94 }, { 0x198c, 0x5b },
	{ 0x1990, 0x1a }, { 0x1994, 0x89 }, { 0x18cc, 0x0 }, { 0x1808, 0x9 }, { 0x1814, 0x5 }, { 0x184c, 0x8 },
	{ 0x1850, 0x8 }, { 0x18d8, 0xf }, { 0x1918, 0x1c }, { 0x18f8, 0x7 }, { 0x19f8, 0x8 }, { 0x1684, 0x15 },
	{ 0x1690, 0x3f }, { 0x16e4, 0x2 }, { 0x1640, 0x9 }, { 0x163c, 0x15 }, { 0x2dc, 0x5 }, { 0x388, 0x77 }, { 0x398, 0xb },
	{ 0x6a4, 0x1e }, { 0x6f4, 0x27 }, { 0x3e0, 0xf }, { 0x60c, 0x1d }, { 0x614, 0x7 }, { 0x620, 0xc1 }, { 0x694, 0x0 },
	{ 0x3d0, 0x8c }, { 0x368, 0x17 }, { 0x370, 0x2e }, { 0x200, 0x0 }, { 0x244, 0x3 },
};

static void
pcie_show(const char *when)
{
	uint32_t ltssm = rd(parf + PARF_LTSSM);

	say("controller %s: device type %u, PHY control %#x, sys control %#x, LTSSM %#x (training %s), int status %#x; "
	    "DBI base %#x, ATU base %#x; PHY: status %#x, power-down %#x, start %#x, reset %#x\n", when,
	    rd(parf + PARF_DEVICE_TYPE), rd(parf + PARF_PHY_CTRL), rd(parf + PARF_SYS_CTRL), ltssm,
	    ltssm & LTSSM_EN ? "on" : "off", rd(parf + PARF_INT_ALL_STATUS), rd(parf + PARF_DBI_BASE), rd(parf + PARF_ATU_BASE),
	    rd(phy + PHY_PCS_STATUS1), rd(phy + PHY_POWER_DOWN), rd(phy + PHY_START), rd(phy + PHY_SW_RESET));
}

static void
link_show(const char *when)
{
	say("link %s: LTSSM state %#x, debug1 %#x (%s%s), ELBI status %#x, PARF int status %#x\n", when,
	    rd(dbi + DBI_PORT_DEBUG0) & 0x3f, rd(dbi + DBI_PORT_DEBUG1),
	    rd(dbi + DBI_PORT_DEBUG1) & (1u << 4) ? "up" : "down", rd(dbi + DBI_PORT_DEBUG1) & (1u << 29) ? ", in training" : "",
	    rd(dbi + ELBI_OFFSET + ELBI_SYS_STTS), rd(parf + PARF_INT_ALL_STATUS));
}

/* an outbound region of the iATU */
static bool
atu_region(unsigned index, uint32_t type, uint64_t base, uint32_t size, uint64_t target)
{
	vm_offset_t r = atu + ((vm_offset_t)index << 9);
	uint32_t last;

	wr(r + ATU_LOWER_BASE, (uint32_t)base);
	wr(r + ATU_UPPER_BASE, (uint32_t)(base >> 32));
	wr(r + ATU_LIMIT, (uint32_t)(base + size - 1));
	wr(r + ATU_LOWER_TARGET, (uint32_t)target);
	wr(r + ATU_UPPER_TARGET, (uint32_t)(target >> 32));
	wr(r + ATU_CTRL1, type);
	wr(r + ATU_CTRL2, ATU_ENABLE);
	if (!wait_for(r + ATU_CTRL2, ATU_ENABLE, ATU_ENABLE, 1000, &last)) {
		say("the iATU's region %u stays off (%#x)\n", index, last);
		return false;
	}
	return true;
}

/* step 4 */
static bool
pcie_link_up(void)
{
	uint32_t last, waited;

	announce("step 4: the controller to root complex mode; the PHY's sequence; PERST# held low, CLKREQ# to the controller");
	pin_set(PIN_PERST, CFG_DRIVE_MA(2) | CFG_PULL_DOWN | CFG_OUTPUT, 0);
	pin_set(PIN_CLKREQ, CFG_FUNC(1) | CFG_DRIVE_MA(2) | CFG_PULL_UP, -1);
	pin_set(PIN_WAKE, CFG_DRIVE_MA(2) | CFG_PULL_UP, -1);
	wr(parf + PARF_DEVICE_TYPE, DEVICE_TYPE_RC);
	wr(parf + PARF_PHY_CTRL, rd(parf + PARF_PHY_CTRL) & ~PHY_TEST_PWR_DOWN);
	wr(parf + PARF_DBI_BASE, (uint32_t)DBI_PHYS);
	wr(parf + PARF_DBI_BASE + 4, 0);
	wr(parf + PARF_ATU_BASE, (uint32_t)ATU_PHYS);
	wr(parf + PARF_ATU_BASE + 4, 0);
	wr(parf + PARF_SLV_ADDR_SIZE, 0);
	wr(parf + PARF_SLV_ADDR_SIZE + 4, 0x80000000u);
	wr(parf + PARF_SYS_CTRL, rd(parf + PARF_SYS_CTRL) & ~MAC_PHY_POWERDOWN_MUX);
	wr(parf + PARF_MHI_CLOCK_RESET, rd(parf + PARF_MHI_CLOCK_RESET) | MHI_CLOCK_BYPASS);
	wr(parf + PARF_PM_CTRL, rd(parf + PARF_PM_CTRL) & ~REQ_NOT_ENTR_L1);
	wr(parf + PARF_AXI_MSTR_WR_HALT, rd(parf + PARF_AXI_MSTR_WR_HALT) | AXI_HALT_EN);
	pcie_show("in root complex mode");

	/* the sequence but its last two writes (the PHY out of reset, started): those after the pipe clock */
	for (unsigned i = 0; i + 2 < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(phy + phy_sequence[i].offset, phy_sequence[i].value);
	}
	say("  the PHY's %lu writes done; the pipe clock to the PHY's\n", sizeof(phy_sequence) / sizeof(phy_sequence[0]) - 2);
	wr(gccr(GCC_PCIE_0_PIPE_MUX), rd(gccr(GCC_PCIE_0_PIPE_MUX)) & ~3u);
	wr(gccr(GCC_VOTE_2), rd(gccr(GCC_VOTE_2)) | 1u << PIPE_BIT);
	for (unsigned i = sizeof(phy_sequence) / sizeof(phy_sequence[0]) - 2; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(phy + phy_sequence[i].offset, phy_sequence[i].value);
	}
	if (!wait_for(phy + PHY_PCS_STATUS1, 1u << 6, 0, 20000, &last)) {
		say("the PHY is not ready after 20 ms: status %#x\n", last);
		pcie_show("with the PHY not ready");
		return false;
	}
	say("  the PHY is ready (status %#x); the pipe clock: CBCR %#x\n", last, rd(gccr(PIPE_CBCR)));

	/* the root port: buses 0 / 1..1, memory and bus mastering; and its own link capabilities */
	say("  root port %04x:%04x, class %#x\n", rd(dbi) & 0xffff, rd(dbi) >> 16, rd(dbi + 8) >> 8);
	wr(dbi + 0x18, 0x00010100);
	wr(dbi + 0x04, rd(dbi + 0x04) | 0x6);

	/* PERST# held 100 ms after the radio had power, at least (pcie-qcom.c, PCIe CEM) */
	waited = ms_since(powered_at);
	if (waited < 100) {
		wait_ms(100 - waited);
	}
	announce("step 4: PERST# released, link training on");
	pin_set(PIN_PERST, CFG_DRIVE_MA(2) | CFG_PULL_DOWN | CFG_OUTPUT, 1);
	wait_us(1000);
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) | LTSSM_EN);
	for (waited = 0; waited < 1000; waited += 10) {
		if ((rd(dbi + DBI_PORT_DEBUG1) & (1u << 4 | 1u << 29)) == 1u << 4) {
			break;
		}
		wait_ms(10);
	}
	link_show(waited < 1000 ? "up" : "NOT up after a second");
	if (waited >= 1000) {
		pins_show();
		return false;
	}
	say("  link up after %u ms\n", waited);
	wait_ms(100);           /* before the first configuration request (PCIe: 100 ms after the link) */
	return atu_region(0, ATU_TYPE_CFG0, CONF_PHYS, 0x100000, 1u << 24);    /* bus 1, device 0, function 0 */
}

/* ---- the radio, as a PCIe device ------------------------------------------------------------------ */

static uint64_t bar0_size;

/* step 5 */
static bool
radio_show(void)
{
	uint32_t id = rd(conf), cap, bar_lo = rd(conf + 0x10), bar_hi = rd(conf + 0x14);

	say("step 5: the radio: %04x:%04x, revision %#x, class %#08x, header type %#x, status %#x, BAR0 %#x %#x\n",
	    id & 0xffff, id >> 16, rd(conf + 8) & 0xff, rd(conf + 8) >> 8, rd(conf + 0x0c) >> 16 & 0xff, rd(conf + 4) >> 16,
	    bar_lo, bar_hi);
	if (id == 0xffffffff || id == 0) {
		say("nobody answers at bus 1\n");
		return false;
	}
	if (id != 0x110717cb) {
		say("this is not a WCN7850 (17cb:1107): stopping here\n");
		return false;
	}
	for (cap = rd(conf + 0x34) & 0xfc; cap != 0 && cap < 0x100; cap = rd(conf + cap) >> 8 & 0xfc) {
		uint32_t head = rd(conf + cap);

		if ((head & 0xff) == 0x10) {    /* PCI Express */
			uint32_t link_cap = rd(conf + cap + 0x0c), link_status = rd(conf + cap + 0x10) >> 16;

			say("  capability %#x: PCI Express: link able to gen %u x%u, now gen %u x%u\n", cap, link_cap & 0xf,
			    link_cap >> 4 & 0x3f, link_status & 0xf, link_status >> 4 & 0x3f);
		} else if ((head & 0xff) == 0x05) {
			say("  capability %#x: MSI, control %#x\n", cap, head >> 16);
		} else if ((head & 0xff) == 0x11) {
			say("  capability %#x: MSI-X, control %#x\n", cap, head >> 16);
		} else {
			say("  capability %#x: id %#x\n", cap, head & 0xff);
		}
	}
	return true;
}

/* MHI's registers at the start of BAR 0 (mhi/common.h), and BHI's where MHI says */
#define MHI_REGLEN              0x00
#define MHI_VER                 0x08
#define MHI_CFG                 0x10
#define MHI_CHDBOFF             0x18
#define MHI_ERDBOFF             0x20
#define MHI_BHIOFF              0x28
#define MHI_BHIEOFF             0x2c
#define MHI_CTRL                0x38
#define MHI_STATUS              0x48
#define BHI_EXECENV             0x28
#define BHI_SERIALNU            0x40
#define BHI_MSMHWID0            0x4c

static const char *
mhi_ee(uint32_t ee)
{
	static const char *const names[] = { "PBL", "SBL", "AMSS", "RDDM", "WFW", "PTHRU", "EDL", "FP" };

	return ee < sizeof(names) / sizeof(names[0]) ? names[ee] : "?";
}

/*
 * The radio's address, for the driver and for en0: wifi_mac=A if given, else one made of the chip's
 * serial (BHI), the same every run (the firmware's own, 00:03:7f:12:xx:xx, is another every
 * start), locally administered so that it is nobody's. With wifi_station=1 en0 is made on it now,
 * before the system starts, as redwood_lx_net_early() does with wifi=1: configd then has its own
 * network service for the radio (made of RedwoodWLAN) and runs DHCP itself, which is what makes
 * wifid show the network joined. An en0 that turns up once the driver is up, seconds after
 * configd began, gets none: the address then came only from net_test's "dhcp en0", and Settings
 * spun where the tick belongs.
 */
static uint8_t radio_address[6];
static bool radio_address_known;

int
shennong_wlan_radio_address(uint8_t *address)
{
	if (radio_address_known) {
		memcpy(address, radio_address, sizeof(radio_address));
	}
	return radio_address_known;
}

static void
address_made(uint32_t serial)
{
	char text[32] = "";
	bool given = false;

	if (radio_address_known) {
		return;
	}
	if (PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) && text[0] != 0 && hakios_wifi_radio_address(radio_address)) {
		radio_address_known = given = true;
	} else {
		radio_address[0] = 0x02;
		radio_address[1] = 0x03;
		radio_address[2] = 0x7f;
		radio_address[3] = (uint8_t)(serial >> 16);
		radio_address[4] = (uint8_t)(serial >> 8);
		radio_address[5] = (uint8_t)serial;
		radio_address_known = true;
	}
	if (driver_wanted && hakios_wifi_option("wifi_station")) {
		lx_os_net_attach(radio_address);
		say("wlan: en0 made now on the radio's address (%s), before the system starts, for configd's own network service\n",
		    given ? "wifi_mac" : "made of the chip's serial");
	}
}

/* step 6 */
static bool
radio_registers(void)
{
	uint32_t lo, hi, bhi;
	uint64_t at;

	announce("step 6: the radio's BAR 0 sized and placed in the memory window, memory and bus mastering on");
	lo = rd(conf + 0x10);
	wr(conf + 0x10, 0xffffffff);
	wr(conf + 0x14, 0xffffffff);
	bar0_size = ~((uint64_t)rd(conf + 0x14) << 32 | (rd(conf + 0x10) & ~0xfu)) + 1;
	if ((lo & 6) != 4 || bar0_size == 0 || bar0_size > MEM_SIZE) {
		say("BAR 0 is not what was expected: %#x, size %#llx\n", lo, (unsigned long long)bar0_size);
		return false;
	}
	at = (MEM_PHYS + bar0_size - 1) & ~(bar0_size - 1);
	if (at + bar0_size > MEM_PHYS + MEM_SIZE) {
		say("BAR 0 (%#llx bytes) does not fit the window\n", (unsigned long long)bar0_size);
		return false;
	}
	wr(conf + 0x10, (uint32_t)at);
	wr(conf + 0x14, (uint32_t)(at >> 32));
	if (!atu_region(1, ATU_TYPE_MEM, MEM_PHYS, MEM_SIZE, MEM_PHYS)) {
		return false;
	}
	/* the root port forwards that memory: base and limit in 1 MB units */
	wr(dbi + 0x20, (uint32_t)((MEM_PHYS + MEM_SIZE - 1) >> 16 & 0xfff0) << 16 | (uint32_t)(MEM_PHYS >> 16 & 0xfff0));
	wr(conf + 0x04, rd(conf + 0x04) | 0x6);
	say("  BAR 0: %#llx bytes at %#llx (reads back %#x %#x); command %#x\n", (unsigned long long)bar0_size,
	    (unsigned long long)at, rd(conf + 0x10), rd(conf + 0x14),
	    rd(conf + 4) & 0xffff);
	bar0 = ml_io_map((vm_offset_t)at, 0x10000);
	say("  MHI: length %#x, version %#x, config %#x, doorbells: channels at %#x, events at %#x; BHI at %#x, BHIe at %#x; "
	    "control %#x, status %#x\n", rd(bar0 + MHI_REGLEN), rd(bar0 + MHI_VER), rd(bar0 + MHI_CFG), rd(bar0 + MHI_CHDBOFF),
	    rd(bar0 + MHI_ERDBOFF), rd(bar0 + MHI_BHIOFF), rd(bar0 + MHI_BHIEOFF), rd(bar0 + MHI_CTRL), rd(bar0 + MHI_STATUS));
	hi = rd(bar0 + MHI_STATUS);
	bhi = rd(bar0 + MHI_BHIOFF);
	if (bhi == 0 || bhi >= 0x10000 - 0x60) {
		say("BHI is not where it should be\n");
		return false;
	}
	say("  BHI: execution environment %u (%s), serial %#x, hardware ids %#x %#x; MHI state %u%s\n",
	    rd(bar0 + bhi + BHI_EXECENV), mhi_ee(rd(bar0 + bhi + BHI_EXECENV)), rd(bar0 + bhi + BHI_SERIALNU),
	    rd(bar0 + bhi + BHI_MSMHWID0), rd(bar0 + bhi + BHI_MSMHWID0 + 4), hi >> 8 & 0xff, hi & 4 ? ", SYSERR" : "");
	address_made(rd(bar0 + bhi + BHI_SERIALNU));
	return true;
}

/* ---- step 7: the SMMU ---------------------------------------------------------------------------------- */

#define SMMU_PHYS               0x15000000UL    /* apps-smmu@15000000 */
#define SMMU_SCR0               0x000           /* bit 0 CLIENTPD (off), bit 10 USFCFG (a stream nobody matches faults) */
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_GFSR               0x048           /* global faults: bit 1 USF (a stream nobody matches) */
#define SMMU_GFSYNR0            0x050
#define SMMU_GFSYNR1            0x054           /* [15:0] the stream */
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define PARF_BDF_TO_SID_CFG     0x2c00          /* bit 0: bypass (the requester id is the stream, with the base) */
#define STREAM_ROOT             0x1400
#define STREAM_RADIO            0x1401
#define PTE_ADDR                0x0000fffffffff000ull

static vm_offset_t smmu;

static void
smmu_faults(const char *when)
{
	say("SMMU %s: global fault status %#x, syndrome %#x %#x (stream %#x)\n", when, rd(smmu + SMMU_GFSR), rd(smmu + SMMU_GFSYNR0),
	    rd(smmu + SMMU_GFSYNR1), rd(smmu + SMMU_GFSYNR1) & 0xffff);
}

static void
smmu_show(void)
{
	static const char *const kinds[] = { "translated by context", "BYPASS, context field", "FAULT, context field", "reserved, context field" };
	uint32_t scr0 = rd(smmu + SMMU_SCR0), idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), matches = idr0 & 0xff;
	uint32_t wide = scr0 & 1u << 3, valid = 0, covering = 0;

	say("step 7: the SMMU is %s; %u stream match registers, %u contexts; a stream nobody matches %s; %s stream numbers\n",
	    scr0 & 1 ? "OFF" : "on", matches, idr1 & 0xff, scr0 & 1u << 10 ? "faults" : "passes untranslated", wide ? "16-bit" : "15-bit");
	for (uint32_t n = 0; n < matches && n < 256; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t stream = smr & (wide ? 0xffff : 0x7fff), mask = smr >> 16 & (wide ? 0xffff : 0x7fff);

		if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
			continue;
		}
		valid++;
		for (uint32_t sid = STREAM_ROOT; sid <= STREAM_RADIO; sid++) {
			if (((stream ^ sid) & ~mask) == 0) {
				say("  match register %u (stream %#x mask %#x) covers %#x: %s %u (s2cr %#x)\n", n, stream, mask, sid,
				    kinds[s2cr >> 16 & 3], s2cr & 0xff, s2cr);
				covering++;
			}
		}
	}
	say("  %u of %u match registers in use; %u cover PCIe0's streams%s\n", valid, matches, covering,
	    covering ? "" : ": NOTHING IS SET UP FOR THEM");
	say("  the controller's requester-to-stream translation: %#x (%s)\n", rd(parf + PARF_BDF_TO_SID_CFG),
	    rd(parf + PARF_BDF_TO_SID_CFG) & 1 ? "bypass" : "by its table");
	smmu_faults("as found");
}

/* ---- step 8: a context of the SMMU for the radio ------------------------------------------------------ */

#define SMMU_CBAR(n)            (4 * (n))       /* in the second page */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00            /* in the context's own page */
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define RADIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as shennong_sound.c's */
#define RADIO_STREAM_MASK       0x7f
#define RADIO_ADDRESSES         0x98000000ull   /* dt: cnss_pci_iommu_group0, qcom,iommu-dma-addr-pool */
#define RADIO_ADDRESSES_SIZE    0x18000000ull
#define RADIO_L3_TABLES         96              /* 2 MB of addresses each (192 MB: the driver's pool and the firmware's memory) */

static struct {
	bool on;
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t second, ctx;                /* the SMMU's second page, and our context's */
	vm_offset_t tables;                     /* the root, one of the second level, RADIO_L3_TABLES of the third */
	unsigned l3_used;
	uint8_t l3_of[512];                     /* for each entry of the second level: which table of the third, 1.. (0: none) */
	uint64_t next;                          /* the first of the radio's addresses not handed out */
} rs;

#define RS_TABLE(i)             ((uint64_t *)(rs.tables + 4096UL * (i)))

static void
tables_flush(void)
{
	for (vm_offset_t line = rs.tables; line < rs.tables + 4096UL * (2 + RADIO_L3_TABLES); line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

static bool
tlb_forget(void)
{
	wr(rs.ctx + SMMU_CB_TLBIASID, rs.context);
	wr(rs.ctx + SMMU_CB_TLBSYNC, 1);
	return wait_for(rs.ctx + SMMU_CB_TLBSTATUS, 1, 0, 100000, NULL);
}

/* `size' bytes of ours from `va' (or, with va 0, from the physical address `physical') behind the radio's next addresses */
static uint64_t
radio_enter(vm_offset_t va, uint64_t physical, uint64_t size);

/* `size' bytes of ours from `va' behind the radio's next addresses; 0 if there are none left */
static uint64_t
radio_map(vm_offset_t va, uint64_t size)
{
	return radio_enter(va, 0, size);
}

static uint64_t
radio_enter(vm_offset_t va, uint64_t physical, uint64_t size)
{
	uint64_t at = rs.next, pages = (size + 4095) / 4096;

	if (!rs.on || at + pages * 4096 > RADIO_ADDRESSES + RADIO_ADDRESSES_SIZE) {
		return 0;
	}
	for (uint64_t i = 0; i < pages; i++) {
		uint64_t iova = at + i * 4096, *l2 = RS_TABLE(1), *l3;
		unsigned l2i = (unsigned)(iova >> 21 & 0x1ff);

		if (rs.l3_of[l2i] == 0) {
			if (rs.l3_used == RADIO_L3_TABLES) {
				return 0;
			}
			rs.l3_of[l2i] = (uint8_t)(++rs.l3_used);
			l2[l2i] = kvtophys((vm_offset_t)RS_TABLE(1 + rs.l3_of[l2i])) | 3;
		}
		l3 = RS_TABLE(1 + rs.l3_of[l2i]);
		l3[iova >> 12 & 0x1ff] = ((va ? kvtophys(va + i * 4096) : physical + i * 4096) & PTE_ADDR) | RADIO_PTE;
	}
	tables_flush();
	tlb_forget();
	rs.next = at + pages * 4096;
	return at;
}

static bool
smmu_on(void)
{
	bool used[256] = { false };
	uint32_t scr0 = rd(smmu + SMMU_SCR0), idr1 = rd(smmu + SMMU_IDR1), matches = rd(smmu + SMMU_IDR0) & 0xff;
	uint32_t contexts = idr1 & 0xff, sees = 1u << 31 | RADIO_STREAM_MASK << 16 | STREAM_ROOT;
	uint64_t root;

	if (scr0 & 1u << 3) {
		say("the SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	rs.page = idr1 >> 31 ? 0x10000 : 0x1000;
	rs.pages = 2u << (idr1 >> 28 & 7);
	rs.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
		} else if (rs.match == ~0u) {
			rs.match = n;
			rs.old_smr = smr;
			rs.old_s2cr = s2cr;
		}
	}
	for (rs.context = 0; rs.context < contexts && used[rs.context]; rs.context++) {
	}
	if (rs.match == ~0u || rs.context >= contexts) {
		say("the SMMU has no match register or no context left\n");
		return false;
	}
	rs.second = ml_io_map(SMMU_PHYS + rs.page, 0x1000);
	rs.ctx = ml_io_map(SMMU_PHYS + (vm_offset_t)(rs.pages + rs.context) * rs.page, 0x1000);
	if (rs.second == 0 || rs.ctx == 0) {
		return false;
	}
	rs.old_sctlr = rd(rs.ctx + SMMU_CB_SCTLR);
	say("step 8: free: match register %u (smr %#x, s2cr %#x) and context %u (sctlr %#x) at %#lx; pages of %#x, %u before "
	    "the contexts\n", rs.match, rs.old_smr, rs.old_s2cr, rs.context, rs.old_sctlr,
	    SMMU_PHYS + (unsigned long)(rs.pages + rs.context) * rs.page, rs.page, rs.pages);
	if (rs.old_sctlr & 1) {
		say("CONTEXT %u IS SWITCHED ON although no stream goes through it: it is somebody's, and is left alone\n", rs.context);
		return false;
	}
	if (kmem_alloc(kernel_map, &rs.tables, 4096UL * (2 + RADIO_L3_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("no memory for the page tables\n");
		return false;
	}
	RS_TABLE(0)[RADIO_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)RS_TABLE(1)) | 3;
	tables_flush();
	root = kvtophys(rs.tables) | (uint64_t)rs.context << 48;
	announce("step 8: THE SMMU IS WRITTEN: our context with our page tables, and PCIe0's streams through it");
	wr(rs.ctx + SMMU_CB_SCTLR, 0);
	wr(rs.second + SMMU_CBA2R(rs.context), SMMU_CBA2R_64_BIT);
	wr(rs.second + SMMU_CBAR(rs.context), SMMU_CBAR_STAGE_1);
	wr(rs.ctx + SMMU_CB_TCR2, SMMU_TCR2);
	wr(rs.ctx + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(rs.ctx + SMMU_CB_TTBR0, (uint32_t)root);
	wr(rs.ctx + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	wr(rs.ctx + SMMU_CB_TTBR1, 0);
	wr(rs.ctx + SMMU_CB_TTBR1 + 4, 0);
	wr(rs.ctx + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(rs.ctx + SMMU_CB_MAIR1, 0);
	wr(rs.ctx + SMMU_CB_FSR, SMMU_FSR_ALL);
	if (!tlb_forget()) {
		say("the SMMU does not finish forgetting old translations\n");
		return false;
	}
	wr(rs.ctx + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	rs.on = true;
	rs.next = RADIO_ADDRESSES;
	wr(smmu + SMMU_S2CR(rs.match), rs.context);
	wr(smmu + SMMU_SMR(rs.match), sees);
	if (rd(smmu + SMMU_SMR(rs.match)) != sees || (rd(smmu + SMMU_S2CR(rs.match)) & 0x300ff) != rs.context ||
	    !(rd(rs.ctx + SMMU_CB_SCTLR) & 1) || rd(rs.ctx + SMMU_CB_TTBR0) != (uint32_t)root) {
		say("THE SMMU DID NOT TAKE WHAT WAS WRITTEN: smr %#x, s2cr %#x, sctlr %#x, ttbr0 %#x\n", rd(smmu + SMMU_SMR(rs.match)),
		    rd(smmu + SMMU_S2CR(rs.match)), rd(rs.ctx + SMMU_CB_SCTLR), rd(rs.ctx + SMMU_CB_TTBR0));
		return false;
	}
	say("  PCIe0's streams (%#x mask %#x) go through context %u, with our page tables (root %#llx)\n", STREAM_ROOT,
	    RADIO_STREAM_MASK, rs.context, (unsigned long long)(root & PTE_ADDR));
	return true;
}

static void
context_faults(const char *when)
{
	uint32_t status;

	if (!rs.on) {
		return;
	}
	status = rd(rs.ctx + SMMU_CB_FSR);
	say("SMMU %s: our context %u: fault status %#x%s, address %#llx, syndrome %#x\n", when, rs.context, status,
	    status & SMMU_FSR_ALL ? " (REFUSED SOMETHING)" : "",
	    (unsigned long long)rd(rs.ctx + SMMU_CB_FAR + 4) << 32 | rd(rs.ctx + SMMU_CB_FAR), rd(rs.ctx + SMMU_CB_FSYNR0));
	if (status & SMMU_FSR_ALL) {
		wr(rs.ctx + SMMU_CB_FSR, status);
	}
}

/* after the radio is unpowered: the SMMU as it was found */
static void
smmu_off(void)
{
	if (!rs.on) {
		return;
	}
	if (!leaving) {
		context_faults("at the end");
	}
	wr(smmu + SMMU_SMR(rs.match), rs.old_smr);
	wr(smmu + SMMU_S2CR(rs.match), rs.old_s2cr);
	wr(rs.ctx + SMMU_CB_SCTLR, rs.old_sctlr);
	rs.on = false;
}

/* ---- step 9: the second-stage loader through BHI ---------------------------------------------------- */

#define BHI_IMGADDR_LOW         0x08
#define BHI_IMGADDR_HIGH        0x0c
#define BHI_IMGSIZE             0x10
#define BHI_IMGTXDB             0x18
#define BHI_STATUS              0x2c            /* [31:30]: 2 done, 3 error */
#define BHI_ERRCODE             0x30
#define SBL_SIZE                0x80000         /* ath12k's mhi.c: sbl_size */

static bool
radio_sbl(void)
{
	char name[32] = "amss20.bin";
	const uint8_t *file;
	uint64_t size = 0, physical, iova;
	vm_offset_t memory = 0;
	uint32_t bhi = rd(bar0 + MHI_BHIOFF), status = 0, ee = 0, waited, length;

	PE_parse_boot_argn("wlan_fw", name, sizeof(name));
	file = hakios_image_file("fw", name, &size, NULL);
	if (file == NULL) {
		say("step 9: no fw/%s in the small image: put the phone's kiwi firmware into fw-wifi/ (see the patch) and build the "
		    "images again\n", name);
		return false;
	}
	length = size < SBL_SIZE ? (uint32_t)size : SBL_SIZE;
	say("step 9: fw/%s, %llu bytes (%02x %02x %02x %02x); its first %u go to the radio\n", name, (unsigned long long)size,
	    file[0], file[1], file[2], file[3], length);
	if (kmem_alloc_contig(kernel_map, &memory, SBL_SIZE, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("no %u bytes of memory in one piece\n", SBL_SIZE);
		return false;
	}
	memcpy((void *)memory, file, length);
	for (vm_offset_t line = memory; line < memory + SBL_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	physical = kvtophys(memory);
	iova = radio_map(memory, SBL_SIZE);
	if (iova == 0) {
		say("the radio's addresses: none for the image\n");
		return false;
	}
	announce("step 9: BHI: the image's address and size to the radio, and its doorbell");
	say("  at the radio's %#llx (ours %#llx, in one piece)\n", (unsigned long long)iova, (unsigned long long)physical);
	wr(bar0 + bhi + BHI_STATUS, 0);
	wr(bar0 + bhi + BHI_IMGADDR_HIGH, (uint32_t)(iova >> 32));
	wr(bar0 + bhi + BHI_IMGADDR_LOW, (uint32_t)iova);
	wr(bar0 + bhi + BHI_IMGSIZE, length);
	wr(bar0 + bhi + BHI_IMGTXDB, 0x1234567 & 0x3fffffff);   /* a session number: any but 0 */
	for (waited = 0; waited < 3000; waited += 5) {
		status = rd(bar0 + bhi + BHI_STATUS);
		if (status >> 30 != 0 || (rd(bar0 + MHI_STATUS) & 4)) {
			break;
		}
		wait_ms(5);
	}
	say("  BHI status %#x (%s) after %u ms; error code %#x, debug %#x %#x %#x; MHI status %#x; execution environment %u (%s)\n",
	    status, status >> 30 == 2 ? "done" : status >> 30 == 3 ? "ERROR" : "nothing", waited, rd(bar0 + bhi + BHI_ERRCODE),
	    rd(bar0 + bhi + BHI_ERRCODE + 4), rd(bar0 + bhi + BHI_ERRCODE + 8), rd(bar0 + bhi + BHI_ERRCODE + 12),
	    rd(bar0 + MHI_STATUS), rd(bar0 + bhi + BHI_EXECENV), mhi_ee(rd(bar0 + bhi + BHI_EXECENV)));
	smmu_faults("after");
	context_faults("after");
	if (status >> 30 != 2) {
		return false;
	}
	for (waited = 0; waited < 2000; waited += 10) {
		ee = rd(bar0 + bhi + BHI_EXECENV);
		if (ee != 0) {
			break;
		}
		wait_ms(10);
	}
	say("  execution environment %u (%s) %u ms later; MHI status %#x (state %u%s)\n", ee, mhi_ee(ee), waited,
	    rd(bar0 + MHI_STATUS), rd(bar0 + MHI_STATUS) >> 8 & 0xff, rd(bar0 + MHI_STATUS) & 4 ? ", SYSERR" : "");
	return ee == 1;
}

/* ---- step 10: the MHI host: contexts of channels, event rings and the command ring; READY, then M0 ------- */

#define MHI_CCABAP              0x58            /* each of these: low word, then high */
#define MHI_ECABAP              0x60
#define MHI_CRCBAP              0x68
#define MHI_CRDB                0x70
#define MHI_CTRLBASE            0x80
#define MHI_CTRLLIMIT           0x88
#define MHI_DATABASE            0x98
#define MHI_DATALIMIT           0xa0
#define MHICTRL_RESET           (1u << 1)
#define MHISTATUS_READY         (1u << 0)
#define MHISTATUS_SYSERR        (1u << 2)
#define MHI_M0                  2
#define MHI_CHANNELS            128             /* ath12k's mhi.c: WCN7850's max_channels */
#define MHI_COMMANDS            128             /* CMD_EL_PER_RING */
#define CH_IPCR_OUT             20
#define CH_IPCR_IN              21

struct mhi_context {                            /* mhi_chan_ctxt, mhi_event_ctxt and mhi_cmd_ctxt alike */
	uint32_t config, type, index;
	uint64_t base, length, read, write;
} __attribute__((packed, aligned(4)));

struct mhi_element {
	uint64_t pointer;
	uint32_t word[2];
};

static const struct {
	uint32_t elements, moderation_ms, vector;
} rings[2] = {
	{ 32, 0, 1 },                           /* control: state changes, execution environments, command completions */
	{ 256, 1, 2 },                          /* IPCR's */
};

static struct {
	volatile struct mhi_context *channel, *event, *command;
	volatile struct mhi_element *ring[2], *commands;
	uint64_t channel_io, event_io, command_io, ring_io[2], commands_io;
	uint32_t seen[2];                       /* the next element of each event ring not looked at */
	uint32_t chdb, erdb, last_ee, last_state;
	bool up;
} mh;

/* memory the radio can reach: zeroed, in one piece; ours through a mapping without the cache, the radio's at *io */
static vm_offset_t
radio_memory(uint32_t size, uint64_t *io)
{
	vm_offset_t memory = 0, uncached;

	size = (size + 4095) & ~4095u;
	if (kmem_alloc_contig(kernel_map, &memory, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		return 0;
	}
	for (vm_offset_t line = memory; line < memory + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_wcomb((vm_offset_t)kvtophys(memory), size);
	*io = uncached ? radio_map(memory, size) : 0;
	return *io ? uncached : 0;
}

static void
wr64(vm_offset_t address, uint64_t value)
{
	wr(address, (uint32_t)value);
	wr(address + 4, (uint32_t)(value >> 32));
}

static void
doorbell(uint32_t offset, uint64_t value)
{
	wr(bar0 + offset + 4, (uint32_t)(value >> 32));
	wr(bar0 + offset, (uint32_t)value);
}

static const char *
mhi_state(uint32_t state)
{
	static const char *const names[] = { "RESET", "READY", "M0", "M1", "M2", "M3", "M3_FAST", "BHI", "SYS_ERR" };

	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

static uint32_t command_code;
static uint64_t command_done;                   /* the radio's address of the command last answered */
static void transfer_done(uint32_t channel, uint32_t code, uint32_t length, uint64_t at);
struct qrtr_header;
static void qmi_received(const struct qrtr_header *h, uint32_t length);
static void link_received(const struct qrtr_header *h, uint32_t length);
static volatile bool link_mode, link_wanted, link_runs;

static void radio_crashed(const char *when);
static void rddm_prepare(void);
static void rddm_read(void);

/* what the radio has put on its event rings since last time; each element given back to it */
static void
events(const char *when)
{
	static bool inside;

	if (inside) {
		return;                         /* (a send from a handler of an event, waiting for a slot) */
	}
	inside = true;
	for (unsigned r = 0; r < 2; r++) {
		uint64_t read = mh.event[r].read;
		uint32_t upto, n = 0;

		__asm__ volatile ("dmb sy");
		if (read < mh.ring_io[r] || read >= mh.ring_io[r] + rings[r].elements * 16ull) {
			say("event ring %u %s: its read pointer %#llx is outside the ring\n", r, when, (unsigned long long)read);
			continue;
		}
		upto = (uint32_t)((read - mh.ring_io[r]) / 16);
		while (mh.seen[r] != upto && n++ < rings[r].elements) {
			volatile struct mhi_element *e = &mh.ring[r][mh.seen[r]];
			uint32_t w0 = e->word[0], w1 = e->word[1], type = w1 >> 16 & 0xff, code = w0 >> 24;
			uint64_t write;

			switch (type) {
			case 0x20:
				mh.last_state = code;
				say("event %u.%u %s: state %u (%s)\n", r, mh.seen[r], when, code, mhi_state(code));
				if (code == 0xff) {
					radio_crashed("at its SYS_ERR");
				}
				break;
			case 0x40:
				mh.last_ee = code;
				say("event %u.%u %s: execution environment %u (%s)\n", r, mh.seen[r], when, code,
				    code == 0x10 ? "DISABLE_TRANSITION" : mhi_ee(code));
				break;
			case 0x21:
				command_done = e->pointer;
				command_code = code;
				say("event %u.%u %s: command done, code %u, for %#llx\n", r, mh.seen[r], when, code,
				    (unsigned long long)e->pointer);
				break;
			case 0x22:
				transfer_done(w1 >> 24, code, w0 & 0xffff, e->pointer);
				break;
			default:
				say("event %u.%u %s: type %#x, channel %u, words %#x %#x, pointer %#llx\n", r, mh.seen[r], when, type,
				    w1 >> 24, w0, w1, (unsigned long long)e->pointer);
				break;
			}
			mh.seen[r] = (mh.seen[r] + 1) % rings[r].elements;
			/* the element just read is the radio's again: the write pointer one past it, wrapping */
			write = mh.event[r].write + 16;
			if (write >= mh.ring_io[r] + rings[r].elements * 16ull) {
				write = mh.ring_io[r];
			}
			mh.event[r].write = write;
			__asm__ volatile ("dsb sy");
			doorbell(mh.erdb + 8 * r, write);
		}
	}
	inside = false;
}

/* events for `ms' milliseconds, or until the execution environment is `ee' */
static uint32_t
events_until(uint32_t ms, uint32_t ee, const char *when)
{
	uint32_t bhi = rd(bar0 + MHI_BHIOFF), waited;

	for (waited = 0; waited < ms; waited += 10) {
		events(when);
		if (rd(bar0 + bhi + BHI_EXECENV) == ee || (rd(bar0 + MHI_STATUS) & MHISTATUS_SYSERR)) {
			break;
		}
		wait_ms(10);
	}
	events(when);
	return waited;
}

static bool
mhi_host(void)
{
	uint32_t config = rd(bar0 + MHI_CFG), value, status;
	uint64_t top = 0xffffffffull;                   /* ath12k: iova_start 0, iova_stop its 32-bit DMA mask */

	mh.chdb = rd(bar0 + MHI_CHDBOFF);
	mh.erdb = rd(bar0 + MHI_ERDBOFF);
	say("step 10: MHI config %#x (%u channels, %u event rings, %u of them the hardware's), doorbells at %#x and %#x\n", config,
	    config & 0xff, config >> 16 & 0xff, config >> 24, mh.chdb, mh.erdb);
	if (mh.chdb == 0 || mh.chdb + 8 * MHI_CHANNELS > 0x10000 || mh.erdb == 0 || mh.erdb + 16 > 0x10000) {
		say("the doorbells are not where they could be\n");
		return false;
	}
	mh.channel = (volatile struct mhi_context *)radio_memory(sizeof(struct mhi_context) * MHI_CHANNELS, &mh.channel_io);
	mh.event = (volatile struct mhi_context *)radio_memory(4096, &mh.event_io);
	mh.ring[0] = (volatile struct mhi_element *)radio_memory(4096, &mh.ring_io[0]);
	mh.ring[1] = (volatile struct mhi_element *)radio_memory(4096, &mh.ring_io[1]);
	mh.commands = (volatile struct mhi_element *)radio_memory(4096, &mh.commands_io);
	if (!mh.channel || !mh.event || !mh.ring[0] || !mh.ring[1] || !mh.commands) {
		say("no memory for MHI's contexts and rings (or no addresses of the radio's for it)\n");
		return false;
	}
	/* the command ring's context after the two of the event rings, in the same page */
	mh.command = mh.event + 2;
	mh.command_io = mh.event_io + 2 * sizeof(struct mhi_context);

	/* IPCR: 20 to the radio, 21 from it, both on event ring 1; doorbell bursts off (2), not polled; disabled for now */
	mh.channel[CH_IPCR_OUT].config = 2u << 8;
	mh.channel[CH_IPCR_OUT].type = 1;
	mh.channel[CH_IPCR_OUT].index = 1;
	mh.channel[CH_IPCR_IN].config = 2u << 8;
	mh.channel[CH_IPCR_IN].type = 2;
	mh.channel[CH_IPCR_IN].index = 1;
	for (unsigned r = 0; r < 2; r++) {
		mh.event[r].config = rings[r].moderation_ms << 16;
		mh.event[r].type = 1;
		mh.event[r].index = rings[r].vector;
		mh.event[r].base = mh.ring_io[r];
		mh.event[r].length = rings[r].elements * 16ull;
		mh.event[r].read = mh.event[r].write = mh.ring_io[r];
	}
	mh.command->base = mh.commands_io;
	mh.command->length = MHI_COMMANDS * 16ull;
	mh.command->read = mh.command->write = mh.commands_io;
	__asm__ volatile ("dsb sy");
	say("  at the radio's: channels %#llx, event rings' contexts %#llx (rings %#llx, %#llx), the command ring's %#llx (ring "
	    "%#llx)\n", (unsigned long long)mh.channel_io, (unsigned long long)mh.event_io, (unsigned long long)mh.ring_io[0],
	    (unsigned long long)mh.ring_io[1], (unsigned long long)mh.command_io, (unsigned long long)mh.commands_io);

	/* mhi_ready_state_transition(): RESET clear, READY set */
	if (!wait_for(bar0 + MHI_CTRL, MHICTRL_RESET, 0, 2000000, &value) ||
	    !wait_for(bar0 + MHI_STATUS, MHISTATUS_READY, MHISTATUS_READY, 2000000, &status)) {
		say("the radio is not READY: control %#x, status %#x\n", rd(bar0 + MHI_CTRL), rd(bar0 + MHI_STATUS));
		return false;
	}
	announce("step 10: MHI: the contexts' addresses to the radio, the event rings given to it, then M0");
	wr64(bar0 + MHI_CCABAP, mh.channel_io);
	wr64(bar0 + MHI_ECABAP, mh.event_io);
	wr64(bar0 + MHI_CRCBAP, mh.command_io);
	wr64(bar0 + MHI_CTRLBASE, 0);
	wr64(bar0 + MHI_DATABASE, 0);
	wr64(bar0 + MHI_CTRLLIMIT, top);
	wr64(bar0 + MHI_DATALIMIT, top);
	wr(bar0 + MHI_CFG, (rd(bar0 + MHI_CFG) & 0x0000ffff) | 2u << 16);   /* NER 2, NHWER 0 */
	for (unsigned r = 0; r < 2; r++) {
		uint64_t last = mh.ring_io[r] + (rings[r].elements - 1) * 16ull;

		mh.event[r].write = last;
		__asm__ volatile ("dsb sy");
		doorbell(mh.erdb + 8 * r, last);
	}
	mh.up = true;
	wr(bar0 + MHI_CTRL, (rd(bar0 + MHI_CTRL) & ~0xff00u) | MHI_M0 << 8);
	wait_for(bar0 + MHI_STATUS, 0xff00, MHI_M0 << 8, 1000000, &status);
	say("  MHI config now %#x; status %#x (state %u, %s%s)\n", rd(bar0 + MHI_CFG), status, status >> 8 & 0xff,
	    mhi_state(status >> 8 & 0xff), status & MHISTATUS_SYSERR ? ", SYSERR" : "");
	events_until(300, ~0u, "after M0");
	smmu_faults("after M0");
	context_faults("after M0");
	return (status >> 8 & 0xff) == MHI_M0;
}

/* ---- step 11: the whole image through BHIe, then the radio's own firmware (AMSS, mission mode) ----------- */

#define BHIE_TXVECADDR          0x2c            /* low, then high */
#define BHIE_TXVECSIZE          0x34
#define BHIE_TXVECDB            0x3c            /* [29:0] a session number */
#define BHIE_TXVECSTATUS        0x44            /* [29:0] its number back, [31:30] 2 done, 3 error */
#define SEGMENT                 0x80000         /* ath12k's seg_len */

struct bhi_vector {
	uint64_t address, size;
};

static bool
radio_amss(void)
{
	char name[32] = "amss20.bin";
	const uint8_t *file;
	uint64_t size = 0, table_io;
	uint32_t bhie = rd(bar0 + MHI_BHIEOFF), bhi = rd(bar0 + MHI_BHIOFF), pieces, status = 0, waited, ee;
	volatile struct bhi_vector *table;
	const uint32_t session = 0x2345678;

	PE_parse_boot_argn("wlan_fw", name, sizeof(name));
	file = hakios_image_file("fw", name, &size, NULL);
	if (file == NULL || size == 0) {
		return false;
	}
	pieces = (uint32_t)((size + SEGMENT - 1) / SEGMENT);
	say("step 11: fw/%s whole (%llu bytes) in %u pieces of %u KB; BHIe at %#x (status %#x)\n", name, (unsigned long long)size,
	    pieces, SEGMENT / 1024, bhie, bhie ? rd(bar0 + bhie + BHIE_TXVECSTATUS) : 0);
	if (bhie == 0 || bhie >= 0x10000 - 0x50 || pieces * sizeof(struct bhi_vector) > 4096) {
		say("BHIe is not where it could be, or the image is too big\n");
		return false;
	}
	table = (volatile struct bhi_vector *)radio_memory(4096, &table_io);
	if (table == NULL) {
		say("no memory for the vector table\n");
		return false;
	}
	for (uint32_t i = 0; i < pieces; i++) {
		vm_offset_t memory = 0;
		uint64_t length = size - (uint64_t)i * SEGMENT < SEGMENT ? size - (uint64_t)i * SEGMENT : SEGMENT, io;

		if (kmem_alloc_contig(kernel_map, &memory, SEGMENT, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("no memory for piece %u\n", i);
			return false;
		}
		memcpy((void *)memory, file + (uint64_t)i * SEGMENT, length);
		for (vm_offset_t line = memory; line < memory + SEGMENT; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		io = radio_map(memory, SEGMENT);
		if (io == 0) {
			say("no addresses of the radio's for piece %u\n", i);
			return false;
		}
		table[i].address = io;
		table[i].size = length;
	}
	__asm__ volatile ("dsb sy");
	say("  the vector table at the radio's %#llx, pieces from %#llx to %#llx\n", (unsigned long long)table_io,
	    (unsigned long long)table[0].address, (unsigned long long)table[pieces - 1].address);
	events_until(1000, ~0u, "before BHIe");
	announce("step 11: BHIe: the vector table's address and size to the radio, and its doorbell");
	wr(bar0 + bhie + BHIE_TXVECADDR + 4, (uint32_t)(table_io >> 32));
	wr(bar0 + bhie + BHIE_TXVECADDR, (uint32_t)table_io);
	wr(bar0 + bhie + BHIE_TXVECSIZE, pieces * (uint32_t)sizeof(struct bhi_vector));
	wr(bar0 + bhie + BHIE_TXVECDB, (rd(bar0 + bhie + BHIE_TXVECDB) & ~0x3fffffffu) | session);
	for (waited = 0; waited < 5000; waited += 5) {
		status = rd(bar0 + bhie + BHIE_TXVECSTATUS);
		if (status >> 30 != 0 || (rd(bar0 + MHI_STATUS) & MHISTATUS_SYSERR)) {
			break;
		}
		wait_ms(5);
	}
	say("  BHIe status %#x (%s, session %#x of %#x) after %u ms; MHI status %#x; execution environment %u (%s)\n", status,
	    status >> 30 == 2 ? "done" : status >> 30 == 3 ? "ERROR" : "nothing", status & 0x3fffffff, session, waited,
	    rd(bar0 + MHI_STATUS), rd(bar0 + bhi + BHI_EXECENV), mhi_ee(rd(bar0 + bhi + BHI_EXECENV)));
	smmu_faults("after BHIe");
	context_faults("after BHIe");
	if (status >> 30 != 2) {
		events("after BHIe");
		return false;
	}
	waited = events_until(10000, 2, "after BHIe");
	ee = rd(bar0 + bhi + BHI_EXECENV);
	say("  execution environment %u (%s) %u ms later; MHI status %#x (state %u, %s%s)\n", ee, mhi_ee(ee), waited,
	    rd(bar0 + MHI_STATUS), rd(bar0 + MHI_STATUS) >> 8 & 0xff, mhi_state(rd(bar0 + MHI_STATUS) >> 8 & 0xff),
	    rd(bar0 + MHI_STATUS) & MHISTATUS_SYSERR ? ", SYSERR" : "");
	if (ee == 2) {
		events_until(2000, ~0u, "in AMSS");     /* mission mode's event, and whatever follows */
		say("  last: execution environment event %u (%s), state event %u (%s)\n", mh.last_ee, mhi_ee(mh.last_ee),
		    mh.last_state, mhi_state(mh.last_state));
	}
	smmu_faults("at the end of step 11");
	context_faults("at the end of step 11");
	if (ee == 2) {
		rddm_prepare();
	}
	return ee == 2;
}

/*
 * The radio's dump when its firmware stops (RDDM: Linux's mhi_rddm_prepare() and mhi_download_rddm_image()):
 * once AMSS runs, memory for it (ath12k's rddm_size for WCN7850, in pieces of 512 KB) is shown to
 * BHIe as its vector for receiving; at a SYS_ERR the radio writes its dump there, and what reads as
 * text in it -- the firmware's own account of why it stopped -- is said.
 */
#define BHIE_RXVECADDR          0x60            /* low, then high */
#define BHIE_RXVECSIZE          0x68
#define BHIE_RXVECDB            0x70
#define BHIE_RXVECSTATUS        0x78
#define RDDM_SIZE               0x780000
#define RDDM_PIECES             (RDDM_SIZE / SEGMENT)

static struct {
	bool ready, read;
	vm_offset_t piece[RDDM_PIECES];
} rddm;

static void
rddm_prepare(void)
{
	uint32_t bhie = rd(bar0 + MHI_BHIEOFF);
	volatile struct bhi_vector *table;
	uint64_t table_io;

	table = (volatile struct bhi_vector *)radio_memory(4096, &table_io);
	if (table == NULL || bhie == 0) {
		return;
	}
	for (uint32_t i = 0; i < RDDM_PIECES; i++) {
		uint64_t io;

		if (kmem_alloc_contig(kernel_map, &rddm.piece[i], SEGMENT, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("RDDM: no memory for piece %u: no dump\n", i);
			return;
		}
		for (vm_offset_t line = rddm.piece[i]; line < rddm.piece[i] + SEGMENT; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		io = radio_map(rddm.piece[i], SEGMENT);
		if (io == 0) {
			say("RDDM: no addresses of the radio's for piece %u: no dump\n", i);
			return;
		}
		table[i].address = io;
		table[i].size = SEGMENT;
	}
	__asm__ volatile ("dsb sy");
	wr(bar0 + bhie + BHIE_RXVECADDR + 4, (uint32_t)(table_io >> 32));
	wr(bar0 + bhie + BHIE_RXVECADDR, (uint32_t)table_io);
	wr(bar0 + bhie + BHIE_RXVECSIZE, RDDM_PIECES * (uint32_t)sizeof(struct bhi_vector));
	wr(bar0 + bhie + BHIE_RXVECDB, (rd(bar0 + bhie + BHIE_RXVECDB) & ~0x3fffffffu) | 0x3456789);
	rddm.ready = true;
	say("RDDM: %u KB for the radio's dump, if its firmware stops, at the radio's %#llx\n", RDDM_SIZE / 1024,
	    (unsigned long long)table_io);
}

static bool
rddm_interesting(const char *text, uint32_t length)
{
	static const char *const words[] = { "ssert", "SSERT", "rror", "RROR", "atal", "ATAL", ".c:", ".c ", "crash", "fail",
					     "panic", "abort", "excep", "Excep", "FAULT", "fault" };

	for (uint32_t i = 0; i < length; i++) {
		for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
			if (strncmp(text + i, words[w], strlen(words[w])) == 0) {
				return true;
			}
		}
	}
	return false;
}

static void
rddm_read(void)
{
	uint32_t bhie = rd(bar0 + MHI_BHIEOFF), status = 0, waited, said = 0, runs = 0;

	if (!rddm.ready || rddm.read) {
		return;
	}
	rddm.read = true;
	for (waited = 0; waited < 3000; waited += 10) {
		status = rd(bar0 + bhie + BHIE_RXVECSTATUS);
		if (status >> 30 != 0) {
			break;
		}
		wait_ms(10);
	}
	say("RDDM: the dump's status %#x (%s) after %u ms\n", status, status >> 30 == 2 ? "written" : status >> 30 == 3 ? "ERROR" :
	    "NOTHING", waited);
	if (status >> 30 != 2) {
		return;
	}
	for (uint32_t i = 0; i < RDDM_PIECES; i++) {
		for (vm_offset_t line = rddm.piece[i]; line < rddm.piece[i] + SEGMENT; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
	}
	__asm__ volatile ("dsb sy");
	/* the dump's head: its first 768 bytes as words, then every run of text in its first 256 KB (the crash's record) */
	for (uint32_t at = 0; at < 0x300; at += 32) {
		const uint32_t *w = (const uint32_t *)(rddm.piece[0] + at);

		say("RDDM head %#05x: %08x %08x %08x %08x %08x %08x %08x %08x\n", at, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	}
	for (int pass = 0; pass < 2; pass++) {
		uint32_t most = pass == 0 ? 70 : 100, first = pass == 0 ? 0 : 1, last = pass == 0 ? 1 : RDDM_PIECES;

		for (uint32_t i = first; i < last && said < most; i++) {
			const char *p = (const char *)rddm.piece[i];
			uint32_t start = 0, end = pass == 0 ? 0x40000 : SEGMENT;

			for (uint32_t at = 0; at <= end && said < most; at++) {
				bool printable = at < end && p[at] >= 0x20 && p[at] < 0x7f;
				char line[161];
				uint32_t n;

				if (printable) {
					continue;
				}
				n = at - start < 160 ? at - start : 160;
				memcpy(line, p + start, n);
				line[n] = 0;
				/* the head: runs of 6 or more; the rest: runs with a telling word that are not the image's own formats (%) */
				if (n >= (pass == 0 ? 6u : 16u) && (pass == 0 || (rddm_interesting(line, n) && strchr(line, '%') == NULL))) {
					say("RDDM %u+%#x: %s\n", i, start, line);
					said++;
				}
				runs += n >= 16;
				start = at + 1;
			}
		}
	}
	say("RDDM: %u runs of text, %u said\n", runs, said);
}

/* ---- step 12: IPCR's channels started; QRTR's HELLO; the radio's services ------------------------------ */

#define CH_ELEMENTS             64              /* ath12k's num_elements for 20 and 21 */
#define RX_BUFFERS              16              /* (divides CH_ELEMENTS: element i always has buffer i % 16) */
#define RX_SIZE                 8192            /* ath12k's buf_len */
#define QRTR_PORT_CTRL          0xfffffffeu
#define QRTR_NODE_BCAST         0xffffffffu
#define QRTR_OUR_NODE           1               /* af_qrtr.c's qrtr_local_nid */
#define QRTR_HELLO              2
#define QRTR_NEW_SERVER         4
#define WLFW_SERVICE            0x45            /* QMI's WLAN firmware service (ath12k's qmi.h) */

struct qrtr_header {
	uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
};

struct qrtr_control {
	uint32_t command, service, instance, node, port;
};

static struct {
	volatile struct mhi_element *ring;
	uint64_t io;
	uint32_t next;                          /* the element the next transfer goes into */
} ch[2];                                        /* 0: channel 20 (to the radio), 1: 21 (from it) */

static volatile uint8_t *rx, *tx;
static uint64_t rx_io, tx_io;
static uint32_t commands_next, rx_count, tx_done;
static uint32_t wlfw_node, wlfw_port, wlfw_instance, remote_node;
static bool wlfw_found, remote_hello;

static const char *
qrtr_type(uint32_t type)
{
	static const char *const names[] = { "?", "DATA", "HELLO", "BYE", "NEW_SERVER", "DEL_SERVER", "DEL_CLIENT",
		                             "RESUME_TX", "EXIT", "PING", "NEW_LOOKUP", "DEL_LOOKUP" };

	return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

/* one more element on a channel's ring, and its doorbell */
static void
channel_put(unsigned c, uint64_t buffer, uint32_t length)
{
	unsigned number = c ? CH_IPCR_IN : CH_IPCR_OUT;
	volatile struct mhi_element *e = &ch[c].ring[ch[c].next];
	uint64_t write;

	e->pointer = buffer;
	e->word[0] = length & 0xffff;
	e->word[1] = 2u << 16 | 1u << 9;        /* a transfer; an event at its end (IEOT) */
	ch[c].next = (ch[c].next + 1) % CH_ELEMENTS;
	write = ch[c].io + ch[c].next * 16ull;
	mh.channel[number].write = write;
	__asm__ volatile ("dsb sy");
	doorbell(mh.chdb + 8 * number, write);
}

/* a transfer event: a packet from the radio on 21 (printed, its buffer queued again), or one of ours sent on 20 */
static void
transfer_done(uint32_t channel, uint32_t code, uint32_t length, uint64_t at)
{
	uint32_t element, buffer;
	const struct qrtr_header *h;

	if (channel == CH_IPCR_OUT) {
		tx_done++;
		say("  sent on channel 20: code %u, %u bytes\n", code, length);
		return;
	}
	if (channel != CH_IPCR_IN || at < ch[1].io || at >= ch[1].io + CH_ELEMENTS * 16ull) {
		say("  a transfer event for channel %u (code %u, %u bytes, element %#llx)\n", channel, code, length,
		    (unsigned long long)at);
		return;
	}
	element = (uint32_t)((at - ch[1].io) / 16);
	buffer = element % RX_BUFFERS;
	h = (const struct qrtr_header *)(uintptr_t)(rx + buffer * RX_SIZE);
	rx_count++;
	if (link_mode && length >= sizeof(*h) && h->version == 1) {
		link_received(h, length);
	} else if (length < sizeof(*h) || h->version != 1) {
		say("  received %u bytes (code %u): not QRTR v1: %08x %08x %08x %08x\n", length, code,
		    ((const volatile uint32_t *)h)[0], ((const volatile uint32_t *)h)[1], ((const volatile uint32_t *)h)[2],
		    ((const volatile uint32_t *)h)[3]);
	} else if (h->type != 1 && length >= sizeof(*h) + 4) {
		const struct qrtr_control *p = (const struct qrtr_control *)(h + 1);

		say("  QRTR %s from %u:%#x to %u:%#x (%u bytes): command %u (%s), service %#x instance %#x node %u port %#x\n",
		    qrtr_type(h->type), h->src_node, h->src_port, h->dst_node, h->dst_port, h->size, p->command,
		    qrtr_type(p->command), p->service, p->instance, p->node, p->port);
		if (h->type == QRTR_HELLO) {
			remote_hello = true;
			remote_node = h->src_node;
		}
		if (h->type == QRTR_NEW_SERVER && p->service == WLFW_SERVICE) {
			wlfw_found = true;
			wlfw_node = p->node;
			wlfw_port = p->port;
			wlfw_instance = p->instance;
		}
	} else if (h->type == 1 && h->dst_port == 0x4000) {
		qmi_received(h, length);
	} else {
		say("  QRTR %s from %u:%#x to %u:%#x, %u bytes\n", qrtr_type(h->type), h->src_node, h->src_port, h->dst_node,
		    h->dst_port, h->size);
	}
	channel_put(1, rx_io + buffer * RX_SIZE, RX_SIZE);
}

static bool
command(uint32_t type, unsigned channel)
{
	volatile struct mhi_element *e = &mh.commands[commands_next];
	uint64_t at = mh.commands_io + commands_next * 16ull, write;

	e->pointer = 0;
	e->word[0] = 0;
	e->word[1] = channel << 24 | type << 16;
	commands_next = (commands_next + 1) % MHI_COMMANDS;
	write = mh.commands_io + commands_next * 16ull;
	mh.command->write = write;
	command_done = 0;
	__asm__ volatile ("dsb sy");
	doorbell(MHI_CRDB, write);
	for (uint32_t waited = 0; waited < 1000 && command_done != at; waited += 5) {
		wait_ms(5);
		events("command");
	}
	say("  command %u for channel %u: %s, code %u; the channel's state %u\n", type, channel,
	    command_done == at ? "answered" : "NOT ANSWERED", command_code, mh.channel[channel].config & 0xff);
	return command_done == at && command_code == 1;
}

static bool
qrtr_hello(void)
{
	struct qrtr_header *h;
	struct qrtr_control *p;

	for (unsigned c = 0; c < 2; c++) {
		unsigned number = c ? CH_IPCR_IN : CH_IPCR_OUT;

		ch[c].ring = (volatile struct mhi_element *)radio_memory(CH_ELEMENTS * 16, &ch[c].io);
		if (ch[c].ring == NULL) {
			say("no memory for channel %u's ring\n", number);
			return false;
		}
		mh.channel[number].base = ch[c].io;
		mh.channel[number].length = CH_ELEMENTS * 16ull;
		mh.channel[number].read = mh.channel[number].write = ch[c].io;
		mh.channel[number].config = (mh.channel[number].config & ~0xffu) | 1;     /* enabled */
	}
	rx = (volatile uint8_t *)radio_memory(RX_BUFFERS * RX_SIZE, &rx_io);
	tx = (volatile uint8_t *)radio_memory(4096 + 8 * 8192, &tx_io);   /* the HELLO's page, then QMI's slots */
	if (rx == NULL || tx == NULL) {
		say("no memory for the buffers\n");
		return false;
	}
	__asm__ volatile ("dsb sy");
	announce("step 12: MHI: START for channels 20 and 21 (IPCR), buffers for 21");
	if (!command(18, CH_IPCR_OUT) || !command(18, CH_IPCR_IN)) {
		return false;
	}
	for (unsigned i = 0; i < RX_BUFFERS; i++) {
		channel_put(1, rx_io + i * RX_SIZE, RX_SIZE);
	}
	events_until(300, ~0u, "after START");

	announce("step 12: QRTR: our HELLO (node 1) on channel 20");
	h = (struct qrtr_header *)(uintptr_t)tx;
	p = (struct qrtr_control *)(h + 1);
	*h = (struct qrtr_header){ 1, QRTR_HELLO, QRTR_OUR_NODE, QRTR_PORT_CTRL, 0, sizeof(*p), QRTR_NODE_BCAST, QRTR_PORT_CTRL };
	*p = (struct qrtr_control){ QRTR_HELLO, 0, 0, 0, 0 };
	__asm__ volatile ("dsb sy");
	channel_put(0, tx_io, sizeof(*h) + sizeof(*p));
	for (uint32_t waited = 0; waited < 3000 && !wlfw_found; waited += 10) {
		events("after HELLO");
		wait_ms(10);
	}
	events_until(500, ~0u, "after HELLO");
	say("step 12: %u packets from the radio, %u of ours sent; its HELLO %s (node %u); WLFW (service %#x) %s", rx_count, tx_done,
	    remote_hello ? "came" : "DID NOT COME", remote_node, WLFW_SERVICE, wlfw_found ? "at " : "NOT ANNOUNCED\n");
	if (wlfw_found) {
		say("node %u port %#x, instance %#x\n", wlfw_node, wlfw_port, wlfw_instance);
	}
	smmu_faults("at the end of step 12");
	context_faults("at the end of step 12");
	return wlfw_found;
}

/* ---- steps 13 and 14: QMI with the radio's WLFW service (ath12k's qmi.c, in its order) ---------------------- */

#define QRTR_DATA               1
#define QRTR_RESUME_TX          7
#define OUR_PORT                0x4000
#define TX_SLOTS                8
#define TX_SLOT                 8192
#define QMI_REQUEST             0
#define QMI_RESPONSE            2
#define QMI_INDICATION          4
#define WLFW_IND_REGISTER       0x0020
#define WLFW_CAP                0x0024
#define WLFW_BDF_DOWNLOAD       0x0025
#define WLFW_HOST_CAP           0x0034
#define WLFW_REQUEST_MEM_IND    0x0035
#define WLFW_RESPOND_MEM        0x0036
#define WLFW_FW_MEM_READY_IND   0x0037
#define WLFW_FW_READY_IND       0x0038
#define WLFW_M3_INFO            0x003c
#define WLFW_PHY_CAP            0x0057
#define BDF_CHUNK               6144            /* QMI_WLANFW_MAX_DATA_SIZE_V01 */
#define BOARD_SIZE              (256 * 1024)    /* ath12k's WCN7850 board_size */
#define INDICATIONS             4
#define QMI_MAX                 4096
#define MEM_SEGMENTS            52

static uint32_t tx_slot, qmi_txn = 1, waiting_txn, answer_length, indication_count, nm_modem = ~0u;
static bool answered;
static uint8_t answer[QMI_MAX];
static struct {
	uint16_t id;
	uint32_t length;
	uint8_t body[QMI_MAX];
} indications[INDICATIONS];
static uint32_t board_id = 0xff;

/* a QRTR packet to the radio from our port `from': header, then `length' bytes */
static void
qrtr_send_from(uint32_t from, uint32_t type, uint32_t dst_node, uint32_t dst_port, const void *payload, uint32_t length)
{
	vm_offset_t slot = (vm_offset_t)tx + 4096 + (vm_offset_t)(tx_slot % TX_SLOTS) * TX_SLOT;
	uint64_t slot_io = tx_io + 4096 + (uint64_t)(tx_slot % TX_SLOTS) * TX_SLOT;
	struct qrtr_header *h = (struct qrtr_header *)slot;

	/* a slot the radio may not have read yet is not written again (the HELLO was sent from outside the slots) */
	for (uint32_t waited = 0; tx_slot + 1 - (tx_done - 1) > TX_SLOTS && waited < 1000; waited++) {
		events("sending");
		wait_us(100);
	}
	tx_slot++;
	*h = (struct qrtr_header){ 1, type, QRTR_OUR_NODE, from, 0, length, dst_node, dst_port };
	memcpy((void *)(slot + sizeof(*h)), payload, length);
	__asm__ volatile ("dsb sy");
	channel_put(0, slot_io, (uint32_t)sizeof(*h) + ((length + 3) & ~3u));
}

static void
qrtr_send(uint32_t type, uint32_t dst_node, uint32_t dst_port, const void *payload, uint32_t length)
{
	qrtr_send_from(OUR_PORT, type, dst_node, dst_port, payload, length);
}

/* a packet from the radio for our port: a QMI response or indication, kept; flow control answered */
static void
qmi_received(const struct qrtr_header *h, uint32_t length)
{
	const uint8_t *m = (const uint8_t *)(h + 1);
	uint32_t size = h->size, kind, txn, id, body;

	if (h->confirm_rx) {
		struct qrtr_control resume = { QRTR_RESUME_TX, QRTR_OUR_NODE, OUR_PORT, 0, 0 };   /* client: node, port */

		qrtr_send(QRTR_RESUME_TX, h->src_node, h->src_port, &resume, sizeof(resume));
	}
	if (size < 7 || size + sizeof(*h) > length) {
		say("  QMI: a short packet (%u bytes)\n", size);
		return;
	}
	kind = m[0];
	txn = m[1] | m[2] << 8;
	id = m[3] | m[4] << 8;
	body = m[5] | m[6] << 8;
	if (body + 7 > size || body > QMI_MAX) {
		say("  QMI: message %#x of %u bytes in a packet of %u\n", id, body, size);
		return;
	}
	if (kind == QMI_RESPONSE && txn == waiting_txn) {
		memcpy(answer, m + 7, body);
		answer_length = body;
		answered = true;
	} else if (kind == QMI_INDICATION) {
		say("  QMI: indication %#x (%u bytes)\n", id, body);
		if (indication_count < INDICATIONS) {
			indications[indication_count].id = (uint16_t)id;
			indications[indication_count].length = body;
			memcpy(indications[indication_count].body, m + 7, body);
			indication_count++;
		}
	} else {
		say("  QMI: type %u, transaction %u, message %#x, %u bytes: not waited for\n", kind, txn, id, body);
	}
}

/* TLVs: writing and finding */
struct tlvs {
	uint8_t data[BDF_CHUNK + 64];
	uint32_t length;
};

static void
tlv(struct tlvs *t, uint8_t type, const void *value, uint32_t length)
{
	t->data[t->length] = type;
	t->data[t->length + 1] = (uint8_t)length;
	t->data[t->length + 2] = (uint8_t)(length >> 8);
	memcpy(t->data + t->length + 3, value, length);
	t->length += 3 + length;
}

static void
tlv_u8(struct tlvs *t, uint8_t type, uint8_t value)
{
	tlv(t, type, &value, 1);
}

static void
tlv_u32(struct tlvs *t, uint8_t type, uint32_t value)
{
	tlv(t, type, &value, 4);
}

static void
tlv_u64(struct tlvs *t, uint8_t type, uint64_t value)
{
	tlv(t, type, &value, 8);
}

static const uint8_t *
tlv_find(const uint8_t *m, uint32_t length, uint8_t type, uint32_t *size)
{
	for (uint32_t at = 0; at + 3 <= length;) {
		uint32_t n = m[at + 1] | m[at + 2] << 8;

		if (at + 3 + n > length) {
			break;
		}
		if (m[at] == type) {
			*size = n;
			return m + at + 3;
		}
		at += 3 + n;
	}
	return NULL;
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
le64(const uint8_t *p)
{
	return le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* a request to WLFW and its response's result (0 success; ~0 none came); the response in answer[] */
static uint32_t
qmi_ask(const char *what, uint16_t id, const struct tlvs *t, bool quiet)
{
	static uint8_t message[sizeof(((struct tlvs *)0)->data) + 7];
	const uint8_t *result;
	uint32_t n, txn = qmi_txn++ & 0xffff, length = t ? t->length : 0;

	message[0] = QMI_REQUEST;
	message[1] = (uint8_t)txn;
	message[2] = (uint8_t)(txn >> 8);
	message[3] = (uint8_t)id;
	message[4] = (uint8_t)(id >> 8);
	message[5] = (uint8_t)length;
	message[6] = (uint8_t)(length >> 8);
	if (length) {
		memcpy(message + 7, t->data, length);
	}
	answered = false;
	waiting_txn = txn;
	qrtr_send(QRTR_DATA, wlfw_node, wlfw_port, message, 7 + length);
	for (uint32_t waited = 0; waited < 5000 && !answered; waited += 2) {
		events("QMI");
		if (!answered) {
			wait_ms(2);
		}
	}
	if (!answered) {
		say("  QMI %s (%#x): NO RESPONSE in 5 s\n", what, id);
		return ~0u;
	}
	result = tlv_find(answer, answer_length, 2, &n);
	if (result == NULL || n < 4) {
		say("  QMI %s (%#x): a response without a result (%u bytes)\n", what, id, answer_length);
		return ~0u;
	}
	if (!quiet || result[0] | result[1]) {
		say("  QMI %s (%#x): result %u, error %u (%u bytes)\n", what, id, result[0] | result[1] << 8,
		    result[2] | result[3] << 8, answer_length);
	}
	return (uint32_t)(result[0] | result[1] << 8) << 16 | (result[2] | result[3] << 8);
}

/* an indication `id', within `ms' milliseconds; its body copied to `body' */
static uint32_t
qmi_indication(uint16_t id, uint32_t ms, uint8_t *body)
{
	for (uint32_t waited = 0;; waited += 5) {
		for (uint32_t i = 0; i < indication_count; i++) {
			if (indications[i].id == id) {
				uint32_t length = indications[i].length;

				memcpy(body, indications[i].body, length);
				indications[i] = indications[--indication_count];
				return length;
			}
		}
		if (waited >= ms) {
			return ~0u;
		}
		events("waiting");
		wait_ms(5);
	}
}

static uint64_t
radio_map_64k(vm_offset_t va, uint64_t size)
{
	rs.next = (rs.next + 0xffff) & ~0xffffull;
	return radio_map(va, size);
}

/* the radio's memory request: every segment from our memory (64 KB aligned), or none if one cannot be had */
static bool
radio_memory_request(const uint8_t *m, uint32_t length)
{
	static uint8_t reply[1 + 17 * MEM_SEGMENTS];
	static struct tlvs t;
	uint32_t n, count, at = 1, total = 0;
	const uint8_t *s = tlv_find(m, length, 1, &n);
	bool all = true;

	if (s == NULL || n < 1) {
		say("  the memory request has no segments\n");
		return false;
	}
	count = s[0] > MEM_SEGMENTS ? MEM_SEGMENTS : s[0];
	reply[0] = (uint8_t)count;
	for (uint32_t i = 0; i < count && at + 9 <= n; i++) {
		uint32_t size = le32(s + at), type = le32(s + at + 4), configs = s[at + 8];
		uint64_t io = 0;
		vm_offset_t memory = 0;

		at += 9 + 13 * configs;
		total += size;
		if (type != 8 && size != 0 && all) {        /* MLO_GLOBAL: none (as ath12k without MLO) */
			if (kmem_alloc_contig(kernel_map, &memory, (size + 4095) & ~4095u, 0, 0, 0xf,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				all = false;
			} else {
				for (vm_offset_t line = memory; line < memory + size; line += 64) {
					__asm__ volatile ("dc civac, %0" : : "r" (line));
				}
				__asm__ volatile ("dsb sy");
				io = radio_map_64k(memory, (size + 4095) & ~4095u);
				all = io != 0;
			}
		}
		say("  segment %u: type %u, %u KB (%u configs) -> %#llx\n", i, type, size / 1024, configs, (unsigned long long)io);
		memcpy(reply + 1 + 17 * i, &io, 8);
		memcpy(reply + 1 + 17 * i + 8, &size, 4);
		memcpy(reply + 1 + 17 * i + 12, &type, 4);
		reply[1 + 17 * i + 16] = 0;
	}
	if (!all) {
		say("  not all of it could be had (%u KB): none given, the radio is to ask again in smaller pieces\n", total / 1024);
		reply[0] = 0;
		count = 0;
	}
	t.length = 0;
	tlv(&t, 1, reply, 1 + 17 * count);
	return qmi_ask("respond mem", WLFW_RESPOND_MEM, &t, false) == 0 && all;
}

static bool
wlfw_start(void)
{
	static struct tlvs t;
	static uint8_t body[QMI_MAX];
	const uint8_t *v;
	uint32_t n, length;
	bool given = false;

	announce("step 13: QMI with WLFW: phy cap, indications registered, host cap");
	if (qmi_ask("phy cap", WLFW_PHY_CAP, NULL, false) != ~0u) {
		if ((v = tlv_find(answer, answer_length, 0x10, &n)) != NULL) {
			say("  phys: %u\n", v[0]);
		}
		if ((v = tlv_find(answer, answer_length, 0x11, &n)) != NULL && n >= 4) {
			say("  board id (phy cap): %#x\n", le32(v));
		}
	}
	t.length = 0;
	tlv_u8(&t, 0x10, 1);                            /* fw_ready */
	tlv_u32(&t, 0x15, 0x4b4e454c);                  /* client id */
	tlv_u8(&t, 0x16, 1);                            /* request_mem */
	tlv_u8(&t, 0x17, 1);                            /* fw_mem_ready */
	tlv_u8(&t, 0x18, 1);                            /* fw_init_done */
	tlv_u8(&t, 0x1b, 1);                            /* cal_done */
	if (qmi_ask("ind register", WLFW_IND_REGISTER, &t, false) != 0) {
		return false;
	}
	if ((v = tlv_find(answer, answer_length, 0x10, &n)) != NULL && n >= 8) {
		say("  firmware status %#llx\n", (unsigned long long)le64(v));
	}
	t.length = 0;
	tlv_u32(&t, 0x10, 1);                           /* num_clients */
	if (nm_modem != ~0u) {
		tlv_u8(&t, 0x13, (uint8_t)nm_modem);
	}
	tlv_u8(&t, 0x14, 1);                            /* bdf_support */
	tlv_u8(&t, 0x16, 1);                            /* m3_support */
	tlv_u8(&t, 0x17, 1);                            /* m3_cache_support */
	tlv_u8(&t, 0x1a, 0);                            /* cal_done */
	tlv_u8(&t, 0x1c, 0);                            /* mem_cfg_mode */
	tlv_u64(&t, 0x27, 1ull << 3 | 1ull << 4);       /* QDSS_CFG_MISS, PCIE_PERST_NO_PULL */
	if (qmi_ask("host cap", WLFW_HOST_CAP, &t, false) != 0) {
		return false;
	}
	for (unsigned tries = 0; tries < 3 && !given; tries++) {
		length = qmi_indication(WLFW_REQUEST_MEM_IND, 5000, body);
		if (length == ~0u) {
			say("  no memory request from the radio in 5 s\n");
			return false;
		}
		say("step 13: the radio asks for memory\n");
		given = radio_memory_request(body, length);
	}
	if (!given) {
		return false;
	}
	if (qmi_indication(WLFW_FW_MEM_READY_IND, 5000, body) == ~0u) {
		say("  no \"memory ready\" from the radio in 5 s\n");
		return false;
	}
	say("step 13: the radio's memory is ready; its capabilities:\n");
	if (qmi_ask("target cap", WLFW_CAP, NULL, false) != 0) {
		return false;
	}
	if ((v = tlv_find(answer, answer_length, 0x10, &n)) != NULL && n >= 8) {
		say("  chip id %#x, family %#x\n", le32(v), le32(v + 4));
	}
	if ((v = tlv_find(answer, answer_length, 0x11, &n)) != NULL && n >= 4) {
		board_id = le32(v);
	}
	if ((v = tlv_find(answer, answer_length, 0x12, &n)) != NULL && n >= 4) {
		say("  soc id %#x\n", le32(v));
	}
	if ((v = tlv_find(answer, answer_length, 0x13, &n)) != NULL && n >= 5) {
		say("  firmware version %#x, built %.*s\n", le32(v), (int)(v[4] < n - 5 ? v[4] : n - 5), (const char *)v + 5);
	}
	if ((v = tlv_find(answer, answer_length, 0x14, &n)) != NULL) {
		say("  build id %.*s\n", (int)n, (const char *)v);
	}
	if ((v = tlv_find(answer, answer_length, 0x18, &n)) != NULL && n >= 4) {
		say("  OTP version %#x\n", le32(v));
	}
	say("  board id %#x\n", board_id);
	return true;
}

/* a file to the radio through BDF_DOWNLOAD, in pieces */
static bool
bdf_send(const char *name, uint8_t type)
{
	static struct tlvs t;
	uint64_t size = 0;
	const uint8_t *file = hakios_image_file("fw", name, &size, NULL);
	uint32_t remaining, segment = 0;

	if (file == NULL) {
		say("  no fw/%s\n", name);
		return false;
	}
	remaining = size > BOARD_SIZE ? BOARD_SIZE : (uint32_t)size;
	if (type == 1 && !(size >= 4 && file[0] == 0x7f && file[1] == 'E' && file[2] == 'L' && file[3] == 'F')) {
		type = 0;
	}
	say("  fw/%s: %llu bytes (%u sent), type %u\n", name, (unsigned long long)size, remaining, type);
	while (remaining) {
		uint32_t piece = remaining > BDF_CHUNK ? BDF_CHUNK : remaining;
		static uint8_t data[2 + BDF_CHUNK];

		t.length = 0;
		tlv_u8(&t, 0x01, 1);
		tlv_u32(&t, 0x10, board_id);
		tlv_u32(&t, 0x11, remaining);
		tlv_u32(&t, 0x12, segment);
		data[0] = (uint8_t)piece;
		data[1] = (uint8_t)(piece >> 8);
		memcpy(data + 2, file, piece);
		tlv(&t, 0x13, data, 2 + piece);
		tlv_u8(&t, 0x14, piece == remaining);
		tlv_u8(&t, 0x15, type);
		if (qmi_ask("bdf download", WLFW_BDF_DOWNLOAD, &t, true) != 0) {
			say("  piece %u of fw/%s refused\n", segment, name);
			return false;
		}
		file += piece;
		remaining -= piece;
		segment++;
	}
	say("  fw/%s: %u pieces taken\n", name, segment);
	return true;
}

static bool
wlfw_files(void)
{
	static struct tlvs t;
	static uint8_t body[QMI_MAX];
	char bdf[32], regdb[32] = "regdb.bin", m3[32] = "phy_ucode20.elf";
	const uint8_t *file;
	uint64_t size = 0, io;
	vm_offset_t memory = 0;
	uint32_t length;

	if (board_id == 0xff) {
		snprintf(bdf, sizeof(bdf), "bdwlan.elf");
	} else if (board_id < 0xff) {
		snprintf(bdf, sizeof(bdf), "bdwlan.e%02x", board_id);
	} else {
		snprintf(bdf, sizeof(bdf), "bdwlan%02x.e%02x", board_id >> 8 & 0xff, board_id & 0xff);
	}
	PE_parse_boot_argn("wlan_bdf", bdf, sizeof(bdf));
	PE_parse_boot_argn("wlan_regdb", regdb, sizeof(regdb));
	PE_parse_boot_argn("wlan_m3", m3, sizeof(m3));
	announce("step 14: the regulatory database and the board's data through QMI, the M3 firmware's address");
	if (!bdf_send(regdb, 4) || !bdf_send(bdf, 1)) {
		return false;
	}
	file = hakios_image_file("fw", m3, &size, NULL);
	if (file == NULL || size == 0) {
		say("  no fw/%s\n", m3);
		return false;
	}
	if (kmem_alloc_contig(kernel_map, &memory, (size + 4095) & ~4095ull, 0, 0, 0xf, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("  no memory for M3\n");
		return false;
	}
	memcpy((void *)memory, file, size);
	for (vm_offset_t line = memory; line < memory + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	io = radio_map_64k(memory, (size + 4095) & ~4095ull);
	if (io == 0) {
		return false;
	}
	say("  fw/%s: %llu bytes at the radio's %#llx\n", m3, (unsigned long long)size, (unsigned long long)io);
	t.length = 0;
	tlv_u64(&t, 0x01, io);
	tlv_u32(&t, 0x02, (uint32_t)size);
	if (qmi_ask("m3 info", WLFW_M3_INFO, &t, false) != 0) {
		return false;
	}
	length = qmi_indication(WLFW_FW_READY_IND, 10000, body);
	say("step 14: %s\n", length == ~0u ? "NO \"FIRMWARE READY\" FROM THE RADIO IN 10 S" : "THE RADIO'S FIRMWARE IS READY");
	events_until(1000, ~0u, "at the end");          /* whatever else it says */
	smmu_faults("at the end of step 14");
	context_faults("at the end of step 14");
	return length != ~0u;
}

/* ---- the link: QRTR over IPCR as the kernel's transport for the QMI helpers (sys/hakios_qrtr.h) ------ */

/*
 * With wlan_link=1, after step 12 a thread of ours looks at MHI's event rings every millisecond (no
 * interrupts yet) and hands the radio's datagrams to ports opened through hakios_qrtr_ops; the QMI
 * helpers (hakios_qmi.c) and through them ath12k's qmi.c talk to WLFW that way. Everything that
 * touches the rings is under link_lock; the callbacks are called without it, from the link's thread,
 * from a queue the event handling fills (they may send, and sending takes the lock).
 */
#define LINK_PORTS              4
#define LINK_SERVERS            16
#define LINK_QUEUE              16

struct hakios_qrtr_port {
	uint32_t number;                        /* 0: not in use */
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* instance as on the wire: version | instance << 8; 0: any */
		bool asked, fresh;
	} lookups[4];
};

static LCK_GRP_DECLARE(link_group, "shennong-wlan");
static LCK_MTX_DECLARE(link_lock_mutex, &link_group);   /* the rings, the tables below */
static LCK_MTX_DECLARE(link_calling_mutex, &link_group);        /* held by the link's thread around callbacks */
#define link_lock               (&link_lock_mutex)
#define link_calling            (&link_calling_mutex)
static struct hakios_qrtr_port link_ports[LINK_PORTS];
static uint32_t link_next_port = OUR_PORT + 1;
static struct {
	uint32_t service, instance, node, port;
	bool there, fresh, gone;
} link_servers[LINK_SERVERS];
static struct {
	uint32_t node, port, to, length;
	uint8_t data[RX_SIZE];
} link_queue[LINK_QUEUE];
static uint32_t link_in, link_out, link_dropped;
static thread_t link_thread_is;

/* (from transfer_done(), under link_lock) a datagram for one of the link's ports, or the router's own word */
static void
link_received(const struct qrtr_header *h, uint32_t length)
{
	const struct qrtr_control *p = (const struct qrtr_control *)(h + 1);

	if (h->type == QRTR_DATA) {
		if (h->confirm_rx) {
			struct qrtr_control resume = { QRTR_RESUME_TX, QRTR_OUR_NODE, h->dst_port, 0, 0 };

			qrtr_send_from(h->dst_port, QRTR_RESUME_TX, h->src_node, h->src_port, &resume, sizeof(resume));
		}
		if (h->size + sizeof(*h) > length || h->size > RX_SIZE || link_in - link_out >= LINK_QUEUE) {
			link_dropped++;
			return;
		}
		link_queue[link_in % LINK_QUEUE].node = h->src_node;
		link_queue[link_in % LINK_QUEUE].port = h->src_port;
		link_queue[link_in % LINK_QUEUE].to = h->dst_port;
		link_queue[link_in % LINK_QUEUE].length = h->size;
		memcpy(link_queue[link_in % LINK_QUEUE].data, h + 1, h->size);
		link_in++;
		return;
	}
	if (length < sizeof(*h) + sizeof(*p)) {
		return;
	}
	if (p->command == QRTR_NEW_SERVER || p->command == 5) {
		unsigned free = LINK_SERVERS, s;

		for (s = 0; s < LINK_SERVERS; s++) {
			if (link_servers[s].there && link_servers[s].node == p->node && link_servers[s].port == p->port) {
				break;
			}
			if (!link_servers[s].there && free == LINK_SERVERS) {
				free = s;
			}
		}
		if (s == LINK_SERVERS) {
			s = free;
		}
		if (s == LINK_SERVERS) {
			return;
		}
		link_servers[s].service = p->service;
		link_servers[s].instance = p->instance;
		link_servers[s].node = p->node;
		link_servers[s].port = p->port;
		link_servers[s].there = p->command == QRTR_NEW_SERVER;
		link_servers[s].gone = p->command != QRTR_NEW_SERVER;
		link_servers[s].fresh = true;
	}
}

static bool
link_matches(uint32_t service, uint32_t instance, unsigned s)
{
	return link_servers[s].service == service && (instance == 0 || link_servers[s].instance == instance);
}

/* the callbacks for what has come, without link_lock */
static void
link_deliver(void)
{
	lck_mtx_lock(link_calling);
	for (;;) {
		uint32_t to, node, port, length;
		hakios_qrtr_receive_t receive = NULL;
		void *context = NULL;

		lck_mtx_lock(link_lock);
		if (link_out == link_in) {
			lck_mtx_unlock(link_lock);
			break;
		}
		to = link_queue[link_out % LINK_QUEUE].to;
		node = link_queue[link_out % LINK_QUEUE].node;
		port = link_queue[link_out % LINK_QUEUE].port;
		length = link_queue[link_out % LINK_QUEUE].length;
		for (unsigned p = 0; p < LINK_PORTS; p++) {
			if (link_ports[p].number == to && to != 0) {
				receive = link_ports[p].receive;
				context = link_ports[p].context;
			}
		}
		lck_mtx_unlock(link_lock);
		if (receive != NULL) {
			receive(context, node, port, link_queue[link_out % LINK_QUEUE].data, length);
		}
		lck_mtx_lock(link_lock);
		link_out++;
		lck_mtx_unlock(link_lock);
	}
	/* servers that came or went, to the ports that asked for them; new questions answered with what is known */
	for (unsigned p = 0; p < LINK_PORTS; p++) {
		for (unsigned l = 0; l < 4; l++) {
			for (unsigned s = 0; s < LINK_SERVERS; s++) {
				hakios_qrtr_server_t server;
				void *context;
				uint32_t service, instance, node, port;
				bool arrived, tell;

				lck_mtx_lock(link_lock);
				tell = link_ports[p].number != 0 && link_ports[p].lookups[l].asked &&
				    link_matches(link_ports[p].lookups[l].service, link_ports[p].lookups[l].instance, s) &&
				    (link_servers[s].fresh || (link_ports[p].lookups[l].fresh && link_servers[s].there));
				server = link_ports[p].server;
				context = link_ports[p].context;
				service = link_servers[s].service;
				instance = link_servers[s].instance;
				node = link_servers[s].node;
				port = link_servers[s].port;
				arrived = link_servers[s].there;
				lck_mtx_unlock(link_lock);
				if (tell && server != NULL) {
					server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
				}
			}
			lck_mtx_lock(link_lock);
			link_ports[p].lookups[l].fresh = false;
			lck_mtx_unlock(link_lock);
		}
	}
	lck_mtx_lock(link_lock);
	for (unsigned s = 0; s < LINK_SERVERS; s++) {
		link_servers[s].fresh = false;
	}
	lck_mtx_unlock(link_lock);
	lck_mtx_unlock(link_calling);
}

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	link_thread_is = current_thread();
	while (link_wanted) {
		lck_mtx_lock(link_lock);
		events("link");
		lck_mtx_unlock(link_lock);
		link_deliver();
		wait_ms(1);
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static struct hakios_qrtr_port *
link_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct hakios_qrtr_port *port = NULL;

	lck_mtx_lock(link_lock);
	for (unsigned p = 0; p < LINK_PORTS && port == NULL; p++) {
		if (link_ports[p].number == 0) {
			port = &link_ports[p];
			bzero(port, sizeof(*port));
			port->number = link_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(link_lock);
	return port;
}

static int
link_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > TX_SLOT - sizeof(struct qrtr_header)) {
		return EINVAL;
	}
	lck_mtx_lock(link_lock);
	qrtr_send_from(port->number, QRTR_DATA, node, to, data, (uint32_t)length);
	lck_mtx_unlock(link_lock);
	return 0;
}

static int
link_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	lck_mtx_lock(link_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = port->lookups[l].fresh = true;
			error = 0;
		}
	}
	lck_mtx_unlock(link_lock);
	return error;
}

static void
link_close(struct hakios_qrtr_port *port)
{
	bool outside = current_thread() != link_thread_is;

	if (port == NULL || port->number == 0) {
		return;
	}
	if (outside) {
		lck_mtx_lock(link_calling);
	}
	lck_mtx_lock(link_lock);
	port->number = 0;
	lck_mtx_unlock(link_lock);
	if (outside) {
		lck_mtx_unlock(link_calling);
	}
}

static const struct hakios_qrtr_ops link_ops = { .open = link_open, .send = link_send, .lookup = link_lookup, .close = link_close };

static bool
link_start(void)
{
	thread_t thread;

	/* what step 12 heard: the radio's servers */
	if (wlfw_found) {
		link_servers[0] = (typeof(link_servers[0])){ WLFW_SERVICE, wlfw_instance, wlfw_node, wlfw_port, true, false, false };
	}
	link_mode = link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_mode = link_wanted = link_runs = false;
		return false;
	}
	thread_deallocate(thread);
	hakios_qmi_set_transport(&link_ops);
	say("the link: QRTR over IPCR is the QMI helpers' transport now (ports from %#x)\n", OUR_PORT + 1);
	return true;
}

static void
link_stop(void)
{
	if (!link_mode) {
		return;
	}
	hakios_qmi_set_transport(NULL);
	link_wanted = false;
	for (unsigned waited = 0; link_runs && waited < 1000; waited++) {
		wait_ms(1);
	}
	link_mode = false;
	say("the link stopped (%u datagrams dropped)\n", link_dropped);
}

/* ---- step 13 with wlan_link=1: WLFW through the QMI helpers, as ath12k's qmi.c will ask -------------------- */

struct phy_cap_resp {
	struct qmi_response_type_v01 resp;
	uint8_t num_phy_valid;
	uint8_t num_phy;
	uint8_t board_id_valid;
	uint32_t board_id;
};

static const struct qmi_elem_info phy_cap_req_ei[] = { { .data_type = QMI_EOTI } };
static const struct qmi_elem_info phy_cap_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .tlv_type = 0x02,
	  .offset = offsetof(struct phy_cap_resp, resp), .ei_array = qmi_response_type_v01_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = 1, .tlv_type = 0x10, .offset = offsetof(struct phy_cap_resp, num_phy_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = 1, .tlv_type = 0x10, .offset = offsetof(struct phy_cap_resp, num_phy) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = 1, .tlv_type = 0x11, .offset = offsetof(struct phy_cap_resp, board_id_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = 4, .tlv_type = 0x11, .offset = offsetof(struct phy_cap_resp, board_id) },
	{ .data_type = QMI_EOTI },
};

static struct qmi_handle helper_handle;
static volatile bool helper_found;

static int
helper_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct sockaddr_qrtr sq = { AF_QIPCRTR, service->node, service->port };

	say("  helpers: WLFW appeared: service %#x version %u instance %u at %u:%#x\n", service->service, service->version,
	    service->instance, service->node, service->port);
	helper_found = kernel_connect(qmi->sock, &sq, sizeof(sq), 0) == 0;
	return 0;
}

static void
helper_del_server(__unused struct qmi_handle *qmi, __unused struct qmi_service *service)
{
	helper_found = false;
}

static const struct qmi_ops helper_ops = { .new_server = helper_new_server, .del_server = helper_del_server };
static const struct qmi_msg_handler helper_handlers[] = { { 0 } };

static bool
helpers_ask(void)
{
	struct phy_cap_resp resp;
	struct qmi_txn txn;
	int error;

	if (!link_start()) {
		return false;
	}
	announce("step 13: WLFW through the QMI helpers: a handle, the lookup (0x45, version 1, instance 1), phy cap");
	error = qmi_handle_init(&helper_handle, 8192, &helper_ops, helper_handlers);
	if (error < 0) {
		say("  qmi_handle_init: %d\n", error);
		return false;
	}
	error = qmi_add_lookup(&helper_handle, WLFW_SERVICE, 1, 1);
	for (unsigned waited = 0; waited < 2000 && !helper_found; waited += 10) {
		wait_ms(10);
	}
	if (error < 0 || !helper_found) {
		say("  the lookup: %d; WLFW %s\n", error, helper_found ? "found" : "NOT FOUND in 2 s");
		qmi_handle_release(&helper_handle);
		return false;
	}
	bzero(&resp, sizeof(resp));
	error = qmi_txn_init(&helper_handle, &txn, phy_cap_resp_ei, &resp);
	if (error >= 0) {
		error = qmi_send_request(&helper_handle, NULL, &txn, WLFW_PHY_CAP, 0, phy_cap_req_ei, NULL);
		if (error < 0) {
			qmi_txn_cancel(&txn);
		} else {
			error = hakios_qmi_txn_wait_ms(&txn, 5000);
		}
	}
	say("step 13: through the helpers: phy cap %d; result %u, error %u; phys %u%s; board id %#x%s\n", error, resp.resp.result,
	    resp.resp.error, resp.num_phy, resp.num_phy_valid ? "" : " (not given)", resp.board_id,
	    resp.board_id_valid ? "" : " (not given)");
	qmi_handle_release(&helper_handle);
	return error >= 0 && resp.resp.result == 0;
}

/* ---- wlan_driver=1: ath12k (wifi/lx12, linked into the kernel as one object) on what the steps brought up ----- */

/*
 * The driver's side (bsd/dev/redwood_lx.c) asks the phone's code for four things; on this phone they
 * are here. The driver comes in after step 12: the radio powered, MHI in M0, AMSS running, QRTR's
 * HELLO exchanged and the link carrying the QMI helpers. ath12k's qmi.c then does what steps 13 and 14
 * do by hand, from its own memory (shown to the radio by shennong_wlan_radio_map()) and its own files
 * (shennong_wlan_driver_file()).
 */
static bool driver_on, driver_stays;

/* What the radio and the SMMU say of a fault: when MHI says SYS_ERR, and when the driver gives up. */
static void
radio_crashed(const char *when)
{
	uint32_t bhi = rd(bar0 + MHI_BHIOFF);

	say("radio %s: MHI status %#x (state %u%s), control %#x; execution environment %u (%s); BHI error code %#x, debug %#x %#x %#x\n",
	    when, rd(bar0 + MHI_STATUS), rd(bar0 + MHI_STATUS) >> 8 & 0xff, rd(bar0 + MHI_STATUS) & 4 ? ", SYSERR" : "",
	    rd(bar0 + MHI_CTRL), rd(bar0 + bhi + BHI_EXECENV), mhi_ee(rd(bar0 + bhi + BHI_EXECENV)), rd(bar0 + bhi + BHI_ERRCODE),
	    rd(bar0 + bhi + BHI_ERRCODE + 4), rd(bar0 + bhi + BHI_ERRCODE + 8), rd(bar0 + bhi + BHI_ERRCODE + 12));
	smmu_faults(when);
	context_faults(when);
	rddm_read();
}

bool
shennong_wlan_here(void)
{
	return true;   /* (air203+: the boot-arg wlan=N gates this driver; our board is corvette) */
}

/* memory in one physical piece, behind the radio's next addresses (64 KB aligned from 64 KB on, as for the firmware's) */
bool
shennong_wlan_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	uint64_t io;

	lck_mtx_lock(&hakios_smmu_lock);
	if (size >= 0x10000) {
		rs.next = (rs.next + 0xffff) & ~0xffffull;
	}
	io = radio_enter(0, physical, (size + 4095) & ~4095ull);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (io == 0) {
		say("driver: no more of the radio's addresses for %llu bytes (%llu MB handed out)\n", (unsigned long long)size,
		    (unsigned long long)((rs.next - RADIO_ADDRESSES) >> 20));
		return false;
	}
	*address = io;
	return true;
}

/* the same without a word, for shennong_bt.c, which asks again and again */
bool
shennong_wlan_driver_up(void)
{
	return driver_on && link_mode;
}

/* the driver's question for the firmware's processor: AMSS runs and QRTR has its link */
bool
shennong_wlan_link_up(void)
{
	say("driver: it asks for the radio's firmware: %s\n", driver_on && link_mode ? "AMSS runs, QRTR's link is up" :
	    "NOT UP");
	return driver_on && link_mode;
}

/*
 * ath12k asks for files by the names of Linux's firmware tree ("ath12k/WCN7850/hw2.0/board.bin");
 * this phone has what Android's driver loads (fw/, from the phone's partitions): the board's data
 * (bdwlan.elf: the board id is 0xff; wlan_bdf=NAME for another), the regulatory database and M3.
 * The others it asks for first and can do without (board-2.bin, firmware-2.bin) are not there;
 * wlan_board2=NAME gives it Linux's board-2.bin (boards by name, chosen by ath12k itself) from fw/.
 */
const uint8_t *
shennong_wlan_driver_file(const char *name, uint64_t *size)
{
	static char bdf[32] = "bdwlan.elf", regdb[32] = "regdb.bin", m3[32] = "phy_ucode20.elf", board2[32] = "";
	const char *last = strrchr(name, '/');
	const char *ours = NULL;

	last = last == NULL ? name : last + 1;
	PE_parse_boot_argn("wlan_bdf", bdf, sizeof(bdf));
	PE_parse_boot_argn("wlan_regdb", regdb, sizeof(regdb));
	PE_parse_boot_argn("wlan_m3", m3, sizeof(m3));
	PE_parse_boot_argn("wlan_board2", board2, sizeof(board2));
	if (strcmp(last, "board-2.bin") == 0 && board2[0] != 0) {
		ours = board2;
	} else if (strcmp(last, "board.bin") == 0) {
		ours = bdf;
	} else if (strcmp(last, "regdb.bin") == 0) {
		ours = regdb;
	} else if (strcmp(last, "m3.bin") == 0) {
		ours = m3;
	}
	return ours == NULL ? NULL : hakios_image_file("fw", ours, size, NULL);
}

static bool
driver_start(void)
{
	int error;

	if (!link_start()) {
		return false;
	}
	announce("step 13: the driver, ath12k: its exchange with WLFW, the copy engines, HTC and WMI");
	driver_on = keep_lines = true;
	error = hakios_lx_start();
	if (error != 0) {
		radio_crashed("after the driver");
	}
	driver_stays = error == 0 && (hakios_wifi_option("wifi_station") || hakios_wifi_option("wifi_join"));
	say("step 13: the driver: %d%s\n", error, driver_stays ? " (it stays up: the radio stays powered)" : "");
	if (!driver_stays) {
		driver_on = false;
	}
	return error == 0;
}

/* ---- the steps ------------------------------------------------------------------------------------------ */

static bool
map_all(void)
{
	gcc_10000 = ml_io_map(GCC_PHYS + 0x10000, 0x1000);
	gcc_52000 = ml_io_map(GCC_PHYS + 0x52000, 0x1000);
	gcc_6b000 = ml_io_map(GCC_PHYS + 0x6b000, 0x2000);
	tcsr_clkref = ml_io_map(TCSR_CLKREF_PHYS & ~0xfffUL, 0x1000) + (TCSR_CLKREF_PHYS & 0xfff);
	pin_page[0] = ml_io_map(TLMM_PHYS + 0x1000UL * 16, 0x4000);
	pin_page[1] = ml_io_map(TLMM_PHYS + 0x1000UL * PIN_PERST, 0x1000);
	pin_page[2] = ml_io_map(TLMM_PHYS + 0x1000UL * PIN_CLKREQ, 0x1000);
	pin_page[3] = ml_io_map(TLMM_PHYS + 0x1000UL * PIN_WAKE, 0x1000);
	parf = ml_io_map(PARF_PHYS, PARF_SIZE);
	phy = ml_io_map(PHY_PHYS, PHY_SIZE);
	dbi = ml_io_map(DBI_PHYS, 0x1000);
	atu = ml_io_map(ATU_PHYS, 0x1000);
	conf = ml_io_map(CONF_PHYS, 0x1000);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);     /* memory, not registers: no alignment rules */
	smmu = ml_io_map(SMMU_PHYS, 0x1000);
	return smmu && gcc_10000 && gcc_52000 && gcc_6b000 && tcsr_clkref && pin_page[0] && pin_page[1] && pin_page[2] && pin_page[3] &&
	       parf && phy && dbi && atu && conf && rsc && cmd_db;
}

static bool
steps(void)
{
	bool running, fine;

	say("step 1: the pins, GCC, TCSR, RPMh (reading only)\n");
	pins_show();
	running = gcc_show();
	if (running) {
		pcie_show("as found");
	} else {
		say("  (the controller's clocks are stopped: it is not read)\n");
	}
	if (!rpmh_setup() || !supplies_show()) {
		say("step 1: RPMh does not have everything the radio needs: stopping\n");
		return false;
	}
	if (last_step < 2) {
		return true;
	}

	announce(volt ? "step 2: RPMh: the supplies on, at the device tree's voltages; the reference clock on" :
	    "step 2: RPMh: the supplies on (the switched-mode ones at their voltages as they are); the reference clock on");
	if (!supplies_on()) {
		return false;
	}
	announce("step 2: PERST# (GPIO 94) low, then wlan-en (GPIO 16) high");
	pin_set(PIN_PERST, CFG_DRIVE_MA(2) | CFG_PULL_DOWN | CFG_OUTPUT, 0);
	pin_set(PIN_WLAN_EN, CFG_DRIVE_MA(16) | CFG_PULL_UP | CFG_OUTPUT, 1);
	powered_at = mach_absolute_time();
	radio_powered = true;
	wait_ms(50);            /* pwrseq-qcom-wcn.c: WCN7850's pwup_delay_ms */
	say("step 2: 50 ms later:\n");
	pin_show(PIN_WLAN_EN, "wlan-en");
	pin_show(PIN_SW_CTRL, "sw-ctrl");
	pin_show(PIN_WLAN_SW_CTRL, "wl-sw-ctrl");
	if (last_step < 3) {
		return true;
	}

	announce("step 3: GCC: PCIe0's power domains, clocks, the reference clock; the controller and its PHY reset");
	if (!pcie_power_on()) {
		gcc_show();
		return false;
	}
	gcc_show();
	pcie_show("after the reset");
	if (last_step < 4) {
		return true;
	}

	if (!pcie_link_up()) {
		return false;
	}
	if (last_step < 5) {
		return true;
	}
	if (!radio_show()) {
		return false;
	}
	if (last_step < 6) {
		return true;
	}
	if (!radio_registers()) {
		return false;
	}
	if (last_step < 7) {
		return true;
	}
	smmu_show();
	if (last_step < 8) {
		return true;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	fine = smmu_on();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!fine || last_step < 9) {
		return fine;
	}
	fine = radio_sbl();
	if (!fine || last_step < 10) {
		return fine;
	}
	fine = mhi_host();
	if (!fine || last_step < 11) {
		return fine;
	}
	fine = radio_amss();
	if (!fine || last_step < 12) {
		return fine;
	}
	fine = qrtr_hello();
	if (!fine || last_step < 13) {
		return fine;
	}
	if (driver_wanted) {
		return driver_start();
	}
	if (link_wanted_option) {
		return helpers_ask();
	}
	fine = wlfw_start();
	if (!fine || last_step < 14) {
		return fine;
	}
	return wlfw_files();
}

static void
wlan_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool fine;

	say("shennong, steps 1 to %u (pause %u ms before writes)\n", last_step, pause_ms);
	if (!map_all()) {
		say("cannot map the registers\n");
		thread_terminate(current_thread());
		return;
	}
	fine = steps();
	if (driver_stays) {
		say("steps over: the driver is up, on the link; the radio stays powered until the run's end\n");
		thread_terminate(current_thread());
		return;
	}
	link_stop();
	if (radio_powered && !keep) {
		say("steps over%s: PERST# low, wlan-en low (the radio unpowered)%s\n", fine ? "" : " (stopped early)",
		    rs.on ? "; the SMMU put back as it was found" : "");
		radio_off();
		lck_mtx_lock(&hakios_smmu_lock);
		smmu_off();
		lck_mtx_unlock(&hakios_smmu_lock);
	} else {
		say("steps over%s%s\n", fine ? "" : " (stopped early)", radio_powered ? "; the radio stays powered (wlan_keep=1)" : "");
	}
	thread_terminate(current_thread());
}

void
shennong_wlan_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("wlan", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	last_step = last_step > 14 ? 14 : last_step;
	PE_parse_boot_argn("wlan_nm", &nm_modem, sizeof(nm_modem));
	PE_parse_boot_argn("wlan_link", &link_wanted_option, sizeof(link_wanted_option));
	PE_parse_boot_argn("wlan_driver", &driver_wanted, sizeof(driver_wanted));
	PE_parse_boot_argn("wlan_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("wlan_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("wlan_volt", &volt, sizeof(volt));
	PE_parse_boot_argn("wlan_tcs", &our_tcs, sizeof(our_tcs));
	our_tcs = our_tcs > 2 ? 2 : our_tcs;
	if (kernel_thread_start(wlan_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the radio unpowered, if it was powered. */
void
shennong_wlan_quiesce(void)
{
	if (keep_lines) {
		hakios_lx_replay();
	}
	if (radio_powered) {
		leaving = true;
		radio_off();
		smmu_off();             /* (no lock: on the way out nothing else runs) */
	}
}
