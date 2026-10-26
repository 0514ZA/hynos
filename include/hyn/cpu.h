/*
 * 混元 OS —— CPU 专属原语（x86_64）
 *
 * 仅供 arch 适配层使用；上层（core/hal）一律通过 hyn/arch.h 的架构无关接口。
 */
#ifndef HYN_CPU_H
#define HYN_CPU_H

#include <hyn/types.h>

const char *arch_name(void);

/* ===== 段选择子（与 GDT 布局一一对应） ===== */
#define HYN_SEL_NULL    0x00
#define HYN_SEL_KCODE32 0x08
#define HYN_SEL_KDATA   0x10
#define HYN_SEL_KCODE64 0x18
#define HYN_SEL_UDATA   0x23    /* 索引 4，RPL=3 */
#define HYN_SEL_UCODE64 0x2B    /* 索引 5，RPL=3 */
#define HYN_SEL_TSS     0x30    /* 索引 6（64 位 TSS 占两个表项） */

/* ===== 控制寄存器 ===== */
static inline uptr hyn_read_cr0(void)
{ uptr v; __asm__ __volatile__("movq %%cr0,%0" : "=r"(v)); return v; }
static inline uptr hyn_read_cr2(void)
{ uptr v; __asm__ __volatile__("movq %%cr2,%0" : "=r"(v)); return v; }
static inline uptr hyn_read_cr3(void)
{ uptr v; __asm__ __volatile__("movq %%cr3,%0" : "=r"(v)); return v; }
static inline uptr hyn_read_cr4(void)
{ uptr v; __asm__ __volatile__("movq %%cr4,%0" : "=r"(v)); return v; }
static inline void hyn_write_cr3(uptr v)
{ __asm__ __volatile__("movq %0,%%cr3" ::"r"(v) : "memory"); }

/* ===== MSR ===== */
#define HYN_MSR_EFER 0xC0000080u
#define HYN_EFER_SCE  0x00000001u
#define HYN_EFER_LME  0x00000100u
#define HYN_EFER_NXE  0x00000800u

static inline u64 hyn_read_msr(u32 msr)
{
    u32 lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | (u64)lo;
}
static inline void hyn_write_msr(u32 msr, u64 value)
{
    __asm__ __volatile__("wrmsr" ::"c"(msr), "a"((u32)value), "d"((u32)(value >> 32)));
}

/* ===== 标志位与指令 ===== */
static inline u64 hyn_read_flags(void)
{ u64 f; __asm__ __volatile__("pushfq; popq %0" : "=r"(f) :: "memory"); return f; }
static inline u16 hyn_read_cs(void)
{ u16 v; __asm__ __volatile__("movw %%cs,%0" : "=r"(v)); return v; }
static inline u16 hyn_read_ss(void)
{ u16 v; __asm__ __volatile__("movw %%ss,%0" : "=r"(v)); return v; }
static inline void hyn_halt(void) { __asm__ __volatile__("hlt"); }
static inline void hyn_pause(void) { __asm__ __volatile__("pause"); }
static inline void hyn_sti(void) { __asm__ __volatile__("sti"); }
static inline void hyn_cli(void) { __asm__ __volatile__("cli"); }

/* ===== CPU 识别 ===== */
typedef struct { u32 eax, ebx, ecx, edx; } hyn_cpuid_t;
void hyn_cpuid(u32 leaf, u32 subleaf, hyn_cpuid_t *out);
const char *hyn_cpu_vendor(void);   /* 12 字符厂商串，静态缓冲 */
const char *hyn_cpu_brand(void);    /* 48 字符品牌串，静态缓冲 */
u64 hyn_cpu_features_edx1(void);    /* CPUID.1:EDX */
u64 hyn_cpu_features_ecx1(void);    /* CPUID.1:ECX */

#endif /* HYN_CPU_H */