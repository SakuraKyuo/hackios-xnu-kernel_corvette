/*
 * metal_test: what iOS here believes about Metal, and what it does with the answer.
 *
 * Nothing in iOS says a word when Metal is missing. A program asks for the system's device with
 * MTLCreateSystemDefaultDevice(); if there is none it gets nil, with no log line, no error and no
 * reason, and whatever the caller does next -- fall back, draw nothing, or stop at an assertion --
 * is all that is ever seen. This prints the whole chain instead, so that a failure somewhere above
 * can be told from a failure here.
 *
 * The chain is short, and every step of it is checked below:
 *
 *   1. Metal's MTLRegisterDevices() asks IOKit for the services of class "IOAcceleratorES" --
 *      that name, and no other, is the whole of how a GPU is found;
 *   2. of each such service it reads two string properties, "MetalPluginName" and
 *      "MetalPluginClassName";
 *   3. it loads /System/Library/Extensions/<MetalPluginName>.bundle and takes the class named by
 *      MetalPluginClassName, or the bundle's principal class;
 *   4. the class must be a subclass of Metal's own private _MTLDevice, or it is dropped;
 *   5. it is sent -initWithAcceleratorPort: with the service, and that object is the device.
 *      MTLCreateSystemDefaultDevice() hands out the one device of that list, under a dispatch_once:
 *      an empty list is an answer given once and kept, silently, for the life of the process.
 *
 * So the question "is there Metal" has exactly one answer, and no capability bit anywhere stands
 * in for it. What iOS publishes about graphics besides -- MobileGestalt's GraphicsFeatureSetClass
 * and GraphicsFeatureSetFallbacks, from the device tree's "graphics-featureset-class" and
 * "graphics-featureset-fallbacks" -- is read by CoreUI and CoreThemeDefinition to choose which
 * compiled artwork to take out of an asset catalog, not by anything that decides how to draw; it
 * is printed here to show that it says the iPhone X's "APPLE4" whether or not a device exists.
 *
 * The last part asks a framework that has a path without Metal whether that path works: CoreImage
 * runs a filter kernel over four pixels and the result is read back. It is the one thing here that
 * can answer "can this system process an image at all", which the camera and Photos both need.
 *
 *   usage: metal_test
 */
#import <CoreFoundation/CoreFoundation.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#import <OpenGLES/EAGL.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <signal.h>
#import <stdio.h>
#import <stdlib.h>
#import <spawn.h>
#import <sys/wait.h>
#import <time.h>
#import <unistd.h>
#import <errno.h>
#import <sys/stat.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);

#define EXTENSIONS      "/System/Library/Extensions"

/*
 * Where a crash happened, in the addresses of the dyld shared cache as ipsw prints them.
 *
 * The cache is loaded at a sliding address, so a program counter means nothing by itself. The slide
 * is worked out from one symbol whose unslid address is known from the cache
 * (MTLCreateSystemDefaultDevice, 0x181a34b3c in 20F66 for iPhone10,3): slide = where it is now
 * minus where the cache says it is. Subtracting that from a program counter gives an address
 * `ipsw dyld a2s <cache> <address>` can name. The same trick is what src/inject.c does in reverse.
 */
#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL

static uintptr_t
cache_slide(void)
{
	return (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
}

static void
fault(int sig, siginfo_t *info, void *uap)
{
	ucontext_t *uc = uap;
	uintptr_t slide = cache_slide();
	uintptr_t pc = 0, lr = 0, fp = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
		fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	}
	/* async-signal-safe enough for a diagnostic that is about to exit anyway */
	printf("metal_test:     signal %d at address %p\n", sig, info != NULL ? info->si_addr : NULL);
	printf("metal_test:     pc  %#lx unslid (cache slide %#lx)\n", pc - slide, slide);
	printf("metal_test:     lr  %#lx unslid\n", lr - slide);
	for (int i = 0; i < 8 && fp > 0x1000; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		printf("metal_test:     #%d  %#lx unslid\n", i, ret - slide);
		if (next <= fp) {
			break;
		}
		fp = next;
	}
	_exit(99);
}

