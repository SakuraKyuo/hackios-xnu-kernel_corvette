/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

#define PAYLOAD_BASE        0xA8000000ULL       /* "Kernel" reserved region of the device memory map */
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x05000000ULL         /* RAM reserved for the payload in the UEFI memory map */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			return bio;
		}
	}
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
static char out_buf[2 * 1024 * 1024];
/* the storage's reads for the kernel land here (block_poll); also where the log is read back */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

/*
 * The stored copy is what survives a reset. A boot that dies in a store (the
 * phone resets, USB never answers again) leaves the lines up to that store in
 * the logdump partition, and the next boot puts them back at the top of the
 * log, so "the log dies with the reset" stops being a blind spot.
 */
#define LOG_DISK_MAGIC   "XNULOADER-DISK1\n"   /* these 16 bytes, exactly */
#define LOG_DISK_HEAD    20u                    /* then a u32: what follows */
#define LOG_PREV_KEEP    (16u * 1024)           /* newest bytes put back, at most */
#define LOG_PREV_READ    (56u * 1024)           /* read at boot, at most */

static int log_quiet;                           /* a flush made before a risky store */

/* What goes to the partition: header, this session's log, the kernel log. */
static void
compose_disk(const char *klog, uint64_t klog_len)
{
	uint64_t n = LOG_DISK_HEAD;

	if (log_len > sizeof(out_buf) - LOG_DISK_HEAD - 8192) {
		klog_len = 0;
	}
	memset(out_buf, 0, LOG_DISK_HEAD);
	memcpy(out_buf, LOG_DISK_MAGIC, 16);
	memcpy(out_buf + n, log_buf, log_len);
	n += log_len;
	if (klog != NULL && klog_len != 0) {
		if (klog_len > sizeof(out_buf) - n - 1) {
			klog_len = sizeof(out_buf) - n - 1;
		}
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
	}
	*(uint32_t *)(out_buf + 16) = (uint32_t)(n - LOG_DISK_HEAD);
	staged_len = n;
}

/*
 * Before the banner, so the restored lines come first and this session follows
 * them. Only the newest bytes are kept: what killed a boot is at the end.
 */
static void
restore_prev(void)
{
	static const char head[] = "== log stored before this boot ==\n";
	efi_block_io *bio = find_log_partition();
	uint64_t bs, take, from, room, n = sizeof(head) - 1;
	uint32_t stored;

	if (bio == NULL || bio->media == NULL || log_len != 0) {
		return;
	}
	bs = bio->media->block_size;
	if (bs == 0 || LOG_PREV_READ > sizeof(block_buffer)) {
		return;
	}
	take = LOG_PREV_READ / bs * bs;
	if (take <= LOG_DISK_HEAD) {
		return;
	}
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, take, block_buffer))) {
		return;
	}
	if (memcmp(block_buffer, LOG_DISK_MAGIC, 16) != 0) {
		return;
	}
	stored = *(uint32_t *)(block_buffer + 16);
	if (stored == 0 || stored > take - LOG_DISK_HEAD) {
		return;
	}
	from = stored > LOG_PREV_KEEP ? stored - LOG_PREV_KEEP : 0;
	stored -= (uint32_t)from;
	room = sizeof(log_buf) - n - 4096;
	if (stored > room) {
		from += stored - room;
		stored = (uint32_t)room;
	}
	if (stored == 0) {
		return;
	}
	memcpy(log_buf, head, n);
	memcpy(log_buf + n, block_buffer + LOG_DISK_HEAD + from, stored);
	log_len = n + stored;
	log_buf[log_len] = 0;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

static void
write_logs(const char *klog, uint64_t klog_len)
{
	efi_block_io *bio = find_log_partition();

	compose_disk(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len; i++) {
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(block_buffer)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, block_buffer);
			same = !EFI_ERROR(rs) && memcmp(block_buffer, out_buf, len) == 0;
		}
	}
	if (!log_quiet) {
		print("log: %d bytes written to logdump (%x), read back %s (%x)\n", len, ws,
		    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	}
	compose_logs(klog, klog_len);
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it iOS takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t gic_redist_size;        /* bytes of redistributor frames: one frame per core */
	uint64_t gic_redist_stride;      /* bytes between frames: 0x20000, or 0x40000 on some SoCs */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .gic_redist_size = 0x100000, .gic_redist_stride = 0x20000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .gic_redist_size = 0x100000, .gic_redist_stride = 0x20000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
};

static const struct platform platform_corvette = {
	/* OnePlus Ace 3 Pro, SM8650 (Pineapple). Core numbers and the GIC come from its device tree. */
	.name = "corvette", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17100000, .gic_redist = 0x17180000, .gic_redist_size = 0x200000, .gic_redist_stride = 0x40000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-1 are Cortex-A520s (capacity 0x400 in the device tree), 2-6 Cortex-A720s (0x700) and
	 * 7 a Cortex-X4 (0x766).
	 */
	.small_cores = 0x3,
	.fb_base = 0xD5100000,          /* the start of the vendor's display reservation; to confirm from the log */
};

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here; a display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_corvette,
};

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
};

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */

static void wdt_pet(void);
static int wdt_armed;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room = sizeof(out_buf) - 8192;

	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * bsd/dev/hakios.ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (bsd/dev/hakios.ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
#define BLOCK_DARWIN_FIRST 25374720ULL                 /* its first and last LBA of unit 0, as the GPT has them */
#define BLOCK_DARWIN_LAST  30617594ULL
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }
#define BLOCK_DARWIN_UNIQ  { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } }

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in this phone's Android device tree */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE, uniq = BLOCK_DARWIN_UNIQ;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &uniq, sizeof(uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (info->start_lba != BLOCK_DARWIN_FIRST || info->end_lba != BLOCK_DARWIN_LAST) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != BLOCK_DARWIN_LAST - BLOCK_DARWIN_FIRST + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, BLOCK_WRITE_FIRST);
	return 1;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information: the block service is off\n");
		return;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = BLOCK_WRITE_FIRST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE) {
			if (block_write_part < 0 || part != (uint32_t)block_write_part || lba < BLOCK_WRITE_FIRST) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || PLAT == &platform_qemu_virt) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
			group1 = READ_SYSREG(S3_0_C12_C12_7);          /* ICC_IGRPEN1_EL1 */
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		usb_poll_command();
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;

	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT == &platform_qemu_virt) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		resident_rd_size = pi.resident_rd;
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (bsd/dev/hakios.torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is iOS's (bsd/dev/hakios.camera.c) */
	int camera_requested = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '2' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '9';
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	for (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes iOS play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it iOS looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = (int)(kernel_mpidr >> plat->core_shift);

	if (more_cores) {
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr = (uint64_t)core << plat->core_shift;
				int64_t state;

				if (core < 0 || core >= plat->cores || mpidr == this_mpidr) {
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (plat->small_cores & (1u << ((cpu_mpidr[i] >> plat->core_shift) & 31))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, plat->gic_redist_size };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_prop_u32(&w, "redistributor-stride", (uint32_t)plat->gic_redist_stride);
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into darwin, %d bytes, read back the same; %d refused\n", block_writes,
		    block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}


/* ------------------------------------------------------------------------ */
/* PCIe0 to the WCN7850 (corvette): the link, brought up here              */
/*                                                                          */
/* The kernel's own S1 writes every register the vendor's                    */
/* msm_pcie_enable_link() writes, and the link still does not leave          */
/* detect.quiet while PHYSTATUS stays set. The one port of this radio to    */
/* the same SoC family that is known to work (ios-dodge,                    */
/* op13_wifi_link_probe) does the bring-up in the firmware, before any      */
/* operating system has taken the clocks and the power domains over. That   */
/* is what this is: the same sequence, run from here.                       */
/*                                                                          */
/*   oem pcie-state   only reads: what the boot loader left, and whether    */
/*                    this address space answers from UEFI at all.          */
/*   oem pcie-link    writes, in the vendor's own order. Every store is     */
/*                    printed on the line before it is made, and read back  */
/*                    on the same line, so a fault names its register.      */
/*                                                                          */
/* The radio is on PCIe0 (dt: qcom,pcie@1c00000): parf 0x01c00000 (the PHY  */
/* 0x2000 after its 0x3000), dm_core 0x60000000 (its config space, with the */
/* ELBI at 0xf20 and the iATU at 0x1000), ECAM 0x60100000 (a megabyte to    */
/* the bus) and the identity MEM32 aperture 0x60300000.                    */
/* ------------------------------------------------------------------------ */

#define PC_GCC_PHYS       0x00100000UL
#define PC_GCC_VOTE       0x52008
#define PC_GCC_VOTE_RCH   0x52000
#define PC_GCC_COLLAPSE   0x5214c
#define PC_GCC_GDSCR      0x6b004
#define PC_GCC_PHY_GDSCR  0x6c000
#define PC_GCC_BCR        0x6b000
#define PC_GCC_PHY_BCR    0x6c01c
#define PC_PARF_PHYS      0x01c00000UL
#define PC_PHY_AT         0x6000
#define PC_CORE_PHYS      0x60000000UL
#define PC_ELBI_AT        0xf20
#define PC_ECAM_PHYS      0x60100000UL
#define PC_MEM32_PHYS     0x60300000UL
#define PC_MEM32_SIZE     0x03d00000UL
#define PC_TCSR_PHYS      0x1f40000UL
#define PC_TLMM_PHYS      0x0f100000UL
#define PC_RSC_PHYS       0x17a20000UL
#define PC_SM_PHYS        0x1d07000UL

#define PC_GDSC_ON        (1u << 31)
#define PC_GDSC_UP        (1u << 16)      /* in CFG_GDSCR, four bytes after the domain's own register */
#define PC_LTSSM_EN       (1u << 8)

/* The always-on processor's mailbox (power-controller@c300000) and the IPCC
 * send register that rings it; see pc_radio_pdc() far below. */
#define PC_QMP_PHYS    0xc300000ULL
#define PC_IPCC_PHYS   0x406000ULL
#define PC_IPCC_SEND   (PC_IPCC_PHYS + 0xc)
#define PC_QMP_MAGIC   0x4d41494cu
#define PC_QMP_UP      0xffffu
#define PC_QMP_MSG_LEN 64u

static inline uint32_t
pc_rd(uint64_t pa)
{
	uint32_t v;

	__asm__ volatile ("dsb sy" ::: "memory");
	v = *(volatile uint32_t *)(uintptr_t)pa;
	__asm__ volatile ("dsb sy" ::: "memory");
	return v;
}

static inline void
pc_wr(uint64_t pa, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)pa = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
pc_ms(uint64_t ms)
{
	BS->stall(ms * 1000);
}

/* A store, what it overwrote, and what the register says the moment after. */
static void pc_flush(void);

static void
pc_set(const char *what, uint32_t off, uint64_t pa, uint32_t value)
{
	uint32_t before = pc_rd(pa);

	print("pcie:   %s +%x: %x -> %x", what, (uint64_t)off, (uint64_t)before, (uint64_t)value);
	pc_flush();
	pc_wr(pa, value);
	print(", %x back\n", (uint64_t)pc_rd(pa));
}

static const char *
pc_ltssm_name(uint32_t state)
{
	switch (state) {
	case 0x00: return "detect.quiet";
	case 0x01: return "detect.active";
	case 0x02: return "polling.active";
	case 0x04: return "polling.configuration";
	case 0x06: return "detect.wait";
	case 0x07: return "configuration.linkwidth.start";
	case 0x0b: return "configuration.complete";
	case 0x0c: return "configuration.idle";
	case 0x0d: return "recovery.lock";
	case 0x10: return "recovery.idle";
	case 0x11: return "L0";
	case 0x13: return "sending electrical idle";
	case 0x14: return "L1.idle";
	case 0x16: return "L2.idle";
	case 0x1a: return "disabled";
	default: return "unnamed";
	}
}

/* The pins this link uses, from the phone's own device tree: wlan-en (16),
 * perst (94) and clkreq# (95). Perst is held low until the link is about to
 * be started; clkreq# carries pcie0_clk_req_n (function 1), not a GPIO. */
#define PC_PIN_WLAN_EN   16
#define PC_PIN_PERST     94
#define PC_PIN_CLKREQ    95
#define PC_PIN_WAKE      96
/* pinctrl-pineapple's control word: pull is bits 1:0 (3 is pull-up), the mux
 * is bits 5:2, drive is 8:6 and output enable is 9; each pin's function table
 * is { gpio, f1, f2, ... } by index, so pin 95's pcie0_clk_req_n is mux 1. */
#define PC_PIN_PULL_UP   3u
#define PC_PIN_MUX(n)    ((uint32_t)(n) << 2)
#define PC_PIN_IN_PULLUP PC_PIN_PULL_UP
#define PC_PIN_OUT_8MA   0x2c0
#define PC_PIN_CLKREQ_N  (PC_PIN_MUX(1) | PC_PIN_PULL_UP)

static void
pc_dump(const char *when)
{
	uint64_t gcc = PC_GCC_PHYS, tlmm = PC_TLMM_PHYS;
	uint32_t core, phy;
	static const struct {
		const char *name;
		uint32_t at;
	} halt[] = { { "slv_q2a", 0x6b01c }, { "slv_axi", 0x6b020 }, { "mstr_axi", 0x6b02c },
		     { "cfg_ahb", 0x6b038 }, { "aux", 0x6b03c }, { "pipe", 0x6b048 },
		     { "phy_rchng", 0x6b054 }, { "cnoc", 0x10058 } };

	print("pcie: %s: gcc %x: votes %x, %x; collapse %x\n", when, gcc,
	    (uint64_t)pc_rd(gcc + PC_GCC_VOTE), (uint64_t)pc_rd(gcc + PC_GCC_VOTE_RCH),
	    (uint64_t)pc_rd(gcc + PC_GCC_COLLAPSE));
	print("pcie:   halted (1 = not running):");
	for (unsigned i = 0; i < sizeof(halt) / sizeof(halt[0]); i++) {
		print(" %s %d", halt[i].name, (uint64_t)(pc_rd(gcc + halt[i].at) & 1));
	}
	print("\n");
	core = pc_rd(gcc + PC_GCC_GDSCR);
	phy = pc_rd(gcc + PC_GCC_PHY_GDSCR);
	print("pcie:   gdsc core %x (on %d, up %d), phy %x (on %d, up %d)\n", (uint64_t)core,
	    (uint64_t)((core >> 31) & 1), (uint64_t)((pc_rd(gcc + PC_GCC_GDSCR + 4) >> 16) & 1), (uint64_t)phy,
	    (uint64_t)((phy >> 31) & 1), (uint64_t)((pc_rd(gcc + PC_GCC_PHY_GDSCR + 4) >> 16) & 1));
	print("pcie:   resets core %x, phy %x, com %x %x %x, block %x %x; clkref %x %x\n",
	    (uint64_t)pc_rd(gcc + PC_GCC_BCR), (uint64_t)pc_rd(gcc + PC_GCC_PHY_BCR),
	    (uint64_t)pc_rd(gcc + 0x6c014), (uint64_t)pc_rd(gcc + 0x6c020), (uint64_t)pc_rd(gcc + 0x6c028),
	    (uint64_t)pc_rd(gcc + 0x6f000), (uint64_t)pc_rd(gcc + 0x6f00c),
	    (uint64_t)pc_rd(PC_TCSR_PHYS + 0xb1100), (uint64_t)pc_rd(PC_TCSR_PHYS + 0xb1114));
	print("pcie:   pins: 16 %x/%x, 17 %x/%x, 18 %x/%x, 94 %x/%x, 95 %x/%x\n",
	    (uint64_t)pc_rd(tlmm + 0x1000 * PC_PIN_WLAN_EN), (uint64_t)pc_rd(tlmm + 0x1000 * PC_PIN_WLAN_EN + 4),
	    (uint64_t)pc_rd(tlmm + 0x11000), (uint64_t)pc_rd(tlmm + 0x11004),
	    (uint64_t)pc_rd(tlmm + 0x12000), (uint64_t)pc_rd(tlmm + 0x12004),
	    (uint64_t)pc_rd(tlmm + 0x1000 * PC_PIN_PERST), (uint64_t)pc_rd(tlmm + 0x1000 * PC_PIN_PERST + 4),
	    (uint64_t)pc_rd(tlmm + 0x1000 * PC_PIN_CLKREQ), (uint64_t)pc_rd(tlmm + 0x1000 * PC_PIN_CLKREQ + 4));
	/* Below the wrapper nothing answers until its power domain is up, so
	 * these are read only when the domain itself says it is on. */
	if (!(core & PC_GDSC_ON)) {
		print("pcie:   the core's power domain is off: parf, the core and the phy are not read\n");
		return;
	}
	print("pcie:   parf %x: ltssm %x (state %d %s), refclk %x, device type %x\n", PC_PARF_PHYS,
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x1b0), (uint64_t)(pc_rd(PC_PARF_PHYS + 0x1b0) & 0x3f),
	    pc_ltssm_name(pc_rd(PC_PARF_PHYS + 0x1b0) & 0x3f), (uint64_t)pc_rd(PC_PARF_PHYS + 0x4c),
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x1000));
	print("pcie:   elbi %x: %x %x %x %x; core id %x, class %x\n", PC_CORE_PHYS + PC_ELBI_AT,
	    (uint64_t)pc_rd(PC_CORE_PHYS + PC_ELBI_AT), (uint64_t)pc_rd(PC_CORE_PHYS + PC_ELBI_AT + 4),
	    (uint64_t)pc_rd(PC_CORE_PHYS + PC_ELBI_AT + 8), (uint64_t)pc_rd(PC_CORE_PHYS + PC_ELBI_AT + 0xc),
	    (uint64_t)pc_rd(PC_CORE_PHYS), (uint64_t)pc_rd(PC_CORE_PHYS + 8));
	if (phy & PC_GDSC_ON) {
		print("pcie:   phy %x: status %x (bit 6 %d, 1 = not ready), power down %x, start %x\n",
		    PC_PARF_PHYS + PC_PHY_AT, (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x214),
		    (uint64_t)((pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x214) >> 6) & 1),
		    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x240),
		    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x244));
	} else {
		print("pcie:   the phy's power domain is off: its registers are not read\n");
	}
}

