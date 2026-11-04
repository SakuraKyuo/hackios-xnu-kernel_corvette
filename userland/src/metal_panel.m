/* Native Metal shader -> IOSurface -> the port's existing framebuffer swap ABI. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int asynchronous;
static IOSurfaceRef reusable_surface;
static id<MTLTexture> reusable_texture;

static void deadline(int sig)
{
	(void)sig;
	static const char message[] = "metal_panel: FAIL process exceeded 40 seconds\n";
	write(STDOUT_FILENO, message, sizeof(message)-1);
	_exit(124);
}

static int completed(id<MTLCommandBuffer> command)
{
	printf("metal_panel: submit draw and texture readback\n");
	[command commit];
	for (unsigned i = 0; i < 5000; i++) {
		MTLCommandBufferStatus status = [command status];
		if (status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError) {
			NSError *error = [command error];
			printf("metal_panel: command status %lu, error %s\n", (unsigned long)status,
			    error != nil ? [[error description] UTF8String] : "none");
			return status == MTLCommandBufferStatusCompleted && error == nil;
		}
		struct timespec delay = { .tv_nsec = 1000000 };
		nanosleep(&delay, NULL);
	}
	printf("metal_panel: FAIL command timed out\n");
	return 0;
}

static int verify(const unsigned char *base, size_t stride, size_t width, size_t height,
    const unsigned char expected[4], const char *kind)
{
	size_t bad = 0;
	for (size_t y = 0; y < height; y++) {
		const unsigned char *row = base + y * stride;
		for (size_t x = 0; x < width*4; x++) {
			if (row[x] != expected[x % 4]) {
				if (bad < 4) printf("metal_panel: %s mismatch at row %lu byte %lu: got %u expected %u\n",
				    kind, (unsigned long)y, (unsigned long)x, row[x], expected[x % 4]);
				bad++;
			}
		}
	}
	printf("metal_panel: %s first BGRA %u %u %u %u; mismatched active bytes %lu/%lu\n", kind,
	    base[0], base[1], base[2], base[3], (unsigned long)bad, (unsigned long)(width*height*4));
	return bad == 0;
}

static void put32(unsigned char *request, size_t offset, uint32_t value)
{
	memcpy(request + offset, &value, sizeof(value));
}

static int swap(io_connect_t framebuffer, IOSurfaceRef surface, uint32_t width, uint32_t height)
{
	uint64_t number = 0;
	uint32_t count = 1;
	kern_return_t result = IOConnectCallScalarMethod(framebuffer, 4, NULL, 0, &number, &count);
	printf("metal_panel: SwapBegin result %#x, number %llu, outputs %u\n", result,
	    (unsigned long long)number, count);
	if (result != KERN_SUCCESS || count != 1) return 0;
	unsigned char request[0x46c] = { 0 }, answer[0x40] = { 0 };
	size_t answer_size = sizeof(answer);
	put32(request, 0x50, (uint32_t)number);
	put32(request, 0x54, IOSurfaceGetID(surface));
	put32(request, 0x64+8, width);
	put32(request, 0x64+12, height);
	put32(request, 0xa4, 1);
	put32(request, 0xc4+8, width);
	put32(request, 0xc4+12, height);
	put32(request, 0x104, 1);
	result = IOConnectCallStructMethod(framebuffer, 5, request, sizeof(request), answer, &answer_size);
	printf("metal_panel: SwapEnd result %#x, surface %u, display %ux%u\n", result,
	    IOSurfaceGetID(surface), width, height);
	if (result != KERN_SUCCESS) return 0;
	printf("metal_panel: air83 SwapWait for %llu: wait for panel copy, not a client GPU wait\n", (unsigned long long)number);
	result = IOConnectCallScalarMethod(framebuffer, 6, &number, 1, NULL, NULL);
	printf("metal_panel: air83 SwapWait result %#x, number %llu\n", result, (unsigned long long)number);
	return result == KERN_SUCCESS;
}

static int draw_surface(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline,
    id<MTLBuffer> vertices, uint32_t width, uint32_t height, const unsigned char color[4],
    io_connect_t framebuffer, const char *name)
{
	printf("metal_panel: %s: create %ux%u BGRA IOSurface\n", name, width, height);
	NSDictionary *properties = @{(__bridge id)kIOSurfaceWidth:@(width), (__bridge id)kIOSurfaceHeight:@(height),
	    (__bridge id)kIOSurfaceBytesPerElement:@4, (__bridge id)kIOSurfacePixelFormat:@(0x42475241)};
	IOSurfaceRef surface = NULL;
	if (asynchronous && framebuffer != 0 && reusable_surface != NULL) {
		surface = (IOSurfaceRef)CFRetain(reusable_surface);
		printf("metal_panel: air83 reuse the same surface %u\n", IOSurfaceGetID(surface));
	} else {
		surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
		if (surface != NULL && asynchronous && framebuffer != 0) {
			reusable_surface = (IOSurfaceRef)CFRetain(surface);
		}
	}
	if (surface == NULL) {
		printf("metal_panel: FAIL no IOSurface\n");
		return 0;
	}
	size_t stride = IOSurfaceGetBytesPerRow(surface), allocation = IOSurfaceGetAllocSize(surface);
	printf("metal_panel: %s: surface %u, row %lu, alloc %lu, plane count %lu, CPU base %p\n", name,
	    IOSurfaceGetID(surface), (unsigned long)stride, (unsigned long)allocation,
	    (unsigned long)IOSurfaceGetPlaneCount(surface), IOSurfaceGetBaseAddress(surface));
	if (stride < width*4 || allocation < stride*height) {
		printf("metal_panel: FAIL invalid surface bounds\n");
		CFRelease(surface);
		return 0;
	}
	MTLTextureDescriptor *description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
	    MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
	description.storageMode = MTLStorageModeShared;
	description.usage = MTLTextureUsageRenderTarget;
	printf("metal_panel: %s: import IOSurface through native Metal texture API\n", name);
	id<MTLTexture> texture = asynchronous && framebuffer != 0 && reusable_texture != nil ? reusable_texture :
	    [device newTextureWithDescriptor:description iosurface:surface plane:0];
	if (asynchronous && framebuffer != 0 && reusable_texture == nil) reusable_texture = texture;
	printf("metal_panel: %s: texture %s\n", name, texture != nil ? "object" : "nil");
	if (texture == nil) {
		printf("metal_panel: FAIL native IOSurface import\n");
		CFRelease(surface);
		return 0;
	}
	printf("metal_panel: %s: texture class %s, size %lux%lu, format %lu, storage %lu, surface identity %d\n",
	    name, object_getClassName(texture), (unsigned long)[texture width], (unsigned long)[texture height],
	    (unsigned long)[texture pixelFormat], (unsigned long)[texture storageMode], [texture iosurface] == surface);
	size_t read_row = (width*4 + 255) & ~(size_t)255;
	id<MTLBuffer> readback = [device newBufferWithLength:read_row*height options:MTLResourceStorageModeShared];
	id<MTLBuffer> constants = [device newBufferWithLength:48 options:MTLResourceStorageModeShared];
	if (readback == nil || constants == nil || [readback contents] == NULL || [constants contents] == NULL) {
		printf("metal_panel: FAIL no constants/readback\n");
		CFRelease(surface);
		return 0;
	}
	memset([readback contents], 0xa5, read_row*height);
	memset([constants contents], 0, 48);
	float *values = [constants contents];
	for (unsigned i = 0; i < 4; i++) values[i] = color[i]/255.0f;
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
	printf("metal_panel: %s: encode shader draw over red clear\n", name);
	id<MTLCommandBuffer> command = [queue commandBuffer];
	id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
	if (command == nil || render == nil) {
		printf("metal_panel: FAIL no command/render encoder\n");
		CFRelease(surface);
		return 0;
	}
	[render setRenderPipelineState:pipeline];
	[render setViewport:(MTLViewport){0, 0, width, height, 0, 1}];
	[render setVertexBuffer:vertices offset:0 atIndex:14];
	[render setFragmentBuffer:constants offset:0 atIndex:14];
	unsigned draws = asynchronous && framebuffer != 0 ? 32 : 1;
	printf("metal_panel: air83 encode %u full-screen draws\n", draws);
	for (unsigned draw = 0; draw < draws; draw++) {
		[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[render endEncoding];
	id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
	if (blit == nil) {
		printf("metal_panel: FAIL no blit encoder\n");
		CFRelease(surface);
		return 0;
	}
	[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
	    sourceSize:MTLSizeMake(width,height,1) toBuffer:readback destinationOffset:0
	    destinationBytesPerRow:read_row destinationBytesPerImage:read_row*height];
	[blit endEncoding];
	int early_swap = asynchronous && framebuffer != 0;
	if (early_swap) {
		printf("metal_panel: air83 async: commit then swap BEFORE any command status/fence wait\n");
		[command commit];
		if (!swap(framebuffer, surface, width, height)) {
			CFRelease(surface);
			return 0;
		}
		printf("metal_panel: air83 async: panel copy complete; now inspect both pixel views\n");
	} else if (!completed(command)) {
		CFRelease(surface);
		return 0;
	}
	unsigned char expected[4] = { color[2], color[1], color[0], color[3] };
	int gpu_ok = verify([readback contents], read_row, width, height, expected, "GPU readback");
	printf("metal_panel: %s: lock IOSurface CPU mapping read-only\n", name);
	kern_return_t locked = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	int surface_ok = 0;
	if (locked == KERN_SUCCESS && IOSurfaceGetBaseAddress(surface) != NULL) {
		surface_ok = verify(IOSurfaceGetBaseAddress(surface), stride, width, height, expected, "IOSurface mapping");
	}
	printf("metal_panel: %s: lock result %#x; GPU pixels=%d surface pixels=%d\n", name, locked, gpu_ok, surface_ok);
	if (locked == KERN_SUCCESS) IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	int ok = gpu_ok && surface_ok;
	if (ok && framebuffer != 0) {
		printf("metal_panel: %s: pixels verified; present surface %u\n", name, IOSurfaceGetID(surface));
		ok = early_swap || swap(framebuffer, surface, width, height);
		if (ok) {
			printf("metal_panel: %s: swap accepted, hold picture for 1 second\n", name);
			sleep(1);
		}
	}
	printf("metal_panel: %s %s: native shader + IOSurface%s\n", ok ? "PASS" : "FAIL", name,
	    framebuffer != 0 ? " + swap accepted" : " readback");
	/* The framebuffer retains a queued surface. Keep texture and memory alive through the hold. */
	CFRelease(surface);
	return ok;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	asynchronous = argc == 2 && strcmp(argv[1], "async") == 0;
	signal(SIGALRM, deadline);
	alarm(40);
	const char *directories[] = { "/private/var/tmp", "/private/var/tmp/.cache",
	    "/private/var/tmp/metal2vulkan", "/private/var/tmp/.cache/metal2vulkan" };
	for (unsigned i=0; i<sizeof(directories)/sizeof(directories[0]); i++) {
		if (mkdir(directories[i],0755)!=0 && errno!=EEXIST) return 1;
	}
	setenv("XDG_CACHE_HOME","/private/var/tmp",1);
	@autoreleasepool {
		@try {
			printf("metal_panel: air83 create native Metal device\n");
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			id<MTLCommandQueue> queue = [device newCommandQueue];
			NSError *error = nil;
			id<MTLLibrary> library = [device newLibraryWithFile:
			    @"/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib" error:&error];
			MTLRenderPipelineDescriptor *description = [[MTLRenderPipelineDescriptor alloc] init];
			description.vertexFunction = [library newFunctionWithName:@"clearVertex"];
			description.fragmentFunction = [library newFunctionWithName:@"clearFragment01"];
			description.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			printf("metal_panel: build BGRA pipeline\n");
			id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:description error:&error];
			if (device == nil || queue == nil || library == nil || pipeline == nil) {
				printf("metal_panel: FAIL device/queue/library/pipeline: %s\n", [[error description] UTF8String]);
				return 1;
			}
			static const float positions[3][4] = { {-1,-1,0,1}, {3,-1,0,1}, {-1,3,0,1} };
			id<MTLBuffer> vertices = [device newBufferWithLength:sizeof(positions) options:MTLResourceStorageModeShared];
			if (vertices == nil || [vertices contents] == NULL) return 1;
			memcpy([vertices contents],positions,sizeof(positions));
			const unsigned char blue[4]={0,0,255,255}, green[4]={0,255,0,255};
			if (!draw_surface(device,queue,pipeline,vertices,64,64,blue,0,"small64")) return 1;
			io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOMobileFramebuffer"));
			io_connect_t framebuffer = 0;
			kern_return_t result = service != 0 ? IOServiceOpen(service,mach_task_self(),0,&framebuffer) : KERN_FAILURE;
			if (service != 0) IOObjectRelease(service);
			printf("metal_panel: framebuffer open result %#x\n",result);
			if (result != KERN_SUCCESS) return 1;
			uint64_t dimensions[2]={0,0};
			uint32_t count=2;
			result=IOConnectCallScalarMethod(framebuffer,8,NULL,0,dimensions,&count);
			printf("metal_panel: display size result %#x: %llux%llu\n",result,
			    (unsigned long long)dimensions[0],(unsigned long long)dimensions[1]);
			if (result!=KERN_SUCCESS || count!=2 || dimensions[0]==0 || dimensions[1]==0 ||
			    dimensions[0]>4096 || dimensions[1]>4096) { IOServiceClose(framebuffer); return 1; }
			int ok = 1;
			unsigned frames = asynchronous ? 6 : 2;
			for (unsigned frame = 0; frame < frames && ok; frame++) {
				printf("metal_panel: air83 panel frame %u/%u, mode %s\n", frame+1, frames, asynchronous ? "async-reuse" : "sync");
				ok = draw_surface(device,queue,pipeline,vertices,(uint32_t)dimensions[0],(uint32_t)dimensions[1],
				    frame % 2 ? green : blue,framebuffer,frame % 2 ? "panel-green" : "panel-blue");
			}
			reusable_texture = nil;
			if (reusable_surface != NULL) { CFRelease(reusable_surface); reusable_surface = NULL; }
			IOServiceClose(framebuffer);
			printf("metal_panel: %s air83 native Metal panel %s (visual confirmation still required)\n",ok ? "PASS" : "FAIL",
			    asynchronous ? "async reuse6" : "sync2");
			return ok ? 0 : 1;
		} @catch(NSException *exception) {
			printf("metal_panel: FAIL exception %s: %s\n",[[exception name] UTF8String],[[exception reason] UTF8String]);
			return 1;
		}
	}
}
