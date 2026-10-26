/* HynOS —— x86_64 架构层公共实现（后续 GDT/IDT/页表/上下文切换在此扩展） */
#include <hyn/arch.h>

const char *arch_name(void)
{
    return "x86_64";
}

void arch_halt_forever(void)
{
    for (;;) {
        arch_irq_disable();
        arch_cpu_relax();
    }
}