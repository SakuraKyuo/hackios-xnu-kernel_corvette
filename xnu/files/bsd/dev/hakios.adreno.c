/*
 * The GPU of the POCO X5 Pro (redwood), an Adreno 642L, woken step by step up to a picture that
 * it fills and the panel shows. Every address below belongs to this phone, and nothing here is
 * touched unless a boot-arg asks for it.
 *
 * Boot-arg gpu=1, on the phone and with the GPU's firmware in the small image: the GPU is
 * started while the system starts and kept for the window server to draw with (the service
 * HakiosGPU; "the GPU kept on" below). Should a step fail, one line says which, and the system
 * draws in software as it does without the boot-arg.
 *
 * The steps are how this was found out, and they stay for looking: how much of the GPU a kernel
 * under Qualcomm's hypervisor may reach was not known, and an access the hypervisor does not
 * like ends the run, at once or a quarter of a minute later, and often without a word. adreno=N
 * performs steps 1 to N and stops; a step says what it is about to touch before it touches it
 * and what it found after, reads come before writes, and a step that does not find what it
 * needs ends the sequence instead of going on blind.
 *
 *   1  reads   what feeds the GPU from outside: its clocks in the global clock controller, and
 *              the names RPMh knows its supplies by
 *   2  reads   the fuses that say which GPU this is
 *   3  reads   the GPU's own clock controller, as the boot loader left it
 *   4  writes  the "CX" power switch on: the half of the GPU block that is not the 3D core --
 *              the power management processor ("GMU"), the bus interface, the GPU's SMMU
 *   5  writes  the clocks of that half on, and measures with the SoC's frequency counter whether
 *              they run: the control register of one of them does not say
 *   6  reads   the first words from the GPU block itself: registers of the GMU (among them a
 *              counter that should be seen counting) and of the bus interface
 *   7  reads   the GMU's own channel to RPMh
 *   8  reads   the SMMU in front of the GPU: its ID registers, and which stream goes where
 *   9  reads   that SMMU's first translation context
 *  10  writes  two words into the GMU's memory and reads them back: the way its firmware goes
 *  11  asks    TrustZone questions that pass no memory: whether it takes calls from this kernel,
 *              and which firmware images it says it authenticates
 *  12  asks    TrustZone to check the signature of the GPU's "zap shader", the signed image
 *              without which the 3D core stays locked in secure mode: its header goes from the
 *              small image (fw/a660_zap.mdt) into memory of ours that TrustZone is pointed at
 *  13  writes  page tables for the memory the GMU's firmware asks for, and the SMMU: a
 *              translation context with those tables, and the GMU's requests sent through it
 *  14  writes  the GMU's firmware (fw/a660_gmu.bin) into its own memory and into that memory
 *  15  writes  the GMU's configuration, and lets its processor out of reset: the firmware runs
 *  16  writes  the GMU's connection to RPMh; starts its message queues ("HFI")
 *  17  talks   to the GMU: the tables it needs, and the request to power the 3D core, whose
 *              registers are then read
 *  18  writes  the 3D core's translation context, with its ring and its microcode
 *              (fw/a660_sqe.fw) in it
 *  19  asks    TrustZone to load the zap shader (and first to give up the one a kernel before
 *              this one may have left; the stop at the end of a run asks for that too)
 *  20  writes  the 3D core's registers; starts its command processor and gives it its first
 *              command
 *  21  commands the 3D core out of secure mode, and to write a word into a page of ours
 *  22  commands a picture filled with three colours, and shows it on the panel
 * When the steps are over, what they started is stopped again, in reverse, and the CX half
 * switched off: nothing of the GPU block outlives the run (its SMMU contexts point into this
 * kernel's memory), and the next one starts from what the phone's own start leaves.
 *
 * The firmware is the phone's own, from /vendor/firmware of its Android. There is no register that holds the chip ID (0x06030500,
 * "qcom,chipid" in the device tree): the fuses of step 2 are all the hardware says about
 * itself, and the GMU's version is a property of its firmware.
 *
 * More boot-args:
 *   adreno_only=K      step K alone (what steps 6 to 10 need, they check for themselves: within
 *                      one session of the loader, power and clocks stay as an earlier run left them)
 *   adreno_skip=MASK   leave out the steps whose bits are set (bit 2 = step 2)
 *   adreno_pause=MS    how long the newest step waits after announcing a group of accesses
 *                      (default 2200). The loader, watching from another core (bootcpu=N), writes
 *                      the end of our log to storage every two seconds, and after a silent reset
 *                      that note is all there is.
 *   adreno_fine=1      the newest step waits like that before every single access
 *   adreno_keep=1      with adreno=21: the GPU is not stopped again but kept for the system to draw
 *                      with, through the service HakiosGPU (see "the GPU kept on" below); it is
 *                      given back when the run ends (adreno_dirty=1: left as a panic leaves it)
 *   adreno_speed=N     step 17 asks the GMU for the 3D core's Nth speed: 1 315 MHz, 2 450 MHz,
 *                      3 550 MHz (the default: the most this phone's GPU is rated for)
 *   adreno_blind=1     step 20 starts the command processor with its microcode nowhere, and
 *   adreno_stream1=N   step 18 leaves the core's second stream out (0) or gives it an empty
 *                      context of its own (2): what the SMMU then records shows what the core
 *                      asks for. With 0 and the microcode in place the phone resets.
 *   adreno_watch=S     after the last step, say once a second for S seconds (default 20) that
 *                      the machine is still alive, and what was written last
 *   adreno_return=1    when all is over, go back to the loader instead of starting the system
 *   display_probe=1    before any step: read what the firmware left the display controller
 *                      doing (display_probe() below; two clocks are turned on, nothing else written)
 * The system does not go on starting until all of this is over, so that the end of the log is
 * ours; the touch screen driver's start would talk across it (leave touchscreen=1 out).
 *
 * Addresses are from the phone's own device tree ("dt:" below; the nodes qcom,kgsl-3d0@3d00000,
 * qcom,gmu@3d69000, clock-controller@3d90000, qcom,gdsc@3d9106c, kgsl-smmu@3da0000,
 * qfprom@780000), register layouts from Linux, where SC7280 is this SoC: drivers/clk/qcom/
 * {gcc,gpucc}-sc7280.c, gdsc.c, clk-rcg2.c, drivers/gpu/drm/msm/adreno/a6xx_gmu.c with
 * registers/adreno/a6xx{,_gmu}.xml, drivers/iommu/arm/arm-smmu/arm-smmu.h, drivers/soc/qcom/
 * cmd-db.c, drivers/firmware/qcom/qcom_scm{,-smc}.c, drivers/soc/qcom/mdt_loader.c. The phone's
 * vendor kernel agrees on every offset used here, and is the only source for the frequency counter.
 */

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/stat.h>
#include <sys/systm.h>
extern bool hakios_gpu_debug(void);

#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

static uint32_t last_step, skip_steps, pause_ms = 2200, fine, watch_seconds = 20;
static uint32_t step;                   /* the one being performed */
static uint32_t speed_bin;              /* which of the GPUs of this kind the fuses say this is (step 2) */
static uint32_t newest_step;            /* the highest one asked for: the one that has not been seen to work */
static char last_write[96] = "nothing written";
static bool quiet;                      /* repeating accesses that have been announced and seen to work: say nothing */

/*
 * The kernel's log reaches the loader when a run is over, and a run that ends in a reset
 * without a word leaves none. The loader also reads a log of the kernel's from another core
 * while it runs (loader_return.c; xl keeps it with XL_FOLLOW): every line said here goes there
 * as well, before the next register is touched.
 */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

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

/* ---- register access ------------------------------------------------------------------------------- */

/*
 * One access is one plain load or store of a general register. Some of what is touched here (the
 * SMMU at least) is not the hardware but the hypervisor's imitation of it: the access traps and
 * is carried out on our behalf, which works for the simplest instruction forms only, and the
 * vendor's SMMU driver warns that this imitation "goes horribly wrong" for a store of the zero
 * register. So the compiler does not get to choose the instruction.
 */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

/* A stretch of registers; mapped when a step first needs it. */
struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(bool single_access)
{
	if (step == newest_step && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

/* What a step is about to do, in a line. */
static void
announce(const char *what)
{
	printf("adreno: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (quiet) {
		return rd(b->base + offset);
	}
	if (step == newest_step) {
		printf("adreno: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	if (!quiet) {
		printf("adreno: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
		snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
		    b->phys + offset);
		hold(true);
	}
	wr(b->base + offset, value);
}

/* Set bits of a register unless they are set. Returns what the register holds afterwards. */
static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

/* Look at a register that has been read before until the masked bits have the value, for so many milliseconds. */
static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* ---- board support --------------------------------------------------------------------------------- */

struct adreno_board {
	const char *name;
	uint32_t cmd_db_phys;
	uint32_t cmd_db_size;
	uint32_t qfprom_phys;
	uint32_t qfprom_size;
	uint32_t qfprom_speed_bin_offset;
	uint32_t qfprom_speed_bin_shift;
	uint32_t qfprom_speed_bin_mask;
	uint32_t gcc_memnoc_cbcr;
	uint32_t gpucc_phys;
	uint32_t gpucc_size;
	uint32_t gpucc_cx_gdscr;
	uint32_t gpucc_cx_hw_ctrl;   /* where the vendor sees CX's power state (bit 31) */
	uint32_t gpucc_gx_gdscr;
	uint32_t gpucc_ahb_cbcr;
	uint32_t gpucc_cxo_cbcr;
	uint32_t gpucc_cx_gmu_cbcr;
	uint32_t gpucc_hub_cx_int_cbcr;
	uint32_t gpucc_smmu_vote_cbcr;
	uint32_t gpucc_gmu_rcgr;
	uint32_t gmu_phys;
	uint32_t gmu_size;
	uint32_t rscc_phys;
	uint32_t rscc_size;
	uint32_t pdc_phys;
	uint32_t pdc_size;
	uint64_t zap_memory;
	uint32_t smmu_phys;
	uint32_t smmu_size;
	uint32_t smmu_stream_mask;
	const char *gmu_fw_name;
	const char *zap_fw_name;
	const char *zap_b02_name;
	const char *sqe_fw_name;
	const char *aqe_fw_name;    /* the BV coprocessor's microcode; none where the GPU has no AQE */
	uint32_t gmu_chipid;
	bool is_a7xx;
};

static const struct adreno_board board_redwood = {
	.name = "redwood (SM7325 / Adreno 642L)",
	.cmd_db_phys = 0x80860000UL,
	.cmd_db_size = 0x20000UL,
	.qfprom_phys = 0x00780000UL,
	.qfprom_size = 0x7000UL,
	.qfprom_speed_bin_offset = 0x41e9,
	.qfprom_speed_bin_shift = 5,
	.qfprom_speed_bin_mask = 0xff,
	.gcc_memnoc_cbcr = 0x7100c,
	.gpucc_phys = 0x03d90000UL,
	.gpucc_size = 0x9000UL,
	.gpucc_cx_gdscr = 0x106c,
	.gpucc_cx_hw_ctrl = 0x1540,
	.gpucc_gx_gdscr = 0x100c,
	.gpucc_ahb_cbcr = 0x1078,
	.gpucc_cxo_cbcr = 0x109c,
	.gpucc_cx_gmu_cbcr = 0x1098,
	.gpucc_hub_cx_int_cbcr = 0x1204,
	.gpucc_smmu_vote_cbcr = 0x5000,
	.gpucc_gmu_rcgr = 0x1120,
	.gmu_phys = 0x03d6a000UL,
	.gmu_size = 0x34000UL,
	.rscc_phys = 0x03de0000UL,
	.rscc_size = 0x2000UL,
	.pdc_phys = 0x0b290000UL,
	.pdc_size = 0x10000UL,
	.zap_memory = 0x8b71a000ull,
	.smmu_phys = 0x03da0000UL,
	.smmu_size = 0x20000UL,
	.smmu_stream_mask = 0x400,
	.gmu_fw_name = "a660_gmu.bin",
	.zap_fw_name = "a660_zap.mdt",
	.zap_b02_name = "a660_zap.b02",
	.sqe_fw_name = "a660_sqe.fw",
	.aqe_fw_name = NULL,
	.gmu_chipid = 0x06035000,
	.is_a7xx = false,
};

static const struct adreno_board board_corvette = {
	.name = "corvette (OnePlus Ace 3 Pro, SM8650 / Adreno 750)",
	.cmd_db_phys = 0x81c60000UL,
	.cmd_db_size = 0x20000UL,
	.qfprom_phys = 0x221c2000UL,
	.qfprom_size = 0x1000UL,
	.qfprom_speed_bin_offset = 0x9b,
	.qfprom_speed_bin_shift = 4,
	.qfprom_speed_bin_mask = 0x07,
	.gcc_memnoc_cbcr = 0x71010,
	.gpucc_phys = 0x03d90000UL,
	.gpucc_size = 0x10000UL,
	.gpucc_cx_gdscr = 0x9108,
	.gpucc_cx_hw_ctrl = 0x9168,   /* dt: syscon@3d99168, the gdsc's hw-ctrl-addr */
	.gpucc_gx_gdscr = 0x905c,
	.gpucc_ahb_cbcr = 0x911c,
	.gpucc_cxo_cbcr = 0x9144,
	.gpucc_cx_gmu_cbcr = 0x913c,
	.gpucc_hub_cx_int_cbcr = 0x9148,
	.gpucc_smmu_vote_cbcr = 0x7000,
	.gpucc_gmu_rcgr = 0x9318,
	/* dt qcom,gen7-gmu: reg = <0x3d68000 0x37000  0xb280000 0x10000  0x3d40000 0x10000>.
	 * 0x3d6a000/0x34000 is what redwood (SM7325) uses: on pineapple the GMU window starts
	 * 0x2000 earlier and runs 0x3000 longer, so every GMU_WORD would land in the wrong one. */
	.gmu_phys = 0x03d68000UL,
	.gmu_size = 0x37000UL,
	.rscc_phys = 0x03d50000UL,
	.rscc_size = 0x10000UL,
	.pdc_phys = 0x0b280000UL,
	.pdc_size = 0x10000UL,
	.zap_memory = 0x9b09a000ull,
	.smmu_phys = 0x03da0000UL,
	.smmu_size = 0x40000UL,
	.smmu_stream_mask = 0x000,
	.gmu_fw_name = "gmu_gen70900.bin",
	.zap_fw_name = "gen70900_zap.mbn",      /* the phone's own image: header, code and hashes in one file */
	.zap_b02_name = "gen70900_zap.b02",     /* (only for a header whose code sits in a file beside it) */
	.sqe_fw_name = "gen70900_sqe.fw",
	.aqe_fw_name = "gen70900_aqe.fw",        /* the phone's own, for the BV coprocessor (AQE0) */
	.gmu_chipid = 0x07090000,
	.is_a7xx = true,
};

static const struct adreno_board *board = &board_corvette;

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/*
 * dt: clock-controller@100000 (qcom,yupik-gcc). The GPU's clocks there, by the names the GPU's
 * nodes give them: gcc_gpu_memnoc_gfx and gcc_gpu_snoc_dvm_gfx (its two bus connections),
 * "axi_clk" = gcc_ddrss_gpu_axi (memory), gcc_gpu_cfg_ahb (the registers of its clock controller);
 * and the two taps of the 600 MHz PLL that its clock controller takes as inputs. None of them is
 * among that node's protected-clocks, the ones the hypervisor keeps to itself. Offsets:
 * gcc-sc7280.c. The touch screen driver uses this controller too.
 */
static struct block gcc = { "gcc", 0x00100000, 0x80000 };
#define GCC_PLL_VOTE            0x52000         /* bit 15: gcc_gpu_gpll0_clk_src (600 MHz), bit 16: ..._div_clk_src (300 MHz) */
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)
#define GCC_GPU_CFG_AHB_CBCR    0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR 0x7100c
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x71018
#define GCC_DDRSS_GPU_AXI_CBCR  0x71154
#define CBCR_ENABLE             (1u << 0)       /* a clock's control register: on */
#define CBCR_OFF                (1u << 31)      /* ... and whether it is in fact off */

/* dt: reserved-memory cmd_db@80860000: the boot loader's list of what RPMh can switch (cmd-db.c) */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

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

static const struct cmd_db_header *cmd_db;

/*
 * Where RPMh has a resource (0: nowhere) and, for a supply rail, the levels it knows: the GMU's
 * firmware is told the levels by their position in this list.
 */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	const struct cmd_db_header *db = cmd_db;
	char id[8] = { 0 };

	if (level_count != NULL) {
		*level_count = 0;
	}
	if (db == NULL) {
		db = cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(board->cmd_db_phys, board->cmd_db_size);  /* memory, not registers */
	}
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				count = 0;
			}
			for (unsigned l = 0; levels != NULL && l < count && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rpmh_resource(const char *name)
{
	uint16_t level[16];
	unsigned count = 0;
	uint32_t address = rpmh_resource(name, level, &count);
	char levels[160];
	int at = 0;

	if (address == 0) {
		printf("adreno: %u: RPMh has no \"%s\"\n", step, name);
		return;
	}
	levels[0] = 0;
	for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
		at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
	}
	printf("adreno: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, address, levels);
}

static bool
step_outside(void)
{
	announce("read the GPU's clocks in the global clock controller (registers the touch screen driver uses too)");
	get(&gcc, "pll votes", GCC_PLL_VOTE);
	get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	get(&gcc, "gpu_memnoc_gfx", board->gcc_memnoc_cbcr);
	get(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
	if (!board->is_a7xx) {
		get(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);
	}

	announce("look up the GPU's supply rails in the boot loader's list of RPMh resources");
	show_rpmh_resource("gfx.lvl");          /* the 3D core's own rail (dt: VDD_GFX_LEVEL, PM8350C S7) */
	show_rpmh_resource("cx.lvl");           /* the rail of the CX half, shared with most of the SoC */
	show_rpmh_resource("mx.lvl");           /* memories */
	return true;
}

/* ---- step 2: the fuses ------------------------------------------------------------------------------- */

/*
 * dt: qfprom@780000 with gpu_speed_bin@1e9 (two bytes, 8 bits from bit 5) and gpu_gaming_bin@1f5
 * (bit 5). Linux has the same cell 0x4000 further on, in the error-corrected copy of the fuses
 * (sc7280.dtsi: efuse@784000), and that is the copy it reads under this hypervisor on sibling
 * boards; so that one first.
 */
static struct block qfprom = { "qfprom", 0x00780000, 0x7000 };
#define QFPROM_CORRECTED        0x4000
#define QFPROM_GPU_SPEED_BIN    0x1e9
#define QFPROM_GPU_GAMING_BIN   0x1f5

static uint32_t
get8(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step == newest_step) {
		printf("adreno: %u: about to read %s %s at %#x (a byte)\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd8(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static bool
step_fuses(void)
{
	uint32_t bin, gaming;

	if (board->is_a7xx) {
		announce("read the GPU's speed bin fuse (qfprom)");
		bin = get8(&qfprom, "speed bin", board->qfprom_speed_bin_offset);
		bin = (bin >> board->qfprom_speed_bin_shift) & board->qfprom_speed_bin_mask;
		speed_bin = bin;
		printf("adreno: %u: speed bin %u: Adreno 750 (Gen 7.9)\n", step, bin);
		return true;
	}

	announce("read the GPU's fuses in the error-corrected copy (0x784000), which Linux reads");
	bin = get8(&qfprom, "speed bin, low", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN);
	bin |= get8(&qfprom, "speed bin, high", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN + 1) << 8;
	bin = bin >> 5 & 0xff;
	speed_bin = bin;
	gaming = get8(&qfprom, "gaming bin", QFPROM_CORRECTED + QFPROM_GPU_GAMING_BIN) >> 5 & 1;
	/* dt: qcom,gpu-models: 0xac and 0xbe are sold as Adreno 643, anything else is the 642L */
	printf("adreno: %u: speed bin %u: an Adreno %s; gaming bin %u\n", step, bin, bin == 0xac || bin == 0xbe ? "643" : "642L",
	    gaming);

	announce("read the same fuses where the phone's device tree has them (0x780000)");
	bin = get8(&qfprom, "raw speed bin, low", QFPROM_GPU_SPEED_BIN);
	bin |= get8(&qfprom, "raw speed bin, high", QFPROM_GPU_SPEED_BIN + 1) << 8;
	printf("adreno: %u: raw speed bin %u\n", step, bin >> 5 & 0xff);
	return true;
}

/* ---- step 3: the GPU's clock controller ----------------------------------------------------------------- */

/*
 * dt: clock-controller@3d90000 (qcom,yupik-gpucc), 0x9000 long; offsets: gpucc-sc7280.c. Its
 * registers answer only while gcc_gpu_cfg_ahb runs, which Linux turns on for good when it
 * starts (gcc-sc7280.c, the end of the probe function).
 */
static struct block gpucc = { "gpucc", 0x03d90000, 0x9000 };
#define GPUCC_PLL0_MODE         0x0000          /* the 3D core's PLL: bit 31 locked, bit 0 output on; +4: times 19.2 MHz */
#define GPUCC_PLL1_MODE         0x0100          /* a 500 MHz PLL, an alternative source for the GMU's clock */
#define GPUCC_CXO_AON_CBCR      0x1004
#define GPUCC_GX_SW_RESET       0x1008          /* dt: gpu_gx_sw_reset */
#define GPUCC_GX_GDSCR          0x100c          /* dt: gpu_gx_gdsc, the 3D core's power switch: the GMU's to operate */
#define GPUCC_GX_GMU_CBCR       0x1064
#define GPUCC_CX_GDSCR          0x106c          /* dt: gpu_cx_gdsc */
#define GPUCC_AHB_CBCR          0x1078          /* gpu_cc_ahb_clk: the register bus inside the GPU block */
#define GPUCC_CX_GMU_CBCR       0x1098          /* gpu_cc_cx_gmu_clk: the GMU */
#define GPUCC_CXO_CBCR          0x109c          /* gpu_cc_cxo_clk: the crystal, for the GMU's timers */
#define GPUCC_GMU_RCGR          0x1120          /* gpu_cc_gmu_clk_src: +0 command, +4 source and divider */
#define GPUCC_HUB_RCGR_A7XX     0x93ec          /* gpu_cc_hub_clk_src on pineapple (a7xx); a6xx's 0x117c is redwood's */
#define GPUCC_CRC_AHB_CBCR      0x9120          /* gpu_cc_crc_ahb_clk (gpucc-pineapple.c) */
#define GPUCC_CB_CBCR           0x1170          /* gpu_cc_cb_clk: Linux keeps it on always */
#define GPUCC_HUB_AON_CBCR      0x1178          /* gpu_cc_hub_aon_clk */
#define GPUCC_HUB_RCGR          0x117c          /* gpu_cc_hub_clk_src */
#define GPUCC_HUB_CX_INT_CBCR   0x1204          /* gpu_cc_hub_cx_int_clk */
#define GPUCC_CX_HW_CTRL        0x1540          /* dt: gpu_cx_hw_ctrl: bit 31 is where the vendor's driver sees CX come on */
#define GPUCC_GX_DOMAIN_MISC    0x158c          /* dt: gpu_gx_domain_addr */
#define GPUCC_SMMU_VOTE_CBCR    0x5000          /* gpu_cc_hlos1_vote_gpu_smmu_clk: our vote for the SMMU's clock */

#define GDSC_SW_COLLAPSE        (1u << 0)       /* a power switch's control register (gdsc.c): off, by software */
#define GDSC_HW_CONTROL         (1u << 1)       /* something else than software operates it */
#define GDSC_POWER_ON           (1u << 31)
#define GDSC_POWER_UP_COMPLETE  (1u << 16)      /* CFG_GDSCR (+4), support-cfg-gdscr: the domain is up */


static bool
step_gpucc(void)
{
	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
	set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE);
	if (!board->is_a7xx) {
		if ((rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) &&
		    !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
			printf("adreno: %u: it stays off (%#x): the GPU's clock controller would not answer\n", step,
			    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
			return false;
		}
	}

	announce("read the GPU's clock controller (0x3d90000) as it is: the first access to the GPU's part of the SoC");
	get(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr);
	if (!board->is_a7xx) {
		get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	}
	get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
	if (!board->is_a7xx) {
		get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
		get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
	}
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 multiplier", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "gmu_clk_src command", board->gpucc_gmu_rcgr);
	get(&gpucc, "gmu_clk_src source", board->gpucc_gmu_rcgr + 4);
	if (!board->is_a7xx) {
		get(&gpucc, "hub_clk_src command", GPUCC_HUB_RCGR);
		get(&gpucc, "hub_clk_src source", GPUCC_HUB_RCGR + 4);
		get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
		get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	}
	get(&gpucc, "ahb", board->gpucc_ahb_cbcr);
	get(&gpucc, "cx_gmu", board->gpucc_cx_gmu_cbcr);
	get(&gpucc, "cxo", board->gpucc_cxo_cbcr);
	/*
	 * A sweep of the controller's branch registers, so this log and the same dump taken on a
	 * running Android can be compared word for word: a branch Android has enabled and this does
	 * not is the answer, and guessing at offsets has produced nothing.
	 */
	for (uint32_t o = 0x9050; o <= 0x9070; o += 4) {
		printf("adreno: %u: sweep %05x = %#x\n", step, o, rd(gpucc.base + o));
	}
	for (uint32_t o = 0x9100; o <= 0x9160; o += 4) {
		printf("adreno: %u: sweep %05x = %#x\n", step, o, rd(gpucc.base + o));
	}
	for (uint32_t o = 0x9300; o <= 0x9320; o += 4) {
		printf("adreno: %u: sweep %05x = %#x\n", step, o, rd(gpucc.base + o));
	}
	if (!board->is_a7xx) {
		get(&gpucc, "cb", GPUCC_CB_CBCR);
		get(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR);
	}
	get(&gpucc, "hub_cx_int", board->gpucc_hub_cx_int_cbcr);
	get(&gpucc, "smmu vote", board->gpucc_smmu_vote_cbcr);
	return true;
}

/* ---- step 4: CX power --------------------------------------------------------------------------------- */

/* Not to touch the GPU's clock controller without the clock of its registers: where step 3 has not been. */
static bool
gpucc_answers(void)
{
	need(&gcc);
	need(&gpucc);
	if (!(rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_ENABLE)) {
		printf("adreno: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x): step 3\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}
	return true;
}

static bool
cx_is_on(void)
{
	/* The GDSC's power state is read from its hw-ctrl register (bit 31), not from the GDSCR control
	 * register: the device tree gives the gdsc an hw-ctrl-addr, and the vendor's gdsc_check_status()
	 * reads that for PWR_ON. */
	return (rd(gpucc.base + board->gpucc_cx_hw_ctrl) & GDSC_POWER_ON) != 0;
}

/*
 * gpu_cc_gx_gdsc has "qcom,support-cfg-gdscr" in the device tree: the vendor's gdsc_check_status()
 * then reads CFG_GDSCR (gdscr + 4) and takes bit 16, not bit 31 of the control register.
 */
static bool
gx_is_on(void)
{
	return (rd(gpucc.base + board->gpucc_gx_gdscr + 4) & GDSC_POWER_UP_COMPLETE) != 0;
}

static void stale_stop(void);

/*
 * The switch hangs on the rail "cx.lvl", which is up for as long as the SoC runs at all; the
 * vendor's driver (gdsc-regulator.c) clears the collapse bit and waits for bit 31 of the register
 * the device tree calls hw-ctrl-addr, Linux for bit 31 of the control register itself.
 */
static bool
step_cx_power(void)
{
	uint32_t gdscr, gx;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cx_gdsc): clear the collapse bit of its power switch");
	gdscr = get(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr);
	get(&gpucc, "cx_hw_ctrl", board->gpucc_cx_hw_ctrl);
	if (cx_is_on()) {
		/*
		 * A kernel that kept the GPU for the system to use (adreno_keep) leaves it running when
		 * its run ends. What it left -- the GMU's processor, the 3D core's power, the GMU's
		 * votes in RPMh, the SMMU's contexts pointing into memory that is somebody else's now --
		 * is taken down as gpu_block_stop() would have, and the start is from nothing.
		 */
		printf("adreno: %u: CX is on already: an earlier kernel left the GPU block running; stopping it first\n", step);
		stale_stop();
		if (cx_is_on()) {
			printf("adreno: %u: CX stays on: going on with it as it is\n", step);
			return true;
		}
		gdscr = rd(gpucc.base + board->gpucc_cx_gdscr);
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("adreno: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr, gdscr & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr);
	get(&gpucc, "cx_hw_ctrl", board->gpucc_cx_hw_ctrl);
	if (!cx_is_on()) {
		printf("adreno: %u: CX does not come on\n", step);
		return false;
	}
	printf("adreno: %u: CX is on\n", step);
	/*
	 * The GMU's window is in the 3D half too: the device tree gives qcom,gen7-gmu
	 * vdd-supply = <&gpu_cc_gx_gdsc> (and vddcx-supply = <&gpu_cc_cx_gdsc>). Reading it with
	 * GX collapsed is an external abort -- which is exactly what the first CM3 read was.
	 */
	get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
	get(&gpucc, "gx cfg_gdscr", board->gpucc_gx_gdscr + 4);
	if (gx_is_on()) {
		/*
		 * The 3D core kept its power while the rest of the block lost it: an ending that went
		 * wrong (a GMU that did not answer, before gpu_block_stop() switched the core off
		 * itself). Such a core is where it was, in the middle of things, and does not take the
		 * steps' commands (seen: step 21); switching it off from here has hung the whole phone
		 * (seen once). Left alone: no GPU until the phone has been restarted.
		 */
		printf("adreno: %u: THE 3D CORE HAS POWER ALREADY, left on by an earlier kernel: not usable until the phone is "
		    "restarted\n", step);
		return false;
	}
	gx = rd(gpucc.base + board->gpucc_gx_gdscr);
	if (gx & GDSC_HW_CONTROL) {
		printf("adreno: %u: the 3D switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	/*
	 * Not ours to switch on. Vendor gdsc.c registers the GX domain with an empty enable
	 * ("gdsc_gx_do_nothing_enable": from SDM845 on, the GPU GX domain is almost entirely
	 * controlled by the GMU running in the CX domain), and gen7_gmu_first_boot brings the GMU
	 * up with CX alone; the CPU is told only how to turn GX OFF again (a6xx_gmu_suspend).
	 * Clearing the collapse bit, collapsing and releasing it again, and voting gfx.lvl at 384
	 * all left CFG_GDSCR bit 16 clear: that is the expected answer, not something to work
	 * around. What the switch says from here is information; the GMU raises it later.
	 */
	printf("adreno: %u: the 3D half is the GMU's to switch on (gpu_cc_gx_gdsc %#x); it says %#x, cfg %#x\n", step,
	    board->gpucc_gx_gdscr, gx, rd(gpucc.base + board->gpucc_gx_gdscr + 4));
	return true;
}

/* ---- step 5: clocks ------------------------------------------------------------------------------------ */

#define RCG_UPDATE              (1u << 0)       /* command register (clk-rcg2.c): take the new source and divider */
#define RCG_ROOT_EN             (1u << 1)       /* command register: force the root on (clk_rcg2_set_force_enable) */
#define RCG_ROOT_OFF            (1u << 31)      /* command register: status, 1 while the root is off (clk_rcg2_is_enabled) */
#define RCG_SOURCE_MASK         0x371fu         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define RCG_GPLL0_DIV(twice)    (6u << 8 | ((twice) - 1))       /* source 6: the 300 MHz tap; twice the divider wanted */

/* Turn a clock on and see it run. Not every clock's status can be believed (halt_check in Linux): `must` says which. */
static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
	if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("adreno: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; its status bit is not one to rely on, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("adreno: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/*
 * An RCG feeds nothing while its root is off, and the branches hanging off it then read back with
 * their own enable bit clear -- which is what the last log shows for gpu_cc_ahb_clk (0x911c = 0).
 * The vendor's clk-rcg2.c has a bit for each half: CMD_ROOT_EN (bit 1) forces the root on,
 * CMD_ROOT_OFF (bit 31) reads 1 while the root is off. Write both, then read the command register
 * back, so the log says whether the clock chain is really running.
 */
static bool
rcg_root_on(struct block *b, const char *name, uint32_t rcgr)
{
	uint32_t cmd = rd(b->base + rcgr);

	if ((cmd & RCG_ROOT_EN) != 0 && (cmd & RCG_ROOT_OFF) == 0) {
		return true;
	}
	put(b, name, rcgr, (cmd & ~RCG_ROOT_OFF) | RCG_ROOT_EN);
	cmd = get(b, name, rcgr);
	return (cmd & RCG_ROOT_EN) != 0 && (cmd & RCG_ROOT_OFF) == 0;
}

static bool measure_clocks(void);

/*
 * The clocks the device tree gives the GMU and the SMMU, at the rates both the vendor's driver
 * and Linux bring the GMU up with: 200 MHz for the GMU, 150 MHz for the "hub" (a6xx_gmu_resume).
 * Both come from the 300 MHz tap of the global 600 MHz PLL, which runs anyway: no PLL of the
 * GPU's own has to be started.
 */
static bool
step_clocks(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("adreno: %u: CX is off: step 4\n", step);
		return false;
	}

	if (board->is_a7xx) {
		announce("in the global clock controller: ask for the 300 MHz tap for the GPU; the GPU's bus clocks on");
		set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
		if (!clock_on(&gcc, "gpu_memnoc_gfx", board->gcc_memnoc_cbcr, false) ||
		    !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, true)) {
			return false;
		}

		/*
		 * The GMU's clock source, gpu_cc_gmu_clk_src: its root-enable bit is what makes the block
		 * answer at all. The a6xx path also programmes source and divider; here only the enable is
		 * restored, because it read 0 while the boot that could reach the GMU window had 0x80000000.
		 */
		{
			get(&gpucc, "gmu_clk_src command", board->gpucc_gmu_rcgr);

			/*
			 * The GMU's clock source, from its own freq table: 19.2 MHz crystal, or GPU_CC_PLL1.
			 * It does not take GPLL0 at all (the a6xx path's RCG_GPLL0_DIV is redwood's arrangement),
			 * and PLL1 is off in this boot ("pll1 mode 0x3d90100 = 0"), so the crystal it is: 19.2 MHz
			 * is slow but it is a real clock, and a real clock is what makes the block answer.
			 */
			if (!clock_source("gmu_clk_src", board->gpucc_gmu_rcgr, 0x001)) {   /* TCXO, D=1 */
				return false;
			}
			/*
			 * Tell the root to run: bit 1 of the command register (CMD_ROOT_EN). Bit 31 is
			 * CMD_ROOT_OFF, the status the vendor's clk_rcg2_is_enabled() reads (1 = off), so the
			 * old code that wrote bit 31 as an "enable" was asking the GMU's clock to stop -- and
			 * an unclocked block is exactly what an external abort on its window looks like.
			 */
			if (!rcg_root_on(&gpucc, "gmu_clk_src", board->gpucc_gmu_rcgr)) {
				return false;
			}
		}
		/*
		 * The hub clock source, gpu_cc_hub_clk_src (cmd_rcgr 0x93ec in the vendor's
		 * gpucc-pineapple.c; the a6xx path's 0x117c is redwood's offset and was never used here).
		 * gpu_cc_ahb_clk hangs off its divider, so without this the branch has no clock to gate --
		 * which is why writing 0x911c read back 0 and looked like a dead register.
		 */
		/* F(200000000, P_GPLL0_OUT_MAIN, 3): source 5 (GPLL0 itself, not its divided tap) with D=3 */
		if (!clock_source("hub_clk_src", GPUCC_HUB_RCGR_A7XX, (5u << 8) | 5u) ||
		    !rcg_root_on(&gpucc, "hub_clk_src", GPUCC_HUB_RCGR_A7XX)) {
			return false;
		}
		/* gpu_cc_crc_ahb_clk (enable 0x9120): a clock the vendor driver has and this one never set */
		if (!clock_on(&gpucc, "crc_ahb", GPUCC_CRC_AHB_CBCR, false)) {
			return false;
		}
		announce("in the GPU's clock controller: the clocks of the GMU and of the SMMU on");
		if (!clock_on(&gpucc, "cxo", board->gpucc_cxo_cbcr, true) ||
		    !clock_on(&gpucc, "ahb", board->gpucc_ahb_cbcr, false) ||
		    !clock_on(&gpucc, "hub_cx_int", board->gpucc_hub_cx_int_cbcr, true) ||
		    !clock_on(&gpucc, "cx_gmu", board->gpucc_cx_gmu_cbcr, true) ||
		    !clock_on(&gpucc, "smmu vote", board->gpucc_smmu_vote_cbcr, false)) {
			return false;
		}
		return true;
	}

	announce("in the global clock controller: ask for the 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
	set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	if (!clock_on(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, true) ||
	    !clock_on(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false)) {
		return false;
	}

	announce("in the GPU's clock controller: the GMU's clock to 200 MHz, the hub's to 150 MHz");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3)) ||       /* 300 MHz / 1.5 */
	    !clock_source("hub_clk_src", GPUCC_HUB_RCGR, RCG_GPLL0_DIV(4))) {       /* 300 MHz / 2 */
		return false;
	}

	announce("in the GPU's clock controller: the clocks of the GMU and of the SMMU on");
	if (!clock_on(&gpucc, "cb", GPUCC_CB_CBCR, false) ||
	    !clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) ||
	    !clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false) ||
	    !clock_on(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR, true) ||
	    !clock_on(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR, true) ||
	    !clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true) ||
	    !clock_on(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR, false)) {
		return false;
	}
	return measure_clocks();
}

/*
 * Whether a clock runs is not always to be had from its control register: the one of the register
 * bus inside the GPU block, gpu_cc_ahb, reads 0 on this phone before and after its enable bit is
 * written (Linux, for which it has "no bit to check", writes the bit and waits 10 us). The SoC can
 * measure instead: a clock controller can put any of its clocks on a debug output, and the global
 * controller counts what it is given there against the crystal. The vendor's kernel shows the
 * result as "clk_measure"; registers, selections and procedure are its debugcc-yupik.c and
 * clk-debug.c (mainline Linux has no counterpart).
 */
#define GCC_DEBUG_MUX           0x62000         /* [9:0] which clock goes to the counter */
#define GCC_DEBUG_MUX_GPUCC     0x146           /* ... the debug output of the GPU's clock controller */
#define GCC_DEBUG_DIV           0x62004         /* [3:0] divider - 1; both controllers divide by 2 */
#define GCC_DEBUG_CBCR          0x62008
#define GCC_XO_DIV4_CBCR        0x6200c         /* the counter's time base: the crystal / 4, 4.8 MHz */
#define GCC_MEASURE_CTL         0x62038         /* [19:0] how many ticks of the time base to count for */
#define MEASURE_RUN             (1u << 20)
#define MEASURE_CLEAR           (1u << 21)
#define GCC_MEASURE_STATUS      0x6203c         /* [24:0] the count */
#define MEASURE_DONE            (1u << 25)
#define MEASURE_BRIEF           0x1000          /* ticks: a millisecond */
#define MEASURE_FULL            0x20000         /* 27 ms */
#define MEASURE_FAILED          0xffffffffu
#define GPUCC_DEBUG_DIV         0x10fc
#define GPUCC_DEBUG_CBCR        0x1100
#define GPUCC_DEBUG_MUX         0x1568          /* [7:0] which of the GPU's clocks */
#define GPUCC_DEBUG_CXO_AON     0x0b
#define GPUCC_DEBUG_GX_GFX3D    0x0d            /* the 3D core's own clock */
#define GPUCC_DEBUG_GX_GMU      0x11
#define GPUCC_DEBUG_AHB         0x12
#define GPUCC_DEBUG_CX_GMU      0x1a
#define GPUCC_DEBUG_CXO         0x1b
#define GPUCC_DEBUG_HUB_CX_INT  0x1c
#define GPUCC_DEBUG_CB          0x26
#define GPUCC_DEBUG_HUB_AON     0x27

/* Let the counter run for so many ticks of its time base; what it counted of the clock on its input. */
static uint32_t
count_clock(uint32_t ticks)
{
	uint32_t control = get(&gcc, "measure control", GCC_MEASURE_CTL) & ~(MEASURE_RUN | MEASURE_CLEAR | 0xfffffu);
	uint32_t status = 0;

	put(&gcc, "measure control", GCC_MEASURE_CTL, control | MEASURE_CLEAR);
	sleep_us(10);
	put(&gcc, "measure control", GCC_MEASURE_CTL, control | ticks);
	put(&gcc, "measure control", GCC_MEASURE_CTL, control | ticks | MEASURE_RUN);
	for (int i = 0; i < 2000 && !(status & MEASURE_DONE); i++) {
		sleep_us(100);
		status = rd(gcc.base + GCC_MEASURE_STATUS);
	}
	put(&gcc, "measure control", GCC_MEASURE_CTL, control | ticks);
	return status & MEASURE_DONE ? get(&gcc, "measure status", GCC_MEASURE_STATUS) & 0x1ffffff : MEASURE_FAILED;
}

/*
 * The rate in Hz of one of the GPU clock controller's clocks; 0 if it does not run, MEASURE_FAILED
 * if the counter does not finish. Everything that is changed for it is put back.
 */
static uint32_t
measure(uint32_t select)
{
	uint32_t gpu_mux = get(&gpucc, "debug mux", GPUCC_DEBUG_MUX), gpu_div = get(&gpucc, "debug divider", GPUCC_DEBUG_DIV);
	uint32_t gpu_cbcr = get(&gpucc, "debug output", GPUCC_DEBUG_CBCR), mux = get(&gcc, "debug mux", GCC_DEBUG_MUX);
	uint32_t div = get(&gcc, "debug divider", GCC_DEBUG_DIV), cbcr = get(&gcc, "debug input", GCC_DEBUG_CBCR);
	uint32_t base = get(&gcc, "crystal/4", GCC_XO_DIV4_CBCR), control = get(&gcc, "measure control", GCC_MEASURE_CTL);
	uint32_t brief, full;

	put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, (gpu_mux & ~0xffu) | select);
	put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, (gpu_div & ~0xfu) | 1);
	put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr | CBCR_ENABLE);
	put(&gcc, "debug mux", GCC_DEBUG_MUX, (mux & ~0x3ffu) | GCC_DEBUG_MUX_GPUCC);
	put(&gcc, "debug divider", GCC_DEBUG_DIV, (div & ~0xfu) | 1);
	put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr | CBCR_ENABLE);
	put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base | CBCR_ENABLE);
	/* a counter whose input stands still keeps its last count: the two counts differ only if the clock runs */
	brief = count_clock(MEASURE_BRIEF);
	full = brief == MEASURE_FAILED ? MEASURE_FAILED : count_clock(MEASURE_FULL);
	put(&gcc, "measure control", GCC_MEASURE_CTL, control);
	put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base);
	put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr);
	put(&gcc, "debug divider", GCC_DEBUG_DIV, div);
	put(&gcc, "debug mux", GCC_DEBUG_MUX, mux);
	put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr);
	put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, gpu_div);
	put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, gpu_mux);

	if (full == MEASURE_FAILED) {
		return MEASURE_FAILED;
	}
	if (full == brief) {
		return 0;
	}
	/* the vendor's formula; times 4 for the two dividers on the way to the counter */
	return (uint32_t)(((uint64_t)full * 10 + 15) * 4800000 / ((uint64_t)MEASURE_FULL * 10 + 35) * 4);
}

