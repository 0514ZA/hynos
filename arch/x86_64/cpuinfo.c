/*
 * HynOS —— CPU 识别实装（x86_64）
 */
#include <hyn/cpu.h>

static char g_vendor[13];
static char g_brand[49];

void hyn_cpuid(u32 leaf, u32 subleaf, hyn_cpuid_t *out)
{
    hyn_cpuid_t r;
    __asm__ __volatile__("cpuid"
                         : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                         : "a"(leaf), "c"(subleaf));
    if (out != NULL) {
        *out = r;
    }
}

const char *hyn_cpu_vendor(void)
{
    hyn_cpuid_t r;

    if (g_vendor[0] == '\0') {
        hyn_cpuid(0, 0, &r);
        __builtin_memcpy(&g_vendor[0], &r.ebx, 4);
        __builtin_memcpy(&g_vendor[4], &r.edx, 4);
        __builtin_memcpy(&g_vendor[8], &r.ecx, 4);
        g_vendor[12] = '\0';
    }
    return g_vendor;
}

const char *hyn_cpu_brand(void)
{
    if (g_brand[0] == '\0') {
        hyn_cpuid_t max_ext;
        u32 base, i;

        hyn_cpuid(0x80000000u, 0, &max_ext);
        if (max_ext.eax < 0x80000004u) {
            __builtin_memcpy(g_brand, "unknown", 8);
            return g_brand;
        }
        for (i = 0; i < 3; i++) {
            hyn_cpuid_t r;
            hyn_cpuid(0x80000002u + i, 0, &r);
            base = i * 16u;
            __builtin_memcpy(&g_brand[base + 0],  &r.eax, 4);
            __builtin_memcpy(&g_brand[base + 4],  &r.ebx, 4);
            __builtin_memcpy(&g_brand[base + 8],  &r.ecx, 4);
            __builtin_memcpy(&g_brand[base + 12], &r.edx, 4);
        }
        g_brand[48] = '\0';
    }
    return g_brand;
}

u64 hyn_cpu_features_edx1(void)
{
    hyn_cpuid_t r;
    hyn_cpuid(1, 0, &r);
    return (u64)r.edx;
}

u64 hyn_cpu_features_ecx1(void)
{
    hyn_cpuid_t r;
    hyn_cpuid(1, 0, &r);
    return (u64)r.ecx;
}