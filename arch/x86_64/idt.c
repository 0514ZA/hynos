/*
 * HynOS —— IDT 实装（x86_64 架构适配层）
 */
#include <hyn/idt.h>
#include <hyn/trap.h>
#include <hyn/cpu.h>

typedef struct {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} __attribute__((packed)) hyn_idt_entry_t;

static hyn_idt_entry_t g_idt[HYN_IDT_ENTRIES];
static hyn_idtr_t      g_idtr;

void hyn_idt_set_gate(u32 vector, u64 handler, u16 selector, u8 attr, u8 ist)
{
    hyn_idt_entry_t *e;

    if (vector >= HYN_IDT_ENTRIES) {
        return;
    }
    e = &g_idt[vector];
    e->offset_low  = (u16)(handler & 0xFFFFu);
    e->selector    = selector;
    e->ist         = (u8)(ist & 0x7u);
    e->type_attr   = attr;
    e->offset_mid  = (u16)((handler >> 16) & 0xFFFFu);
    e->offset_high = (u32)((handler >> 32) & 0xFFFFFFFFu);
    e->zero        = 0;
}

u64 hyn_idt_get_handler(u32 vector)
{
    const hyn_idt_entry_t *e;

    if (vector >= HYN_IDT_ENTRIES) {
        return 0;
    }
    e = &g_idt[vector];
    return ((u64)e->offset_high << 32) | ((u64)e->offset_mid << 16) | (u64)e->offset_low;
}

u8 hyn_idt_get_attr(u32 vector)
{
    return (vector < HYN_IDT_ENTRIES) ? g_idt[vector].type_attr : 0u;
}

/*
 * 门的 DPL。P=0（表项为空）时按 0 返回：空表项既不是门，也不该被当成"用户可进"。
 * 自检靠 hyn_idt_gate_dpl 遍历全表统计 DPL=3 的门数——"syscall 是唯一入口"这条纪律
 * 因此不靠代码走查，而是可离线核对的硬件事实。
 */
u32 hyn_idt_gate_dpl(u32 vector)
{
    u8 attr;

    if (vector >= HYN_IDT_ENTRIES) {
        return 0u;
    }
    attr = g_idt[vector].type_attr;
    if ((attr & 0x80u) == 0u) {
        return 0u;
    }
    return (u32)((attr >> 5) & 0x3u);
}

void hyn_idt_init(void)
{
    u32 v;
    u32 dedicated = HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT;   /* 0..47 有专属桩 */

    for (v = 0; v < HYN_IDT_ENTRIES; v++) {
        g_idt[v].offset_low = 0;
        g_idt[v].selector = 0;
        g_idt[v].ist = 0;
        g_idt[v].type_attr = 0;
        g_idt[v].offset_mid = 0;
        g_idt[v].offset_high = 0;
        g_idt[v].zero = 0;
    }

    for (v = 0; v < dedicated; v++) {
        u8 attr = (v < HYN_TRAP_EXC_COUNT) ? HYN_IDT_ATTR_TRAP_GATE : HYN_IDT_ATTR_INT_GATE;
        hyn_idt_set_gate(v, hyn_isr_stub_table[v], HYN_SEL_KCODE64, attr, 0);
    }
    for (v = dedicated; v < HYN_IDT_ENTRIES; v++) {
        hyn_idt_set_gate(v, (u64)(uptr)hyn_isr_default, HYN_SEL_KCODE64, HYN_IDT_ATTR_INT_GATE, 0);
    }

    /*
     * syscall 唯一入口：向量 0x80 单独改成 DPL=3 的中断门。
     * 上面两轮已经把 0x80 设成 DPL=0 的默认桩，这里覆盖它——全表只有这一个门允许
     * Ring3 进入，"唯一入口"因此在硬件层成立，且可被自检逐项核对。
     */
    hyn_idt_set_gate(HYN_TRAP_SYSCALL_VECTOR, (u64)(uptr)isr_128,
                     HYN_SEL_KCODE64, HYN_IDT_ATTR_USER_INT_GATE, 0);

    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base  = (u64)(uptr)g_idt;
    __asm__ __volatile__("lidt %0" ::"m"(g_idtr) : "memory");
}