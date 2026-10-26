/*
 * 混元 OS —— 用户程序链接脚本
 *
 * 关键约束：用户程序要被内核拷到"任意一个动态分配的用户页"上执行，
 * 所以链接基址取 0，且必须做到完全位置无关（build.sh 里的位置无关性硬校验会拒绝：
 * 残留重定位表、GOT、PLT，或已分配段不连续）。代码按 -fPIC 编译，
 * 所有跨段访问都是 RIP 相对，因此镜像整体平移后照旧可用。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x0;

    .text : {
        KEEP(*(.text.entry))        /* 入口桩必须排最前：镜像首字节即入口 */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }

    .data : { *(.data .data.*) }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /* 链接期就不需要的东西：用户程序没有动态链接、没有异常表、没有调试段 */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
        *(.debug_*)
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.hash)
        *(.hash)
    }
}