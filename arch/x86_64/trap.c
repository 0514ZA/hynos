/*
 * 混元 OS —— 陷阱分派与异常捕获（硬核底座之一）
 *
 * 分派顺序：
 *   受控探测预告 -> syscall 唯一入口(0x80) -> 注册处理者 -> 硬件中断默认应答
 *   -> Ring3 线程异常（只结束该线程）
 *   -> 内核态线程异常：先问故障域要不要接管（分组线程出错只重置本组）
 *   -> 未被接管：如实转储并停机
 *
 * "syscall 是 Ring3 进入内核的唯一入口"在这里体现为一条代码路径：
 * 向量 0x80 是全表唯一 DPL=3 的门（见 arch/x86_64/idt.c），它的出口只有
 * hyn_syscall_enter 一个；其余门一律 DPL=0，Ring3 根本进不来。
 */
#include <hyn/trap.h>
#include <hyn/kprintf.h>
#include <hyn/cpu.h>
#include <hyn/idt.h>
#include <hyn/pic.h>
#include <hyn/timer.h>
#include <hyn/arch.h>
#include <hyn/syscall.h>
#include <hyn/fault.h>       /* 内核态异常的第二道闸：分组故障域自己接管本组异常 */

#define TRAP_NONE ((u32)HYN_TRAP_VECTORS)

static const char *const g_exc_names[HYN_TRAP_EXC_COUNT] = {
    "#DE 除零错误", "#DB 调试异常", "#NMI 不可屏蔽中断", "#BP 断点",
    "#OF 溢出", "#BR 越界", "#UD 非法指令", "#NM 设备不可用",
    "#DF 双重错误", "保留(9)", "#TS 无效 TSS", "#NP 段不存在",
    "#SS 栈段错误", "#GP 通用保护错误", "#PF 页错误", "保留(15)",
    "#MF 浮点错误", "#AC 对齐检查", "#MC 机器检查", "#XM SIMD 浮点异常",
    "#VE 虚拟化异常", "#CP 控制保护异常", "保留(22)", "保留(23)",
    "保留(24)", "保留(25)", "保留(26)", "保留(27)",
    "#HV 注入异常", "#VC 控制流异常", "#SX 安全异常", "保留(31)"
};

static hyn_trap_handler_t g_handlers[HYN_TRAP_VECTORS];
static u64                g_counts[HYN_TRAP_VECTORS];
static u32                g_expect_vector = TRAP_NONE;
static u64                g_expect_rip;
static u64                g_expect_hits[HYN_TRAP_VECTORS];

const char *hyn_trap_name(u32 vector)
{
    static char buf[24];

    if (vector < HYN_TRAP_EXC_COUNT) {
        return g_exc_names[vector];
    }
    if (vector >= HYN_TRAP_IRQ_BASE && vector < HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT) {
        return "硬件中断";
    }
    if (vector == HYN_TRAP_SYSCALL_VECTOR) {
        return "syscall 入口(int 0x80)";
    }
    ksnprintf(buf, sizeof(buf), "未定义向量(%u)", vector);
    return buf;
}

u64 hyn_trap_count(u32 vector)
{
    return (vector < HYN_TRAP_VECTORS) ? g_counts[vector] : 0;
}

void hyn_trap_reset_counts(void)
{
    u32 i;
    for (i = 0; i < HYN_TRAP_VECTORS; i++) {
        g_counts[i] = 0;
        g_expect_hits[i] = 0;
    }
}

hyn_status_t hyn_trap_set_handler(u32 vector, hyn_trap_handler_t handler)
{
    if (vector >= HYN_TRAP_VECTORS || handler == NULL) {
        return HYN_EINVAL;
    }
    g_handlers[vector] = handler;
    return HYN_OK;
}

void hyn_trap_expect(u32 vector, u64 recover_rip)
{
    if (vector >= HYN_TRAP_VECTORS) {
        return;
    }
    /*
     * 命中计数自本次预告起算：调用方只关心"这一次探测有没有被抓到"，不该被迫
     * 自己记基线。同一向量会被多个自检项反复使用（例如 #PF），累计计数会让
     * 第二个使用者读到"上次的命中"，把真通过当成失败。
     */
    g_expect_hits[vector] = 0;
    g_expect_vector = vector;
    g_expect_rip = recover_rip;
}

int hyn_trap_expect_hit(u32 vector)
{
    return (vector < HYN_TRAP_VECTORS) ? (int)g_expect_hits[vector] : 0;
}

static void trap_dump(const hyn_trap_frame_t *f)
{
    kprintf("\n[陷阱] 未处理异常：向量 %u (%s)  错误码=0x%x\n",
            (u32)f->vector, hyn_trap_name((u32)f->vector), (u32)f->error_code);
    kprintf("[陷阱] RIP=0x%p CS=0x%x RFLAGS=0x%x\n",
            (void *)f->rip, (u32)f->cs, (u32)f->rflags);
    kprintf("[陷阱] RAX=0x%p RBX=0x%p RCX=0x%p RDX=0x%p\n",
            (void *)f->rax, (void *)f->rbx, (void *)f->rcx, (void *)f->rdx);
    kprintf("[陷阱] RSI=0x%p RDI=0x%p RBP=0x%p RSP=0x%p\n",
            (void *)f->rsi, (void *)f->rdi, (void *)f->rbp, (void *)f->rsp);
    if (f->vector == 14u) {
        kprintf("[陷阱] CR2 (触发地址) = 0x%p\n", (void *)hyn_read_cr2());
    }
    if (f->cs != HYN_SEL_KCODE64) {
        kprintf("[陷阱] 特权级变化：来自 Ring%u（RSP=0x%p SS=0x%x）\n",
                (u32)(f->cs & 0x3u), (void *)f->rsp, (u32)f->ss);
    }
}

