/*
 * 混元 OS —— 用户程序接入约定（内核侧）
 *
 * 这一层解决的问题只有一个：**用户态库与内核之间不许"对不上"**。
 *
 * 用户程序（libhyn + Shell/Agent 骨架）只 include include/hyn/abi.h，看不到任何内核内部头；
 * 而内核侧要保证 abi.h 里那份"用户视角的契约"与内核真实的 ALPC 消息、分组快照、
 * syscall 表**逐字段一致**。两侧若是各写各的，就只有"跑起来才发现对不上"这一条路——
 * 本文件把这件事挪到编译期：下面每条 _Static_assert 一旦不成立，构建直接失败。
 *
 * 覆盖范围：
 *   1) 定宽类型等价；           2) 错误码 / syscall 号 / 域号 / op 常量；
 *   3) 消息与载荷结构体的 sizeof 与逐字段偏移；4) 记录页布局与用户入口参数块；
 *   5) 用户程序镜像（由 tools/mkuserimg.py 从 ELF 段表生成，内核构建期链进内核）。
 */
#ifndef HYN_USERPROG_H
#define HYN_USERPROG_H

#include <hyn/types.h>
#include <hyn/abi.h>
#include <hyn/alpc.h>
#include <hyn/syscall.h>
#include <hyn/fault.h>
#include <hyn/ring3.h>

/* ============================================================
 * 1) 定宽类型等价：abi.h 用的是自己的别名，必须与内核 u8/u16/u32/u64 同宽同布局
 * ============================================================ */
_Static_assert(sizeof(hyn_abi_u8)  == sizeof(u8),  "abi 定宽类型 u8 与内核不一致");
_Static_assert(sizeof(hyn_abi_u16) == sizeof(u16), "abi 定宽类型 u16 与内核不一致");
_Static_assert(sizeof(hyn_abi_u32) == sizeof(u32), "abi 定宽类型 u32 与内核不一致");
_Static_assert(sizeof(hyn_abi_u64) == sizeof(u64), "abi 定宽类型 u64 与内核不一致");
_Static_assert(sizeof(hyn_abi_u64) == 8u,          "abi 的 u64 在 x86_64 上必须恰好 8 字节");

/* ============================================================
 * 2) 常量等价：错误码、syscall 号、域号与状态、组间 op
 * ============================================================ */
#define HYN_USERPROG_ASSERT_CONST(abi_name, k_name) \
    _Static_assert((abi_name) == (k_name), #abi_name " 与内核 " #k_name " 不一致")

HYN_USERPROG_ASSERT_CONST(HYN_ABI_OK,           HYN_OK);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EINVAL,       HYN_EINVAL);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_ENOMEM,       HYN_ENOMEM);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_ENOENT,       HYN_ENOENT);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EPERM,        HYN_EPERM);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_ENOTSUP,      HYN_ENOTSUP);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EBUSY,        HYN_EBUSY);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EFAULT,       HYN_EFAULT);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EIO,          HYN_EIO);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EDOMAIN,      HYN_EDOMAIN);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_ETIMEDOUT,    HYN_ETIMEDOUT);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_EFAULT_FAULT, HYN_EFAULT_FAULT);

HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_PING,      HYN_SYS_PING);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_GET_TID,   HYN_SYS_GET_TID);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_CHECK_CPL, HYN_SYS_CHECK_CPL);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_YIELD,     HYN_SYS_YIELD);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_ALPC_SEND, HYN_SYS_ALPC_SEND);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_ALPC_RECV, HYN_SYS_ALPC_RECV);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_SYS_EXIT,      HYN_SYS_EXIT);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_PING_MAGIC,    HYN_SYS_PING_MAGIC);

_Static_assert(HYN_ABI_MSG_MAGIC == HYN_ALPC_MSG_MAGIC, "消息 magic 不一致");
HYN_USERPROG_ASSERT_CONST(HYN_ABI_MSG_REQUEST, HYN_ALPC_MSG_REQUEST);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_MSG_REPLY,   HYN_ALPC_MSG_REPLY);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_MSG_NOTIFY,  HYN_ALPC_MSG_NOTIFY);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_MSG_MAX,     HYN_MSG_MAX);

HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_CORE,    HYN_DOMAIN_CORE);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_STORAGE, HYN_DOMAIN_STORAGE);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_NET,     HYN_DOMAIN_NET);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_BUS,     HYN_DOMAIN_BUS);

HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_OFFLINE,   HYN_DOMAIN_STATE_OFFLINE);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_ONLINE,    HYN_DOMAIN_STATE_ONLINE);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_FAULTED,   HYN_DOMAIN_STATE_FAULTED);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_DOMAIN_RESETTING, HYN_DOMAIN_STATE_RESETTING);

HYN_USERPROG_ASSERT_CONST(HYN_ABI_OP_PING,          HYN_GROUP_OP_PING);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_OP_INFO,          HYN_GROUP_OP_INFO);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_OP_CRASH,         HYN_GROUP_OP_CRASH);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_OP_STORAGE_PROBE, HYN_GROUP_OP_STORAGE_PROBE);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_OP_NET_PROBE,     HYN_GROUP_OP_NET_PROBE);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_OP_BUS_ENUM,      HYN_GROUP_OP_BUS_ENUM);

HYN_USERPROG_ASSERT_CONST(HYN_ABI_USER_CS,     HYN_RING3_USER_CS);
HYN_USERPROG_ASSERT_CONST(HYN_ABI_REC_MSG_OFF, HYN_RING3_MSG_OFF);
_Static_assert(HYN_ABI_REC_DONE == HYN_RING3_DONE, "记录页收尾标记不一致");

/* 记录页内的消息缓冲区 + 一条完整消息必须落在同一个 4KiB 页里（否则内核拷不进去） */
_Static_assert((HYN_ABI_REC_MSG_OFF + sizeof(hyn_abi_msg_t)) <= 4096u,
               "记录页里的消息区装不下一条 ALPC 消息");

#undef HYN_USERPROG_ASSERT_CONST

/* ============================================================
 * 3) 消息与载荷结构体：sizeof 与逐字段偏移都必须与内核同名结构一致
 * ============================================================ */
_Static_assert(sizeof(hyn_abi_msg_t) == sizeof(hyn_alpc_msg_t), "ALPC 消息体大小不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, magic)         == __builtin_offsetof(hyn_alpc_msg_t, magic),         "msg.magic 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, msg_id)        == __builtin_offsetof(hyn_alpc_msg_t, msg_id),        "msg.msg_id 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, type)          == __builtin_offsetof(hyn_alpc_msg_t, type),          "msg.type 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, flags)         == __builtin_offsetof(hyn_alpc_msg_t, flags),         "msg.flags 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, sender_id)     == __builtin_offsetof(hyn_alpc_msg_t, sender_id),     "msg.sender_id 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, sender_domain) == __builtin_offsetof(hyn_alpc_msg_t, sender_domain), "msg.sender_domain 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, target_domain) == __builtin_offsetof(hyn_alpc_msg_t, target_domain), "msg.target_domain 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, len)           == __builtin_offsetof(hyn_alpc_msg_t, len),           "msg.len 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, reply_to)      == __builtin_offsetof(hyn_alpc_msg_t, reply_to),      "msg.reply_to 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_msg_t, data)          == __builtin_offsetof(hyn_alpc_msg_t, data),          "msg.data 偏移不一致");
_Static_assert(sizeof(((hyn_abi_msg_t *)0)->sender_id) == sizeof(u32), "msg.sender_id 字段宽度不一致");
_Static_assert(sizeof(((hyn_abi_msg_t *)0)->reply_to)  == sizeof(u64), "msg.reply_to 字段宽度不一致");
_Static_assert(sizeof(((hyn_abi_msg_t *)0)->data)      == sizeof(((hyn_alpc_msg_t *)0)->data), "msg.data 容量不一致");

