/* Adapt Apple's source-compiled libraries to the native plug-in's AIR library objects. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include "metal_debug.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

typedef id<MTLLibrary> (*SourceCreate)(id, SEL, NSString *, MTLCompileOptions *, NSError **)
    __attribute__((ns_returns_retained));
typedef void (*SourceCreateAsync)(id, SEL, NSString *, MTLCompileOptions *,
    void (^)(id<MTLLibrary>, NSError *));
static SourceCreate original_source;
static SourceCreateAsync original_source_async;

static NSError *bridge_error(NSString *description)
{
	return [NSError errorWithDomain:@"org.hakios.metal-source-bridge" code:1
	    userInfo:@{NSLocalizedDescriptionKey:description}];
}

static id<MTLLibrary> native_library(id<MTLDevice> device, id<MTLLibrary> compiled, NSError **error)
{
	if (compiled == nil) return nil;
	Class native_class = objc_getClass("NBLibrary");
	if (native_class != Nil && [(id)compiled isKindOfClass:native_class]) return compiled;
	SEL serialize = sel_registerName("serializeToURL:error:");
	NSMethodSignature *signature = [(id)compiled methodSignatureForSelector:serialize];
	if (signature == nil || [signature numberOfArguments] != 4 ||
	    (strcmp([signature methodReturnType], @encode(BOOL)) != 0)) {
		if (error != NULL) *error = bridge_error(@"compiled library has no verified serializeToURL:error: method");
		return nil;
	}
	char temporary[] = "/private/var/tmp/hakios-air78-XXXXXX";
	int fd = mkstemp(temporary);
	if (fd < 0) {
		if (error != NULL) *error = bridge_error(@"cannot create RAM temporary library");
		return nil;
	}
	close(fd);
	NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:temporary]];
	if (hakios_metal_debug_enabled()) printf("metal_bridge: air78 serialize %s via verified BOOL method\n", class_getName(object_getClass(compiled)));
	NSError *failure = nil;
	BOOL (*write_library)(id, SEL, NSURL *, NSError **) = (void *)objc_msgSend;
	BOOL written = NO;
	@try {
		written = write_library(compiled, serialize, url, &failure);
	} @catch (NSException *exception) {
		failure = bridge_error([NSString stringWithFormat:@"serialization exception %@: %@",
		    [exception name], [exception reason]]);
	}
	if (!written) {
		unlink(temporary);
		printf("metal_bridge: air78 serialization failed: %s\n", [[failure description] UTF8String]);
		if (error != NULL) *error = failure ?: bridge_error(@"library serialization returned false");
		return nil;
	}
	NSData *bytes = [NSData dataWithContentsOfURL:url options:0 error:&failure];
	unlink(temporary);
	if (bytes == nil || [bytes length] < 88 || [bytes length] > 32 * 1024 * 1024 ||
	    memcmp([bytes bytes], "MTLB", 4) != 0) {
		printf("metal_bridge: air78 rejected serialized library: %lu bytes, magic %02x %02x %02x %02x\n",
		    (unsigned long)[bytes length], [bytes length] >= 4 ? ((const unsigned char *)[bytes bytes])[0] : 0,
		    [bytes length] >= 4 ? ((const unsigned char *)[bytes bytes])[1] : 0,
		    [bytes length] >= 4 ? ((const unsigned char *)[bytes bytes])[2] : 0,
		    [bytes length] >= 4 ? ((const unsigned char *)[bytes bytes])[3] : 0);
		if (error != NULL) *error = failure ?: bridge_error(@"serialized output is not a bounded MTLB container");
		return nil;
	}
	void *copy = malloc([bytes length]);
	if (copy == NULL) {
		if (error != NULL) *error = bridge_error(@"cannot copy serialized AIR library");
		return nil;
	}
	memcpy(copy, [bytes bytes], [bytes length]);
	dispatch_data_t data = dispatch_data_create(copy, [bytes length],
	    dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_FREE);
	if (data == nil) {
		free(copy);
		if (error != NULL) *error = bridge_error(@"cannot create AIR dispatch data");
		return nil;
	}
	if (hakios_metal_debug_enabled()) printf("metal_bridge: air78 import serialized MTLB: %lu bytes\n", (unsigned long)[bytes length]);
	failure = nil;
	id<MTLLibrary> native = [device newLibraryWithData:data error:&failure];
	if (hakios_metal_debug_enabled()) printf("metal_bridge: air78 native library %s, functions %lu, error %s\n",
	    native != nil ? class_getName(object_getClass(native)) : "nil",
	    native != nil ? (unsigned long)[[native functionNames] count] : 0,
	    failure != nil ? [[failure description] UTF8String] : "none");
	if (native == nil && error != NULL) *error = failure ?: bridge_error(@"native AIR import returned nil");
	return native;
}

static id<MTLLibrary> source_library(id self, SEL cmd, NSString *source, MTLCompileOptions *options,
    NSError **error) __attribute__((ns_returns_retained));
static id<MTLLibrary> source_library(id self, SEL cmd, NSString *source, MTLCompileOptions *options,
    NSError **error)
{
	NSError *failure = nil;
	id<MTLLibrary> compiled = original_source(self, cmd, source, options, &failure);
	if (compiled == nil) {
		printf("metal_bridge: air78 compiler returned nil: %s\n", [[failure description] UTF8String]);
		if (error != NULL) *error = failure;
		return nil;
	}
	id<MTLLibrary> native = native_library(self, compiled, &failure);
	if (error != NULL) *error = failure;
	return native;
}

static void source_library_async(id self, SEL cmd, NSString *source, MTLCompileOptions *options,
    void (^completion)(id<MTLLibrary>, NSError *))
{
	original_source_async(self, cmd, source, options, ^(id<MTLLibrary> compiled, NSError *failure) {
		@autoreleasepool {
			id<MTLLibrary> native = compiled != nil ? native_library(self, compiled, &failure) : nil;
			if (completion != nil) completion(native, failure);
		}
	});
}

/* Process-local inherited API adapter: do not create a GPU device from a dyld constructor. */
static BOOL native_device(id device)
{
	Class cls = objc_getClass("NativeBridgeMetalDevice");
	return cls != Nil && [device isKindOfClass:cls];
}

