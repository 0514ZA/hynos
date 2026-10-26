/*
 * HynOS —— GDT 实装（x86_64 架构适配层）
 */
#include <hyn/gdt.h>
#include <hyn/cpu.h>
#include <hyn/kprintf.h>
#include <hyn/tss.h>

static u64          g_gdt[HYN_GDT_ENTRIES];
static hyn_gdtr_t   g_gdtr;
static bool_t       g_ready;

u64 hyn_gdt_make_desc(u32 base, u32 limit, u8 access, u8 flags)
{
    u64 d = 0;
    d |= (u64)(limit & 0xFFFFu);
    d |= (u64)(base & 0xFFFFFFu) << 16;
    d |= (u64)access << 40;
    d |= (u64)((limit >> 16) & 0xFu) << 48;
    d |= (u64)(flags & 0xFu) << 52;
    d |= (u64)(base >> 24) << 56;
    return d;
}

void hyn_gdt_set_entry(u32 index, u64 raw)
{
    if (index < HYN_GDT_ENTRIES) {
        g_gdt[index] = raw;
    }
}

u64 hyn_gdt_get_entry(u32 index)
{
    return (index < HYN_GDT_ENTRIES) ? g_gdt[index] : 0;
}

static void gdt_load(void)
{
    g_gdtr.limit = (u16)(sizeof(g_gdt) - 1);
    g_gdtr.base  = (u64)(uptr)g_gdt;
    __asm__ __volatile__("lgdt %0" ::"m"(g_gdtr) : "memory");

    /* 远返回重装 CS（0x18 = 64 位内核代码段），再重装数据段寄存器 */
    __asm__ __volatile__(
        "pushq $0x18\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        ::: "rax", "memory");
}

void hyn_gdt_init(void)
{
    u32 i;

    for (i = 0; i < HYN_GDT_ENTRIES; i++) {
        g_gdt[i] = 0;
    }

    g_gdt[0] = 0;                                                          /* 0x00 空 */
    g_gdt[1] = hyn_gdt_make_desc(0, 0xFFFFF, 0x9A, 0xC);                   /* 0x08 32 位代码 */
    g_gdt[2] = hyn_gdt_make_desc(0, 0xFFFFF, 0x92, 0xC);                   /* 0x10 数据 */
    g_gdt[3] = hyn_gdt_make_desc(0, 0xFFFFF, 0x9A, 0xA);                   /* 0x18 64 位代码 */
    g_gdt[4] = hyn_gdt_make_desc(0, 0xFFFFF, 0xF2, 0xC);                   /* 0x20 用户数据 */
    g_gdt[5] = hyn_gdt_make_desc(0, 0xFFFFF, 0xFA, 0xA);                   /* 0x28 64 位用户代码 */

    gdt_load();

    /*
     * TSS 必须在 GDT 装载之后装：它写的是 GDT 第 6/7 项（选择子 0x30），
     * 而 lgdt 已把表基址指向 g_gdt，写表项对 CPU 立即可见，随后 ltr 才能成功。
     * 64 位下 TSS 唯一不可省略的用途就是给"Ring3 陷入 Ring0"提供内核栈 rsp0。
     */
    if (hyn_tss_init() != HYN_OK) {
        kprintf("[GDT] TSS 装载失败：Ring3 将无法陷入内核\n");
    } else {
        kprintf("[GDT] TSS 已装载（选择子 0x%x，rsp0 由线程切换更新，I/O 位图关闭）\n",
                (u32)HYN_SEL_TSS);
    }

    g_ready = HYN_TRUE;
    kprintf("[GDT] 已装载 %u 项，CS=0x%x SS=0x%x\n",
            (u32)HYN_GDT_ENTRIES, (u32)hyn_read_cs(), (u32)hyn_read_ss());
    HYN_UNUSED(g_ready);
}