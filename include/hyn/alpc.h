/*
 * 混元 OS —— ALPC 端口消息总线接口（硬核底座）
 *
 * ALPC 是全局统一消息主干道：
 *   · syscall 是 Ring3 进入内核的唯一入口，用户在 Ring3 发起的请求最终都变成 ALPC 消息；
 *   · 跨故障域通信强制走 ALPC 消息，禁止跨域直接函数调用（由 hyn_domain_call_guard 把关）；
 *   · 端口是"命名对象"，落在对象管理器里，因此端口的创建/查找/销毁复用同一套句柄与代次规则。
 *
 * 端口命名约定（用于自检与故障域审计）：core.* / storage.* / net.* / bus.*
 * 名字里声明的域必须与该端口的归属域一致，不一致直接拒绝——这样"跨域"这件事
 * 在名字层面就是可见、可查的，而不是藏在调用图里。
 */
#ifndef HYN_ALPC_H
#define HYN_ALPC_H

#include <hyn/types.h>
#include <hyn/object.h>

#define HYN_ALPC_MAX_PORTS   16u              /* 端口对象上限（静态池）：
                                              * 三大分组各一条服务端口 + 四个域各一条跨域请求端口，
                                              * 再给 Ring3 用户态（Shell/Agent）留出余量 */
#define HYN_ALPC_QUEUE_CAP   8u               /* 每端口消息队列深度：满了就背压 */
#define HYN_ALPC_MAX_CONNS   16u              /* 每端口允许的已建链对端数 */
#define HYN_ALPC_MSG_MAGIC   0x504E5948u      /* 内存中读作 "HYNP" */

/* 消息类型（业务层可继续扩展） */
#define HYN_ALPC_MSG_REQUEST 0x00000001u
#define HYN_ALPC_MSG_REPLY   0x00000002u
#define HYN_ALPC_MSG_NOTIFY  0x00000003u

typedef struct hyn_alpc_msg {
    u32 magic;
    u32 msg_id;             /* 发送序号：应答原样回填，便于请求/应答配对 */
    u32 type;
    u32 flags;
    u32 sender_id;          /* 发送端点号（端口存活期内唯一，创建时由内核发号） */
    u8  sender_domain;
    u8  target_domain;
    u8  reserved0[2];
    u32 len;                /* data 中有效字节数（<= HYN_MSG_MAX） */
    u32 reserved1;
    u64 reply_to;           /* 应答应发往的端口 id；0 表示无需应答 */
    u8  data[HYN_MSG_MAX];
} hyn_alpc_msg_t;

/* 派发回调：返回后消息即被消费（本里程碑不做"处理失败则重投") */
typedef void (*hyn_alpc_handler_t)(u64 port_id, const hyn_alpc_msg_t *msg, void *ctx);

hyn_status_t hyn_alpc_init(void);

/* 创建端口：名字与 domain 必须一致；返回端口对象 id（内核内部以此为端口身份） */
hyn_status_t hyn_alpc_port_create(const char *name, hyn_domain_t domain, u64 *out_port_id);

/* 按名打开端口：返回端口 id 与归属域（内核内部使用；用户态句柄路径在 syscall 里程碑提供） */
hyn_status_t hyn_alpc_port_open(const char *name, u64 *out_port_id, hyn_domain_t *out_domain);

/*
 * 建链：单向建立"本端口可以向对端端口投递"的许可；跨域建链会打印审计行。
 * 参数统一用端口对象 id（与 create/open/send/receive 一致）——端点号是内核内部概念，
 * 只出现在消息体的 sender_id 里，调用方不必也不能自行推算。
 * 请求/应答这类双向会话需建两条链（每个方向各一条）。
 */
hyn_status_t hyn_alpc_connect(u64 port_id, u64 peer_port_id);
hyn_status_t hyn_alpc_disconnect(u64 port_id, u64 peer_port_id);
int          hyn_alpc_is_connected(u64 port_id, u64 peer_port_id);

/* 端点号：填消息 sender_id 用；端口不存在返回 0 */
u32          hyn_alpc_endpoint_of(u64 port_id);

/*
 * 投递 / 收取：空队列收取返回 HYN_ENOENT，满队列投递返回 HYN_EBUSY（背压）。
 * 投递许可在投递口强制校验：跨域投递必须先建链，未建链返回 HYN_EDOMAIN 并计入拦截审计；
 * 同域本就允许直接调用，故不强制建链。消息必须携带有效发送端点号。
 */
hyn_status_t hyn_alpc_send(u64 port_id, const hyn_alpc_msg_t *msg);
hyn_status_t hyn_alpc_receive(u64 port_id, hyn_alpc_msg_t *out_msg);
u32          hyn_alpc_pending(u64 port_id);

/*
 * 阻塞收取（随调度器里程碑接入）：队列空时把当前线程挂到 chan=(void*)port_id 上，
 * 由投递方在入队后唤醒；返回 HYN_OK 表示收到，HYN_ETIMEDOUT 表示等到超时。
 * 非阻塞语义（hyn_alpc_receive）保留不变：内核内部轮询派发路径仍用非阻塞版本。
 */
hyn_status_t hyn_alpc_receive_wait(u64 port_id, hyn_alpc_msg_t *out_msg, u64 timeout_ticks);

/* 一轮派发：把各端口队列里的消息交给 handler；返回本轮派发条数 */
u32          hyn_alpc_dispatch(hyn_alpc_handler_t handler, void *ctx);

hyn_domain_t hyn_alpc_port_domain(u64 port_id);   /* 端口不存在时返回 HYN_DOMAIN_MAX */
int          hyn_alpc_port_alive(u64 port_id);

/* 消息构造辅助（sender_id 由调用方用 hyn_alpc_endpoint_of 填入） */
void hyn_alpc_msg_init(hyn_alpc_msg_t *msg, u32 type, u32 msg_id,
                       u8 sender_domain, u8 target_domain,
                       const void *payload, u32 len);

/*
 * 故障域调用纪律：同一域内允许直接调用；跨域只能经 ALPC 消息。
 * 返回 HYN_EDOMAIN 表示这次跨域直调被拒绝。
 */
hyn_status_t hyn_domain_call_guard(hyn_domain_t caller, hyn_domain_t target, int via_alpc);

u32 hyn_alpc_port_count(void);
u32 hyn_alpc_msg_count(void);
u32 hyn_domain_blocked_count(void);      /* 被拒绝的跨域直调次数（审计用） */
u32 hyn_alpc_permit_denied_count(void);  /* 被拒绝的"未建链跨域投递"次数（审计用） */

#endif /* HYN_ALPC_H */