static uint32_t
measure_quietly(uint32_t select)
{
	uint32_t hz;

	quiet = true;
	hz = measure(select);
	quiet = false;
	return hz;
}

/*
 * Measure the clocks step 5 has turned on. The crystal's comes first and aloud: its rate is known,
 * so it shows whether the counter can be believed; one that is off (the 3D core's) comes last.
 */
static bool
measure_clocks(void)
{
	static const struct {
		const char *name;
		uint32_t select;
	} clocks[] = {
		{ "cx_gmu (200 MHz was asked for)", GPUCC_DEBUG_CX_GMU }, { "hub_cx_int (from 150 MHz)", GPUCC_DEBUG_HUB_CX_INT },
		{ "hub_aon", GPUCC_DEBUG_HUB_AON }, { "cb", GPUCC_DEBUG_CB }, { "cxo_aon", GPUCC_DEBUG_CXO_AON },
		{ "gx_gmu (should be off)", GPUCC_DEBUG_GX_GMU }, { "ahb, THE ONE IN QUESTION,", GPUCC_DEBUG_AHB },
	};
	uint32_t hz, ahb_hz = MEASURE_FAILED;

	announce("measure the GPU block's clocks with the SoC's frequency counter (debug registers at gcc 0x162000.., "
	    "gpucc 0x3d910fc, 0x3d91100, 0x3d91568)");
	hz = measure(GPUCC_DEBUG_CXO);
	if (hz == MEASURE_FAILED) {
		printf("adreno: %u: the counter did not finish: it cannot be used as it is set up here\n", step);
		return false;
	}
	printf("adreno: %u: measured: cxo runs at %u Hz (the crystal: 19200000)\n", step, hz);
	if (hz < 18600000 || hz > 19800000) {
		printf("adreno: %u: the counter does not show the crystal's rate: nothing it says can be used\n", step);
		return false;
	}
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		hz = measure_quietly(clocks[i].select);
		if (hz == MEASURE_FAILED) {
			printf("adreno: %u: measured: %s: the counter did not finish\n", step, clocks[i].name);
		} else {
			printf("adreno: %u: measured: %s runs at %u Hz\n", step, clocks[i].name, hz);
		}
		if (clocks[i].select == GPUCC_DEBUG_AHB) {
			ahb_hz = hz;
		}
	}
	if (ahb_hz != MEASURE_FAILED && ahb_hz >= 1000000) {
		printf("adreno: %u: THE REGISTER BUS OF THE GPU BLOCK HAS ITS CLOCK, whatever its control register says\n", step);
		return true;
	}
	/* for study: what else is there in the clock controller that could be its switch */
	printf("adreno: %u: the register bus of the GPU block has no clock; what is not 0 in gpucc 0x1000..0x15fc:\n", step);
	for (uint32_t offset = 0x1000; offset < 0x1600; offset += 16) {
		uint32_t row[4] = { rd(gpucc.base + offset), rd(gpucc.base + offset + 4), rd(gpucc.base + offset + 8),
			            rd(gpucc.base + offset + 12) };

		if (row[0] | row[1] | row[2] | row[3]) {
			printf("adreno: %u: gpucc %#x: %#x %#x %#x %#x\n", step, gpucc.phys + offset, row[0], row[1], row[2],
			    row[3]);
		}
	}
	return false;
}

/*
 * What every later step stands on: CX powered, the GMU's clock running by its control register,
 * and the register bus's clock running by measurement (see above).
 */
static bool
cx_ready(void)
{
	uint32_t gmu_clock, ahb_hz;

	if (!gpucc_answers()) {
		return false;
	}
	gmu_clock = rd(gpucc.base + board->gpucc_cx_gmu_cbcr);
	/* GX is the GMU's (step 4 says why), so its state is no gate for reading the GMU's own
	 * window: the vendor's driver reads that window with CX alone. */
	if (!cx_is_on() || (gmu_clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno: %u: needs CX on (it is %s) and the GMU's clock running (cx_gmu %#x): steps 4 and 5\n", step,
		    cx_is_on() ? "on" : "off", gmu_clock);
		return false;
	}
	/* (a7xx used to return here. Then nothing checked the register-bus clock, and step 6's first
	 * read of the GMU came back as an external abort -- an unclocked block. Same check for both.) */
	ahb_hz = measure_quietly(GPUCC_DEBUG_AHB);
	if (ahb_hz == MEASURE_FAILED) {
		printf("adreno: %u: the frequency counter did not finish (step 5 shows whether it works)\n", step);
	} else {
		printf("adreno: %u: the GPU block's register bus clock (ahb) measures %u Hz\n", step, ahb_hz);
	}
	/* The counter is a port of the SM7325 (yupik) debug block and does not finish on this SoC.
	 * Fall back to the one bit that decides whether the GMU answers at all: the root enable of
	 * its clock source. Set, the block is clocked and reading it is safe; clear, it is not. */
	if (ahb_hz == MEASURE_FAILED) {
		uint32_t rcg = rd(gpucc.base + board->gpucc_gmu_rcgr);
		vm_offset_t misc = ml_io_map(0x03d9e000, 0x1000);

		/*
		 * gmu_clk_src's bit 31 did not take the write (step 5 shows the attempt, this gives 0 after
		 * it), so it is status or held by hardware and is no use as a gate. What says whether a
		 * register in the CX block will answer is another register in the same block: read cx_misc
		 * first and report it, then let step 6 try the GMU window -- a gate that only ever refuses
		 * teaches nothing.
		 */
		printf("adreno: %u: the counter cannot measure (a port of SM7325's debug block); gmu_clk_src %#x, "
		    "its bit 31 did not take the write\n", step, rcg);
		printf("adreno: %u: cx_misc (0x3d9e000) answers %#x; going on to the GMU at %#x, %#x long\n", step,
		    misc != 0 ? rd(misc) : 0xffffffffu, board->gmu_phys, board->gmu_size);
		return true;
	}
	if (ahb_hz < 1000000) {
		printf("adreno: %u: without that clock a register of the GPU block would not answer: not touching it\n", step);
		return false;
	}
	return true;
}

/* ---- step 6: the first words from the GPU block ------------------------------------------------------- */

/*
 * Offsets in the GMU's own words, as gen7_reg.h/a6xx_reg.h count them: kgsl_regmap_read() turns
 * GEN7_GMU_CM3_SYSRESET (0x1f800) into 0x3d00000 + 4 * 0x1f800, and the window this kernel maps
 * (dt qcom,gen7-gmu reg[0]) begins at 0x3d68000, so 0x68000 comes off. The fork's a6xx_gmu.xml
 * numbers (CM3_SYSRESET at word 0x5000) are a different map: they land at 0x3d7c000, a hole in
 * the window, and the bus aborted on the read (fault_code 0x10, esr 0x96000010).
 */
static struct block gmu = { "gmu", 0, 0 };
#define GMU_WORD(d)             (4u * (d) - 0x68000u)
#define GMU_ITCM                GMU_WORD(0x1b400)       /* 16K of code (its data begins at 0x1c400) */
#define GMU_DTCM                GMU_WORD(0x1c400)       /* 16K of data */
#define GMU_DTCM_VERSIONS       GMU_WORD(0x1c400 + 0xff8) /* five words where loaded firmware carries its versions */
#define GMU_CM3_SYSRESET        GMU_WORD(0x1f800)       /* 1: the processor is held in reset */
#define GMU_ICACHE_CONFIG       GMU_WORD(0x1f400)
#define GMU_DCACHE_CONFIG       GMU_WORD(0x1f401)
#define GMU_SYS_BUS_CONFIG      GMU_WORD(0x1f40f)
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x1f801)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x1f81c)       /* 0xbabeface once firmware has started */
#define GMU_CM3_CFG             GMU_WORD(0x1f82d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x1f888)       /* counts the 19.2 MHz crystal */
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x1f889)
#define GMU_PWR_COL_INTER_FRAME_CTRL GMU_WORD(0x1f8c0)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x1f8d0)    /* gen7's GFX_PWR_CLK_STATUS: bit 0, the 3D core has no power; bit 1, no clock */
#define GMU_RPMH_CTRL           GMU_WORD(0x1f8e8)
#define GMU_RPMH_CTRL_ALL       0xf11                   /* the RPMh interface, and every vote the firmware may cast */
#define GMU_RPMH_POWER_STATE    GMU_WORD(0x1f8ec)
#define GMU_CX_FAL_INTF         GMU_WORD(0x1f8f0)
#define GMU_CX_FALNEXT_INTF     GMU_WORD(0x1f8f1)
#define GMU_PWR_COL_CP_MSG      GMU_WORD(0x1f900)       /* at start: where the log is, and its pages - 1 */
#define GMU_PWR_COL_CP_RESP     GMU_WORD(0x1f901)       /* ... and how far it was written before */
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x1f980)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x1f981)
#define GMU_HFI_SFR_ADDR        GMU_WORD(0x1f982)       /* at start: which GPU this is */
#define GMU_HFI_QTBL_INFO       GMU_WORD(0x1f984)
#define GMU_HFI_QTBL_ADDR       GMU_WORD(0x1f985)
#define GMU_HFI_CTRL_INIT       GMU_WORD(0x1f986)
#define GMU_GMU2HOST_INTR_CLR   GMU_WORD(0x1f991)
#define GMU_GMU2HOST_INTR_INFO  GMU_WORD(0x1f992)       /* bit 0: a message for us; bit 31: the 3D core is ours */
#define GMU_HOST2GMU_INTR_SET   GMU_WORD(0x1f994)       /* bit 0: a message for the GMU; bit 30, 31: see step 17 */
#define GMU_GENERAL_7           GMU_WORD(0x1f9cc)
#define GEN7_GMU_GENERAL_8      GMU_WORD(0x1f9cd)
#define GEN7_GMU_GENERAL_9      GMU_WORD(0x1f9ce)
#define GEN7_GMU_GENERAL_10     GMU_WORD(0x1f9cf)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x23b05)
#define GMU_RSCC_CONTROL_REQ    GMU_WORD(0x23b07)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x23b08)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x23b0c)
#define GMU_AO_AHB_FENCE_CTRL   GMU_WORD(0x23b10)
#define GMU_AHB_FENCE_RANGE_0   GMU_WORD(0x23b11)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x23b13)

/*
 * dt: qcom,kgsl-3d0 "kgsl_3d0_reg_memory" at 0x3d00000 is the 3D core, and without power (the GMU
 * gives it) not to be touched -- except its bus interface ("GBIF", words 0x3c00 on in a6xx.xml),
 * which is in the CX half: the vendor's driver writes GBIF_HALT before it starts the GMU.
 * "cx_misc" at 0x3d9e000 is a page of odds and ends in the CX half.
 */
static struct block gbif = { "gbif", 0x03d00000 + 4 * 0x3c00, 0x1000 };
#define GBIF_SCACHE_CNTL0       (4 * 0x01)
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)
static struct block cx_misc = { "cx_misc", 0x03d9e000, 0x1000 };
#define CX_MISC_SYSTEM_CACHE_CNTL_0 (4 * 1)
#define CX_MISC_SYSTEM_CACHE_CNTL_1 (4 * 2)

static bool
step_first_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz;

	if (!cx_ready()) {
		return false;
	}
	if (gmu.base == 0) {
		gmu.phys = (uint32_t)board->gmu_phys;
		gmu.size = (uint32_t)board->gmu_size;
	}
	announce("read registers of the GMU: the first words from the GPU block itself");
	printf("adreno: %u: its window is %#x, %#x long (the device tree's qcom,gen7-gmu reg[0])\n", step,
	    gmu.phys, gmu.size);
	/*
	 * qcom,gdsc@3d68024 (gx_clkctl_gx_gdsc) sits inside this window but is status = "disabled"
	 * in Android's device tree, so its answer says nothing about the block being usable. It is
	 * kept only because the address is implemented: it answered 0 with GX collapsed, before the
	 * CM3 read aborted.
	 */
	get(&gmu, "gx_clkctl gdscr (gx_clkctl_gx_gdsc)", 0x24);
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "general_7", GMU_GENERAL_7);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	printf("adreno: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
	    "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz (firmware may start it): see the rest");
	if (!board->is_a7xx) {
		for (uint32_t i = 0; i < 5; i++) {
			get(&gmu, "dtcm, where firmware has its versions", GMU_DTCM_VERSIONS + 4 * i);
		}
	}

	announce("read the GPU's bus interface (0x3d0f000) and the CX odds and ends (0x3d9e000)");
	get(&gbif, "scache_cntl0", GBIF_SCACHE_CNTL0);
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&cx_misc, "system_cache_cntl_0", CX_MISC_SYSTEM_CACHE_CNTL_0);
	get(&cx_misc, "system_cache_cntl_1", CX_MISC_SYSTEM_CACHE_CNTL_1);
	return true;
}

/* ---- step 7: the GMU's channel to RPMh ----------------------------------------------------------------- */

/*
 * dt: qcom,kgsl-3d0 "rscc" at 0x3de0000: the same kind of command slots through which this
 * kernel asks RPMh for the touch screen's supply, but the GMU's own; through them its firmware
 * raises the 3D core's rail. Offsets in words: a6xx_gmu.xml, domain A6XX_RSCC.
 */
static struct block rscc = { "rscc", 0x03de0000, 0x2000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0     (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)
#define RSCC_ASLEEP             (1u << 16)      /* of rsc_status0: handed over to the always-on sequencer */
#define RSCC_DRV_CONFIG         0x00c
#define RSCC_SLOTS              0xd00           /* +0 interrupts enabled, +4 pending */
#define RSCC_SLOT_SIZE          0x2a0

/*
 * The channel's state and its four command slots (Linux's rpmh-rsc.c: slot n at 0xd00 + 0x2a0 n:
 * +0x10 commands whose completion is waited for, +0x14 control, +0x18 idle, +0x1c commands
 * enabled, then 20 bytes a command from +0x30: number, address, data, status).
 */
static void
rscc_show(void)
{
	get(&rscc, "drv config", RSCC_DRV_CONFIG);
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "slots' interrupts enabled", RSCC_SLOTS);
	get(&rscc, "slots' interrupts pending", RSCC_SLOTS + 4);
	for (uint32_t slot = 0; slot < 4; slot++) {
		vm_offset_t at = rscc.base + RSCC_SLOTS + RSCC_SLOT_SIZE * slot;

		printf("adreno: %u: rscc slot %u: wait %#x, control %#x, idle %#x, enabled %#x; commands (number, address, data, "
		    "status):\n", step, slot, rd(at + 0x10), rd(at + 0x14), rd(at + 0x18), rd(at + 0x1c));
		printf("adreno: %u:   %x %x %x %x | %x %x %x %x | %x %x %x %x | %x %x %x %x\n", step,
		    rd(at + 0x30), rd(at + 0x34), rd(at + 0x38), rd(at + 0x3c), rd(at + 0x44), rd(at + 0x48), rd(at + 0x4c),
		    rd(at + 0x50), rd(at + 0x58), rd(at + 0x5c), rd(at + 0x60), rd(at + 0x64), rd(at + 0x6c), rd(at + 0x70),
		    rd(at + 0x74), rd(at + 0x78));
	}
}

static bool
step_rscc(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the GMU's RPMh channel");
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	return true;
}

/* ---- steps 8 and 9: the SMMU --------------------------------------------------------------------------- */

/*
 * dt: kgsl-smmu@3da0000 (qcom,qsmmu-v500: an Arm MMU-500), 0x20000 long, with "qcom,skip-init":
 * the vendor's driver leaves its stream table as it finds it. Its streams (dt: qcom,kgsl-iommu
 * and qcom,gmu, "iommus"): 0 the GPU, 1 its low-priority twin, 2 secure content, 5 the GMU.
 * The global registers here are the ones the hypervisor imitates; offsets: arm-smmu.h.
 */
static struct block smmu = { "smmu", 0x03da0000, 0x20000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page; [17:16] kind of context */
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;

/* The layout of the SMMU's register space from its ID registers; false if it is not what the device tree says. */
static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("adreno: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page > smmu.size) {
		printf("adreno: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	return true;
}

static bool
step_smmu(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x3da0000): global registers, which the hypervisor answers for");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);

	announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader and hypervisor left them");
	for (uint32_t i = 0; i < smmu_matches && i < 16; i++) {
		get(&smmu, "smr", SMMU_SMR(i));
		get(&smmu, "s2cr", SMMU_S2CR(i));
	}
	for (uint32_t i = 0; i < smmu_contexts && i < 16; i++) {
		get(&smmu, "cbar", smmu_page + SMMU_CBAR(i));
	}
	return true;
}

/*
 * Only context 0, the one both the vendor's driver and Linux give the GPU itself: of the others
 * at least one belongs to TrustZone (secure content), and not all can be expected to answer.
 */
static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!cx_ready()) {
		return false;
	}
	announce("read translation context 0 of the SMMU: registers of the hardware itself, not the hypervisor's");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- step 10: the GMU's memory -------------------------------------------------------------------------- */

/*
 * Firmware is loaded by writing it through these registers into the processor's memory while
 * the processor is held in reset (a6xx_gmu_fw_load). Two words of data memory go there and back,
 * and what was there is put back.
 */
static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2];

	if (!cx_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory (0x3d71000), read them back");
	if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == 0xbabeface) {
		printf("adreno: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("adreno: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- memory the GPU block reaches ------------------------------------------------------------------- */

/*
 * Whatever in the GPU block reads or writes memory -- the GMU's processor, later the 3D core --
 * does it through the SMMU, which makes of the address it uses ("the GPU's address" below) a
 * physical one by walking page tables of ours, as a processor's MMU does: here always three
 * levels of 4K pages, 39 bits of address (dt: qcom,use-3-lvl-tables). The SMMU has seven such
 * translations ("contexts") and a table of which stream of requests -- 0 is the 3D core, 5 the
 * GMU (dt: iommus) -- goes through which. Formats and the order of programming: Linux
 * drivers/iommu/io-pgtable-arm.c and arm-smmu.c.
 */
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_CACHED              (1ull << 2)     /* memory attribute 1 of SMMU_MAIR0; without it 0, uncached */
#define PTE_UNPRIVILEGED        (1ull << 6)     /* the 3D core's accesses are; the GMU's are not */
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)

#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_STREAM_MASK        0x400           /* dt: the second cell of every "iommus" of this SMMU */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))                       /* in the second page, like CBAR */
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0           0x68
#define SMMU_CB_FSYNR1           0x6c
#define SMMU_CB_RESUME           0x008
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12) /* on; faults recorded, no interrupt */
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */

extern void loader_return(void);

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* Push what this processor has written to a stretch of memory out of its cache, and forget it: the GPU block reads memory. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
}

#define SPACE_TABLES            4096            /* was 1024; need up to 1792+ for 3.5 GB address space */

struct gpu_space {
	const char *name;
	uint32_t context;               /* which of the SMMU's */
	uint32_t control;               /* bits of its control register besides the usual */
	uint32_t table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
		bool changed;           /* since it was last written out of this processor's cache (space_commit()) */
	} tables[SPACE_TABLES];
	uint64_t *last_entry, *last_table;      /* the level 2 entry looked up last, and its table */
	uint32_t last_changed;                  /* the table an entry was written in last */
};

static uint64_t *
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;

	if (space->table_count == SPACE_TABLES) {
		return NULL;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return NULL;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
	space->tables[space->table_count++].table = table;
	return table;
}

/* The table an entry of a higher table leads to; made if the entry is empty. */
/* An entry of one of the tables has been written: that table is to go out to memory. */
static void
space_changed(struct gpu_space *space, const uint64_t *entry)
{
	if ((uintptr_t)entry - (uintptr_t)space->tables[space->last_changed].table < 4096) {
		space->tables[space->last_changed].changed = true;      /* page after page of one buffer */
		return;
	}
	for (uint32_t i = space->table_count; i-- > 0;) {
		if ((uintptr_t)entry - (uintptr_t)space->tables[i].table < 4096) {
			space->tables[i].changed = true;
			space->last_changed = i;
			return;
		}
	}
}

/* The SMMU reads the tables from memory, not from this processor's cache. */
static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->table_count; i++) {
		if (space->tables[i].changed) {
			to_memory((vm_offset_t)space->tables[i].table, 4096);
			space->tables[i].changed = false;
		}
	}
}

static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
				return space->tables[i].table;
			}
		}
		return NULL;
	}
	table = space_new_table(space);
	if (table != NULL) {
		*entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
		space_changed(space, entry);
	}
	return table;
}

/* One 4K page. */
static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
		return false;
	}
	level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
	if (level2 == NULL) {
		return false;
	}
	if (space->last_entry == &level2[address >> 21 & 0x1ff]) {
		level3 = space->last_table;             /* page after page of one buffer: the same table */
	} else {
		level3 = space_next(space, &level2[address >> 21 & 0x1ff]);
		space->last_entry = &level2[address >> 21 & 0x1ff];
		space->last_table = level3;
	}
	if (level3 == NULL) {
		space->last_entry = NULL;
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
	space_changed(space, level3);
	return true;
}

/* Memory of ours that the GPU block sees at an address of its own. */
struct gpu_block {
	uint64_t address;               /* the GPU's */
	uint32_t size;
	vm_offset_t memory;             /* ours */
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
			return false;
		}
	}
	to_memory(block->memory, block->size);
	printf("adreno: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
	    address + block->size - 1, kvtophys(block->memory));
	return true;
}

