/*
 * HynOS —— libhyn：Ring3 用户态库（ALPC 消息总线的用户侧封装）
 *
 * 定位与纪律（对应架构里"上层所有应用跑在 Ring3，靠 syscall + ALPC 与内核通信"）：
 *   · 本库只依赖 include/hyn/abi.h 这一份契约，不 include 任何内核内部头：
 *     端口编号、线程、页表、故障域槽位这些内核概念在这里完全不可见；
 *   · 用户态进入内核的唯一通道是 int $0x80（SYS_* 调用号），本库不做任何特权指令；
 *   · "新增业务功能写用户态程序，不用修改内核"——业务代码只需要本库 + abi.h 就能
 *     向内核服务端口发请求、收应答。
 *
 * 两个刻意的设计：
 *   1) 库内不保存任何全局状态：连接状态由调用方在自己的栈上放一个 hyn_conn_t 持有。
 *      这样用户程序可以被装载到任意地址、可以并发多份，也不会把状态藏在库里。
 *   2) 所有返回值都是内核统一错误码（0 = 成功）。库绝不把失败包装成"看起来成功"。
 */
#ifndef LIBHYN_H
#define LIBHYN_H

#include <hyn/abi.h>

/* ============================================================
 * 一、syscall 原语：int $0x80，rax=调用号，rdi/rsi/rdx=参数，rax=返回值
 * ============================================================ */

/* 通用入口：nr + 最多 3 个参数；out_rdx 可为 NULL（内核用 rdx 回传额外结果时用得上） */
hyn_abi_u64 hyn_syscall(hyn_abi_u64 nr, hyn_abi_u64 a0, hyn_abi_u64 a1, hyn_abi_u64 a2,
                        hyn_abi_u64 *out_rdx);

hyn_abi_u64 hyn_sys_ping(hyn_abi_u64 magic, hyn_abi_u64 *out_magic);   /* 回声自检 */
hyn_abi_u64 hyn_sys_get_tid(hyn_abi_u64 *out_tid);                     /* 取本线程 id */
hyn_abi_u64 hyn_sys_check_cpl(hyn_abi_u64 cs);                         /* 核对 %cs 与内核帧 */
hyn_abi_u64 hyn_sys_yield(void);                                       /* 主动让出 CPU */
hyn_abi_u64 hyn_sys_alpc_send(hyn_abi_u64 target_port, const hyn_abi_msg_t *msg);
hyn_abi_u64 hyn_sys_alpc_recv(hyn_abi_u64 self_port, hyn_abi_msg_t *msg, hyn_abi_u64 ticks);

/* 退出当前用户线程（不返回） */
void hyn_exit(hyn_abi_u64 code);

/* 读当前 %cs（用户程序自证 CPL=3 用；自己说的不算，交给内核核对） */
hyn_abi_u64 hyn_self_cs(void);

/* ============================================================
 * 二、ALPC 消息助手：拼消息 / 投递 / 阻塞收取
 * ============================================================ */

/* 按契约初始化一条消息；payload 可为 NULL（只带 op 的请求） */
void hyn_msg_init(hyn_abi_msg_t *m, hyn_abi_u32 type, hyn_abi_u32 msg_id,
                  hyn_abi_u32 flags, const void *payload, hyn_abi_u32 len);

hyn_abi_u64 hyn_msg_send(hyn_abi_u64 target_port, const hyn_abi_msg_t *m);

/*
 * 阻塞收取。ticks 的语义按内核 ALPC 的约定：
 *   ticks > 0  —— 最多等这么多节拍，等不到返回 HYN_ABI_ETIMEDOUT；
 *   ticks == 0 —— 不限时等到底（用户态拿不到"只探一下"的非阻塞语义，此处如实照搬）。
 * 调用方必须先绑定身份端口（内核侧绑定），且只能收自己端口的消息。
 */
hyn_abi_u64 hyn_msg_recv(hyn_abi_u64 self_port, hyn_abi_msg_t *m, hyn_abi_u64 ticks);

/* ============================================================
 * 三、连接对象：一个"我到某个内核服务端口"的会话（状态放在调用方栈上）
 * ============================================================ */
typedef struct hyn_conn {
    hyn_abi_u64 self_port;       /* 我的身份端口（内核绑定，改不了） */
    hyn_abi_u64 target_port;     /* 对端服务端口 */
    hyn_abi_u32 next_msg_id;     /* 请求序号：应答按它配对 */
    hyn_abi_u32 pad0;
} hyn_conn_t;

void hyn_conn_init(hyn_conn_t *c, hyn_abi_u64 self_port, hyn_abi_u64 target_port);

/*
 * 一次完整往返：发 REQUEST(op 放 flags) -> 阻塞等同一 msg_id 的 REPLY。
 * 返回：0 = 收到配对应答；非 0 = 内核错误码（含超时 ETIMEDOUT）。
 * out/out_cap/out_len_actual 用于取应答载荷；reply 可为 NULL（不需要看应答头时）。
 */
hyn_abi_u64 hyn_conn_call(hyn_conn_t *c, hyn_abi_u32 op,
                          const void *in, hyn_abi_u32 in_len,
                          void *out, hyn_abi_u32 out_cap, hyn_abi_u32 *out_len_actual,
                          hyn_abi_msg_t *reply, hyn_abi_u64 timeout_ticks);

/* 业务入口：由用户程序实现（entry.S 负责把参数块指针传进来） */
void hyn_user_main(hyn_abi_user_args_t *args);

#endif /* LIBHYN_H */