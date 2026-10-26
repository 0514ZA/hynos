/*
 * 混元智能分层混合内核 —— UEFI（PE32+）引导程序 BOOTX64.EFI
 *
 * 定位：UEFI 固件（OVMF 等）认到的第一个可执行体。它只负责把"内核负载"送进内存并
 * 交棒给内核镜像内的 UEFI 进入点 _start_uefi（arch/x86_64/boot/entry_uefi.S），
 * 由那里完成 GDT/页表切换。架构上与 MBR 引导扇区对等，但两者进入的是同一个内核镜像。
 *
 * 三个设计选择（都是为了把代码量和出错面压到最小）：
 *   1) 不读磁盘、不调用文件系统协议：内核负载以 .payload 节内嵌在本 PE 镜像里，
 *      随固件加载本文件时一并落进内存，此处只需 memcpy 到 0x100000。
 *      因此本程序依赖的固件接口只有三个：
 *        SystemTable->ConOut->OutputString   （日志）
 *        BootServices->GetMemoryMap          （拿内存映射 + MapKey）
 *        BootServices->ExitBootServices      （交还固件，之后不再使用任何固件服务）
 *   2) 全部代码与数据寻址只用 RIP 相对，故本 PE 无需任何重定位项（.reloc 为空表）；
 *      固件把它加载到任何地址都能正确运行。
 *   3) 调用约定按 UEFI x86_64（MS x64）手写：参数 RCX/RDX/R8/R9，第五参数在栈上
 *      [rsp+32]，调用点前保留 32 字节影子空间，且调用点 rsp 16 字节对齐。
 *
 * 构建期回填（由 tools/mkpe.py 写入，符号名固定）：
 *   g_kernel_size       : 内嵌内核负载字节数
 *   g_kernel_entry_off  : 内核镜像内 _start_uefi 相对 0x100000 的偏移
 */

/* ---- 固件结构体固定偏移（UEFI 2.x 规范） ---- */
.set ST_CONOUT,        0x40        /* EFI_SYSTEM_TABLE.ConOut */
.set ST_BOOTSERVICES,  0x60        /* EFI_SYSTEM_TABLE.BootServices */
.set CONOUT_OUTPUT,    0x08        /* EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.OutputString */
.set BS_GETMEMMAP,     0x38        /* EFI_BOOT_SERVICES.GetMemoryMap */
.set BS_EXITBOOTSERV,  0xE8        /* EFI_BOOT_SERVICES.ExitBootServices */

/* ---- efi_main 栈帧（0x88 字节：8+136 使调用点 16 字节对齐） ---- */
.set FRAME_SIZE,       0x88
.set SLOT_ARG5,        0x20        /* 第五参数槽（32 字节影子空间之后） */
.set SLOT_IMAGEHANDLE, 0x28
.set SLOT_SYSTABLE,    0x30
.set SLOT_CONOUT,      0x38
.set SLOT_BOOTSERV,    0x40
.set SLOT_MAPSIZE,     0x48        /* 以下四槽与 get_memmap 的槽位布局一致 */
.set SLOT_MAPKEY,      0x50
.set SLOT_DESCSIZE,    0x58
.set SLOT_DESCVER,     0x60

.set KERNEL_DST,       0x00100000
.set BOOTINFO_ADDR,    0x6000
.set MEMBUFSIZE,       0x10000     /* 64KB 内存映射缓冲 */
.set PAYLOAD_MIN,      0x1000      /* 负载合法下限 */
.set PAYLOAD_MAX,      0x40000     /* 负载合法上限，与 uefi.ld 预留量一致 */
.set HYN_MAGIC,        0x48594E31  /* 'HYN1'，与 include/hyn/bootinfo.h 一致 */
.set BOOT_METHOD_UEFI, 1

