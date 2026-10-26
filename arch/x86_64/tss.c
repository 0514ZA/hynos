/*
 * 混元 OS —— x86_64 TSS 实装（arch 适配层）
 *
 * 只做硬件要求的最小可用形态：提供 TSS.rsp0 作为"Ring3 陷入内核时的栈"。
 * 每次线程切换都会把 rsp0 更新为下一个线程的内核栈顶，因此 Ring3 的陷阱帧
 * 总是落在"当前线程自己的内核栈"上，不会串到别的线程栈里去。
 *
 * I/O 位图基址指向 TSS 末尾（= 没有位图）：Ring3 执行 in/out 会直接 #GP，
 * 而不是悄悄摸到端口——这是"用户态不许碰硬件"的硬件级兜底。
 */
#include <hyn/tss.h>
#include <hyn/gdt.h>
#include <hyn/cpu.h>
#include <hyn/arch.h>

static hyn_tss64_t g_tss;
static bool_t      g_tss_ready;

hyn_status_t hyn_tss_init(void)
{
    u64 base = (u64)(uptr)&g_tss;
    u32 limit = (u32)(sizeof(g_tss) - 1u);
    u64 lo, hi;

    if (g_tss_ready) {
        return HYN_OK;
    }

    g_tss.rsp0 = 0ull;
    g_tss.iomap_base = (u16)sizeof(g_tss);

    /* 64 位模式下的 TSS 描述符：type=1001（可用 64 位 TSS），P=1，DPL=0，G=0（limit 按字节） */
    lo = (u64)(limit & 0xFFFFu)
       | ((base & 0xFFFFFFull) << 16)
       | (0x89ull << 40)
       | ((u64)((limit >> 16) & 0xFull) << 48)
       | (((base >> 24) & 0xFFull) << 56);
    hi = ((base >> 32) & 0xFFFFFFFFull) << 8;

    hyn_gdt_set_entry(6u, lo);      /* 0x30：TSS 低 8 字节 */
    hyn_gdt_set_entry(7u, hi);      /* 0x38：TSS 高 8 字节 */

    __asm__ __volatile__("ltr %0" ::"r"((u16)HYN_SEL_TSS) : "memory");

    g_tss_ready = HYN_TRUE;
    return HYN_OK;
}

void hyn_tss_set_rsp0(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
}

u64 hyn_tss_rsp0(void)
{
    return g_tss.rsp0;
}

int hyn_tss_ready(void)
{
    return (g_tss_ready == HYN_TRUE) ? 1 : 0;
}

/* arch.h 的架构无关入口：x86_64 下就是写 TSS.rsp0 */
void arch_set_kernel_stack(u64 stack_top)
{
    hyn_tss_set_rsp0(stack_top);
}