/*
 * Darwin-on-hakios: the kernel's side of the Wi-Fi driver's object.
 *
 * The driver of the phone's radio is Linux's ath11k, compiled unmodified outside this tree on a
 * layer that gives it the kernel interfaces it expects, and tied into one object that the
 * kernel links when it is there (tools/mk-ath11k.sh and wifi/lx in the repository; the kernel
 * is built with that object when xnu-build.sh finds it). The object shows one name, lx_start(),
 * and asks for the calls of sys/hakios.lx.h, which are here: memory, time, threads, one lock
 * to sleep under, the kernel's options. What it asks of the radio itself -- its firmware's
 * files, its view of memory, the processor its firmware runs on -- is hakios.wifi.c's to
 * answer, which knows the phone.
 *
 * On the Xiaomi 14 Pro the object is ath12k's (tools/mk-ath12k.sh, wifi/lx12), and the phone's side is
 * shennong_wlan.c's (shennong_wlan_here()).
 *
 * A kernel built without the object has the lx_start() at the end of this file, which says so.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.net.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);

#define say hakios_wifi_say

/* shennong_wlan.c: the phone's side (the Xiaomi 14 Pro's radio) */
extern bool		 shennong_wlan_here(void);
extern bool		 shennong_wlan_radio_map(uint64_t physical, uint64_t size, unsigned long long *address);
extern const uint8_t	*shennong_wlan_driver_file(const char *name, uint64_t *size);
extern bool		 shennong_wlan_link_up(void);
extern int		 shennong_wlan_radio_address(uint8_t *address);

static LCK_GRP_DECLARE(lx_lock_group, "hakios-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);

/* ---- memory ------------------------------------------------------------------------------------------ */

static void *lx_piece;
static unsigned long lx_piece_size;

/*
 * (air226) a piece for the radio, asked for at the kernel's start: the same request made once the
 * system has started (lx_os_radio_memory) finds no continuous run left.
 */
bool
lx_os_radio_piece(unsigned long size)
{
	vm_offset_t address = 0;

	if (lx_piece != NULL) {
		return true;
	}
	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &address, size, PAGE_SIZE, 0, 0, 0, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("lx: no %lu bytes of memory in one piece at the start\n", size);
		return false;
	}
	bzero((void *)address, size);
	lx_piece = (void *)address;
	lx_piece_size = size;
	say("lx: %lu bytes of memory kept in one piece for the radio\n", size);
	return true;
}

void *
lx_os_alloc(unsigned long size)
{
	vm_offset_t address = 0;

	/*
	 * (air225) the radio must reach the driver's big blocks: their pages have to be physically
	 * in one piece.  kalloc_data gives no such promise and fails once the system has started.
	 */
	if (size >= (1UL << 20) &&
	    kmem_alloc_contig(kernel_map, &address, size, PAGE_SIZE, 0, 0, 0, VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
		bzero((void *)address, size);
		return (void *)address;
	}
	if (size >= (1UL << 20)) {
		say("lx: the VM has no %lu bytes of memory in one piece\n", size);
		return NULL;
	}
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
lx_os_free(void *memory, unsigned long size)
{
	if (size >= (1UL << 20)) {
		kmem_free(kernel_map, (vm_offset_t)memory, size);
	} else {
		kfree_data(memory, size);
	}
}

unsigned long long
lx_os_radio_reach(const void *memory, unsigned long size)
{
	vm_offset_t start = (vm_offset_t)memory, first = start & ~(vm_offset_t)4095, end = start + size;
	uint64_t physical = kvtophys(first);
	unsigned long long address = 0;

	if (size == 0 || physical == 0) {
		return 0;
	}
	for (vm_offset_t page = first + 4096; page < end; page += 4096) {
		if (kvtophys(page) != physical + (page - first)) {
			say("lx: %lu bytes of the heap the radio is to reach are not in one physical piece\n", size);
			return 0;
		}
	}
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < end; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (!(shennong_wlan_here() ? shennong_wlan_radio_map(physical, end - first, &address) :
	    hakios_wifi_radio_map(physical, end - first, &address))) {
		return 0;
	}
	return address + (start - first);
}

/*
 * In one physical piece, so that it can be mapped a second time without this processor's cache
 * in between (as hakios.adreno.c does with what it shares with the GPU); the first, cached
 * view is written out and never used again.
 */
void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;
	kern_return_t kr;

	size = round_page(size);
	if (lx_piece != NULL && size <= lx_piece_size) {
		/* air228: carve the front and keep the remainder -- the ath12k driver asks for a
		 * 7.27 MB piece and its pool for 4 MB pieces one after another; a single reserve of
		 * 32 MB serves them all only if it is carved, not consumed whole. */
		cached = (vm_offset_t)lx_piece;
		lx_piece = (void *)((uint8_t *)lx_piece + size);
		lx_piece_size -= size;
	} else {
		kr = kmem_alloc_contig(kernel_map, &cached, size, PAGE_SIZE, 0, 0, 0, VM_KERN_MEMORY_IOKIT);
		if (kr != KERN_SUCCESS) {
			say("lx: no %lu bytes of memory in one piece (%d)\n", size, kr);
			return NULL;
		}
	}
	physical = kvtophys(cached);
	bzero((void *)cached, size);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !(shennong_wlan_here() ? shennong_wlan_radio_map(physical, size, address) :
	    hakios_wifi_radio_map(physical, size, address))) {
		return NULL;
	}
	return (void *)uncached;
}

