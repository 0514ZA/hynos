/*
 * 混元 OS —— syscall 唯一入口（纯 C，架构无关，硬核底座）
 *
 * Ring3 只能通过 IDT 向量 0x80（唯一 DPL=3 的门）进入内核，本文件是那个门的唯一出口。
 * 参数直接取自陷阱栈帧（见 arch/x86_64/isr.S），不走寄存器传参的额外约定：
 *   rax = 调用号 / 返回值（0 = 成功，非 0 = 统一错误码）；rdi/rsi/rdx = 参数 0..2。
 *
 * 三条纪律，逐条都在实现里强制、且有对应自检：
 *   1) 用户内存类调用（ALPC 收发）只接受 Ring3 调用者：内核态调用一律 EPERM（否则内核
 *      线程就能借 syscall 绕过身份校验去偷看用户信箱）；
 *   2) 调用者必须已绑定身份端口，且只允许收自己端口的消息；
 *   3) 用户指针按"用户地址窗口 + 逐页已映射"双重校验；消息发送者身份由内核回填
 *      （sender_id / sender_domain / reply_to 用户填什么都一律覆盖），伪造计数入审计。
 *
 * 带外取值与内核分发走 ALPC：sys_alpc_send 校验通过后并不自己处理消息，而是交给
 * ALPC 总线（hyn_alpc_send），由目标域的服务线程在 ALPC 语义下收取。
 */
#include <hyn/syscall.h>
#include <hyn/thread.h>
#include <hyn/alpc.h>
#include <hyn/ring3.h>
#include <hyn/vmm.h>
#include <hyn/arch.h>
#include <hyn/kstring.h>
#include <hyn/kprintf.h>

typedef hyn_status_t (*hyn_sys_fn_t)(hyn_trap_frame_t *f);

typedef struct {
    u32          nr;
    const char  *name;
    hyn_sys_fn_t fn;
    u8           user_only;      /* 1 = 只接受 Ring3 调用者 */
} hyn_sys_entry_t;

/* 审计与取证计数（自检按这些计数断言"纪律真的被执行了"） */
static u32 g_total;
static u32 g_user_count;
static u32 g_kernel_denied;
static u32 g_noport_denied;
static u32 g_foreign_denied;
static u32 g_badptr;
static u32 g_unknown;
static u32 g_spoof;
static u32 g_alpc_sent;
static u32 g_user_fault;
static u64 g_first_user_cs, g_first_user_rip;
static u64 g_last_user_cs,  g_last_user_rip;
static int g_last_exit_code;

/* ------------------------------------------------------------------ */
/* 调用实现                                                          */
/* ------------------------------------------------------------------ */

static hyn_status_t sys_ping(hyn_trap_frame_t *f)
{
    if ((u32)f->rdi != HYN_SYS_PING_MAGIC) {
        return HYN_EINVAL;
    }
    f->rdx = (u64)HYN_SYS_PING_MAGIC;      /* 把 magic 回显出去，证明参数真到了内核 */
    return HYN_OK;
}

static hyn_status_t sys_get_tid(hyn_trap_frame_t *f)
{
    f->rdx = hyn_thread_id(hyn_thread_current());
    return HYN_OK;
}

/* 让程序把自己读到的 %cs 交内核核对：答案是内核手里的帧，不是程序自己说的 */
static hyn_status_t sys_check_cpl(hyn_trap_frame_t *f)
{
    if ((f->cs & 0xFFFFull) != (f->rdi & 0xFFFFull)) {
        return HYN_EINVAL;
    }
    if ((f->cs & 3ull) != 3ull) {
        return HYN_EPERM;
    }
    return HYN_OK;
}

static hyn_status_t sys_yield(hyn_trap_frame_t *f)
{
    HYN_UNUSED(f);
    hyn_sched_yield();
    return HYN_OK;
}

