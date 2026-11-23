/*
 * The radio's bus for ath12k, for as long as ath12k's own (pci.c, mhi.c and wifi7/pci.c) is not
 * built: what stands between the driver's core and the Xiaomi 14 Pro's WCN7850 on PCIe0. The
 * kernel's bsd/dev/shennong_wlan.c has done what pci.c and mhi.c would by the time the driver
 * starts: the radio is powered, its link is up, its memory is behind the SMMU, MHI is in M0 with
 * the firmware running (AMSS), and IPCR's channels carry QRTR -- which the kernel's QMI helpers
 * (sys/hakios.qmi.h) send the driver's QMI over. So power_up() here only asks whether all that
 * is so, and the driver's qmi.c then has its exchange with the firmware itself (host cap, the
 * memory, the board's data, M3, "ready").
 *
 * What this file does in pci.c's place, in its order: the core allocated, the chip's version
 * read through the register window (wifi7/pci.c: ath12k_wifi7_pci_probe() for WCN7850), the
 * hardware's parameters (ath12k_wifi7_hw_init()), the rings and copy engines (as
 * ath12k_pci_probe() does), and the core's start. The chip's registers are read and written
 * as pci.c does (the window of 0x310c for anything past the first 512 KB); there are no
 * interrupts: a thread looks at the copy engines and, once the data path's rings are set up,
 * at their groups, every millisecond.
 *
 * And what the layer (wifi/lx) lacks for ath12k: a handful of LinuxKPI's and FreeBSD's functions.
 */
#define LX_QMI_AS_IT_IS				/* (this file is where the driver's requests go past) */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/printk.h>
#include <linux/mm.h>
#include <net/cfg80211.h>
#include <sys/time.h>

#include "core.h"
#include "ce.h"
#include "debug.h"
#include "dp.h"
#include "hal.h"
#include "hif.h"
#include "htc.h"
#include "qmi.h"
#include "wifi7/core.h"
#include "wifi7/hw.h"

#include <lx_os.h>
#include "lx.h"

/* The radio's BAR0, where the kernel (shennong_wlan.c, step 5) put it in PCIe0's window. */
#define RADIO_BAR0		0x60400000ull
#define RADIO_BAR0_SIZE		0x200000ul

/* pci.c and wifi7/pci.c, for WCN7850 */
#define WINDOW_ENABLE_BIT	0x40000000
#define WINDOW_VALUE_MASK	GENMASK(24, 19)
#define WINDOW_START		0x80000
#define WINDOW_RANGE_MASK	GENMASK(18, 0)
#define WINDOW_REG_ADDRESS	0x310c
#define PCI_MHIREGLEN_REG	0x1E0E100
#define PCI_MHI_REGION_END	0x1E0EFFC
#define TCSR_SOC_HW_VERSION	0x1B00000
#define WCN7850_DEVICE_ID	0x1107
#define QCOM_VENDOR_ID		0x17cb

#define DATA_BUDGET		64

static struct {
	spinlock_t window_lock;
	u32 window;				/* what the window register holds */
	unsigned reads, writes;
	volatile bool looking;			/* the thread that looks at the copy engines is wanted ... */
	volatile bool looks;			/* ... and runs */
	volatile bool ce;			/* ... and looks at the copy engines (the driver's "CE interrupts on") */
	volatile bool data;			/* ... and at the data path's rings as well */
	volatile unsigned passes;		/* how many times it has looked */
	volatile bool stopped;			/* the driver has stopped the bus: its start has failed, or it is on its way down */
	struct mutex service;			/* the thread's looking, and the driver's calls that must not cross it */
} bus;

/* ---- the chip's registers ---------------------------------------------------------------------- */

static void
window_select(struct ath12k_base *ab, u32 offset)
{
	u32 window = u32_get_bits(offset, WINDOW_VALUE_MASK);

	if (window != bus.window) {
		iowrite32(WINDOW_ENABLE_BIT | window, (char *)ab->mem + WINDOW_REG_ADDRESS);
		(void)ioread32((char *)ab->mem + WINDOW_REG_ADDRESS);
		bus.window = window;
	}
}

static bool
in_mhi_region(u32 offset)
{
	return (offset >= PCI_MHIREGLEN_REG && offset <= PCI_MHI_REGION_END);
}

