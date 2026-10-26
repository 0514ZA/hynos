/*
 * 混元 OS —— ALPC 端口消息总线（纯 C，架构无关，硬核底座）
 *
 * 本文件只做"消息主干道"：端口命名对象、连接许可、队列、收发、派发、跨域纪律。
 * 不掺业务语义（存储/网络/Agent 的语义由各自域的用户态程序承载）。
 *
 * 实现取舍（如实标注，不夸大）：
 *   · 端口体来自静态池（g_ports），对象管理器只挂名与句柄，尚未接入对象专用分配器；
 *   · 队列为定长环形缓冲，满则背压返回 HYN_EBUSY——宁可让发送方知道"堵了"，
 *     也不做静默丢消息这种会掩盖问题的行为；
 *   · 派发是"轮询一轮"（内核当前还没有等待队列；阻塞等待随调度器里程碑接入）。
 */
#include <hyn/alpc.h>
#include <hyn/object.h>
#include <hyn/thread.h>          /* 阻塞收取要挂到调度器的 chan 上（对应的头文件不反向依赖线程） */
#include <hyn/kprintf.h>
#include <hyn/kstring.h>

typedef struct alpc_port {
    u8  used;
    u8  domain;
    u16 reserved0;
    u32 endpoint;                       /* 域内端点号 = 对象 id 低 32 位 */
    u32 head;                           /* 队列头（收取位置） */
    u32 tail;                           /* 队列尾（投递位置） */
    u32 count;
    u32 conn_count;
    u32 conns[HYN_ALPC_MAX_CONNS];      /* 已建链对端端点号 */
    u64 object_id;
    u64 name_hash;
    char name[HYN_NAME_MAX];
    hyn_alpc_msg_t queue[HYN_ALPC_QUEUE_CAP];
} alpc_port_t;

static alpc_port_t g_ports[HYN_ALPC_MAX_PORTS];
static u32 g_port_count;                /* 当前存活端口数 */
static u32 g_msg_count;                 /* 累计投递消息数 */
static u32 g_blocked_count;             /* 被拒绝的跨域直调次数 */
static u32 g_permit_denied_count;       /* 被拒绝的"未建链跨域投递"次数（审计用） */
static u32 g_endpoint_seq;              /* 端点号发号器：单调递增，绝不复用 */
static int g_class_ready;

/* 端口命名域前缀：名字里声明的域必须与端口归属域一致 */
static const char *const g_domain_prefixes[HYN_DOMAIN_MAX] = {
    "core.", "storage.", "net.", "bus."
};

static const char *domain_name(hyn_domain_t d)
{
    switch (d) {
    case HYN_DOMAIN_CORE:    return "硬核底座";
    case HYN_DOMAIN_STORAGE: return "存储组";
    case HYN_DOMAIN_NET:     return "网络组";
    case HYN_DOMAIN_BUS:     return "硬件总线组";
    default:                 return "未知域";
    }
}

/* 从端口名解析出域；无法解析返回 HYN_DOMAIN_MAX */
static hyn_domain_t domain_from_name(const char *name)
{
    u8 i;

    for (i = 0u; i < (u8)HYN_DOMAIN_MAX; i++) {
        unsigned long n = strlen(g_domain_prefixes[i]);
        if (strncmp(name, g_domain_prefixes[i], n) == 0) {
            return (hyn_domain_t)i;
        }
    }
    return HYN_DOMAIN_MAX;
}

static alpc_port_t *port_lookup(u64 port_id)
{
    u32 i;

    if (port_id == 0ull) {
        return NULL;
    }
    for (i = 0; i < HYN_ALPC_MAX_PORTS; i++) {
        if (g_ports[i].used && g_ports[i].object_id == port_id) {
            return &g_ports[i];
        }
    }
    return NULL;
}

/*
 * 按端点号找端口。
 * 端点号由创建时的发号器分配，而不是取"对象 id 的低 32 位"——后者会让两个端口撞号，
 * 而连接许可以及销毁时的许可撤销都是按端点号匹配的，撞号就会误伤别的端口。
 */
