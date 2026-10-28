/*
 * The touch screen of the POCO X5 Pro (redwood) and of the OnePlus Ace 3 Pro (corvette): either a
 * Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 on an SPI bus, as a source of touches for
 * /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=1, and the addresses of
 * each board are its row of the table below (boot-arg touch_board=1: the corvette).
 *
 * There is no clock, pin, regulator or SPI framework in this kernel, so the driver does all of
 * it itself, in the order the vendor's Linux drivers do:
 *   power    the chip's I/O supply is LDO2 of the PM8350C, switched through RPMh (one request
 *            written into the "always on" processor's command slots); the analog supply is
 *            behind a GPIO
 *   clocks   the serial engine's three clocks, by vote in the global clock controller
 *   pins     four GPIOs to the serial engine, plus reset, interrupt and the analog supply switch
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; or, if the engine cannot be used,
 *            the same four pins driven by hand
 *   panel    GPIO96=1 and GPIO68=0 identify the FocalTech variant; everything else is Goodix,
 *            the same discriminator the vendor sm8350-kernel-OKX uses
 *   chip     Goodix: find where it keeps the touch report, then read it when the IRQ line signals;
 *            FocalTech: read the report at register 0x01 with FTS 5.x SPI framing (cmd 0xC1 for
 *            read, 0xC0 for write, two-byte CRC-16/Kermit)
 * No interrupt is used: a thread looks at the interrupt line 125 times a second (boot-arg
 * touch_poll_us=N: every N microseconds instead of 8000). With touch_lat=1 a line for every report
 * read ("tl: c before US seen US read US": the look before, the look that found the line low, the
 * report read), for measuring how long a touch takes to show (tools/latency.py).
 *
 * Register layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,rpmh-rsc,
 * cmd-db}.c, drivers/clk/qcom/gcc-sc7280.c, drivers/pinctrl/qcom/pinctrl-sc7280.c,
 * drivers/input/touchscreen/{goodix_berlin_*,focaltech_touch/focaltech_core}.c, and the phone's
 * own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern void hakios_tl(const char *format, ...) __printflike(1, 2);

extern void loader_log_line(const char *line);

/*
 * What this driver has to say goes into the loader's log (read over USB: "oem log", then
 * get_staged) rather than onto the screen, which during a boot is a mess of other text. A run that
 * hangs can still be read: the loader keeps the kernel's log buffer for exactly that.
 */
static void
touch_note(const char *format, ...) __printflike(1, 2);

static void
touch_note(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}
#define printf touch_note

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- the boards this driver knows --------------------------------------------------------------- */

/*
 * Everything below differs from board to board, so it is written down per board rather than once
 * with the first phone's numbers; each column comes from that phone's device tree. Boot-arg
 * touch_board=1 picks the second row (touchscreen=1 is still what turns the driver on at all).
 */
enum touch_chip { TOUCH_GOODIX, TOUCH_FOCALTECH, TOUCH_SYNAPTICS };

struct touch_board {
	vm_offset_t tlmm_phys, tlmm_size;       /* the pin controller */
	vm_offset_t cmd_db_phys, rsc_phys;      /* RPMh: the boot loader's command database, our slots */
	vm_offset_t se_phys;                    /* the serial engine the chip sits on */
	bool engine;                            /* the engine can drive the bus; else four pins by hand */
	enum touch_chip chip;                   /* which chip this board has */
	bool reset_high;                        /* the level that holds the chip in reset */
	bool swap_data_pins;                    /* which of the two data pins is the input is not written down */
	uint32_t raw_max_x, raw_max_y;          /* the chip's own coordinate range */
	uint32_t power_delay_ms;                /* from the last supply on to the first reset */
	uint32_t reset_active_ms, reset_delay_ms;       /* hold in reset, then wait for the firmware */
	unsigned pin_miso, pin_mosi, pin_clk, pin_cs;
	unsigned pin_int, pin_reset;            /* interrupt: low = a report waits; reset: low = in reset */
	int pin_avdd;                           /* -1: the analog supply is a regulator, not a switch */
	int pin_focaltech_id, pin_goodix_id;    /* -1: no strap pins -- the board has a Goodix */
	const char *iovdd, *avdd;               /* the RPMh resources of the two supplies */
	uint32_t iovdd_uv, avdd_uv, regulator_mode;
	bool regulator_enable_only;             /* the boot loader set voltage and mode: only enable is written */
};

/* redwood (POCO X5 Pro, SM8450): the original phone; the serial engine drives the bus. */
static const struct touch_board board_redwood = {
	.tlmm_phys = 0x0f100000UL, .tlmm_size = 0x100000UL,
	.cmd_db_phys = 0x80860000UL, .rsc_phys = 0x18220000UL,
	.se_phys = 0x00a94000UL, .engine = true,
	.chip = TOUCH_GOODIX, .reset_high = false, .swap_data_pins = true,
	.raw_max_x = 10800, .raw_max_y = 24000,
	.power_delay_ms = 15, .reset_active_ms = 2, .reset_delay_ms = 100,
	.pin_miso = 52, .pin_mosi = 53, .pin_clk = 54, .pin_cs = 55,
	.pin_int = 81, .pin_reset = 105, .pin_avdd = 107,
	.pin_focaltech_id = 68, .pin_goodix_id = 96,
	.iovdd = "ldoc2", .iovdd_uv = 1800000, .regulator_mode = 7,
};

/*
 * corvette (OnePlus Ace 3 Pro, SM8650): pinctrl@f000000, the chip on gpio48..51 (QUP1 SE4 at
 * 0xa90000 -- gpio48 is the input), interrupt on gpio162, reset on gpio161. The I/O supply is
 * L4B of the PMIC (ldob4, 1.8 V) through RPMh; the analog supply is switched by gpio108, high
 * on. The chip is a Synaptics S3910: reset low for 10 ms, 80 ms after it is released, and the
 * panel reaches 20224 x 44480 in the chip's own units (16 times the 1264 x 2780 pixels).
 * The engine is left alone: driving the four pins by hand needs no clock votes and no mux
 * numbers of a chip this driver knows only from its device tree.
 */
static const struct touch_board board_corvette = {
	.tlmm_phys = 0x0f100000UL, .tlmm_size = 0x100000UL,
	.cmd_db_phys = 0x81c60000UL, .rsc_phys = 0x17a20000UL,
	.se_phys = 0x00a90000UL, .engine = false,
	.chip = TOUCH_SYNAPTICS, .reset_high = false, .swap_data_pins = false,
	.raw_max_x = 12640, .raw_max_y = 27800,   /* the chip counts in tenths of a screen pixel (s8 corners) */
	.power_delay_ms = 200, .reset_active_ms = 10, .reset_delay_ms = 80,
	.pin_miso = 48, .pin_mosi = 49, .pin_clk = 50, .pin_cs = 51,
	.pin_int = 162, .pin_reset = 161, .pin_avdd = 108,
	.pin_focaltech_id = -1, .pin_goodix_id = -1,
	.iovdd = "ldob4", .iovdd_uv = 1800000, .avdd = NULL, .regulator_mode = 4,
	.regulator_enable_only = true,
};

static const struct touch_board *board = &board_redwood;

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               (board->tlmm_phys)
#define TLMM_SIZE               (board->tlmm_size)
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_MISO                (board->pin_miso)
#define PIN_MOSI                (board->pin_mosi)
#define PIN_CLK                 (board->pin_clk)
#define PIN_CS                  (board->pin_cs)
#define PIN_INT                 (board->pin_int)        /* low = the chip has a report */
#define PIN_RESET               (board->pin_reset)      /* low = in reset (touch_reset_high=1: high) */
#define PIN_AVDD                (board->pin_avdd)       /* high = analog supply on */
#define PIN_FOCALTECH_ID        (board->pin_focaltech_id)       /* FocalTech if this reads 0 ... */
#define PIN_GOODIX_ID           (board->pin_goodix_id)          /* ... and this reads 1 */
#define PIN_FUNCTION_SPI        1       /* "qup15" */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_SIZE             0x20000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS_BASE            0xd00            /* dt: qcom,tcs-offset */
#define RSC_TCS_STRIDE          0x2a0            /* the distance between two slot sets */
#define RSC_ID                  0x00             /* the controller's version: major [23:16], minor [15:8] */
#define TCS(reg)                (rsc + RSC_TCS_BASE + RSC_TCS_STRIDE * rsc_slot + (reg))
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_READ_4_BYTES      0x00108
#define MSGID_RESPONSE          0x100
#define CMD_STATUS_COMPL        (1u << 16)

/* Which slot set of the controller we use: the first three are the "active" ones (dt: qcom,tcs-config). */
static unsigned rsc_slot;
#define RSC_IRQ_BIT             (1u << rsc_slot)        /* the interrupt and status bit of that set */

/*
 * The interrupt, status and clear registers belong to the whole controller, not to one slot set: they
 * are one register with a bit per set, and Linux's rpmh-rsc reads them at the base of the slot area
 * without the per-set stride. Read at a per-set offset, the status of the set we used never came up,
 * which is exactly what the read in s20 showed (the command had completed, the status bit had not).
 */
#define TCS_IRQ(reg)            (rsc + RSC_TCS_BASE + (reg))

/*
 * The registers of a command slot set, from Linux's rpmh-rsc.c: two layouts, and the controller's ID
 * register says which one it has. SM8650 answers version 3, whose command slots are 24 bytes apart and
 * whose slot registers sit further in; read with the 2.7 offsets, its status comes back 0 forever, so
 * the slot looks busy and no request is ever sent.
 */