/* pci.c: ath12k_pci_read32(), with the dynamic window only (WCN7850 has no static map); no MHI wakeup: MHI stays in M0 */
static u32
bus_read32(struct ath12k_base *ab, u32 offset)
{
	u32 value;

	bus.reads++;
	if (offset < WINDOW_START) {
		return (ioread32((char *)ab->mem + offset));
	}
	spin_lock_bh(&bus.window_lock);
	window_select(ab, offset);
	if (in_mhi_region(offset)) {
		value = ioread32((char *)ab->mem + ((offset - PCI_MHIREGLEN_REG) & WINDOW_RANGE_MASK));
	} else {
		value = ioread32((char *)ab->mem + WINDOW_START + (offset & WINDOW_RANGE_MASK));
	}
	spin_unlock_bh(&bus.window_lock);
	return (value);
}

static void
bus_write32(struct ath12k_base *ab, u32 offset, u32 value)
{
	bus.writes++;
	if (offset < WINDOW_START) {
		iowrite32(value, (char *)ab->mem + offset);
		return;
	}
	spin_lock_bh(&bus.window_lock);
	window_select(ab, offset);
	if (in_mhi_region(offset)) {
		iowrite32(value, (char *)ab->mem + ((offset - PCI_MHIREGLEN_REG) & WINDOW_RANGE_MASK));
	} else {
		iowrite32(value, (char *)ab->mem + WINDOW_START + (offset & WINDOW_RANGE_MASK));
	}
	spin_unlock_bh(&bus.window_lock);
}

/* ---- the thread that stands for the radio's interrupts -------------------------------------------- */

static bool
group_has_rings(struct ath12k_base *ab, unsigned i)
{
	const struct ath12k_hw_ring_mask *mask = ab->hw_params->ring_mask;

	return (mask->tx[i] || mask->rx[i] || mask->rx_err[i] || mask->rx_wbm_rel[i] || mask->reo_status[i] ||
	    mask->host2rxdma[i] || mask->rx_mon_dest[i] || mask->rx_mon_status[i]);
}

static void
bus_looks(void *argument)
{
	struct ath12k_base *ab = argument;

	bus.looks = true;
	while (bus.looking) {
		mutex_lock(&bus.service);
		if (bus.ce) {
			for (unsigned i = 0; i < ab->hw_params->ce_count; i++) {
				if (!(ath12k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)) {
					ath12k_ce_per_engine_service(ab, (u16)i);
				}
			}
		}
		if (bus.data && ab->dp != NULL) {
			for (unsigned i = 0; i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++) {
				if (group_has_rings(ab, i)) {
					while (bus.data && ath12k_dp_service_srng(ath12k_ab_to_dp(ab), &ab->ext_irq_grp[i], DATA_BUDGET) >=
					    DATA_BUDGET) {
					}
				}
			}
		}
		bus.passes++;
		mutex_unlock(&bus.service);
		if (bus.passes % 1000 == 0) {
			lx_station_tick();
		}
		lx_pause_ms(1);
	}
	bus.looks = false;
}

/* The thread passes once more after this: whatever it was doing with what was just turned off is over. */
static void
bus_settle(void)
{
	unsigned passes = bus.passes;

	for (unsigned waited = 0; bus.looks && bus.passes - passes < 2 && waited < 1000; waited++) {
		lx_pause_ms(1);
	}
}

static void
bus_quiet(void)
{
	bus.data = false;
	bus.ce = false;
	bus.looking = false;
	while (bus.looks) {
		lx_pause_ms(1);
	}
}

/* ---- the bus's operations ------------------------------------------------------------------------- */

/* pci.c: ath12k_pci_start(), with the thread where it enables the copy engines' interrupts */
static int
bus_start(struct ath12k_base *ab)
{
	printf("lx12: the driver starts the bus: buffers for the copy engines that receive (%u reads and %u writes of the chip's "
	    "registers so far)\n", bus.reads, bus.writes);
	bus.ce = true;
	ath12k_ce_rx_post_buf(ab);
	if (!bus.looks) {
		bus.looking = true;
		if (lx_os_thread("wifi-rings", bus_looks, ab) != 0) {
			bus.looking = bus.ce = false;
			return (-ENOMEM);
		}
	}
	lx_tell("lx12: the copy engines have their buffers and are looked at every millisecond: the bus is started\n");
	return (0);
}