/*
 * A second view of a block without this processor's cache in between, for memory that both
 * sides write a word at a time. Only blocks of one page of ours: those are in one piece.
 */
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	uint64_t phys = kvtophys(block->memory);

	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (kvtophys(block->memory + offset) != phys + offset) {
			return 0;
		}
	}
	return ml_io_map_wcomb(phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

/* What a context has recorded of a request it could not serve. */
static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {             /* bits 10:9 only tell the format of the syndrome */
		return false;
	}
	printf("adreno: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
	    (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
	    rd(smmu.base + context + SMMU_CB_FSYNR0));
	return true;
}

/*
 * One more stream through a context that is on. A match is taken only as the device tree has
 * it, a stream and the mask 0x400: one entry for streams 0 and 1 (mask 0x401) reads back 0.
 */
static bool
space_stream(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | board->smmu_stream_mask << 16 | stream);
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | board->smmu_stream_mask << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context) {
		printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	return true;
}

/* Give a context our tables and send a stream through it; `match` is the entry of the stream table to use. */
static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;        /* tagged with the context's number */

	for (uint32_t i = 0; i < space->table_count; i++) {
		to_memory((vm_offset_t)space->tables[i].table, 4096);
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
	put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
	put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
	put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
	put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
	put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
	put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
	put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("adreno: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
		    rd(smmu.base + context + SMMU_CB_TLBSTATUS));
		return false;
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | board->smmu_stream_mask << 16 | stream);

	/* the global ones are the hypervisor's to grant: see what it made of them */
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | board->smmu_stream_mask << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
	    get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	get(&smmu, "cb tcr", context + SMMU_CB_TCR);
	get(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context));
	get(&smmu, "cb tcr2", context + SMMU_CB_TCR2);
	get(&smmu, "cb fsr", context + SMMU_CB_FSR);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	return true;
}

/* A stream goes nowhere again, and its context translates nothing. */
static void
space_off(struct gpu_space *space, uint32_t match)
{
	put(&smmu, "smr", SMMU_SMR(match), 0);
	put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
	put(&smmu, "cb sctlr", smmu_context(space) + SMMU_CB_SCTLR, 0);
}

/* ---- step 11: TrustZone -------------------------------------------------------------------------------- */

/*
 * The 3D core comes out of reset in "secure" mode, and on a phone only TrustZone lets it out: it
 * does when handed the vendor's signed zap shader to authenticate (a6xx_gpu.c: a6xx_hw_init;
 * qcom_scm.c: qcom_scm_pas_*; the GPU is image 13). Whether it will do that for this kernel
 * decides whether there can be a GPU at all. Questions that pass no memory show whether it
 * listens: the one Linux asks before anything else (is the call "is this call available"
 * available), whether the calls that authenticate an image are, and which images it says it
 * "supports". The last is asked of the GPU's and, for comparison, of images this phone is known
 * to start that way (dt: qcom,pas-id of the audio, modem and video firmware): no GPU driver asks
 * it, Linux's or the vendor's, so what the answer for the GPU means is not known beforehand.
 * A call is an SMC, which the hypervisor passes on; convention: qcom_scm-smc.c.
 */
#define SCM_FAST                (1ull << 31)            /* answered at once; without it a call may be preempted */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* two: a number, and the address of memory it may read and write */
#define SCM_TWO_VALUES          2                       /* two plain numbers: Linux's SCM_ARGS(2, VALUE, VALUE) */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */
#define PAS_GPU                 13

/* One call. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0;

	printf("adreno: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	hold(true);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adreno: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adreno: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static bool
step_trustzone(void)
{
	static const struct {
		const char *name;
		uint32_t id;
	} images[] = { { "audio", 1 }, { "modem", 4 }, { "video", 9 }, { "GPU zap shader", PAS_GPU } };
	uint64_t answer = 0, how = SCM_FAST | SCM_64 | SCM_SIP;
	uint32_t fast[4] = { 0 }, slow[4] = { 0 };

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE, 0,
	    0, &answer) != 0 || answer != 1) {
		how = SCM_FAST | SCM_SIP;       /* the same with 32-bit arguments, Linux's second try */
		if (scm("the same, the 32-bit way", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
		    0, 0, &answer) != 0 || answer != 1) {
			printf("adreno: %u: TrustZone does not answer the way it answers Linux\n", step);
			return false;
		}
	}

	announce("ask TrustZone whether it has the calls that authenticate a firmware image");
	scm("for the call that takes an image's signature", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_INIT_IMAGE, 0, 0, &answer);
	scm("for the call that authenticates and starts an image", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_AUTH_AND_RESET, 0, 0, &answer);

	announce("ask TrustZone which images it \"supports\": three this phone starts through it, and the GPU's; each asked "
	    "both ways, at once and (as Linux does) as a call that may be preempted");
	for (unsigned i = 0; i < 4; i++) {
		answer = 0;
		fast[i] = scm(images[i].name, how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0, &answer) == 0 && answer;
		answer = 0;
		slow[i] = scm(images[i].name, (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0,
		    0, &answer) == 0 && answer;
	}
	printf("adreno: %u: \"supported\", asked at once / as Linux asks: audio %u/%u, modem %u/%u, video %u/%u, GPU %u/%u\n",
	    step, fast[0], slow[0], fast[1], slow[1], fast[2], slow[2], fast[3], slow[3]);
	printf("adreno: %u: %s\n", step, fast[3] || slow[3] ? "TrustZone lists the GPU's zap shader" :
	    fast[0] || slow[0] || fast[1] || slow[1] || fast[2] || slow[2] ?
	    "TrustZone lists others but not the GPU's: step 12 will show whether that matters" :
	    "TrustZone lists none, not even those the phone starts: this question tells nothing here");
	return true;
}

/* ---- step 12: the zap shader's signature ----------------------------------------------------------------- */

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);


/*
 * A file of the small image that travels with the kernel -- userland/build.sh puts what is in
 * fw-gpu/ into its directory fw/ -- found in the memory of the RAM disk itself: at this point of
 * the start that image is the root if there is no system image, and not mounted at all if there
 * is one (it is /dev/md1 then; see userland/src/stage0.c).
 */
static bool image_asked_quietly;        /* image_file() says nothing: is the file there at all */

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = hakios_image_file(directory, name, size, &disk);  /* (hakios.image.c) */

	if (data != NULL) {
		if (!image_asked_quietly) {
			printf("adreno: %u: %s/%s: %llu bytes in the image of /dev/md%d\n", step, directory, name, *size, disk);
		}
		return data;
	}
	if (!image_asked_quietly) {
		printf("adreno: %u: no %s/%s in the small image: put the phone's /vendor/firmware/%s into fw-gpu/ and run "
		    "userland/build.sh\n", step, directory, name, name);
	}
	return NULL;
}

/*
 * The first of the three calls with which Linux has a zap shader loaded (mdt_loader.c:
 * qcom_mdt_pas_init, qcom_scm.c: qcom_scm_pas_init_image): TrustZone is given the image's header --
 * the ELF header and the block of hashes and certificates, which is all of the file a660_zap.mdt
 * -- and checks the signature. Nothing of the GPU is involved, so it can be asked now. The other
 * two (where the image will be, and "authenticate and start") need the 3D core powered.
 *
 * The header goes in memory of ours, page-aligned and in one piece. Qualcomm's note at that
 * call says that for these calls the hypervisor itself arranges TrustZone's access to the memory
 * ("creates SHM bridges on behalf of Linux"), and that TrustZone protects the memory while it
 * works: so it is not touched again here, and never freed.
 */
#define ZAP_MEMORY_SIZE 0x2000u   /* (the reserved region gpu_microcode_region@9b09a000) */

/* Where the code segment sits in the image, how big the image is, and where the segment is inside the
 * file, all read out of the program table by zap_header(): gen70900's segment has p_vaddr 0x1000,
 * a660's has 0. `zap_combined`: the file is the whole image the phone ships (gen70900_zap.mbn -- header,
 * code and hashes in one file, the hash segment at the offset the header names, 0x2000), not a header
 * with its code beside it (.mdt + .b02). TrustZone takes the file as it is, and a header whose hash
 * segment has been moved to make a .mdt breaks the hash of the header itself. */
static uint32_t zap_segment_at;
static uint32_t zap_segment_from;
static uint32_t zap_segment_size;
static uint32_t zap_image_size = 0x1000;
static const uint8_t * zap_file;
static uint64_t zap_file_size;
static uint64_t zap_metadata_size;      /* the header and the hashes, one after the other, as the PAS call takes them */
static bool zap_combined;

/* The zap shader's header in a page of ours, as TrustZone wants it; where that page is in physical memory. */
static bool
zap_header(uint64_t *phys)
{
	const uint8_t *header;
	vm_offset_t page = 0;
	uint64_t size = 0, bytes;
	uint32_t segments, hashes = 0, hashes_at = 0, table, ehdr;
	uint32_t lowest = ~0u, highest = 0;

	header = image_file("fw", board->zap_fw_name, &size);
	if (header == NULL) {
		return false;
	}
	/* a 32-bit ELF: the count of segments at 44, the table of them at 52, 32 bytes each; in a table entry
	 * the file's own offset of the segment at +4, where it goes in memory at +8, its sizes at +16 and +20 */
	segments = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || segments < 2 || size > 0x4000) {
		printf("adreno: %u: that is not such an image (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step,
		    header[0], header[1], header[2], header[3], segments, size);
		return false;
	}
	table = 52 + 32 * segments;
	/*
	 * What TrustZone is handed is not the file but a header with the segment of hashes directly after
	 * it: the loader copies phdr[0]'s p_filesz bytes (the ELF header and the table of segments) and
	 * then the hash segment, no padding between them (mdt_loader.c: qcom_mdt_read_metadata). This
	 * phone's image keeps its hashes at their own offset (0x2000), which is not those bytes.
	 */
	memcpy(&ehdr, header + 52 + 16, sizeof(ehdr));
	if (ehdr == 0 || ehdr > table) {
		ehdr = table;
	}
	zap_segment_at = 0;
	zap_segment_from = 0;
	zap_segment_size = 0;
	zap_image_size = 0x1000;
	for (uint32_t i = 1; i < segments; i++) {
		uint32_t p_offset, p_flags, p_filesz, p_paddr, p_memsz;
		memcpy(&p_offset, header + 52 + 32 * i + 4, sizeof(p_offset));
		memcpy(&p_filesz, header + 52 + 32 * i + 16, sizeof(p_filesz));
		memcpy(&p_flags, header + 52 + 32 * i + 24, sizeof(p_flags));
		memcpy(&p_paddr, header + 52 + 32 * i + 12, sizeof(p_paddr));   /* where it goes, not p_vaddr */
		memcpy(&p_memsz, header + 52 + 32 * i + 20, sizeof(p_memsz));
		if ((p_flags & (7u << 24)) == (2u << 24)) { /* QCOM_MDT_TYPE_HASH */
			hashes = p_filesz;
			hashes_at = p_offset;
			break;
		}
		if (p_filesz != 0) {                    /* the segment that goes in memory */
			zap_segment_at = p_paddr;
			zap_segment_from = p_offset;
			zap_segment_size = p_filesz;
		}
		if (p_memsz != 0) {                     /* where the image lies in the memory the pas call names */
			if (p_paddr < lowest) {
				lowest = p_paddr;
			}
			if (p_paddr + p_memsz > highest) {
				highest = p_paddr + p_memsz;
			}
		}
	}
	/*
	 * The image is relocatable (0x08000000 in its flags), so its segments lie relative to the lowest
	 * address it names rather than at that address: the loader puts each one at p_paddr - min inside
	 * the memory the mem-setup call was given, and the span it names begins at that min
	 * (gen70900's one segment has p_paddr 0x1000: it goes at the start of the region).
	 */
	if (lowest == ~0u) {
		lowest = 0;
	}
	zap_segment_at -= lowest;
	zap_image_size = (((highest > lowest ? highest : lowest) - lowest) + 0xfffu) & ~0xfffu;
	if (zap_image_size > ZAP_MEMORY_SIZE) {
		printf("adreno: %u: the zap image wants %#x bytes, the reserved memory is %#x\n", step,
		    zap_image_size, ZAP_MEMORY_SIZE);
		return false;
	}
	if (hashes == 0) {
		memcpy(&hashes, header + 52 + 32 + 16, sizeof(hashes));
	}
	/*
	 * Two shapes of file: the phone's own image, which holds the header, the code and the hashes (the
	 * hash segment at the offset the header names, 0x2000), or a header that is the header followed by
	 * the hashes alone, with the code in a file beside it. Neither shape is what TrustZone is handed:
	 * the pair built below is.
	 */
	zap_combined = size != (uint64_t)table + hashes;
	if (!zap_combined && hashes_at != table) {
		printf("adreno: %u: the hashes are at %#x, not after the table (%#x): not either shape TrustZone takes\n",
		    step, hashes_at, table);
		return false;
	}
	printf("adreno: %u: %u segments, %u bytes of table, %u of hashes at %#x, %llu bytes in the file: %s\n", step,
	    segments, table, hashes, hashes_at, size,
	    zap_combined ? "THE IMAGE ITSELF, the code inside the file" : "a header alone, the code in a file beside it");
	/*
	 * The buffer TrustZone reads: the header and the hashes, page-aligned and in one piece of physical
	 * memory (four kilobytes one page; the allocator does not promise that pages follow one another,
	 * so ask again until they do).
	 */
	bytes = (ehdr + hashes + 4095) & ~4095ull;
	for (int tries = 0; tries < 64 && page == 0; tries++) {
		uint64_t first;

		if (kmem_alloc(kernel_map, &page, bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			printf("adreno: %u: no memory\n", step);
			return false;
		}
		first = kvtophys(page);
		for (uint64_t offset = 4096; offset < bytes; offset += 4096) {
			if (kvtophys(page + offset) != first + offset) {
				kmem_free(kernel_map, page, bytes);
				page = 0;
				break;
			}
		}
	}
	if (page == 0) {
		printf("adreno: %u: no %llu bytes in one piece of physical memory for the image\n", step, bytes);
		return false;
	}
	if (ehdr + hashes > size || (uint64_t)(hashes_at ? hashes_at : table) + hashes > size) {
		printf("adreno: %u: the header and its %u bytes of hashes are not inside the %llu-byte file\n", step,
		    hashes, size);
		return false;
	}
	/* the header as it is, then the hashes directly after it: what the PAS init call reads */
	memcpy((void *)page, header, ehdr);
	memcpy((void *)(page + ehdr), header + (hashes_at ? hashes_at : table), hashes);
	to_memory(page, ehdr + hashes);         /* TrustZone reads memory, not this processor's cache */
	*phys = kvtophys(page);
	zap_file = header;
	zap_file_size = size;
	zap_metadata_size = ehdr + hashes;
	return true;
}

static bool
step_zap_signature(void)
{
	uint64_t header = 0, answer = ~0ull;
	int32_t result;

	announce("find the zap shader's header in the small image that came with the kernel");
	if (!zap_header(&header)) {
		return false;
	}
	announce("hand the header to TrustZone to check its signature (image 13); a call that may be preempted, as Linux's is");
	result = scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER,
	    PAS_GPU, header, zap_metadata_size, &answer);
	if (result == 0 && answer == 0) {
		printf("adreno: %u: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n", step);
		return true;
	}
	printf("adreno: %u: TrustZone does not take it: the call returns %d, its answer is %#llx\n", step, result, answer);
	return false;
}

/* ---- step 13: memory for the GMU ------------------------------------------------------------------------ */

/*
 * The GMU is a Cortex-M3 with 16K of code memory and 16K of data memory of its own (written
 * through registers, step 10); the rest of its firmware and everything it shares with us it
 * reaches through the SMMU, at addresses the firmware file names: a file of blocks, each "this
 * goes at address A" or, with no data, "I need N bytes at address A" (vendor's
 * adreno_a6xx_gmu.c: a6xx_gmu_parse_fw, a6xx_gmu_load_fw). Besides those, where the vendor's
 * driver puts them: 16K for the firmware's log and a page for the table and queues of the
 * message protocol ("HFI").
 */
#define GMU_ITCM_ADDRESS        0x00000000u
#define GMU_DTCM_ADDRESS        0x10004000u     /* a6xx_gmu_fw_load: the 650 family's */
#define GMU_TCM_SIZE            0x4000u
#define GMU_LOG_ADDRESS         0x60002000u     /* after the two pages the firmware asks for at 0x60000000 */
#define GMU_LOG_SIZE            0x4000u
#define GMU_HFI_ADDRESS         0x60006000u
#define GMU_STREAM              5
#define GMU_MATCH               1               /* entry of the stream table; 0 is kept for the 3D core */
/*
 * The vendor's driver for this GPU does not write the 3D core's ring registers at all: the GMU keeps
 * the ring, and the host hands it work as one message on a dispatch queue (gpu_hwsched=1). That needs
 * the seventeen queues of its HFI table (adreno_gen7_hwsched_hfi.c: HFI_QUEUE_MAX = HFI_QUEUE_DEFAULT_CNT
 * 3 + HFI_QUEUE_DISPATCH_MAX_CNT 14), four to a page of memory of ours, after the table's own page.
 */
#define HFI_HWSCHED_QUEUES      17
#define HFI_QUEUE_BLOCK_SIZE    0x4000u         /* one uncached view; four queues of a page each fit in it */
#define HFI_QUEUE_BLOCKS        5
#define HFI_HWSCHED_QUEUE_AT(n) (0x4000u + 0x1000u * (n))       /* as an offset from GMU_HFI_ADDRESS */
#define HFI_QUEUE_BLOCK(n)      ((n) / 4)
#define HFI_QUEUE_IN_BLOCK(n)   ((n) % 4)

struct gmu_firmware_block {
	uint32_t address, size, type, value;
};
#define GMU_FIRMWARE_NEEDS      1               /* types of blocks without data: memory wanted, value = bytes */
#define GMU_FIRMWARE_NEEDS_KEPT 7

/* boot-arg gpu_hwsched=1: the vendor's way, through the GMU's dispatch queue, instead of the ring. */
static bool
hwsched_on(void)
{
	static int on = -1;

	if (on < 0) {
		uint32_t asked = 0;

		PE_parse_boot_argn("gpu_hwsched", &asked, sizeof(asked));
		on = asked != 0;
	}
	return on;
}

/* boot-arg adreno_dms=0: the processor's memory work stays its own (gen7_cp_init_cmds' BIT(11) mask). */
static bool
dms_on(void)
{
	static int on = -1;

	if (on < 0) {
		uint32_t asked = 1;

		PE_parse_boot_argn("adreno_dms", &asked, sizeof(asked));
		on = asked != 0;
	}
	return on;
}

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_blocks[12], gmu_log, gmu_hfi;
static struct gpu_block hfi_blocks[HFI_QUEUE_BLOCKS];   /* the hwsched queues, four to each */
static vm_offset_t hfi_data[HFI_HWSCHED_QUEUES];        /* where each queue's words are, in memory of ours */
static unsigned gmu_block_count;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on, gmu_runs, gmu_talks, core_is_on, rsc_awake, cx_off;
static void gmu_post_mortem(void);

static bool
in_tcm(uint32_t address)
{
	return address - GMU_ITCM_ADDRESS < GMU_TCM_SIZE || address - GMU_DTCM_ADDRESS < GMU_TCM_SIZE;
}

static bool
step_gmu_space(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("find the GMU's firmware in the small image, and make the memory it asks for");
	gmu_firmware = image_file("fw", board->gmu_fw_name, &gmu_firmware_size);
	if (gmu_firmware == NULL) {
		return false;
	}
	for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
		struct gmu_firmware_block block;

		memcpy(&block, gmu_firmware + at, sizeof(block));
		at += sizeof(block) + block.size;
		if (at > gmu_firmware_size) {
			printf("adreno: %u: the file ends inside a block\n", step);
			return false;
		}
		if (block.size != 0 || (block.type != GMU_FIRMWARE_NEEDS && block.type != GMU_FIRMWARE_NEEDS_KEPT) ||
		    in_tcm(block.address)) {
			continue;
		}
		if (gmu_block_count == sizeof(gmu_blocks) / sizeof(gmu_blocks[0]) || (block.address & 4095) ||
		    block.value > 0x100000 || !block_new(&gmu_space, &gmu_blocks[gmu_block_count++], block.address, block.value,
		    PTE_GMU)) {
			printf("adreno: %u: cannot make %#x bytes at %#x for the firmware\n", step, block.value, block.address);
			return false;
		}
	}
	if (!block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, PAGE_SIZE, PTE_GMU)) {
		return false;
	}
	/*
	 * The table's page is followed by the queues, one page each. Four queues share one block of memory
	 * of ours so that block_uncached() can look at them through one window (the vendor's own table has
	 * them all in one allocation; here each block only has to be in one piece by itself).
	 */
	if (hwsched_on()) {
		for (unsigned i = 0; i < HFI_QUEUE_BLOCKS; i++) {
			if (!block_new(&gmu_space, &hfi_blocks[i], GMU_HFI_ADDRESS + HFI_QUEUE_BLOCK_SIZE * (i + 1),
			    HFI_QUEUE_BLOCK_SIZE, PTE_GMU)) {
				return false;
			}
		}
	}

	announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it: the first "
	    "writes to the SMMU, the global ones (cbar, cba2r, s2cr, smr) through the hypervisor");
	if (!smmu_layout()) {
		return false;
	}
	gmu_space_is_on = true;         /* whatever of it has been written by the time something fails is taken back */
	return space_on(&gmu_space, GMU_MATCH, GMU_STREAM);
}

/* ---- step 14: the GMU's firmware ----------------------------------------------------------------------- */


static bool
step_gmu_firmware(void)
{
	uint32_t wrong = 0;

	if (!cx_ready() || gmu_firmware == NULL) {
		return false;
	}
	announce("load the GMU's firmware: its first 16K of code and of data through the GMU's registers (0x3d6d000, "
	    "0x3d71000), the rest into the memory just made");
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	for (int pass = 0; pass < 2; pass++) {
		for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
			struct gmu_firmware_block block;
			const uint8_t *data = gmu_firmware + at + sizeof(block);

			memcpy(&block, gmu_firmware + at, sizeof(block));
			at += sizeof(block) + block.size;
			if (block.size == 0) {
				continue;
			}
			if (in_tcm(block.address) && in_tcm(block.address + block.size - 1)) {
				uint32_t registers = block.address < GMU_DTCM_ADDRESS ? GMU_ITCM + block.address - GMU_ITCM_ADDRESS :
				    GMU_DTCM + block.address - GMU_DTCM_ADDRESS;

				for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
					uint32_t word;

					memcpy(&word, data + offset, sizeof(word));
					if (pass == 0) {
						wr(gmu.base + registers + offset, word);
					} else if (rd(gmu.base + registers + offset) != word) {
						wrong++;
					}
				}
				if (pass == 1) {
					printf("adreno: %u: %#x bytes at the GMU's %#x, through its registers\n", step, block.size,
					    block.address);
				}
				continue;
			}
			for (unsigned i = 0; pass == 0 && i <= gmu_block_count; i++) {
				if (i == gmu_block_count) {
					printf("adreno: %u: the firmware has %#x bytes for %#x, where it asked for no memory\n", step,
					    block.size, block.address);
					return false;
				}
				if (block.address >= gmu_blocks[i].address &&
				    block.address + block.size <= gmu_blocks[i].address + gmu_blocks[i].size) {
					memcpy((void *)(gmu_blocks[i].memory + (block.address - gmu_blocks[i].address)), data, block.size);
					to_memory(gmu_blocks[i].memory, gmu_blocks[i].size);
					printf("adreno: %u: %#x bytes at the GMU's %#x, in memory\n", step, block.size, block.address);
					break;
				}
			}
		}
	}
	snprintf(last_write, sizeof(last_write), "step %u wrote the GMU's firmware into its code and data memory", step);
	if (wrong) {
		printf("adreno: %u: %u words of the GMU's own memory do not read back as written\n", step, wrong);
		return false;
	}
	printf("adreno: %u: THE GMU'S FIRMWARE IS LOADED; its versions, read back from the GMU: core %#x, power %#x, HFI %#x\n",
	    step, rd(gmu.base + GMU_DTCM_VERSIONS), rd(gmu.base + GMU_DTCM_VERSIONS + 8), rd(gmu.base + GMU_DTCM_VERSIONS + 16));
	return true;
}

/* ---- step 15: start the GMU ---------------------------------------------------------------------------- */

#define GMU_CHIP                0x06035000      /* dt qcom,chipid 0x06030500 the way the firmware wants it */
#define GMU_STARTED_MASK        0x1ff           /* of fw_init_result, for firmware newer than core 0x20010004 */
#define GMU_STARTED             0x100
#define GMU_FAILED              0xe00

/* The table at the head of the HFI page and the three queues after it, as the vendor's driver lays them out. */
struct hfi_queue {
	uint32_t status, start, type, words, message_words, unused[5], read, write;
};
struct hfi_table {
	uint32_t version, words, first_queue, queue_words, queues, active_queues;
	struct hfi_queue queue[17];
};
#define HFI_QUEUES              3               /* 0: ours to the GMU; 1: its answers; 2: its debugging messages */
#define HFI_QUEUE_WORDS         1024
#define HFI_QUEUE_AT(n)         (((uint32_t)sizeof(struct hfi_table) + 15) / 16 * 16 + 4096 * (n))

static volatile struct hfi_table *hfi;  /* the uncached view */

/*
 * Everything the vendor's driver writes before it lets the processor go (a6xx_gmu_register_config,
 * a6xx_gmu_power_config), in its order; then out of reset (a6xx_gmu_device_start).
 */
static bool
step_gmu_start(void)
{
	static const uint8_t priority[HFI_QUEUES] = { 10, 10, 40 };
	/* the vendor's: the three message queues at priority 0, then the dispatch queues by RB priority */
	static const uint8_t hwsched_priority[HFI_HWSCHED_QUEUES] = {
		0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4
	};
	vm_offset_t hfi_block_base[HFI_QUEUE_BLOCKS] = { 0 };
	uint32_t result = 0;

	if (!cx_ready() || !gmu_space_is_on) {
		return false;
	}
	hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		printf("adreno: %u: the page for the messages is not in one piece\n", step);
		return false;
	}
	hfi->words = sizeof(struct hfi_table) / 4;
	hfi->first_queue = 6;
	hfi->queue_words = sizeof(struct hfi_queue) / 4;
	hfi->queues = hfi->active_queues = hwsched_on() ? HFI_HWSCHED_QUEUES : HFI_QUEUES;
	for (unsigned i = 0; i < hfi->queues; i++) {
		if (hwsched_on()) {
			unsigned block = HFI_QUEUE_BLOCK(i);

			if (hfi_block_base[block] == 0) {
				hfi_block_base[block] = block_uncached(&hfi_blocks[block]);
				if (hfi_block_base[block] == 0) {
					printf("adreno: %u: the queues in block %u are not in one piece\n", step, block);
					return false;
				}
			}
			hfi_data[i] = hfi_block_base[block] + 4096 * HFI_QUEUE_IN_BLOCK(i);
			hfi->queue[i].start = GMU_HFI_ADDRESS + HFI_HWSCHED_QUEUE_AT(i);
			hfi->queue[i].type = i | (uint32_t)hwsched_priority[i] << 8;
		} else {
			hfi_data[i] = (vm_offset_t)hfi + HFI_QUEUE_AT(i);
			hfi->queue[i].start = GMU_HFI_ADDRESS + HFI_QUEUE_AT(i);
			hfi->queue[i].type = i | (uint32_t)priority[i] << 8;
		}
		hfi->queue[i].words = HFI_QUEUE_WORDS;
		hfi->queue[i].status = 1;
	}

	announce("tell the GMU where things are, by its registers: the message table, the log, which GPU this is; its "
	    "caches and its use of RPMh; then let its processor out of reset");
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		printf("adreno: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
		return false;
	}
	/*
	 * Our own leaving (gpu_block_stop) hands the GMU's RPMh channel to the always-on sequencer, and
	 * until it is taken back nothing the GMU asks of RPMh gets there: the vendor's
	 * a6xx_rscc_wakeup_sequence, before the firmware starts.
	 */
	rscc_show();
	if (rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP) {
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 2);
		if (!wait_for(&gmu, GMU_RSCC_CONTROL_ACK, 2, 2, 100) || !wait_for(&rscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, 100)) {
			printf("adreno: %u: the GMU's RPMh channel does not wake\n", step);
		}
		get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
		get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
		get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	}
	rsc_awake = true;
	put(&gmu, "cx_falnext_intf", GMU_CX_FALNEXT_INTF, 1);
	put(&gmu, "cx_fal_intf", GMU_CX_FAL_INTF, 1);
	if (!board->is_a7xx) {
		put(&gmu, "general_7", GMU_GENERAL_7, 1);
	}
	put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
	put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 2);
	put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
	put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
	if (board->is_a7xx) {
		put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, (1u << 31) | (0x32u << 18) | 0x8a0u);
	} else {
		put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, 1u << 31 | (0xa << 2) << 18 | 0x8a0);
	}
	put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
	put(&gbif, "halt", GBIF_HALT, 0);
	if (board->is_a7xx) {
		put(&gmu, "general_9", GEN7_GMU_GENERAL_9, 0);
		put(&gmu, "general_10", GEN7_GMU_GENERAL_10, board->gmu_chipid);
		put(&gmu, "general_8", GEN7_GMU_GENERAL_8, (GMU_LOG_ADDRESS & 0xfffff000u) | ((GMU_LOG_SIZE / 4096 - 1) & 0xffu));
	} else {
		put(&gmu, "pwr_col_cp_resp", GMU_PWR_COL_CP_RESP, 0);
		put(&gmu, "hfi_sfr_addr", GMU_HFI_SFR_ADDR, GMU_CHIP);
		put(&gmu, "pwr_col_cp_msg", GMU_PWR_COL_CP_MSG, GMU_LOG_ADDRESS | (GMU_LOG_SIZE / 4096 - 1));
	}
	put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
	put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
	put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
	if (!board->is_a7xx) {
		put(&gmu, "pwr_col_inter_frame_ctrl", GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
		set(&gmu, "rpmh_ctrl", GMU_RPMH_CTRL, GMU_RPMH_CTRL_ALL);
	}
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 0xffffffff);

	gmu_runs = true;
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
	for (int i = 0; i < 5000 && (result & GMU_STARTED_MASK) != GMU_STARTED && !(result & GMU_FAILED); i++) {
		sleep_us(100);
		result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT);
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	if (space_faulted(&gmu_space) || (result & GMU_STARTED_MASK) != GMU_STARTED) {
		printf("adreno: %u: the GMU's firmware does not come up\n", step);
		gmu_post_mortem();
		return false;
	}
	printf("adreno: %u: THE GMU'S FIRMWARE RUNS\n", step);
	return true;
}

/* ---- step 16: the GMU's way to RPMh, and its message queues ------------------------------------------------ */

/*
 * The GMU raises and drops the 3D core's supply by itself, through command slots of its own
 * (step 7) and a sequencer in the always-on part of the SoC, "PDC". On this SoC the always-on
 * processor programs the PDC; left to us are its two enabling registers and the GMU side's
 * little program (vendor's a6xx_load_pdc_ucode with pdc_in_aop, a6xx_load_rsc_ucode: after the
 * firmware has started and before the message queues are).
 */
static struct block pdc = { "pdc", 0x0b290000, 0x10000 };       /* dt: qcom,gmu "kgsl_gmu_pdc_cfg" */
#define PDC_GPU_ENABLE          (4 * 0x1140)
#define PDC_GPU_SEQ_START_ADDR  (4 * 0x1148)
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)