void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);

	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

/* ---- saying things ----------------------------------------------------------------------------------- */

/*
 * What the driver said (and what the phone's side adds, hakios_lx_keep()), kept as well, the last
 * 96 KB: the kernel's message buffer is overrun by others long before a run ends. On the way back
 * to the loader hakios_lx_replay() puts it into the loader's log, which is what reaches the host.
 */
extern void loader_log_putc(char c);
static LCK_MTX_DECLARE(lx_kept_lock, &lx_lock_group);
static char lx_kept[96 * 1024];
static size_t lx_kept_at;
static bool lx_kept_wrapped, lx_kept_any;

void
hakios_lx_keep(const char *text)
{
	lck_mtx_lock(&lx_kept_lock);
	lx_kept_any = true;
	for (const char *c = text; *c != 0; c++) {
		lx_kept[lx_kept_at] = *c;
		if (++lx_kept_at == sizeof(lx_kept)) {
			lx_kept_at = 0;
			lx_kept_wrapped = true;
		}
	}
	lck_mtx_unlock(&lx_kept_lock);
}
#define lx_keep hakios_lx_keep

/* (on the way out: nothing else runs, no lock is taken) */
void
hakios_lx_replay(void)
{
	size_t n = lx_kept_wrapped ? sizeof(lx_kept) : lx_kept_at, at = lx_kept_wrapped ? lx_kept_at : 0;

	if (!lx_kept_any) {
		return;
	}
	for (const char *c = "\n--- the Wi-Fi driver's lines, again (the last 96 KB) ---\n"; *c != 0; c++) {
		loader_log_putc(*c);
	}
	for (size_t i = 0; i < n; i++) {
		char c = lx_kept[(at + i) % sizeof(lx_kept)];

		if (c != 0) {
			loader_log_putc(c);
		}
	}
	for (const char *c = "\n--- that was all ---\n"; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

void
lx_os_print(const char *text)
{
	lx_keep(text);
	say("%s", text);
}

void
lx_os_tell(const char *text)
{
	lx_keep(text);
	hakios_wifi_tell("%s", text);
}

void
lx_os_panic(const char *text)
{
	panic("%s", text);
}

/* ---- time, threads, sleeping --------------------------------------------------------------------------- */

unsigned long long
lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
lx_os_thread_self(void)
{
	return current_thread();
}

struct lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct lx_thread start = *(struct lx_thread *)parameter;

	kfree_type(struct lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
lx_os_lock(void)
{
	lck_mtx_lock(&lx_lock);
}

void
lx_os_unlock(void)
{
	lck_mtx_unlock(&lx_lock);
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;

	if (nanoseconds == 0) {
		return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)address);
}

/* ---- the rest of the machine ------------------------------------------------------------------------- */

void
lx_os_random(void *buffer, unsigned long size)
{
	read_random(buffer, (u_int)size);
}

unsigned
lx_os_option(const char *name)
{
	return hakios_wifi_option(name);
}

int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	uint64_t length = 0;
	const uint8_t *file = shennong_wlan_here() ? shennong_wlan_driver_file(name, &length) : hakios_wifi_driver_file(name, &length);

	if (file == NULL) {
		return ENOENT;
	}
	*data = file;
	*size = (unsigned long)length;
	return 0;
}

int
lx_os_rproc_boot(void)
{
	return (shennong_wlan_here() ? shennong_wlan_link_up() : hakios_wifi_dsp_is_up()) ? 0 : ENXIO;
}

int
lx_os_radio_address(unsigned char *address)
{
	return shennong_wlan_here() ? shennong_wlan_radio_address(address) : hakios_wifi_radio_address(address);
}

int
lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
	return hakios_wifi_network(name, name_length, passphrase, passphrase_length);
}

