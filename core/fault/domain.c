/*
 * HynOS —— 快通道分组故障域框架（硬核底座上的隔离纪律）
 *
 * 这个文件是"分组故障域"的骨架：分组注册、端口与建链、服务线程、异常捕获、单独重置、
 * 以及"跨域只能走 ALPC 消息"这条纪律的强制点。
 *
 * 三条实现纪律（与 include/hyn/fault.h 的说明一致）：
 *   1) 跨域调用强制走 ALPC：hyn_domain_call 是"请求消息 -> 对方服务线程处理 -> 应答消息"的往返；
 *      同域直调走 hyn_group_local_call，跨域一律 HYN_EDOMAIN（并计入审计）。
 *   2) 组内异常只影响本组：hyn_domain_fault_hook 只结束出错的线程 + 标记本组 FAULTED + 置待重置；
 *      底座（CORE）线程的异常绝不在这里吞掉，交回陷阱框架如实停机。
 *   3) 重置协议固定 quiesce -> reset -> probe：先静默、再重认领硬件、最后重建软件与服务，
 *      完成时 epoch + 1（调用方据此判断"这是重启过的组"）。
 *
 * 服务线程模型（本轮取舍，如实说明）：
 *   · 每组一条服务线程，按 5 个节拍轮询自己的服务端口；收到请求就地处理并回一条应答；
 *   · 串行调用模型：同一域内的跨域调用共用一条请求端口，应答按 msg_id 配对，
 *     配不上的一律计入 msg_discarded（不静默丢弃）；
 *   · CRASH 请求会让服务线程在异常里退出，因此应答不会发出——调用方拿到的是
 *     "对方不可用"的明确结论（FAULTED -> HYN_EFAULT_FAULT），而不是被挂到超时。
 */
#include <hyn/fault.h>
#include <hyn/alpc.h>
#include <hyn/thread.h>
#include <hyn/kprintf.h>
#include <hyn/kstring.h>

#define HYN_DOMAIN_SVC_WAIT_TICKS      5ull    /* 服务线程轮询间隔：50ms @100Hz */
#define HYN_DOMAIN_QUIESCE_WAIT_TICKS  60ull   /* 等旧服务线程退出的上限（0.6s） */
#define HYN_DOMAIN_REPLY_TIMEOUT_TICKS 60ull   /* 跨域应答等待上限（0.6s） */
#define HYN_DOMAIN_REPLY_SLICE_TICKS   5ull    /* 应答等待分片：便于期间复查目标组状态 */

/*
 * 故障注入地址：必须同时满足两条，否则触发不了页错误：
 *   1) 规范地址（canonical）——高 16 位必须全 0 或全 1，否则先撞 #GP(13) 而不是 #PF(14)；
 *   2) 页表里未映射——恒等映射只覆盖 [0, 4GiB)，用户窗口在 [256GiB, 257GiB)，
 *      故取 4GiB 这个左闭右开的边界点：规范、非恒等、非用户窗口，且 PDPT 项为 0。
 */
#define HYN_DOMAIN_CRASH_ADDR 0x0000000100000000ull

/* 故意解引用未映射地址：用于验证本组异常捕获真的生效（不会返回） */
static void domain_crash(void);

typedef struct domain_slot {
    u8   used;                  /* 已注册（CORE 由框架直接建立） */
    u8   state;                 /* hyn_domain_state_t */
    u8   domain;                /* hyn_domain_t */
    u8   reserved0;
    u32  epoch;                 /* 每重置一次自增：第几代 */
    u32  probe_count;
    u32  quiesce_count;
    u32  reset_count;
    u32  fault_count;
    u32  msg_served;
    u32  msg_discarded;
    u32  resources;
    u32  devices_total;
    u32  devices_ready;
    u32  last_fault_vector;
    u32  last_fault_error;
    u64  last_fault_thread;
    u64  req_port;              /* 本域的跨域请求端口（应答回到这里） */
    u64  svc_port;              /* 本域的服务端口（CORE 无） */
    u64  svc_thread_id;
    hyn_thread_t *svc_thread;
    volatile u32 stop_svc;
    volatile u32 pending_reset;
    const hyn_domain_ops_t *ops;
    char name[16];
    char last_fault_thread_name[16];
    hyn_group_info_t info;      /* 取证访问器用的快照（每次读取重建） */
} domain_slot_t;