/* pci.c: ath12k_pci_stop() */
static void
bus_stop(struct ath12k_base *ab)
{
	bus.stopped = true;
	bus_quiet();
	ath12k_ce_cleanup_pipes(ab);
	printf("lx12: the driver stops the bus (%u reads and %u writes of the chip's registers in all)\n", bus.reads, bus.writes);
}

static void
bus_ce_irq_enable(struct ath12k_base *ab)
{
	bus.ce = true;
}

static void
bus_ce_irq_disable(struct ath12k_base *ab)
{
	bus.ce = false;
	bus_settle();
}

/* pci.c: ath12k_pci_ext_irq_enable(): the data path's rings are set up, and are looked at from here on */
static void
bus_irq_enable(struct ath12k_base *ab)
{
	bus.data = true;
}

static void
bus_irq_disable(struct ath12k_base *ab)
{
	bus.data = false;
	bus_settle();
}

/* pci.c: ath12k_pci_map_service_to_pipe(): which copy engines carry a service, by the chip's table */
static int
bus_map_service_to_pipe(struct ath12k_base *ab, u16 service, u8 *up, u8 *down)
{
	bool has_up = false, has_down = false;

	for (u32 i = 0; i < ab->hw_params->svc_to_ce_map_len; i++) {
		const struct service_to_pipe *entry = &ab->hw_params->svc_to_ce_map[i];
		u32 direction = __le32_to_cpu(entry->pipedir);

		if (__le32_to_cpu(entry->service_id) != service) {
			continue;
		}
		if (direction == PIPEDIR_IN || direction == PIPEDIR_INOUT) {
			*down = (u8)__le32_to_cpu(entry->pipenum);
			has_down = true;
		}
		if (direction == PIPEDIR_OUT || direction == PIPEDIR_INOUT) {
			*up = (u8)__le32_to_cpu(entry->pipenum);
			has_up = true;
		}
	}
	return (has_up && has_down ? 0 : -ENOENT);
}

/*
 * pci.c: ath12k_pci_power_up() resets the chip and has MHI load and start its firmware. Here the
 * kernel has done that before the driver started; this asks whether the firmware runs and QRTR
 * has its link (sys/hakios.lx.h: lx_os_rproc_boot()).
 */
static int
bus_power_up(struct ath12k_base *ab)
{
	int error = lx_os_rproc_boot();

	lx_tell("lx12: the driver asks for the radio's power and firmware: %s\n", error == 0 ? "the firmware runs (AMSS), "
	    "with QRTR's link" : "NOT THERE");
	bus.window = 0;
	return (-error);
}

/* (the kernel takes the radio's power once the driver is down: shennong_wlan.c) */
static void
bus_power_down(struct ath12k_base *ab, bool is_suspend)
{
	bus_quiet();
}

static const struct ath12k_hif_ops bus_ops = {
	.read32 = bus_read32,
	.write32 = bus_write32,
	.irq_enable = bus_irq_enable,
	.irq_disable = bus_irq_disable,
	.start = bus_start,
	.stop = bus_stop,
	.power_up = bus_power_up,
	.power_down = bus_power_down,
	.map_service_to_pipe = bus_map_service_to_pipe,
	.ce_irq_enable = bus_ce_irq_enable,
	.ce_irq_disable = bus_ce_irq_disable,
	/* (no interrupt vectors are offered: the rings are then set up without any, see ce.c and dp.c) */
};