/* 1: the seven branch clocks of PCIe0, in two votes (Linux's gcc-sm8650.c). */
static void
pc_clocks(void)
{
	uint64_t gcc = PC_GCC_PHYS;
	uint32_t vote = pc_rd(gcc + PC_GCC_VOTE), rchng = pc_rd(gcc + PC_GCC_VOTE_RCH);

	print("pcie: 1: the branch clocks of PCIe0, at gcc %x\n", gcc);
	pc_set("vote (7 branches)", PC_GCC_VOTE, gcc + PC_GCC_VOTE, vote | 0x7f);
	pc_set("vote (phy_rchng)", PC_GCC_VOTE_RCH, gcc + PC_GCC_VOTE_RCH, rchng | (1u << 22));
	pc_ms(2);
}

/* 2: one power domain, out of collapse, with its power-up handshake waited for.
 * The handshake is bit 16 of CFG_GDSCR, four bytes after the domain's own
 * register; the domain's bit 31 is the state and bit 0 the software collapse. */
static int
pc_gdsc(const char *name, uint32_t gdscr, uint32_t collapse_bit)
{
	uint64_t at = PC_GCC_PHYS + gdscr;
	uint32_t value = pc_rd(at), cfg = pc_rd(at + 4), polls, collapse;

	print("pcie: 2: %s +%x: %x (on %d, collapse %d), cfg %x (up %d)\n", name, (uint64_t)gdscr,
	    (uint64_t)value, (uint64_t)((value >> 31) & 1), (uint64_t)(value & 1), (uint64_t)cfg,
	    (uint64_t)((cfg >> 16) & 1));
	if ((cfg & PC_GDSC_UP) && (value & PC_GDSC_ON)) {
		print("pcie:   %s is already up\n", name);
		return 1;
	}
	collapse = pc_rd(PC_GCC_PHYS + PC_GCC_COLLAPSE);
	if (collapse & (1u << collapse_bit)) {
		pc_set("collapse: cleared", PC_GCC_COLLAPSE, PC_GCC_PHYS + PC_GCC_COLLAPSE,
		    collapse & ~(1u << collapse_bit));
	}
	/* From the vendor tree (gdsc-regulator.c), a domain that has a
	 * collapse vote is enabled by that vote alone: SW_COLLAPSE is never
	 * written for it, so it is not written here either. */
	for (polls = 0; polls < 100; polls++) {
		value = pc_rd(at);
		cfg = pc_rd(at + 4);
		if ((cfg & PC_GDSC_UP) && (value & PC_GDSC_ON)) {
			break;
		}
		pc_ms(1);
	}
	print("pcie:   %s after %d ms: %x (on %d), cfg %x (up %d)\n", name, (uint64_t)polls, (uint64_t)value,
	    (uint64_t)((value >> 31) & 1), (uint64_t)cfg, (uint64_t)((cfg >> 16) & 1));
	return (cfg & PC_GDSC_UP) && (value & PC_GDSC_ON);
}


static void pc_radio(void);

/* Put the log on the partition before a store that may reset the phone. */
static void
pc_flush(void)
{
	log_quiet++;
	flush_log();
	log_quiet--;
}

/* ------------------------------------------------------------------------ */
/* The firmware's page tables, for the windows below the PCIe wrapper       */
/*
 * The loader runs at EL1 (the banner's CurrentEL=1), in the translation
 * regime the firmware left on (TTBR0_EL1, 4 KB granules), and that map holds
 * the pages the firmware itself used, not the device tree's. Reading 0x1c00000
 * answered and reading
 * 0x1c01000 raised a level-3 translation fault (esr 96000007, far 1c01000),
 * so each window below the wrapper has to be filled in before it is read.
 *
 * The walk is the hardware's: L0 to L2 must already be tables, and only a
 * missing L3 entry is added. The attribute bits are copied from a mapping
 * that answers today (0x100000, the GCC), because the MAIR_EL2 index of a
 * device region is the firmware's choice, not ours.
 */

#define PC_TT_POOL 8

static uint64_t pc_tt_pool[PC_TT_POOL][512] __attribute__((aligned(0x1000)));
static unsigned pc_tt_used;

static uint64_t
pc_tt_base(void)
{
	return READ_SYSREG(TTBR0_EL1) & 0x0000fffffffff000ULL;
}

/* TCR_EL1.T0SZ gives 64 - T0SZ bits of virtual address, and with 4 KB
 * granules a region of more than 39 bits starts the walk at level 0, more
 * than 30 at level 1, and anything smaller at level 2. */
static int
pc_tt_start(void)
{
	int bits = 64 - (int)(READ_SYSREG(TCR_EL1) & 0x3f);

	return bits > 39 ? 0 : bits > 30 ? 1 : 2;
}

/* Walk to the level-3 entry that maps va, adding a table at any level whose
 * entry is still invalid. With *block: 1 = a block already maps va, 2 = the
 * pool is out of tables. */
static uint64_t *
pc_tt_l3(uint64_t va, int *block)
{
	uint64_t table = pc_tt_base();
	int start = pc_tt_start();

	*block = 0;
	for (int level = start; level <= 2; level++) {
		uint64_t *entry = (uint64_t *)(uintptr_t)
		    (table + (((va >> (12 + 9 * (3 - level))) & 0x1ff) * 8));
		uint64_t d = *entry;

		if (!(d & 1)) {
			if (pc_tt_used >= PC_TT_POOL) {
				*block = 2;
				return NULL;
			}
			table = (uint64_t)(uintptr_t)pc_tt_pool[pc_tt_used++];
			*entry = table | 3;
			clean_dcache((uint64_t)(uintptr_t)entry, (uint64_t)(uintptr_t)entry + 8);
			continue;
		}
		if (!(d & 2)) {
			*block = 1;
			return NULL;
		}
		table = d & 0x0000fffffffff000ULL;
	}
	return (uint64_t *)(uintptr_t)(table + (((va >> 12) & 0x1ff) * 8));
}

/* The descriptor that maps va today, or 0: the attribute template. */
static uint64_t
pc_tt_desc(uint64_t va)
{
	uint64_t table = pc_tt_base();
	int start = pc_tt_start();

	for (int level = start; level <= 2; level++) {
		uint64_t d = *(volatile uint64_t *)(uintptr_t)
		    (table + (((va >> (12 + 9 * (3 - level))) & 0x1ff) * 8));

		if (!(d & 1)) {
			return 0;
		}
		if (!(d & 2)) {
			return d;
		}
		table = d & 0x0000fffffffff000ULL;
	}
	return *(volatile uint64_t *)(uintptr_t)(table + (((va >> 12) & 0x1ff) * 8));
}

/* 1 = the page answers now, either because it already did or because the
 * entry was added; an existing entry is never rewritten. */
static int
pc_map_page(uint64_t pa, uint32_t attrs, uint32_t *added, uint32_t *present)
{
	int block = 0;
	uint64_t *entry = pc_tt_l3(pa, &block);

	if (entry == NULL) {
		return block == 1;
	}
	if (*entry & 1) {
		(*present)++;
		return 1;
	}
	*entry = (pa & 0x0000fffffffff000ULL) | (uint64_t)(attrs & 0xffc) | 3;
	clean_dcache((uint64_t)(uintptr_t)entry, (uint64_t)(uintptr_t)entry + 8);
	(*added)++;
	return 1;
}

static void
pc_map_windows(void)
{
	static const struct {
		const char *name;
		uint64_t base, len;
	} win[] = {
		{ "parf+phy", PC_PARF_PHYS, 0x8000 },
		{ "core elbi+atu", PC_CORE_PHYS, 0x2000 },
		{ "ecam", PC_ECAM_PHYS, 0x100000 },
		{ "mem32", PC_MEM32_PHYS, 0x10000 },
		{ "pcie sm", PC_SM_PHYS, 0x2000 },
		{ "cmd db", 0x81c60000ULL, 0x20000 },
		{ "spmi arbiter", 0x0c400000ULL, 0x90000 },
		{ "aop qmp", PC_QMP_PHYS, 0x1000 },
		{ "ipcc", PC_IPCC_PHYS, 0x1000 },
	};
	uint64_t tpl = pc_tt_desc(PC_GCC_PHYS);
	uint32_t attrs = (uint32_t)(tpl & 0xfff);

	print("pcie: map: ttbr0_el1 %x tcr_el1 %x, start level %d, template %x from %x\n",
	    pc_tt_base(), READ_SYSREG(TCR_EL1), (uint64_t)pc_tt_start(), (uint64_t)attrs, PC_GCC_PHYS);
	if ((tpl & 1) == 0) {
		print("pcie: map: %x is not mapped either: nothing is added\n", PC_GCC_PHYS);
		return;
	}
	for (unsigned w = 0; w < sizeof(win) / sizeof(win[0]); w++) {
		uint32_t added = 0, present = 0, failed = 0;

		for (uint64_t pa = win[w].base; pa < win[w].base + win[w].len; pa += 0x1000) {
			if (!pc_map_page(pa, attrs, &added, &present)) {
				failed++;
			}
		}
		print("pcie: map %s %x +%x: %d added, %d already, %d failed\n", win[w].name,
		    win[w].base, win[w].len, (uint64_t)added, (uint64_t)present, (uint64_t)failed);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	__asm__ volatile ("tlbi vmalle1" ::: "memory");
	__asm__ volatile ("dsb sy; isb" ::: "memory");
	/* the page that faulted last time is the proof; the log is on the
	 * partition before the read, so a fault here still names it */
	print("pcie: map: parf +1000 reads %x, +1b0 reads %x\n",
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x1000), (uint64_t)pc_rd(PC_PARF_PHYS + 0x1b0));
	pc_flush();
}

/* The vote bit is the enable for these domains. From the vendor tree:
 * pineapple-gdsc.dtsi puts gcc_pcie_0_gdsc on vote bit 0 and gcc_pcie_0_phy_gdsc
 * on bit 3 of gcc_apcs_gdsc_vote_ctrl (0x15214c), and gdsc-regulator.c clears
 * the bit to enable and never writes SW_COLLAPSE for a domain that has a
 * collapse vote. The store is put on the partition first, so a reset here
 * still says whether the register read back. */
static int
pc_gdsc_vote(const char *name, uint32_t gdscr, uint32_t bit, int enable)
{
	uint64_t gcc = PC_GCC_PHYS, at = gcc + gdscr;
	uint32_t vote = pc_rd(gcc + PC_GCC_COLLAPSE), want, value, cfg, polls;

	want = enable ? (vote & ~(1u << bit)) : (vote | (1u << bit));
	print("pcie: vote %s (bit %d): %x -> %x\n", name, (uint64_t)bit, (uint64_t)vote,
	    (uint64_t)want);
	pc_flush();
	pc_wr(gcc + PC_GCC_COLLAPSE, want);
	print("pcie:   stored, %x back\n", (uint64_t)pc_rd(gcc + PC_GCC_COLLAPSE));
	for (polls = 0; polls < 100; polls++) {
		value = pc_rd(at);
		cfg = pc_rd(at + 4);
		if ((cfg & PC_GDSC_UP) && (value & PC_GDSC_ON)) {
			break;
		}
		pc_ms(1);
	}
	print("pcie:   %s after %d ms: %x (on %d), cfg %x (up %d)\n", name, (uint64_t)polls,
	    (uint64_t)value, (uint64_t)((value >> 31) & 1), (uint64_t)cfg,
	    (uint64_t)((cfg >> 16) & 1));
	return (cfg & PC_GDSC_UP) && (value & PC_GDSC_ON);
}

