#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* This settings-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static ULONG get32(const unsigned char *p)
{
    return (ULONG)p[0] | (ULONG)p[1] << 8 | (ULONG)p[2] << 16 | (ULONG)p[3] << 24;
}

/* The EEPROM's section checksum as Cxbx-Reloaded computes it: the dword sum,
 * the carries counted apart and added back at the end, inverted. */
static ULONG checksum(const unsigned char *p, unsigned bytes)
{
    ULONG high = 0, low = 0;
    unsigned i;
    for (i = 0; i + 4 <= bytes; i += 4) {
        ULONG v = get32(p + i);
        unsigned long long sum = ((unsigned long long)high << 32 | low) + v;
        high = (ULONG)(sum >> 32);
        low += v;
    }
    return ~(high + low);
}

static ULONG single(ULONG index)
{
    ULONG value = 0, type = 0, length = 0;
    if (xbox_ExQueryNonVolatileSetting(index, &type, &value, sizeof value, &length))
        return 0xFFFFFFFFu;
    return value;
}

int main(void) {
    ULONG value=0xdeadbeef, type=0, length=0;
    NTSTATUS status=xbox_ExQueryNonVolatileSetting(XC_AUDIO,&type,&value,sizeof(value),&length);
    if(status || value!=0 || type!=4 || length!=sizeof(value)) {
        fprintf(stderr,"Unexpected audio setting: status=%lx value=%lx type=%lu length=%lu\n",status,value,type,length);
        return 1;
    }
    puts("PASS: EEPROM reports stereo PCM without unsupported encoded output.");

    /* XC_MAX_OS (0xFF): the 0x60-byte user section, REG_BINARY, agreeing with
     * the single-value answers, with a valid checksum. */
    {
        unsigned char user[0x100], all[0x100];
        memset(user, 0xCC, sizeof user);
        type = 0; length = 0;
        status = xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, user, sizeof user, &length);
        if (status || type != 3 || length != 0x60) {
            fprintf(stderr, "XC_MAX_OS: status=%lx type=%lu length=%lu\n", status, type, length);
            return 1;
        }
        if (user[0x60] != 0xCC) {
            fprintf(stderr, "XC_MAX_OS wrote past its 0x60 bytes\n");
            return 1;
        }
        if (get32(user + 0x30) != single(XC_LANGUAGE) || get32(user + 0x34) != single(XC_VIDEO) ||
            get32(user + 0x38) != single(XC_AUDIO) || get32(user + 0x5C) != single(XC_DVD_REGION) ||
            get32(user + 0x58) != single(XC_MISC)) {
            fprintf(stderr, "XC_MAX_OS disagrees with the single-value indices\n");
            return 1;
        }
        if (get32(user + 0x04) != 0 || get32(user + 0x18) != 0 || get32(user + 0x1C) != 0) {
            fprintf(stderr, "XC_MAX_OS time zone is not UTC without daylight saving\n");
            return 1;
        }
        if (get32(user) != checksum(user + 4, 0x5C)) {
            fprintf(stderr, "XC_MAX_OS checksum %08lx, expected %08lx\n",
                    get32(user), checksum(user + 4, 0x5C));
            return 1;
        }
        /* XC_MAX_ALL (0xFFFF): all 256 bytes, the user section at 0x60 and the
         * factory section at 0x30 with its own checksum. */
        status = xbox_ExQueryNonVolatileSetting(0xFFFF, &type, all, sizeof all, &length);
        if (status || type != 3 || length != 0x100 || memcmp(all + 0x60, user, 0x60) != 0) {
            fprintf(stderr, "XC_MAX_ALL: status=%lx type=%lu length=%lu or user section differs\n",
                    status, type, length);
            return 1;
        }
        if (get32(all + 0x30) != checksum(all + 0x34, 0x2C) ||
            memcmp(all + 0x34, "100000000000", 12) != 0 ||
            get32(all + 0x58) != single(XC_FACTORY_AV_REGION)) {
            fprintf(stderr, "XC_MAX_ALL factory section is inconsistent\n");
            return 1;
        }
        /* A buffer too small for the section is refused, and says how much
         * it needs. */
        length = 0;
        status = xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, user, 0x20, &length);
        if (status != (NTSTATUS)0xC0000023L || length != 0x60) {
            fprintf(stderr, "XC_MAX_OS short buffer: status=%lx length=%lu\n", status, length);
            return 1;
        }
    }
    puts("PASS: EEPROM sections (XC_MAX_OS, XC_MAX_ALL) agree with the single values.");
    return 0;
}
