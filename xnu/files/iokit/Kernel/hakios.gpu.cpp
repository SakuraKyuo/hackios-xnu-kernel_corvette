/*
 * HakiosGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/hakios.adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/hakios-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Not here yet: interrupts (waiting is asking again five times a millisecond), more than one
 * program's worth of care (every client's buffers are in the one address space; a client that
 * goes takes its own away), recovery of a GPU that has stopped answering (it is reported, see
 * hakios_gpu_report(), and stays stopped), fences and buffers as file descriptors.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;
void proc_selfname(char * buf, int size);
}
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
extern "C" void hakios_gpu_trace(const char * format, ...) __printflike(1, 2);
extern "C" bool hakios_gpu_debug(void);

extern "C" {
/* bsd/dev/hakios.adreno.c ("the GPU kept on"; declared in bsd/sys/hakios.input.h, which is not seen from here) */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void * start, uint64_t size);
void hakios_gpu_target_words(uint32_t * words, uint32_t count);
uint32_t hakios_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
void hakios_gpu_write_probe(uint64_t address);
void hakios_gpu_request_context_reset(void);
extern uint32_t gpu_log;
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
bool hakios_gpu_idle_now(const char * what);
bool hakios_gpu_await_idle(uint32_t ms);
void hakios_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hakios_gpu_report(const char * why);
void hakios_gpu_stop(void);

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kHakiosShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
	kHakiosLog             = 0x80,         /* ours: a program's line into the loader's log (what fastboot reads back) */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};
struct hakios_log {
	uint32_t length;                        /* the bytes of text that follow */
	char text[188];
};

#define kHakiosDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_BO_CACHED           0x00010000
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_ADDRESSES_START     0x20000000ull
#define GPU_ADDRESSES_SIZE      0xe0000000ull
/* Carved from the existing low-4G allocator; no additional GPU address bits. */
#define GPU_NATIVE_VA_SIZE       (256ull << 20)
#define GPU_NATIVE_COMPOSITOR_VA_SIZE (512ull << 20)

/* A compositor's fixed pools alone consume 128-192 MiB. Reserve its VA from
 * the same low-4G allocator; this is virtual capacity, not physical memory. */
static uint64_t
nativeVaSizeForProcess(const char *name)
{
    return strcmp(name, "backboardd") == 0 ? GPU_NATIVE_COMPOSITOR_VA_SIZE : GPU_NATIVE_VA_SIZE;
}

#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (hakios_gpu_...) */
static uint32_t gNativeClientId;
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
} gGPUStat;

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	if (!gGPUReported && milliseconds > 0) {
		gGPUReported = true;
		hakios_gpu_report("wait timeout");
	}
	return false;
}

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	if (gGPULock == NULL) {
		return true;
	}
	bool done = true;
	uint64_t patience, limit;
	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	limit = mach_absolute_time() + patience;
	IOLockLock(gGPULock);
	for (;;) {
		uint32_t fence = surface->drawn;
		if (fence == 0) {
			break;
		}
		uint64_t now = mach_absolute_time(), remaining = 0;
		if (now < limit) {
			absolutetime_to_nanoseconds(limit - now, &remaining);
		}
		bool pending = hakios_gpu_ready() && !hakios_gpu_done(fence);
		done = !hakios_gpu_ready() || gpuWait(fence, (remaining + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
		if (!done) {
			break;
		}
		/* gpuWait may release gGPULock; do not publish pixels for a superseded fence. */
		if (surface->drawn != fence) {
			if (mach_absolute_time() >= limit) {
				done = false;
				break;
			}
			continue;
		}
		uint8_t * pixels = surface->pixels();
		uint32_t before = *(volatile uint32_t *)pixels;
		hakios_gpu_to_memory(pixels, surface->info.allocSize - surface->info.offset);
		uint32_t after = *(volatile uint32_t *)pixels;
		hakios_gpu_trace("tl: air83 display fence: surface %u fence %u pending %u first %08x -> %08x\n",
		    surface->sid, fence, pending, before, after);
		break;
	}
	IOLockUnlock(gGPULock);
	return done;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hakios_gpu_trace("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HakiosGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID,
	    UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOService);

static char gAdrenoStreamLine[128];
static uint32_t gAdrenoStreamPos = 0;
static char gAdrenoStreamWho[32];
static thread_call_t gStreamFlushCall = NULL;

static void
adreno_stream_flush(void)
{
	if (gAdrenoStreamPos > 0) {
		IOLog("NativeAdreno: [%s] %s\n", gAdrenoStreamWho[0] ? gAdrenoStreamWho : "client", gAdrenoStreamLine);
		gAdrenoStreamLine[0] = '\0';
		gAdrenoStreamPos = 0;
		gAdrenoStreamWho[0] = '\0';
	}
}

static void
adreno_stream_flush_call(thread_call_param_t param0, thread_call_param_t param1)
{
	(void)param0;
	(void)param1;
	adreno_stream_flush();
}

extern "C" void
hakios_adreno_handle_marker(UInt32 type)
{
	uint32_t val = type & 0xffff;
	char who[32] = {};
	proc_selfname(who, sizeof(who));

	if (val >= 0x8000) {
		adreno_stream_flush();
		/* Telemetry row markers: 0xC000 | row starts row, 0x8000 | (a << 7) | b appends 2 chars */
		static unsigned s_row = 48, s_len = 0;
		static char s_rows[48][96];
		if (val >= 0xC000) {
			s_row = val & 0x3f;
			s_len = 0;
			if (s_row < 48) {
				s_rows[s_row][0] = '\0';
			}
		} else if (s_row < 48) {
			char pair[2] = { (char)((val >> 7) & 0x7f), (char)(val & 0x7f) };
			for (int i = 0; i < 2; i++) {
				if (pair[i] && s_len < sizeof(s_rows[0]) - 1) {
					s_rows[s_row][s_len++] = pair[i];
					s_rows[s_row][s_len] = '\0';
				}
			}
		}
		return;
	}

	if (val >= 0x7000 && val < 0x8000) {
		if (val == 0x7fff || val == 0x7ffe) {
			adreno_stream_flush();
			strlcpy(gAdrenoStreamWho, who, sizeof(gAdrenoStreamWho));
		} else if (val < 0x7100 && gAdrenoStreamPos < sizeof(gAdrenoStreamLine) - 1) {
			char c = (char)(val & 0xff);
			if (c == '\n') {
				adreno_stream_flush();
			} else {
				if (!gAdrenoStreamWho[0]) {
					strlcpy(gAdrenoStreamWho, who, sizeof(gAdrenoStreamWho));
				}
				gAdrenoStreamLine[gAdrenoStreamPos++] = c;
				gAdrenoStreamLine[gAdrenoStreamPos] = '\0';
				if (gStreamFlushCall != NULL) {
					uint64_t deadline;
					clock_absolutetime_interval_to_deadline(10 * NSEC_PER_MSEC, &deadline);
					thread_call_enter_delayed(gStreamFlushCall, deadline);
				}
			}
		}
		return;
	}

	adreno_stream_flush();
	IOLog("NativeAdreno: [%s] marker 0x%04x\n", who, val);
}

extern "C" IOReturn
hakios_adreno_new_user_client(IOService * service, task_t owningTask, void * securityID,
    UInt32 type, OSDictionary * properties, IOUserClient ** handler)
{
	if ((type >> 16) == 0x4147 && (type & 0xffff)) {
		if (handler) {
			*handler = NULL;
		}
		hakios_adreno_handle_marker(type);
		return kIOReturnUnsupported;
	}

	adreno_stream_flush();
	return service->IOService::newUserClient(owningTask, securityID, type, properties, handler);
}

bool
HakiosGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setName("HakiosGPU");
	registerService();
	return true;
}

IOReturn
HakiosGPU::newUserClient(task_t owningTask, void * securityID,
    UInt32 type, OSDictionary * properties,
    IOUserClient ** handler)
{
	return hakios_adreno_new_user_client(this, owningTask, securityID, type, properties, handler);
}

void
HakiosGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPULock = IOLockAlloc();
	if (gStreamFlushCall == NULL) {
		gStreamFlushCall = thread_call_allocate(adreno_stream_flush_call, NULL);
	}
	gpu = OSTypeAlloc(HakiosGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosGPU: the service did not start\n");
	}
	gpu->release();
}

