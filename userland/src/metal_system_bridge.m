/* Observe actual system Metal failures without substituting shaders or capabilities. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdarg.h>
#include <dlfcn.h>
#include "metal_debug.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#import <dispatch/dispatch.h>

void hakios_metal_renderer_log(const char *format, ...)
{
    bool failure=strstr(format,"FAIL") || strstr(format,"exception") || strstr(format,"STOP") || strstr(format,"unavailable");
    if (!failure && !hakios_metal_debug_enabled()) return;
    static unsigned messages;
    if (!failure && __sync_fetch_and_add(&messages,1)>=32) return;
    char buffer[768];
    va_list args; va_start(args, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    size_t length = n < 0 ? 0 : (size_t)n < sizeof(buffer) ? (size_t)n : sizeof(buffer)-1;
    if (!length) return;
    if(failure) {
        /* Keep distinct errors available even if a missing library is retried. */
        static uint64_t recorded[32];
        uint64_t hash=1469598103934665603ULL;
        for(size_t i=0;i<length;i++)hash=(hash^(unsigned char)buffer[i])*1099511628211ULL;
        if(!hash)hash=1;
        bool inserted=false;
        for(unsigned i=0;i<32;i++) {
            uint64_t value=__atomic_load_n(&recorded[i],__ATOMIC_RELAXED);
            if(value==hash)return;
            if(value==0 && __sync_bool_compare_and_swap(&recorded[i],0,hash)){inserted=true;break;}
        }
        if(!inserted)return;
    }
    char prefix[96];
    int prefix_length=snprintf(prefix,sizeof(prefix),"metal [%s:%d]: ",getprogname(),getpid());
    if(prefix_length>0)write(STDERR_FILENO,prefix,(size_t)prefix_length);
    write(STDERR_FILENO, buffer, length);
    int fd = open("/private/var/tmp/hakios-air90-metal.log", O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0644);
    if (fd >= 0) { if(prefix_length>0)write(fd,prefix,(size_t)prefix_length); write(fd, buffer, length); close(fd); }
}

typedef id (*ObjectCreate)(id, SEL, id, NSError **) __attribute__((ns_returns_retained));
static ObjectCreate original_url, original_file, original_data, original_pipeline;
static unsigned load_count, pipeline_count, failures;
static void associate_library_metadata(id library, NSData *bytes);

static id watched_library(id self, SEL cmd, id argument, NSError **error) __attribute__((ns_returns_retained));
static id watched_library(id self, SEL cmd, id argument, NSError **error)
{
    ObjectCreate original = cmd == @selector(newLibraryWithURL:error:) ? original_url :
        cmd == @selector(newLibraryWithFile:error:) ? original_file : original_data;
    unsigned sequence = __sync_add_and_fetch(&load_count, 1);
    if (sequence <= 8)
        hakios_metal_renderer_log("air90 Metal library begin: %s request=%u\n", sel_getName(cmd), sequence);
    NSError *failure = nil;
    id value = original(self, cmd, argument, &failure);
    if (value != nil) {
        NSData *bytes=nil;
        if (cmd == @selector(newLibraryWithData:error:)) {
            const void *base=NULL; size_t length=0;
            dispatch_data_t mapped=dispatch_data_create_map((dispatch_data_t)argument,&base,&length);
            if (mapped && length<=128*1024*1024) bytes=[NSData dataWithBytes:base length:length];
        } else if (cmd == @selector(newLibraryWithFile:error:)) {
            bytes=[NSData dataWithContentsOfFile:argument];
        } else {
            bytes=[NSData dataWithContentsOfURL:argument];
        }
        associate_library_metadata(value,bytes);
    }
    if (value == nil && __sync_fetch_and_add(&failures,1) < 16)
        hakios_metal_renderer_log("Metal library FAIL: %s resource=%s error=%s\n", sel_getName(cmd),
            cmd==@selector(newLibraryWithData:error:)?"<data>":[[argument description] UTF8String]?:"<nil>",
            [[failure description] UTF8String] ?: "nil result without NSError");
    else if (sequence <= 8)
        hakios_metal_renderer_log("air90 Metal library result: %s class=%s\n", sel_getName(cmd),
            value ? class_getName(object_getClass(value)) : "nil");
    if (error) *error = failure;
    return value;
}

static id watched_pipeline(id self, SEL cmd, MTLRenderPipelineDescriptor *descriptor, NSError **error)
    __attribute__((ns_returns_retained));
