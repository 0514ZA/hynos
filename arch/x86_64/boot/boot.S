/*
 * HynOS 智能分层混合内核 —— 自研引导扇区（MBR，16 位实模式部分）
 *
 * 体积纪律：引导活动部分必须塞进 512 字节，且不得覆盖 0x1BE 起的 MBR 分区表。
 * 回填字段占 0x1B0..0x1B7，故代码与数据的可用空间为 432 字节；因此这里只做"最少的事"：
 *   1) 打开 A20
 *   2) int 15h/AX=E801 取物理内存容量，暂存到 0x7000
 *   3) EDD(int 13h/AH=42h) 把内核镜像读到 0x10000
 *   4) 装载临时 GDT、打开 CR0.PE，远跳到内核镜像首字节（32 位入口 0x10000）
 * 后续工作（迁移到 0x100000、写 boot_info、PML4 页表、进入长模式）全部由
 * 内核镜像内的 32 位阶段完成 —— 那里不再受 512 字节限制。
 *
 * 回填字段（由 tools/mkgpt.py 写入，本文件只按固定物理地址读取）：
 *   0x7C00 + 0x1B0 : u16 内核占用扇区数
 *   0x7C00 + 0x1B4 : u32 内核字节数（由内核 32 位阶段读取）
 *
 * 磁盘布局（同一镜像同时支持 BIOS 与 UEFI 两条路径）：
 *   LBA 0        本 MBR（前 432 字节代码 + 回填字段 + 保护性分区项 + 0x55AA）
 *   LBA 1..33    GPT 主头与分区项数组（本路径不使用，也不破坏）
 *   LBA 34 起    内核镜像裸区（起始 LBA 写死在下方 mbr_lba 初值里）
 */

.set MBR_BASE,        0x7C00
.set FIELD_SECTORS,   0x7C00 + 0x1B0
.set MEM_SIZE_SLOT,   0x7000            /* E801: 1MB..16MB 的 KB 数 */
.set MEM_BLOCK_SLOT,  0x7002            /* E801: 16MB 以上的 64KB 块数 */
.set KERNEL_ENTRY32,  0x00010000        /* 内核镜像首字节：32 位入口 */
.set KERNEL_SEG,      0x1000            /* 内核装载段：0x1000 -> 0x10000 */
.set KERNEL_LBA,      34                /* 内核裸区起始 LBA（避开 GPT 主头与分区项数组） */

.code16
.section .boot, "ax"
.globl _mbr_start
_mbr_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $MBR_BASE, %sp
    movw %ax, %fs
    movw %ax, %gs
    movb %dl, mbr_drive
    call mbr_serial_init
    movb $'M', %al
    call mbr_putc
    movb mbr_drive, %al
    call mbr_hex8

    /* ---- 打开 A20（Fast A20） ---- */
    inb $0x92, %al
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
    movb $'A', %al
    call mbr_putc

    /* ---- 物理内存容量：int 15h / AX=E801 ---- */
    movw $0xE801, %ax
    int $0x15
    jc .Lmem_done
    movw %ax, MEM_SIZE_SLOT
    movw %bx, MEM_BLOCK_SLOT
.Lmem_done:

    /* ---- EDD 读取内核镜像到 0x10000 ---- */
    movw FIELD_SECTORS, %ax
    movw %ax, mbr_remain
.Lread_loop:
    movw mbr_remain, %ax
    testw %ax, %ax
    jz .Lload_done
    cmpw $32, %ax
    jbe .Lchunk_ok
    movw $32, %ax
.Lchunk_ok:
    movw %ax, mbr_chunk
    movw %ax, dap_count
    movw mbr_dstseg, %ax
    movw %ax, dap_segment
    movl mbr_lba, %eax
    movl %eax, dap_lba
    movl mbr_lba+4, %eax
    movl %eax, dap_lba+4

    movb $'D', %al
    call mbr_putc
    movw $dap, %si
    movb mbr_drive, %dl
    movb $0x42, %ah
    int $0x13
    jc .Lread_err

    /* remain -= chunk; lba += chunk; dstseg += chunk * 32 段 */
    movw mbr_chunk, %ax
    movzwl %ax, %eax
    addl %eax, mbr_lba
    movw mbr_chunk, %ax
    shlw $5, %ax
    addw %ax, mbr_dstseg
    movw mbr_chunk, %ax
    subw %ax, mbr_remain
    jmp .Lread_loop

