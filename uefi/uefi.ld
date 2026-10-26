/*
 * 混元 OS —— UEFI 引导程序链接脚本（ELF 阶段；随后由 tools/mkpe.py 转成 PE32+）
 *
 * 与内核链接脚本的差别：
 *   · 装载地址 0x10000000 只用于对齐 PE 的 RVA；代码全部 RIP 相对寻址，
 *     固件把镜像放到任意地址都能跑（PE 的 .reloc 为空表，见 mkpe.py）
 *   · .data 末尾预留 256KB，作为"内嵌内核负载区"，由 mkpe.py 把内核镜像字节写进去
 */
ENTRY(efi_main)

SECTIONS
{
    . = 0x10000000;

    .text : {
        *(.text.uefi)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
        . = ALIGN(16);
        __kernel_payload_start = .;
        . += 0x40000;                       /* 256KB 内嵌负载预留 */
        __kernel_payload_end = .;
    }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __uloader_bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __uloader_bss_end = .;
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
        *(.debug_*)
        *(.interp)
        *(.dynamic)
    }
}