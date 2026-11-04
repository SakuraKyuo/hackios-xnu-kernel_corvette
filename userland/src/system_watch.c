/* Observe the real launchd session without creating a diagnostic Metal device. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
extern int reboot(int howto);
#define RB_HALT 0x08 /* XNU bsd/sys/reboot.h; absent from the public SDK. */
#include <time.h>
#include <unistd.h>

static void resource(const char *path)
{
    unsigned char magic[4] = {0};
    struct stat st;
    FILE *f = fopen(path, "rb");
    int valid = f && fread(magic, 1, 4, f) == 4 && stat(path, &st) == 0;
    if (f) fclose(f);
    if (valid)
        printf("air90 resource: %s size=%lld magic=%02x%02x%02x%02x\n", path,
            (long long)st.st_size, magic[0], magic[1], magic[2], magic[3]);
    else
        printf("air90 resource: MISSING %s errno=%d\n", path, errno);
}

static int snapshot(unsigned elapsed, int verbose)
{
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t size = 0;
    struct kinfo_proc *procs = NULL;
    int error = -1;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) break;
        size += 32 * sizeof(*procs);
        if (size > 4 * 1024 * 1024) break;
        procs = malloc(size);
        if (!procs) break;
        error = sysctl(mib, 4, procs, &size, NULL, 0);
        if (error == 0) break;
        free(procs); procs = NULL;
        if (errno != ENOMEM) break;
    }
    if (error || !procs) {
        printf("air90 monitor: t=%u process query failed errno=%d\n", elapsed, errno);
        return 0;
    }
    size_t count = size / sizeof(*procs);
    static pid_t previous_backboard=-1,previous_springboard=-1;
    pid_t current_backboard=0,current_springboard=0;
    for(size_t i=0;i<count;i++) {
        if(!strcmp(procs[i].kp_proc.p_comm,"backboardd"))current_backboard=procs[i].kp_proc.p_pid;
        if(!strcmp(procs[i].kp_proc.p_comm,"SpringBoard"))current_springboard=procs[i].kp_proc.p_pid;
    }
    if(current_backboard==previous_backboard && current_springboard==previous_springboard) {
        free(procs);return current_backboard && current_springboard;
    }
    previous_backboard=current_backboard;previous_springboard=current_springboard;
    verbose=0;
    int backboard = 0, springboard = 0;
    printf("air90 monitor: t=%u processes=%zu tracked:", elapsed, count);
    for (size_t i = 0; i < count; i++) {
        const char *name = procs[i].kp_proc.p_comm;
        if (!strcmp(name, "backboardd")) backboard++;
        if (!strcmp(name, "SpringBoard")) springboard++;
        if (!strcmp(name, "backboardd") || !strcmp(name, "SpringBoard") ||
            !strcmp(name, "runningboardd") || !strcmp(name, "notifyd") ||
            !strcmp(name, "cfprefsd") || !strcmp(name, "logd") || !strcmp(name, "lsd") ||
            !strncmp(name, "containermanag", 14) || !strcmp(name, "usermanagerd"))
            printf(" %s(pid=%d,ppid=%d,state=%d)", name, procs[i].kp_proc.p_pid,
                procs[i].kp_eproc.e_ppid, procs[i].kp_proc.p_stat);
    }
    printf("; backboardd=%d SpringBoard=%d\n", backboard, springboard);
    if (verbose) {
        printf("air90 processes t=%u:", elapsed);
        for (size_t i = 0; i < count; i++)
            printf(" %d:%s", procs[i].kp_proc.p_pid, procs[i].kp_proc.p_comm);
        printf("\n");
    }
    free(procs);
    return backboard && springboard;
}

/* Replay the bounded first-failure record so the loader's tail retains it. */
static void renderer_trace(unsigned elapsed)
{
    char buffer[1025];
    FILE *file=fopen("/private/var/tmp/hakios-air90-metal.log","rb");
    if (!file) return;
    static long offset=0;
    if(fseek(file,offset,SEEK_SET)!=0){fclose(file);return;}
    int first=fgetc(file);if(first==EOF){fclose(file);return;}ungetc(first,file);
    printf("air90 renderer trace replay t=%u begin\n",elapsed);
    size_t total=0, got;
    while (total<32768 && (got=fread(buffer,1,1024,file))!=0) {
        buffer[got]=0; fputs(buffer,stdout); total+=got;
    }
    offset=ftell(file);
    fclose(file);
    printf("air90 renderer trace replay t=%u end (%zu bytes)\n",elapsed,total);
}

static void cpu_fault_trace(unsigned elapsed)
{
    FILE *file=fopen("/private/var/tmp/hakios-air94-fault.log","rb");
    if(!file)return;
    static long offset=0;
    if(fseek(file,offset,SEEK_SET)!=0){fclose(file);return;}
    int first=fgetc(file);if(first==EOF){fclose(file);return;}ungetc(first,file);
    char buffer[1025];size_t got,total=0;
    printf("air94 CPU fault replay t=%u begin\n",elapsed);
    while(total<32768 && (got=fread(buffer,1,1024,file))!=0) {
        buffer[got]=0;fputs(buffer,stdout);total+=got;
    }
    offset=ftell(file);
    fclose(file);
    printf("air94 CPU fault replay end bytes=%zu\n",total);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "watchdog")) {
        printf("air90 watchdog: automatic halt disabled; system stays running\n");
        return 0;
    }
    printf("air90 stage: launchd monitor started pid=%d ppid=%d; real system UI, no diagnostic drawing\n",
        getpid(), getppid());
    resource("/System/Library/Frameworks/QuartzCore.framework/default.metallib");
    resource("/System/Library/Frameworks/CoreImage.framework/ci_stdlib.metallib");
    resource("/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib");
    const char *names[] = {"MPSCore", "MPSImage", "MPSFunctions", "MPSMatrix"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "/System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/%s.framework/default.metallib", names[i]);
        resource(path);
    }
    struct statfs mounts;
    if (!statfs("/private/var", &mounts))
        printf("air90 data: /private/var filesystem=%s mounted=%s\n", mounts.f_fstypename, mounts.f_mntonname);
    int alive = 0;
    for (unsigned elapsed = 0; elapsed <= 300; elapsed += 5) {
        if (elapsed == 10 || elapsed == 30 || elapsed == 60 || elapsed == 120 || elapsed == 180 || elapsed == 300)
            renderer_trace(elapsed);
        if((elapsed % 10)==0)cpu_fault_trace(elapsed);
        alive = snapshot(elapsed, elapsed == 10 || elapsed == 30 || elapsed == 60 || elapsed == 300);
        if (elapsed < 300) sleep(5);
    }
    printf("air90 end: both UI process names present=%d; require GPU retire, framebuffer swaps and visible UI to assess success\n", alive);
    printf("air90 stage: initial observation complete; system remains running (no timed halt)\n");
    for (unsigned elapsed=360;;elapsed+=60) {
        sleep(60);
        renderer_trace(elapsed);
        cpu_fault_trace(elapsed);
        snapshot(elapsed,0);
    }
}
