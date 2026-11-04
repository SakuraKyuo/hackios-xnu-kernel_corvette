#ifndef HAKIOS_METAL_DEBUG_H
#define HAKIOS_METAL_DEBUG_H
#include <stdbool.h>
#include <stdlib.h>
static inline bool hakios_metal_debug_enabled(void)
{
    const char *value=getenv("HAKIOS_GPU_DEBUG");
    return value && value[0]=='1';
}
#endif