static bool
step_gmu_queues(void)
{
	static const uint32_t program[5] = { 0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad };
	uint32_t tcs2_val = board->is_a7xx ? 0x80000000 : 0x80000021;

	if (!cx_ready() || !gmu_runs) {
		return false;
	}
	pdc.phys = board->pdc_phys;
	pdc.size = board->pdc_size;
	announce("enable the GPU's part of the always-on sequencer (pdc)");
	get(&pdc, "gpu_enable", PDC_GPU_ENABLE);
	get(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR);
	put(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR, 0);
	put(&pdc, "gpu_enable", PDC_GPU_ENABLE, 0x80000001);
	get(&pdc, "gpu_enable", PDC_GPU_ENABLE);

	announce("give the GMU's RPMh channel its sleep and wake program (rscc), unless it has it");
	if (get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0) == program[0] &&
	    get(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 4) == tcs2_val) {
		goto programmed;
	}
	put(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0, 1u << 24);
	put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
	put(&rscc, "hidden_tcs_cmd0_data", RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	put(&rscc, "hidden_tcs_cmd0_addr", RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	put(&rscc, "hidden_tcs_cmd1_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 2, 0);
	put(&rscc, "hidden_tcs_cmd1_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 2, 0);
	put(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 4, tcs2_val);
	put(&rscc, "hidden_tcs_cmd2_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 4, 0);
	put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
	put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
	put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (unsigned i = 0; i < 5; i++) {
		put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0 + 4 * i, program[i]);
	}
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);

programmed:
	announce("ask the GMU to start its message queues");
	put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
	if (!wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 500)) {
		get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
		get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
		space_faulted(&gmu_space);
		printf("adreno: %u: the GMU does not start its message queues\n", step);
		return false;
	}
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	gmu_talks = true;
	printf("adreno: %u: THE GMU'S MESSAGE QUEUES ARE UP\n", step);
	return !space_faulted(&gmu_space);
}

/* ---- step 17: talk to the GMU; the 3D core powered ---------------------------------------------------------- */

/*
 * A message is words: the first says which (bits 7:0), how many words (15:8), a request or an
 * answer (19:16), and a number (31:20) that the answer repeats in its second word; the third word
 * of an answer is the GMU's verdict, 0 for done. Vendor's adreno_a6xx_hfi.c, Linux a6xx_hfi.c.
 */
#define HFI_BW_TABLE            3
#define HFI_PERF_TABLE          4
#define HFI_START               10
#define HFI_CORE_FW_START       14
#define HFI_TABLE_MSG           15
#define HFI_TABLE_GPU_PERF      1
#define HFI_GX_BW_PERF_VOTE     30
#define HFI_PREPARE_SLUMBER     33
#define HFI_ERROR               100
#define HFI_ANSWER              1               /* in bits 19:16 */
#define HFI_NO_INDEX            0xff

/*
 * The rest of the vendor's message numbers (adreno_hfi.h enum hfi_msg_type and enum hfi_mem_kind), and
 * what the hwsched path needs of its protocol (adreno_gen7_hwsched_hfi.c).
 */
#define HFI_FEATURE_CTRL        11
#define HFI_MEM_ALLOC           20              /* F2H: the GMU wants memory, and waits for the answer */
#define HFI_GMU_CNTR_REGISTER   110             /* F2H: it wants a performance counter */
#define HFI_REGISTER_CONTEXT    128
#define HFI_ISSUE_CMD           130             /* H2F: work, on a dispatch queue */
#define HFI_ISSUE_CMD_RAW       131             /* H2F: command processor words, through the GMU */
#define HFI_TS_RETIRE           133             /* F2H: a submission's timestamp has retired */
#define HFI_CONTEXT_POINTERS    134
#define HFI_FEATURE_HWSCHED     1
#define HFI_FEATURE_PREEMPTION  2               /* the gen8 port disables it before START (adreno8_hwsched_start) */
#define HFI_FEATURE_IFPC        9
#define HFI_FEATURE_KPROF       16
#define HFI_FEATURE_DMS         27
#define HFI_FEATURE_AQE         29
#define HFI_CTXT_FLAG_NOTIFY    (1u << 5)
#define HWSCHED_CONTEXT         1               /* the one context this driver registers */
#define HWSCHED_CTXT_BANK       0               /* the SMMU context the 3D core's table translates in */
#define HWSCHED_DISPATCH_QUEUE  3               /* HFI_DSP_ID_0: the first dispatch queue */
#define HWSCHED_DISPATCH_IRQ    (1u << 2)       /* DISPQ_IRQ_BIT(0) */
#define HWSCHED_TOKEN_AT        0x50            /* in the scratch page: where a submission notes it arrived */
#define HWSCHED_MEMSTORE_KIND   22              /* HFI_MEMKIND_MEMSTORE: a context's timestamps */
#define HWSCHED_ALLOCS          64
#define HWSCHED_MAX_IBS         64
#define HWSCHED_CTXQ_WORDS      12              /* gmu_context_queue_header, in words */
#define HWSCHED_CTXQ_SIZE       4096u           /* the page the vendor gives each context's queue */
#define HFI_GET_VALUE           12
#define HFI_VALUE_CONTEXT_QUEUE 121             /* answered 1 if the firmware wants per-context queues */
#define HWSCHED_GPU_AT          0x18000000ull   /* the allocations, in both address spaces, above everything else */
#define HWSCHED_GPU_END         0x1f000000ull
#define HWSCHED_GMU_AT          0x62000000ull
#define HWSCHED_GMU_END         0x70000000ull

struct hfi_table_perf {
	uint32_t header;
	uint32_t version;
	uint32_t type;
	struct {
		uint32_t count;
		uint32_t stride;
		struct {
			uint32_t vote, cx_vote, khz;
		} levels[4];
	} gx;
	struct {
		uint32_t count;
		uint32_t stride;
		struct {
			uint32_t vote, khz;
		} levels[2];
	} cx;
};

struct hfi_perf_table {
	uint32_t header, gpu_levels, gmu_levels;
	struct {
		uint32_t vote, acd, khz;
	} gpu[16];
	struct {
		uint32_t vote, khz;
	} gmu[4];
};
struct hfi_bw_table {
	uint32_t header, levels, cnoc_commands, ddr_commands, cnoc_wait, ddr_wait;
	uint32_t cnoc_address[6], cnoc_data[2][6], ddr_address[8], ddr_data[16][8];
};

static uint32_t hfi_number;
static uint32_t hfi_answer;             /* what the last answer carried in its third word */
static uint32_t hwsched_ctxq_value = 0xffffffffu;       /* the GMU's answer for HFI_VALUE_CONTEXT_QUEUE */
static bool post_mortem_done;
static uint32_t hwsched_retired_ts;     /* last context timestamp reported by the GMU */
static bool hwsched_mem_reply(const uint32_t *request, uint32_t words);
static void hwsched_cntr_reply(const uint32_t *request, uint32_t words);

/* Everything waiting in a queue of the GMU's; true if the answer to the message numbered `number` was among it. */
static bool
hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
	volatile struct hfi_queue *queue = &hfi->queue[which];
	volatile uint32_t *data = (volatile uint32_t *)hfi_data[which];
	bool answered = false;

	while (queue->read != queue->write) {
		uint32_t message[32] = { 0 }, at = queue->read % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;

		for (uint32_t i = 0; i < words; i++) {
			if (i < 32) {
				message[i] = data[at];
			}
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		queue->read = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if ((message[0] >> 16 & 0xf) == HFI_ANSWER && message[1] >> 20 == number) {
			*verdict = message[2];
			answered = true;
		} else if (hwsched_on() && (message[0] & 0xff) == HFI_MEM_ALLOC) {
			hwsched_mem_reply(message, words);      /* the GMU waits for this one: answer it now */
		} else if (hwsched_on() && (message[0] & 0xff) == HFI_GMU_CNTR_REGISTER) {
			hwsched_cntr_reply(message, words);
		} else if (hwsched_on() && (message[0] & 0xff) == HFI_TS_RETIRE) {
			/* a submission's timestamp retiring: what it carries is worth seeing, the run after
			 * the first one leaving the core busy for good */
			if (message[1] == HWSCHED_CONTEXT && message[2] > hwsched_retired_ts) {
				hwsched_retired_ts = message[2];
			}
			if (hakios_gpu_debug()) printf("adreno: %u: a timestamp has retired: %#x %#x %#x %#x %#x %#x\n", step,
			    message[1], message[2], message[3], message[4], message[5], message[6]);
		} else {
			message[31] = 0;
			if ((message[0] & 0xff) == HFI_ERROR) {         /* vendor's adreno_a6xx_receive_err_req */
				printf("adreno: %u: THE GMU REPORTS AN ERROR: %u, %u, \"%s\"\n", step, message[1] >> 16,
				    message[1] & 0xffff, (const char *)&message[2]);
			} else {
				/*
				 * Everything the message carries, not only its first six words: this is where the
				 * firmware's own account of a submission that never retired arrives (26 words in
				 * the runs that stall at the client's last CP_WAIT_FOR_IDLE).
				 */
				printf("adreno: %u: the GMU says (queue %u, %u words, id %#x):\n", step, which, words,
				    message[0] & 0xff);
				for (uint32_t i = 0; i < words && i < 32; i += 6) {
					uint32_t w[6] = { 0, 0, 0, 0, 0, 0 };

					for (uint32_t j = 0; j < 6 && i + j < words && i + j < 32; j++) {
						w[j] = message[i + j];
					}
					printf("adreno: %u:   %#x %#x %#x %#x %#x %#x\n", step, w[0], w[1], w[2], w[3], w[4], w[5]);
				}
			}
		}
		if (words == 0) {
			break;
		}
	}
	return answered;
}

/* A word of the GMU's own data, by the GMU's address for it; only what lies in memory of ours. */
static uint32_t
gmu_data(uint32_t address)
{
	for (unsigned i = 0; i < gmu_block_count; i++) {
		if (address - (uint32_t)gmu_blocks[i].address < gmu_blocks[i].size - 3) {
			vm_offset_t at = gmu_blocks[i].memory + (address - (uint32_t)gmu_blocks[i].address);

			to_memory(at, 4);               /* what the GMU wrote, not what our cache remembers */
			return *(volatile uint32_t *)at;
		}
	}
	return 0xdeadbeef;
}

/*
 * What can be seen of a GMU that has stopped answering: the tail of its log (16 bytes an event:
 * which, the crystal's counter, two values; the firmware's function at 0x1a98; where it writes
 * next is in pwr_col_cp_resp), its record of what it was asked for and what it has (firmware
 * 1.8's data at 0x108454: +0x28 is what is in flight, +0x40.. what is in force), its RPMh
 * channel, and the 3D core's switch and PLLs, which only the GMU's firmware writes.
 */
#define GMU_STATE_ADDRESS       0x108454

static void
gmu_post_mortem(void)
{
	uint32_t next = rd(gmu.base + (board->is_a7xx ? GEN7_GMU_GENERAL_9 : GMU_PWR_COL_CP_RESP)) & 0xffff, entries = GMU_LOG_SIZE / 16;
	const volatile uint32_t *log = (const volatile uint32_t *)gmu_log.memory;

	to_memory(gmu_log.memory, GMU_LOG_SIZE);
	printf("adreno: %u: the GMU's log (event, time, two values), %u events in, the last 40:\n", step, next);
	for (uint32_t i = next > 40 ? next - 40 : 0; i < next && i + 4 <= entries; i += 4) {
		printf("adreno: %u:   %x %x %x %x | %x %x %x %x | %x %x %x %x | %x %x %x %x\n", step,
		    log[4 * i], log[4 * i + 1], log[4 * i + 2], log[4 * i + 3],
		    log[4 * i + 4], log[4 * i + 5], log[4 * i + 6], log[4 * i + 7],
		    log[4 * i + 8], log[4 * i + 9], log[4 * i + 10], log[4 * i + 11],
		    log[4 * i + 12], log[4 * i + 13], log[4 * i + 14], log[4 * i + 15]);
	}
	for (uint32_t i = 0; i < 24; i += 8) {
		printf("adreno: %u: the GMU's state +%#x: %x %x %x %x %x %x %x %x\n", step, 4 * i,
		    gmu_data(GMU_STATE_ADDRESS + 4 * i), gmu_data(GMU_STATE_ADDRESS + 4 * i + 4),
		    gmu_data(GMU_STATE_ADDRESS + 4 * i + 8), gmu_data(GMU_STATE_ADDRESS + 4 * i + 12),
		    gmu_data(GMU_STATE_ADDRESS + 4 * i + 16), gmu_data(GMU_STATE_ADDRESS + 4 * i + 20),
		    gmu_data(GMU_STATE_ADDRESS + 4 * i + 24), gmu_data(GMU_STATE_ADDRESS + 4 * i + 28));
	}
	rscc_show();
	get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
	get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 multiplier", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
}

/* Send a message whose first word is yet to be made and wait for the GMU's verdict on it. */
static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
	volatile struct hfi_queue *queue = &hfi->queue[0];
	volatile uint32_t *data = (volatile uint32_t *)hfi_data[0];
	uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff, verdict = 0, waiting = 0;
	bool answered = false;

	message[0] = id | words << 8 | number << 20;
	printf("adreno: %u: about to send the GMU %s: message %u, %u words, number %u\n", step, what, id, words, number);
	snprintf(last_write, sizeof(last_write), "step %u sent the GMU message %u (%s)", step, id, what);
	hold(true);
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		data[at] = i < words ? message[i] : 0xfafafafa;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	queue->write = at;
	settle();
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
	for (int i = 0; i < 10000 && !answered; i++) {
		sleep_us(100);
		waiting = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
		if (waiting & 1) {
			wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
			answered = hfi_receive(1, number, &verdict);
		}
		if (rd(gmu.base + GMU_CM3_FW_INIT_RESULT) & GMU_FAILED) {
			break;
		}
	}
	hfi_receive(2, ~0u, &waiting);
	/* the vendor's check_ack_failure(): only 0xffffffff in the third word says the GMU refused */
	if (!answered || (hwsched_on() ? verdict == 0xffffffffu : verdict != 0)) {
		printf("adreno: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
		    answered ? "says no" : "does not answer", verdict, what, rd(gmu.base + GMU_CM3_FW_INIT_RESULT),
		    rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
		space_faulted(&gmu_space);
		if (!post_mortem_done) {
			post_mortem_done = true;
			gmu_post_mortem();
		}
		return false;
	}
	hfi_answer = verdict;
	printf("adreno: %u: the GMU accepts %s\n", step, what);
	return true;
}

/* Put a message in one of the GMU's queues and knock: `irq` is a bit of host2gmu_intr_set. */
static bool
hfi_write_queue(unsigned which, const uint32_t *message, uint32_t words, uint32_t irq)
{
	volatile struct hfi_queue *queue = &hfi->queue[which];
	volatile uint32_t *data = (volatile uint32_t *)hfi_data[which];
	uint32_t at = queue->write, aligned = (words + 3) / 4 * 4;
	/* the vendor's empty_space: a queue with read == write is empty, and has all of it to give */
	uint32_t room = at >= queue->read ? HFI_QUEUE_WORDS - (at - queue->read) : queue->read - at;

	if (queue->status == 0 || room <= aligned) {
		printf("adreno: %u: no room in the GMU's queue %u for %u words (%u free)\n", step, which, words, room);
		return false;
	}
	for (uint32_t i = 0; i < aligned; i++) {
		data[at] = i < words ? message[i] : 0xfafafafa;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	settle();
	queue->write = at;
	settle();
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, irq);
	return true;
}

/* A value of the GMU's firmware (the vendor's gen7_hwsched_hfi_get_value: three words, its answer the value). */
static uint32_t
hfi_value(uint32_t prop)
{
	uint32_t message[3] = { 0, prop, 0 };

	hfi_answer = 0xffffffffu;
	if (!hfi_send("a value of its firmware", HFI_GET_VALUE, message, 3)) {
		return 0xffffffffu;
	}
	return hfi_answer;
}

/* One of the GMU's features on or off (the vendor's hfi_feature_ctrl: four words, an answer). */
static bool
hfi_feature(const char *what, uint32_t feature, uint32_t enable)
{
	uint32_t message[4] = { 0, feature, enable, 0 };

	return hfi_send(what, HFI_FEATURE_CTRL, message, 4);
}

/*
 * A vote for a level of a supply: the level, its place in RPMh's list for the rail, and the place
 * of the level the memories' rail must have with it (vendor's setup_volt_dependency_tbl).
 */
static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels, uint32_t *vote)
{
	unsigned at, with = 0;

	for (at = 0; at < rail_levels && rail[at] < level; at++) {
	}
	if (at == rail_levels || mx_levels == 0) {
		return false;
	}
	while (with + 1 < mx_levels && mx[with] < rail[at]) {
		with++;
	}
	*vote = (uint32_t)rail[at] << 16 | with << 8 | at;
	return true;
}

/* dt: kgsl_3d0_reg_memory: the 3D core's registers; a6xx.xml counts in words */
static struct block gpu = { "gpu", 0x03d00000, 0x40000 };
#define GPU_WORD(n)             (4u * (n))
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define RBBM_CLOCK_CNTL         GPU_WORD(0x00ae)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define GMU_CORE_REQUEST        (1u << 30)      /* of host2gmu_intr_set: keep the 3D core up for us; bit 31: no more */
#define GMU_CORE_GRANTED        (1u << 31)      /* of gmu2host_intr_info */

static uint32_t core_khz;               /* the 3D core's speed, as asked of the GMU */

/*
 * What the vendor's driver sends at this point (a6xx_hfi_start; Linux the same with fewer
 * levels): the table of speeds of the 3D core with the supply level each needs -- the phone's
 * three lowest (dt: qcom,gpu-pwrlevels: 315, 450, 550 MHz at "levels" 64, 128, 192) -- and of the
 * GMU itself; the table of bus bandwidth votes, of which Linux gets by with the one for "off"
 * (a6xx_hfi.c: adreno_7c3_build_bw_table); "start"; and the speed wanted now. Then the request
 * to keep the 3D core powered while we use it (a6xx_gmu_oob_set), and its registers can be read.
 */
static bool
step_core_power(void)
{
	static const struct {
		uint32_t khz, level;
	} speeds_a6xx[] = { { 0, 0 }, { 315000, 64 }, { 450000, 128 }, { 550000, 192 } };
	static const struct {
		uint32_t khz, level;
	} speeds_a7xx[] = { { 0, 0 }, { 231000, 52 }, { 310000, 56 }, { 422000, 64 } };
	static struct hfi_perf_table perf;
	static struct hfi_table_perf table_perf;
	static struct hfi_bw_table bw;
	uint32_t core_start[2] = { 0, 0 }, start[1] = { 0 }, vote[4] = { 0, 1, 3, HFI_NO_INDEX }, granted = 0, speed = 0;
	uint16_t gfx[16], cx[16], mx[16];
	unsigned gfx_levels, cx_levels, mx_levels;
	uint32_t num_speeds = 4;

	if (!cx_ready() || !gmu_talks) {
		return false;
	}
	if (!rpmh_resource("gfx.lvl", gfx, &gfx_levels) || !rpmh_resource("cx.lvl", cx, &cx_levels) ||
	    !rpmh_resource("mx.lvl", mx, &mx_levels)) {
		printf("adreno: %u: RPMh's list lacks one of the rails\n", step);
		return false;
	}
	if (board->is_a7xx) {
		table_perf.version = 0;
		table_perf.type = HFI_TABLE_GPU_PERF;
		table_perf.gx.count = 4;
		table_perf.gx.stride = 3;
		for (unsigned i = 0; i < 4; i++) {
			table_perf.gx.levels[i].khz = speeds_a7xx[i].khz;
			table_perf.gx.levels[i].cx_vote = 0xffffffff;
			if (!rail_vote(speeds_a7xx[i].level, gfx, gfx_levels, mx, mx_levels, &table_perf.gx.levels[i].vote)) {
				printf("adreno: %u: RPMh has no level %u for the 3D core's rail\n", step, speeds_a7xx[i].level);
				return false;
			}
		}
		table_perf.cx.count = 2;
		table_perf.cx.stride = 2;
		table_perf.cx.levels[0].khz = 0;
		table_perf.cx.levels[1].khz = 200000;
		if (!rail_vote(0, cx, cx_levels, mx, mx_levels, &table_perf.cx.levels[0].vote) ||
		    !rail_vote(64, cx, cx_levels, mx, mx_levels, &table_perf.cx.levels[1].vote)) {
			return false;
		}
		printf("adreno: %u: votes: 3D core %#x %#x %#x %#x, GMU %#x %#x\n", step,
		    table_perf.gx.levels[0].vote, table_perf.gx.levels[1].vote,
		    table_perf.gx.levels[2].vote, table_perf.gx.levels[3].vote,
		    table_perf.cx.levels[0].vote, table_perf.cx.levels[1].vote);
	} else {
		perf.gpu_levels = sizeof(speeds_a6xx) / sizeof(speeds_a6xx[0]);
		for (unsigned i = 0; i < perf.gpu_levels; i++) {
			perf.gpu[i].khz = speeds_a6xx[i].khz;
			perf.gpu[i].acd = 0xffffffff;           /* no adaptive clocking */
			if (!rail_vote(speeds_a6xx[i].level, gfx, gfx_levels, mx, mx_levels, &perf.gpu[i].vote)) {
				printf("adreno: %u: RPMh has no level %u for the 3D core's rail\n", step, speeds_a6xx[i].level);
				return false;
			}
		}
		perf.gmu_levels = 2;
		perf.gmu[1].khz = 200000;
		if (!rail_vote(0, cx, cx_levels, mx, mx_levels, &perf.gmu[0].vote) ||
		    !rail_vote(48, cx, cx_levels, mx, mx_levels, &perf.gmu[1].vote)) {  /* "minimum SVS", the vendor's choice */
			return false;
		}
		printf("adreno: %u: votes: 3D core %#x %#x %#x %#x, GMU %#x %#x\n", step, perf.gpu[0].vote, perf.gpu[1].vote,
		    perf.gpu[2].vote, perf.gpu[3].vote, perf.gmu[0].vote, perf.gmu[1].vote);
	}

	bw.levels = 1;
	bw.ddr_commands = 3;
	bw.ddr_wait = 7;
	bw.ddr_address[0] = rpmh_resource("SH0", NULL, NULL);
	bw.ddr_address[1] = rpmh_resource("MC0", NULL, NULL);
	bw.ddr_address[2] = rpmh_resource("ACV", NULL, NULL);
	bw.ddr_data[0][0] = bw.ddr_data[0][1] = bw.ddr_data[0][2] = 0x40000000;
	bw.cnoc_commands = 1;
	bw.cnoc_wait = 1;
	bw.cnoc_address[0] = rpmh_resource("CN0", NULL, NULL);
	bw.cnoc_data[0][0] = 0x40000000;
	bw.cnoc_data[1][0] = 0x60000001;
	printf("adreno: %u: RPMh's bus resources: SH0 %#x, MC0 %#x, ACV %#x, CN0 %#x (Linux has 0x50004, 0x50000, 0x50088, "
	    "0x5006c)\n", step, bw.ddr_address[0], bw.ddr_address[1], bw.ddr_address[2], bw.cnoc_address[0]);
	if (bw.ddr_address[0] == 0 || bw.ddr_address[1] == 0 || bw.ddr_address[2] == 0 || bw.cnoc_address[0] == 0) {
		return false;
	}

	if (PE_parse_boot_argn("adreno_speed", &speed, sizeof(speed)) && speed >= 1 && speed < num_speeds) {
		vote[2] = speed;
	}
	core_khz = board->is_a7xx ? speeds_a7xx[vote[2]].khz : speeds_a6xx[vote[2]].khz;
	printf("adreno: %u: the speed wanted: %u MHz\n", step, core_khz / 1000);
	announce("send the GMU its tables (speeds and supply levels, bus votes), \"start\", and the speed wanted; "
	    "from here on it casts votes in RPMh by itself");
	if (board->is_a7xx) {
		if (!hfi_send("the table of speeds", HFI_TABLE_MSG, &table_perf.header, sizeof(table_perf) / 4)) {
			return false;
		}
	} else {
		if (!hfi_send("the table of speeds", HFI_PERF_TABLE, &perf.header, sizeof(perf) / 4)) {
			return false;
		}
	}
	if (!hfi_send("the table of bus votes", HFI_BW_TABLE, &bw.header, sizeof(bw) / 4)) {
		return false;
	}
	if (hwsched_on()) {
		/*
		 * What gen7_hwsched_hfi_start() sends after the tables, in its order: IFPC off, then the
		 * dispatcher on -- without it the dispatch queues are not taken --, then the profiler and DMS
		 * as this GPU's catalogue keeps them.
		 */
		/*
		 * adreno_hfi_ifpc=1 turns IFPC on instead of off. The client's own stream ends with
		 * CP_SET_AMBLE (0x55, "handles IFPC save/restore") and CP_SET_MARKER 0x400/0x800 just
		 * before its last CP_WAIT_FOR_IDLE, and ifpc_is_off at the firmware's level while the
		 * stream asks for a save/restore is the one combination that can leave that wait with
		 * nothing to wait for. Off is what the port that runs sends; this is the other side of it.
		 */
		{
			uint32_t ifpc_hfi = 0;

			(void)PE_parse_boot_argn("adreno_hfi_ifpc", &ifpc_hfi, sizeof(ifpc_hfi));
			if (!hfi_feature(ifpc_hfi ? "IFPC on" : "IFPC off", HFI_FEATURE_IFPC, ifpc_hfi) ||
			    !hfi_feature("the dispatcher", HFI_FEATURE_HWSCHED, 1) ||
			    !hfi_feature("the profiler", HFI_FEATURE_KPROF, 1)) {
				return false;
			}
		}
		/*
		 * The gen8 port turns preemption off before it starts the core (adreno8_hwsched_start:
		 * {"disable HWSCHED preemption", {2, 0, 0}}); this driver has never said it, so the
		 * firmware kept whatever it boots with. It is the one piece of the dispatcher's global
		 * state the port that runs sets and this one leaves alone.
		 */
		if (!hfi_feature("preemption off", HFI_FEATURE_PREEMPTION, 0)) {
			return false;
		}
		if (dms_on() && !hfi_feature("DMS", HFI_FEATURE_DMS, 1)) {
			return false;
		}
		hwsched_ctxq_value = hfi_value(HFI_VALUE_CONTEXT_QUEUE);
		printf("adreno: %u: the GMU's answer for context queues: %u (%#x)\n", step, hwsched_ctxq_value,
		    hwsched_ctxq_value);
		printf("adreno: %u: DMS is %s (adreno_dms)\n", step, dms_on() ? "on" : "off");
	}
	if (!hfi_send("\"start the core firmware\"", HFI_CORE_FW_START, core_start, 2)) {
		return false;
	}
	if (hwsched_on() && !hfi_feature("the AQE coprocessor", HFI_FEATURE_AQE, 1)) {
		return false;
	}
	if (!hfi_send("the speed wanted", HFI_GX_BW_PERF_VOTE, vote, 4)) {
		return false;
	}

	announce("ask the GMU to power the 3D core and keep it powered");
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
	put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, GMU_CORE_REQUEST);
	for (int i = 0; i < 5000 && !(granted & GMU_CORE_GRANTED); i++) {
		sleep_us(100);
		granted = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
	}
	core_is_on = true;                      /* asked for, at least: to be given back */
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, GMU_CORE_GRANTED);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	/*
	 * Take the 3D core out of secure mode here, as soon as it has power and before its command
	 * processor is started: the vendor writes this register after the processor is started (gen7_rb_start),
	 * and that write is not taken (step 21 shows it read back as it was). Step 21 keeps its own attempt.
	 */
	get(&gpu, "rbbm_secvid_trust_cntl, once the core has power", GPU_WORD(0xf400));
	put(&gpu, "rbbm_secvid_trust_cntl, nought, once the core has power", GPU_WORD(0xf400), 0);
	get(&gpu, "rbbm_secvid_trust_cntl, read back, once the core has power", GPU_WORD(0xf400));
	if (!(granted & GMU_CORE_GRANTED) || (rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS) & 0xc0) != 0) {
		printf("adreno: %u: the GMU does not power the 3D core (gmu2host_intr_info %#x, fw_init_result %#x)\n", step,
		    granted, rd(gmu.base + GMU_CM3_FW_INIT_RESULT));
		space_faulted(&gmu_space);
		return false;
	}
	if (!hfi_send("\"start\"", HFI_START, start, 1)) {
		return false;
	}
	if (!board->is_a7xx) {
		printf("adreno: %u: measured: the 3D core's clock runs at %u Hz (%u MHz was asked for), the GMU's in it at %u Hz\n", step,
		    measure_quietly(GPUCC_DEBUG_GX_GFX3D), core_khz / 1000, measure_quietly(GPUCC_DEBUG_GX_GMU));
	} else {
		printf("adreno: %u: the 3D core was asked for %u MHz\n", step, core_khz / 1000);
	}

	announce("read registers of the 3D core itself (0x3d00000): the first time it has power");
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	get(&gpu, "rbbm_clock_cntl", RBBM_CLOCK_CNTL);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	printf("adreno: %u: THE 3D CORE HAS POWER AND ANSWERS\n", step);
	return !space_faulted(&gmu_space);
}

/* ---- step 18: the 3D core's memory and its microcode -------------------------------------------------------- */

/*
 * The 3D core takes its orders as words in a ring of memory, which a small processor inside it
 * ("SQE", with microcode of its own: a660_sqe.fw) reads up to where a register says we have
 * written. Like the GMU it reaches memory through the SMMU: stream 0, to which this gives
 * context 0, with tables of ours that hold the ring, the microcode and later what is drawn into.
 * What is written to the core's registers before its processor is started, and in what order,
 * is Linux's a6xx_gpu.c hw_init for this GPU, less what only matters once it draws (clock gating,
 * the protection of registers against user programs' commands, counters).
 */
#define CORE_STREAM             0
#define CORE_MATCH              0
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)
#define PTE_READ_ONLY           (1ull << 7)     /* Linux gives the core its microcode so (MSM_BO_GPU_READONLY) */
#define CORE_RING_ADDRESS       0x01000000u
#define CORE_RING_SIZE          0x8000u         /* Linux's; 8192 words */
#define CORE_SQE_ADDRESS        0x01100000u
#define CORE_AQE_ADDRESS        0x01300000u     /* the BV coprocessor's microcode (gen70900_aqe.fw) */
#define CORE_PWRUP_ADDRESS      0x01310000u     /* the command processor's register init list (one page) */
#define CORE_SCRATCH_ADDRESS    0x01200000u
#define CORE_CTX_SAVE_ADDRESS   0x01400000u     /* where the processor saves a context: its own kernel gives it 4208 KB */
#define CORE_CTX_SECURE_ADDRESS 0x01c00000u     /* ... and where it saves a secure one */
#define CORE_CTX_SIZE           0x420000u
#define CORE_GUESS_ADDRESS      0x00007000u     /* the page of the address the processor keeps asking for, 0x7da0 */
#define CORE_GUESS_SIZE         0x1000u
#define CORE_TARGET_ADDRESS     0x10000000u
#define CORE_NOWHERE            0x7f000000u     /* never given anything */

/*
 * Where the client's own command buffer says its render target is. gpu.cpp finds that address
 * when it submits and names it here; the next submission then asks the command processor to
 * put a word there. If it can is the question: the target is client memory of the 3D core's
 * address space, and what the pipeline does with it has not landed yet.
 */
static uint64_t s_write_probe;

void
hakios_gpu_write_probe(uint64_t address)
{
	s_write_probe = address;
}

#define GBIF_QSB_SIDE0          (4 * 0x03)      /* of the bus interface (step 6) */
#define RBBM_GBIF_CLIENT_QOS_CNTL GPU_WORD(0x0011)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_AQE_INSTR_BASE_0     GPU_WORD(0x0b70) /* gen7_reg.h: GEN7_CP_AQE_INSTR_BASE_LO_0 (AQE0, the BV coprocessor) */
#define CP_AQE_INSTR_BASE_1     GPU_WORD(0x0b72) /* ... _LO_1 (AQE1, the LPAC one) */
#define CP_CHICKEN_DBG          GPU_WORD(0x0841)
#define CP_ADDR_MODE_CNTL       GPU_WORD(0x0842)
#define CP_APRIV_CNTL           GPU_WORD(0x0844)
#define CP_ROQ_THRESHOLDS_1     GPU_WORD(0x08c1)
#define CP_ROQ_THRESHOLDS_2     GPU_WORD(0x08c2)
#define CP_MEM_POOL_SIZE        GPU_WORD(0x08c3)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define CP_LPAC_PROG_FIFO_SIZE  GPU_WORD(0x0b34)
#define UCHE_WRITE_RANGE_MAX    GPU_WORD(0x0e05)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_GMEM_RANGE_MIN     GPU_WORD(0x0e0b) /* gen7_reg.h: UCHE_GMEM_RANGE_MIN_LO, then MIN_HI, MAX_LO, MAX_HI */
#define UCHE_DBG_CNTL_1         GPU_WORD(0x0e12)
#define A750_GMEM_BASE          (16u << 20)     /* ALIGN(gmem_size 3 MiB, uche_gmem_alignment 16 MiB) */
#define A750_GMEM_SIZE          (3u << 20)      /* the a750's table: gmem_size = 3 * SZ_1M */
#define UCHE_FILTER_CNTL        GPU_WORD(0x0e18)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define RB_DBG_ECO_CNTL         GPU_WORD(0x8e04)
#define PC_DBG_ECO_CNTL         GPU_WORD(0x9e00)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define RBBM_SECVID_TSB_ADDR_MODE_CNTL GPU_WORD(0xf810)
#define RBBM_SECVID_TRUST_CNTL  GPU_WORD(0xf400)
#define RBBM_SECVID_TSB_STATUS  GPU_WORD(0xfc00)        /* what TrustZone said about the zap shader */
#define CP_RB_CNTL_VALUE        (12u | 2u << 8 | 1u << 27)      /* 2^12 pairs of words; do not copy the read position to memory */

