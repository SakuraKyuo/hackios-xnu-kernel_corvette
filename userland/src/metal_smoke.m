/* Metal memory and render-clear checks without shader libraries or compiler services. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void expired(int sig)
{
	(void)sig;
	static const char message[] = "metal_smoke: FAIL process exceeded 45 seconds\n";
	write(STDOUT_FILENO, message, sizeof(message) - 1);
	_exit(124);
}

static int complete(id<MTLCommandBuffer> command, const char *operation)
{
	printf("metal_smoke: %s: commit\n", operation);
	[command commit];
	for (unsigned i = 0; i < 5000; i++) {
		MTLCommandBufferStatus status = [command status];
		if (status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError) {
			NSError *error = [command error];
			printf("metal_smoke: %s: status %lu, error %s\n", operation, (unsigned long)status,
			    error != nil ? [[error description] UTF8String] : "none");
			return status == MTLCommandBufferStatusCompleted && error == nil;
		}
		struct timespec delay = { .tv_nsec = 1000000 };
		nanosleep(&delay, NULL);
	}
	printf("metal_smoke: FAIL %s: completion timed out, status %lu\n", operation,
	    (unsigned long)[command status]);
	return 0;
}

static int fill_buffer(id<MTLDevice> device, id<MTLCommandQueue> queue)
{
	const NSUInteger size = 4096;
	printf("metal_smoke: fill: allocate shared buffer (%lu bytes)\n", (unsigned long)size);
	id<MTLBuffer> buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
	if (buffer == nil || [buffer contents] == NULL) {
		printf("metal_smoke: FAIL fill: no mapped buffer\n");
		return 0;
	}
	memset([buffer contents], 0xa5, size);
	id<MTLCommandBuffer> command = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
	if (command == nil || blit == nil) {
		printf("metal_smoke: FAIL fill: no command or blit encoder\n");
		return 0;
	}
	printf("metal_smoke: fill: encode 0xa5 -> 0x5a\n");
	[blit fillBuffer:buffer range:NSMakeRange(0, size) value:0x5a];
	[blit endEncoding];
	if (!complete(command, "fill")) return 0;
	const unsigned char *pixels = [buffer contents];
	NSUInteger bad = 0;
	for (NSUInteger i = 0; i < size; i++) bad += pixels[i] != 0x5a;
	printf("metal_smoke: fill: first %02x %02x %02x %02x, mismatched bytes %lu/%lu\n",
	    pixels[0], pixels[1], pixels[2], pixels[3], (unsigned long)bad, (unsigned long)size);
	printf("metal_smoke: %s fill\n", bad == 0 ? "PASS" : "FAIL");
	return bad == 0;
}

static int clear_texture(id<MTLDevice> device, id<MTLCommandQueue> queue)
{
	const NSUInteger width = 64, height = 64, row = 256, size = row * height;
	printf("metal_smoke: clear: allocate private RGBA8 64x64 target and shared readback\n");
	MTLTextureDescriptor *description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
	    MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
	description.storageMode = MTLStorageModePrivate;
	description.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> texture = [device newTextureWithDescriptor:description];
	id<MTLBuffer> readback = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
	if (texture == nil || readback == nil || [readback contents] == NULL) {
		printf("metal_smoke: FAIL clear: no target or mapped readback\n");
		return 0;
	}
	static const unsigned char colors[3][4] = {
		{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}
	};
	for (unsigned frame = 0; frame < 3; frame++) {
		const unsigned char *expected = colors[frame];
		memset([readback contents], 0xa5, size);
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(expected[0] / 255.0,
		    expected[1] / 255.0, expected[2] / 255.0, 1.0);
		printf("metal_smoke: clear frame %u/3: encode load-clear %u %u %u %u\n", frame + 1,
		    expected[0], expected[1], expected[2], expected[3]);
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
		if (command == nil || render == nil) {
			printf("metal_smoke: FAIL clear: no command or render encoder\n");
			return 0;
		}
		[render endEncoding];
		printf("metal_smoke: clear frame %u/3: encode texture -> readback\n", frame + 1);
		id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
		if (blit == nil) {
			printf("metal_smoke: FAIL clear: no readback blit encoder\n");
			return 0;
		}
		[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		    sourceSize:MTLSizeMake(width, height, 1) toBuffer:readback destinationOffset:0
		    destinationBytesPerRow:row destinationBytesPerImage:size];
		[blit endEncoding];
		if (!complete(command, "clear/readback")) return 0;
		const unsigned char *pixels = [readback contents];
		NSUInteger bad = 0, first_bad = size;
		for (NSUInteger i = 0; i < size; i++) {
			if (pixels[i] != expected[i % 4]) {
				if (first_bad == size) first_bad = i;
				bad++;
			}
		}
		printf("metal_smoke: clear frame %u/3: first %u %u %u %u; last %u %u %u %u; mismatched bytes %lu/%lu\n",
		    frame + 1, pixels[0], pixels[1], pixels[2], pixels[3], pixels[size-4], pixels[size-3],
		    pixels[size-2], pixels[size-1], (unsigned long)bad, (unsigned long)size);
		if (bad != 0) {
			printf("metal_smoke: FAIL clear: first mismatch at %lu: got %u expected %u\n",
			    (unsigned long)first_bad, pixels[first_bad], expected[first_bad % 4]);
			return 0;
		}
	}
	printf("metal_smoke: PASS clear: all 3 colors match at every pixel\n");
	return 1;
}

/* Runtime type evidence for the source-compiled versus native AIR library boundary. */
static void bridge_methods(Class cls, const char *kind)
{
	unsigned count = 0;
	Method *methods = class_copyMethodList(cls, &count);
	printf("metal_smoke: air77 %s class %s: %u own methods\n", kind, class_getName(cls), count);
	unsigned printed = 0;
	for (unsigned i = 0; i < count && printed < 96; i++) {
		const char *name = sel_getName(method_getName(methods[i]));
		if (strstr(name, "Library") || strstr(name, "library") || strstr(name, "Function") ||
		    strstr(name, "function") || strstr(name, "Data") || strstr(name, "data") ||
		    strstr(name, "serial") || strstr(name, "IR") || strstr(name, "bitcode") ||
		    strstr(name, "Binary") || strstr(name, "binary") || strcmp(name, "device") == 0) {
			printf("metal_smoke: air77 method %s %s %s\n", class_getName(cls), name,
			    method_getTypeEncoding(methods[i]));
			printed++;
		}
	}
	free(methods);
}