static domain_slot_t g_domains[HYN_DOMAIN_MAX];

static u32  g_msg_seq;
static u32  g_call_total;
static u32  g_call_denied;
static u32  g_msg_discarded;
static u32  g_fault_total;
static char g_last_msg[128];

static const char *const g_svc_port_names[HYN_DOMAIN_MAX] = {
    NULL, "storage.svc", "net.svc", "bus.svc"
};
static const char *const g_req_port_names[HYN_DOMAIN_MAX] = {
    "core.req", "storage.req", "net.req", "bus.req"
};
static const char *const g_svc_thread_names[HYN_DOMAIN_MAX] = {
    NULL, "svc.storage", "svc.net", "svc.bus"
};
static const char *const g_short_names[HYN_DOMAIN_MAX] = {
    "CORE", "STOR", "NET", "BUS"
};

static const char *short_name(hyn_domain_t d)
{
    return ((u32)d < (u32)HYN_DOMAIN_MAX) ? g_short_names[d] : "?";
}

static void audit_call(hyn_domain_t caller, hyn_domain_t target, u32 op, hyn_status_t st)
{
    (void)ksnprintf(g_last_msg, (u64)sizeof(g_last_msg),
                    "%s->%s op=0x%x st=%u", short_name(caller), short_name(target),
                    (u32)op, (u32)st);
}

/* ---- 内部：槽位查找与状态名 ---- */

static domain_slot_t *slot_of(hyn_domain_t d)
{
    if ((u32)d >= (u32)HYN_DOMAIN_MAX) {
        return NULL;
    }
    return &g_domains[(u32)d];
}

const char *hyn_domain_state_name(hyn_domain_state_t s)
{
    switch (s) {
    case HYN_DOMAIN_STATE_OFFLINE:   return "未启动";
    case HYN_DOMAIN_STATE_ONLINE:    return "在线";
    case HYN_DOMAIN_STATE_FAULTED:   return "故障(待重置)";
    case HYN_DOMAIN_STATE_RESETTING: return "重置中";
    default:                         return "未知";
    }
}

const char *hyn_domain_name(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    if (s == NULL || s->used == 0u) {
        return "未注册";
    }
    return s->name;
}

int hyn_domain_registered(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    return (s != NULL && s->used != 0u) ? 1 : 0;
}

hyn_thread_t *hyn_domain_svc_thread(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    return (s == NULL) ? NULL : s->svc_thread;
}

u64 hyn_domain_svc_port(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    return (s == NULL) ? 0ull : s->svc_port;
}

u32 hyn_domain_fault_total(void)    { return g_fault_total; }
u32 hyn_domain_call_total(void)     { return g_call_total; }
u32 hyn_domain_call_denied(void)    { return g_call_denied; }
u32 hyn_domain_msg_discarded(void)  { return g_msg_discarded; }
const char *hyn_domain_last_msg(void) { return g_last_msg; }

void hyn_domain_report(hyn_domain_t d, u32 resources, u32 devices_total, u32 devices_ready)
{
    domain_slot_t *s = slot_of(d);

    if (s == NULL) {
        return;
    }
    s->resources     = resources;
    s->devices_total = devices_total;
    s->devices_ready = devices_ready;
}

static void fill_info(const domain_slot_t *s, hyn_group_info_t *out)
{
    memset(out, 0, (unsigned long)sizeof(*out));
    out->domain           = (u32)s->domain;
    out->state            = (u32)s->state;
    out->epoch            = s->epoch;
    out->devices_total    = s->devices_total;
    out->devices_ready    = s->devices_ready;
    out->resources        = s->resources;
    out->probe_count      = s->probe_count;
    out->quiesce_count    = s->quiesce_count;
    out->reset_count      = s->reset_count;
    out->fault_count      = s->fault_count;
    out->msg_served       = s->msg_served;
    out->msg_discarded    = s->msg_discarded;
    out->last_fault_vector = s->last_fault_vector;
    out->last_fault_error  = s->last_fault_error;
    out->last_fault_thread = s->last_fault_thread;
    out->svc_port          = s->svc_port;
    out->svc_thread        = s->svc_thread_id;
    strncpy_safe(out->name, s->name, 16);
    strncpy_safe(out->last_fault_thread_name, s->last_fault_thread_name, 16);
}

