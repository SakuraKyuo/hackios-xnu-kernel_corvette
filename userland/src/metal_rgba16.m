/* Separate real float-target validation from factory and compiler startup. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t current_stage;
static int probe_fd=-1;
static void record(const char *format,...) __attribute__((format(printf,1,2)));
static void record(const char *format,...)
{
    char line[1024];va_list arguments;va_start(arguments,format);
    int n=vsnprintf(line,sizeof(line),format,arguments);va_end(arguments);
    size_t size=n<0?0:(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1;
    if(size){write(2,line,size);if(probe_fd>=0)write(probe_fd,line,size);}
}
static void stage(unsigned number,const char *name)
{
    current_stage=(sig_atomic_t)number;record("rgba16 stage %u: %s\n",number,name);
}
static void expired(int signal)
{
    (void)signal;
    char line[]="rgba16 FAIL deadline at stage 00\n";
    unsigned number=(unsigned)current_stage;
    line[30]=(char)('0'+(number/10)%10);line[31]=(char)('0'+number%10);
    write(2,line,sizeof(line)-1);if(probe_fd>=0)write(probe_fd,line,sizeof(line)-1);
    _exit(124);
}
static uint64_t nanoseconds(void)
{
    struct timespec value;clock_gettime(CLOCK_MONOTONIC,&value);
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
static BOOL complete(id<MTLCommandBuffer> command)
{
    [command commit];uint64_t start=nanoseconds();
    while(command.status!=MTLCommandBufferStatusCompleted && command.status!=MTLCommandBufferStatusError &&
        nanoseconds()-start<5000000000ull)usleep(1000);
    if(command.status!=MTLCommandBufferStatusCompleted || command.error) {
        record("rgba16 FAIL completion status=%lu error=%s\n",(unsigned long)command.status,command.error.description.UTF8String?:"none");return NO;
    }
    return YES;
}
static BOOL render_case(id<MTLCommandQueue> queue,id<MTLTexture> texture,id<MTLBuffer> readback,id<MTLRenderPipelineState> pipeline)
{
    memset(readback.contents,0xa5,readback.length);
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=pipeline?MTLClearColorMake(0,0,0,0):MTLClearColorMake(.25,.5,.75,1);
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
    if(!command || !encoder){record("rgba16 FAIL command/encoder\n");return NO;}
    if(pipeline){[encoder setRenderPipelineState:pipeline];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    if(!blit){record("rgba16 FAIL readback encoder\n");return NO;}
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(16,16,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:4096];
    [blit endEncoding];if(!complete(command))return NO;
    const uint16_t expected[4]={0x3400,0x3800,0x3a00,0x3c00};unsigned mismatch=0;
    const unsigned char *bytes=readback.contents;
    for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16*4;x++) {
        uint16_t value;memcpy(&value,bytes+y*256+x*2,2);if(value!=expected[x%4])mismatch++;
    }
    uint16_t first[4];memcpy(first,bytes,sizeof(first));
    record("rgba16 %s %s format=%lu mismatches=%u/1024 first=%04x,%04x,%04x,%04x\n",
        mismatch?"FAIL":"PASS",pipeline?"shader-draw":"load-clear",(unsigned long)texture.pixelFormat,mismatch,first[0],first[1],first[2],first[3]);
    return mismatch==0;
}
int main(void)
{
    probe_fd=open("/private/var/tmp/hakios-air90-metal.log",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    signal(SIGALRM,expired);alarm(45);stage(1,"main entered; create system device");
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device){record("rgba16 FAIL no device\n");return 1;}
        stage(2,"device returned; create queue/float target/readback");
        record("rgba16 device=%s name=%s\n",object_getClassName(device),device.name.UTF8String);
        MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:16 height:16 mipmapped:NO];
        descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[device newTextureWithDescriptor:descriptor];
        id<MTLBuffer> readback=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue=[device newCommandQueue];
        if(!texture || !readback.contents || !queue){record("rgba16 FAIL resources\n");return 2;}
        stage(3,"real clear/readback; no library or source compiler");
        if(!render_case(queue,texture,readback,nil))return 3;
        stage(4,"clear passed; compile real vertex/fragment source");
        NSError *error=nil;
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 v(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
            "fragment half4 f() { return half4(0.25h,0.5h,0.75h,1.0h); }\n";
        id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
        if(!library){record("rgba16 FAIL library %s\n",error.description.UTF8String?:"none");return 4;}
        stage(5,"source returned; create real format115 pipeline");
        MTLRenderPipelineDescriptor *pipeline=[MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction=[library newFunctionWithName:@"v"];pipeline.fragmentFunction=[library newFunctionWithName:@"f"];
        pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float;
        id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:pipeline error:&error];
        if(!state){record("rgba16 FAIL pipeline %s\n",error.description.UTF8String?:"none");return 5;}
        stage(6,"real shader draw/readback");if(!render_case(queue,texture,readback,state))return 6;
        stage(7,"both numerical float-target cases passed");alarm(0);
    }
    return 0;
}