static void bridge_object(id object, const char *kind, int inspect_objects)
{
	if (object == nil) {
		printf("metal_smoke: air77 %s: nil\n", kind);
		return;
	}
	printf("metal_smoke: air77 %s: class %s\n", kind, class_getName(object_getClass(object)));
	for (Class cls = object_getClass(object); cls != Nil && cls != [NSObject class]; cls = class_getSuperclass(cls)) {
		bridge_methods(cls, kind);
		unsigned count = 0;
		Ivar *ivars = class_copyIvarList(cls, &count);
		for (unsigned i = 0; i < count && i < 48; i++) {
			const char *name = ivar_getName(ivars[i]);
			const char *type = ivar_getTypeEncoding(ivars[i]);
			printf("metal_smoke: air77 ivar %s.%s type %s offset %ld\n", class_getName(cls),
			    name, type != NULL ? type : "?", (long)ivar_getOffset(ivars[i]));
			/* Read only runtime-declared object ivars. Never guess a C++ backing layout. */
			if (inspect_objects && type != NULL && type[0] == '@' &&
			    (strstr(name, "data") || strstr(name, "Data") || strstr(name, "library") || strstr(name, "Library"))) {
				id backing = object_getIvar(object, ivars[i]);
				if (backing != nil && backing != object) {
					printf("metal_smoke: air77 backing %s: class %s\n", name, class_getName(object_getClass(backing)));
					bridge_methods(object_getClass(backing), "backing");
				}
			}
		}
		free(ivars);
	}
}