const hyn_group_info_t *hyn_domain_info(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    if (s == NULL) {
        return NULL;
    }
    fill_info(s, &s->info);
    return &s->info;
}

/* ---- 组内请求处理 ---- */

static hyn_status_t domain_execute(domain_slot_t *s, u32 op,
                                   const u8 *in, u32 in_len, u8 *out, u32 *out_len)
{
    if (op == HYN_GROUP_OP_PING) {
        hyn_group_ping_t p;

        if (out == NULL || *out_len < (u32)sizeof(p)) {
            return HYN_EBUSY;
        }
        memset(&p, 0, (unsigned long)sizeof(p));
        if (in != NULL && in_len >= 4u) {
            memcpy(&p.op_echo, in, 4u);      /* 原样回填：核对参数真的过了消息总线 */
        }
        p.domain        = (u32)s->domain;
        p.state         = (u32)s->state;
        p.epoch         = s->epoch;
        p.resources     = s->resources;
        p.devices_ready = s->devices_ready;
        memcpy(out, &p, (unsigned long)sizeof(p));
        *out_len = (u32)sizeof(p);
        return HYN_OK;
    }

    if (op == HYN_GROUP_OP_INFO) {
        hyn_group_info_t g;

        if (out == NULL || *out_len < (u32)sizeof(g)) {
            return HYN_EBUSY;
        }
        fill_info(s, &g);
        memcpy(out, &g, (unsigned long)sizeof(g));
        *out_len = (u32)sizeof(g);
        return HYN_OK;
    }

    if (op == HYN_GROUP_OP_CRASH) {
        domain_crash();                      /* 不会返回：异常由本组捕获接管 */
        return HYN_EFAULT_FAULT;
    }

    if (s->ops != NULL && s->ops->dispatch != NULL) {
        return s->ops->dispatch(op, in, in_len, out, out_len);
    }
    return HYN_ENOENT;
}

static void serve_request(domain_slot_t *s, const hyn_alpc_msg_t *req)
{
    u8             out[HYN_MSG_MAX];
    /*
     * out_len 是"进出双向"参数：进来时表示 out 的容量，出去时是本组实际写入的字节数。
     * 必须以容量入参——各分组的 dispatch 都按 *out_len 做容量校验，若传 0 会被判成
     * "缓冲区不够"，于是所有请求都只能得到 HYN_EBUSY。
     */
    u32            out_len = (u32)sizeof(out);
    u32            op = req->flags;
    hyn_status_t   st;
    hyn_alpc_msg_t rep;

    memset(out, 0, (unsigned long)sizeof(out));
    s->msg_served++;

    if (op == HYN_GROUP_OP_STORAGE_PROBE || op == HYN_GROUP_OP_NET_PROBE ||
        op == HYN_GROUP_OP_BUS_ENUM) {
        s->probe_count++;
    }

    st = domain_execute(s, op, req->data, req->len, out, &out_len);

    if (req->reply_to == 0ull) {
        return;                              /* 请求方不要应答 */
    }
    hyn_alpc_msg_init(&rep, HYN_ALPC_MSG_REPLY, req->msg_id,
                      s->domain, req->sender_domain, out, out_len);
    rep.flags     = (u32)st;
    rep.sender_id = hyn_alpc_endpoint_of(s->svc_port);
    if (hyn_alpc_send(req->reply_to, &rep) != HYN_OK) {
        s->msg_discarded++;                  /* 应答发不出去：记账，不假装送达 */
    }
}

/* 服务线程主体：轮询本组服务端口；退出条件只由本组的 stop_svc 决定 */
static void svc_body(void *arg)
{
    hyn_domain_t   d = (hyn_domain_t)(uptr)arg;
    domain_slot_t *s = &g_domains[(u32)d];

    for (;;) {
        hyn_alpc_msg_t req;

        if (s->stop_svc != 0u) {
            break;
        }
        if (hyn_alpc_receive_wait(s->svc_port, &req, HYN_DOMAIN_SVC_WAIT_TICKS) != HYN_OK) {
            continue;                        /* 超时：回去复查 stop 标志 */
        }
        if (req.type != HYN_ALPC_MSG_REQUEST) {
            s->msg_discarded++;
            continue;
        }
        serve_request(s, &req);
        /* CRASH 会让本线程在异常里退出，正常请求才会回到这里 */
    }
    hyn_thread_exit(0);
}

/*
 * 底座侧"安全点代服务"：在硬核底座自己的执行流上，替某个域收取并处理一条跨域请求。
 *
 * 为什么需要它：CORE 域没有服务线程（底座不承载驱动，也就不需要常驻服务循环），
 * 但 Ring3 用户程序要向底座提问（组规模、分组状态）时，请求就落在 core.req 上。
 * 底座的纪律是"只在安全点处理"，所以这里必须是**非阻塞**收取：有就处理一条，
 * 没有就如实返回 ENOENT —— 绝不把底座主线程挂在端口上等（那等于把底座交给别人调度）。
 *
 * 语义（对调用方严格）：
 *   · 返回 HYN_OK       ：确实处理掉了一条请求，out_op / out_msg_id / out_sender_domain 被填上；
 *   · 返回 HYN_ENOENT   ：该端口此刻没有消息（什么都没做）；
 *   · 返回 HYN_EINVAL   ：端口不属于任何域的请求口，或收到的不是 REQUEST（记为丢弃）。
 */
hyn_status_t hyn_domain_serve_port(u64 port_id, u32 *out_op, u32 *out_msg_id,
                                   u8 *out_sender_domain)
{
    u32 i;

    for (i = 0u; i < (u32)HYN_DOMAIN_MAX; i++) {
        domain_slot_t *s = &g_domains[i];
        hyn_alpc_msg_t req;
        hyn_status_t   st;

        if (s->used == 0u || s->req_port == 0ull || s->req_port != port_id) {
            continue;
        }
        st = hyn_alpc_receive(port_id, &req);
        if (st != HYN_OK) {
            return HYN_ENOENT;
        }
        if (req.type != HYN_ALPC_MSG_REQUEST) {
            s->msg_discarded++;
            return HYN_EINVAL;
        }
        if (out_op != NULL) {
            *out_op = req.flags;
        }
        if (out_msg_id != NULL) {
            *out_msg_id = req.msg_id;
        }
        if (out_sender_domain != NULL) {
            *out_sender_domain = req.sender_domain;
        }
        serve_request(s, &req);
        return HYN_OK;
    }
    return HYN_EINVAL;
}

/* 某域的跨域请求端口 id（0 表示该域没有请求口） */
u64 hyn_domain_req_port(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    return (s == NULL) ? 0ull : s->req_port;
}

static void start_svc(hyn_domain_t d)
{
    domain_slot_t *s = &g_domains[(u32)d];

    s->stop_svc    = 0u;
    s->svc_thread  = hyn_thread_create(g_svc_thread_names[(u32)d], svc_body,
                                       (void *)(uptr)(u64)(u32)d,
                                       HYN_THREAD_PRIO_NORMAL, (u8)d);
    s->svc_thread_id = (s->svc_thread != NULL) ? hyn_thread_id(s->svc_thread) : 0ull;
}

/* ---- 生命周期：静默与重置 ---- */

static void quiesce_slot(domain_slot_t *s)
{
    s->state = (u8)HYN_DOMAIN_STATE_RESETTING;
    s->quiesce_count++;
    s->stop_svc = 1u;

    /* 旧服务线程已死（被异常接管过）就不必等；活着的按轮询间隔自行退出 */
    if (s->svc_thread != NULL && hyn_thread_state(s->svc_thread) != HYN_THREAD_DEAD) {
        (void)hyn_sched_wait_thread(s->svc_thread, HYN_DOMAIN_QUIESCE_WAIT_TICKS);
    }
    s->svc_thread = NULL;

    if (s->ops != NULL && s->ops->quiesce != NULL) {
        (void)s->ops->quiesce();
    }
}