/* ---- frames: the radio as the link behind en0 (sys/hakios.net.h) ------------------------------------ */

/*
 * A frame from the network stack comes on whatever thread sends it, and the driver's side keeps
 * a record of every thread that enters it: so frames are put in a queue here, and one thread of
 * ours takes them to the driver. A queue that is full drops what comes (TCP sends it again).
 */
#define LX_QUEUE        256

struct lx_frame {
	uint16_t length;
	uint8_t bytes[HAKIOS_NET_FRAME_MAX];
};

static bool lx_link_attached;
static LCK_MTX_DECLARE(lx_queue_lock, &lx_lock_group);
static struct lx_frame *lx_queue;
static uint32_t lx_queue_in, lx_queue_out;      /* frames put in and taken out, counted from the start */
static uint64_t lx_queue_dropped;
/* how sending goes, for lx_os_net_say(): time in the driver, the deepest the queue got, frames since the last word */
static uint64_t lx_send_ns, lx_send_most_ns;
static uint32_t lx_send_frames, lx_queue_deepest;

static void
lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		struct lx_frame *frame;

		lck_mtx_lock(&lx_queue_lock);
		while (lx_queue_in == lx_queue_out) {
			lck_mtx_sleep(&lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_queue_in, THREAD_UNINT);
		}
		frame = &lx_queue[lx_queue_out % LX_QUEUE];     /* (stays ours until lx_queue_out moves) */
		lck_mtx_unlock(&lx_queue_lock);
		uint64_t began = lx_os_nanoseconds(), took;
		lx_transmit(frame->bytes, frame->length);
		took = lx_os_nanoseconds() - began;
		lck_mtx_lock(&lx_queue_lock);
		lx_send_ns += took;
		lx_send_most_ns = took > lx_send_most_ns ? took : lx_send_most_ns;
		lx_send_frames++;
		lx_queue_out++;
		lck_mtx_unlock(&lx_queue_lock);
	}
}

