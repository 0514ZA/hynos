/*
 * 混元 OS —— Ring3 示例程序 Agent 骨架
 *
 * 与 Shell 骨架的分工：Shell 问底座（core.req，同域），Agent 问的是**另一个故障域**——
 * 网络组的服务口 net.svc。这条路径是"跨故障域强制走 ALPC 消息"在用户态一侧的落点：
 * 用户程序自己没有跨域直调的能力，它只能把请求投进消息总线，由网络组的服务线程处理。
 *
 * 因此这里的读数全部是"跨域取回的真实硬件事实"（网卡厂商/设备号、BAR、链路、MAC、
 * IP 校验和复算结果），而不是内核里的缓存值；记录页里的 k0..k3 就按这个口径被内核核对。
 */
#include <hyn/abi.h>
#include "libhyn.h"

void hyn_user_main(hyn_abi_user_args_t *args);

void hyn_user_main(hyn_abi_user_args_t *args)
{
    hyn_abi_record_t     *rec;
    hyn_abi_msg_t         rep;
    hyn_abi_group_ping_t  ping;
    hyn_abi_net_probe_t   np;
    hyn_conn_t            conn;
    hyn_abi_u32           echo = HYN_ABI_PING_MAGIC;
    hyn_abi_u32           len  = 0u;
    hyn_abi_u64           st;

    if (args == 0) {
        hyn_exit(1u);
    }
    rec = (hyn_abi_record_t *)(unsigned long)args->record_va;

    /* ① 身份与运行环：Agent 也是 Ring3 程序，同样要自证"我在 CPL=3" */
    rec->magic       = HYN_ABI_REC_MAGIC;
    rec->abi_version = HYN_ABI_VERSION;
    rec->kind        = HYN_ABI_KIND_AGENT;
    rec->self_port   = args->self_port;
    rec->target_port = args->target_port;

    (void)hyn_sys_ping(HYN_ABI_PING_MAGIC, &rec->sys_ping_echo);
    rec->tid = 0ull;
    (void)hyn_sys_get_tid(&rec->tid);
    rec->cs      = hyn_self_cs();
    rec->cpl_ret = hyn_sys_check_cpl(rec->cs);

    hyn_conn_init(&conn, args->self_port, args->target_port);

    /* ② 跨域 PING：应答来源域应当是网络组（内核据此核对"消息真的跨了域"） */
    st = hyn_conn_call(&conn, HYN_ABI_OP_PING, &echo, 4u,
                       &ping, (hyn_abi_u32)sizeof(ping), &len, &rep, 40ull);
    rec->ping_ret    = st;
    rec->ping_echo   = ping.op_echo;
    rec->ping_domain = rep.sender_domain;
    rec->ping_msg_id = rep.msg_id;
    rec->ping_state  = ping.state;
    rec->ping_epoch  = ping.epoch;

    /* ③ 跨域 NET_PROBE：把网卡硬件事实取回来（k0..k3 按 Agent 口径解释） */
    st = hyn_conn_call(&conn, HYN_ABI_OP_NET_PROBE, 0, 0u,
                       &np, (hyn_abi_u32)sizeof(np), &len, &rep, 40ull);
    rec->req2_ret    = st;
    rec->req2_len    = len;
    rec->req2_domain = rep.sender_domain;
    rec->k0          = np.nic_found;
    rec->k1          = ((hyn_abi_u64)np.pci_vendor << 16) | (hyn_abi_u64)np.pci_device;
    rec->k2          = np.cksum_expected;
    rec->k3          = np.cksum_result;

    /*
     * ④ 负向一：去收**别人端口**的消息 —— 内核应拒绝（EPERM）。
     * 端口身份是内核绑的，用户程序不能借"换端口号"去翻别人的信箱。
     */
    rec->recv_to = hyn_msg_recv(args->target_port, &rep, 8ull);

    /* ⑤ 负向二：把内核地址当收件缓冲区 —— 同样应在门口被挡下（EFAULT） */
    rec->badptr_ret = hyn_sys_alpc_recv(args->self_port, (hyn_abi_msg_t *)0x100000ull, 4ull);

    /* ⑥ 收尾 */
    rec->done = HYN_ABI_REC_DONE;
    hyn_exit(0u);
}