static alpc_port_t *port_by_endpoint(u32 endpoint)
{
    u32 i;

    if (endpoint == 0u) {
        return NULL;
    }
    for (i = 0; i < HYN_ALPC_MAX_PORTS; i++) {
        if (g_ports[i].used && g_ports[i].endpoint == endpoint) {
            return &g_ports[i];
        }
    }
    return NULL;
}

static int conn_index(const alpc_port_t *p, u32 endpoint)
{
    u32 i;

    for (i = 0; i < p->conn_count; i++) {
        if (p->conns[i] == endpoint) {
            return (int)i;
        }
    }
    return -1;
}

/* 端口对象被销毁时的清理：把其它端口里指向本端点的连接许可一并撤掉 */
static void alpc_port_on_destroy(void *body, void *ctx)
{
    alpc_port_t *p = (alpc_port_t *)body;
    u32 i;

    HYN_UNUSED(ctx);
    if (p == NULL) {
        return;
    }
    for (i = 0; i < HYN_ALPC_MAX_PORTS; i++) {
        alpc_port_t *q = &g_ports[i];
        int idx;
        if (!q->used) {
            continue;
        }
        idx = conn_index(q, p->endpoint);
        if (idx >= 0) {
            q->conns[(u32)idx] = q->conns[q->conn_count - 1u];
            q->conn_count--;
        }
    }
    if (g_port_count > 0u) {
        g_port_count--;
    }
}

hyn_status_t hyn_alpc_init(void)
{
    hyn_status_t st;

    memset(g_ports, 0, (unsigned long)sizeof(g_ports));
    g_port_count  = 0u;
    g_msg_count   = 0u;
    g_blocked_count = 0u;
    g_permit_denied_count = 0u;
    g_endpoint_seq = 0u;

    st = hyn_class_register(HYN_OBJ_TYPE_ALPC_PORT, "ALPC 端口",
                            alpc_port_on_destroy, NULL);
    if (st != HYN_OK) {
        return st;
    }
    g_class_ready = 1;
    return HYN_OK;
}