/* The core's power domain one write at a time: stage 3 reset the phone, and this
 * says which of its two writes does it. 1 clears only the collapse bit, 2 only
 * SW_COLLAPSE, 3 both (the stage-3 order), 4 all five collapse bits and then
 * SW_COLLAPSE, 8 writes the same value back (is any store fatal), 5 clocks and
 * the phy's vote, 6 clocks and the core's vote, 7 rails, clocks, phy, core. */
static void
pc_gdsc_try(uint32_t what)
{
	uint64_t gcc = PC_GCC_PHYS, at = PC_GCC_PHYS + PC_GCC_GDSCR;
	uint32_t value = pc_rd(at), collapse = pc_rd(gcc + PC_GCC_COLLAPSE);

	print("\npcie: gdsc try %d: gdscr %x (on %d, collapse %d), collapse reg %x\n",
	    (uint64_t)what, (uint64_t)value, (uint64_t)((value >> 31) & 1), (uint64_t)(value & 1),
	    (uint64_t)collapse);
	if (what == 8) {
		print("pcie:   control: the same value, written back\n");
		pc_flush();
		pc_wr(gcc + PC_GCC_COLLAPSE, collapse);
		print("pcie:   stored, %x back\n", (uint64_t)pc_rd(gcc + PC_GCC_COLLAPSE));
		pc_dump("vote written");
		return;
	}
	if (what == 5) {
		pc_clocks();
		pc_gdsc_vote("pcie_0_phy_gdsc", PC_GCC_PHY_GDSCR, 3, 1);
		pc_dump("gdsc try");
		return;
	}
	if (what == 6) {
		pc_clocks();
		pc_gdsc_vote("pcie_0_gdsc", PC_GCC_GDSCR, 0, 1);
		pc_dump("gdsc try");
		return;
	}
	if (what == 7) {
		pc_radio();
		pc_clocks();
		pc_gdsc_vote("pcie_0_phy_gdsc", PC_GCC_PHY_GDSCR, 3, 1);
		pc_gdsc_vote("pcie_0_gdsc", PC_GCC_GDSCR, 0, 1);
		pc_dump("gdsc try");
		return;
	}
	if (what == 1 || what == 3) {
		pc_set("collapse bit 0: cleared", PC_GCC_COLLAPSE, gcc + PC_GCC_COLLAPSE, collapse & ~1u);
	} else if (what == 4) {
		pc_set("collapse bits 0-4: cleared", PC_GCC_COLLAPSE, gcc + PC_GCC_COLLAPSE,
		    collapse & ~0x1fu);
	}
	if (what >= 2) {
		value = pc_rd(at);
		if (value & 1) {
			pc_set("pcie_0_gdsc: sw collapse cleared", PC_GCC_GDSCR, at, value & ~1u);
		}
	}
	for (unsigned i = 0; i < 20; i++) {
		pc_ms(1);
	}
	pc_dump("gdsc try");
}

/* 3: the resets. A core that has never been reset is not a core that is out
 * of reset: both are held for two milliseconds and released. The device tree
 * names only the first two; gcc-pineapple.c has five, and the COM PHY is the
 * one that holds the SerDes' PLL. */
static void
pc_resets(void)
{
	uint64_t gcc = PC_GCC_PHYS;
	static const uint32_t com[] = { 0x6c014, 0x6c020, 0x6c028 };
	static const uint32_t blocks[] = { 0x6f000, 0x6f00c };

	/* Clocks on, gdsc on, no parf or phy write of ours yet: this is the
	 * boot loader's state, if it left one. */
	print("pcie: 3: handed over parf +0 %x, +20 %x, +40 %x, +4c %x, +b8 %x, +1b0 %x, +1000 %x\n",
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x0), (uint64_t)pc_rd(PC_PARF_PHYS + 0x20),
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x40), (uint64_t)pc_rd(PC_PARF_PHYS + 0x4c),
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0xb8), (uint64_t)pc_rd(PC_PARF_PHYS + 0x1b0),
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x1000));
	print("pcie: 3: handed over phy +200 %x, +214 %x, +218 %x, +240 %x, +244 %x, +11e0 %x, +1200 %x\n",
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x200),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x214),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x218),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x240),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x244),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x11e0),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x1200));
	print("pcie: 3: resets: core %x, phy %x, com %x %x %x, block %x %x\n",
	    (uint64_t)pc_rd(gcc + PC_GCC_BCR), (uint64_t)pc_rd(gcc + PC_GCC_PHY_BCR),
	    (uint64_t)pc_rd(gcc + com[0]), (uint64_t)pc_rd(gcc + com[1]), (uint64_t)pc_rd(gcc + com[2]),
	    (uint64_t)pc_rd(gcc + blocks[0]), (uint64_t)pc_rd(gcc + blocks[1]));
	pc_set("core reset: held", PC_GCC_BCR, gcc + PC_GCC_BCR, 1);
	pc_set("phy reset: held", PC_GCC_PHY_BCR, gcc + PC_GCC_PHY_BCR, 1);
	pc_ms(2);
	pc_set("core reset: released", PC_GCC_BCR, gcc + PC_GCC_BCR, 0);
	pc_set("phy reset: released", PC_GCC_PHY_BCR, gcc + PC_GCC_PHY_BCR, 0);
	pc_ms(2);
	for (unsigned i = 0; i < sizeof(com) / sizeof(com[0]); i++) {
		pc_set("com phy: held", com[i], gcc + com[i], 1);
		pc_ms(1);
		pc_set("com phy: released", com[i], gcc + com[i], 0);
		pc_ms(1);
	}
	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		if (pc_rd(gcc + blocks[i]) & 1) {
			pc_set("phy block: released", blocks[i], gcc + blocks[i], 0);
		}
	}
	print("pcie: 3: after: core %x, phy %x, com %x %x %x\n", (uint64_t)pc_rd(gcc + PC_GCC_BCR),
	    (uint64_t)pc_rd(gcc + PC_GCC_PHY_BCR), (uint64_t)pc_rd(gcc + com[0]),
	    (uint64_t)pc_rd(gcc + com[1]), (uint64_t)pc_rd(gcc + com[2]));
}

/* 4: the pins. wlan-en is driven low here and raised after the rails are up,
 * so that the radio sees a power-on edge; perst stays low until 6. */
static void
pc_pins(void)
{
	uint64_t tlmm = PC_TLMM_PHYS + 0x1000 * PC_PIN_WLAN_EN;

	print("pcie: 4: pins: 16 %x/%x, 94 %x/%x, 95 %x/%x\n",
	    (uint64_t)pc_rd(tlmm), (uint64_t)pc_rd(tlmm + 4),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_CLKREQ),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_CLKREQ + 4));
	pc_set("pin 16 wlan-en io: low", 0, tlmm + 4, 0);
	/* 0x3c3 = gpio, output enable, drive 16 mA, pull up: the values the
	 * phone's cnss_wlan_en_active pinctrl and the working dodge uefi use. */
	pc_set("pin 16 wlan-en config: 16 mA, pull up", 0, tlmm, 0x3c3u);
	pc_set("pin 94 perst io: low", 0, PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4, 0);
	/* 0x201 = gpio, output enable, 2 mA, pull down, as pcie0_perst_default
	 * and the working dodge uefi program it. */
	pc_set("pin 94 perst config: 2 mA, pull down", 0,
	    PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST, 0x201u);
	pc_set("pin 95 clkreq: mux pcie0_clk_req_n, pull up", 0,
	    PC_TLMM_PHYS + 0x1000 * PC_PIN_CLKREQ, PC_PIN_CLKREQ_N);
	pc_set("pin 96 wake: gpio input, pull up", 0, PC_TLMM_PHYS + 0x1000 * PC_PIN_WAKE,
	    PC_PIN_IN_PULLUP);
	print("pcie: 4: clkreq %x/%x, wake %x/%x, perst %x/%x\n",
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_CLKREQ),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_CLKREQ + 4),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_WAKE),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_WAKE + 4),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4));
}

/* 6: the radio's seven supplies. The address column is the one the command
 * database carries for each resource (air164: 0x40a00 .. 0x44100). The name is
 * the cmd-db resource name, so the firmware client below can look it up again;
 * the label is only what the log says it powers. */
static const struct {
	const char *name;
	const char *label;
	uint32_t address;
	uint32_t millivolts;
} pc_rails[] = {
	{ "ldob15", "vdd-wlan-io",   0x40a00, 1800 },
	{ "ldoc3",  "vdd-wlan-io12", 0x42000, 1200 },
	{ "smpi4",  "vdd-wlan",      0x44100, 940 },
	{ "smpc2",  "vdd-wlan-aon",  0x40300, 976 },
	{ "smpc3",  "vdd-wlan-dig",  0x40400, 915 },
	{ "smpc6",  "vdd-wlan-rfa1", 0x40600, 1864 },
	{ "smpc1",  "vdd-wlan-rfa2", 0x40200, 1316 },
	/* pcie0's analog supply. Its node has no vreg-* names, so the values
	 * come from pcie1's node, which asks for the same three PMIC rails.
	 * The firmware client takes all ten votes but can only place seven of
	 * them in its own TCS set, and the direct RSC path below skipped these
	 * three because the address column was zero. The addresses are the ones
	 * air164's cmd-db lookup printed, so write them like the other seven. */
	{ "ldoi3",  "pcie 1p2",      0x44600, 1200 },
	{ "ldoe3",  "pcie 0p9",      0x43200, 880 },
	{ "ldoi1",  "pcie qref",     0x44500, 880 },
};

#define PC_MSGID_WRITE_4  0x10008u
#define PC_MSGID_WRITE_RESP_4  0x10108u   /* LEN 8 | RESP_REQ | WRITE */
#define PC_MSGID_READ_4   0x10108u
#define PC_RAIL_PROBE     0x40a00u      /* ldob15 vdd-wlan-io: read only */

/*
 * One RPMh "door" (TCS) per group. 0x2a0 is qcom,tcs-offset's stride for a v3
 * RSC; which index carries active-only traffic is what "oem pcie-rsc" finds.
 */
static int pc_rsc_tcs;
static int pc_rsc_drv = 2;      /* apps RSC drv block: 0x17a00000 + 0x10000 * n */
static uint32_t pc_rsc_status;  /* CMD_STATUS of the last command, kept before it is cleared */
static uint32_t pc_rsc_idle;    /* polls spent waiting for the TCS to be free */

static uint64_t
pc_rsc_base(int tcs)
{
	return PC_RSC_PHYS - 0x20000ULL + 0x10000ULL * (uint64_t)pc_rsc_drv +
	    0xd00 + 0x2a0ULL * (uint64_t)tcs;
}

/* One command in slot set 0. The layout is the 3.x one (a command stride of
 * 24 bytes, CONTROL at 0x24); the major version is in the DRV's first word. */
static int
pc_rsc(uint32_t address, uint32_t msgid, uint32_t data, int wants_answer, uint32_t *answer)
{
	uint64_t base = pc_rsc_base(pc_rsc_tcs);
	uint32_t irq_enable, polls, done;

	if (((pc_rd(PC_RSC_PHYS) >> 16) & 0xff) != 3) {
		print("pcie: rsc: not version 3 (%x): not written\n", (uint64_t)pc_rd(PC_RSC_PHYS));
		return 0;
	}
	irq_enable = pc_rd(base);
	pc_wr(base + 0x08, 1);
	pc_wr(base + 0x24, 0);
	pc_wr(base + 0x2c, 0);
	for (polls = 0; polls < 500; polls++) {
		if (pc_rd(base + 0x28) != 0) {
			break;
		}
		BS->stall(100);
	}
	pc_rsc_idle = polls;
	pc_wr(base + 0x34, msgid);
	pc_wr(base + 0x38, address);
	pc_wr(base + 0x3c, data);
	pc_wr(base + 0x20, wants_answer ? 1 : 0);
	pc_wr(base, irq_enable | 1);
	pc_wr(base + 0x2c, 1);
	BS->stall(10);
	pc_wr(base + 0x24, (1u << 16) | (1u << 24));
	for (polls = 0; polls < 2000; polls++) {
		if (pc_rd(base + 0x04) & 1) {
			break;
		}
		BS->stall(50);
	}
	pc_rsc_status = pc_rd(base + 0x40);
	if (answer != NULL) {
		*answer = pc_rd(base + 0x44);
	}
	done = (pc_rsc_status & (1u << 16)) != 0;   /* CMD_STATUS.COMPL */
	pc_wr(base + 0x08, 1);
	pc_wr(base + 0x24, 0);
	pc_wr(base + 0x2c, 0);
	pc_wr(base, irq_enable);
	return done;
}

/*
 * "oem pcie-rsc": vote the seven supplies through the firmware's own RPMh
 * client and raise wlan-en. This is the route ios-dodge takes on the SM8750:
 * CmdDbDxe turns a resource name into its address, RpmhDxe keeps the vote and
 * aggregates it with every other client, and a cold reset releases it. Nothing
 * here touches the PCIe controller, so it is safe on a cold phone.
 *
 * The ABI is the one recovered from RpmhDxe (create at 0x30, issue at 0x98,
 * revision 0x10001); the offsets are asserted at compile time.
 */
struct pc_cmddb_protocol {
	uint64_t revision;
	void *query;
	efi_status (EFIAPI *address)(void *self, const char *name, uint32_t *address);
};

struct pc_rpmh_protocol {
	uint64_t revision;
	void *init;
	efi_status (EFIAPI *valid)(uint32_t drv, uint8_t *valid);
	efi_status (EFIAPI *count)(uint32_t drv, uint32_t *count);
	void *create_internal, *create_explicit;
	efi_status (EFIAPI *create)(uint32_t drv, const char *name, void **client);
	void *invalidate, *callback, *destroy, *find, *churn_all, *churn_single;
	void *finish, *finish_sw, *issue_set, *invalidate_sleep, *invalidate_wake, *read;
	efi_status (EFIAPI *issue)(void *client, uint32_t sleep_set, uint8_t wait,
	    uint32_t address, uint32_t data, uint32_t *request);
};

_Static_assert(__builtin_offsetof(struct pc_rpmh_protocol, create) == 0x30,
    "RpmhDxe create ABI");
_Static_assert(__builtin_offsetof(struct pc_rpmh_protocol, issue) == 0x98,
    "RpmhDxe issue ABI");

static int pc_rails_client_state;   /* 0 untried, 1 voted, -1 not available */

/* 1 when the firmware took the votes, 0 when it has no such client. The vote
 * itself is retained by the driver, so the handle is not kept here. */