/*
 * A request of the driver's to the firmware, on its way out (linux/soc/qcom/qmi.h, ours): any
 * mode but mission, and the copy engines are let go of first, as lx_bus.c does for ath11k.
 */
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == QMI_WLANFW_WLAN_MODE_REQ_V01) {
		u32 mode = ((const struct qmi_wlanfw_wlan_mode_req_msg_v01 *)structure)->mode;

		if (mode != ATH12K_FIRMWARE_MODE_NORMAL) {
			bus.ce = false;
			bus.data = false;
			bus_settle();
		}
		lx_tell("lx12: the firmware is asked for mode %u (0: mission, 4: off)\n", mode);
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- the entries ---------------------------------------------------------------------------------- */

static struct ath12k_base *volatile driver;	/* the driver, while it is up: between lx_start() and lx_stop() */

static void
say_radio(struct ath12k_base *ab)
{
	const struct ath12k_wmi_target_cap_arg *t = &ab->target_caps;

	printf("lx12: THE FIRMWARE SAYS OF THE RADIO: %u radio(s); %u chains (mask %#x); phy %#x, HT %#x, VHT %#x, system %#x; "
	    "its address %02x:%02x:%02x:%02x:%02x:%02x\n", ab->num_radios, t->num_rf_chains, t->txrx_chainmask,
	    t->phy_capability, t->ht_cap_info, t->vht_cap_info, t->sys_cap_info, ab->mac_addr[0], ab->mac_addr[1],
	    ab->mac_addr[2], ab->mac_addr[3], ab->mac_addr[4], ab->mac_addr[5]);
}

int
lx_start(void)
{
	static struct device device;
	struct ath12k_base *ab;
	unsigned long long began;
	u32 version;
	bool up;
	int ret;

	if (lx_prepare() != 0) {
		lx_tell("lx12: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	ath12k_debug_mask = lx_os_option("wifi_quiet") ? 0 : ATH12K_DBG_QMI | ATH12K_DBG_BOOT | ATH12K_DBG_WMI | ATH12K_DBG_REG |
	    ATH12K_DBG_MAC | ATH12K_DBG_PCI;
	if (lx_os_option("wifi_debug")) {
		ath12k_debug_mask = lx_os_option("wifi_debug");
	}
	spin_lock_init(&bus.window_lock);
	mutex_init(&bus.service);
	device.kobj.name = "wifi";

	/* pci.c: ath12k_pci_probe() */
	ab = ath12k_core_alloc(&device, 0, ATH12K_BUS_PCI);
	if (ab == NULL) {
		return (ENOMEM);
	}
	ab->dev = &device;
	ab->hif.ops = &bus_ops;
	ab->fw_mode = ATH12K_FIRMWARE_MODE_NORMAL;
	ab->mem = lx_os_map_registers(RADIO_BAR0, RADIO_BAR0_SIZE);
	ab->mem_len = RADIO_BAR0_SIZE;
	if (ab->mem == NULL) {
		lx_tell("lx12: the radio's registers cannot be mapped\n");
		return (ENOMEM);
	}
	ab->id.vendor = QCOM_VENDOR_ID;
	ab->id.device = WCN7850_DEVICE_ID;
	/*
	 * The subsystem ids Linux reads from the configuration space: Qualcomm's own (17cb:1107), as on
	 * their reference boards -- the name board-2.bin knows with board id 255 (the phone's).
	 */
	ab->id.subsystem_vendor = QCOM_VENDOR_ID;
	ab->id.subsystem_device = WCN7850_DEVICE_ID;

	/* wifi7/pci.c: ath12k_wifi7_pci_probe(), for WCN7850 */
	ab->id.bdf_search = ATH12K_BDF_SEARCH_BUS_AND_BOARD;
	ab->static_window_map = false;
	ab->target_mem_mode = ATH12K_QMI_MEMORY_MODE_DEFAULT;
	version = bus_read32(ab, TCSR_SOC_HW_VERSION);
	printf("lx12: the chip says it is version %u.%u (%#x)\n", u32_get_bits(version, GENMASK(11, 8)),
	    u32_get_bits(version, GENMASK(7, 4)), version);
	if (u32_get_bits(version, GENMASK(11, 8)) != 2) {
		lx_tell("lx12: a WCN7850 of version %u: ath12k knows version 2 only; taken as 2\n", u32_get_bits(version,
		    GENMASK(11, 8)));
	}
	ab->hw_rev = ATH12K_HW_WCN7850_HW20;
	if ((ret = ath12k_wifi7_hw_init(ab)) != 0) {
		lx_tell("lx12: the driver does not know the chip: %d\n", ret);
		return (-ret);
	}

	/* back in ath12k_pci_probe() */
	if ((ret = ath12k_core_pre_init(ab)) != 0 || (ret = ath12k_hal_srng_init(ab)) != 0 ||
	    (ret = ath12k_ce_alloc_pipes(ab)) != 0) {
		lx_tell("lx12: the driver does not get as far as its exchange with the firmware: %d\n", ret);
		return (-ret);
	}
	ab->qmi.ce_cfg.tgt_ce = ab->hw_params->target_ce_config;
	ab->qmi.ce_cfg.tgt_ce_len = ab->hw_params->target_ce_count;
	ab->qmi.ce_cfg.svc_to_ce_map = ab->hw_params->svc_to_ce_map;
	ab->qmi.ce_cfg.svc_to_ce_map_len = ab->hw_params->svc_to_ce_map_len;
	ab->qmi.service_ins_id = ab->hw_params->qmi_service_ins_id;
	for (unsigned i = 0; i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++) {
		ab->ext_irq_grp[i].ab = ab;
		ab->ext_irq_grp[i].grp_id = i;
		ab->ext_irq_grp[i].num_irq = group_has_rings(ab, i) ? 1 : 0;
	}
	if ((ret = ath12k_wifi7_arch_init(ab)) != 0) {
		lx_tell("lx12: the data path cannot be had: %d\n", ret);
		return (-ret);
	}
	printf("lx12: the driver (\"%s\") is set up; its exchange with the firmware begins (QMI instance %#x)\n",
	    ab->hw_params->name, ab->qmi.service_ins_id);
	if ((ret = ath12k_core_init(ab)) != 0) {
		lx_tell("lx12: the driver's start fails: %d\n", ret);
		return (-ret);
	}

	/* the rest happens on the driver's own threads, until the core has registered the radio or the exchange has failed */
	began = lx_os_nanoseconds();
	while (!test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) && !test_bit(ATH12K_FLAG_QMI_FAIL, &ab->dev_flags) &&
	    !bus.stopped && lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) && bus.looks;
	lx_tell("lx12: %llu ms after its start the driver %s; its flags: %#lx\n", (lx_os_nanoseconds() - began) / 1000000ull,
	    up ? "IS UP: the firmware has said \"unified ready\" and the radio is registered" :
	    test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) ? "HAS GIVEN UP on the way" :
	    "HAS NOT GOT THROUGH its exchange with the firmware", ab->dev_flags);
	if (!up) {
		lx_pause_ms(600);
		bus_quiet();
		return (test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) ? ETIMEDOUT : EIO);
	}
	lx_pause_ms(1000);			/* (for what the firmware says unasked once it is ready) */
	say_radio(ab);
	driver = ab;
	if (lx_os_option("wifi_station")) {
		if ((ret = lx_station_offer()) == 0) {
			return (0);
		}
	} else if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) == 0) {
			if (lx_os_option("wifi_hold")) {
				lx_pause_ms(1000 * lx_os_option("wifi_hold"));
				lx_station_say();
			}
			return (0);
		}
	} else {
		ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
	}
	lx_stop();
	return (ret);
}

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