static void bridge_device(id<MTLDevice> device)
{
	static const char *selectors[] = {
		"newLibraryWithSource:options:error:", "newLibraryWithSource:options:completionHandler:",
		"newLibraryWithData:error:", "newLibraryWithFile:error:",
		"newRenderPipelineStateWithDescriptor:error:", "newComputePipelineStateWithFunction:error:"
	};
	for (unsigned i = 0; i < sizeof(selectors) / sizeof(selectors[0]); i++) {
		SEL selector = sel_registerName(selectors[i]);
		for (Class cls = object_getClass(device); cls != Nil; cls = class_getSuperclass(cls)) {
			unsigned count = 0;
			Method *methods = class_copyMethodList(cls, &count);
			int found = 0;
			for (unsigned j = 0; j < count; j++) {
				if (sel_isEqual(method_getName(methods[j]), selector)) {
					Dl_info info = { 0 };
					dladdr((const void *)method_getImplementation(methods[j]), &info);
					printf("metal_smoke: air77 owner %s: %s; image %s; type %s\n", selectors[i],
					    class_getName(cls), info.dli_fname != NULL ? info.dli_fname : "?",
					    method_getTypeEncoding(methods[j]));
					found = 1;
					break;
				}
			}
			free(methods);
			if (found) break;
		}
	}
}

