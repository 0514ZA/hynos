/*
 * HynOS —— syscall 唯一入口（Ring3 -> Ring0）
 *
 * 入口形态：IDT 向量 0x80 的 DPL=3 中断门。全内核只有这一个门允许 Ring3 进入，
 * 其它门（异常 0..31、硬件中断 32..47）一律 DPL=0 ——"syscall 是唯一入口"这条纪律
 * 因此在硬件层就能被核对（自检直接遍历 IDT 统计 DPL=3 的门数）。
 *
 * 调用约定（x86_64，参数直接取自陷阱栈帧，与 arch/x86_64/isr.S 一一对应）：
 *   rax = 调用号；rdi/rsi/rdx = 参数 0..2；rax = 返回值（0 = 成功，非 0 = 统一错误码）；
 *   需要额外输出的调用把结果放在 rdx（例如"取线程 id"）。
 *
 * 三条纪律（都在实现里强制，且有自检项）：
 *   1) 用户内存类调用（ALPC 收发）只接受 Ring3 调用者，内核态调用一律 EPERM；
 *   2) 调用者必须已绑定身份端口，且 recv 只能收自己端口的消息（不能偷看别人信箱）；
 *   3) 所有用户指针按"用户地址窗口 + 已映射"双重校验；消息发送者身份由内核回填，
 *      用户填的 sender_id / sender_domain / reply_to 一律被覆盖（防伪造）。
 */
#ifndef HYN_SYSCALL_H
#define HYN_SYSCALL_H

#include <hyn/types.h>
#include <hyn/trap.h>

/* 调用号（0x00xx 基础能力 / 0x001x 消息总线 / 0x002x 生命周期） */
#define HYN_SYS_PING         0x0001u   /* (magic) -> rax=0，rdx 回显 magic */
#define HYN_SYS_GET_TID      0x0002u   /* () -> rax=0，rdx=线程 id */
#define HYN_SYS_CHECK_CPL    0x0003u   /* (cs) -> rax=0 表示与内核帧的 CS 一致 */
#define HYN_SYS_YIELD        0x0004u   /* () -> rax=0（让出 CPU） */
#define HYN_SYS_ALPC_SEND    0x0010u   /* (目标端口 id, 消息*) -> rax=0 */
#define HYN_SYS_ALPC_RECV    0x0011u   /* (本端口 id, 消息*, 超时节拍) -> rax=0 */
#define HYN_SYS_EXIT         0x0020u   /* (退出码) -> 不返回 */

#define HYN_SYS_PING_MAGIC   0x48594E53u   /* 'HYNS'，用于核对参数真的到了内核 */

/* 由陷阱分派调用：向量 0x80 的唯一出口 */
void hyn_syscall_enter(hyn_trap_frame_t *frame);

/*
 * Ring3 程序触发异常（越权访存、非法指令等）时的接管入口：
 * 结束该用户线程并留审计痕迹，返回 1 表示已接管（内核继续运行）；
 * 返回 0 表示不该由它接管（交回陷阱框架按未处理异常处理）。
 */
int  hyn_user_fault(hyn_trap_frame_t *frame);

/* 用户指针校验：整个 [va, va+len) 必须落在用户地址窗口内且逐页已映射 */
int  hyn_syscall_user_va_ok(u64 va, u32 len);

/* 取证与汇报用统计 */
u32  hyn_syscall_table_size(void);
u32  hyn_syscall_total(void);
u32  hyn_syscall_user_count(void);        /* 来自 Ring3 的调用次数 */
u32  hyn_syscall_kernel_denied(void);     /* 内核态调用"仅限 Ring3"的调用被拒次数 */
u32  hyn_syscall_noport_denied(void);     /* 未绑定身份端口被拒次数 */
u32  hyn_syscall_foreign_denied(void);    /* 试图收他人端口消息被拒次数 */
u32  hyn_syscall_badptr_count(void);      /* 非法用户指针被拒次数 */
u32  hyn_syscall_unknown_count(void);     /* 未知调用号次数 */
u32  hyn_syscall_spoof_fixed(void);       /* 用户伪造发送者身份被纠正次数 */
u32  hyn_syscall_alpc_sent(void);         /* 经 syscall 投递进 ALPC 的消息条数 */
u32  hyn_user_fault_count(void);
u64  hyn_syscall_first_user_cs(void);     /* 第一次来自 Ring3 的帧 CS */
u64  hyn_syscall_first_user_rip(void);    /* 第一次来自 Ring3 的帧 RIP */
u64  hyn_syscall_last_user_cs(void);
u64  hyn_syscall_last_user_rip(void);
int  hyn_syscall_last_exit_code(void);
const char *hyn_syscall_name(u32 nr);

#endif /* HYN_SYSCALL_H */