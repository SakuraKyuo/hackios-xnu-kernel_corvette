/* Verify the cold dyld/libSystem path before PID 1 becomes launchd. */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    static const unsigned char expected[16] = {
        0xe5,0x42,0x0d,0xb3,0x30,0xd0,0x33,0xaa,0x8b,0x7e,0x05,0xb5,0x03,0x8b,0xb4,0x6d
    };
    bool (*get_uuid)(unsigned char *) = dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_uuid");
    const void *(*get_range)(size_t *) = dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_range");
    unsigned char uuid[16] = {0};
    size_t length = 0;
    const void *base = get_range ? get_range(&length) : NULL;
    Dl_info info = {0};
    bool valid = get_uuid && get_uuid(uuid) && base && length &&
        !memcmp(uuid, expected, sizeof(uuid)) && dladdr((const void *)printf, &info) && info.dli_fname;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("air86 cache probe: pid=%d ppid=%d; libSystem entered before launchd\n", getpid(), getppid());
    printf("air86 cache probe: uuid=");
    for (unsigned i = 0; i < sizeof(uuid); i++) printf("%02x", uuid[i]);
    printf(" range=%p size=%zu printf-library=%s\n", base, length, info.dli_fname ? info.dli_fname : "unresolved");
    printf("air86 cache probe: %s\n", valid ? "PASS exact System cache mapped" : "FAIL cache identity or libSystem lookup");
    return valid ? 0 : 1;
}
