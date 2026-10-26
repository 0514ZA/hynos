/*
 * HynOS —— UEFI 引导进入点（.text.bootuefi，内核镜像内）
 *
 * 与 BIOS 路径的区别：UEFI 固件已经把控制权交给引导程序（PE32+）时，CPU 已处于
 * 长模式，但运行在现场是"固件的 GDT + 固件的页表"之下。本阶段负责把运行现场切换
 * 成"HynOS 自己的 GDT + HynOS 自己的 0..4GB 恒等映射"，然后交给内核 64 位入口。
 *
 * 约定（由引导程序保证）：
 *   · 内核镜像已被复制到 0x100000
 *   · boot_info 结构已写在 0x6000（magic / 内存容量 / 内核大小 / 引导方式=UEFI）
 * 本阶段不使用固件的任何服务，也不依赖固件页表中 0x100000 之外的任何映射。
 */
.section .text.bootuefi, "ax"
.code64
.globl _start_uefi
.type _start_uefi, @function
_start_uefi:
    /* 1) 装载HynOS 自己的 GDT，并重装 CS 与数据段寄存器 */
    leaq uefi_gdt_desc(%rip), %rax
    lgdt (%rax)

    pushq $0x18
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* 2) 页表区 0x70000..0x76000 清零 */
    movl $0x70000, %edi
    movl $0x1800, %ecx
    xorl %eax, %eax
    rep stosl

    /* 3) PML4[0] -> PDPT(0x71000)；PDPT[0..3] -> PD0..PD3 */
    movl $0x00071003, 0x70000
    xorl %ecx, %ecx
    movl $0x72000, %edx
2:
    movl %edx, %eax
    orl $0x03, %eax
    movl %eax, 0x71000(,%rcx,8)
    addl $0x1000, %edx
    incl %ecx
    cmpl $4, %ecx
    jb 2b

    /* 4) 4 个 PD × 512 个 2MB 页 = 0..4GB 恒等映射
     *    与 BIOS 路径同理：PAE/长模式页表项是 8 字节，步长必须为 8。 */
    xorl %ecx, %ecx
    movl $0x72000, %edi
    movl $0x00000083, %eax
3:
    movl %eax, (%edi)
    addl $0x00200000, %eax
    addl $8, %edi
    incl %ecx
    cmpl $2048, %ecx
    jb 3b

    movl $0x70000, %eax
    movq %rax, %cr3

    /* 5) 交给内核 64 位入口（entry.S：设段寄存器与引导栈后调用 kernel_main） */
    movabsq $_start64, %rax
    jmp *%rax
.size _start_uefi, . - _start_uefi

/* UEFI 进入时固件 GDT 与本表的 0x08/0x10/0x18 语义不同，故此处自带一份 */
.balign 8
uefi_gdt_desc:
    .word uefi_gdt_end - uefi_gdt - 1
    .quad uefi_gdt
uefi_gdt:
    .quad 0x0000000000000000      /* 0x00 空描述符 */
    .quad 0x00CF9A000000FFFF      /* 0x08 32 位代码段 */
    .quad 0x00CF92000000FFFF      /* 0x10 数据段 */
    .quad 0x00AF9A000000FFFF      /* 0x18 64 位代码段 */
uefi_gdt_end: