/*
 * 混元 OS —— Ring3 示例程序 Shell 骨架
 *
 * 这个程序就是"新增业务功能"的标准形态：纯用户态、只 include libhyn 与 ABI 契约，
 * 通过 syscall + ALPC 向内核服务端口（core.req，硬核底座）提问，内核在安全点应答。
 *
 * 它跑完会把一份"自证材料"写在记录页里（记录页码由内核分配并传给程序）：
 *   · 我是谁（tid / %cs / 自读 %cs 与内核帧核对结果 / 端口身份）
 *   · 两次真实往返的结果（应答来源域、序号、载荷长度、关键读数）
 *   · 两次负向对照（没消息就该超时；非法指针就该被挡下）—— 内核不糊弄用户，用户也不糊弄内核
 * 只有整条流程都跑完，才写 done 标记；任何一步失败都会如实留在记录里，不掩饰。
 */
#include <hyn/abi.h>
#include "libhyn.h"

void hyn_user_main(hyn_abi_user_args_t *args);

void hyn_user_main(hyn_abi_user_args_t *args)
{
    hyn_abi_record_t    *rec;
    hyn_abi_msg_t       *rep_buf;      /* 用记录页里的消息区当应答缓冲：这一页是我拿到的工作区 */
    hyn_abi_msg_t        rep;
    hyn_abi_group_ping_t ping;
    hyn_abi_group_info_t info;
    hyn_conn_t           conn;
    hyn_abi_u32          echo = HYN_ABI_PING_MAGIC;
    hyn_abi_u32          len  = 0u;
    hyn_abi_u64          st;

    if (args == 0) {
        hyn_exit(1u);
    }
    rec     = (hyn_abi_record_t *)(unsigned long)args->record_va;
    rep_buf = (hyn_abi_msg_t *)(unsigned long)(args->record_va + HYN_ABI_REC_MSG_OFF);

    /* ① 开场自报家门：先写 magic/版本/类别，再问内核"我是谁、我在哪一环" */
    rec->magic       = HYN_ABI_REC_MAGIC;
    rec->abi_version = HYN_ABI_VERSION;
    rec->kind        = HYN_ABI_KIND_SHELL;
    rec->self_port   = args->self_port;
    rec->target_port = args->target_port;

    (void)hyn_sys_ping(HYN_ABI_PING_MAGIC, &rec->sys_ping_echo);
    rec->tid = 0ull;
    (void)hyn_sys_get_tid(&rec->tid);
    rec->cs      = hyn_self_cs();
    rec->cpl_ret = hyn_sys_check_cpl(rec->cs);

    hyn_conn_init(&conn, args->self_port, args->target_port);

    /* ② PING 往返：核对回声（参数真的过了消息总线）、来源域、序号回填、组状态 */
    st = hyn_conn_call(&conn, HYN_ABI_OP_PING, &echo, 4u,
                       &ping, (hyn_abi_u32)sizeof(ping), &len, &rep, 40ull);
    rec->ping_ret    = st;
    rec->ping_echo   = ping.op_echo;
    rec->ping_domain = rep.sender_domain;
    rec->ping_msg_id = rep.msg_id;
    rec->ping_state  = ping.state;
    rec->ping_epoch  = ping.epoch;

    /* ③ INFO 往返：把应答收在自己的消息区里，取回底座的分组规模读数 */
    st = hyn_conn_call(&conn, HYN_ABI_OP_INFO, 0, 0u,
                       &info, (hyn_abi_u32)sizeof(info), &len, rep_buf, 40ull);
    rec->req2_ret    = st;
    rec->req2_len    = len;
    rec->req2_domain = rep_buf->sender_domain;
    rec->k0          = info.devices_total;
    rec->k1          = info.devices_ready;
    rec->k2          = info.resources;
    rec->k3          = info.probe_count;

    /* ④ 负向一：这段时间本来就没有消息 —— 内核应如实返回"超时"，而不是编一条出来 */
    rec->recv_to = hyn_msg_recv(args->self_port, &rep, 8ull);

    /* ⑤ 负向二：把一个内核地址当用户缓冲区交给内核 —— 应被挡在门口（EFAULT） */
    rec->badptr_ret = hyn_sys_alpc_send(args->target_port, (const hyn_abi_msg_t *)0x100000ull);

    /* ⑥ 收尾：整条流程跑完才落 done */
    rec->done = HYN_ABI_REC_DONE;
    hyn_exit(0u);
}