void
lx_stop(void)
{
	struct ath12k_base *ab = driver;

	if (ab == NULL) {
		return;
	}
	driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	ath12k_core_deinit(ab);
	bus_quiet();
	lx_tell("lx12: the driver is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}

/* ---- the device tree, of which there is none ------------------------------------------------------ */

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

const struct property *
of_find_property(const struct device_node *np, const char *name, int *length)
{
	if (length != NULL) {
		*length = 0;
	}
	return (NULL);
}

struct device_node *
of_parse_phandle(const struct device_node *np, const char *name, int index)
{
	return (NULL);
}

/* ---- what LinuxKPI and FreeBSD have that the layer lacks for ath12k --------------------------------- */

int
device_printf(device_t dev, const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	lx_vformat(line, sizeof(line), format, list);
	va_end(list);
	lx_os_print(line);
	return (0);
}

/*
 * Rate limits (dev_warn_ratelimited(), printk_ratelimited()): at most "most" a second from each
 * place that asks. Everything used to be said; then ath12k's "Received with invalid mcs in VHT
 * mode 11" (an access point sending VHT MCS 10 and 11, which the driver's table does not have)
 * came once a frame received, 27000 of a run's 30000 lines, each printed on the receiving thread.
 */
int
eventratecheck(struct timeval *last, int *count, int most)
{
	long now = (long)(lx_os_nanoseconds() / 1000000000ull);

	if (last->tv_sec != now) {
		last->tv_sec = now;
		*count = 0;
	}
	return (most < 0 || ++*count <= most);
}

/* the mutexes LinuxKPI declares statically: a zeroed one is a free one here (lx_base.c) */
void
sx_sysinit(const void *argument)
{
}

void
si_meminfo(struct sysinfo *si)
{
	memset(si, 0, sizeof(*si));
	si->totalram = (16ul << 30) / PAGE_SIZE;
	si->freeram = (8ul << 30) / PAGE_SIZE;
	si->mem_unit = 1;
}

/* the bounds of FreeBSD's section of per-processor data, by the names its headers give them in asm: there is none */
uintptr_t *lx12_pcpu_start __asm("__start_set_pcpu");
uintptr_t *lx12_pcpu_stop __asm("__stop_set_pcpu");

struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	unsigned int headroom = skb_headroom(skb);
	struct sk_buff *copy = dev_alloc_skb(headroom + skb->len);

	if (copy == NULL) {
		return (NULL);
	}
	skb_reserve(copy, headroom > NET_SKB_PAD ? headroom - NET_SKB_PAD : 0);
	memcpy(skb_put(copy, skb->len), skb->data, skb->len);
	memcpy(copy->cb, skb->cb, sizeof(copy->cb));
	copy->protocol = skb->protocol;
	copy->priority = skb->priority;
	copy->dev = skb->dev;
	return (copy);
}

int
lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii)
{
	const uint8_t *bytes = buf;
	size_t at = 0, n = len < (size_t)rowsize ? len : (size_t)rowsize;

	if (linebuflen == 0) {
		return (0);
	}
	linebuf[0] = 0;
	for (size_t i = 0; i < n && at + 4 < linebuflen; i++) {
		at += snprintf(linebuf + at, linebuflen - at, "%s%02x", i == 0 ? "" : " ", bytes[i]);
	}
	if (ascii && at + n + 3 < linebuflen) {
		linebuf[at++] = ' ';
		linebuf[at++] = ' ';
		for (size_t i = 0; i < n; i++) {
			linebuf[at++] = bytes[i] >= 0x20 && bytes[i] < 0x7f ? (char)bytes[i] : '.';
		}
		linebuf[at] = 0;
	}
	return ((int)at);
}