static hyn_status_t reset_slot(domain_slot_t *s)
{
    hyn_status_t st;

    quiesce_slot(s);                          /* 1) 静默 + 回收本组资源 */

    if (s->ops != NULL && s->ops->reset != NULL) {
        st = s->ops->reset();                 /* 2) 重置：重新认领本组硬件 */
        if (st != HYN_OK) {
            s->state = (u8)HYN_DOMAIN_STATE_OFFLINE;
            return st;
        }
    }
    if (s->ops != NULL && s->ops->probe != NULL) {
        st = s->ops->probe();                 /* 3) 重建本组软件结构并上报账目 */
        if (st != HYN_OK) {
            s->state = (u8)HYN_DOMAIN_STATE_OFFLINE;
            return st;
        }
    }

    s->state          = (u8)HYN_DOMAIN_STATE_ONLINE;
    s->epoch++;                               /* 新一代：调用方可据此判断组被重启过 */
    s->reset_count++;
    s->pending_reset  = 0u;

    start_svc((hyn_domain_t)s->domain);       /* 服务线程重建 */
    return HYN_OK;
}

hyn_status_t hyn_domain_quiesce(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    if (d == HYN_DOMAIN_CORE) {
        return HYN_EDOMAIN;                   /* 底座不可静默（否则连日志都没了） */
    }
    if (s == NULL || s->used == 0u) {
        return HYN_ENOENT;
    }
    quiesce_slot(s);
    return HYN_OK;
}

hyn_status_t hyn_domain_reset(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    if (d == HYN_DOMAIN_CORE) {
        return HYN_EDOMAIN;                   /* 底座不可重置 */
    }
    if (s == NULL || s->used == 0u) {
        return HYN_ENOENT;
    }
    return reset_slot(s);
}

int hyn_domain_pending_reset(hyn_domain_t d)
{
    domain_slot_t *s = slot_of(d);

    if (s == NULL) {
        return 0;
    }
    return (s->pending_reset != 0u) ? 1 : 0;
}

hyn_status_t hyn_domain_service_pending(void)
{
    hyn_status_t first = HYN_OK;
    u32           d;

    for (d = 1u; d < (u32)HYN_DOMAIN_MAX; d++) {
        if (g_domains[d].used != 0u && g_domains[d].pending_reset != 0u) {
            hyn_status_t st = reset_slot(&g_domains[d]);
            if (st != HYN_OK && first == HYN_OK) {
                first = st;
            }
        }
    }
    return first;
}

/* ---- 异常捕获 ---- */

int hyn_domain_fault_hook(const hyn_trap_frame_t *f)
{
    hyn_thread_t  *t;
    domain_slot_t *s;
    u8             d;

    if (f == NULL) {
        return 0;
    }
    t = hyn_thread_current();
    if (t == NULL) {
        return 0;
    }
    d = hyn_thread_domain(t);
    if (d == (u8)HYN_DOMAIN_CORE || (u32)d >= (u32)HYN_DOMAIN_MAX) {
        return 0;                             /* 底座/未知归属：交回陷阱框架如实停机 */
    }
    s = &g_domains[(u32)d];
    if (s->used == 0u) {
        return 0;
    }

    s->fault_count++;
    s->last_fault_vector  = (u32)f->vector;
    s->last_fault_error   = (u32)f->error_code;
    s->last_fault_thread  = hyn_thread_id(t);
    strncpy_safe(s->last_fault_thread_name, hyn_thread_name(t), 16);
    s->state              = (u8)HYN_DOMAIN_STATE_FAULTED;
    s->pending_reset      = 1u;               /* 重置放到安全点做，不在异常上下文里做 */
    s->stop_svc           = 1u;
    g_fault_total++;

    kprintf("[故障域] %s 捕获内核态异常：向量 %u 错误码 0x%x（线程 %s）；本组标记 FAULTED，"
            "底座与其余分组继续运行\n",
            s->name, (u32)f->vector, (u32)f->error_code, s->last_fault_thread_name);

    /* 只结束出错线程；它回到调度器时本组已是 FAULTED，服务端口不再有人应答 */
    hyn_thread_exit((int)(HYN_DOMAIN_FAULT_EXIT_BASE | (u32)f->vector));
    return 1;                                 /* 不会到达 */
}

/* ---- 组间调用（跨域强制走 ALPC） ---- */

static hyn_status_t wait_reply(domain_slot_t *ts, domain_slot_t *cs, u32 msg_id,
                               void *out, u32 out_cap, u32 *out_len_actual)
{
    u64 deadline = hyn_sched_ticks() + HYN_DOMAIN_REPLY_TIMEOUT_TICKS;

    for (;;) {
        hyn_alpc_msg_t rep;
        u64            now, left, slice;

        /* 目标组已不可用：给"对方不可用"的明确结论，而不是挂到超时 */
        if (ts->state == (u8)HYN_DOMAIN_STATE_FAULTED) {
            return HYN_EFAULT_FAULT;
        }
        if (ts->state != (u8)HYN_DOMAIN_STATE_ONLINE) {
            return HYN_EBUSY;
        }

        now = hyn_sched_ticks();
        if (now >= deadline) {
            if (hyn_alpc_receive(cs->req_port, &rep) != HYN_OK) {
                return HYN_ETIMEDOUT;         /* 到点仍无应答 */
            }
        } else {
            left  = deadline - now;
            slice = (left > HYN_DOMAIN_REPLY_SLICE_TICKS) ? HYN_DOMAIN_REPLY_SLICE_TICKS : left;
            if (hyn_alpc_receive_wait(cs->req_port, &rep, slice) != HYN_OK) {
                continue;                     /* 分片到点：复查目标组状态后继续等 */
            }
        }

        if (rep.type != HYN_ALPC_MSG_REPLY || rep.msg_id != msg_id) {
            g_msg_discarded++;                /* 配不上的应答：记账后继续等 */
            continue;
        }
        if (out != NULL && out_cap > 0u) {
            u32 n = (rep.len < out_cap) ? rep.len : out_cap;
            if (n > 0u) {
                memcpy(out, rep.data, (unsigned long)n);
            }
        }
        if (out_len_actual != NULL) {
            *out_len_actual = rep.len;
        }
        return (hyn_status_t)rep.flags;       /* 0 = 成功；非 0 是对方给出的处理结论 */
    }
}

hyn_status_t hyn_domain_call(hyn_domain_t caller, hyn_domain_t target, u32 op,
                             const void *in, u32 in_len,
                             void *out, u32 out_len, u32 *out_len_actual)
{
    domain_slot_t *cs;
    domain_slot_t *ts;
    hyn_alpc_msg_t req;
    u32            msg_id;
    hyn_status_t   st;

    if ((u32)caller >= (u32)HYN_DOMAIN_MAX || (u32)target >= (u32)HYN_DOMAIN_MAX) {
        return HYN_EINVAL;
    }
    if (caller == target) {
        return HYN_EDOMAIN;                   /* 同域应走 hyn_group_local_call，不占消息总线 */
    }
    st = hyn_domain_call_guard(caller, target, 1);
    if (st != HYN_OK) {
        return st;
    }

    cs = slot_of(caller);
    ts = slot_of(target);
    if (cs == NULL || ts == NULL || cs->used == 0u || ts->used == 0u) {
        return HYN_ENOENT;
    }
    if (cs->req_port == 0ull || ts->svc_port == 0ull) {
        return HYN_ENOENT;                    /* 目标没有服务端口：如实说"没有入口" */
    }
    if (ts->state == (u8)HYN_DOMAIN_STATE_FAULTED) {
        g_call_denied++;
        audit_call(caller, target, op, HYN_EFAULT_FAULT);
        return HYN_EFAULT_FAULT;
    }
    if (ts->state != (u8)HYN_DOMAIN_STATE_ONLINE) {
        g_call_denied++;
        audit_call(caller, target, op, HYN_EBUSY);
        return HYN_EBUSY;
    }
    if (in != NULL && in_len > HYN_MSG_MAX) {
        return HYN_EINVAL;
    }

    msg_id = ++g_msg_seq;
    if (msg_id == 0u) {
        msg_id = ++g_msg_seq;
    }
    hyn_alpc_msg_init(&req, HYN_ALPC_MSG_REQUEST, msg_id,
                      (u8)caller, (u8)target, in, in_len);
    req.flags     = op;                       /* op 走 flags，参数走 data */
    req.sender_id = hyn_alpc_endpoint_of(cs->req_port);
    req.reply_to  = cs->req_port;             /* 应答回到本域的请求端口 */

    st = hyn_alpc_send(ts->svc_port, &req);
    if (st != HYN_OK) {
        g_call_denied++;
        audit_call(caller, target, op, st);
        return st;                            /* 投递被拒（未建链/队列满）：如实返回 */
    }

    st = wait_reply(ts, cs, msg_id, out, out_len, out_len_actual);
    if (st == HYN_OK) {
        g_call_total++;
    } else {
        g_call_denied++;
    }
    audit_call(caller, target, op, st);
    return st;
}