static int
pc_rails_protocol(void)
{
	static const efi_guid cmddb_guid = { 0x1c34f691, 0xd33d, 0x4e14,
	    { 0xb8, 0xf8, 0x32, 0xc6, 0xc0, 0x29, 0xe9, 0x5b } };
	static const efi_guid rpmh_guid = { 0xe1d0b573, 0xf29f, 0x495d,
	    { 0x93, 0x52, 0xca, 0x0d, 0xd4, 0x2a, 0x0a, 0x74 } };
	struct pc_cmddb_protocol *db = NULL;
	struct pc_rpmh_protocol *rpmh = NULL;
	void *client = NULL;
	efi_status s, t;
	uint8_t valid = 0;
	uint32_t count = 0, voted = 0;

	if (pc_rails_client_state != 0) {
		return pc_rails_client_state > 0;
	}
	pc_rails_client_state = -1;
	s = BS->locate_protocol(&cmddb_guid, NULL, (void **)&db);
	t = BS->locate_protocol(&rpmh_guid, NULL, (void **)&rpmh);
	print("pcie: rails: cmd-db %x (rev %x), rpmh %x (rev %x)\n", (uint64_t)s,
	    db != NULL ? (uint64_t)db->revision : 0, (uint64_t)t,
	    rpmh != NULL ? (uint64_t)rpmh->revision : 0);
	if (EFI_ERROR(s) || EFI_ERROR(t) || db == NULL || rpmh == NULL ||
	    db->address == NULL || rpmh->create == NULL || rpmh->issue == NULL) {
		print("pcie: rails: no firmware client, the tcs path has to do it\n");
		return 0;
	}
	/* drv 2 is the block the device tree gives the applications RSC. */
	s = rpmh->valid(2, &valid);
	t = rpmh->count(2, &count);
	print("pcie: rails: drv 2 valid %d (%x), tcs count %d (%x)\n", (uint64_t)valid,
	    (uint64_t)s, (uint64_t)count, (uint64_t)t);
	if (EFI_ERROR(s) || EFI_ERROR(t) || !valid || count < 3) {
		return 0;
	}
	s = rpmh->create(2, "hackios-corvette-wifi", &client);
	print("pcie: rails: client %x handle %x\n", (uint64_t)s,
	    (uint64_t)(uintptr_t)client);
	if (EFI_ERROR(s) || client == NULL) {
		return 0;
	}
	for (unsigned r = 0; r < sizeof(pc_rails) / sizeof(pc_rails[0]); r++) {
		uint32_t address = 0, request = 0;
		efi_status dbs, vs, ms, es;

		dbs = db->address(db, pc_rails[r].name, &address);
		if (EFI_ERROR(dbs) || address == 0) {
			print("pcie: rails: %s: no address (%x)\n", pc_rails[r].name,
			    (uint64_t)dbs);
			continue;
		}
		/* Voltage, then mode 7 (high power), then enable: the order the
		 * working dodge uefi uses. Without the mode vote an ldo can stay in
		 * low power mode and limit the radio's inrush. */
		vs = rpmh->issue(client, 0, 0, address, pc_rails[r].millivolts, &request);
		ms = rpmh->issue(client, 0, 0, address + 8, 7, &request);
		es = rpmh->issue(client, 0, 0, address + 4, 1, &request);
		print("pcie: rails: %s %s at %x: %d mV %x, mode %x, enable %x (request %d)\n",
		    pc_rails[r].name, pc_rails[r].label, (uint64_t)address,
		    (uint64_t)pc_rails[r].millivolts, (uint64_t)vs, (uint64_t)ms,
		    (uint64_t)es, (uint64_t)request);
		if (!EFI_ERROR(vs) && !EFI_ERROR(es)) {
			voted++;
		}
	}
	/* The radio's crystal is an RPMh clock resource, not a supply: both
	 * working bring-ups vote it before the chip is powered. Without it
	 * the pcie block has no reference and never leaves reset. */
	{
		static const char *const clocks[] = { "rfclka1", "rfclka0", "rfclkb1" };

		for (unsigned c = 0; c < sizeof clocks / sizeof clocks[0]; c++) {
			uint32_t address = 0, request = 0;
			efi_status cs = db->address(db, clocks[c], &address);

			if (EFI_ERROR(cs) || address == 0) {
				print("pcie: rails: clock %s: no address (%x)\n",
				    clocks[c], (uint64_t)cs);
				continue;
			}
			cs = rpmh->issue(client, 0, 0, address, 1, &request);
			print("pcie: rails: clock %s at %x: on %x (request %d)\n",
			    clocks[c], (uint64_t)address, (uint64_t)cs, (uint64_t)request);
		}
	}
	BS->stall(10000);
	print("pcie: rails: %d of %d votes taken\n", (uint64_t)voted,
	    (uint64_t)(sizeof(pc_rails) / sizeof(pc_rails[0])));
	pc_rails_client_state = (int)voted == (int)(sizeof(pc_rails) / sizeof(pc_rails[0])) ? 1 : -1;
	return pc_rails_client_state > 0;
}

static void
pc_rsc_probe(void)
{
	uint64_t wlan = PC_TLMM_PHYS + 0x1000 * PC_PIN_WLAN_EN;

	print("\npcie: rsc: the firmware rail vote\n");
	if (!pc_rails_protocol()) {
		print("pcie: rsc: nothing was voted\n");
		return;
	}
	pc_set("pin 16 wlan-en io: high", 0, wlan + 4, 2);
	pc_ms(100);
	print("pcie: rsc: done\n");
}

/* The always-on processor's mailbox. qcom_aoss.c opens it with five words of
 * its message memory; the waits here are polls, not interrupts, and the
 * doorbell is the one redwood_adsp.c rings: IPCC's send register at +0xc,
 * client 0 (the AOP) in the upper half, signal 0 in the lower. */

/* qcom,pdc_init_table of this phone's cnss-kiwi node, verbatim and in order:
 * the up and down level of every rail the PDC's rf and bb domains own.
 * cnss_aop_pdc_reconfig() sends exactly these before the radio is powered. */
static const char *const pc_pdc_msg[] = {
	"{class: wlan_pdc, ss: rf, res: s3c.v, upval: 916}",
	"{class: wlan_pdc, ss: rf, res: s3c.v, upval: 916}",
	"{class: wlan_pdc, ss: rf, res: s1c.v, upval: 1316}",
	"{class: wlan_pdc, ss: rf, res: s1c.v, upval: 1316}",
	"{class: wlan_pdc, ss: rf, res: s6c.v, upval: 1864}",
	"{class: wlan_pdc, ss: rf, res: s6c.v, upval: 1864}",
	"{class: wlan_pdc, ss: bb, res: s2c.v, upval: 976}",
	"{class: wlan_pdc, ss: bb, res: s2c.v, upval: 976}",
	"{class: wlan_pdc, ss: bb, res: s4i.v, upval: 940}",
	"{class: wlan_pdc, ss: bb, res: s4i.v, upval: 940}",
};

static void
pc_qmp_kick(void)
{
	pc_wr(PC_IPCC_SEND, 0);      /* client 0, signal 0 */
}

static int
pc_qmp_wait(uint32_t off, uint32_t want)
{
	/* qmp_open() and qmp_send() both give the far side a second. */
	for (unsigned i = 0; i < 1000; i++) {
		if (pc_rd(PC_QMP_PHYS + off) == want) {
			return 1;
		}
		pc_ms(1);
	}
	return 0;
}

/* How many of the ten the AOP took; 0 also means the mailbox was not ours to
 * use. A message is never written over one that is still in the buffer. */
static int
pc_radio_pdc(void)
{
	uint32_t off, size;
	int taken = 0;

	print("\npcie: 6a: the PDC table the radio's rails need first\n");
	if (pc_rd(PC_QMP_PHYS) != PC_QMP_MAGIC || pc_rd(PC_QMP_PHYS + 4) != 1) {
		print("pcie:   qmp +0 %x +4 %x: no mailbox, the table is left alone\n",
		    (uint64_t)pc_rd(PC_QMP_PHYS), (uint64_t)pc_rd(PC_QMP_PHYS + 4));
		return 0;
	}
	off = pc_rd(PC_QMP_PHYS + 0x38);
	size = pc_rd(PC_QMP_PHYS + 0x34);
	print("pcie:   qmp mailbox +%x size %x; ucore link %x ack %x, channel %x ack %x\n",
	    (uint64_t)off, (uint64_t)size, (uint64_t)pc_rd(PC_QMP_PHYS + 0xc),
	    (uint64_t)pc_rd(PC_QMP_PHYS + 0x10), (uint64_t)pc_rd(PC_QMP_PHYS + 0x14),
	    (uint64_t)pc_rd(PC_QMP_PHYS + 0x18));
	if (size < PC_QMP_MSG_LEN + 4 || off + size > 0x400) {
		print("pcie:   the mailbox does not hold a %d byte message\n",
		    (uint64_t)PC_QMP_MSG_LEN);
		return 0;
	}
	/* qmp_open(): ack the far side's link, raise ours, then the channel. */
	pc_wr(PC_QMP_PHYS + 0x10, pc_rd(PC_QMP_PHYS + 0xc));
	pc_wr(PC_QMP_PHYS + 0x24, PC_QMP_UP);
	pc_qmp_kick();
	if (!pc_qmp_wait(0x28, PC_QMP_UP)) {
		print("pcie:   the always-on processor did not ack the link\n");
		return 0;
	}
	pc_wr(PC_QMP_PHYS + 0x2c, PC_QMP_UP);
	pc_qmp_kick();
	if (!pc_qmp_wait(0x14, PC_QMP_UP)) {
		print("pcie:   the always-on processor did not open its channel\n");
		return 0;
	}
	pc_wr(PC_QMP_PHYS + 0x18, PC_QMP_UP);
	pc_qmp_kick();
	if (!pc_qmp_wait(0x30, PC_QMP_UP)) {
		print("pcie:   the always-on processor did not ack the channel\n");
		return 0;
	}
	print("pcie:   qmp open: link %x ack %x, channel %x ack %x\n",
	    (uint64_t)pc_rd(PC_QMP_PHYS + 0x24), (uint64_t)pc_rd(PC_QMP_PHYS + 0x28),
	    (uint64_t)pc_rd(PC_QMP_PHYS + 0x2c), (uint64_t)pc_rd(PC_QMP_PHYS + 0x30));
	/* qmp_send(): the payload at +4, its length at +0, 64 bytes, NUL padded,
	 * written 32 bits at a time; the length word going back to 0 is the ack. */
	for (unsigned m = 0; m < sizeof(pc_pdc_msg) / sizeof(pc_pdc_msg[0]); m++) {
		const char *msg = pc_pdc_msg[m];
		unsigned n = 0;

		if (pc_rd(PC_QMP_PHYS + off) != 0) {
			print("pcie:   message %d not sent: one is still in the buffer\n",
			    (uint64_t)m);
			break;
		}
		while (n < PC_QMP_MSG_LEN - 1 && msg[n] != 0) {
			n++;
		}
		for (unsigned k = 0; k < PC_QMP_MSG_LEN; k += 4) {
			uint32_t w = 0;

			for (unsigned b = 0; b < 4; b++) {
				if (k + b < n) {
					w |= (uint32_t)(uint8_t)msg[k + b] << (8 * b);
				}
			}
			pc_wr(PC_QMP_PHYS + off + 4 + k, w);
		}
		pc_wr(PC_QMP_PHYS + off, PC_QMP_MSG_LEN);
		pc_rd(PC_QMP_PHYS + off);       /* the read back qmp_send() does */
		pc_qmp_kick();
		if (!pc_qmp_wait(off, 0)) {
			print("pcie:   message %d \"%s\" was not consumed\n",
			    (uint64_t)m, msg);
			pc_wr(PC_QMP_PHYS + off, 0);
			break;
		}
		taken++;
		print("pcie:   pdc %d/%d taken: %s\n", (uint64_t)(m + 1),
		    (uint64_t)(sizeof(pc_pdc_msg) / sizeof(pc_pdc_msg[0])), msg);
	}
	print("pcie: 6a: %d of %d PDC messages taken\n", (uint64_t)taken,
	    (uint64_t)(sizeof(pc_pdc_msg) / sizeof(pc_pdc_msg[0])));
	return taken;
}

static void
pc_radio(void)
{
	uint64_t wlan = PC_TLMM_PHYS + 0x1000 * PC_PIN_WLAN_EN;
	unsigned on = 0;

	print("\npcie: 6: the radio's 7 supplies\n");
	/* The status pins as the chip leaves them, before anything is voted:
	 * 17 is bt-en, 18 sw-ctrl and 19 wlan-sw-ctrl. */
	print("pcie:   pins before the rails: 16 %x, 17 %x, 18 %x, 19 %x\n",
	    (uint64_t)pc_rd(wlan + 4), (uint64_t)pc_rd(PC_TLMM_PHYS + 0x11004),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x12004), (uint64_t)pc_rd(PC_TLMM_PHYS + 0x13004));
	(void)pc_radio_pdc();
	/* Firmware client first; the TCS below is the fallback when it is absent. */
	(void)pc_rails_protocol();
	/* Every step is a write with RESP_REQ and is waited for. Nothing is read
	 * back: a read command here is a write of its data word, so the readbacks
	 * of the earlier rounds were writing zero and switching the rail off. */
	for (unsigned r = 0; r < sizeof(pc_rails) / sizeof(pc_rails[0]); r++) {
		uint32_t address = pc_rails[r].address;
		int volt, mode, enable;

		if (address == 0) {
			print("pcie:   %s %s: no tcs address, left to the client\n",
			    pc_rails[r].name, pc_rails[r].label);
			continue;
		}
		volt = pc_rsc(address, PC_MSGID_WRITE_RESP_4, pc_rails[r].millivolts, 1, NULL);
		mode = pc_rsc(address + 8, PC_MSGID_WRITE_RESP_4, 7, 1, NULL);
		enable = pc_rsc(address + 4, PC_MSGID_WRITE_RESP_4, 1, 1, NULL);
		print("pcie:   %s %s at %x: %d mV %s, mode %s, enable %s\n",
		    pc_rails[r].name, pc_rails[r].label, (uint64_t)address,
		    (uint64_t)pc_rails[r].millivolts, volt ? "completed" : "silent",
		    mode ? "completed" : "silent", enable ? "completed" : "silent");
		if (enable) {
			on++;
		}
		pc_ms(2);
	}
	print("pcie: 6: %d of %d enables completed\n", (uint64_t)on,
	    (uint64_t)(sizeof(pc_rails) / sizeof(pc_rails[0])));
	pc_set("pin 16 wlan-en io: high", 0, wlan + 4, 2);
	pc_ms(100);
	print("pcie:   pins after wlan-en: 16 %x, 17 %x, 18 %x, 19 %x\n",
	    (uint64_t)pc_rd(wlan + 4), (uint64_t)pc_rd(PC_TLMM_PHYS + 0x11004),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x12004), (uint64_t)pc_rd(PC_TLMM_PHYS + 0x13004));
	/* The table programs the radio's own pmu, which is what generates
	 * VDD13_PMU_PCIE_I and VDD19_PMU_PCIE_I, so send it once more now that
	 * the chip is powered and has pulled sw-ctrl up. The values are the
	 * same, so the second pass cannot make anything worse. */
	(void)pc_radio_pdc();
}

