/*
 * The Wi-Fi of the POCO X5 Pro (redwood), from nothing and step by step. Every address below
 * belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The radio is a WCN6750. Its firmware does not run on the application processor but on a
 * processor of its own inside the SoC, the "WPSS" (a Hexagon DSP), which stays in reset until
 * somebody loads it: the application processor puts the signed image -- the phone's own
 * wpss.mdt and wpss.b02 to wpss.b07, from /vendor/firmware_mnt/image of its Android -- into the
 * memory reserved for it and asks TrustZone to authenticate and start it, as for the GPU's zap
 * shader (hakios.adreno.c). Everything after that is conversation with the running firmware.
 * Its first words go through what this file begins with:
 *
 *   SMEM    two megabytes of memory that every processor of the SoC can reach, laid out by the
 *           boot loader, in which each of them allocates numbered items
 *   SMP2P   a pair of such items between two processors, with named entries of 32 bits: in
 *           the DSP's are "I am up", "I have crashed", "I have stopped"
 *   IPCC    the SoC's doorbells: a register to ring another processor's with, and one that
 *           says who rang ours
 * and through one thing that only the loading needs: the mailbox of the always-on processor
 * (the "AOP", which runs RPMh, the keeper of the supply rails), which is told that the image is
 * being loaded before it honours the DSP's own requests for power.
 *
 * Boot-arg wifi_step=N performs steps 1 to N and stops; a step says what it is about to touch before
 * it touches it and what it found after, reads come before writes, and a step that does not
 * find what it needs ends the sequence instead of going on blind.
 *
 *   1  reads   the shared memory area: how the boot loader laid it out, and whether the DSP
 *              has left anything in it (it has if it was started since the phone was)
 *   2  reads   the always-on processor's mailbox and the doorbells, as they are
 *   3  reads   RPMh: where it has the supplies involved, and what the application processor's
 *              sequencer holds in its command slots (what was last asked for from here)
 *   4  reads   the supplies themselves, from the two PMICs that make the radio's three
 *              (PM7325 S7, S1, S8) and the two the DSP runs from (CX and MX: PM8350C S2 and
 *              S10). The only thing written is the command word that asks the PMIC arbiter for
 *              a read
 *   5  reads   the radio's pins
 *   6  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   7  writes  the always-on processor's mailbox open: five words of its message memory, and
 *              its doorbell
 *   8  asks    TrustZone to take the image's header and check its signature (image 6), and
 *              tells it where the image will be
 *   9  writes  the image into the DSP's memory, and reads it back
 *  10  writes  our SMP2P item into the shared memory: allocated there, under the SoC's lock for
 *              that, unless a kernel before this one has made it; filled in afresh either way
 *  11  writes  "the image is loaded" to the always-on processor, asks TrustZone to authenticate
 *              the image and let the DSP out of reset, and watches the shared memory and our
 *              doorbell for the DSP's first words, answering in our item what SMP2P's rules
 *              have us answer
 *  12  reads   GLINK's queues in the shared memory, and what the DSP has put into its own
 *  13  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our
 *              intents; from here a thread of ours listens to the DSP
 *  14  talks   QRTR on that channel: hello both ways, the services the DSP tells of; a port
 *              of ours looks the Wi-Fi firmware's service up (sys/hakios.qrtr.h)
 *  15  talks   to the Wi-Fi firmware's service, what a WCN6750's driver says to it before it
 *              touches the radio: a stand-in for ath11k's own code, see hakios.wlfw.c
 *  16  reads   seven words of the radio's registers, where the firmware said they are: only
 *              with wifi_radio_read=1, and only with the firmware in mission mode (outside
 *              it a read there has ended the kernel)
 * Steps 1 to 5 are the survey; they write to nothing but the arbiter's command word.
 *
 * What is started is stopped: when the steps are over, wherever a run ends in order while the
 * DSP runs, and on a panic's way back to the loader, TrustZone is asked to shut the DSP down
 * and the always-on processor is told that the image is gone. A kernel that dies otherwise
 * leaves the DSP running, and the kernel after it cannot ask the hardware whether that is so. But the shared memory outlives our
 * kernels (the phone is not restarted between them), and the DSP's own item in it is there if
 * and only if the DSP has run since the phone was started: when step 8 finds it, TrustZone is
 * first asked for the shutdown. On a phone just started nothing is asked that is not needed.
 *
 * The DSP is asked to stop before TrustZone shuts it down (Linux's order; dsp_ask_to_stop()),
 * and that matters: a DSP shut down by TrustZone alone -- as the first version of this did,
 * having no SMP2P item to ask with, so that the DSP never got as far as saying it was up --
 * could not be started again before the phone was restarted. Twice that was tried, with the
 * same image written and read back again and accepted by TrustZone again, and both times the
 * phone reset without a word at the request to authenticate and start, or within two seconds
 * of it. Asked first, and having acknowledged, it starts again. So step 8, having had a DSP
 * shut down that did not say it had stopped, goes no further unless told to (wifi_again=1).
 * (Whether the asking cures it or the DSP's having come up fully before, was not told apart.)
 *
 * Nothing here switches a supply rail or changes a pin.
 *
 * wifi=1 is all of it for whoever uses the phone: the steps that are needed (not the ones that
 * only look), ath11k as step 15 for real, its station given to the rest of the kernel to ask
 * (sys/hakios.sta.h: iOS's Wi-Fi side does, through bsd/dev/hakios.apple80211.c), the network
 * of fw/network joined if the small image has that file, and everything left running while the
 * system does -- on a thread of its own, so that the system's start does not wait for it, and
 * without a word in the log but the outcome (what the steps say is kept, and shown if the radio
 * does not come up). Without the radio's firmware in the small image it says so in a line and
 * does nothing. (wifi_loud=1 with it: nothing is kept back, for looking into a start that goes
 * wrong.) The rest of this comment is about the steps one by one, wifi_step=N, which is how
 * all this was found out.
 *
 * More boot-args, for wifi_step=N:
 *   wifi_new=K         steps K and later have not been seen to work (default: the last one)
 *   wifi_pause=MS      how long such a step waits after announcing a group of accesses
 *                      (default 2200: the loader, watching from another core, notes the end of
 *                      our log every two seconds, and after a silent reset that is all there is)
 *   wifi_fine=1        such a step waits like that before every single write and call
 *   wifi_wait=S        how long step 11 watches for the DSP to say that it is up (default 10)
 *   wifi_watch=S       ... and how long it goes on watching once it has (default 3)
 *   wifi_listen=S      how long step 14 waits for the Wi-Fi firmware's service (default 3)
 *   wifi_ask=1         step 14 also asks that service a question (its capabilities) and
 *                      writes down the beginning of an answer
 *   wifi_cal=1         step 15 asks for cold-boot calibration as ath11k does when it is on, and
 *                      waits up to a minute for it; wifi_ready=S: how long it waits for
 *                      "firmware ready" otherwise (default 5)
 *   wifi_mission=1     step 15 goes on to the configuration and the mode "mission"
 *   wifi_radio_read=1  step 16 does read (2: whatever mode the firmware is in)
 *   wifi_keep=1        when the steps are over the DSP and the link stay up while the system
 *                      runs, for whoever uses sys/hakios.qrtr.h; stopped when the run ends
 *   wifi_dirty=1       the DSP is not stopped: left as a kernel that died leaves it
 *   wifi_again=1       start the DSP although the one before it was shut down without having
 *                      acknowledged a stop: THAT HAS RESET THE PHONE, twice of two
 *   wifi_return=1      when all is over, go back to the loader instead of starting the system
 *   wifi_smmu=1        before any step: read what the boot chain has set up in the SMMU for the
 *                      radio's stream (smmu_look() below; nothing is written)
 *   wifi_smmu=2        ... and give that stream a context of the SMMU with page tables of ours,
 *                      so that the radio can be shown memory (radio_space_on() below: THE
 *                      SMMU IS WRITTEN); taken back when the DSP is stopped
 *   wifi_mac=A         the radio's own address for the driver (hakios_wifi_radio_address()
 *                      below; xl takes it from a file that is not in the repository)
 *   wifi_scan=1        with wifi_lx=2: once the driver is up, an interface in station mode is
 *                      made and the radio scans, listening only; what it heard goes to the log
 *                      (other people's networks by name and address: the log is not for
 *                      passing on)
 *   wifi_join=1        with wifi_lx=2: the station joins the network named in fw/network of
 *                      the small image (hakios_wifi_network() below) and the driver stays
 *                      up as the link behind en0; with wifi_keep=1 while the system runs,
 *                      else until the steps are over (wifi_hold=S: S seconds longer)
 *   wifi_station=1     with wifi_lx=2: the driver stays up and its station is the kernel's to
 *                      ask (sys/hakios.sta.h); nothing is joined by the driver itself
 *   wifi_file=0        with wifi=1: the network of fw/network is not joined at the start
 *   wifi_irq=1         the radio's interrupts (MSI through its SMMU context to the GIC) instead
 *                      of looking at its rings every millisecond (lx_bus.c); wifi=1 stands for it
 *   wifi_ba=1          block ack: the access point's requests to send aggregates are answered
 *                      (lx_80211.c); without, the link carries about a fifth; wifi=1 stands for it
 *   wifi_wide=1        the channel as wide as the access point offers (40 or 80 MHz) instead of
 *                      20 MHz; wifi=1 stands for it
 *   wifi_lx_step=MS    the driver's start says each stub it comes to and waits MS there: for
 *                      a run that ends without a word (follow it with XL_FOLLOW=2)
 *   wifi_lx=1          step 15 is ath11k's own instead of the stand-in (hakios.lx.c), with
 *                      the radio's registers neither read nor written; 2: for real (which
 *                      wants wifi_smmu=2: the radio is given addresses of its rings)
 * The system does not go on starting until all of this is over, so that the end of the log is
 * ours.
 *
 * No interrupt is taken: what the DSP and the always-on processor say is looked for. What an
 * interrupt would take is said at the end of this file.
 *
 * Addresses are from the phone's own device tree ("dt:" below: qcom,wpss@8a00000,
 * qcom,smp2p-wpss, qcom,glink/wpss, qcom,ipcc@408000, power-controller@c300000, rsc@18200000,
 * qcom,wcn6750, reserved-memory), layouts from Linux, where SC7280 is this SoC:
 * drivers/soc/qcom/{smem,smp2p,qcom_aoss,rpmh-rsc,cmd-db,mdt_loader}.c,
 * drivers/mailbox/qcom-ipcc.c, drivers/remoteproc/qcom_q6v5{,_pas}.c,
 * drivers/firmware/qcom_scm{,-smc}.c, drivers/spmi/spmi-pmic-arb.c,
 * drivers/regulator/qcom_spmi-regulator.c, drivers/pinctrl/qcom/pinctrl-msm.c.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qrtr.h>
#include <sys/hakios.sta.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);

static LCK_GRP_DECLARE(wifi_lock_group, "hakios-wifi");

/*
 * What the audio DSP's start (hakios.adsp.c) uses as well, while this may be at work beside it:
 * the mailbox to the always-on processor, one message at a time, and the allocation of items in
 * the shared memory (whose hardware lock both take under the same number, so it does not keep
 * the two apart).
 */
LCK_MTX_DECLARE(hakios_aop_mailbox_lock, &wifi_lock_group);
LCK_MTX_DECLARE(hakios_smem_alloc_lock, &wifi_lock_group);
LCK_MTX_DECLARE(hakios_rsc_lock, &wifi_lock_group);      /* RPMh's sequencer (TCS 0 of the RSC): the audio DSP's and the modem's drivers both ask through it */
/* ... and the sound's (hakios.sound.c): a free match register and context of the SMMU, found and taken */
LCK_MTX_DECLARE(hakios_smmu_lock, &wifi_lock_group);

static uint32_t last_step, pause_ms = 2200, fine, watch_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";

/* As in hakios.adreno.c: every line goes to the log the loader reads from another core as well. */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static LCK_MTX_DECLARE(say_lock, &wifi_lock_group);      /* two threads talk here: a line at a time */
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * With wifi=1 what the steps and the driver say is not for the log of an ordinary session: it is
 * kept here, the last of it, and shown only if the radio does not come up. What that log is for
 * is said with hakios_wifi_tell().
 */
static bool quiet;
static char kept[64 * 1024];
static uint32_t kept_at;
static bool kept_wrapped;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (quiet && !aloud) {
		for (const char *c = line; *c != 0; c++) {
			kept[kept_at] = *c;
			if (++kept_at == sizeof(kept)) {
				kept_at = 0;
				kept_wrapped = true;
			}
		}
	} else {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		printf("%s", line);
	}
	lck_mtx_unlock(&say_lock);
}

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

/* (for hakios.wlfw.c and the driver's side, which say their lines the same way) */
int
hakios_wifi_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return 0;
}

/* A line for whoever runs the phone, not only for whoever looks for a fault: said also when the rest is kept. */
int
hakios_wifi_tell(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return 0;
}
#define tell hakios_wifi_tell

/* The last of what was kept, into the log after all: the radio has not come up. */
static void
say_kept(void)
{
	uint32_t length = kept_wrapped ? sizeof(kept) : kept_at, show = length < 24 * 1024 ? length : 24 * 1024;
	char piece[201];

	quiet = false;
	for (uint32_t i = length - show; i < length; i += sizeof(piece) - 1) {
		uint32_t n = length - i < sizeof(piece) - 1 ? length - i : sizeof(piece) - 1;

		for (uint32_t k = 0; k < n; k++) {
			piece[k] = kept[(kept_wrapped ? kept_at + i + k : i + k) % sizeof(kept)];
		}
		piece[n] = 0;
		say_line(piece, true);
	}
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

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* ---- options --------------------------------------------------------------------------------------- */

static bool whole;                      /* wifi=1: all of it, by itself (see the top of this file) */
static volatile bool whole_runs;        /* ... and the thread that does it is at work */

/*
 * A number given to the kernel when it was started; with wifi=1 the ones that stands for are as if
 * given, unless given otherwise (wifi_irq=0: the rings looked at as before).
 */
uint32_t
hakios_wifi_option(const char *name)
{
	static const struct {
		const char *name;
		uint32_t value;
	} stands_for[] = { { "wifi_lx", 2 }, { "wifi_smmu", 2 }, { "wifi_station", 1 }, { "wifi_keep", 1 }, { "wifi_irq", 1 },
			   { "wifi_ba", 1 }, { "wifi_wide", 1 } };
	uint32_t value = 0;

	if (strcmp(name, "wifi_quiet") == 0) {
		return quiet;
	}
	if (PE_parse_boot_argn(name, &value, sizeof(value))) {
		return value;
	}
	for (unsigned i = 0; whole && i < sizeof(stands_for) / sizeof(stands_for[0]); i++) {
		if (strcmp(name, stands_for[i].name) == 0) {
			return stands_for[i].value;
		}
	}
	return 0;
}

/* ---- register access ------------------------------------------------------------------------------- */

/* One access is one plain load or store (see hakios.adreno.c: the hypervisor carries some of them out). */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
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
	if (step >= new_from && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

/* What a step is about to do, in a line. */
static void
announce(const char *what)
{
	printf("wifi: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step >= new_from && fine) {
		printf("wifi: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("wifi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("wifi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	hold(true);
	wr(b->base + offset, value);
}

/* ---- step 1: the shared memory area ------------------------------------------------------------------- */

/*
 * dt: reserved-memory smem@80900000 (qcom,smem). At its end, in the last 4K, the boot loader's
 * table of partitions: one per pair of processors that share items privately, and one, the
 * "global" one, for the rest. A partition begins with a header; items are allocated from both
 * of its ends, each behind (from the top: in front of) an entry that says which item it is and
 * how long. Who is who: we are processor 0; the Wi-Fi DSP is 13 (dt: qcom,remote-pid of
 * qcom,smp2p-wpss). Nothing here writes to this memory.
 */
#define SMEM_PHYS               0x80900000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_WPSS               13
#define SMEM_GLOBAL             0xfffe          /* both "hosts" of the global partition */
#define SMEM_VERSION            (0x40 + 4 * 7)  /* of the old header at the start: the boot loader's word, [31:16] */
#define SMEM_VERSION_PARTITIONS 12              /* ... which is this where everything is in partitions */
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];                       /* "$TOC" */
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];                       /* "$PRT" */
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;                          /* with the padding */
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
/* An item found: where its data is in the area, and how long. */
struct smem_item {
	uint32_t item, offset, size;
};

/* The DSP's items (dt: qcom,smem of qcom,smp2p-wpss, qcom,smem-id of qcom,wpss@8a00000). */
#define ITEM_SMP2P_OURS         616             /* SMP2P from us to the DSP: ours to allocate, when there is something to say */
#define ITEM_SMP2P_THEIRS       617             /* ... and from the DSP to us */
#define ITEM_CRASH_REASON       626             /* a line of text the DSP leaves when it gives up */

static const volatile uint8_t *smem;

/* A piece of the area, copied: other processors write there while we look. */
static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		/* memory, not registers; and from the kernel's own map: where ml_io_map() puts things there are 8 MB in all */
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

/* A word of the area, written in one store: other processors read there while we write. */
static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

/* The partition of two processors: where it begins in the area, and how its entries are aligned. 0: there is none. */
static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

/* The items of a partition, up to `max` of them; how many there are. */
static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	/* from the bottom: an entry, then its item */
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	/* from the top: an entry, with its item in front of it; entries are aligned as the table says */
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_WPSS }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

/* An SMP2P item: who it is from and to, and up to 16 named words. */
struct smp2p {
	uint32_t magic;                         /* "$SMP" */
	uint32_t version;                       /* [7:0]; [31:8] features: bit 0, "restarts are acknowledged" */
	uint16_t local, remote, total, valid;
	uint32_t flags;                         /* bit 0 "restart done", bit 1 "restart acknowledged" */
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u              /* the feature: a side that restarts says so in its flags, the other answers in its own */
#define SMP2P_RESTART_DONE      (1u << 0)       /* flags: toggled by the side that has restarted */
#define SMP2P_RESTART_ACK       (1u << 1)       /* ... and made equal to it by the other */
/* where the words of an item are, for writing them one at a time */
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12              /* entries there is room for; << 16: entries that are valid */
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,wpss@8a00000). */
#define DSP_FATAL               (1u << 0)       /* "err-fatal": it has given up */
#define DSP_READY               (1u << 1)       /* "err-ready": it is up, and would say so if it gave up */
#define DSP_HANDOVER            (1u << 2)       /* "proxy-unvote": it has asked for its own power; ours is no longer needed */
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* The DSP's SMP2P item as it is now, if there is one. */
static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;                     /* (an item, once allocated, stays where it is) */
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

/* Which of an item's entries has that name; -1: none. */
static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* The word of the DSP's entry "slave-kernel". */
static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("wifi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

/*
 * The DSP's last words, if it has left any: its firmware makes the item when it starts, empty,
 * and writes a line into it when it gives up.
 */
static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("wifi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("wifi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("wifi: %u: THE DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* A partition, in two or more lines. */
static void
show_partition(const char *name, uint16_t one, uint16_t other)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(one, other, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("wifi: %u: there is no partition for %s\n", step, name);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("wifi: %u: the partition for %s: at %#lx, %#x bytes, of which %#x free; %u items\n", step, name, SMEM_PHYS + partition,
	    size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("wifi: %u:   items (bytes):%s\n", step, line);
	}
}

static bool
step_smem(void)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smp2p words;
	uint32_t version = 0, size = 0, at;

	announce("map the shared memory area (0x80900000, 2 MB: memory, not registers) and read how the boot loader laid it out");
	if (!smem_peek(SMEM_VERSION, &version, sizeof(version)) || !smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table))) {
		return false;
	}
	printf("wifi: %u: the boot loader's version word is %#x; the table of partitions begins %02x %02x %02x %02x, version %u, "
	    "%u entries\n", step, version, table.magic[0], table.magic[1], table.magic[2], table.magic[3], table.version, table.count);
	if (version >> 16 != SMEM_VERSION_PARTITIONS || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 || table.count > 64) {
		printf("wifi: %u: that is not the layout this knows (version 12, everything in partitions)\n", step);
		return false;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			printf("wifi: %u: partition %u: processors %#x and %#x, at %#lx, %#x bytes, flags %#x, alignment %u\n", step, i,
			    entry.host0, entry.host1, SMEM_PHYS + entry.offset, entry.size, entry.flags, entry.cacheline);
		}
	}
	show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	show_partition("everybody (the global one)", SMEM_GLOBAL, SMEM_GLOBAL);

	at = smem_item(ITEM_SMP2P_OURS, &size);
	printf("wifi: %u: item %u, SMP2P from us to the DSP: %s\n", step, ITEM_SMP2P_OURS, at ? "there" : "not there");
	show_crash_reason();
	if (dsp_words(&words)) {
		show_words("item 617, SMP2P from the DSP to us, IS THERE (the DSP has run since the phone was started)", &words);
	} else {
		printf("wifi: %u: item %u, SMP2P from the DSP to us: not there (the DSP has not run since the phone was started)\n", step,
		    ITEM_SMP2P_THEIRS);
	}
	return true;
}

/* ---- step 2: the always-on processor's mailbox, and the doorbells -------------------------------------- */

/*
 * dt: power-controller@c300000 (qcom,sm8350-aoss-qmp; "qmp" is the name of the protocol): 1K
 * of message memory that takes 32-bit accesses only. It begins with a description of the two
 * directions -- "ucore" is the always-on processor, "mcore" is us -- each with a link state, a
 * channel state, the other side's acknowledgement of both, and where and how big its mailbox
 * is. A message is text; it goes into our mailbox behind its length, the doorbell is rung, and
 * the always-on processor has taken it when the length reads zero.
 */
static struct block aoss = { "mailbox", 0x0c300000, 0x400 };
#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c      /* "LIAM" */
#define QMP_VERSION             0x04
#define QMP_FEATURES            0x08
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10            /* ours to write: we have seen their link state */
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18            /* ours to write */
#define QMP_THEIR_BOX_SIZE      0x1c
#define QMP_THEIR_BOX           0x20
#define QMP_OUR_LINK            0x24            /* ours to write */
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c            /* ours to write */
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64              /* what Linux sends, whatever the text's length */

/*
 * dt: qcom,ipcc@408000 (its interrupt: SPI 229). A doorbell is a processor ("client") and one
 * of its signals; clients: the always-on processor 0, the Wi-Fi DSP 24; signals: 0 the mailbox
 * or GLINK, 2 SMP2P (dt: the mboxes of power-controller@c300000, qcom,smp2p-wpss, glink wpss).
 */
static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };
#define IPCC_SEND               0x0c            /* write client << 16 | signal: ring it */
#define IPCC_RECEIVED           0x10            /* who rang us, the same way; all ones: nobody */
#define IPCC_ENABLE             0x14            /* write: hear that client's signal */
#define IPCC_DISABLE            0x18            /* write: no longer */
#define IPCC_CLEAR              0x1c            /* write: that one has been heard */
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_WPSS               24u
#define IPCC_SMP2P              2u              /* the signal */

static bool
step_mailbox(void)
{
	uint32_t magic, box, size;

	announce("read the description at the start of the always-on processor's message memory (0xc300000)");
	magic = get(&aoss, "magic", QMP_MAGIC);
	get(&aoss, "version", QMP_VERSION);
	get(&aoss, "features", QMP_FEATURES);
	get(&aoss, "their link", QMP_THEIR_LINK);
	get(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK);
	get(&aoss, "their channel", QMP_THEIR_CHANNEL);
	get(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK);
	get(&aoss, "their mailbox's size", QMP_THEIR_BOX_SIZE);
	get(&aoss, "their mailbox", QMP_THEIR_BOX);
	get(&aoss, "our link", QMP_OUR_LINK);
	get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK);
	get(&aoss, "our channel", QMP_OUR_CHANNEL);
	get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	if (magic != QMP_MAGIC_VALUE) {
		printf("wifi: %u: that is not the mailbox's description\n", step);
		return false;
	}
	if (size >= QMP_MESSAGE + 4 && box >= 0x3c && box + size <= aoss.size) {
		get(&aoss, "the length of the message in our mailbox", box);
	}

	announce("read who has rung our doorbell (IPCC, 0x408000: the first access to that block)");
	get(&ipcc, "received", IPCC_RECEIVED);
	return true;
}

/* ---- step 3: RPMh -------------------------------------------------------------------------------------- */

/* dt: reserved-memory cmd_db@80860000: the boot loader's list of what RPMh can switch (cmd-db.c; as in hakios.adreno.c) */
#define CMD_DB_PHYS             0x81c60000UL     /* dt: aop_cmd_db_region@81c60000 (redwood's 0x80860000 is not this soc) */
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

static bool
cmd_db_there(void)
{
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);       /* memory, not registers */
	}
	return cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c;
}

/* The resource of that name, or (name NULL) the one an address belongs to: the nearest at or below it. */
static const struct cmd_db_entry *
rpmh_resource(const char *name, uint32_t address)
{
	const struct cmd_db_entry *best = NULL;
	char id[8] = { 0 };

	if (!cmd_db_there()) {
		return NULL;
	}
	if (name != NULL) {
		strlcpy(id, name, sizeof(id));
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (name != NULL ? memcmp(entry[i].id, id, sizeof(id)) == 0 :
			    entry[i].address <= address && address - entry[i].address < 0x10 &&
			    (best == NULL || entry[i].address > best->address)) {
				best = &entry[i];
				if (name != NULL) {
					return best;
				}
			}
		}
	}
	return best;
}

static void
show_rpmh_resource(const char *name, const char *what)
{
	const struct cmd_db_entry *entry = rpmh_resource(name, 0);
	const uint8_t *data;
	char levels[160];
	int at = 0;

	if (entry == NULL) {
		printf("wifi: %u: RPMh has no \"%s\" (%s)\n", step, name, what);
		return;
	}
	levels[0] = 0;
	if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".lvl") == 0) {
		/* a rail asked for by level: the levels it has, which a request names by position */
		for (unsigned s = 0; s < 8; s++) {
			const struct cmd_db_slave *slave = &cmd_db->slave[s];
			const struct cmd_db_entry *first = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

			if (entry < first || entry >= first + slave->count ||
			    (unsigned)slave->data_offset + entry->offset + entry->length > CMD_DB_SIZE - 0x1000) {
				continue;
			}
			data = cmd_db->data + slave->data_offset + entry->offset;
			at = snprintf(levels, sizeof(levels), "; levels:");
			for (unsigned l = 0; l < entry->length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
				unsigned level = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && level == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
			}
		}
	}
	printf("wifi: %u: RPMh has \"%s\" at %#x (%s)%s\n", step, name, entry->address, what, levels);
}

/*
 * dt: rsc@18200000 (qcom,rpmh-rsc), the application processor's sequencer for requests to RPMh;
 * of its three sets of registers ("DRV") the operating system's is the third (qcom,drv-id 2).
 * Requests wait in "TCS"s of up to 16 commands, a command being an address of RPMh's and a
 * value: by the device tree two TCSs for requests made at once, then three for what is to
 * hold while the processor sleeps and three for when it wakes. What the slots hold now is what
 * was last put there by whoever ran here before us: the boot loader.
 */
static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_ID                  0x000           /* [23:16] the major version: the layout below is that of versions before 3 */
#define RSC_CONFIG              0x00c           /* per DRV six bits, the number of its TCSs; [31:27] commands in a TCS */
#define RSC_TCS(n)              (0xd00 + 672 * (n))             /* dt: qcom,tcs-offset */
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c            /* a bit per command */
#define TCS_COMMAND(c)          (0x30 + 20 * (c))               /* its flags; then its address, its value, its status */

static bool
step_rpmh(void)
{
	static const char *const kinds[] = { "at once", "at once", "asleep", "asleep", "asleep", "awake", "awake", "awake" };
	uint32_t id, config, count, commands;

	announce("look up the supplies involved in the boot loader's list of RPMh resources (memory at 0x80860000)");
	if (!cmd_db_there()) {
		printf("wifi: %u: the list is not there\n", step);
		return false;
	}
	show_rpmh_resource("cx.lvl", "the DSP's logic; dt: vdd_cx of qcom,wpss@8a00000, asked 384 while it boots");
	show_rpmh_resource("mx.lvl", "its memories; dt: vdd_mx, the same");
	show_rpmh_resource("xo.lvl", "the crystal oscillator; dt: its clock \"xo\"");
	show_rpmh_resource("smpb7", "PM7325 S7: the radio's vdd-cx-mx");
	show_rpmh_resource("smpb1", "PM7325 S1: the radio's vdd-1.8-xo");
	show_rpmh_resource("smpb8", "PM7325 S8: the radio's vdd-1.3-rfa");

	announce("read the application processor's RPMh sequencer (0x18220000: the first access to that block): its version, "
	    "and the commands its slots hold");
	id = get(&rsc, "id", RSC_ID);
	config = get(&rsc, "configuration", RSC_CONFIG);
	if (((id >> 16) & 0xff) >= 3) {
		printf("wifi: %u: version %u of the sequencer has its registers elsewhere: not read\n", step, (id >> 16) & 0xff);
		return true;
	}
	count = (config >> 12) & 0x3f;
	commands = (config >> 27) & 0x1f;
	printf("wifi: %u: the operating system's part has %u TCSs of %u commands\n", step, count, commands);
	for (uint32_t t = 0; t < count && t < 8; t++) {
		uint32_t enabled = rd(rsc.base + RSC_TCS(t) + TCS_ENABLED);

		printf("wifi: %u: TCS %u (%s): commands enabled %#x, control %#x, status %#x\n", step, t, kinds[t], enabled,
		    rd(rsc.base + RSC_TCS(t) + TCS_CONTROL), rd(rsc.base + RSC_TCS(t) + TCS_STATUS));
		for (uint32_t c = 0; c < commands && c < 16; c++) {
			uint32_t flags = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c));
			uint32_t address = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 4);
			uint32_t value = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 8);
			const struct cmd_db_entry *entry = rpmh_resource(NULL, address);

			if (address == 0 && !(enabled & 1u << c)) {
				continue;
			}
			printf("wifi: %u:   command %u%s: %#x (%.8s + %u) = %#x, flags %#x\n", step, c, enabled & 1u << c ? "" :
			    " (not enabled)", address, entry ? entry->id : "?", entry ? address - entry->address : 0, value, flags);
		}
	}
	return true;
}

/* ---- step 4: the supplies themselves ------------------------------------------------------------------- */

/*
 * The PMIC arbiter, as in hakios.buttons.c: reads go through the "observer" channels, which
 * every system has for every peripheral, whoever owns it. dt: qcom,spmi@c440000; on the bus the
 * PM7325 is slave 1 and the PM8350C slave 2.
 */
static struct block arb_core = { "arbiter", 0x0c440000, 0x1100 };
static struct block arb_cnfg = { "arbiter's owners", 0x0c40a000, 0x1000 };
static struct block arb_observer = { "arbiter's observer", 0x0e600000, 0x10000 };
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what follows is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */

static uint32_t arb_channels;

/* The channel of a peripheral (slave << 8 | peripheral), as Linux chooses: the first, unless a later one is ours. -1: none. */
static int
spmi_channel(uint16_t id)
{
	int found = -1;

	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core.base + ARB_CHANNEL(n));

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		if (found < 0 || (rd(arb_cnfg.base + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
			found = (int)n;
		}
	}
	return found;
}

/* Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what went wrong. */
static const char *
spmi_read(int channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t at = arb_observer.base + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)channel;
	uint64_t start = now_us();
	uint32_t status, data[2];

	wr(at + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(at + OBSERVER_STATUS);
	} while (!(status & STATUS_DONE) && now_us() - start < 2000);
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	}
	if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	}
	if (status & STATUS_FAILURE) {
		return "the transfer failed (nobody at that address?)";
	}
	if (status & STATUS_DROPPED) {
		return "the transfer was dropped";
	}
	data[0] = rd(at + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(at + OBSERVER_DATA1) : 0;
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/*
 * Where the PMICs have their regulators is not in the device tree (on this phone they are
 * RPMh's to switch, and the tree knows them by RPMh's names only), so the arbiter's table is
 * asked which peripherals each PMIC has, and each peripheral what it is (registers 0x04 and
 * 0x05, its type and subtype). Of a regulator's control (qcom_spmi-regulator.c, the kinds
 * "ftsmps426" and "hfs430" that PMICs of this generation continue): 0x08 its status (bit 7: its
 * output is in regulation), 0x40/0x41 the voltage asked for in millivolts, 0x42/0x43 the voltage
 * it makes, 0x45 its mode (the low three bits: 4 retention, 5 low power, 6 automatic, 7 full
 * power), 0x46 bit 7: enabled. Whether a rail that is on was left on by the boot loader for good
 * or is held by somebody's request to RPMh, these registers do not say.
 */
#define TYPE_BUCK               0x03            /* a switching regulator's control */
#define TYPE_FTS                0x1c            /* ... of the fast kind */
#define TYPE_ULT_BUCK           0x22

static bool
step_rails(void)
{
	uint32_t version;

	announce("map the PMIC arbiter (0xc440000, 0xc40a000, 0xe600000; the buttons' driver reads through it too) and read "
	    "its version");
	version = get(&arb_core, "version", ARB_VERSION);
	arb_channels = get(&arb_core, "features", ARB_FEATURES) & 0x7ff;
	need(&arb_cnfg);
	need(&arb_observer);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("wifi: %u: this knows version 5 of the arbiter only: the supplies are not read\n", step);
		return true;
	}
	if (arb_channels == 0 || arb_channels > 512) {
		arb_channels = 512;
	}

	announce("ask every peripheral of the PM7325 (slave 1) and the PM8350C (slave 2) what it is, and the switching "
	    "regulators among them what they make (the first transfers of ours on the SPMI bus: for each a command word "
	    "written to the arbiter, nothing to a PMIC)");
	for (uint16_t slave = 1; slave <= 2; slave++) {
		static uint8_t types[256][2];
		static bool there[256];
		char line[320];
		int at = 0;

		bzero(there, sizeof(there));
		for (uint32_t n = 0; n < arb_channels; n++) {
			uint32_t map = rd(arb_core.base + ARB_CHANNEL(n)), peripheral = (map >> 8) & 0xff;

			if (map == 0 || ((map >> 16) & 0xf) != slave || there[peripheral]) {
				continue;
			}
			there[peripheral] = spmi_read((int)n, 0x04, types[peripheral], 2) == NULL;
		}
		line[0] = 0;
		for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
			if (there[peripheral]) {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x:%x/%x", peripheral, types[peripheral][0],
				    types[peripheral][1]);
			}
			if (at > 0 && (at > (int)sizeof(line) - 80 || peripheral == 255)) {
				printf("wifi: %u: slave %u (%s), peripheral:type/subtype:%s\n", step, slave, slave == 1 ? "PM7325" : "PM8350C",
				    line);
				at = 0;
			}
		}
		for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
			int channel;
			uint8_t status[2], control[8];
			const char *problem;

			if (!there[peripheral] || (types[peripheral][0] != TYPE_BUCK && types[peripheral][0] != TYPE_FTS &&
			    types[peripheral][0] != TYPE_ULT_BUCK)) {
				continue;
			}
			channel = spmi_channel((uint16_t)(slave << 8 | peripheral));
			problem = channel < 0 ? "no channel" : spmi_read(channel, 0x08, status, 2);
			if (problem == NULL) {
				problem = spmi_read(channel, 0x40, control, 8);
			}
			if (problem != NULL) {
				printf("wifi: %u: slave %u, the regulator at %#x: %s\n", step, slave, peripheral << 8, problem);
				continue;
			}
			printf("wifi: %u: slave %u, the regulator at %#x (type %#x/%#x): %s, status %#x %#x (%s); asked %u mV, makes %u "
			    "mV; mode %#x; registers 0x40..0x47: %02x %02x %02x %02x %02x %02x %02x %02x\n", step, slave, peripheral << 8,
			    types[peripheral][0], types[peripheral][1], control[6] & 0x80 ? "ENABLED" : "NOT ENABLED", status[0], status[1],
			    status[0] & 0x80 ? "in regulation" : "not in regulation", (unsigned)(control[0] | control[1] << 8),
			    (unsigned)(control[2] | control[3] << 8), control[5], control[0], control[1], control[2], control[3], control[4],
			    control[5], control[6], control[7]);
		}
	}
	return true;
}

/* ---- step 5: pins -------------------------------------------------------------------------------------- */

/*
 * dt: pinctrl@f000000 (pin n's registers: 0x1000 * n above 0xf100000). Pin 84 is described there
 * as the radio's enable ("cnss_wlan_en": output, high when active), but no node uses the
 * description; Bluetooth's node calls the same pin "wl-reset" and has 85 as its own reset and 86
 * as its "sw-ctrl".
 *
 * (The DSP's clocks -- three in the global clock controller have its name: gcc-sc7280.c,
 * gcc_wpss_ahb_clk at 0x9d154 and two more -- are not looked at: a read there is answered with
 * an external abort, which ends the kernel. They are TrustZone's; the device tree gives the
 * loader's node the crystal alone.)
 */
#define FIRST_PIN               84
static struct block tlmm = { "tlmm", 0x0f100000 + 0x1000 * FIRST_PIN, 0x3000 };        /* the three pins' registers */
#define GPIO_CFG(n)             (0x1000 * ((n) - FIRST_PIN))            /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (0x1000 * ((n) - FIRST_PIN) + 4)        /* bit 0 in, bit 1 out */

static bool
step_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { 84, "the radio's enable or reset" }, { 85, "Bluetooth's reset" }, { 86, "Bluetooth's sw-ctrl" } };

	announce("read how the firmware left the radio's pins (registers the touch screen's driver uses too)");
	need(&tlmm);
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(tlmm.base + GPIO_CFG(pins[i].pin)), io = rd(tlmm.base + GPIO_IO(pins[i].pin));

		printf("wifi: %u: gpio%u (%s): configuration %#x: function %u, pull %u, %u mA, %s; reads %u\n", step, pins[i].pin,
		    pins[i].what, cfg, (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & 1u << 9) ? (io & 2 ? "DRIVES HIGH" :
		    "DRIVES LOW") : "an input", io & 1);
	}
	return true;
}

/* ---- step 6: the firmware ------------------------------------------------------------------------------ */

#define PAS_WPSS                6               /* dt: qcom,pas-id of qcom,wpss@8a00000 */
#define WPSS_MEMORY             0x9ae00000u     /* dt: reserved-memory wlan@9ae00000, that node's memory-region */
#define WPSS_MEMORY_SIZE        0x01900000u

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);


/*
 * A file of fw/ in the small image that travels with the kernel -- userland/build.sh puts what
 * is in fw-wifi/ there --, found in the memory of the RAM disk itself (see hakios.adreno.c).
 */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (hakios.image.c) */

	if (data != NULL) {
		return data;
	}
	printf("wifi: %u: no fw/%s in the small image: put the phone's /vendor/firmware_mnt/image/%s into fw-wifi/ and run "
	    "userland/build.sh\n", step, name, name);
	return NULL;
}

/*
 * The image as its header describes it (mdt_loader.c): a 32-bit ELF whose header, table of
 * segments and block of hashes and certificates are the file wpss.mdt, and whose loadable
 * segments are a file each, wpss.bNN for segment NN. The segments are "relocatable": they go
 * where the memory reserved for the DSP is, which on this phone is where they say they are.
 */
#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)      /* the segment that is the block of hashes */
#define MAX_SEGMENTS            16

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;                    /* in the DSP's memory */
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;                          /* of the memory all of them take, from the start of the DSP's */
} image;

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the DSP's image in the small image that came with the kernel, and check it against its own header");
	image.segments = 0;
	header = image_file("wpss.mdt", &size);
	if (header == NULL) {
		return false;
	}
	/* a 32-bit ELF: the count of segments at 44, the table of them at 52, 32 bytes each */
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("wifi: %u: fw/wpss.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;                       /* (not one that is loaded) */
		}
		if (segment.paddr < WPSS_MEMORY || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)WPSS_MEMORY + WPSS_MEMORY_SIZE) {
			printf("wifi: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			return false;
		}
		image.segment[image.segments].at = segment.paddr - WPSS_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "wpss.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				return false;
			}
			if (file_size != segment.file_size) {
				printf("wifi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				return false;
			}
			files += file_size;
		}
		printf("wifi: %u: segment %u: %#x bytes at %#x, of which %#x from its file\n", step, i, segment.memory_size, segment.paddr,
		    segment.file_size);
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != WPSS_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("wifi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		image.segments = 0;
		return false;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - WPSS_MEMORY;
	printf("wifi: %u: the image is whole: a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x bytes "
	    "of the DSP's %#x from %#x\n", step, size, hashes, image.segments, files, image.size, WPSS_MEMORY_SIZE, WPSS_MEMORY);
	return true;
}

/* ---- step 7: the mailbox, opened ----------------------------------------------------------------------- */

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: the mailbox is not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring(uint32_t client, uint32_t signal, const char *whose)
{
	put(&ipcc, whose, IPCC_SEND, client << 16 | signal);
}

static struct {
	uint32_t glink, smp2p, mailbox, other;  /* rings heard, by kind */
	uint32_t said;                          /* how many have been written to the log */
	uint64_t since;
} doorbell;

/*
 * Who has rung us (see the end of this file for what IPCC is seen to do): each pending doorbell
 * is read and told that it has been heard, which is what an interrupt handler would do before
 * anything else. The first few are written to the log.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		if (who == (IPCC_WPSS << 16)) {
			doorbell.glink++;
		} else if (who == (IPCC_WPSS << 16 | IPCC_SMP2P)) {
			doorbell.smp2p++;
		} else if (who == (IPCC_AOP << 16)) {
			doorbell.mailbox++;
		} else {
			doorbell.other++;
		}
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("wifi: doorbell: +%llu.%03llu ms: client %u, signal %u; heard and cleared\n", now / 1000, now % 1000, who >> 16,
			    who & 0xffff);
		}
	}
}

/*
 * Linux's qmp_open(), a step at a time: acknowledge their link, raise ours and wait for their
 * acknowledgement, raise our channel and wait for theirs, acknowledge that and wait for their
 * acknowledgement of ours. Whatever of it has been done already -- by a kernel before this one:
 * the always-on processor is restarted only with the phone -- is found done and left.
 */
static bool step_mailbox_open_locked(void);

static bool
step_mailbox_open(void)
{
	bool open;

	lck_mtx_lock(&hakios_aop_mailbox_lock);
	open = step_mailbox_open_locked();
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return open;
}

static bool
step_mailbox_open_locked(void)
{
	uint32_t box, size;

	announce("open the mailbox to the always-on processor: words of the message memory, and its doorbell");
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("wifi: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("wifi: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (get(&aoss, "our link", QMP_OUR_LINK) == QMP_UP && get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK) == QMP_UP &&
	    get(&aoss, "our channel", QMP_OUR_CHANNEL) == QMP_UP && get(&aoss, "their channel", QMP_THEIR_CHANNEL) == QMP_UP &&
	    get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("wifi: %u: the mailbox is open already (a kernel before this one opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, get(&aoss, "their link", QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second (it says %#x); our link is "
		    "taken down again\n", step, rd(aoss.base + QMP_OUR_LINK_ACK));
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	printf("wifi: %u: the always-on processor has seen our link\n", step);
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second (it says %#x); ours is taken down "
		    "again\n", step, rd(aoss.base + QMP_THEIR_CHANNEL));
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second (it says %#x); it is taken "
		    "down again\n", step, rd(aoss.base + QMP_OUR_CHANNEL_ACK));
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	printf("wifi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN; the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()). */
static bool mailbox_send_locked(const char *text);

static bool
mailbox_send(const char *text)
{
	bool sent;

	if (leaving) {
		return mailbox_send_locked(text);       /* (one processor left, and no lock taken: see hakios_wifi_quiesce()) */
	}
	lck_mtx_lock(&hakios_aop_mailbox_lock);
	sent = mailbox_send_locked(text);
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return sent;
}

static bool
mailbox_send_locked(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };

	if (our_box == 0) {
		return false;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("wifi: %u: about to tell the always-on processor \"%s\": %u bytes into our mailbox at %#x, its length in "
	    "front, and the doorbell\n", step, text, QMP_MESSAGE, aoss.phys + our_box);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	hold(true);
	if (rd(aoss.base + our_box) != 0) {
		printf("wifi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		return false;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		return false;
	}
	printf("wifi: %u: the always-on processor has taken it\n", step);
	return true;
}

/* ---- SMP2P, our half ----------------------------------------------------------------------------------- */

/*
 * Our item (616) is made by step 10, in our partition with the DSP, and like everything in the
 * shared memory it then stays until the phone is restarted: a kernel after this one finds it.
 * Our entries in it (dt: the children of qcom,smp2p-wpss that are not interrupt controllers):
 * "master-kernel", whose bit 0 asks the DSP to stop (dt: "qcom,force-stop" of qcom,wpss@8a00000),
 * and "wlan", whose bit 0 is the radio driver's to tell the firmware that we sleep. A change is
 * followed by a ring of the DSP's SMP2P doorbell.
 */
#define OUR_ENTRY_STOP          "master-kernel"
#define OUR_ENTRY_RADIO         "wlan"
#define DSP_STOP                (1u << 0)       /* of "master-kernel" */

static void glink_reset(void);
static void link_stop(void);

static uint32_t our_item;               /* where our SMP2P item is in the area; 0: not looked for, or not there */
static bool smp2p_agreed;               /* the DSP's item and ours have the same version: what follows applies */
static bool smp2p_restart_acks;         /* ... and both sides have the feature */
static bool smp2p_restart_acked;        /* the DSP's "restart done" as we have last answered it */

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/*
 * What Linux does when the DSP rings for SMP2P (smp2p.c: qcom_smp2p_intr), done instead whenever
 * the DSP's item is seen to have changed: agree on the features once, and answer a restart.
 */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			printf("wifi: %u: about to write our item's features at %#lx: %#x (what both sides have)\n", step,
			    SMEM_PHYS + our_item + SMP2P_AT_VERSION, features);
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("wifi: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("wifi: %u: the DSP's flags say it has restarted (%#x): about to answer in our flags at %#lx (bit 1 = %u) and ring "
		    "its SMP2P doorbell\n", step, theirs->flags, SMEM_PHYS + our_item + SMP2P_AT_FLAGS, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_SMP2P);
	}
}

/*
 * Linux's qcom_q6v5_request_stop(): our bit "stop" set, the DSP's "stop ack" waited for (five
 * seconds there too), our bit taken back. It is asked of a DSP that says it is up and has said
 * neither that it has given up nor that it has stopped, through whatever item of ours is in the
 * shared memory -- this kernel's or, for a DSP left running by a kernel before, that kernel's.
 * True: the DSP has stopped by its own word, now or (its entry still says so) before.
 */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("wifi: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("wifi: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("wifi: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("wifi: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("wifi: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx (it holds %#x), and its SMP2P doorbell\n",
	    step, OUR_ENTRY_STOP, SMEM_PHYS + value_at, ours.entry[entry].value);
	snprintf(last_write, sizeof(last_write), "step %u asked the DSP to stop", step);
	hold(true);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("wifi: %u: %s after %llu.%03llu ms; its entry says %#x, its flags %#x\n", step, state & DSP_STOP_ACK ?
	    "THE DSP ACKNOWLEDGES THE STOP" : "THE DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000, (now - asked) % 1000, state,
	    theirs.flags);
	/* (taken back, as Linux does, before TrustZone is asked) */
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

/* ---- step 8: TrustZone takes the header ---------------------------------------------------------------- */

/* The calls, as in hakios.adreno.c (qcom_scm.c: qcom_scm_pas_*; convention: qcom_scm-smc.c). */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* two: a number, and the address of memory it may read and write */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */

static LCK_MTX_DECLARE(dsp_lock, &wifi_lock_group);     /* the DSP being started or stopped */
static bool dsp_started;                /* TrustZone has let it out of reset, and has not shut it down since */
static bool stopped_by_its_word;        /* the shutdown last asked for was of a DSP that had acknowledged a stop */
static uint32_t dirty;

/* One call, as Linux makes these: one that may be preempted. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("wifi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
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
			printf("wifi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("wifi: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

/*
 * The DSP is asked to stop, if it can be; then, whatever it says, TrustZone is to shut it down
 * (it takes the image's memory back, cleared), and the always-on processor to know that the
 * image is gone: it was told of one by us, or by the kernel that left the DSP running.
 */
static void radio_space_off(void);

static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;
	bool ours = dsp_started;                /* (not the DSP of a kernel before this one, shut down before ours is started) */

	printf("wifi: %u: %s: the DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		hakios_lx_stop();                      /* (the driver, if it stayed up: it leaves its network and goes down in order) */
		hakios_wlfw_stop(step);                /* (what the stand-in of step 15 has to say first, while there is a link) */
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the Wi-Fi DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_WPSS, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT SHUT THE DSP DOWN: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: THE WI-FI DSP IS SHUT DOWN\n", step);
	wr(ipcc.base + IPCC_DISABLE, IPCC_WPSS << 16 | IPCC_SMP2P);     /* (as the boot chain left it) */
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
	/*
	 * Nobody is left to use the radio's stream. Only after a DSP of ours: with wifi=1 the radio
	 * is shown its memory before the DSP is started, and the shutdown of an earlier kernel's
	 * DSP, which comes in between, took that away again -- the radio's first reach for memory,
	 * at mission mode, was then a stream the SMMU knows nothing of, and the phone went down
	 * without a word: every second start, and never a first.
	 */
	if (ours) {
		radio_space_off();
	}
	return true;
}

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	uint32_t again = 0;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		/* (see the top of this file: it has run since the phone was started, and may be running) */
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down a DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !(PE_parse_boot_argn("wifi_again", &again, sizeof(again)) && again)) {
			printf("wifi: %u: THE DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and a start "
			    "after that has reset the phone (see the top of hakios.wifi.c; wifi_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("wifi: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started, and "
		    "there is nothing to shut down\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 6), in a page of ours");
	/* in memory of ours, page-aligned and in one piece, not touched again and never freed (see hakios.adreno.c) */
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));       /* TrustZone reads memory, not this processor's cache */
	}
	settle();
	result = scm("to take the Wi-Fi DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_WPSS, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: TRUSTZONE ACCEPTS THE WI-FI DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_WPSS, WPSS_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, WPSS_MEMORY);
	return true;
}

/* ---- step 9: the image, in the DSP's memory ------------------------------------------------------------ */

static bool image_written;

/* Eight bytes of a segment as they are to be in the DSP's memory, from byte `at` of it: the file's, then zeroes. */
static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the DSP's memory (0x9ae00000, 25 MB: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	/*
	 * (The memory is looked at only now: TrustZone has just said that it is not its own. And it
	 * is mapped for as long as this takes: 25 MB do not fit where ml_io_map() puts what stays.)
	 */
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(WPSS_MEMORY, WPSS_MEMORY_SIZE, VM_WIMG_WCOMB);
	printf("wifi: %u: the memory begins %#llx, the image %#llx\n", step, dsp_memory[0], segment_word(0, 0));
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;

		if ((image.segment[s].at | image.segment[s].memory_size) & 7) {
			printf("wifi: %u: a segment that does not begin and end at a multiple of eight bytes: not written\n", step);
			return false;
		}
		printf("wifi: %u: about to write %#x bytes at %#x (%#x of them the file's)\n", step, image.segment[s].memory_size,
		    WPSS_MEMORY + image.segment[s].at, image.segment[s].file_size);
		snprintf(last_write, sizeof(last_write), "step %u wrote a segment of the image at %#x", step,
		    WPSS_MEMORY + image.segment[s].at);
		hold(true);
		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			to[i / 8] = segment_word(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;

		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
	}
	printf("wifi: %u: %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step, written, cleared,
	    wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, WPSS_MEMORY_SIZE);
	image_written = wrong == 0;
	return image_written;
}

/* ---- step 10: our SMP2P item --------------------------------------------------------------------------- */

/*
 * The SoC's lock for allocating in the shared memory: dt: syscon@1f40000 and the hwlock
 * "qcom,tcsr-mutex" on it (0x1000 a lock), of which qcom,smem has the fourth ("hwlocks": 3). A
 * register that takes the number of whoever writes it while it holds zero, and keeps it until
 * zero is written (qcom_hwspinlock.c). The application processor's number is 1. It is held
 * around the allocation only, and released on every way out; Linux waits a second for it,
 * and so does this: a lock that somebody keeps is said, not waited for.
 */
static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one. False, and why said: not made. */
static bool our_item_make_locked(uint32_t partition);

static bool
our_item_make(uint32_t partition)
{
	bool made;

	lck_mtx_lock(&hakios_smem_alloc_lock);
	made = our_item_make_locked(partition);
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

static bool
our_item_make_locked(uint32_t partition)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (sizeof(struct smp2p) + 7) & ~7u, need = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	owner = get(&smem_lock, "owner", LOCK_OWNER);
	printf("wifi: %u: about to take the lock (write %u at %#x, until it reads back so, for a second at most), write an entry "
	    "of %u bytes for item %u with %u bytes behind it where the partition's items from the bottom end, move the "
	    "partition's mark by %u, and release the lock (write 0)\n", step, LOCK_US, smem_lock.phys + LOCK_OWNER,
	    (uint32_t)sizeof(entry), ITEM_SMP2P_OURS, item_size, need);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	hold(true);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("wifi: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		return false;
	}
	/* under the lock: the partition as it is now, every entry in it from the bottom, and the room left */
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("wifi: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == ITEM_SMP2P_OURS ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("wifi: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need > header.free_cached) {
		printf("wifi: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need, header.free_cached - at);
		goto out;
	}
	/* the entry first, whole; only then the mark, in one store: nobody sees half of either */
	smem_poke(partition + at, SMEM_CANARY | ITEM_SMP2P_OURS << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - (uint32_t)sizeof(struct smp2p));     /* padding behind the item; none in front */
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("wifi: %u: the lock is released (it reads %u)%s\n", step, rd(smem_lock.base + LOCK_OWNER), made ? "; the entry is at "
	    "the old mark, the mark moved" : "");
	return made;
}

static bool
step_our_item(void)
{
	static const char *const names[] = { OUR_ENTRY_STOP, OUR_ENTRY_RADIO };
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition;

	announce("look at our partition with the DSP in the shared memory, and for an SMP2P item of ours (616) in it");
	partition = smem_partition(SMEM_US, SMEM_WPSS, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("wifi: %u: there is no such partition\n", step);
		return false;
	}
	show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	printf("wifi: %u: its items from the bottom end at +%#x, those from the top begin at +%#x\n", step, header.free_uncached,
	    header.free_cached);
	our_item = our_item_find();
	if (our_item != 0) {
		printf("wifi: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		announce("make the item: the SoC's lock for the shared memory (0x1f43000: the first access to that block) around it");
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("wifi: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("wifi: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
		show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	}

	announce("fill our item in (340 bytes at the address above): zeroes, then from processor 0 to 13, room for 16 entries, "
	    "the entries \"master-kernel\" and \"wlan\" at zero, feature bit 0, and last the version, which makes it valid; "
	    "then the DSP's SMP2P doorbell, as Linux rings it");
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_WPSS << 16);
	for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t name[4] = { 0 };

		strlcpy((char *)name, names[i], sizeof(name));
		for (uint32_t word = 0; word < 4; word++) {
			smem_poke(our_item + SMP2P_AT_NAME(i) + 4 * word, name[word]);
		}
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | (uint32_t)(sizeof(names) / sizeof(names[0])) << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	ring(IPCC_WPSS, IPCC_SMP2P, "the DSP's SMP2P doorbell");
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("wifi: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);

	announce("see that our two marks in GLINK's descriptor are at zero before the DSP starts");
	glink_reset();
	return true;
}

/* ---- step 11: the DSP, started ------------------------------------------------------------------------- */

/*
 * Linux's order (qcom_q6v5_pas.c: adsp_start): the always-on processor is told, then TrustZone
 * asked. Linux also asks RPMh for the crystal and for CX and MX at level 384 until the DSP says
 * it has asked for its own power ("proxy" requests, dt: qcom,proxy-reg-names); that is not done
 * here -- no supply is touched from this file --, and the DSP starts on what the boot loader
 * left: level 256 for both (step 3). Then the shared memory and our doorbell are watched, every
 * 200 microseconds: for the DSP's item to appear, for every change in it, for a ring; and what
 * SMP2P's rules have us answer is answered (dsp_listen()).
 *
 * What has been seen: 6.5 ms after TrustZone returns the DSP rings our doorbell for GLINK. At
 * 32.5 ms its SMP2P item is there (on a later start in the same session of the phone: is
 * written afresh, with "restart done" in its flags, which we answer) -- version 1, feature bit
 * 0, from processor 13 to 0, the entries "smp2p", "slave-kernel" and soon "wlan". At 50 to 60 ms
 * "slave-kernel" says 6: it is up, and has asked for its own power (Android's kernel sees the
 * same word 39 ms after its start). Without an item of ours -- before step 10 existed -- the
 * DSP's item stayed without entries for as long as it was watched.
 * Asked to stop, the DSP answers within 13 ms: "slave-kernel" becomes 0x10e, bit 3 the
 * acknowledgement.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	bool rung;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written || our_box == 0) {
		printf("wifi: %u: the image is not in place, or the mailbox not open\n", step);
		return false;
	}
	PE_parse_boot_argn("wifi_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	if (have) {
		show_words("before the start, the DSP's item says (from its last run; believed again once it changes)", &last);
	}
	announce("tell the doorbells that rings from before this kernel have been heard and that the DSP's SMP2P ring is "
	    "listened for, tell the always-on processor that the image is loaded, and ask TrustZone to authenticate it and let "
	    "the DSP out of reset");
	/* (nobody answered them, and a ring after the start is to be the DSP's) */
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	/* (the boot chain listens for the DSP's GLINK ring and no other: without this an SMP2P ring leaves no trace) */
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_WPSS << 16 | IPCC_SMP2P);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!mailbox_send("{class: image, res: load_state, name: wpss, val: on}")) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the Wi-Fi DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_WPSS, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;                  /* (whatever the answer: the memory is not ours to look at now) */
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT START THE DSP: the call returns %d, its answer is %#llx\n", step, result, answer);
		mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("wifi: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE WI-FI DSP OUT OF RESET; watching the shared memory "
	    "and our doorbell for %u s\n", step, wait_seconds);

	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = true;
			spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + watch_seconds * 1000000ull;
				printf("wifi: %u: THE WI-FI DSP SAYS IT IS UP: its entry \"slave-kernel\" says %#x (bit 1: \"error ready\"), "
				    "%llu.%03llu ms after TrustZone started it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("wifi: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start\n", step,
				    (now - started) / 1000, (now - started) % 1000);
			}
			if (state & DSP_FATAL) {
				printf("wifi: %u: THE WI-FI DSP SAYS IT HAS GIVEN UP (bit 0 of \"slave-kernel\")\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	if (have) {
		show_words("at the end of the watch the DSP's item says", &last);
	}
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("and ours", &words);
	}
	rung = doorbell.glink + doorbell.smp2p != 0;
	printf("wifi: %u: the watch is over after %llu ms; doorbells heard: GLINK %u, SMP2P %u, the mailbox %u, others %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at != 0) {
		printf("wifi: %u: the DSP said it was up%s\n", step, handed_over ? ", and that it has asked for its own power" : "");
	} else if (spoke) {
		printf("wifi: %u: THE WI-FI DSP RUNS BUT HAS NOT SAID \"UP\": it has %s%s, with %u entries\n", step, rung ?
		    "rung our doorbell" : "", have ? (rung ? " and has its item in the shared memory" :
		    "its item in the shared memory") : "", last.valid);
	} else {
		printf("wifi: %u: NOT A WORD FROM THE DSP: no ring, and %s\n", step, have ? "its item in the shared memory is as a "
		    "kernel before this one left it" : "no item of its in the shared memory");
	}
	return ready_at != 0 || spoke;
}

/* ---- GLINK ------------------------------------------------------------------------------------------- */

/*
 * GLINK between us and the DSP is two queues of bytes in the shared memory and a descriptor
 * (Linux: qcom_glink_smem.c). Items of our partition with the DSP: 478 the descriptor, four
 * words; 479 our queue, which we write and the DSP reads (both made by the boot chain: they are
 * there before anything of ours runs); 480 the DSP's queue, which it makes when it starts. Each
 * side moves the head of its own queue and the tail of the other's, and rings the other's
 * GLINK doorbell when it has moved one. Our two marks are put to zero before the DSP starts
 * (step 10): it reads our queue from the start when it comes up.
 *
 * What goes through the queues (qcom_glink_native.c) are messages of eight bytes -- a command
 * and two parameters -- some with more behind them, each padded to eight: first the two sides'
 * versions, then channels by name, each side giving a channel a number of its own. To send on
 * a channel a side needs an "intent" of the other's: the other's promise of a buffer of some
 * size, with a number; a message names the intent it uses, and the receiver says when it is
 * done with it and whether it may be used again. A side without a fitting intent asks for one.
 *
 * The DSP opens three channels, and we answer one: "IPCRTR", which carries QRTR (below).
 */
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0               /* the descriptor's words: how far the DSP has read our queue (its to move) */
#define GLINK_OUR_HEAD          4               /* how far we have written it */
#define GLINK_THEIR_TAIL        8               /* how far we have read the DSP's queue */
#define GLINK_THEIR_HEAD        12              /* how far it has written it (its to move) */
#define GLINK_RESERVE           16              /* of our queue, never filled (Linux's reserve, and room for one short message) */
#define IPCC_GLINK              0u              /* the signal */

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u              /* the feature Linux offers over this transport */
#define GLINK_CHANNEL           "IPCRTR"
#define GLINK_US                1               /* our number for that channel */

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {                           /* a whole message from the channel, waiting for the link's thread */
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

static LCK_MTX_DECLARE(glink_lock, &wifi_lock_group);   /* the queues' marks and everything below */
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;      /* where in the shared memory */
	bool up;                                /* the queues are found and ours to use */
	bool broken;                            /* something in the DSP's queue was not understood: nothing more is read */
	uint32_t features;
	bool agreed;                            /* the versions are settled */
	uint32_t their_channel;                 /* the DSP's number for IPCRTR; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
	uint32_t said, heard, trace;            /* messages each way; how many more are written to the log */
} glink;

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

/* The three items; false if one is missing or not of a shape this knows. */
static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

/* Our two marks to zero, if they are not: before the DSP starts (see above). */
static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("wifi: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("wifi: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("wifi: %u: about to write our two marks there (+4, +8): 0\n", step);
		hold(true);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

/* How much of the DSP's queue is unread. */
static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

/* How much we may write into our queue now. */
static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

/* A message into our queue, in two pieces, padded to eight bytes; then our mark, and the DSP's doorbell. */
static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

/* A short message of ours. */
static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("wifi: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

/* An intent of ours of that size: a buffer, and the DSP told of it. False: there are no more. */
static bool
glink_offer(uint32_t size)
{
	uint32_t words[2] = { size, glink.our_intent_count + 1 }, i = glink.our_intent_count;

	if (i >= sizeof(glink.our_intents) / sizeof(glink.our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (glink.our_intents[i].data == NULL || glink.our_intents[i].size != size) {
		glink.our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);       /* (kept for good: a later link uses it again) */
	}
	if (glink.our_intents[i].data == NULL) {
		return false;
	}
	glink.our_intents[i].size = size;
	glink.our_intents[i].fill = 0;
	glink.our_intent_count++;
	glink_say(GLINK_INTENT, GLINK_US, 1, words, sizeof(words));
	return true;
}

/*
 * Everything the DSP has put into its queue, taken out and acted on: Linux's
 * qcom_glink_native_rx(). With glink_lock held; whoever waits for something from the DSP calls
 * this, so it must not wait itself. Whole messages for the channel are left in a list for the
 * link's thread.
 */
static void
glink_pump(void)
{
	static const struct {
		uint32_t size, count;
	} intents[] = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } };           /* dt: qcom,intents of qcom,wpss_qrtr */
	struct glink_message message;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);                /* the size of this piece, and what is still to come */
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("wifi: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;                                 /* (the rest of it is on its way) */
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE) {
			glink.trace--;
			printf("wifi: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			if (strcmp(name, GLINK_CHANNEL) != 0) {
				printf("wifi: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				break;
			}
			printf("wifi: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, GLINK_US);
			glink.their_channel = message.param1;
			glink_say(GLINK_OPEN_ACK, glink.their_channel, 0, NULL, 0);
			if (!glink.asked) {
				bzero(name, sizeof(name));
				strlcpy(name, GLINK_CHANNEL, sizeof(name));
				glink_say(GLINK_OPEN, GLINK_US, sizeof(GLINK_CHANNEL), name, (sizeof(GLINK_CHANNEL) + 7) & ~7u);
				glink.asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_US) {
				glink.acknowledged = true;
			}
			break;
		case GLINK_INTENT:
			for (uint32_t i = 0; i < message.param2 && message.param1 == glink.their_channel; i++) {
				glink_peek(words, 8 + 8 * i, 8);        /* a size and a number */
				for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
					if (!glink.their_intents[n].there || glink.their_intents[n].id == words[1]) {
						glink.their_intents[n].there = true;
						glink.their_intents[n].in_use = false;
						glink.their_intents[n].size = words[0];
						glink.their_intents[n].id = words[1];
						break;
					}
				}
				if (glink.trace > 0) {
					printf("wifi: glink:   the DSP will take %u bytes into its intent %u\n", words[0], words[1]);
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
				if (glink.their_intents[n].there && glink.their_intents[n].id == message.param2) {
					glink.their_intents[n].in_use = false;
					glink.their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if (message.param1 == glink.their_channel) {
				bool granted = glink_offer(message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, GLINK_US, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			glink.request_answered = true;
			glink.request_granted = message.param2 != 0;
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			if (message.param1 != glink.their_channel || intent >= glink.our_intent_count ||
			    words[0] > glink.our_intents[intent].size - glink.our_intents[intent].fill) {
				printf("wifi: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(glink.our_intents[intent].data + glink.our_intents[intent].fill, 16, words[0]);
			glink.our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				/* whole: into the list, and the intent is the DSP's to use again */
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + glink.our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = glink.our_intents[intent].fill;
					memcpy(packet->data, glink.our_intents[intent].data, packet->length);
					if (glink.last != NULL) {
						glink.last->next = packet;
					} else {
						glink.first = packet;
					}
					glink.last = packet;
				}
				glink.our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, GLINK_US, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if (message.param1 == glink.their_channel) {
				printf("wifi: glink: THE DSP CLOSES \"%s\"\n", GLINK_CHANNEL);
				glink.open = false;
				glink.their_channel = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("wifi: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (glink.their_channel != 0 && glink.acknowledged && !glink.open) {
			glink.open = true;
			printf("wifi: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
			    GLINK_CHANNEL, GLINK_US, glink.their_channel);
			for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
				for (unsigned n = 0; n < intents[i].count; n++) {
					glink_offer(intents[i].size);
				}
			}
		}
	}
}

/*
 * One whole message to the DSP on the channel (Linux's __qcom_glink_send(), for messages that
 * go in one piece). Waits, up to five seconds, for an intent of the DSP's that it fits and
 * for room in our queue, asking for the intent if there is none.
 */
static int
glink_send(const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, GLINK_US, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !glink.open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
			if (!glink.their_intents[n].there || glink.their_intents[n].size < length) {
				continue;
			}
			fits = true;                            /* (one that is in use comes back: that is waited for) */
			if (!glink.their_intents[n].in_use && (found < 0 || glink.their_intents[n].size < glink.their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			glink.request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, GLINK_US, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && glink.request_answered && !glink.request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	glink.their_intents[found].in_use = true;
	header.message.param2 = glink.their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR -------------------------------------------------------------------------------------------- */

/*
 * What the channel carries (Linux: net/qrtr/af_qrtr.c, ns.c, smd.c): datagrams from a port of a
 * node to a port of a node, one in each GLINK message, behind a header of eight words. A node's
 * services announce themselves to the other nodes through a port that all have, the control
 * port: when a link comes up each side says hello, and is told the other's services one by one
 * ("new server": a service, an instance, a node, a port), and later their going. We are node 1
 * (Linux's default, and what this phone's Android is); the DSP says who it is in what it sends.
 * The interface for the rest of the kernel is sys/hakios.qrtr.h: the four calls at the end of
 * this section, handed to the QMI helpers (bsd/dev/hakios.qmi.c), which is what speaks to the
 * services.
 */
#define HAKIOS_QRTR_MAX        8192            /* the most a datagram holds, either way */
#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu     /* the control port */
#define QRTR_FIRST_PORT         0x4000u         /* where Linux begins numbering ports that ask for no number */
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7 };

struct qrtr_header {                            /* version 1, which Linux sends; version 2 is read as well */
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];                      /* a server: service, instance, node, port; a client: node, port */
};

struct hakios_qrtr_port {
	uint32_t number;                        /* 0: free */
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* the instance as on the wire: version | instance << 8; 0: any */
		bool asked;
		bool fresh;                     /* not yet told of the services that were there before it */
	} lookups[4];
};

static LCK_MTX_DECLARE(qrtr_lock, &wifi_lock_group);    /* the tables below */
static LCK_MTX_DECLARE(qrtr_calling, &wifi_lock_group); /* held by the link's thread around a callback */
static struct hakios_qrtr_port qrtr_ports[16];
static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer;                      /* the DSP's node; 0: it has not said */
static uint32_t qrtr_next_port = QRTR_FIRST_PORT, qrtr_in, qrtr_out, qrtr_hellos;
static thread_t link_thread_is;

/* One datagram out, of any type. */
static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	if (length == 0 || length > HAKIOS_QRTR_MAX) {
		return EMSGSIZE;
	}
	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static bool
qrtr_matches(uint32_t service, uint32_t instance, unsigned server)
{
	return qrtr_servers[server].there && qrtr_servers[server].service == service &&
	       (instance == 0 || qrtr_servers[server].instance == instance);
}

/* Everybody who looked a service up is told that it has come, or gone. From the link's thread. */
static void
qrtr_tell(uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server = NULL;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number != 0 && qrtr_ports[p].lookups[l].asked && !qrtr_ports[p].lookups[l].fresh &&
			    qrtr_ports[p].lookups[l].service == service &&
			    (qrtr_ports[p].lookups[l].instance == 0 || qrtr_ports[p].lookups[l].instance == instance)) {
				server = qrtr_ports[p].server;
			}
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			if (server != NULL) {
				server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
			}
		}
	}
}

/* The control port's part: Linux's name service, without services of our own to tell of. */
static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;                                  /* (answered once: two that answer every hello never end) */
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("wifi: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		lck_mtx_lock(&qrtr_lock);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		lck_mtx_unlock(&qrtr_lock);
		printf("wifi: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		qrtr_tell(control->words[0], control->words[1], control->words[2], control->words[3], true);
		break;
	case QRTR_DEL_SERVER:
	case QRTR_BYE:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			bool gone;

			lck_mtx_lock(&qrtr_lock);
			gone = qrtr_servers[i].there && (control->command == QRTR_BYE ? qrtr_servers[i].node == header->from_node :
			    qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]);
			qrtr_servers[i].there = qrtr_servers[i].there && !gone;
			lck_mtx_unlock(&qrtr_lock);
			if (gone) {
				printf("wifi: qrtr: service %u at node %u port %u has gone\n", qrtr_servers[i].service, qrtr_servers[i].node,
				    qrtr_servers[i].port);
				qrtr_tell(qrtr_servers[i].service, qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port, false);
			}
		}
		break;
	default:
		break;                                  /* (a client's going, and what else there is: nothing here wants it) */
	}
}

/* A message from the channel: one datagram. From the link's thread. */
static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	hakios_qrtr_receive_t receive = NULL;
	void *context = NULL;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		/* version 2: version, type, flags, a length of options; the size; then four numbers of 16 bits */
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("wifi: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("wifi: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;                                 /* (we never ask for one: GLINK's intents hold us back already) */
	}
	if (header.confirm) {
		/* the sender counts what it has sent to this port, and waits for this before it sends many more */
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].number == header.to_port && header.type == QRTR_DATA) {
			receive = qrtr_ports[p].receive;
			context = qrtr_ports[p].context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (receive != NULL) {
		receive(context, header.from_node, header.from_port, data + at, header.size);
	}
}

/* Lookups made since the last look are told of the services that were there before them. From the link's thread. */
static void
qrtr_fresh(void)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server;
			uint32_t service, instance;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number == 0 || !qrtr_ports[p].lookups[l].fresh) {
				lck_mtx_unlock(&qrtr_lock);
				continue;
			}
			qrtr_ports[p].lookups[l].fresh = false;
			server = qrtr_ports[p].server;
			service = qrtr_ports[p].lookups[l].service;
			instance = qrtr_ports[p].lookups[l].instance;
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && server != NULL; i++) {
				if (qrtr_matches(service, instance, i)) {
					server(context, true, qrtr_servers[i].service, qrtr_servers[i].instance & 0xff,
					    qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port);
				}
			}
		}
	}
}

static struct hakios_qrtr_port *
qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct hakios_qrtr_port *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL; p++) {
		if (qrtr_ports[p].number == 0) {
			port = &qrtr_ports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > HAKIOS_QRTR_MAX) {
		return EINVAL;
	}
	return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static int
qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = true;
			port->lookups[l].fresh = true;
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}

static void
qrtr_close(struct hakios_qrtr_port *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };
	bool outside = current_thread() != link_thread_is;

	if (port == NULL || port->number == 0) {
		return;
	}
	if (outside) {
		lck_mtx_lock(&qrtr_calling);            /* (a callback of the port's that is running ends first) */
	}
	lck_mtx_lock(&qrtr_lock);
	port->number = 0;
	lck_mtx_unlock(&qrtr_lock);
	if (outside) {
		lck_mtx_unlock(&qrtr_calling);
	}
	if (qrtr_peer != 0) {
		qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
	}
}

static const struct hakios_qrtr_ops qrtr_ops = { .open = qrtr_open, .send = qrtr_send, .lookup = qrtr_lookup, .close = qrtr_close };

/* ---- the link's thread -------------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;
/*
 * Until the DSP may have said something. Nothing tells us today, so: a millisecond. This is the
 * one function to change when the doorbells' interrupt can be taken (the end of this file): it
 * then blocks until the handler has seen a ring of the DSP's.
 */
static void
link_wait(void)
{
	sleep_us(1000);
}

/* Everything the DSP says once it runs: its SMP2P item, its queue, and from that the datagrams, handed on from here. */
static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		link_wait();
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("wifi: link: THE WI-FI DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = glink.first;
			if (packet != NULL) {
				glink.first = packet->next;
				glink.last = glink.first == NULL ? NULL : glink.last;
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			lck_mtx_lock(&qrtr_calling);
			qrtr_input(packet->data, packet->length);
			lck_mtx_unlock(&qrtr_calling);
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		lck_mtx_lock(&qrtr_calling);
		qrtr_fresh();
		lck_mtx_unlock(&qrtr_calling);
	}
	link_runs = false;
	thread_terminate(current_thread());
}

/* The link given up, before the DSP is stopped: Linux too takes GLINK down first and tells the DSP nothing of it. */
static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("wifi: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams; doorbells heard: "
		    "GLINK %u, SMP2P %u, the mailbox %u, others %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = glink.open = false;
	while ((packet = glink.first) != NULL) {
		glink.first = packet->next;
		kfree_data(packet, sizeof(*packet) + packet->length);
	}
	glink.last = NULL;
	lck_mtx_unlock(&glink_lock);
	lck_mtx_lock(&qrtr_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
	lck_mtx_unlock(&qrtr_lock);
}

/* ---- step 12: GLINK's queues, looked at --------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues in the shared memory, and read what the DSP has put into its own "
	    "(nothing is taken out)");
	if (!glink_find()) {
		printf("wifi: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("wifi: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("wifi: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;                                  /* (how long the others are, the reading below knows) */
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 13: GLINK, spoken ---------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the DSP: our version into our queue and its doorbell, then whatever its messages call for "
	    "(writes: our queue, our two marks in the descriptor, the doorbell); a thread of ours listens from here on, "
	    "every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.asked = glink.acknowledged = glink.open = glink.broken = false;
	glink.their_channel = glink.our_intent_count = glink.said = glink.heard = 0;
	bzero(glink.their_intents, sizeof(glink.their_intents));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("wifi: %u: no thread\n", step);
		return false;
	}
	link_thread_is = thread;
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);                                   /* (for the intents each side tells the other of, to be in the log) */
	printf("wifi: %u: %llu ms after our version: versions %s (features %#x); the channel \"%s\" is %s\n", step,
	    (now_us() - began) / 1000, glink.agreed ? "agreed" : "NOT AGREED", glink.features, GLINK_CHANNEL, glink.open ?
	    "OPEN" : "NOT OPEN");
	return glink.open;
}

/* ---- step 14: QRTR ------------------------------------------------------------------------------------- */

#define SERVICE_WLFW            0x45            /* the Wi-Fi firmware's service, the one the radio's driver talks to */

static uint32_t wlfw_node, wlfw_port;
static volatile uint32_t wlfw_answer;           /* bytes of an answer received; 0: none */
static uint8_t wlfw_answer_bytes[64];

static void
wlfw_service(__unused void *context, bool arrived, __unused uint32_t service, __unused uint32_t version, __unused uint32_t instance,
    uint32_t node, uint32_t port)
{
	printf("wifi: qrtr: THE WI-FI FIRMWARE'S SERVICE (%#x) %s node %u port %u\n", SERVICE_WLFW, arrived ? "IS AT" : "has left",
	    node, port);
	wlfw_node = arrived ? node : 0;
	wlfw_port = arrived ? port : 0;
}

static void
wlfw_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	printf("wifi: qrtr: a datagram of %lu bytes from node %u port %u\n", (unsigned long)length, node, port);
	memcpy(wlfw_answer_bytes, data, length < sizeof(wlfw_answer_bytes) ? length : sizeof(wlfw_answer_bytes));
	wlfw_answer = (uint32_t)length;
}

/* ---- step 15: the exchange with the Wi-Fi firmware's service ---------------------------------------- */

static const uint8_t *image_file(const char *name, uint64_t *size);

const uint8_t *
hakios_wifi_file(const char *name, uint64_t *size)
{
	return image_file(name, size);
}

static void radio_space_faults(void);

/*
 * With wifi_lx: ath11k itself in place of the stand-in. Its qmi.c has the exchange, core.c asks
 * for mission mode with the configuration ce.c and hal.c work out (the shadow registers), has
 * them set the copy engines' rings up in the radio's registers and waits for the firmware's
 * first message over a copy engine; how far the parts built so far go from there, and what is
 * in their place meanwhile, is wifi/lx/src/lx_bus.c's to say. wifi_lx=2 IS WHERE THE RADIO'S
 * REGISTERS ARE WRITTEN.
 *
 * What the phone said (13 October, ath11k of Linux 7.0.14: core.c, hal.c, qmi.c, ce.c, hw.c,
 * fw.c): with wifi_lx=1 the driver's own exchange runs to the end in 9 s -- capabilities, the
 * regulatory database in 4 pieces and the board's file in 10, cold-boot calibration (7 s),
 * "ready", the configuration with the 30 shadow registers the driver works out, mission mode
 * -- and the mode "off" when the driver winds down. With wifi_lx=2 and wifi_smmu=2, 100 words
 * of ring set-up go into the registers of copy engines 0, 1, 2, 3, 4, 5 and 7 without an
 * abort, 510 buffers are posted on each of the three receiving rings, and 9 ms later the
 * firmware's first message is in a buffer of copy engine 2: HTC's "ready" (2 credits of 2176
 * bytes, 5 endpoints). The SMMU records no fault. No interrupt is involved: the rings are
 * looked at every millisecond.
 *
 * With htc.c and wmi.c built as well (the same day): 29 ms after the bus is started HTC has the
 * firmware's "ready" and asks for the WMI service -- the first thing sent over a copy engine;
 * it is given endpoint 1, up on copy engine 3 and down on 2 --, the firmware announces the
 * radio over WMI ("service ready", in three events, within 60 ms), takes the driver's
 * configuration and says "unified ready" 270 ms after the bus was started. Of the radio it
 * says: one, of two chains, for 2.4 and 5 GHz (2312..2732 and 4900..7125 MHz, and capabilities
 * for 6 GHz); the rules of the regulatory domain it starts in (CN: 2402..2482, 5170..5330,
 * 5735..5835 MHz); and no address of its own (all zeros: the tree's "use-nv-mac" has Android
 * give it one). Then the driver is taken down in order, mode "off" included.
 *
 * With mac.c, reg.c and peer.c built, and with wifi_scan=1 (the same day): the driver registers
 * its radio with what stands in for mac80211 (wifi/lx/src/lx_80211.c) -- 14 channels at
 * 2.4 GHz, 28 at 5, 60 at 6, of which the firmware's regulatory domain allows 13, 13 and none
 * --, takes the address it was given, starts the radio, makes an interface in station mode
 * and has the firmware scan those 26 channels, listening only. The scan takes 4.1 s (150 ms a
 * channel), and what the radio heard is in the log: 5 to 7 access points in three runs, at
 * 2.4 and at 5 GHz, the strongest at -40 dBm, the weakest at -84, noise at -108 to -111. The
 * firmware works a country out of what it hears and the driver sets it (after the scan). Then
 * the interface is removed, the radio stopped, and the driver's own removal runs to its end.
 * The data path is still stubs: the firmware scans without it.
 *
 * With the data path built (dp.c and its neighbours: 18 of the driver's files, no stubs) and
 * wifi_join=1 (the same day): the rings of the data path are set up -- 636 words into the
 * radio's registers before the bus is started, 44 MB of ours shown to the radio -- and HTC
 * connects the data service besides WMI. The station asks for the network it was given by
 * name in a scan (1.9 s), finds its access point (channel 36, -53 dBm; WPA2 with a passphrase,
 * CCMP), authenticates, associates (20 MHz, HT and VHT, two streams) and answers the access
 * point's handshake; 130 ms after the association both keys are with the firmware and frames
 * pass. With wifi_keep=1 and the system started on top, configd's DHCP has an address in 4 to
 * 6 s, names resolve, and pages are fetched over HTTPS: iOS on the network through the
 * phone's own radio. 20 MB come down in 10.5 s (15 Mbit/s) with one processor, the rings
 * looked at by a thread and the radio's buffers not behind the processor's cache; what of
 * that holds the rate down was not taken apart. With four processors the network works as
 * well. At the run's end the network is left, the driver removes itself and the DSP stops
 * when asked.
 *
 * As wifi=1, in the session the phone is ordinarily run in (four processors, the GPU drawing,
 * touch, every stock job on demand), the same day: all of the above on a thread beside the
 * system's start, which is not held up by it (its "first boot is finished" comes 23 to 26 s
 * after the first line from user space, with the radio and without); the network is joined 12 s
 * after the kernel began on the radio; a job of ours that is in every image has configd
 * configure en0 as soon as it appears. Safari's search field then shows the search engine's
 * suggestions as one types, which come over this network. (The pages themselves Safari
 * fetches -- the lock and the site's name appear -- and does not draw: the page is black, with
 * the GPU and without. That is not the network's doing.)
 *
 * A PHONE THAT RESETS WITHOUT A WORD: twice on the way, two seconds after calibration. The
 * driver's registration failed (the hash table peer.c needs was a stub that fails), and where
 * its start fails that late core.c asks for the mode "off" before it stops the bus; our thread
 * was then still writing ring pointers into registers that had stopped answering. The read at
 * step 16 outside mission mode was a bus error and a panic; this was neither, and the loader
 * went with it. The cause was not seen in a log (there was none): it is what the code does,
 * and with a hash table that works and the copy engines let go of before any request for a
 * mode other than mission (lx_bus.c: lx_qmi_send_request()) it has not happened again.
 */
static bool
step_driver(uint32_t how)
{
	int error;

	announce(how >= 2 ? "hand the radio to ath11k itself: its exchange with the firmware up to mission mode, then THE RADIO'S "
	    "REGISTERS ARE WRITTEN, by ce.c and hal.c: the copy engines' rings (every write is said as it is made)" :
	    "hand the firmware to ath11k itself: its own exchange up to mission mode; the radio's registers are neither read "
	    "nor written (wifi_lx=1), and nothing is written but datagrams");
	error = hakios_lx_start();
	radio_space_faults();
	printf("wifi: %u: the driver's start returns %d\n", step, error);
	return error == 0;
}

static bool
step_exchange(void)
{
	uint32_t driver = 0;

	if (!glink.open) {
		return false;
	}
	if ((driver = hakios_wifi_option("wifi_lx")) != 0) {
		return step_driver(driver);
	}
	announce("have the exchange a WCN6750's driver has with the Wi-Fi firmware's service before it touches the radio, by "
	    "hand in ath11k's order (hakios.wlfw.c): indications, capabilities both ways, the regulatory database and the "
	    "board's data; nothing is written but datagrams");
	return hakios_wlfw_exchange(step);
}

/* ---- step 16: the radio's registers, looked at ------------------------------------------------------- */

/*
 * The firmware says where the radio's registers are (step 15: 0x61e00000, 2 MB), and ath11k
 * maps that address as it is and reads and writes there: no SMMU is involved, it is the
 * processor's own view (qmi.c: ath11k_qmi_request_device_info(), devm_ioremap()). In the
 * phone's device tree the address is no device's: it lies in the memory window of the first
 * PCIe controller (pcie@1c00000: "ranges" 0x60300000 + 0x3d00000), whose node the tree keeps
 * for another use. Whether a kernel under this phone's hypervisor may read there is what this
 * step asks, with four reads and no write.
 *
 * THE ANSWER: YES, ONCE THE FIRMWARE IS IN MISSION MODE, AND ONLY THEN. With the firmware
 * calibrated and "ready" but in no mode, the first read ended the kernel with an asynchronous
 * abort -- a bus error, where the DSP's clocks, which the hypervisor keeps to itself, gave a
 * synchronous one -- and half a minute later the phone restarted by itself (the panic had left
 * the DSP running; a panic's way back now stops it, and with that the phone stays up). After
 * the configuration and the mode "mission" (step 15 with wifi_mission=1), which is when ath11k
 * begins to use the registers, all seven words below read: the copy engines' rings at zero,
 * copy engine 1's control 0x80, the reorder block's enable 0x3000150, and 0x40140110 where the
 * chip's PCI siblings have their hardware version. ath11k has no wake-up to make before an
 * access on this chip and no window register to write (ahb.c: the pci_ops for wcn6750 have
 * neither, and the windows are fixed). So the step reads only in mission mode, and only when
 * told to (wifi_radio_read=1; 2: in any state, which HAS ENDED THE KERNEL).
 *
 * The 2 MB are not the chip's registers one to one (ahb.c: ath11k_ahb_get_window_start_wcn6750()):
 * the first 512K are, the second 512K show the chip's 512K at 0xa00000 (the data path), the
 * third those at 0x1b80000 (the copy engines). The words read: the two ath11k itself reads
 * first when it sets the chip up (hal.c: the control word of copy engine 1's destination ring;
 * hw.c: the reorder block's "general enable"), the reorder ring's id word, and the word where
 * this chip's PCI siblings have their hardware version (pci.c: TCSR_SOC_HW_VERSION), which this
 * one may or may not have.
 */
#define RADIO_WINDOW            0x80000u
#define RADIO_DATA_PATH         0x00a00000u     /* hal.h: HAL_SEQ_WCSS_UMAC_OFFSET */
#define RADIO_COPY_ENGINES      0x01b80000u     /* hw.c: wcn6750_regs.hal_seq_wcss_umac_ce0_src_reg */

static bool
step_registers(void)
{
	/*
	 * The first: the word ath11k touches first once the firmware is in mission mode (it writes
	 * it: ce.c: ath11k_ce_init_pipes() begins with copy engine 0's source ring, and hal.c:
	 * ath11k_hal_srng_src_hw_init() with that ring's interrupt address). The rest only if the
	 * first answers: the ring's base and id, the first word ath11k reads (copy engine 1's
	 * destination ring control), the reorder block's two, and the hardware version of this
	 * chip's PCI siblings.
	 */
	static const struct {
		uint32_t chip;                  /* the register's address in the chip */
		const char *name;
	} words[] = {
		{ 0x01b80000 + 0x48, "copy engine 0, source ring, interrupt address (low)" },
		{ 0x01b80000 + 0x00, "copy engine 0, source ring, base (low)" },
		{ 0x01b80000 + 0x08, "copy engine 0, source ring, id" },
		{ 0x01b83000 + 0xb0, "copy engine 1, destination ring control" },
		{ 0x00a38000 + 0x000, "reorder block, general enable" },
		{ 0x00a38000 + 0x1f4, "reorder ring 1, id" },
		{ 0x00000224, "hardware version (if this chip has it where its siblings do)" },
	};
	static struct block radio = { "radio", 0, 0 };
	uint64_t address = 0;
	uint32_t size = 0, asked = 0;
	bool mission = false;

	if (!hakios_wlfw_registers(&address, &size, &mission) || address > 0xffffffffull || size < 3 * RADIO_WINDOW) {
		printf("wifi: %u: the firmware has not said where the radio's registers are (step 15)\n", step);
		return false;
	}
	PE_parse_boot_argn("wifi_radio_read", &asked, sizeof(asked));
	if (asked == 0 || (!mission && asked < 2)) {
		printf("wifi: %u: THE RADIO'S REGISTERS (%#llx) ARE NOT READ: %s (see the top of this step in hakios.wifi.c)\n", step,
		    address, asked == 0 ? "not asked for (wifi_radio_read=1)" : "the firmware is not in mission mode (wifi_mission=1), "
		    "and outside it a read there has ended the kernel with a bus error (wifi_radio_read=2 to read all the same)");
		return false;
	}
	announce("read the radio's registers where the firmware says they are, the word ath11k touches first before any other; "
	    "if nothing answers there the kernel ends at that read");
	printf("wifi: %u: the registers: %#x bytes at %#llx; the firmware %s\n", step, size, address, mission ? "is in mission mode" :
	    "IS NOT IN MISSION MODE (wifi_mission=1 gets it there): the state in which the read failed before");
	radio.phys = (uint32_t)address;
	radio.size = size;
	radio.base = ml_io_map_unmappable(radio.phys, radio.size, VM_WIMG_IO);
	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		uint32_t chip = words[i].chip, at;

		/* ahb.c: which of the three windows a register of the chip is seen through, and where in it */
		at = chip < RADIO_WINDOW ? chip : (chip ^ RADIO_DATA_PATH) < RADIO_WINDOW ? RADIO_WINDOW + (chip & (RADIO_WINDOW - 1)) :
		    2 * RADIO_WINDOW + (chip & (RADIO_WINDOW - 1));
		printf("wifi: %u: about to read the radio's %s (the chip's %#x) at %#x\n", step, words[i].name, chip, radio.phys + at);
		snprintf(last_write, sizeof(last_write), "step %u was reading the radio's registers at %#x", step, radio.phys + at);
		sleep_ms(i == 0 ? pause_ms : 50);
		printf("wifi: %u: THE RADIO'S %s (%#x) = %#x\n", step, words[i].name, radio.phys + at, rd(radio.base + at));
	}
	ml_io_unmap(radio.base, radio.size);
	return true;
}

static bool
step_qrtr(void)
{
	struct hakios_qrtr_port *port;
	uint32_t ask = 0, seconds = 3, services = 0;

	if (!glink.open) {
		return false;
	}
	PE_parse_boot_argn("wifi_listen", &seconds, sizeof(seconds));
	announce("listen for QRTR on the channel: the DSP's hello is answered with ours, its services are written down; and a "
	    "port of ours looks the Wi-Fi firmware's service up, as the radio's driver will");
	port = qrtr_open(wlfw_receive, wlfw_service, NULL);
	if (port == NULL || qrtr_lookup(port, SERVICE_WLFW, 1, 0) != 0) {
		printf("wifi: %u: no port\n", step);
		return false;
	}
	for (unsigned ms = 0; ms < seconds * 1000 && !(wlfw_port != 0 && ms >= 500); ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("wifi: %u: QRTR: the DSP is node %u; hellos from it %u; datagrams received %u, sent %u; services it has told of: "
	    "%u; the Wi-Fi firmware's service: %s\n", step, qrtr_peer, qrtr_hellos, qrtr_in, qrtr_out, services, wlfw_port != 0 ?
	    "THERE" : "NOT THERE");
	if (wlfw_port != 0 && PE_parse_boot_argn("wifi_ask", &ask, sizeof(ask)) && ask) {
		/*
		 * A question the service answers without having been told anything first? A QMI request is
		 * seven bytes before its content: 0 for "request", a number of ours for the exchange, the
		 * message's number, the content's length. 0x24 is "capabilities" and has no content.
		 */
		static const uint8_t question[7] = { 0, 0x01, 0x00, 0x24, 0x00, 0x00, 0x00 };
		int error;

		announce("ask the Wi-Fi firmware's service for its capabilities (QMI message 0x24, seven bytes) and wait a second "
		    "for an answer");
		wlfw_answer = 0;
		error = qrtr_send(port, wlfw_node, wlfw_port, question, sizeof(question));
		for (unsigned ms = 0; ms < 1000 && error == 0 && wlfw_answer == 0; ms++) {
			sleep_ms(1);
		}
		if (error != 0 || wlfw_answer == 0) {
			printf("wifi: %u: %s\n", step, error != 0 ? "THE QUESTION COULD NOT BE SENT" : "no answer within a second");
		} else {
			char text[3 * 48 + 1] = "";

			for (uint32_t i = 0; i < wlfw_answer && i < 48; i++) {
				snprintf(text + 3 * i, 4, " %02x", wlfw_answer_bytes[i]);
			}
			printf("wifi: %u: THE SERVICE ANSWERS: %u bytes, beginning%s\n", step, wlfw_answer, text);
		}
	}
	qrtr_close(port);
	return qrtr_hellos != 0 && qrtr_out != 0;
}

/* ---- beside the steps: the radio's stream in the SMMU (boot-arg wifi_smmu=1) ------------------------------ */

/*
 * What the radio reads and writes in memory by itself (its copy engines, the rings of the data
 * path) goes through the application processors' SMMU as stream 0x1c00, with the mask 1 (dt:
 * "iommus" of qcom,wcn6750; the SMMU is apps-smmu@15000000, an Arm MMU-500 like the GPU's).
 * Whether the boot chain leaves that stream passing untranslated, translated or faulting decides
 * what has to be made before the radio is given an address. This reads the SMMU's global
 * registers -- the ones the hypervisor answers for, see hakios.adreno.c -- and says which of
 * its stream match registers cover that stream. Nothing is written.
 */
static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
#define SMMU_SCR0               0x000           /* bit 0: the whole SMMU off; bit 10: a stream nobody matches faults; bit 3:
	                                         * streams are numbered with 16 bits */
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define RADIO_STREAM            0x1c00
#define RADIO_STREAM_MASK       0x0001

static void
smmu_look(void)
{
	static const char *const kinds[] = { "translated by context", "PASSED UNTRANSLATED (bypass; the number is", "MADE TO FAULT "
		                             "(the number is", "of a reserved kind (the number is" };
	uint32_t scr0, idr0, idr1, matches, valid = 0, covering = 0;

	printf("wifi: 0: read the global registers of the application processors' SMMU (0x15000000: the first access of ours to "
	    "that block; the hypervisor answers for them)\n");
	sleep_ms(pause_ms);
	scr0 = get(&apps_smmu, "scr0", SMMU_SCR0);
	idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	idr1 = get(&apps_smmu, "idr1", SMMU_IDR1);
	matches = idr0 & 0xff;
	printf("wifi: 0: the SMMU is %s; %u stream match registers, %u contexts; a stream nobody matches %s\n", scr0 & 1 ? "OFF" : "on",
	    matches, idr1 & 0xff, scr0 & 1u << 10 ? "faults" : "passes untranslated");
	for (uint32_t n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		/* with 16-bit stream numbers switched on "valid" moves from bit 31 of the one to bit 10 of the other */
		uint32_t wide = scr0 & 1u << 3, stream = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

		if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
			continue;
		}
		valid++;
		if (((stream ^ RADIO_STREAM) & ~(mask | RADIO_STREAM_MASK)) != 0) {
			continue;
		}
		printf("wifi: 0: match register %u covers the radio's stream: stream %#x mask %#x (%#x), %s %u; s2cr %#x)\n", n, stream,
		    mask, smr, kinds[(s2cr >> 16) & 3], s2cr & 0xff, s2cr);
		covering++;
	}
	printf("wifi: 0: %u of the %u match registers are in use; %u cover stream %#x%s\n", valid, matches, covering, RADIO_STREAM,
	    covering ? "" : ": THE BOOT CHAIN HAS SET NOTHING UP FOR THE RADIO'S STREAM");
}

/* ---- beside the steps: the radio's view of memory (boot-arg wifi_smmu=2) ----------------------------- */

/*
 * The boot chain sets nothing up for the radio's stream, and a stream nobody matches faults
 * (smmu_look()): before the radio can be given the address of a ring, the stream needs a match
 * register, a translation context and page tables, as hakios.adreno.c makes them in the GPU's
 * own SMMU -- there are the formats and the order of programming, and what the hypervisor does
 * with the global registers. Here they are found rather than known: the first match register
 * that is not valid, and the lowest context that no valid match register sends a stream
 * through and that is not switched on (Linux, too, takes the lowest free ones, on top of what
 * the boot chain left: dt "qcom,skip-init").
 *
 * The radio's addresses are where Android's tree puts them (dt: qcom,iommu-dma-addr-pool of
 * qcom,wcn6750: 256 MB from 0xa0000000), handed out upwards and never taken back; its faults
 * are recorded and do not stop it (dt: qcom,iommu-faults "stall-disable", "HUPCF"), and nobody
 * takes their interrupt: radio_space_faults() looks.
 *
 * Everything written is put back when the DSP has been stopped, and on the way back to the
 * loader after a panic (radio_space_off()).
 *
 * What the phone said (13 October): the hypervisor grants it. The boot chain uses match
 * registers 0 to 7 and contexts 0 to 6; register 8 and context 7 (at 0x15087000) were taken,
 * everything read back as written (but two bits that the SMMU does not keep), and the radio
 * then read and wrote the memory it was shown without a fault.
 */
#define RADIO_ADDRESSES         0xa0000000ull
#define RADIO_ADDRESSES_SIZE    0x10000000u
#define RADIO_TABLES            32              /* of 4K: the root, one of the second level, and 2 MB of addresses each other */
#define RADIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11) /* a page; for requests of either
	                                                                                  * privilege; shared, accessed, not
	                                                                                  * global: Linux's for a device */
#define RADIO_PTE_DEVICE        (RADIO_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)  /* ... a device's registers: Linux's
	                                                                                  * IOMMU_MMIO (MAIR entry 2), never
	                                                                                  * executed */
/*
 * Where the radio raises an interrupt by writing (its "MSI"): resource "msi_addr" of the device
 * tree's node for it, a register of the GIC that raises the SPI whose number is written; the
 * node's interrupts are SPI 768..799, so vector v is the GIC's number 800 + v.
 */
#define RADIO_MSI_REGISTER      0x17a10040ull
#define RADIO_FIRST_SPI         768u
#define PTE_ADDRESS             0x0000fffffffff000ull
#define SMMU_SGFSR              0x048           /* what the SMMU as a whole has refused ... */
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054           /* ... and of which stream */
#define SMMU_CBAR(n)            (4 * (n))       /* in the second page; [17:16] kind of context */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
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
/* on; faults recorded, no interrupt, and a fault does not hold up the requests after it */
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */

static struct {
	uint32_t wanted;                /* wifi_smmu */
	bool found, on;
	uint32_t page, pages;           /* of the SMMU's registers: a page's size, and how many before the contexts' */
	uint32_t match, context;        /* the stream match register and the context taken for the radio */
	uint32_t old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;             /* RADIO_TABLES of them, the root first */
	uint32_t table_count;
	uint64_t next;                  /* of the radio's addresses, the first not handed out (from RADIO_ADDRESSES) */
} radio_space;
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };              /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the radio's context", 0, 0x1000 };

static uint64_t *
radio_table(uint32_t n)
{
	return (uint64_t *)(radio_space.tables + 4096 * n);
}

/* The table an entry of a higher table leads to; made if the entry is empty. */
static uint64_t *
radio_next(uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < radio_space.table_count; i++) {
			if (kvtophys((vm_offset_t)radio_table(i)) == (*entry & PTE_ADDRESS)) {
				return radio_table(i);
			}
		}
		return NULL;
	}
	if (radio_space.table_count == RADIO_TABLES) {
		return NULL;
	}
	table = radio_table(radio_space.table_count++);
	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

/* Our memory at `physical` behind the radio's addresses from `address` on; the SMMU reads the tables from memory. */
static bool
radio_enter(uint64_t address, uint64_t physical, uint64_t size, uint64_t attributes)
{
	if (radio_space.tables == 0) {
		if (kmem_alloc(kernel_map, &radio_space.tables, round_page(4096 * RADIO_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		radio_space.table_count = 1;
	}
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = address + offset, *level2 = radio_next(&radio_table(0)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : radio_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			printf("wifi: %u: no page table left for the radio's %#llx\n", step, at);
			return false;
		}
		level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
	}
	for (vm_offset_t line = radio_space.tables; line < radio_space.tables + 4096 * RADIO_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	return true;
}

/* Which match register and which context are free; nothing is written. */
static bool
radio_space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		printf("wifi: %u: this SMMU numbers its streams with 16 bits, which nothing here is written for\n", step);
		return false;
	}
	radio_space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	radio_space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * radio_space.pages * radio_space.page != 0x100000) {    /* dt: "reg" of apps-smmu */
		printf("wifi: %u: the SMMU says it has 2 x %u pages of %#x bytes, which is not the megabyte the device tree gives "
		    "it: its contexts are not looked for\n", step, radio_space.pages, radio_space.page);
		return false;
	}
	radio_space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));

		if (smr >> 31) {
			printf("wifi: %u: match register %u is in use: stream %#x mask %#x, s2cr %#x\n", step, n, smr & 0x7fff,
			    smr >> 16 & 0x7fff, s2cr);
			used[s2cr & 0xff] = true;
		} else if (radio_space.match == ~0u) {
			radio_space.match = n;
			radio_space.old_smr = smr;
			radio_space.old_s2cr = s2cr;
		}
	}
	for (radio_space.context = 0; radio_space.context < contexts && used[radio_space.context]; radio_space.context++) {
	}
	if (radio_space.match == ~0u || radio_space.context >= contexts) {
		printf("wifi: %u: the SMMU has no match register or no context left\n", step);
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + radio_space.page;
	apps_smmu_context.phys = apps_smmu.phys + (radio_space.pages + radio_space.context) * radio_space.page;
	printf("wifi: %u: free: match register %u (smr %#x, s2cr %#x) and context %u, whose registers are at %#x; the SMMU's "
	    "pages are of %#x bytes, %u before the contexts'\n", step, radio_space.match, radio_space.old_smr,
	    radio_space.old_s2cr, radio_space.context, apps_smmu_context.phys, radio_space.page, radio_space.pages);
	get(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context));
	get(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context));
	radio_space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0);
	get(&apps_smmu_context, "fsr", SMMU_CB_FSR);
	if (radio_space.old_sctlr & 1) {
		printf("wifi: %u: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left "
		    "alone\n", step, radio_space.context);
		return false;
	}
	radio_space.found = true;
	return true;
}

/* What the SMMU has recorded of requests of the radio's that it could not serve. */
static void
radio_space_faults(void)
{
	uint32_t status, global;

	if (!radio_space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		printf("wifi: %u: the SMMU has refused the radio nothing (context %u: fsr %#x; sgfsr 0)\n", step, radio_space.context,
		    status);
		return;
	}
	tell("wifi: %u: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: "
	    "status %#x, syndromes %#x %#x (the second names the stream)\n", step, radio_space.context, status,
	    (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 | rd(apps_smmu_context.base + SMMU_CB_FAR),
	    rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global, rd(apps_smmu.base + SMMU_SGFSYNR0),
	    rd(apps_smmu.base + SMMU_SGFSYNR1));
}

/* The radio's stream through a context with our tables. */
static bool
radio_space_on(void)
{
	uint32_t smr = SMMU_SMR(radio_space.match), s2cr = SMMU_S2CR(radio_space.match), sees = 1u << 31 | RADIO_STREAM_MASK << 16 |
	    RADIO_STREAM;
	uint64_t root = kvtophys(radio_space.tables) | (uint64_t)radio_space.context << 48;     /* tagged with the context's number */

	printf("wifi: %u: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) and is switched on, and "
	    "match register %u sends stream %#x mask %#x through it; before: smr %#x, s2cr %#x, the context's sctlr %#x\n", step,
	    radio_space.context, root & PTE_ADDRESS, radio_space.match, RADIO_STREAM, RADIO_STREAM_MASK, radio_space.old_smr,
	    radio_space.old_s2cr, radio_space.old_sctlr);
	sleep_ms(pause_ms);
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, radio_space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			printf("wifi: %u: the SMMU does not finish forgetting old translations\n", step);
			return false;
		}
		sleep_ms(1);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	radio_space.on = true;                  /* (from here on there is something to take back) */
	put(&apps_smmu, "s2cr", s2cr, radio_space.context);
	put(&apps_smmu, "smr", smr, sees);

	/* the global ones are the hypervisor's to grant: see what it made of them */
	if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != radio_space.context ||
	    (get(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) ||
	    get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("wifi: %u: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", step,
		    RADIO_STREAM, radio_space.context);
		return false;
	}
	get(&apps_smmu_context, "tcr", SMMU_CB_TCR);
	get(&apps_smmu_context, "tcr2", SMMU_CB_TCR2);
	get(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context));
	get(&apps_smmu, "sgfsr", SMMU_SGFSR);
	printf("wifi: %u: THE RADIO'S STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", step, radio_space.context);
	radio_space_faults();
	return true;
}

/* The stream goes nowhere again, and the context translates nothing: as they were found. */
static void
radio_space_off(void)
{
	if (!radio_space.on) {
		return;
	}
	radio_space_faults();
	printf("wifi: %u: the radio's stream and context %u of the SMMU are put back as they were found\n", step, radio_space.context);
	put(&apps_smmu, "smr", SMMU_SMR(radio_space.match), radio_space.old_smr);
	put(&apps_smmu, "s2cr", SMMU_S2CR(radio_space.match), radio_space.old_s2cr);
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, radio_space.old_sctlr);
	get(&apps_smmu, "smr", SMMU_SMR(radio_space.match));
	get(&apps_smmu, "s2cr", SMMU_S2CR(radio_space.match));
	get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	radio_space.on = false;
}

/*
 * For the driver (hakios.lx.c): memory of ours that the radio is to see, at the address
 * returned. Without wifi_smmu=2 it has an address and the radio is shown nothing.
 */
static bool
radio_map(uint64_t physical, uint64_t size, uint64_t attributes, unsigned long long *address)
{
	size = (size + 4095) & ~4095ull;
	if (radio_space.next + size > RADIO_ADDRESSES_SIZE) {
		return false;
	}
	*address = RADIO_ADDRESSES + radio_space.next;
	radio_space.next += size;
	if (whole) {
		radio_space.wanted = 2;
	}
	if (radio_space.wanted != 2) {
		printf("wifi: %u: THE RADIO IS NOT SHOWN the %llu KB of ours at %#llx that the driver will know as %#llx: that takes "
		    "a context of the SMMU (wifi_smmu=2)\n", step, size >> 10, physical, *address);
		return true;
	}
	lck_mtx_lock(&hakios_smmu_lock);       /* (the sound may be looking for a free context at the same time) */
	if ((!radio_space.found && !radio_space_find()) || !radio_enter(*address, physical, size, attributes)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	printf("wifi: %u: the radio is to see %llu KB of ours (%#llx) at %#llx\n", step, size >> 10, physical, *address);
	if (!radio_space.on) {
		bool on = radio_space_on();

		lck_mtx_unlock(&hakios_smmu_lock);
		return on;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, radio_space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	return true;
}

bool
hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	return radio_map(physical, size, RADIO_PTE, address);
}

/*
 * What the radio is to write, and where, to raise its interrupt vector 0 (vector v: data + v): the
 * GIC's register that raises an SPI, shown to the radio in its SMMU context as a device's page.
 * Once; the GIC's number of the SPI of vector 0 goes to *first_spi.
 */
bool
hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi)
{
	static unsigned long long page;

	if (page == 0) {
		tell("wifi: wifi_irq: THE RADIO'S SMMU CONTEXT GETS AN ENTRY for the GIC's register that raises an interrupt (%#llx), "
		    "as a device's page\n", RADIO_MSI_REGISTER);
		if (!radio_map(RADIO_MSI_REGISTER & ~4095ull, 4096, RADIO_PTE_DEVICE, &page)) {
			page = 0;
			return false;
		}
	}
	*address = page + (RADIO_MSI_REGISTER & 4095);
	*data = RADIO_FIRST_SPI + 32;
	*first_spi = RADIO_FIRST_SPI + 32;
	tell("wifi: wifi_irq: the radio raises its interrupt vector v by writing %u + v at %#llx\n", *data, *address);
	return true;
}

/*
 * wifi_smmu=2 in a run that does not get as far as the driver: the context is set up with empty
 * tables, so that whatever the stream asked for would be refused as it is now, and taken back.
 * This is the first write to this SMMU, and to see whether the hypervisor grants it.
 */
static void
radio_space_try(void)
{
	bool as_found;

	if (!radio_space_find() || !radio_enter(RADIO_ADDRESSES, 0, 0, RADIO_PTE)) {
		return;
	}
	as_found = radio_space_on();
	sleep_ms(pause_ms);
	radio_space_off();
	printf("wifi: %u: %s\n", step, as_found ? "THE SMMU TOOK A CONTEXT FOR THE RADIO'S STREAM, and it has been taken back" :
	    "THE SMMU DID NOT TAKE A CONTEXT FOR THE RADIO'S STREAM");
}

/* ---- for the driver (hakios.lx.c): the firmware's processor, and files ------------------------------- */

bool
hakios_wifi_dsp_is_up(void)
{
	printf("wifi: %u: the driver asks for the firmware's processor: it %s\n", step, dsp_started && glink.open ? "has been up "
	    "since step 11, with the link of step 13" : "IS NOT UP");
	return dsp_started && glink.open;
}

/*
 * ath11k asks for files by the names Linux's firmware tree has ("ath11k/WCN6750/hw1.0/board.bin");
 * this phone has what Android's driver loads (fw-wifi/, see userland/build.sh): its board's own
 * data file and regulatory database, which are what the two names below stand for. The others
 * it asks for first and can do without (a container of several boards' data, a table of the
 * firmware's features) are not there.
 */
const uint8_t *
hakios_wifi_driver_file(const char *name, uint64_t *size)
{
	static const char *const files[][2] = { { "board.bin", "bdwlan.elf" }, { "regdb.bin", "regdb.bin" } };
	const char *last = strrchr(name, '/');

	last = last == NULL ? name : last + 1;
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (strcmp(last, files[i][0]) == 0) {
			return image_file(files[i][1], size);
		}
	}
	return NULL;
}

/*
 * The radio's own address. Its firmware has none (it says zeros): Android keeps the phone's in
 * its storage and hands it to the radio's driver (dt: "use-nv-mac"). Here it is given to the
 * kernel when that is started -- boot-arg wifi_mac, six bytes in hexadecimal with colons
 * between -- and is nowhere in these sources. The log says that one was given and how long it
 * was, not which.
 */
bool
hakios_wifi_radio_address(uint8_t *address)
{
	char text[32] = "";
	uint8_t bytes[6];
	unsigned count = 0, digits = 0, value = 0;
	bool good;

	if (!PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) || text[0] == 0) {
		printf("wifi: %u: the driver asks for the radio's address: none was given (wifi_mac)\n", step);
		return false;
	}
	good = true;
	for (const char *c = text;; c++) {
		int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ?
		    *c - 'A' + 10 : -1;

		if (digit >= 0 && digits < 2) {
			value = value << 4 | (unsigned)digit;
			digits++;
		} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
			bytes[count++] = (uint8_t)value;
			digits = value = 0;
		} else {
			good = false;
		}
		if (*c == 0 || !good) {
			break;
		}
	}
	good = good && count == 6 && !(bytes[0] & 1) && (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5]) != 0;
	printf("wifi: %u: the driver asks for the radio's address: one was given, %lu characters, %s\n", step,
	    (unsigned long)strlen(text), good ? "six bytes: it is the radio's for this run" : "WHICH IS NOT A STATION'S ADDRESS: "
	    "not used");
	if (good) {
		memcpy(address, bytes, sizeof(bytes));
	}
	return good;
}

/*
 * The network the radio is to join: the file "network" among the firmware in the small image,
 * the name on its first line, the passphrase on its second (userland/build.sh takes it from
 * fw-wifi/network, which is in no repository). Not a boot argument: those are echoed by the
 * loader and printed with a panic. The log says how long the two are, and nothing else of them.
 */
bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	const uint8_t *file = image_file("network", &size), *end = file + size, *line = file;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		printf("wifi: %u: which network to join: none was given (fw/network of the small image)\n", step);
		return false;
	}
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	printf("wifi: %u: which network to join: a name of %u bytes and a passphrase of %u bytes were given%s\n", step,
	    lengths[0], lengths[1], lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63 ? ": NOT A NAME OF 1 TO 32 BYTES AND A "
	    "PASSPHRASE OF 8 TO 63" : "");
	if (lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63) {
		return false;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return true;
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_smem, step_mailbox, step_rpmh, step_rails, step_pins, step_firmware, step_mailbox_open, step_header, step_image,
	step_our_item, step_start, step_queues, step_link, step_qrtr, step_exchange, step_registers,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/*
 * Where a run ends in order while the DSP runs (the run's time is up, the host asks for its
 * end), and when the steps are over: the DSP is stopped (dsp_shutdown()).
 */
void
hakios_wifi_stop(void)
{
	/* (a radio that is still on its way up is let get there first, or fail: half a minute at the most) */
	for (unsigned ms = 0; whole_runs && ms < 40000; ms += 10) {
		sleep_ms(10);
	}
	hakios_apple80211_summary();
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && !dirty) {
		bool down = dsp_shutdown("the run is about to end");

		if (whole) {
			tell("wifi: the radio is off: its DSP %s, and TrustZone %s\n", stopped_by_its_word ? "stopped when asked" :
			    "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
		}
	}
	lck_mtx_unlock(&dsp_lock);
}

/*
 * The same when the kernel goes back to the loader without having been through the above: after
 * a panic. A DSP left running then is one that nobody listens to any more, and the next kernel
 * has to find and stop it; better it is stopped now. One processor is left, with its interrupts
 * off: nothing here takes a lock or sleeps (the waits are loops), and the link's thread is
 * simply never run again. The DSP is asked first, for two seconds at most; whatever it says,
 * TrustZone is asked to shut it down.
 */
void
hakios_wifi_quiesce(void)
{
	hakios_apple80211_summary();           /* (also when the run is ended from the phone: five fingers) */
	if (!dsp_started || dirty) {
		return;
	}
	leaving = true;
	new_from = ~0u;                         /* (no pauses) */
	dsp_shutdown("the kernel is on its way back to the loader");
}

#define HAKIOS_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin (its "redwood") */

extern vm_offset_t pe_arm_get_soc_base_phys(void);

/* The steps' end: the DSP stopped, or left as asked. */
static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && dirty) {
		printf("wifi: THE DSP IS LEFT RUNNING (wifi_dirty), as a kernel that died would leave it\n");
	} else if (dsp_started && done == last_step && hakios_wifi_option("wifi_keep")) {
		printf("wifi: THE DSP STAYS ON while the system runs (wifi_keep)%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	if (!dsp_started) {
		radio_space_off();              /* (if the DSP could not be shut down in order: all the same) */
	}
	lck_mtx_unlock(&dsp_lock);
}

/*
 * The network of the file, joined the way anybody joins one: by asking the station. iOS's Wi-Fi
 * side may ask for another later, or for none.
 */
static void
join_given(uint64_t began)
{
	static struct hakios_sta_join what;
	struct hakios_sta_state *state;
	unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
	uint64_t size = 0;
	uint32_t given = 1;
	int error;

	if (image_file("network", &size) == NULL) {
		tell("wifi: no network to join was given (fw-wifi/network, see the README): the radio waits to be asked\n");
		return;
	}
	if (PE_parse_boot_argn("wifi_file", &given, sizeof(given)) && given == 0) {
		tell("wifi: the network of the file is not joined at the start (wifi_file=0): the radio waits to be asked\n");
		return;
	}
	bzero(&what, sizeof(what));
	if (!hakios_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
		tell("wifi: the network given in fw/network is not a name and a passphrase: nothing is joined\n");
		return;
	}
	what.ssid_length = (uint8_t)name_length;
	what.passphrase_length = (uint8_t)passphrase_length;
	what.security = HAKIOS_STA_WPA2_PSK;
	error = hakios_sta_ask_join_standing(&what);
	bzero(&what, sizeof(what));
	for (unsigned waited = 0; error == 0 && hakios_sta_busy() && waited < 300; waited++) {
		sleep_ms(100);
	}
	if ((state = kalloc_data(sizeof(*state), Z_WAITOK)) == NULL) {
		return;
	}
	hakios_sta_state(state);
	if (state->phase == HAKIOS_STA_JOINED) {
		tell("wifi: THE NETWORK OF THE FILE IS JOINED, %llu.%llu s after the kernel began on the radio\n", (now_us() - began) /
		    1000000, (now_us() - began) / 100000 % 10);
	} else {
		tell("wifi: THE NETWORK OF THE FILE IS NOT JOINED (%d, %d): the radio waits to be asked. The last of what was said:\n",
		    error, state->last_error);
		say_kept();
	}
	kfree_data(state, sizeof(*state));
}

/*
 * wifi=1: the whole of it on a thread of its own, so that the system starts meanwhile (the
 * firmware's calibration alone takes seven seconds). The steps that only look are left out.
 */
static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const uint8_t needed[] = { 1, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint64_t began = now_us();
	uint32_t done = 0, failed = 0;

	/* the memory the radio is shown is one piece: taken now, while that is easy to find */
	if (hakios_lx_prepare() != 0) {
		tell("wifi: NO MEMORY FOR THE RADIO in one piece: it is not brought up\n");
		whole_runs = false;
		thread_terminate(current_thread());
	}
	for (unsigned i = 0; i < sizeof(needed) && failed == 0; i++) {
		step = needed[i];
		if (step == 15) {
			tell("wifi: the radio's DSP is up and linked, %llu ms in; now the driver\n", (now_us() - began) / 1000);
		}
		if (steps[step - 1]()) {
			done = step;
		} else {
			failed = step;
		}
	}
	last_step = needed[sizeof(needed) - 1];
	if (failed == 0) {
		tell("wifi: THE RADIO IS UP, %llu.%llu s after the kernel began on it\n", (now_us() - began) / 1000000,
		    (now_us() - began) / 100000 % 10);
		join_given(began);
	} else {
		tell("wifi: THE RADIO DID NOT COME UP: step %u failed, %llu s in. The last of what was said on the way:\n", failed,
		    (now_us() - began) / 1000000);
		say_kept();
	}
	steps_over(failed == 0 ? last_step : 0);
	step = done;
	whole_runs = false;
	thread_terminate(current_thread());
}

/* ---- another phone: the OnePlus Ace 3 Pro (corvette) --------------------------------------------------------
 *
 * Its radio is a WCN7850 ("Kiwi"), a PCIe device: the firmware runs inside the chip, and the application
 * processor loads it over PCIe's MHI. Every register the steps above touch -- the WPSS's memory, TrustZone's
 * image ids, SMEM's items, the AOP's mailbox -- belongs to redwood's WCN6750 and has no counterpart here, so
 * none of them may be run on this phone.
 *
 * wifi_s0=1 is the first look, and it writes nothing: the pins the radio and PCIe0 are wired to, and what the
 * boot loader left of PCIe0's clocks, power domain, reset and link. Reads only, each announced before it is
 * made, and nothing is read whose block is known to be off (a read there can end the kernel: see the top of
 * hakios.goodix.c on the same hazard).
 */

#define CORVETTE_TLMM_PHYS      0x0f100000      /* (redwood's TLMM is at 0x0f000000, one megabyte lower) */
#define CORVETTE_TLMM_SIZE      0x80000

#define CORVETTE_S1_MAPS          64

struct corvette_s1_mapping {
	vm_offset_t physical;
	vm_size_t size;
	vm_offset_t virtual;
};

static struct corvette_s1_mapping corvette_s1_mappings[CORVETTE_S1_MAPS];
static unsigned corvette_s1_mapping_count;
LCK_MTX_DECLARE(corvette_s1_mapping_lock, &wifi_lock_group);

/* The kernel's permanent I/O submap is only 8 MiB; S1 helpers must share these device mappings. */
static vm_offset_t
corvette_s1_io_map(vm_offset_t physical, vm_size_t size)
{
	vm_offset_t virtual;

	lck_mtx_lock(&corvette_s1_mapping_lock);
	for (unsigned i = 0; i < corvette_s1_mapping_count; i++) {
		struct corvette_s1_mapping *mapping = &corvette_s1_mappings[i];
		if (physical >= mapping->physical && physical - mapping->physical <= mapping->size &&
		    size <= mapping->size - (physical - mapping->physical)) {
			virtual = mapping->virtual + (physical - mapping->physical);
			lck_mtx_unlock(&corvette_s1_mapping_lock);
			return virtual;
		}
	}
	if (corvette_s1_mapping_count == CORVETTE_S1_MAPS) {
		lck_mtx_unlock(&corvette_s1_mapping_lock);
		printf("wifi: corvette MMIO map cache full; refusing another permanent mapping\n");
		return 0;
	}
	virtual = ml_io_map(physical, size);
	corvette_s1_mappings[corvette_s1_mapping_count++] = (struct corvette_s1_mapping) {
		.physical = physical, .size = size, .virtual = virtual
	};
	lck_mtx_unlock(&corvette_s1_mapping_lock);
	return virtual;
}

/* The pins, from the phone's own device tree: qcom,cnss-kiwi@b0000000's wlan-en/bt-en/sw-ctrl, and
 * qcom,pcie@1c00000's perst (94) and clkreq (95). wlan_en_active drives GPIO 16 high, wlan_en_sleep low. */
static void
kiwi_s0_pins(void)
{
	static const struct {
		uint32_t pin;
		const char *name;
	} pins[] = { { 16, "wlan-en" }, { 17, "bt-en" }, { 18, "sw-ctrl" }, { 19, "wlan-sw-ctrl" },
		     { 94, "pcie0 perst" }, { 95, "pcie0 clkreq" } };
	vm_offset_t tlmm;

	printf("wifi: s0: the pins, at TLMM %#x, read only\n", CORVETTE_TLMM_PHYS);
	tlmm = corvette_s1_io_map(CORVETTE_TLMM_PHYS, CORVETTE_TLMM_SIZE);
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		vm_offset_t config_at = tlmm + 0x1000 * (vm_offset_t)pins[i].pin;
		uint32_t config = rd(config_at), io = rd(config_at + 4);

		printf("wifi: s0:   pin %u %-14s config %#x: function %u, %s, pull %u, drive %u; "
		    "io %#x: in %u, out %u\n", pins[i].pin, pins[i].name, config, (config >> 2) & 0xf,
		    (config & (1u << 9)) ? "output" : "input", config & 3, (config >> 6) & 7, io, io & 1,
		    (io >> 1) & 1);
	}
}

/*
 * PCIe0, from the phone's device tree (qcom,pcie@1c00000): parf 0x01c00000 (0x3000), phy 0x01c06000
 * (0x2000), dm_core 0x60000000 (the core's own config space, 0xf1d), elbi 0x60000f20 (0xa8), and conf
 * 0x60100000 (0x100000 of ECAM: a megabyte to the bus, so bus 0 is at 0x60100000 and bus 1 at 0x60200000).
 *
 * All of it is dark until the clocks below run and the power domain is up; reading any of it while it is
 * not has ended kernels on this family of SoC (a bus timeout that the always-on processor answers with a
 * reset). So the clocks and the power domain are looked at first, and the hardware after them only if the
 * boot loader has left it all on, which it does not do for us: it is there to be read, not relied on.
 */
#define CORVETTE_GCC_PHYS               0x00100000
#define CORVETTE_GCC_SIZE               0x70000         /* (the whole of GCC is 0x1f4200, but the PCIe0 end is below 0x70000) */

/* The seven branch clocks of PCIe0 are voted on in two registers (Linux's gcc-sm8650.c), and each has a
 * halt bit of its own: reading that bit 0 as 1 means the clock is not running yet. */
#define CORVETTE_PCIE0_VOTE             0x52008         /* bit 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux, 4 pipe, 5 slv_q2a, 6 cnoc_pcie_sf_axi */
#define CORVETTE_PCIE0_VOTE_RCHNG       0x52000         /* bit 22 phy_rchng */
#define CORVETTE_PCIE0_GDSCR            0x6b004
#define CORVETTE_PCIE0_PHY_GDSCR        0x6c000
#define CORVETTE_PCIE0_BCR              0x6b000         /* bit 0: the core is held in reset */
#define CORVETTE_PCIE0_PHY_BCR          0x6c01c         /* bit 0: the PHY is held in reset */

#define CORVETTE_PCIE0_PARF             0x01c00000
#define CORVETTE_PCIE0_PHY              0x01c06000
#define CORVETTE_PCIE0_CORE             0x60000000
#define CORVETTE_PCIE0_ELBI             0x60000f20
#define CORVETTE_PCIE0_ECAM             0x60100000
#define CORVETTE_PCIE0_MEM32            0x60300000
#define CORVETTE_PCIE0_MEM32_SIZE       0x03d00000
#define CORVETTE_PCIE_IATU_REGION_STRIDE 0x200
#define CORVETTE_IATU_CTRL1             0x00
#define CORVETTE_IATU_CTRL2             0x04
#define CORVETTE_IATU_LBAR              0x08
#define CORVETTE_IATU_UBAR              0x0c
#define CORVETTE_IATU_LAR               0x10
#define CORVETTE_IATU_LTAR              0x14
#define CORVETTE_IATU_UTAR              0x18
#define CORVETTE_IATU_CFG0              0x04
#define CORVETTE_IATU_MEM               0x00
#define CORVETTE_IATU_ENABLE             (1u << 31)
#define CORVETTE_CFG0_BUS1_DEV0          0x01000000u

static uint32_t corvette_s2_iatu_saved[2][7];
static bool corvette_s2_iatu_saved_valid;

/* Linux's gdsc.c: bit 31 is the status (1: powered), bit 16 the "power up complete" of the handshake, and
 * bit 0 the software collapse that turns a domain off. */
#define CORVETTE_GDSC_POWER_ON          (1u << 31)
#define CORVETTE_GDSC_UP_COMPLETE       (1u << 16)
#define CORVETTE_GDSC_SW_COLLAPSE       (1u << 0)

/* Both PCIe0 domains have dt's qcom,support-cfg-gdscr, which is Linux's POLL_CFG_GDSCR (gdsc.c): the
 * handshake's bits are in the register four bytes after the GDSCR (CFG_GDSCR, CFG_GDSCR_OFFSET), not in
 * the GDSCR itself. There, bit 16 is "power up complete" and bit 15 "power down complete". */
#define CORVETTE_GDSC_CFG               0x4
#define CORVETTE_GDSC_CFG_UP_COMPLETE   (1u << 16)
#define CORVETTE_GDSC_CFG_DOWN_COMPLETE (1u << 15)

/*
 * The reads below are only made when the clocks run and the power domain answers, which the caller has
 * just checked. The offsets are mainline's (drivers/pci/controller/dwc/pcie-qcom.c): PARF_PHY_REFCLK 0x4c,
 * PARF_LTSSM 0x1b0 (its state in bits 5:0), ELBI's SYS_CTRL 0x04 (bit 0: link training enabled) and
 * SYS_STTS 0x08 (the bridge's own LTSSM state in bits 17:12). The PHY's status is the phone's device tree's
 * qcom,phy-status-offset 0x214, bit 6 (qcom,phy-status-bit); its power-down register is 0x240.
 */
static void
kiwi_s0_pcie(bool phy_on)
{
	vm_offset_t parf = corvette_s1_io_map(CORVETTE_PCIE0_PARF, 0x8000);      /* (the PHY's 0x2000 comes after the PARF's 0x3000) */
	vm_offset_t core = corvette_s1_io_map(CORVETTE_PCIE0_CORE, 0x10000);     /* (0xf1d of config space, then the ELBI at 0xf20) */
	vm_offset_t ecam = corvette_s1_io_map(CORVETTE_PCIE0_ECAM, 0x100000);    /* (bus 0 only: bus 1 is the bridge's to reach) */
	uint32_t ltssm = rd(parf + 0x1b0), control = rd(core + 0xf20 + 0x04);
	uint32_t state = rd(core + 0xf20 + 0x08), id = rd(ecam);

	printf("wifi: s0:   parf %#x: ltssm %#x (state %u), phy refclk %#x, [0x24] %#x, [0x28] %#x, testbus %u\n",
	    CORVETTE_PCIE0_PARF, ltssm, ltssm & 0x3f, rd(parf + 0x4c), rd(parf + 0x24), rd(parf + 0x28),
	    rd(parf + 0x224));
	printf("wifi: s0:   elbi %#x: control %#x (link training enabled %u), status %#x (ltssm %u)\n",
	    CORVETTE_PCIE0_ELBI, control, control & 1, state, (state >> 12) & 0x3f);
	if (phy_on) {
		uint32_t status = rd(parf + 0x6000 + 0x214);

		printf("wifi: s0:   phy %#x: status %#x (ready %u), power down %#x\n", CORVETTE_PCIE0_PHY, status,
		    (status >> 6) & 1, rd(parf + 0x6000 + 0x240));
	} else {
		printf("wifi: s0:   the PHY's power domain is off, so its registers are not read\n");
	}
	printf("wifi: s0:   the core's own config space %#x: %#x (vendor %#x, device %#x), class/revision %#x\n",
	    CORVETTE_PCIE0_CORE, id, id & 0xffff, id >> 16, rd(core + 8));
	printf("wifi: s0:   ECAM's bus 0, the root complex: %#x (vendor %#x, device %#x); the radio answers at "
	    "its own bus through the bridge, once the link is up\n", id, id & 0xffff, id >> 16);
}

static void
kiwi_s0_gcc(void)
{
	static const struct {
		const char *name;
		uint32_t halt;
	} clocks[] = { { "slv_q2a", 0x6b01c }, { "slv_axi", 0x6b020 }, { "mstr_axi", 0x6b02c },
		       { "cfg_ahb", 0x6b038 }, { "aux", 0x6b03c }, { "pipe", 0x6b048 },
		       { "phy_rchng", 0x6b054 }, { "cnoc_pcie_sf_axi", 0x10058 } };
	vm_offset_t gcc = corvette_s1_io_map(CORVETTE_GCC_PHYS, CORVETTE_GCC_SIZE);
	uint32_t vote = rd(gcc + CORVETTE_PCIE0_VOTE), vote_rchng = rd(gcc + CORVETTE_PCIE0_VOTE_RCHNG);
	uint32_t core = rd(gcc + CORVETTE_PCIE0_GDSCR), phy = rd(gcc + CORVETTE_PCIE0_PHY_GDSCR);
	bool running = true;

	printf("wifi: s0: PCIe0's clocks, at GCC %#x, read only: what the boot loader left\n", CORVETTE_GCC_PHYS);
	printf("wifi: s0:   votes %#x (slv_axi %u, mstr_axi %u, cfg_ahb %u, aux %u, pipe %u, slv_q2a %u, "
	    "cnoc_pcie_sf_axi %u) and %#x bit 22 (phy_rchng %u)\n", vote, vote & 1, (vote >> 1) & 1, (vote >> 2) & 1,
	    (vote >> 3) & 1, (vote >> 4) & 1, (vote >> 5) & 1, (vote >> 6) & 1, vote_rchng, (vote_rchng >> 22) & 1);
	printf("wifi: s0:   halted");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		uint32_t halted = rd(gcc + clocks[i].halt) & 1;

		printf("%s %s %u", i == 0 ? " (1 = not running):" : ",", clocks[i].name, halted);
		if (halted) {
			running = false;
		}
	}
	printf("\n");
	printf("wifi: s0:   pcie_0_gdsc %#x: power on %u, up complete %u, collapse %u; pcie_0_phy_gdsc %#x: "
	    "power on %u, up complete %u, collapse %u\n", core, (core & CORVETTE_GDSC_POWER_ON) ? 1 : 0,
	    (core & CORVETTE_GDSC_UP_COMPLETE) ? 1 : 0, (core & CORVETTE_GDSC_SW_COLLAPSE) ? 1 : 0, phy,
	    (phy & CORVETTE_GDSC_POWER_ON) ? 1 : 0, (phy & CORVETTE_GDSC_UP_COMPLETE) ? 1 : 0,
	    (phy & CORVETTE_GDSC_SW_COLLAPSE) ? 1 : 0);
	printf("wifi: s0:   resets: pcie_0_core %#x (held %u), pcie_0_phy %#x (held %u)\n",
	    rd(gcc + CORVETTE_PCIE0_BCR), rd(gcc + CORVETTE_PCIE0_BCR) & 1, rd(gcc + CORVETTE_PCIE0_PHY_BCR),
	    rd(gcc + CORVETTE_PCIE0_PHY_BCR) & 1);

	if (!running || !(core & CORVETTE_GDSC_POWER_ON)) {
		printf("wifi: s0: PCIe0 is off, so nothing below it is read: a register there does not answer, and "
		    "the kernel that waits for it is not the one that comes back. Turning it on is the next step "
		    "(wifi_s1), not this one\n");
		return;
	}
	kiwi_s0_pcie(phy & CORVETTE_GDSC_POWER_ON);
}

static void
kiwi_s0(void)
{
	printf("wifi: s0: corvette: a WCN7850 behind PCIe0. Nothing is written; each read is announced first\n");
	kiwi_s0_pins();
	kiwi_s0_gcc();
	printf("wifi: s0: done\n");
}

/* ---- the same phone, S1: turning PCIe0 on ---------------------------------------------------------------
 *
 * S0 left the picture clear: the seven branch clocks of PCIe0 are not voted, both of its power domains are
 * collapsed, its two resets are released, and every pin it and the radio are wired to still sits at the
 * reset default. So nothing below the controller answers, and the radio behind it has no link to anything.
 *
 * This turns the controller on and looks after every write: the value it overwrites is printed first and the
 * register read back at once, so the last line of the log names the step that stopped. The order is the one
 * Linux's gcc-sm8650.c, gdsc.c and pcie-qcom.c take: the clocks, then the domains they feed, then the
 * resets' state, then the pins that power the radio and hold it in reset, then the PHY's own sequence, then
 * the link's training. The radio's firmware, MHI and its own config space belong to S2: nothing of those is
 * touched here, and no register below the controller is read until training has reached L0 (a read there
 * ends the kernel, see S0's note above).
 */

#define CORVETTE_PCIE0_COLLAPSE         0x5214c         /* bit 0: pcie_0_gdsc, bit 3: pcie_0_phy_gdsc (set = collapsed) */
#define CORVETTE_PCIE0_PARF_SIZE        0x8000          /* (PARF's 0x3000, then the PHY's 0x2000, in one mapping) */
#define CORVETTE_PCIE0_PHY_AT           0x6000          /* the PHY, from the start of that mapping */
#define CORVETTE_PHY_STATUS_OFFSET      0x214           /* dt: qcom,phy-status-offset ... */
#define CORVETTE_PHY_STATUS_READY       (1u << 6)       /* ... and qcom,phy-status-bit */
#define CORVETTE_PARF_PHY_REFCLK        0x4c            /* (mainline's PARF_PHY_REFCLK: bit 16 ss, bit 12 use-pad) */
#define CORVETTE_PARF_LTSSM_OFFSET      0x1b0           /* (PARF_LTSSM: state in bits 5:0) */
#define CORVETTE_PARF_SYS_CTRL          0x00            /* (PARF_SYS_CTRL: the wake-up and clock-gating bits) */
#define CORVETTE_PARF_PHY_CTRL          0x40            /* bit 0: the PHY held in its lowest power state */
#define CORVETTE_PHY_TEST_PWR_DOWN      (1u << 0)
#define CORVETTE_PARF_MAC_PHY_PWR_MUX   (1u << 29)      /* (PARF_SYS_CTRL: lets the PHY's power-down be driven) */
#define CORVETTE_PARF_MHI_CLOCK_RESET   0x174           /* bit 0 ahb clk, bit 1 mstr axi clk, bit 4 bypass */
#define CORVETTE_PARF_Q2A_FLUSH_OFFSET  0x1ac
#define CORVETTE_PARF_PM_CTRL           0x20            /* bit 5: L1 is not to be entered */
#define CORVETTE_PARF_REQ_NOT_ENTR_L1   (1u << 5)
#define CORVETTE_PARF_AXI_HALT_V2       0x1a8
#define CORVETTE_PARF_AXI_HALT_EN       (1u << 31)
#define CORVETTE_PARF_DEVICE_TYPE_OFF   0x1000          /* 4 is a root complex */
#define CORVETTE_PARF_DEVICE_TYPE_RC    0x4
#define CORVETTE_PARF_MHI_ON            ((1u << 4) | (1u << 1) | (1u << 0))
#define CORVETTE_PARF_SYS_CTRL_ON       0x365E          /* the vendor msm_pcie_enable_link() writes this exact
                                                 * value: MST/SLV_WAKEUP_EN, MSTR_ACLK_CGC_DIS, CORE_CLK_CGC_DIS,
                                                 * AUX_PWR_DET, L23/L1_CLK_RMV_DIS, plus bit8 and bit3 (the AUX/Core
                                                 * clk sync for L1ss). S1 used 0x3656 -- bit9(SLV_ACLK_CGC_DIS) in
                                                 * place of bit8+bit3 -- which is NOT what the vendor writes. */
#define CORVETTE_PARF_LTSSM_EN          (1u << 8)
#define CORVETTE_REFCLK_USE_PAD         (1u << 12)      /* (PARF_PHY_REFCLK: take the clock from the pad) */
#define CORVETTE_PCIE0_SM               0x1d07000       /* dt: the link's own state machine (reg-name pcie_sm) */
#define CORVETTE_PCIE0_SM_SIZE          0x7000
/* SM8550 and after have the V2 pair for the DBI's and the ATU's base (pcie-qcom.c's
 * PARF_DBI_BASE_ADDR_V2 / PARF_ATU_BASE_ADDR), and the older single registers (0x168/0x16c) read 0
 * on this SoC -- so they are the ones that matter here. The device tree's reg-names give the two
 * addresses: dm_core 0x60000000 (the core's own config space) and iatu 0x60001000. */
#define CORVETTE_PARF_DBI_BASE_V2       0x350
#define CORVETTE_PARF_DBI_BASE_V2_HI    0x354
#define CORVETTE_PARF_SLV_SIZE_V2       0x358
#define CORVETTE_PARF_SLV_SIZE_V2_HI    0x35c
#define CORVETTE_PARF_ATU_BASE          0x634
#define CORVETTE_PARF_ATU_BASE_HI       0x638
#define CORVETTE_PARF_SLV_SIZE          0x4000000       /* (dt: qcom,slv-addr-space-size; the vendor writes this exact value) */
/* The core's own config space (DBI), as Linux's dw_pcie_setup_rc() and dw_pcie_setup() fill it in. The
 * port was told nothing: PORT_LINK_CONTROL read 0, which is a port with no lanes at all, and a DWC LTSSM
 * does not leave detect for a port it believes has none. */
#define CORVETTE_DBI_COMMAND            0x04            /* bit 1 memory, bit 2 master, bit 8 serr */
#define CORVETTE_DBI_CLASS              0x08            /* revision | prog-if | subclass | class */
#define CORVETTE_DBI_PRIMARY_BUS        0x18
#define CORVETTE_DBI_PORT_LINK_CONTROL  0x710           /* bits 21:16 the lanes, bit 5 the dll's link enable */
#define CORVETTE_DBI_LINK_WIDTH_SPEED   0x80c           /* bits 15:8 the link's own width */
#define CORVETTE_DBI_DEBUG1             0x72c
#define CORVETTE_DBI_MISC_CONTROL_1     0x8bc           /* bit 0: writes to the read-only half are taken */
#define CORVETTE_LINK_LANES_MASK        (0x3fu << 16)
#define CORVETTE_LINK_ONE_LANE          (1u << 16)
#define CORVETTE_LINK_DLL_ENABLE        (1u << 5)
#define CORVETTE_LINK_WIDTH_MASK        (0xffu << 8)
#define CORVETTE_LINK_WIDTH_ONE_LANE    (1u << 8)
#define CORVETTE_SPEED_CHANGE           (1u << 17)      /* (link width/speed: ask the link to retrain) */
#define CORVETTE_BUS_NUMBERS            0x00ff0100      /* (primary 0, secondary 1, subordinate 0xff) */
#define CORVETTE_COMMAND_BITS           0x107           /* (io, memory, master, serr) */
#define CORVETTE_CLASS_PCI_BRIDGE       0x06040000      /* (class 6, subclass 4: a PCI-to-PCI bridge) */
#define CORVETTE_ELBI_SYS_CTRL_OFFSET   0x04            /* (bit 0: link training enabled) */
#define CORVETTE_ELBI_SYS_STTS_OFFSET   0x08            /* (bits 17:12: the bridge's LTSSM state) */
#define CORVETTE_PIN_WLAN_EN            16              /* dt: qcom,cnss-kiwi@b0000000's wlan-en-gpio */
#define CORVETTE_PIN_PERST              94              /* dt: qcom,pcie@1c00000's perst-gpio */
#define CORVETTE_PIN_CLKREQ             95             /* dt: qcom,pcie@1c00000's clkreq-gpio */
/*
 * The clock request pin is NOT a GPIO. The device tree gives it function "pcie0_clk_req_n" in the
 * pcie0_clkreq_default pinctrl state, which is the first non-GPIO function of pin 95 (mux value 1),
 * and only that function routes the signal into the PCIe controller. As a plain GPIO the level could
 * be read, but the controller never saw the endpoint asking for its clock, so the reference clock was
 * never driven out to it and its PHY never came up. pull-up(3) | function 1 (bits 4:2) | 2 mA (bits
 * 7:6 = 0) | input (bit 9 clear) = 0x7, matching pcie0_clkreq_default (drive-strength 2, bias-pull-up).
 */
#define CORVETTE_PIN_CLKREQ_N          0x7
/*
 * And the same pin as the device tree reads it in its sleep state: pcie0_clkreq_sleep is plain gpio
 * with bias-pull-up, so pull-up (bits 1:0 = 3) | function 0 | 2 mA | input = 0x3.
 */
#define CORVETTE_PIN_CLKREQ_SLEEP      0x3
#define CORVETTE_PIN_OUTPUT_8MA         0x2c0           /* function 0, no pull, drive 3 (8 mA), output: as hakios.goodix.c */

/* dw_pcie_ltssm: the names of the states this link can be seen in (the rest are printed by number). */
static const char *
kiwi_s1_ltssm_name(uint32_t state)
{
	switch (state) {
	case 0x00: return "detect.quiet";
	case 0x01: return "detect.active";
	case 0x02: return "polling.active";
	case 0x03: return "polling.compliance";
	case 0x04: return "polling.configuration";
	case 0x05: return "pre-detect.quiet";
	case 0x06: return "detect.wait";
	case 0x07: return "configuration.linkwidth.start";
	case 0x08: return "configuration.linkwidth.accept";
	case 0x09: return "configuration.lanenum.wait";
	case 0x0a: return "configuration.lanenum.accept";
	case 0x0b: return "configuration.complete";
	case 0x0c: return "configuration.idle";
	case 0x0d: return "recovery.lock";
	case 0x0e: return "recovery.speed";
	case 0x0f: return "recovery.receiver-config";
	case 0x10: return "recovery.idle";
	case 0x11: return "L0";
	case 0x12: return "L0s";
	case 0x13: return "sending electrical idle";
	case 0x14: return "L1.idle";
	case 0x15: return "L1 (sending electrical idle)";
	case 0x16: return "L2.idle";
	case 0x17: return "L2 (sending electrical idle)";
	case 0x18: return "disabled.entry";
	case 0x19: return "disabled.idle";
	case 0x1a: return "disabled";
	case 0x1b: return "hot reset.entry";
	case 0x1c: return "hot reset";
	case 0x1d: case 0x1e: case 0x1f: case 0x20: return "recovery.equalization";
	default: return NULL;
	}
}

/* A read, with the register it came from: the step it belongs to is the last line before the next one. */
static uint32_t
kiwi_s1_read(const char *what, uint32_t offset, vm_offset_t at)
{
	uint32_t value = rd(at);

	printf("wifi: s1:   %-24s +%#05x: %#x\n", what, offset, value);
	return value;
}

/* A write: what it overwrote, what was written, and what the register says the moment after. */
static void
kiwi_s1_write(const char *what, uint32_t offset, vm_offset_t at, uint32_t value)
{
	uint32_t before = rd(at);

	/*
	 * Said before it is made, and read back on the next line: a store to an address that does not answer
	 * ends the kernel at once, and then the last line of the log is the one that names the register.
	 */
	printf("wifi: s1:   %-24s +%#05x: %#x -> %#x\n", what, offset, before, value);
	wr(at, value);
	printf("wifi: s1:   %-24s +%#05x reads %#x\n", "", offset, rd(at));
}

/* 1/7: the seven branch clocks of PCIe0, in two votes (Linux's gcc-sm8650.c). */
static bool
kiwi_s1_clocks(vm_offset_t gcc)
{
	static const struct {
		const char *name;
		uint32_t halt;
	} clocks[] = { { "slv_q2a", 0x6b01c }, { "slv_axi", 0x6b020 }, { "mstr_axi", 0x6b02c },
		       { "cfg_ahb", 0x6b038 }, { "aux", 0x6b03c }, { "pipe", 0x6b048 },
		       { "phy_rchng", 0x6b054 }, { "cnoc_pcie_sf_axi", 0x10058 } };
	uint32_t vote = rd(gcc + CORVETTE_PCIE0_VOTE), rchng = rd(gcc + CORVETTE_PCIE0_VOTE_RCHNG);
	bool running = true;

	printf("wifi: s1: 1/7: the branch clocks of PCIe0, at GCC %#x\n", CORVETTE_GCC_PHYS);
	kiwi_s1_write("vote (7 branches)", CORVETTE_PCIE0_VOTE, gcc + CORVETTE_PCIE0_VOTE, vote | 0x7f);
	kiwi_s1_write("vote (phy_rchng)", CORVETTE_PCIE0_VOTE_RCHNG, gcc + CORVETTE_PCIE0_VOTE_RCHNG, rchng | (1u << 22));
	sleep_ms(2);

	printf("wifi: s1:   halted (1 = not running):");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		uint32_t halted = rd(gcc + clocks[i].halt) & 1;

		printf("%s %s %u", i == 0 ? "" : ",", clocks[i].name, halted);
		if (halted) {
			running = false;
		}
	}
	printf("\n");
	printf("wifi: s1:   %s\n", running ? "1/7: the seven run" :
	    "1/7: some are halted; with the power domains still down that proves nothing, and 2b/7 looks again");
	return running;
}

/* 2/7: one power domain, taken out of collapse, with its power-up handshake waited for. */
static bool
kiwi_s1_gdsc(const char *name, vm_offset_t gcc, uint32_t gdscr, uint32_t collapse_bit)
{
	uint32_t value = rd(gcc + gdscr), cfg = rd(gcc + gdscr + CORVETTE_GDSC_CFG), now = value, polls;

	/*
	 * The handshake is in CFG_GDSCR, not in the GDSCR: S1's first run waited for bit 16 of the GDSCR and
	 * read 0 for ever while the domain was in fact up (its bit 31 said so), and the whole chain after this
	 * step was never reached. The GDSCR's bit 31 is the state of the domain, its bit 0 the software
	 * collapse; CFG_GDSCR's bit 16 is the power-up handshake and bit 15 the power-down one.
	 */
	printf("wifi: s1: 2/7: %s at +%#x: gdscr %#x (power on %u, collapse %u), cfg %#x (up complete %u, "
	    "down complete %u)\n", name, gdscr, value, value >> 31, value & 1, cfg,
	    (cfg & CORVETTE_GDSC_CFG_UP_COMPLETE) ? 1 : 0, (cfg & CORVETTE_GDSC_CFG_DOWN_COMPLETE) ? 1 : 0);
	if ((cfg & CORVETTE_GDSC_CFG_UP_COMPLETE) && (value & CORVETTE_GDSC_POWER_ON)) {
		printf("wifi: s1:   already up: nothing is written\n");
	} else {
		/*
		 * A votable domain is held by its collapse bit in 0x5214c (gcc-sm8650.c's .collapse_ctrl and
		 * .collapse_mask: bit 0 the core's, bit 3 the PHY's), and that is what Linux clears; the GDSCR's
		 * own software collapse goes with it. Both are printed before they are written.
		 */
		uint32_t collapse = kiwi_s1_read("collapse bits", CORVETTE_PCIE0_COLLAPSE, gcc + CORVETTE_PCIE0_COLLAPSE);

		if (collapse & (1u << collapse_bit)) {
			kiwi_s1_write("collapse: cleared", CORVETTE_PCIE0_COLLAPSE, gcc + CORVETTE_PCIE0_COLLAPSE,
			    collapse & ~(1u << collapse_bit));
		} else {
			printf("wifi: s1:   this domain's collapse bit is already clear\n");
		}
		if (value & CORVETTE_GDSC_SW_COLLAPSE) {
			kiwi_s1_write(name, gdscr, gcc + gdscr, value & ~CORVETTE_GDSC_SW_COLLAPSE);
		}
	}
	/*
	 * The handshake, waited for with the bit this SoC actually uses. Linux polls the same pair and gives it
	 * 100 tries of 100 microseconds.
	 */
	for (polls = 0; polls < 100; polls++) {
		now = rd(gcc + gdscr);
		cfg = rd(gcc + gdscr + CORVETTE_GDSC_CFG);
		if ((cfg & CORVETTE_GDSC_CFG_UP_COMPLETE) && (now & CORVETTE_GDSC_POWER_ON)) {
			break;
		}
		sleep_us(100);
	}
	printf("wifi: s1:   %s after %u x 100 us: power on %u, up complete %u, collapse %u; gdscr %#x, cfg %#x\n",
	    name, polls, now >> 31, (cfg & CORVETTE_GDSC_CFG_UP_COMPLETE) ? 1 : 0, now & 1, now, cfg);
	return (cfg & CORVETTE_GDSC_CFG_UP_COMPLETE) && (now & CORVETTE_GDSC_POWER_ON);
}

/* 2b/7: the same clock registers, read again now that both power domains are up. Before them a halt bit
 * proves nothing -- a clock whose domain has no power is halted whatever its branch vote says. */
static void
kiwi_s1_clocks_again(vm_offset_t gcc)
{
	static const struct {
		const char *name;
		uint32_t halt;
	} clocks[] = { { "slv_axi", 0x6b020 }, { "mstr_axi", 0x6b02c }, { "cfg_ahb", 0x6b038 },
		       { "aux", 0x6b03c }, { "pipe", 0x6b048 }, { "slv_q2a", 0x6b01c },
		       { "phy_rchng", 0x6b054 }, { "cnoc_pcie_sf_axi", 0x10058 } };
	uint32_t halted = 0;

	printf("wifi: s1: 2b/7: the clock registers again, with both power domains up\n");
	printf("wifi: s1:   vote (7 branches)        +%#x: %#x\n", CORVETTE_PCIE0_VOTE, rd(gcc + CORVETTE_PCIE0_VOTE));
	printf("wifi: s1:   vote (phy_rchng)         +%#x: %#x\n", CORVETTE_PCIE0_VOTE_RCHNG,
	    rd(gcc + CORVETTE_PCIE0_VOTE_RCHNG));
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		printf("wifi: s1:   halt %-19s +%#x: %#x\n", clocks[i].name, clocks[i].halt, rd(gcc + clocks[i].halt));
		halted |= rd(gcc + clocks[i].halt) & 1;
	}
	printf("wifi: s1:   %s\n", halted ? "a halt bit is still set: that clock's source is not running" :
	    "2b/7: every halt bit is 0, so they run (the pipe one cannot answer: its halt_check is "
	    "BRANCH_HALT_SKIP)");
}

/* 3/7: the two resets, pulsed as Linux's qcom_pcie_init_2_9_0 pulses them. Neither S0 nor S1's first runs
 * did this, on the reading that a reset register reading 0 means "not held"; a core that has never been
 * reset is not the same as a core that is out of reset, and this core's port logic looked exactly like the
 * former. Asserted, two milliseconds, released, two milliseconds. */
static void
kiwi_s1_resets(vm_offset_t gcc)
{
	printf("wifi: s1: 3/7: the two resets\n");
	printf("wifi: s1:   pcie_0_core reset        +%#x: %#x\n", CORVETTE_PCIE0_BCR, rd(gcc + CORVETTE_PCIE0_BCR));
	printf("wifi: s1:   pcie_0_phy reset         +%#x: %#x\n", CORVETTE_PCIE0_PHY_BCR,
	    rd(gcc + CORVETTE_PCIE0_PHY_BCR));
	kiwi_s1_write("pcie_0_core reset: held", CORVETTE_PCIE0_BCR, gcc + CORVETTE_PCIE0_BCR, 1);
	kiwi_s1_write("pcie_0_phy reset: held", CORVETTE_PCIE0_PHY_BCR, gcc + CORVETTE_PCIE0_PHY_BCR, 1);
	sleep_ms(2);
	kiwi_s1_write("pcie_0_core reset: released", CORVETTE_PCIE0_BCR, gcc + CORVETTE_PCIE0_BCR, 0);
	kiwi_s1_write("pcie_0_phy reset: released", CORVETTE_PCIE0_PHY_BCR, gcc + CORVETTE_PCIE0_PHY_BCR, 0);
	sleep_ms(2);
	printf("wifi: s1: 3/7: pulsed: core %#x, phy %#x (bit 0 would be held)\n", rd(gcc + CORVETTE_PCIE0_BCR),
	    rd(gcc + CORVETTE_PCIE0_PHY_BCR));

	/*
	 * And the three the device tree does not name. gcc-pineapple.c has five resets for this port, not two:
	 *
	 *	GCC_PCIE_0_BCR                    0x6b000   the core        (pulsed above)
	 *	GCC_PCIE_0_PHY_BCR                0x6c01c   the phy         (pulsed above)
	 *	GCC_PCIE_0_LINK_DOWN_BCR          0x6c014   link down       -- never touched by S1
	 *	GCC_PCIE_0_NOCSR_COM_PHY_BCR      0x6c020   the SerDes' common block -- never touched
	 *	GCC_PCIE_0_PHY_NOCSR_COM_PHY_BCR  0x6c028   and its NOC side     -- never touched
	 *
	 * The COM PHY is what holds the SerDes' PLL. A port whose COM PHY is still in reset answers every
	 * register write, holds its power domain up, and never clears PHYSTATUS -- which is exactly what S1
	 * has been looking at for five runs. They are read first, then pulsed the way an SBR does it.
	 */
	{
		static const struct {
			const char *name;
			uint32_t at;
		} more[] = { { "link down", 0x6c014 }, { "com phy", 0x6c020 }, { "com phy nocser", 0x6c028 },
			     { "pcie phy", 0x6f000 }, { "pcie phy cfg ahb", 0x6f00c } };

		printf("wifi: s1: 3b/7: the other resets of this port, and of the phy block beside it\n");
		for (unsigned i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
			printf("wifi: s1:   %-19s +%#x: %#x%s\n", more[i].name, more[i].at,
			    rd(gcc + more[i].at), rd(gcc + more[i].at) & 1 ? " (held)" : "");
		}
		for (unsigned i = 0; i < 3; i++) {
			kiwi_s1_write("com phy reset: held", more[i].at, gcc + more[i].at, 1);
			sleep_ms(1);
			kiwi_s1_write("com phy reset: released", more[i].at, gcc + more[i].at, 0);
			sleep_ms(1);
		}
		/* The two global ones are only released if they are held: pcie1 shares them. */
		for (unsigned i = 3; i < sizeof(more) / sizeof(more[0]); i++) {
			if (rd(gcc + more[i].at) & 1) {
				kiwi_s1_write("phy block reset: released", more[i].at, gcc + more[i].at, 0);
			}
		}
		printf("wifi: s1: 3b/7: com phy %#x %#x, link down %#x, phy block %#x %#x\n", rd(gcc + 0x6c020),
		    rd(gcc + 0x6c028), rd(gcc + 0x6c014), rd(gcc + 0x6f000), rd(gcc + 0x6f00c));
	}
}

/* 4/7: the two pins, with the offsets hakios.goodix.c uses for its own (TLMM + 0x1000 * pin, io four bytes
 * in). wlan-en is driven high here; perst is held low and released in 6/7, one moment before training, the
 * way Linux does it -- a radio that comes out of reset into a half-configured controller is worse off than
 * one that waits. */
static void
kiwi_s1_pins(vm_offset_t tlmm)
{
	uint32_t wlan = 0x1000UL * CORVETTE_PIN_WLAN_EN, perst = 0x1000UL * CORVETTE_PIN_PERST;

	printf("wifi: s1: 4/7: the pins, at TLMM %#x\n", CORVETTE_TLMM_PHYS);
	printf("wifi: s1:   pin %u wlan-en config    +%#x: %#x\n", CORVETTE_PIN_WLAN_EN, wlan, rd(tlmm + wlan));
	printf("wifi: s1:   pin %u wlan-en io        +%#x: %#x\n", CORVETTE_PIN_WLAN_EN, wlan + 4, rd(tlmm + wlan + 4));
	kiwi_s1_write("pin 16 wlan-en io: keep radio disabled until rails are on", wlan + 4,
	    tlmm + wlan + 4, 0);
	kiwi_s1_write("pin 16 wlan-en config", wlan, tlmm + wlan, 0x2c0);
	printf("wifi: s1:   pin %u is %s\n", CORVETTE_PIN_WLAN_EN, (rd(tlmm + wlan + 4) & 1) ? "high" : "low");
	printf("wifi: s1:   pin %u perst config      +%#x: %#x\n", CORVETTE_PIN_PERST, perst, rd(tlmm + perst));
	kiwi_s1_write("pin 94 perst io: low", perst + 4, tlmm + perst + 4, 0);
	kiwi_s1_write("pin 94 perst config", perst, tlmm + perst, 0x2c0);
	printf("wifi: s1:   pin %u is %s: held until the link is about to be started\n", CORVETTE_PIN_PERST,
	    (rd(tlmm + perst + 4) & 1) ? "high" : "low");
}

/*
 * 5/7: the PHY's own sequence, straight from the phone's device tree (qcom,phy-sequence): triples of
 * register offset, value and a delay in milliseconds, 115 of them. Every write prints what it overwrote and
 * what reads back, so a write that does not stick is the last line of the log rather than a mystery three
 * steps later.
 */
/*
 * The reference clock, out to the far end.
 *
 * The device tree gives this port fifteen clocks, and the one S1 had never heard of is the one that
 * matters most: "pcie_clkref_en", which comes from the TCSR clock controller at 0x1f40000 and is the
 * switch that drives the hundred megahertz reference clock out of the SoC onto the reference clock pins.
 * The vendor's clock driver (drivers/clk/qcom/tcsrcc-pineapple.c) has it as tcsr_pcie_0_clkref_en: an
 * enable register at 0xb1100, bit 0.
 *
 * Without it the radio, sitting at the far end of these lanes, has no reference clock at all. Its
 * receiver cannot work, so it cannot answer the host's receiver detection, and the state machine on
 * this side sends pulses into a wire that nothing is listening on -- which is what S1 has been seeing:
 * the machine starts, says "detect.active", and falls back to "detect.quiet" because nobody answered.
 *
 * The device tree's numbering for this clock is ambiguous -- the entry sits in a list the decompiler
 * merged out of several properties, and the id it carries is the second controller's -- so both of the
 * controller's PCIe clock-reference enables are set. The second one belongs to the other PCIe port,
 * which this board does not use for a radio; setting it turns on an output nothing reads.
 */
#define CORVETTE_TCSR_PHYS          0x1f40000      /* dt: clock-controller@1f40000, qcom,pineapple-tcsrcc */
#define CORVETTE_TCSR_SIZE          0xc0000
#define CORVETTE_TCSR_CLKREF_EN_0   0xb1100        /* tcsr_pcie_0_clkref_en */
#define CORVETTE_TCSR_CLKREF_EN_1   0xb1114        /* tcsr_pcie_1_clkref_en */

static void
kiwi_s1_clkref(void)
{
	vm_offset_t tcsr = corvette_s1_io_map(CORVETTE_TCSR_PHYS, CORVETTE_TCSR_SIZE);

	printf("wifi: s1: 5/7: the reference clock out to the radio (tcsr %#x)\n", CORVETTE_TCSR_PHYS);
	kiwi_s1_write("tcsr pcie 0 clock reference enable", CORVETTE_TCSR_CLKREF_EN_0,
	    tcsr + CORVETTE_TCSR_CLKREF_EN_0, rd(tcsr + CORVETTE_TCSR_CLKREF_EN_0) | 1u);
	kiwi_s1_write("tcsr pcie 1 clock reference enable", CORVETTE_TCSR_CLKREF_EN_1,
	    tcsr + CORVETTE_TCSR_CLKREF_EN_1, rd(tcsr + CORVETTE_TCSR_CLKREF_EN_1) | 1u);
	printf("wifi: s1:   the clock reference enables now: pcie 0 %#x, pcie 1 %#x\n",
	    rd(tcsr + CORVETTE_TCSR_CLKREF_EN_0), rd(tcsr + CORVETTE_TCSR_CLKREF_EN_1));
}

/* dt: qcom,phy-sequence: 115 triples of (offset, value, delay in ms), written in this order by the
 * vendor's pcie_phy_init() (pci-msm.c:4697-4709). The existing 5/7 step and the vendor port below share
 * the one table. */
static const uint32_t kiwi_phy_sequence[] = {
	0x240, 0x03, 0x00, 0x0c0, 0x01, 0x00, 0x0cc, 0x62, 0x00, 0x0d0, 0x02, 0x00, 0x060, 0xf8, 0x00,
	0x064, 0x01, 0x00, 0x000, 0x93, 0x00, 0x004, 0x01, 0x00, 0x0e0, 0x90, 0x00, 0x0e4, 0x82, 0x00,
	0x0f4, 0x07, 0x00, 0x070, 0x02, 0x00, 0x010, 0x02, 0x00, 0x074, 0x16, 0x00, 0x014, 0x16, 0x00,
	0x078, 0x36, 0x00, 0x018, 0x36, 0x00, 0x110, 0x08, 0x00, 0x0bc, 0x0a, 0x00, 0x120, 0x42, 0x00,
	0x080, 0x04, 0x00, 0x084, 0x0d, 0x00, 0x020, 0x0a, 0x00, 0x024, 0x1a, 0x00, 0x088, 0x41, 0x00,
	0x028, 0x34, 0x00, 0x090, 0xab, 0x00, 0x094, 0xaa, 0x00, 0x098, 0x01, 0x00, 0x030, 0x55, 0x00,
	0x034, 0x55, 0x00, 0x038, 0x01, 0x00, 0x140, 0x14, 0x00, 0x164, 0x34, 0x00, 0x03c, 0x01, 0x00,
	0x01c, 0x04, 0x00, 0x174, 0x16, 0x00, 0x1bc, 0x0f, 0x00, 0x170, 0xa0, 0x00, 0x11a4, 0x38, 0x00,
	0x10dc, 0x11, 0x00, 0x1160, 0xbf, 0x00, 0x1164, 0xbf, 0x00, 0x1168, 0xb7, 0x00,
	0x116c, 0xea, 0x00, 0x115c, 0x3f, 0x00, 0x1174, 0x5c, 0x00, 0x1178, 0x9c, 0x00,
	0x117c, 0x1a, 0x00, 0x1180, 0x89, 0x00, 0x1170, 0xdc, 0x00, 0x1188, 0x94, 0x00,
	0x118c, 0x5b, 0x00, 0x1190, 0x1a, 0x00, 0x1194, 0x89, 0x00, 0x10cc, 0x00, 0x00,
	0x1008, 0x09, 0x00, 0x1014, 0x05, 0x00, 0x104c, 0x08, 0x00, 0x1050, 0x08, 0x00,
	0x10d8, 0x0f, 0x00, 0x1118, 0x1c, 0x00, 0x10f8, 0x07, 0x00, 0x11f8, 0x08, 0x00,
	0x1600, 0x00, 0x00, 0xe84, 0x15, 0x00, 0xe90, 0x3f, 0x00, 0xee4, 0x02, 0x00, 0xe40, 0x09, 0x00,
	0xe3c, 0x15, 0x00, 0x19a4, 0x38, 0x00, 0x18dc, 0x11, 0x00, 0x1960, 0xbf, 0x00,
	0x1964, 0xbf, 0x00, 0x1968, 0xb7, 0x00, 0x196c, 0xea, 0x00, 0x195c, 0x3f, 0x00,
	0x1974, 0x5c, 0x00, 0x1978, 0x9c, 0x00, 0x197c, 0x1a, 0x00, 0x1980, 0x89, 0x00,
	0x1970, 0xdc, 0x00, 0x1988, 0x94, 0x00, 0x198c, 0x5b, 0x00, 0x1990, 0x1a, 0x00,
	0x1994, 0x89, 0x00, 0x18cc, 0x00, 0x00, 0x1808, 0x09, 0x00, 0x1814, 0x05, 0x00,
	0x184c, 0x08, 0x00, 0x1850, 0x08, 0x00, 0x18d8, 0x0f, 0x00, 0x1918, 0x1c, 0x00,
	0x18f8, 0x07, 0x00, 0x19f8, 0x08, 0x00, 0x1684, 0x15, 0x00, 0x1690, 0x3f, 0x00,
	0x16e4, 0x02, 0x00, 0x1640, 0x09, 0x00, 0x163c, 0x15, 0x00, 0x2dc, 0x05, 0x00,
	0x388, 0x77, 0x00, 0x398, 0x0b, 0x00, 0x6a4, 0x1e, 0x00, 0x6f4, 0x27, 0x00, 0x3e0, 0x0f, 0x00,
	0x60c, 0x1d, 0x00, 0x614, 0x07, 0x00, 0x620, 0xc1, 0x00, 0x694, 0x00, 0x00, 0x3d0, 0x8c, 0x00,
	0x368, 0x17, 0x00, 0x370, 0x2e, 0x00, 0x200, 0x00, 0x00, 0x244, 0x03, 0x00
};
#define KIWI_PHY_SEQUENCE_WRITES (sizeof(kiwi_phy_sequence) / sizeof(kiwi_phy_sequence[0]) / 3)

static bool
kiwi_s1_phy(vm_offset_t parf)
{
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;
	uint32_t writes = KIWI_PHY_SEQUENCE_WRITES, status = 0, polls;

	/* the reference clock the radio needs, before the PHY is given its tables */
	kiwi_s1_clkref();

	/* The device type has to be RC before the PHY is given its tables. The vendor's
	 * msm_pcie_enable_link() writes DEVICE_TYPE = 0x4 at its very top and only calls
	 * pcie_phy_init() later; S1 used to run the whole PHY sequence in 5/7 and only set the device
	 * type afterwards in 5b/7, so the PHY initialised without knowing it is the root complex. */
	if ((rd(parf + CORVETTE_PARF_DEVICE_TYPE_OFF) & 0xf) != CORVETTE_PARF_DEVICE_TYPE_RC) {
		kiwi_s1_write("device type: RC before the PHY tables", CORVETTE_PARF_DEVICE_TYPE_OFF,
		    parf + CORVETTE_PARF_DEVICE_TYPE_OFF, CORVETTE_PARF_DEVICE_TYPE_RC);
	} else {
		printf("wifi: s1: 5/7: device type is already RC (%#x) before the PHY tables\n",
		    rd(parf + CORVETTE_PARF_DEVICE_TYPE_OFF));
	}

	printf("wifi: s1: 5/7: the PHY's sequence, %u writes from %#x (dt: qcom,phy-sequence)\n", writes,
	    CORVETTE_PCIE0_PHY);
	for (uint32_t i = 0; i < writes; i++) {
		uint32_t offset = kiwi_phy_sequence[3 * i], value = kiwi_phy_sequence[3 * i + 1],
		    delay = kiwi_phy_sequence[3 * i + 2];
		uint32_t before = rd(phy + offset);

		printf("wifi: s1:   phy %3u/%u +%#05x: %#x -> %#x\n", i + 1, writes, offset, before, value);
		wr(phy + offset, value);
		printf("wifi: s1:                  +%#05x reads %#x\n", offset, rd(phy + offset));
		if (delay) {
			sleep_ms(delay);
		}
	}
	/*
	 * PHYSTATUS is bit 6 of 0x214, and it means the opposite of what S1 assumed for eleven runs: Linux's
	 * qmp_pcie_power_on() polls it and waits for it to be CLEAR, and so does the vendor's
	 * pcie_phy_is_ready() -- "if (readl(phy + phy_status_offset) & BIT(phy_status_bit)) return false;".
	 * Every run so far printed "ready 1" and went on; the PHY was in fact timed out, which is why no pipe
	 * clock ever came out of it and why the link state machine never left detect.quiet.
	 */
	for (polls = 0; polls < 200; polls++) {
		status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
		if (!(status & CORVETTE_PHY_STATUS_READY)) {
			break;
		}
		sleep_ms(1);
	}
	printf("wifi: s1:   phy +%#x = %#x: %s after %u ms; power down %#x, start %#x\n",
	    CORVETTE_PHY_STATUS_OFFSET, status,
	    (status & CORVETTE_PHY_STATUS_READY) ? "NOT READY (bit 6 still set)" : "ready (bit 6 clear)", polls,
	    rd(phy + 0x240), rd(phy + 0x244));
	/* The state registers the phone's own device tree lists for debugging (qcom,phy-debug-reg): they say
	 * whether the SerDes' PLL locked and whether the PCS started, which the one "ready" bit does not. */
	printf("wifi: s1:   the phy's own debug: %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x\n",
	    rd(phy + 0x214), rd(phy + 0x218), rd(phy + 0x21c), rd(phy + 0x220), rd(phy + 0x224), rd(phy + 0x228),
	    rd(phy + 0x22c), rd(phy + 0x230), rd(phy + 0x234), rd(phy + 0x238), rd(phy + 0x600));

	/*
	 * If it is not ready, 0x240 comes next -- and its polarity is the opposite of what S1 assumed. Linux
	 * clears those two bits in qmp_pcie_power_off() with the comment "Put PHY into POWER DOWN state:
	 * active low", and qmp_pcie_init() sets them. So 3 is powered and 0 is powered down; the sequence's
	 * first write of 3 is the PHY being switched on, and every "powered up" write S1 made before this one
	 * wrote 0, which switched it off. That also means the reference-clock experiments of the earlier runs
	 * were all done on a PHY that had just been powered down, and their verdict has to be taken again.
	 */
	if (status & CORVETTE_PHY_STATUS_READY) {
		kiwi_s1_write("phy 0x240: powered (three is on)", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
		kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
		sleep_ms(1);
		kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
		for (polls = 0; polls < 200; polls++) {
			status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
			if (!(status & CORVETTE_PHY_STATUS_READY)) {
				break;
			}
			sleep_ms(1);
		}
		printf("wifi: s1:   after it: %#x (bit 6 %u) after %u ms; 0x218 %#x, 0x21c %#x, 0x600 %#x\n", status,
		    (status >> 6) & 1, polls, rd(phy + 0x218), rd(phy + 0x21c), rd(phy + 0x600));
	}
	if (status & CORVETTE_PHY_STATUS_READY) {
		/* Then the variant that keeps the reference-clock driver disabled, for a PHY fed from the pad. */
		kiwi_s1_write("phy 0x240: still powered", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
		sleep_ms(1);
		kiwi_s1_write("phy 0x244: start again", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
		for (polls = 0; polls < 200; polls++) {
			status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
			if (!(status & CORVETTE_PHY_STATUS_READY)) {
				break;
			}
			sleep_ms(1);
		}
		printf("wifi: s1:   after that: %#x (bit 6 %u) after %u ms\n", status, (status >> 6) & 1, polls);
	}
	printf("wifi: s1:   5/7: the phy is %s. S1 goes on either way, so one run still ends with everything "
	    "else reported.\n", (status & CORVETTE_PHY_STATUS_READY) ?
	    "NOT ready: no pipe clock can come out of it and the link cannot train" : "ready");
	return true;
}

/*
 * The 43 offsets the phone's own device tree lists for this PHY (qcom,phy-debug-reg), one per line so that
 * the offset is already in the log when the value is read -- if one of the SerDes windows aborts, the log
 * names the register that caused it.
 *
 * Air183 read this set once, in 5i/7, which is before the RPMh vote for the PHY's two parent rails. Air184
 * reads it at both points: the run showed the link state only starts moving after the vote and the second
 * sequence, so the set taken then is the one that describes a PHY that works.
 */
static void
kiwi_s1_phy_regs(vm_offset_t parf, const char *when)
{
	static const uint32_t debug[] = {
	    0x1cc, 0x1d0, 0x1d4, 0x1d8, 0x1dc, 0x1e0, 0x1e4, 0x1f8,
	    0xed0, 0x16d0, 0xedc, 0x16dc, 0x11e0, 0x19e0, 0xa00, 0x1200,
	    0xa04, 0x1204, 0xa08, 0x1208, 0xa0c, 0x120c, 0xa10, 0x1210,
	    0xa14, 0x1214, 0xa18, 0x1218, 0xc20, 0x1420, 0x214, 0x218,
	    0x21c, 0x220, 0x224, 0x228, 0x22c, 0x230, 0x234, 0x238,
	    0x23c, 0x600, 0x604,
	};
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;
	unsigned count = sizeof(debug) / sizeof(debug[0]);

	printf("wifi: s1:   the phy's debug set %s, %u offsets from qcom,phy-debug-reg, one per line:\n", when,
	    count);
	for (unsigned i = 0; i < count; i++) {
		printf("wifi: s1:   +%#05x = ", debug[i]);
		printf("%#x\n", rd(phy + debug[i]));
	}
}

/*
 * 5i/7: the PHY's own opinion, and the two things that could still be missing.
 *
 * 5/7 establishes that PHYSTATUS (bit 6 of 0x214) never clears, and that neither power-down control nor a
 * second start changes it. The device tree names a whole set of registers for exactly this (qcom,phy-debug-reg,
 * 43 of them, PCS and SerDes), so they are read first: whether the SerDes' PLL locked is in there, and a PLL
 * that has not locked explains PHYSTATUS staying set whatever else is written.
 *
 * Then the two candidates that a PLL needs:
 *
 *  - the reference clock. PARF 0x4c bit 12 says whether the PHY takes it from the pad or from the SoC's own
 *    source; both are tried, and PHYSTATUS is the observer this time -- unlike a link state that cannot move
 *    without a pipe clock, this bit answers on its own.
 *  - the PHY's reset, pulsed here rather than in 3/7: some PHYs latch their configuration only when the reset
 *    goes away after the tables are in, and 3/7 lets it go before.
 */
static void
kiwi_s1_phy_deep(vm_offset_t gcc, vm_offset_t parf)
{
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;
	uint32_t refclk = rd(parf + CORVETTE_PARF_PHY_REFCLK), polls;
	/* Read before anything else: the reference-clock loop below asks whether the PHY is still down, and
	 * an uninitialised answer would let it skip all four combinations and report a ready PHY that is not. */
	uint32_t status = rd(parf + CORVETTE_PCIE0_PHY_AT + CORVETTE_PHY_STATUS_OFFSET);

	/*
	 * The experiments come first and the reading last. S1's twenty-second run went 900E here: reading the
	 * whole qcom,phy-debug-reg set while the PHY is powered down is not safe -- the SerDes' own window
	 * answers only when it is clocked, and an unclocked window on this NoC aborts instead of reading zero.
	 * So the twelve registers that have answered in every run are read first, one per line and after the
	 * offset is printed, and the rest of the set is read only once PHYSTATUS says the PHY is up.
	 */
	{
		static const uint32_t always[] = { 0x214, 0x218, 0x21c, 0x600, 0x604 };

		printf("wifi: s1:   the registers that have always answered, one per line so that the offset is in "
		    "the log before it is read:\n");
		for (unsigned i = 0; i < sizeof(always) / sizeof(always[0]); i++) {
			printf("wifi: s1:   +%#05x = ", always[i]);
			printf("%#x\n", rd(phy + always[i]));
		}
	}

	/*
	 * The reference clock: bit 9 and bit 12 of PARF 0x4c, all four ways. mainline calls bit 12 USE_PAD and
	 * bit 16 the spread-spectrum enable; several vendor trees put USE_PAD at bit 9 and the external clock
	 * at 12. Nothing here decides which is right -- the four combinations are tried, with 0x240 held at 3
	 * throughout so that the PHY is powered while it is asked, and PHYSTATUS is the judge.
	 */
	printf("wifi: s1:   parf refclk %#x: bit 9 %u, bit 12 %u; trying all four\n", refclk, (refclk >> 9) & 1,
	    (refclk >> 12) & 1);
	for (unsigned bits = 0; bits < 4 && (status & CORVETTE_PHY_STATUS_READY); bits++) {
		uint32_t want = (refclk & ~((1u << 9) | (1u << 12))) | ((bits & 1u) << 9) | (((bits >> 1) & 1u) << 12);

		kiwi_s1_write("refclk: a combination", CORVETTE_PARF_PHY_REFCLK, parf + CORVETTE_PARF_PHY_REFCLK, want);
		sleep_ms(2);
		kiwi_s1_write("phy 0x240: powered (three is on)", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
		kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
		kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
		for (polls = 0; polls < 100; polls++) {
			status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
			if (!(status & CORVETTE_PHY_STATUS_READY)) {
				break;
			}
			sleep_ms(1);
		}
		printf("wifi: s1:   with refclk bit9 %u bit12 %u: phy %#x after %u ms, 0x240 %#x, 0x244 %#x\n",
		    (want >> 9) & 1, (want >> 12) & 1, status, polls, rd(phy + 0x240), rd(phy + 0x244));
	}
	if (status & CORVETTE_PHY_STATUS_READY) {
		/* Neither source helped. Put the bits back where they were and try the reset behind the tables. */
		kiwi_s1_write("refclk: as it was", CORVETTE_PARF_PHY_REFCLK, parf + CORVETTE_PARF_PHY_REFCLK, refclk);
		kiwi_s1_write("pcie_0_phy reset: held", CORVETTE_PCIE0_PHY_BCR, gcc + CORVETTE_PCIE0_PHY_BCR, 1);
		sleep_ms(2);
		kiwi_s1_write("pcie_0_phy reset: released", CORVETTE_PCIE0_PHY_BCR, gcc + CORVETTE_PCIE0_PHY_BCR, 0);
		sleep_ms(2);
		kiwi_s1_write("phy 0x240: powered (three is on)", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
		kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
		kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
		for (polls = 0; polls < 200; polls++) {
			status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
			if (!(status & CORVETTE_PHY_STATUS_READY)) {
				break;
			}
			sleep_ms(1);
		}
		printf("wifi: s1:   after a reset behind the tables: phy %#x after %u ms\n", status, polls);
	}
	printf("wifi: s1:   5i/7: the phy is %s\n", (status & CORVETTE_PHY_STATUS_READY) ?
	    "still not ready: its reference clock or its supply is missing, not its configuration" : "ready");

	/*
	 * Air183: the whole set is read whether or not PHYSTATUS cleared.
	 *
	 * Runs up to air182 skipped it whenever bit 6 of 0x214 was still set, so the only SerDes registers this
	 * port ever saw were the five that answer cold. Air184 keeps that, and adds a second read of the same
	 * set in 5k/7, after the PHY's parent rails have been voted.
	 */
	if (status & CORVETTE_PHY_STATUS_READY) {
		kiwi_s1_write("phy 0x240: powered (three is on)", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
		kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
		kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
		sleep_ms(5);
		printf("wifi: s1:   the phy is still down (0x214 %#x, 0x240 %#x); the set is read anyway\n",
		    rd(phy + CORVETTE_PHY_STATUS_OFFSET), rd(phy + 0x240));
	}
	kiwi_s1_phy_regs(parf, "after the sequence, before the rails are voted");
}

/*
 * 5j/7: the two rails this port hangs from, read-only.
 *
 * The device tree settles it: gcc_pcie_0_gdsc's parent-supply is pm_v8_s8_level and gcc_pcie_0_phy_gdsc's is
 * pm_v8_s3_level, both "qcom,rpmh-arc-regulator", and the nodes they sit in carry qcom,resource-name
 * "cx.lvl" and "mx.lvl". A GDSC on a rail that nobody has voted answers its own handshake bits -- which is
 * what 2/7 has read for twenty runs -- while the domain behind it has no supply at all. That is the shape
 * of every symptom here: the PHY's registers take writes and read back, the PCS reports, and the SerDes'
 * PLL never starts because there is no power under it.
 *
 * Nothing is written here. Linux votes these rails through RPMh's ARC drivers; this reads what the boot
 * loader's own votes left in the sleep and wake sets of the sequencer, which is what a vote looks like
 * when it is in place, and the level tables so that the vote can be written on the next run and not
 * guessed at. The first set is not read: its slots are transient and reading them has cost this project a
 * 900E before.
 */
static void
kiwi_s1_rpmh(void)
{
	static const char *const names[2] = { "cx.lvl", "mx.lvl" };

	printf("wifi: s1: 5j/7: the rails under gcc_pcie_0_gdsc (pm_v8_s8_level -> cx.lvl) and under "
	    "gcc_pcie_0_phy_gdsc (pm_v8_s3_level -> mx.lvl)\n");
	if (!cmd_db_there()) {
		printf("wifi: s1:   the boot loader's list of RPMh resources is not there\n");
		return;
	}
	for (unsigned n = 0; n < 2; n++) {
		const struct cmd_db_entry *entry = rpmh_resource(names[n], 0);
		char levels[160];
		int at = 0;

		if (entry == NULL) {
			printf("wifi: s1:   RPMh has no \"%s\"\n", names[n]);
			continue;
		}
		levels[0] = 0;
		for (unsigned s = 0; s < 8; s++) {
			const struct cmd_db_slave *slave = &cmd_db->slave[s];
			const struct cmd_db_entry *first =
			    (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);
			const uint8_t *data;

			if (entry < first || entry >= first + slave->count ||
			    (unsigned)slave->data_offset + entry->offset + entry->length > CMD_DB_SIZE - 0x1000) {
				continue;
			}
			data = cmd_db->data + slave->data_offset + entry->offset;
			at = snprintf(levels, sizeof(levels), ", levels:");
			for (unsigned l = 0; l < entry->length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
				unsigned level = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && level == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
			}
		}
		printf("wifi: s1:   %s at %#x%s\n", names[n], entry->address, levels);
	}

	/*
	 * The sequencer itself. corvette's apps_rsc is at 0x17a00000 with three DRVs of 0x10000 each and the
	 * operating system's is the third (dt: reg-names drv-0..drv-2, drv@2), whose TCSs start at 0xd00 and
	 * number five (qcom,tcs-config = <2 3 0 2 1 2 3 0 4 1>: two at-once, two asleep, one awake). The
	 * 0x18220000 the rest of this file uses is redwood's, and reading it here is what S1's twenty-fifth run
	 * died of. The first two TCSs are not read: they hold requests in flight.
	 */
	{
		struct block apps = { "apps rsc", 0x17a20000, 0x10000 };

		need(&apps);
		rsc.base = apps.base;
	}
	printf("wifi: s1:   what the last votes left in the sleep and wake sets (TCS 2..4 of drv-2):\n");
	for (unsigned t = 2; t < 5; t++) {
		uint32_t enabled = rd(rsc.base + RSC_TCS(t) + TCS_ENABLED);

		if (enabled == 0) {
			printf("wifi: s1:   tcs %u: nothing enabled\n", t);
			continue;
		}
		for (unsigned c = 0; c < 16; c++) {
			const struct cmd_db_entry *entry;
			uint32_t address;

			if (!(enabled & (1u << c))) {
				continue;
			}
			address = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 4);
			entry = rpmh_resource(NULL, address);
			printf("wifi: s1:   tcs %u cmd %u: %#x = %#x (%s)\n", t, c, address,
			    rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 8),
			    entry != NULL ? entry->id : "not in the list");
		}
	}
	printf("wifi: s1:   5j/7: nothing was written; the next run votes these two by level, with the numbers "
	    "above\n");
}

/*
 * 5k/7: the vote itself.
 *
 * 5j/7 answered what was missing: cx.lvl sits at 0x30000 and mx.lvl at 0x30010, both with the level table
 * 0 16 64 128 192 256 320 384 416 480, and no TCS of the sleep or wake set holds a vote -- the boot loader
 * left none. So the two parents of this port's power domains have never been asked for anything, and a GDSC
 * whose parent rail is unvoted reports its own handshake while the domain behind it has no supply. That is
 * the whole of the PHY's behaviour for twenty runs.
 *
 * The command is written the way hakios.mpss.c writes the modem's, which is the one path on this phone that
 * has been seen to work: one command, in the first of the two "at once" TCSs, waited for, and the TCS put
 * back as it was found. The value is the position of the level in the rail's own table, not the level
 * itself, which is what that path sends.
 *
 * The level is 384: the modem asks for it for cx and mss on this phone, and it is the highest entry below
 * the two turbo ones, so nothing is being pushed beyond what this SoC already runs at.
 *
 * The PHY's sequence is written again afterwards, because it was written while the rail was down: whatever
 * it put into the SerDes was put into a block that had no supply under it.
 */
#define CORVETTE_RSC_PHYS       0x17a20000      /* dt: apps_rsc rsc@17a00000, drv@2 (reg-names "drv-2") */
#define CORVETTE_RSC_IRQ_STATUS (0xd00 + 0x04)
#define CORVETTE_RSC_IRQ_CLEAR  (0xd00 + 0x08)
#define CORVETTE_TCS_ACTIVE     (1u << 16)
#define CORVETTE_TCS_TRIGGER    (1u << 24)
#define CORVETTE_MSGID_WRITE    (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define CORVETTE_RAIL_LEVEL     384              /* the level the modem path asks cx and mss for */

/*
 * Where a TCS's registers are depends on the RSC's major version, and drivers/soc/qcom/rpmh-rsc.c has two
 * tables for it: version 2.7 has a command stride of 20 bytes with CONTROL at 0x14, version 3.0 has 24 with
 * CONTROL at 0x24. S1's twenty-eighth run wrote the 2.7 layout and read back a control register of 0 and a
 * command status of 0x6023e28f -- which is not a status at all, it is another register. The version is in
 * the DRV's first word, bits [23:16].
 */
struct kiwi_tcs {
	uint32_t stride;        /* bytes between commands */
	uint32_t control;
	uint32_t status;
	uint32_t enable;
	uint32_t msgid;
	uint32_t addr;
	uint32_t data;
	uint32_t cmdsts;
	uint32_t wait;
	uint32_t resp;
};

static const struct kiwi_tcs kiwi_tcs_2_7 = { 20, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 0x10, 0x40 };
static const struct kiwi_tcs kiwi_tcs_3_0 = { 24, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 0x20, 0x44 };

static const struct kiwi_tcs *
kiwi_s1_tcs_layout(vm_offset_t base)
{
	static const struct kiwi_tcs *chosen;   /* (one controller, so the answer does not change) */

	if (chosen == NULL) {
		uint32_t id = rd(base);

		printf("wifi: s1:   the sequencer's drv: id %#x (major %u), %u TCSs\n", id, (id >> 16) & 0xff,
		    (id >> 6) & 0x3f);
		chosen = ((id >> 16) & 0xff) >= 3 ? &kiwi_tcs_3_0 : &kiwi_tcs_2_7;
	}
	return chosen;
}

/* The position of that level in the rail's own table: what RPMh is given, not the level. */
static int
kiwi_s1_rail_position(const struct cmd_db_entry *entry, uint32_t level)
{
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *first = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);
		const uint8_t *data;

		if (entry < first || entry >= first + slave->count) {
			continue;
		}
		if ((unsigned)slave->data_offset + entry->offset + entry->length > CMD_DB_SIZE - 0x1000) {
			continue;
		}
		data = cmd_db->data + slave->data_offset + entry->offset;
		for (unsigned l = 0; l < entry->length / 2u && l < 16; l++) {
			unsigned got = data[2 * l] | data[2 * l + 1] << 8;

			if (got == level) {
				return (int)l;
			}
			if (got == 0 && l > 0) {
				break;
			}
		}
	}
	return -1;
}

/*
 * One command, in one TCS: cleared, written, enabled, triggered with MODE_ENABLE|TRIGGER, and then the
 * command's own status word is polled for COMPLETE (bit 16) -- the interrupt is not used, so nothing depends
 * on an IRQ that this kernel has no table for.
 */
static bool
kiwi_s1_vote(const char *name, uint32_t level, vm_offset_t base, uint32_t tcs)
{
	const struct cmd_db_entry *entry = rpmh_resource(name, 0);
	const struct kiwi_tcs *t = kiwi_s1_tcs_layout(base);
	vm_offset_t at, irq;
	uint32_t address, status = 0, polls;
	uint64_t began;
	int position;

	if (entry == NULL) {
		printf("wifi: s1:   RPMh has no \"%s\": not voted\n", name);
		return false;
	}
	position = kiwi_s1_rail_position(entry, level);
	if (position < 0) {
		printf("wifi: s1:   %s has no level %u in its table: not voted\n", name, level);
		return false;
	}
	address = entry->address;
	at = base + 0xd00 + 672 * tcs;
	irq = base + 0xd00;
	printf("wifi: s1: 5k/7: %s at %#x = position %d (level %u), one command in TCS %u (control at %#x)\n",
	    name, address, position, level, tcs, t->control);

	wr(irq + 0x08, 1u << tcs);                              /* clear this TCS's done bit */
	wr(at + t->enable, 0);
	wr(at + t->control, 0);
	wr(at + t->msgid, CORVETTE_MSGID_WRITE);
	wr(at + t->addr, address);
	wr(at + t->data, (uint32_t)position);
	wr(at + t->enable, 1);
	sleep_us(10);
	wr(at + t->control, CORVETTE_TCS_ACTIVE | CORVETTE_TCS_TRIGGER);

	began = now_us();
	for (polls = 0; polls < 2000; polls++) {
		status = rd(at + t->cmdsts);
		if (status & (1u << 16)) {                      /* the command says it is done */
			break;
		}
		sleep_us(50);
	}
	printf("wifi: s1:   %s after %llu us: the command's status %#x (done %u), the TCS's %#x, control %#x\n",
	    (status & (1u << 16)) ? "RPMh HAS DONE IT" : "RPMh HAS NOT SAID DONE", now_us() - began, status,
	    (status >> 16) & 1, rd(at + t->status), rd(at + t->control));
	wr(irq + 0x08, 1u << tcs);
	wr(at + t->control, 0);
	wr(at + t->enable, 0);
	return (status & (1u << 16)) != 0;
}

static bool
kiwi_s1_rail(const char *name, vm_offset_t base)
{
	for (uint32_t tcs = 0; tcs < 2; tcs++) {
		if (kiwi_s1_vote(name, CORVETTE_RAIL_LEVEL, base, tcs)) {
			return true;
		}
	}
	return false;
}

/*
 * The GPU's bring-up (hakios.adreno.c) needs one vote of its own: gpu_cc_gx_gdsc's parent rail is
 * gfx.lvl and the vendor's gdsc_toggle_logic() enables that supply before it clears the collapse bit.
 * Same sequencer, same two TCS of the awake set that S1's cx and mx votes went through.
 */
bool
hakios_rpmh_level(const char *name, uint32_t level)
{
	vm_offset_t base = corvette_s1_io_map(CORVETTE_RSC_PHYS, 0x10000);

	for (uint32_t tcs = 0; tcs < 2; tcs++) {
		if (kiwi_s1_vote(name, level, base, tcs)) {
			return true;
		}
	}
	return false;
}

/*
 * The reference clock, searched rather than guessed.
 *
 * PARF 0x4c decides where the PHY takes its hundred megahertz from, and S1 has only ever read it
 * (0x1019: use-pad set, bit 9 clear). The one time the four combinations were tried, the PHY was in
 * the wrong power state and the observation was a single sample of a bit that reads differently from one
 * boot to the next -- so the answer was never really found.
 *
 * What is flaky is the PHY's own lock: the same code reports 0x214 = 0x86 (bit 6 clear, ready) on one
 * boot and 0x41 on the next. A PLL that locks sometimes is a PLL whose reference clock is marginal, and
 * the reference clock is the one thing here that has two possible sources and no evidence for either.
 *
 * So the four combinations are tried again, each one with the PHY started afresh, and each one judged
 * by how many of fifty samples over two hundred milliseconds say the PHY is ready. A single sample
 * cannot tell a locked PLL from a lucky one; fifty can.
 */
static void
kiwi_s1_refclk_search(vm_offset_t parf)
{
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;
	uint32_t base = rd(parf + CORVETTE_PARF_PHY_REFCLK);
	uint32_t best = base, best_ready = 0, value, ready, i, j;

	printf("wifi: s1: 5k/7: the reference clock, searched: parf 0x4c reads %#x (bit 9 %u, bit 12 %u)\n", base,
	    (base >> 9) & 1, (base >> 12) & 1);
	for (i = 0; i < 4; i++) {
		value = (base & ~((1u << 9) | (1u << 12))) | ((i & 1u) ? (1u << 9) : 0) | ((i & 2u) ? (1u << 12) : 0);
		kiwi_s1_write("refclk: a combination", CORVETTE_PARF_PHY_REFCLK, parf + CORVETTE_PARF_PHY_REFCLK,
		    value);
		kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
		kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
		ready = 0;
		for (j = 0; j < 50; j++) {
			if (!(rd(phy + CORVETTE_PHY_STATUS_OFFSET) & CORVETTE_PHY_STATUS_READY)) {
				ready++;
			}
			sleep_ms(4);
		}
		printf("wifi: s1:   refclk %#x (bit 9 %u, bit 12 %u): %u of 50 samples say the phy is ready\n", value,
		    (value >> 9) & 1, (value >> 12) & 1, ready);
		if (ready > best_ready) {
			best_ready = ready;
			best = value;
		}
	}
	/* The four combinations are only observed. The vendor's msm_pcie_enable_link() never writes
	 * 0x4c, so whatever scored highest is discarded and the boot firmware's value (0x1019) goes
	 * back -- that is the value the working vendor kernel trains the link with. */
	printf("wifi: s1:   best was %#x (%u of 50 ready), but %#x goes back: the vendor never writes this register\n",
	    best, best_ready, base);
	kiwi_s1_write("refclk: the boot firmware's value back", CORVETTE_PARF_PHY_REFCLK,
	    parf + CORVETTE_PARF_PHY_REFCLK, base);
	kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
	kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);

	/*
	 * PARF 0x4c is left exactly as the boot firmware set it (0x1019). The vendor's own
	 * msm_pcie_enable_link() never writes 0x4c: it relies on the PHY sequence (0x240=3 and
	 * 0x620=0xc1) plus the TCSR clkref_en to drive the reference clock out. mainline pcie-qcom.c
	 * clears USE_PAD(12) and sets SSP_EN(16), but that is the mainline tree's own behaviour and the
	 * vendor kernel brings the radio up with 0x1019 in place -- so S1 must not overwrite it. S1 used
	 * to write 0x10019 (use pad off, ssp on), the opposite of the working vendor register value.
	 */
	value = rd(parf + CORVETTE_PARF_PHY_REFCLK);
	printf("wifi: s1:   parf refclk reads %#x: use pad %u, ssp %u; left as the boot firmware and the vendor's kernel leave it\n",
	    value, (value >> 12) & 1, (value >> 16) & 1);
	kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
	kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
}

/*
 * Air189: read the radio's clock request pin, then put it back on the PCIe function.  The pin is only
 * readable as a plain GPIO, so it is switched for the duration of one read and switched back.
 */
static uint32_t
kiwi_s1_clkreq_level(vm_offset_t tlmm)
{
	uint32_t level;

	wr(tlmm + 0x1000UL * CORVETTE_PIN_CLKREQ, CORVETTE_PIN_CLKREQ_SLEEP);
	level = rd(tlmm + 0x1000UL * CORVETTE_PIN_CLKREQ + 4) & 1;
	wr(tlmm + 0x1000UL * CORVETTE_PIN_CLKREQ, CORVETTE_PIN_CLKREQ_N);
	return level;
}

static void
kiwi_s1_rails(vm_offset_t gcc, vm_offset_t parf)
{
	vm_offset_t base = corvette_s1_io_map(CORVETTE_RSC_PHYS, 0x10000);
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;
	uint32_t status, polls;

	printf("wifi: s1: 5k/7: asking RPMh for the two rails under this port, at level %u\n",
	    CORVETTE_RAIL_LEVEL);
	if (!cmd_db_there()) {
		printf("wifi: s1:   the list of RPMh resources is not there: not voted\n");
		return;
	}
	kiwi_s1_rail("cx.lvl", base);
	sleep_ms(5);
	kiwi_s1_rail("mx.lvl", base);
	sleep_ms(5);

	printf("wifi: s1:   the domains again: core %#x %#x, phy %#x %#x\n", rd(gcc + CORVETTE_PCIE0_GDSCR),
	    rd(gcc + CORVETTE_PCIE0_GDSCR + CORVETTE_GDSC_CFG), rd(gcc + CORVETTE_PCIE0_PHY_GDSCR),
	    rd(gcc + CORVETTE_PCIE0_PHY_GDSCR + CORVETTE_GDSC_CFG));

	printf("wifi: s1:   the phy's sequence again, now that its rail has been asked for\n");
	kiwi_s1_phy(parf);
	kiwi_s1_write("phy 0x240: powered (three is on)", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
	kiwi_s1_write("phy 0x200: reset off", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);
	kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
	for (polls = 0; polls < 300; polls++) {
		status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
		if (!(status & CORVETTE_PHY_STATUS_READY)) {
			break;
		}
		sleep_ms(1);
	}
	printf("wifi: s1:   5k/7: after the vote and the sequence: phy %#x %s after %u ms\n", status,
	    (status & CORVETTE_PHY_STATUS_READY) ? "NOT ready" : "READY", polls);
	kiwi_s1_phy_regs(parf, "after the rails were voted and the sequence written again");
	/*
	 * The clock request. The device tree gives this port its own clkreq pin (95), and that pin is an
	 * input: it is the far end that asks for the reference clock, not the host that gives it. So the pin
	 * is read here, never driven -- and the controller's own override (PARF 0x2b0, CLKREQ_IN_ENABLE bit 1
	 * and CLKREQ_IN_VALUE bit 3) is what decides what the controller makes of it. Driving it as an output,
	 * which is what this used to do, is backwards.
	 *
	 * It runs whether the PHY is ready or not: for twenty runs it only ever ran when the PHY was down,
	 * so the one case where it could have mattered was the one case it never saw.
	 */
	{
		vm_offset_t tlmm = corvette_s1_io_map(CORVETTE_TLMM_PHYS, CORVETTE_TLMM_SIZE);
		uint32_t clkreq = 0x1000UL * CORVETTE_PIN_CLKREQ;

		/*
		 * The clock request is the far end pulling pin 95 low, and it reaches the PCIe controller only
		 * when the pin carries function 1 (pcie0_clk_req_n), not as a GPIO. With the pin a GPIO the level
		 * read back is real but useless: the controller never sees it, so it never drives the reference
		 * clock out and the radio's PHY never comes up.
		 */
		kiwi_s1_write("pin 95 clkreq: pcie0_clk_req_n", clkreq, tlmm + clkreq, CORVETTE_PIN_CLKREQ_N);
		printf("wifi: s1:   clkreq: pin %u cfg %#x io %#x (in %u, out %u); parf 0x2b0 %#x\n",
		    CORVETTE_PIN_CLKREQ, rd(tlmm + clkreq), rd(tlmm + clkreq + 4), rd(tlmm + clkreq + 4) & 1,
		    (rd(tlmm + clkreq + 4) >> 1) & 1, rd(parf + 0x2b0));
		printf("wifi: s1:   clkreq reads %u: %s\n", rd(tlmm + clkreq + 4) & 1,
		    (rd(tlmm + clkreq + 4) & 1) ? "the radio is not asking for a clock" :
		    "THE RADIO IS ASKING FOR A CLOCK");
		for (unsigned value = 0; value <= 2; value++) {
			uint32_t over = value == 0 ? 0 : (value == 1 ? (1u << 1) : ((1u << 1) | (1u << 3)));

			kiwi_s1_write(value == 0 ? "parf clkreq: the input is left alone" :
			    (value == 1 ? "parf clkreq: taken over, low" : "parf clkreq: taken over, high"),
			    0x2b0, parf + 0x2b0, over);
			kiwi_s1_write("phy 0x240: powered (three is on)", 0x240, phy + 0x240, rd(phy + 0x240) | 3u);
			kiwi_s1_write("phy 0x244: start", 0x244, phy + 0x244, rd(phy + 0x244) | 3u);
			for (polls = 0; polls < 100; polls++) {
				status = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
				if (!(status & CORVETTE_PHY_STATUS_READY)) {
					break;
				}
				sleep_ms(1);
			}
			printf("wifi: s1:   with clkreq %s: phy %#x %s after %u ms; pin 95 in %u\n",
			    value == 0 ? "left alone" : (value == 1 ? "taken over low" : "taken over high"), status,
			    (status & CORVETTE_PHY_STATUS_READY) ? "NOT ready" : "READY", polls,
			    rd(tlmm + clkreq + 4) & 1);
		}
		kiwi_s1_write("parf clkreq: its input back", 0x2b0, parf + 0x2b0, 0);
		printf("wifi: s1:   5k/7: clkreq leaves the phy %#x %s, parf 0x2b0 %#x\n", status,
		    (status & CORVETTE_PHY_STATUS_READY) ? "NOT ready" : "READY", rd(parf + 0x2b0));
	}

	/* and last of all, the reference clock: the one thing with two sources and no evidence for either */
	kiwi_s1_refclk_search(parf);
}

/* 5b/7: the core's own configuration, in PARF (Linux's qcom_pcie_post_init_2_9_0). The ELBI is inside the
 * core at 0x60000000, and until PARF passes the core's AHB clock through (BYPASS together with AHB_CLK_EN) a
 * write to ELBI_SYS_CTRL is dropped and reads back 0 -- which is what S1's second run saw with the PHY ready,
 * both power domains up and the pins right. The registers are read before they are written, and the DBI's
 * base, the address space's size and the ECAM's base are read only: they say whether the bus will answer. */
static bool
kiwi_s1_core(vm_offset_t parf)
{
	uint32_t device, mhi, ctrl, flush;

	kiwi_s1_read("parf sys ctrl", CORVETTE_PARF_SYS_CTRL, parf + CORVETTE_PARF_SYS_CTRL);
	kiwi_s1_read("parf mhi clock/reset", CORVETTE_PARF_MHI_CLOCK_RESET, parf + CORVETTE_PARF_MHI_CLOCK_RESET);
	kiwi_s1_read("parf q2a flush", CORVETTE_PARF_Q2A_FLUSH_OFFSET, parf + CORVETTE_PARF_Q2A_FLUSH_OFFSET);
	kiwi_s1_read("parf dbi base", 0x168, parf + 0x168);
	kiwi_s1_read("parf slv space size", 0x16c, parf + 0x16c);
	kiwi_s1_read("parf ecam base", 0x380, parf + 0x380);
	device = kiwi_s1_read("parf device type", CORVETTE_PARF_DEVICE_TYPE_OFF, parf + CORVETTE_PARF_DEVICE_TYPE_OFF);

	printf("wifi: s1: 5b/7: the core's own configuration (a root complex is device type %#x)\n",
	    CORVETTE_PARF_DEVICE_TYPE_RC);
	if ((device & 0xf) != CORVETTE_PARF_DEVICE_TYPE_RC) {
		kiwi_s1_write("device type: RC", CORVETTE_PARF_DEVICE_TYPE_OFF, parf + CORVETTE_PARF_DEVICE_TYPE_OFF,
		    CORVETTE_PARF_DEVICE_TYPE_RC);
	} else {
		printf("wifi: s1:   it is a root complex already\n");
	}

	/*
	 * "Force PHY out of lowest power state" (Linux's qcom_pcie_init_2_7_0, the sequence this SoC's
	 * configuration uses): PARF_PHY_CTRL bit 0 holds the PHY down, and qcom_pcie_deinit_2_7_0 sets it
	 * again when the controller is torn down -- so it is a real gate, not a debugging knob. Nothing in S1
	 * has ever cleared it, and a PHY held down still answers its own status registers (its "ready" bit is
	 * the end of the sequence, not a clock) while handing the core no pipe clock at all, which is exactly
	 * a link whose LTSSM cannot leave detect.
	 */
	{
		uint32_t phy_control = rd(parf + CORVETTE_PARF_PHY_CTRL);

		if (phy_control & CORVETTE_PHY_TEST_PWR_DOWN) {
			kiwi_s1_write("parf phy ctrl: awake", CORVETTE_PARF_PHY_CTRL, parf + CORVETTE_PARF_PHY_CTRL,
			    phy_control & ~CORVETTE_PHY_TEST_PWR_DOWN);
		} else {
			printf("wifi: s1:   parf phy ctrl %#x: the PHY is not held down\n", phy_control);
		}
	}

	mhi = rd(parf + CORVETTE_PARF_MHI_CLOCK_RESET);
	if ((mhi & CORVETTE_PARF_MHI_ON) != CORVETTE_PARF_MHI_ON) {
		kiwi_s1_write("mhi clock/reset: on", CORVETTE_PARF_MHI_CLOCK_RESET,
		    parf + CORVETTE_PARF_MHI_CLOCK_RESET, mhi | CORVETTE_PARF_MHI_ON);
	} else {
		printf("wifi: s1:   mhi clock/reset %#x already passes them: bypass %u, ahb clk %u, mstr axi clk %u\n",
		    mhi, (mhi >> 4) & 1, mhi & 1, (mhi >> 1) & 1);
	}

	/*
	 * The wake-up and clock-gating bits, and one cleared: MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN, which upstream
	 * clears for the same reason it clears the bit above -- it is the mux through which something else can
	 * put the PHY's MAC side back to sleep.
	 */
	ctrl = rd(parf + CORVETTE_PARF_SYS_CTRL);
	if ((ctrl & CORVETTE_PARF_SYS_CTRL_ON) != CORVETTE_PARF_SYS_CTRL_ON || (ctrl & CORVETTE_PARF_MAC_PHY_PWR_MUX)) {
		kiwi_s1_write("sys ctrl: awake", CORVETTE_PARF_SYS_CTRL, parf + CORVETTE_PARF_SYS_CTRL,
		    (ctrl | CORVETTE_PARF_SYS_CTRL_ON) & ~CORVETTE_PARF_MAC_PHY_PWR_MUX);
	} else {
		printf("wifi: s1:   sys ctrl %#x already has them, and the phy's power-down mux is off\n", ctrl);
	}

	/*
	 * The rest of Linux's qcom_pcie_init_2_7_0, which S1 has never done either: L1 is allowed in (PM_CTRL's
	 * REQ_NOT_ENTR_L1 cleared), and the AXI master's writes are let through (AXI_MSTR_WR_ADDR_HALT_V2's EN,
	 * bit 31). Neither starts a link on its own; both are what the vendor's own bring-up writes.
	 */
	{
		uint32_t pm = rd(parf + CORVETTE_PARF_PM_CTRL);

		if (pm & CORVETTE_PARF_REQ_NOT_ENTR_L1) {
			kiwi_s1_write("pm ctrl: L1 allowed", CORVETTE_PARF_PM_CTRL, parf + CORVETTE_PARF_PM_CTRL,
			    pm & ~CORVETTE_PARF_REQ_NOT_ENTR_L1);
		} else {
			printf("wifi: s1:   pm ctrl %#x: L1 is allowed already\n", pm);
		}
	}
	{
		uint32_t halt = rd(parf + CORVETTE_PARF_AXI_HALT_V2);

		if (!(halt & CORVETTE_PARF_AXI_HALT_EN)) {
			kiwi_s1_write("axi writes: let through", CORVETTE_PARF_AXI_HALT_V2, parf + CORVETTE_PARF_AXI_HALT_V2,
			    halt | CORVETTE_PARF_AXI_HALT_EN);
		} else {
			printf("wifi: s1:   axi halt v2 %#x: writes are let through already\n", halt);
		}
	}

	flush = rd(parf + CORVETTE_PARF_Q2A_FLUSH_OFFSET);
	if (flush != 0) {
		kiwi_s1_write("q2a flush: cleared", CORVETTE_PARF_Q2A_FLUSH_OFFSET,
		    parf + CORVETTE_PARF_Q2A_FLUSH_OFFSET, 0);
	} else {
		printf("wifi: s1:   q2a flush is already clear\n");
	}

	device = rd(parf + CORVETTE_PARF_DEVICE_TYPE_OFF);
	mhi = rd(parf + CORVETTE_PARF_MHI_CLOCK_RESET);
	printf("wifi: s1:   5b/7: device type now %#x, mhi clock/reset now %#x (bypass %u)\n", device, mhi,
	    (mhi >> 4) & 1);
	return (device & 0xf) == CORVETTE_PARF_DEVICE_TYPE_RC;
}

/* 5c/7: the core's base addresses, and whether the core answers at all. The device tree's dm_core
 * (0x60000000) is where the core's own config space is; PARF_DBI_BASE_ADDR_V2 points the core at it, and
 * ATU_BASE_ADDR at the iATU (0x60001000). Read first: S1's third run found the older pair (0x168/0x16c) at
 * zero, and these two, if they are at zero as well, mean nothing has ever told the core where it lives --
 * which is why every read below 0x60000f20 came back 0 and the ELBI took no write. */
static void
kiwi_s1_dbi(vm_offset_t parf, vm_offset_t dm_core)
{
	uint32_t id;

	kiwi_s1_read("parf dbi base v2", CORVETTE_PARF_DBI_BASE_V2, parf + CORVETTE_PARF_DBI_BASE_V2);
	kiwi_s1_read("parf dbi base v2 hi", CORVETTE_PARF_DBI_BASE_V2_HI, parf + CORVETTE_PARF_DBI_BASE_V2_HI);
	kiwi_s1_read("parf atu base", CORVETTE_PARF_ATU_BASE, parf + CORVETTE_PARF_ATU_BASE);
	kiwi_s1_read("parf slv size v2", CORVETTE_PARF_SLV_SIZE_V2, parf + CORVETTE_PARF_SLV_SIZE_V2);
	kiwi_s1_read("parf slv size v2 hi", CORVETTE_PARF_SLV_SIZE_V2_HI, parf + CORVETTE_PARF_SLV_SIZE_V2_HI);
	id = kiwi_s1_read("core's config space", 0, dm_core);

	printf("wifi: s1: 5c/7: the core's base addresses (dm_core %#x, iatu %#x, from the device tree)\n",
	    CORVETTE_PCIE0_CORE, CORVETTE_PCIE0_CORE + 0x1000);
	/* The vendor's msm_pcie_enable_link() writes 0 to DBI_BASE_ADDR_V2 ("change DBI base
	 * address"); the bootloader's 0 is the offset the core already decodes at. Writing the
	 * mainline absolute 0x60000000 here makes the core stop decoding its own config space
	 * (reads 0xffffffff), so every port/link write never reached the core and the link stayed
	 * in detect.quiet. This was the bug behind the endless detect.quiet. */
	if (rd(parf + CORVETTE_PARF_DBI_BASE_V2) != 0) {
		kiwi_s1_write("dbi base v2: 0", CORVETTE_PARF_DBI_BASE_V2, parf + CORVETTE_PARF_DBI_BASE_V2, 0);
	}
	if (rd(parf + CORVETTE_PARF_DBI_BASE_V2_HI) != 0) {
		kiwi_s1_write("dbi base v2: hi 0", CORVETTE_PARF_DBI_BASE_V2_HI, parf + CORVETTE_PARF_DBI_BASE_V2_HI, 0);
	}
	/* The vendor never touches ATU_BASE; the bootloader leaves it at 0x1000 (an offset into
	 * the DBI window, not the mainline absolute address). Leave it as the bootloader had it. */
	if (rd(parf + CORVETTE_PARF_ATU_BASE) != 0x1000) {
		kiwi_s1_write("atu base: 0x1000", CORVETTE_PARF_ATU_BASE, parf + CORVETTE_PARF_ATU_BASE, 0x1000);
	}
	if (rd(parf + CORVETTE_PARF_ATU_BASE_HI) != 0) {
		kiwi_s1_write("atu base: hi 0", CORVETTE_PARF_ATU_BASE_HI, parf + CORVETTE_PARF_ATU_BASE_HI, 0);
	}
	/* The vendor writes the DT's slv-addr-space-size into the low word and leaves the high word 0.
	 * S1 used to write 0 into the low word and 2 GB into the high one, which is a 2^63 window that
	 * nothing decodes; with it wrong, ECAM reads would fail even after the link came up. */
	if (rd(parf + CORVETTE_PARF_SLV_SIZE_V2) != CORVETTE_PARF_SLV_SIZE) {
		kiwi_s1_write("slv size v2: 64 MB", CORVETTE_PARF_SLV_SIZE_V2, parf + CORVETTE_PARF_SLV_SIZE_V2,
		    CORVETTE_PARF_SLV_SIZE);
	}
	if (rd(parf + CORVETTE_PARF_SLV_SIZE_V2_HI) != 0) {
		kiwi_s1_write("slv size v2: hi 0", CORVETTE_PARF_SLV_SIZE_V2_HI, parf + CORVETTE_PARF_SLV_SIZE_V2_HI,
		    0);
	}

	id = rd(dm_core);
	printf("wifi: s1:   5c/7: the core's config space %#x now reads %#x (vendor %#x, device %#x), class/revision "
	    "%#x\n", CORVETTE_PCIE0_CORE, id, id & 0xffff, id >> 16, rd(dm_core + 8));
	printf("wifi: s1:   %s\n", (id == 0 || id == 0xffffffff) ?
	    "it still reads nothing: the core is not decoding its own config space" :
	    "the core answers");
}

/* 5d/7: the port itself, as Linux's dw_pcie_setup_rc() leaves it: bus numbers, the command register, the
 * class code (which read 0xff, i.e. unconfigured), and above all the number of lanes -- PORT_LINK_CONTROL
 * read 0, and a DWC port that is told it has no lanes keeps its LTSSM in detect for ever, which is exactly
 * what the last second of every run so far has shown. One lane is right here: the radio is an x1 device. */
static void
kiwi_s1_port(vm_offset_t dm_core)
{
	uint32_t command = kiwi_s1_read("dbi command", CORVETTE_DBI_COMMAND, dm_core + CORVETTE_DBI_COMMAND);
	uint32_t class = kiwi_s1_read("dbi class/revision", CORVETTE_DBI_CLASS, dm_core + CORVETTE_DBI_CLASS);
	uint32_t bus = kiwi_s1_read("dbi bus numbers", CORVETTE_DBI_PRIMARY_BUS, dm_core + CORVETTE_DBI_PRIMARY_BUS);
	uint32_t link = kiwi_s1_read("dbi port link control", CORVETTE_DBI_PORT_LINK_CONTROL,
	    dm_core + CORVETTE_DBI_PORT_LINK_CONTROL);
	uint32_t width = kiwi_s1_read("dbi link width/speed", CORVETTE_DBI_LINK_WIDTH_SPEED,
	    dm_core + CORVETTE_DBI_LINK_WIDTH_SPEED);
	uint32_t misc = kiwi_s1_read("dbi misc control 1", CORVETTE_DBI_MISC_CONTROL_1,
	    dm_core + CORVETTE_DBI_MISC_CONTROL_1);

	printf("wifi: s1: 5d/7: the port: %u lanes configured, class %#x, command %#x; the radio is an x1 device\n",
	    (link >> 16) & 0x3f, class >> 16, command);
	kiwi_s1_write("misc control 1: ro on", CORVETTE_DBI_MISC_CONTROL_1, dm_core + CORVETTE_DBI_MISC_CONTROL_1,
	    misc | 1);
	kiwi_s1_write("dbi bus numbers", CORVETTE_DBI_PRIMARY_BUS, dm_core + CORVETTE_DBI_PRIMARY_BUS,
	    (bus & 0xff000000) | CORVETTE_BUS_NUMBERS);
	kiwi_s1_write("dbi command", CORVETTE_DBI_COMMAND, dm_core + CORVETTE_DBI_COMMAND,
	    (command & 0xffff0000) | CORVETTE_COMMAND_BITS);
	kiwi_s1_write("dbi class: pci bridge", CORVETTE_DBI_CLASS, dm_core + CORVETTE_DBI_CLASS,
	    (class & 0xffff) | CORVETTE_CLASS_PCI_BRIDGE);
	/*
	 * Neither of these is written, and that is the vendor's own behaviour rather than an omission. pcie0's
	 * node has no qcom,target-link-width and no num-lanes, so msm_pcie_enable_link() never reaches
	 * msm_pcie_set_link_width() and msm_pcie_link_train() never touches 0x710 or the width field: what the
	 * bootloader left there -- x2 in the lane field, 0x130290 in the width/speed word -- is what Android
	 * trains with. S1 used to force both to one lane, which no vendor path ever does. The one write
	 * link_train() does make to this word is the field at bits 12:8, and 5h/7 does that.
	 */
	printf("wifi: s1:   port link control and width/speed left as the bootloader had them: lanes %u, width %#x\n",
	    (link >> 16) & 0x3f, width);
	kiwi_s1_write("misc control 1: ro off", CORVETTE_DBI_MISC_CONTROL_1, dm_core + CORVETTE_DBI_MISC_CONTROL_1, misc);

	printf("wifi: s1:   5d/7: port link control now %#x (%u lanes, dll link %u), link width/speed %#x (%u lanes)\n",
	    rd(dm_core + CORVETTE_DBI_PORT_LINK_CONTROL), (rd(dm_core + CORVETTE_DBI_PORT_LINK_CONTROL) >> 16) & 0x3f,
	    (rd(dm_core + CORVETTE_DBI_PORT_LINK_CONTROL) >> 5) & 1, rd(dm_core + CORVETTE_DBI_LINK_WIDTH_SPEED),
	    (rd(dm_core + CORVETTE_DBI_LINK_WIDTH_SPEED) >> 8) & 0xff);
}

/* Walks the capability list the core answers with and returns the PCIe capability's offset (0 if there is
 * none). The list starts at the DBI's 0x34; this is the same walk Linux's dw_pcie_find_capability() makes,
 * and it matters: the first entry on this core is the power management capability at 0x40, which has no link
 * registers at all, and S1's sixth run read the link's status out of it and called the link down. */
static uint32_t
kiwi_s1_capability(vm_offset_t dm_core)
{
	uint32_t at = rd(dm_core + 0x34) & 0xfc;

	for (unsigned i = 0; i < 8 && at >= 0x40 && at <= 0xfc; i++) {
		uint32_t word = rd(dm_core + at);

		printf("wifi: s1:   capability at %#x: id %#x, next %#x\n", at, word & 0xff, (word >> 8) & 0xfc);
		if ((word & 0xff) == 0x10) {
			return at;
		}
		at = (word >> 8) & 0xfc;
	}
	return 0;
}

/*
 * The link's state for a while: every change printed, the last state returned.
 *
 * The state is taken from PARF_LTSSM bits 5:0, NOT from the ELBI. On this generation (the 1.9.0 ops
 * that sm8550/sm8650 bind) the ELBI's status register is not wired to the state machine at all -- it
 * reads 0x1 here, so its "state" field is always 0 -- which is why every earlier run reported
 * detect.quiet whatever the link was doing. Linux reads PARF_LTSSM for this SoC for the same reason.
 * The ELBI is sampled once, so that the log shows what it is worth.
 */
static uint32_t
kiwi_s1_watch(vm_offset_t elbi, uint32_t ms)
{
	vm_offset_t parf = corvette_s1_io_map(CORVETTE_PCIE0_PARF, CORVETTE_PCIE0_PARF_SIZE);
	static bool told;
	uint32_t elbi_status = rd(elbi + CORVETTE_ELBI_SYS_STTS_OFFSET);
	uint32_t state = 0xffffffff, last = 0xffffffff, milliseconds;

	if (!told) {
		told = true;
		printf("wifi: s1:   elbi +0x08 reads %#x (its state field %#x): on this generation the state is "
		    "not there, it is in parf +0x1b0 bits 5:0, which is what is watched from here on\n",
		    elbi_status, (elbi_status >> 12) & 0x3f);
	}
	for (milliseconds = 0; milliseconds < ms; milliseconds++) {
		state = rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f;
		if (state != last) {
			printf("wifi: s1:   ltssm %#x %s, %u ms in\n", state,
			    kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)", milliseconds);
			last = state;
		}
		if (state == 0x11) {
			break;
		}
		sleep_ms(1);
	}
	return state;
}

/*
 * 5l/7: the radio's own supplies.
 *
 * The supplies come first, so that nothing behind the link can be blamed for a link that does not
 * train. (The "it keeps falling back to detect.quiet" that sent earlier runs down this road was a
 * misreading of the ELBI -- see kiwi_s1_watch -- so what the link actually does is not known yet.)
 *
 * This board feeds the radio from seven regulators of the PMICs, named by qcom,cnss-kiwi in its
 * vdd-wlan*-supply properties, and nothing outside the vendor's own WLAN driver ever switches them on.
 * Each is one command: a regulator's enable register sits four bytes into its RPMh address, so the
 * request is a single write of 1 -- the shape the touch panel's own supply takes, and the only shape
 * this controller was seen to accept (three commands at once took the device down once already).
 *
 * The names below are the resources of the command database; they come from the qcom,resource-name of
 * the regulator node each supply points at, not from the node's own name.
 */
#define CORVETTE_VRM_VOLTAGE    0               /* a regulator's registers, from its RPMh address */
#define CORVETTE_VRM_ENABLE     (1 * 4)
#define CORVETTE_VRM_MODE       (2 * 4)
#define CORVETTE_MSGID_WRITE_4  0x10008u        /* four bytes, no answer wanted, a write */
#define CORVETTE_MSGID_READ_4   0x00108u        /* ... and a read, whose answer is the whole point */
#define CORVETTE_MSGID_RESPONSE 0x100u

#define CORVETTE_PIN_BT_EN      17              /* dt: qcom,bt-en-gpio */
#define CORVETTE_PIN_SW_CTRL    18              /* dt: qcom,sw-ctrl-gpio: the radio drives it when it is up */

struct kiwi_rail {
	const char *supply;                             /* what qcom,cnss-kiwi calls it */
	const char *resource;                           /* its name in the command database */
	uint32_t microvolts;                            /* what the device tree asks for, for the log only */
	uint32_t mode;                                  /* the hpm mode its node initialises it to, or 0 */
};

/*
 * All seven are voted HPM -- pmic_mode 7 -- between the voltage and the enable. Only ldob15 and ldoc3
 * name a mode in this board's device tree (qcom,init-mode = <4>), but the working OnePlus 13 loader
 * (ios-dodge, op13_wifi_link_probe) votes mode 7 on every WLAN rail, LDOs and SMPS alike, and then
 * reads the endpoint's own config space back, so the whole rail set is voted here the same way. A rail
 * still sitting in the PMIC's own low-power mode is the one platform-level difference that air163 did
 * not rule out; the readback below says whether each vote took.
 */
static const struct kiwi_rail kiwi_rails[] = {
	{ "vdd-wlan-io", "ldob15", 1800000, 7 },
	{ "vdd-wlan-io12", "ldoc3", 1200000, 7 },
	{ "vdd-wlan", "smpi4", 940000, 7 },
	{ "vdd-wlan-aon", "smpc2", 976000, 7 },
	{ "vdd-wlan-dig", "smpc3", 915000, 7 },
	{ "vdd-wlan-rfa1", "smpc6", 1864000, 7 },
	{ "vdd-wlan-rfa2", "smpc1", 1316000, 7 },
};

/*
 * One command in one slot set, and whether the controller finished it. A write is fire and forget, the
 * way Linux sends them; a read asks for an answer and it lands in the command's own response word. The
 * done bit belongs to the whole controller -- one per slot set -- so it is read at the base of the slot
 * area, without the per-set stride.
 */
static bool
kiwi_s1_command(vm_offset_t base, uint32_t tcs, uint32_t address, uint32_t msgid, uint32_t data,
    bool wants_answer, uint32_t *answer)
{
	const struct kiwi_tcs *t = kiwi_s1_tcs_layout(base);
	vm_offset_t at = base + 0xd00 + 672 * tcs, irq = base + 0xd00;
	uint32_t irq_enable, polls;

	irq_enable = rd(irq);
	wr(irq + 0x08, 1u << tcs);
	wr(at + t->control, 0);
	wr(at + t->enable, 0);
	for (polls = 0; polls < 500; polls++) {          /* (the set is asked to be idle before it is written) */
		if (rd(at + t->status) != 0) {
			break;
		}
		sleep_us(100);
	}
	wr(at + t->msgid, msgid);
	wr(at + t->addr, address);
	wr(at + t->data, data);
	wr(at + t->wait, wants_answer ? 1 : 0);
	wr(irq, irq_enable | (1u << tcs));
	wr(at + t->enable, 1);
	sleep_us(10);
	wr(at + t->control, CORVETTE_TCS_ACTIVE | CORVETTE_TCS_TRIGGER);

	for (polls = 0; polls < 2000; polls++) {
		if (rd(irq + 0x04) & (1u << tcs)) {
			break;
		}
		sleep_us(50);
	}
	if (answer != NULL) {
		*answer = rd(at + t->resp);
	}
	wr(irq + 0x08, 1u << tcs);
	wr(at + t->control, 0);
	wr(at + t->enable, 0);
	wr(irq, irq_enable);
	return polls < 2000;
}

/*
 * The seven supplies, one at a time, each read first -- so that the log says what the boot loader left
 * and what the command changed -- and then the chip is given its own power-on edge: wlan-en was raised
 * in 4/7, before any of these rails existed, and a chip that never saw its enable rise after its
 * supplies came up has not been through a power-on reset.
 */
static void
kiwi_s1_radio(vm_offset_t parf, vm_offset_t tlmm)
{
	vm_offset_t base = corvette_s1_io_map(CORVETTE_RSC_PHYS, 0x10000);
	const unsigned count = sizeof(kiwi_rails) / sizeof(kiwi_rails[0]);
	unsigned found = 0, on = 0;
	uint32_t wlan_en = 0x1000UL * CORVETTE_PIN_WLAN_EN;
	uint32_t sw_ctrl = 0x1000UL * CORVETTE_PIN_SW_CTRL;
	uint32_t bt_en = 0x1000UL * CORVETTE_PIN_BT_EN;

	printf("wifi: s1: 5l/7: the radio's %u supplies, one command each (dt: qcom,cnss-kiwi)\n", count);
	printf("wifi: s1:   before: pin 16 wlan-en %#x, pin 18 sw-ctrl %#x, pin 17 bt-en %#x\n", rd(tlmm + wlan_en + 4),
	    rd(tlmm + sw_ctrl + 4), rd(tlmm + bt_en + 4));
	if (!cmd_db_there()) {
		printf("wifi: s1:   the list of RPMh resources is not there: the radio stays unpowered\n");
		return;
	}
	for (unsigned r = 0; r < count; r++) {
		const struct kiwi_rail *rail = &kiwi_rails[r];
		const struct cmd_db_entry *entry = rpmh_resource(rail->resource, 0);
		uint32_t address = entry == NULL ? 0 : entry->address;
		uint32_t wanted = rail->microvolts / 1000, millivolts = 0, enable = 0;

		/* (regulators carry 4 in bits 19:16 of their address; anything else is not one) */
		if (address == 0 || ((address >> 16) & 0xf) != 4) {
			printf("wifi: s1:   %s: no regulator called \"%s\" in the list (%#x): left alone\n", rail->supply,
			    rail->resource, address);
			continue;
		}
		found++;
		kiwi_s1_command(base, 0, address + CORVETTE_VRM_VOLTAGE, CORVETTE_MSGID_READ_4 | CORVETTE_MSGID_RESPONSE,
		    0, true, &millivolts);
		kiwi_s1_command(base, 0, address + CORVETTE_VRM_ENABLE, CORVETTE_MSGID_READ_4 | CORVETTE_MSGID_RESPONSE,
		    0, true, &enable);
		printf("wifi: s1:   %s (%s) at %#x: reads %u mV, enable %u (dt asks %u mV)\n", rail->supply,
		    rail->resource, address, millivolts, enable, wanted);
		/*
		 * The read back is not to be believed: these VRM resources answer with whatever happened to
		 * sit in the response word, so one rail read "enable 2147483649" and was skipped on that
		 * account while still being off. The voltage and the enable are therefore written
		 * unconditionally, one command at a time -- the controller takes one per request, and three
		 * at once once took the whole device down.
		 */
		if (kiwi_s1_command(base, 0, address + CORVETTE_VRM_VOLTAGE, CORVETTE_MSGID_WRITE_4, wanted, false,
		    NULL)) {
			printf("wifi: s1:   %s: voltage written %u mV\n", rail->supply, wanted);
		} else {
			printf("wifi: s1:   %s: the controller did not finish the voltage command\n", rail->supply);
		}
		sleep_ms(1);
		if (rail->mode != 0) {
			/* (mode first: the vendor reorders its requests for exactly this reason) */
			if (kiwi_s1_command(base, 0, address + CORVETTE_VRM_MODE, CORVETTE_MSGID_WRITE_4,
			    rail->mode, false, NULL)) {
				printf("wifi: s1:   %s: mode %u (hpm) written\n", rail->supply, rail->mode);
			} else {
				printf("wifi: s1:   %s: the controller did not finish the mode command\n",
				    rail->supply);
			}
			sleep_ms(1);
		}
		if (kiwi_s1_command(base, 0, address + CORVETTE_VRM_ENABLE, CORVETTE_MSGID_WRITE_4, 1, false, NULL)) {
			printf("wifi: s1:   %s: enabled\n", rail->supply);
			on++;
		} else {
			printf("wifi: s1:   %s: the controller did not finish the enable command\n", rail->supply);
		}
		sleep_ms(2);
	}
	/*
	 * What the rails answer once the commands have gone through. Bit 31 of a response word is the
	 * controller's own flag and not part of the value, so it is masked off here. A rail that still reads
	 * back disabled, or at another voltage, is the only evidence available that the commands reached
	 * the hardware at all -- and it is the one question the host side of the link cannot answer.
	 */
	for (unsigned r = 0; r < count; r++) {
		const struct kiwi_rail *rail = &kiwi_rails[r];
		const struct cmd_db_entry *entry = rpmh_resource(rail->resource, 0);
		uint32_t address = entry == NULL ? 0 : entry->address;
		uint32_t millivolts = 0, mode = 0, enable = 0;

		if (address == 0 || ((address >> 16) & 0xf) != 4) {
			continue;
		}
		kiwi_s1_command(base, 0, address + CORVETTE_VRM_VOLTAGE,
		    CORVETTE_MSGID_READ_4 | CORVETTE_MSGID_RESPONSE, 0, true, &millivolts);
		kiwi_s1_command(base, 0, address + CORVETTE_VRM_MODE,
		    CORVETTE_MSGID_READ_4 | CORVETTE_MSGID_RESPONSE, 0, true, &mode);
		kiwi_s1_command(base, 0, address + CORVETTE_VRM_ENABLE,
		    CORVETTE_MSGID_READ_4 | CORVETTE_MSGID_RESPONSE, 0, true, &enable);
		printf("wifi: s1:   %s reads back %u mV (%#x), mode %u, enable %u\n", rail->supply,
		    millivolts & 0x7fffffff, millivolts, mode & 0x7fffffff, enable & 0x7fffffff);
	}
	printf("wifi: s1:   5l/7: %u of %u supplies are on (%u of them were found in the list)\n", on, count, found);
	printf("wifi: s1:   after: pin 18 sw-ctrl %#x (the radio drives it when it is up)\n", rd(tlmm + sw_ctrl + 4));

	kiwi_s1_write("pin 16 wlan-en io: low", 0, tlmm + wlan_en + 4, 0);
	/* air198: hold the enable low 150 ms instead of 10, so the module goes through a real power-on
	 * edge whatever the bootloader left running. The vendor's cnss2 timing is not in the sources
	 * here; 150 ms is the low the working UniXNU board (WifiProbe.cpp) uses for exactly this reason. */
	sleep_ms(150);
	kiwi_s1_write("pin 16 wlan-en io: high", 0, tlmm + wlan_en + 4, 2);
	/* (time for the chip to see its supplies and its enable before its reset is let go) */
	/*
	 * Air191: one and a half seconds instead of a hundred milliseconds.
	 *
	 * The vendor's PCIe driver never powers the wifi rails: in pci-msm.c msm_pcie_vreg_init() only
	 * takes the supplies listed on the pcie node, while the seven vdd-wlan-* rails belong to the
	 * cnss-kiwi node and are handled by a different driver.  On the phone those rails are therefore
	 * already up long before the link is trained -- from the bootloader and from cnss2 -- and the
	 * module's own PMU has had seconds to boot.  Here they are switched on a few hundred milliseconds
	 * before perst is released, which is the one timing difference the register dumps cannot show.
	 */
	sleep_ms(1500);
	printf("wifi: s1:   pin 16 io now %#x; pin 18 sw-ctrl %#x\n", rd(tlmm + wlan_en + 4), rd(tlmm + sw_ctrl + 4));
	/*
	 * Air193: the air189 control-line probe is gone.  It answered its question (the radio follows
	 * wlan-en and drives clkreq), and lowering the radio's enable 50 ms before the PHY sequence is
	 * something the vendor driver never does.  From here the flow is vendor-only.
	 */
}

/*
 * 5h/7: the rest of what the vendor's msm_pcie_enable_link() and msm_pcie_link_train() write, and
 * nothing that S1 had touched so far.
 *
 * Reading drivers/pci/controller/pci-msm.c of the vendor's own kernel (the oneplus/sm8650_b_16.0.0_ace_3_pro
 * branch) side by side with S1's steps shows four writes that the vendor does and S1 never did:
 *
 *  - PARF 0x2c4 (CFG_BITS_3) bit 0. The vendor's comment says what it is: "Disable override for fal10_veto
 *    logic to de-assert Qactive signal". Qactive is the signal that tells the far end the link is wanted;
 *    while the veto logic holds it, the endpoint never leaves its own reset and never answers a receiver
 *    detection. Bit 8 of the same register goes with it, and the DWC's LANE_SKEW_OFF (0x714 bit 5) goes
 *    with that: they are the three that have to be in place before the link is trained.
 *  - the DWC's PCIE_GEN3_GEN2_CTRL (0x80c) bits 12 to 16, which the vendor sets right before it enables
 *    link training.
 *  - the DWC's AUX_CLK_FREQ (0xb40), which the vendor fills from the device tree's qcom,aux-clk-freq
 *    (0x14 here, so twenty megahertz) and which the L1SS timeout arithmetic is done in.
 *  - PARF 0x22c (INT_ALL_MASK), which the vendor clears so that the link's own events are not masked.
 */
#define CORVETTE_PARF_CFG_BITS_3        0x2c4           /* bit 0: the Qactive veto; bit 8: with it */
#define CORVETTE_PARF_INT_ALL_MASK      0x22c
#define CORVETTE_CORE_LANE_SKEW_OFF     0x714
#define CORVETTE_CORE_GEN3_GEN2_CTRL    0x80c
#define CORVETTE_CORE_DEVICE_CTRL_STATUS 0x78           /* bits 14:12 = max read request size */
#define CORVETTE_CORE_AUX_CLK_FREQ      0xb40
#define CORVETTE_AUX_CLK_FREQ_MHZ       20              /* dt: qcom,aux-clk-freq = <0x14> */

static void
kiwi_s1_vendor_extras(vm_offset_t parf, vm_offset_t core)
{
	uint32_t value;

	printf("wifi: s1: 5h/7: the vendor's own writes, none of which S1 had done\n");
	/*
	 * Air186: the three PARF writes of msm_pcie_enable_link() that S1 still had not made, and that no
	 * earlier log had even read. They come before everything else the vendor does, and two of them are
	 * read-modify-writes whose result is only correct if the bits around them are kept.
	 *
	 *  0x40  bit 0 cleared -- the vendor's comment is "enable PCIe clocks and resets";
	 *  0x174 bit 4 set     -- msm_pcie_write_mask(parf + MHI_CLOCK_RESET_CTRL, 0, BIT(4)), a set, not a
	 *                         clear: the mask argument is 0 and the value is BIT(4);
	 *  0x1a8 bit 31 set    -- PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT, BIT(31) | the boot value. (0x4d0 is a
	 *                         different register entirely: PCIE20_PARF_L1SS_SLEEP_MODE_HANDLER_STATUS.)
	 */
	value = rd(parf + 0x40);
	printf("wifi: s1:   parf phy ctrl        +0x40: %#x (bit 0 %u)\n", value, value & 1);
	kiwi_s1_write("parf phy ctrl: bit 0 off", 0x40, parf + 0x40, value & ~1u);
	value = rd(parf + 0x174);
	printf("wifi: s1:   mhi clock reset ctrl +0x174: %#x (bit 4 %u)\n", value, (value >> 4) & 1);
	kiwi_s1_write("mhi clock reset ctrl: bit 4 on", 0x174, parf + 0x174, value | (1u << 4));
	value = rd(parf + 0x1a8);
	printf("wifi: s1:   axi write halt       +0x1a8: %#x\n", value);
	kiwi_s1_write("axi write halt: bit 31 on", 0x1a8, parf + 0x1a8, value | (1u << 31));
	value = rd(parf + CORVETTE_PARF_CFG_BITS_3);
	printf("wifi: s1:   parf cfg bits 3      +0x2c4: %#x (qactive veto %u, bit 8 %u)\n", value, value & 1,
	    (value >> 8) & 1);
	kiwi_s1_write("parf cfg bits 3: the qactive veto off", CORVETTE_PARF_CFG_BITS_3,
	    parf + CORVETTE_PARF_CFG_BITS_3, value & ~1u);
	kiwi_s1_write("parf cfg bits 3: bit 8", CORVETTE_PARF_CFG_BITS_3,
	    parf + CORVETTE_PARF_CFG_BITS_3, rd(parf + CORVETTE_PARF_CFG_BITS_3) | (1u << 8));

	value = rd(core + CORVETTE_CORE_LANE_SKEW_OFF);
	printf("wifi: s1:   lane skew off        +0x714: %#x (bit 5 %u)\n", value, (value >> 5) & 1);
	kiwi_s1_write("lane skew off: on", CORVETTE_CORE_LANE_SKEW_OFF, core + CORVETTE_CORE_LANE_SKEW_OFF,
	    value | (1u << 5));

	/*
	 * link_train()'s first write is write_reg_field(dm_core, 0x80c, 0x1f00, 1): the field at bits 12:8 is
	 * cleared and then set to 1. S1 ORed 0x1f00 in instead, which leaves that field at 0x1f -- a value no
	 * vendor path ever produces on this part. With the bootloader's 0x130290 this lands on 0x130190, which
	 * is exactly what the vendor's own read-modify-write produces.
	 */
	value = rd(core + CORVETTE_CORE_GEN3_GEN2_CTRL);
	printf("wifi: s1:   gen3/gen2 control    +0x80c: %#x (bits 12:16 %u)\n", value, (value >> 12) & 0x1f);
	kiwi_s1_write("gen3/gen2 control: field 0x1f00 to 1", CORVETTE_CORE_GEN3_GEN2_CTRL,
	    core + CORVETTE_CORE_GEN3_GEN2_CTRL, (value & ~0x1f00u) | 0x100u);

	/* the max read request size, 2 KB, which the vendor's link_train writes after the gen3/gen2 word */
	value = rd(core + CORVETTE_DBI_MISC_CONTROL_1);
	kiwi_s1_write("misc control 1: ro on", CORVETTE_DBI_MISC_CONTROL_1, core + CORVETTE_DBI_MISC_CONTROL_1,
	    value | 1);
	value = rd(core + CORVETTE_CORE_DEVICE_CTRL_STATUS);
	/*
	 * link_train() writes dev->tlp_rd_size into this field, and pcie0's node has no qcom,tlp-rd-size, so
	 * the vendor writes zero -- 128-byte reads, the reset value. S1 wrote 5, which is not even a legal
	 * Max_Read_Request_Size encoding (1 is 256 B, 2, 3, 4 is 2 KB; 5 is reserved), so this now writes zero
	 * as the vendor does.
	 */
	printf("wifi: s1:   device ctrl/status    +0x78: %#x (max read req size %u)\n", value, (value >> 12) & 7);
	kiwi_s1_write("device ctrl/status: max read request size 0", CORVETTE_CORE_DEVICE_CTRL_STATUS,
	    core + CORVETTE_CORE_DEVICE_CTRL_STATUS, value & ~0x7000u);

	printf("wifi: s1:   aux clock frequency  +0xb40: %#x\n", rd(core + CORVETTE_CORE_AUX_CLK_FREQ));
	kiwi_s1_write("aux clock frequency", CORVETTE_CORE_AUX_CLK_FREQ, core + CORVETTE_CORE_AUX_CLK_FREQ,
	    CORVETTE_AUX_CLK_FREQ_MHZ);

	printf("wifi: s1:   interrupt mask       +0x22c: %#x\n", rd(parf + CORVETTE_PARF_INT_ALL_MASK));
	kiwi_s1_write("interrupt mask: nothing masked", CORVETTE_PARF_INT_ALL_MASK,
	    parf + CORVETTE_PARF_INT_ALL_MASK, 0);
	/*
	 * Air191: the mask word a stock RC0 reads at L0 is 0x7f80d202 -- msm_pcie_enable_link() clears
	 * the register and then sets only the events it serves.  Writing the observed word back makes
	 * this port's interrupt state identical to the phone's.  It has no effect on training; it is
	 * included because it was the only PARF difference left in the air188 diff.
	 */
	kiwi_s1_write("interrupt mask: the vendor's own bits", CORVETTE_PARF_INT_ALL_MASK,
	    parf + CORVETTE_PARF_INT_ALL_MASK, 0x7f80d202u);
}

/*
 * The vendor's own failure dump. msm_pcie.c prints three register lists taken from the device tree when the
 * PHY or the link will not come up -- qcom,parf-debug-reg, qcom,dbi-debug-reg and qcom,phy-debug-reg, read by
 * pcie_parf_dump(), pcie_dbi_dump() and pcie_phy_dump(). Printing the same lists here means this snapshot can
 * be compared register for register against a vendor dump taken from Android on the same part, which is the
 * only oracle left: every write this step makes is already the vendor's write.
 *
 * The order is the device tree's, with one change: the PHY's SerDes and PLL windows (0xed0, 0x16d0, 0x11e0,
 * 0x19e0) are read last. Those are the only reads here that can fault on a PHY that is not powered, and by the
 * time they are reached everything worth having has already been printed.
 */
static void
kiwi_s1_vendor_dump(vm_offset_t parf, vm_offset_t core)
{
	static const uint32_t parf_regs[] = {
		0x1b0, 0x24, 0x28, 0x224, 0x500, 0x4d0, 0x4d4, 0x3c0, 0x630, 0x230, 0x00,
		/* Air188: the rest of the range a stock RC0 keeps at L0, so the two dumps can be diffed
		 * offset by offset.  PARF is always on, so these reads are the same ones pcie_parf_dump()
		 * makes and cannot reach a powered-down page. */
		0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x2c, 0x30, 0x34, 0x38,
		0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c,
		0x1a4, 0x1a8, 0x1ac, 0x1b4, 0x1b8, 0x1bc, 0x1c0,
		0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220,
		0x228, 0x22c, 0x234, 0x238, 0x23c,
		0x2b0, 0x2b4, 0x2b8, 0x2bc, 0x2c0, 0x2c4, 0x2c8, 0x2cc,
		0x2d0, 0x2d4, 0x2d8, 0x2dc, 0x2e0, 0x2e4, 0x2e8, 0x2ec
	};
	static const uint32_t dbi_regs[] = {
		0x104, 0x110, 0x80, 0x1f4, 0x730, 0x734, 0x738, 0x73c
	};
	static const uint32_t phy_regs[] = {
		0x214, 0x218, 0x21c, 0x220, 0x224, 0x228, 0x22c, 0x230, 0x234, 0x238, 0x23c,
		0x1cc, 0x1d0, 0x1d4, 0x1d8, 0x1dc, 0x1e0, 0x1e4, 0x1f8,
		0x600, 0x604,
		0xa00, 0x1200, 0xa04, 0x1204, 0xa08, 0x1208, 0xa0c, 0x120c,
		0xa10, 0x1210, 0xa14, 0x1214, 0xa18, 0x1218, 0xc20, 0x1420,
		0xed0, 0x16d0, 0xedc, 0x16dc, 0x11e0, 0x19e0
	};
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;

	printf("wifi: s1:   dump: the vendor's parf / dbi / phy debug lists, as pcie_parf_dump() reads them\n");
	for (uint32_t i = 0; i < sizeof(parf_regs) / sizeof(parf_regs[0]); i++) {
		printf("wifi: s1:     parf +%#05x %#x\n", parf_regs[i], rd(parf + parf_regs[i]));
	}
	for (uint32_t i = 0; i < sizeof(dbi_regs) / sizeof(dbi_regs[0]); i++) {
		printf("wifi: s1:     dbi  +%#05x %#x\n", dbi_regs[i], rd(core + dbi_regs[i]));
	}
	for (uint32_t i = 0; i < sizeof(phy_regs) / sizeof(phy_regs[0]); i++) {
		printf("wifi: s1:     phy  +%#05x %#x\n", phy_regs[i], rd(phy + phy_regs[i]));
	}
}

/* 6/7: the reference clock, the capability the link reports itself through, then training. */
/*
 * The PMIC, through the v7 observer.  See the top of this block for why the
 * version 5 reader above cannot be used here.  Reads only: an APID's write
 * channel is at chnls + 0x1000 * apid, and spmi-pmic-arb.c only uses one when
 * the APID's owner (chnls + 0x104 + 0x1000 * apid) is this EE -- that table is
 * in the region this environment cannot access, and a write there took a
 * synchronous external abort in the loader.
 */
#define CORVETTE_ARB_CORE_PHYS  0x0c400000
#define CORVETTE_ARB_CORE_SIZE  0x3000
#define CORVETTE_ARB_OBS_PHYS   0x0c440000
#define CORVETTE_ARB_OBS_SIZE   0x80000

static int
corvette_pmic_read(vm_offset_t obs, uint32_t apid, uint8_t reg, uint8_t *out)
{
	vm_offset_t channel = obs + 0x20 * (vm_offset_t)apid;   /* (ee 0: the dt's qcom,ee) */
	uint32_t status = 0;
	unsigned spin;

	wr(channel + 0x00, (1u << 27) | ((uint32_t)reg << 4));
	for (spin = 0; spin < 5000; spin++) {
		status = rd(channel + 0x08);
		if (status & 0x1u) {
			break;
		}
	}
	if (!(status & 0x1u) || (status & 0xeu)) {
		return -1;
	}
	*out = (uint8_t)rd(channel + 0x18);
	return 0;
}

/*
 * Every regulator the arbiter's table names, printed as: ppid, kind, setpoint,
 * enable, and whether the PMIC itself says it is in regulation.  The radio's
 * rails are the SMPS of PM8550VS_C (s1c/s2c/s3c/s6c) and PM8550VS_I (s4i) plus
 * PM8550's LDO15 (vdd-wlan-io) and the VS's LDO3 (vdd-wlan-io12).
 */
static void
kiwi_s1_pmic(void)
{
	vm_offset_t core = corvette_s1_io_map(CORVETTE_ARB_CORE_PHYS, CORVETTE_ARB_CORE_SIZE);
	vm_offset_t obs = corvette_s1_io_map(CORVETTE_ARB_OBS_PHYS, CORVETTE_ARB_OBS_SIZE);
	uint32_t apids, n, answered = 0, regulators = 0, enabled = 0;

	if (core == 0 || obs == 0) {
		printf("wifi: s1: 6a/7: the arbiter's windows did not map; the supplies are not read\n");
		return;
	}
	apids = rd(core + 0x04) & 0x7ff;
	printf("wifi: s1: 6a/7: the pmic through the v7 observer (core %#x, obs %#x, %u apids)\n",
	    CORVETTE_ARB_CORE_PHYS, CORVETTE_ARB_OBS_PHYS, apids);
	if (apids == 0 || apids > 1024) {
		printf("wifi: s1: 6a/7: the arbiter reports no usable apid table\n");
		return;
	}
	for (n = 0; n < apids; n++) {
		uint32_t map = rd(core + 0x2000 + 4 * n), ppid, mv;
		uint8_t type = 0, status = 0, enable = 0, lo = 0, hi = 0;

		if (map == 0) {
			continue;
		}
		ppid = (map >> 8) & 0xfff;
		if (corvette_pmic_read(obs, n, 0x04, &type) != 0) {
			continue;
		}
		answered++;
		if (type != 0x03 && type != 0x04) {
			continue;
		}
		regulators++;
		(void)corvette_pmic_read(obs, n, 0x08, &status);
		(void)corvette_pmic_read(obs, n, 0x46, &enable);
		(void)corvette_pmic_read(obs, n, 0x40, &lo);
		(void)corvette_pmic_read(obs, n, 0x41, &hi);
		mv = ((uint32_t)hi << 8) | lo;
		if (enable & 0x80) {
			enabled++;
		}
		printf("wifi: s1: 6a/7:   %#05x %s %2u: %u mV, enable %u, %s\n", ppid,
		    type == 0x03 ? "smps" : "ldo",
		    type == 0x03 ? ((ppid & 0xff) - 0x9b) / 3 + 1 : (ppid & 0xff) - 0x3f,
		    mv, (enable >> 7) & 1, (status & 0x80) ? "in regulation" : "not in regulation");
	}
	/* air198: the sleep clock pin. The OEM DT (volcano-pmic-overlay.dtsi) puts alt_sleep_clk on
	 * PMK8550 gpio5, func1, output-enable; the earlier probe read page 0xbb, which is another gpio
	 * by every mapping in play. The peripheral base is not pinned down in the sources here, so all
	 * four candidate pages are read: whichever is a GPIO (type 0x10) and what its mode/function say
	 * is the sleep clock's actual state. Reads only -- the write channel aborts in this environment. */
	{
		static const uint32_t pages[] = { 0xba, 0xbb, 0xbc, 0xbd };
		for (unsigned k = 0; k < 4; k++) {
			int gapid = -1;
			uint8_t gtype = 0xee, gsub = 0xee, mode2 = 0xee, mode1 = 0xee, pull = 0xee, func = 0xee, ena = 0xee;
			for (uint32_t g = 0; g < apids; g++) {
				uint32_t gmap = rd(core + 0x2000 + 4 * g);
				if (gmap && ((gmap >> 8) & 0xfff) == pages[k]) {
					gapid = (int)g;
					break;
				}
			}
			if (gapid < 0) {
				printf("wifi: s1: 6a/7:   gpio page %#05x: not in the arbiter's table\n", pages[k]);
				continue;
			}
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x04, &gtype);
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x05, &gsub);
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x40, &mode2);
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x41, &mode1);
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x42, &pull);
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x44, &func);
			(void)corvette_pmic_read(obs, (uint32_t)gapid, 0x46, &ena);
			printf("wifi: s1: 6a/7:   gpio page %#05x (apid %d): type %#x sub %#x, mode2 %#x mode1 %#x, "
			    "pull %#x, func %#x, enable %#x\n", pages[k], gapid, gtype, gsub, mode2, mode1, pull,
			    func, ena);
		}
	}
	printf("wifi: s1: 6a/7: %u of %u answered, %u regulators, %u enabled\n", answered, apids,
	    regulators, enabled);
}

	/*
	 * air203: the mainline bring-up for this SoC and radio, as shennong_wlan.c (the Xiaomi 14 Pro
	 * port -- the same SM8650 and WCN7850, booted the same way, Wi-Fi working) runs it, adapted to
	 * this file's names. The vendor-specific layer is absent on purpose: no INT_ALL_MASK, no
	 * PARF_SYS_CTRL 0x365E, no CESTA/CRM/pcie_sm/sm-seq, no 0x2b0 experiments. SLV 2 GiB, SYS_CTRL's
	 * bit 29 only, PM_CTRL touched, the two RCGs programmed explicitly (0x001 / 0x605 -- the latter
	 * is what the boot loader had left, minus its hardware-control bit), the pipe mux parked on the
	 * crystal through the PHY's table writes and back on the PHY before the last two writes, PERST#
	 * released at least 100 ms after the radio got power, and the link accepted by DBI debug1's
	 * bit 4. Selected with pcie_mainline=1.
	 */
static bool
kiwi_s1_mainline_port(vm_offset_t parf, vm_offset_t core, vm_offset_t tlmm, vm_offset_t gcc)
{
	uint32_t state, polls;
	vm_offset_t tcsr = corvette_s1_io_map(CORVETTE_TCSR_PHYS, CORVETTE_TCSR_SIZE);
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;

	printf("wifi: s1: 6m/7: the mainline bring-up, as shennong_wlan.c (the same SM8650 and WCN7850) runs it\n");

	/* air205: the radio's own reference clock. shennong_wlan.c votes RPMH_RF_CLK1 ("clka1",
	 * clk-rpmh.c sm8650) with VRM_ENABLE = 1 -- "the radio's 38.4 MHz" -- as the last of its
	 * supplies, before any PCIe bring-up. The module asserts CLKREQ# asking for exactly this
	 * clock (air200: pad low; air202: the gate never opened); without the vote its radio side
	 * never runs and its PCIe PHY never presents a receiver. Our seven rails never included it:
	 * it is a clock resource, not a regulator. */
	{
		vm_offset_t base = corvette_s1_io_map(CORVETTE_RSC_PHYS, 0x10000);
		const struct cmd_db_entry *entry = rpmh_resource("clka1", 0);
		uint32_t address = entry ? entry->address : 0;
		printf("wifi: s1: 6m/7: clka1 (RPMH_RF_CLK1, the radio's 38.4 MHz) at %#x\n", address);
		if (address == 0) {
			printf("wifi: s1: 6m/7: clka1 is not in the command database: the clock is not voted\n");
		} else if (kiwi_s1_command(base, 0, address + CORVETTE_VRM_ENABLE,
		    CORVETTE_MSGID_WRITE_4, 1, false, NULL)) {
			printf("wifi: s1: 6m/7: clka1 voted on\n");
		} else {
			printf("wifi: s1: 6m/7: clka1: RPMh did not take the vote\n");
		}
	}

	/* shennong's step 3, after the two domains (2/7 has powered them): the mux parks on the
	 * crystal, the two rcgs take their settings, the reference gate opens, then both blocks reset. */
	kiwi_s1_write("6m pipe mux: the crystal", 0x6b070, gcc + 0x6b070,
	    (rd(gcc + 0x6b070) & ~3u) | 2u);
	kiwi_s1_write("6m aux rcg cfg", 0x6b078, gcc + 0x6b078,
	    (rd(gcc + 0x6b078) & ~0x371fu) | 0x001u);
	kiwi_s1_write("6m rchng rcg cfg", 0x6b05c, gcc + 0x6b05c,
	    (rd(gcc + 0x6b05c) & ~0x371fu) | 0x605u);
	kiwi_s1_write("6m aux rcg update", 0x6b074, gcc + 0x6b074,
	    rd(gcc + 0x6b074) | 1u);
	kiwi_s1_write("6m rchng rcg update", 0x6b058, gcc + 0x6b058,
	    rd(gcc + 0x6b058) | 1u);
	for (polls = 0; polls < 2000 && (rd(gcc + 0x6b074) & 1u); polls++) {
		sleep_us(10);
	}
	for (polls = 0; polls < 2000 && (rd(gcc + 0x6b058) & 1u); polls++) {
		sleep_us(10);
	}
	kiwi_s1_write("6m tcsr clkref: open", 0xb1100, tcsr + 0xb1100, rd(tcsr + 0xb1100) | 1u);
	sleep_us(100);
	kiwi_s1_write("6m bcr core: pulse", CORVETTE_PCIE0_BCR, gcc + CORVETTE_PCIE0_BCR, 1u);
	sleep_us(1000);
	kiwi_s1_write("6m bcr core: out", CORVETTE_PCIE0_BCR, gcc + CORVETTE_PCIE0_BCR, 0u);
	sleep_us(1000);
	kiwi_s1_write("6m bcr phy: pulse", CORVETTE_PCIE0_PHY_BCR, gcc + CORVETTE_PCIE0_PHY_BCR, 1u);
	sleep_us(1000);
	kiwi_s1_write("6m bcr phy: out", CORVETTE_PCIE0_PHY_BCR, gcc + CORVETTE_PCIE0_PHY_BCR, 0u);
	sleep_us(1000);

	/* shennong's step 4, first half: the controller to root complex with the mainline register
	 * set. PERST# is already held low, and CLKREQ#/WAKE# carry their pcie0 functions (4/7). */
	kiwi_s1_write("6m device type: RC", 0x1000, parf + 0x1000, 4u);
	kiwi_s1_write("6m phy ctrl: bit 0 off", 0x40, parf + 0x40, rd(parf + 0x40) & ~1u);
	kiwi_s1_write("6m dbi base", 0x350, parf + 0x350, CORVETTE_PCIE0_CORE);
	kiwi_s1_write("6m dbi base hi", 0x354, parf + 0x354, 0);
	kiwi_s1_write("6m atu base", 0x634, parf + 0x634, CORVETTE_PCIE0_CORE + 0x1000);
	kiwi_s1_write("6m atu base hi", 0x638, parf + 0x638, 0);
	kiwi_s1_write("6m slv size: 2 GiB", 0x358, parf + 0x358, 0);
	kiwi_s1_write("6m slv size hi", 0x35c, parf + 0x35c, 0x80000000u);
	kiwi_s1_write("6m sys ctrl: bit 29 off", 0x00, parf + 0x00, rd(parf + 0x00) & ~(1u << 29));
	kiwi_s1_write("6m mhi clock/reset: bit 4", 0x174, parf + 0x174, rd(parf + 0x174) | (1u << 4));
	kiwi_s1_write("6m pm ctrl: bit 5 off", 0x20, parf + 0x20, rd(parf + 0x20) & ~(1u << 5));
	kiwi_s1_write("6m axi halt: bit 31", 0x1a8, parf + 0x1a8, rd(parf + 0x1a8) | (1u << 31));

	/* the phy's sequence, every write but the last two; then the pipe clock back to the phy, and
	 * only then the last two (out of reset, started). The pipe branch was voted in 1/7. */
	printf("wifi: s1: 6m/7: the phy's %u writes without its last two\n", KIWI_PHY_SEQUENCE_WRITES - 2);
	for (uint32_t i = 0; i + 2 < KIWI_PHY_SEQUENCE_WRITES; i++) {
		wr(parf + CORVETTE_PCIE0_PHY_AT + kiwi_phy_sequence[3 * i],
		    kiwi_phy_sequence[3 * i + 1]);
	}
	kiwi_s1_write("6m pipe mux: the phy's", 0x6b070, gcc + 0x6b070,
	    rd(gcc + 0x6b070) & ~3u);
	for (uint32_t i = KIWI_PHY_SEQUENCE_WRITES - 2; i < KIWI_PHY_SEQUENCE_WRITES; i++) {
		wr(parf + CORVETTE_PCIE0_PHY_AT + kiwi_phy_sequence[3 * i],
		    kiwi_phy_sequence[3 * i + 1]);
	}
	for (polls = 0; polls < 2000 && (rd(phy + CORVETTE_PHY_STATUS_OFFSET) & (1u << 6)); polls++) {
		sleep_us(10);
	}
	state = rd(phy + CORVETTE_PHY_STATUS_OFFSET);
	printf("wifi: s1: 6m/7: the phy %s after %u x 10 us (0x214 %#x); pipe cbcr %#x; serdes locks "
	    "0xa00 %#x, 0x1200 %#x\n", (state & (1u << 6)) ? "NOT ready" : "ready", polls, state,
	    rd(gcc + 0x6b048), rd(phy + 0xa00), rd(phy + 0x1200));
	if (state & (1u << 6)) {
		return false;
	}

	/* the root port: buses 0/1/1, memory and bus mastering */
	kiwi_s1_write("6m buses 0/1/1", 0x18, core + 0x18, 0x00010100u);
	kiwi_s1_write("6m command: mem + master", 0x04, core + 0x04, rd(core + 0x04) | 0x6u);

	/* PERST# release: the radio has had power for seconds (4/7 raised wlan-en), so the CEM's
	 * 100 ms are long past. */
	state = rd(tlmm + 0x1000UL * CORVETTE_PIN_PERST + 4);
	kiwi_s1_write("6m perst: released", 0, tlmm + 0x1000UL * CORVETTE_PIN_PERST + 4, state | 2u);
	sleep_us(1000);
	kiwi_s1_write("6m ltssm: enable", CORVETTE_PARF_LTSSM_OFFSET, parf + CORVETTE_PARF_LTSSM_OFFSET,
	    rd(parf + CORVETTE_PARF_LTSSM_OFFSET) | (1u << 8));
	for (polls = 0; polls < 100; polls++) {
		sleep_ms(50);
		if ((rd(core + 0x72c) & ((1u << 4) | (1u << 29))) == (1u << 4)) {
			break;
		}
		if (polls % 10 == 9) {
			printf("wifi: s1: 6m/7:   +%u ms: debug0 %#x, debug1 %#x, serdes 0xa00 %#x, "
			    "0x1200 %#x, pipe cbcr %#x\n", 50 * (polls + 1), rd(core + 0x728),
			    rd(core + 0x72c), rd(phy + 0xa00), rd(phy + 0x1200), rd(gcc + 0x6b048));
		}
	}
	printf("wifi: s1: 6m/7: the link %s after %u x 50 ms (debug0 %#x, debug1 %#x)\n",
	    polls < 100 ? "up" : "NOT up after five seconds", polls, rd(core + 0x728),
	    rd(core + 0x72c));
	if (polls >= 100) {
		printf("wifi: s1: 6m/7: at fail: serdes locks 0xa00 %#x, 0x1200 %#x; pipe cbcr %#x\n",
		    rd(phy + 0xa00), rd(phy + 0x1200), rd(gcc + 0x6b048));
		/* air204: the air188 control -- the mux to the crystal for a moment, to tell a state
		 * machine frozen for the lack of the phy's pipe clock from one held for another reason;
		 * then back to the phy's clock. */
		kiwi_s1_write("6m air204 mux: the crystal", 0x6b070, gcc + 0x6b070,
		    (rd(gcc + 0x6b070) & ~3u) | 2u);
		sleep_ms(200);
		printf("wifi: s1: 6m/7:   mux=2 control: ltssm %#x, serdes 0xa00 %#x, 0x1200 %#x\n",
		    rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f, rd(phy + 0xa00), rd(phy + 0x1200));
		kiwi_s1_write("6m air204 mux: the phy's", 0x6b070, gcc + 0x6b070,
		    rd(gcc + 0x6b070) & ~3u);
		return false;
	}
	sleep_ms(100);

	/* air207: shennong's last line of step 4 -- the outbound iATU region 0 turns the conf
	 * window into type-0 config requests for bus 1 device 0. Without it the conf window answers
	 * 0xff (air206's reads sat at L0 and still saw nothing). */
	{
		vm_offset_t r = core + 0x1000;
		wr(r + 0x08, (uint32_t)(CORVETTE_PCIE0_ECAM & 0xffffffffu));
		wr(r + 0x0c, 0);
		wr(r + 0x10, (uint32_t)(CORVETTE_PCIE0_ECAM + 0x100000 - 1));
		wr(r + 0x14, 1u << 24);
		wr(r + 0x18, 0);
		wr(r + 0x00, 0x4);
		wr(r + 0x04, 0x80000000u);
		for (polls = 0; polls < 1000 && !(rd(r + 0x04) & 0x80000000u); polls++) {
			sleep_us(10);
		}
		printf("wifi: s1: 6m/7: the iATU's config region %s\n",
		    (rd(r + 0x04) & 0x80000000u) ? "enabled" : "stayed off");
		/* shennong's step 5, first read: the radio's config space through the translated window. */
		{
			vm_offset_t conf = corvette_s1_io_map(CORVETTE_PCIE0_ECAM, 0x1000);
			uint32_t id = rd(conf), cap;
			printf("wifi: s1: 6m/7: the radio: %04x:%04x, revision %#x, class %#08x, bar0 %#x %#x\n",
			    id & 0xffff, id >> 16, rd(conf + 8) & 0xff, rd(conf + 8) >> 8,
			    rd(conf + 0x10), rd(conf + 0x14));
			printf("wifi: s1: 6m/7: %s\n", id == 0x110717cb ? "THE RADIO ANSWERS" :
			    (id == 0xffffffff || id == 0) ? "nobody answers at bus 1" : "this is not a WCN7850");
			if (id == 0x110717cb) {
				for (cap = rd(conf + 0x34) & 0xfc; cap != 0 && cap < 0x100;
				     cap = rd(conf + cap) >> 8 & 0xfc) {
					uint32_t head = rd(conf + cap);
					if ((head & 0xff) == 0x10) {   /* PCI Express */
						uint32_t ls = rd(conf + cap + 0x10) >> 16;
						printf("wifi: s1: 6m/7: the radio's link status %#x: up %u, speed %u, "
						    "width %u\n", ls, (ls >> 13) & 1, ls & 0xf, (ls >> 4) & 0x3f);
						break;
					}
				}
			}
		}
	}
	return true;
}

/*
 * The vendor's training path, ported whole: msm_pcie_enable_link() (pci-msm.c:6056-6211),
 * msm_pcie_link_train() (5719-5815), and the two helpers they call, msm_pcie_config_controller_phy()
 * (4840-4867) and msm_pcie_config_core_preset() (4781-4837). Lines are that file's.
 *
 * The power, clock, GDSC, reset and CETSA steps before it are the ones 1/7 to 5f/7 already make; this
 * function starts where msm_pcie_enable_link() starts, at PARF_DEVICE_TYPE, and ends where its
 * check_ep_access does -- at the endpoint's config space, which kiwi_s1()'s 7/7 (or s2) step reads.
 *
 * What it adds over the existing 6/7, and why it is worth one run: the vendor writes PARF_SYS_CTRL as an
 * absolute 0x365E (6/7 ORs bits into whatever was there), writes the Secondary PCIe capability's preset
 * registers, writes the vendor's DBI device word and AER bits, and polls with 5 ms x 20 instead of S1's
 * one-millisecond sampling of PARF_LTSSM. Selected with pcie_vendor_port=1; without it nothing below is
 * reached and 6/7 runs as before.
 */
static uint16_t
kiwi_vendor_ext_capability(vm_offset_t core, uint16_t want)
{
	uint32_t pos = 0x100, header = rd(core + pos);

	if (header == 0) {
		return 0;
	}
	for (unsigned ttl = (0x1000 - 0x100) / 8; ttl > 0; ttl--) {
		if ((header >> 16) == want) {
			return (uint16_t)pos;
		}
		pos = (header >> 20) & 0xffc;
		if (pos < 0x100) {
			break;
		}
		header = rd(core + pos);
	}
	return 0;
}

/* 4840-4867: for every supported speed from 8.0 GT/s upwards, the Gen3/Gen4 rate shadow and the
 * equalisation settings. Both device-tree properties it reads are absent from this node, so their
 * values are the vendor's zero-initialised defaults. */
static void
kiwi_vendor_controller_phy(vm_offset_t core)
{
	uint32_t lnkcap = rd(core + 0x70), sls = lnkcap & 0xf, value;

	printf("wifi: vendor:   controller/phy settings (lnkcap %#x: max speed %u, max width %u)\n", lnkcap, sls,
	    (lnkcap >> 4) & 0x3f);
	for (uint32_t speed = 3; speed <= sls; speed++) {
		value = rd(core + 0x890);
		kiwi_s1_write("related: rate shadow select", 0x890, core + 0x890,
		    (value & ~0x3000000u) | ((speed - 3) << 24));
		value = rd(core + 0x8a8);
		kiwi_s1_write("eq control: pset request vector", 0x8a8, core + 0x8a8, value & ~0xffff00u);
		value = rd(core + 0x890);
		kiwi_s1_write("related: zrxdc noncompl clear", 0x890, core + 0x890, value & ~1u);
		value = rd(core + 0x8ac);
		kiwi_s1_write("eq fb: t min phase23", 0x8ac, core + 0x8ac, value & ~0x1fu);
	}
}

/* 4781-4837: the preset hints, written into the Secondary PCIe extended capability (0xC in) for Gen3 and
 * the 16.0 GT/s one (0x20 in) for Gen4, one dword per two (or four) lanes. This is the write no earlier
 * round made; the value is qcom,core-preset, absent here, so the vendor's default 0x55555555. */
static void
kiwi_vendor_core_preset(vm_offset_t core)
{
	uint32_t lnkcap = rd(core + 0x70), sls = lnkcap & 0xf, mlw = (lnkcap >> 4) & 0x3f, value;
	uint16_t cap;
	uint32_t lanes, per;

	value = rd(core + 0x8bc);
	kiwi_s1_write("misc control: ro writes on", 0x8bc, core + 0x8bc, value | 1u);
	if (sls >= 3) {
		cap = kiwi_vendor_ext_capability(core, 0x19);   /* (PCI_EXT_CAP_ID_SECPCI) */
		if (cap == 0) {
			printf("wifi: vendor:   no Secondary PCIe extended capability: no Gen3 preset written\n");
		} else {
			for (lanes = 0, per = 2; lanes < (mlw + per - 1) / per; lanes++) {
				kiwi_s1_write("secondary pcie preset", 0xc + 4 * lanes,
				    core + cap + 0xc + 4 * lanes, 0x55555555u);
			}
		}
	}
	if (sls >= 4) {
		cap = kiwi_vendor_ext_capability(core, 0x26);   /* (PCI_EXT_CAP_ID_PL_16GT) */
		if (cap == 0) {
			printf("wifi: vendor:   no Physical Layer 16.0 GT/s capability: no Gen4 preset written\n");
		} else {
			for (lanes = 0, per = 4; lanes < (mlw + per - 1) / per; lanes++) {
				kiwi_s1_write("pl 16gt preset", 0x20 + 4 * lanes,
				    core + cap + 0x20 + 4 * lanes, 0x55555555u);
			}
		}
	}
	value = rd(core + 0x8bc);
	kiwi_s1_write("misc control: ro writes off", 0x8bc, core + 0x8bc, value & ~1u);
}

static bool
kiwi_s1_vendor_port(vm_offset_t parf, vm_offset_t core, vm_offset_t tlmm, vm_offset_t gcc)
{
	vm_offset_t elbi = core + (CORVETTE_PCIE0_ELBI - CORVETTE_PCIE0_CORE);
	uint32_t value, lnkcap, status = 0, state = 0, polls, link_status = 0;
	bool up = false, vendor_up = false;

	/* air201: msm_pcie_enable() calls msm_pcie_parf_cesta_config() between the core/PHY reset and
	 * msm_pcie_enable_link(): with the pcie_sm present (stock's own bootargs enable it via
	 * pci-msm-drv.pcie_sm_regs), it sets PARF_CESTA_CLKREQ_SEL (BIT(0)) and
	 * PARF_CESTA_L1SUB_TIMEOUT_EXT_INT_EN (BIT(3)) at parf + qcom,pcie-clkreq-offset (0x2c48,
	 * pineapple-pcie.dtsi L429) -- the vendor's comment says "Set clkreq to be accessed by CESTA".
	 * Without that bit the power sequencer never sees the module's CLKREQ request: air200 read
	 * clkreq-req 0 and the gate closed at both probes while the pad sat low, and the link held at
	 * detect.active in every run before this. */
	value = rd(parf + 0x2c48);
	printf("wifi: vendor:   air201: parf cesta config 0x2c48 before %#x\n", value);
	kiwi_s1_write("cesta clkreq sel", 0x2c48, parf + 0x2c48, value | 0x9u);
	printf("wifi: vendor: the vendor's own training path, msm_pcie_enable_link() + "
	    "msm_pcie_link_train() whole\n");
	/* air198: the aux and rchng RCGs, read whole. air188 printed only the CMD words (0 = idle);
	 * the CFG at cmd_rcgr+4 has never been read. The OEM end state (gcc-pineapple.c, ace3pro branch)
	 * is CFG = 0 for both: aux has only bi_tcxo at 19.2 MHz, and stock's connection-state debugfs
	 * says phy_rchng parent = bi_tcxo, rate 19200000. A non-zero CFG here means the bootloader left
	 * the sources somewhere else, and no branch vote can make a wrong source tick. */
	/* air200: the pcie_sm power sequencer's own view, read-only. Stock's bootargs enable the
	 * same sequencer (pci-msm-drv.pcie_sm_regs=...); its program counter says whether the thirteen
	 * loaded steps have run to the end, and its CLKREQ gate words say whether the module's request
	 * for the reference clock was ever served. A detect.active that never falls back to quiet looks
	 * like a link held for a clock, so the gate status is the first suspect. */
	{
		vm_offset_t sm = corvette_s1_io_map(CORVETTE_PCIE0_SM, CORVETTE_PCIE0_SM_SIZE);
		static const uint32_t sm_regs[14] = { 0x1040, 0x1048, 0x1050, 0x1058, 0x1060, 0x1068,
			0x1070, 0x1078, 0x1080, 0x1088, 0x1090, 0x1094, 0x1098, 0x109c };
		static const char *const sm_names[14] = {
			"ovr-pwr-val", "ovr-pwr-mask", "ovr-wait-val", "ovr-wait-mask",
			"ovr-br-val", "ovr-br-mask", "seq-pwr-sta", "seq-wait-sta",
			"seq-br-sta", "seq-pc", "seq-start", "clkreq-gate", "clkreq-ungate", "clkreq-req"
		};
		for (unsigned k = 0; k < 14; k++) {
			printf("wifi: vendor:   air200 probe: pcie_sm +0x%04x %-13s %#x\n", sm_regs[k],
			    sm_names[k], rd(sm + sm_regs[k]));
		}
		printf("wifi: vendor:   air200 probe: pads perst 94 in %u, clkreq 95 in %u, wake 96 in %u\n",
		    rd(tlmm + 94 * 0x1000 + 4) & 1u, rd(tlmm + 95 * 0x1000 + 4) & 1u,
		    rd(tlmm + 96 * 0x1000 + 4) & 1u);
		printf("wifi: vendor:   air200 probe: pcs 0x210 %#x 0x214 %#x 0x218 %#x 0x21c %#x, "
		    "pwr-dn 0x240 %#x, start 0x244 %#x\n", rd(parf + CORVETTE_PCIE0_PHY_AT + 0x210),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x214), rd(parf + CORVETTE_PCIE0_PHY_AT + 0x218),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x21c), rd(parf + CORVETTE_PCIE0_PHY_AT + 0x240),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x244));
		printf("wifi: vendor:   air200 probe: serdes 0x008 %#x 0x00c %#x 0x0d0 %#x 0x0d8 %#x "
		    "0x14c %#x 0x150 %#x, gdsc core %#x phy %#x\n", rd(parf + CORVETTE_PCIE0_PHY_AT + 0x008),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x00c), rd(parf + CORVETTE_PCIE0_PHY_AT + 0x0d0),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x0d8), rd(parf + CORVETTE_PCIE0_PHY_AT + 0x14c),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x150), rd(gcc + CORVETTE_PCIE0_GDSCR),
		    rd(gcc + CORVETTE_PCIE0_PHY_GDSCR));
	}
	printf("wifi: vendor:   air198 probe: aux rcg cmd +0x6b074 %#x cfg +0x6b078 %#x, rchng cmd +0x6b058 %#x "
	    "cfg +0x6b05c %#x\n", rd(gcc + 0x6b074), rd(gcc + 0x6b078), rd(gcc + 0x6b058), rd(gcc + 0x6b05c));
	printf("wifi: vendor:   air198 probe: branch halts aux +0x6b03c %#x, pipe +0x6b048 %#x, rchng +0x6b054 %#x "
	    "(bit 31 set = halted)\n", rd(gcc + 0x6b03c), rd(gcc + 0x6b048), rd(gcc + 0x6b054));
	/* air199: the bootloader leaves the rchng RCG on a source that is not in the OEM parent map at
	 * all (air198 read CFG 0x100605: src 5, div 3, bit 16 set; the OEM parent_map_0 of this clock
	 * is {BI_TCXO:0, GPLL0_MAIN:1, GPLL0_EVEN:6}). Stock's connection-state debugfs says phy_rchng
	 * parent = bi_tcxo, rate 19200000 -- the CCF's safe config of an enable_safe_config rcg whose
	 * freq_tbl starts at F(19200000, P_BI_TCXO, 1). This kernel has no CCF, so the leftover stood
	 * through every earlier run. One register, one variable: park it on the OEM end state and let
	 * the update land before the PHY is started. The aux RCG already reads CFG 0 and is untouched. */
	printf("wifi: vendor:   air199: rchng cfg before %#x\n", rd(gcc + 0x6b05c));
	kiwi_s1_write("rchng cfg: bi_tcxo", 0x6b05c, gcc + 0x6b05c, 0);
	kiwi_s1_write("rchng update", 0x6b058, gcc + 0x6b058, rd(gcc + 0x6b058) | 1u);
	for (polls = 0; polls < 100 && (rd(gcc + 0x6b058) & 1u); polls++) {
		sleep_us(10);
	}
	printf("wifi: vendor:   rchng update landed after %u polls: cmd %#x, cfg now %#x\n", polls,
	    rd(gcc + 0x6b058), rd(gcc + 0x6b05c));

	/* 6063: configure PCIe to RC mode */
	kiwi_s1_write("device type: RC", CORVETTE_PARF_DEVICE_TYPE_OFF, parf + CORVETTE_PARF_DEVICE_TYPE_OFF,
	    CORVETTE_PARF_DEVICE_TYPE_RC);
	/* 6066-6067 are skipped on purpose: dev->l1_supported is false unless debugfs sets it, so the vendor
	 * does not touch PM_CTRL in this path either. */
	/* 6070: enable PCIe clocks and resets */
	value = rd(parf + CORVETTE_PARF_PHY_CTRL);
	kiwi_s1_write("parf phy ctrl: bit 0 off", CORVETTE_PARF_PHY_CTRL, parf + CORVETTE_PARF_PHY_CTRL,
	    value & ~1u);
	/* 6073: change DBI base address */
	kiwi_s1_write("dbi base: 0", CORVETTE_PARF_DBI_BASE_V2, parf + CORVETTE_PARF_DBI_BASE_V2, 0);
	/* 6075: PARF_SYS_CTRL = 0x365E, an absolute value, not an OR of bits into the boot value */
	kiwi_s1_write("parf sys ctrl: 0x365e", CORVETTE_PARF_SYS_CTRL, parf + CORVETTE_PARF_SYS_CTRL,
	    CORVETTE_PARF_SYS_CTRL_ON);
	/* 6077: MHI clock/reset, bit 4 set (write_mask(..., 0, BIT(4))) */
	value = rd(parf + CORVETTE_PARF_MHI_CLOCK_RESET);
	kiwi_s1_write("mhi clock/reset: bit 4", CORVETTE_PARF_MHI_CLOCK_RESET,
	    parf + CORVETTE_PARF_MHI_CLOCK_RESET, value | (1u << 4));
	/* 6081-6096: the mask is cleared, then the events the vendor serves are set. The bits are the
	 * file's enum msm_pcie_irq_event: LINK_DOWN 1, L1SUB_TIMEOUT 9, BRIDGE_FLUSH_N 12, AER_LEGACY 14,
	 * AER_ERR 15, MSI_0..MSI_7 23..30, i.e. 0x7f80d202 - the value stock prints in dmesg and the 6/7
	 * branch above already writes. air195 read the bits as 8/11/12-13/21-28 and wrote 0x1fe03902. */
	kiwi_s1_write("interrupt mask: cleared", CORVETTE_PARF_INT_ALL_MASK, parf + CORVETTE_PARF_INT_ALL_MASK, 0);
	kiwi_s1_write("interrupt mask: the vendor's events", CORVETTE_PARF_INT_ALL_MASK,
	    parf + CORVETTE_PARF_INT_ALL_MASK, 0x7f80d202u);
	/* 6102: the slave address space's size (qcom,slv-addr-space-size) */
	kiwi_s1_write("slv size: 64 MB", CORVETTE_PARF_SLV_SIZE_V2, parf + CORVETTE_PARF_SLV_SIZE_V2,
	    CORVETTE_PARF_SLV_SIZE);
	/* 6115-6120: no qcom,wr-halt-size on this node, so the vendor keeps the register and sets bit 31 */
	value = rd(parf + CORVETTE_PARF_AXI_HALT_V2);
	kiwi_s1_write("axi write halt: bit 31", CORVETTE_PARF_AXI_HALT_V2, parf + CORVETTE_PARF_AXI_HALT_V2,
	    value | (1u << 31));
	/* 6128-6130 pcie_tcsr_init() has no qcom,tcsr-config here; 5/7's clkref write already made it. */
	/* 6133 + 4697-4709: pcie_phy_init()'s sequence */
	printf("wifi: vendor:   pcie_phy_init(): %u sequence writes\n", KIWI_PHY_SEQUENCE_WRITES);
	for (unsigned i = 0; i < KIWI_PHY_SEQUENCE_WRITES; i++) {
		uint32_t offset = kiwi_phy_sequence[3 * i], written = kiwi_phy_sequence[3 * i + 1];

		kiwi_s1_write("phy sequence", offset, parf + CORVETTE_PCIE0_PHY_AT + offset, written);
		if (kiwi_phy_sequence[3 * i + 2]) {
			sleep_ms(kiwi_phy_sequence[3 * i + 2]);
		}
	}
	/* 4711-4712: PHY_STABILIZATION_DELAY 995-1005 us */
	sleep_us(1000);
	/* 4715 and 4721: the pipe clock is one of the seven branches voted in 1/7, and 3/7 cycled both pipe
	 * resets; there is no clock framework in the kernel to repeat either call. */
	/* 4727-4743: wait for the PHY's ready bit, ten tries of a millisecond */
	for (polls = 0; polls < 10; polls++) {
		status = rd(parf + CORVETTE_PCIE0_PHY_AT + CORVETTE_PHY_STATUS_OFFSET);
		if (!(status & CORVETTE_PHY_STATUS_READY)) {
			break;
		}
		sleep_us(1000);
	}
	printf("wifi: vendor:   phy +%#x = %#x: %s after %u tries\n", CORVETTE_PHY_STATUS_OFFSET, status,
	    (status & CORVETTE_PHY_STATUS_READY) ? "NOT ready" : "ready", polls);
	if (status & CORVETTE_PHY_STATUS_READY) {
		printf("wifi: vendor:   the vendor returns -ENODEV here; the link is not trained\n");
		return false;
	}
	/* 6138-6139: phy_aux_clk_mux has no qcom,phy-aux-clk-mux on this node */
	/* 6141: usleep_range(ep_latency * 1000) with qcom,ep-latency = 10 */
	sleep_ms(10);
	/* 6143-6145: no qcom,ep-gpio on this node */
	/* 6147-6158: the maximum width, and no qcom,target-link-width, so no link-width write */
	lnkcap = rd(core + 0x70);
	printf("wifi: vendor:   lnkcap %#x: max speed %u, max width %u; no qcom,target-link-width\n", lnkcap,
	    lnkcap & 0xf, (lnkcap >> 4) & 0x3f);
	/* 6161: disable the fal10 veto's override, so Qactive is de-asserted */
	value = rd(parf + CORVETTE_PARF_CFG_BITS_3);
	kiwi_s1_write("parf cfg bits 3: qactive veto off", CORVETTE_PARF_CFG_BITS_3,
	    parf + CORVETTE_PARF_CFG_BITS_3, value & ~1u);
	/* 6167: and bit 8 of the same register */
	kiwi_s1_write("parf cfg bits 3: bit 8", CORVETTE_PARF_CFG_BITS_3, parf + CORVETTE_PARF_CFG_BITS_3,
	    rd(parf + CORVETTE_PARF_CFG_BITS_3) | (1u << 8));
	/* 6168: the DWC's LANE_SKEW_OFF bit 5 */
	value = rd(core + CORVETTE_CORE_LANE_SKEW_OFF);
	kiwi_s1_write("lane skew off: bit 5", CORVETTE_CORE_LANE_SKEW_OFF, core + CORVETTE_CORE_LANE_SKEW_OFF,
	    value | (1u << 5));
	/* air196: the capability on both sides of the endpoint's reset. The vendor reads it only after the
	 * release; in air195 that read said max speed 0, while every earlier run, which read it before the
	 * release, said 0x7a4c23 (max speed 3). */
	lnkcap = rd(core + 0x70);
	printf("wifi: vendor:   lnkcap before the endpoint's reset: %#x (max speed %u, max width %u)\n",
	    lnkcap, lnkcap & 0xf, (lnkcap >> 4) & 0x3f);
	/* 6170-6176: release the endpoint's reset, then PERST_PROPAGATION_DELAY 1000-1005 us */
	printf("wifi: vendor:   releasing the endpoint's reset (pin 94)\n");
	kiwi_s1_write("pin 94 perst: high", 0, tlmm + 0x1000UL * CORVETTE_PIN_PERST + 4, 2);
	sleep_us(1000);

	lnkcap = rd(core + 0x70);
	printf("wifi: vendor:   lnkcap after the endpoint's reset: %#x (max speed %u, max width %u)\n",
	    lnkcap, lnkcap & 0xf, (lnkcap >> 4) & 0x3f);
	/* 5719: msm_pcie_link_train() */
	printf("wifi: vendor: msm_pcie_link_train()\n");
	/* 5724-5725: PCIE_GEN3_GEN2_CTRL's field at bits 12:8, cleared and then set to 1 */
	value = rd(core + CORVETTE_CORE_GEN3_GEN2_CTRL);
	kiwi_s1_write("gen3/gen2 control: field 0x1f00 to 1", CORVETTE_CORE_GEN3_GEN2_CTRL,
	    core + CORVETTE_CORE_GEN3_GEN2_CTRL, (value & ~0x1f00u) | 0x100u);
	kiwi_vendor_controller_phy(core);
	kiwi_vendor_core_preset(core);
	/* 5733-5736: target_link_speed is qcom,target-link-speed, absent, so LNKCTL2 is not written */
	printf("wifi: vendor:   qcom,target-link-speed is absent, so LNKCTL2 is not written\n");
	/* 5739-5740: max TLP read size, PCIE_TLP_RD_SIZE's default 5 */
	value = rd(core + CORVETTE_CORE_DEVICE_CTRL_STATUS);
	kiwi_s1_write("device control: max read request 5", CORVETTE_CORE_DEVICE_CTRL_STATUS,
	    core + CORVETTE_CORE_DEVICE_CTRL_STATUS, (value & ~0x7000u) | (5u << 12));
	/* air196: the vendor gets the pipe clock from the clock framework's pcie_pipe_clk_init(); there is
	 * no clock framework here. 6b/7 found this mux at 0 (PHY PIPE, unclocked on this board) and moved it
	 * to 2 (BI_TCXO), the only action that ever took LTSSM out of detect.quiet. Read it, and write only
	 * when it is not already 2, so the log says which of the two it found. */
	value = rd(gcc + 0x6b070) & 3u;
	printf("wifi: vendor:   pipe mux before LTSSM: %#x (%s)\n", value,
	    value == 2 ? "BI_TCXO" : value == 0 ? "PHY PIPE" : "other");
	if (value != 2) {
		kiwi_s1_write("pipe mux: BI_TCXO", 0x6b070, gcc + 0x6b070, (rd(gcc + 0x6b070) & ~3u) | 2u);
	}
	/* 5743: one read-modify-write, then poll; the enable is never disturbed again */
	kiwi_s1_write("parf ltssm: enable", CORVETTE_PARF_LTSSM_OFFSET, parf + CORVETTE_PARF_LTSSM_OFFSET,
	    rd(parf + CORVETTE_PARF_LTSSM_OFFSET) | CORVETTE_PARF_LTSSM_EN);
	/* 5748-5754: the vendor's poll, LINK_UP_TIMEOUT 5000-5101 us and at most 20 checks, on ELBI's link-up
	 * bit (0x400) together with the DLL's active bit. */
	for (polls = 0; polls <= 20; polls++) {
		sleep_us(5000);
		value = rd(elbi + CORVETTE_ELBI_SYS_STTS_OFFSET);
		link_status = rd(core + 0x70 + 0x10);
		state = rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f;
		printf("wifi: vendor:   poll %u: elbi +0x08 %#x (link up %u), link status %#x (dll %u), parf "
		    "state %#x %s, mux %#x\n", polls, value, (value >> 10) & 1, link_status >> 16,
		    (link_status >> 29) & 1, state,
		    kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)",
		    rd(gcc + 0x6b070) & 3u);
		if ((value & 0x400) && (link_status & (1u << 29))) {
			vendor_up = true;
			break;
		}
	}
	if (vendor_up) {
		printf("wifi: vendor:   the vendor's own criterion is met: ELBI link-up and DLL active\n");
		up = true;
	} else if (state == 0x11 || (link_status & (1u << 13))) {
		printf("wifi: vendor:   the vendor's criterion is not met, but parf state %#x / link status %#x say "
		    "the link is up: ELBI's link-up bit does not follow this wrapper, as 6/7's note says\n", state,
		    link_status >> 16);
		up = true;
	} else {
		printf("wifi: vendor:   the link is not up: parf state %#x %s, link status %#x (dll %u), elbi %#x\n",
		    state, kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)", link_status >> 16,
		    (link_status >> 29) & 1, value);
	}
	/* air196: the vendor allows 100 ms. If the mux was the missing piece LTSSM should move within tens
	 * of milliseconds; this window records a further second and accepts the same two signs of a link the
	 * branch above accepts, so a link that arrives late still lets 7/7 report the endpoint. */
	if (!up) {
		printf("wifi: vendor:   the vendor's 100 ms are over; 40 more samples at 25 ms, recording only\n");
		for (unsigned extra = 0; extra < 40; extra++) {
			sleep_us(25000);
			state = rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f;
			link_status = rd(core + 0x70 + 0x10);
			printf("wifi: vendor:   +%u ms: parf state %#x %s, link status %#x (dll %u), mux %#x\n",
			    25 * (extra + 1), state,
			    kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)",
			    link_status >> 16, (link_status >> 29) & 1, rd(gcc + 0x6b070) & 3u);
			if (state == 0x11 || (link_status & (1u << 29)) || (link_status & (1u << 13))) {
				printf("wifi: vendor:   +%u ms: the link is up by the same signs\n", 25 * (extra + 1));
				up = true;
				break;
			}
		}
	}
	/* air200: the same sequencer and PCS words at failure time, to be compared with the entry
	 * probe: a program counter that moved says the sequencer ran; one that did not, or a CLKREQ
	 * gate still closed, says what held the link. */
	{
		vm_offset_t sm = corvette_s1_io_map(CORVETTE_PCIE0_SM, CORVETTE_PCIE0_SM_SIZE);
		printf("wifi: vendor:   air200 fail: seq-pc %#x, pwr-ctrl-sta %#x, wait-ev-sta %#x, "
		    "br-ev-sta %#x, clkreq gate %u ungate %u req %#x\n", rd(sm + 0x1088), rd(sm + 0x1070),
		    rd(sm + 0x1078), rd(sm + 0x1080), rd(sm + 0x1094) & 1u, rd(sm + 0x1098) & 1u,
		    rd(sm + 0x109c));
		printf("wifi: vendor:   air200 fail: pcs 0x214 %#x, pads perst %u clkreq %u wake %u, "
		    "gdsc core %#x phy %#x\n", rd(parf + CORVETTE_PCIE0_PHY_AT + 0x214),
		    rd(tlmm + 94 * 0x1000 + 4) & 1u, rd(tlmm + 95 * 0x1000 + 4) & 1u,
		    rd(tlmm + 96 * 0x1000 + 4) & 1u, rd(gcc + CORVETTE_PCIE0_GDSCR),
		    rd(gcc + CORVETTE_PCIE0_PHY_GDSCR));
		printf("wifi: vendor:   air201 fail: cesta cfg 0x2c48 %#x, parf clkreq 0x2b0 %#x\n",
		    rd(parf + 0x2c48), rd(parf + 0x2b0));
	}
	/* air198: the DesignWare port debug words, an independent read of the LTSSM the parf state
	 * already reports. The working UniXNU board accepts a link only when debug0's bits 5:0 are 0x11
	 * and debug1's bit 4 is set. */
	printf("wifi: vendor:   air198 probe: port debug0 %#x (ltssm %u), debug1 %#x (bit 4 %u)\n",
	    rd(core + 0x728), rd(core + 0x728) & 0x3f, rd(core + 0x72c), (rd(core + 0x72c) >> 4) & 1);
	status = link_status >> 16;
	printf("wifi: vendor:   link status %#x: up %u, speed %u, width %u\n", status, (status >> 13) & 1,
	    status & 0xf, (status >> 4) & 0x3f);

	/* air202: the sequencer parked at step 8 never served the module's CLKREQ request (air200:
	 * gate 0, req 0, pad low at both probes; air201's CLKREQ_SEL was already set by the boot
	 * loader, and its BIT(3) did not stick). Do the machine's step by hand: issue SEQ_CLKREQ_UNGATE
	 * (0x1098) and watch the link for half a second. If the reference clock reaching the module
	 * wakes its PHY, detect completes and 7/7 reads the endpoint; if nothing moves, the module is
	 * deaf outside this machine. */
	{
		vm_offset_t sm = corvette_s1_io_map(CORVETTE_PCIE0_SM, CORVETTE_PCIE0_SM_SIZE);
		printf("wifi: vendor:   air202: before ungate: gate %#x ungate %#x req %#x\n",
		    rd(sm + 0x1094), rd(sm + 0x1098), rd(sm + 0x109c));
		kiwi_s1_write("air202 clkreq ungate", 0x1098, sm + 0x1098, 1u);
		sleep_us(100);
		printf("wifi: vendor:   air202: after ungate: gate %#x ungate %#x req %#x\n",
		    rd(sm + 0x1094), rd(sm + 0x1098), rd(sm + 0x109c));
		for (unsigned extra = 0; extra < 20; extra++) {
			sleep_us(25000);
			state = rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f;
			link_status = rd(core + 0x70 + 0x10);
			printf("wifi: vendor:   air202 +%u ms: parf state %#x %s, link status %#x, 0x2b0 %#x\n",
			    25 * (extra + 1), state,
			    kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)",
			    link_status >> 16, rd(parf + 0x2b0));
			if (state == 0x11 || (link_status & (1u << 13))) {
				printf("wifi: vendor:   air202 +%u ms: the link woke\n", 25 * (extra + 1));
				up = true;
				break;
			}
		}
		printf("wifi: vendor:   air202: cesta cfg 0x2c48 %#x, parf 0x2b0 %#x after the watch\n",
		    rd(parf + 0x2c48), rd(parf + 0x2b0));
	}

	/* 6206 msm_pcie_config_sid() fills the BDF-to-SID tables for the SMMU, which is S2's subject; the
	 * iATU's CFG0 region is 7/7's or s2's. The rest of msm_pcie_config_controller() is these four. */
	value = rd(core + 0x70c);
	kiwi_s1_write("ack f aspm: n fts bit", 0x70c, core + 0x70c, value | (1u << 15));
	kiwi_s1_write("aux clock frequency: 20 MHz", CORVETTE_CORE_AUX_CLK_FREQ, core + CORVETTE_CORE_AUX_CLK_FREQ,
	    CORVETTE_AUX_CLK_FREQ_MHZ);
	/* 4904: cpl_timeout has no device-tree property and bridge_found is not known before enumeration */
	value = rd(core + 0x3c);
	kiwi_s1_write("bridge control: aer", 0x3c, core + 0x3c, value | (3u << 16));
	value = rd(core + CORVETTE_CORE_DEVICE_CTRL_STATUS);
	kiwi_s1_write("device control: aer", CORVETTE_CORE_DEVICE_CTRL_STATUS, core + CORVETTE_CORE_DEVICE_CTRL_STATUS,
	    value | 0xfu);
	/* 6209 msm_pcie_check_ep_access() is the read of the endpoint's config space that kiwi_s1() makes
	 * after this returns; it is not repeated here (a read of ECAM on an untrained link ends the kernel). */
	return up;
}

static bool
kiwi_s1_link(vm_offset_t parf, vm_offset_t core, vm_offset_t tlmm)
{
	vm_offset_t elbi = core + (CORVETTE_PCIE0_ELBI - CORVETTE_PCIE0_CORE);
	uint32_t refclk = kiwi_s1_read("parf refclk", CORVETTE_PARF_PHY_REFCLK, parf + CORVETTE_PARF_PHY_REFCLK);
	uint32_t ltssm = kiwi_s1_read("parf ltssm", CORVETTE_PARF_LTSSM_OFFSET, parf + CORVETTE_PARF_LTSSM_OFFSET);
	uint32_t capability, status = 0, state, value;

	printf("wifi: s1: 6/7: refclk %#x: use pad %u, spread spectrum %u; ltssm %#x (state %#x)\n", refclk,
	    (refclk >> 12) & 1, (refclk >> 16) & 1, ltssm, ltssm & 0x3f);

	capability = kiwi_s1_capability(core);
	if (capability != 0) {
		status = rd(core + capability + 0x10) >> 16;
		printf("wifi: s1:   link capability %#x (max speed %u, max width %u), link status %#x (up %u, speed %u, "
		    "width %u) before training\n", rd(core + capability + 0x0c), rd(core + capability + 0x0c) & 0xf,
		    (rd(core + capability + 0x0c) >> 4) & 0x3f, status, (status >> 13) & 1, status & 0xf,
		    (status >> 4) & 0x3f);
	}

	/*
	 * The ELBI, all of it, and the core's debug registers beside it. S1's ninth run found PORT_DEBUG0 reading
	 * 0 -- the core's own view of "train!" never went high -- while PARF's own ltssm enable (bit 8) took the
	 * write and stayed. So either the ELBI is not where mainline puts it, or it is not clocked, or the
	 * wrapper takes that enable from somewhere else; a whole-window read says which of the first two.
	 */
	printf("wifi: s1:   the elbi, all of it, at %#x (about to read 16 words):\n", CORVETTE_PCIE0_ELBI);
	printf("wifi: s1:     %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(elbi), rd(elbi + 4), rd(elbi + 8), rd(elbi + 0xc),
	    rd(elbi + 0x10), rd(elbi + 0x14), rd(elbi + 0x18), rd(elbi + 0x1c));
	printf("wifi: s1:     %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(elbi + 0x20), rd(elbi + 0x24), rd(elbi + 0x28),
	    rd(elbi + 0x2c), rd(elbi + 0x30), rd(elbi + 0x34), rd(elbi + 0x38), rd(elbi + 0x3c));
	printf("wifi: s1:   the port's own registers: link control %#x, debug0 %#x, debug1 %#x\n",
	    rd(core + 0x710), rd(core + 0x728), rd(core + 0x72c));

	/*
	 * The enable. Nine runs have written ELBI_SYS_CTRL at +0x04 and never had it read back; the whole window
	 * shows why: +0x00 and +0x08 answer (+0x00 reads 0x1280, +0x08 reads 1) and +0x04 is dead. So this
	 * wrapper's ELBI is not laid out as mainline's, and the control register is the first one, not the
	 * fourth. Both are written here -- +0x00 with its own value plus bit 0, +0x04 the way mainline does it --
	 * and the log says which of the two reads back.
	 */
	/*
	 * The endpoint's reset is released here, as Linux does it: after the controller's init, after the PHY's
	 * own sequence, and one moment before training is started -- so that the radio comes out of reset into a
	 * controller that is ready for it, rather than ten milliseconds into an init that has not happened yet.
	 */
	/* the vendor's own writes, before the endpoint's reset is let go */
	kiwi_s1_vendor_extras(parf, core);

	/*
	 * The clock request. Under function 1 (pcie0_clk_req_n) the pin belongs to the controller and its
	 * GPIO input bit means nothing, which is why "clkreq reads 0, the radio is asking for a clock"
	 * was never a reading at all. The level is taken with the pin as a plain input carrying the
	 * device tree's pull-up, and the pin is then handed back to the controller. A radio that wants a
	 * clock holds the line low.
	 */
	kiwi_s1_write("pin 95 clkreq: read as a gpio with a pull-up", 0x1000UL * CORVETTE_PIN_CLKREQ,
	    tlmm + 0x1000UL * CORVETTE_PIN_CLKREQ, CORVETTE_PIN_CLKREQ_SLEEP);
	{
		uint32_t level = rd(tlmm + 0x1000UL * CORVETTE_PIN_CLKREQ + 4) & 1;

		printf("wifi: s1:   clkreq before the endpoint's reset is released: %u %s\n", level,
		    level ? "high, the radio is not asking for a clock" : "LOW, the radio is asking for one");
	}
	/* the clock request must reach the controller, so the pin carries function 1, not GPIO */
	kiwi_s1_write("pin 95 clkreq: pcie0_clk_req_n", 0x1000UL * CORVETTE_PIN_CLKREQ,
	    tlmm + 0x1000UL * CORVETTE_PIN_CLKREQ, CORVETTE_PIN_CLKREQ_N);
	/*
	 * Air180: 2/7 left the mux at BI_TCXO and 2c/7 still read 2, but by the time training started the
	 * register read 0 again -- the core/PHY reset in 3/7 (or the PHY's own pipe handshake) puts it back
	 * to its default. So air179's experiment never ran. Set it here, after every reset and before the
	 * endpoint's reset is released, so the whole attempt is clocked by the crystal, and say what it read
	 * both before and after the write.
	 */
	{
		vm_offset_t gcc = corvette_s1_io_map(CORVETTE_GCC_PHYS, CORVETTE_GCC_SIZE);

		printf("wifi: s1:   pipe mux just before the experiment: %#x (2/7 asked for 2)\n",
		    rd(gcc + 0x6b070) & 3);
		kiwi_s1_write("pipe mux: BI_TCXO, after the resets", 0x6b070,
		    gcc + 0x6b070, (rd(gcc + 0x6b070) & ~3u) | 2u);
	}
	kiwi_s1_write("pin 94 perst io: high", 0, tlmm + 0x1000UL * CORVETTE_PIN_PERST + 4, 2);
	printf("wifi: s1:   pin 94 is %s, the endpoint's reset released\n",
	    (rd(tlmm + 0x1000UL * CORVETTE_PIN_PERST + 4) & 1) ? "high" : "still low");
	/*
	 * The clock request, taken from the endpoint itself now that its reset is released and the reference
	 * clock is supposed to be running. A radio that has a clock and is out of reset may let CLKREQ# go; one
	 * that is still asking has either no clock or is not out of reset. This is the only reading the link has
	 * from the far side, so it is taken twice: here, and again after the training window below.
	 */
	{
		uint32_t clkreq = 0x1000UL * CORVETTE_PIN_CLKREQ, level;

		kiwi_s1_write("pin 95 clkreq: read as a gpio with a pull-up", clkreq,
		    tlmm + clkreq, CORVETTE_PIN_CLKREQ_SLEEP);
		level = rd(tlmm + clkreq + 4) & 1;
		printf("wifi: s1:   clkreq once the endpoint's reset was released: %u %s\n", level,
		    level ? "high, the radio let the request go" : "LOW, it is still asking for a clock");
		kiwi_s1_write("pin 95 clkreq: pcie0_clk_req_n", clkreq,
		    tlmm + clkreq, CORVETTE_PIN_CLKREQ_N);
	}
	/*
	 * One millisecond, which is the vendor's own number and the only one that has evidence behind it.
	 *
	 * msm_pcie_enable_link() releases PERST and then sleeps usleep_range(PERST_PROPAGATION_DELAY_US_MIN,
	 * PERST_PROPAGATION_DELAY_US_MAX) -- 1000 to 1005 microseconds, the defaults, because pcie0 has no
	 * qcom,perst-delay-us-min -- before msm_pcie_link_train() sets PARF_LTSSM bit 8. The phone's own dmesg
	 * shows what that buys: PERST released at 65.314318 and "PCIe RC0 link initialized" at 65.331193, so
	 * the endpoint presents its receiver termination and finishes training in seventeen milliseconds.
	 *
	 * S1 waited a whole second here -- an invention of an earlier round, first at 100 ms and then at
	 * 1000 ms, neither of which changed the outcome. A thousand times the vendor's delay is not a
	 * conservative choice: it leaves the endpoint sitting with its reset released and no training activity
	 * for a second, which is not a state the vendor ever puts it in.
	 */
	sleep_ms(1);

	/*
	 * The vendor's msm_pcie_link_train() enables training with a single read-modify-write of
	 * PARF_LTSSM -- write_mask(parf + 0x1b0, 0, BIT(8)) -- which sets bit 8 and leaves everything
	 * else alone. It never touches ELBI +0x00 or ELBI +0x04: the LT_ENABLE bit in ELBI_SYS_CTRL
	 * belongs to the older 2.1.0 op set, and this generation trains through PARF_LTSSM bit 8 only.
	 * S1 used to raise an edge (clear, 1 ms, set) AND write ELBI +0x00/+0x04 -- neither of which the
	 * vendor does, and writing the old LT_ENABLE beside the new one is exactly the kind of thing
	 * that keeps a DWC wrapper from ever leaving detect.quiet.
	 */
	/*
	 * Air192: msm_pcie_config_controller_phy(), the one vendor function the mechanical extraction
	 * found that this implementation never had.  In pci-msm.c it runs at the top of link_train() for
	 * every supported speed from 8.0 GT/s upwards:
	 *   PCIE_GEN3_RELATED (0x890)               bits 25:24 rate shadow select, bit 0 cleared
	 *   PCIE_GEN3_EQ_CONTROL (0x8a8)            bits 23:8  qcom,eq-pset-req-vec (absent here, so 0)
	 *   PCIE_GEN3_EQ_FB_MODE_DIR_CHANGE (0x8ac) bits 4:0   qcom,eq-fmdc-t-min-phase23 (dt: 1)
	 * The link capability reads back max speed 3, so the vendor loop runs once, for Gen3, and the
	 * shadow selector is 0.  These are equalisation settings for a link that is already training;
	 * they are here for exactness, not because they can lift detect.
	 */
	value = rd(core + 0x890);
	printf("wifi: s1:   related 0x890 %#x, eq control 0x8a8 %#x, eq fb 0x8ac %#x before the gen3 settings\n",
	    value, rd(core + 0x8a8), rd(core + 0x8ac));
	kiwi_s1_write("related: rate shadow select gen3", 0x890, core + 0x890, value & ~0x3000000u);
	kiwi_s1_write("related: zrxdc noncompl clear", 0x890, core + 0x890, rd(core + 0x890) & ~1u);
	value = rd(core + 0x8a8);
	kiwi_s1_write("eq control: pset request vector 0", 0x8a8, core + 0x8a8, value & ~0xffff00u);
	value = rd(core + 0x8ac);
	kiwi_s1_write("eq fb: t min phase23 1", 0x8ac, core + 0x8ac, (value & ~0x1fu) | 1u);

	value = rd(elbi);
	kiwi_s1_write("parf ltssm: enable", CORVETTE_PARF_LTSSM_OFFSET, parf + CORVETTE_PARF_LTSSM_OFFSET,
	    rd(parf + CORVETTE_PARF_LTSSM_OFFSET) | CORVETTE_PARF_LTSSM_EN);
	/*
	 * The first forty milliseconds, one millisecond at a time. Detect.Quiet is a state the state machine
	 * leaves on its own once its pipe clock runs -- the vendor's own dmesg shows seventeen milliseconds
	 * from the enable to a trained link. A state field that reads 0 for all forty says the pipe clock the
	 * PCS hands the core is not running at all, and no LTSSM setting can repair that. The windows below
	 * report only where the state ended up, so this is the one place the timeline exists.
	 */
	{
		vm_offset_t gcc = corvette_s1_io_map(CORVETTE_GCC_PHYS, CORVETTE_GCC_SIZE);
		uint32_t previous = 0xffff, changes = 0, mux_entry = rd(gcc + 0x6b070) & 3;

		printf("wifi: s1:   ltssm at 1 ms, the first 80 ms:");
		for (unsigned ms = 0; ms < 80; ms++) {
			uint32_t now = rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f;

			printf(" %u", now);
			if (now != previous) {
				changes++;
				previous = now;
			}
			sleep_ms(1);
		}
		printf("\n");
		printf("wifi: s1:   it %s\n", changes <= 1 ?
		    "never moved: the state machine is not being clocked out of detect.quiet" :
		    "moved; the pipe clock runs and the failure is later in training");
		/* 0x6b070 mux (0 = PHY PIPE), 0x6b048 pipe branch, 0x6b074 aux RCG, 0x6b058 rchng RCG */
		printf("wifi: s1:   pipe source %#x at entry, %#x after 80 ms; pipe branch %#x, aux rcg %#x, "
		    "rchng rcg %#x, clkreq %#x\n", mux_entry, rd(gcc + 0x6b070) & 3, rd(gcc + 0x6b048),
		    rd(gcc + 0x6b074), rd(gcc + 0x6b058), rd(parf + 0x2b0));
		printf("wifi: s1:   phy 0x214 %#x, phy 0x240 %#x, phy 0x244 %#x\n",
		    rd(parf + CORVETTE_PCIE0_PHY_AT + CORVETTE_PHY_STATUS_OFFSET),
		    rd(parf + CORVETTE_PCIE0_PHY_AT + 0x240), rd(parf + CORVETTE_PCIE0_PHY_AT + 0x244));
	}
	/*
	 * Air185: the sweep is gone.
	 *
	 * Air181 to air184 wrote PARF 0x4c eight ways and forced the pipe mux onto the PHY's PIPE. Neither is
	 * what the phone runs: stock reports gcc_pcie_0_pipe_clk_src on bi_tcxo (2), rate 19.2 MHz, with no
	 * consumer, and its PARF 0x4c is the boot value 0x1019. No sweep ever separated the eight
	 * combinations, so the link now trains with both registers exactly as the boot firmware left them and
	 * the only thing recorded is what they hold.
	 */
	{
		vm_offset_t gcc = corvette_s1_io_map(CORVETTE_GCC_PHYS, CORVETTE_GCC_SIZE);
		uint32_t refclk = rd(parf + CORVETTE_PARF_PHY_REFCLK), pipe = rd(gcc + 0x6b070);

		printf("wifi: s1: 6b/7: the reference clock and the pipe selector are left as the boot firmware set "
		    "them: parf 0x4c %#x (bit 9 %u, 12 %u, 16 %u), gcc 0x6b070 %#x\n", refclk,
		    (refclk >> 9) & 1, (refclk >> 12) & 1, (refclk >> 16) & 1, pipe & 3);
	}
	state = kiwi_s1_watch(elbi, 300);
	printf("wifi: s1:   after them: state %#x, elbi +0x00 %#x, +0x08 %#x, debug0 %#x (bit 0 is the core's own "
	    "\"train\")\n", state, value, rd(elbi + 8), rd(core + 0x728));
	/*
	 * The enable stays exactly where msm_pcie_link_train() put it. Air160 took PARF_LTSSM bit 8 down
	 * for two milliseconds to watch the core's own "train" bit move, then put it back: that aborts the
	 * attempt that has just started, and the vendor does not do it -- one write, then poll. From here
	 * the LTSSM state is sampled continuously, with the enable never disturbed again.
	 */
	state = kiwi_s1_watch(elbi, 200);
	/* Air193: the air190 second perst edge is gone; the vendor releases perst exactly once. */

	/*
	 * Two more things upstream does and S1 has never done. First the link control register itself, which
	 * nothing has read: bit 5 is "link disable", and a port whose link is disabled keeps its LTSSM where it
	 * is whatever else is written. Then the speed change request -- dw_pcie_setup_rc ends with
	 * LINK_WIDTH_SPEED_CONTROL |= SPEED_CHANGE, which is what makes a DWC port bring its link up again after
	 * its registers were programmed.
	 */
	if (capability != 0) {
		uint32_t control = rd(core + capability + 0x10);

		printf("wifi: s1:   link control %#x: link disable %u, retrain %u, target speed %u\n",
		    control & 0xffff, (control >> 5) & 1, (control >> 15) & 1, control & 0xf);
		/*
		 * Nothing is written here any more. The link disable bit and SPEED_CHANGE are both mainline
		 * dw_pcie_setup_rc() habits: the vendor's link_train() makes one write to this word (the field at
		 * 5h/7) and then enables the LTSSM, and the bootloader's SPEED_CHANGE bit is left alone by the
		 * vendor for the whole life of the link. S1 both set and cleared it, and cleared a bit Android
		 * never clears.
		 */
		state = kiwi_s1_watch(elbi, 300);
		printf("wifi: s1:   with the link control left alone: state %#x, link status %#x, debug0 %#x\n", state,
		    rd(core + capability + 0x10) >> 16, rd(core + 0x728));
	}

	state = kiwi_s1_watch(elbi, 1000);
	if (capability != 0) {
		status = rd(core + capability + 0x10) >> 16;
	}
	/*
	 * Air184: three more samples over 300 ms. Air183 reached detect.active in 9 ms and was still there
	 * 700 ms later, but one reading at the end cannot tell "stuck" from "retrying"; sampling the state,
	 * the ELBI's status word and the link status together says whether anything in the core moves at all.
	 */
	printf("wifi: s1:   three samples over 300 ms, the state now %#x:\n", state);
	for (unsigned i = 0; i < 3; i++) {
		printf("wifi: s1:   +%u00 ms: parf %#x (state %#x), elbi +0x08 %#x, link status %#x\n", i,
		    rd(parf + CORVETTE_PARF_LTSSM_OFFSET), rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f,
		    rd(elbi + CORVETTE_ELBI_SYS_STTS_OFFSET),
		    capability != 0 ? rd(core + capability + 0x10) >> 16 : 0);
		if (i != 2)
			sleep_ms(100);
	}
	printf("wifi: s1:   the core's own port state: debug0 %#x (bit 0 is its own \"train\"), debug1 %#x, link "
	    "width/speed %#x\n", rd(core + 0x728), rd(core + 0x72c), rd(core + 0x80c));
	printf("wifi: s1:   ltssm says %#x %s after a further 700 ms; parf says %#x (state %#x); the link's own "
	    "status %#x (up %u, speed %u, width %u)\n", state,
	    kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)",
	    rd(parf + CORVETTE_PARF_LTSSM_OFFSET), rd(parf + CORVETTE_PARF_LTSSM_OFFSET) & 0x3f, status,
	    (status >> 13) & 1, status & 0xf, (status >> 4) & 0x3f);
	{
		uint32_t clkreq = 0x1000UL * CORVETTE_PIN_CLKREQ, level, i;

		kiwi_s1_write("pin 95 clkreq: read as a gpio with a pull-up", clkreq,
		    tlmm + clkreq, CORVETTE_PIN_CLKREQ_SLEEP);
		/*
		 * One sample said only "low" in air159 and air160; a timeline says whether the request ever
		 * drops. CLKREQ# is released once the endpoint has a stable reference clock, so a run of
		 * zeros across two hundred milliseconds means the radio never saw one.
		 */
		printf("wifi: s1:   clkreq over 200 ms at the end of the window:");
		for (i = 0; i < 6; i++) {
			level = rd(tlmm + clkreq + 4) & 1;
			printf(" %u", level);
			if (i != 5)
				sleep_ms(40);
		}
		printf(" (0 = the radio is still asking for a clock)\n");
		kiwi_s1_write("pin 95 clkreq: pcie0_clk_req_n", clkreq,
		    tlmm + clkreq, CORVETTE_PIN_CLKREQ_N);
	}
	kiwi_s1_vendor_dump(parf, core);
	/* Either one is enough to go on to the bus: a link the capability calls up is up, whatever a status
	 * register that has never answered says. */
	if (state != 0x11 && !(status & (1u << 13))) {
		printf("wifi: s1:   the link is not up: parf ltssm state %#x %s, link status dllla %u. The seven "
		    "RPMh rail commands completed, but their response words do not verify physical voltage. The "
		    "radio has not answered link detection; external power and reference-clock delivery remain "
		    "unverified.\n", state,
		    kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)", (status >> 13) & 1);
	}
	return state == 0x11 || (status & (1u << 13)) != 0;
}

/*
 * 2c/7: the link's own state machine (dt: reg-name pcie_sm, 0x1d07000).
 *
 * Where its program goes is not what the device tree's property names suggest. The vendor's own driver,
 * drivers/pci/controller/pci-msm.c in OnePlus' kernel for this phone (msm_pcie_cesta_load_sm_seq), writes:
 *
 *	qcom,pcie-sm-seq         at pcie_sm + 0, + 4, + 8 ...             (not at the "start" offset)
 *	qcom,pcie-sm-branch-seq  at pcie_sm + branch_offset + 4, + 8 ...  (0x1000, 0x1004 ...)
 *	then sets BIT(0) of the register at qcom,pcie-sm-start-offset     (0x1090) -- that word is the run bit
 *
 * S1 wrote the thirteen words at 0x1090, which is why 0x1090 then read back 0x1 (the run bit, set by the
 * first word's lowest bit) and the other twelve read 0: the program had been written into the run register
 * and its neighbours. With the program at +0, the branches at +0x1000 and the run bit last, the state
 * machine has what the vendor's driver gives it -- and the vendor loads it at probe time, long before the
 * link is started.
 */
#define CORVETTE_SM_SEQ_OFFSET          0x0000          /* (pcie_sm + 4*i, from zero) */
#define CORVETTE_SM_BRANCH_OFFSET       0x1000          /* dt: qcom,pcie-sm-branch-offset */
#define CORVETTE_SM_START_OFFSET        0x1090          /* dt: qcom,pcie-sm-start-offset, bit 0 runs it */
#define CORVETTE_SM_WORDS               13
#define CORVETTE_SM_BRANCH_WORDS        7

static void
kiwi_s1_state_machine(void)
{
	static const uint32_t sequence[CORVETTE_SM_WORDS] = {
		0x1c018081, 0x70074002, 0x50028000, 0x28007003, 0x80804002, 0x70021c01, 0x18001802,
		0x70005000, 0x10004000, 0x80814002, 0x18001c01, 0x1c018080, 0x100
	};
	static const uint32_t branches[CORVETTE_SM_BRANCH_WORDS] = { 0x04, 0x1c, 0x24, 0x2c, 0x00, 0x00, 0x00 };
	vm_offset_t sm = corvette_s1_io_map(CORVETTE_PCIE0_SM, CORVETTE_PCIE0_SM_SIZE);
	uint32_t before[CORVETTE_SM_WORDS], after[CORVETTE_SM_WORDS], run, same = 0;

	printf("wifi: s1: 2c/7: the state machine at %#x: its program goes at +0, its branches at +%#x, and "
	    "+%#x is the run bit\n", CORVETTE_PCIE0_SM, CORVETTE_SM_BRANCH_OFFSET, CORVETTE_SM_START_OFFSET);
	for (unsigned i = 0; i < CORVETTE_SM_WORDS; i++) {
		before[i] = rd(sm + CORVETTE_SM_SEQ_OFFSET + 4 * i);
	}
	printf("wifi: s1:   +0 before: %#x %#x %#x %#x %#x %#x %#x\n", before[0], before[1], before[2], before[3],
	    before[4], before[5], before[6]);
	printf("wifi: s1:   +0 before: %#x %#x %#x %#x %#x %#x, run bit %#x\n", before[7], before[8], before[9],
	    before[10], before[11], before[12], rd(sm + CORVETTE_SM_START_OFFSET));

	for (unsigned i = 0; i < CORVETTE_SM_WORDS; i++) {
		kiwi_s1_write("state machine", CORVETTE_SM_SEQ_OFFSET + 4 * i, sm + CORVETTE_SM_SEQ_OFFSET + 4 * i,
		    sequence[i]);
	}
	for (unsigned i = 0; i < CORVETTE_SM_BRANCH_WORDS; i++) {
		kiwi_s1_write("state machine branch", CORVETTE_SM_BRANCH_OFFSET + 4 * i,
		    sm + CORVETTE_SM_BRANCH_OFFSET + 4 * i, branches[i]);
	}
	for (unsigned i = 0; i < CORVETTE_SM_WORDS; i++) {
		after[i] = rd(sm + CORVETTE_SM_SEQ_OFFSET + 4 * i);
		if (after[i] == sequence[i]) {
			same++;
		}
	}

	/* The run bit last, as the vendor does: the machine is told to run only once its program is in. It is
	 * taken down and up again even when it was already set, so that the edge is there whatever happened in
	 * an earlier boot. */
	run = rd(sm + CORVETTE_SM_START_OFFSET);
	kiwi_s1_write("state machine: stop", CORVETTE_SM_START_OFFSET, sm + CORVETTE_SM_START_OFFSET, run & ~1u);
	kiwi_s1_write("state machine: run", CORVETTE_SM_START_OFFSET, sm + CORVETTE_SM_START_OFFSET, run | 1);

	printf("wifi: s1:   +0 after: %#x %#x %#x %#x %#x %#x %#x\n", after[0], after[1], after[2], after[3],
	    after[4], after[5], after[6]);
	printf("wifi: s1:   +0 after: %#x %#x %#x %#x %#x %#x\n", after[7], after[8], after[9], after[10],
	    after[11], after[12]);
	printf("wifi: s1:   2c/7: %u of %u program words read back as written, run bit now %#x\n", same,
	    CORVETTE_SM_WORDS, rd(sm + CORVETTE_SM_START_OFFSET));
}


/*
 * 5f/7: what actually powers this block.
 *
 * The phone's own bootargs say where it lives:
 *
 *	pci-msm-drv.pcie_sm_regs=0x1D07000,0x1040,0x1048,0x3000,0x2
 *
 * -- base 0x1d07000, power control at +0x1040, its mask at +0x1048, one instance every 0x3000, two of
 * them. drivers/pci/controller/pci-msm.c writes 1 into both registers of every instance when it loads,
 * and the comment above that loop says why: "If the override is not done then CX Power Collapse won't
 * happen". S1 has never written them, so this block has been running on nothing but what the CESTA state
 * machine voted, and CESTA is told what to vote by the CRM block at 0x1d01000, which nothing here has
 * spoken to either.
 *
 * Two questions, in this order, because the first decides what the second means:
 *
 *   - link disable, port link control bit 5. An LTSSM that is running leaves detect.quiet for DISABLED
 *     within a millisecond of being told to; one that still reads 0 has no clock to run on. That is the
 *     whole difference between "the link will not train" and "the link is not allowed to try".
 *   - the override. Control then mask, as the vendor writes them, and the state sampled for 300 ms.
 *
 * Last, whether the power-up took 5c/7 and 5d/7 with it: if the core stops answering, its base addresses
 * and its port are set again before 6/7 runs, so this run still ends with a link attempt. The CRM is read
 * after all of that and before nothing at all -- it is the one window here that nobody has touched, and
 * if it is the read that does not come back, everything above it is already in the log.
 */
#define CORVETTE_SM_PWR_CTRL            0x1040          /* (the vendor's override: force the power state) */
#define CORVETTE_SM_PWR_MASK            0x1048
#define CORVETTE_SM_INSTANCE            0x3000          /* (bootargs: 0x1D07000,0x1040,0x1048,0x3000,0x2) */
#define CORVETTE_PCIE_CRM               0x1d01000       /* (dt: crm@1d01000, label pcie_crm) */
/* dt: crm@1d01000 has four ranges -- "base" 0x1d01000, "crm_b" 0x1d04800, "crm_c" 0x1d05000, "crm_v" 0x1d04000.
 * The votes go to "base" (DRV_BASE 0x200, resource stride 4, PWR_ST0..4 at 0x0/0x40/0x80/0x600/0x640); the status
 * crm_read_curr_perf_ol() returns comes out of "crm_c", 0x4000 further on. */
#define CORVETTE_PCIE_CRM_SIZE          0x6000          /* (0x1d01000 .. 0x1d07000, pcie_sm starts at the end) */
#define CORVETTE_CRMC_AGGR_PERF_OL      0x4004          /* (crm_c + 0x04) */
#define CORVETTE_CRMC_CURR_PERF_OL      0x400c          /* (crm_c + 0x0c) */
#define CORVETTE_CRMC_SEQ_STATUS        0x41a0          /* (crm_c + 0x1a0) */

static void
kiwi_s1_power(vm_offset_t parf, vm_offset_t core, vm_offset_t elbi)
{
	vm_offset_t sm = corvette_s1_io_map(CORVETTE_PCIE0_SM, CORVETTE_PCIE0_SM_SIZE);
	vm_offset_t crm = corvette_s1_io_map(CORVETTE_PCIE_CRM, CORVETTE_PCIE_CRM_SIZE);
	uint32_t capability = kiwi_s1_capability(core), control, state;

	printf("wifi: s1: 5f/7: what powers this block: pcie_sm %#x + %#x (control), + %#x (mask), one "
	    "instance every %#x\n", CORVETTE_PCIE0_SM, CORVETTE_SM_PWR_CTRL, CORVETTE_SM_PWR_MASK,
	    CORVETTE_SM_INSTANCE);
	printf("wifi: s1:   override now: instance 0 control %#x mask %#x; instance 1 control %#x mask %#x\n",
	    rd(sm + CORVETTE_SM_PWR_CTRL), rd(sm + CORVETTE_SM_PWR_MASK),
	    rd(sm + CORVETTE_SM_INSTANCE + CORVETTE_SM_PWR_CTRL),
	    rd(sm + CORVETTE_SM_INSTANCE + CORVETTE_SM_PWR_MASK));

	/* The one bit that says whether there is a clock under the state machine at all. */
	if (capability != 0) {
		control = rd(core + capability + 0x10);
		/* The Link Disable bit is left alone. The vendor never writes DBI 0x80 at all -- its only writes
		 * to the capability are LNKCTL2 (skipped: pcie0 has no qcom,target-link-speed) and DEVCAP (the max
		 * read request size in link_train). S1 toggled this bit just to watch the state machine, which is
		 * one more bit written that Android never writes. */
		printf("wifi: s1:   link disable %u (left alone; the vendor never writes this word)\n",
		    (control >> 5) & 1);
	}

	/*
	 * Nothing is written to these two any more.
	 *
	 * The vendor touches them exactly twice in its whole life. pcie_init() writes 0x1 to both, per instance,
	 * but only when the pci-msm-drv.pcie_sm_regs module parameter is on the command line. Then
	 * msm_pcie_cesta_load_sm_seq() -- the function that programs the sequence 2c/7 has just loaded --
	 * clears both to 0 as its very first act, before it writes a single instruction word. So by the time
	 * Android enables a link the override is 0, and the bootloader leaves it at 0 as well: the line above
	 * reads 0/0 for both instances on this phone.
	 *
	 * S1 wrote 2, which is neither of the vendor's two values and is not a bit any vendor path sets. With a
	 * state machine that owns this block's power sequencing, an override bit nobody publishes is a good way
	 * to have the hardware hold the PHY, or the reference clock, in a state no driver asked for.
	 */
	state = kiwi_s1_watch(elbi, 300);
	printf("wifi: s1:   5f/7: the override left as the bootloader had it: state %#x %s, control %#x, mask %#x\n",
	    state, kiwi_s1_ltssm_name(state) ? kiwi_s1_ltssm_name(state) : "(unnamed)",
	    rd(sm + CORVETTE_SM_PWR_CTRL), rd(sm + CORVETTE_SM_PWR_MASK));

	/* If the power-up reset the core, 5c/7 and 5d/7 are gone; put them back rather than lose the run. */
	if (rd(core) == 0) {
		printf("wifi: s1:   the power-up took the core's base addresses with it: setting them again\n");
		kiwi_s1_dbi(parf, core);
		kiwi_s1_port(core);
	} else {
		printf("wifi: s1:   the core still answers %#x, so 5c/7 held\n", rd(core));
	}

	printf("wifi: s1:   about to read the crm at %#x, which nothing has spoken to yet\n", CORVETTE_PCIE_CRM);
	printf("wifi: s1:   crm: version %#x, parameters %#x %#x, enable %#x\n", rd(crm), rd(crm + 4), rd(crm + 8),
	    rd(crm + 0xc));
	printf("wifi: s1:   crm channel 0: busy %#x, update %#x, behave %#x; channel 1: %#x, %#x, %#x\n",
	    rd(crm + 0x1000), rd(crm + 0x1020), rd(crm + 0x1040), rd(crm + 0x1004), rd(crm + 0x1024),
	    rd(crm + 0x1044));
}

/*
 * 2c/7: the pipe clock, and the vote CESTA is waiting for.
 *
 * Everything the core does after "train!" runs on the pipe clock, and the pipe clock is not one of the
 * seven branches S1 voted: those are the wrapper's own clocks, and the core's pipe input comes from the
 * PHY through a two-input mux in GCC --
 *
 *	gcc_pcie_0_pipe_clk_src: gcc 0x6b070, one bit, the PHY's clock on one side and the crystal on the other
 *	gcc_pcie_0_pipe_clk:     gcc 0x52008 bit 4 (voted), with halt_reg 0x6b048 and BRANCH_HALT_SKIP,
 *	                         which is why 2b/7 could not say anything about it
 *	gcc_pcie_0_phy_rchng_clk_src: gcc 0x6b058, 100 MHz from gpll0, the PHY's rate-change clock
 *
 * A state machine clocked by the crystal instead of by the PHY sits in detect.quiet for ever: it has a
 * clock, so it counts, but the clock is not the one the PCS hands it. So both values of that one bit are
 * tried, and the log says which one, if either, lets the state move.
 *
 * Then the vote. The bootargs name the CESTA registers, and the CRM at 0x1d01000 is where the votes go:
 * its parameters read back as two channels, five power states per channel, two resources, two hardware
 * drivers -- that is the layout drivers/soc/qcom/crm.c describes, so the vote can be placed exactly:
 *
 *	h / w 0, channel 0: 0x200 + pwr_state offset + 4 * resource     (pwr_state offsets 0, 0x40, 0x80,
 *	h / w 0, channel 0: 0x600, 0x640 for the last two)
 *	then 0x1020 = BIT(0) to switch the channel in, and 0x1000 bit 0 to say it took.
 *
 * The two states that are used are PERF_LVL_GEN1, the level msm_pcie_cesta_map_save() puts D0 at, and the
 * channel is switched in only after every word is in -- a half-written channel is a channel CESTA may
 * act on.
 */
#define CORVETTE_GCC_PIPE_SRC           0x6b070         /* (gcc-pineapple parent map 7: 0=PHY PIPE, 2=BI_TCXO) */
static uint32_t corvette_s1_pipe_src_original;
static bool corvette_s1_pipe_src_changed;
#define CORVETTE_GCC_PIPE_HALT          0x6b048
#define CORVETTE_GCC_AUX_RCG            0x6b074
#define CORVETTE_GCC_RCHNG_RCG          0x6b058
#define CORVETTE_CRM_DRV0               0x200           /* (dt bootargs: pcie_sm_regs places the sm; the crm is its own block) */
#define CORVETTE_CRM_CHN_BUSY           0x1000
#define CORVETTE_CRM_CHN_UPDATE         0x1020
#define CORVETTE_CRM_RESOURCES          2
#define CORVETTE_CRM_PWR_STATES         5
#define CORVETTE_CRM_PERF_GEN3          4               /* (PERF_LVL_GEN3: the vendor adds PERF_LVL_L1SS to the
                                                         *   highest speed the port can do, and qcom,bw-scale has
                                                         *   three entries, so Gen3 -> 3 + 1 = 4) */

static const uint32_t corvette_crm_pwr_state[] = { 0x000, 0x040, 0x080, 0x600, 0x640 };

static void
kiwi_s1_cesta_d0(vm_offset_t gcc)
{
	vm_offset_t crm = corvette_s1_io_map(CORVETTE_PCIE_CRM, CORVETTE_PCIE_CRM_SIZE);

	/* Match msm_pcie_enable(): apply D0 immediately after loading CESTA, before core/PHY reset. */
	printf("wifi: s1: 2c/7: CESTA D0 vote; PIPE selector %#x, branch vote %#x\n",
	    rd(gcc + CORVETTE_GCC_PIPE_SRC) & 3, rd(gcc + 0x52008) & (1u << 4));

	/*
	 * Last, the vote CESTA reads. pcie_crm_desc.set_chn_behave is false, so the channel behaves by itself
	 * and CHN_BEHAVE stays 0 -- that part of the earlier read was normal. Five power states, two
	 * resources, channel 0, and the channel is switched in only once every word is in it.
	 */
	printf("wifi: s1:   about to vote D0 through the crm at %#x: five states, two resources, channel 0\n",
	    CORVETTE_PCIE_CRM);
	/*
	 * The vendor's own vote: drivers/pci/controller/pci-msm.c's msm_pcie_cesta_map_apply() writes the
	 * D0 map to MAX_POWER_STATE (two) power states of one resource -- the resource being the controller
	 * index -- and the value in the map is the performance level the port is to be scaled to. Everything
	 * else in the cache stays at zero, and the flush writes those zeros out too.
	 */
	for (unsigned i = 0; i < CORVETTE_CRM_PWR_STATES; i++) {
		for (unsigned j = 0; j < CORVETTE_CRM_RESOURCES; j++) {
			vm_offset_t at = crm + CORVETTE_CRM_DRV0 + corvette_crm_pwr_state[i] + 4 * j;
			uint32_t value = (j == 0 && i < 2) ? CORVETTE_CRM_PERF_GEN3 : 0;

			kiwi_s1_write("crm vote", CORVETTE_CRM_DRV0 + corvette_crm_pwr_state[i] + 4 * j, at, value);
		}
	}
	/*
	 * The CRM's own view of the level, before the vote. crm.c: CRMC's CURR_PERF_OL is at 0x0c and
	 * AGGR_PERF_OL at 0x04, one resource every 0x200 -- and msm_pcie_cesta_curr_perf_ol() in pci-msm.c
	 * prints exactly this value by name ("Current perf ol is Gen3 speed") on the working phone. The word
	 * S1 used to read at 0x1c0 is PERF_OL_STATUS, which crm.c defines as IRQ_STATUS: it reads 0 because
	 * nothing faulted, and it says nothing about whether the vote landed.
	 */
	printf("wifi: s1:   crm before the vote: aggr perf ol %#x, curr perf ol %#x, seq status %#x "
	    "(0 D3COLD, 1 L1SS, 2 Gen1, 3 Gen2, 4 Gen3; read from crm_c at %#x)\n",
	    rd(crm + CORVETTE_CRMC_AGGR_PERF_OL), rd(crm + CORVETTE_CRMC_CURR_PERF_OL),
	    rd(crm + CORVETTE_CRMC_SEQ_STATUS), CORVETTE_PCIE_CRM + 0x4000);
	kiwi_s1_write("crm: channel 0 switched in", CORVETTE_CRM_CHN_UPDATE, crm + CORVETTE_CRM_CHN_UPDATE, 1);
	for (unsigned i = 0; i < 100 && !(rd(crm + CORVETTE_CRM_CHN_BUSY) & 1); i++) {
		sleep_ms(1);
	}
	printf("wifi: s1:   2c/7: after the vote: busy %#x, update %#x, aggr perf ol %#x, curr perf ol %#x, "
	    "seq status %#x\n", rd(crm + CORVETTE_CRM_CHN_BUSY), rd(crm + CORVETTE_CRM_CHN_UPDATE),
	    rd(crm + CORVETTE_CRMC_AGGR_PERF_OL), rd(crm + CORVETTE_CRMC_CURR_PERF_OL),
	    rd(crm + CORVETTE_CRMC_SEQ_STATUS));
	if ((rd(crm + CORVETTE_CRMC_CURR_PERF_OL) & 0xff) != CORVETTE_CRM_PERF_GEN3) {
		printf("wifi: s1:   the crm's current level is %u, the vote asked for %u\n",
		    rd(crm + CORVETTE_CRMC_CURR_PERF_OL) & 0xff, CORVETTE_CRM_PERF_GEN3);
	} else {
		printf("wifi: s1:   the crm is at the level the vote asked for: Gen3\n");
	}
}

static const uint32_t corvette_iatu_regs[7] = {
	CORVETTE_IATU_CTRL1, CORVETTE_IATU_CTRL2, CORVETTE_IATU_LBAR,
	CORVETTE_IATU_UBAR, CORVETTE_IATU_LAR, CORVETTE_IATU_LTAR,
	CORVETTE_IATU_UTAR,
};

static void
corvette_s2_iatu_restore(vm_offset_t iatu)
{
	if (!corvette_s2_iatu_saved_valid) {
		return;
	}
	for (int region = 1; region >= 0; region--) {
		vm_offset_t at = iatu + (uint32_t)region * CORVETTE_PCIE_IATU_REGION_STRIDE;
		wr(at + CORVETTE_IATU_CTRL2, 0);
		for (int reg = 6; reg >= 2; reg--) {
			wr(at + corvette_iatu_regs[reg], corvette_s2_iatu_saved[region][reg]);
		}
		wr(at + CORVETTE_IATU_CTRL1, corvette_s2_iatu_saved[region][0]);
		wr(at + CORVETTE_IATU_CTRL2, corvette_s2_iatu_saved[region][1]);
	}
	__asm__ volatile ("dsb sy");
	corvette_s2_iatu_saved_valid = false;
	printf("wifi: s2: both outbound iATU regions restored to their entry values\n");
}

static bool
corvette_s2_iatu_program(vm_offset_t iatu, unsigned region, uint32_t type,
    uint32_t host, uint32_t limit, uint32_t target)
{
	vm_offset_t at = iatu + region * CORVETTE_PCIE_IATU_REGION_STRIDE;
	uint32_t values[7] = { type, CORVETTE_IATU_ENABLE, host, 0, limit, target, 0 };

	wr(at + CORVETTE_IATU_CTRL2, 0);
	for (unsigned reg = 0; reg < 7; reg++) {
		if (reg == 1) {
			continue;
		}
		wr(at + corvette_iatu_regs[reg], values[reg]);
	}
	__asm__ volatile ("dsb sy");
	wr(at + CORVETTE_IATU_CTRL2, CORVETTE_IATU_ENABLE);
	__asm__ volatile ("dsb sy");
	for (unsigned reg = 0; reg < 7; reg++) {
		uint32_t actual = rd(at + corvette_iatu_regs[reg]);
		printf("wifi: s2:   iATU region %u reg +%#x = %#x (wanted %#x)\n", region,
		    corvette_iatu_regs[reg], actual, values[reg]);
		if (actual != values[reg]) {
			return false;
		}
	}
	return true;
}

/*
 * S2 uses the vendor's outbound CFG0 translation for bus 1, followed by the DT-declared
 * identity MEM32 aperture. BAR reads are allowed only when the endpoint already reports a
 * 32-bit BAR inside that aperture. The firmware's original iATU state is restored by S1 cleanup.
 */
static bool
kiwi_s2_iatu(vm_offset_t core)
{
	vm_offset_t iatu = core + 0x1000;
	vm_offset_t conf = corvette_s1_io_map(CORVETTE_PCIE0_ECAM, 0x1000);
	uint32_t endpoint, class, bar0, mem_value;
	const uint32_t addresses[2][7] = {
		{ CORVETTE_IATU_CTRL1, CORVETTE_IATU_CTRL2, CORVETTE_IATU_LBAR,
		  CORVETTE_IATU_UBAR, CORVETTE_IATU_LAR, CORVETTE_IATU_LTAR, CORVETTE_IATU_UTAR },
		{ CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_CTRL1,
		  CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_CTRL2,
		  CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_LBAR,
		  CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_UBAR,
		  CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_LAR,
		  CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_LTAR,
		  CORVETTE_PCIE_IATU_REGION_STRIDE + CORVETTE_IATU_UTAR },
	};

	printf("wifi: s2: save the two outbound iATU regions before programming vendor CFG0 and DT MEM32 windows\n");
	for (unsigned region = 0; region < 2; region++) {
		for (unsigned reg = 0; reg < 7; reg++) {
			corvette_s2_iatu_saved[region][reg] = rd(iatu + addresses[region][reg]);
			printf("wifi: s2:   saved region %u reg +%#x = %#x\n", region, addresses[region][reg],
			    corvette_s2_iatu_saved[region][reg]);
		}
	}
	corvette_s2_iatu_saved_valid = true;

	if (!corvette_s2_iatu_program(iatu, 0, CORVETTE_IATU_CFG0,
	    CORVETTE_PCIE0_ECAM, CORVETTE_PCIE0_ECAM + 0xfff, CORVETTE_CFG0_BUS1_DEV0)) {
		printf("wifi: s2: CFG0 iATU region did not read back; endpoint config is not accessed\n");
		return false;
	}
	if (!corvette_s2_iatu_program(iatu, 1, CORVETTE_IATU_MEM,
	    CORVETTE_PCIE0_MEM32, CORVETTE_PCIE0_MEM32 + CORVETTE_PCIE0_MEM32_SIZE - 1,
	    CORVETTE_PCIE0_MEM32)) {
		printf("wifi: s2: MEM32 iATU region did not read back; endpoint config is not accessed\n");
		return false;
	}

	printf("wifi: s2: CFG0 window at %#x now targets bus 1/device 0; about to read endpoint identity\n",
	    CORVETTE_PCIE0_ECAM);
	endpoint = rd(conf);
	class = rd(conf + 8);
	bar0 = rd(conf + 0x10);
	printf("wifi: s2: endpoint id %#x (vendor %#x device %#x), class/revision %#x, BAR0 %#x\n",
	    endpoint, endpoint & 0xffff, endpoint >> 16, class, bar0);
	if (endpoint == 0xffffffff || (endpoint & 0xffff) != 0x17cb || (endpoint >> 16) != 0x1107) {
		printf("wifi: s2: endpoint identity mismatch; do not access MEM32 aperture\n");
		return false;
	}

	if (bar0 == 0 || bar0 == 0xffffffff || (bar0 & 1) || (bar0 & 6) != 0) {
		printf("wifi: s2: BAR0 is unset, IO, or 64-bit; skip endpoint MMIO read\n");
		return true;
	}
	bar0 &= ~0xfu;
	if (bar0 < CORVETTE_PCIE0_MEM32 || bar0 > CORVETTE_PCIE0_MEM32 + CORVETTE_PCIE0_MEM32_SIZE - 4) {
		printf("wifi: s2: BAR0 %#x is outside the DT MEM32 aperture; skip endpoint MMIO read\n", bar0);
		return true;
	}
	{
		vm_offset_t bar = corvette_s1_io_map(bar0 & ~0xfffu, 0x1000);
		mem_value = rd(bar + (bar0 & 0xfff));
		printf("wifi: s2: BAR0 first dword at %#x = %#x\n", bar0, mem_value);
	}
	return true;
}

/* 7/7: the two devices behind the link, through ECAM. Reached only with the link at L0: before that these
 * addresses do not answer, and the kernel that waits for an answer is not the one that comes back. */
static void
kiwi_s1_bus(void)
{
	/* air206: bus 0 rides the same 1 MiB window 5/7 mapped (cache hit); bus 1 device 0 gets its
	 * own 4 KiB window instead of the 2 MiB one that blew the 8 MiB I/O submap after the link
	 * came up (air205's panic: kmem_alloc 0x200000, KERN_NO_SPACE). */
	vm_offset_t ecam = corvette_s1_io_map(CORVETTE_PCIE0_ECAM, 0x100000);      /* (bus 0 only) */
	vm_offset_t radio_cfg = corvette_s1_io_map(CORVETTE_PCIE0_ECAM + 0x100000, 0x1000);
	uint32_t root, root_class, root_header;

	/* Said before the first read: if ECAM is not answering yet, this is the line the log ends on. */
	printf("wifi: s1: 7/7: ECAM %#x: about to read bus 0's config space\n", CORVETTE_PCIE0_ECAM);
	root = rd(ecam);
	root_class = rd(ecam + 8);
	root_header = rd(ecam + 0x0c);
	uint32_t capability = rd(ecam + 0x34) & 0xfc, link = 0;

	if (capability >= 0x40 && capability <= 0xfc) {
		link = rd(ecam + capability + 0x10) >> 16;      /* (the same sixteen-bit register: link control, then status) */
	}
	printf("wifi: s1: 7/7: ECAM %#x: bus 0 %04x:%04x (expected 17cb:0113), class/revision %#x, header %#x\n",
	    CORVETTE_PCIE0_ECAM, root & 0xffff, root >> 16, root_class, root_header >> 16);
	printf("wifi: s1:   bus 0's link status %#x: up %u, speed %u, width %u\n", link, (link >> 13) & 1, link & 0xf,
	    (link >> 4) & 0x3f);
	printf("wifi: s1:   %s\n", (root & 0xffff) == 0x17cb && (root >> 16) == 0x0113 ? "the root complex answers" :
	    "the root complex does not answer as expected");

	/* Bus 1 last, and only after bus 0 has said its piece: if this read is the one that does not come back,
	 * everything above it is already in the log. */
	printf("wifi: s1:   bus 1, %#x: about to read the radio's config space\n", CORVETTE_PCIE0_ECAM + 0x100000);
	{
		uint32_t radio = rd(radio_cfg), radio_class = rd(radio_cfg + 8), bar = rd(radio_cfg + 0x10);

		printf("wifi: s1:   bus 1 %04x:%04x (expected 17cb:1107), class/revision %#x, bar0 %#x\n", radio & 0xffff,
		    radio >> 16, radio_class, bar);
		printf("wifi: s1:   %s\n", (radio & 0xffff) == 0x17cb && (radio >> 16) == 0x1107 ? "THE RADIO ANSWERS" :
		    "the radio does not answer: bus 1 is not its config space yet");
	}
}

/*
 * The way back.
 *
 * "fastboot oem stop" does not reset this SoC: xnuloader flags the kernel, the kernel returns through its
 * stub, and everything the kernel left behind is still there -- clocks, power domains, the sequencer, the
 * PHY. That was harmless while S1 only read; now it writes, and a second run that starts from a
 * half-configured PCIe block reads a window with no clock under it, which on this NoC aborts instead of
 * returning zero. That is the 900E that stop-then-stage started to produce.
 *
 * So everything this step changes is put back, in the reverse order it was changed: the pipe clock to the
 * crystal, the pcie_sm override withdrawn, the CRM channel given back, the sequencer's TCSs left as they
 * were found, and the PHY back under reset and out of power. Nothing is collapsed and no rail is given
 * back: the power domains were up when S1 arrived, and they stay up.
 */
static void
kiwi_s1_clean(void)
{
	vm_offset_t gcc = corvette_s1_io_map(CORVETTE_GCC_PHYS, CORVETTE_GCC_SIZE);
	vm_offset_t sm = corvette_s1_io_map(CORVETTE_PCIE0_SM, CORVETTE_PCIE0_SM_SIZE);
	vm_offset_t crm = corvette_s1_io_map(CORVETTE_PCIE_CRM, CORVETTE_PCIE_CRM_SIZE);
	vm_offset_t parf = corvette_s1_io_map(CORVETTE_PCIE0_PARF, CORVETTE_PCIE0_PARF_SIZE);
	vm_offset_t phy = parf + CORVETTE_PCIE0_PHY_AT;
	vm_offset_t seq = corvette_s1_io_map(CORVETTE_RSC_PHYS, 0x10000);

	printf("wifi: s1: clean: putting back what this step changed, so that a stop and a new run start level\n");
	if (corvette_s1_pipe_src_changed) {
		kiwi_s1_write("pipe source: restore entry selector", CORVETTE_GCC_PIPE_SRC,
		    gcc + CORVETTE_GCC_PIPE_SRC, corvette_s1_pipe_src_original);
		corvette_s1_pipe_src_changed = false;
	}
	kiwi_s1_write("pcie_sm +0x1040: override withdrawn", CORVETTE_SM_PWR_CTRL, sm + CORVETTE_SM_PWR_CTRL, 0);
	kiwi_s1_write("pcie_sm +0x1048: its mask withdrawn", CORVETTE_SM_PWR_MASK, sm + CORVETTE_SM_PWR_MASK, 0);
	kiwi_s1_write("crm: channel 0 given back", CORVETTE_CRM_CHN_UPDATE, crm + CORVETTE_CRM_CHN_UPDATE, 0);
	{
		const struct kiwi_tcs *t = kiwi_s1_tcs_layout(seq);

		for (uint32_t tcs = 0; tcs < 2; tcs++) {
			vm_offset_t at = seq + 0xd00 + 672 * tcs;

			wr(seq + 0xd00 + 0x08, 1u << tcs);
			wr(at + t->control, 0);
			wr(at + t->enable, 0);
		}
		printf("wifi: s1:   tcs 0: control %#x, enabled %#x; tcs 1: control %#x, enabled %#x\n",
		    rd(seq + 0xd00 + t->control), rd(seq + 0xd00 + t->enable),
		    rd(seq + 0xd00 + 672 + t->control), rd(seq + 0xd00 + 672 + t->enable));
	}
	kiwi_s1_write("phy 0x240: powered, as it was found", 0x240, phy + 0x240, 3);
	kiwi_s1_write("phy 0x200: reset off, as it was found", 0x200, phy + 0x200, rd(phy + 0x200) & ~1u);

	/*
	 * And the core's own configuration, which is what a second run trips over. Once 5c/7 writes the DBI's
	 * base (0x60000000) and 5b/7 makes it a root complex, the core decodes that window for real; on the
	 * next boot, with the PHY never having come up, that window answers with an abort instead of the zero
	 * it answers on a clean machine -- which is a 900E at whichever point the run first touches it, well
	 * before any of the PCIe work. Taking the addresses back leaves the core as the boot loader left it:
	 * not decoding anything, so a read of 0x60000000 returns zero again.
	 */
	if (corvette_s2_iatu_saved_valid) {
		corvette_s2_iatu_restore(corvette_s1_io_map(CORVETTE_PCIE0_CORE, 0x10000) + 0x1000);
	}
	kiwi_s1_write("parf dbi base v2: given back", 0x350, parf + 0x350, 0);
	kiwi_s1_write("parf dbi base v2 hi", 0x354, parf + 0x354, 0);
	kiwi_s1_write("parf atu base: as it was", 0x634, parf + 0x634, 0x1000);
	kiwi_s1_write("parf atu base hi", 0x638, parf + 0x638, 0);
	kiwi_s1_write("parf slv size v2: as it was", 0x358, parf + 0x358, 0x1000000);
	kiwi_s1_write("parf slv size v2 hi", 0x35c, parf + 0x35c, 0);
	kiwi_s1_write("parf device type: nothing", CORVETTE_PARF_DEVICE_TYPE_OFF, parf + CORVETTE_PARF_DEVICE_TYPE_OFF,
	    0);
	kiwi_s1_write("parf mhi clock/reset: as it was", CORVETTE_PARF_MHI_CLOCK_RESET,
	    parf + CORVETTE_PARF_MHI_CLOCK_RESET, 3);
	kiwi_s1_write("parf ltssm: off", CORVETTE_PARF_LTSSM_OFFSET, parf + CORVETTE_PARF_LTSSM_OFFSET, 0);
	kiwi_s1_write("parf clkreq: its input back", 0x2b0, parf + 0x2b0, 0);
	printf("wifi: s1:   the core's windows are given back: dbi base %#x, atu base %#x, device type %#x\n",
	    rd(parf + 0x350), rd(parf + 0x634), rd(parf + CORVETTE_PARF_DEVICE_TYPE_OFF));
	printf("wifi: s1:   clean: the machine is as S1 found it\n");
}

static void
kiwi_s1(bool probe_iatu)
{
	const char *stopped = NULL;
	vm_offset_t gcc, tlmm, parf, core;

	printf("wifi: s1: corvette: PCIe0 on, one write at a time; each write prints what it overwrote and reads back\n");
	gcc = corvette_s1_io_map(CORVETTE_GCC_PHYS, CORVETTE_GCC_SIZE);
	tlmm = corvette_s1_io_map(CORVETTE_TLMM_PHYS, CORVETTE_TLMM_SIZE);
	parf = corvette_s1_io_map(CORVETTE_PCIE0_PARF, CORVETTE_PCIE0_PARF_SIZE);
	core = corvette_s1_io_map(CORVETTE_PCIE0_CORE, 0x10000);         /* (of it, only the ELBI at 0xf20 is read before L0) */

	/* CNSS powers the radio before msm_pcie_enable() starts the host controller. */
	printf("wifi: s1: CNSS order: hold PERST, enable radio rails and WLAN_EN, then initialize PCIe host\n");
	kiwi_s1_pins(tlmm);
	kiwi_s1_radio(parf, tlmm);

	kiwi_s1_clocks(gcc);                                    /* (its halt bits prove nothing yet: 2b/7 repeats them) */
	if (!kiwi_s1_gdsc("pcie_0_gdsc", gcc, CORVETTE_PCIE0_GDSCR, 0)) {
		stopped = "2/7 pcie_0_gdsc will not come up out of collapse";
	} else if (!kiwi_s1_gdsc("pcie_0_phy_gdsc", gcc, CORVETTE_PCIE0_PHY_GDSCR, 3)) {
		stopped = "2/7 pcie_0_phy_gdsc will not come up out of collapse";
	}
	if (stopped == NULL) {
		uint32_t source = rd(gcc + CORVETTE_GCC_PIPE_SRC);
		/*
		 * Air184: the selector is left where the boot firmware put it (PHY PIPE, 0).
		 *
		 * Air179 forced BI_TCXO here to answer one question -- is the core clocked at all -- and air180
		 * to air183 trained with the crystal, where a pipe interface does not exist at all. Air183
		 * settled it the other way: with the mux on PHY PIPE and the PHY's rails voted, all eight
		 * reference-clock combinations move the state to detect.active in 9 ms. So the crystal is no
		 * longer needed, and the link is trained the way the phone trains it.
		 */
		corvette_s1_pipe_src_original = source;
		corvette_s1_pipe_src_changed = false;
		printf("wifi: s1: 2/7 post-GDSC PIPE selector %#x (SM8650: 0=PHY PIPE, 2=BI_TCXO), left alone\n",
		    source & 3);
		kiwi_s1_clocks_again(gcc);
		/*
		 * Air187: the SerDes' lock bits, read before anything writes to the PHY.
		 *
		 * The stock phone reads 0xa00 = 0x13 and 0x1200 = 0x13 both while the link is carrying
		 * traffic and after it was suspended, so bits 0 and 1 there are not a product of L0: they
		 * are the reference clock / PLL lock. Every run of ours reads 0x10 with bit 0 and bit 1
		 * clear, which means the PHY's analogue side never started. This print decides which of the
		 * two it is: 0x13 here means the boot loader left a locked SerDes and the reset below is
		 * what loses it; 0x10 means the PHY never had a clock to lock to.
		 */
		{
			vm_offset_t early_phy = parf + CORVETTE_PCIE0_PHY_AT;
			static const uint32_t early[] = { 0xa00, 0x1200, 0x1d0, 0x1d4, 0x214, 0x1420 };

			printf("wifi: s1: 2b/7: the SerDes as the boot loader left it, before any PHY write:\n");
			for (unsigned i = 0; i < sizeof(early) / sizeof(early[0]); i++) {
				printf("wifi: s1:   +%#05x = %#x\n", early[i], rd(early_phy + early[i]));
			}
		}
		/* Match pci-msm.c: load CESTA and vote D0 before resetting core/PHY. The mainline path
		 * never loads or starts the sequencer (shennong runs the same silicon without it). */
		if (!hakios_wifi_option("pcie_mainline")) {
			kiwi_s1_state_machine();
			kiwi_s1_cesta_d0(gcc);
		}
		kiwi_s1_resets(gcc);
		if (!kiwi_s1_phy(parf)) {
			stopped = "5/7 the PHY's ready bit did not come up after its own sequence";
		} else if ((kiwi_s1_phy_deep(gcc, parf), kiwi_s1_rpmh(), kiwi_s1_rails(gcc, parf), kiwi_s1_pmic(),
		    !kiwi_s1_core(parf))) {
			stopped = "5b/7 the core does not take its own configuration (PARF writes do not stick)";
		} else {
			kiwi_s1_dbi(parf, core);        /* (5c/7: says whether the core answers, and sets its base addresses) */
			kiwi_s1_port(core);             /* (5d/7: the port's own configuration: lanes, bus, class) */
			kiwi_s1_power(parf, core, core + (CORVETTE_PCIE0_ELBI - CORVETTE_PCIE0_CORE));  /* (5f/7: what powers it) */
			if (hakios_wifi_option("pcie_mainline")) {
				if (!kiwi_s1_mainline_port(parf, core, tlmm, gcc)) {
					stopped = "6m/7 the mainline bring-up's link training did not reach L0";
				}
			} else if (hakios_wifi_option("pcie_vendor_port")) {
				if (!kiwi_s1_vendor_port(parf, core, tlmm, gcc)) {
					stopped = "6/7 the vendor port's link training did not reach L0";
				}
			} else if (!kiwi_s1_link(parf, core, tlmm)) {
				stopped = "6/7 link training did not reach L0";
			}
		}
	}
	if (stopped != NULL) {
		printf("wifi: s1: STOPPED at %s. The steps after it are not attempted, and nothing below the "
		    "controller was read\n", stopped);
		kiwi_s1_clean();
		return;
	}
	if (probe_iatu) {
		(void)kiwi_s2_iatu(core);
	} else if (hakios_wifi_option("pcie_mainline")) {
		/* the mainline port read the radio itself, through its CFG0 iATU region */
	} else {
		kiwi_s1_bus();
	}
	printf("wifi: s1: done: PCIe0 link reached L0\n");
	kiwi_s1_clean();
}

/*
 * Called while the system starts, before there is a root file system. With wifi_step=N the start
 * waits until the steps are over (see hakios_adreno_init()); with wifi=1 it does not.
 */
extern void shennong_wlan_init(void);   /* (shennong_wlan.c, selected by pcie_mainline=1) */

void
hakios_wifi_init(void)
{
	uint32_t on = 0, back = 0, done = 0, s0 = 0, s1 = 0, s2 = 0;

	/* air207b: with pcie_mainline=1 the whole bring-up and the radio driver are shennong's
	 * (shennong_wlan.c, the working Xiaomi 14 Pro port for this SoC and radio); our diagnostic
	 * flow stays behind for wifi_s1 without the flag. */
	if (hakios_wifi_option("pcie_mainline")) {
		/*
		 * (air222) the interface before the system starts: wifid opens the Wi-Fi interface once,
		 * at its own start, and a radio that turns up twelve seconds later is one it has
		 * already failed to open.  wifi_mac gives the address to make en0 with; the driver
		 * takes it over when the radio is up.
		 */
		hakios_lx_net_early();
		{ extern bool lx_os_radio_piece(unsigned long size);
		  /* air229: the firmware's own QMI ask is a 6.94 MB and an 8.06 MB piece, or, when
		   * those cannot be had, some 15.2 MB of half-megabyte chunks; the 48 MB pool (20 of
		   * rings, 28 of buffers) and the big pieces all come out of this reserve (air228:
		   * at 32 MB the second big piece failed and the chunks died at the 12 MB ceiling). */
		  if (!lx_os_radio_piece(96UL << 20)) {
			  if (!lx_os_radio_piece(48UL << 20)) {
				  lx_os_radio_piece(32UL << 20);
			  }
		  } }
		shennong_wlan_init();
		return;
	}
	uint64_t size = 0;
	thread_t thread;

	/* corvette: the first look at its radio, which shares nothing with the steps below */
	if (PE_parse_boot_argn("wifi_s0", &s0, sizeof(s0)) && s0) {
		kiwi_s0();
		return;
	}
	/* ... and the step after it: PCIe0 itself, powered and read back, with nothing of the radio's touched */
	if (PE_parse_boot_argn("wifi_s2", &s2, sizeof(s2)) && s2) {
		kiwi_s1(true);
		return;
	}
	if (PE_parse_boot_argn("wifi_s1", &s1, sizeof(s1)) && s1) {
		kiwi_s1(false);
		return;
	}
	PE_parse_boot_argn("wifi_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("wifi", &on, sizeof(on)) || on == 0)) {
		return;
	}
	if (last_step == 0 && on != 1) {
		printf("wifi: wifi=%u is not understood: wifi=1 turns the radio on, wifi_step=N goes the first N steps of that\n", on);
		return;
	}
	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		if (last_step != 0 && PE_parse_boot_argn("wifi_lx", &back, sizeof(back)) && back) {
			/* a check of what is under the driver: it gets as far as finding no firmware to talk to */
			printf("wifi: this is not the phone; with wifi_lx the driver is started all the same, for what it does before "
			    "it needs a radio\n");
			printf("wifi: the driver's start returns %d\n", hakios_lx_start());
		} else {
			printf("wifi: wifi=1 and wifi_step=N are for the phone's radio, and this is not the phone\n");
		}
		return;
	}
	hakios_qmi_set_transport(&qrtr_ops);
	if (last_step == 0) {
		if (image_file("wpss.mdt", &size) == NULL) {
			printf("wifi: wifi=1, but the radio's firmware is not in the small image (fw-wifi/, see the README): no Wi-Fi\n");
			return;
		}
		whole = whole_runs = true;
		quiet = !(PE_parse_boot_argn("wifi_loud", &back, sizeof(back)) && back);
		hakios_lx_net_early();
		pause_ms = watch_seconds = 0;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			whole_runs = false;
			tell("wifi: no thread: the radio is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("wifi: the radio is being brought up beside the system's start (wifi=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	new_from = last_step;
	PE_parse_boot_argn("wifi_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("wifi_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("wifi_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("wifi_watch", &watch_seconds, sizeof(watch_seconds));
	PE_parse_boot_argn("wifi_dirty", &dirty, sizeof(dirty));

	printf("wifi: steps up to %u; those from %u on are new and wait %u ms %s\n", last_step, new_from, pause_ms,
	    fine ? "before every write and call" : "after each announcement");
	if ((radio_space.wanted = hakios_wifi_option("wifi_smmu")) != 0) {
		smmu_look();
		if (radio_space.wanted == 2 && last_step < 15) {
			uint32_t known_from = new_from;

			new_from = 0;                   /* (this is new: it waits as a new step does) */
			radio_space_try();
			new_from = known_from;
		}
	}
	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("wifi: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("wifi: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;                   /* (what follows has been announced) */
	steps_over(done);
	printf("wifi: over after step %u; %s\n", done, last_write);
	back = 0;
	if (PE_parse_boot_argn("wifi_return", &back, sizeof(back)) && back) {
		printf("wifi: back to the loader (wifi_return)\n");
		loader_return();
	}
	printf("wifi: the system goes on starting\n");
}

/*
 * What interrupts would take, instead of looking. The buttons want the same (the PMIC arbiter's
 * line, see the end of hakios.buttons.c), and the radio will: so the general case first.
 *
 * The general case is one shared peripheral interrupt (an "SPI": the interrupt controller's
 * number for it is 32 more than the device tree's) with a handler. osfmk/arm64/gic_v3_irq.c
 * drives the processors' interfaces, takes the timer's interrupt and the two that processors
 * send each other, and masks and counts anything else that fires. What it lacks:
 *   - a table from interrupt number to handler and argument, and a call with which a driver
 *     fills an entry (number, level or edge, handler, argument); gic_irq_handler() calls the
 *     handler where it now masks, between taking the interrupt (ICC_IAR1_EL1) and ending it
 *     (ICC_EOIR1_EL1);
 *   - for that number, in the distributor (it is mapped; only its control register and the
 *     "disable" registers are written today): group 1 like everything here (GICD_IGROUPR), the
 *     trigger (GICD_ICFGR), a priority (GICD_IPRIORITYR), the processor it goes to
 *     (GICD_IROUTER: the boot processor), and enabled (GICD_ISENABLER); and the reverse when
 *     the driver lets go and before the kernel returns to the loader, which must find the
 *     lines as it left them.
 *   Whether the hypervisor passes this kernel's writes to the distributor for these lines is
 *   not known: none has been made. A handler runs with interrupts off on whatever thread was
 *   interrupted; it should quiet its device and wake a thread, no more.
 *
 * Ours, given that:
 *   - The doorbells have one line for everything rung at the application processor: SPI 229,
 *     level, high (dt: qcom,ipcc@408000). What IPCC is seen to do: it keeps one mark per
 *     doorbell (a client and a signal), not a count -- 46 GLINK messages from the DSP were five
 *     rings heard. A doorbell is recorded only if IPCC has been told that it is listened for
 *     (client << 16 | signal written to IPCC_ENABLE): the boot chain leaves the DSP's GLINK
 *     doorbell so and no other, and the DSP's SMP2P rings left no trace until step 11 enabled
 *     them. IPCC_RECEIVED names one recorded doorbell at a time, the same one until that word
 *     is written to IPCC_CLEAR, then the next, then "nobody". So a handler reads IPCC_RECEIVED
 *     until it says nobody, writes each word it gets to IPCC_CLEAR, and passes it on (this is
 *     doorbells() above, called from a loop today): client 0 signal 0 is the always-on
 *     processor's mailbox (every wait in mailbox_wait()), client 24 signal 2 the Wi-Fi DSP's
 *     SMP2P item (the watch of step 11, the wait in dsp_ask_to_stop()), client 24 signal 0 its
 *     GLINK. Since a mark is not a count, whoever is woken looks until there is nothing left:
 *     the link's thread does. With that, link_wait() is the one function to change.
 *   - The DSP's watchdog has a line of its own (SPI 587), raised when its firmware hangs
 *     without managing to say so in the shared memory.
 *   - The radio itself, later, raises 32 more (SPI 768 to 799, edge), by writing their numbers
 *     to a register of the interrupt controller (dt: "msi_addr" of qcom,wcn6750): its copy
 *     engines and the rings of the data path. Those can be looked at in turn as well, at the
 *     price of a thread that never sleeps while the link is up.
 */