hyn_status_t hyn_group_local_call(hyn_domain_t caller, hyn_domain_t target, u32 op,
                                  const void *in, u32 in_len,
                                  void *out, u32 out_len, u32 *out_len_actual)
{
    domain_slot_t *ts;
    u8             tmp[HYN_MSG_MAX];
    u32            cap = out_len;
    hyn_status_t   st;

    if ((u32)caller >= (u32)HYN_DOMAIN_MAX || (u32)target >= (u32)HYN_DOMAIN_MAX) {
        return HYN_EINVAL;
    }
    st = hyn_domain_call_guard(caller, target, 0);
    if (st != HYN_OK) {
        return st;                            /* 跨域直调：拒绝（并已计入审计） */
    }
    ts = slot_of(target);
    if (ts == NULL || ts->used == 0u) {
        return HYN_ENOENT;
    }
    if (in != NULL && in_len > HYN_MSG_MAX) {
        return HYN_EINVAL;
    }
    if (cap > (u32)sizeof(tmp)) {
        cap = (u32)sizeof(tmp);
    }
    memset(tmp, 0, (unsigned long)sizeof(tmp));
    st = domain_execute(ts, op, (const u8 *)in, in_len, (out != NULL) ? tmp : NULL, &cap);
    if (st == HYN_OK && out != NULL && cap > 0u) {
        memcpy(out, tmp, (unsigned long)cap);
    }
    if (out_len_actual != NULL) {
        *out_len_actual = cap;
    }
    return st;
}

/* ---- 注册与启动 ---- */

hyn_status_t hyn_domain_register(hyn_domain_t d, const hyn_domain_ops_t *ops)
{
    domain_slot_t *s = slot_of(d);

    if (s == NULL || ops == NULL) {
        return HYN_EINVAL;
    }
    s->ops      = ops;
    s->domain   = (u8)d;
    s->used     = 1u;
    s->state    = (u8)HYN_DOMAIN_STATE_OFFLINE;
    s->epoch    = 1u;
    s->pending_reset    = 0u;
    s->stop_svc         = 0u;
    s->last_fault_vector = 0xFFFFFFFFu;
    strncpy_safe(s->name, (ops->name != NULL) ? ops->name : "未命名分组", 16);
    return HYN_OK;
}

/* 建链（幂等）：请求口 -> 对方服务口（发请求），本组服务口 -> 对方请求口（回应答） */
static void wire_all(void)
{
    u32 i, j;

    for (i = 0u; i < (u32)HYN_DOMAIN_MAX; i++) {
        for (j = 0u; j < (u32)HYN_DOMAIN_MAX; j++) {
            if (i == j) {
                continue;
            }
            if (g_domains[i].req_port != 0ull && g_domains[j].svc_port != 0ull &&
                !hyn_alpc_is_connected(g_domains[i].req_port, g_domains[j].svc_port)) {
                (void)hyn_alpc_connect(g_domains[i].req_port, g_domains[j].svc_port);
            }
            if (g_domains[i].svc_port != 0ull && g_domains[j].req_port != 0ull &&
                !hyn_alpc_is_connected(g_domains[i].svc_port, g_domains[j].req_port)) {
                (void)hyn_alpc_connect(g_domains[i].svc_port, g_domains[j].req_port);
            }
        }
    }
}

