/*
 * HynOS —— 受控探测桩（自检专用，x86_64）
 *
 * 这些函数故意触发异常，用于验证"异常被捕获且可恢复"这一能力。
 */
.section .text, "ax"
.code64

/* u64 hyn_trap_probe_breakpoint(void)：触发 #BP，恢复后返回 1 */
.globl hyn_trap_probe_breakpoint
.type hyn_trap_probe_breakpoint, @function
hyn_trap_probe_breakpoint:
    xorl %eax, %eax
    int3                    /* #BP：CPU 保存的 RIP 已指向下一条指令，无需修正 */
    movl $1, %eax
    ret
.size hyn_trap_probe_breakpoint, . - hyn_trap_probe_breakpoint

/* void hyn_trap_probe_pagefault(u64 bad_addr)：读取未映射地址触发 #PF */
.globl hyn_trap_probe_pagefault
.type hyn_trap_probe_pagefault, @function
hyn_trap_probe_pagefault:
    movq %rdi, %rax
    movq (%rax), %rcx       /* #PF 在此处触发 */
.globl hyn_trap_probe_pagefault_after
hyn_trap_probe_pagefault_after:
    ret
.size hyn_trap_probe_pagefault, . - hyn_trap_probe_pagefault

/* u64 hyn_trap_probe_gpf(void)：装载非法段选择子触发 #GP */
.globl hyn_trap_probe_gpf
.type hyn_trap_probe_gpf, @function
hyn_trap_probe_gpf:
    xorl %eax, %eax
    movw $0x0000, %ax
.globl hyn_trap_probe_gpf_after
hyn_trap_probe_gpf_after:
    ret
.size hyn_trap_probe_gpf, . - hyn_trap_probe_gpf