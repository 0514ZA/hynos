/*
 * HynOS 智能分层混合内核 —— 内核镜像内的 32 位引导阶段（.text.boot32，镜像首字节）
 *
 * 由 MBR 在保护模式下远跳到 0x10000 进入。此处不受 512 字节限制，负责完成：
 *   1) 把内核镜像从 0x10000 迁移到 0x100000（自迁移，无需重定位符号）
 *   2) 写 boot_info 结构到 0x6000（magic / 内存容量 / 内核大小）
 *   3) 建立 0..4GB 的 2MB 页恒等映射（PML4 -> PDPT -> PD，四级页表）
 *   4) 打开 PAE/LME/PG 进入长模式，远跳到 64 位入口 _start64
 *
 * 迁移前阶段的硬纪律：此时代码运行于 0x10000 而链接地址是 0x100000，
 * 因此除远跳的立即数操作数外，**不得引用本镜像内的任何符号**；
 * 只使用字面量常量（MBR 回填字段、0x10000/0x100000、MBR 基址）。
 * 迁移后阶段运行地址与链接地址一致，可自由引用符号。
 */

.set MBR_BASE,      0x7C00
.set FIELD_SIZE,    0x7C00 + 0x1B4      /* u32 内核字节数（MBR 回填） */
.set FIELD_SECTORS, 0x7C00 + 0x1B0      /* u16 内核占用扇区数（MBR 回填） */
.set MEM_SIZE_SLOT, 0x7000              /* MBR 十六位阶段写入的 E801 结果 */
.set MEM_BLOCK_SLOT,0x7002
.set BOOTINFO_ADDR, 0x6000
.set KERNEL_DST,    0x00100000

.section .text.boot32, "ax"
.code32
.globl _start32
/* 串口打点（MBR 已初始化 COM1；此处只写数据口，无符号引用，迁移前后都能用） */
.macro DBG_CH ch
    movw $0x3F8, %dx
    movb $\ch, %al
    outb %al, %dx
.endm

_start32:
    /* ===== 迁移前阶段（禁止引用本镜像符号） ===== */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $MBR_BASE, %esp

    movl FIELD_SIZE, %ecx
    addl $3, %ecx                 /* 按 4 字节向上取整，确保尾部字节不被漏掉 */
    shrl $2, %ecx
    movl $0x00010000, %esi
    movl $KERNEL_DST, %edi
    rep movsl
    DBG_CH '2'

    .byte 0xEA                    /* 远跳到迁移后的副本（链接地址已可自由引用符号） */
    .long _boot32_relocated
    .word 0x0008

/* ===== 迁移后阶段（运行于 0x100000，可自由引用符号） ===== */
_boot32_relocated:
    DBG_CH '3'
    /* 装载内核自己的 GDT（含 64 位代码段描述符），并重新装载 CS */
    .byte 0x66
    lgdt kernel_gdt_desc
    .byte 0xEA
    .long _boot32_cs_reloaded
    .word 0x0008

_boot32_cs_reloaded:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $0x0007C000, %esp

    /* ---- boot_info 结构（0x6000） ---- */
    movl $0x48594E31, BOOTINFO_ADDR           /* magic 'HYN1' */
    movzwl MEM_SIZE_SLOT, %eax
    shll $10, %eax
    addl $0x00100000, %eax
    movzwl MEM_BLOCK_SLOT, %ebx
    shll $16, %ebx
    addl %ebx, %eax
    cmpl $0x01000000, %eax
    jae .Lmem_ok
    movl $0xFFFFFFFF, %eax                    /* 取不到有效容量：交内核兜底并标注 */
.Lmem_ok:
    movl %eax, BOOTINFO_ADDR + 4
    movl FIELD_SIZE, %eax
    movl %eax, BOOTINFO_ADDR + 8
    movzwl FIELD_SECTORS, %eax
    movl %eax, BOOTINFO_ADDR + 12
    movl $0, BOOTINFO_ADDR + 16               /* 引导方式 = 0：BIOS/MBR */
    movl $0, BOOTINFO_ADDR + 20               /* 保留字段 */

    /* ---- 页表区 0x70000..0x76000 清零 ---- */
    movl $0x70000, %edi
    movl $0x1800, %ecx                        /* 24KB / 4 = 6144 dword */
    xorl %eax, %eax
    rep stosl

    /* PML4[0] -> PDPT(0x71000)；PDPT[0..3] -> PD0..PD3 */
    movl $0x00071003, 0x70000
    xorl %ecx, %ecx
    movl $0x72000, %edx
.Lpdpt:
    movl %edx, %eax
    orl $0x03, %eax
    movl %eax, 0x71000(,%ecx,8)
    addl $0x1000, %edx
    incl %ecx
    cmpl $4, %ecx
    jb .Lpdpt

    /* 4 个 PD × 512 个 2MB 页 = 覆盖 0..4GB 恒等映射
     * 注意：PAE/长模式页表项一律 8 字节，故步长必须是 8；
     * 若误用 4 字节步长，PD[0] 会被读成 0x00200083000000a3（基址越出物理内存），
     * 开分页后的第一条取指即落到未映射区，表现为 Triple fault。 */
    xorl %ecx, %ecx
    movl $0x72000, %edi
    movl $0x00000083, %eax                    /* present | rw | PS(2MB) */
.Lpd:
    movl %eax, (%edi)
    addl $0x00200000, %eax
    addl $8, %edi
    incl %ecx
    cmpl $2048, %ecx
    jb .Lpd

    /* ---- 打开 PAE / LME / PG，进入长模式 ---- */
    DBG_CH '4'
    movl $0x70000, %eax
    movl %eax, %cr3
    movl %cr4, %eax
    orl $0x00000020, %eax                     /* CR4.PAE */
    movl %eax, %cr4
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x00000100, %eax                     /* EFER.LME */
    wrmsr
    movl %cr0, %eax
    orl $0x80000001, %eax                     /* CR0.PG | CR0.PE */
    movl %eax, %cr0
    DBG_CH '5'

    .byte 0xEA                                /* 远跳到 64 位入口 _start64 */
    .long _start64
    .word 0x0018

/* ------------------------------------------------------------------ */
/* 内核态 GDT（供保护模式重装 CS 与长模式使用）                        */
/* ------------------------------------------------------------------ */
.balign 8
kernel_gdt_desc:
    .word kernel_gdt_end - kernel_gdt - 1
    .long kernel_gdt
kernel_gdt:
    .quad 0x0000000000000000      /* 0x00 空描述符 */
    .quad 0x00CF9A000000FFFF      /* 0x08 32 位代码段（本阶段用） */
    .quad 0x00CF92000000FFFF      /* 0x10 数据段 */
    .quad 0x00AF9A000000FFFF      /* 0x18 64 位代码段 */
kernel_gdt_end: