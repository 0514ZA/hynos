/*
 * 混元 OS —— x86_64 CPU 原语（端口 IO / 中断开关 / 停机）
 * 仅 arch 架构适配层使用，上层一律通过 <hyn/arch.h> 调用。
 */

.code64
.text

/* void hyn_outb(u16 port, u8 value) */
.globl hyn_outb
hyn_outb:
    movl %edi, %edx
    movb %sil, %al
    outb %al, %dx
    ret

/* u8 hyn_inb(u16 port) */
.globl hyn_inb
hyn_inb:
    movl %edi, %edx
    xorl %eax, %eax
    inb %dx, %al
    ret

/*
 * 32 位端口访问：PCI 配置空间（0xCF8 地址口 / 0xCFC 数据口）语义上就是一次 dword
 * 读写，用 8 位端口拼出来会读到错的值，所以 arch 层必须补出 dword 访问。
 */
/* void hyn_outl(u16 port, u32 value) */
.globl hyn_outl
hyn_outl:
    movl %edi, %edx
    movl %esi, %eax
    outl %eax, %dx
    ret

/* u32 hyn_inl(u16 port) */
.globl hyn_inl
hyn_inl:
    movl %edi, %edx
    xorl %eax, %eax
    inl %dx, %eax
    ret

/* void hyn_io_wait(void) */
.globl hyn_io_wait
hyn_io_wait:
    xorl %eax, %eax
    outb %al, $0x80
    ret

/* void arch_halt(void) —— 停机直到下一次中断 */
.globl arch_cpu_relax
arch_cpu_relax:
    hlt
    ret

/* void arch_irq_disable(void) */
.globl arch_irq_disable
arch_irq_disable:
    cli
    ret

/* void arch_irq_enable(void) */
.globl arch_irq_enable
arch_irq_enable:
    sti
    ret

/* uptr arch_irq_save(void) —— 返回保存的中断标志并关中断 */
.globl arch_irq_save
arch_irq_save:
    pushfq
    popq %rax
    cli
    ret

/* void arch_irq_restore(uptr flags) */
.globl arch_irq_restore
arch_irq_restore:
    testl $0x200, %edi
    jz 1f
    sti
1:
    ret

/* u64 arch_fault_address(void) —— 最近一次 #PF 的故障线性地址（CR2） */
.globl arch_fault_address
arch_fault_address:
    movq %cr2, %rax
    ret

/* void arch_debug_char(char c) —— 写入 QEMU 调试端口 0xE9 */
.globl arch_debug_char
arch_debug_char:
    movb %dil, %al
    outb %al, $0xE9
    ret

/* void arch_qemu_exit(u32 code) —— isa-debug-exit 设备退出端口 0xF4 */
.globl arch_qemu_exit
arch_qemu_exit:
    movl %edi, %eax
    outl %eax, $0xF4
    ret