_Static_assert(sizeof(hyn_abi_group_ping_t) == sizeof(hyn_group_ping_t), "PING 应答结构大小不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_ping_t, op_echo)       == __builtin_offsetof(hyn_group_ping_t, op_echo),       "ping.op_echo 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_ping_t, domain)        == __builtin_offsetof(hyn_group_ping_t, domain),        "ping.domain 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_ping_t, state)         == __builtin_offsetof(hyn_group_ping_t, state),         "ping.state 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_ping_t, epoch)         == __builtin_offsetof(hyn_group_ping_t, epoch),         "ping.epoch 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_ping_t, resources)     == __builtin_offsetof(hyn_group_ping_t, resources),     "ping.resources 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_ping_t, devices_ready) == __builtin_offsetof(hyn_group_ping_t, devices_ready), "ping.devices_ready 偏移不一致");

_Static_assert(sizeof(hyn_abi_group_info_t) == sizeof(hyn_group_info_t), "INFO 快照结构大小不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, domain)                 == __builtin_offsetof(hyn_group_info_t, domain),                 "info.domain 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, state)                  == __builtin_offsetof(hyn_group_info_t, state),                  "info.state 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, epoch)                  == __builtin_offsetof(hyn_group_info_t, epoch),                  "info.epoch 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, devices_total)          == __builtin_offsetof(hyn_group_info_t, devices_total),          "info.devices_total 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, devices_ready)          == __builtin_offsetof(hyn_group_info_t, devices_ready),          "info.devices_ready 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, resources)              == __builtin_offsetof(hyn_group_info_t, resources),              "info.resources 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, probe_count)            == __builtin_offsetof(hyn_group_info_t, probe_count),            "info.probe_count 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, quiesce_count)          == __builtin_offsetof(hyn_group_info_t, quiesce_count),          "info.quiesce_count 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, reset_count)            == __builtin_offsetof(hyn_group_info_t, reset_count),            "info.reset_count 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, fault_count)            == __builtin_offsetof(hyn_group_info_t, fault_count),            "info.fault_count 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, msg_served)             == __builtin_offsetof(hyn_group_info_t, msg_served),             "info.msg_served 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, msg_discarded)          == __builtin_offsetof(hyn_group_info_t, msg_discarded),          "info.msg_discarded 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, last_fault_vector)      == __builtin_offsetof(hyn_group_info_t, last_fault_vector),      "info.last_fault_vector 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, last_fault_error)       == __builtin_offsetof(hyn_group_info_t, last_fault_error),       "info.last_fault_error 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, last_fault_thread)      == __builtin_offsetof(hyn_group_info_t, last_fault_thread),      "info.last_fault_thread 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, svc_port)               == __builtin_offsetof(hyn_group_info_t, svc_port),               "info.svc_port 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, svc_thread)             == __builtin_offsetof(hyn_group_info_t, svc_thread),             "info.svc_thread 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, name)                   == __builtin_offsetof(hyn_group_info_t, name),                   "info.name 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_group_info_t, last_fault_thread_name) == __builtin_offsetof(hyn_group_info_t, last_fault_thread_name), "info.last_fault_thread_name 偏移不一致");
_Static_assert(sizeof(((hyn_abi_group_info_t *)0)->name) == sizeof(((hyn_group_info_t *)0)->name), "info.name 容量不一致");