/* commands (Mesa's adreno_pm4.xml); a packet is a header word and so many more */
#define CP_NOP                  0x10
#define CP_WAIT_MEM_WRITES      0x12
#define CP_WAIT_FOR_ME          0x13
#define CP_WAIT_FOR_IDLE        0x26
#define CP_MEM_WRITE            0x3d
#define CP_EVENT_WRITE          0x46
#define CP_ME_INIT              0x48
#define CP_SET_MARKER           0x65
#define CP_SET_SECURE_MODE      0x66
#define CP_MEMCPY               0x75

/*
 * Linux (qcom_adreno_smmu_write_sctlr): after a fault the core's other requests are served, or it
 * hangs. And the core has a second stream, 1 (dt: gfx3d_lpac; Linux's sc7280.dtsi gives the GPU
 * both): once started, the microcode starts a second command processor ("LPAC"), which fetches
 * its own part of the microcode as that stream at once -- seen on the phone as a request for
 * 0x7e60 into the microcode, refused by an empty context given to stream 1 (adreno_stream1=2).
 * With nothing at all set up for stream 1 the phone resets without a word the moment the
 * command processor is started (adreno_stream1=0); a start with the microcode nowhere
 * (adreno_blind=1) does not get that far.
 */
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)
static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
/* adreno_stream1=2: the second stream gets a context of its own with nothing in it, to see what it asks for */
static struct gpu_space second_space = {
	.name = "the 3D core's second stream", .context = 2, .control = SMMU_SCTLR_SERVE_AFTER_FAULT,
};
#define SECOND_MATCH            2
static bool second_space_is_on, second_stream_is_on;
static struct gpu_block core_ring, core_sqe, core_aqe, core_pwrup, core_scratch, core_target, core_ctx_save,
    core_ctx_secure, core_guess;
static bool aqe_is_loaded, pwrup_is_loaded;

static const uint32_t pwrup_ifpc[] = {
	/* gen7_ifpc_pwrup_reglist */
	0xb604, 0xae02, 0x843, 0x84f, 0xb09, 0x850, 0x851, 0x852, 0x853, 0x854, 0x855, 0x856, 0x857, 0x858, 0x859, 0x85a,
	0x85b, 0x85c, 0x85d, 0x85e, 0x85f, 0x860, 0x861, 0x862, 0x863, 0x864, 0x865, 0x866, 0x867, 0x868, 0x869, 0x86a,
	0x86b, 0x86c, 0x86d, 0x86e, 0x86f, 0x870, 0x871, 0x872, 0x873, 0x874, 0x875, 0x876, 0x877, 0x878, 0x879, 0x87a,
	0x87b, 0x87c, 0x87d, 0x87e, 0x87f, 0x98d,
	/* gen7_9_x_ifpc_pwrup_reglist (the bicubic weights) */
	0xb609, 0xb60a, 0xb60b, 0xb60c,
};

static const uint32_t pwrup_preempt[] = {
	/* gen7_pwrup_reglist */
	0xe09, 0xe0a, 0xe07, 0xe08, 0xe0b, 0xe0c, 0xe0d, 0xe0e, 0xe17, 0xe01, 0x8e08, 0x8e28, 0x8602, 0x8e50, 0xe3a, 0xe19,
	0xb602,
};

/*
 * The list the command processor restores the 3D blocks' registers from, as the vendor builds it
 * (gen7_patch_pwrup_reglist): a cpu_gpu_lock of four words first -- two requests, the turn, and then the
 * lengths as u8 ifpc, u8 preemption, u16 dynamic -- and after it, for each register, its index and the
 * value the hardware holds now.
 */
static void
pwrup_list_build(void)
{
	volatile uint32_t *w = (volatile uint32_t *)core_pwrup.memory;
	uint32_t at = 4;
	uint32_t ifpc = sizeof(pwrup_ifpc) / sizeof(pwrup_ifpc[0]);
	uint32_t preempt = sizeof(pwrup_preempt) / sizeof(pwrup_preempt[0]);

	w[0] = 0;                                       /* gpu_req */
	w[1] = 0;                                       /* cpu_req */
	w[2] = 0;                                       /* turn */
	w[3] = ifpc | preempt << 8;                     /* u8 ifpc_list_len, u8 preemption_list_len, no triplets */
	for (uint32_t i = 0; i < ifpc; i++) {
		w[at++] = pwrup_ifpc[i];
		w[at++] = rd(gpu.base + 4 * pwrup_ifpc[i]);
	}
	for (uint32_t i = 0; i < preempt; i++) {
		w[at++] = pwrup_preempt[i];
		w[at++] = rd(gpu.base + 4 * pwrup_preempt[i]);
	}
	to_memory(core_pwrup.memory, core_pwrup.size);
	printf("adreno: %u: the processor's power up list at %#x: %u ifpc and %u preemption duplets, %u bytes\n", step,
	    CORE_PWRUP_ADDRESS, ifpc, preempt, 4 * at);
}

/*
 * For a test that has the core draw into the picture's page instead of a client's buffer: what
 * that page holds now, as this processor reads it (out of the cache first).
 */
void
hakios_gpu_target_words(uint32_t *words, uint32_t count)
{
	if (core_target.memory == NULL) {
		for (uint32_t i = 0; i < count; i++) {
			words[i] = 0xffffffffu;
		}
		return;
	}
	to_memory(core_target.memory, 64);
	for (uint32_t i = 0; i < count; i++) {
		words[i] = *(volatile uint32_t *)(core_target.memory + 4 * i);
	}
}
static bool core_space_is_on, core_runs, zap_is_loaded;
#define RING_WORDS              (CORE_RING_SIZE / 4)
static uint32_t ring_at;                /* where the next word goes: the ring goes round */
static uint32_t ring_sent;              /* up to where the core has been told to read */
static bool hwsched_run(const char *what);      /* ... through the GMU's queue, further down */

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_ring.memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

/* The header of a command with so many words after it. */
static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

/* A register of the 3D core (by its word number) written by command, in the order of the other commands. */
static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

/* What has been written to the ring since the last time: out to memory, and the core told how far to read. */
static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_ring.memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu.base + CP_RB_WPTR, ring_at);
}

/* Let the 3D core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	if (hwsched_on()) {
		return hwsched_run(what);
	}
	printf("adreno: %u: about to have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
	snprintf(last_write, sizeof(last_write), "step %u had the 3D core %s", step, what);
	hold(true);
	ring_send();
	for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
		sleep_us(100);
		taken = rd(gpu.base + CP_RB_RPTR);
		status = rd(gpu.base + RBBM_STATUS);
	}
	if (taken == ring_at && !(status & ~1u) && rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
		printf("adreno: %u: the 3D core has done it (ring read up to %u, status %#x)\n", step, taken, status);
		return true;
	}
	printf("adreno: %u: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, "
	    "command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken, ring_at, status,
	    rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS),
	    rd(gpu.base + CP_PROTECT_STATUS));
	space_faulted(&core_space);
	return false;
}

/* ---- the vendor's way: the GMU's dispatch queue, and the memory it asks for --------------------------------- */

static uint32_t hwsched_ts_high;        /* the highest timestamp the context has been given */

/*
 * Everything here is what adreno_gen7_hwsched_hfi.c does for a submission, in this driver's terms: the
 * GMU asks for memory (F2H_MSG_MEM_ALLOC on queue 1) and this answers on queue 0; it asks for a
 * performance counter and gets noughts; the context is registered (H2F_MSG_REGISTER_CONTEXT, then
 * H2F_MSG_CONTEXT_POINTERS); and work is handed over as one H2F_MSG_ISSUE_CMD on the first dispatch
 * queue, whose interrupt is BIT(2) of host2gmu_intr_set.
 */
struct hwsched_alloc {
	uint64_t gpu;                   /* the 3D core's address of it (0: it is the GMU's alone) */
	uint32_t gmu;                   /* the GMU's own address of it */
	uint32_t handle;                /* its handle, which the reply repeats */
	uint32_t kind;                  /* what it wanted it for */
	uint32_t size;
	vm_offset_t memory;
};

static struct hwsched_alloc hwsched_allocs[HWSCHED_ALLOCS];
static unsigned hwsched_alloc_count;
static uint64_t hwsched_gpu_at = HWSCHED_GPU_AT;
static uint32_t hwsched_gmu_at = (uint32_t)HWSCHED_GMU_AT;
static struct hwsched_alloc *hwsched_memstore;
static uint32_t hwsched_ts;             /* numbers what this driver submits itself */
static bool hwsched_context_is_registered;
static struct hwsched_alloc *hwsched_ctxq;      /* the context's own queue, if the GMU asked for one */
static uint32_t hwsched_ctxq_number;
static vm_offset_t hwsched_ctxq_base;   /* only this uncached CPU view may access the queue */

/* Report changed completion state, distinguishing host markers from GMU retirement. */
static void
hwsched_context_progress(const char *where)
{
    if (!hakios_gpu_debug()) return;
	static uint32_t last[7] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
	volatile uint32_t *head = (volatile uint32_t *)hwsched_ctxq_base;
	uint32_t values[7];
	bool changed = false;

	if (head == NULL) {
		return;
	}
	values[0] = head[5];
	values[1] = head[6];
	values[2] = head[3];
	values[3] = head[4];
	values[4] = hwsched_retired_ts;
	values[5] = values[6] = 0;
	if (hwsched_memstore != NULL) {
		/* The CPU only reads this allocation after hwsched_make() initialized it. */
		vm_offset_t sop = hwsched_memstore->memory + 8 * HWSCHED_CONTEXT;
		to_memory(sop, 12);
		values[5] = *(volatile uint32_t *)sop;
		values[6] = *(volatile uint32_t *)(sop + 8);
	}
	for (unsigned i = 0; i < 7; i++) {
		changed |= last[i] != values[i];
		last[i] = values[i];
	}
	if (!changed) {
		return;
	}
	printf("adreno: air73 progress %s: ctxq read %u write %u out %u sync %u; retired %u sop %u eop %u\n",
	    where, values[0], values[1], values[2], values[3], values[4], values[5], values[6]);
}

/*
 * Memory for the GMU, in both address spaces: its own for what its firmware reaches, the 3D core's for
 * what the command processor writes when it draws or notes a time. The vendor keeps one pool for both
 * (gen7_reserve_gmu_kernel_block); here each buffer is mapped where the request wants to see it.
 */
