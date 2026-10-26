/*
 * HynOS —— Ring3 自检探针的接口约定（用户地址窗口 / 记录结构 / 探针符号）
 *
 * 这不是内核 API，而是"内核与自检用用户态程序之间的约定"：
 *   · 探针程序运行在 Ring3（CPL=3），只能通过 int 0x80 与内核说话；
 *   · 内核把探针的代码页/栈页/记录页映射进用户地址窗口（U=1），再 iretq 送它进去；
 *   · 记录页由用户态程序写、内核读——它的内容就是"Ring3 真的跑过"的证据，
 *     而且记录里放的是程序自己读到的 %cs、收到的消息字段，内核无法代它伪造。
 *
 * 用户地址窗口：内核恒等映射只覆盖 0..4GiB 且为 supervisor 页，用户窗口另开在
 * 高位（0x40_0000_0000 起），两者不可能重叠——Ring3 越权摸内核页必然 #PF。
 */
#ifndef HYN_RING3_H
#define HYN_RING3_H

#include <hyn/types.h>

#define HYN_USER_BASE   0x0000004000000000ull   /* 用户地址窗口基址 */
#define HYN_USER_TOP    0x0000004040000000ull   /* 窗口上限（1GiB，够本里程碑用） */

/* 用户程序记录页里的收尾标记：只有跑完整条流程才会写 */
#define HYN_RING3_REC_MAGIC  0x334E4952ull      /* 内存中读作 "RIN3" */
#define HYN_RING3_DONE       0x00005A5Aull

/* 探针自己填的 %cs（RPL3 的用户代码段选择子） */
#define HYN_RING3_USER_CS    0x2Bull

/* 记录页内的消息缓冲区偏移（用户程序在这里拼请求/收应答） */
#define HYN_RING3_MSG_OFF    0x100u

/*
 * 记录结构：全部 u64 顺序排列，偏移与 arch/x86_64/ring3_prog.S 里的 .equ 常量一致，
 * 由下面一组静态断言把关（改布局忘了改汇编会在编译期直接报错）。
 */
typedef struct hyn_ring3_record {
    u64 magic;          /* 0x00  "RIN3"：说明程序至少跑到了第一条记录写入 */
    u64 tid_ret;        /* 0x08  SYS_GET_TID 返回的线程 id */
    u64 cpl_arg;        /* 0x10  程序读到的 %cs（交给内核核对，自己说的不算） */
    u64 cpl_ret;        /* 0x18  SYS_CHECK_CPL 返回值（0 = 与内核帧一致） */
    u64 badptr_ret;     /* 0x20  传内核地址当用户缓冲区的返回值（期望 EFAULT） */
    u64 send_ret;       /* 0x28  请求投递返回值（0 = 成功） */
    u64 foreign_ret;    /* 0x30  尝试收他人端口消息的返回值（期望 EPERM） */
    u64 recv_ret;       /* 0x38  阻塞收取应答的返回值（0 = 收到，10 = 超时） */
    u64 reply_type;     /* 0x40  应答类型（期望 REPLY） */
    u64 reply_msg_id;   /* 0x48  应答序号（期望原样回填） */
    u64 reply_domain;   /* 0x50  应答来源域（期望 NET） */
    u64 reply_len;      /* 0x58  应答载荷长度 */
    u64 reply_word;     /* 0x60  应答载荷前 4 字节按小端读作整数 */
    u64 done;           /* 0x68  收尾标记 0x5A5A */
} hyn_ring3_record_t;

_Static_assert(__builtin_offsetof(hyn_ring3_record_t, tid_ret)      == 0x08, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, cpl_arg)      == 0x10, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, cpl_ret)      == 0x18, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, badptr_ret)   == 0x20, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, send_ret)     == 0x28, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, foreign_ret)  == 0x30, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, recv_ret)     == 0x38, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, reply_type)   == 0x40, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, reply_msg_id) == 0x48, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, reply_domain) == 0x50, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, reply_len)    == 0x58, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, reply_word)   == 0x60, "ring3 记录布局与汇编不一致");
_Static_assert(__builtin_offsetof(hyn_ring3_record_t, done)         == 0x68, "ring3 记录布局与汇编不一致");
_Static_assert(sizeof(hyn_ring3_record_t) == 0x70, "ring3 记录布局与汇编不一致");

/*
 * 探针程序（arch/x86_64/ring3_prog.S，收在 .text.userprog 段里，内核构建期一并装载）：
 *   good  正常流程：自省 CPL -> 取线程 id -> 非法指针负向 -> 伪造身份发请求 -> 阻塞收应答
 *   fault 越权流程：第一条访存就去读内核地址，用于验证"用户异常只杀线程"
 */
extern const u8 __ring3_probe_good_start[];
extern const u8 __ring3_probe_good_end[];
extern const u8 __ring3_probe_fault_start[];
extern const u8 __ring3_probe_fault_end[];

#endif /* HYN_RING3_H */