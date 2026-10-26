/*
 * HynOS —— libhyn 实现（Ring3 用户态）
 *
 * 全部能力都经由唯一入口 int $0x80 落到内核，库本身不做任何特权动作：
 *   · 消息缓冲用调用方的栈（本库没有 .data/.bss，因此可以被装到任意地址）；
 *   · 应答按 msg_id 与请求配对，配不上就报 EIO —— 宁可失败，也不把别人的应答当自己的；
 *   · 内核给什么错误码就原样返回什么，不做任何"失败也算成功"的包装。
 */
#include <hyn/abi.h>
#include "libhyn.h"

/* ============================================================
 * 小工具：显式字节循环（用户程序用 -fno-builtin 编译，不会变成对 libc 的调用）
 * ============================================================ */
static void hyn_copy(void *dst, const void *src, hyn_abi_u32 n)
{
    hyn_abi_u8       *d = (hyn_abi_u8 *)dst;
    const hyn_abi_u8 *s = (const hyn_abi_u8 *)src;
    hyn_abi_u32 i;

    for (i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

static void hyn_zero(void *dst, hyn_abi_u32 n)
{
    hyn_abi_u8 *d = (hyn_abi_u8 *)dst;
    hyn_abi_u32 i;

    for (i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

/* ============================================================
 * syscall 原语
 * ============================================================ */
hyn_abi_u64 hyn_syscall(hyn_abi_u64 nr, hyn_abi_u64 a0, hyn_abi_u64 a1, hyn_abi_u64 a2,
                        hyn_abi_u64 *out_rdx)
{
    register hyn_abi_u64 rax __asm__("rax") = nr;
    register hyn_abi_u64 rdi __asm__("rdi") = a0;
    register hyn_abi_u64 rsi __asm__("rsi") = a1;
    register hyn_abi_u64 rdx __asm__("rdx") = a2;

    __asm__ volatile("int $0x80"
                     : "+r"(rax), "+r"(rdx)
                     : "r"(rdi), "r"(rsi)
                     : "rcx", "r8", "r9", "r10", "r11", "memory");

    if (out_rdx != 0) {
        *out_rdx = rdx;
    }
    return rax;
}

hyn_abi_u64 hyn_sys_ping(hyn_abi_u64 magic, hyn_abi_u64 *out_magic)
{
    return hyn_syscall(HYN_ABI_SYS_PING, magic, 0ull, 0ull, out_magic);
}

hyn_abi_u64 hyn_sys_get_tid(hyn_abi_u64 *out_tid)
{
    return hyn_syscall(HYN_ABI_SYS_GET_TID, 0ull, 0ull, 0ull, out_tid);
}

hyn_abi_u64 hyn_sys_check_cpl(hyn_abi_u64 cs)
{
    return hyn_syscall(HYN_ABI_SYS_CHECK_CPL, cs, 0ull, 0ull, 0);
}

hyn_abi_u64 hyn_sys_yield(void)
{
    return hyn_syscall(HYN_ABI_SYS_YIELD, 0ull, 0ull, 0ull, 0);
}

hyn_abi_u64 hyn_sys_alpc_send(hyn_abi_u64 target_port, const hyn_abi_msg_t *msg)
{
    return hyn_syscall(HYN_ABI_SYS_ALPC_SEND, target_port, (hyn_abi_u64)(unsigned long)msg,
                       0ull, 0);
}

hyn_abi_u64 hyn_sys_alpc_recv(hyn_abi_u64 self_port, hyn_abi_msg_t *msg, hyn_abi_u64 ticks)
{
    return hyn_syscall(HYN_ABI_SYS_ALPC_RECV, self_port, (hyn_abi_u64)(unsigned long)msg,
                       ticks, 0);
}

void hyn_exit(hyn_abi_u64 code)
{
    (void)hyn_syscall(HYN_ABI_SYS_EXIT, code, 0ull, 0ull, 0);
    for (;;) {
        /* SYS_EXIT 不返回；万一回来也不能继续跑，就地停住等内核收尸 */
    }
}

hyn_abi_u64 hyn_self_cs(void)
{
    hyn_abi_u32 cs = 0u;

    __asm__ volatile("movl %%cs, %0" : "=r"(cs));
    return (hyn_abi_u64)cs;
}

/* ============================================================
 * 消息助手
 * ============================================================ */
void hyn_msg_init(hyn_abi_msg_t *m, hyn_abi_u32 type, hyn_abi_u32 msg_id,
                  hyn_abi_u32 flags, const void *payload, hyn_abi_u32 len)
{
    if (m == 0) {
        return;
    }
    hyn_zero(m, (hyn_abi_u32)sizeof(*m));
    m->magic  = HYN_ABI_MSG_MAGIC;
    m->msg_id = msg_id;
    m->type   = type;
    m->flags  = flags;
    if (len > HYN_ABI_MSG_MAX) {
        len = HYN_ABI_MSG_MAX;
    }
    m->len = len;
    if (payload != 0 && len != 0u) {
        hyn_copy(m->data, payload, len);
    }
}

hyn_abi_u64 hyn_msg_send(hyn_abi_u64 target_port, const hyn_abi_msg_t *m)
{
    return hyn_sys_alpc_send(target_port, m);
}

hyn_abi_u64 hyn_msg_recv(hyn_abi_u64 self_port, hyn_abi_msg_t *m, hyn_abi_u64 ticks)
{
    return hyn_sys_alpc_recv(self_port, m, ticks);
}

/* ============================================================
 * 连接对象与一次完整往返
 * ============================================================ */
void hyn_conn_init(hyn_conn_t *c, hyn_abi_u64 self_port, hyn_abi_u64 target_port)
{
    if (c == 0) {
        return;
    }
    c->self_port    = self_port;
    c->target_port  = target_port;
    c->next_msg_id  = 1u;      /* 序号从 1 起：0 留着表示"没序号" */
    c->pad0         = 0u;
}

hyn_abi_u64 hyn_conn_call(hyn_conn_t *c, hyn_abi_u32 op,
                          const void *in, hyn_abi_u32 in_len,
                          void *out, hyn_abi_u32 out_cap, hyn_abi_u32 *out_len_actual,
                          hyn_abi_msg_t *reply, hyn_abi_u64 timeout_ticks)
{
    hyn_abi_msg_t req;
    hyn_abi_msg_t rep;
    hyn_abi_u64   want_id;
    hyn_abi_u64   st;

    if (c == 0) {
        return HYN_ABI_EINVAL;
    }
    want_id        = (hyn_abi_u64)c->next_msg_id;
    hyn_msg_init(&req, HYN_ABI_MSG_REQUEST, c->next_msg_id, op, in, in_len);
    c->next_msg_id = (hyn_abi_u32)(want_id + 1ull);

    st = hyn_msg_send(c->target_port, &req);
    if (st != HYN_ABI_OK) {
        return st;                       /* 内核拒绝投递：原样上报，不重试、不伪装 */
    }

    st = hyn_msg_recv(c->self_port, &rep, timeout_ticks);
    if (st != HYN_ABI_OK) {
        return st;
    }
    if (rep.type != HYN_ABI_MSG_REPLY) {
        return HYN_ABI_EIO;              /* 收到通知/请求：不是我要的应答 */
    }
    if ((hyn_abi_u64)rep.msg_id != want_id) {
        return HYN_ABI_EIO;              /* 序号对不上：拒绝把别人的应答当成自己的 */
    }

    if (out != 0 && out_cap != 0u) {
        hyn_abi_u32 n = rep.len;

        if (n > out_cap) {
            n = out_cap;
        }
        if (n > HYN_ABI_MSG_MAX) {
            n = HYN_ABI_MSG_MAX;
        }
        hyn_copy(out, rep.data, n);
        if (out_len_actual != 0) {
            *out_len_actual = n;
        }
    } else if (out_len_actual != 0) {
        *out_len_actual = rep.len;
    }
    if (reply != 0) {
        hyn_copy(reply, &rep, (hyn_abi_u32)sizeof(rep));
    }
    return HYN_ABI_OK;
}