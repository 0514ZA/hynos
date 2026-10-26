/*
 * HynOS —— 引导扇区（MBR）链接脚本
 *
 * 整个引导代码 + 数据必须落在 512 字节内，并被 BIOS 加载到 0x7C00。
 * 通过 objcopy -O binary 得到纯二进制，首字节对应 0x7C00。
 */
ENTRY(_mbr_start)

SECTIONS
{
    . = 0x7C00;

    .boot : {
        KEEP(*(.boot))
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
        *(.debug_*)
    }
}