/* The run is ending (hakios.adreno.c): nobody is in the middle of anything when the GPU goes. */
void
HakiosGPUStop(void)
{
	IOLockLock(gGPULock);
	hakios_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
	bool                            client_iova;
    bool                            native_release_pending; /* no new submissions own this BO */
	bool                            imported;
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
	bool                            client_iova;
	bool                            imported;
};
#define GPU_LEFT                64

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */

class HakiosGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosGPUUserClient);

	task_t                  fTask;
    uint64_t                fNativeVaStart;
    uint64_t                fNativeVaSize;
    uint64_t                fNativeBytesHigh;
    uint32_t                fNativeId;
    uint32_t                fNativeMapCount;
    uint32_t                fNativeSubmitCount;
    uint32_t                fNativeWaitLogged;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn syncImported(bool toGPU, uint32_t fence);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hakios_show * request);
	IOReturn handleTunnel(uint32_t method, const void * input, size_t input_size, void * output, uint32_t * output_size);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if ((type >> 16) == 0x4147 && (type & 0xffff)) {
		hakios_adreno_handle_marker(type);
		return false;
	}
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	hakios_gpu_request_context_reset();
	return true;
}

bool
HakiosGPUUserClient::start(IOService * provider)
{
	if (!hakios_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

static void probeForget(struct gpu_buffer * b);

/*
 * With the lock held, and the GPU done with everything it has been given (`idle`). If it is
 * not -- it has stopped answering -- the buffer's memory stays the GPU's for good: it may yet be
 * written, and must not be somebody else's by then.
 */
void
HakiosGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	probeForget(b);                         /* (a probe reads through the buffer's view: it goes with the buffer) */
	bool busy = hakios_gpu_ready() && b->fence != 0 && !hakios_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
		left->client_iova = b->client_iova;
		left->imported = b->imported;
	} else {
		if (busy) {
			static uint32_t said;

			if (b->surface == NULL) {
				b->memory->retain();
			} else {
				b->surface->memory->retain();
			}
			if (said++ < 8) {
				IOLog("HakiosGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && !b->client_iova && !b->imported && hakios_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* (now the kept one's) */
		} else if (hakios_gpu_ready()) {
			if (b->address != 0) {
				hakios_gpu_unmap(b->address, b->size);
				if (!b->client_iova) {
					hakios_gpu_address_free(b->address, b->size);
				}
			}
		}
		if (b->memory != NULL) {
			if (b->imported) {
				b->memory->complete(kIODirectionInOut);
			}
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
HakiosGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (hakios_gpu_ready() && !hakios_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && !left->client_iova && !left->imported && hakios_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (hakios_gpu_ready()) {
			if (left->address != 0) {
				hakios_gpu_unmap(left->address, left->size);
				if (!left->client_iova) {
					hakios_gpu_address_free(left->address, left->size);
				}
			}
		}
		if (left->memory != NULL) {
			if (left->imported) {
				left->memory->complete(kIODirectionInOut);
			}
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (hakios_gpu_ready()) {
			uint32_t last = hakios_gpu_last();

			/*
			 * The three steps of metal_test run in processes of their own, one after the other: each
			 * closes its client while the one before it has just had the core do a whole command
			 * stream, and the next process then submits the same 290 words and stops in the client's
			 * own CP_WAIT_FOR_IDLE. What the core and the GMU say at the moment the memory is taken
			 * away is asked here, before and after the wait, so that a teardown too early shows.
			 */
			if (hakios_gpu_debug()) hakios_gpu_idle_now("a client closes, before the wait and its memory goes");
			gpuWait(last, 1000);
			/*
			 * The fence means the command processor has read everything; the core behind it may
			 * still be draining into exactly the memory that is about to be unmapped (seen: a
			 * close at fence 2 read `still busy`, rbbm 0xc30405, and everything after it stuck).
			 * Wait for the drain, the way the gen8 port does.
			 */
			{
				bool idle = hakios_gpu_await_idle(1000);

				hakios_gpu_trace("tl: a client closes: %s\n",
				    idle ? "the core is idle, its memory can go" : "THE CORE NEVER WENT IDLE, its memory goes anyway");
			}
			if (hakios_gpu_debug()) hakios_gpu_idle_now("a client closes, after the wait");
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("HakiosGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
        if (fNativeVaStart != 0) {
            if (fLeftCount == 0 && hakios_gpu_ready() && hakios_gpu_done(hakios_gpu_last())) {
                hakios_gpu_address_free(fNativeVaStart, fNativeVaSize);
                hakios_gpu_trace("tl: air87 Native client %u VA released\n", fNativeId);
            } else {
                hakios_tl("tl: air87 Native client %u VA retained for unfinished GPU work\n", fNativeId);
            }
            fNativeVaStart = 0;
            fNativeVaSize = 0;
        }
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HakiosGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HakiosGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
HakiosGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hakios_gpu_address(size)) == 0) {
		IOLog("HakiosGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hakios_gpu_mapped();
	if (!mapped) {
		hakios_gpu_unmap(b->address, size);
		hakios_gpu_address_free(b->address, size);
		IOLog("HakiosGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	b->client_iova = false;
	b->imported = false;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
HakiosGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hakios_gpu_ready()) {
		hakios_gpu_unmap(k->address, k->size);
		hakios_gpu_address_free(k->address, k->size);
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
HakiosGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HakiosGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HakiosGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	if (gGPUCached) {
		hakios_gpu_to_memory(memory->getBytesNoCopy(), size);
	}

	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kHakiosDescribe), no buffer is made.
 */
IOReturn
HakiosGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		/* (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows the GPU takes) */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(s->memory, PAGE_SIZE, size, 0, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

static uint64_t sProbeAddress;
static struct gpu_buffer * sProbeBuffer;
static uint32_t sProbeOffset;
static uint64_t sBinAddress;
static struct gpu_buffer * sBinBuffer;
static uint32_t sBinOffset;
static uint32_t sBinBefore[8];
static bool sRedirected;                        /* the target moved into the picture's page: read that instead */

/* boot-arg gpu_redirect=0: the client's commands are left alone, so its own target gets the pixels. */
static bool
redirectTargets(void)
{
	static int on = -1;

	if (on < 0) {
		uint32_t asked = 1;

		PE_parse_boot_argn("gpu_redirect", &asked, sizeof(asked));
		on = asked != 0;
	}
	return on;
}

/* A buffer is going away, and with it the memory a probe would read: what was pending is dropped. */
static void
probeForget(struct gpu_buffer * b)
{
	if (b == sProbeBuffer) {
		sProbeAddress = 0;
		sProbeBuffer = NULL;
	}
	if (b == sBinBuffer) {
		sBinAddress = 0;
		sBinBuffer = NULL;
	}
}

/*
 * Two places the core is expected to write, both read here once it is done, after
 * hakios_gpu_to_memory has cleaned and invalidated the lines: the address the client draws into,
 * and the visibility stream the binning pass writes before any render pass. The client's own
 * readback has shown that the command processor reaches the first (it put a word there). The
 * stream is copied when the submission is handed over and compared afterwards, because the client
 * writes command buffers of its own into that buffer too: only what changed after the hand-over
 * was written by the core.
 */
static void
probeReport(void)
{
	uint32_t * at;

	if (sProbeBuffer == NULL && sBinBuffer == NULL && !sRedirected) {
		return;
	}
	if (!gpuWait(hakios_gpu_last(), 0)) {
		return;                 /* (not done: look again at the next submission) */
	}
	if (sProbeBuffer != NULL && sProbeBuffer->kernel != NULL && sProbeOffset + 64 <= sProbeBuffer->size) {
		at = (uint32_t *)((uint8_t *) sProbeBuffer->kernel + sProbeOffset);
		hakios_gpu_to_memory(at, 64);
		hakios_gpu_trace("tl: probe: the client named %#llx to draw into; memory holds %08x %08x %08x %08x %08x %08x\n",
		    (unsigned long long) sProbeAddress, at[0], at[1], at[2], at[3], at[4], at[5]);
	}
	if (sRedirected) {
		uint32_t words[8] = { 0 };

		hakios_gpu_target_words(words, 8);
		hakios_gpu_trace("tl: probe: the picture's page now holds %08x %08x %08x %08x %08x %08x %08x %08x\n",
		    words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
		sRedirected = false;
	}
	sProbeAddress = 0;
	sProbeBuffer = NULL;
	if (sBinBuffer != NULL && sBinBuffer->kernel != NULL && sBinOffset + 32 <= sBinBuffer->size) {
		uint32_t changed = 0;

		at = (uint32_t *)((uint8_t *) sBinBuffer->kernel + sBinOffset);
		hakios_gpu_to_memory(at, 32);
		for (uint32_t i = 0; i < 8; i++) {
			changed += at[i] != sBinBefore[i];
		}
		hakios_gpu_trace("tl: probe: the bin data at %#llx: before %08x %08x %08x %08x, after %08x %08x %08x %08x, %u of 8 changed\n",
		    (unsigned long long) sBinAddress, sBinBefore[0], sBinBefore[1], sBinBefore[2], sBinBefore[3],
		    at[0], at[1], at[2], at[3], changed);
	}
	sBinAddress = 0;
	sBinBuffer = NULL;
}

IOReturn
HakiosGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("HakiosGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (seed == b->seed && !b->seen && began - b->pushed < again) {
				continue;
			}
			b->seed = seed;
			b->pushed = began;
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		hakios_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	hakios_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("HakiosGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
		if (gpu_log) {
			/*
			 * What the command buffer holds where this processor sees it, at the address the core
			 * will read it from: a page of zeroes is a core told to do nothing, however many words
			 * the submission counted. Eight words a line keep inside the log's 192 bytes.
			 */
			uint32_t * at = (uint32_t *)((uint8_t *)b->kernel + cmds[i].submit_offset);
			uint32_t n = cmds[i].size / 4, used = 0;

			for (uint32_t w = 0; w < n; w++) {
				used += at[w] != 0;
			}
			hakios_gpu_trace("tl: ib %u at %#llx: %u words, %u of them not zero\n", i,
			    (unsigned long long)(b->address + cmds[i].submit_offset), n, used);
			if (n >= 8) {
				hakios_gpu_trace("tl: ib %u word 0 on: %08x %08x %08x %08x %08x %08x %08x %08x\n", i,
				    at[0], at[1], at[2], at[3], at[4], at[5], at[6], at[7]);
			}
			for (uint32_t w = 0; w < n; w += 8) {
				uint32_t word8[8] = { 0 };

				for (uint32_t k = 0; k < 8 && w + k < n; k++) {
					word8[k] = at[w + k];
				}
				hakios_gpu_trace("tl: ib %u +%u: %08x %08x %08x %08x %08x %08x %08x %08x\n", i, w,
				    word8[0], word8[1], word8[2], word8[3], word8[4], word8[5], word8[6], word8[7]);
			}
		}
		/*
		 * RB_MRT[0].BASE, in a type-4 packet of eight dwords: 0x8825 is its low word, and the
		 * word after it the address the client has this core write pixels to.
		 */
		/*
		 * RB_MRT[0].BASE, found by walking the packets: a type-4 packet writes its count of
		 * dwords from its register index on, so this address is the payload word at regidx
		 * 0x8825 and not a header word. Type-7 packets are stepped over whole so that no
		 * payload word is taken for a header. Two packets of the client's buffer carry it.
		 */
		{
			uint32_t * at = (uint32_t *)((uint8_t *) b->kernel + cmds[i].submit_offset);
			uint32_t n = cmds[i].size / 4, found = 0;
			uint64_t ib2[8];
			uint32_t ib2n[8], count2 = 0;

			for (uint32_t w = 0; w < n;) {
				uint32_t h = at[w];

				if ((h & 0xf0000000u) == 0x40000000u) {
					uint32_t at4 = (h >> 8) & 0x3ffffu, n4 = h & 0x3fu;

					if (n4 != 0 && 0x8825u - at4 < n4 && w + 1 + (0x8825u - at4) < n) {
						found = at[w + 1 + (0x8825u - at4)];
					}
					w += n4 + 1;
				} else if ((h & 0xf0000000u) == 0x70000000u) {
					uint32_t op7 = (h >> 16) & 0x7fu, n7 = h & 0x3fffu;

					/* CP_SET_BIN_DATA5: where the binning pass puts its visibility stream */
					if (op7 == 0x55u && n7 >= 1 && w + 1 < n) {
						uint64_t bin = at[w + 1] | ((uint64_t)(n7 >= 2 ? at[w + 2] : 0) << 32);

						if (bin != 0 && sBinAddress == 0) {
							sBinAddress = bin;
						}
					}
					/* CP_INDIRECT_BUFFER: the command buffer this one tells the core to run next */
					if (op7 == 0x3fu && n7 == 3 && w + 3 < n && count2 < 8) {
						ib2[count2] = at[w + 1] | ((uint64_t)at[w + 2] << 32);
						ib2n[count2++] = at[w + 3];
					}
					w += n7 + 1;
				} else {
					w++;
				}
			}
			/*
			 * The buffer a CP_INDIRECT_BUFFER points at holds the draw itself. Its words are here
			 * where this processor sees them: what the core is told to draw with, and whether that
			 * is a draw at all.
			 */
			for (uint32_t k = 0; k < count2; k++) {
				struct gpu_buffer * p = NULL;
				uint32_t off = 0, n2;

				for (uint32_t j = 0; j < request->nr_bos; j++) {
					struct gpu_buffer * q = buffer(bos[j].handle);

					if (q != NULL && q->kernel != NULL && ib2[k] >= q->address &&
					    ib2[k] - q->address + 4 <= q->size) {
						p = q;
						off = (uint32_t)(ib2[k] - q->address);
						break;
					}
				}
				if (p == NULL || !gpu_log) {
					continue;
				}
				n2 = ib2n[k];
				if (n2 > (uint32_t)((p->size - off) / 4)) {
					n2 = (uint32_t)((p->size - off) / 4);
				}
				if (n2 > 256) {
					n2 = 256;
				}
				hakios_gpu_trace("tl: ib2 %#llx at %#x: %u words\n", (unsigned long long) ib2[k], off, n2);
				for (uint32_t w2 = 0; w2 < n2; w2 += 8) {
					uint32_t word8[8] = { 0 };

					for (uint32_t t = 0; t < 8 && w2 + t < n2; t++) {
						word8[t] = ((uint32_t *) p->kernel)[off / 4 + w2 + t];
					}
					hakios_gpu_trace("tl: ib2 +%u: %08x %08x %08x %08x %08x %08x %08x %08x\n", w2,
					    word8[0], word8[1], word8[2], word8[3], word8[4], word8[5], word8[6], word8[7]);
				}
			}
			if (found != 0 && redirectTargets()) {
				uint32_t moved = 0;

				sProbeAddress = found;
				/*
				 * Every place this buffer, or the ones it points at, names the target is moved into
				 * the picture's page at 0x10000000: that page is in the core's own address space, and
				 * what the core writes there is read from here. Only the destination changes; the
				 * client's commands are otherwise the client's.
				 */
				for (uint32_t w2 = 0; w2 < n; w2++) {
					if (at[w2] == found) {
						at[w2] = 0x10000000u;
						moved++;
					}
				}
				for (uint32_t k = 0; k < count2; k++) {
					struct gpu_buffer * q = NULL;
					uint32_t off2 = 0, n2;

					for (uint32_t j = 0; j < request->nr_bos; j++) {
						struct gpu_buffer * r = buffer(bos[j].handle);

						if (r != NULL && r->kernel != NULL && ib2[k] >= r->address &&
						    ib2[k] - r->address + 4 <= r->size) {
							q = r;
							off2 = (uint32_t)(ib2[k] - r->address);
							break;
						}
					}
					if (q == NULL) {
						continue;
					}
					n2 = ib2n[k];
					if (n2 > (uint32_t)((q->size - off2) / 4)) {
						n2 = (uint32_t)((q->size - off2) / 4);
					}
					for (uint32_t w2 = 0; w2 < n2; w2++) {
						uint32_t * qw = (uint32_t *)((uint8_t *) q->kernel + off2);

						if (qw[w2] == found) {
							qw[w2] = 0x10000000u;
							moved++;
						}
					}
				}
				sRedirected = true;
				if (gpu_log) {
					hakios_gpu_trace("tl: probe: ib %u names %#x for the draw, moved in %u words\n", i, found, moved);
				}
			} else if (gpu_log) {
				hakios_gpu_trace("tl: probe: ib %u names no target\n", i);
			}
		}
	}
	/*
	 * The address is in a buffer of its own, not in the one the commands are in: the render
	 * target is separate from the command buffer, and what this processor reads of it is what
	 * the client reads back. Find the buffer that holds it among the submission's.
	 */
	if (sProbeAddress != 0) {
		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * p = buffer(bos[i].handle);

			if (p != NULL && p->kernel != NULL && sProbeAddress >= p->address &&
			    sProbeAddress - p->address + 64 <= p->size) {
				sProbeBuffer = p;
				sProbeOffset = (uint32_t)(sProbeAddress - p->address);
				break;
			}
		}
		if (sProbeBuffer == NULL) {
			if (gpu_log) {
				hakios_gpu_trace("tl: probe: %#x is in no buffer of this submission\n", (uint32_t)sProbeAddress);
			}
			sProbeAddress = 0;
		} else {
			if (gpu_log) {
				hakios_gpu_trace("tl: probe: %#x is %#x into a buffer, %u bytes on\n",
				    (uint32_t)sProbeAddress, sProbeOffset, (uint32_t)sProbeBuffer->size);
			}
			hakios_gpu_write_probe(sProbeAddress);
		}
	}
	if (sBinAddress != 0) {
		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * p = buffer(bos[i].handle);

			if (p != NULL && p->kernel != NULL && sBinAddress >= p->address &&
			    sBinAddress - p->address + 32 <= p->size) {
				sBinBuffer = p;
				sBinOffset = (uint32_t)(sBinAddress - p->address);
				break;
			}
		}
		if (sBinBuffer == NULL) {
			sBinAddress = 0;
		} else if (gpu_log) {
			hakios_gpu_trace("tl: probe: bin data %#x is %#x into a buffer\n", (uint32_t)sBinAddress, sBinOffset);
		}
	}
	if (sBinBuffer != NULL && sBinBuffer->kernel != NULL && sBinOffset + 32 <= sBinBuffer->size) {
		uint32_t * at = (uint32_t *)((uint8_t *) sBinBuffer->kernel + sBinOffset);

		hakios_gpu_to_memory(at, 32);           /* (what the client has written there; the core writes next) */
		for (uint32_t i = 0; i < 8; i++) {
			sBinBefore[i] = at[i];
		}
	}
	request->fence = hakios_gpu_submit(addresses, words, count);
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hakios_gpu_trace("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
			}
		}
	}
	reap();
	if (request->fence == 0 && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("the ring is full");
	}
	if (fSubmits++ < 4) {
		hakios_gpu_trace("HakiosGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hakios_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		IOLog("HakiosGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		IOLog("HakiosGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
HakiosGPUUserClient::show(const struct hakios_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	if (type == 0x5343) { /* 'SC' - Scanout Framebuffer */
		if (vinfo.v_baseaddr == 0 || vinfo.v_rowbytes == 0 || vinfo.v_height == 0) {
			return kIOReturnNotReady;
		}
		IOMemoryDescriptor * mem = IOMemoryDescriptor::withAddressRange(
		    (mach_vm_address_t)vinfo.v_baseaddr,
		    (mach_vm_size_t)vinfo.v_rowbytes * vinfo.v_height,
		    kIODirectionInOut,
		    kernel_task);
		if (mem == NULL) {
			return kIOReturnNoMemory;
		}
		if (options != NULL) {
			*options = 0;
		}
		*memory = mem;
		return kIOReturnSuccess;
	}

	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

/*
 * Native imports are user mappings of cached memory (including IOSurface pixels), not the
 * driver's write-combined BOs. Their memory descriptor pins pages but does not make this
 * non-coherent GPU see CPU writes, nor invalidate CPU cache lines after GPU writes.
 * Use a temporary kernel view of the prepared descriptor and the same explicit dc civac
 * helper as the working cached BO path. IOMemoryDescriptor::performOperation can become
 * a no-op under XNU's __ARM_COHERENT_IO__ assumption, which this GPU port does not satisfy.
 * gGPULock is held by the caller. Do not cache-synchronize an import still owned by the GPU.
 */
IOReturn
HakiosGPUUserClient::syncImported(bool toGPU, uint32_t fence)
{
	bool any = false;
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].imported) {
			any = true;
			break;
		}
	}
	if (!any) {
		return kIOReturnSuccess;
	}
	/* This client may reuse the same cached import in consecutive command buffers. */
	if (toGPU && hakios_gpu_last() != 0 && !gpuWait(hakios_gpu_last(), 1000)) {
		hakios_tl("tl: air82 import sync: previous GPU work did not complete\n");
		return kIOReturnTimeout;
	}
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		struct gpu_buffer * b = &fBuffers[i];
		if (b->memory == NULL || !b->imported) {
			continue;
		}
		if (!toGPU && (b->fence == 0 || (int32_t)(fence - b->fence) < 0 || b->pushed == b->fence)) {
			continue;
		}
		auto mapping = b->memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
		if (!mapping || mapping->getVirtualAddress() == 0 || mapping->getLength() < b->size) {
			hakios_tl("tl: air82 import sync: cannot map handle %u, %llu bytes\n", i + 1, b->size);
			return kIOReturnNoMemory;
		}
		void *base = (void *)mapping->getVirtualAddress();
		uint32_t first = *(volatile uint32_t *)base;
		hakios_gpu_to_memory(base, b->size);
		uint32_t fresh = *(volatile uint32_t *)base;
		IOByteCount contiguous = 0;
		addr64_t phys = b->memory->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
		hakios_gpu_trace("tl: air82 import %s: handle %u iova %#llx phys %#llx bytes %llu fence %u first %08x -> %08x\n",
		    toGPU ? "to GPU" : "to CPU", i + 1, b->address, (uint64_t)phys, b->size, b->fence, first, fresh);
		if (!toGPU) {
			b->pushed = b->fence;
		}
	}
	return kIOReturnSuccess;
}

#define NATIVE_ADRENO_TUNNEL_MAGIC 0x4e544e4c /* 'NTNL' */

enum {
	kNativeAdrenoAbiVersion = 2,
	kNativeAdrenoAbiMaxBuffers = 128,
	kNativeAdrenoAbiMaxBoBytesShift = 28,
	kNativeAdrenoMethodQuery = 1,
	kNativeAdrenoMethodBoAlloc = 2,
	kNativeAdrenoMethodBoMap = 3,
	kNativeAdrenoMethodBoFree = 4,
	kNativeAdrenoMethodSubmit = 5,
	kNativeAdrenoMethodWaitFence = 6,
	kNativeAdrenoMethodGetTimestamp = 7,
	kNativeAdrenoMethodGetStatus = 8,
	kNativeAdrenoMethodSetMetadata = 9,
	kNativeAdrenoMethodGetMetadata = 10,
	kNativeAdrenoMethodCreateQueue = 11,
	kNativeAdrenoMethodCloseQueue = 12,
	kNativeAdrenoMethodBoCpuMap = 13,
	kNativeAdrenoMethodBoCpuUnmap = 14,
	kNativeAdrenoMethodGetDeviceInfo = 15,
	kNativeAdrenoMethodBoImport = 16,
};

struct NativeAdrenoAbiParams {
	uint32_t abi_version;
	uint32_t input_bits;
	uint32_t output_bits;
	uint32_t flags;
	uint64_t max_bo_bytes;
};

struct NativeAdrenoAbiDeviceInfo {
	uint32_t chip_id;
	uint32_t gmem_bytes;
	uint64_t gmem_base;
	uint64_t uche_trap_base;
	uint64_t va_start;
	uint64_t va_size;
	uint32_t highest_bank_bit;
	uint32_t ubwc_swizzle;
	uint32_t macrotile_mode;
	uint32_t ubwc_version;
	uint32_t flags;
	uint32_t reserved;
};

struct NativeAdrenoAbiBoAlloc {
	uint64_t bytes;
	uint32_t align_log2;
	uint32_t contiguous;
};

struct NativeAdrenoAbiBoAllocReply {
	uint64_t handle;
};

struct NativeAdrenoAbiBoMap {
	uint64_t handle;
	uint64_t iova;
	uint32_t permissions;
	uint32_t writeback;
	uint32_t shareability;
	uint32_t reserved;
};

struct NativeAdrenoAbiBoMapReply {
	uint64_t iova;
};

struct NativeAdrenoAbiBoFree {
	uint64_t handle;
};

struct NativeAdrenoAbiBoCpuMap {
	uint64_t handle;
};

struct NativeAdrenoAbiBoCpuMapReply {
	uint32_t token;
	uint32_t reserved;
	uint64_t bytes;
};

struct NativeAdrenoAbiBoCpuUnmap {
	uint64_t handle;
	uint32_t token;
	uint32_t reserved;
};

struct NativeAdrenoAbiBoImport {
	uint64_t address;
	uint64_t bytes;
};

struct NativeAdrenoAbiSubmitBuffer {
	uint64_t handle;
	uint64_t iova;
	uint64_t bytes;
};

struct NativeAdrenoAbiSubmit {
	uint32_t buffer_count;
	uint32_t reserved;
	uint64_t queue;
	uint64_t wait_fence;
	uint64_t wait_timeout_ns;
	NativeAdrenoAbiSubmitBuffer buffers[kNativeAdrenoAbiMaxBuffers];
};

struct NativeAdrenoAbiSubmitReply {
	uint64_t fence;
};

struct NativeAdrenoAbiWaitFence {
	uint64_t fence;
	uint64_t timeout_ns;
};

struct NativeAdrenoAbiTimestamp {
	uint64_t ticks;
};

struct NativeAdrenoAbiStatus {
	uint64_t faults;
	uint64_t suspends;
};

struct NativeAdrenoAbiQueueReply {
	uint64_t queue;
};

enum {
	kNativeAdrenoAbiMetadataBytes = 64,
};

struct NativeAdrenoAbiMetadata {
	uint64_t handle;
	uint32_t bytes;
	uint32_t reserved;
	char text[kNativeAdrenoAbiMetadataBytes];
};

static uint32_t
ibNop(void)
{
	static uint32_t at = 0;
	static bool read = false;

	if (!read) {
		read = true;
		PE_parse_boot_argn("adreno_ib_nop", &at, sizeof(at));
	}
	return at;
}

static uint32_t
ibNopPackets(void)
{
	static uint32_t pkts = 1;
	static bool read = false;

	if (!read) {
		read = true;
		PE_parse_boot_argn("adreno_ib_nop_pkts", &pkts, sizeof(pkts));
	}
	return pkts == 0 ? 1 : pkts;
}

static uint32_t
ibCut(void)
{
	static uint32_t cut = 0;
	static bool read = false;

	if (!read) {
		read = true;
		PE_parse_boot_argn("adreno_ib_cut", &cut, sizeof(cut));
	}
	return cut;
}

IOReturn
HakiosGPUUserClient::handleTunnel(uint32_t method, const void * input, size_t input_size, void * output, uint32_t * output_size)
{
	adreno_stream_flush();
	switch (method) {
	case kNativeAdrenoMethodQuery: {
		if (output == NULL || *output_size < sizeof(NativeAdrenoAbiParams)) {
			return kIOReturnBadArgument;
		}
		NativeAdrenoAbiParams reply;
		bzero(&reply, sizeof(reply));
		reply.abi_version = kNativeAdrenoAbiVersion;
		reply.input_bits = 48;
		reply.output_bits = 36;
		reply.flags = 1; /* NativeAdrenoParamsHostImport */
		reply.max_bo_bytes = 1ULL << kNativeAdrenoAbiMaxBoBytesShift;
		memcpy(output, &reply, sizeof(reply));
		*output_size = sizeof(reply);
		hakios_gpu_trace("HakiosGPU: NativeAdreno query handled (abi=%u)\n", reply.abi_version);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodGetDeviceInfo: {
		if (output == NULL || *output_size < sizeof(NativeAdrenoAbiDeviceInfo)) {
			return kIOReturnBadArgument;
		}
		NativeAdrenoAbiDeviceInfo reply;
		bzero(&reply, sizeof(reply));
		uint64_t val = 0;
		if (hakios_gpu_parameter(0x03, &val)) {
			reply.chip_id = (uint32_t)val;
		} else {
			reply.chip_id = 0x43051401;
		}
		if (hakios_gpu_parameter(0x02, &val)) {
			reply.gmem_bytes = (uint32_t)val;
		} else {
			reply.gmem_bytes = 3 * 1024 * 1024;
		}
		reply.gmem_base = 0x1000000ull;
		reply.uche_trap_base = 0x0001fffffffff000ull;
        /* All clients share core_space. Never advertise the same VA interval
         * to independent userspace allocators: BoMap used to replace another
         * live client's PTEs, including that client's command buffers. */
        char who[32] = {};
        proc_selfname(who, sizeof(who));
        IOLockLock(gGPULock);
        if (fNativeVaStart == 0) {
            uint64_t requested = nativeVaSizeForProcess(who);
            fNativeVaStart = hakios_gpu_address(requested);
            if (fNativeVaStart != 0) {
                fNativeVaSize = requested;
                fNativeId = ++gNativeClientId;
            }
        }
        reply.va_start = fNativeVaStart;
        reply.va_size = fNativeVaSize;
        IOLockUnlock(gGPULock);
        if (reply.va_start == 0) {
            hakios_tl("tl: air101 Native VA exhausted: [%s] needs %llu MiB private window\n",
                who, nativeVaSizeForProcess(who) >> 20);
            return kIOReturnNoResources;
        }
        hakios_gpu_trace("tl: air87 Native client %u [%s] VA [%#llx,%#llx), %llu MiB reserved\n",
            fNativeId, who, reply.va_start, reply.va_start + reply.va_size, reply.va_size >> 20);
        hakios_gpu_trace("tl: air101 Native quota: client %u [%s] %llu MiB at %#llx\n",
            fNativeId, who, reply.va_size >> 20, reply.va_start);
		if (hakios_gpu_parameter(0x10, &val)) {
			reply.highest_bank_bit = (uint32_t)val;
		} else {
			reply.highest_bank_bit = 16;
		}
		if (hakios_gpu_parameter(0x12, &val)) {
			reply.ubwc_swizzle = (uint32_t)val;
		} else {
			reply.ubwc_swizzle = 6;
		}
		if (hakios_gpu_parameter(0x13, &val)) {
			reply.macrotile_mode = (uint32_t)val;
		} else {
			reply.macrotile_mode = 1;
		}
		reply.ubwc_version = 4;
		reply.flags = 1; /* NativeAdrenoDeviceClientIova */
		memcpy(output, &reply, sizeof(reply));
		*output_size = sizeof(reply);
		hakios_gpu_trace("HakiosGPU: NativeAdreno device info: chip %#x, gmem %uKB, va %#llx..%#llx\n",
		    reply.chip_id, reply.gmem_bytes >> 10, reply.va_start, reply.va_start + reply.va_size);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodBoAlloc: {
		if (input == NULL || input_size < sizeof(NativeAdrenoAbiBoAlloc) ||
		    output == NULL || *output_size < sizeof(NativeAdrenoAbiBoAllocReply)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiBoAlloc * req = (const NativeAdrenoAbiBoAlloc *)input;
		NativeAdrenoAbiBoAllocReply * reply = (NativeAdrenoAbiBoAllocReply *)output;
		uint64_t size = round_page_64(req->bytes);

		if (size == 0 || size > GPU_BUFFER_MAX) {
			return kIOReturnBadArgument;
		}
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithOptions(
		    kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared | (gGPUCached ? 0 : kIOMapWriteCombineCache),
		    (vm_size_t)size, PAGE_SIZE);
		if (memory == NULL) {
			return kIOReturnNoMemory;
		}
		if (gGPUCached) {
			hakios_gpu_to_memory(memory->getBytesNoCopy(), size);
		}

		IOLockLock(gGPULock);
		struct gpu_buffer * b = NULL;
		uint32_t handle = 0;
		for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
			uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;
			if (fBuffers[i].memory == NULL) {
				b = &fBuffers[i];
				handle = i + 1;
				fNextBuffer = i + 1;
				break;
			}
		}
		if (b == NULL) {
			IOLockUnlock(gGPULock);
			memory->release();
			return kIOReturnNoResources;
		}
		bzero(b, sizeof(*b));
		b->memory = memory;
		b->kernel = (uint8_t *)memory->getBytesNoCopy();
		b->size = size;
		b->flags = 0;
		b->address = 0;
		b->seen = false;
		b->client_iova = true;
		b->imported = false;
		fBufferCount++;
		fBufferBytes += size;
		IOLockUnlock(gGPULock);

		char who[32] = {};
		proc_selfname(who, sizeof(who));
		hakios_gpu_trace("NativeAdreno: [%s] BoAlloc handle=%u bytes=%llu\n", who, handle, size);

		reply->handle = handle;
		*output_size = sizeof(*reply);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodBoMap: {
		if (input == NULL || input_size < sizeof(NativeAdrenoAbiBoMap) ||
		    output == NULL || *output_size < sizeof(NativeAdrenoAbiBoMapReply)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiBoMap * req = (const NativeAdrenoAbiBoMap *)input;
		NativeAdrenoAbiBoMapReply * reply = (NativeAdrenoAbiBoMapReply *)output;

		IOLockLock(gGPULock);
		struct gpu_buffer * b = buffer((uint32_t)req->handle);
		if (b == NULL) {
			IOLockUnlock(gGPULock);
			return kIOReturnBadArgument;
		}
		if (b->address != 0) {
			IOLockUnlock(gGPULock);
			return kIOReturnBusy;
		}
        if (fNativeVaStart == 0 || fNativeVaSize == 0 || (req->iova & 4095) || req->iova < fNativeVaStart ||
            req->iova - fNativeVaStart >= fNativeVaSize ||
            b->size > fNativeVaSize - (req->iova - fNativeVaStart)) {
            hakios_tl("tl: air87 reject BoMap client %u: iova %#llx size %#llx outside [%#llx,%#llx)\n",
                fNativeId, req->iova, b->size, fNativeVaStart, fNativeVaStart + fNativeVaSize);
            IOLockUnlock(gGPULock);
            return kIOReturnBadArgument;
        }
        reap();
        for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
            const struct gpu_buffer *other = &fBuffers[i];
            if (other != b && other->memory != NULL && other->address != 0 &&
                req->iova < other->address + other->size && other->address < req->iova + b->size) {
                hakios_tl("tl: air87 reject overlapping BoMap client %u handles %llu/%u\n",
                    fNativeId, req->handle, i + 1);
                IOLockUnlock(gGPULock);
                return kIOReturnBusy;
            }
        }
        for (uint32_t i = 0; i < fLeftCount; i++) {
            if (req->iova < fLeft[i].address + fLeft[i].size && fLeft[i].address < req->iova + b->size) {
                hakios_tl("tl: air87 reject deferred BoMap client %u handle %llu fence %u\n",
                    fNativeId, req->handle, fLeft[i].fence);
                IOLockUnlock(gGPULock);
                return kIOReturnBusy;
            }
        }
        b->address = req->iova;
		bool mapped = true;
		for (uint64_t offset = 0; offset < b->size && mapped; offset += 4096) {
			IOByteCount length = 0;
			addr64_t phys = b->memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
			mapped = phys != 0 && hakios_gpu_map(b->address + offset, phys, (req->permissions & 2) == 0);
		}
		hakios_gpu_mapped();
		if (!mapped) {
			hakios_gpu_unmap(b->address, b->size);
			b->address = 0;
			IOLockUnlock(gGPULock);
			return kIOReturnNoMemory;
		}

		char who[32] = {};
		proc_selfname(who, sizeof(who));
        if (++fNativeMapCount <= 4 || (fNativeMapCount & 31) == 0)
            hakios_gpu_trace("tl: air87 map client %u [%s] handle %llu iova %#llx bytes %llu (map %u)\n",
                fNativeId, who, req->handle, req->iova, b->size, fNativeMapCount);

        if (fBufferBytes > fNativeBytesHigh &&
            (fBufferBytes >> 25) > (fNativeBytesHigh >> 25)) {
            fNativeBytesHigh = fBufferBytes;
            hakios_gpu_trace("tl: air101 Native bytes: client %u [%s] live %llu MiB, BOs %u, VA window %llu MiB\n",
                fNativeId, who, fBufferBytes >> 20, fBufferCount, fNativeVaSize >> 20);
        }
		reply->iova = b->address;
		*output_size = sizeof(*reply);
		IOLockUnlock(gGPULock);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodBoFree: {
		if (input == NULL || input_size < sizeof(NativeAdrenoAbiBoFree)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiBoFree * req = (const NativeAdrenoAbiBoFree *)input;

		IOLockLock(gGPULock);
        if (!hakios_gpu_ready()) {
            IOLockUnlock(gGPULock);
            return kIOReturnNotReady;
        }
		struct gpu_buffer * b = buffer((uint32_t)req->handle);
		if (b == NULL) {
			IOLockUnlock(gGPULock);
			return kIOReturnBadArgument;
		}
        /* Native's reaper returns the IOVA to its heap only after BoFree
         * succeeds. Its existing Busy path retains this handle/IOVA for retry.
         * Do not report success while freeBuffer would keep old PTEs in fLeft:
         * the next allocation could otherwise reuse that still-live address. */
        if (b->fence != 0 && !hakios_gpu_done(b->fence)) {
            /* Vulkan has dropped its final BO reference. Preserve the last
             * real use, not the fences of unrelated later submissions. */
            b->native_release_pending = true;
            static uint32_t reportedFence;
            if (reportedFence != b->fence) {
                reportedFence = b->fence;
                hakios_gpu_trace("tl: air99 BoFree busy: client %u handle %llu iova %#llx bytes %llu fence %u; user retains VA\n",
                    fNativeId, req->handle, b->address, b->size, b->fence);
            }
            IOLockUnlock(gGPULock);
            return kIOReturnBusy;
        }
        if (b->native_release_pending) {
            static uint32_t releases;
            if (++releases <= 4 || (releases & 63) == 0)
                hakios_gpu_trace("tl: air100 BoFree released: client %u handle %llu iova %#llx fence %u (release %u)\n",
                    fNativeId, req->handle, b->address, b->fence, releases);
        }
		freeBuffer(b);
		reap();
		IOLockUnlock(gGPULock);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodBoCpuMap: {
		if (input == NULL || input_size < sizeof(NativeAdrenoAbiBoCpuMap) ||
		    output == NULL || *output_size < sizeof(NativeAdrenoAbiBoCpuMapReply)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiBoCpuMap * req = (const NativeAdrenoAbiBoCpuMap *)input;
		NativeAdrenoAbiBoCpuMapReply * reply = (NativeAdrenoAbiBoCpuMapReply *)output;

		IOLockLock(gGPULock);
		struct gpu_buffer * b = buffer((uint32_t)req->handle);
		if (b == NULL) {
			IOLockUnlock(gGPULock);
			return kIOReturnBadArgument;
		}
		reply->token = (uint32_t)req->handle;
		reply->reserved = 0;
		reply->bytes = b->size;
		*output_size = sizeof(*reply);
		IOLockUnlock(gGPULock);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodBoCpuUnmap:
		return kIOReturnSuccess;

	case kNativeAdrenoMethodBoImport: {
		if (input == NULL || input_size < sizeof(NativeAdrenoAbiBoImport) ||
		    output == NULL || *output_size < sizeof(NativeAdrenoAbiBoAllocReply)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiBoImport * req = (const NativeAdrenoAbiBoImport *)input;
		NativeAdrenoAbiBoAllocReply * reply = (NativeAdrenoAbiBoAllocReply *)output;
		uint64_t size = round_page_64(req->bytes);

		if (size == 0 || size > GPU_BUFFER_MAX || (req->address & 4095)) {
			return kIOReturnBadArgument;
		}
		IOMemoryDescriptor * memory = IOMemoryDescriptor::withAddressRange(
		    (mach_vm_address_t)req->address,
		    (mach_vm_size_t)size,
		    kIODirectionInOut,
		    fTask != NULL ? fTask : current_task());
		if (memory == NULL) {
			return kIOReturnNoMemory;
		}
		if (memory->prepare(kIODirectionInOut) != kIOReturnSuccess) {
			memory->release();
			return kIOReturnVMError;
		}

		IOLockLock(gGPULock);
		struct gpu_buffer * b = NULL;
		uint32_t handle = 0;
		for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
			uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;
			if (fBuffers[i].memory == NULL) {
				b = &fBuffers[i];
				handle = i + 1;
				fNextBuffer = i + 1;
				break;
			}
		}
		if (b == NULL) {
			IOLockUnlock(gGPULock);
			memory->complete(kIODirectionInOut);
			memory->release();
			return kIOReturnNoResources;
		}
		bzero(b, sizeof(*b));
		b->memory = memory;
		b->kernel = NULL;
		b->size = size;
		b->flags = MSM_BO_CACHED;
		b->address = 0;
		b->seen = true;
		b->client_iova = true;
		b->imported = true;
		b->surface = MiniIOSurfaceCopyForMemory(memory, size);
		if (b->surface != NULL) {
			hakios_gpu_trace("tl: air83 import surface: handle %u surface %u bytes %llu\n",
			    handle, b->surface->sid, size);
		}
		fBufferCount++;
		fBufferBytes += size;
		hakios_gpu_trace("tl: air82 BoImport: handle %u user %#llx bytes %llu\n", handle, req->address, size);
		IOLockUnlock(gGPULock);

		reply->handle = handle;
		*output_size = sizeof(*reply);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodSubmit: {
		if (input == NULL || input_size < sizeof(uint32_t) * 2 + sizeof(uint64_t) * 3 ||
		    output == NULL || *output_size < sizeof(NativeAdrenoAbiSubmitReply)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiSubmit * req = (const NativeAdrenoAbiSubmit *)input;
		NativeAdrenoAbiSubmitReply * reply = (NativeAdrenoAbiSubmitReply *)output;

        if (req->buffer_count == 0 || req->buffer_count > GPU_SUBMIT_CMDS ||
            input_size < offsetof(NativeAdrenoAbiSubmit, buffers) +
                req->buffer_count * sizeof(NativeAdrenoAbiSubmitBuffer)) {
			return kIOReturnBadArgument;
		}

		uint64_t addresses[GPU_SUBMIT_CMDS];
		uint32_t words[GPU_SUBMIT_CMDS];

		IOLockLock(gGPULock);
        if (gGPUReported) {
            IOLockUnlock(gGPULock);
            return kIOReturnNotResponding;
        }
		/* Imports need synchronization even when only the IB handles are listed here. */
		IOReturn synchronized = syncImported(true, 0);
		if (synchronized != kIOReturnSuccess) {
			IOLockUnlock(gGPULock);
			return synchronized;
		}
		for (uint32_t i = 0; i < req->buffer_count; i++) {
			struct gpu_buffer * b = buffer((uint32_t)req->buffers[i].handle);
            if (b == NULL || b->native_release_pending || b->address == 0 || (req->buffers[i].iova & 3) ||
                (req->buffers[i].bytes & 3) || req->buffers[i].bytes == 0 ||
                req->buffers[i].iova < b->address || req->buffers[i].iova - b->address >= b->size ||
                req->buffers[i].bytes > b->size - (req->buffers[i].iova - b->address)) {
                hakios_tl("tl: air87 reject submit IB client %u: index %u handle %llu iova %#llx bytes %llu\n",
                    fNativeId, i, req->buffers[i].handle, req->buffers[i].iova, req->buffers[i].bytes);
                IOLockUnlock(gGPULock);
                return kIOReturnBadArgument;
            }
			/*
			 * Every buffer the core is about to read is pushed out of this processor's cache, not
			 * only the imported ones. Allocated buffers carried `seen = false`, so their words were
			 * never cleaned: the first client's command buffer happened to be in memory, and the
			 * next one -- its pages freshly allocated and its words written by the program -- could
			 * be fetched by the command processor before they reached it. What the core then runs
			 * is not what this processor wrote.
			 */
			if (b->kernel != NULL) {
				hakios_gpu_to_memory(b->kernel, b->size);
			}
			addresses[i] = req->buffers[i].iova;
			words[i] = (uint32_t)(req->buffers[i].bytes / 4);
		}
		{
			uint32_t cut = ibCut();

			if (cut != 0 && words[0] > cut) {
				words[0] = cut;
				hakios_gpu_trace("tl: adreno_ib_cut: the first command buffer is cut to %u words\n", cut);
			}
		}
		hakios_gpu_to_memory(NULL, 0);

		char who[32] = {};
		proc_selfname(who, sizeof(who));
        bool logIB = ++fNativeSubmitCount <= 2 && hakios_gpu_debug();
        if (logIB || (fNativeSubmitCount & 31) == 0)
            hakios_gpu_trace("tl: air87 submit client %u [%s] number %u next fence %u IB %#llx words %u\n",
                fNativeId, who, fNativeSubmitCount, hakios_gpu_last() + 1, addresses[0], words[0]);
		{
			/* what the core is about to be given, where this processor sees it */
			struct gpu_buffer * b0 = buffer((uint32_t)req->buffers[0].handle);
			uint32_t off = b0 != NULL ? (uint32_t)((req->buffers[0].iova - b0->address) / 4) : 0;

			hakios_gpu_trace("tl: tunnel ib 0 at %#llx: %u words\n",
			    (unsigned long long)req->buffers[0].iova, words[0]);
            if (logIB && b0 != NULL && b0->kernel != NULL && words[0] != 0 && words[0] <= 1024 &&
			    off + words[0] <= b0->size / 4) {
				uint32_t * at = (uint32_t *)b0->kernel + off;
				uint32_t n = words[0];

				for (uint32_t k = 0; k < n; k += 8) {
					uint32_t w[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

					for (uint32_t j = 0; j < 8 && k + j < n; j++) {
						w[j] = at[k + j];
					}
					hakios_gpu_trace("tl: tunnel ib %u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					    k, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
				}
			}
			uint32_t nop = ibNop();

			if (nop != 0 && b0 != NULL && b0->kernel != NULL && nop < words[0] &&
			    off + words[0] <= b0->size / 4) {
				uint32_t * at = (uint32_t *)b0->kernel + off;
				uint32_t pkts = ibNopPackets();
				uint32_t i = nop;

				for (uint32_t p = 0; p < pkts && i < words[0]; p++) {
					uint32_t w = at[i], type = w >> 28;
					uint32_t cnt = (type == 7) ? (w & 0x7FFF) : (type == 4) ? (w & 0x1F) : 0;
					uint32_t end = i + 1 + cnt;

					if (end > words[0]) {
						end = words[0];
					}
					hakios_gpu_trace("tl: adreno_ib_nop: words %u..%u (%08x) become NOPs\n",
					    i, end - 1, w);
					for (uint32_t k = i; k < end; k++) {
						at[k] = 0x70100000;     /* CP_NOP, no payload */
					}
					i = end;
				}
				hakios_gpu_to_memory(b0->kernel, b0->size);
			}
		}

		uint32_t fence = hakios_gpu_submit(addresses, words, req->buffer_count);
		if (fence == 0) {
			IOLockUnlock(gGPULock);
			return kIOReturnBusy;
		}
		for (uint32_t i = 0; i < req->buffer_count; i++) {
			struct gpu_buffer * b = buffer((uint32_t)req->buffers[i].handle);
			if (b != NULL) {
				b->fence = fence;
			}
		}
        /* The ABI lists IBs only. Fence every active Native BO conservatively.
         * A BO already handed to BoFree has no future userspace references;
         * keep its recorded retirement fence stable until the release retry. */
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
            if (fBuffers[i].memory != NULL && fBuffers[i].client_iova &&
                !fBuffers[i].native_release_pending) {
				fBuffers[i].fence = fence;
				if (fBuffers[i].surface != NULL) {
					fBuffers[i].surface->drawn = fence;
					hakios_gpu_trace("tl: air83 surface submitted: surface %u fence %u\n", fBuffers[i].surface->sid, fence);
				}
			}
		}
		reply->fence = fence;
		*output_size = sizeof(*reply);
		IOLockUnlock(gGPULock);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodWaitFence: {
		if (input == NULL || input_size < sizeof(NativeAdrenoAbiWaitFence)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiWaitFence * req = (const NativeAdrenoAbiWaitFence *)input;
		if (req->fence == 0) {
			return kIOReturnBadArgument;
		}

		IOLockLock(gGPULock);
		if (req->timeout_ns == 0) {
			bool done = hakios_gpu_done((uint32_t)req->fence);
			IOReturn synchronized = done ? syncImported(false, (uint32_t)req->fence) : kIOReturnTimeout;
			IOLockUnlock(gGPULock);
			return synchronized;
		}

		uint64_t patience = req->timeout_ns / 1000000ULL;
		if (patience == 0) {
			patience = 1;
		}

		char who[32] = {};
		proc_selfname(who, sizeof(who));
        if (fNativeWaitLogged != (uint32_t)req->fence) {
            fNativeWaitLogged = (uint32_t)req->fence;
            if (fNativeSubmitCount <= 2 || gGPUReported)
                hakios_gpu_trace("tl: air87 wait client %u [%s] fence %llu timeout_ns %llu\n",
                    fNativeId, who, req->fence, req->timeout_ns);
        }
        if (gGPUReported) {
            IOLockUnlock(gGPULock);
            /* Retrying a known failed fence must not become an 8-us log loop. */
            delay_for_interval((uint32_t)(patience < 100 ? patience : 100), NSEC_PER_MSEC);
            return kIOReturnTimeout;
        }

		bool done = gpuWait((uint32_t)req->fence, patience);
		IOReturn synchronized = done ? syncImported(false, (uint32_t)req->fence) : kIOReturnTimeout;
		IOLockUnlock(gGPULock);
		return synchronized;
	}

	case kNativeAdrenoMethodGetTimestamp: {
		if (output == NULL || *output_size < sizeof(NativeAdrenoAbiTimestamp)) {
			return kIOReturnBadArgument;
		}
		NativeAdrenoAbiTimestamp * reply = (NativeAdrenoAbiTimestamp *)output;
		reply->ticks = mach_absolute_time();
		*output_size = sizeof(*reply);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodGetStatus: {
		if (output == NULL || *output_size < sizeof(NativeAdrenoAbiStatus)) {
			return kIOReturnBadArgument;
		}
		NativeAdrenoAbiStatus * reply = (NativeAdrenoAbiStatus *)output;
		reply->faults = 0;
		reply->suspends = 0;
		*output_size = sizeof(*reply);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodSetMetadata: {
		if (input != NULL && input_size >= sizeof(NativeAdrenoAbiMetadata)) {
			const NativeAdrenoAbiMetadata * meta = (const NativeAdrenoAbiMetadata *)input;
			char line[kNativeAdrenoAbiMetadataBytes + 1] = {};
			uint32_t len = meta->bytes < kNativeAdrenoAbiMetadataBytes ? meta->bytes : kNativeAdrenoAbiMetadataBytes;
			memcpy(line, meta->text, len);
			line[len] = '\0';
			char who[32] = {};
			proc_selfname(who, sizeof(who));
			IOLog("NativeAdreno: [%s] metadata report: %s\n", who, line);
		}
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodGetMetadata:
		return kIOReturnSuccess;

	case kNativeAdrenoMethodCreateQueue: {
		if (output == NULL || *output_size < sizeof(NativeAdrenoAbiQueueReply)) {
			return kIOReturnBadArgument;
		}
		NativeAdrenoAbiQueueReply * reply = (NativeAdrenoAbiQueueReply *)output;
		reply->queue = 1;
		*output_size = sizeof(*reply);
		return kIOReturnSuccess;
	}

	case kNativeAdrenoMethodCloseQueue:
		return kIOReturnSuccess;

	case 0x7001: { /* NGP_METHOD_MODE */
		struct NGPMode {
			uint32_t version;
			uint32_t format;
			uint32_t width;
			uint32_t height;
			uint32_t stride;
			uint64_t bytes;
		};
		if (output == NULL || *output_size < sizeof(struct NGPMode)) {
			return kIOReturnBadArgument;
		}
		struct NGPMode * mode = (struct NGPMode *)output;
		mode->version = 1;
		mode->format = 1; /* NGP_BGRX8888 */
		mode->width = vinfo.v_width;
		mode->height = vinfo.v_height;
		mode->stride = vinfo.v_rowbytes;
		mode->bytes = (uint64_t)vinfo.v_rowbytes * vinfo.v_height;
		*output_size = sizeof(*mode);
		return kIOReturnSuccess;
	}

	case 0x7002: { /* NGP_METHOD_WAIT_SWAP */
		IOSleep(16);
		return kIOReturnTimeout;
	}

	case 0x7003: { /* NGP_METHOD_COMPLETE_SWAP */
		return kIOReturnSuccess;
	}

	default:
		IOLog("HakiosGPU: unknown tunnel method %u\n", method);
		return kIOReturnUnsupported;
	}
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	if (args == NULL || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_gpu_ready() && selector != kHakiosShow && selector != kHakiosLog) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}

	/* NativeAdreno wire tunnel from Turnip / NativeAdrenoMetal */
	if (args->structureInput != NULL && args->structureInputSize >= 8 &&
	    ((const uint32_t *)args->structureInput)[0] == NATIVE_ADRENO_TUNNEL_MAGIC) {
		const uint32_t * header = (const uint32_t *)args->structureInput;
		uint32_t method = header[1];
		if (method == 0xffffffffu) {
			return kIOReturnSuccess;
		}
		{
			static uint32_t sWaits = 0;

            if (method == kNativeAdrenoMethodSubmit || method == kNativeAdrenoMethodGetDeviceInfo ||
                (method == kNativeAdrenoMethodWaitFence && sWaits++ < 3)) {
				hakios_gpu_trace("tl: tunnel method %#x, in %u bytes, out %u\n", method,
				    (unsigned) args->structureInputSize, (unsigned) args->structureOutputSize);
			}
		}
		return handleTunnel(method, (const uint8_t *)args->structureInput + 8,
		    args->structureInputSize - 8, args->structureOutput, &args->structureOutputSize);
	}

	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hakios_surface surface;
		struct hakios_show show;
		struct hakios_log log;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r)) {
		return kIOReturnBadArgument;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	if (selector != kHakiosLog) {
		hakios_gpu_trace("tl: msm method %#x, in %u bytes, out %u\n", selector,
		    (unsigned) args->structureInputSize, (unsigned) args->structureOutputSize);
	}

	switch (selector) {
	case kMsmGetParam:
		ret = hakios_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(hakios_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, hakios_gpu_last());
			}
		}
		probeReport();
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		probeReport();                          /* (the client's submission is done: its probes are read now) */
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosShow:
		ret = show(&r.show);
		break;
	case kHakiosLog:                                /* ours: a program's line into the loader's log */
		if (r.log.length >= sizeof(r.log.text)) {
			r.log.length = sizeof(r.log.text) - 1;
		}
		r.log.text[r.log.length] = 0;
		hakios_gpu_trace("%s\n", r.log.text);
		break;
	default:
		IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