/* 6: the reference clock out to the far end. Without it the radio's receiver
 * cannot answer a receiver detection, whatever else is configured. */
static void
pc_clkref(void)
{
	print("pcie: 6: the reference clock, tcsr %x\n", PC_TCSR_PHYS);
	/* air166aj left these alone and the run never came back; the images
	 * that did complete always drove them, so drive them. */
	pc_set("tcsr pcie 0 clkref", 0xb1100, PC_TCSR_PHYS + 0xb1100,
	    pc_rd(PC_TCSR_PHYS + 0xb1100) | 1u);
	pc_set("tcsr pcie 1 clkref", 0xb1114, PC_TCSR_PHYS + 0xb1114,
	    pc_rd(PC_TCSR_PHYS + 0xb1114) | 1u);
	/* The refgen is a pll of its own and stage 7 reaches the phy's start
	 * bit within tens of microseconds: let it settle first. */
	pc_ms(20);
	print("pcie: 6: refgen settled 20 ms, clkref %x %x\n",
	    (uint64_t)pc_rd(PC_TCSR_PHYS + 0xb1100), (uint64_t)pc_rd(PC_TCSR_PHYS + 0xb1114));
}

/* 7: the PHY's own sequence, 115 triples of offset, value and delay from the
 * phone's device tree (qcom,phy-sequence). The device type has to be RC before
 * the tables go in: the vendor's msm_pcie_enable_link() writes it first. */
static const uint32_t pc_phy_sequence[] = {
	0x240, 0x03, 0x00, 0x0c0, 0x01, 0x00, 0x0cc, 0x62, 0x00,
	0x0d0, 0x02, 0x00, 0x060, 0xf8, 0x00, 0x064, 0x01, 0x00,
	0x000, 0x93, 0x00, 0x004, 0x01, 0x00, 0x0e0, 0x90, 0x00,
	0x0e4, 0x82, 0x00, 0x0f4, 0x07, 0x00, 0x070, 0x02, 0x00,
	0x010, 0x02, 0x00, 0x074, 0x16, 0x00, 0x014, 0x16, 0x00,
	0x078, 0x36, 0x00, 0x018, 0x36, 0x00, 0x110, 0x08, 0x00,
	0x0bc, 0x0a, 0x00, 0x120, 0x42, 0x00, 0x080, 0x04, 0x00,
	0x084, 0x0d, 0x00, 0x020, 0x0a, 0x00, 0x024, 0x1a, 0x00,
	0x088, 0x41, 0x00, 0x028, 0x34, 0x00, 0x090, 0xab, 0x00,
	0x094, 0xaa, 0x00, 0x098, 0x01, 0x00, 0x030, 0x55, 0x00,
	0x034, 0x55, 0x00, 0x038, 0x01, 0x00, 0x140, 0x14, 0x00,
	0x164, 0x34, 0x00, 0x03c, 0x01, 0x00, 0x01c, 0x04, 0x00,
	0x174, 0x16, 0x00, 0x1bc, 0x0f, 0x00, 0x170, 0xa0, 0x00,
	0x11a4, 0x38, 0x00, 0x10dc, 0x11, 0x00, 0x1160, 0xbf, 0x00,
	0x1164, 0xbf, 0x00, 0x1168, 0xb7, 0x00, 0x116c, 0xea, 0x00,
	0x115c, 0x3f, 0x00, 0x1174, 0x5c, 0x00, 0x1178, 0x9c, 0x00,
	0x117c, 0x1a, 0x00, 0x1180, 0x89, 0x00, 0x1170, 0xdc, 0x00,
	0x1188, 0x94, 0x00, 0x118c, 0x5b, 0x00, 0x1190, 0x1a, 0x00,
	0x1194, 0x89, 0x00, 0x10cc, 0x00, 0x00, 0x1008, 0x09, 0x00,
	0x1014, 0x05, 0x00, 0x104c, 0x08, 0x00, 0x1050, 0x08, 0x00,
	0x10d8, 0x0f, 0x00, 0x1118, 0x1c, 0x00, 0x10f8, 0x07, 0x00,
	0x11f8, 0x08, 0x00, 0x1600, 0x00, 0x00, 0xe84, 0x15, 0x00,
	0xe90, 0x3f, 0x00, 0xee4, 0x02, 0x00, 0xe40, 0x09, 0x00,
	0xe3c, 0x15, 0x00, 0x19a4, 0x38, 0x00, 0x18dc, 0x11, 0x00,
	0x1960, 0xbf, 0x00, 0x1964, 0xbf, 0x00, 0x1968, 0xb7, 0x00,
	0x196c, 0xea, 0x00, 0x195c, 0x3f, 0x00, 0x1974, 0x5c, 0x00,
	0x1978, 0x9c, 0x00, 0x197c, 0x1a, 0x00, 0x1980, 0x89, 0x00,
	0x1970, 0xdc, 0x00, 0x1988, 0x94, 0x00, 0x198c, 0x5b, 0x00,
	0x1990, 0x1a, 0x00, 0x1994, 0x89, 0x00, 0x18cc, 0x00, 0x00,
	0x1808, 0x09, 0x00, 0x1814, 0x05, 0x00, 0x184c, 0x08, 0x00,
	0x1850, 0x08, 0x00, 0x18d8, 0x0f, 0x00, 0x1918, 0x1c, 0x00,
	0x18f8, 0x07, 0x00, 0x19f8, 0x08, 0x00, 0x1684, 0x15, 0x00,
	0x1690, 0x3f, 0x00, 0x16e4, 0x02, 0x00, 0x1640, 0x09, 0x00,
	0x163c, 0x15, 0x00, 0x2dc, 0x05, 0x00, 0x388, 0x77, 0x00,
	0x398, 0x0b, 0x00, 0x6a4, 0x1e, 0x00, 0x6f4, 0x27, 0x00,
	0x3e0, 0x0f, 0x00, 0x60c, 0x1d, 0x00, 0x614, 0x07, 0x00,
	0x620, 0xc1, 0x00, 0x694, 0x00, 0x00, 0x3d0, 0x8c, 0x00,
	0x368, 0x17, 0x00, 0x370, 0x2e, 0x00, 0x200, 0x00, 0x00,
	0x244, 0x03, 0x00
};

/* The LTSSM sits in detect.quiet until the phy drives a PIPE clock into the
 * controller, so a short LTSSM enable answers "is the phy's PLL running right
 * now?" with the hardware itself. The state is cleared again afterwards. */
/*
 * Count a clock through gcc's debug mux. The counter runs against the 4.8 MHz
 * reference for 0x20000 ticks (about 27 ms) and the status word holds the
 * number of edges seen in that window.
 */
static uint32_t
pc_measure(const char *what, uint32_t sel)
{
	const uint64_t gcc = PC_GCC_PHYS;
	uint32_t ctl, st, count;
	int spins;

	pc_wr(gcc + 0x62004, (uint32_t)pc_rd(gcc + 0x62004) | 1u);
	pc_wr(gcc + 0x62000, ((uint32_t)pc_rd(gcc + 0x62000) & ~0xfu) | 2u);
	pc_wr(gcc + 0x62024, sel & 0x3ffu);

	ctl = (uint32_t)pc_rd(gcc + 0x62048);
	pc_wr(gcc + 0x62048, ctl & ~(1u << 20));
	pc_wr(gcc + 0x62048, (ctl & ~(1u << 20)) | (1u << 21));
	pc_wr(gcc + 0x62048, ctl & ~((1u << 20) | (1u << 21)));

	ctl = ((ctl & ~0xfffffu) | 0x20000u);
	pc_wr(gcc + 0x62048, ctl);
	pc_wr(gcc + 0x62048, ctl | (1u << 20));

	st = (uint32_t)pc_rd(gcc + 0x6204c);
	for (spins = 0; spins < 200 && (st & (1u << 25)) == 0; spins++) {
		pc_ms(1);
		st = (uint32_t)pc_rd(gcc + 0x6204c);
	}

	pc_wr(gcc + 0x62048, ctl);
	st = (uint32_t)pc_rd(gcc + 0x6204c);
	count = st & 0x1ffffffu;

	print("pcie: measure %s (sel %x): %d counts, done %d, %d Hz\n",
	    what, sel, count, (st >> 25) & 1,
	    (uint64_t)count * 4800000u / 0x20000u);
	return count;
}

static uint32_t
pc_pipe_probe(const char *where)
{
	uint64_t parf = PC_PARF_PHYS;
	uint32_t state;

	pc_set("probe: ltssm on", 0x1b0, parf + 0x1b0, pc_rd(parf + 0x1b0) | PC_LTSSM_EN);
	pc_ms(30);
	state = (uint32_t)(pc_rd(parf + 0x1b0) & 0x3f);
	pc_set("probe: ltssm off", 0x1b0, parf + 0x1b0, pc_rd(parf + 0x1b0) & ~PC_LTSSM_EN);
	print("pcie: probe %s: ltssm %x %s, elbi %x, phy +214 %x, +21c %x\n", where,
	    (uint64_t)state, pc_ltssm_name(state), (uint64_t)pc_rd(PC_CORE_PHYS + 0xf28),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x214),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x21c));
	return state;
}