hyn_status_t hyn_domain_init(void)
{
    hyn_status_t st;
    u32          d;

    memset(g_domains, 0, (unsigned long)sizeof(g_domains));
    g_msg_seq      = 0u;
    g_call_total   = 0u;
    g_call_denied  = 0u;
    g_msg_discarded = 0u;
    g_fault_total  = 0u;
    g_last_msg[0]  = '\0';

    for (d = 0u; d < (u32)HYN_DOMAIN_MAX; d++) {
        g_domains[d].domain = (u8)d;
        g_domains[d].state  = (u8)HYN_DOMAIN_STATE_OFFLINE;
        g_domains[d].last_fault_vector = 0xFFFFFFFFu;
    }

    /* 硬核底座也算一个"域"（有请求口、可被调用、可回快照），但不承载驱动、不可重置 */
    g_domains[(u32)HYN_DOMAIN_CORE].used  = 1u;
    g_domains[(u32)HYN_DOMAIN_CORE].state = (u8)HYN_DOMAIN_STATE_ONLINE;
    g_domains[(u32)HYN_DOMAIN_CORE].epoch = 1u;
    strncpy_safe(g_domains[(u32)HYN_DOMAIN_CORE].name, "硬核底座", 16);

    st = hyn_domain_register(HYN_DOMAIN_STORAGE, hyn_group_storage_ops());
    if (st != HYN_OK) {
        return st;
    }
    st = hyn_domain_register(HYN_DOMAIN_NET, hyn_group_net_ops());
    if (st != HYN_OK) {
        return st;
    }
    st = hyn_domain_register(HYN_DOMAIN_BUS, hyn_group_bus_ops());
    if (st != HYN_OK) {
        return st;
    }

    /* 端口：底座一条请求口，三大分组各一条服务口 + 一条请求口 */
    for (d = 0u; d < (u32)HYN_DOMAIN_MAX; d++) {
        if (g_req_port_names[d] != NULL) {
            st = hyn_alpc_port_create(g_req_port_names[d], (hyn_domain_t)d,
                                      &g_domains[d].req_port);
            if (st != HYN_OK) {
                return st;
            }
        }
        if (g_svc_port_names[d] != NULL) {
            st = hyn_alpc_port_create(g_svc_port_names[d], (hyn_domain_t)d,
                                      &g_domains[d].svc_port);
            if (st != HYN_OK) {
                return st;
            }
        }
    }

    wire_all();

    /* 逐组：认领真实硬件、重建软件结构、上报账目，然后起服务线程 */
    for (d = 1u; d < (u32)HYN_DOMAIN_MAX; d++) {
        domain_slot_t *s = &g_domains[d];

        s->probe_count++;                     /* 首次 probe 也是 probe */
        st = (s->ops != NULL && s->ops->probe != NULL) ? s->ops->probe() : HYN_OK;
        if (st != HYN_OK) {
            s->state = (u8)HYN_DOMAIN_STATE_OFFLINE;
            return st;
        }
        s->state = (u8)HYN_DOMAIN_STATE_ONLINE;
        start_svc((hyn_domain_t)d);
        if (s->svc_thread == NULL) {
            s->state = (u8)HYN_DOMAIN_STATE_OFFLINE;
            return HYN_ENOMEM;                /* 线程池满：不静默降级 */
        }
    }
    return HYN_OK;
}

static void domain_crash(void)
{
    volatile u64 *p = (volatile u64 *)(uptr)HYN_DOMAIN_CRASH_ADDR;

    (void)p[0];                               /* 未映射：在这里触发内核态 #PF */
}