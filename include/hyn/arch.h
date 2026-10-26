/*
 * HynOS —— 架构适配层统一接口
 *
 * 上层（core/hal）只允许通过这些接口触碰 CPU 专属能力，
 * 真正的实现位于 arch/x86_64/，未来 aarch64/riscv64 只需提供同名实现。
 */
#ifndef HYN_ARCH_H
#define HYN_ARCH_H

#include <hyn/types.h>

#define HYN_ARCH_X86_64 1

const char *arch_name(void);

/* ---- 端口 IO（x86 专属，仅 arch 层与 HAL 底层可用） ---- */
void hyn_outb(u16 port, u8 value);
u8   hyn_inb(u16 port);
void hyn_outl(u16 port, u32 value);       /* dword 端口访问：PCI 配置空间用 */
u32  hyn_inl(u16 port);
void hyn_io_wait(void);

/* ---- 线程上下文切换与进入用户态 ----
 * 栈的保存/恢复是 CPU 专属动作，实现在 arch/x86_64/thread.S；上层（core/thread）只调这些接口。
 */
void hyn_cpu_switch(u64 *save_rsp, u64 new_rsp);      /* 调用者负责关/开中断包夹 */
void hyn_cpu_thread_entry(void) __attribute__((noreturn));   /* 新线程首次着陆点 */
void hyn_arch_enter_user(u64 entry_rip, u64 user_rsp) __attribute__((noreturn));

/* 当前线程的内核栈顶：从 Ring3 陷入时 CPU 去 TSS.rsp0 取它（每次线程切换更新） */
void arch_set_kernel_stack(u64 stack_top);

/* ---- CPU 控制 ---- */
void arch_halt_forever(void) __attribute__((noreturn));
void arch_cpu_relax(void);
void arch_irq_disable(void);
void arch_irq_enable(void);
uptr arch_irq_save(void);
void arch_irq_restore(uptr flags);

/* ---- 故障地址：最近一次 #PF 的 CR2（用户态越权访存的取证依据） ---- */
u64 arch_fault_address(void);

/* ---- 早期串行调试出口（QEMU 调试口 0xE9，无副作用） ---- */
void arch_debug_char(char c);

/* ---- QEMU 自检退出口（isa-debug-exit 设备，缺失时写端口无副作用） ---- */
void arch_qemu_exit(u32 code);

#endif /* HYN_ARCH_H */