static void
pc_phy(void)
{
	uint64_t parf = PC_PARF_PHYS, phy = PC_PARF_PHYS + PC_PHY_AT;
	uint32_t writes = sizeof(pc_phy_sequence) / sizeof(pc_phy_sequence[0]) / 3, status = 0, polls;

	/* gcc_pcie_0_phy_rchng_clk_src, gcc 6b058: the vendor node asks for the
	 * rate change clock at 100 MHz (GPLL0_OUT_EVEN / 3) and the boot loader
	 * left it on the TCXO. Gate the branch, move the source, gate it back. */
	{
		uint32_t cfg = pc_rd(PC_GCC_PHYS + 0x6b058);

		print("pcie: 7: rate change clk: gpll0 %x, src %x, votes %x\n",
		    (uint64_t)pc_rd(PC_GCC_PHYS), (uint64_t)cfg,
		    (uint64_t)pc_rd(PC_GCC_PHYS + 0x52000));
		cfg = pc_rd(PC_GCC_PHYS + 0x6b058);
		print("pcie: 7: rate change clk: reread %x\n", (uint64_t)cfg);
		pc_set("rate change clk: 100 MHz from gpll0", 0x6b058, PC_GCC_PHYS + 0x6b058,
		    (cfg & ~0x7ffu) | (6u << 8) | 5u);
		pc_ms(2);
		print("pcie: 7: rate change clk now: src %x, votes %x, gpll0 %x\n",
		    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b058),
		    (uint64_t)pc_rd(PC_GCC_PHYS + 0x52000), (uint64_t)pc_rd(PC_GCC_PHYS));
	}

	/* msm_pcie_enable_link() writes four PARF registers before the phy
	 * tables: PHY_CTRL bit 0 cleared is what lets the controller's PIPE
	 * clocks and resets run, SYS_CTRL carries the clock requests, PM_CTRL
	 * bit 5 lifts the "never enter L1" veto, and MHI clock/reset ctrl bit 4
	 * releases the MHI path's reset. The S1 wrote only the device type. */
	print("pcie: 7: parf sys %x, phy ctrl %x, pm ctrl %x, mhi ctrl %x\n",
	    (uint64_t)pc_rd(parf), (uint64_t)pc_rd(parf + 0x40), (uint64_t)pc_rd(parf + 0x20),
	    (uint64_t)pc_rd(parf + 0x174));

	/* The board default is 1019, pad selected: leave it alone. Route the
	 * pipe clock before the tables instead, the way qmp_pcie_power_on does. */
	/* 1019 is the board's own value and the hardware puts it back after
	 * every write, so this register is read only in practice. */
	print("pcie: 7: refclk +4c left alone: %x\n", (uint64_t)pc_rd(parf + 0x4c));
	pc_set("pipe mux: phy before the tables", 0x6b070, PC_GCC_PHYS + 0x6b070, 0);
	pc_set("pipe clk: on before the tables", 0x52008, PC_GCC_PHYS + 0x52008,
	    pc_rd(PC_GCC_PHYS + 0x52008) | (1u << 4));
	pc_ms(3);

	/* Stage 3 pulsed these before the refgen existed, so the phy's pll block
	 * may never have left reset. Same five resets, same polarity, now that
	 * the reference is settled and the boot loader's clocks are running. */
	{
		static const uint32_t com[] = { 0x6c014, 0x6c020, 0x6c028 };

		pc_set("reset again: core held", PC_GCC_BCR, PC_GCC_PHYS + PC_GCC_BCR, 1);
		pc_set("reset again: phy held", PC_GCC_PHY_BCR, PC_GCC_PHYS + PC_GCC_PHY_BCR, 1);
		pc_ms(2);
		pc_set("reset again: core released", PC_GCC_BCR, PC_GCC_PHYS + PC_GCC_BCR, 0);
		pc_set("reset again: phy released", PC_GCC_PHY_BCR, PC_GCC_PHYS + PC_GCC_PHY_BCR, 0);
		pc_ms(2);
		for (unsigned i = 0; i < sizeof(com) / sizeof(com[0]); i++) {
			pc_set("reset again: com held", com[i], PC_GCC_PHYS + com[i], 1);
			pc_ms(1);
			pc_set("reset again: com released", com[i], PC_GCC_PHYS + com[i], 0);
			pc_ms(1);
		}
	}
	/* The core reset above takes the PARF block back to its defaults, so
	 * everything that has to survive goes in afterwards, in the order
	 * msm_pcie_enable_link() uses: reset, then device type and the four
	 * enables, then the tables. */
	pc_set("device type: RC after the reset", 0x1000, parf + 0x1000, 4);
	pc_set("phy ctrl: clocks and resets on", 0x40, parf + 0x40, pc_rd(parf + 0x40) & ~1u);
	print("pcie: 7: after the reset: device type %x, sys %x, phy ctrl %x\n",
	    (uint64_t)pc_rd(parf + 0x1000), (uint64_t)pc_rd(parf),
	    (uint64_t)pc_rd(parf + 0x40));
	print("pcie: 7: the phy's sequence, %d writes from %x\n", (uint64_t)writes, phy);
	for (uint32_t i = 0; i < writes; i++) {
		uint32_t offset = pc_phy_sequence[3 * i], value = pc_phy_sequence[3 * i + 1];
		uint32_t delay = pc_phy_sequence[3 * i + 2], before = pc_rd(phy + offset);

		pc_wr(phy + offset, value);
		if (pc_rd(phy + offset) != value) {
			print("pcie:   phy %d/%d +%x did not stick: %x -> %x\n",
			    (uint64_t)(i + 1), (uint64_t)writes, (uint64_t)offset,
			    (uint64_t)before, (uint64_t)pc_rd(phy + offset));
		}
		if (delay) {
			pc_ms(delay);
		}
	}
	/* PHYSTATUS is bit 6 of 0x214 and it means the opposite of "ready": Linux
	 * polls it until it is CLEAR ("if (status & BIT(6)) return false"). */
	for (polls = 0; polls < 200; polls++) {
		status = pc_rd(phy + 0x214);
		if (!(status & 0x40)) {
			break;
		}
		pc_ms(1);
	}
	print("pcie: 7: phy +214 %x (bit 6 %d), %d ms; 240 %x, 244 %x, 218 %x, 21c %x, 600 %x\n",
	    (uint64_t)status, (uint64_t)((status >> 6) & 1), (uint64_t)polls, (uint64_t)pc_rd(phy + 0x240),
	    (uint64_t)pc_rd(phy + 0x244), (uint64_t)pc_rd(phy + 0x218), (uint64_t)pc_rd(phy + 0x21c),
	    (uint64_t)pc_rd(phy + 0x600));
	/* gcc_pcie_0_pipe_clk_src at gcc + 0x6b070: 0 is the phy, 2 is the
	 * TCXO. The controller's PIPE clock is what carries the receiver
	 * detect handshake, so it must come from the phy. */
	print("pcie: 7: pipe mux %x (0 = phy), rchng mux %x, pipe halt %x, votes %x\n",
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b070), (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b058),
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b048), (uint64_t)pc_rd(PC_GCC_PHYS + 0x52008));
	print("pcie: 7: aux halt %x, cfg halt %x, rchng halt %x, aux src %x, rchng src %x\n",
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b03c), (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b038),
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b054), (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b074),
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b058));
	/* A hardware gated GCC branch can keep the source it was voted on
	 * latched, so take the pipe clock down across the mux change. */
	pc_set("pipe clk: vote off", 0x52008, PC_GCC_PHYS + 0x52008,
	    pc_rd(PC_GCC_PHYS + 0x52008) & ~(1u << 4));
	pc_set("pipe mux: phy", 0x6b070, PC_GCC_PHYS + 0x6b070,
	    pc_rd(PC_GCC_PHYS + 0x6b070) & ~3u);
	pc_ms(1);
	pc_set("pipe clk: vote on", 0x52008, PC_GCC_PHYS + 0x52008,
	    pc_rd(PC_GCC_PHYS + 0x52008) | (1u << 4));
	pc_ms(1);
	print("pcie: 7: refclk before the restart +4c %x\n", (uint64_t)pc_rd(parf + 0x4c));
	/* Restart the phy's power sequencer now that the pipe clock is routed,
	 * then let the pll settle before reading anything back. */
	pc_set("phy power down: down", 0x240, phy + 0x240, 0);
	pc_ms(1);
	pc_set("phy power down: up", 0x240, phy + 0x240, 3);
	pc_ms(10);
	print("pcie: 7: phy after the restart +214 %x, +21c %x, +600 %x, +604 %x\n",
	    (uint64_t)pc_rd(phy + 0x214), (uint64_t)pc_rd(phy + 0x21c),
	    (uint64_t)pc_rd(phy + 0x600), (uint64_t)pc_rd(phy + 0x604));
	print("pcie: 7: refclk after the restart +4c %x\n", (uint64_t)pc_rd(parf + 0x4c));
	print("pcie: 7: phy com %x %x %x %x %x %x %x %x\n",
	    (uint64_t)pc_rd(phy + 0x220), (uint64_t)pc_rd(phy + 0x224),
	    (uint64_t)pc_rd(phy + 0x228), (uint64_t)pc_rd(phy + 0x22c),
	    (uint64_t)pc_rd(phy + 0x230), (uint64_t)pc_rd(phy + 0x234),
	    (uint64_t)pc_rd(phy + 0x238), (uint64_t)pc_rd(phy + 0x23c));
	print("pcie: 7: phy pll %x %x %x %x %x %x %x %x\n",
	    (uint64_t)pc_rd(phy + 0x11e0), (uint64_t)pc_rd(phy + 0x1200),
	    (uint64_t)pc_rd(phy + 0x1204), (uint64_t)pc_rd(phy + 0x1208),
	    (uint64_t)pc_rd(phy + 0x120c), (uint64_t)pc_rd(phy + 0x1210),
	    (uint64_t)pc_rd(phy + 0x1214), (uint64_t)pc_rd(phy + 0x1218));
	(void)pc_pipe_probe("after the phy tables");
	pc_measure("gcc_pcie_0_aux_clk", 0x14cu);
	pc_measure("pcie_0_pipe_clk from the phy", 0x14fu);
	pc_measure("gcc_pcie_0_pipe_clk branch", 0x14du);
	pc_measure("gcc_pcie_0_phy_rchng_clk", 0x14eu);
}

/* 8: the core's own config space. The vendor writes 0 to DBI_BASE_ADDR_V2 (the
 * core keeps decoding at the offset the boot loader left); the ATU base stays
 * 0x1000, and the DT's slv-addr-space-size goes in the low word. */
static void
pc_dbi(void)
{
	uint64_t parf = PC_PARF_PHYS, core = PC_CORE_PHYS;
	uint32_t id, command, class, bus, link, misc;

	print("pcie: 8: dbi base %x, atu base %x, slv size %x; core id %x\n",
	    (uint64_t)pc_rd(parf + 0x350), (uint64_t)pc_rd(parf + 0x634), (uint64_t)pc_rd(parf + 0x358),
	    (uint64_t)pc_rd(core));
	if (pc_rd(parf + 0x350) != 0) {
		pc_set("dbi base v2: 0", 0x350, parf + 0x350, 0);
	}
	if (pc_rd(parf + 0x354) != 0) {
		pc_set("dbi base v2 hi: 0", 0x354, parf + 0x354, 0);
	}
	if (pc_rd(parf + 0x634) != 0x1000) {
		pc_set("atu base: 0x1000", 0x634, parf + 0x634, 0x1000);
	}
	if (pc_rd(parf + 0x638) != 0) {
		pc_set("atu base hi: 0", 0x638, parf + 0x638, 0);
	}
	if (pc_rd(parf + 0x358) != 0x4000000) {
		pc_set("slv size v2: 64 MB", 0x358, parf + 0x358, 0x4000000);
	}
	if (pc_rd(parf + 0x35c) != 0) {
		pc_set("slv size v2 hi: 0", 0x35c, parf + 0x35c, 0);
	}
	id = pc_rd(core);
	if (id == 0 || id == 0xffffffff) {
		print("pcie:   the core still reads %x: it is not decoding its own config space\n", (uint64_t)id);
		return;
	}
	command = pc_rd(core + 0x04);
	class = pc_rd(core + 0x08);
	bus = pc_rd(core + 0x18);
	link = pc_rd(core + 0x710);
	misc = pc_rd(core + 0x8bc);
	print("pcie:   command %x, class %x, bus %x, port link control %x (%d lanes), misc %x\n",
	    (uint64_t)command, (uint64_t)class, (uint64_t)bus, (uint64_t)link, (uint64_t)((link >> 16) & 0x3f),
	    (uint64_t)misc);
	pc_set("misc control 1: ro on", 0x8bc, core + 0x8bc, misc | 1);
	pc_set("bus numbers", 0x18, core + 0x18, (bus & 0xff000000) | 0x00ff0100);
	pc_set("command", 0x04, core + 0x04, (command & 0xffff0000) | 0x107);
	pc_set("class: pci bridge", 0x08, core + 0x08, (class & 0xffff) | 0x06040000);
	pc_set("misc control 1: ro off", 0x8bc, core + 0x8bc, misc);
}

/* 9: the four writes the vendor makes that the kernel's S1 had to learn one at
 * a time: the Qactive veto (0x2c4 bit 0, with bit 8), LANE_SKEW_OFF (0x714
 * bit 5), the gen3/gen2 field at 0x80c bits 12:8, and INT_ALL_MASK (0x22c). */
static void
pc_extras(void)
{
	uint64_t parf = PC_PARF_PHYS, core = PC_CORE_PHYS;
	uint32_t value, misc;

	value = pc_rd(parf + 0x2c4);
	print("pcie: 9: parf cfg bits 3 %x (qactive veto %d), lane skew %x, gen3/gen2 %x, int mask %x\n",
	    (uint64_t)value, (uint64_t)(value & 1), (uint64_t)pc_rd(core + 0x714), (uint64_t)pc_rd(core + 0x80c),
	    (uint64_t)pc_rd(parf + 0x22c));
	pc_set("qactive veto off", 0x2c4, parf + 0x2c4, value & ~1u);
	pc_set("cfg bits 3: bit 8", 0x2c4, parf + 0x2c4, pc_rd(parf + 0x2c4) | (1u << 8));
	pc_set("lane skew off: on", 0x714, core + 0x714, pc_rd(core + 0x714) | (1u << 5));
	value = pc_rd(core + 0x80c);
	pc_set("gen3/gen2: field to 1", 0x80c, core + 0x80c, (value & ~0x1f00u) | 0x100);
	misc = pc_rd(core + 0x8bc);
	pc_wr(core + 0x8bc, misc | 1);
	pc_set("max read request size 0", 0x78, core + 0x78, pc_rd(core + 0x78) & ~0x7000u);
	pc_wr(core + 0x8bc, misc);
	pc_set("aux clock frequency 20 MHz", 0xb40, core + 0xb40, 20);
	pc_set("interrupt mask: nothing masked", 0x22c, parf + 0x22c, 0);
	/* The DT gives qcom,pcie-clkreq-offset = 0x2c48 and a qcom,pcie-sm-seq,
	 * so the vendor routes the clkreq pin and the l1ss timeout to the state
	 * machine instead of the legacy path: PARF_CESTA_CLKREQ_SEL (bit 0) and
	 * PARF_CESTA_L1SUB_TIMEOUT_EXT_INT_EN (bit 3). ios-dodge writes 9 here
	 * after the phy tables and before the endpoint's reset is released. */
	print("pcie: 9: cesta route %x at parf +2c48\n", (uint64_t)pc_rd(parf + 0x2c48));
	pc_set("cesta route: clkreq + l1ss to", 0x2c48, parf + 0x2c48,
	    pc_rd(parf + 0x2c48) | 9u);
}

/* The window the DT names in qcom,pcie-sm-debug, in that order: 0x1040 is
 * PWR_CTRL, 0x1048 PWR_MASK, 0x1090 the run bit, and 0x1094 to 0x109c the
 * machine's own program counter and status. */
static void
pc_sm_dump(const char *when)
{
	static const uint32_t at[14] = { 0x1040, 0x1048, 0x1050, 0x1058, 0x1060, 0x1068,
		0x1070, 0x1078, 0x1080, 0x1088, 0x1090, 0x1094, 0x1098, 0x109c };
	uint32_t v[14];

	for (unsigned i = 0; i < 14; i++) {
		v[i] = pc_rd(PC_SM_PHYS + at[i]);
	}
	print("pcie: 10: sm %s: %x %x %x %x %x %x %x / %x %x %x %x %x %x %x\n", when,
	    (uint64_t)v[0], (uint64_t)v[1], (uint64_t)v[2], (uint64_t)v[3], (uint64_t)v[4],
	    (uint64_t)v[5], (uint64_t)v[6], (uint64_t)v[7], (uint64_t)v[8], (uint64_t)v[9],
	    (uint64_t)v[10], (uint64_t)v[11], (uint64_t)v[12], (uint64_t)v[13]);
}

/* 10: the link's own state machine. The program goes at +0, the branches at
 * +0x1000, and the run bit is bit 0 of +0x1090 -- the vendor's
 * msm_pcie_cesta_load_sm_seq() order, which the kernel's S1 also follows. */
static void
pc_sm(void)
{
	static const uint32_t sequence[13] = {
		0x1c018081, 0x70074002, 0x50028000, 0x28007003, 0x80804002, 0x70021c01, 0x18001802,
		0x70005000, 0x10004000, 0x80814002, 0x18001c01, 0x1c018080, 0x100
	};
	static const uint32_t branches[7] = { 0x04, 0x1c, 0x24, 0x2c, 0x00, 0x00, 0x00 };
	uint64_t sm = PC_SM_PHYS;
	uint32_t same = 0, run;

	print("pcie: 10: the state machine at %x: program, branches, then the run bit\n", sm);
	for (unsigned i = 0; i < 13; i++) {
		pc_wr(sm + 4 * i, sequence[i]);
		same += pc_rd(sm + 4 * i) == sequence[i];
	}
	for (unsigned i = 0; i < 7; i++) {
		pc_wr(sm + 0x1000 + 4 * i, branches[i]);
	}
	run = pc_rd(sm + 0x1090);
	/* The kernel's command line names this block:
	 * pci-msm-drv.pcie_sm_regs=0x1D07000,0x1040,0x1048,0x3000,0x2 -- base,
	 * PWR_CTRL, PWR_MASK, instance stride, two instances. The vendor sets bit
	 * 0 of both for every instance at module_init and clears both for the
	 * instance it is about to program, right before loading the sequence:
	 * that bit is the software override of the machine's power handshake.
	 * This firmware never cleared it, so print what the boot loader left,
	 * hand the handshake back, and watch the machine's own counter. */
	pc_sm_dump("left");
	pc_set("sm pwr ctrl: cleared", 0x1040, sm + 0x1040, pc_rd(sm + 0x1040) & ~1u);
	pc_set("sm pwr mask: cleared", 0x1048, sm + 0x1048, pc_rd(sm + 0x1048) & ~1u);
	pc_wr(sm + 0x1090, run & ~1u);
	pc_wr(sm + 0x1090, run | 1);
	pc_sm_dump("started");
	{
		uint32_t pc0 = pc_rd(sm + 0x1094), pc1 = pc0;
		unsigned i;

		for (i = 0; i < 50; i++) {
			pc_ms(1);
			pc1 = pc_rd(sm + 0x1094);
			if (pc1 != pc0) {
				print("pcie: 10: sm moved %x -> %x after %d ms\n", (uint64_t)pc0,
				    (uint64_t)pc1, (uint64_t)(i + 1));
				break;
			}
		}
		if (pc1 == pc0) {
			print("pcie: 10: sm did not move in 50 ms, counter %x\n", (uint64_t)pc0);
		}
	}
	pc_sm_dump("50 ms");
	print("pcie: 10: %d of 13 program words read back, run bit %x\n", (uint64_t)same,
	    (uint64_t)pc_rd(sm + 0x1090));
	(void)pc_pipe_probe("after the state machine");
	pc_measure("pcie_0_pipe_clk, ltssm running", 0x14fu);
	pc_measure("gcc_pcie_0_pipe_clk, ltssm running", 0x14du);
}