static bool
lx_link_transmit(mbuf_t packet, size_t length)
{
	struct lx_frame *frame;

	lck_mtx_lock(&lx_queue_lock);
	if (length > HAKIOS_NET_FRAME_MAX || lx_queue_in - lx_queue_out == LX_QUEUE) {
		lx_queue_dropped++;
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame = &lx_queue[lx_queue_in % LX_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	lx_queue_in++;
	lx_queue_deepest = lx_queue_in - lx_queue_out > lx_queue_deepest ? lx_queue_in - lx_queue_out : lx_queue_deepest;
	lck_mtx_unlock(&lx_queue_lock);
	thread_wakeup((event_t)&lx_queue_in);
	return true;
}

static struct hakios_net_link lx_link = {
	.name = "the phone's radio (ath11k)",
	.wireless = true,
	.transmit = lx_link_transmit,
};

void
lx_os_net_attach(const unsigned char *address)
{
	thread_t thread;

	if (lx_link_attached) {
		return;
	}
	lx_queue = kalloc_data(LX_QUEUE * sizeof(*lx_queue), Z_WAITOK | Z_ZERO);
	if (lx_queue == NULL || kernel_thread_start(lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
		say("lx: no memory or no thread for the frames to send: the radio's frames go nowhere\n");
		return;
	}
	thread_set_thread_name(thread, "wifi-send");
	thread_deallocate(thread);
	memcpy(lx_link.address, address, sizeof(lx_link.address));
	lx_link_attached = hakios_net_attach(&lx_link);
	say("lx: %s\n", lx_link_attached ? "the radio is the link behind en0" : "EN0 HAS A LINK ALREADY: the radio's frames go "
	    "nowhere");
	lx_keep(lx_link_attached ? "lx: the radio is the link behind en0\n" : "lx: EN0 HAS A LINK ALREADY: the radio's frames go nowhere\n");
}

/*
 * With wifi=1 the interface is made when the kernel starts, before the radio is up: iOS's Wi-Fi
 * side looks for it and for its registry entry as soon as it runs, once, and a radio that
 * turns up twelve seconds later is one it has already failed to open.
 */
void
hakios_lx_net_early(void)
{
	uint8_t address[6];

	if (hakios_wifi_radio_address(address)) {
		lx_os_net_attach(address);
	}
}

void
lx_os_net_link(int up)
{
	char line[200];

	snprintf(line, sizeof(line), "lx: the network is %s: en0 %s; frames from the kernel so far %u, %llu dropped\n",
	    up ? "joined" : "left", lx_link_attached ? "is the radio's" : "IS NOT THE RADIO'S (its frames go nowhere)",
	    lx_queue_in, lx_queue_dropped);
	say("%s", line);
	lx_keep(line);
	if (lx_link_attached) {
		hakios_net_link_state(up != 0);
	}
}

/*
 * How sending went since the last word (called with the driver's word every ten seconds): the
 * frames, the time each took in the driver on average and at most, the deepest the queue got
 * (LX_QUEUE: a full queue drops) -- whether the kernel's frames wait for the driver, or the
 * driver for the kernel's frames.
 */
void
lx_os_net_say(void)
{
	uint64_t ns, most;
	uint32_t frames, deepest;
	uint64_t dropped;

	lck_mtx_lock(&lx_queue_lock);
	ns = lx_send_ns;
	most = lx_send_most_ns;
	frames = lx_send_frames;
	deepest = lx_queue_deepest;
	dropped = lx_queue_dropped;
	lx_send_ns = lx_send_most_ns = 0;
	lx_send_frames = lx_queue_deepest = 0;
	lck_mtx_unlock(&lx_queue_lock);
	if (frames != 0) {
		char line[200];

		snprintf(line, sizeof(line), "lx: sending: %u frames from the kernel, %llu us each in the driver (at most %llu); the "
		    "queue at most %u of %u deep; %llu dropped in all\n", frames, ns / frames / 1000, most / 1000, deepest, LX_QUEUE,
		    dropped);
		say("%s", line);
		lx_keep(line);
	}
}

void
lx_os_net_input(const unsigned char *frame, unsigned long length)
{
	if (lx_link_attached && length <= HAKIOS_NET_FRAME_MAX) {
		hakios_net_input(frame, length);
	}
}

/* ---- the radio's interrupts ------------------------------------------------------------------------------- */

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);

#define LX_VECTORS      32

static struct {
	void (*function)(void *);
	void *argument;
	bool attached;
} lx_vector[LX_VECTORS];
static uint32_t lx_irq_first;                   /* the GIC's number of vector 0; 0: no interrupts */
static uint32_t lx_irq_pending;                 /* the vectors that have fired and are not dealt with yet */
static uint32_t lx_irq_running;                 /* the vectors whose function the thread is in */
static thread_t lx_irq_thread;
static LCK_MTX_DECLARE(lx_irq_lock, &lx_lock_group);

/* In interrupt context: the vector masked, and the thread told. */
static void
lx_irq_arrived(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	os_atomic_or(&lx_irq_pending, 1u << (intid - lx_irq_first), relaxed);
	thread_wakeup((event_t)&lx_irq_pending);
}

static void
lx_irq_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint32_t pending;

		for (;;) {
			assert_wait((event_t)&lx_irq_pending, THREAD_UNINT);
			if ((pending = os_atomic_xchg(&lx_irq_pending, 0, acquire)) != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
		}
		for (unsigned v = 0; v < LX_VECTORS; v++) {
			if (!(pending & 1u << v)) {
				continue;
			}
			lck_mtx_lock(&lx_irq_lock);
			if (!lx_vector[v].attached) {
				lck_mtx_unlock(&lx_irq_lock);
				continue;
			}
			lx_irq_running |= 1u << v;
			lck_mtx_unlock(&lx_irq_lock);
			lx_vector[v].function(lx_vector[v].argument);
			lck_mtx_lock(&lx_irq_lock);
			lx_irq_running &= ~(1u << v);
			lck_mtx_unlock(&lx_irq_lock);
			thread_wakeup((event_t)&lx_irq_running);
		}
	}
}

int
lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
	uint32_t first = 0;

	if (shennong_wlan_here() || !hakios_wifi_radio_msi(address, data, &first)) {     /* (the Xiaomi 14 Pro: none yet) */
		return 0;
	}
	lx_irq_first = first;
	return 1;
}