/* Signatures checked against the AIR modules in the staged System image. */
static int shader_draw(id<MTLDevice> device, id<MTLCommandQueue> queue, int from_data)
{
	NSString *path = @"/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib";
	NSError *error = nil;
	id<MTLLibrary> library;
	const char *mode = from_data == 3 ? "source-async" : from_data == 2 ? "source" : from_data ? "data" : "file";
	printf("metal_smoke: draw: library mode %s, parent %d\n", mode, getppid());
	if (from_data >= 2) {
		NSString *msl = @"#include <metal_stdlib>\n"
		    "using namespace metal;\n"
		    "vertex float4 clearVertex(constant float4 *vtx [[buffer(14)]], uint vid [[vertex_id]]) { return vtx[vid]; }\n"
		    "struct ClearFragmentInput { float4 f; int4 i; uint4 ui; };\n"
		    "fragment float4 clearFragment01(constant ClearFragmentInput &color [[buffer(14)]]) { return color.f; }\n";
		printf("metal_smoke: draw: compile equivalent MSL with newLibraryWithSource\n");
		if (from_data == 3) {
			dispatch_semaphore_t ready = dispatch_semaphore_create(0);
			__block id<MTLLibrary> answer = nil;
			__block NSError *failure = nil;
			printf("metal_smoke: air80 async: request source compilation\n");
			[device newLibraryWithSource:msl options:nil completionHandler:^(id<MTLLibrary> result, NSError *problem) {
				answer = result;
				failure = problem;
				printf("metal_smoke: air80 async: callback library %s, error %s\n",
				    result != nil ? class_getName(object_getClass(result)) : "nil",
				    problem != nil ? [[problem description] UTF8String] : "none");
				dispatch_semaphore_signal(ready);
			}];
			if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0) {
				printf("metal_smoke: FAIL async: compiler callback timed out\n");
				return 0;
			}
			library = answer;
			error = failure;
		} else {
			library = [device newLibraryWithSource:msl options:nil error:&error];
		}
	} else if (from_data) {
		NSData *bytes = [NSData dataWithContentsOfFile:path options:0 error:&error];
		if (bytes == nil || [bytes length] == 0) {
			printf("metal_smoke: FAIL draw: read library: %s\n", [[error description] UTF8String]);
			return 0;
		}
		void *copy = malloc([bytes length]);
		if (copy == NULL) {
			printf("metal_smoke: FAIL draw: no library copy\n");
			return 0;
		}
		memcpy(copy, [bytes bytes], [bytes length]);
		dispatch_data_t data = dispatch_data_create(copy, [bytes length],
		    dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_FREE);
		if (data == nil) {
			free(copy);
			printf("metal_smoke: FAIL draw: no dispatch data\n");
			return 0;
		}
		error = nil;
		library = [device newLibraryWithData:data error:&error];
	} else {
		library = [device newLibraryWithFile:path error:&error];
	}
	printf("metal_smoke: draw: library %s, error %s\n", library != nil ? "object" : "nil",
	    error != nil ? [[error description] UTF8String] : "none");
	if (library == nil) return 0;
	bridge_device(device);
	bridge_object(library, "library", 1);
	NSArray<NSString *> *names = [library functionNames];
	printf("metal_smoke: draw: %lu functions, clearVertex=%d clearFragment01=%d\n",
	    (unsigned long)[names count], [names containsObject:@"clearVertex"], [names containsObject:@"clearFragment01"]);
	printf("metal_smoke: draw: create vertex and fragment functions\n");
	id<MTLFunction> vertex = [library newFunctionWithName:@"clearVertex"];
	id<MTLFunction> fragment = [library newFunctionWithName:@"clearFragment01"];
	if (vertex == nil || fragment == nil) {
		printf("metal_smoke: FAIL draw: vertex %s fragment %s\n", vertex != nil ? "object" : "nil",
		    fragment != nil ? "object" : "nil");
		return 0;
	}
	bridge_object(vertex, "vertex", 1);
	bridge_object(fragment, "fragment", 1);
	printf("metal_smoke: draw: library device %s, vertex device %s, fragment device %s\n",
	    [library device] == device ? "same" : "different", [vertex device] == device ? "same" : "different",
	    [fragment device] == device ? "same" : "different");
	printf("metal_smoke: draw: function types %lu/%lu; create pipeline\n",
	    (unsigned long)[vertex functionType], (unsigned long)[fragment functionType]);
	MTLRenderPipelineDescriptor *pipeline_description = [[MTLRenderPipelineDescriptor alloc] init];
	pipeline_description.vertexFunction = vertex;
	pipeline_description.fragmentFunction = fragment;
	pipeline_description.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	error = nil;
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_description error:&error];
	printf("metal_smoke: draw: pipeline %s, error %s\n", pipeline != nil ? "object" : "nil",
	    error != nil ? [[error description] UTF8String] : "none");
	if (pipeline == nil) return 0;

	const NSUInteger width = 64, height = 64, row = 256, size = row * height;
	static const float positions[3][4] = {
		{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}
	};
	printf("metal_smoke: draw: allocate vertices, constants, target and readback\n");
	id<MTLBuffer> vertices = [device newBufferWithLength:sizeof(positions) options:MTLResourceStorageModeShared];
	id<MTLBuffer> constants = [device newBufferWithLength:48 options:MTLResourceStorageModeShared];
	id<MTLBuffer> readback = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
	MTLTextureDescriptor *texture_description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
	    MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
	texture_description.storageMode = MTLStorageModePrivate;
	texture_description.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> texture = [device newTextureWithDescriptor:texture_description];
	if (vertices == nil || constants == nil || readback == nil || texture == nil ||
	    [vertices contents] == NULL || [constants contents] == NULL || [readback contents] == NULL) {
		printf("metal_smoke: FAIL draw: missing resource or CPU mapping\n");
		return 0;
	}
	memcpy([vertices contents], positions, sizeof(positions));
	static const unsigned char colors[2][4] = { {0, 255, 0, 255}, {0, 0, 255, 255} };
	const unsigned frames = from_data == 3 ? 96 : 2;
	for (unsigned frame = 0; frame < frames; frame++) {
		const unsigned char *expected = colors[frame % 2];
		float *values = [constants contents];
		memset(values, 0, 48);
		for (unsigned i = 0; i < 4; i++) values[i] = expected[i] / 255.0f;
		memset([readback contents], 0xa5, size);
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
		printf("metal_smoke: draw frame %u/%u: red clear then triangle, shader color %u %u %u %u\n", frame + 1, frames,
		    expected[0], expected[1], expected[2], expected[3]);
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
		if (command == nil || render == nil) {
			printf("metal_smoke: FAIL draw: no command or render encoder\n");
			return 0;
		}
		[render setRenderPipelineState:pipeline];
		[render setViewport:(MTLViewport){0, 0, width, height, 0, 1}];
		[render setVertexBuffer:vertices offset:0 atIndex:14];
		[render setFragmentBuffer:constants offset:0 atIndex:14];
		[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[render endEncoding];
		printf("metal_smoke: draw frame %u/%u: copy texture -> readback\n", frame + 1, frames);
		id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
		if (blit == nil) {
			printf("metal_smoke: FAIL draw: no readback encoder\n");
			return 0;
		}
		[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		    sourceSize:MTLSizeMake(width, height, 1) toBuffer:readback destinationOffset:0
		    destinationBytesPerRow:row destinationBytesPerImage:size];
		[blit endEncoding];
		if (!complete(command, "draw/readback")) return 0;
		const unsigned char *pixels = [readback contents];
		NSUInteger bad = 0, first_bad = size;
		for (NSUInteger i = 0; i < size; i++) {
			if (pixels[i] != expected[i % 4]) {
				if (first_bad == size) first_bad = i;
				bad++;
			}
		}
		printf("metal_smoke: draw frame %u/%u: first %u %u %u %u; last %u %u %u %u; mismatched bytes %lu/%lu\n",
		    frame + 1, frames, pixels[0], pixels[1], pixels[2], pixels[3], pixels[size-4], pixels[size-3],
		    pixels[size-2], pixels[size-1], (unsigned long)bad, (unsigned long)size);
		if (bad != 0) {
			printf("metal_smoke: FAIL draw: first mismatch at %lu: got %u expected %u\n",
			    (unsigned long)first_bad, pixels[first_bad], expected[first_bad % 4]);
			return 0;
		}
	}
	printf("metal_smoke: PASS draw-%s: %u frames, every pixel matches\n", mode, frames);
	return 1;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, expired);
	alarm(45);
	static const char *directories[] = {
		"/private/var/tmp", "/private/var/tmp/.cache", "/private/var/tmp/metal2vulkan",
		"/private/var/tmp/.cache/metal2vulkan"
	};
	for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		if (mkdir(directories[i], 0755) != 0 && errno != EEXIST) {
			printf("metal_smoke: FAIL shader cache directory %s: errno %d\n", directories[i], errno);
			return 1;
		}
	}
	setenv("XDG_CACHE_HOME", "/private/var/tmp", 1);
	@autoreleasepool {
		@try {
			printf("metal_smoke: air80 pid %d, test %s: create Metal device\n", getpid(),
			    argc == 2 ? argv[1] : "invalid");
			if (argc != 2 || (strcmp(argv[1], "fill") && strcmp(argv[1], "clear") &&
			    strcmp(argv[1], "draw-file") && strcmp(argv[1], "draw-data") && strcmp(argv[1], "draw-source") && strcmp(argv[1], "draw-source-async"))) {
				printf("metal_smoke: usage: metal_smoke fill|clear|draw-file|draw-data|draw-source|draw-source-async\n");
				return 2;
			}
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			if (device == nil) {
				printf("metal_smoke: FAIL no device\n");
				return 1;
			}
			printf("metal_smoke: air84 ordinary client: no explicit adapter installation\n");
			printf("metal_smoke: device %s; create command queue\n", [[device name] UTF8String]);
			id<MTLCommandQueue> queue = [device newCommandQueue];
			if (queue == nil) {
				printf("metal_smoke: FAIL no command queue\n");
				return 1;
			}
			int ok;
			if (strcmp(argv[1], "fill") == 0) ok = fill_buffer(device, queue);
			else if (strcmp(argv[1], "clear") == 0) ok = clear_texture(device, queue);
			else ok = shader_draw(device, queue, strcmp(argv[1], "draw-source-async") == 0 ? 3 :
			    strcmp(argv[1], "draw-source") == 0 ? 2 :
			    strcmp(argv[1], "draw-data") == 0);
			alarm(0);
			return ok ? 0 : 1;
		} @catch (NSException *exception) {
			printf("metal_smoke: FAIL exception %s: %s\n", [[exception name] UTF8String],
			    [[exception reason] UTF8String]);
			return 1;
		}
	}
}