void hyn_trap_dispatch(hyn_trap_frame_t *f)
{
    u32 v = (u32)f->vector;

    if (v < HYN_TRAP_VECTORS) {
        g_counts[v]++;
    }

    /* 1) 受控自检：命中即修正 RIP 后返回，内核继续运行 */
    if (v == g_expect_vector && v < HYN_TRAP_VECTORS) {
        g_expect_hits[v]++;
        g_expect_vector = TRAP_NONE;
        if (g_expect_rip != 0) {
            f->rip = g_expect_rip;
        }
        return;
    }

    /*
     * 2) syscall 唯一入口：向量 0x80 —— 全表唯一的 DPL=3 门（在 hyn_idt_init 里单独设置）。
     *    正常路径来自 Ring3；内核态若有人 int 0x80 探测也走同一条出口，由
     *    hyn_syscall_enter 按调用者 CS 判别并拒绝"仅限 Ring3"的调用——入口唯一，
     *    判别集中在出口，不存在"绕开检查的第二条进核路径"。
     */
    if (v == HYN_TRAP_SYSCALL_VECTOR) {
        hyn_syscall_enter(f);
        return;
    }

    /* 3) 注册的处理者 */
    if (v < HYN_TRAP_VECTORS && g_handlers[v] != NULL) {
        g_handlers[v](f);
        if (v >= HYN_TRAP_IRQ_BASE && v < HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT) {
            hyn_pic_eoi(v - HYN_TRAP_IRQ_BASE);
        }
        return;
    }

    /* 4) 无处理者的硬件中断：应答后忽略 */
    if (v >= HYN_TRAP_IRQ_BASE && v < HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT) {
        hyn_pic_eoi(v - HYN_TRAP_IRQ_BASE);
        return;
    }

    /*
     * 5) Ring3 线程触发的异常：只结束该线程，内核继续运行。
     *    用户程序越权访存不该拖垮整机。
     */
    if ((f->cs & 3ull) == 3ull && hyn_user_fault(f)) {
        return;
    }

    /*
     * 5b) 内核态异常的第二道闸：故障域异常捕获。
     *     归属某个分组故障域（存储组/网络组/硬件总线组）的线程在内核态出错时，由本组自己
     *     接管：结束该故障线程 + 标记本组 FAULTED，硬核底座与其余两组继续运行。
     *     归属硬核底座（CORE）的线程出错一律不接管，走 6) 如实停机——故障域纪律是
     *     "分组崩溃可隔离重置"，不是"把内核异常统统吞掉"。
     */
    if (hyn_domain_fault_hook(f) != 0) {
        return;
    }

    /* 6) 未处理异常（含未定义向量）：如实转储并停机 */
    trap_dump(f);
    kpanic("内核捕获到未处理异常，已停机（向量 %u）", v);
}

static void irq0_timer(hyn_trap_frame_t *frame)
{
    HYN_UNUSED(frame);
    hyn_timer_tick();
}

void hyn_trap_panic_handler(hyn_trap_frame_t *frame)
{
    trap_dump(frame);
    kpanic("内核捕获到未处理异常，已停机（向量 %u）", (u32)frame->vector);
}

hyn_status_t hyn_trap_init(void)
{
    u32 i;
    hyn_status_t st;

    for (i = 0; i < HYN_TRAP_VECTORS; i++) {
        g_handlers[i] = NULL;
        g_counts[i] = 0;
        g_expect_hits[i] = 0;
    }
    g_expect_vector = TRAP_NONE;

    /* 先把 IDT 装进 CPU：这一步没做，任何 int3/异常都会因"无门"而 #GP -> #DF -> 三重故障 */
    hyn_idt_init();

    st = hyn_timer_init(100);            /* 100Hz 时钟节拍 */
    if (st != HYN_OK) {
        return st;
    }
    if (hyn_trap_set_handler(HYN_TRAP_IRQ_BASE + 0, irq0_timer) != HYN_OK) {
        return HYN_EINVAL;
    }

    hyn_pic_init();                      /* 全部屏蔽 */
    hyn_pic_unmask(0);                   /* 只放开时钟 */

    kprintf("[中断] IDT 已装载 %u 个门（异常 0..%u / 硬件中断 %u..%u / 未定义 %u 之后）\n",
            (u32)HYN_IDT_ENTRIES, (u32)(HYN_TRAP_EXC_COUNT - 1),
            (u32)HYN_TRAP_IRQ_BASE, (u32)(HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT - 1),
            (u32)(HYN_TRAP_IRQ_BASE + HYN_TRAP_IRQ_COUNT));
    kprintf("[中断] syscall 唯一入口 = 向量 0x%x（门属性 0x%x / DPL=%u），其余门一律 DPL=0\n",
            (u32)HYN_TRAP_SYSCALL_VECTOR, (u32)hyn_idt_get_attr(HYN_TRAP_SYSCALL_VECTOR),
            hyn_idt_gate_dpl(HYN_TRAP_SYSCALL_VECTOR));
    kprintf("[中断] 8259 已重映射（主片基址 %u / 从片基址 %u），时钟 %uHz，仅放开 IRQ0\n",
            (u32)HYN_TRAP_IRQ_BASE, (u32)(HYN_TRAP_IRQ_BASE + 8), hyn_timer_hz());
    return HYN_OK;
}