int
lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
	kern_return_t kr;

	if (vector >= LX_VECTORS || lx_irq_first == 0 || function == NULL) {
		return EINVAL;
	}
	if (lx_irq_thread == THREAD_NULL) {
		hakios_wifi_tell("lx: wifi_irq: THE GIC'S DISTRIBUTOR IS WRITTEN for the radio's interrupts: each vector's SPI (%u + v) edge-"
		    "triggered, group 1, routed to this processor, then enabled\n", lx_irq_first);
		if (kernel_thread_start(lx_irq_runs, NULL, &lx_irq_thread) != KERN_SUCCESS) {
			lx_irq_thread = THREAD_NULL;
			return ENOMEM;
		}
		thread_set_thread_name(lx_irq_thread, "wifi-irq");
	}
	lck_mtx_lock(&lx_irq_lock);
	lx_vector[vector].function = function;
	lx_vector[vector].argument = argument;
	lx_vector[vector].attached = true;
	lck_mtx_unlock(&lx_irq_lock);
	if ((kr = gic_v3_spi_attach(lx_irq_first + vector, true, lx_irq_arrived, NULL)) != KERN_SUCCESS) {
		lck_mtx_lock(&lx_irq_lock);
		lx_vector[vector].attached = false;
		lck_mtx_unlock(&lx_irq_lock);
		return kr == KERN_NAME_EXISTS ? EBUSY : EINVAL;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	return 0;
}

void
lx_os_irq_enable(unsigned vector)
{
	if (vector < LX_VECTORS && lx_vector[vector].attached) {
		gic_v3_spi_unmask(lx_irq_first + vector);
	}
}

void
lx_os_irq_disable(unsigned vector)
{
	if (vector >= LX_VECTORS || !lx_vector[vector].attached) {
		return;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	if (current_thread() == lx_irq_thread) {
		return;                         /* (from the vector's own function, or another's: nothing to wait for) */
	}
	lck_mtx_lock(&lx_irq_lock);
	while (lx_irq_running & 1u << vector) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
}

void
lx_os_irq_detach_all(void)
{
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		if (lx_vector[v].attached) {
			gic_v3_spi_detach(lx_irq_first + v);
		}
	}
	lck_mtx_lock(&lx_irq_lock);
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		lx_vector[v].attached = false;
	}
	while (lx_irq_running != 0 && current_thread() != lx_irq_thread) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
	os_atomic_store(&lx_irq_pending, 0, relaxed);
}

unsigned long long
lx_os_irq_count(unsigned vector)
{
	return vector < LX_VECTORS && lx_irq_first != 0 ? gic_v3_spi_count(lx_irq_first + vector) : 0;
}

/* ---- the station (sys/hakios.sta.h) --------------------------------------------------------------------- */

void
lx_os_station(const struct hakios_sta_ops *ops)
{
	hakios_sta_attach(ops);
}

void
lx_os_station_lost(int error)
{
	hakios_sta_lost(error);
}

/* ---- the driver ---------------------------------------------------------------------------------------- */

/* The object has its own; this is the one of a kernel built without it. */
__attribute__((weak)) int
lx_start(void)
{
	say("lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built (tools/mk-ath11k.sh or tools/mk-ath12k.sh, then "
	    "build the kernel again)\n");
	return ENXIO;
}

__attribute__((weak)) void
lx_stop(void)
{
}

__attribute__((weak)) int
lx_prepare(void)
{
	return 0;
}

__attribute__((weak)) int
lx_transmit(__unused const unsigned char *frame, __unused unsigned long length)
{
	return ENXIO;
}

int
hakios_lx_start(void)
{
	return lx_start();
}

void
hakios_lx_stop(void)
{
	lx_stop();
}

int
hakios_lx_prepare(void)
{
	return lx_prepare();
}