/*
 * Some of what is asked below can take the process down with it, so each such question is asked by
 * a fresh process: metal_test re-executes itself with the question as its argument (see main()).
 *
 * It must be a new process and not just a forked one. A forked child may not use Objective-C at
 * all when the parent had more than one thread, and metal_test does -- linking CoreImage and
 * OpenGLES is enough. A first attempt did fork, and every probe then died of SIGABRT inside the
 * runtime itself:
 *
 *   objc[98]: +[__NSCFNumber initialize] may have been in progress in another thread when fork()
 *   was called. We cannot safely call it or ignore it in the fork() child process. Crashing
 *   instead.
 *
 * -- objc_initializeAfterForkError, which says nothing whatever about CoreImage. exec() clears
 * that, because the runtime starts afresh.
 */
static int
in_a_new_process(const char *what, const char *argument, int secs)
{
	extern char **environ;
	char *const argv[] = { (char *)"/redwood/bin/metal_test", (char *)argument, NULL };
	pid_t pid;
	int status = 0, waited = 0;

	printf("metal_test: %s (asked by a process of its own)\n", what);
	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
		printf("metal_test:   cannot start %s %s\n", argv[0], argument);
		return 0;
	}
	while (waitpid(pid, &status, WNOHANG) == 0) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };

		nanosleep(&ts, NULL);
		if (++waited > secs * 10) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			printf("metal_test:   %s: still going after %d s, killed (it wedges here)\n", what, secs);
			return 1;
		}
	}
	if (WIFSIGNALED(status)) {
		printf("metal_test:   %s: died of signal %d\n", what, WTERMSIG(status));
	} else if (WEXITSTATUS(status) == 99) {
		printf("metal_test:   %s: it faulted, and said above where\n", what);
	} else if (WEXITSTATUS(status) != 0) {
		printf("metal_test:   %s: ended with status %d\n", what, WEXITSTATUS(status));
	}
	return 0;
}

/* A string property of an IOKit service, or NULL; the caller frees it. */
static char *
service_string(io_service_t service, const char *name)
{
	CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
	char *text = NULL;

	CFRelease(key);
	if (value != NULL) {
		if (CFGetTypeID(value) == CFStringGetTypeID()) {
			text = calloc(1, 256);
			if (text != NULL && !CFStringGetCString(value, text, 256, kCFStringEncodingUTF8)) {
				free(text);
				text = NULL;
			}
		}
		CFRelease(value);
	}
	return text;
}

/* Step 1 and 2: the services Metal looks for, and the two properties it reads of each. */
static int
accelerators(void)
{
	io_iterator_t it = IO_OBJECT_NULL;
	CFMutableDictionaryRef matching = IOServiceMatching("IOAcceleratorES");
	kern_return_t kr;
	io_service_t service;
	int found = 0;

	if (matching == NULL) {
		printf("metal_test: IOServiceMatching(\"IOAcceleratorES\") failed\n");
		return 0;
	}
	/* IOServiceGetMatchingServices consumes the dictionary. */
	kr = IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &it);
	if (kr != KERN_SUCCESS) {
		printf("metal_test: IOServiceGetMatchingServices failed, %#x\n", kr);
		return 0;
	}
	while ((service = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		io_name_t name = { 0 };
		char *plugin = service_string(service, "MetalPluginName");
		char *class_name = service_string(service, "MetalPluginClassName");

		IORegistryEntryGetName(service, name);
		printf("metal_test:   IOAcceleratorES \"%s\": MetalPluginName %s, MetalPluginClassName %s\n",
		    name, plugin ? plugin : "(none)", class_name ? class_name : "(none)");
		free(plugin);
		free(class_name);
		IOObjectRelease(service);
		found++;
	}
	IOObjectRelease(it);
	if (found == 0) {
		printf("metal_test:   none: Metal will find no device at all\n");
	}
	return found;
}