static struct hwsched_alloc *
hwsched_make(uint32_t size, uint32_t align, bool for_core, uint32_t kind)
{
	struct hwsched_alloc *alloc;
	uint32_t exponent = 12;                 /* a page at the least */
	uint32_t rounds, want = align & 0xff;   /* the GMU names its alignment as a power of two */

	if (hwsched_alloc_count == HWSCHED_ALLOCS) {
		printf("adreno: %u: the GMU has asked for memory %u times: no more\n", step, HWSCHED_ALLOCS);
		return NULL;
	}
	if (want > exponent && want < 21) {
		exponent = want;
	}
	rounds = (size + (1u << exponent) - 1) >> exponent << exponent;
	if (rounds > 0x800000) {
		printf("adreno: %u: the GMU asks for %#x bytes: more than this driver gives\n", step, size);
		return NULL;
	}
	if (hwsched_gmu_at + rounds > (uint32_t)HWSCHED_GMU_END ||
	    (for_core && hwsched_gpu_at + rounds > HWSCHED_GPU_END)) {
		printf("adreno: %u: these allocations no longer fit\n", step);
		return NULL;
	}
	alloc = &hwsched_allocs[hwsched_alloc_count];
	memset(alloc, 0, sizeof(*alloc));
	if (kmem_alloc(kernel_map, &alloc->memory, rounds, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	alloc->size = rounds;
	alloc->kind = kind;
	alloc->gmu = hwsched_gmu_at;
	alloc->gpu = for_core ? hwsched_gpu_at : 0;
	for (uint32_t offset = 0; offset < rounds; offset += 4096) {
		if (!space_map(&gmu_space, alloc->gmu + offset, kvtophys(alloc->memory + offset), PTE_GMU) ||
		    (for_core && !space_map(&core_space, alloc->gpu + offset, kvtophys(alloc->memory + offset),
		    PTE_CORE))) {
			printf("adreno: %u: cannot give the GMU %#x bytes at %#x\n", step, rounds, alloc->gmu);
			return NULL;
		}
	}
	to_memory(alloc->memory, rounds);
	space_commit(&gmu_space);
	if (for_core) {
		space_commit(&core_space);
	}
	hwsched_gmu_at += rounds;
	if (for_core) {
		hwsched_gpu_at += rounds;
	}
	hwsched_alloc_count++;
	printf("adreno: %u: the GMU has %#x bytes at its %#x (the 3D core's %#llx), kind %u\n", step, rounds,
	    alloc->gmu, alloc->gpu, kind);
	return alloc;
}

/* The buffer whose words the GMU writes a context's timestamps to (HFI_MEMKIND_MEMSTORE). */
static struct hwsched_alloc *
hwsched_make_memstore(void)
{
	if (hwsched_memstore == NULL) {
		hwsched_memstore = hwsched_make(4096, 0, true, HWSCHED_MEMSTORE_KIND);
	}
	return hwsched_memstore;
}

/*
 * The answer to the GMU's request for memory (mem_alloc_reply, process_mem_alloc): the descriptor's
 * words come back, with this driver's addresses and size in them, and host_mem_handle carrying the
 * GMU's handle -- which is how it knows the request succeeded.
 */
static bool
hwsched_mem_reply(const uint32_t *request, uint32_t words)
{
	uint32_t version = words > 1 ? request[1] : 0;
	uint32_t base = version > 0 ? 2 : 1;    /* hfi_mem_alloc_cmd: hdr, version, desc; legacy: hdr, reserved, desc */
	uint32_t reply[11] = { 0 };
	struct hwsched_alloc *alloc = NULL;
	uint32_t kind, handle, size, align;

	if (words < base + 9) {
		printf("adreno: %u: the GMU's request for memory is %u words: no answer\n", step, words);
		return false;
	}
	kind = request[base + 3];
	handle = request[base + 5];
	size = request[base + 7];               /* desc: gpu_addr 2 words, flags, kind, host, gmu, gmu_addr, size, align */
	align = request[base + 8];
	for (unsigned i = 0; i < hwsched_alloc_count; i++) {
		if (hwsched_allocs[i].kind == kind && hwsched_allocs[i].handle == handle) {
			alloc = &hwsched_allocs[i];     /* asked for twice: the same memory answers both */
		}
	}
	if (alloc == NULL) {
		alloc = kind == HWSCHED_MEMSTORE_KIND ? hwsched_make_memstore() :
		    hwsched_make(size, align, true, kind);
	}
	if (alloc == NULL) {
		printf("adreno: %u: cannot answer the GMU for %#x bytes (kind %u, handle %u)\n", step, size, kind, handle);
		return false;
	}
	alloc->handle = handle;
	reply[0] = HFI_MEM_ALLOC | 1u << 16 | 11u << 8 | (++hfi_number & 0xfff) << 20;
	reply[1] = request[0];                  /* the request this answers */
	reply[2] = (uint32_t)alloc->gpu;
	reply[3] = (uint32_t)(alloc->gpu >> 32);
	reply[4] = request[base + 2];           /* the flags it asked for */
	reply[5] = kind;
	reply[6] = handle;                      /* host_mem_handle: the GMU's own, its sign of success */
	reply[7] = handle;                      /* gmu_mem_handle */
	reply[8] = alloc->gmu;                  /* gmu_addr */
	reply[9] = alloc->size;                 /* size */
	reply[10] = align;
	printf("adreno: %u: the GMU asked for %#x bytes (kind %u, align %#x): it has its %#x, the 3D core's %#llx\n",
	    step, size, kind, align, alloc->gmu, alloc->gpu);
	return hfi_write_queue(0, reply, 11, 1);        /* on the command queue, interrupt bit 0 */
}

/* A counter the GMU asked for: two words of noughts say that this driver has none to give. */
static void
hwsched_cntr_reply(const uint32_t *request, uint32_t words)
{
	uint32_t reply[6] = { 0 };
	uint32_t group = words > 1 ? request[1] : 0, countable = words > 2 ? request[2] : 0;

	reply[0] = HFI_GMU_CNTR_REGISTER | 1u << 16 | 6u << 8 | (++hfi_number & 0xfff) << 20;
	reply[1] = request[0];
	reply[2] = group;
	reply[3] = countable;
	reply[4] = 0;                           /* cntr_lo: nought is "no counter came of this" */
	reply[5] = 0;                           /* cntr_hi */
	printf("adreno: %u: the GMU asks for counter %u of group %u: none\n", step, countable, group);
	hfi_write_queue(0, reply, 6, 1);
}

/*
 * The context the GMU is to hand work to: its number, the page table of the 3D core's address space
 * (with the SMMU's context number in its top bits, the way space_on() writes TTBR0), the bank it
 * translates in, and where the GMU notes its timestamps.
 */
static bool
hwsched_register_context(void)
{
	struct hwsched_alloc *store;
	uint64_t table = core_space.tables[0].phys | (uint64_t)core_space.context << 48;
	uint32_t message[10] = { 0 };

	if (hwsched_context_is_registered) {
		return true;
	}
	store = hwsched_make_memstore();
	if (store == NULL) {
		return false;
	}
	message[1] = HWSCHED_CONTEXT;
	message[2] = HFI_CTXT_FLAG_NOTIFY;
	message[3] = (uint32_t)table;
	message[4] = (uint32_t)(table >> 32);
	message[5] = HWSCHED_CONTEXT;           /* ctxt_idr */
	message[6] = HWSCHED_CTXT_BANK;
	if (!hfi_send("the context the GMU hands work to", HFI_REGISTER_CONTEXT, message, 7)) {
		return false;
	}
	/*
	 * adreno_ctxq=0: no context's queue, and every submission goes to the GMU as a raw command
	 * buffer (HFI 131), which is the one path the gen8 port that runs uses (adreno8_hwsched_raw).
	 * The queue path is where a run cut to two words -- a single CP_THREAD_CONTROL -- comes back
	 * idle the first time and leaves the core busy for good the second.
	 */
	{
		static int ctxq_wanted = -1;

		if (ctxq_wanted < 0) {
			uint32_t asked = 1;

			(void)PE_parse_boot_argn("adreno_ctxq", &asked, sizeof(asked));
			ctxq_wanted = asked != 0;
		}
		if (!ctxq_wanted && hwsched_ctxq_value == 1) {
			printf("adreno: %u: the context's queue is asked for and left alone (adreno_ctxq=0)\n", step);
			hwsched_ctxq_value = 0;
		}
	}
	if (hwsched_ctxq_value == 1 && hwsched_ctxq == NULL) {
		volatile uint32_t *head;
		struct gpu_block view = { 0 };
		struct hwsched_alloc *queue = hwsched_make(HWSCHED_CTXQ_SIZE, 0, false, 0xff);

		if (queue == NULL) {
			return false;
		}
		/*
		 * The GMU owns read_index while this CPU owns write_index in the same cache line.
		 * Cleaning a cached copy after updating write_index can restore an old read_index.
		 * hwsched_make() has cleaned and invalidated the zeroed allocation; from here on,
		 * access its contents only through the same uncached view used for the HFI queues.
		 */
		view.memory = queue->memory;
		view.size = HWSCHED_CTXQ_SIZE;
		hwsched_ctxq_base = block_uncached(&view);
		if (hwsched_ctxq_base == 0) {
			printf("adreno: air72 ctxq: cannot map the uncached CPU view\n");
			return false;
		}
		hwsched_ctxq = queue;
		head = (volatile uint32_t *)hwsched_ctxq_base;
		head[0] = 0;                                                    /* version */
		head[1] = hwsched_ctxq->gmu + 4 * HWSCHED_CTXQ_WORDS;           /* where its words begin */
		head[2] = HWSCHED_CTXQ_SIZE / 4 - HWSCHED_CTXQ_WORDS;           /* how many of them */
		__asm__ volatile ("dsb sy" : : : "memory");
		printf("adreno: air72 ctxq: uncached CPU view, GMU %#x, %u queue words\n",
		    hwsched_ctxq->gmu, head[2]);
		hwsched_context_progress("init");
	}
	message[1] = HWSCHED_CONTEXT;
	message[2] = (uint32_t)(store->gpu + 8 * HWSCHED_CONTEXT);              /* sop */
	message[3] = (uint32_t)((store->gpu + 8 * HWSCHED_CONTEXT) >> 32);
	message[4] = (uint32_t)(store->gpu + 8 * HWSCHED_CONTEXT + 8);          /* eop */
	message[5] = (uint32_t)((store->gpu + 8 * HWSCHED_CONTEXT + 8) >> 32);
	message[6] = 0;                         /* user_ctxt_record_addr: none */
	message[7] = 0;
	/*
	 * The version of this record. It has been nought with the context's queue attached, and the
	 * firmware's own account of the boot's submissions is where that shows: timestamps 1, 2 and 3
	 * are retired and 4 -- the first one after the queue was attached -- is not, nor is any of the
	 * client's; and the queue's read index stops at 36 from then on. Say 1 when a queue is given.
	 */
	message[8] = hwsched_ctxq != NULL ? 1 : 0;
	message[9] = hwsched_ctxq != NULL ? hwsched_ctxq->gmu : 0;      /* gmu_context_queue_addr */
	if (!hfi_send("where it notes the context's timestamps", HFI_CONTEXT_POINTERS, message, 10)) {
		return false;
	}
	hwsched_context_is_registered = true;
	return true;
}

/* A command packet's first word: type 7, so many words, the opcode, with the parity bits the ring has. */
static uint32_t
pm4_header7(uint32_t opcode, uint32_t words)
{
	return 7u << 28 | words | odd_parity(words) << 15 | opcode << 16 | odd_parity(opcode) << 23;
}

/*
 * Hand the command processor a buffer to run, as the vendor does to start it and to take it out of secure
 * mode (H2F_MSG_ISSUE_CMD_RAW, gen7_hwsched_coldboot_gpu's cp_init): one CP_INDIRECT_BUFFER naming the
 * buffer, so that the message stays five words however long the buffer is -- a message holds at most 255
 * words and a frame's buffer is longer.
 */
/* A submission into the context's own queue (gen7_gmu_context_queue_write). */
static bool
hwsched_context_submit(uint32_t address, uint32_t words, uint32_t ts)
{
	volatile uint32_t *head = (volatile uint32_t *)hwsched_ctxq_base;
	volatile uint32_t *data = (volatile uint32_t *)(hwsched_ctxq_base + 4 * HWSCHED_CTXQ_WORDS);
	uint32_t queue_words = head[2], at = head[6], read = head[5];
	uint32_t message_words = 11, aligned = (message_words + 3) / 4 * 4;
	uint32_t room = at >= read ? queue_words - (at - read) : read - at;
	uint32_t message[11];

	if (room <= aligned) {
		printf("adreno: %u: the context's queue has no room for %u words (%u of %u)\n", step, message_words,
		    room, queue_words);
		return false;
	}
	message[0] = HFI_ISSUE_CMD | message_words << 8 | (++hwsched_ctxq_number & 0xfff) << 20;
	message[1] = HWSCHED_CONTEXT;
	message[2] = HFI_CTXT_FLAG_NOTIFY;
	message[3] = ts;
	message[4] = 0;                         /* profile_gpuaddr_lo */
	message[5] = 0;
	message[6] = 1;                         /* one command buffer */
	message[7] = 0;                         /* big_ib_gmu_va */
	message[8] = address;
	message[9] = 0;
	message[10] = words * 4;                /* its length in bytes, as hfi_issue_ib counts it */
	for (uint32_t i = 0; i < aligned; i++) {
		data[at] = i < message_words ? message[i] : 0xfafafafa;
		at = (at + 1) % queue_words;
	}
	/* Publish the packet before its write index, then the index before the doorbell. */
	__asm__ volatile ("dsb sy" : : : "memory");
	head[6] = at;
	__asm__ volatile ("dsb sy" : : : "memory");
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, HWSCHED_DISPATCH_IRQ);
	hwsched_context_progress("submit");
	if (hakios_gpu_debug()) printf("adreno: %u: a dispatch at %u of %u words from %#x goes into the context's queue (read %u, ts %u)\n",
	    step, at, message_words, address, read, ts);
	return true;
}

static bool
hwsched_issue_raw(uint32_t address, uint32_t words, uint32_t ts)
{
	uint32_t message[5] = { 0 };

	if (words == 0 || words > RING_WORDS) {
		return false;
	}
	if (ts > hwsched_ts_high) {
		hwsched_ts_high = ts;
	}
	message[1] = pm4_header7(0x3f /* CP_INDIRECT_BUFFER */, 3);
	message[2] = address;
	message[3] = 0;
	message[4] = words;
	return hfi_send("the client's own command buffer, as it built it", HFI_ISSUE_CMD_RAW, message, 5);
}

static bool
hwsched_submit_window(uint32_t address, uint32_t words, uint32_t ts)
{
	uint32_t message[5] = { 0 };

	if (words == 0 || words > RING_WORDS) {
		return false;
	}
	if (ts > hwsched_ts_high) {
		hwsched_ts_high = ts;
	}
	message[1] = pm4_header7(0x3f /* CP_INDIRECT_BUFFER, named further down the file */, 3);
	message[2] = address;                   /* the buffer, in the 3D core's own addresses */
	message[3] = 0;
	message[4] = words;                     /* its length: CP_INDIRECT_BUFFER counts words */
	if (hwsched_ctxq != NULL) {
		return hwsched_context_submit(address, words, ts);
	}
	return hfi_send("the client's command buffer", HFI_ISSUE_CMD_RAW, message, 5);
}

/* Whatever the GMU has said, without printing it (a retired timestamp, an error already seen). */
static void
hwsched_drain(void)
{
	uint32_t verdict = 0;

	if (rd(gmu.base + GMU_GMU2HOST_INTR_INFO) & 1) {
		wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
	}
	hfi_receive(1, ~0u, &verdict);
	hfi_receive(2, ~0u, &verdict);
	hwsched_context_progress("drain");
}

/*
 * Let the 3D core have what is in the ring: one command buffer, with a write of a number that is new
 * each time at its end, so that the number's arrival is what says the core has done all of it. The
 * vendor's submissions are never waited for; this driver's steps are.
 */
static bool
hwsched_flush(void)
{
	uint32_t size, token;
	volatile uint32_t *at = (volatile uint32_t *)(core_scratch.memory + HWSCHED_TOKEN_AT);

	if (ring_at == 0) {
		return true;
	}
	token = ++hwsched_ts;
	*at = 0;
	to_memory((vm_offset_t)at, 4);
	ring_command(0x46 /* CP_EVENT_WRITE */, 4);
	ring(0x04 /* CACHE_FLUSH */ | 1u << 27 /* WRITE_ENABLED: without it the event writes nothing */);
	ring(CORE_SCRATCH_ADDRESS + HWSCHED_TOKEN_AT);
	ring(0);
	ring(token);
	size = 4 * ring_at;
	to_memory(core_ring.memory, 4 * RING_WORDS);
	ring_at = 0;
	ring_sent = 0;
	if (!hwsched_submit_window(CORE_RING_ADDRESS, size / 4, token)) {
		return false;
	}
	for (int i = 0; i < 20000; i++) {
		sleep_us(100);
		to_memory((vm_offset_t)at, 4);
		if (*at == token) {
			printf("adreno: %u: the 3D core has done it (the GMU's queue took %u words)\n", step, size / 4);
			return true;
		}
		if (i % 100 == 0) {
			hwsched_drain();
		}
		if (rd(gmu.base + GMU_CM3_FW_INIT_RESULT) & GMU_FAILED) {
			break;
		}
	}
	printf("adreno: %u: the 3D core does not get through the GMU's queue: the number did not arrive; command "
	    "processor: fault %#x, interrupts %#x, protection %#x\n", step, rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	space_faulted(&core_space);
	return false;
}

/* The ring's commands, given to the GMU instead of to the ring's registers. */
static bool
hwsched_run(const char *what)
{
	printf("adreno: %u: about to have the 3D core %s: %u words for the GMU's queue\n", step, what, ring_at);
	snprintf(last_write, sizeof(last_write), "step %u had the 3D core %s", step, what);
	hold(true);
	return hwsched_flush();
}

static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
	put(b, name, offset, (uint32_t)value);
	put(b, name, offset + 4, (uint32_t)(value >> 32));
}

static bool
step_core_space(void)
{
	const uint8_t *microcode;
	uint64_t size = 0, aqe_size = 0;
	uint32_t stream1 = 1;

	if (!cx_ready() || !core_is_on) {
		return false;
	}
	PE_parse_boot_argn("adreno_stream1", &stream1, sizeof(stream1));
	announce("make the 3D core's memory: its ring, its microcode, a page to try writing to");
	microcode = image_file("fw", board->sqe_fw_name, &size);
	if (microcode == NULL || size < 8 || size > 0x40000 ||
	    !block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE) ||
	    !block_new(&core_space, &core_ctx_save, CORE_CTX_SAVE_ADDRESS, CORE_CTX_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_ctx_secure, CORE_CTX_SECURE_ADDRESS, CORE_CTX_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_guess, CORE_GUESS_ADDRESS, CORE_GUESS_SIZE, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, microcode + 4, size - 4);       /* the file's first word is not microcode */
	to_memory(core_sqe.memory, core_sqe.size);
	if (board->is_a7xx && block_new(&core_space, &core_pwrup, CORE_PWRUP_ADDRESS, 4096, PTE_CORE)) {
		pwrup_is_loaded = true;
	}
	/*
	 * The BV coprocessor's microcode, beside the command processor's: the vendor programs its base
	 * (CP_AQE_INSTR_BASE_0, "AQE0 (BV coprocessor)") for every gen7 target, and this GPU's catalogue
	 * entry carries ADRENO_AQE. Left at nothing, the engine the 3D work is handed to has no microcode
	 * to run: a submission that reaches the pipeline then never comes back and faults nowhere.
	 */
	if (board->aqe_fw_name != NULL) {
		const uint8_t *aqe = image_file("fw", board->aqe_fw_name, &aqe_size);

		if (aqe == NULL || aqe_size < 8 || aqe_size > 0x40000 ||
		    !block_new(&core_space, &core_aqe, CORE_AQE_ADDRESS, (uint32_t)aqe_size - 4, PTE_CORE | PTE_READ_ONLY)) {
			printf("adreno: %u: no usable fw/%s: the BV coprocessor has no microcode\n", step,
			    board->aqe_fw_name);
		} else {
			memcpy((void *)core_aqe.memory, aqe + 4, aqe_size - 4);  /* the file's first word is not microcode */
			to_memory(core_aqe.memory, core_aqe.size);
			aqe_is_loaded = true;
			printf("adreno: %u: THE BV COPROCESSOR HAS MICROCODE: %llu bytes at %#x\n", step, aqe_size - 4,
			    CORE_AQE_ADDRESS);
		}
	}

	announce("give the SMMU's context 0 these tables and send the 3D core's requests (stream 0) through it");
	core_space_is_on = true;
	if (!space_on(&core_space, CORE_MATCH, CORE_STREAM)) {
		return false;
	}
	if (stream1 == 2) {
		if ((second_space.root = space_new_table(&second_space)) == NULL) {
			return false;
		}
		second_space_is_on = true;
		if (!space_on(&second_space, SECOND_MATCH, 1)) {
			return false;
		}
	} else if (stream1 == 1) {
		second_stream_is_on = true;
		if (!space_stream(&core_space, SECOND_MATCH, 1)) {
			return false;
		}
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	return !space_faulted(&core_space);
}

/* ---- step 19: the zap shader, loaded -------------------------------------------------------------------- */

/*
 * The rest of what Linux does with the zap shader (mdt_loader.c: qcom_mdt_pas_init,
 * qcom_mdt_load; adreno_gpu.c: zap_shader_load_mdt): after the header (step 12), TrustZone is
 * told where the image will be -- the two pages the device tree reserves for it
 * (reserved-memory gpu_micro_code@8b71a000, the zap-shader node's memory-region) --, the image's
 * one segment (the file a660_zap.b02, relocatable: at the start of that memory) is put there,
 * and TrustZone is asked to authenticate it and run it. The vendor's driver does this before it
 * starts the 3D core's command processor (a6xx_rb_start: a6xx_microcode_load), Linux after;
 * this phone's own order is kept. Once that is done the memory is TrustZone's, and nothing
 * here can see whether it is so; to write there again would be the end of the run. Our kernels
 * come and go without the phone restarting, so a kernel leaves no zap shader behind: when it
 * gives the GPU back it has TrustZone take the shader away again (gpu_block_stop(); the
 * vendor's driver does the same when it is unloaded: kgsl_zap_shader_unload), and the next one
 * loads it as if the phone had just started. A kernel that did not end in order cannot have
 * done that. So the same request is made here first: TrustZone answers it with success whether
 * it had a shader or not, and hands the memory back cleared (it then reads as zeroes). If it
 * refuses the header or the place, this kernel has not loaded a shader, whatever TrustZone keeps
 * from before: the vendor's gen7_rb_start() is in the same position when its load fails, and it
 * then releases secure mode by writing RBBM_SECVID_TRUST_CNTL instead of by a command. Step 21
 * does that, and does the command only when the shader was loaded. (There was a boot-arg
 * adreno_zap=0 for the second and later runs of a started phone; nothing reads it now.)
 */
#define ZAP_MEMORY              0x8b71a000ull
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_THREE_VALUES        3

/* TrustZone is to give the zap shader up, and its memory with it. */
static bool
zap_unload(void)
{
	uint64_t answer = ~0ull;

	return scm("to give up the zap shader", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer) == 0 &&
	       answer == 0;
}

static bool
step_zap_load(void)
{
	const uint8_t *segment;
	volatile uint8_t *memory;
	uint64_t size = 0, header = 0, answer = ~0ull;

	if (!cx_ready() || !core_is_on) {
		return false;
	}
	announce("ask TrustZone to give up a zap shader that a kernel before this one may have left");
	printf("adreno: %u: %s\n", step, zap_unload() ? "TrustZone has given up what it had (it says so with none, too)" :
	    "TRUSTZONE DOES NOT GIVE IT UP: the next answers tell whether it keeps one");
	announce("hand the zap shader's header to TrustZone, tell it where the image goes, put the image there, and ask "
	    "TrustZone to authenticate and run it");
	if (!zap_header(&header)) {
		return false;
	}
	if (zap_combined) {
		/* the phone's image holds its own code: the segment is inside the file just handed over */
		segment = zap_file + zap_segment_from;
		if (zap_segment_size == 0 || zap_segment_from + zap_segment_size > zap_file_size) {
			printf("adreno: %u: the image names no code segment inside itself (%u at %#x of %llu)\n", step,
			    zap_segment_size, zap_segment_from, zap_file_size);
			return false;
		}
		size = zap_segment_size;
		printf("adreno: %u: the code is inside the image: %llu bytes at its %#x\n", step, size, zap_segment_from);
	} else {
		segment = image_file("fw", board->zap_b02_name, &size);
		if (segment == NULL || size == 0 || size > 0x1000) {
			return false;
		}
	}
	/*
	 * The image is a buffer argument: its physical address and, in the next register, its length. Both were
	 * handed over before with the length left at nothing, and TrustZone refused the image (0x300045 with the
	 * buffer shape, -10 when it was given two plain numbers instead).
	 */
	printf("adreno: %u: the metadata is %llu bytes at %#llx: the %llu-byte file's header and its hashes, one after the other\n",
	    step, zap_metadata_size, header, zap_file_size);
	if (scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER,
	    PAS_GPU, header, zap_metadata_size, &answer) != 0 || answer != 0) {
		/* vendor kgsl_zap_shader_load() fails here, and gen7_rb_start() then has zap_loaded
		 * false: no shader is ours, so step 21 writes the register instead of commanding the
		 * core. TrustZone may well have one from before; either way it is not ours to use. */
		printf("adreno: %u: TrustZone does not take the signature (%#llx): NO ZAP SHADER IS LOADED; step 21 is the "
		    "vendor's fallback, the register alone\n", step, answer);
		return true;
	}
	if (scm("where the image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, board->zap_memory,
	    (zap_image_size + 0xfffu) & ~0xfffull,
	    &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not take the place (%#llx): NO ZAP SHADER IS LOADED; step 21 is the "
		    "vendor's fallback, the register alone\n", step, answer);
		return true;
	}
	/* (the memory is looked at only now: TrustZone has just said that it is not its own) */
	memory = (volatile uint8_t *)ml_io_map_wcomb(board->zap_memory, ZAP_MEMORY_SIZE);
	printf("adreno: %u: the reserved memory begins %02x %02x %02x %02x, the zap shader %02x %02x %02x %02x\n", step,
	    memory[0], memory[1], memory[2], memory[3], segment[0], segment[1], segment[2], segment[3]);
	printf("adreno: %u: about to write %llu bytes at %#llx\n", step, size, board->zap_memory);
	printf("adreno: %u: the segment goes at %#llx, its place in the image being %#x (the image is %#x bytes)\n", step,
	    board->zap_memory + zap_segment_at, zap_segment_at, zap_image_size);
	snprintf(last_write, sizeof(last_write), "step %u wrote the zap shader into memory at %#llx", step,
	    board->zap_memory + zap_segment_at);
	hold(true);
	for (uint64_t i = 0; i < ZAP_MEMORY_SIZE; i++) {
		memory[i] = 0;
	}
	for (uint64_t i = 0; i < size; i++) {
		memory[zap_segment_at + i] = segment[i];
	}
	settle();
	if (scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU, 0,
	    0, &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
		return false;
	}
	printf("adreno: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
	zap_is_loaded = true;
	return true;
}

/* ---- step 20: the 3D core's registers, its command processor started, its first command ------------------- */

static bool
step_core_start(void)
{
	static const uint32_t address_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0x0e00, 0xae01,
		                                  0xb601, 0xf810 };
	uint32_t blind = 0;

	if (!cx_ready() || !core_is_on || !core_space_is_on) {
		return false;
	}
	announce("write the 3D core's registers as Linux does before it starts the command processor: the first writes "
	    "to the core itself");
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
	put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
	{
		/*
		 * The window the 3D pipeline's accesses are held to, as the vendor names it (gen7_hw_init ->
		 * _set_secvid): the IOMMU's secure aperture, not nought. kgsl gives the GPU 256 MB at
		 * 0xf0000000 when it walks a 32-bit address space, and 0x100000000 for 0xefffff00 bytes when it
		 * walks a 64-bit one. Noughts leave the pipeline with no window at all.
		 */
		uint32_t secvid = 1;

		PE_parse_boot_argn("adreno_secvid", &secvid, sizeof(secvid));
		if (secvid == 1) {
			put64(&gpu, "rbbm_secvid_tsb_trusted_base, the secure aperture", RBBM_SECVID_TSB_TRUSTED_BASE, 0xf0000000ull);
			put(&gpu, "rbbm_secvid_tsb_trusted_size, the secure aperture", RBBM_SECVID_TSB_TRUSTED_SIZE,
			    (0x10000000u / 0x1000u) << 12);
		} else if (secvid == 2) {
			put64(&gpu, "rbbm_secvid_tsb_trusted_base, the secure aperture, 64 bits", RBBM_SECVID_TSB_TRUSTED_BASE,
			    0x100000000ull);
			put(&gpu, "rbbm_secvid_tsb_trusted_size, the secure aperture, 64 bits", RBBM_SECVID_TSB_TRUSTED_SIZE,
			    (0xefffff00u / 0x1000u) << 12);
		} else {
			put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
			put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
		}
		get(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE);
		get(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE);
		get(&gpu, "rbbm_secvid_tsb_status", RBBM_SECVID_TSB_STATUS);
		get(&gpu, "rbbm_secvid_tsb_status, high", RBBM_SECVID_TSB_STATUS + 4);
	}
	if (!board->is_a7xx) {
		for (unsigned i = 0; i < sizeof(address_modes) / sizeof(address_modes[0]); i++) {
			put(&gpu, "address mode: 64 bits", GPU_WORD(address_modes[i]), 1);
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		put(&gbif, "qsb_side", GBIF_QSB_SIDE0 + 4 * i, 0x00071620);
	}
	if (board->is_a7xx) {
		put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 0x02120212);
	} else {
		put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 3);
	}
	put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	if (!board->is_a7xx) {
		put64(&gpu, "uche_write_range_max", UCHE_WRITE_RANGE_MAX, 0x0001ffffffffffc0ull);
	}
	put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, 0x0001fffffffff000ull);
	put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, 0x0001fffffffff000ull);
	if (board->is_a7xx) {
		/*
		 * The window in the core's address space that is the GPU's own on-chip memory, as the vendor
		 * gives it: its size rounded up to the alignment this GPU's table names (3 MiB with a 16 MiB
		 * alignment, so it begins at 0x01000000), and its end. Until now no window was named at all.
		 */
		put64(&gpu, "uche_gmem_range_min", UCHE_GMEM_RANGE_MIN, A750_GMEM_BASE);
		put64(&gpu, "uche_gmem_range_max", UCHE_GMEM_RANGE_MIN + 8, A750_GMEM_BASE + A750_GMEM_SIZE - 1);
		get(&gpu, "uche_gmem_range_min", UCHE_GMEM_RANGE_MIN);
		get(&gpu, "uche_gmem_range_max", UCHE_GMEM_RANGE_MIN + 8);
		/*
		 * The vendor's own lock-up fix: "Disable LPAC hard sync event to fix lock up issue when
		 * BR/BV event fifo is full" (the a750 has LPAC). Left as it is, the BR/BV event path can
		 * fill up and stop, which is what a draw that never finishes and never faults looks like.
		 */
		put(&gpu, "uche_dbg_cntl_1, the LPAC hard sync switched off", UCHE_DBG_CNTL_1,
		    rd(gpu.base + UCHE_DBG_CNTL_1) | (1u << 30));
		get(&gpu, "uche_dbg_cntl_1", UCHE_DBG_CNTL_1);
	}
	if (board->is_a7xx) {
		put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 0x800000);
	} else {
		put(&gpu, "uche_filter_cntl", UCHE_FILTER_CNTL, 0x804);
		put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 0x4);
		put(&gpu, "cp_roq_thresholds_2", CP_ROQ_THRESHOLDS_2, 0x02000140);
		put(&gpu, "cp_roq_thresholds_1", CP_ROQ_THRESHOLDS_1, 0x8040362c);
		put(&gpu, "cp_lpac_prog_fifo_size", CP_LPAC_PROG_FIFO_SIZE, 0x20);
		put(&gpu, "cp_mem_pool_size", CP_MEM_POOL_SIZE, 128);
		put(&gpu, "pc_dbg_eco_cntl", PC_DBG_ECO_CNTL, 0x00200200);
	}
	put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
	/*
	 * How textures in the GPU's own tiled and compressed layouts are laid out in memory (Linux's
	 * a6xx_calc_ubwc_config, a6xx_set_ubwc_config for this GPU: highest bank bit 14, and the
	 * rest as below); Mesa is told the same (hakios_gpu_parameter()).
	 */
	if (board->is_a7xx) {
		put(&gpu, "rb_nc_mode_cntl", GPU_WORD(0x8e08), (1u << 11) | (1u << 4) | (3u << 1));
		put(&gpu, "tpl1_nc_mode_cntl", GPU_WORD(0xb604), (3u << 1));
		put(&gpu, "sp_nc_mode_cntl", GPU_WORD(0xae02), (2u << 4) | (3u << 1));
		put(&gpu, "uche_mode_cntl", GPU_WORD(0x0e01), (3u << 21));
	} else {
		put(&gpu, "rb_nc_mode_cntl", GPU_WORD(0x8e08), 1u << 11 | 1u << 4 | 1u << 1);
		put(&gpu, "tpl1_nc_mode_cntl", GPU_WORD(0xb604), 1u << 1);
		put(&gpu, "sp_nc_mode_cntl", GPU_WORD(0xae02), 2u << 4 | 1u << 1);
		put(&gpu, "uche_mode_cntl", GPU_WORD(0x0e01), 1u << 21);
	}
	put(&gpu, "rbbm_nc_mode_cntl", GPU_WORD(0x0534), 1);
	put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
	if (board->is_a7xx) {
		put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0xcfffff);
	} else {
		put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x1fffff);
	}
	put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
	if (!board->is_a7xx) {
		/* (gen7.9 is left alone here: the vendor writes CP_CHICKEN_DBG 1 only for gen7.0/7.2/7.4/7.11) */
		put(&gpu, "cp_chicken_dbg", CP_CHICKEN_DBG, 1);
		put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 0);
	}
	put(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL, 1u << 6 | 1u << 5 | 1u << 3 | 1u << 2 | 1u << 1 | (board->is_a7xx ? 1u : 0));
	if (!board->is_a7xx) {
		put(&gpu, "rb_dbg_eco_cntl", RB_DBG_ECO_CNTL, 0x04100000);     /* Mesa's value for this GPU */
	} else {
		/*
		 * What the vendor's gen7 init writes and the a6xx one does not: the UCHE's way to the
		 * bus and the length of its command queue's, the rasterizer's bank bit, the two eco
		 * settings gen7.9 has, and the two command processors a7xx has besides the one that
		 * draws -- the binning one (BV) and LPAC -- without whose admission nothing they are
		 * handed is run.
		 */
		put(&gpu, "uche_gbif_gx_config", GPU_WORD(0x0e3a), 0x10240e0);
		put(&gpu, "uche_cmdq_config", GPU_WORD(0x0e3c), 0x6690e);
		/*
		 * gen7.9 alone has bits 31:29 of GBIF_CX_CONFIG set to 1 (the vendor's gen7_start):
		 * the readback is with the others below.
		 */
		put(&gpu, "gbif_cx_config, the gen7.9 setting", GPU_WORD(0x3c00),
		    (rd(gpu.base + GPU_WORD(0x3c00)) & ~(7u << 29)) | (1u << 29));
		/* (the phone's GPU table: highest_bank_bit 16, so that field is 16 - 13 = 3, not 1) */
		put(&gpu, "gras_nc_mode_cntl", GPU_WORD(0x8602), 3u << 5);
		put(&gpu, "tpl1_dbg_eco_cntl1", GPU_WORD(0xb602), 0xc0700);
		put(&gpu, "rb_cmp_dbg_eco_cntl", GPU_WORD(0x8e28), rd(gpu.base + GPU_WORD(0x8e28)) | (1u << 19));
		put(&gpu, "cp_bv_apriv_cntl", GPU_WORD(0x0ad0), 0xf);
		put(&gpu, "cp_lpac_apriv_cntl", GPU_WORD(0x0b31), 0xf);
		/*
		 * Read back what is in the 3D core's own power domain (not the ones beside the bus):
		 * what a write there came back as says whether those blocks are powered and clocked at
		 * all, which is the difference between a core that ignores commands and a core that
		 * does not see them.
		 */
		get(&gpu, "rb_nc_mode_cntl, read back", GPU_WORD(0x8e08));
		get(&gpu, "gras_nc_mode_cntl, read back", GPU_WORD(0x8602));
		get(&gpu, "tpl1_dbg_eco_cntl1, read back", GPU_WORD(0xb602));
		get(&gpu, "uche_gbif_gx_config, read back", GPU_WORD(0x0e3a));
		/*
		 * What the clock gating is set to, before anything is written to it: the vendor's gen7 init
		 * turns hardware clock gating on at the end (gen7_hwcg_set) with the comment that leaving
		 * it off is what causes register accesses to fail. These five say what state it is in.
		 */
		get(&gpu, "rbbm_clock_cntl", GPU_WORD(0x0ae));
		get(&gpu, "rbbm_clock_cntl_global", GPU_WORD(0x0ad));
		get(&gpu, "rbbm_cgc_global_load_cmd", GPU_WORD(0x11e));
		get(&gpu, "rbbm_cgc_p2s_status", GPU_WORD(0x122));
		get(&gpu, "gbif_cx_config", GPU_WORD(0x3c00));
	}
	/*
	 * adreno_blind=1: the command processor is started with its microcode at an address where
	 * nothing is. All it can do then is ask the SMMU for it, and the SMMU's record of the refusal
	 * shows that its requests come through context 0 as meant, and what they look like.
	 */
	/*
	 * adreno_hwcg=1: what the vendor's gen7 init does last (gen7_hwcg_set) and this port never
	 * has -- turn the hardware clock gating on, in the vendor's order, whose comment says that
	 * leaving it is what makes register accesses fail. Off unless asked for: a run without the
	 * argument is the same run as before.
	 */
	{
		int hwcg = 0;

		PE_parse_boot_argn("adreno_hwcg", &hwcg, sizeof(hwcg));
		if (hwcg) {
			put(&gpu, "gbif_cx_config", GPU_WORD(0x3c00), (rd(gpu.base + GPU_WORD(0x3c00)) & ~(7u << 29)) | (1u << 29));
			put(&gpu, "rbbm_clock_cntl_global, one", GPU_WORD(0x0ad), 1);
			put(&gpu, "rbbm_cgc_global_load_cmd", GPU_WORD(0x11e), 1);
			put(&gpu, "rbbm_cgc_p2s_trig_cmd", GPU_WORD(0x11f), 1);
			for (unsigned k = 0; k < 1000 && (rd(gpu.base + GPU_WORD(0x122)) & 1) == 0; k++) {
				sleep_us(1);
			}
			get(&gpu, "rbbm_cgc_p2s_status, after the trigger", GPU_WORD(0x122));
			put(&gpu, "rbbm_clock_cntl_global, nought", GPU_WORD(0x0ad), 0);
		}
	}
	PE_parse_boot_argn("adreno_blind", &blind, sizeof(blind));
	put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, blind ? CORE_NOWHERE : CORE_SQE_ADDRESS);
	if (aqe_is_loaded) {
		put64(&gpu, "cp_aqe_instr_base_0, the BV coprocessor", CP_AQE_INSTR_BASE_0, CORE_AQE_ADDRESS);
		put64(&gpu, "cp_aqe_instr_base_1, the LPAC one", CP_AQE_INSTR_BASE_1, CORE_AQE_ADDRESS);
		get(&gpu, "cp_aqe_instr_base_0", CP_AQE_INSTR_BASE_0);
	}
	if (!hwsched_on()) {
		put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
		put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
	} else {
		printf("adreno: %u: the ring registers are the GMU's to keep (gpu_hwsched=1): the work goes through "
		    "its queue\n", step);
	}
	get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "cp_rb_cntl", CP_RB_CNTL);
	get(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL);
	get(&gpu, "rbbm_status", RBBM_STATUS);

	announce(blind ? "start the 3D core's command processor with its microcode nowhere" :
	    "start the 3D core's command processor");
	core_runs = true;
	if (!hwsched_on()) {
		put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);        /* the GMU starts it in hwsched's way */
		sleep_ms(20);
	}
	printf("adreno: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
	    "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + CP_RB_RPTR));
	get(&smmu, "sgfsr", SMMU_SGFSR);
	get(&smmu, "sgfsynr0", SMMU_SGFSR + 8);
	get(&smmu, "sgfsynr1", SMMU_SGFSR + 12);
	if (blind) {
		return space_faulted(&core_space);
	}
	if (second_space_is_on) {
		sleep_ms(200);
		return space_faulted(&second_space) | !space_faulted(&core_space);
	}
	if (space_faulted(&core_space)) {
		return false;
	}

	announce("give the 3D core its first command, \"initialize\"");
	if (hwsched_on()) {
		if (pwrup_is_loaded) {
			pwrup_list_build();
		}
		/*
		 * The same ten words, as one message to the GMU instead of into a ring (vendor's cp_init:
		 * H2F_MSG_ISSUE_CMD_RAW carrying gen7_cp_init_cmds' CP_THREAD_CONTROL and CP_ME_INIT), and
		 * then the context the GMU is to hand work to. No CP_SET_PSEUDO_REGISTER and no
		 * CP_CONTEXT_SWITCH_YIELD: the GMU keeps those for itself (the vendor sends neither).
		 */
		uint32_t message[11] = { 0 };

		message[1] = pm4_header7(0x17 /* CP_THREAD_CONTROL */, 1);
		message[2] = 1u << 27;
		message[3] = pm4_header7(CP_ME_INIT, 7);
		uint32_t mask = (pwrup_is_loaded ? 0x14b : 0x84b) | (dms_on() ? 0 : 1u << 11 /* "disable DMS" */);

		message[4] = mask;
		message[5] = 0x00000003;                /* several hardware contexts */
		message[6] = 0x20000000;                /* error detection */
		message[7] = 0x00000002;                /* operation mode mask */
		message[8] = pwrup_is_loaded ? CORE_PWRUP_ADDRESS : 0;
		message[9] = 0;
		message[10] = pwrup_is_loaded ? 1u << 31 : 0;
		printf("adreno: %u: the processor is told mask %#x, %s list at %#x, and the vendor's last word %#x\n",
		    step, mask, pwrup_is_loaded ? "a register init" : "no",
		    pwrup_is_loaded ? CORE_PWRUP_ADDRESS : 0, pwrup_is_loaded ? 1u << 31 : 0u);
		if (!hfi_send("the processor's first command", HFI_ISSUE_CMD_RAW, message, 11) ||
		    !hwsched_register_context()) {
			return false;
		}
		printf("adreno: %u: THE 3D CORE TAKES COMMANDS\n", step);
		return true;
	}
	if (board->is_a7xx) {
		if (pwrup_is_loaded) {
			pwrup_list_build();
		}
		/*
		 * The vendor's mask (gen7_cp_init_cmds): BIT(8) turns the register init list with its spinlock
		 * on, and BIT(11) -- "disable DMS" -- is left out because this GPU's table has ADRENO_DMS ("DMS
		 * is enabled from the CP side by default; disable it if not supported"). Until now the two bits
		 * were the other way round and the list was a null address.
		 */
		printf("adreno: %u: the processor is told mask %#x, %s list at %#x, and the vendor's last word %#x\n", step,
		    pwrup_is_loaded ? 0x14b : 0x84b, pwrup_is_loaded ? "a register init" : "no",
		    pwrup_is_loaded ? CORE_PWRUP_ADDRESS : 0, pwrup_is_loaded ? 1u << 31 : 0u);
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(1u << 27);
		ring_command(CP_ME_INIT, 7);
		ring(pwrup_is_loaded ? 0x14b : 0x84b);
		ring(0x00000003);                       /* several hardware contexts */
		ring(0x20000000);                       /* error detection */
		ring(0x00000002);
		ring(pwrup_is_loaded ? CORE_PWRUP_ADDRESS : 0);
		ring(0);
		ring(pwrup_is_loaded ? 1u << 31 : 0);
	} else {
		ring_command(CP_ME_INIT, 8);            /* a6xx_cp_init */
		ring(0x0000002f);
		ring(0x00000003);                       /* several hardware contexts */
		ring(0x20000000);                       /* error detection */
		ring(0);
		ring(0);
		ring(0);
		ring(0);
		ring(0);
	}
	if (!ring_run("initialize its command processor")) {
		return false;
	}
	/*
	 * Where the processor saves a context, and a secure one (the vendor's gen7_post_start: CP_SET_PSEUDO_REGISTER
	 * with PRIV_NON_SECURE_SAVE_ADDR and PRIV_SECURE_SAVE_ADDR). It has to be told before it is given anything to
	 * hand to the 3D pipeline: without it, every such hand-off left an SMMU record of a refused read at IOVA
	 * 0x7da0 and the pipeline never ran.
	 */
	ring_command(0x56 /* CP_SET_PSEUDO_REGISTER */, 6);
	ring(1);                                        /* PRIV_NON_SECURE_SAVE_ADDR */
	ring(CORE_CTX_SAVE_ADDRESS);
	ring(0);
	ring(2);                                        /* PRIV_SECURE_SAVE_ADDR */
	ring(CORE_CTX_SECURE_ADDRESS);
	ring(0);
	if (!ring_run("tell it where it saves a context")) {
		return false;
	}
	/*
	 * One context switch, so that the processor has a context of its own rather than the one it came out of
	 * reset with (the vendor's gen7_post_start: CP_CONTEXT_SWITCH_YIELD with four words, then it waits for the
	 * processor to rest).
	 */
	ring_command(0x6b /* CP_CONTEXT_SWITCH_YIELD */, 4);
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	if (!ring_run("switch to a context of its own")) {
		printf("adreno: %u: the processor did not come to rest after a context switch\n", step);
		return false;
	}
	printf("adreno: %u: THE PROCESSOR HAS A CONTEXT OF ITS OWN\n", step);
	printf("adreno: %u: the processor saves a context at %#x and a secure one at %#x\n", step,
	    CORE_CTX_SAVE_ADDRESS, CORE_CTX_SECURE_ADDRESS);
	printf("adreno: %u: THE 3D CORE TAKES COMMANDS\n", step);
	return true;
}

/* ---- step 21: out of secure mode; a word written on command ------------------------------------------------ */

/*
 * Vendor gen7_rb_start() takes the core out of secure mode in one of two ways. With the zap
 * shader loaded it has the command processor do it (CP_SET_SECURE_MODE, 0); without it, it
 * writes RBBM_SECVID_TRUST_CNTL = 0 and lets the core be. (Both CP pipelines answered our
 * command with their microcode-error bits, 0x4002, when step 19 had loaded no shader: the note
 * in gen7_rb_start() is literal.) Which way is ours follows step 19, unless adreno_secure says
 * otherwise: 1 the command, 2 the command and then the register, 3 the register alone.
 */
static bool
step_core_commands(void)
{
	volatile uint32_t *scratch = (volatile uint32_t *)core_scratch.memory;
	uint32_t how = 0;
	uint32_t trust = 0;
	uint32_t interrupts;
	bool out_of_secure = true;

	if (!cx_ready() || !core_runs) {
		return false;
	}
	PE_parse_boot_argn("adreno_secure", &how, sizeof(how));
	PE_parse_boot_argn("adreno_trust", &trust, sizeof(trust));
	if (how == 0) {
		how = zap_is_loaded ? 1 : 3;
	}
	announce("command the 3D core out of secure mode (it asks the zap shader), then to write a word into a page of ours");
	if (how == 1 || how == 2) {
		if (hwsched_on()) {
			uint32_t message[3] = { 0, pm4_header7(CP_SET_SECURE_MODE, 1), 0 };

			/* the vendor's send_switch_to_unsecure(): H2F_MSG_ISSUE_CMD_RAW, three words */
			out_of_secure = hfi_send("leave secure mode", HFI_ISSUE_CMD_RAW, message, 3);
		} else {
			ring_command(CP_SET_SECURE_MODE, 1);
			ring(0);
			out_of_secure = ring_run("leave secure mode");
		}
	}
	if (!out_of_secure || how == 3 || how == 1) {
		get(&gpu, "rbbm_secvid_trust_cntl", RBBM_SECVID_TRUST_CNTL);
		put(&gpu, "rbbm_secvid_trust_cntl", RBBM_SECVID_TRUST_CNTL, 0);
		get(&gpu, "rbbm_secvid_trust_cntl", RBBM_SECVID_TRUST_CNTL);
		/*
		 * A one that will not go to nought is not ours to argue with: in that mode the core may
		 * touch the memory its trusted region names and nothing else, and every pixel write
		 * outside it is thrown away where no status register looks.
		 */
		printf("adreno: %u: tsb status %#x %#x: %s\n", step, rd(gpu.base + RBBM_SECVID_TSB_STATUS),
		    rd(gpu.base + RBBM_SECVID_TSB_STATUS + 4),
		    rd(gpu.base + RBBM_SECVID_TRUST_CNTL) != 0 ? "the core is still secure" : "the core is not secure");
		/*
		 * A nine hundred E came from this block on the first try: one of these writes is a
		 * security violation, so each one now needs its own value of adreno_trust. Unset,
		 * this only reads.
		 */
		if (trust == 2) {
			put(&gpu, "rbbm_secvid_trust_cntl, a one", RBBM_SECVID_TRUST_CNTL, 1);
			get(&gpu, "rbbm_secvid_trust_cntl after a one", RBBM_SECVID_TRUST_CNTL);
			put(&gpu, "rbbm_secvid_trust_cntl, a nought again", RBBM_SECVID_TRUST_CNTL, 0);
			get(&gpu, "rbbm_secvid_trust_cntl after a nought", RBBM_SECVID_TRUST_CNTL);
		}
		if (trust == 1) {
			/*
			 * Tried once, and it is a nine hundred E: these two registers take the secure
			 * carveout TrustZone names and nothing else, so this stays behind adreno_trust=1.
			 */
			put64(&gpu, "rbbm_secvid_tsb_trusted_base, the client's memory", RBBM_SECVID_TSB_TRUSTED_BASE,
			    0x20000000ull);         /* GPU_ADDRESSES_START, defined below this function */
			put(&gpu, "rbbm_secvid_tsb_trusted_size, four gigabytes", RBBM_SECVID_TSB_TRUSTED_SIZE,
			    0xfffff000);
			put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
			get(&gpu, "rbbm_secvid_tsb_trusted_size, read back", RBBM_SECVID_TSB_TRUSTED_SIZE);
		}
	}
	interrupts = rd(gpu.base + CP_INTERRUPT_STATUS);
	if (interrupts != 0) {
		printf("adreno: %u: the command processor still reports %#x%s%s: a command it cannot run does not stop the test\n",
		    step, interrupts, interrupts & (1u << 1) ? " CP_UCODEERROR" : "",
		    interrupts & (1u << 14) ? " CP_UCODEERRORBV" : "");
	}
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS);
	ring(0);
	ring(0x600dc0de);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("adreno: %u: the page reads %#x: %s\n", step, scratch[0], scratch[0] == 0x600dc0de ?
	    "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what it was told to write");
	return scratch[0] == 0x600dc0de;
}


/* ---- step 22: a frame filled by the GPU, on the panel ------------------------------------------------------ */

/*
 * The picture is filled by the command processor itself. The 2D engine's registers (GRAS_2D, RB_2D,
 * SP_2D; Mesa's older names, fd3_blitter's way) were written here and put not one pixel in memory on
 * this GPU: the vendor kernel never touches them, and on a6xx and later even Mesa draws with the 3D
 * pipeline. The command processor's own copies need no such engine, and they are what hakios-metal
 * fills and blits with on its Adreno: CP_MEM_WRITE puts the first words down, then CP_MEMCPY copies
 * what is already there into the rest, twice as much each time. One packet moves at most
 * FILL_COPY_WORDS words: hakios-metal found a much larger one hangs the core.
 */
#define FILL_HEAD_WORDS         64              /* what CP_MEM_WRITE puts down before the copies */
#define FILL_COPY_WORDS         65536           /* words in one CP_MEMCPY: 256 KB */
#define TARGET_WIDTH            1080
#define TARGET_HEIGHT           2400
#define TARGET_ROW              4352            /* 1088 pixels: the next multiple of 64 bytes */
#define TARGET_PITCH            (TARGET_ROW / 4)        /* in words */
#define EVENT_FLUSH_COLOR       0x1d
#define EVENT_CACHE_FLUSH       0x04

extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);

/* CP_MEM_WRITE: the address in two words, then the words to put there; a packet counts 14 bits of them. */
static void
ring_mem_write(uint32_t address, const uint32_t *values, uint32_t words)
{
	while (words != 0) {
		uint32_t n = words > 0x3ffd ? 0x3ffd : words;
		ring_command(CP_MEM_WRITE, 2 + n);
		ring(address);
		ring(0);
		for (uint32_t i = 0; i < n; i++) {
			ring(values[i]);
		}
		address += 4 * n;
		values += n;
		words -= n;
	}
}

