/*
 * 混元 OS —— Ring3 用户程序入口桩（libhyn 运行时的一部分）
 *
 * 装载约定（与内核 include/hyn/userprog.h 一致）：内核把入口 RIP 指向本桩，并把用户栈顶
 * 设成"参数块起点"，栈顶按 hyn_abi_user_args_t 布局依次放着
 *   +0x00 target_port / +0x08 record_va / +0x10 self_port / +0x18 arena_va
 *
 * 因此这里采用"栈顶即参数块"的约定：把进入时的 rsp 直接当参数指针交给 C 代码，
 * 之后再向下腾出调用帧（参数块在帧上方，不会被写坏）。
 *
 * ABI 细节：
 *   · 入口处 rsp 由内核设为 16 字节对齐，本桩对齐后再 call —— 被调方看到的栈满足
 *     SysV 约定（call 压返回地址后 rsp % 16 == 8）；
 *   · 用户程序按 -mno-red-zone 编译，下方 1KiB 帧不会越过内核给的那一页栈。
 */
.section .text.entry, "ax"
.globl _start
.globl hyn_start
.type _start, @function
.type hyn_start, @function

_start:
hyn_start:
    movq  %rsp, %rdi            /* 参数块在栈顶：rdi = hyn_abi_user_args_t* */
    subq  $0x400, %rsp          /* 向下腾出 1KiB 调用帧 */
    andq  $-16, %rsp            /* 调用点 16 字节对齐（SysV） */
    xorl  %ebp, %ebp            /* 剥掉帧指针链，避免依赖内核留下的旧值 */
    call  hyn_user_main

    /* 正常不会走到这里：hyn_user_main 收尾会调 hyn_exit。
     * 走回来了也按"退出码 0"走同一扇门，不私自造第二条退出路径。 */
    xorl  %edi, %edi
    movl  $0x0020, %eax         /* HYN_ABI_SYS_EXIT */
    int   $0x80
1:
    jmp   1b
.size _start, . - _start