_Static_assert(sizeof(hyn_abi_net_probe_t) == sizeof(hyn_net_probe_t), "NET_PROBE 结构大小不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, nic_found)       == __builtin_offsetof(hyn_net_probe_t, nic_found),       "net_probe.nic_found 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, pci_vendor)      == __builtin_offsetof(hyn_net_probe_t, pci_vendor),      "net_probe.pci_vendor 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, pci_device)      == __builtin_offsetof(hyn_net_probe_t, pci_device),      "net_probe.pci_device 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, bar0_size)       == __builtin_offsetof(hyn_net_probe_t, bar0_size),       "net_probe.bar0_size 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, reg_ok)          == __builtin_offsetof(hyn_net_probe_t, reg_ok),          "net_probe.reg_ok 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, mac_hi)          == __builtin_offsetof(hyn_net_probe_t, mac_hi),          "net_probe.mac_hi 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, mac_lo)          == __builtin_offsetof(hyn_net_probe_t, mac_lo),          "net_probe.mac_lo 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, link_up)         == __builtin_offsetof(hyn_net_probe_t, link_up),         "net_probe.link_up 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, ctrl_status_raw) == __builtin_offsetof(hyn_net_probe_t, ctrl_status_raw), "net_probe.ctrl_status_raw 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, cksum_result)    == __builtin_offsetof(hyn_net_probe_t, cksum_result),    "net_probe.cksum_result 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, cksum_expected)  == __builtin_offsetof(hyn_net_probe_t, cksum_expected),  "net_probe.cksum_expected 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_net_probe_t, conns)           == __builtin_offsetof(hyn_net_probe_t, conns),           "net_probe.conns 偏移不一致");
_Static_assert(sizeof(((hyn_abi_net_probe_t *)0)->pci_vendor) == sizeof(u16), "net_probe.pci_vendor 字段宽度不一致");

/* ============================================================
 * 4) 用户入口参数块与记录页布局
 * ============================================================ */
_Static_assert(sizeof(hyn_abi_user_args_t) == 32u, "用户入口参数块大小不一致");
_Static_assert(__builtin_offsetof(hyn_abi_user_args_t, target_port) == 0x00, "参数块 target_port 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_user_args_t, record_va)   == 0x08, "参数块 record_va 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_user_args_t, self_port)   == 0x10, "参数块 self_port 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_user_args_t, arena_va)    == 0x18, "参数块 arena_va 偏移不一致");

_Static_assert(sizeof(hyn_abi_record_t) == 0xC8, "记录页结构大小不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, magic)         == 0x00, "record.magic 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, abi_version)   == 0x08, "record.abi_version 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, kind)          == 0x10, "record.kind 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, tid)           == 0x18, "record.tid 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, cs)            == 0x20, "record.cs 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, cpl_ret)       == 0x28, "record.cpl_ret 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, self_port)     == 0x30, "record.self_port 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, target_port)   == 0x38, "record.target_port 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, ping_ret)      == 0x40, "record.ping_ret 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, ping_echo)     == 0x48, "record.ping_echo 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, ping_domain)   == 0x50, "record.ping_domain 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, ping_msg_id)   == 0x58, "record.ping_msg_id 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, ping_state)    == 0x60, "record.ping_state 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, ping_epoch)    == 0x68, "record.ping_epoch 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, req2_ret)      == 0x70, "record.req2_ret 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, req2_len)      == 0x78, "record.req2_len 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, req2_domain)   == 0x80, "record.req2_domain 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, k0)            == 0x88, "record.k0 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, k1)            == 0x90, "record.k1 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, k2)            == 0x98, "record.k2 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, k3)            == 0xA0, "record.k3 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, recv_to)       == 0xA8, "record.recv_to 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, badptr_ret)    == 0xB0, "record.badptr_ret 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, done)          == 0xB8, "record.done 偏移不一致");
_Static_assert(__builtin_offsetof(hyn_abi_record_t, sys_ping_echo) == 0xC0, "record.sys_ping_echo 偏移不一致");

/* ============================================================
 * 5) 用户程序镜像：由 tools/mkuserimg.py 从 ELF 段表生成（build/include/hyn/userimg.h）
 *    内核把它们作为 const 数组链进自己，运行期整段拷到动态分配的用户页上执行。
 * ============================================================ */
#define HYN_USERPROG_CODE_PAGES_MAX 4u   /* 每个示例程序代码不超过 4 页（16KiB） */

extern const unsigned char g_user_shell_img[];
extern const unsigned long  g_user_shell_img_size;
extern const unsigned char g_user_agent_img[];
extern const unsigned long  g_user_agent_img_size;

#endif /* HYN_USERPROG_H */