/* 11: the endpoint's reset, then training.
 *
 * The vendor releases PERST and waits one millisecond (PERST_PROPAGATION_DELAY
 * 1000-1005 us) before setting PARF_LTSSM bit 8, with one read-modify-write and
 * no edge. Everything after that is polling: the state is in PARF_LTSSM bits
 * 5:0, not in the ELBI, whose status register is not wired to the LTSSM on this
 * generation. */
/* One fixed budget of LTSSM reads: detect.active pulses every 12 ms, and the
 * highest state seen. Anything above 1 means a receiver answered. */
static uint32_t
pc_window(const char *tag)
{
	uint64_t parf = PC_PARF_PHYS;
	uint32_t i, pulses = 0, last = 0xffffffff, top = 0;

	for (i = 0; i < 250000; i++) {
		uint32_t now = pc_rd(parf + 0x1b0) & 0x3f;

		if (now == 1 && last != 1)
			pulses++;
		if (now > top)
			top = now;
		last = now;
	}
	print("pcie: 11: window %s: %d reads, top %x, pulses %d, clkreq %x, override %x, route %x, pm %x\n",
	    tag, (uint64_t)i, (uint64_t)top, (uint64_t)pulses,
	    (uint64_t)(pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_CLKREQ + 4) & 1),
	    (uint64_t)pc_rd(parf + 0x2b0), (uint64_t)pc_rd(parf + 0x2c48),
	    (uint64_t)pc_rd(parf + 0x24));
	return top;
}

/* The pad's level as the gpio block sees it: while the pad is muxed to the
 * controller its gpio input is disconnected, so mux it to gpio with a pull
 * up, read, and put the board's mux back. */
static uint32_t
pc_pin_probe(uint32_t pin, const char *what)
{
	uint64_t base = PC_TLMM_PHYS + 0x1000 * pin;
	uint32_t cfg = pc_rd(base), level;

	pc_wr(base, 0x3);		/* gpio, input, pull up */
	pc_ms(2);
	level = pc_rd(base + 4) & 1;
	pc_wr(base, cfg);
	print("pcie: 11: %s: pin %d level %d, config %x\n", what, (uint64_t)pin,
	    (uint64_t)level, (uint64_t)cfg);
	return level;
}


/* ---- the PMIC itself, over the SPMI arbiter ----------------------------- */
/* The rails above are votes on the always-on processor's sequencer: its
 * "completed" answer only says the command was taken, not that a regulator is
 * on. The PMIC's own registers do say that, and the radio's sleep clock is one
 * of them: PMK8550 gpio3, which Android's PMIC pinctrl driver turns into a
 * 32.768 kHz output and which nothing configures when the phone boots through
 * this loader. Layouts: spmi-pmic-arb v7 (core 0xc400000, cnfg 0xc42d000,
 * observer 0xc440000, system 0), pinctrl-spmi-gpio (mode 0x40, pull 0x41,
 * function source 0x44, master enable 0x46), regulator (type 0x04, status 0x08
 * bit 7 in regulation, voltage 0x40/0x41 in mV, enable 0x46 bit 7). */
#define PC_ARB_CORE   0x0c400000UL
#define PC_ARB_CNFG   0x0c42d000UL
#define PC_ARB_OBS    0x0c440000UL
#define PC_ARB_MAP(n) (0x2000 + 4 * (n))
#define PC_ARB_CH(n)  (0x20 * (n))
#define PC_OBS_CMD    0x00
#define PC_OBS_STATUS 0x08
#define PC_OBS_DATA0  0x18
#define PC_OBS_DATA1  0x1c

/* One transfer. bytes != 0 reads count (1 to 8) registers from reg on; bytes
 * == 0 writes value to reg. Returns 0, -1 when the arbiter never answered,
 * -2 when it refused or the transfer failed. */
static int
pc_spmi(uint32_t channel, uint8_t reg, uint32_t value, uint8_t *bytes, uint32_t count)
{
	uint64_t ch = PC_ARB_OBS + PC_ARB_CH(channel), spin;
	uint32_t status = 0;

	if (bytes != 0) {
		pc_wr(ch + PC_OBS_CMD, (1u << 27) | (uint32_t)reg << 4 | (count - 1));
	} else {
		pc_wr(ch + PC_OBS_DATA0, value);
		pc_wr(ch + PC_OBS_CMD, (uint32_t)reg << 4 | (count - 1));
	}
	for (spin = 0; spin < 5000; spin++) {
		status = pc_rd(ch + PC_OBS_STATUS);
		if (status & 1u) {
			break;
		}
	}
	if (!(status & 1u)) {
		return -1;
	}
	if (status & 0xeu) {
		return -2;
	}
	if (bytes != 0) {
		uint32_t first = pc_rd(ch + PC_OBS_DATA0);
		uint32_t second = count > 4 ? pc_rd(ch + PC_OBS_DATA1) : 0;
		uint32_t i;

		for (i = 0; i < count; i++) {
			bytes[i] = (uint8_t)((i < 4 ? first : second) >> (8 * (i % 4)));
		}
	}
	return 0;
}

/* The arbiter's channel of the peripheral with this PPID, or -1. */
static int
pc_spmi_channel(uint32_t ppid, uint32_t *owner)
{
	uint32_t apids = pc_rd(PC_ARB_CORE + 4) & 0x7ff, n;

	if (apids == 0 || apids > 1024) {
		return -1;
	}
	for (n = 0; n < apids; n++) {
		uint32_t map = pc_rd(PC_ARB_CORE + PC_ARB_MAP(n));

		if (!map || ((map >> 8) & 0xfff) != ppid) {
			continue;
		}
		*owner = pc_rd(PC_ARB_CNFG + 4 * n) & 7;
		return (int)n;
	}
	return -1;
}

/* Every SMPS and LDO the arbiter knows, read the way the regulator driver
 * would: what it is, whether it is in regulation, and at what voltage. */
static int
pc_rail_wanted(uint32_t mv)
{
	static const uint32_t want[] = {1800, 1200, 940, 976, 915, 1316, 1864};
	unsigned i;

	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		uint32_t d = mv > want[i] ? mv - want[i] : want[i] - mv;

		if (d <= 4) {
			return 1;
		}
	}
	return 0;
}

static void
pc_pmic_survey(const char *when)
{
	uint32_t apids = pc_rd(PC_ARB_CORE + 4) & 0x7ff, n, answered = 0, found = 0, wanted = 0;

	print("pcie: 6b: the pmic's own view %s (%d apids)\n", when, (uint64_t)apids);
	if (apids == 0 || apids > 1024) {
		print("pcie: 6b: the arbiter reports no usable apids\n");
		return;
	}
	/* Only the rails this loader votes are printed: the whole table is 46
	 * lines and the staged copy of the log has no room for it. */
	for (n = 0; n < apids; n++) {
		uint32_t map = pc_rd(PC_ARB_CORE + PC_ARB_MAP(n));
		uint32_t ppid, owner, mv;
		uint8_t type = 0, st = 0, en = 0, lo = 0, hi = 0;

		if (!map) {
			continue;
		}
		ppid = (map >> 8) & 0xfff;
		owner = pc_rd(PC_ARB_CNFG + 4 * n) & 7;
		if (pc_spmi(n, 0x04, 0, &type, 1) != 0) {
			continue;
		}
		answered++;
		if (type != 0x03 && type != 0x04) {
			continue;
		}
		found++;
		(void)pc_spmi(n, 0x08, 0, &st, 1);
		(void)pc_spmi(n, 0x46, 0, &en, 1);
		(void)pc_spmi(n, 0x40, 0, &lo, 1);
		(void)pc_spmi(n, 0x41, 0, &hi, 1);
		mv = ((uint32_t)hi << 8) | lo;
		if (!(en & 0x80) || !pc_rail_wanted(mv)) {
			continue;
		}
		print("pcie: 6b:   %x %s %d mV enabled, %s (owner %d)\n", (uint64_t)ppid,
		    type == 0x03 ? "smps" : "ldo", (uint64_t)mv,
		    (st & 0x80) ? "in regulation" : "not in regulation", (uint64_t)owner);
		wanted++;
	}
	print("pcie: 6b: %d answered of %d, %d regulators, %d enabled at a voted voltage\n",
	    (uint64_t)answered, (uint64_t)apids, (uint64_t)found, (uint64_t)wanted);
	flush_log();
}

/* PMK8550 gpio3 (dt: alt_sleep_clk, function func1, output, no bias) is the
 * radio's 32.768 kHz sleep clock. After a boot through this loader it is an
 * enabled input with no source, so the WCN7850's PMU has no sleep clock at
 * all; K90 records the same state and sets this pin from its own power code. */
static void
pc_pmic_sleep_clock(void)
{
	static const uint8_t regs[10] = {0x04, 0x05, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x4a};
	uint8_t r[10] = {0};
	uint32_t page, channel = 0xffffffff, owner = 0, i;
	int s1, s2, s3;

	for (page = 0xb8; page <= 0xbf; page++) {
		uint32_t ppid = page, ch;
		uint8_t type = 0;
		int rc;

		for (ch = 0; ch < 1024; ch++) {
			uint32_t map = pc_rd(PC_ARB_CORE + PC_ARB_MAP(ch));

			if (map && ((map >> 8) & 0xfff) == ppid) {
				break;
			}
		}
		if (ch == 1024) {
			continue;
		}
		rc = pc_spmi(ch, 0x04, 0, &type, 1);
		print("pcie: 6b: page %x channel %d type %x %s\n", (uint64_t)ppid,
		    (uint64_t)ch, (uint64_t)type, rc == 0 ? "" : "(no answer)");
		if (ppid == 0xbb) {
			channel = ch;
			owner = pc_rd(PC_ARB_CNFG + 4 * ch) & 7;
		}
	}
	if (channel == 0xffffffff) {
		print("pcie: 6b: no channel for pmk8550 gpio3 (page bb of sid 0)\n");
		return;
	}
	for (i = 0; i < sizeof(regs); i++) {
		(void)pc_spmi(channel, regs[i], 0, &r[i], 1);
	}
	print("pcie: 6b: gpio3 type %x subtype %x, mode %x, pull %x, source %x, enable %x, power %x (owner %d)\n",
	    (uint64_t)r[0], (uint64_t)r[1], (uint64_t)r[2], (uint64_t)r[3], (uint64_t)r[6],
	    (uint64_t)r[8], (uint64_t)r[9], (uint64_t)owner);
	if ((r[0] & 0x1f) != 0x10) {
		print("pcie: 6b: page bb is not a gpio (type %x), left alone\n", (uint64_t)r[0]);
		return;
	}
	if ((r[8] & 0x80) && (r[2] & 3) == 1 && (r[6] & 0xf) == 2) {
		print("pcie: 6b: the sleep clock already runs as an output, function 1\n");
		return;
	}
	/* The first write repeats the enable register's current value, so a wrong
	 * channel path cannot change the PMIC: if it does not read back, nothing
	 * else is written. */
	/* The write channel (chnls) aborts on access from here (esr 96000010
	 * at 0xc56e010), so this stays read only and the pin keeps the state
	 * it was found in. */
	print("pcie: 6b: the sleep clock stays as found; chnls is not reachable\n");
	for (i = 0; i < sizeof(regs); i++) {
		(void)pc_spmi(channel, regs[i], 0, &r[i], 1);
	}
	print("pcie: 6b: gpio3 now mode %x, source %x, enable %x\n",
	    (uint64_t)r[2], (uint64_t)r[6], (uint64_t)r[8]);
	flush_log();
}

/* 11: the link, in the order the two working WCN7850 ports use (sheng's
 * WifiProbe on SM8550, K90's mhi_pcie_power on SM8750): the root port is
 * configured while PERST# is held low, PERST# stays low for 100 ms after the
 * power-up, then one rising edge, a millisecond, and the LTSSM. The vendor
 * state machine (0x1d07000) is not used: neither of those ports runs it. */
