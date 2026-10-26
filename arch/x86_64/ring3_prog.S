/*
 * HynOS —— Ring3 自检探针（x86_64，用户态代码）
 *
 * 这两段代码不是内核代码：内核把它们拷进 U=1 的用户页，再用 iretq 送进 Ring3 执行。
 * 它们只依赖三样东西：syscall 入口（int 0x80）、自己的用户栈、自己的记录页。
 *
 * 用户栈上的参数块由内核布置（见 core/kmain.c: ring3_prepare）：
 *   0(%rsp)  = 服务端口 id（请求投递目标）
 *   8(%rsp)  = 记录页基址（本程序写、内核读）
 *   16(%rsp) = 自身身份端口 id（收取应答用）
 *
 * 记录字段偏移与 include/hyn/ring3.h 的 hyn_ring3_record_t 一致（C 侧有静态断言把关）。
 */
.section .text.userprog, "ax"
.code64

.equ REC_MAGIC,    0x00
.equ REC_TID,      0x08
.equ REC_CPL_ARG,  0x10
.equ REC_CPL_RET,  0x18
.equ REC_BADPTR,   0x20
.equ REC_SEND,     0x28
.equ REC_FOREIGN,  0x30
.equ REC_RECV,     0x38
.equ REC_RTYPE,    0x40
.equ REC_RMSGID,   0x48
.equ REC_RDOMAIN,  0x50
.equ REC_RLEN,     0x58
.equ REC_RWORD,    0x60
.equ REC_DONE,     0x68
.equ REC_MSG,      0x100          /* 消息缓冲区在记录页内的偏移 */
/* 消息体字段偏移（与 hyn_alpc_msg_t 一致） */
.equ MSG_MAGIC,    0x00
.equ MSG_ID,       0x04
.equ MSG_TYPE,     0x08
.equ MSG_FLAGS,    0x0C
.equ MSG_SENDER,   0x10
.equ MSG_SDOM,     0x14
.equ MSG_TDOM,     0x15
.equ MSG_LEN,      0x18
.equ MSG_REPLY,    0x20
.equ MSG_DATA,     0x28

/* ---------- 探针甲：正常流程 ---------- */
.globl __ring3_probe_good_start
__ring3_probe_good_start:
    movq (%rsp), %r12                /* 目标端口 id */
    movq 8(%rsp), %r13               /* 记录页 */
    movq 16(%rsp), %r15              /* 自身端口 id */
    movq $0x334E4952, REC_MAGIC(%r13)    /* "RIN3"：内核由此判定"用户代码确实跑起来了" */

    /* ① 自省：把自己读到的 %cs 交内核核对（自己说的不算，内核手里的帧才算） */
    xorl %edi, %edi
    movw %cs, %di
    movq %rdi, REC_CPL_ARG(%r13)
    movl $3, %eax                    /* SYS_CHECK_CPL */
    int $0x80
    movq %rax, REC_CPL_RET(%r13)

    /* ② SYS_GET_TID：状态在 rax，线程 id 在 rdx */
    movl $2, %eax
    int $0x80
    movq %rdx, REC_TID(%r13)

    /* ③ 负向：把内核地址当消息缓冲区递下去，应被拒（EFAULT=7，且内核不得解引用它） */
    movl $0x10, %eax                 /* SYS_ALPC_SEND */
    movq %r12, %rdi
    movq $0x100000, %rsi             /* 内核恒等映射地址：不属于用户窗口 */
    int $0x80
    movq %rax, REC_BADPTR(%r13)

    /* ④ 拼请求：故意伪造发送者身份，内核应覆盖成绑定端口的身份 */
    movq %r13, %r14
    addq $REC_MSG, %r14
    movl $0x504E5948, MSG_MAGIC(%r14)     /* "HYNP" */
    movl $0x52513151, MSG_ID(%r14)        /* "RQ1Q"：应答应原样回填 */
    movl $1, MSG_TYPE(%r14)               /* REQUEST */
    movl $0, MSG_FLAGS(%r14)
    movl $0xBEEF, MSG_SENDER(%r14)        /* sender_id：伪造 */
    movb $3, MSG_SDOM(%r14)               /* sender_domain：伪造为 BUS */
    movb $2, MSG_TDOM(%r14)               /* target_domain = NET */
    movw $0, 0x16(%r14)
    movl $4, MSG_LEN(%r14)                /* 载荷 4 字节 */
    movl $0, 0x1C(%r14)
    movq $0, MSG_REPLY(%r14)              /* reply_to：由内核按身份回填 */
    movl $0x31305152, MSG_DATA(%r14)      /* "RQ01" */

    /* ⑤ 投递请求：CORE 域用户端口 -> NET 域服务端口，跨域必经 ALPC */
    movl $0x10, %eax
    movq %r12, %rdi
    movq %r14, %rsi
    int $0x80
    movq %rax, REC_SEND(%r13)

    /* ⑥ 负向：试着收别人端口的消息，应被拒（EPERM=4） */
    movl $0x11, %eax
    movq %r12, %rdi                  /* 服务端口：不是我的 */
    movq %r14, %rsi
    movl $0, %edx
    int $0x80
    movq %rax, REC_FOREIGN(%r13)

    /* ⑦ 阻塞收应答（限时 300 拍）：被投递唤醒才是正常路径 */
    movl $0x11, %eax
    movq %r15, %rdi                  /* 我自己的身份端口 */
    movq %r14, %rsi
    movl $300, %edx
    int $0x80
    movq %rax, REC_RECV(%r13)

    /* ⑧ 拆应答：类型 / 序号 / 来源域 / 长度 / 载荷 */
    xorl %eax, %eax
    movl MSG_TYPE(%r14), %eax
    movq %rax, REC_RTYPE(%r13)
    xorl %eax, %eax
    movl MSG_ID(%r14), %eax
    movq %rax, REC_RMSGID(%r13)
    xorl %eax, %eax
    movzbl MSG_SDOM(%r14), %eax
    movq %rax, REC_RDOMAIN(%r13)
    xorl %eax, %eax
    movl MSG_LEN(%r14), %eax
    movq %rax, REC_RLEN(%r13)
    xorl %eax, %eax
    movl MSG_DATA(%r14), %eax
    movq %rax, REC_RWORD(%r13)

    movq $0x5A5A, REC_DONE(%r13)     /* 跑完整条流程的标记 */
    movl $0x20, %eax                 /* SYS_EXIT */
    movq $0x5A5A, %rdi
    int $0x80
1:  jmp 1b                           /* 不该到达：SYS_EXIT 不返回 */
.globl __ring3_probe_good_end
__ring3_probe_good_end:

/* ---------- 探针乙：越权访存（第一条访存指令就去读内核地址） ---------- */
.globl __ring3_probe_fault_start
__ring3_probe_fault_start:
    movq $0x100000, %rax             /* 内核恒等映射地址（supervisor 页） */
    movq (%rax), %rcx                /* #PF 在此触发：内核只应结束本线程 */
    /* 不该走到这里：如果越权访问没被拦下，就写脏记录页自证 */
    movq 8(%rsp), %r13
    movq $0xBAD, (%r13)
    movl $0x20, %eax
    movq $0xDEAD, %rdi
    int $0x80
1:  jmp 1b
.globl __ring3_probe_fault_end
__ring3_probe_fault_end: