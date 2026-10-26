/*
 * HynOS —— 中断/异常入口桩（x86_64）
 *
 * 每个桩在栈上压入 [错误码][向量号]，随后统一进入 isr_common：
 *   isr_common 保存通用寄存器、调用 C 分派器、恢复现场并 iretq。
 *
 * 栈帧内存布局（低 -> 高），必须与 include/hyn/trap.h 的 hyn_trap_frame_t 一致：
 *   rax rbx rcx rdx rsi rdi rbp r8..r15 | vector | error_code | rip cs rflags [rsp ss]
 */
.section .text, "ax"
.code64

.macro ISR_NOERR vec
    .globl isr_\vec
    .type isr_\vec, @function
isr_\vec:
    pushq $0
    pushq $\vec
    jmp isr_common
    .size isr_\vec, . - isr_\vec
.endm

.macro ISR_ERR vec
    .globl isr_\vec
    .type isr_\vec, @function
isr_\vec:
    pushq $\vec
    jmp isr_common
    .size isr_\vec, . - isr_\vec
.endm

ISR_NOERR 0      /* #DE 除零 */
ISR_NOERR 1      /* #DB 调试 */
ISR_NOERR 2      /* NMI */
ISR_NOERR 3      /* #BP 断点 */
ISR_NOERR 4      /* #OF 溢出 */
ISR_NOERR 5      /* #BR 越界 */
ISR_NOERR 6      /* #UD 非法指令 */
ISR_NOERR 7      /* #NM 设备不可用 */
ISR_ERR   8      /* #DF 双重错误 */
ISR_NOERR 9
ISR_ERR   10     /* #TS 无效 TSS */
ISR_ERR   11     /* #NP 段不存在 */
ISR_ERR   12     /* #SS 栈段错误 */
ISR_ERR   13     /* #GP 通用保护 */
ISR_ERR   14     /* #PF 页错误 */
ISR_NOERR 15
ISR_NOERR 16     /* #MF 浮点错误 */
ISR_ERR   17     /* #AC 对齐检查 */
ISR_NOERR 18     /* #MC 机器检查 */
ISR_NOERR 19     /* #XM SIMD 浮点 */
ISR_NOERR 20     /* #VE 虚拟化 */
ISR_ERR   21     /* #CP 控制保护 */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28     /* #HV */
ISR_ERR   29     /* #VC */
ISR_ERR   30     /* #SX */
ISR_NOERR 31
ISR_NOERR 32     /* IRQ0  时钟 */
ISR_NOERR 33     /* IRQ1  键盘 */
ISR_NOERR 34     /* IRQ2  级联 */
ISR_NOERR 35     /* IRQ3  串口2 */
ISR_NOERR 36     /* IRQ4  串口1 */
ISR_NOERR 37     /* IRQ5 */
ISR_NOERR 38     /* IRQ6  软驱 */
ISR_NOERR 39     /* IRQ7  并口 */
ISR_NOERR 40     /* IRQ8  RTC */
ISR_NOERR 41     /* IRQ9 */
ISR_NOERR 42     /* IRQ10 */
ISR_NOERR 43     /* IRQ11 */
ISR_NOERR 44     /* IRQ12 鼠标 */
ISR_NOERR 45     /* IRQ13 FPU */
ISR_NOERR 46     /* IRQ14 主 IDE */
ISR_NOERR 47     /* IRQ15 次 IDE */

/*
 * syscall 入口桩：向量 0x80。它与 0..47 的桩共用 isr_common，
 * 因此 Ring3 进来时同样会把完整陷阱帧交给 C 层（rax=调用号、rdi/rsi/rdx=参数）。
 */
ISR_NOERR 128

/* 未定义向量（48..255）通用桩：向量号填哨兵，由 C 层如实报告 */
.globl hyn_isr_default
.type hyn_isr_default, @function
hyn_isr_default:
    pushq $0
    pushq $-1
    jmp isr_common
.size hyn_isr_default, . - hyn_isr_default

.globl isr_common
.type isr_common, @function
isr_common:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    cld
    movq %rsp, %rdi
    call hyn_trap_dispatch

    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    addq $16, %rsp          /* 丢弃 vector 与 error_code */
    iretq

/* 向量 -> 桩地址 映射表（供 IDT 装载） */
.section .rodata
.globl hyn_isr_stub_table
hyn_isr_stub_table:
    .quad isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    .quad isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    .quad isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    .quad isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    .quad isr_32, isr_33, isr_34, isr_35, isr_36, isr_37, isr_38, isr_39
    .quad isr_40, isr_41, isr_42, isr_43, isr_44, isr_45, isr_46, isr_47