/* What the registry does have that looks like a GPU, whatever its class is called. */
static void
gpu_services(void)
{
	static const char *const classes[] = { "HakiosGPU", "IOGPU", "AGXAccelerator", "IOAccelerator" };
	int found = 0;

	for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		CFMutableDictionaryRef matching = IOServiceMatching(classes[i]);
		io_iterator_t it = IO_OBJECT_NULL;
		io_service_t service;

		if (matching == NULL || IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &it) != KERN_SUCCESS) {
			continue;
		}
		while ((service = IOIteratorNext(it)) != IO_OBJECT_NULL) {
			io_name_t name = { 0 };
			char *client = service_string(service, "IOUserClientClass");

			IORegistryEntryGetName(service, name);
			printf("metal_test:   %s \"%s\": IOUserClientClass %s\n", classes[i], name,
			    client ? client : "(none)");
			free(client);
			IOObjectRelease(service);
			found++;
		}
		IOObjectRelease(it);
	}
	if (found == 0) {
		printf("metal_test:   none\n");
	}
}

/* Steps 3 and 4: could a plug-in bundle in this image be loaded and taken for a device at all. */
static void
plugins(void)
{
	NSFileManager *files = [NSFileManager defaultManager];
	NSArray<NSString *> *entries = [files contentsOfDirectoryAtPath:@EXTENSIONS error:NULL];
	Class device_class = objc_getClass("_MTLDevice");
	int bundles = 0;

	printf("metal_test:   Metal's own base class _MTLDevice: %s\n", device_class ? "there" : "not there");
	if (entries == nil) {
		printf("metal_test:   %s is not in this image: no plug-in could be loaded\n", EXTENSIONS);
		return;
	}
	for (NSString *entry in entries) {
		if (![entry hasSuffix:@".bundle"]) {
			continue;
		}
		NSString *path = [@EXTENSIONS stringByAppendingPathComponent:entry];
		NSBundle *bundle = [NSBundle bundleWithPath:path];
		NSString *executable = [bundle objectForInfoDictionaryKey:@"CFBundleExecutable"];
		NSString *principal = [bundle objectForInfoDictionaryKey:@"NSPrincipalClass"];
		Class loaded = Nil;
		const char *verdict;

		bundles++;
		/* Taking the class is what loads the bundle's code; it is the step that fails first. */
		loaded = principal != nil ? [bundle classNamed:principal] : [bundle principalClass];
		if (loaded == Nil) {
			verdict = "the class does not load";
		} else if (device_class == Nil) {
			verdict = "loaded, but there is no _MTLDevice to compare it with";
		} else if ([loaded isSubclassOfClass:device_class]) {
			verdict = "loaded, and a _MTLDevice: Metal would take it";
		} else {
			verdict = "loaded, but not a _MTLDevice: Metal would drop it";
		}
		printf("metal_test:   %s: executable %s, principal class %s -- %s\n", [entry UTF8String],
		    executable ? [executable UTF8String] : "(none)",
		    principal ? [principal UTF8String] : "(none)", verdict);
	}
	if (bundles == 0) {
		printf("metal_test:   no .bundle in %s\n", EXTENSIONS);
	}
}

/* Step 5, as a program sees it. */
static void
the_device(void)
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();

	if (device == nil) {
		printf("metal_test:   MTLCreateSystemDefaultDevice() = nil (and says nothing about why)\n");
		return;
	}
	printf("metal_test:   MTLCreateSystemDefaultDevice() = \"%s\", class %s\n",
	    [[device name] UTF8String], class_getName([(id)device class]));
	printf("metal_test:   registry id %llu, unified memory %d, max threads per group %lu, "
	    "Apple family 1/4 %d/%d\n", (unsigned long long)[device registryID], (int)[device hasUnifiedMemory],
	    (unsigned long)[device maxThreadsPerThreadgroup].width,
	    (int)[device supportsFamily:MTLGPUFamilyApple1], (int)[device supportsFamily:MTLGPUFamilyApple4]);
}

/*
 * The five steps of the note at the top, done by hand -- and asked by a process of its own, so
 * that a step that takes the process down still names itself in the last line printed. The first
 * line is Metal's own, before any of ours: this process is fresh and Metal has never built its
 * list, so nil there is Metal's answer given with a device in the registry.
 */
@interface NSObject (HakiosMetalProbe)
- (id)initWithAcceleratorPort:(io_service_t)port;
@end