.Lread_err:
    movb %ah, mbr_err
    movb %dl, mbr_drv_seen
    movb $'R', %al
    call mbr_putc
    movb mbr_err, %al
    call mbr_hex8
    movb mbr_drv_seen, %al
    call mbr_hex8
    movb mbr_remain+1, %al
    call mbr_hex8
    movb mbr_remain, %al
    call mbr_hex8
.Lread_hang:
    cli
    hlt
    jmp .Lread_hang

/* ---- COM1(0x3F8) 串口输出：115200 8N1，轮询 THRE ---- */
mbr_serial_init:
    movw $0x3FB, %dx
    movb $0x83, %al
    outb %al, %dx
    movw $0x3F8, %dx
    movb $0x01, %al
    outb %al, %dx
    movw $0x3F9, %dx
    xorw %ax, %ax
    outb %al, %dx
    movb $0x03, %al
    movw $0x3FB, %dx
    outb %al, %dx
    ret

mbr_putc:                       /* AL = 字符 */
    movb %al, %bh
    movw $0x3FD, %dx
9:  inb %dx, %al
    testb $0x20, %al
    jz 9b
    movw $0x3F8, %dx
    movb %bh, %al
    outb %al, %dx
    ret

mbr_hex8:                       /* AL -> 两个十六进制字符 */
    movb %al, %bl
    shrb $4, %al
    call mbr_hex4
    movb %bl, %al
    andb $0x0F, %al
mbr_hex4:
    addb $'0', %al
    cmpb $'9', %al
    jbe 8f
    addb $7, %al
8:  call mbr_putc
    ret

.Lload_done:
    movb $'G', %al
    call mbr_putc
    /* ---- 装载临时 GDT，进入 32 位保护模式 ---- */
    .byte 0x66                 /* LGDT 使用 32 位操作数（limit16 + base32） */
    lgdt gdt_desc
    movl %cr0, %eax
    orl $0x00000001, %eax
    movl %eax, %cr0
    .byte 0x66                 /* 远跳 ptr16:32：32 位入口 + 32 位代码段 0x08 */
    .byte 0xEA
    .long KERNEL_ENTRY32
    .word 0x0008

/* ------------------------------------------------------------------ */
/* 引导期数据                                                          */
/* ------------------------------------------------------------------ */
.balign 2
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
gdt:
    .quad 0x0000000000000000      /* 0x00 空描述符 */
    .quad 0x00CF9A000000FFFF      /* 0x08 32 位代码（进入保护模式用） */
    .quad 0x00CF92000000FFFF      /* 0x10 数据段 */
gdt_end:

/* 磁盘地址包（EDD，int 13h/AH=42h）——严格 16 字节，布局无 reserved 字段：
 *   0x00 u16 包大小(=0x10)  0x02 u16 扇区数(≤127)  0x04 u16 缓冲区偏移
 *   0x06 u16 缓冲区段      0x08 u64 起始 LBA
 * 多写一个 reserved 字段会让 BIOS 把扇区数读成 0，从而回 AH=01h 拒绝请求。 */
.balign 2
dap:
dap_size:       .word 0x0010
dap_count:      .word 0x0000
dap_offset:     .word 0x0000
dap_segment:    .word 0x0000
dap_lba:        .long 0
                .long 0

/* 循环状态：初值直接写在镜像里，省下运行时赋值指令 */
mbr_drive:       .byte 0
mbr_err:         .byte 0
mbr_drv_seen:    .byte 0
mbr_chunk:       .word 0
mbr_remain:      .word 0
mbr_dstseg:      .word KERNEL_SEG
mbr_lba:         .long KERNEL_LBA      /* 初值直接写在镜像里，省下运行时赋值指令 */
                 .long 0