static id watched_pipeline(id self, SEL cmd, MTLRenderPipelineDescriptor *descriptor, NSError **error)
{
    unsigned sequence=__sync_add_and_fetch(&pipeline_count,1);
    if (sequence <= 8)
        hakios_metal_renderer_log("air90 Metal pipeline begin: request=%u pixelFormat=%lu samples=%lu\n",
            sequence, (unsigned long)descriptor.colorAttachments[0].pixelFormat, (unsigned long)descriptor.sampleCount);
    /* Capture functions before the original serializer can touch the descriptor. */
    id vertex=descriptor.vertexFunction,fragment=descriptor.fragmentFunction;
    void *caller=__builtin_return_address(0);
    NSError *failure=nil;
    id value=original_pipeline(self, cmd, descriptor, &failure);
    if(!strcmp(getprogname(),"chronod") && descriptor.colorAttachments[0].pixelFormat==MTLPixelFormatRGBA16Float) {
        static unsigned targets;
        if(__sync_fetch_and_add(&targets,1)==0)
            fprintf(stderr,"metal [chronod:%d]: RenderBox real pipeline format=115 created=%d\n",getpid(),value!=nil);
    }
    if(value==nil) {
        Dl_info info={0};dladdr(caller,&info);
        hakios_metal_renderer_log("Metal pipeline FAIL inputs: format=%lu vertex=%s:%s fragment=%s:%s caller=%s+%#lx label=%s\n",
            (unsigned long)descriptor.colorAttachments[0].pixelFormat,
            vertex?class_getName(object_getClass(vertex)):"nil",vertex?[(id<MTLFunction>)vertex name].UTF8String?:"<unnamed>":"nil",
            fragment?class_getName(object_getClass(fragment)):"nil",fragment?[(id<MTLFunction>)fragment name].UTF8String?:"<unnamed>":"nil",
            info.dli_fname?:"unknown",(unsigned long)((uintptr_t)caller-(uintptr_t)info.dli_fbase),descriptor.label.UTF8String?:"<none>");
    }
    if (value == nil && __sync_fetch_and_add(&failures,1) < 16)
        hakios_metal_renderer_log("air90 Metal pipeline FAIL: format=%lu error=%s\n",
            (unsigned long)descriptor.colorAttachments[0].pixelFormat,
            [[failure description] UTF8String] ?: "nil result without NSError");
    else if (sequence <= 8)
        hakios_metal_renderer_log("air90 Metal pipeline result: class=%s\n",
            value ? class_getName(object_getClass(value)) : "nil");
    if (error) *error=failure;
    return value;
}

static void watch_method(Class cls, SEL selector, IMP replacement, ObjectCreate *original)
{
    Method method=class_getInstanceMethod(cls,selector);
    if (!method || method_getNumberOfArguments(method)!=4) return;
    char type[8]={0}; method_getReturnType(method,type,sizeof(type));
    if (type[0]!='@') return;
    *original=(ObjectCreate)method_getImplementation(method);
    class_replaceMethod(cls,selector,replacement,method_getTypeEncoding(method));
    hakios_metal_renderer_log("air90 Metal observe: %s %s\n",class_getName(cls),sel_getName(selector));
}

/* Use the real library's TYPE=5 records; no fabricated capability/name answers. */
#include "metal_library_metadata.h"
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
static char external_names_key;
static unsigned metadata_count;
static int add_external_name(void *context,const char *name,size_t length)
{
    NSMutableArray *names=(__bridge NSMutableArray *)context;
    NSString *value=[[NSString alloc] initWithBytes:name length:length encoding:NSUTF8StringEncoding];
    if (!value) return 0;
    [names addObject:value];return 1;
}
static void associate_library_metadata(id library, NSData *bytes)
{
    if (!library || !bytes) return;
    Class native=objc_getClass("NBLibrary");
    if (!native || ![library isKindOfClass:native]) return;
    NSMutableArray *names=[NSMutableArray array];
    if (!hakios_mtlb_extern_names(bytes.bytes,bytes.length,add_external_name,(__bridge void *)names)) {
        hakios_metal_renderer_log("air90 library metadata FAIL bytes=%lu\n",(unsigned long)bytes.length);
        return;
    }
    objc_setAssociatedObject(library,&external_names_key,[names copy],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (__sync_fetch_and_add(&metadata_count,1)<8)
        hakios_metal_renderer_log("air90 library metadata: bytes=%lu external-functions=%lu\n",
            (unsigned long)bytes.length,(unsigned long)names.count);
}
static id external_function_names(id self,SEL cmd)
{
    (void)cmd;
    id names=objc_getAssociatedObject(self,&external_names_key);
    if (!names) {
        hakios_metal_renderer_log("air90 externFunctionNames: no observed MTLB metadata\n");
        [NSException raise:NSInternalInconsistencyException format:@"Native library has no captured MTLB metadata"];
    }
    return names;
}
static ObjectCreate original_bundle;
static id watched_bundle_library(id self,SEL cmd,NSBundle *bundle,NSError **error) __attribute__((ns_returns_retained));
static id watched_bundle_library(id self,SEL cmd,NSBundle *bundle,NSError **error)
{
    id value=original_bundle(self,cmd,bundle,error);
    if(value) associate_library_metadata(value,[NSData dataWithContentsOfURL:[bundle URLForResource:@"default" withExtension:@"metallib"]]);
    return value;
}
static void install_native_compat(id device);
/* RenderBox's verified own parser accepts extended -> color mode4. In its
 * surface path that creates a real RGBA16Float target before drawing, avoiding
 * unsupported private RGB8_444_2P. Never relabel existing texture storage. */
static void configure_renderbox_native_target(id device)
{
    if(strcmp(getprogname(),"chronod"))return;
    Class native=objc_getClass("NativeBridgeMetalDevice");
    if(!native || ![device isKindOfClass:native])return;
    static BOOL configured;
    if(configured)return;configured=YES;
    const char *mode=getenv("RB_COLOR_MODE");
    if(!mode && setenv("RB_COLOR_MODE","extended",0)==0)
        fprintf(stderr,"metal [chronod:%d]: RenderBox selects extended color mode before target creation (RGBA16Float path)\n",getpid());
    else if(mode)
        fprintf(stderr,"metal [chronod:%d]: RenderBox keeps explicit RB_COLOR_MODE=%s\n",getpid(),mode);
}

/* Objective-C init transfers ownership of its receiver and returns +1. */
typedef id (*NativeInit)(id __attribute__((ns_consumed)),SEL,uint32_t) __attribute__((ns_returns_retained));
static NativeInit original_native_init;
/* Record the actual CPU fault without logging through Foundation in a handler. */
#include <signal.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#include <stdint.h>
static int fault_fd=-1;
static uintptr_t fault_cache_slide;
static uintptr_t fault_plugin_base;
static uintptr_t fault_bridge_base, fault_main_base;
static pthread_mutex_t fault_install_lock=PTHREAD_MUTEX_INITIALIZER;
static struct sigaction fault_previous[3];
static const int fault_signals[3]={SIGBUS,SIGSEGV,SIGABRT};
static unsigned char fault_stack[65536] __attribute__((aligned(16)));
static size_t fault_append(char *out,size_t at,size_t limit,const char *text)
{
    while(*text && at<limit)out[at++]=*text++;
    return at;
}
static size_t fault_hex(char *out,size_t at,size_t limit,uintptr_t value)
{
    const char *hex="0123456789abcdef";
    at=fault_append(out,at,limit,"0x");
    for(int shift=(int)(sizeof(value)*8)-4;shift>=0;shift-=4)
        if(at<limit)out[at++]=hex[(value>>shift)&15];
    return at;
}
static void record_cpu_fault(int sig,siginfo_t *info,void *context)
{
    char line[3072];size_t at=0,limit=sizeof(line)-1;
    ucontext_t *uc=context;
    uintptr_t pc=0,lr=0,sp=0,fp=0,esr=0,far=0;
    if(uc && uc->uc_mcontext) {
        pc=uc->uc_mcontext->__ss.__pc;lr=uc->uc_mcontext->__ss.__lr;
        sp=uc->uc_mcontext->__ss.__sp;fp=uc->uc_mcontext->__ss.__fp;
        esr=uc->uc_mcontext->__es.__esr;far=uc->uc_mcontext->__es.__far;
    }
    at=fault_append(line,at,limit,"air94 CPU FAULT backboardd signal=");at=fault_hex(line,at,limit,(uintptr_t)sig);
    at=fault_append(line,at,limit," code=");at=fault_hex(line,at,limit,info?(uintptr_t)info->si_code:0);
    at=fault_append(line,at,limit," address=");at=fault_hex(line,at,limit,info?(uintptr_t)info->si_addr:0);
    at=fault_append(line,at,limit," pc=");at=fault_hex(line,at,limit,pc);
    at=fault_append(line,at,limit," lr=");at=fault_hex(line,at,limit,lr);
    at=fault_append(line,at,limit," sp=");at=fault_hex(line,at,limit,sp);
    at=fault_append(line,at,limit," fp=");at=fault_hex(line,at,limit,fp);
    at=fault_append(line,at,limit," cache-slide=");at=fault_hex(line,at,limit,fault_cache_slide);
    at=fault_append(line,at,limit," cache-pc=");at=fault_hex(line,at,limit,pc-fault_cache_slide);
    at=fault_append(line,at,limit," plugin-base=");at=fault_hex(line,at,limit,fault_plugin_base);
    at=fault_append(line,at,limit," plugin-pc=");at=fault_hex(line,at,limit,pc-fault_plugin_base);
    at=fault_append(line,at,limit," bridge-base=");at=fault_hex(line,at,limit,fault_bridge_base);
    at=fault_append(line,at,limit," main-base=");at=fault_hex(line,at,limit,fault_main_base);
    at=fault_append(line,at,limit," pid=");at=fault_hex(line,at,limit,(uintptr_t)getpid());
    at=fault_append(line,at,limit," esr=");at=fault_hex(line,at,limit,esr);
    at=fault_append(line,at,limit," far=");at=fault_hex(line,at,limit,far);
    at=fault_append(line,at,limit,"\n");
    if(uc && uc->uc_mcontext) {
        for(unsigned i=0;i<29;i++) {
            at=fault_append(line,at,limit," x");at=fault_hex(line,at,limit,i);
            at=fault_append(line,at,limit,"=");at=fault_hex(line,at,limit,uc->uc_mcontext->__ss.__x[i]);
        }
        at=fault_append(line,at,limit,"\n");
    }
    if(at) {
        write(STDERR_FILENO,line,at);
        if(fault_fd>=0)write(fault_fd,line,at);
    }
    /* Preserve existing crash handling; never continue past an invalid access. */
    for(unsigned i=0;i<3;i++)if(fault_signals[i]==sig) {
        struct sigaction previous=fault_previous[i];
        sigaction(sig,&previous,NULL);
        if(previous.sa_handler!=SIG_DFL && previous.sa_handler!=SIG_IGN) {
            if(previous.sa_flags&SA_SIGINFO)previous.sa_sigaction(sig,info,context);
            else previous.sa_handler(sig);
            return;
        }
        struct sigaction action={0};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
        sigaction(sig,&action,NULL);
        kill(getpid(),sig);
        return;
    }
}
static void install_cpu_fault_record(void)
{
    if(strcmp(getprogname(),"backboardd"))return;
    pthread_mutex_lock(&fault_install_lock);
    if(fault_fd<0)fault_fd=open("/private/var/tmp/hakios-air94-fault.log",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    fault_cache_slide=(uintptr_t)&MTLCreateSystemDefaultDevice-0x181a34b3cULL;
    if(original_native_init) {
        Dl_info info={0};
        if(dladdr((const void *)original_native_init,&info))fault_plugin_base=(uintptr_t)info.dli_fbase;
    }
    Dl_info bridge={0};
    if(dladdr((const void *)record_cpu_fault,&bridge))fault_bridge_base=(uintptr_t)bridge.dli_fbase;
    fault_main_base=(uintptr_t)_dyld_get_image_header(0);
    /* A static alternate stack belongs to one thread only. Respect an existing stack. */
    if(pthread_main_np()) {
        stack_t current={0};
        if(sigaltstack(NULL,&current)==0 && (current.ss_flags&SS_DISABLE)) {
            stack_t stack={.ss_sp=fault_stack,.ss_size=sizeof(fault_stack),.ss_flags=0};
            sigaltstack(&stack,NULL);
        }
    }
    unsigned installed=0;
    for(unsigned i=0;i<3;i++) {
        struct sigaction current={0};
        if(sigaction(fault_signals[i],NULL,&current)!=0 ||
            ((current.sa_flags&SA_SIGINFO) && current.sa_sigaction==record_cpu_fault))continue;
        struct sigaction action={0};sigemptyset(&action.sa_mask);
        action.sa_sigaction=record_cpu_fault;action.sa_flags=SA_SIGINFO|SA_ONSTACK;
        fault_previous[i]=current;
        if(sigaction(fault_signals[i],&action,NULL)==0)installed++;
    }
    if(installed)hakios_metal_renderer_log("air94 CPU fault record installed: signals=%u fd=%d main=0x%lx bridge=0x%lx\n",
        installed,fault_fd,(unsigned long)fault_main_base,(unsigned long)fault_bridge_base);
    pthread_mutex_unlock(&fault_install_lock);
}

static id watched_native_init(id self __attribute__((ns_consumed)),SEL cmd,uint32_t port) __attribute__((ns_returns_retained));
static id watched_native_init(id self __attribute__((ns_consumed)),SEL cmd,uint32_t port)
{
    id device=original_native_init(self,cmd,port);
    if (device) {
        configure_renderbox_native_target(device);
        install_native_compat(device);
    }
    install_cpu_fault_record();
    return device;
}
static void watch_factory(Class cls)
{
    if (!cls || strcmp(class_getName(cls),"NativeAdrenoMetalDevice")) return;
    @synchronized((id)cls) {
        if (original_native_init) return;
        SEL sel=sel_registerName("initWithAcceleratorPort:");
        Method m=class_getInstanceMethod(cls,sel);
        if (!m || method_getNumberOfArguments(m)!=3) return;
        original_native_init=(NativeInit)method_getImplementation(m);
        class_replaceMethod(cls,sel,(IMP)watched_native_init,method_getTypeEncoding(m));
    }
}
typedef Class (*BundleClass)(id,SEL,NSString *);
static BundleClass original_class_named;
static Class watched_class_named(id self,SEL cmd,NSString *name)
{
    Class cls=original_class_named(self,cmd,name);
    watch_factory(cls);return cls;
}
typedef Class (*BundlePrincipal)(id,SEL);
static BundlePrincipal original_principal;
static Class watched_principal(id self,SEL cmd)
{
    Class cls=original_principal(self,cmd);
    watch_factory(cls);return cls;
}
/* Native C ABI verified against nmb_function_new_specialized and its caller.
 * GPU constants carry logical component bytes, excluding host vector padding. */
typedef struct {
    uint64_t data_type;
    uint64_t index;
    const char *name;
    const void *data;
    uint64_t bytes;
} HakiosNativeConstant;
_Static_assert(sizeof(HakiosNativeConstant)==40,"Native constant wire record");
typedef void *(*NativeSpecialize)(void *,const char *,const HakiosNativeConstant *,uint32_t);
typedef void (*NativeFunctionRelease)(void *);
typedef const char *(*NativeLastError)(void);
typedef id (*CreateSpecialized)(id,SEL,id,id,NSError **) __attribute__((ns_returns_retained));
static CreateSpecialized original_specialize;
static unsigned uchar3_specializations;

static NSUInteger native_constant_bytes(NSUInteger type)
{
    /* Match the existing qualified serializer, changing only uchar3's data length. */
    switch(type) {
        case 3:case 17:case 29:case 33:case 38:case 42:case 48:case 52:case 56:return 4;
        case 4:case 19:case 30:case 34:case 40:case 44:case 81:case 85:return 8;
        case 6:case 32:case 36:case 82:case 86:return 16;
        case 16:case 37:case 41:case 46:case 50:case 54:return 2;
        case 45:case 49:case 53:return 1;
        case 84:case 88:return 32;
        case 51:return 3;
        default:return 0;
    }
}
static id specialize_uchar3(id self,SEL cmd,NSString *name,id constants,NSError **error)
    __attribute__((ns_returns_retained));
static id specialize_uchar3(id self,SEL cmd,NSString *name,id constants,NSError **error)
{
    if(!constants)return original_specialize(self,cmd,name,constants,error);
    Class lib=objc_getClass("NBLibrary");
    if(!lib || ![self isKindOfClass:lib])return original_specialize(self,cmd,name,constants,error);
    SEL indexed_sel=sel_registerName("newIndexedConstantArray"),named_sel=sel_registerName("newNamedConstantArray");
    if(![constants respondsToSelector:indexed_sel] || ![constants respondsToSelector:named_sel])
        return original_specialize(self,cmd,name,constants,error);
    id (*copy_array)(id,SEL) __attribute__((ns_returns_retained))=(void *)objc_msgSend;
    NSArray *indexed=copy_array(constants,indexed_sel),*named=copy_array(constants,named_sel);
    NSUInteger (*integer)(id,SEL)=(void *)objc_msgSend;
    void *(*pointer)(id,SEL)=(void *)objc_msgSend;
    id (*object)(id,SEL)=(void *)objc_msgSend;
    BOOL has_uchar3=NO;
    for(NSArray *array in @[indexed?:@[],named?:@[]])
        for(id constant in array)if(integer(constant,sel_registerName("dataType"))==51)has_uchar3=YES;
    if(!has_uchar3)return original_specialize(self,cmd,name,constants,error);
    NSUInteger count=indexed.count+named.count;
    if(count==0 || count>4096)return original_specialize(self,cmd,name,constants,error);
    HakiosNativeConstant *records=calloc(count,sizeof(*records));
    if(!records)return nil;
    NSMutableArray *keep_names=[NSMutableArray array];
    NSUInteger at=0;BOOL valid=YES;
    for(unsigned group=0;group<2;group++) {
        NSArray *array=group?named:indexed;
        for(id constant in array) {
            NSUInteger type=integer(constant,sel_registerName("dataType"));
            NSUInteger bytes=native_constant_bytes(type);
            void *data=pointer(constant,sel_registerName("data"));
            if(!bytes || !data){valid=NO;break;}
            records[at].data_type=type;records[at].data=data;records[at].bytes=bytes;
            if(group) {
                NSString *key=object(constant,@selector(name));
                if(![key isKindOfClass:[NSString class]]){valid=NO;break;}
                [keep_names addObject:key];records[at].name=key.UTF8String;
            } else records[at].index=integer(constant,sel_registerName("index"));
            at++;
        }
        if(!valid)break;
    }
    if(!valid || at!=count){free(records);return original_specialize(self,cmd,name,constants,error);}
    static void *module;
    if(!module)module=dlopen("/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal",RTLD_LAZY|RTLD_NOLOAD);
    NativeSpecialize make=module?(NativeSpecialize)dlsym(module,"nmb_function_new_specialized"):NULL;
    NativeFunctionRelease drop=module?(NativeFunctionRelease)dlsym(module,"nmb_function_release"):NULL;
    NativeLastError last=module?(NativeLastError)dlsym(module,"nmb_last_error"):NULL;
    if(!make || !drop){free(records);return original_specialize(self,cmd,name,constants,error);}
    /* Let the verified original construct the NBFunction and its labels/cache key;
     * replace its not-yet-submitted handle with the same backend's packed values. */
    NSError *failure=nil;
    id function=original_specialize(self,cmd,name,constants,&failure);
    Class native_function=objc_getClass("NBFunction");
    if(!function || !native_function || ![function isKindOfClass:native_function]) {
        free(records);if(error)*error=failure;return function;
    }
    void *library_handle=pointer(self,sel_registerName("handle"));
    void *new_handle=make(library_handle,name.UTF8String,records,(uint32_t)count);
    if(__sync_fetch_and_add(&uchar3_specializations,1)<16)
        hakios_metal_renderer_log("air93 uchar3 packed: name=%s records=%lu logical-bytes=3 handle=%s error=%s\n",
            name.UTF8String,(unsigned long)count,new_handle?"created":"nil",new_handle?"none":last?last():"unavailable");
    free(records);
    if(!new_handle) {
        if(error)*error=[NSError errorWithDomain:@"org.hakios.uchar3" code:1
            userInfo:@{NSLocalizedDescriptionKey:last && last()?[NSString stringWithUTF8String:last()]:@"Native specialization failed"}];
        return nil;
    }
    void *old_handle=pointer(function,sel_registerName("handle"));
    void (*set_handle)(id,SEL,void *)=(void *)objc_msgSend;
    set_handle(function,sel_registerName("setHandle:"),new_handle);
    if(old_handle)drop(old_handle);
    if(error)*error=nil;
    return function;
}
static void install_uchar3_specialization(void)
{
    Class library=objc_getClass("NBLibrary");
    SEL selector=@selector(newFunctionWithName:constantValues:error:);
    Method method=library?class_getInstanceMethod(library,selector):NULL;
    if(!method || method_getNumberOfArguments(method)!=5 || original_specialize)return;
    original_specialize=(CreateSpecialized)method_getImplementation(method);
    class_replaceMethod(library,selector,(IMP)specialize_uchar3,method_getTypeEncoding(method));
}

static void install_native_compat(id device)
{
    static BOOL installed;
    Class native=objc_getClass("NativeBridgeMetalDevice");
    if (!native || ![device isKindOfClass:native]) return;
    @synchronized((id)native) {
        if(installed)return;installed=YES;
        Class library=objc_getClass("NBLibrary");
        Method abi=class_getInstanceMethod(objc_getClass("_MTLLibrary"),sel_registerName("externFunctionNames"));
        if(library && abi && !class_getInstanceMethod(library,sel_registerName("externFunctionNames")))
            class_addMethod(library,sel_registerName("externFunctionNames"),(IMP)external_function_names,method_getTypeEncoding(abi));
        install_uchar3_specialization();
        watch_method(native,@selector(newLibraryWithURL:error:),(IMP)watched_library,&original_url);
        watch_method(native,@selector(newLibraryWithFile:error:),(IMP)watched_library,&original_file);
        watch_method(native,@selector(newLibraryWithData:error:),(IMP)watched_library,&original_data);
        watch_method(native,@selector(newDefaultLibraryWithBundle:error:),(IMP)watched_bundle_library,&original_bundle);
        watch_method(native,@selector(newRenderPipelineStateWithDescriptor:error:),(IMP)watched_pipeline,&original_pipeline);
        hakios_metal_renderer_log("air90 Native compatibility installed in %s[%d]\n",getprogname(),getpid());
    }
}
/* Establish RenderBox's one-time environment before its color-mode cache is
 * initialized. Inspect the published accelerator, never create a device here. */
static void configure_renderbox_before_startup(void)
{
    if(strcmp(getprogname(),"chronod") || getenv("RB_COLOR_MODE"))return;
    io_iterator_t iterator=0;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOAcceleratorES"),&iterator)!=KERN_SUCCESS)return;
    io_service_t service;BOOL native=NO;
    while((service=IOIteratorNext(iterator))) {
        CFTypeRef plugin=IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginName"),kCFAllocatorDefault,0);
        if(plugin && CFGetTypeID(plugin)==CFStringGetTypeID() && CFEqual(plugin,CFSTR("NativeAdrenoMetal")))native=YES;
        if(plugin)CFRelease(plugin);IOObjectRelease(service);
        if(native)break;
    }
    IOObjectRelease(iterator);
    if(native && setenv("RB_COLOR_MODE","extended",0)==0)
        fprintf(stderr,"metal [chronod:%d]: Native accelerator selects RenderBox extended mode before startup\n",getpid());
}
/* Cocoa returns the device class before Metal sends its factory initializer. */
__attribute__((constructor)) static void install_bundle_observer(void)
{
    configure_renderbox_before_startup();
    Class bundle=objc_getClass("NSBundle");
    Method m=class_getInstanceMethod(bundle,@selector(classNamed:));
    Method p=class_getInstanceMethod(bundle,@selector(principalClass));
    if(m && p) {
        original_class_named=(BundleClass)method_getImplementation(m);
        original_principal=(BundlePrincipal)method_getImplementation(p);
        class_replaceMethod(bundle,@selector(classNamed:),(IMP)watched_class_named,method_getTypeEncoding(m));
        class_replaceMethod(bundle,@selector(principalClass),(IMP)watched_principal,method_getTypeEncoding(p));
    }
    watch_factory(objc_getClass("NativeAdrenoMetalDevice"));
    install_cpu_fault_record();
}
@interface NSObject (HakiosNativeFactory)
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
@end

static id recover_default_device_on_worker(void)
{
    /* Executed after the normal MTL API has returned nil, outside its dispatch_once.
     * Refresh the already-published accelerator through the same declared factory. */
    static id recovered;
    if(recovered)return recovered;
    void (*add_device)(id)=dlsym(RTLD_DEFAULT,"MTLAddDevice");
    if(!add_device) {hakios_metal_renderer_log("air90 recovery: MTLAddDevice unavailable\n");return nil;}
    io_iterator_t iterator=0;
    kern_return_t kr=IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOAcceleratorES"),&iterator);
    hakios_metal_renderer_log("air90 recovery: IOAcceleratorES match=%#x\n",kr);
    if(kr!=KERN_SUCCESS)return nil;
    io_service_t service;
    while((service=IOIteratorNext(iterator))) {
        CFTypeRef plugin=IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginName"),kCFAllocatorDefault,0);
        CFTypeRef classname=IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginClassName"),kCFAllocatorDefault,0);
        if(plugin && classname && CFGetTypeID(plugin)==CFStringGetTypeID() && CFGetTypeID(classname)==CFStringGetTypeID() &&
           CFEqual(plugin,CFSTR("NativeAdrenoMetal")) && CFEqual(classname,CFSTR("NativeAdrenoMetalDevice"))) {
            NSString *path=[NSString stringWithFormat:@"/System/Library/Extensions/%@.bundle",(__bridge NSString *)plugin];
            NSBundle *bundle=[NSBundle bundleWithPath:path];NSError *failure=nil;
            BOOL loaded=[bundle loadAndReturnError:&failure];
            hakios_metal_renderer_log("air90 recovery: bundle loaded=%d error=%s\n",loaded,[[failure description] UTF8String]?:"none");
            Class cls=loaded?[bundle classNamed:(__bridge NSString *)classname]:Nil;
            Class base=objc_getClass("_MTLDevice");
            if(cls && base && [cls isSubclassOfClass:base]) {
                watch_factory(cls);
                hakios_metal_renderer_log("air90 recovery: factory begin worker-stack=%zu bytes\n",
                    pthread_get_stacksize_np(pthread_self()));
                /* A normal init expression lets ARC consume the allocated receiver.
                 * Casting objc_msgSend to a plain C function loses that convention. */
                id device=[[cls alloc] initWithAcceleratorPort:service];
                hakios_metal_renderer_log("air90 recovery: factory result=%s\n",device?class_getName(object_getClass(device)):"nil");
                if(device) {
                    install_native_compat(device);
                    add_device(device);
                    id selected=MTLCreateSystemDefaultDevice();
                    hakios_metal_renderer_log("air90 recovery: registered default=%s same-device=%d\n",
                        selected?class_getName(object_getClass(selected)):"nil",selected==device);
                    recovered=selected;
                }
            }
        }
        if(plugin)CFRelease(plugin);if(classname)CFRelease(classname);IOObjectRelease(service);
        if(recovered)break;
    }
    IOObjectRelease(iterator);return recovered;
}

/* The plugin factory reserves 0x114e0 bytes before its nested calls.
 * CoreAnimation can invoke renderer() on a much smaller thread stack. Keep
 * initialization on a separate explicit stack; the compositor keeps its software
 * fallback while the real device is being made and registered. */
static atomic_int recovery_state;
static void *recovery_main(void *unused)
{
    (void)unused;
    @autoreleasepool {
        @try {
            hakios_metal_renderer_log("air90 recovery worker started stack=%zu bytes\n",
                pthread_get_stacksize_np(pthread_self()));
            id device=recover_default_device_on_worker();
            hakios_metal_renderer_log("air90 recovery worker finished device=%s\n",
                device?class_getName(object_getClass(device)):"nil");
            atomic_store_explicit(&recovery_state,device?2:3,memory_order_release);
        } @catch (NSException *exception) {
            hakios_metal_renderer_log("air90 recovery worker exception: %s %s\n",
                [[exception name] UTF8String],[[exception reason] UTF8String]);
            atomic_store_explicit(&recovery_state,3,memory_order_release);
        }
    }
    return NULL;
}
static id recover_default_device(void)
{
    int state=atomic_load_explicit(&recovery_state,memory_order_acquire);
    if(state==2)return MTLCreateSystemDefaultDevice();
    int expected=0;
    if(state==0 && atomic_compare_exchange_strong(&recovery_state,&expected,1)) {
        pthread_attr_t attr;
        int error=pthread_attr_init(&attr);
        BOOL initialized=error==0;
        if(!error)error=pthread_attr_setstacksize(&attr,4*1024*1024);
        if(!error)error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        pthread_t worker;
        if(!error)error=pthread_create(&worker,&attr,recovery_main,NULL);
        if(initialized)pthread_attr_destroy(&attr);
        hakios_metal_renderer_log("air90 recovery scheduled error=%d caller-stack=%zu bytes\n",
            error,pthread_get_stacksize_np(pthread_self()));
        if(error)atomic_store_explicit(&recovery_state,3,memory_order_release);
    }
    return nil;
}

/* Called only after CoreAnimation's real renderer attempt, never from a constructor. */
void hakios_metal_renderer_diagnose(unsigned attempt)
{
    @autoreleasepool {
        @try {
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();
            hakios_metal_renderer_log("air90 Metal discovery: attempt=%u device=%s name=%s\n",attempt,
                device ? class_getName(object_getClass(device)) : "nil", device ? [[device name] UTF8String] : "nil");
            if (!device && !strcmp(getprogname(),"backboardd")) device=recover_default_device();
            if (!device) return;
            install_native_compat(device);
            id<MTLCommandQueue> queue=[device newCommandQueue];
            hakios_metal_renderer_log("air90 Metal queue: class=%s same-device=%d\n",
                queue ? class_getName(object_getClass(queue)) : "nil", queue && queue.device==device);
        } @catch (NSException *exception) {
            hakios_metal_renderer_log("air90 Metal discovery exception: %s %s\n",
                [[exception name] UTF8String],[[exception reason] UTF8String]);
        }
    }
}