struct rsc_layout {
	uint32_t irq_enable, irq_status, irq_clear, wait, control, status, cmd_enable, cmd_msgid, cmd_addr, cmd_data, cmd_status, cmd_resp, cmd_stride;
};
static const struct rsc_layout rsc_layout_v2_7 = { 0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 0x40, 20 };
static const struct rsc_layout rsc_layout_v3 = { 0x00, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 0x44, 24 };
static const struct rsc_layout *rsc_regs = &rsc_layout_v2_7;

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8

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
		printf("goodix: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
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
	for (int i = 0; i < 2000; i++) {
		if ((rd(address) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("goodix: RPMh: timeout waiting for %s (status %#x)\n", what, rd(address));
	return false;
}

/*
 * Which of the two register layouts this controller has, from the two candidate status registers: the one
 * that is a real register in the layout reads "idle" (not 0), the other is reserved space there and reads
 * 0. The controller's ID register at offset 0 is not read: that part of the block is not ours to look at,
 * and reading it has taken the device down (900E) once. rsc_regs stays NULL if neither reads idle.
 */
static void
rsc_read_layout(void)
{
	uint32_t idle_v2_7 = rd(TCS(0x18)), idle_v3 = rd(TCS(0x28));

	if (idle_v3 != 0) {
		rsc_regs = &rsc_layout_v3;
	} else if (idle_v2_7 != 0) {
		rsc_regs = &rsc_layout_v2_7;
	} else {
		rsc_regs = NULL;
	}
	printf("goodix: RPMh: the status of slot set 0 reads %#x at 0x18 (the 2.7 register) and %#x at 0x28 (the 3.x one): %s\n",
	    idle_v2_7, idle_v3,
	    rsc_regs == &rsc_layout_v3 ? "the 3.x layout" : rsc_regs == &rsc_layout_v2_7 ? "the 2.7 layout" :
	    "neither reads idle, so no request will be written");
}

/*
 * The slot set to use: the first of the three "active" ones (dt: qcom,tcs-config) that the boot loader
 * left no request in and that is idle. It used set 0 and left seven commands enabled there, so a request
 * written into that set would be sent along with the boot loader's own votes.
 */
static void
rsc_pick_slot(void)
{
	if (rsc_regs == NULL) {
		return;
	}
	for (unsigned set = 0; set < 3; set++) {
		rsc_slot = set;
		if (rd(TCS(rsc_regs->cmd_enable)) == 0 && rd(TCS(rsc_regs->status)) != 0) {
			printf("goodix: RPMh: slot set %u is free: the requests go there\n", set);
			return;
		}
	}
	rsc_slot = 0;
	printf("goodix: RPMh: every active slot set is in use: set 0 is used anyway\n");
}

/*
 * Send up to three register writes to RPMh and wait until the controller says the set is done. The
 * writes are fire and forget, the way Linux sends them: no answer is asked for, so none has to arrive.
 */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done;

	if (rsc_regs == NULL) {
		printf("goodix: RPMh: the register layout is not known: no request is sent\n");
		return false;
	}
	irq_enable = rd(TCS_IRQ(rsc_regs->irq_enable));

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(rsc_regs->status)) == 0; i++) {
		if (i > 2000) {
			printf("goodix: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS_IRQ(rsc_regs->irq_clear), RSC_IRQ_BIT);
	wr(TCS(rsc_regs->cmd_enable), 0);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(rsc_regs->cmd_msgid + rsc_regs->cmd_stride * i), MSGID_WRITE_4_BYTES);
		wr(TCS(rsc_regs->cmd_addr + rsc_regs->cmd_stride * i), address[i]);
		wr(TCS(rsc_regs->cmd_data + rsc_regs->cmd_stride * i), data[i]);
	}
	wr(TCS(rsc_regs->wait), 0);
	wr(TCS(rsc_regs->cmd_enable), (1u << count) - 1);
	wr(TCS_IRQ(rsc_regs->irq_enable), irq_enable | RSC_IRQ_BIT);
	wr(TCS(rsc_regs->control), TCS_CONTROL_ENABLE);
	wr(TCS(rsc_regs->control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ(rsc_regs->irq_status), RSC_IRQ_BIT, RSC_IRQ_BIT, "the request");
	printf("goodix: RPMh: after the request: control %#x, cmd enable %#x, irq status %#x, command status %#x\n",
	    rd(TCS(rsc_regs->control)), rd(TCS(rsc_regs->cmd_enable)), rd(TCS_IRQ(rsc_regs->irq_status)),
	    rd(TCS(rsc_regs->cmd_status)));
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(rsc_regs->cmd_enable), 0);
	wr(TCS(rsc_regs->wait), 0);
	wr(TCS_IRQ(rsc_regs->irq_clear), RSC_IRQ_BIT);
	wr(TCS_IRQ(rsc_regs->irq_enable), irq_enable);
	return done;
}

/*
 * Ask the controller to read one register of a resource, and return what it answered. A read changes
 * nothing: it is the smallest request a slot set can be asked to carry, so it is what we try first.
 */
static bool
rpmh_read(uint32_t address, uint32_t *value)
{
	uint32_t irq_enable;
	bool done;

	if (rsc_regs == NULL) {
		printf("goodix: RPMh: the register layout is not known: nothing is read\n");
		return false;
	}
	irq_enable = rd(TCS_IRQ(rsc_regs->irq_enable));
	for (int i = 0; rd(TCS(rsc_regs->status)) == 0; i++) {
		if (i > 2000) {
			printf("goodix: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS_IRQ(rsc_regs->irq_clear), RSC_IRQ_BIT);
	wr(TCS(rsc_regs->cmd_enable), 0);
	wr(TCS(rsc_regs->cmd_msgid), MSGID_READ_4_BYTES | MSGID_RESPONSE);
	wr(TCS(rsc_regs->cmd_addr), address);
	wr(TCS(rsc_regs->cmd_data), 0);
	wr(TCS(rsc_regs->wait), 1);
	wr(TCS_IRQ(rsc_regs->irq_enable), irq_enable | RSC_IRQ_BIT);
	wr(TCS(rsc_regs->cmd_enable), 1);
	wr(TCS(rsc_regs->control), TCS_CONTROL_ENABLE);
	wr(TCS(rsc_regs->control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ(rsc_regs->irq_status), RSC_IRQ_BIT, RSC_IRQ_BIT, "the read");
	printf("goodix: RPMh: after the read: control %#x, cmd enable %#x, irq status %#x, command status %#x, response %#x\n",
	    rd(TCS(rsc_regs->control)), rd(TCS(rsc_regs->cmd_enable)), rd(TCS_IRQ(rsc_regs->irq_status)),
	    rd(TCS(rsc_regs->cmd_status)), rd(TCS(rsc_regs->cmd_resp)));
	if (done) {
		*value = rd(TCS(rsc_regs->cmd_resp));
	}
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(rsc_regs->control), rd(TCS(rsc_regs->control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(rsc_regs->cmd_enable), 0);
	wr(TCS(rsc_regs->wait), 0);
	wr(TCS_IRQ(rsc_regs->irq_clear), RSC_IRQ_BIT);
	wr(TCS_IRQ(rsc_regs->irq_enable), irq_enable);
	return done;
}

/* One supply on: a regulator of the PMIC, through RPMh. Nothing else is connected to the I/O one. */
static bool
regulator_on(const char *name, uint32_t microvolts, uint32_t mode)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3], data[3];
	unsigned count;

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("goodix: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	/*
	 * On corvette the boot loader has already set this supply's voltage and mode, and only the enable is
	 * missing. Writing the other two registers along with it took the device down (900E); a single
	 * command is what the controller takes.
	 */
	if (board->regulator_enable_only) {
		address[0] = base + VRM_ENABLE;
		data[0] = 1;
		count = 1;
	} else {
		address[0] = base + VRM_VOLTAGE;
		address[1] = base + VRM_ENABLE;
		address[2] = base + VRM_MODE;
		data[0] = microvolts;
		data[1] = 1;
		data[2] = mode;
		count = 3;
	}
	printf("goodix: RPMh: %s is at %#x (%s)\n", name, base,
	    rsc_regs == NULL ? "the slot layout is not known, nothing will be written" : "asking for it");
	if (rsc_regs != NULL) {
		printf("goodix: RPMh: slot %u status %#x, control %#x, irq enable %#x status %#x\n", rsc_slot,
		    rd(TCS(rsc_regs->status)), rd(TCS(rsc_regs->control)), rd(TCS_IRQ(rsc_regs->irq_enable)),
		    rd(TCS_IRQ(rsc_regs->irq_status)));
	}
	if (!rpmh_write(address, data, count)) {
		return false;
	}
	printf("goodix: %s on (%u mV)\n", name, microvolts / 1000);
	return true;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP1_CORE_2X       (1u << 18)
#define VOTE_QUP1_CORE          (1u << 19)
#define VOTE_QUP1_M_AHB         (1u << 20)
#define VOTE_QUP1_S_AHB         (1u << 21)
#define VOTE_QUP1_SE5           (1u << 27)             /* serial engine 13 is the 6th of the second group */
#define GCC_QUP1_M_AHB_CBCR     (gcc + 0x18004)         /* bit 31: the clock is off */
#define GCC_QUP1_S_AHB_CBCR     (gcc + 0x18008)
#define GCC_QUP1_SE5_CBCR       (gcc + 0x185fc)
#define GCC_QUP1_SE5_CMD        (gcc + 0x18600)         /* bit 0: take the new configuration */
#define GCC_QUP1_SE5_CFG        (gcc + 0x18604)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP1_SE5_DFSR       (gcc + 0x18614)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP1_SE5_PERF_CFG(l) (gcc + 0x1861c + 4 * (l))
#define GCC_QUP1_SE5_PERF_M(l)  (gcc + 0x1865c + 4 * (l))
#define GCC_QUP1_SE5_PERF_N(l)  (gcc + 0x1869c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB);
	if (rd(GCC_QUP1_SE5_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP1_SE5_PERF_CFG(level)), rd(GCC_QUP1_SE5_PERF_M(level)),
			    rd(GCC_QUP1_SE5_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("goodix: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP1_SE5_CFG, (rd(GCC_QUP1_SE5_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP1_SE5_CMD, rd(GCC_QUP1_SE5_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP1_SE5_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP1_SE5_CMD) & 1) {
			printf("goodix: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP1_SE5_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_SE5);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP1_M_AHB_CBCR) | rd(GCC_QUP1_S_AHB_CBCR) | rd(GCC_QUP1_SE5_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("goodix: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR),
	    rd(GCC_QUP1_SE5_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("goodix: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

static unsigned bitbang_in, bitbang_out;        /* which pin is the input: set from the board */

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the Goodix chip ------------------------------------------------------------------------------- */

#define GOODIX_FW_VERSION       0x10014         /* 28 bytes */
#define GOODIX_IC_INFO          0x10070         /* a 16-bit length, then that many bytes */
#define GOODIX_MAX_READ         1024
#define GOODIX_MAX_TOUCHES      10
#define GOODIX_STATUS_TOUCH     0x80
#define GOODIX_STATUS_REQUEST   0x40
#define GOODIX_REQUEST_RESET    3

static uint8_t spi_tx[8 + GOODIX_MAX_READ], spi_rx[8 + GOODIX_MAX_READ];
static uint32_t touch_address, command_address;
static const char *report_rate = "240";        /* what the status line says (touch_rate=480) */
static uint32_t max_x = 10800, max_y = 24000;   /* the device tree's; the chip may say otherwise */

static bool
goodix_read(uint32_t address, uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf1;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	spi_tx[5] = spi_tx[6] = spi_tx[7] = 0xff;
	memset(spi_tx + 8, 0, length);
	if (!spi_transfer(spi_tx, spi_rx, 8 + length)) {
		return false;
	}
	memcpy(data, spi_rx + 8, length);
	return true;
}

static bool
goodix_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf0;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	memcpy(spi_tx + 5, data, length);
	return spi_transfer(spi_tx, NULL, 5 + length);
}

/* The chip's blocks end in the 16-bit sum of their bytes; all zeros or all ones is no answer at all. */
static bool
goodix_checksum_ok(const uint8_t *data, uint32_t length)
{
	uint32_t sum = 0;
	bool blank = true;

	if (length < 2) {
		return false;
	}
	for (uint32_t i = 0; i < length - 2; i++) {
		sum += data[i];
		blank &= data[i] == 0 || data[i] == 0xff;
	}
	return !blank && (uint16_t)sum == (uint16_t)(data[length - 2] | data[length - 1] << 8);
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* boot-arg touch_reset_high=0|1: the level that holds the chip in reset; the board's own unless given. */
static int reset_high = -1;

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, reset_high ? true : false);
	sleep_ms(board->reset_active_ms);
	gpio_set(PIN_RESET, reset_high ? false : true);
	sleep_ms(board->reset_delay_ms);
}

/* Is the chip there and running its firmware? Finds where it keeps the touch report. */
static bool
goodix_probe(bool verbose)
{
	static uint8_t info[GOODIX_MAX_READ];
	uint8_t version[28];
	uint32_t length, offset;

	if (!goodix_read(GOODIX_FW_VERSION, version, sizeof(version))) {
		return false;
	}
	if (!goodix_checksum_ok(version, sizeof(version))) {
		if (verbose) {
			printf("goodix: no valid firmware version: %02x %02x %02x %02x %02x %02x %02x %02x ... %02x %02x\n",
			    version[0], version[1], version[2], version[3], version[4], version[5], version[6], version[7],
			    version[26], version[27]);
		}
		return false;
	}
	printf("goodix: chip \"%.6s\", firmware \"%.8s\" %02x.%02x.%02x.%02x, sensor %u\n", (const char *)version,
	    (const char *)version + 10, version[18], version[19], version[20], version[21], version[23]);

	if (!goodix_read(GOODIX_IC_INFO, info, 2)) {
		return false;
	}
	length = le16(info);
	if (length < 40 || length >= GOODIX_MAX_READ || !goodix_read(GOODIX_IC_INFO, info, length) ||
	    !goodix_checksum_ok(info, length)) {
		printf("goodix: bad chip description (length %u)\n", length);
		return false;
	}
	/* the length, the version (16 bytes), the features (10), four counts, then five lists of 16-bit numbers */
	offset = 2 + 16 + 10 + 4;
	for (int i = 0; i < 5; i++) {
		if (offset >= length) {
			return false;
		}
		offset += 1 + 2u * info[offset];
	}
	if (offset + 48 + 2 > length) {
		printf("goodix: chip description too short (%u, lists end at %u)\n", length, offset);
		return false;
	}
	touch_address = le32(info + offset + 44);
	command_address = le32(info + offset + 0);      /* (the "misc" part: the command buffer comes first) */
	if (offset + 122 + 20 + 2 <= length) {
		uint16_t x = le16(info + offset + 122 + 16), y = le16(info + offset + 122 + 18);

		if (x && y) {
			max_x = x;
			max_y = y;
		}
	}
	printf("goodix: touch report at %#x, coordinates up to %u x %u\n", touch_address, max_x, max_y);
	return touch_address != 0;
}

/*
 * A command to the chip (Android's brl_send_cmd(): goodix_brl_hw.c of the phone's kernel): sixteen
 * bytes at the command address -- state, acknowledgement, length, command, its data, and the sum of
 * the bytes from the length on -- then the acknowledgement looked for, 0x80 when done.
 */
#define GOODIX_CMD_HIGH_RATE    0xc0            /* data 1: report 480 times a second instead of 240 (Xiaomi's game mode) */
#define GOODIX_ACK_OK           0x80
#define GOODIX_ACK_BUSY         0x02

static bool
goodix_command(uint8_t command, uint8_t data)
{
	uint8_t buffer[16], answer[16];
	uint32_t sum;

	if (command_address == 0) {
		return false;
	}
	memset(buffer, 0, sizeof(buffer));
	buffer[2] = 5;
	buffer[3] = command;
	buffer[4] = data;
	sum = buffer[2] + buffer[3] + buffer[4];
	buffer[5] = (uint8_t)sum;
	buffer[6] = (uint8_t)(sum >> 8);
	for (int attempt = 0; attempt < 6; attempt++) {
		if (!goodix_write(command_address, buffer, sizeof(buffer))) {
			return false;
		}
		for (int look = 0; look < 6; look++) {
			if (!goodix_read(command_address, answer, sizeof(answer))) {
				return false;
			}
			if (answer[1] == GOODIX_ACK_OK) {
				return true;
			}
			sleep_ms(1);
			if (answer[1] != GOODIX_ACK_BUSY && answer[1] != 0) {
				break;
			}
		}
	}
	return false;
}

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[GOODIX_MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static void
goodix_report(const uint8_t *touches, uint32_t count)
{
	static unsigned shown;

	for (int i = 0; i < GOODIX_MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *t = touches + 8 * i;
		uint32_t id = t[0] >> 4, kind = t[0] & 0xf;
		uint32_t x = le16(t + 2), y = le16(t + 4);

		if (id >= GOODIX_MAX_TOUCHES || kind == 1 || kind == 3) {       /* 1 and 3: a stylus */
			continue;
		}
		if (shown < 40) {
			shown++;
			printf("goodix: finger %u at %u, %u (of %u x %u)\n", id, x, y, max_x, max_y);
		}
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		x = x >= max_x ? 65535 : x * 65535 / max_x;
		y = y >= max_y ? 65535 : y * 65535 / max_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		contact[id].seen = true;
		if (!contact[id].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* Read the report if there is one, and tell the chip it has been read. */
static void
goodix_poll(void)
{
	static uint8_t report[8 + 8 * GOODIX_MAX_TOUCHES + 2];
	static unsigned bad;
	const uint8_t zero = 0;
	const uint32_t first = 8 + 8 * 2 + 2;           /* the header and two fingers: all there is, most of the time */
	uint32_t count;

	seen.polls++;
	if (!goodix_read(touch_address, report, first) || report[0] == 0) {
		return;
	}
	seen.reports++;
	count = report[2] & 0xf;
	if ((report[0] & GOODIX_STATUS_TOUCH) && count > 2 && count <= GOODIX_MAX_TOUCHES &&
	    !goodix_read(touch_address + 8, report + 8, 8 * count + 2)) {
		return;
	}
	goodix_write(touch_address, &zero, 1);
	if (!goodix_checksum_ok(report, 8)) {
		seen.bad++;
		if (bad++ < 8) {
			printf("goodix: bad report header: %02x %02x %02x %02x %02x %02x %02x %02x\n", report[0], report[1],
			    report[2], report[3], report[4], report[5], report[6], report[7]);
		}
		return;
	}
	if (report[0] & GOODIX_STATUS_REQUEST) {
		if ((report[2] & 0xf) == GOODIX_REQUEST_RESET || report[2] == GOODIX_REQUEST_RESET) {
			printf("goodix: the chip asks to be reset\n");
			touch_reset();
		}
		return;
	}
	if (!(report[0] & GOODIX_STATUS_TOUCH)) {
		return;
	}
	if (count > GOODIX_MAX_TOUCHES || (count && !goodix_checksum_ok(report + 8, count * 8 + 2))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("goodix: bad report (%u touches)\n", count);
		}
		return;
	}
	goodix_report(report + 8, count);
}

/* ---- the FocalTech FT3680 chip --------------------------------------------------------------------- */

/*
 * FTS 5.x SPI framing from sm8350-kernel-OKX drivers/input/touchscreen/focaltech_3680/
 * focaltech_spi.c (verbatim):
 *   TX[0] = register address (writebuf[0] / cmd[0])
 *   TX[1] = FOCALTECH_READ_CTRL (0xA0 = READ_CMD, CRC_EN set) or FOCALTECH_WRITE_CTRL (0x00)
 *   TX[2..3] = data length BE (for read: datalen; for write: writelen - 1)
 *   TX[4..6] = three dummy bytes (SPI_DUMMY_BYTE)
 *   TX[7..7+len-1] = write data (empty on read; chip drives MISO here)
 *   TX[last-1..last] = CRC-16/Kermit of the data bytes, little-endian (only when READ_CMD or
 *                     when a vendor WRITE op needs CRC; vendor WRITE_CMD = 0x00 has no CRC)
 * SPI_HEADER_LENGTH = 4 + SPI_DUMMY_BYTE = 7.
 */
#define FOCALTECH_READ_CTRL         0xa0		/* READ_CMD = 0x80 | DATA_CRC_EN(0x20) */
#define FOCALTECH_WRITE_CTRL        0x00		/* WRITE_CMD, no CRC on TX */
#define FOCALTECH_CHIP_ID_H         0xa3
#define FOCALTECH_CHIP_ID_L         0x9f
#define FOCALTECH_TOUCH_DATA        0x01		/* the touch report lives at this address */
#define FOCALTECH_MAX_TOUCHES       10
#define FOCALTECH_TOUCH_REPORT_LEN  (2 + 6 * FOCALTECH_MAX_TOUCHES)
#define FOCALTECH_MAX_TRANSFER      (7 + FOCALTECH_TOUCH_REPORT_LEN + 2)

static uint8_t focaltech_tx[FOCALTECH_MAX_TRANSFER], focaltech_rx[FOCALTECH_MAX_TRANSFER];

/* CRC-16/Kermit (polynomial 0x8408 reflected, initial 0xffff) -- same as vendor rckit_crc16(). */
static uint16_t
focaltech_crc(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static bool
focaltech_read(uint8_t address, uint8_t *data, uint32_t length)
{
	const uint32_t data_offset = 7;
	uint32_t transfer_length = data_offset + length + 2;

	if (length == 0 || transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		memset(focaltech_tx, 0, transfer_length);
		memset(focaltech_rx, 0, transfer_length);
		focaltech_tx[0] = address;		/* vendor: TX[0] = cmd[0] = reg address */
		focaltech_tx[1] = FOCALTECH_READ_CTRL;	/* vendor: TX[1] = ctrl = 0xA0 */
		focaltech_tx[2] = (uint8_t)(length >> 8);
		focaltech_tx[3] = (uint8_t)length;
		if (!spi_transfer(focaltech_tx, focaltech_rx, transfer_length)) {
			sleep_us(150);
			continue;
		}
		/* vendor: bit 7 of status (rx[3]) is BUSY, bit 5 is CRC error */
		if (!(focaltech_rx[3] & 0xa0)) {
			uint16_t expected = focaltech_crc(focaltech_rx + data_offset, length);
			uint16_t received = (uint16_t)(focaltech_rx[data_offset + length] |
			    focaltech_rx[data_offset + length + 1] << 8);
			if (expected == received) {
				memcpy(data, focaltech_rx + data_offset, length);
				sleep_us(150);		/* minimum CS-high time in the vendor driver */
				return true;
			}
		}
		sleep_us(150);
	}
	return false;
}

/*
 * Vendor fts_write: no CRC on TX. For a 1-byte "command" (length == 0) the frame is just the
 * four header bytes { addr, 0x00, 0x00, 0x00 } with no dummy and no data. For length > 0 the
 * three dummy bytes precede the data: { addr, 0x00, lenH, lenL, 0, 0, 0, data... }. The chip
 * drives the "status" byte on rx[3] -- bits 7 and 5 set mean busy or CRC error (vendor retries
 * SPI_RETRY_NUMBER = 3 times on failure before giving up).
 */
static bool
focaltech_write(uint8_t address, const uint8_t *data, uint32_t length)
{
	uint32_t transfer_length;

	if (length == 0) {
		transfer_length = 4;
	} else {
		transfer_length = 7 + length;
	}
	if (transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	memset(focaltech_tx, 0, transfer_length);
	focaltech_tx[0] = address;
	focaltech_tx[1] = FOCALTECH_WRITE_CTRL;
	focaltech_tx[2] = (uint8_t)(length >> 8);
	focaltech_tx[3] = (uint8_t)length;
	if (data && length > 0) {
		memcpy(focaltech_tx + 7, data, length);
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		if (spi_transfer(focaltech_tx, focaltech_rx, transfer_length) &&
		    !(focaltech_rx[3] & 0xa0)) {
			sleep_us(150);
			return true;
		}
		sleep_us(150);
	}
	return false;
}

/*
 * Vendor fts_fwupg_get_boot_state() reads the chip's "boot ID" via the FT boot-ROM protocol:
 *   TX = { 0x55, 0xAA }                 (FTS_CMD_START1, FTS_CMD_START2 -- enter boot)
 *   wait FTS_CMD_START_DELAY = 12 ms
 *   TX = { 0x90 }, RX = 2 bytes         (FTS_CMD_READ_ID, incell cmd length)
 * If the two RX bytes are the chip's boot ID (0x56 0x62 for FT3680), firmware is present and
 * valid in flash but has not yet been started; the host must issue FTS_CMD_FIRMWARE_START
 * (0x08) to make the boot ROM jump to it.
 */
#define FTS_CMD_START1              0x55
#define FTS_CMD_START2              0xaa
#define FTS_CMD_START_DELAY         12		/* ms, between enter-boot and first command */
#define FTS_CMD_READ_BOOT_ID        0x90

/* Boot-ROM commands for firmware (pramboot) upload, from focaltech_3658u/focaltech_flash.h. */
#define FTS_ROMBOOT_CMD_SET_PRAM_ADDR   0xad
#define FTS_ROMBOOT_CMD_WRITE           0xae
#define FTS_ROMBOOT_CMD_START_APP       0x08
#define FTS_FLASH_PACKET_LENGTH         32		/* bytes per PRAM write */

#include "hakios.focaltech_fw.h"

/*
 * Vendor fts_read_bootid(): send { 0x55, 0xAA } as a 2-byte framed write (treated as a write of
 * one data byte 0xAA to register 0x55), wait 12 ms, then do a framed read of 2 bytes at
 * register 0x90. The chip answers with its ROM ID (0x56 0x62 for FT3680) if it is sitting in
 * the boot ROM waiting for a firmware download.
 */
static bool
focaltech_read_boot_id(uint8_t *high, uint8_t *low)
{
	uint8_t aa = FTS_CMD_START2;
	uint8_t out[2] = { 0 };
	bool ok;

	if (!focaltech_write(FTS_CMD_START1, &aa, 1)) {
		return false;
	}
	sleep_ms(12);			/* FTS_CMD_START_DELAY */
	ok = focaltech_read(FTS_CMD_READ_BOOT_ID, out, 2);
	*high = out[0];
	*low  = out[1];
	return ok;
}

/* Vendor fts_wait_tp_to_valid: poll chip ID for up to TIMEOUT_READ_REG (1 s) at 200 ms steps. */
static bool
focaltech_wait_valid(uint8_t *high, uint8_t *low)
{
	for (int attempt = 0; attempt < 5; attempt++) {
		if (focaltech_read(FOCALTECH_CHIP_ID_H, high, 1) &&
		    focaltech_read(FOCALTECH_CHIP_ID_L, low, 1) &&
		    *high == 0x56 && *low == 0x62) {
			printf("focaltech: FT3680 runtime ID valid after %d attempt(s)\n", attempt + 1);
			return true;
		}
		sleep_ms(200);			/* vendor INTERVAL_READ_REG */
	}
	return false;
}

/*
 * Reset the chip and send 0x55/0xAA to put it in boot ROM, as vendor fts_get_ic_information
 * does. Try up to 3 times with escalating settle delays (12, 20, 28 ms). Returns true if the
 * chip is sitting in its boot ROM, with { 0x56, 0x62 } read back from the boot-ID register.
 */
static bool
focaltech_enter_boot_rom(void)
{
	uint8_t boot_high = 0, boot_low = 0;

	for (int cnt = 0; cnt < 3; cnt++) {
		gpio_set(PIN_RESET, false);
		sleep_ms(1);
		gpio_set(PIN_RESET, true);
		sleep_ms(FTS_CMD_START_DELAY + cnt * 8);
		if (focaltech_read_boot_id(&boot_high, &boot_low) &&
		    boot_high == 0x56 && boot_low == 0x62) {
			printf("focaltech: in boot ROM after reset %d\n", cnt);
			return true;
		}
		printf("focaltech: boot ID attempt %d: %02x %02x (not 56 62)\n", cnt, boot_high, boot_low);
	}
	return false;
}

/*
 * Download the embedded firmware (pramboot + application) into the chip's PRAM via the boot
 * ROM's SPI_V2 upload protocol. Each 32-byte packet is sent as two framed writes: first set
 * the PRAM destination address (SET_PRAM_ADDR + 3 BE address bytes), then WRITE + data bytes.
 */
static bool
focaltech_write_pram(const uint8_t *buf, uint32_t len)
{
	uint8_t addr_bytes[3];

	for (uint32_t offset = 0; offset < len; offset += FTS_FLASH_PACKET_LENGTH) {
		uint32_t chunk = FTS_FLASH_PACKET_LENGTH;
		if (offset + chunk > len) {
			chunk = len - offset;
		}
		addr_bytes[0] = (uint8_t)(offset >> 16);
		addr_bytes[1] = (uint8_t)(offset >> 8);
		addr_bytes[2] = (uint8_t)offset;
		if (!focaltech_write(FTS_ROMBOOT_CMD_SET_PRAM_ADDR, addr_bytes, 3)) {
			printf("focaltech: PRAM address write failed at offset %u\n", offset);
			return false;
		}
		if (!focaltech_write(FTS_ROMBOOT_CMD_WRITE, buf + offset, chunk)) {
			printf("focaltech: PRAM data write failed at offset %u\n", offset);
			return false;
		}
	}
	return true;
}

static bool
focaltech_start_app(void)
{
	uint8_t cmd = FTS_ROMBOOT_CMD_START_APP;

	return focaltech_write(cmd, NULL, 0);
}

/*
 * Full chip bring-up: look for an already-running firmware; if none, reset to boot ROM and
 * download the embedded firmware into PRAM; then start it and wait for runtime to come up.
 */
static bool
focaltech_probe(bool verbose)
{
	uint8_t high = 0, low = 0;

	/* Maybe firmware is already running (previous session, warm boot). */
	if (focaltech_wait_valid(&high, &low)) {
		return true;
	}

	if (!focaltech_enter_boot_rom()) {
		if (verbose) {
			printf("focaltech: chip not in boot ROM; last read runtime %02x %02x\n", high, low);
		}
		return false;
	}

	printf("focaltech: downloading %u bytes of firmware to PRAM...\n", FOCALTECH_FW_LENGTH);
	if (!focaltech_write_pram(focaltech_fw, FOCALTECH_FW_LENGTH)) {
		printf("focaltech: PRAM download failed\n");
		return false;
	}
	printf("focaltech: PRAM download complete; starting application\n");
	if (!focaltech_start_app()) {
		printf("focaltech: start-app command failed\n");
		return false;
	}
	sleep_ms(300);			/* firmware needs time to initialize all peripherals */

	if (focaltech_wait_valid(&high, &low)) {
		return true;
	}
	if (verbose) {
		printf("focaltech: firmware did not come up (last runtime read %02x %02x)\n", high, low);
	}
	return false;
}

/* ---- FocalTech touch reports --------------------------------------------------------------------- */

/*
 * Per-chip report layout for FT3680 (vendor focaltech_core.c, FTS_TOUCH_OFF_* offsets):
 *   report[0] = gesture (0 outside gesture mode)
 *   report[1] = (chip_type << 4) | finger_count
 *   report[2 + 6*i + 0] = E_XH:  bits 7..6 = event (0 = down, 1 = up, 2 = contact)
 *                                bits 3..0 = x bits 14..11
 *   report[2 + 6*i + 1] = XL:    x bits 10..3
 *   report[2 + 6*i + 2] = ID_YH: bits 7..4 = finger id, bits 3..0 = y bits 14..11
 *   report[2 + 6*i + 3] = YL:    y bits 10..3
 *   report[2 + 6*i + 4] = PRE:   bits 7..5 = x bits 2..0, bits 4..2 = y bits 2..0
 *                                (also the pressure byte -- the lowest bits of each 15-bit axis
 *                                 are packed in with it)
 *   report[2 + 6*i + 5] = AREA:  touch area / major
 * Coordinates are 15-bit "super-high resolution" sub-pixel values; the touch panel's actual
 * extent is reported through the device tree as x_max/y_max (10800 x 24000 for redwood).
 */
#define FOCALTECH_EVENT_DOWN        0
#define FOCALTECH_EVENT_UP          1
#define FOCALTECH_EVENT_CONTACT     2

static struct {
	bool down, seen;
	uint32_t x, y;
} focaltech_contact[FOCALTECH_MAX_TOUCHES];

static void
focaltech_contact_update(uint32_t id, uint32_t event, uint32_t x, uint32_t y,
    uint32_t max_report_x, uint32_t max_report_y)
{
	static unsigned shown;

	if (id >= FOCALTECH_MAX_TOUCHES || max_report_x == 0 || max_report_y == 0) {
		return;
	}
	if (event == FOCALTECH_EVENT_UP) {
		focaltech_contact[id].seen = true;
		if (focaltech_contact[id].down) {
			focaltech_contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, focaltech_contact[id].x, focaltech_contact[id].y);
		}
		return;
	}
	seen.touches++;
	switch (flip) {
	case 1: x = max_report_x - 1 - x; break;
	case 2: y = max_report_y - 1 - y; break;
	case 3: x = max_report_x - 1 - x; y = max_report_y - 1 - y; break;
	}
	x = (uint32_t)(((uint64_t)x * 65535) / (max_report_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (max_report_y - 1));
	seen.last_x = x;
	seen.last_y = y;
	focaltech_contact[id].seen = true;
	if (!focaltech_contact[id].down) {
		focaltech_contact[id].down = true;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
	} else if (focaltech_contact[id].x != x || focaltech_contact[id].y != y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	focaltech_contact[id].x = x;
	focaltech_contact[id].y = y;
	if (shown++ < 8) {
		printf("focaltech: finger %u %s at %u, %u\n", id,
		    event == FOCALTECH_EVENT_DOWN ? "down" : "move", x, y);
	}
}

/* Panel size in the FT3680 super-high-resolution reports (device tree x_max/y_max on redwood). */
static uint32_t focaltech_max_x = 10800, focaltech_max_y = 24000;

static void
focaltech_report(const uint8_t *report)
{
	uint32_t count = report[1] & 0x0f;

	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		focaltech_contact[i].seen = false;
	}
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		const uint8_t *touch = report + 2 + 6 * i;
		uint32_t id = (touch[2] >> 4) & 0x0f;
		uint32_t event, x, y;

		if (id >= FOCALTECH_MAX_TOUCHES) {
			break;			/* no more valid slots in this report */
		}
		event = (touch[0] >> 6) & 0x3;
		x = ((uint32_t)(touch[0] & 0x0f) << 11) | ((uint32_t)touch[1] << 3) |
		    ((uint32_t)touch[4] >> 5);
		y = ((uint32_t)(touch[2] & 0x0f) << 11) | ((uint32_t)touch[3] << 3) |
		    (((uint32_t)touch[4] & 0x1c) >> 2);
		focaltech_contact_update(id, event, x, y, focaltech_max_x, focaltech_max_y);
	}
	(void)count;
	/* Fingers not in this report are up (vendor driver uses a bitmap; we remember explicitly). */
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (!focaltech_contact[i].seen && focaltech_contact[i].down) {
			focaltech_contact[i].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, focaltech_contact[i].x, focaltech_contact[i].y);
		}
	}
}

static void
focaltech_poll(void)
{
	static uint8_t report[FOCALTECH_TOUCH_REPORT_LEN];
	static unsigned bad;

	seen.polls++;
	if (!focaltech_read(FOCALTECH_TOUCH_DATA, report, sizeof(report))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("focaltech: SPI read failed for touch report\n");
		}
		return;
	}
	seen.reports++;
	focaltech_report(report);
}

/* ---- the Synaptics S3910 chip (TCM over SPI) -------------------------------------------------------- */

/*
 * From the phone's own driver (vendor/oplus/kernel/touchpanel/oplus_touchscreen_v2/Synaptics/
 * Syna_tcm_S3910): a command is one transfer, [command][length low][length high][payload]; the
 * chip answers on the interrupt line with [0xA5][code][length low][length high][payload][0x5A],
 * reads and writes being the same number of bytes, 0xFF on the master's data line while the chip
 * drives the other. A message longer than the nine bytes read up front continues with transfers
 * that begin [0xA5][0x03] and carry the rest. The touch report is a bit string whose layout the
 * chip itself describes, as a list of (code, width in bits) pairs: 0 ends it, 1..3 loop over the
 * objects (the active ones only or all of them), 4 rounds the position up to the next byte, 6 is
 * an object's index, 7 what it is (1 a finger, 2 a gloved one), 8 and 9 where it is, and 24 how
 * many objects the loop holds. Fields are least significant bit first.
 */
#define SYNA_MARKER             0xa5
#define SYNA_HEADER_SIZE        4
#define SYNA_WINDOW             48              /* the read s13 and s14 proved: header and most payloads */
#define SYNA_MAX_MESSAGE        1024
#define SYNA_MAX_TOUCHES        10              /* contact[] is shared with the Goodix chip: same size */

#define SYNA_CMD_IDENTIFY       0x02
#define SYNA_CMD_ENABLE_REPORT  0x05
#define SYNA_CMD_RUN_APP        0x14
#define SYNA_CMD_GET_TOUCH_REPORT_CONFIG 0x25

#define SYNA_STATUS_OK          0x01
#define SYNA_STATUS_ERROR       0x0f

#define SYNA_REPORT_IDENTIFY    0x10
#define SYNA_REPORT_TOUCH       0x11

#define SYNA_MODE_APPLICATION   0x01
#define SYNA_MODE_BOOTLOADER    0x0b

static uint8_t syna_tx[SYNA_MAX_MESSAGE];
static uint8_t syna_message[SYNA_MAX_MESSAGE];          /* the last message: header, payload, padding */
static uint32_t syna_message_length;                    /* its payload length */
static uint8_t syna_message_code;
static uint8_t syna_config[SYNA_MAX_MESSAGE];           /* the touch report layout the chip describes */
static uint32_t syna_config_length;

/* One transfer, `length` bytes each way; on a read the master's data line idles high (0xFF). */
static bool
syna_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	if (length > sizeof(syna_tx)) {
		return false;
	}
	if (tx) {
		memcpy(syna_tx, tx, length);
	} else {
		memset(syna_tx, 0xff, length);
	}
	return spi_transfer(syna_tx, rx, length);
}

/* The chip raises the attention line when a message is waiting; wait for it to fall. */
static bool
syna_wait_attention(uint32_t timeout_ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(timeout_ms, NSEC_PER_MSEC, &deadline);
	while (gpio_get(PIN_INT)) {
		if (mach_absolute_time() >= deadline) {
			return false;
		}
		sleep_us(200);
	}
	return true;
}

/*
 * Read one message into syna_message; false when there is nothing readable. The chip streams
 * [a5][code][length][payload] and pads the rest of the window with 5a (s13); the length field is what
 * counts, and a command answer carries no 5a of its own (s8: the 128 byte layout answer ends in ff).
 * A message longer than the window is finished with one more read of just the bytes that are missing:
 * the chip continues where the window stopped (s8), and over reading is what broke the next command
 * in s15.
 */
static bool
syna_read_message(void)
{
	uint32_t total;

	if (!syna_transfer(NULL, syna_message, SYNA_WINDOW) || syna_message[0] != SYNA_MARKER) {
		return false;
	}
	syna_message_code = syna_message[1];
	syna_message_length = le16(syna_message + 2);
	if (syna_message_code < SYNA_REPORT_IDENTIFY && syna_message_code != SYNA_STATUS_OK) {
		return false;           /* idle, busy, an error */
	}
	total = SYNA_HEADER_SIZE + syna_message_length;
	if (total > sizeof(syna_message)) {
		return false;
	}
	if (total > SYNA_WINDOW && !syna_transfer(NULL, syna_message + SYNA_WINDOW, total - SYNA_WINDOW)) {
		return false;
	}
	return true;
}

/*
 * Take whatever is already waiting off the line. A command written while the attention line is still
 * low is swallowed by the chip (s8: the layout request after the announcement got no answer at all),
 * so the line is emptied first; reports thrown away here are the price of that.
 */
static void
syna_drain(void)
{
	uint32_t drained = 0;

	while (drained < 8 && !gpio_get(PIN_INT) && syna_read_message()) {
		drained++;
	}
	if (drained) {
		printf("synaptics: %u message(s) were still on the line, taken off\n", drained);
	}
}

/* Send a command and wait for its answer, which stays in syna_message. */
static bool
syna_command(uint8_t command, const uint8_t *payload, uint32_t length, uint32_t timeout_ms)
{
	uint8_t out[64];
	uint64_t deadline;
	uint32_t rejected = 0;

	if (length + 3 > sizeof(out)) {
		return false;
	}
	out[0] = command;
	out[1] = (uint8_t)length;
	out[2] = (uint8_t)(length >> 8);
	if (length) {
		memcpy(out + 3, payload, length);
	}
	syna_drain();
	if (!syna_transfer(out, NULL, length + 3)) {
		return false;
	}
	clock_interval_to_deadline(timeout_ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (!gpio_get(PIN_INT)) {
			if (syna_read_message()) {
				if (syna_message_code == SYNA_REPORT_IDENTIFY || syna_message_code == SYNA_STATUS_OK) {
					return true;    /* (a touch report ahead of the answer is dropped) */
				}
				if (syna_message_code == SYNA_STATUS_ERROR) {
					return false;
				}
				if (syna_message_code != SYNA_REPORT_TOUCH) {
					printf("synaptics: while waiting for command %#x: code %#x, length %u\n", command,
					    syna_message_code, syna_message_length);
				}
			} else if (rejected++ < 3) {
				printf("synaptics: command %#x: the line carried %02x %02x %02x %02x, not a message\n", command,
				    syna_message[0], syna_message[1], syna_message[2], syna_message[3]);
			}
		}
		sleep_us(200);
	}
	if (rejected) {
		printf("synaptics: command %#x: %u reads found nothing usable\n", command, rejected);
	}
	return false;
}

/*
 * Send one command and print whatever comes back, raw: for the probes only. The chip is asked by
 * writing [command][length low][length high] in one chip select, and answers, if at all, on the
 * attention line within the timeout.
 */
static void
syna_show_answer(uint8_t command, uint32_t timeout_ms)
{
	static uint8_t frame[3];
	static uint8_t raw[48];
	uint64_t deadline;
	uint32_t seen = 0;

	frame[0] = command;
	frame[1] = 0;
	frame[2] = 0;
	if (!syna_transfer(frame, NULL, sizeof(frame))) {
		printf("synaptics: probe %#x: the write itself failed\n", command);
		return;
	}
	printf("synaptics: probe %#x: written, int %u\n", command, gpio_get(PIN_INT));
	clock_interval_to_deadline(timeout_ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (!gpio_get(PIN_INT)) {
			if (!syna_transfer(NULL, raw, sizeof(raw))) {
				printf("synaptics: probe %#x: the read failed\n", command);
				return;
			}
			seen++;
			printf("synaptics: probe %#x: answer %u: %02x %02x %02x %02x, length %u, int %u\n", command,
			    seen, raw[0], raw[1], raw[2], raw[3], le16(raw + 2), gpio_get(PIN_INT));
			printf("synaptics: probe %#x:   then %02x %02x %02x %02x %02x %02x %02x %02x\n", command,
			    raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11]);
			return;
		}
		sleep_us(200);
	}
	printf("synaptics: probe %#x: nothing came back, int %u\n", command, gpio_get(PIN_INT));
}

/*
 * What the chip says it is: the first byte of the payload is the firmware mode. Asked for, the answer
 * comes with a plain OK code and the identification as payload (s14), not with the announcement code.
 */
static uint8_t
synaptics_identify(void)
{
	if (!syna_command(SYNA_CMD_IDENTIFY, NULL, 0, 1000) || syna_message_length < 1) {
		return 0;
	}
	return syna_message[SYNA_HEADER_SIZE];
}

/*
 * After a reset the chip announces itself, unasked: the message is already waiting on the line. Ask only
 * when nothing is there; asking over a message that is still half read gets an empty answer, which is
 * what the first probe of corvette saw (mode 0 while the line was carrying a5 10 1c 00 01 ...).
 */
static uint8_t
synaptics_identify_first(void)
{
	if (syna_wait_attention(500) && syna_read_message() && syna_message_code == SYNA_REPORT_IDENTIFY) {
		return syna_message[SYNA_HEADER_SIZE];
	}
	return synaptics_identify();
}

/* Reset the chip, get its firmware running, and learn where it puts the fields of a touch report. */
static bool
synaptics_probe(bool verbose)
{
	static const uint8_t enable[2] = { SYNA_REPORT_TOUCH, 0 };
	uint8_t mode;
	uint32_t shown;

	touch_reset();                  /* the vendor: in reset 10 ms, 80 ms more before the first word */
	mode = synaptics_identify_first();
	printf("synaptics: firmware mode %#x after reset\n", mode);
	if (mode == SYNA_MODE_BOOTLOADER) {
		syna_command(SYNA_CMD_RUN_APP, NULL, 0, 1000);
		sleep_ms(200);
		mode = synaptics_identify_first();
		printf("synaptics: firmware mode %#x after the switch\n", mode);
	}
	if (mode != SYNA_MODE_APPLICATION) {
		if (verbose) {
			printf("synaptics: the application firmware is not running\n");
		}
		return false;
	}

	if (!syna_command(SYNA_CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0, 1000)) {
		/* s16: the very same request answers in s8 and is lost here, so it is asked again */
		sleep_ms(50);
		if (!syna_command(SYNA_CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0, 1000)) {
			sleep_ms(100);
			if (!syna_command(SYNA_CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0, 1000)) {
				printf("synaptics: the layout request went unanswered\n");
				return false;
			}
		}
	}
	printf("synaptics: the layout answer: code %#x, length %u\n", syna_message_code, syna_message_length);
	if (syna_message_code != SYNA_STATUS_OK) {
		printf("synaptics: the layout answer is not an OK status\n");
		return false;
	}
	syna_config_length = syna_message_length < sizeof(syna_config) ? syna_message_length : sizeof(syna_config);
	memcpy(syna_config, syna_message + SYNA_HEADER_SIZE, syna_config_length);
	printf("synaptics: touch report layout, %u bytes:", syna_config_length);
	for (shown = 0; shown < syna_config_length && shown < 32; shown++) {
		printf(" %02x", syna_config[shown]);
	}
	printf("%s\n", syna_config_length > 32 ? " ..." : "");

	if (syna_command(SYNA_CMD_ENABLE_REPORT, enable, 1, 300)) {
		printf("synaptics: touch reports enabled\n");
	} else {
		printf("synaptics: the report enable was not accepted (touches may still come)\n");
	}
	return true;
}

/* The layout's interpreter: where a field sits is only in the chip's own list. */
static uint8_t syna_object_status[SYNA_MAX_TOUCHES];
static uint32_t syna_object_x[SYNA_MAX_TOUCHES], syna_object_y[SYNA_MAX_TOUCHES];
static bool syna_object_seen[SYNA_MAX_TOUCHES];

static uint32_t
syna_field(uint32_t offset, uint32_t bits)
{
	uint32_t value = 0;

	if (bits > 32) {
		return 0;
	}
	for (uint32_t i = 0; i < bits; i++) {
		uint32_t bit = offset + i;

		if (bit / 8 >= syna_message_length) {
			break;                  /* the report is shorter than its layout says */
		}
		value |= (uint32_t)((syna_message[SYNA_HEADER_SIZE + bit / 8] >> (bit % 8)) & 1) << i;
	}
	return value;
}

/* The compositor copes badly with a move for every poll of 2 ms (s8: a flicker while swiping). */
static bool
syna_move_due(void)
{
	static uint64_t last;
	uint64_t now, last_ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	absolutetime_to_nanoseconds(last, &last_ns);
	if (now - last_ns < 8000000) {  /* 8 ms: at most 125 moves a second */
		return false;
	}
	last = mach_absolute_time();
	return true;
}

static void
synaptics_report(void)
{
	uint32_t idx = 0, offset = 0, obj = 0, next = 0, active_objects = 0, loops = 0;
	bool active_only = false, counted = false;

	for (uint32_t i = 0; i < SYNA_MAX_TOUCHES; i++) {
		syna_object_seen[i] = false;
		contact[i].seen = false;
	}
	while (idx < syna_config_length) {
		uint32_t code = syna_config[idx++];

		switch (code) {
		case 0:                 /* TOUCH_END */
			idx = syna_config_length;
			break;
		case 1:                 /* TOUCH_FOREACH_ACTIVE_OBJECT */
			obj = 0;
			next = idx;
			active_only = true;
			break;
		case 2:                 /* TOUCH_FOREACH_OBJECT */
			obj = 0;
			next = idx;
			active_only = false;
			break;
		case 3: {               /* TOUCH_FOREACH_END */
			uint32_t end = idx - 1;

			if (!active_only) {
				if (++obj < SYNA_MAX_TOUCHES && end > next) {
					idx = next;
				}
			} else if (counted) {
				if (++loops < active_objects && end > next) {
					idx = next;
				}
			} else if (end > next && offset < syna_message_length * 8) {
				idx = next;
			}
			break;
		}
		case 4:                 /* TOUCH_PAD_TO_NEXT_BYTE */
			offset = (offset + 7) & ~7u;
			break;
		case 6:                 /* TOUCH_OBJECT_N_INDEX */
		case 7:                 /* TOUCH_OBJECT_N_CLASSIFICATION */
		case 8:                 /* TOUCH_OBJECT_N_X_POSITION */
		case 9: {               /* TOUCH_OBJECT_N_Y_POSITION */
			uint32_t bits = idx < syna_config_length ? syna_config[idx++] : 0;
			uint32_t value = syna_field(offset, bits);

			offset += bits;
			if (code == 6) {
				obj = value < SYNA_MAX_TOUCHES ? value : SYNA_MAX_TOUCHES;
			} else if (obj < SYNA_MAX_TOUCHES) {
				if (code == 7) {
					syna_object_status[obj] = (uint8_t)value;
					syna_object_seen[obj] = true;
				} else if (code == 8) {
					syna_object_x[obj] = value;
				} else {
					syna_object_y[obj] = value;
				}
			}
			break;
		}
		case 24: {              /* TOUCH_NUM_OF_ACTIVE_OBJECTS */
			uint32_t bits = idx < syna_config_length ? syna_config[idx++] : 0;

			active_objects = syna_field(offset, bits);
			offset += bits;
			counted = true;
			if (active_objects == 0) {
				idx = syna_config_length;       /* nothing down: every finger has lifted */
			}
			break;
		}
		default:
			/* every other code carries its width in bits next */
			if (idx < syna_config_length) {
				offset += syna_config[idx++];
			}
			break;
		}
	}

	for (uint32_t i = 0; i < SYNA_MAX_TOUCHES; i++) {
		uint32_t x, y;

		if (!syna_object_seen[i] || (syna_object_status[i] != 1 && syna_object_status[i] != 2)) {
			continue;
		}
		x = syna_object_x[i];
		y = syna_object_y[i];
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		x = x >= max_x ? 65535 : x * 65535 / max_x;
		y = y >= max_y ? 65535 : y * 65535 / max_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		contact[i].seen = true;
		if (!contact[i].down) {
			printf("synaptics: down at %u, %u (of %u x %u)\n", seen.last_x, seen.last_y, max_x, max_y);
			hakios_touch_post(HAKIOS_TOUCH_DOWN, i, x, y);
		} else if ((contact[i].x != x || contact[i].y != y) && syna_move_due()) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, i, x, y);
		}
		contact[i].down = true;
		contact[i].x = x;
		contact[i].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t i = 0; i < SYNA_MAX_TOUCHES; i++) {
		if (contact[i].down && !contact[i].seen) {
			contact[i].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, contact[i].x, contact[i].y);
		}
	}
}

static void
synaptics_poll(void)
{
	static unsigned bad;

	seen.polls++;
	if (!syna_read_message()) {
		seen.bad++;
		if (bad < 2) {
			/* what the chip's data line says, when the header does not: all 0xFF is a dead line */
			printf("synaptics: no message: %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    syna_message[0], syna_message[1], syna_message[2], syna_message[3], syna_message[4],
			    syna_message[5], syna_message[6], syna_message[7], syna_message[8]);
		} else if (bad < 8) {
			printf("synaptics: no readable message\n");
		}
		bad++;
		return;
	}
	if (syna_message_code == SYNA_REPORT_TOUCH) {
		seen.reports++;
		synaptics_report();
	} else if (syna_message_code == SYNA_REPORT_IDENTIFY) {
		printf("synaptics: the chip identified itself again (mode %#x)\n", syna_message[SYNA_HEADER_SIZE]);
	}
}

/* Which touch chip is on this board. */
static enum touch_chip touch_chip;

static enum touch_chip
touch_chip_detect(void)
{
	if (board->pin_goodix_id < 0) {
		return board->chip;             /* no strap pins: the board's device tree says which chip */
	}
	gpio_configure(PIN_FOCALTECH_ID, 0, PULL_NONE, 2, false);
	gpio_configure(PIN_GOODIX_ID, 0, PULL_NONE, 2, false);
	if (gpio_get(PIN_GOODIX_ID) && !gpio_get(PIN_FOCALTECH_ID)) {
		return TOUCH_FOCALTECH;
	}
	return TOUCH_GOODIX;
}

static bool
chip_probe(bool verbose)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		return focaltech_probe(verbose);
	}
	if (touch_chip == TOUCH_SYNAPTICS) {
		return synaptics_probe(verbose);
	}
	touch_reset();
	return goodix_probe(verbose);
}

static void
chip_poll(void)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		focaltech_poll();
	} else if (touch_chip == TOUCH_SYNAPTICS) {
		synaptics_poll();
	} else {
		goodix_poll();
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	if (board->engine) {
		printf("goodix: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins cs/clk/mosi/miso cfg %#x %#x %#x %#x,"
		    " reset %#x/%u, int %#x/%u", when, rd(GCC_VOTE), rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR),
		    rd(GCC_QUP1_SE5_CBCR), rd(GCC_QUP1_SE5_CFG), rd(GCC_QUP1_SE5_DFSR), rd(GPIO_CFG(PIN_CS)),
		    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_RESET)),
		    rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)));
	} else {
		printf("goodix: %s: pins cs/clk/mosi/miso cfg %#x %#x %#x %#x, reset %#x/%u, int %#x/%u", when,
		    rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_MISO)),
		    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)));
	}
	if (board->pin_avdd >= 0) {
		printf(", avdd %#x/%u", rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)));
	}
	if (board->pin_goodix_id >= 0) {
		printf(", straps %u %u", gpio_get(PIN_GOODIX_ID), gpio_get(PIN_FOCALTECH_ID));
	}
	printf("\n");
}