hyn_status_t hyn_alpc_port_create(const char *name, hyn_domain_t domain, u64 *out_port_id)
{
    hyn_status_t st;
    u64 obj_id = 0ull;
    hyn_handle_t handle = 0ull;
    u32 i, slot = HYN_ALPC_MAX_PORTS;

    if (!g_class_ready || name == NULL || out_port_id == NULL) {
        return HYN_EINVAL;
    }
    if ((u32)domain >= HYN_DOMAIN_MAX) {
        return HYN_EINVAL;
    }
    if (domain_from_name(name) != domain) {
        return HYN_EINVAL;                /* 名字声明的域与归属域不一致 */
    }
    for (i = 0; i < HYN_ALPC_MAX_PORTS; i++) {
        if (!g_ports[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == HYN_ALPC_MAX_PORTS) {
        return HYN_ENOMEM;
    }

    st = hyn_object_create(HYN_OBJ_TYPE_ALPC_PORT, name, domain, &g_ports[slot],
                           (u32)sizeof(alpc_port_t), &obj_id, &handle);
    if (st != HYN_OK) {
        return st;
    }

    /* 对象管理器给出"新信封"，端口逻辑在这里装填并登记 */
    memset(&g_ports[slot], 0, (unsigned long)sizeof(alpc_port_t));
    g_ports[slot].used      = 1u;
    g_ports[slot].domain    = (u8)domain;
    g_ports[slot].endpoint  = ++g_endpoint_seq;   /* 端点号：存活期内唯一，不复用 */
    g_ports[slot].object_id = obj_id;
    g_ports[slot].name_hash = kstr_hash(name);
    strncpy_safe(g_ports[slot].name, name, HYN_NAME_MAX);

    /*
     * 创建期句柄即取即还：端口体在内核静态池里，生命周期由内核显式销毁管理
     * （hyn_object_destroy 会在"无任何打开句柄"时才允许销毁）。用户态要拿句柄
     * 走 syscall 路径（T5）后再由权限层决定是否发放，内核内部不长期持有句柄。
     */
    (void)hyn_object_close(handle);
    g_port_count++;
    *out_port_id = obj_id;
    return HYN_OK;
}

hyn_status_t hyn_alpc_port_open(const char *name, u64 *out_port_id, hyn_domain_t *out_domain)
{
    hyn_handle_t h = 0ull;
    hyn_handle_info_t info;
    hyn_status_t st;
    alpc_port_t *p;

    if (name == NULL || out_port_id == NULL) {
        return HYN_EINVAL;
    }
    st = hyn_object_open(name, &h);
    if (st != HYN_OK) {
        return HYN_ENOENT;
    }
    if (hyn_handle_info(h, &info) != HYN_OK) {
        (void)hyn_object_close(h);
        return HYN_ENOENT;
    }
    (void)hyn_object_close(h);           /* 内核内部只需 id，句柄即取即还 */

    p = port_lookup(info.object_id);
    if (p == NULL) {
        return HYN_ENOENT;
    }
    if (domain_from_name(name) != (hyn_domain_t)p->domain) {
        return HYN_EFAULT;               /* 打开时也要核对域声明 */
    }
    if (out_domain != NULL) {
        *out_domain = (hyn_domain_t)p->domain;
    }
    *out_port_id = info.object_id;
    return HYN_OK;
}

hyn_status_t hyn_alpc_connect(u64 port_id, u64 peer_port_id)
{
    alpc_port_t *p = port_lookup(port_id);
    alpc_port_t *peer;
    hyn_status_t g;
    int cross;

    if (p == NULL) {
        return HYN_ENOENT;
    }
    peer = port_lookup(peer_port_id);
    if (peer == NULL) {
        return HYN_ENOENT;
    }
    if (peer == p) {
        return HYN_EFAULT;
    }
    if (p->conn_count >= HYN_ALPC_MAX_CONNS) {
        return HYN_ENOMEM;
    }
    if (conn_index(p, peer->endpoint) >= 0) {
        return HYN_EBUSY;
    }

    /* 跨域建链本身就是"经 ALPC 建立通路"，故通过；这里做审计与纪律校验 */
    g = hyn_domain_call_guard((hyn_domain_t)p->domain, (hyn_domain_t)peer->domain, 1);
    if (g != HYN_OK) {
        return g;
    }
    cross = (p->domain != peer->domain) ? 1 : 0;

    p->conns[p->conn_count] = peer->endpoint;
    p->conn_count++;

    if (cross) {
        kprintf("[ALPC] 跨域建链：%s(%s) -> %s(%s)\n",
                p->name, domain_name((hyn_domain_t)p->domain),
                peer->name, domain_name((hyn_domain_t)peer->domain));
    }
    return HYN_OK;
}

hyn_status_t hyn_alpc_disconnect(u64 port_id, u64 peer_port_id)
{
    alpc_port_t *p = port_lookup(port_id);
    alpc_port_t *peer = port_lookup(peer_port_id);
    int idx;

    if (p == NULL || peer == NULL) {
        return HYN_ENOENT;
    }
    idx = conn_index(p, peer->endpoint);
    if (idx < 0) {
        return HYN_ENOENT;
    }
    p->conns[(u32)idx] = p->conns[p->conn_count - 1u];
    p->conn_count--;
    return HYN_OK;
}

int hyn_alpc_is_connected(u64 port_id, u64 peer_port_id)
{
    alpc_port_t *p = port_lookup(port_id);
    alpc_port_t *peer = port_lookup(peer_port_id);

    if (p == NULL || peer == NULL) {
        return 0;
    }
    return (conn_index(p, peer->endpoint) >= 0) ? 1 : 0;
}

hyn_status_t hyn_alpc_send(u64 port_id, const hyn_alpc_msg_t *msg)
{
    alpc_port_t *p = port_lookup(port_id);

    if (p == NULL || msg == NULL) {
        return HYN_ENOENT;
    }
    if (msg->magic != HYN_ALPC_MSG_MAGIC) {
        return HYN_EINVAL;
    }
    if (msg->len > HYN_MSG_MAX) {
        return HYN_EINVAL;
    }
    if ((u32)msg->sender_domain >= HYN_DOMAIN_MAX) {
        return HYN_EINVAL;
    }
    /*
     * 投递许可：跨域投递必须先建链。否则"跨故障域强制走 ALPC 消息"这条纪律在投递口
     * 落不了地——建链就只是记账，任意端口都能给任意端口发消息。同域本就允许直接调用，
     * 故不强制建链。被拒绝的次数单独计数，便于审计"谁想越界说话"。
     */
    if ((hyn_domain_t)msg->sender_domain != (hyn_domain_t)p->domain) {
        alpc_port_t *src = port_by_endpoint(msg->sender_id);
        if (src == NULL || conn_index(src, p->endpoint) < 0) {
            g_permit_denied_count++;
            return HYN_EDOMAIN;
        }
    }
    if (p->count >= HYN_ALPC_QUEUE_CAP) {
        return HYN_EBUSY;                /* 背压：如实告诉发送方堵了，不静默丢弃 */
    }
    p->queue[p->tail] = *msg;
    p->tail = (p->tail + 1u) % HYN_ALPC_QUEUE_CAP;
    p->count++;
    g_msg_count++;

    /*
     * 入队后唤醒在该端口上阻塞等待的线程（chan 就用端口 id 本身）。
     * 顺序必须是"先入队、后唤醒"：被唤醒的一方重试收取时一定能拿到消息，
     * 不会出现"醒来发现还是空、只好再睡"的空转。此调用在无调度器（自检早期）
     * 或无人阻塞时是安全的空操作——它只在真正有 BLOCKED 线程时才动状态。
     */
    (void)hyn_sched_wake_chan((void *)(uptr)port_id, 1u);
    return HYN_OK;
}

/*
 * 阻塞收取（调度器接入后的语义）：队列为空就把当前线程挂到 chan=(void*)port_id 上，
 * 由投递方在入队后唤醒。超时返回 HYN_ETIMEDOUT；被唤醒后重试收取，仍空则按剩余
 * 时间继续等——伪唤醒与"消息被别的消费者抢走"都被这里自然吸收，调用方不必自写重试循环。
 *
 * 与非阻塞版的关系：hyn_alpc_receive 保留原样，内核内部轮询派发
 * （hyn_alpc_dispatch）与"只探一下有没有"的场景继续用它，两者不互相消费。
 * timeout_ticks==0 表示不限时等待（仅用于确实不该被超时打断的内核用例）。
 */
hyn_status_t hyn_alpc_receive_wait(u64 port_id, hyn_alpc_msg_t *out_msg, u64 timeout_ticks)
{
    u64 deadline;
    int woken;

    if (out_msg == NULL) {
        return HYN_EINVAL;
    }
    if (port_lookup(port_id) == NULL) {
        return HYN_ENOENT;
    }
    if (hyn_alpc_receive(port_id, out_msg) == HYN_OK) {
        return HYN_OK;                 /* 已有消息：不必进调度器，直接拿走 */
    }
    if (!hyn_sched_ready()) {
        return HYN_ENOENT;             /* 调度器未接入：如实返回"没有消息"，不假装阻塞 */
    }

    deadline = (timeout_ticks == 0ull) ? 0ull : (hyn_sched_ticks() + timeout_ticks);
    for (;;) {
        u64 left = 0ull;

        if (deadline != 0ull) {
            u64 now = hyn_sched_ticks();
            if (now >= deadline) {
                return HYN_ETIMEDOUT;
            }
            left = deadline - now;
        }
        woken = hyn_sched_block_timeout((void *)(uptr)port_id, left);
        if (hyn_alpc_receive(port_id, out_msg) == HYN_OK) {
            return HYN_OK;
        }
        if (woken == HYN_SCHED_TIMEOUT) {
            return HYN_ETIMEDOUT;
        }
    }
}

hyn_status_t hyn_alpc_receive(u64 port_id, hyn_alpc_msg_t *out_msg)
{
    alpc_port_t *p = port_lookup(port_id);

    if (p == NULL || out_msg == NULL) {
        return HYN_ENOENT;
    }
    if (p->count == 0u) {
        return HYN_ENOENT;
    }
    *out_msg = p->queue[p->head];
    p->head = (p->head + 1u) % HYN_ALPC_QUEUE_CAP;
    p->count--;
    return HYN_OK;
}

u32 hyn_alpc_pending(u64 port_id)
{
    alpc_port_t *p = port_lookup(port_id);

    return (p == NULL) ? 0u : p->count;
}

u32 hyn_alpc_dispatch(hyn_alpc_handler_t handler, void *ctx)
{
    u32 i, dispatched = 0u;

    for (i = 0; i < HYN_ALPC_MAX_PORTS; i++) {
        alpc_port_t *p = &g_ports[i];

        if (!p->used) {
            continue;
        }
        while (p->count > 0u) {
            hyn_alpc_msg_t msg = p->queue[p->head];
            p->head = (p->head + 1u) % HYN_ALPC_QUEUE_CAP;
            p->count--;
            dispatched++;

            kprintf("[ALPC] 端口 %s 收到消息：类型=0x%x 长度=%u 来源域=%s 目标域=%s 跨域=%u\n",
                    p->name, msg.type, msg.len,
                    domain_name((hyn_domain_t)msg.sender_domain),
                    domain_name((hyn_domain_t)msg.target_domain),
                    (msg.sender_domain != msg.target_domain) ? 1u : 0u);
            if (handler != NULL) {
                handler(p->object_id, &msg, ctx);
            }
        }
    }
    return dispatched;
}

hyn_domain_t hyn_alpc_port_domain(u64 port_id)
{
    alpc_port_t *p = port_lookup(port_id);

    return (p == NULL) ? HYN_DOMAIN_MAX : (hyn_domain_t)p->domain;
}

int hyn_alpc_port_alive(u64 port_id)
{
    return (port_lookup(port_id) != NULL) ? 1 : 0;
}

u32 hyn_alpc_endpoint_of(u64 port_id)
{
    alpc_port_t *p = port_lookup(port_id);

    return (p == NULL) ? 0u : p->endpoint;
}

u32 hyn_alpc_permit_denied_count(void)
{
    return g_permit_denied_count;
}

void hyn_alpc_msg_init(hyn_alpc_msg_t *msg, u32 type, u32 msg_id,
                       u8 sender_domain, u8 target_domain,
                       const void *payload, u32 len)
{
    if (msg == NULL) {
        return;
    }
    memset(msg, 0, (unsigned long)sizeof(*msg));
    msg->magic         = HYN_ALPC_MSG_MAGIC;
    msg->msg_id        = msg_id;
    msg->type          = type;
    msg->sender_domain = sender_domain;
    msg->target_domain = target_domain;
    if (len > HYN_MSG_MAX) {
        len = HYN_MSG_MAX;
    }
    msg->len = len;
    if (payload != NULL && len > 0u) {
        memcpy(msg->data, payload, (unsigned long)len);
    }
}

hyn_status_t hyn_domain_call_guard(hyn_domain_t caller, hyn_domain_t target, int via_alpc)
{
    if ((u32)caller >= HYN_DOMAIN_MAX || (u32)target >= HYN_DOMAIN_MAX) {
        return HYN_EINVAL;
    }
    if (caller == target) {
        return HYN_OK;                   /* 同域：允许直接调用 */
    }
    if (via_alpc) {
        return HYN_OK;                   /* 跨域：必须经 ALPC 消息 */
    }
    g_blocked_count++;
    return HYN_EDOMAIN;                  /* 跨域直调：拒绝 */
}

u32 hyn_alpc_port_count(void)
{
    return g_port_count;
}

u32 hyn_alpc_msg_count(void)
{
    return g_msg_count;
}

u32 hyn_domain_blocked_count(void)
{
    return g_blocked_count;
}