static void
metal_steps(void)
{
	id<MTLDevice> own = MTLCreateSystemDefaultDevice();
	mach_port_t main_port = MACH_PORT_NULL;
	kern_return_t kr;
	CFMutableDictionaryRef matching;
	io_iterator_t it = IO_OBJECT_NULL;
	io_service_t service;
	io_name_t name = { 0 };
	char *plugin, *class_name;

	printf("metal_test:   [pid %d] Metal's own first: MTLCreateSystemDefaultDevice() = %s\n",
	    getpid(), own != nil ? "an object" : "nil");

	/* step 1: the port Metal asks on, then the match, on that port */
	kr = IOMainPort(MACH_PORT_NULL, &main_port);
	printf("metal_test:   IOMainPort() = %#x, port %#x\n", kr, (unsigned) main_port);
	if (kr != KERN_SUCCESS) {
		main_port = MACH_PORT_NULL;     /* how main() asks; the rest can still be tested */
	}
	matching = IOServiceMatching("IOAcceleratorES");
	kr = IOServiceGetMatchingServices(main_port, matching, &it);
	printf("metal_test:   IOServiceGetMatchingServices(that port) = %#x\n", kr);
	if (kr != KERN_SUCCESS) {
		return;
	}
	service = IOIteratorNext(it);
	IOObjectRelease(it);
	if (service == IO_OBJECT_NULL) {
		printf("metal_test:   the iterator came back empty: Metal would find no device\n");
		return;
	}

	/* step 2: the two properties, read the way Metal reads them */
	IORegistryEntryGetName(service, name);
	plugin = service_string(service, "MetalPluginName");
	class_name = service_string(service, "MetalPluginClassName");
	printf("metal_test:   service \"%s\": plugin \"%s\", class \"%s\"\n", name,
	    plugin ? plugin : "(none)", class_name ? class_name : "(none)");
	if (plugin != NULL) {
		/* step 3: "/System/Library/Extensions" + name + ".bundle" */
		NSString *path = [@EXTENSIONS stringByAppendingString:
		    [NSString stringWithFormat:@"/%s.bundle", plugin]];
		NSBundle *bundle;

		printf("metal_test:   %s: %s\n", [path UTF8String],
		    access([path UTF8String], R_OK) == 0 ? "there" : "missing");
		bundle = [NSBundle bundleWithPath:path];
		printf("metal_test:   [NSBundle bundleWithPath:] = %s\n", bundle != nil ? "a bundle" : "nil");
		if (bundle != nil) {
			/* step 4: the class, and whether Metal would keep it */
			Class cls = class_name != NULL
			    ? [bundle classNamed:[NSString stringWithUTF8String:class_name]]
			    : [bundle principalClass];
			Class base = objc_getClass("_MTLDevice");

			printf("metal_test:   classNamed: = %s\n", cls != Nil ? class_getName(cls) : "Nil");
			if (cls != Nil) {
				printf("metal_test:   a _MTLDevice: %s\n",
				    base != Nil && [cls isSubclassOfClass:base] ? "yes" : "no");

				/* step 5: what the driver does with the port of the service */
				io_connect_t connection = IO_OBJECT_NULL;
				kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
				printf("metal_test:   IOServiceOpen(service) = %#x\n", kr);
				if (kr == KERN_SUCCESS) {
					IOServiceClose(connection);
				}
				printf("metal_test:   about to send -initWithAcceleratorPort:\n");
				id device = [[cls alloc] initWithAcceleratorPort:service];
				printf("metal_test:   -initWithAcceleratorPort: = %s\n",
				    device != nil ? "an object" : "nil");
				if (device != nil) {
					printf("metal_test:     class %s\n", class_getName([device class]));
					if ([device respondsToSelector:@selector(name)]) {
						printf("metal_test:     name \"%s\"\n", [[device name] UTF8String]);
					}
				}
			}
		}
	}
	free(plugin);
	free(class_name);
	IOObjectRelease(service);

	printf("metal_test:   Metal's own again: MTLCreateSystemDefaultDevice() = %s\n",
	    MTLCreateSystemDefaultDevice() != nil ? "an object" : "nil");
}

