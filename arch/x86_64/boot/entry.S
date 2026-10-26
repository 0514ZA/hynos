/*
 * 混元 OS —— 64 位内核入口（镜像第一个字节所在段）
 *
 * 由引导扇区在长模式下远跳到 0x100000。此处只做最小准备：
 *   设置数据段、装载内核引导栈（由链接脚本在 .stack 段预留）、
 *   把 boot_info 指针（约定在 0x6000）传给 kernel_main。
 * 其余初始化（GDT 重装、IDT、中断、虚拟内存接管）都在 arch 层与内核主线里完成。
 */

.section .text.boot, "ax"
.code64
.globl _start64

_start64:
    movw $0x3F8, %dx
    movb $'6', %al
    outb %al, %dx
    cli
    movw $0x0010, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    leaq _boot_stack_top(%rip), %rsp
    movl $0x6000, %edi            /* boot_info 物理地址（恒等映射内） */
    call kernel_main

.Lkernel_hang:
    cli
    hlt
    jmp .Lkernel_hang