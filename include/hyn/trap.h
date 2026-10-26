/*
 * 混元 OS —— 陷阱/中断/异常框架接口
 *
 * 该框架是硬核底座的一部分：异常捕获是"故障域单独重置"能力的前置条件。
 */
#ifndef HYN_TRAP_H
#define HYN_TRAP_H

#include <hyn/types.h>

#define HYN_TRAP_VECTORS      256
#define HYN_TRAP_EXC_COUNT    32    /* 0..31 CPU 异常 */
#define HYN_TRAP_IRQ_BASE     32    /* 0..31 分别映射到 32..47 */
#define HYN_TRAP_IRQ_COUNT    16
#define HYN_TRAP_SYSCALL_VECTOR 0x80u   /* Ring3 进入内核的唯一门（DPL=3） */
#define HYN_TRAP_UNDEFINED    0xFFFFFFFFFFFFFFFFull

/*
 * 陷阱栈帧（与 arch/x86_64/isr.S 的压栈顺序严格对应）
 * 内存由低到高：通用寄存器 -> 向量/错误码 -> CPU 压入的返回现场
 */
typedef struct hyn_trap_frame {
    u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 vector;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss;   /* rsp/ss 仅在特权级切换时有效 */
} hyn_trap_frame_t;

typedef void (*hyn_trap_handler_t)(hyn_trap_frame_t *frame);

/* 初始化：装载 IDT、重映射 8259、启动 PIT，并挂上默认 IRQ0 处理 */
hyn_status_t hyn_trap_init(void);

hyn_status_t hyn_trap_set_handler(u32 vector, hyn_trap_handler_t handler);
const char  *hyn_trap_name(u32 vector);
u64          hyn_trap_count(u32 vector);
void         hyn_trap_reset_counts(void);

/*
 * 自检用：预告"下一次某向量异常属于受控探测"，命中后内核记录并把 RIP
 * 修正到 recover_rip（为 0 表示无需修正，例如 #BP 已指向下一条指令）。
 * hyn_trap_expect 同时清零该向量的命中计数，故 hyn_trap_expect_hit 的语义是
 * "自最近一次预告以来命中几次"——同一向量被多个自检项复用时也不会串味。
 */
void hyn_trap_expect(u32 vector, u64 recover_rip);
int  hyn_trap_expect_hit(u32 vector);

/* 由 arch/x86_64/isr.S 调用 */
void hyn_trap_dispatch(hyn_trap_frame_t *frame);

extern const u64 hyn_isr_stub_table[HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT];
extern void hyn_isr_default(void);
extern void isr_128(void);              /* syscall 入口桩（DPL=3 的门专用） */

#endif /* HYN_TRAP_H */