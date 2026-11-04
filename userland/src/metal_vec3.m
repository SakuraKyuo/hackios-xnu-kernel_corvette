/* Exercise uchar3 function specialization through the real Native pipeline. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
typedef unsigned char uchar3_storage __attribute__((ext_vector_type(3)));
_Static_assert(sizeof(uchar3_storage)==4, "Metal uchar3 storage is four bytes");
static int probe_print(const char *format, ...) __attribute__((format(printf,1,2)));
static int probe_print(const char *format, ...)
{
    char text[1536]; va_list args;va_start(args,format);
    int n=vsnprintf(text,sizeof(text),format,args);va_end(args);
    size_t length=n<0?0:(size_t)n<sizeof(text)?(size_t)n:sizeof(text)-1;
    write(STDOUT_FILENO,text,length);
    int fd=open("/private/var/tmp/hakios-air90-metal.log",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    if(fd>=0){write(fd,text,length);close(fd);}
    return n;
}
#define printf probe_print
static void timeout(int sig) { (void)sig;const char *s="air92 vec3 FAIL: deadline\n";write(2,s,strlen(s));_exit(124); }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);signal(SIGALRM,timeout);alarm(45);
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        printf("air92 vec3 start: device=%s\n",device?[[device name] UTF8String]:"nil");
        if(!device)return 1;
        NSError *error=nil;
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
            "constant uchar3 values [[function_constant(0)]];\n"
            "kernel void check_vec3(device uint *out [[buffer(0)]], uint id [[thread_position_in_grid]]) {"
            " if(id==0){out[0]=values.x;out[1]=values.y;out[2]=values.z;out[3]=0x7351cafeu;} }\n";
        id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
        printf("air92 vec3 library=%s error=%s\n",library?NSStringFromClass([(id)library class]).UTF8String:"nil",
            error.description.UTF8String?:"none");
        if(!library)return 1;
        id<MTLCommandQueue> queue=[device newCommandQueue];
        const unsigned char cases[2][4]={{13,97,241,0xa5},{255,0,128,0x5a}};
        for(unsigned frame=0;frame<2;frame++) {
            MTLFunctionConstantValues *constants=[MTLFunctionConstantValues new];
            /* uchar3 has the four-byte storage/alignment of the Metal vector ABI;
             * the fourth byte is deliberately different and is not a fourth lane. */
            [constants setConstantValue:cases[frame] type:MTLDataTypeUChar3 atIndex:0];
            error=nil;
            id<MTLFunction> function=[library newFunctionWithName:@"check_vec3" constantValues:constants error:&error];
            printf("air92 vec3 specialization %u: function=%s error=%s\n",frame,
                function?NSStringFromClass([(id)function class]).UTF8String:"nil",error.description.UTF8String?:"none");
            if(!function)return 1;
            id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
            if(!pipeline){printf("air92 vec3 FAIL pipeline: %s\n",error.description.UTF8String);return 1;}
            id<MTLBuffer> buffer=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            if(!buffer || !buffer.contents || !queue)return 1;
            memset(buffer.contents,0xa5,4096);
            id<MTLCommandBuffer> command=[queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!command || !encoder)return 1;
            [encoder setComputePipelineState:pipeline];[encoder setBuffer:buffer offset:0 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            [encoder endEncoding];[command commit];
            for(unsigned i=0;i<5000 && command.status!=MTLCommandBufferStatusCompleted && command.status!=MTLCommandBufferStatusError;i++)usleep(1000);
            const uint32_t *out=buffer.contents;
            BOOL ok=command.status==MTLCommandBufferStatusCompleted && !command.error &&
                out[0]==cases[frame][0] && out[1]==cases[frame][1] && out[2]==cases[frame][2] && out[3]==0x7351cafe;
            printf("air92 vec3 %s case=%u got=%u,%u,%u,%08x expected=%u,%u,%u,7351cafe padding=%02x status=%lu error=%s\n",
                ok?"PASS":"FAIL",frame,out[0],out[1],out[2],out[3],cases[frame][0],cases[frame][1],cases[frame][2],
                cases[frame][3],(unsigned long)command.status,command.error.description.UTF8String?:"none");
            if(!ok)return 1;
        }
        printf("air92 vec3 PASS: both independently specialized GPU results match\n");
    }
    return 0;
}