static uint32_t
pc_link(void)
{
	uint64_t parf = PC_PARF_PHYS, core = PC_CORE_PHYS;
	uint32_t state = 0xffffffff, capability = 0, status = 0, at, ms;

	for (at = pc_rd(core + 0x34) & 0xfc; capability == 0 && at >= 0x40 && at <= 0xfc; ) {
		uint32_t word = pc_rd(core + at);

		if ((word & 0xff) == 0x10) {
			capability = at;
			break;
		}
		at = (word >> 8) & 0xfc;
	}
	print("pcie: 11: ltssm %x (state %d), capability %x, link status %x\n",
	    (uint64_t)pc_rd(parf + 0x1b0), (uint64_t)(pc_rd(parf + 0x1b0) & 0x3f),
	    (uint64_t)capability,
	    (uint64_t)(capability ? pc_rd(core + capability + 0x10) >> 16 : 0));
	pc_set("device type: RC", 0x1000, parf + 0x1000, 4);
	/* The vendor enables the master wakeup and disables core clock gating in
	 * sys ctrl; the boot loader leaves both off. */
	pc_set("parf sys: master wakeup, core cgc off", 0x0, parf,
	    pc_rd(parf) | (1u << 1) | (1u << 6));
	/* With no state manager running the clkreq signal has to go to the
	 * controller, not to cesta, or the controller never sees it. */
	pc_set("cesta route: clkreq to parf", 0x2c48, parf + 0x2c48,
	    pc_rd(parf + 0x2c48) & ~1u);
	/* cnss2 drives bt-en low whenever the link will not come up. */
	pc_set("pin 17 bt-en config: output", 0, PC_TLMM_PHYS + 0x1000 * 17, 0x2c0);
	pc_set("pin 17 bt-en io: low", 0, PC_TLMM_PHYS + 0x1000 * 17 + 4, 0);
	/* Stock's config space: link control width field 3 (x2), link
	 * capabilities max width 2, link control 2 Gen1 first (K90 trains at
	 * Gen1 before it asks for more). */
	pc_set("link width: x2 like stock", 0x710, core + 0x710,
	    (pc_rd(core + 0x710) & ~(0x3fu << 16)) | (3u << 16));
	pc_set("dbi misc: writes allowed", 0x8bc, core + 0x8bc,
	    pc_rd(core + 0x8bc) | 1u);
	pc_set("link cap: max width 2", 0x7c, core + 0x7c,
	    (pc_rd(core + 0x7c) & ~(0x3fu << 4)) | (2u << 4));
	pc_set("link ctrl 2: target gen2 like stock", 0xa0, core + 0xa0,
	    (pc_rd(core + 0xa0) & ~0xfu) | 2u);
	/* Stock's gen3 related register has bit 0 clear; ours inherits it set.
	 * air166aq cleared it, and that is the only build that probed. */
	pc_set("gen3 related: zrxdc compliant", 0x890, core + 0x890,
	    pc_rd(core + 0x890) & ~1u);
	pc_set("gen3/gen2: field to 1 like stock", 0x80c, core + 0x80c,
	    (pc_rd(core + 0x80c) & ~(0xfu << 16)) | (1u << 16));
	pc_set("root port bus numbers", 0x18, core + 0x18, 0x00010100u);
	pc_set("root port command: memory, bus master", 0x4, core + 0x4,
	    (pc_rd(core + 0x4) & 0xffffu) | 6u);
	pc_set("dbi misc: read only again", 0x8bc, core + 0x8bc,
	    pc_rd(core + 0x8bc) & ~1u);
	/* qcom_ep_reset_deassert: PERST# low for at least 100 ms after the module
	 * is powered, then high, then the LTSSM a millisecond later. */
	/* One rising edge, from the level stage 4 left asserted: the RC pulses
	 * detect.active again only when PERST# has been high for a while before
	 * the LTSSM starts, which is what air166aq did (180 ms) and air167 lost. */
	pc_set("pin 94 perst io: high", 0, PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4, 2);
	pc_ms(1);
	/* bit 0 of the io word is the level on the pad, bit 1 what is driven:
	 * if the line does not read high, the module never sees PERST# release. */
	print("pcie: 11: perst pad config %x io %x (level %d)\n",
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST),
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4),
	    (uint64_t)(pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4) & 1));
	pc_ms(179);
	print("pcie: 11: perst pad after 180 ms io %x (level %d)\n",
	    (uint64_t)pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4),
	    (uint64_t)(pc_rd(PC_TLMM_PHYS + 0x1000 * PC_PIN_PERST + 4) & 1));
	pc_set("mhi clock reset: bit 4", 0x174, parf + 0x174,
	    pc_rd(parf + 0x174) | (1u << 4));
	pc_set("parf ltssm: enable", 0x1b0, parf + 0x1b0,
	    pc_rd(parf + 0x1b0) | PC_LTSSM_EN);
	for (ms = 0; ms < 1000; ms++) {
		state = pc_rd(parf + 0x1b0) & 0x3f;
		if (ms == 1 || ms == 10 || ms == 100 || ms == 500) {
			print("pcie: 11: +%d ms: ltssm %x %s, port debug0 %x, debug1 %x, link %x\n",
			    (uint64_t)ms, (uint64_t)state, pc_ltssm_name(state),
			    (uint64_t)pc_rd(core + 0x728), (uint64_t)pc_rd(core + 0x72c),
			    (uint64_t)pc_rd(core + 0x80));
		}
		if (state == 0x11 && (pc_rd(core + 0x72c) & (1u << 4))) {
			break;
		}
		pc_ms(1);
	}
	if (capability != 0) {
		status = pc_rd(core + capability + 0x10) >> 16;
	}
	print("pcie: 11: after %d ms: ltssm %x %s, link status %x (up %d, speed %d, width %d)\n",
	    (uint64_t)ms, (uint64_t)state, pc_ltssm_name(state), (uint64_t)status,
	    (uint64_t)((status >> 13) & 1), (uint64_t)(status & 0xf),
	    (uint64_t)((status >> 4) & 0x3f));
	print("pcie: 11: parf refclk %x, phy +214 %x (ready %d), parf 1b0 %x\n",
	    (uint64_t)pc_rd(parf + 0x4c), (uint64_t)pc_rd(parf + PC_PHY_AT + 0x214),
	    (uint64_t)((pc_rd(parf + PC_PHY_AT + 0x214) >> 6) & 1),
	    (uint64_t)pc_rd(parf + 0x1b0));
	return state;
}


/* 12: only with the link up, the endpoint's own register space. The outbound
 * iATU is given the vendor's CFG0 window to bus 1 device 0 and the DT's MEM32
 * aperture; the identity at 0x60100000 is what says whether the radio answered
 * -- and it needs no oscilloscope to be believed. */
static int
pc_iatu_region(unsigned region, uint32_t type, uint32_t host, uint32_t limit, uint32_t target)
{
	static const uint32_t regs[7] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18 };
	uint32_t values[7] = { type, 0x80000000u, host, 0, limit, target, 0 };
	uint64_t at = PC_CORE_PHYS + 0x1000 + region * 0x200;

	pc_wr(at + 0x04, 0);
	for (unsigned i = 0; i < 7; i++) {
		pc_wr(at + regs[i], values[i]);
	}
	pc_wr(at + 0x04, 0x80000000u);
	for (unsigned i = 0; i < 7; i++) {
		if (pc_rd(at + regs[i]) != values[i]) {
			print("pcie: 12: region %d +%x reads %x, not %x\n", (uint64_t)region, (uint64_t)regs[i],
			    (uint64_t)pc_rd(at + regs[i]), (uint64_t)values[i]);
			return 0;
		}
	}
	return 1;
}

static void
pc_identity(void)
{
	uint64_t ecam = PC_ECAM_PHYS;
	uint32_t identity, class, bar0;

	print("pcie: 12: the iATU: cfg0 to bus 1 device 0, mem32 to %x\n", PC_MEM32_PHYS);
	if (!pc_iatu_region(0, 0x4, (uint32_t)ecam, (uint32_t)(ecam + 0xfff), 0x01000000)) {
		return;
	}
	if (!pc_iatu_region(1, 0x0, (uint32_t)PC_MEM32_PHYS,
	    (uint32_t)(PC_MEM32_PHYS + PC_MEM32_SIZE - 1), (uint32_t)PC_MEM32_PHYS)) {
		return;
	}
	identity = pc_rd(ecam);
	class = pc_rd(ecam + 8);
	bar0 = pc_rd(ecam + 0x10);
	print("pcie: 12: endpoint id %x (vendor %x, device %x), class %x, bar0 %x\n", (uint64_t)identity,
	    (uint64_t)(identity & 0xffff), (uint64_t)(identity >> 16), (uint64_t)class, (uint64_t)bar0);
	if (identity == 0xffffffff || (identity & 0xffff) != 0x17cb) {
		print("pcie: 12: no endpoint answered: the link is not carrying it\n");
		return;
	}
	print("pcie: 12: THE RADIO ANSWERED (%x): the link is up and its config space is readable\n",
	    (uint64_t)identity);
	if (bar0 != 0 && bar0 != 0xffffffff && !(bar0 & 1) && (bar0 & 6) == 0) {
		uint32_t bar = bar0 & ~0xfu;

		if (bar >= PC_MEM32_PHYS && bar <= PC_MEM32_PHYS + PC_MEM32_SIZE - 4) {
			print("pcie: 12: bar0 %x reads %x\n", (uint64_t)bar,
			    (uint64_t)pc_rd(PC_MEM32_PHYS + (bar - (uint32_t)PC_MEM32_PHYS)));
		}
	}
}

static void pc_clocks_again_note(void);

static void
pc_bringup(void)
{
	uint32_t ltssm;
	int core_up, phy_up;

	print("\npcie: PCIe0 for the WCN7850: the kernel's S1, run in the firmware instead\n");
	pc_dump("before");
	pc_pins();
	pc_clocks();
	core_up = pc_gdsc("pcie_0_gdsc", PC_GCC_GDSCR, 0);
	phy_up = pc_gdsc("pcie_0_phy_gdsc", PC_GCC_PHY_GDSCR, 3);
	if (!core_up || !phy_up) {
		print("pcie: the power domains did not come up (%d/%d): parf, the phy and the link are left alone\n",
		    (uint64_t)core_up, (uint64_t)phy_up);
		return;
	}
	pc_clocks_again_note();
	pc_resets();
	pc_radio();
	pc_pmic_survey("after the rails");
	pc_pmic_sleep_clock();
	pc_clkref();
	pc_phy();
	pc_dbi();
	pc_extras();
	pc_sm();
	ltssm = pc_link();
	if (ltssm == 0x11) {
		pc_identity();
	} else {
		print("pcie: the link did not reach L0, so nothing behind it is read\n");
	}
	print("pcie: done: ltssm %x %s\n", (uint64_t)ltssm, pc_ltssm_name(ltssm));
}


/* The same sequence as pc_bringup(), cut at stage n. Every stage redoes the ones
 * before it, so "oem pcie-stage 3" is "1, 2 and 3" and a reset names the stage
 * that did it: the reply for the stage that reset the phone never arrives. */
static void
pc_stage(uint32_t upto)
{
	uint32_t ltssm;

	/* What the boot loader handed over, before this round touches anything. */
	print("pcie: 0: left parf +0 %x, +20 %x, +40 %x, +4c %x, +1b0 %x, +1000 %x\n",
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x0), (uint64_t)pc_rd(PC_PARF_PHYS + 0x20),
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x40), (uint64_t)pc_rd(PC_PARF_PHYS + 0x4c),
	    (uint64_t)pc_rd(PC_PARF_PHYS + 0x1b0), (uint64_t)pc_rd(PC_PARF_PHYS + 0x1000));
	print("pcie: 0: left phy +200 %x, +214 %x, +240 %x, +244 %x, +11e0 %x, +1200 %x\n",
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x200),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x214),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x240),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x244),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x11e0),
	    (uint64_t)pc_rd(PC_PARF_PHYS + PC_PHY_AT + 0x1200));
	print("pcie: 0: left gcc pipes %x, pipe mux %x, rchng %x, gdsc %x %x\n",
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x52008), (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b070),
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b058), (uint64_t)pc_rd(PC_GCC_PHYS + 0x6b004),
	    (uint64_t)pc_rd(PC_GCC_PHYS + 0x6c000));
	print("\npcie: stages 1..%d (1 pins, 2 clocks, 3 core gdsc, 4 phy gdsc, 5 resets,\n",
	    (uint64_t)upto);
	print("pcie:         6 rails, 7 clkref, 8 phy, 9 dbi, 10 link)\n");
	if (upto >= 1) {
		pc_pins();
	}
	if (upto >= 2) {
		pc_clocks();
	}
	if (upto >= 3 && !pc_gdsc("pcie_0_gdsc", PC_GCC_GDSCR, 0)) {
		print("pcie: the core's power domain did not come up\n");
		return;
	}
	if (upto >= 4 && !pc_gdsc("pcie_0_phy_gdsc", PC_GCC_PHY_GDSCR, 3)) {
		print("pcie: the phy's power domain did not come up\n");
		return;
	}
	if (upto >= 5) {
		pc_resets();
	}
	if (upto >= 6) {
		pc_radio();
		pc_pmic_survey("after the rails");
		pc_pmic_sleep_clock();
	}
	if (upto >= 7) {
		pc_clkref();
	}
	if (upto >= 8) {
		pc_phy();
	}
	if (upto >= 9) {
		pc_dbi();
		pc_extras();
		pc_sm();
		}
	if (upto >= 10) {
		ltssm = pc_link();
		print("pcie: ltssm %x %s\n", (uint64_t)ltssm, pc_ltssm_name(ltssm));
	}
	pc_dump("after");
}

/* The clock registers again, now that both power domains are up: before that a
 * halt bit proves nothing, because a clock whose domain has no power is halted
 * whatever its branch vote says. */
static void
pc_clocks_again_note(void)
{
	uint64_t gcc = PC_GCC_PHYS;
	static const uint32_t halt[] = { 0x6b01c, 0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048, 0x6b054, 0x10058 };
	uint32_t halted = 0;

	for (unsigned i = 0; i < sizeof(halt) / sizeof(halt[0]); i++) {
		halted |= pc_rd(gcc + halt[i]) & 1;
	}
	print("pcie: 2b: with both domains up: vote %x/%x, a halt bit is %s\n",
	    (uint64_t)pc_rd(gcc + PC_GCC_VOTE), (uint64_t)pc_rd(gcc + PC_GCC_VOTE_RCH),
	    halted ? "still set (that clock's source is not running)" : "clear");
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap") || starts_with(cmd, "oem pcie"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x04f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
			}
			if (got == total) {
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem pcie")) {
			/* "oem pcie-state" only reads; "oem pcie-link" runs the sequence.
			 * Both "oem pcie-state" and "oem pcie state" are accepted, and a
			 * bare "oem pcie" is the read-only dump: the writing sequence must
			 * never be reached by a typo. */
			const char *arg = cmd + 8;

			while (*arg == ' ' || *arg == '-' || *arg == '_') {
				arg++;
			}
			if (PLAT == &platform_corvette) {
				pc_map_windows();
			}
			if (PLAT != &platform_corvette) {
				fb_reply("FAIL", "not a corvette");
			} else if (*arg == 0 || starts_with(arg, "state")) {
				pc_dump("now");
				fb_reply("OKAY", "");
			} else if (starts_with(arg, "gdsc")) {
				const char *num = arg + 4;
				uint32_t n = 0;

				while (*num == ' ' || *num == '=') {
					num++;
				}
				while (*num >= '0' && *num <= '9') {
					n = n * 10 + (uint32_t)(*num - '0');
					num++;
				}
				if (n < 1 || n > 8) {
					fb_reply("FAIL", "oem pcie-gdsc 1..8");
				} else {
					pc_gdsc_try(n);
					fb_reply("OKAY", "");
				}
			} else if (starts_with(arg, "stage")) {
				/* "oem pcie-stage 3" runs stages 1..3 and answers OKAY only if
				 * stage 3 came back: the first stage whose reply is missing is
				 * the one that reset the phone. */
				const char *num = arg + 5;
				uint32_t n = 0;

				while (*num == ' ' || *num == '=') {
					num++;
				}
				while (*num >= '0' && *num <= '9') {
					n = n * 10 + (uint32_t)(*num - '0');
					num++;
				}
				if (n < 1 || n > 10) {
					fb_reply("FAIL", "oem pcie-stage 1..10");
				} else {
					pc_stage(n);
					fb_reply("OKAY", "");
				}
			} else if (starts_with(arg, "rsc")) {
				pc_rsc_probe();
				fb_reply("OKAY", "");
			} else if (starts_with(arg, "link") || starts_with(arg, "up")) {
				pc_bringup();
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "oem pcie-state or oem pcie-link");
			}
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(PHONE_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	if (dma.control != 0) {
		print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
		return;
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = PHONE_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	restore_prev();
	print("== XNULOADER v3 ==\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);

	install_exception_hook();
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	reboot();
	return 0;
}