static id<MTLLibrary> auto_source_library(id self, SEL cmd, NSString *source, MTLCompileOptions *options,
    NSError **error) __attribute__((ns_returns_retained));
static id<MTLLibrary> auto_source_library(id self, SEL cmd, NSString *source, MTLCompileOptions *options,
    NSError **error)
{
	if (!native_device(self)) return original_source(self, cmd, source, options, error);
	if (hakios_metal_debug_enabled()) fprintf(stderr, "metal_auto: air84 native synchronous source request, pid %d\n", getpid());
	return source_library(self, cmd, source, options, error);
}

static void auto_source_library_async(id self, SEL cmd, NSString *source, MTLCompileOptions *options,
    void (^completion)(id<MTLLibrary>, NSError *))
{
	if (!native_device(self)) {
		original_source_async(self, cmd, source, options, completion);
		return;
	}
	if (hakios_metal_debug_enabled()) fprintf(stderr, "metal_auto: air84 native asynchronous source request, pid %d\n", getpid());
	source_library_async(self, cmd, source, options, completion);
}

__attribute__((constructor))
static void automatic_source_bridge(void)
{
	@autoreleasepool {
		Class apple = objc_getClass("_MTLDevice");
		SEL sync = @selector(newLibraryWithSource:options:error:);
		SEL async = @selector(newLibraryWithSource:options:completionHandler:);
		Method sync_method = apple != Nil ? class_getInstanceMethod(apple, sync) : NULL;
		Method async_method = apple != Nil ? class_getInstanceMethod(apple, async) : NULL;
		if (sync_method == NULL || async_method == NULL ||
		    method_getNumberOfArguments(sync_method) != 5 || method_getNumberOfArguments(async_method) != 5) {
			fprintf(stderr, "metal_auto: air84 FAIL missing or incompatible _MTLDevice source methods, pid %d\n", getpid());
			return;
		}
		/* The original bytecode bridge below is identical to the air78/80 implementation. */
		original_source = (SourceCreate)method_getImplementation(sync_method);
		original_source_async = (SourceCreateAsync)method_getImplementation(async_method);
		class_replaceMethod(apple, sync, (IMP)auto_source_library, method_getTypeEncoding(sync_method));
		class_replaceMethod(apple, async, (IMP)auto_source_library_async, method_getTypeEncoding(async_method));
		/* Init has mounted /private/var in RAM before the allowlisted clients start. */
		const char *directories[] = { "/private/var/tmp", "/private/var/tmp/.cache",
		    "/private/var/tmp/metal2vulkan", "/private/var/tmp/.cache/metal2vulkan" };
		for (unsigned i = 0; i < sizeof(directories)/sizeof(directories[0]); i++) {
			if (mkdir(directories[i], 0755) != 0 && errno != EEXIST) {
				fprintf(stderr, "metal_auto: air84 cache directory %s errno %d\n", directories[i], errno);
			}
		}
		if (getenv("XDG_CACHE_HOME") == NULL) setenv("XDG_CACHE_HOME", "/private/var/tmp", 0);
		if (hakios_metal_debug_enabled()) fprintf(stderr, "metal_auto: air84 loaded in %s[%d], inherited source APIs adapted; native-device requests only\n",
		    getprogname(), getpid());
	}
}
