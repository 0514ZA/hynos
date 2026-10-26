/*
 * 混元 OS —— 内核镜像链接脚本（x86_64）
 *
 * 布局约定：
 *   .boot.text  必须排在最前，保证镜像第一个字节就是 64 位入口（自研引导直接跳 0x100000）
 *   .text/.rodata/.data 常规段
 *   .bss        由内核早期自行清零（__bss_start/__bss_end 供 C 使用）
 *   .stack      引导栈（NOLOAD，不占镜像体积，且排除在 .bss 清零范围之外）
 */
ENTRY(_start64)

SECTIONS
{
    . = 0x100000;

    /* 镜像首字节必须是 32 位引导阶段（MBR 远跳到 0x10000 进入） */
    .boot32 : {
        KEEP(*(.text.boot32))
    }

    .boot.text : {
        KEEP(*(.text.boot))
    }

    /* UEFI 路径的内核进入点：BOOTX64.EFI 跳入，其偏移由 tools/mkpe.py 回填 */
    .boot.uefi : {
        KEEP(*(.text.bootuefi))
    }

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    .stack (NOLOAD) : {
        . = ALIGN(4096);
        _boot_stack_bottom = .;
        . += 0x10000;                 /* 64KB 内核引导栈 */
        . = ALIGN(16);
        _boot_stack_top = .;
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
        *(.debug_*)
        *(.gcc_except_table*)
        *(.interp)
        *(.dynamic)
    }
}