static hyn_status_t sys_alpc_send(hyn_trap_frame_t *f)
{
    u64 target     = f->rdi;
    u64 uptr_msg   = f->rsi;
    u64 my_port    = hyn_thread_port_id(hyn_thread_current());
    hyn_alpc_msg_t msg;
    hyn_domain_t   my_dom, tgt_dom;
    u32            my_ep;
    hyn_status_t   st;

    if (my_port == 0ull) {
        g_noport_denied++;                 /* 没有身份就不能替谁说话 */
        return HYN_EPERM;
    }
    if (!hyn_syscall_user_va_ok(uptr_msg, (u32)sizeof(hyn_alpc_msg_t))) {
        g_badptr++;                        /* 先校验再解引用：绝不按用户给的值去摸内存 */
        return HYN_EFAULT;
    }

    memcpy(&msg, (const void *)(uptr)uptr_msg, (unsigned long)sizeof(msg));
    if (msg.magic != HYN_ALPC_MSG_MAGIC) {
        g_badptr++;
        return HYN_EINVAL;
    }

    my_dom  = hyn_alpc_port_domain(my_port);
    tgt_dom = hyn_alpc_port_domain(target);
    my_ep   = hyn_alpc_endpoint_of(my_port);

    /* 身份一律由内核回填：用户填的发送者/回信地址全部作废（改了几处就记几次） */
    if (msg.sender_id != my_ep) {
        msg.sender_id = my_ep;
        g_spoof++;
    }
    if ((hyn_domain_t)msg.sender_domain != my_dom) {
        msg.sender_domain = (u8)my_dom;
        g_spoof++;
    }
    if (msg.reply_to != my_port) {
        msg.reply_to = my_port;            /* 应答应回到"发信的那个端口"，不能指去别处 */
        g_spoof++;
    }
    msg.target_domain = (u8)tgt_dom;

    st = hyn_alpc_send(target, &msg);
    if (st == HYN_OK) {
        g_alpc_sent++;                     /* 经 syscall 真正落到 ALPC 总线上的消息条数 */
    }
    return st;
}

static hyn_status_t sys_alpc_recv(hyn_trap_frame_t *f)
{
    u64 port     = f->rdi;
    u64 uptr_msg = f->rsi;
    u64 ticks    = f->rdx;
    u64 my_port  = hyn_thread_port_id(hyn_thread_current());
    hyn_alpc_msg_t msg;
    hyn_status_t st;

    if (my_port == 0ull) {
        g_noport_denied++;
        return HYN_EPERM;
    }
    if (port != my_port) {
        g_foreign_denied++;                /* 只能收自己端口的消息：不许翻别人的信箱 */
        return HYN_EPERM;
    }
    if (!hyn_syscall_user_va_ok(uptr_msg, (u32)sizeof(hyn_alpc_msg_t))) {
        g_badptr++;
        return HYN_EFAULT;
    }

    st = hyn_alpc_receive_wait(port, &msg, ticks);
    if (st == HYN_OK) {
        memcpy((void *)(uptr)uptr_msg, &msg, (unsigned long)sizeof(msg));
    }
    return st;
}

static hyn_status_t sys_exit(hyn_trap_frame_t *f)
{
    g_last_exit_code = (int)f->rdi;
    hyn_thread_exit((int)f->rdi);          /* 不返回 */
    return HYN_OK;                         /* 仅用于满足函数签名，实际不可达 */
}

/* ------------------------------------------------------------------ */
/* 调用表与入口                                                      */
/* ------------------------------------------------------------------ */

static const hyn_sys_entry_t g_sys_table[] = {
    { HYN_SYS_PING,      "PING",      sys_ping,      0u },
    { HYN_SYS_GET_TID,   "GET_TID",   sys_get_tid,   0u },
    { HYN_SYS_CHECK_CPL, "CHECK_CPL", sys_check_cpl, 0u },
    { HYN_SYS_YIELD,     "YIELD",     sys_yield,     0u },
    { HYN_SYS_ALPC_SEND, "ALPC_SEND", sys_alpc_send, 1u },
    { HYN_SYS_ALPC_RECV, "ALPC_RECV", sys_alpc_recv, 1u },
    { HYN_SYS_EXIT,      "EXIT",      sys_exit,      1u }
};

static const hyn_sys_entry_t *sys_lookup(u32 nr)
{
    u32 i;

    for (i = 0u; i < (u32)HYN_ARRAY_SIZE(g_sys_table); i++) {
        if (g_sys_table[i].nr == nr) {
            return &g_sys_table[i];
        }
    }
    return NULL;
}

