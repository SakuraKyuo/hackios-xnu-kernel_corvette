/* Bounded MTLB external-function metadata reader. All integers are little endian. */
#ifndef HAKIOS_METAL_LIBRARY_METADATA_H
#define HAKIOS_METAL_LIBRARY_METADATA_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int (*hakios_mtlb_name_sink)(void *, const char *, size_t);
static uint16_t hm_u16(const unsigned char *p) { return (uint16_t)p[0] | (uint16_t)p[1]<<8; }
static uint32_t hm_u32(const unsigned char *p) { return (uint32_t)hm_u16(p) | (uint32_t)hm_u16(p+2)<<16; }
static uint64_t hm_u64(const unsigned char *p) { return (uint64_t)hm_u32(p) | (uint64_t)hm_u32(p+4)<<32; }
static int hakios_mtlb_extern_names(const void *bytes, size_t length, hakios_mtlb_name_sink sink, void *context)
{
    const unsigned char *p=bytes;
    if (!p || length<88 || memcmp(p,"MTLB",4)) return 0;
    uint64_t total=hm_u64(p+16), table=hm_u64(p+24), table_size=hm_u64(p+32);
    if (total>length || total<88 || table>total || total-table<4 || table_size>total-table-4) return 0;
    /* The function-list length excludes its leading 32-bit record count. */
    size_t end=(size_t)(table+4+table_size), at=(size_t)table;
    if (end-at<4) return 0;
    uint32_t count=hm_u32(p+at);at+=4;
    if (count>65536) return 0;
    for (uint32_t i=0;i<count;i++) {
        if (at>end || end-at<4) return 0;
        uint32_t size=hm_u32(p+at);
        if (size<8 || size>end-at) return 0;
        size_t stop=at+size,q=at+4,name_at=0,name_length=0;
        unsigned type=255;int terminated=0;
        while (q<stop) {
            if (stop-q<4) return 0;
            if (!memcmp(p+q,"ENDT",4)) { terminated=1;break; }
            if (stop-q<6) return 0;
            uint16_t n=hm_u16(p+q+4);
            if (n>stop-q-6) return 0;
            if (!memcmp(p+q,"NAME",4)) { name_at=q+6;name_length=n; }
            if (!memcmp(p+q,"TYPE",4)) { if(n!=1)return 0;type=p[q+6]; }
            q+=6+n;
        }
        if (!terminated || !name_at || !name_length || type==255) return 0;
        if (type==5) {
            if (p[name_at+name_length-1]!=0 || memchr(p+name_at,0,name_length-1)) return 0;
            if (sink && !sink(context,(const char *)p+name_at,name_length-1)) return 0;
        }
        at=stop;
    }
    return 1;
}
#endif