/* What iOS publishes about graphics besides: artwork, not rendering (see the note above). */
static void
gestalt(void)
{
	static const char *const keys[] = {
		"GraphicsFeatureSetClass", "GraphicsFeatureSetFallbacks", "ArtworkDeviceSubType",
		"HasExtendedColorDisplay", "main-screen-class", "DeviceSupportsMetal",
	};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
		CFTypeRef answer = MGCopyAnswer(key);
		CFStringRef text = answer ? CFCopyDescription(answer) : NULL;
		char line[256] = "(none)";

		if (text != NULL) {
			CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		printf("metal_test:   %s = %s\n", keys[i], line);
		if (answer != NULL) {
			CFRelease(answer);
		}
		CFRelease(key);
	}
}

/*
 * A framework that has a path without Metal: CoreImage. A filter kernel is run over four pixels
 * and the result read back, which tells whether this system can process an image at all.
 */
/* `options` is nil for the default context, or a dictionary to ask for something else. */
static NSDictionary *coreimage_options;
/* ... or the context is made over an EAGLContext of ours instead (coreimage_eagl). */
static int coreimage_over_eagl;

/*
 * The way back for CoreImage, if there is one. iOS 16.5's CoreImage carries a whole second back
 * end on OpenGL ES -- CI::GLContext, CI::GLMainProgram, CI::GLTextureManager beside the
 * CI::MetalContext family -- it is linked against OpenGLES, it compiles its shaders as GLSL (not
 * AIR), and CIGLIsUsable() is compiled to "return 1". The public way in is
 * +[CIContext contextWithEAGLContext:], and the one thing it wants of the library underneath,
 * CIEAGLContextTexImageIOSurface, is a call src/eagl.c already answers for CoreAnimation. So: make
 * an EAGLContext (which on an image built with GL=1, on a phone with gpu=1, is our Mesa behind
 * src/eagl.c) and hand it over.
 */
#define HAKIOS_GL      "/redwood/lib/libhakios.gl.dylib"

static EAGLContext *
make_eagl(void)
{
	EAGLContext *gl;

	/*
	 * Our OpenGL ES has to be put behind EAGLContext in THIS process first. libhakios.dylib does
	 * it only for backboardd (src/inject.c, opengl_es(), which is called for that name alone), and
	 * the way it does it -- replacing methods of EAGLContext through the ObjC runtime -- is local
	 * to a process. The library installs itself from a constructor, so loading it is enough; what
	 * it replaced it says itself in lines "hakios-gl:". Without this, EAGLContext here would be
	 * iOS's own, which is AppleMetalGLRenderer over Metal -- the thing that is missing.
	 */
	if (access(HAKIOS_GL, R_OK) != 0) {
		printf("metal_test:   %s is not in this image: build it with GL=1\n", HAKIOS_GL);
		return nil;
	}
	/*
	 * coreimage_over_eagl == 2: Mesa's software driver instead of the phone's GPU. Worth asking
	 * separately, because the window server already holds the GPU while this runs: if freedreno
	 * cannot have it too, softpipe still answers the question that matters -- whether CoreImage's
	 * OpenGL ES back end works at all behind an EAGLContext of ours.
	 */
	if (coreimage_over_eagl == 2) {
		setenv("OSMESA_SOFTWARE", "1", 1);
		printf("metal_test:   OSMESA_SOFTWARE=1: Mesa's software driver, not the GPU\n");
	}
	if (dlopen(HAKIOS_GL, RTLD_NOW) == NULL) {
		printf("metal_test:   %s: %s\n", HAKIOS_GL, dlerror());
		return nil;
	}
	printf("metal_test:   %s loaded; EAGLContext is ours now\n", HAKIOS_GL);
	gl = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];

	if (gl == nil) {
		printf("metal_test:   no OpenGL ES 3 context; trying 2\n");
		gl = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
	}
	if (gl == nil) {
		printf("metal_test:   no EAGLContext at all: this image has no OpenGL ES\n");
		return nil;
	}
	printf("metal_test:   EAGLContext: %s, API %d\n", class_getName([gl class]), (int)[gl API]);
	if (![EAGLContext setCurrentContext:gl]) {
		printf("metal_test:   the EAGLContext would not become current\n");
	}
	return gl;
}