/* Have the core fill rows [top, top + rows) of the picture with one colour, a word per pixel. */
static void
ring_fill(uint32_t top, uint32_t rows, uint32_t pixel)
{
	uint32_t base = CORE_TARGET_ADDRESS + top * TARGET_ROW;
	uint32_t words = rows * TARGET_PITCH;
	uint32_t pattern[FILL_HEAD_WORDS];
	uint32_t have;

	for (uint32_t i = 0; i < FILL_HEAD_WORDS; i++) {
		pattern[i] = pixel;
	}
	ring_mem_write(base, pattern, FILL_HEAD_WORDS);
	/* what comes after reads what this wrote: the copy lands before the next packet is taken */
	ring_command(CP_WAIT_MEM_WRITES, 0);
	ring_command(CP_WAIT_FOR_ME, 0);
	have = FILL_HEAD_WORDS;
	while (have < words) {
		uint32_t n = words - have;
		if (n > have) {
			n = have;
		}
		if (n > FILL_COPY_WORDS) {
			n = FILL_COPY_WORDS;
		}
		ring_command(CP_MEMCPY, 5);
		ring(n);
		ring(base);
		ring(0);
		ring(base + 4 * have);
		ring(0);
		ring_command(CP_WAIT_MEM_WRITES, 0);
		ring_command(CP_WAIT_FOR_ME, 0);
		have += n;
	}
}

static bool
step_frame(void)
{
	const volatile uint32_t *pixels;
	uint32_t first = 0xffff8000;        /* orange, the bytes 00 80 ff ff */

	if (!cx_ready() || !core_runs) {
		return false;
	}
	announce("make a picture's worth of memory for the 3D core (10 MB) and have it fill three bands of colour");
	if (!block_new(&core_space, &core_target, CORE_TARGET_ADDRESS, TARGET_ROW * TARGET_HEIGHT, PTE_CORE)) {
		return false;
	}
	/* tables that the SMMU has in use have grown: it has not seen these addresses, so nothing stale to forget */
	for (uint32_t i = 0; i < core_space.table_count; i++) {
		to_memory((vm_offset_t)core_space.tables[i].table, 4096);
	}
	/* the first word by itself, read back before the copies: is the picture's memory the core's to write? */
	ring_mem_write(CORE_TARGET_ADDRESS, &first, 1);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	if (!ring_run("write the picture's first word")) {
		return false;
	}
	to_memory(core_target.memory, 64);
	pixels = (const volatile uint32_t *)core_target.memory;
	printf("adreno: %u: the picture's first word is %#x (told 0xffff8000, the bytes 00 80 ff ff)\n", step, pixels[0]);
	if (pixels[0] != 0xffff8000) {
		printf("adreno: %u: the picture's memory is not the 3D core's to write\n", step);
		return false;
	}
	/* three bands, orange, white, then blue */
	ring_fill(0, 800, 0xffff8000);
	ring_fill(800, 800, 0xffffffff);
	ring_fill(1600, 800, 0xff0080ff);
	/*
	 * What it has drawn out of its caches into memory. Each of the two events is followed by an
	 * address and a word that the core writes there when it is done (Linux's a6xx_submit; Mesa's
	 * tu6_emit_event_write: the events named ..._TS): the core takes those three words whether
	 * they are there or not.
	 */
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_FLUSH_COLOR | (board->is_a7xx ? (1u << 27) : 0));
	ring(CORE_SCRATCH_ADDRESS + 32);
	ring(0);
	ring(0x0c0c0c0c);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH | (board->is_a7xx ? (1u << 27) : 0));
	ring(CORE_SCRATCH_ADDRESS + 16);
	ring(0);
	ring(0x0d0e0d0e);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	if (!ring_run("fill a picture")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("adreno: %u: the words it writes when its caches are in memory: %#x, %#x (told: 0xc0c0c0c, 0xd0e0d0e)\n", step,
		((volatile uint32_t *)core_scratch.memory)[8], ((volatile uint32_t *)core_scratch.memory)[4]);
	to_memory(core_target.memory, core_target.size);
	pixels = (const volatile uint32_t *)core_target.memory;
	printf("adreno: %u: the picture: the first pixel is %#x (told 0xffff8000), one in the middle %#x (told 0xffffffff), one "
		"in the last row %#x (told 0xff0080ff)\n", step, pixels[0], pixels[1200 * TARGET_PITCH + 540],
		pixels[2399 * TARGET_PITCH + 1079]);
	if (pixels[0] != 0xffff8000 || pixels[1200 * TARGET_PITCH + 540] != 0xffffffff ||
		pixels[2399 * TARGET_PITCH + 1079] != 0xff0080ff) {
		printf("adreno: %u: the picture is not what the 3D core was told to draw\n", step);
		return false;
	}
	/*
	 * A sample-count query is not a generic memory-write probe on A7xx. Mesa's A750 query
	 * path establishes query state and pairs CP_EVENT_WRITE7 ZPASS_DONE begin/end events;
	 * its tu_query_pool.cc/tu_autotune.cc warn about firmware errors with unmatched events.
	 * This boot probe had neither setup nor an end event, and its picture+4 destination
	 * was not a query result allocation. Replace that event with an equally sized NOP
	 * on A7xx, preserving timestamp 4 and the GMU ring layout for comparison with air72.
	 */
	{
		uint32_t before = pixels[1], after;

		ring_command(board->is_a7xx ? 0x10 /* CP_NOP */ : CP_EVENT_WRITE, 3);
		ring(0x1015);
		ring(CORE_TARGET_ADDRESS + 4);
		ring(0);
		ring_command(CP_WAIT_FOR_IDLE, 0);
		if (!ring_run(board->is_a7xx ? "finish boot without a standalone Z-pass query" :
		    "ask the 3D core for the Z-pass counter")) {
			return false;
		}
		to_memory(core_target.memory, 64);
		after = *(volatile uint32_t *)(core_target.memory + 4);
		if (board->is_a7xx) {
			printf("adreno: air73 boot: standalone Z-pass query replaced by an equal-size NOP; ts %u, pixel %#x -> %#x\n",
			    hwsched_ts, before, after);
			hwsched_drain();
		} else {
			printf("adreno: %u: the picture's second word was %#x; the core's own Z-pass event made it %#x\n", step, before, after);
		}
		/*
		 * What the processor put in the page it kept asking for (0x70000..0x7ffff).
		 */
		to_memory(core_guess.memory, 64);
		{
			const volatile uint32_t *g = (const volatile uint32_t *)core_guess.memory;

			printf("adreno: %u: the page it asks for, at 0x7da0: %#x %#x %#x %#x %#x %#x %#x %#x\n", step,
			    g[0xda0 / 4], g[0xda0 / 4 + 1], g[0xda0 / 4 + 2], g[0xda0 / 4 + 3], g[0xda0 / 4 + 4],
			    g[0xda0 / 4 + 5], g[0xda0 / 4 + 6], g[0xda0 / 4 + 7]);
		}
	}
	/*
	 * The GMU's own account of the core it was asked to power: its log, the words it keeps of its
	 * state, and the power domain's registers. If it has refused the core, this is where it says so.
	 */
	announce("read what the GMU kept of its own running, after the core was asked to power up");
	gmu_post_mortem();
	/* Keep the GPU draw/readback self-test; show its pattern only on request. */
	{
		int test_pattern = 0;
		PE_parse_boot_argn("gpu_test_pattern", &test_pattern, sizeof(test_pattern));
		if (test_pattern) {
			MiniIOMobileFramebufferShow((const uint8_t *)core_target.memory, TARGET_WIDTH, TARGET_HEIGHT, TARGET_ROW);
			printf("adreno: %u: GPU test pattern shown (gpu_test_pattern=1)\n", step);
		}
	}
	return true;
}

/* ---- the way back ------------------------------------------------------------------------------------- */

/*
 * Nothing of the GPU block may outlive the kernel that started it: the memory its SMMU context
 * points into is the next kernel's to use. In the order of Linux's a6xx_gmu_shutdown: the 3D
 * core given back, the GMU told to drop its votes ("slumber") and stopped, its RPMh channel put
 * to sleep; then the processor into reset and its stream sent nowhere.
 */
/*
 * The 3D core's own power switch is the GMU's to operate. Where the GMU is gone and has left it
 * on (it is in reset; it did not answer when told to slumber), the switch is turned off from
 * here (vendor's a6xx_gmu_suspend): a core that keeps its power while the rest of the block
 * loses it is found by the next start as the last one left it, in the middle of things.
 */
static void
gx_off(void)
{
    if (board->is_a7xx) {
        printf("adreno: air87: GX power remains owned by the GMU; reboot bootloader before another full test\n");
        return;
    }
	uint32_t gx = get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);

	/* gpu_cc_gx_gdsc reports its state in CFG_GDSCR bit 16, not in bit 31: see gx_is_on(). */
	if (gx_is_on()) {
		put(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr, gx | GDSC_SW_COLLAPSE);
		for (int i = 0; i < 1000 && gx_is_on(); i++) {
			sleep_us(100);
		}
		get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
		get(&gpucc, "gx cfg_gdscr", board->gpucc_gx_gdscr + 4);
	}
}

/* What step 4 does when it finds the GPU block running: see there. Nothing here has been set up by this kernel. */
static void
stale_stop(void)
{
	if (!board->is_a7xx) {
		need(&gmu);
		need(&rscc);
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	}
	gx_off();
	if (!board->is_a7xx && rd(rscc.base + RSCC_SEQ_MEM_0_DRV0) == 0xeaaae5a0 && !(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
		wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100);
		get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	}
	if (!board->is_a7xx && smmu_layout()) {
		for (uint32_t match = 0; match < 3; match++) {
			put(&smmu, "smr", SMMU_SMR(match), 0);
			put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
			put(&smmu, "cb sctlr", (smmu_pages + match) * smmu_page + SMMU_CB_SCTLR, 0);
		}
	}
	put(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr, rd(gpucc.base + board->gpucc_cx_gdscr) | GDSC_SW_COLLAPSE);
	for (int i = 0; i < 1000 && cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr);
	if (!board->is_a7xx) {
		get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	}
}

/*
 * The GPU block's bus interface is told to let what is under way finish and to start nothing
 * more (Linux's a6xx_bus_clear_pending_transactions). (Without it, a core whose SMMU context is
 * taken away under a draw leaves the SMMU waiting for good, and the CX half cannot be switched
 * off any more: only a restart of the phone gets the GPU back.)
 * `for_good`: the 3D core may be in the middle of a frame and nobody will tell it to stop (a
 * panic; a core that does not come to rest): its own side is halted too, and all of it stays
 * halted, so that nothing of it reaches memory after this, whatever becomes of its tables.
 * Otherwise the halt is lifted again at once, as Linux does: the GMU reaches its memory through
 * the same interface, and one that cannot read its messages does not answer "slumber".
 */
static void
core_bus_halt(bool for_good)
{
	if (for_good) {
		wr(gpu.base + RBBM_GBIF_HALT, 1);
		for (int i = 0; i < 100000 && !(rd(gpu.base + RBBM_GBIF_HALT_ACK) & 1); i++) {
		}
	}
	wr(gbif.base + GBIF_HALT, 1);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 1); i++) {
	}
	wr(gbif.base + GBIF_HALT, 2);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 2); i++) {
	}
	if (!for_good) {
		wr(gbif.base + GBIF_HALT, 0);
	}
}

static bool core_stuck;                 /* the 3D core has not finished what it was given, and is not waited for */

static void
gpu_block_stop(void)
{
	uint32_t slumber[3] = { 0, 0, 0 };
	bool power_off = false, slumbered = false;

	step = 0;
	newest_step = ~0u;
	if (core_runs && core_is_on) {
		/* (what the core looks like when it is given back: a GMU that then does not answer may have its reasons here) */
		printf("adreno: %u: the 3D core before it is stopped: status %#x, interrupts %#x, ring read up to %u of %u; the GMU "
		    "sees it busy: %#x\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
		    rd(gpu.base + CP_RB_RPTR), ring_at, rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS));
		core_bus_halt(core_stuck);
		printf("adreno: %u: the GPU block's bus has been halted%s (rbbm_gbif_halt_ack %#x, gbif halt_ack %#x)\n", step,
		    core_stuck ? ", the 3D core's side too, and stays so" : " and let go again", rd(gpu.base + RBBM_GBIF_HALT_ACK),
		    rd(gbif.base + GBIF_HALT_ACK));
	}
	if (core_is_on) {
		put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, 1u << 31);
		core_is_on = false;
	}
	if (gmu_talks) {
		slumbered = hfi_send("\"slumber\"", HFI_PREPARE_SLUMBER, slumber, 3);
		if (!wait_for(&gmu, GMU_AO_GPU_CX_BUSY_STATUS, 1u << 23, 0, 100)) {
			get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
		}
		get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
		get(&gpucc, "gx_gdscr", board->gpucc_gx_gdscr);
		gmu_talks = false;
	}
	/*
	 * A GMU that has slumbered has put everything down itself. One that has not answered "slumber"
	 * has not: on a7xx it was left running, and the next kernel -- started without a reset of the
	 * SoC by a loader that only stops this one -- then loads its firmware and its SMMU tables on
	 * top of a live one, which is a security violation and the nine hundred E seen there. So it is
	 * reset here whenever it did not answer, on every GPU.
	 */
	if (gmu_runs && (!board->is_a7xx || !slumbered)) {
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
		gx_off();
		/* (what the next kernel finds: the sequencer in reset, the domain collapsed) */
		get(&gmu, "cm3_sysreset, after", GMU_CM3_SYSRESET);
		get(&gpucc, "gx_gdscr, after", board->gpucc_gx_gdscr);
	}
	if (zap_is_loaded) {
		/* the next kernel loads its own: see step_zap_load() */
		printf("adreno: %u: the zap shader: %s\n", step, zap_unload() ? "TrustZone has given it up" :
		    "TRUSTZONE DOES NOT GIVE IT UP; the next kernel will find it there");
		zap_is_loaded = false;
	}
	/* vendor's a6xx_rscc_sleep_sequence: the GMU's votes in RPMh are dropped, by the always-on sequencer */
	if (rsc_awake && rd(rscc.base + RSCC_SEQ_MEM_0_DRV0) == 0xeaaae5a0) {
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
		if (!wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100)) {
			printf("adreno: %u: the GMU's RPMh channel does not go to sleep\n", step);
		}
		get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	}
	rsc_awake = false;
	if (gmu_space_is_on) {
		space_faulted(&gmu_space);
		space_off(&gmu_space, GMU_MATCH);
		gmu_space_is_on = false;
		power_off = true;
	}
	if (second_space_is_on) {
		space_faulted(&second_space);
		space_off(&second_space, SECOND_MATCH);
		second_space_is_on = false;
	}
	if (second_stream_is_on) {
		put(&smmu, "smr", SMMU_SMR(SECOND_MATCH), 0);
		put(&smmu, "s2cr", SMMU_S2CR(SECOND_MATCH), SMMU_S2CR_FAULT);
		second_stream_is_on = false;
	}
	if (core_space_is_on) {
		space_faulted(&core_space);
		space_off(&core_space, CORE_MATCH);
		core_space_is_on = core_runs = false;
	}
	/*
	 * And the CX half off again, as it was when the phone started: the GMU with all it remembers
	 * and the SMMU lose their power, and the next time starts from nothing (vendor's
	 * a6xx_gmu_suspend). The switch takes votes; ours is all this withdraws.
	 */
	if (power_off && gpucc_answers()) {
		put(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr, rd(gpucc.base + board->gpucc_cx_gdscr) | GDSC_SW_COLLAPSE);
		for (int i = 0; i < 1000 && cx_is_on(); i++) {
			sleep_us(100);
		}
		get(&gpucc, "cx_gdscr", board->gpucc_cx_gdscr);
		if (!board->is_a7xx) {
			get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
		}
		cx_off = !cx_is_on();
	}
}

/*
 * The same when there is no time for asking: the kernel is on its way back to the loader (the
 * end of a run, a panic). Only the two writes that keep the GMU out of memory that is no longer
 * its to use.
 */
void
hakios_adreno_quiesce(void)
{
	if (cx_off) {
		return;
	}
	if (gmu_runs) {
		wr(gmu.base + GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (core_runs) {
		core_bus_halt(true);
		core_runs = false;
	}
	if (gmu_space_is_on) {
		wr(smmu.base + smmu_context(&gmu_space) + SMMU_CB_SCTLR, 0);
		gmu_space_is_on = false;
	}
	if (core_space_is_on) {
		wr(smmu.base + smmu_context(&core_space) + SMMU_CB_SCTLR, 0);
		core_space_is_on = false;
	}
	if (second_space_is_on) {
		wr(smmu.base + smmu_context(&second_space) + SMMU_CB_SCTLR, 0);
		second_space_is_on = false;
	}
}

/* ---- the GPU kept on, for the system to draw with --------------------------------------------------- */

/*
 * With boot-arg adreno_keep=1 the steps are not undone: the GPU block stays as step 21 leaves it,
 * the 3D core powered and taking commands, and the service HakiosGPU (iokit/Kernel/hakios.gpu.cpp)
 * hands it to programs through the interface of Linux's msm driver, which is what Mesa's
 * freedreno speaks. That file keeps the buffers and the clients; what is the GPU's is here:
 * addresses in the 3D core's SMMU context, the ring, and the word by which the core says how
 * far it has got (a "fence": a submission's number, written by the core when it is through
 * with it). The caller holds one lock around all of these.
 */
#define GPU_CHIP_ID             0x06030500ull   /* dt: qcom,chipid */
#define GPU_ADDRESSES_START     0x20000000ull   /* above everything the steps use; below 4 GB, as Linux keeps them */
#define GPU_ADDRESSES_SIZE      0xe0000000ull
#define GPU_FENCE_AT            0x40            /* in the scratch page */
#define GPU_TIMES_AT            0x400           /* ... and when the core began and ended each of its last submissions */
#define GPU_TIMES               64
#define CP_ALWAYS_ON_COUNTER    0x0980          /* ticks of the 19.2 MHz crystal, 64 bits: for the core to read itself */
#define CP_REG_TO_MEM           0x3e
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define CP_IB2_BASE             GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE         GPU_WORD(0x092d)
#define CP_INDIRECT_BUFFER      0x3f
#define EVENT_INVALIDATE_DEPTH  0x18
#define EVENT_INVALIDATE_COLOR  0x19

static bool gpu_kept;
static uint32_t gpu_fence;              /* the number of the last submission */
static uint32_t gpu_timed;              /* ... of the last whose times have been added up */
static uint64_t gpu_ticks;              /* how long the core has worked on the submissions added up */
static uint32_t gpu_timed_count;
static uint32_t gpu_timed_by[7];        /* ... how many of them under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
static uint32_t gpu_exact = 1;          /* boot-arg gpu_exact=0: the core does not wait to be idle before it notes the time */
uint32_t gpu_log;                       /* boot-arg gpu_log=1: a line for every submission and the time the core spent on it */
static uint32_t gpu_frame = 0;          /* boot-arg gpu_frame: 0 = the plain framing (proven on the A750), 1/2 = the a7xx one, untried */
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
static struct {
	uint64_t start, size;
} gpu_free[4096];                       /* the addresses not in use, in order */
static unsigned gpu_free_count;

extern void HakiosGPUStart(void);
extern void HakiosGPUStop(void);       /* takes the clients' lock, then calls hakios_gpu_stop() */
static void gpu_block_stop(void);
static void gpu_left_behind(const char *when);

/*
 * A run that ends in order -- its time is up, the host asks for the end, five fingers on the
 * screen, a halt: whoever is about to call loader_return() for one of these calls
 * hakios_adreno_stop() first, from a thread that may wait -- gives the GPU back as the steps'
 * own ending does, and the next kernel finds the GPU block as the phone's start leaves it. A
 * panic cannot: it leaves the GPU block running but quiet (hakios_adreno_quiesce()), and the
 * next kernel's step 4 takes it down (stale_stop()). Boot-arg adreno_dirty=1 makes every ending
 * a panic's in this respect, to see that work.
 */
void
hakios_gpu_stop(void)
{
	if (gpu_kept) {
		uint32_t waited = 0;

		gpu_kept = false;                       /* (nothing more is taken) */
		quiet = false;
		/*
		 * What the core has been given, it finishes first: told to slumber in the middle of a
		 * frame, the GMU may not answer. Half a second at the most; then its bus is halted
		 * for good, and its power switched off from here.
		 */
		while (!hakios_gpu_done(gpu_fence) && waited < 5000) {
			sleep_us(100);
			waited++;
		}
		if (gpu_log && gpu_fence != 0) {
			gpu_left_behind("at the end");
		}
		core_stuck = !hakios_gpu_done(gpu_fence);
		printf("adreno: the run is about to end: the GPU is given back (%s after %u us)\n",
		    core_stuck ? "STILL AT WORK" : "it had finished what it was given", 100 * waited);
		gpu_block_stop();
	}
}

void
hakios_adreno_stop(void)
{
	uint32_t dirty = 0;

	if (gpu_kept && !(PE_parse_boot_argn("adreno_dirty", &dirty, sizeof(dirty)) && dirty)) {
		HakiosGPUStop();
	}
}

bool
hakios_gpu_ready(void)
{
	return gpu_kept;
}

/* The answers to msm's GET_PARAM (Linux's adreno_get_param for this GPU; the numbers are msm_drm.h's). */
bool
hakios_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none, the chip ID says it */
	case 0x06:                              /* GMEM_BASE */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = board->is_a7xx ? (3 * 1024 * 1024) : (512 * 1024);
		return true;
	case 0x03:                              /* CHIP_ID, with the speed bin above it */
		*value = (board->is_a7xx ? 0x43051401ull : GPU_CHIP_ID) | ((uint64_t)speed_bin << 32);
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = core_khz * 1000ull;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT */
		*value = board->is_a7xx ? 16 : 14;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE */
		*value = 6;
		return true;
	case 0x13:                              /* MACROTILE_MODE */
		*value = 1;
		return true;
	default:                                /* among them VA_START, VA_SIZE: addresses are ours to give */
		return false;
	}
}

/* Addresses for a buffer of so many bytes; 0: none left. */
uint64_t
hakios_gpu_address(uint64_t size)
{
	size = (size + 0xffff) & ~0xffffull;
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size >= size) {
			uint64_t address = gpu_free[i].start;

			gpu_free[i].start += size;
			gpu_free[i].size -= size;
			if (gpu_free[i].size == 0) {
				memmove(&gpu_free[i], &gpu_free[i + 1], (--gpu_free_count - i) * sizeof(gpu_free[0]));
			}
			if (gpu_log) {
				hakios_tl("tl: buffer %#llx on, %#llx bytes\n", address, size);
			}
			return address;
		}
	}
	return 0;
}

void
hakios_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = (size + 0xffff) & ~0xffffull;
	for (at = 0; at < gpu_free_count && gpu_free[at].start < address; at++) {
	}
	if (at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size == address) {
		gpu_free[at - 1].size += size;
		if (at < gpu_free_count && address + size == gpu_free[at].start) {
			gpu_free[at - 1].size += gpu_free[at].size;
			memmove(&gpu_free[at], &gpu_free[at + 1], (--gpu_free_count - at) * sizeof(gpu_free[0]));
		}
	} else if (at < gpu_free_count && address + size == gpu_free[at].start) {
		gpu_free[at].start = address;
		gpu_free[at].size += size;
	} else if (gpu_free_count < sizeof(gpu_free) / sizeof(gpu_free[0])) {
		memmove(&gpu_free[at + 1], &gpu_free[at], (gpu_free_count++ - at) * sizeof(gpu_free[0]));
		gpu_free[at].start = address;
		gpu_free[at].size = size;
	}                                       /* (or else these addresses are lost) */
}

/* One 4K page of ours at an address of the 3D core's; after the last of a buffer's, hakios_gpu_mapped(). */
bool
hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

void
hakios_gpu_mapped(void)
{
	space_commit(&core_space);
}

void
hakios_gpu_unmap(uint64_t address, uint64_t size)
{
	uint32_t context = smmu_context(&core_space);

	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map(&core_space, address + offset, 0, 0);
	}
	space_commit(&core_space);
	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

/* Memory that the 3D core and a program see without this processor's cache in between: out of that cache. */
void
hakios_gpu_to_memory(void *start, uint64_t size)
{
	to_memory((vm_offset_t)start, size);
}

/* How far the 3D core has got. */
bool
hakios_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	/*
	 * The GMU announces each retired submission on its answer queue, and that queue holds only
	 * sixty-four of them (1024 words, sixteen a message). Nothing waits for those announcements,
	 * so they are emptied here: left alone the queue fills, the GMU has no room to say that a
	 * submission is done, and it stops dispatching the next one. air67d showed it exactly: the
	 * fence stayed at 63 and submission 64 never reached the core.
	 */
	if (hwsched_on() && hfi != NULL) {
		hwsched_drain();
	}
	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
hakios_gpu_last(void)
{
	return gpu_fence;
}

/*
 * Whether the core still has work in it, said the way the gen8 port waits for it (adreno8
 * gpu_wait_idle: GMU_AO_GPU_CX_BUSY_STATUS bit 23), with what this processor can see beside it.
 * A client that is closing asks this before its memory is taken away.
 */
bool
hakios_gpu_idle_now(const char *what)
{
	uint32_t busy, status, fence;

	if (hwsched_on() && hfi != NULL) {
		hwsched_drain();
	}
	to_memory(core_scratch.memory, 0x80);
	busy = rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS);
	status = rd(gpu.base + RBBM_STATUS);
	fence = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
	printf("adreno: gpu: %s: fence %u, gmu busy %#x (%s), rbbm status %#x, gx gdscr %#x, sp/ra %#x\n", what,
	    fence, busy, (busy & (1u << 23)) ? "still busy" : "idle", status,
	    rd(gpucc.base + board->gpucc_gx_gdscr), rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS));
	return (busy & (1u << 23)) == 0;
}

/*
 * Wait until the GMU says the core has nothing in flight (bit 23 of its busy status), or until
 * `ms` has gone by. The fence this driver leaves in memory is written when the command processor
 * has read the buffer to its end; the 3D pipeline behind it can still be draining, and the client
 * port that runs waits for this bit after every raw command it sends (adreno8 gpu_wait_idle).
 */
bool
hakios_gpu_await_idle(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		if ((rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS) & (1u << 23)) == 0) {
			return true;
		}
		if (hwsched_on() && hfi != NULL) {
			hwsched_drain();
		}
		sleep_us(1000);
	}
	return (rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS) & (1u << 23)) == 0;
}

/* The core is to write down what time it is: at one end of submission `fence`. */
static void
ring_time(uint32_t fence, uint32_t end)
{
	ring_command(CP_REG_TO_MEM, 3);
	ring(CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);       /* two words, a 64-bit register */
	ring(CORE_SCRATCH_ADDRESS + GPU_TIMES_AT + 16 * (fence % GPU_TIMES) + 8 * end);
	ring(0);
}

/* Add up the times of the submissions the core has finished since this was last done. */
static void
gpu_times(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
	uint32_t done;

	to_memory(core_scratch.memory, 4096);
	done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
	if (done - gpu_timed > GPU_TIMES) {
		gpu_timed = done - GPU_TIMES;           /* (the older ones have been written over) */
	}
	while (gpu_timed != done) {
		uint32_t slot = ++gpu_timed % GPU_TIMES;

		if (gpu_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };     /* (19.2 ticks a microsecond) */
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			if (gpu_log) {
				/* "tl: q FENCE took US": what the core spent on that submission (iokit/Kernel/hakios.gpu.cpp: tl: Q) */
				hakios_tl("tl: q %u took %llu\n", gpu_timed, ticks * 10 / 192);
			}

			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			gpu_timed_by[by]++;
			gpu_ticks += ticks;
			gpu_timed_count++;
		}
	}
}

/*
 * How long the 3D core has worked since this was last asked, on how many submissions, and how
 * many of those took it under 0.1, 0.25, 0.5, 1, 2, 4 milliseconds and longer.
 */
void
hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	gpu_times();
	*microseconds = gpu_ticks * 10 / 192;
	*submissions = gpu_timed_count;
	memcpy(by, gpu_timed_by, sizeof(gpu_timed_by));
	bzero(gpu_timed_by, sizeof(gpu_timed_by));
	gpu_ticks = 0;
	gpu_timed_count = 0;
}

static bool s_a7xx_reset_pending = true;

void
hakios_gpu_request_context_reset(void)
{
	s_a7xx_reset_pending = true;
}

/*
 * What the core and its SMMU say about the submission that should be done by now: whether the
 * command processor took every word of what it was given (CP_IB1_REM_SIZE is zero once it has,
 * and still the whole count when it never fetched the buffer at all), what it faulted on, where
 * its ring stopped, and what the SMMU recorded both for the 3D core's context and for the streams
 * nobody has mapped. Boot-arg gpu_log=1 gates it; the faults are cleared after they are read, so
 * that the next report is about the next submission.
 */
/* Which block of the GPU asked for what the SMMU refused: the low byte of FSYNR1, as gen7_iommu_fault_block has it. */
static const char *
gpu_faulting_block(uint32_t fsynr1)
{
	static const char *const blocks[] = { "CP", "UCHE", "UCHE LPAC", "UCHE client", "CCU", "flag cache", "PREFETCH",
	    "GMU", "UCHE LPAC client" };
	uint32_t mid = fsynr1 & 0xff;

	return mid < sizeof(blocks) / sizeof(blocks[0]) ? blocks[mid] : "not one of the streams";
}

static void
gpu_left_behind(const char *when)
{
	uint32_t context = smmu_context(&core_space);
	uint32_t status = rd(smmu.base + context + SMMU_CB_FSR);
	uint32_t address = rd(smmu.base + context + SMMU_CB_FAR);      /* 32 bits: SMMU-500, not 64 */
	uint32_t syndrome = rd(smmu.base + context + SMMU_CB_FSYNR0);
	uint32_t block = rd(smmu.base + context + SMMU_CB_FSYNR1);
	uint32_t global = rd(smmu.base + SMMU_SGFSR);
	bool fault = (status & SMMU_FSR_ALL) != 0;      /* bits 10:9 only tell the format of the syndrome */

	hakios_tl("tl: %s: fence %u, ring read %u of %u; ib1 %#llx rem %u, ib2 rem %u, scratch %u\n", when,
	    gpu_fence, rd(gpu.base + CP_RB_RPTR), ring_at,
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE),
	    rd(gpu.base + CP_IB1_REM_SIZE), rd(gpu.base + CP_IB2_REM_SIZE),
	    *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT));
	hakios_tl("tl: %s: fault %#x, interrupts %#x, protect %#x; rbbm status %#x, interrupts %#x, secure %#x\n", when,
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + RBBM_SECVID_TRUST_CNTL));
	hakios_tl("tl: %s: smmu context %u %s: status %#x at %#x, fsynr %#x %#x (%s)\n", when, core_space.context,
	    fault ? "FAULTED" : "no fault", status, address, syndrome, block, gpu_faulting_block(block));
	hakios_tl("tl: %s: smmu global %#x, a one %#x, a two %#x\n", when, global,
	    rd(smmu.base + SMMU_SGFSR + 8), rd(smmu.base + SMMU_SGFSR + 12));
	/*
	 * Whether the core is powered while its work is with it: the domain's power switch, the
	 * sequencer's account of the core's power and clock, and the votes it holds in RPMh.
	 */
	hakios_tl("tl: %s: gx: gdscr %#x, sp/ra power and clock %#x, rpmh power %#x\n", when,
	    rd(gpucc.base + board->gpucc_gx_gdscr), rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS),
	    rd(gmu.base + GMU_RPMH_POWER_STATE));
	/*
	 * Whether the GPU thinks work is still under way: the second status word (the first one reads
	 * 1 even when it is idle) and the GMU's own count of what is busy in the CX/GX halves.
	 */
	hakios_tl("tl: %s: rbbm status3 %#x, gmu sees busy %#x\n", when,
	    rd(gpu.base + GPU_WORD(0x0213)), rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS));
	/* what the processor put in the page it kept asking for (0x7da0 is in it) */
	to_memory(core_guess.memory, 64);
	{
		const volatile uint32_t *g = (const volatile uint32_t *)core_guess.memory;

		hakios_tl("tl: %s: at 0x7da0: %#x %#x %#x %#x %#x %#x %#x %#x\n", when, g[0xda0 / 4], g[0xda0 / 4 + 1],
		    g[0xda0 / 4 + 2], g[0xda0 / 4 + 3], g[0xda0 / 4 + 4], g[0xda0 / 4 + 5], g[0xda0 / 4 + 6],
		    g[0xda0 / 4 + 7]);
	}
	/*
	 * Every fault bit, whatever kind, and then RESUME, which is the only way the stalled
	 * transaction goes and the record of it is let go: Linux's kgsl_iommu_pagefault_resume.
	 */
	if (status != 0) {
		wr(smmu.base + context + SMMU_CB_FSR, 0xffffffff);
		wr(smmu.base + context + SMMU_CB_RESUME, 1);
	}
	if (global != 0) {
		wr(smmu.base + SMMU_SGFSR, global);
	}
}