void hyn_syscall_enter(hyn_trap_frame_t *f)
{
    const hyn_sys_entry_t *e;
    u32 nr;
    int from_user;

    if (f == NULL) {
        return;
    }
    nr        = (u32)f->rax;
    from_user = ((f->cs & 3ull) == 3ull) ? 1 : 0;

    g_total++;
    if (from_user) {
        g_user_count++;
        if (g_user_count == 1u) {
            g_first_user_cs  = f->cs;
            g_first_user_rip = f->rip;
        }
        g_last_user_cs  = f->cs;
        g_last_user_rip = f->rip;
    }

    e = sys_lookup(nr);
    if (e == NULL) {
        g_unknown++;
        f->rax = (u64)HYN_ENOTSUP;
        return;
    }
    if (e->user_only && !from_user) {
        g_kernel_denied++;                 /* 内核态不许调用"仅限用户"的入口 */
        f->rax = (u64)HYN_EPERM;
        return;
    }

    f->rax = (u64)e->fn(f);
}

/*
 * Ring3 线程触发异常（越权访存、非法指令等）：结束该线程，内核继续运行。
 * 这里返回 1 表示"已接管"；返回 0 让陷阱框架按未处理异常处理（内核态异常不该被吞）。
 */
int hyn_user_fault(hyn_trap_frame_t *f)
{
    u32 v;

    if (f == NULL) {
        return 0;
    }
    if ((f->cs & 3ull) != 3ull) {
        return 0;                          /* 内核态异常：交回陷阱框架，转储后停机 */
    }
    v = (u32)f->vector;
    if (v >= HYN_TRAP_EXC_COUNT) {
        return 0;                          /* 用户态中断不该出现，交回默认路径 */
    }

    g_user_fault++;
    g_last_exit_code = (int)(0x10000u | v);
    kprintf("[Ring3] 用户线程异常：向量 %u (%s) 错误码=0x%x RIP=%p —— 只结束该线程，内核继续运行\n",
            v, hyn_trap_name(v), (u32)f->error_code, (void *)f->rip);
    hyn_thread_exit(0x10000 | (int)v);
    return 1;                              /* 不可达 */
}

/*
 * 用户指针校验：整段 [va, va+len) 必须落在用户窗口内且逐页已映射。
 * 如实标注局限：这里按"窗口 + 已映射"校验，没有检查 PTE 的 U 位（arch_mmu_query 只回物理地址）。
 * 用户窗口与内核恒等映射在地址上不可能重叠，且窗口页全部以 U=1 建立，故当前形态足够；
 * 待 mmu 层补出"查询权限位"接口后再收紧。
 */
int hyn_syscall_user_va_ok(u64 va, u32 len)
{
    u64 end, p;

    if (len == 0u) {
        return 0;
    }
    if (va < HYN_USER_BASE) {
        return 0;
    }
    end = va + (u64)len;
    if (end < va || end > HYN_USER_TOP) {
        return 0;
    }
    for (p = va & ~0xFFFull; p < end; p += 0x1000ull) {
        if (vmm_translate(p) == 0ull) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 取证访问器                                                        */
/* ------------------------------------------------------------------ */

const char *hyn_syscall_name(u32 nr)
{
    const hyn_sys_entry_t *e = sys_lookup(nr);

    return (e != NULL) ? e->name : "未知调用号";
}

u32 hyn_syscall_table_size(void)      { return (u32)HYN_ARRAY_SIZE(g_sys_table); }
u32 hyn_syscall_total(void)           { return g_total; }
u32 hyn_syscall_user_count(void)      { return g_user_count; }
u32 hyn_syscall_kernel_denied(void)   { return g_kernel_denied; }
u32 hyn_syscall_noport_denied(void)   { return g_noport_denied; }
u32 hyn_syscall_foreign_denied(void)  { return g_foreign_denied; }
u32 hyn_syscall_badptr_count(void)    { return g_badptr; }
u32 hyn_syscall_unknown_count(void)   { return g_unknown; }
u32 hyn_syscall_spoof_fixed(void)     { return g_spoof; }
u32 hyn_syscall_alpc_sent(void)       { return g_alpc_sent; }
u32 hyn_user_fault_count(void)        { return g_user_fault; }
u64 hyn_syscall_first_user_cs(void)   { return g_first_user_cs; }
u64 hyn_syscall_first_user_rip(void)  { return g_first_user_rip; }
u64 hyn_syscall_last_user_cs(void)    { return g_last_user_cs; }
u64 hyn_syscall_last_user_rip(void)   { return g_last_user_rip; }
int hyn_syscall_last_exit_code(void)  { return g_last_exit_code; }