/* Power the chip up from nothing, in the vendor driver's order. */
static void
power_on(void)
{
	gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	if (!regulator_on(board->iovdd, board->iovdd_uv, board->regulator_mode)) {
		printf("goodix: the I/O supply did not answer\n");
	}
	sleep_ms(3);
	if (board->pin_avdd >= 0) {
		gpio_output(PIN_AVDD, true, PULL_NONE, 2);
	} else if (!regulator_on(board->avdd, board->avdd_uv, board->regulator_mode)) {
		printf("goodix: the analog supply did not answer\n");
	}
	sleep_ms(board->power_delay_ms);
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

static const char *
chip_name(void)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		return "focaltech";
	}
	return touch_chip == TOUCH_SYNAPTICS ? "synaptics" : "goodix";
}

static bool
bring_up(void)
{
	show_state("as found");
	touch_chip = touch_chip_detect();
	printf("goodix: selected %s\n", chip_name());
	max_x = board->raw_max_x;
	max_y = board->raw_max_y;
	power_on();

	if (board->engine) {
		const char *problem = "its clocks cannot be turned on";

		if (clocks_on()) {
			uint32_t spi_hz = 1000000;

			printf("goodix: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
			    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
			/* boot-arg touch_spi_hz=N: the bus at N Hz (Android's device tree gives the chip 5 MHz) */
			PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
			if (spi_hz < 100000 || spi_hz > 10000000) {
				spi_hz = 1000000;
			}
			printf("goodix: the bus at %u Hz\n", spi_hz);
			problem = geni_spi_init(spi_hz);
		}
		if (problem == NULL) {
			use_engine_pins();
			spi_transfer = geni_transfer;
			show_state("powered, on the serial engine");
			if (chip_probe(true)) {
				return true;
			}
			printf("goodix: %s did not answer through the serial engine; trying the pins by hand\n", chip_name());
		} else {
			printf("goodix: %s; driving the pins by hand\n", problem);
		}
	} else {
		printf("goodix: this board drives the bus by hand\n");
	}

	/*
	 * By hand: on redwood which of the two data pins is the input is not written down, so try
	 * both; on corvette the device tree has it (gpio48 is the input), and turning the other one
	 * into an output would fight the chip's own driver of that line.
	 */
	spi_transfer = bitbang_transfer;
	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	if (chip_probe(true)) {
		printf("goodix: %s answers on pins driven by hand (data in on gpio%u)\n", chip_name(), bitbang_in);
		return true;
	}
	if (board->swap_data_pins) {
		bitbang_in = PIN_MOSI;
		bitbang_out = PIN_MISO;
		bitbang_init();
		if (chip_probe(true)) {
			printf("goodix: %s answers on pins driven by hand (data in on gpio%u)\n", chip_name(), bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
goodix_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000, latency = 0, reads = 0;
	uint64_t before = 0, reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* a report read every 80 ms even with the line high, in case it is missed */

	uint32_t selected = 0;

	PE_parse_boot_argn("touch_board", &selected, sizeof(selected));
	board = selected == 1 ? &board_corvette : &board_redwood;
	if (reset_high < 0) {
		reset_high = board->reset_high ? 1 : 0;
	}
	report_rate = board->chip == TOUCH_SYNAPTICS ? "60" : "240";

	uint32_t stage = 100;

	/* boot-arg touch_stage=N: stop after step N of the bring-up, to find the write that resets the board */
	PE_parse_boot_argn("touch_stage", &stage, sizeof(stage));
	if (stage < 100) {
		printf("goodix: staged bring-up, stage %u\n", stage);
	}
	if (stage == 0) {
		printf("goodix: stage 0: the thread runs, nothing mapped\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage < 100) {
		printf("goodix: mapping tlmm %#lx for %#lx bytes\n", (unsigned long)board->tlmm_phys,
		    (unsigned long)board->tlmm_size);
	}
	tlmm = ml_io_map(board->tlmm_phys, board->tlmm_size);
	if (stage < 100) {
		printf("goodix: tlmm mapped at %#lx\n", (unsigned long)tlmm);
	}
	if (board->engine) {
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		se = ml_io_map(board->se_phys, SE_SIZE);
	}
	if (stage < 100) {
		printf("goodix: mapping rsc %#lx\n", (unsigned long)board->rsc_phys);
	}
	rsc = ml_io_map(board->rsc_phys, RSC_SIZE);
	if (stage < 100) {
		printf("goodix: rsc mapped at %#lx\n", (unsigned long)rsc);
	}
	rsc_read_layout();
	rsc_pick_slot();
	cmd_db = ml_io_map_wcomb(board->cmd_db_phys, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	if (stage < 100) {
		printf("goodix: cmd_db mapped at %#lx\n", (unsigned long)cmd_db);
	}

	if (stage == 7) {
		printf("goodix: stage 7: one read at a time, to see which address stalls\n");
		printf("goodix: stage 7: cmd_db[0] before the read\n");
		printf("goodix: stage 7: cmd_db[0] = %#x\n", rd(cmd_db));
		printf("goodix: stage 7: tlmm+0x0 before the read\n");
		printf("goodix: stage 7: tlmm+0x0 = %#x\n", rd(tlmm));
		printf("goodix: stage 7: tlmm+0x1000 before the read\n");
		printf("goodix: stage 7: tlmm+0x1000 = %#x\n", rd(tlmm + 0x1000));
		printf("goodix: stage 7: tlmm+0x33000 (cs, gpio51) before the read\n");
		printf("goodix: stage 7: tlmm+0x33000 = %#x\n", rd(tlmm + 0x33000));
		printf("goodix: stage 7 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 20) {
		uint32_t base = rpmh_address(board->iovdd);
		unsigned chosen = 3;
		uint32_t value = 0;

		printf("goodix: stage 20: RPMh, a read only: the I/O supply %s is at %#x, the layout is %s\n", board->iovdd, base,
		    rsc_regs == &rsc_layout_v3 ? "the 3.x one" : rsc_regs == &rsc_layout_v2_7 ? "the 2.7 one" : "not known");
		for (unsigned set = 0; rsc_regs != NULL && set < 3; set++) {
			rsc_slot = set;
			printf("goodix: stage 20: set %u: status %#x, control %#x, cmd enable %#x, wait %#x\n", set,
			    rd(TCS(rsc_regs->status)), rd(TCS(rsc_regs->control)), rd(TCS(rsc_regs->cmd_enable)),
			    rd(TCS(rsc_regs->wait)));
		}
		/* Every set, every command: which of them name the supply, and with what. The boot loader's own
		 * votes are still there, and they say whether it left the supply already on. */
		for (unsigned set = 0; rsc_regs != NULL && set < 8; set++) {
			rsc_slot = set;
			for (unsigned i = 0; i < 16; i++) {
				uint32_t addr = rd(TCS(rsc_regs->cmd_addr + rsc_regs->cmd_stride * i));

				if (addr >= base && addr < base + 0xc) {
					printf("goodix: stage 20: set %u, command %u: msgid %#x, address %#x, data %#x, status %#x\n",
					    set, i, rd(TCS(rsc_regs->cmd_msgid + rsc_regs->cmd_stride * i)), addr,
					    rd(TCS(rsc_regs->cmd_data + rsc_regs->cmd_stride * i)),
					    rd(TCS(rsc_regs->cmd_status + rsc_regs->cmd_stride * i)));
				}
			}
		}
		/* The three sets of the "active" group (dt: qcom,tcs-config): the first that the boot loader left
		 * no request in is the one to use. Set 0 it did, with seven commands still enabled. */
		for (unsigned set = 0; rsc_regs != NULL && set < 3; set++) {
			rsc_slot = set;
			if (rd(TCS(rsc_regs->cmd_enable)) == 0 && rd(TCS(rsc_regs->status)) != 0) {
				chosen = set;
				break;
			}
		}
		if (base == 0 || rsc_regs == NULL) {
			printf("goodix: stage 20: no supply address or no known layout: nothing is sent\n");
			printf("goodix: stage 20 done\n");
			thread_terminate(current_thread());
			return;
		}
		if (chosen == 3) {
			printf("goodix: stage 20: every active set is in use: nothing is sent\n");
		} else {
			rsc_slot = chosen;
			printf("goodix: stage 20: set %u is free: reading the enable of %s\n", chosen, board->iovdd);
			if (rpmh_read(base + VRM_ENABLE, &value)) {
				printf("goodix: stage 20: the answer: enable %#x\n", value);
				if (rpmh_read(base + VRM_VOLTAGE, &value)) {
					printf("goodix: stage 20: the answer: voltage %#x\n", value);
				} else {
					printf("goodix: stage 20: the voltage read did not answer\n");
				}
			} else {
				printf("goodix: stage 20: the enable read did not answer\n");
			}
		}
		printf("goodix: stage 20 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 10) {
		/*
		 * Everything of the power-up that does not go through RPMh: no regulator write at all, only the
		 * analog supply pin, a reset pulse and the pins driven by hand. If the chip answers here, the
		 * I/O supply is already on and the writes that took the device down three times are not needed.
		 */
		printf("goodix: stage 10: no RPMh: avdd, a reset pulse, then IDENTIFY on pins driven by hand\n");
		gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
		gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		sleep_ms(board->power_delay_ms);
		touch_reset();
		printf("goodix: stage 10: after the reset pulse: reset level %u, int level %u", rd(GPIO_IO(PIN_RESET)) & 1,
		    rd(GPIO_IO(PIN_INT)) & 1);
		if (board->pin_avdd >= 0) {
			printf(", avdd level %u", rd(GPIO_IO(PIN_AVDD)) & 1);
		}
		printf("\n");
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		printf("goodix: stage 10: identify says mode %#x\n", synaptics_identify());
		printf("goodix: stage 10 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 11) {
		/*
		 * One command only, into the set the boot loader did not touch: is it the write itself, or one
		 * of the three registers, that the controller refuses? Nothing else is touched here.
		 */
		uint32_t base = rpmh_address(board->iovdd);
		uint32_t address[1] = { base + VRM_ENABLE };
		uint32_t data[1] = { 1 };

		rsc_slot = 2;
		printf("goodix: stage 11: one write only: the enable of %s (%#x), on slot set 2\n", board->iovdd, base);
		if (base == 0 || rsc_regs == NULL) {
			printf("goodix: stage 11: no supply address or no known layout: nothing is sent\n");
		} else if (rpmh_write(address, data, 1)) {
			printf("goodix: stage 11: the write was taken\n");
		} else {
			printf("goodix: stage 11: the write did not confirm\n");
		}
		printf("goodix: stage 11 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 12) {
		/*
		 * The one write that s11 showed is taken, then the chip, and then whatever it has already put on
		 * the line by itself. Nothing is asked for: after a reset the announcement is waiting.
		 */
		uint32_t base = rpmh_address(board->iovdd);
		uint32_t address[1] = { base + VRM_ENABLE };
		uint32_t data[1] = { 1 };

		rsc_slot = 2;
		printf("goodix: stage 12: the enable of %s (%#x) first, then the chip\n", board->iovdd, base);
		if (base != 0 && rsc_regs != NULL) {
			printf("goodix: stage 12: %s\n", rpmh_write(address, data, 1) ? "enabled" : "did not confirm");
		}
		sleep_ms(3);
		gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
		gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		sleep_ms(board->power_delay_ms);
		touch_reset();
		printf("goodix: stage 12: after the reset pulse: reset %u, int %u", rd(GPIO_IO(PIN_RESET)) & 1,
		    rd(GPIO_IO(PIN_INT)) & 1);
		if (board->pin_avdd >= 0) {
			printf(", avdd %u", rd(GPIO_IO(PIN_AVDD)) & 1);
		}
		printf("\n");
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		if (syna_wait_attention(1000) && syna_read_message()) {
			printf("goodix: stage 12: it announced itself: code %#x, length %u, mode %#x, then %02x %02x"
			    " %02x %02x %02x %02x\n", syna_message_code, syna_message_length, syna_message[4],
			    syna_message[5], syna_message[6], syna_message[7], syna_message[8], syna_message[9],
			    syna_message[10]);
		} else {
			printf("goodix: stage 12: nothing was announced (int %u)\n", gpio_get(PIN_INT));
		}
		printf("goodix: stage 12 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 15) {
		/*
		 * The 133 byte answer to 0x25 taken in three windows of 48: s14 showed that the chip continues
		 * where a window stopped rather than starting over, and that a 256 byte window fails, so this
		 * shows what each window carries and whether a continuation is marked at its head.
		 */
		uint32_t base = rpmh_address(board->iovdd);
		uint32_t address[1] = { base + VRM_ENABLE };
		uint32_t data[1] = { 1 };
		static uint8_t frame[3] = { SYNA_CMD_GET_TOUCH_REPORT_CONFIG, 0, 0 };
		static uint8_t chunk[48];
		uint32_t window;

		rsc_slot = 2;
		printf("goodix: stage 15: the answer to 0x25 in three windows of 48\n");
		if (base != 0 && rsc_regs != NULL) {
			printf("goodix: stage 15: %s\n", rpmh_write(address, data, 1) ? "enabled" : "did not confirm");
		}
		sleep_ms(3);
		gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
		gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		sleep_ms(board->power_delay_ms);
		touch_reset();
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		if (syna_wait_attention(1000) && syna_read_message()) {
			printf("goodix: stage 15: the announcement: code %#x, mode %#x\n", syna_message_code,
			    syna_message[SYNA_HEADER_SIZE]);
		} else {
			printf("goodix: stage 15: no announcement, int %u\n", gpio_get(PIN_INT));
		}
		if (!syna_transfer(frame, NULL, sizeof(frame))) {
			printf("goodix: stage 15: the write itself failed\n");
		}
		printf("goodix: stage 15: after the write, int %u\n", gpio_get(PIN_INT));
		for (window = 0; window < 3; window++) {
			if (!syna_wait_attention(500)) {
				printf("goodix: stage 15: window %u: no attention, int %u\n", window,
				    gpio_get(PIN_INT));
				break;
			}
			if (!syna_transfer(NULL, chunk, sizeof(chunk))) {
				printf("goodix: stage 15: window %u: the read failed\n", window);
				break;
			}
			printf("goodix: stage 15: window %u: %02x %02x %02x %02x %02x %02x %02x %02x"
			    " ... %02x %02x %02x %02x, int %u\n", window, chunk[0], chunk[1], chunk[2], chunk[3],
			    chunk[4], chunk[5], chunk[6], chunk[7], chunk[44], chunk[45], chunk[46], chunk[47],
			    gpio_get(PIN_INT));
		}
		printf("goodix: stage 15 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 14) {
		/*
		 * The other direction: up to s13 only the chip had spoken. First its announcement as the answer
		 * that it is good, then three commands the chip must answer, the identification being the one
		 * whose answer is already known, so that a silent 0x25 can be told from a silent chip.
		 */
		uint32_t base = rpmh_address(board->iovdd);
		uint32_t address[1] = { base + VRM_ENABLE };
		uint32_t data[1] = { 1 };

		rsc_slot = 2;
		printf("goodix: stage 14: the enable, the reset, then commands written to the chip\n");
		if (base != 0 && rsc_regs != NULL) {
			printf("goodix: stage 14: %s\n", rpmh_write(address, data, 1) ? "enabled" : "did not confirm");
		}
		sleep_ms(3);
		gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
		gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		sleep_ms(board->power_delay_ms);
		touch_reset();
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		if (syna_wait_attention(1000) && syna_read_message()) {
			printf("goodix: stage 14: the announcement: code %#x, mode %#x, int %u\n", syna_message_code,
			    syna_message[SYNA_HEADER_SIZE], gpio_get(PIN_INT));
		} else {
			printf("goodix: stage 14: no announcement, int %u\n", gpio_get(PIN_INT));
		}
		syna_show_answer(SYNA_CMD_IDENTIFY, 1000);
		syna_show_answer(0x0c, 1000);
		syna_show_answer(SYNA_CMD_GET_TOUCH_REPORT_CONFIG, 1000);
		syna_show_answer(SYNA_CMD_ENABLE_REPORT, 1000);
		printf("goodix: stage 14 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 13) {
		/*
		 * The line itself, without any parsing: the enable that s11 showed is safe, the reset pulse, and
		 * then one long read that shows the whole announcement as the chip laid it out. Whatever the bytes
		 * are, they are printed, so that the framing of a long message stops being a guess.
		 */
		uint32_t base = rpmh_address(board->iovdd);
		uint32_t address[1] = { base + VRM_ENABLE };
		uint32_t data[1] = { 1 };
		static uint8_t raw[48];
		uint64_t deadline;
		uint32_t total, i, polls;

		rsc_slot = 2;
		printf("goodix: stage 13: the enable of %s (%#x), then one read of %u bytes\n", board->iovdd, base,
		    (uint32_t)sizeof(raw));
		if (base != 0 && rsc_regs != NULL) {
			printf("goodix: stage 13: %s\n", rpmh_write(address, data, 1) ? "enabled" : "did not confirm");
		}
		sleep_ms(3);
		gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
		gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		sleep_ms(board->power_delay_ms);
		touch_reset();
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		/*
		 * s13 answered "too early": the line was still high when the read went out, and the announcement
		 * arrived only afterwards. So wait for the line to fall, then read once: every chip select starts
		 * the message over at its first byte, which is why a second, separate read cannot continue one.
		 */
		polls = 0;
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		while (gpio_get(PIN_INT) && mach_absolute_time() < deadline) {
			sleep_us(200);
			polls++;
		}
		printf("goodix: stage 13: after the reset pulse: reset %u, int %u after %u polls of 200 us\n",
		    rd(GPIO_IO(PIN_RESET)) & 1, gpio_get(PIN_INT), polls);
		if (!syna_transfer(NULL, raw, sizeof(raw))) {
			printf("goodix: stage 13: the transfer itself failed\n");
		} else {
			for (i = 0; i < sizeof(raw); i += 8) {
				printf("goodix: stage 13: %u: %02x %02x %02x %02x %02x %02x %02x %02x\n", i, raw[i],
				    raw[i + 1], raw[i + 2], raw[i + 3], raw[i + 4], raw[i + 5], raw[i + 6], raw[i + 7]);
			}
			total = SYNA_HEADER_SIZE + le16(raw + 2) + 1;
			printf("goodix: stage 13: marker %02x, code %#x, length %u, total %u, last %02x, int %u\n",
			    raw[0], raw[1], le16(raw + 2), total, total < sizeof(raw) ? raw[total - 1] : 0,
			    gpio_get(PIN_INT));
		}
		printf("goodix: stage 13 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 9) {
		uint32_t base = rpmh_address(board->iovdd);

		rsc_slot = 0;
		printf("goodix: stage 9: RPMh, reads only: the I/O supply %s is at %#x\n", board->iovdd, base);
		printf("goodix: stage 9: slot set 0: irq enable %#x, irq status %#x\n", rd(TCS(0x00)), rd(TCS(0x04)));
		printf("goodix: stage 9: slot set 0: 2.7 wait %#x, control %#x, status %#x, cmd enable %#x\n",
		    rd(TCS(0x10)), rd(TCS(0x14)), rd(TCS(0x18)), rd(TCS(0x1c)));
		printf("goodix: stage 9: slot set 0: 3.x wait %#x, control %#x, status %#x, cmd enable %#x\n",
		    rd(TCS(0x20)), rd(TCS(0x24)), rd(TCS(0x28)), rd(TCS(0x2c)));
		printf("goodix: stage 9: slot set 0: commands, the 2.7 layout (number, address, data, status):\n");
		for (unsigned i = 0; i < 3; i++) {
			printf("goodix: stage 9:   %#x %#x %#x %#x\n", rd(TCS(0x30 + 20 * i)), rd(TCS(0x34 + 20 * i)),
			    rd(TCS(0x38 + 20 * i)), rd(TCS(0x3c + 20 * i)));
		}
		printf("goodix: stage 9 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 16) {
		/*
		 * The whole bring-up, then everything the line carries for fifteen seconds, raw and unfiltered:
		 * the report layout is known (s8), so what is left is to see whether a finger on the screen
		 * produces a message the chip hands over.
		 */
		uint8_t raw[48];
		uint64_t deadline;

		max_x = board->raw_max_x;
		max_y = board->raw_max_y;
		printf("goodix: stage 16: the bring-up, then every message for fifteen seconds\n");
		power_on();
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		if (synaptics_probe(true)) {
			uint32_t heard = 0, ticks = 0;

			printf("goodix: stage 16: TOUCH THE SCREEN NOW, three minutes of listening begin\n");
			clock_interval_to_deadline(180000, NSEC_PER_MSEC, &deadline);
			while (mach_absolute_time() < deadline) {
				if (!gpio_get(PIN_INT) && syna_transfer(NULL, raw, sizeof(raw))) {
					heard++;
					printf("goodix: stage 16: %02x %02x %02x %02x | %02x %02x %02x %02x\n", raw[0], raw[1],
					    raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
				}
				if (++ticks % 50000 == 0) {     /* 50000 polls of 200 us: a line every ten seconds */
					printf("goodix: stage 16: still listening, %u messages so far, TICK\n", heard);
				}
				sleep_us(200);
			}
			printf("goodix: stage 16: the three minutes are over, %u messages heard\n", heard);
		}
		printf("goodix: stage 16 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 8) {
		/*
		 * The whole of the bring-up once, then stop: nothing keeps looking at the chip, so the
		 * loader's USB stays as quiet as it is with stage 2 and the log can be fetched at leisure.
		 */
		max_x = board->raw_max_x;
		max_y = board->raw_max_y;
		printf("goodix: stage 8: power, reset, IDENTIFY, the report layout, the enable\n");
		power_on();
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		if (synaptics_probe(true)) {
			uint64_t deadline;

			printf("goodix: stage 8: TOUCH THE SCREEN NOW, two minutes of reports\n");
			clock_interval_to_deadline(120000, NSEC_PER_MSEC, &deadline);
			while (mach_absolute_time() < deadline) {
				if (!gpio_get(PIN_INT)) {
					synaptics_poll();
				}
				sleep_ms(2);
			}
			printf("goodix: stage 8: %u polls, %u reports, %u unreadable, %u touches, last %u %u\n",
			    seen.polls, seen.reports, seen.bad, seen.touches, seen.last_x, seen.last_y);
		}
		printf("goodix: stage 8 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 1) {
		printf("goodix: stage 1: registers mapped, nothing read or written\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 2) {
		printf("goodix: stage 2: reading the pin state, one pin at a time\n");
		printf("goodix: stage 2: cs %#x\n", rd(GPIO_CFG(PIN_CS)));
		printf("goodix: stage 2: clk %#x\n", rd(GPIO_CFG(PIN_CLK)));
		printf("goodix: stage 2: mosi %#x\n", rd(GPIO_CFG(PIN_MOSI)));
		printf("goodix: stage 2: miso %#x\n", rd(GPIO_CFG(PIN_MISO)));
		printf("goodix: stage 2: reset cfg %#x\n", rd(GPIO_CFG(PIN_RESET)));
		printf("goodix: stage 2: reset level %u\n", rd(GPIO_IO(PIN_RESET)) & 1);
		printf("goodix: stage 2: int cfg %#x\n", rd(GPIO_CFG(PIN_INT)));
		printf("goodix: stage 2: int level %u\n", rd(GPIO_IO(PIN_INT)) & 1);
		if (board->pin_avdd >= 0) {
			printf("goodix: stage 2: avdd cfg %#x\n", rd(GPIO_CFG(PIN_AVDD)));
			printf("goodix: stage 2: avdd level %u\n", rd(GPIO_IO(PIN_AVDD)) & 1);
		}
		printf("goodix: stage 2 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 3) {
		printf("goodix: stage 3: reset low, interrupt as an input, analog supply high\n");
		gpio_output(PIN_RESET, reset_high ? true : false, PULL_UP, 8);
		gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		printf("goodix: stage 3 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 4) {
		printf("goodix: stage 4: the I/O supply\n");
		if (!regulator_on(board->iovdd, board->iovdd_uv, board->regulator_mode)) {
			printf("goodix: the I/O supply did not answer\n");
		}
		sleep_ms(3);
		if (board->pin_avdd >= 0) {
			gpio_output(PIN_AVDD, true, PULL_NONE, 2);
		}
		printf("goodix: stage 4 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 5) {
		printf("goodix: stage 5: power, then a reset pulse\n");
		power_on();
		touch_reset();
		printf("goodix: stage 5 done\n");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 6) {
		printf("goodix: stage 6: power, reset, then IDENTIFY on pins driven by hand\n");
		power_on();
		touch_reset();
		spi_transfer = bitbang_transfer;
		bitbang_in = PIN_MISO;
		bitbang_out = PIN_MOSI;
		bitbang_init();
		printf("goodix: stage 6: identify says mode %#x\n", synaptics_identify());
		thread_terminate(current_thread());
		return;
	}

	if (!bring_up()) {
		printf("goodix: no %s touch screen\n", chip_name());
		thread_terminate(current_thread());
		return;
	}
	printf("goodix: %s touch screen ready; the interrupt line is looked at every %u us\n", chip_name(), poll_us);
	{
		uint32_t rate = 0;

		/* boot-arg touch_rate=480: the chip reports twice as often (Xiaomi's game mode; 240 otherwise) */
		if (PE_parse_boot_argn("touch_rate", &rate, sizeof(rate)) && rate == 480 && touch_chip == TOUCH_GOODIX) {
			bool done = goodix_command(GOODIX_CMD_HIGH_RATE, 1);

			report_rate = done ? "480 (acknowledged)" : "240 (480 NOT ACKNOWLEDGED)";
			printf("goodix: reports 480 times a second (command %#x at %#x): %s\n", GOODIX_CMD_HIGH_RATE, command_address,
			    done ? "acknowledged" : "NOT ACKNOWLEDGED");
		}
	}
	for (unsigned tick = 1;; tick++) {
		uint64_t looked = micros_now(), read_at;

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			chip_poll();
			read_at = micros_now();
			reads++;
			reading += read_at - looked;
			if (latency) {
				hakios_tl("tl: c before %llu seen %llu read %llu\n", before, looked, read_at);
			}
		} else if (++idle >= idle_looks) {
			idle = 0;
			chip_poll();
			reads++;
			reading += micros_now() - looked;
		}
		before = looked;
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("%s: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us, reports %s a second\n",
			    chip_name(), seen.reports, seen.polls, seen.bad, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low", reads ? reading / reads : 0, poll_us, report_rate);
		}
		sleep_us(poll_us);
	}
}

void
hakios_goodix_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("touch_reset_high", &reset_high, sizeof(reset_high));
	if (kernel_thread_start(goodix_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