/*
 * Command buffers for the 3D core to run, in order: each an address and a count of words. As
 * Linux's a6xx_submit puts them in the ring. Returns the submission's number; 0: the ring is
 * full and the core is not reading it.
 */
uint32_t
hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	/*
	 * More words on the 3D core's path: the two markers that wrap the list of command buffers, and
	 * the twelve of the thread control and IFPC markers that frame the whole submission.
	 */
	uint32_t wanted = (board->is_a7xx ? (s_a7xx_reset_pending ? 91 : 65) : 26) + 4 * count + 4;
	static int ifpc = -1;                   /* adreno_ifpc=0: submit without the vendor's IFPC markers */

	if (ifpc < 0) {
		uint32_t asked = 1;

		PE_parse_boot_argn("adreno_ifpc", &asked, sizeof(asked));
		ifpc = asked != 0;
	}
	uint32_t room = 0;

	if (hwsched_on()) {
		/*
		 * There is no ring pointer for the host to read any more: the GMU keeps it. What has to be
		 * true before the ring is written over is that the submission before this one has retired,
		 * which is the fence this driver left in its client's memory.
		 */
		for (int i = 0; i < 5000 && gpu_fence != 0 && !hakios_gpu_done(gpu_fence); i++) {
			sleep_us(100);
		}
		/*
		 * The fence says the command processor read the last submission to its end; it does not
		 * say the pipeline behind it drained. The two runs of the client's 265-word stream that
		 * left the core busy (rbbm 0xc30405, gmu busy bit 23) had their fence written all the same,
		 * and the next submission then waited for a core that never idled. Let the drain finish
		 * before the next buffer is written, as the port that runs does after each raw command.
		 */
		if (gpu_fence != 0) {
			hakios_gpu_await_idle(500);
		}
	} else {
		for (int i = 0; i < 20000; i++) {
			room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
			if (room >= wanted) {
				break;
			}
			sleep_us(100);
		}
		if (room < wanted) {
			return 0;
		}
	}
	if (gpu_log && gpu_fence != 0) {
		gpu_left_behind("the one before");
	}
	gpu_times();
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	/*
	 * The context's timestamps must not go back. The boot's own frames gave the GMU 1..4 through
	 * the step that fills a picture (hwsched_submit_window's token), and the first submission of
	 * a client then began again at 1: from that point the firmware stopped retiring timestamps
	 * ("a timestamp has retired" is in the log up to 0x1, 0x2, 0x3 of the boot, never after), and
	 * the second client's submission was the one it left the core busy on.
	 */
	if (gpu_fence <= hwsched_ts_high) {
		gpu_fence = hwsched_ts_high + 1;
	}

	if (board->is_a7xx && gpu_frame != 0 && !hwsched_on()) {
		const uint32_t at4 = CORE_SCRATCH_ADDRESS + GPU_FENCE_AT;
		const uint32_t bv = CORE_SCRATCH_ADDRESS + GPU_FENCE_AT + 8;

		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring((1u << 31) | 1 /* SYNC_THREADS | THREAD_BR */);

		if (gpu_frame >= 2 && s_a7xx_reset_pending) {
			s_a7xx_reset_pending = false;
			ring_command(0x14 /* CP_WAIT_TIMESTAMP */, 4);
			ring(0);
			ring(at4);
			ring(0);
			ring(gpu_fence - 1);

			ring_command(0x17, 1);
			ring((1u << 31) | 3 /* SYNC_THREADS | THREAD_BOTH */);

			ring_command(0x1f /* CP_RESET_CONTEXT_STATE */, 1);
			ring(0xf /* RESET_ALL */);

			ring_command(0x17, 1);
			ring((1u << 31) | 3);

			ring_command(0x46 /* CP_EVENT_WRITE */, 1);
			ring(0x26 /* LRZ_FLUSH_INVALIDATE */);

			ring_command(0x17, 1);
			ring((1u << 31) | 1 /* SYNC_THREADS | THREAD_BR */);

			ring_register(0x050e /* RBBM_PERFCTR_SRAM_INIT_CMD */, 1);

			ring_command(0x46, 1);
			ring(0x31 /* CACHE_INVALIDATE */);

			ring_command(0x3c /* CP_WAIT_REG_MEM */, 6);
			ring(3 /* WAIT_WRITE_EQ */);
			ring(0x050f /* RBBM_PERFCTR_SRAM_INIT_STATUS */);
			ring(0);
			ring(1);
			ring(1);
			ring(0);
		}

		/* CACHE_FLUSH7 & CACHE_INVALIDATE7 */
		ring_command(0x46, 1);
		ring(0x32);
		ring_command(0x46, 1);
		ring(0x33);

		ring_command(0x26 /* CP_WAIT_FOR_IDLE */, 0);

		/* Switch to BOTH threads (BR and BV) for user command execution */
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(3 /* THREAD_BOTH */);

		/* Disable IFPC */
		ring_command(0x65 /* CP_SET_MARKER */, 1);
		ring(0x101);

		for (uint32_t i = 0; i < count; i++) {
			ring_command(CP_INDIRECT_BUFFER, 3);
			ring((uint32_t)addresses[i]);
			ring((uint32_t)(addresses[i] >> 32));
			ring(words[i]);
		}

		/* Framed fence */
		ring_register(CP_SCRATCH_REG2, gpu_fence);

		/* CCU cache flush on BR */
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(1 /* THREAD_BR */);
		ring_command(0x46, 1);
		ring(0x18 /* CCU_INVALIDATE_DEPTH */);
		ring_command(0x46, 1);
		ring(0x19 /* CCU_INVALIDATE_COLOR */);

		/* BV timestamp write */
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(2 /* THREAD_BV */);
		ring_command(0x46, 4);
		ring(0x31 | (1u << 27) /* CACHE_CLEAN | WRITE_ENABLED */);
		ring(bv);
		ring(0);
		ring(gpu_fence);

		/* BR waits for BV timestamp */
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(1 /* THREAD_BR */);
		ring_command(0x14 /* CP_WAIT_TIMESTAMP */, 4);
		ring(0);
		ring(bv);
		ring(0);
		ring(gpu_fence);

		/* Final ring timestamp write with interrupt to fence */
		ring_command(0x46, 4);
		ring(0x31 | (1u << 27) | (1u << 31) /* CACHE_CLEAN | WRITE_ENABLED | IRQ */);
		ring(at4);
		ring(0);
		ring(gpu_fence);

		/* Re-enable both threads and IFPC */
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(3 /* THREAD_BOTH */);
		ring_command(0x65 /* CP_SET_MARKER */, 1);
		ring(0x100);
	} else {
		if (!hwsched_on()) {
			ring_time(gpu_fence, 0);
		}
		if (board->is_a7xx && ifpc != 0 && !hwsched_on()) {
			/*
			 * Both threads are told to stop collapsing the core's power in the middle of a frame, and
			 * the work then goes to the core's own thread -- the vendor's gen7_ringbuffer_addcmds does
			 * this before every submission it makes (CP_SET_MARKER 0x101, "IFPC disable").
			 */
			ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
			ring(3 /* SET_THREAD_BOTH */);
			ring_command(CP_SET_MARKER, 1);
			ring(0x101 /* IFPC disable */);
			ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
			ring(1 /* SET_THREAD_BR */);
		}
		/*
		 * The command buffers are handed over as one list, marked at its start and its end, as the
		 * vendor's gen7_ringbuffer_submitcmd does (CP_SET_MARKER 0x00d, then the indirect buffers, then
		 * 0x00e). Until now they went in unmarked: the processor fetched them -- CP_IB1_REM_SIZE
		 * reached nought -- and nothing of what they asked the 3D pipeline for happened.
		 */
		if (board->is_a7xx) {
			ring_command(CP_SET_MARKER, 1);
			ring(0x00d);                            /* IB1LIST start */
		}
		for (uint32_t i = 0; i < count; i++) {
			ring_command(CP_INDIRECT_BUFFER, 3);
			ring((uint32_t)addresses[i]);
			ring((uint32_t)(addresses[i] >> 32));
			ring(words[i]);
		}
		if (board->is_a7xx) {
			ring_command(CP_SET_MARKER, 1);
			ring(0x00e);                            /* IB1LIST end */
		}
		/*
		 * The client's stream sets the pair of threads itself (its first packet is
		 * CP_THREAD_CONTROL 3, so the command processor carries on as the BV thread). What this
		 * driver puts after its list must run as the 3D core's own thread. A run cut to two words
		 * left the pair on BOTH: every packet below was skipped -- the fence stayed nought with
		 * the core idle and the ring read 252 -- and the submission never retired.
		 */
		ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
		ring(1 /* THREAD_BR */);
		/* how far the core got: a word once it is back from the client's buffers */
		ring_command(CP_MEM_WRITE, 3);
		ring(CORE_SCRATCH_ADDRESS + 0x60);
		ring(0);
		ring(0xbeef0001);
		/* the two cache invalidations come after the list, as the vendor puts them */
		ring_command(CP_EVENT_WRITE, 1);
		ring(EVENT_INVALIDATE_DEPTH);
		ring_command(CP_EVENT_WRITE, 1);
		ring(EVENT_INVALIDATE_COLOR);
		if (s_write_probe != 0 && gpu_log) {
			/* CP_MEM_WRITE: the address in two words, then the word: [addr lo, addr hi, data] */
			ring_command(CP_MEM_WRITE, 3);
			ring((uint32_t)s_write_probe);
			ring((uint32_t)(s_write_probe >> 32));
			ring(0xdeadbeef);
			s_write_probe = 0;
		}
		if (gpu_exact) {
			ring_command(CP_WAIT_FOR_IDLE, 0);      /* the time is the pipeline's, not the command processor's, which is ahead of it */
		}
		/* ... and a word past the cache invalidations and the wait for the pipeline */
		ring_command(CP_MEM_WRITE, 3);
		ring(CORE_SCRATCH_ADDRESS + 0x64);
		ring(0);
		ring(0xbeef0002);
		if (hwsched_on()) {
			/*
			 * The fence as a plain write of this processor's, which it has made every time it was asked
			 * (step 21's word, the breadcrumbs above); the counter read and the scratch register the ring
			 * path puts here are left out, the core stopping at them.
			 */
			ring_command(CP_MEM_WRITE, 3);
			ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
			ring(0);
			ring(gpu_fence);
		} else {
			ring_time(gpu_fence, 1);
			ring_register(CP_SCRATCH_REG2, gpu_fence);
			ring_command(CP_EVENT_WRITE, 4);
			ring(EVENT_CACHE_FLUSH | (1u << 27) /* the write needs WRITE_ENABLED: step 22's own events show it */);
			ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
			ring(0);
			ring(gpu_fence);
		}
		if (board->is_a7xx && ifpc != 0 && !hwsched_on()) {
			/* the core may rest between frames again, as the vendor ends every submission */
			ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
			ring(3 /* SET_THREAD_BOTH */);
			ring_command(CP_SET_MARKER, 1);
			ring(0x100 /* IFPC enable */);
			ring_command(0x17 /* CP_THREAD_CONTROL */, 1);
			ring(1 /* SET_THREAD_BR */);
		}
	}
	if (hwsched_on()) {
		uint32_t ring_words = ring_at;
		static int raw_ib = -1;

		if (raw_ib < 0) {
			uint32_t asked = 0;

			(void)PE_parse_boot_argn("adreno_raw_ib", &asked, sizeof(asked));
			raw_ib = asked != 0;
		}
		if (raw_ib && count == 1) {
			/*
			 * adreno_raw_ib=1: the client's own buffer goes to the GMU as the command buffer,
			 * exactly as the client built it -- no ring, no markers, no tail of this driver's.
			 * Nothing of ours writes a fence then, so the core going quiet is the answer and the
			 * fence word is written from here.
			 */
			ring_at = 0;
			ring_sent = 0;
			if (!hwsched_issue_raw(addresses[0], words[0], gpu_fence)) {
				return 0;
			}
			if (hakios_gpu_await_idle(500)) {
				((volatile uint32_t *)core_scratch.memory)[GPU_FENCE_AT / 4] = gpu_fence;
				to_memory(core_scratch.memory, 0x80);
			}
			return gpu_fence;
		}

		/*
		 * Everything above wrote the ring; here it goes to the GMU as one command buffer with the
		 * length in bytes (gen7_hwsched_submit_drawobj, hfi_issue_ib). No answer is waited for -- the
		 * fence written at the end of it is the answer, and hakios_gpu_done() reads it.
		 */
		to_memory(core_ring.memory, 4 * RING_WORDS);
		ring_at = 0;
		ring_sent = 0;
		if (ring_words == 0 || !hwsched_submit_window(CORE_RING_ADDRESS, ring_words, gpu_fence)) {
			return 0;
		}
	} else {
		ring_send();
	}
	return gpu_fence;
}

/* A submission does not come back: what the 3D core and its SMMU context say. */
void
hakios_gpu_report(const char *why)
{
	uint32_t context = smmu_context(&core_space);

	printf("adreno: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; status "
	    "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why, gpu_fence,
	    *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), rd(gpu.base + CP_RB_RPTR), ring_at,
	    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	to_memory(core_scratch.memory, 0x80);
	printf("adreno: gpu:   the words the core left: fence %#x, a submission's token %#x, back from the buffers %#x, "
	    "past the waits %#x\n", ((volatile uint32_t *)core_scratch.memory)[GPU_FENCE_AT / 4],
	    ((volatile uint32_t *)core_scratch.memory)[HWSCHED_TOKEN_AT / 4],
	    ((volatile uint32_t *)core_scratch.memory)[0x60 / 4], ((volatile uint32_t *)core_scratch.memory)[0x64 / 4]);
	printf("adreno: gpu:   in the first command buffer at %#llx, %u words to go; in the second at %#llx, %u to go\n",
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
	step = 0;
	if (space_faulted(&core_space)) {
		wr(smmu.base + context + SMMU_CB_FSR, SMMU_FSR_ALL);    /* so that the next one shows */
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks, step_first_words, step_rscc, step_smmu,
	step_smmu_context, step_gmu_memory, step_trustzone, step_zap_signature, step_gmu_space, step_gmu_firmware,
	/*
	 * The zap shader is loaded after the command processor is started: that is the order of the vendor's
	 * gen7_rb_start (CP init, then adreno_zap_shader_load, then out of secure mode), and the order the
	 * phone's own kernel uses. Asking TrustZone for the image before the processor ran (the order here
	 * until now) was refused.
	 */
	step_gmu_start, step_gmu_queues, step_core_power, step_core_space, step_core_start, step_zap_load, step_core_commands,
	step_frame,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/* ---- beside the steps: how warm the GPU is ------------------------------------------------------------------ */

/*
 * The SoC's temperature sensors report by themselves, in a block that is always on (dt: tsens1,
 * whose readings are at 0xc265000; Linux's tsens-v2.c): a status word a sensor, the last reading
 * in its low 12 bits, signed, in tenths of a degree, and bit 21 when there is one. Sensors 1 and
 * 2 sit at the GPU (dt: gpuss-0, gpuss-1). Read, never written: once a minute while the GPU is
 * kept, for the line beside the kernel's line about memory (osfmk/arm64/loader_return.c). A
 * sensor caught between two readings has none for a moment and is asked again; one that still
 * has none is put down as -273.0.
 */
static struct block tsens = { "tsens1", 0x0c265000, 0x200 };
#define TSENS_STATUS(sensor)    (0xa0 + 4 * (sensor))
#define TSENS_VALID             (1u << 21)

void
hakios_adreno_minute(void)
{
	int32_t tenths[2];

	if (!gpu_kept || board->is_a7xx) {
		return;
	}
	need(&tsens);
	for (unsigned i = 0; i < 2; i++) {
		tenths[i] = -2730;
		for (unsigned again = 0; again < 16 && tenths[i] == -2730; again++) {
			uint32_t status = rd(tsens.base + TSENS_STATUS(1 + i));

			if (status & TSENS_VALID) {
				tenths[i] = (int32_t)(status << 20) >> 20;
			}
		}
	}
	printf("adreno: the GPU's sensors read %d.%d and %d.%d degrees; its clock is set to %u MHz\n", tenths[0] / 10,
	    tenths[0] % 10 < 0 ? -(tenths[0] % 10) : tenths[0] % 10, tenths[1] / 10,
	    tenths[1] % 10 < 0 ? -(tenths[1] % 10) : tenths[1] % 10, core_khz / 1000);
}

/* ---- beside the steps: what the firmware left the display doing (boot-arg display_probe=1) ---------------- */

/*
 * The panel is a DSI video panel (dt: qcom,mdss_dsi_m20_36_02_0a_dsc_vid: 1080 x 2400 at 120
 * frames a second, 2432 lines with the porches; or at 60 with a longer vertical front porch):
 * the display controller sends it the framebuffer over and over at the rate of its timing
 * registers, which nobody has touched since the firmware. This reads them. Each read is behind
 * a check that what it reads has its clock: the bus clock of the display's clock controller (by
 * GCC, which answers always; the firmware leaves it off, Linux keeps it on always, and so it is
 * turned on here: one write), in that controller the display block's power switch and the bus
 * clock of its registers (the firmware set the panel going and took that clock away again; it
 * is turned on here: the other write, and the last), then the timing of the interface the panel
 * hangs on (Linux's dpu_hw_intf.c has the registers' meaning) and its two counters, read twice a
 * known time apart: how many frames a second the panel is really sent. Neither clock is turned
 * off again. Every access is announced and waited for (adreno_pause), as in the newest step.
 *
 * Seen: 476 clocks a line (360 of them picture: the panel takes it compressed three to one),
 * 2432 lines a frame of which 2400 are shown, and 119.93 frames a second: the panel runs in
 * its 120 Hz mode, whatever is put into the framebuffer 60 times a second. The two counters
 * count although the register that enables them reads 0; the line counter goes from 0 to 2431,
 * 429 lines in 1.47 ms. (The first clock is found off again by the next kernel.)
 */
static struct block dispcc = { "dispcc", 0x0af00000, 0x2000 };          /* dt: clock-controller@af00000 */
static struct block disp_intf = { "intf1", 0x0ae36000, 0x300 };         /* dt: mdss_dsi0's mdp_intf_base */
#define GCC_DISP_AHB_CBCR       0x27004
#define DISPCC_MDSS_CORE_GDSCR  0x1004          /* dt: disp_cc_mdss_core_gdsc */
#define DISPCC_MDSS_PCLK0_CBCR  0x1010
#define DISPCC_MDSS_MDP_CBCR    0x1014
#define DISPCC_MDSS_AHB_CBCR    0x1050
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_HSYNC_CTL          0x008           /* clocks a line << 16 | of which the pulse */
#define INTF_VSYNC_PERIOD       0x00c           /* clocks a frame */
#define INTF_DISPLAY_V_START    0x01c           /* clocks before the first line shown, and */
#define INTF_DISPLAY_V_END      0x024           /* ... up to the last */
#define INTF_DISPLAY_HCTL       0x03c
#define INTF_FRAME_LINE_COUNT_EN 0x0a8
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0

static void
display_probe(void)
{
	uint32_t saved_fine = fine, line, frame, shown, counting, frames[2], lines[2], bus;
	uint64_t times[2];

	step = newest_step = 0;
	fine = 1;
	announce("the display: has its clock controller its bus clock (gcc 0x127004; GCC answers always)");
	if (get(&gcc, "disp_ahb_cbcr", GCC_DISP_AHB_CBCR) & CBCR_OFF) {
		announce("turn the bus clock of the display's clock controller on (gcc 0x127004 bit 0: THE ONE WRITE)");
		put(&gcc, "disp_ahb_cbcr", GCC_DISP_AHB_CBCR, rd(gcc.base + GCC_DISP_AHB_CBCR) | CBCR_ENABLE);
		sleep_ms(10);
	}
	/*
	 * Turned on, the register still says the clock is off (seen: 0x8000000b). Its bit 1 is
	 * set as the firmware left it: the clock is gated by the hardware itself while nobody
	 * talks to the block, and that is what "off" then means. Linux turns the clock on the same
	 * way, never looks at this bit and reads the clock controller next; so does this, if the
	 * clock is at least asked for.
	 */
	if (!(get(&gcc, "disp_ahb_cbcr", GCC_DISP_AHB_CBCR) & CBCR_ENABLE)) {
		printf("adreno: 0: the display's clock controller has no bus clock, and does not get one: nothing more is read\n");
		goto out;
	}
	announce("the display block's power switch and clocks, in its clock controller (0xaf00000: NOT READ BEFORE)");
	get(&dispcc, "mdss_mdp_cbcr", DISPCC_MDSS_MDP_CBCR);
	get(&dispcc, "mdss_pclk0_cbcr", DISPCC_MDSS_PCLK0_CBCR);
	if (!(get(&dispcc, "mdss_core_gdscr", DISPCC_MDSS_CORE_GDSCR) & GDSC_POWER_ON)) {
		printf("adreno: 0: the display block is without power: nothing more is read\n");
		goto out;
	}
	bus = get(&dispcc, "mdss_ahb_cbcr", DISPCC_MDSS_AHB_CBCR);
	if (bus & CBCR_OFF) {
		/* (seen: the power switch on, the display's own clock and the pixel clock running, this one off: 0x80000000) */
		announce("turn the bus clock of the display block's registers on (dispcc 0xaf01050 bit 0: THE ONE WRITE THERE)");
		put(&dispcc, "mdss_ahb_cbcr", DISPCC_MDSS_AHB_CBCR, bus | CBCR_ENABLE);
		sleep_ms(10);
		bus = get(&dispcc, "mdss_ahb_cbcr", DISPCC_MDSS_AHB_CBCR);
	}
	if (bus & CBCR_OFF) {
		/* (Linux waits for this bit to go, and gives up if it does not: there is no clock to read the block with) */
		printf("adreno: 0: the bus clock of the display block's registers does not run (%#x): nothing more is read\n", bus);
		goto out;
	}
	announce("the timing of the interface the panel hangs on (0xae36000: NOT READ BEFORE)");
	get(&disp_intf, "timing_engine_en", INTF_TIMING_ENGINE_EN);
	line = get(&disp_intf, "hsync_ctl", INTF_HSYNC_CTL) >> 16;
	frame = get(&disp_intf, "vsync_period", INTF_VSYNC_PERIOD);
	shown = get(&disp_intf, "display_v_end", INTF_DISPLAY_V_END) - get(&disp_intf, "display_v_start", INTF_DISPLAY_V_START);
	get(&disp_intf, "display_hctl", INTF_DISPLAY_HCTL);
	counting = get(&disp_intf, "frame_line_count_en", INTF_FRAME_LINE_COUNT_EN);
	frames[0] = get(&disp_intf, "frame_count", INTF_FRAME_COUNT);
	lines[0] = get(&disp_intf, "line_count", INTF_LINE_COUNT);
	/* the two counters together, twice, a second apart by our own clock */
	quiet = true;
	for (int i = 0; i < 2; i++) {
		frames[i] = rd(disp_intf.base + INTF_FRAME_COUNT);
		lines[i] = rd(disp_intf.base + INTF_LINE_COUNT);
		absolutetime_to_nanoseconds(mach_absolute_time(), &times[i]);
		if (rd(disp_intf.base + INTF_FRAME_COUNT) != frames[i]) {
			i--;                            /* (a frame began between the two reads: once more) */
			continue;
		}
		if (i == 0) {
			sleep_ms(1000);
		}
	}
	quiet = false;
	if (line != 0) {
		printf("adreno: 0: THE DISPLAY: %u lines a frame, of which %u shown, %u clocks a line (the device tree's timing "
		    "for 120 frames a second has 2432 lines; for 60, about twice that)\n", frame / line, (shown + 1) / line, line);
	}
	printf("adreno: 0: the interface's counters (frame_line_count_en %#x): frame %u line %u, then frame %u line %u %llu us "
	    "later\n", counting, frames[0], lines[0], frames[1], lines[1], (times[1] - times[0]) / 1000);
	if (line != 0 && frame >= line && frames[1] != frames[0] && times[1] > times[0]) {
		/* in thousandths of a frame a second: the frames gone by, and the part of one that the lines say */
		uint64_t per_frame = frame / line;
		uint64_t gone = (uint64_t)(frames[1] - frames[0]) * per_frame + lines[1] - lines[0];
		uint64_t rate = gone * 1000000000ull / per_frame * 1000 / (times[1] - times[0]);

		printf("adreno: 0: THE PANEL IS SENT %llu.%03llu FRAMES A SECOND (%llu lines in that time)\n", rate / 1000, rate % 1000,
		    gone);
	}
	/* where the scan is, read eight times a millisecond and a bit apart: how fast it goes down the panel */
	for (int i = 0; i < 8; i++) {
		uint64_t nanos;

		absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
		printf("adreno: 0: the scan: at %llu us line %u of frame %u\n", nanos / 1000 % 1000000, rd(disp_intf.base + INTF_LINE_COUNT),
		    rd(disp_intf.base + INTF_FRAME_COUNT));
		sleep_us(1300);
	}
out:
	fine = saved_fine;
	newest_step = 0;
}

/*
 * Boot-arg gpu=1: is this the phone, and is its GPU's firmware in the small image? Then the
 * GPU is started and kept for the system to draw with, which is what
 * "adreno=21 adreno_skip=0x1c00 adreno_pause=0 adreno_keep=1" asks for at length: the steps
 * that only look (10 to 12) left out, no pauses, no watch. Anywhere else -- QEMU, whose machine
 * has its peripherals elsewhere -- not one register of the above is touched.
 */
#define HAKIOS_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin (its "redwood") */

extern vm_offset_t pe_arm_get_soc_base_phys(void);

static bool
start_by_itself(void)
{
	const char *const files[] = { board->gmu_fw_name, board->sqe_fw_name, board->zap_fw_name, board->zap_b02_name };
	const char *missing = NULL;
	uint64_t size;

	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		printf("adreno: THE GPU IS NOT STARTED: gpu=1 is for the phone, and this is not it\n");
		return false;
	}
	image_asked_quietly = true;
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]) && missing == NULL; i++) {
		if (image_file("fw", files[i], &size) == NULL) {
			missing = files[i];
		}
	}
	image_asked_quietly = false;
	if (missing != NULL) {
		printf("adreno: THE GPU IS NOT STARTED: the small image has no fw/%s (the phone's /vendor/firmware/%s goes into "
		    "fw-gpu/); the system draws with its software renderer\n", missing, missing);
		return false;
	}
	return true;
}

/*
 * Called while the system starts, before there is a root file system, and the start waits: for
 * as long as the steps and the watch after them last, nothing else writes to the log, so that
 * its end -- all that the loader's notes keep -- is about what is being done here.
 */
void
hakios_adreno_init(void)
{
	static const char *const what[] = {
		"", "the clocks outside", "the fuses", "the clock controller", "the CX half's power", "the clocks", "the first words",
		"the GMU's RPMh sequencer", "the SMMU", "an SMMU context", "the GMU's memory", "TrustZone", "the zap shader's header",
		"the GMU's address space", "the GMU's firmware", "the GMU's start", "the GMU's message queues", "the 3D core's power",
		"the 3D core's address space", "the zap shader", "the 3D core's start", "the 3D core's first commands", "a frame"
	};
	uint32_t only = 0, done = 0, failed = 0, keep = 0, bsel = 1;
	bool by_itself = false;

	PE_parse_boot_argn("adreno_board", &bsel, sizeof(bsel));
	if (bsel == 0) {
		board = &board_redwood;
	} else {
		board = &board_corvette;
	}
	qfprom.phys = board->qfprom_phys;
	qfprom.size = board->qfprom_size;
	gpucc.phys = board->gpucc_phys;
	gpucc.size = board->gpucc_size;
	gmu.phys = board->gmu_phys;
	gmu.size = board->gmu_size;
	rscc.phys = board->rscc_phys;
	rscc.size = board->rscc_size;
	smmu.phys = board->smmu_phys;
	smmu.size = board->smmu_size;
	cmd_db = NULL;

	if (PE_parse_boot_argn("display_probe", &only, sizeof(only)) && only) {
		display_probe();
	}
	if (!PE_parse_boot_argn("adreno", &last_step, sizeof(last_step))) {
		if (!(PE_parse_boot_argn("gpu", &only, sizeof(only)) && only) || !start_by_itself()) {
			return;
		}
		by_itself = true;
		last_step = STEPS;             /* step 21 (commands) is proven; the picture on the panel is step 22 */
		skip_steps = 1u << 10 | 1u << 11 | 1u << 12;
		pause_ms = 0;
		watch_seconds = 0;
		keep = 1;
	}
	PE_parse_boot_argn("adreno_skip", &skip_steps, sizeof(skip_steps));
	if (PE_parse_boot_argn("adreno_only", &only, sizeof(only)) && only > 0 && only <= STEPS) {
		last_step = only;
		skip_steps = ~(1u << only);
	}
	PE_parse_boot_argn("adreno_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("adreno_fine", &fine, sizeof(fine));
	if (last_step == 0) {
		return;                                 /* (adreno=0: the GPU is left alone) */
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	PE_parse_boot_argn("adreno_watch", &watch_seconds, sizeof(watch_seconds));
	for (newest_step = last_step; newest_step > 0 && (skip_steps & 1u << newest_step); newest_step--) {
	}

	printf("adreno: steps up to %u, without those in mask %#x; step %u is the new one and waits %u ms %s\n", last_step,
	    skip_steps, newest_step, pause_ms, fine ? "before every access" : "after each announcement");
	for (step = 1; step <= last_step; step++) {
		if (skip_steps & 1u << step) {
			continue;
		}
		if (!steps[step - 1]()) {
			printf("adreno: step %u: FAILED; the steps after it are not attempted\n", step);
			failed = step;
			break;
		}
		printf("adreno: step %u: done\n", step);
		done = step;
	}
	PE_parse_boot_argn("adreno_keep", &keep, sizeof(keep));
	if (keep) {
		if (done >= 21 && core_runs) {
			gpu_free[0].start = GPU_ADDRESSES_START;
			gpu_free[0].size = GPU_ADDRESSES_SIZE;
			gpu_free_count = 1;
			bzero((void *)core_scratch.memory, 4096);
			to_memory(core_scratch.memory, 4096);
			PE_parse_boot_argn("gpu_exact", &gpu_exact, sizeof(gpu_exact));
			PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
			PE_parse_boot_argn("gpu_frame", &gpu_frame, sizeof(gpu_frame));
			gpu_kept = true;
			quiet = true;
			printf("adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU\n");
			if (gpu_log) {
				gpu_left_behind("as the system takes it");
			}
			HakiosGPUStart();
			return;
		}
		if (!by_itself) {
			printf("adreno: not kept: the steps did not get as far as a 3D core that takes commands\n");
		}
	}
	if (by_itself) {
		/* (one line for whoever wonders why the picture is drawn slowly; the steps' own lines are above it) */
		printf("adreno: THE GPU IS NOT STARTED: step %u (%s) failed; the system draws with its software renderer\n",
		    failed != 0 ? failed : 21, what[failed != 0 && failed <= STEPS ? failed : 21]);
	}
	/*
	 * Punishment for a write may come late (a reset some 17 seconds after it has been seen):
	 * keep the end of the log saying where things stand.
	 */
	for (uint32_t second = 1; second <= watch_seconds; second++) {
		sleep_ms(1000);
		printf("adreno: alive %u s after step %u; %s\n", second, done, last_write);
	}
	gpu_block_stop();
	if (PE_parse_boot_argn("adreno_return", &only, sizeof(only)) && only) {
		printf("adreno: over; back to the loader (adreno_return)\n");
		loader_return();
	}
	printf("adreno: over; the system goes on starting\n");
}