.section .text.uefi, "ax"
.code64
.globl efi_main
.type efi_main, @function
efi_main:
    cld                                  /* 方向标志清零：rep movsb 自低向高复制 */
    subq $FRAME_SIZE, %rsp

    movq %rcx, SLOT_IMAGEHANDLE(%rsp)    /* RCX = ImageHandle */
    movq %rdx, SLOT_SYSTABLE(%rsp)       /* RDX = SystemTable */
    movq ST_CONOUT(%rdx), %rax
    movq %rax, SLOT_CONOUT(%rsp)
    movq ST_BOOTSERVICES(%rdx), %rax
    movq %rax, SLOT_BOOTSERV(%rsp)

    /* ---- 日志：横幅 ---- */
    leaq s_banner(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8

    /* ---- 校验内嵌负载大小 ---- */
    movq g_kernel_size(%rip), %r12
    leaq s_payload(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    movq %r12, %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_nl(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8

    cmpq $PAYLOAD_MIN, %r12
    jb .Lfail_payload
    cmpq $PAYLOAD_MAX, %r12
    ja .Lfail_payload

    /* ---- 先把内核装载区（0x100000）与 boot_info 页（0x6000）用 AllocatePages 占下来 ----
     * 裸写未申请的内存是错的：若该区间压在固件自己的数据上，固件会在 ExitBootServices
     * 内部崩掉（现场只见固件地址上的 #UD）。分配必须在取内存映射之前做完——分配会改
     * MapKey，先取映射再分配会让 MapKey 失效。 */
    movq SLOT_BOOTSERV(%rsp), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call reserve_kernel_area
    testq %rax, %rax
    jz .Lresv_ok
    movq %rax, 0x70(%rsp)                /* 暂存失败状态码（print_* 会打掉调用者寄存器） */
    leaq s_err_alloc(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    movq 0x70(%rsp), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_nl(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    cli
.Lresv_hang:
    hlt
    jmp .Lresv_hang
.Lresv_ok:

    /* ---- 内核负载 -> 0x100000 ---- */
    leaq __kernel_payload_start(%rip), %rsi
    movq $KERNEL_DST, %rdi
    movq %r12, %rcx
    rep movsb

    leaq s_copied(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8

    /* ---- 交棒前的全部日志：必须放在「取最终内存映射」之前 ----
     * 【踩坑记录，勿改回去】ExitBootServices 校验的是「最近一次 GetMemoryMap 返回的
     * MapKey」。取映射与调用之间只要再做任何固件调用（控制台输出、固件定时事件、
     * 任何分配），内存映射就可能变化、MapKey 立即作废；固件随即返回
     * EFI_INVALID_PARAMETER = 0x8000000000000002（QEMU 实测 + 反汇编证实）。
     * 更糟的是：失败后重取映射再重试，会直接踩进固件自身的断言陷阱
     * （实测 #UD @RIP=0x0E700315，该处指令为 ud2）。
     * 因此最终序列必须紧凑：打印 → cli → 取映射 → 算内存 → 写 boot_info →
     * ExitBootServices(一次)，中间不得再有任何固件调用。
     */
    leaq s_ready(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    movq $KERNEL_DST, %rax
    addq g_kernel_entry_off(%rip), %rax
    movq %rax, %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_loader_addr(%rip), %rcx       /* 固件实际把本镜像装在哪儿（便于核对 RIP 归属） */
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    leaq efi_main(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_exitbs(%rip), %rcx            /* 即将交给固件的 ImageHandle（MapKey 待取，失败时再打印） */
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    movq SLOT_IMAGEHANDLE(%rsp), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_nl(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8

    /* ---- 关中断：冻结固件定时事件，确保刚取回的 MapKey 在调用前不再失效 ---- */
    cli

    /* ---- 取最终内存映射：rdi = 槽位指针，rsi = BootServices，rdx = 缓冲 ---- */
    leaq SLOT_MAPSIZE(%rsp), %rdi
    movq SLOT_BOOTSERV(%rsp), %rsi
    leaq g_memmap_buf(%rip), %rdx
    call get_memmap
    testq %rax, %rax
    js .Lfail_memmap

    /* ---- 由内存映射推算物理内存顶端 ----
     * 【踩坑记录】原先取 Type 2..11 的最大末地址，结果内核里"物理内存"打印成 0 字节：
     * 11 = EfiMemoryMappedIO 描述符覆盖 PCI/MMIO 空洞，其末地址正好是 4GiB 对齐的
     * 0x1_00000000，写进 u32 的 total_mem_bytes 后低 32 位恰好是 0。
     * 现在只认 1..10（可用 RAM 类），并显式跳过末地址超过 4GB 的描述符。
     */
    movq SLOT_MAPSIZE(%rsp), %rcx        /* 映射数据实际字节数 */
    movq SLOT_DESCSIZE(%rsp), %r14       /* 描述符步长（固件给，通常 40/48） */
    leaq g_memmap_buf(%rip), %r15
    xorl %r13d, %r13d                    /* max end（物理内存顶端） */
    testq %r14, %r14
    jz .Lmem_done
.Lmem_loop:
    testq %rcx, %rcx
    jz .Lmem_done
    movl (%r15), %eax                    /* Type */
    cmpl $1, %eax
    jb .Lmem_next                        /* 0 = Reserved：不是可用 RAM */
    cmpl $10, %eax
    ja .Lmem_next                        /* 11 = MMIO / 12 = IOPort / 13 = PAL / 14 = NVDIMM */
    movq 24(%r15), %rax                  /* NumberOfPages */
    shlq $12, %rax
    addq 8(%r15), %rax                   /* + PhysicalStart */
    movq %rax, %rdx
    shrq $32, %rdx
    jnz .Lmem_next                       /* 末地址超过 4GB（MMIO/PCI 窗口）不计入物理内存 */
    cmpq %r13, %rax
    jbe .Lmem_next
    movq %rax, %r13
.Lmem_next:
    addq %r14, %r15
    subq %r14, %rcx
    jmp .Lmem_loop
.Lmem_done:

    /* ---- boot_info @ 0x6000（内核按此结构认领引导来源） ---- */
    movl $HYN_MAGIC, BOOTINFO_ADDR
    movl %r13d, BOOTINFO_ADDR + 4        /* total_mem_bytes */
    movl %r12d, BOOTINFO_ADDR + 8        /* kernel_bytes */
    movl $0, BOOTINFO_ADDR + 12          /* kernel_sectors：UEFI 路径无扇区概念 */
    movl $BOOT_METHOD_UEFI, BOOTINFO_ADDR + 16

    /* ---- ExitBootServices：一次到位 ----
     * 规范要求两个参数：RCX = ImageHandle，RDX = MapKey。此调用与上方 GetMemoryMap
     * 之间不得插入任何固件调用；失败即打印状态码与所用 MapKey 后停机，不再重试
     * （重试会踩进固件断言陷阱，见上方踩坑记录）。交还固件后其代码页可能被回收，
     * 因此全程保持中断关闭。
     */
    movq SLOT_IMAGEHANDLE(%rsp), %rcx
    movq SLOT_MAPKEY(%rsp), %rdx
    movq SLOT_BOOTSERV(%rsp), %rax
    call *BS_EXITBOOTSERV(%rax)
    testq %rax, %rax
    jns .Lexit_ok
    movq %rax, 0x70(%rsp)                /* 先存状态码：后续 print 会打掉 scratch 寄存器 */
    leaq s_err_exit(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    movq 0x70(%rsp), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_mapkey(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    movq SLOT_MAPKEY(%rsp), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_hex
    leaq s_nl(%rip), %rcx
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
    jmp .Lhang

.Lexit_ok:
    /* 固件服务已交还：此后只做纯 CPU 动作。关中断后跳入内核 UEFI 进入点。 */
    cli
    movq $KERNEL_DST, %rax
    addq g_kernel_entry_off(%rip), %rax
    jmp *%rax

/* ---- 失败路径：能打日志就打，然后停机 ---- */
.Lfail_payload:
    leaq s_err_payload(%rip), %rcx
    jmp .Lfail_log
.Lfail_memmap:
    leaq s_err_memmap(%rip), %rcx
.Lfail_log:
    movq SLOT_CONOUT(%rsp), %rdx
    call print_utf8
.Lhang:
    cli
    hlt
    jmp .Lhang
.size efi_main, . - efi_main

/* ------------------------------------------------------------------ */
/* get_memmap：取内存映射，内部最多重试 4 次                            */
/*   rdi = 槽位 {u64 map_size, u64 map_key, u64 desc_size, u64 desc_ver} */
/*   rsi = BootServices，rdx = 64KB 缓冲                                */
/*   返回 rax = 0 成功 / -1 失败                                        */
/* ------------------------------------------------------------------ */
.type get_memmap, @function
get_memmap:
    subq $0x48, %rsp                     /* 32 影子 + 8 第五参数 + 临时槽 */
    movq %rdi, 0x28(%rsp)
    movq %rsi, 0x30(%rsp)
    movq %rdx, 0x38(%rsp)
    movq $4, 0x40(%rsp)
.Lgm_retry:
    movq 0x28(%rsp), %r10
    movq $MEMBUFSIZE, %rax
    movq %rax, 0(%r10)                   /* map_size = 缓冲容量（固件会回填实际需要） */
    movq %r10, %rcx
    movq 0x38(%rsp), %rdx
    leaq 8(%r10), %r8
    leaq 16(%r10), %r9
    leaq 24(%r10), %rax
    movq %rax, 0x20(%rsp)                /* 第五参数：&DescriptorVersion */
    movq 0x30(%rsp), %rax
    call *BS_GETMEMMAP(%rax)
    testq %rax, %rax
    jns .Lgm_ok
    movq 0x28(%rsp), %r10
    movq 0(%r10), %rax
    cmpq $MEMBUFSIZE, %rax
    ja .Lgm_fail                         /* 需要超过 64KB：不再重试 */
    decq 0x40(%rsp)
    jnz .Lgm_retry
.Lgm_fail:
    movq $-1, %rax
    addq $0x48, %rsp
    ret
.Lgm_ok:
    xorl %eax, %eax
    addq $0x48, %rsp
    ret
.size get_memmap, . - get_memmap

/* ------------------------------------------------------------------ */
/* print_utf8：把以 0 结尾的 UTF-8 串转成 UTF-16 后交给固件控制台        */
/*   rcx = UTF-8 串，rdx = ConOut；支持 1/2/3 字节序列（覆盖 BMP）       */
/* ------------------------------------------------------------------ */
.type print_utf8, @function
print_utf8:
    subq $0x128, %rsp                    /* 0x20 起 128 个 UTF-16 码元 + 影子空间 */
    movq %rdx, %r10
    xorl %r9d, %r9d
.Lu_loop:
    movzbl (%rcx), %eax
    testb %al, %al
    jz .Lu_done
    incq %rcx
    cmpb $0x80, %al
    jb .Lu_emit                          /* 1 字节 */
    cmpb $0xE0, %al
    jb .Lu_2                             /* 2 字节 */
    /* 3 字节 */
    movl %eax, %r11d
    andl $0x0F, %r11d
    shll $12, %r11d
    movzbl (%rcx), %eax
    andl $0x3F, %eax
    shll $6, %eax
    orl %eax, %r11d
    movzbl 1(%rcx), %eax
    andl $0x3F, %eax
    orl %eax, %r11d
    addq $2, %rcx
    movl %r11d, %eax
    jmp .Lu_emit
.Lu_2:
    movl %eax, %r11d
    andl $0x1F, %r11d
    shll $6, %r11d
    movzbl (%rcx), %eax
    andl $0x3F, %eax
    orl %eax, %r11d
    incq %rcx
    movl %r11d, %eax
.Lu_emit:
    movw %ax, 0x20(%rsp,%r9,2)
    incq %r9
    cmpq $127, %r9
    jb .Lu_loop
.Lu_done:
    movw $0x0000, 0x20(%rsp,%r9,2)
    movq %r10, %rcx
    leaq 0x20(%rsp), %rdx
    call *CONOUT_OUTPUT(%rcx)
    addq $0x128, %rsp
    ret
.size print_utf8, . - print_utf8

/* ------------------------------------------------------------------ */
/* print_hex：输出 "0x" + 16 位十六进制（便于日志中核对偏移/长度）       */
/*   rcx = 值，rdx = ConOut                                             */
/* ------------------------------------------------------------------ */
.type print_hex, @function
print_hex:
    subq $0x48, %rsp
    movq %rdx, %r10
    movq %rcx, %r11
    movw $0x0030, 0x20(%rsp)             /* '0' */
    movw $0x0078, 0x22(%rsp)             /* 'x' */
    xorl %r9d, %r9d
.Lh_loop:
    movq %r11, %rax
    shrq $60, %rax
    andl $0x0F, %eax
    cmpl $10, %eax
    jb .Lh_digit
    addl $0x27, %eax                     /* 10..15 -> 'a'..'f' */
.Lh_digit:
    addl $0x30, %eax
    movw %ax, 0x24(%rsp,%r9,2)
    shlq $4, %r11
    incl %r9d
    cmpl $16, %r9d
    jb .Lh_loop
    movw $0x0000, 0x44(%rsp)
    movq %r10, %rcx
    leaq 0x20(%rsp), %rdx
    call *CONOUT_OUTPUT(%rcx)
    addq $0x48, %rsp
    ret
.size print_hex, . - print_hex

/* ------------------------------------------------------------------ */
/* reserve_kernel_area：用 AllocatePages 占住内核装载区与 boot_info 页
 *   rcx = BootServices 表指针，rdx = ConOut（本函数不用，仅保持调用形态一致）
 *   返回 rax = 0 成功；非 0 失败（负数 = 固件状态码；-1 = 固件没按期望地址分配）
 *
 *   AllocatePages(Type, MemoryType, Pages, Memory*)
 *     Type       = 2  AllocateAddress（必须给我指定的这个物理地址）
 *     MemoryType = 2  EfiLoaderData
 *     Pages      = ceil(字节数 / 4096)
 *     Memory*    -> 入参即期望地址，出参由固件回填实际分配到的地址
 *   参数位置 RCX/RDX/R8/R9（MS x64），地址暂存在 [rsp+0x30]（影子空间之上）。
 *
 *   为什么必须先占再写：内核装载区 0x100000 与 boot_info 页 0x6000 都不是"我们的"
 *   内存。裸写若压在固件自己的数据上，固件会在 ExitBootServices 内部崩掉——实测
 *   表现为固件地址上的 #UD，且日志里看不到本程序自己的任何一行报错。
 * ------------------------------------------------------------------ */
.section .text.uefi, "ax"
.code64
/* 0x28 = EFI_BOOT_SERVICES.AllocatePages（同表 0x38 = GetMemoryMap 已实测可用，
   故该表字段枚举可信：0x28 AllocatePages / 0x30 FreePages / 0xE8 ExitBootServices） */
.set BS_ALLOCPAGES, 0x28
.type reserve_kernel_area, @function
reserve_kernel_area:
    subq $0x48, %rsp
    movq %rcx, 0x38(%rsp)                /* BootServices：固件调用会打掉 r10/r11，必须落栈 */
    movq %rdx, 0x40(%rsp)

    /* 内核装载区：0x100000 起，ceil(kernel_bytes / 4096) 页 */
    movq g_kernel_size(%rip), %r8
    addq $0xFFF, %r8
    shrq $12, %r8
    movq $KERNEL_DST, %rax
    movq %rax, 0x30(%rsp)
    leaq 0x30(%rsp), %r9
    movl $2, %ecx                        /* AllocateAddress */
    movl $2, %edx                        /* EfiLoaderData */
    movq 0x38(%rsp), %rax
    call *BS_ALLOCPAGES(%rax)
    testq %rax, %rax
    js .Lrk_fail
    movq 0x30(%rsp), %rax
    cmpq $KERNEL_DST, %rax
    jne .Lrk_wrong

    /* boot_info 页：0x6000 一页 */
    movq $BOOTINFO_ADDR, %rax
    movq %rax, 0x30(%rsp)
    leaq 0x30(%rsp), %r9
    movl $1, %r8d
    movl $2, %ecx
    movl $2, %edx
    movq 0x38(%rsp), %rax
    call *BS_ALLOCPAGES(%rax)
    testq %rax, %rax
    js .Lrk_fail
    movq 0x30(%rsp), %rax
    cmpq $BOOTINFO_ADDR, %rax
    jne .Lrk_wrong

    xorl %eax, %eax                      /* 0 = 成功 */
    addq $0x48, %rsp
    ret

.Lrk_fail:
    addq $0x48, %rsp
    ret                                  /* rax = 固件状态码 */
.Lrk_wrong:
    movq $-1, %rax
    addq $0x48, %rsp
    ret
.size reserve_kernel_area, . - reserve_kernel_area

/* ------------------------------------------------------------------ */
/* 只读数据（固件控制台只认 UTF-16，这里用 UTF-8 源串，运行时转换）      */
/* ------------------------------------------------------------------ */
.section .rodata, "a"
s_banner:     .asciz "混元 OS UEFI 引导程序 BOOTX64.EFI  [GPT + ESP 引导路径]\r\n"
s_payload:    .asciz "内嵌内核负载 : "
s_copied:     .asciz "内核负载已复制到 0x100000，开始取内存映射\r\n"
s_ready:      .asciz "即将取最终内存映射、交还固件，并跳入内核进入点 : "
s_loader_addr:.asciz " 本镜像装载地址 : "
s_exitbs:     .asciz "\r\n即将调用 ExitBootServices：ImageHandle="
s_mapkey:     .asciz "  MapKey="
s_nl:         .asciz "\r\n"
s_err_payload:.asciz "错误：内嵌内核负载大小越界，停机。\r\n"
s_err_memmap: .asciz "错误：GetMemoryMap 失败（缓冲 64KB 仍不足），停机。\r\n"
s_err_alloc:  .asciz "错误：AllocatePages 占不住内核装载区 0x100000 或 boot_info 页 0x6000，固件状态码 : "
s_err_exit:   .asciz "错误：ExitBootServices 返回非零状态（通常是 MapKey 已失效），状态码 : "

/* ------------------------------------------------------------------ */
/* 构建期回填字段（tools/mkpe.py 按符号名查地址后写入）                 */
/* ------------------------------------------------------------------ */
.section .data, "aw"
.balign 8
.globl g_kernel_size
g_kernel_size:        .quad 0
.globl g_kernel_entry_off
g_kernel_entry_off:   .quad 0

/* ------------------------------------------------------------------ */
/* 运行期缓冲（不占镜像体积，固件按 SizeOfImage 分配并清零）             */
/* ------------------------------------------------------------------ */
.section .bss, "aw", @nobits
.balign 16
g_memmap_buf:
    .skip MEMBUFSIZE