/*
 * cfg80211's work for a radio: run in order, one at a time, with the radio's lock held, on a
 * work item of the system's queue (there is one radio here).
 */
static LIST_HEAD(wiphy_works);
static struct wiphy *wiphy_of_works;
static spinlock_t wiphy_works_lock;
static bool wiphy_works_ready;
static struct work_struct wiphy_works_run;
static struct workqueue_struct *wiphy_works_queue;

static void
wiphy_works_runs(struct work_struct *work)
{
	for (;;) {
		struct wiphy_work *item = NULL;

		wiphy_lock(wiphy_of_works);
		spin_lock_bh(&wiphy_works_lock);
		if (!list_empty(&wiphy_works)) {
			item = list_first_entry(&wiphy_works, struct wiphy_work, entry);
			list_del_init(&item->entry);
		}
		spin_unlock_bh(&wiphy_works_lock);
		if (item != NULL) {
			item->fn(wiphy_of_works, item);
		}
		wiphy_unlock(wiphy_of_works);
		if (item == NULL) {
			break;
		}
	}
}

static void
wiphy_works_init(void)
{
	if (!wiphy_works_ready) {
		spin_lock_init(&wiphy_works_lock);
		INIT_WORK(&wiphy_works_run, wiphy_works_runs);
		wiphy_works_queue = alloc_ordered_workqueue("wiphy-work", 0);
		wiphy_works_ready = true;
	}
}

void
linuxkpi_wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work)
{
	wiphy_works_init();
	spin_lock_bh(&wiphy_works_lock);
	wiphy_of_works = wiphy;
	if (list_empty(&work->entry)) {
		list_add_tail(&work->entry, &wiphy_works);
	}
	spin_unlock_bh(&wiphy_works_lock);
	queue_work(wiphy_works_queue, &wiphy_works_run);
}

void
linuxkpi_wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work)
{
	wiphy_works_init();
	spin_lock_bh(&wiphy_works_lock);
	if (!list_empty(&work->entry)) {
		list_del_init(&work->entry);
	}
	spin_unlock_bh(&wiphy_works_lock);
}

/* (called with the radio's lock held: what is queued is run here, by the caller) */
void
linuxkpi_wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work)
{
	wiphy_works_init();
	for (;;) {
		struct wiphy_work *item = NULL;

		spin_lock_bh(&wiphy_works_lock);
		if (work != NULL ? !list_empty(&work->entry) : !list_empty(&wiphy_works)) {
			item = work != NULL ? work : list_first_entry(&wiphy_works, struct wiphy_work, entry);
			list_del_init(&item->entry);
		}
		spin_unlock_bh(&wiphy_works_lock);
		if (item == NULL) {
			break;
		}
		item->fn(wiphy, item);
	}
}