static void
coreimage(void)
{
	@autoreleasepool {
		CIContext *context;
		CIImage *source, *result;
		CIFilter *filter;
		CGImageRef image;
		CGRect rect = CGRectMake(0, 0, 4, 4);
		unsigned char pixels[4 * 4 * 4];
		CGColorSpaceRef space;
		CGContextRef bitmap;

		/* said before the call, not after: this is the call that was dying */
		printf("metal_test:   asking for a CIContext (%s)...\n", coreimage_over_eagl ? "over an EAGLContext"
		    : coreimage_options == nil ? "the default one" : "software renderer");
		if (coreimage_over_eagl) {
			EAGLContext *gl = make_eagl();

			if (gl == nil) {
				return;
			}
			context = [CIContext contextWithEAGLContext:gl];
		} else {
			context = [CIContext contextWithOptions:coreimage_options];
		}
		if (context == nil) {
			printf("metal_test:   no CIContext: CoreImage cannot work here\n");
			return;
		}
		/*
		 * Which of CoreImage's two backends the default context took. iOS 16.5 has both:
		 * CI::GLContext (with CI::GLMainProgram, CI::GLTextureManager) and CI::MetalContext
		 * (with CI::MetalDAG, CI::MetalKernel); -[CIContext initWithOptions:] chooses between
		 * them by CIGLIsUsable(). -isOpenGLBased and -device are private, so they are asked for
		 * only if they are there.
		 */
		printf("metal_test:   CIContext: %s\n", class_getName([context class]));
		/*
		 * Sent through a typed objc_msgSend, not -performSelector:, because -isOpenGLBased
		 * returns a BOOL and not an object: ARC would take the return value for one.
		 */
		SEL gl_sel = @selector(isOpenGLBased);
		if ([context respondsToSelector:gl_sel]) {
			BOOL (*ask)(id, SEL) = (BOOL (*)(id, SEL))objc_msgSend;
			printf("metal_test:   backend: %s\n", ask(context, gl_sel)
			    ? "OpenGL ES (CI::GLContext)" : "Metal (CI::MetalContext)");
		} else {
			printf("metal_test:   backend: -isOpenGLBased is not there to ask\n");
		}
		SEL dev_sel = @selector(device);
		if ([context respondsToSelector:dev_sel]) {
			id (*ask_device)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
			id device = ask_device(context, dev_sel);
			printf("metal_test:   the context's Metal device: %s\n",
			    device ? class_getName([device class]) : "nil");
		}
		/* orange, inverted by a kernel: 1, 0.5, 0 -> 0, 0.5, 1 */
		source = [[CIImage imageWithColor:[CIColor colorWithRed:1.0 green:0.5 blue:0.0]]
		    imageByCroppingToRect:rect];
		filter = [CIFilter filterWithName:@"CIColorInvert"];
		if (filter == nil) {
			printf("metal_test:   no CIColorInvert filter\n");
			return;
		}
		[filter setValue:source forKey:kCIInputImageKey];
		result = [filter valueForKey:kCIOutputImageKey];
		if (result == nil) {
			printf("metal_test:   the filter gave no image\n");
			return;
		}
		printf("metal_test:   running the filter kernel...\n");
		image = [context createCGImage:result fromRect:rect];
		if (image == NULL) {
			printf("metal_test:   CoreImage did not render: a filter kernel cannot run here\n");
			return;
		}
		memset(pixels, 0, sizeof(pixels));
		space = CGColorSpaceCreateDeviceRGB();
		bitmap = CGBitmapContextCreate(pixels, 4, 4, 8, 4 * 4, space,
		    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
		if (bitmap != NULL) {
			CGContextDrawImage(bitmap, rect, image);
			printf("metal_test:   CoreImage rendered: first pixel %u %u %u %u (default color-managed output; use ci-linear for a numeric reference)\n",
			    pixels[0], pixels[1], pixels[2], pixels[3]);
			CGContextRelease(bitmap);
		} else {
			printf("metal_test:   CoreImage rendered, but the pixels could not be read back\n");
		}
		CGColorSpaceRelease(space);
		CGImageRelease(image);
	}
}

/* Explicit linear color spaces make the CPU reference independent of default gamma conversion. */
static int coreimage_linear(void)
{
	@autoreleasepool {
		CGColorSpaceRef linear = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (linear == NULL || device == nil) {
			printf("metal_test: air79 FAIL linear: no color space or Metal device\n");
			if (linear != NULL) CGColorSpaceRelease(linear);
			return 0;
		}
		NSDictionary *options = @{kCIContextWorkingColorSpace:(__bridge id)linear,
		    kCIContextOutputColorSpace:(__bridge id)linear};
		printf("metal_test: air79 linear: create CIContext with native Metal device, linear input/working/output\n");
		CIContext *context = [CIContext contextWithMTLDevice:device options:options];
		if (context == nil) {
			printf("metal_test: air79 FAIL linear: no CIContext\n");
			CGColorSpaceRelease(linear);
			return 0;
		}
		unsigned char input[64], output[64];
		for (unsigned i = 0; i < 16; i++) {
			input[4*i] = i * 17;
			input[4*i+1] = 64 + (i % 4) * 32;
			input[4*i+2] = 32 + (i / 4) * 48;
			input[4*i+3] = 255;
		}
		memset(output, 0xa5, sizeof(output));
		NSData *data = [NSData dataWithBytes:input length:sizeof(input)];
		CIImage *source = [CIImage imageWithBitmapData:data bytesPerRow:16 size:CGSizeMake(4, 4)
		    format:kCIFormatRGBA8 colorSpace:linear];
		CIFilter *filter = [CIFilter filterWithName:@"CIColorInvert"];
		[filter setValue:source forKey:kCIInputImageKey];
		CIImage *result = [filter valueForKey:kCIOutputImageKey];
		if (source == nil || filter == nil || result == nil) {
			printf("metal_test: air79 FAIL linear: missing image or filter\n");
			CGColorSpaceRelease(linear);
			return 0;
		}
		printf("metal_test: air79 linear: render 16 distinct RGBA pixels, compare RGB=255-input, alpha=255, tolerance 1\n");
		[context render:result toBitmap:output rowBytes:16 bounds:CGRectMake(0, 0, 4, 4)
		    format:kCIFormatRGBA8 colorSpace:linear];
		unsigned bad = 0, maximum = 0;
		for (unsigned i = 0; i < sizeof(output); i++) {
			unsigned expected = i % 4 == 3 ? 255 : 255 - input[i];
			unsigned difference = output[i] > expected ? output[i] - expected : expected - output[i];
			if (difference > maximum) maximum = difference;
			if (difference > 1) {
				if (bad < 8) printf("metal_test: air79 mismatch byte %u: got %u expected %u\n", i, output[i], expected);
				bad++;
			}
		}
		printf("metal_test: air79 linear: first %u %u %u %u; last %u %u %u %u; mismatched bytes %u/64, max error %u\n",
		    output[0], output[1], output[2], output[3], output[60], output[61], output[62], output[63], bad, maximum);
		printf("metal_test: air79 %s CoreImage linear inversion\n", bad == 0 ? "PASS" : "FAIL");
		CGColorSpaceRelease(linear);
		return bad == 0;
	}
}

/*
 * The plug-in asks for a directory for its shaders' cache before it will make a device: its
 * strings name XDG_CACHE_HOME and ".cache/metal2vulkan", and, with neither set, a home
 * directory this image does not have -- which is the "no-cachedir" it prints and then stops.
 * This makes a directory that can be written, under the first of these that will have it, and
 * sets XDG_CACHE_HOME to it before the first question about Metal is asked.
 */
static void
shader_cache_dir(void)
{
	static const char *candidates[] = {
		"/private/var/mobile/Library/Caches",
		"/private/var/tmp",
		"/redwood/cache",
	};
	char base[256] = { 0 };
	int i;

	@autoreleasepool {
		printf("metal_test:   home directory: %s; XDG_CACHE_HOME: %s\n",
		    NSHomeDirectory() != nil ? [NSHomeDirectory() UTF8String] : "(none)",
		    getenv("XDG_CACHE_HOME") != NULL ? getenv("XDG_CACHE_HOME") : "(unset)");
	}
	for (i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
		char path[300];

		mkdir(candidates[i], 0755);
		/* both shapes it might append: "/metal2vulkan" and "/.cache/metal2vulkan" */
		snprintf(path, sizeof(path), "%s/.cache", candidates[i]);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/metal2vulkan", candidates[i]);
		if (mkdir(path, 0755) == 0 || errno == EEXIST) {
			snprintf(base, sizeof(base), "%s", candidates[i]);
			break;
		}
	}
	if (base[0] == 0) {
		printf("metal_test:   no directory for the shader cache could be made\n");
		return;
	}
	setenv("XDG_CACHE_HOME", base, 1);
	printf("metal_test:   the shader cache directory: %s\n", base);
}

int
main(int argc, char **argv)
{
	struct sigaction sa = { 0 };

	/* every line as it happens: a call that kills the process must not take the log with it */
	setvbuf(stdout, NULL, _IONBF, 0);
	/* and where it died, since the kernel's report never arrives here (see fault()) */
	sa.sa_sigaction = fault;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);

	/* before anything asks Metal: the plug-in wants a shader cache directory first */
	shader_cache_dir();
	printf("metal_test: air84 ordinary client: no explicit adapter installation\n");

	/* one question, asked by a process of its own (see in_a_new_process) */
	if (argc > 1) {
		if (strcmp(argv[1], "ci-linear") == 0) {
			return coreimage_linear() ? 0 : 1;
		} else if (strcmp(argv[1], "ci-default") == 0) {
			coreimage();
		} else if (strcmp(argv[1], "ci-software") == 0) {
			coreimage_options = @{ @"kCIContextUseSoftwareRenderer": @YES };
			coreimage();
		} else if (strcmp(argv[1], "ci-eagl") == 0) {
			coreimage_over_eagl = 1;
			coreimage();
		} else if (strcmp(argv[1], "ci-eagl-software") == 0) {
			coreimage_over_eagl = 2;
			coreimage();
		} else if (strcmp(argv[1], "metal-device") == 0) {
			the_device();
		} else if (strcmp(argv[1], "metal-steps") == 0) {
			metal_steps();
		} else if (strcmp(argv[1], "plugins") == 0) {
			plugins();
		} else {
			printf("metal_test: no such question: %s\n", argv[1]);
			return 2;
		}
		return 0;
	}

	printf("metal_test: the services Metal looks for (IOAcceleratorES)\n");
	accelerators();
	printf("metal_test: what the registry has that is a GPU\n");
	gpu_services();
	if (in_a_new_process("the device a program gets", "metal-device", 20)) {
		printf("metal_test: the device is wedged: the rest is not asked (a run that ends says more)\n");
		return 0;
	}
	in_a_new_process("Metal's five steps, one by one", "metal-steps", 60);
	printf("metal_test: what iOS publishes about graphics (artwork, not rendering)\n");
	gestalt();
	printf("metal_test: CoreImage, which has a path without Metal\n");
	/*
	 * The ones that matter go first: a session can run out before the end, and these are the
	 * answer everything else is waiting for. The default context is known to crash in
	 * CI::GLContext::init() with no GL context to work with (see docs/metal.md), so the question
	 * is whether giving it one -- ours, Mesa behind src/eagl.c -- is enough.
	 */
	in_a_new_process("a CIContext over an EAGLContext (the GPU)", "ci-eagl", 120);
	in_a_new_process("a CIContext over an EAGLContext (software Mesa)", "ci-eagl-software", 180);
	in_a_new_process("the default CIContext", "ci-default", 60);
	in_a_new_process("a software-renderer CIContext", "ci-software", 60);
	/*
	 * Last on purpose: taking a bundle's principal class loads that bundle's code and runs its
	 * initialisers -- for AGXMetalA11.bundle, Apple's own driver for the A11's GPU, which this
	 * image does carry.
	 */
	in_a_new_process("loading the plug-in bundles of " EXTENSIONS, "plugins", 90);
	return 0;
}
