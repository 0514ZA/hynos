/*
 * 混元 OS —— x86_64 线程上下文切换与进入用户态（arch 适配层）
 *
 * 切换现场只保存"被调用者保存寄存器 + 中断标志"：
 *   hyn_cpu_switch 是一个普通函数，调用者已按 ABI 保存好自己的易失寄存器，
 *   因此这里保存 r15/r14/r13/r12/rbx/rbp/rflags 就足够还原一条执行流的全部可见状态。
 *
 * 栈上保存顺序（低地址 -> 高地址）：r15 r14 r13 r12 rbx rbp rflags ret
 * 新线程的初始栈必须按完全相同的顺序布置（见 core/thread/sched.c: prepare_stack），
 * 否则首次切入即现场错位——两条实现必须严格对称。
 */
.section .text, "ax"
.code64

/*
 * void hyn_cpu_switch(u64 *save_rsp, u64 new_rsp)
 *   把当前栈指针存进 *save_rsp，切到 new_rsp 指向的现场继续执行。
 * 调用者负责在调用前后关/开中断（sched.c 用 arch_irq_save/restore 包夹）。
 */
.globl hyn_cpu_switch
.type hyn_cpu_switch, @function
hyn_cpu_switch:
    pushfq
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)            /* 保存旧现场 */
    movq %rsi, %rsp              /* 切到新现场 */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popfq
    ret                         /* 首次切入的线程：ret 落到 hyn_cpu_thread_entry */
.size hyn_cpu_switch, . - hyn_cpu_switch

/*
 * 新线程的着陆点：由上面那条 ret 进入，随后调 C 层入口跳板（cdecl 无参数）。
 */
.globl hyn_cpu_thread_entry
.type hyn_cpu_thread_entry, @function
hyn_cpu_thread_entry:
    call hyn_thread_trampoline
    /* 跳板以 hyn_thread_exit 收尾、不会返回；这里只做兜底，避免落进野地址 */
    hlt
    jmp hyn_cpu_thread_entry
.size hyn_cpu_thread_entry, . - hyn_cpu_thread_entry

/*
 * void hyn_arch_enter_user(u64 entry_rip, u64 user_rsp) —— iretq 进 Ring3，不返回。
 * 压入 iretq 需要的 5 项：SS RSP RFLAGS CS RIP。
 * CS/SS 用 GDT 里 RPL=3 的用户选择子（0x2B / 0x23），RFLAGS 里只留 IF=1。
 */
.globl hyn_arch_enter_user
.type hyn_arch_enter_user, @function
hyn_arch_enter_user:
    pushq $0x23                 /* SS  = 用户数据段（RPL3） */
    pushq %rsi                  /* RSP = 用户栈 */
    pushq $0x202                /* RFLAGS：IF=1 */
    pushq $0x2B                 /* CS  = 64 位用户代码段（RPL3） */
    pushq %rdi                  /* RIP = 用户入口 */
    iretq
.size hyn_arch_enter_user, . - hyn_arch_enter_user