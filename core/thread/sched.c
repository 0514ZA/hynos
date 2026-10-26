/*
 * HynOS —— 进程/线程模型与优先级调度（纯 C，架构无关，硬核底座）
 *
 * 本文件是硬核底座里"最不能挂"的那一块：它决定 CPU 时间怎么分、谁在什么时候跑。
 * 设计取向：
 *   · 静态线程池：固定 12 个槽位、每线程 16KiB 内核栈，全部在 .bss 里。
 *     内核态不动态分配线程栈，避免"分配失败还要处理失败"的泥潭；
 *   · 优先级 + 同优先级轮转：pick_next 从当前线程之后一圈找"就绪且优先级最高"的，
 *     同优先级自然形成 Round-Robin；
 *   · 时间片抢占可开关：打开时每个时钟节拍都检查"是否有更高/同级的就绪线程"，
 *     有就把当前线程换下去。抢占只切给"非空闲线程"，避免没活干时白抖；
 *   · 阻塞/唤醒用 chan（通道）标识：等消息、等线程退出都走同一条路径，不烧 CPU；
 *   · 切换纪律：改状态、换 current、换内核栈、切栈必须在关中断状态下连续完成，
 *     否则时钟节拍可能插在"状态已改、栈还没换"之间，把线程状态搞脏。
 *
 * 与 arch 的分工：栈的保存/恢复、进入用户态都是 CPU 专属动作，落在
 * arch/x86_64/thread.S（hyn_cpu_switch / hyn_cpu_thread_entry / hyn_arch_enter_user）。
 */
#include <hyn/thread.h>
#include <hyn/object.h>          /* HYN_DOMAIN_* 域标签：线程归属故障域 */
#include <hyn/arch.h>
#include <hyn/kstring.h>
#include <hyn/kprintf.h>

extern u8 _boot_stack_bottom[];
extern u8 _boot_stack_top[];

struct hyn_thread {
    u64      kstack_top;                    /* 内核栈顶：切换时写进 TSS.rsp0 */
    u64      rsp;                           /* hyn_cpu_switch 保存的栈指针 */
    void   (*entry)(void *);
    void    *arg;
    void    *chan;                          /* 正在等待的通道（BLOCKED 时有效） */
    u64      wake_tick;                     /* 到期节拍；0 = 不限时 */
    u64      id;                            /* 单调发号（内核身份） */
    u64      port_id;                       /* ALPC 身份端口；0 = 未绑定 */
    char     name[HYN_THREAD_NAME_MAX];
    u64      stack_low;
    u64      stack_high;
    u32      prio;
    u32      run_count;                     /* 被调度运行的次数 */
    u32      yield_count;                   /* 主动让出次数（时间片抢占不计入） */
    i32      exit_code;
    i32      wake_result;                   /* HYN_SCHED_WOKE / HYN_SCHED_TIMEOUT */
    u8       state;
    u8       domain;
    u8       is_idle;
    u8       reserved;
    u8       stack[HYN_THREAD_STACK_SIZE] __attribute__((aligned(16)));
};

static struct hyn_thread g_pool[HYN_SCHED_MAX_THREADS];

static struct hyn_thread *g_current;
static struct hyn_thread *g_main;
static struct hyn_thread *g_idle;
static u64 g_thread_seq;
static u64 g_ticks;
static u32 g_switch_count;
static u32 g_preempt_count;
static u32 g_idle_run_count;
static int g_preempt;
static int g_ready;

static const char *const g_state_names[] = {
    "未用", "就绪", "运行", "阻塞", "已退出"
};

/* ------------------------------------------------------------------ */
/* 栈准备：与 arch/x86_64/thread.S 的 hyn_cpu_switch 严格对称        */
/* ------------------------------------------------------------------ */

static void prepare_stack(struct hyn_thread *t)
{
    u64 top = (u64)(uptr)&t->stack[HYN_THREAD_STACK_SIZE];
    u64 *sp;

    top &= ~0xFull;                              /* 16 字节对齐（ABI 要求） */
    sp = (u64 *)(uptr)top;
    *--sp = (u64)(uptr)hyn_cpu_thread_entry;     /* switch 的 ret 落点 */
    *--sp = 0x202ull;                            /* RFLAGS：IF=1（新线程开着中断跑） */
    *--sp = 0ull;                                /* rbp */
    *--sp = 0ull;                                /* rbx */
    *--sp = 0ull;                                /* r12 */
    *--sp = 0ull;                                /* r13 */
    *--sp = 0ull;                                /* r14 */
    *--sp = 0ull;                                /* r15 */
    /* 栈上顺序（低 -> 高）：r15 r14 r13 r12 rbx rbp rflags ret —— 与 pop 顺序一致 */

    t->rsp        = (u64)(uptr)sp;
    t->kstack_top = top;
    t->stack_low  = (u64)(uptr)&t->stack[0];
    t->stack_high = top;
}

static struct hyn_thread *pool_alloc(void)
{
    u32 i;
    struct hyn_thread *dead = NULL;

    for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
        if (g_pool[i].state == (u8)HYN_THREAD_UNUSED) {
            return &g_pool[i];
        }
        if (dead == NULL && g_pool[i].state == (u8)HYN_THREAD_DEAD) {
            dead = &g_pool[i];
        }
    }
    /*
     * 已退出的槽位可以被复用（内核栈随之复用）。前提是"等待它退出的人已经醒了"，
     * 自检按"先 wait 再创建"的顺序推进；本里程碑不引入僵尸回收队列。
     */
    return dead;
}

static struct hyn_thread *thread_new(const char *name, void (*entry)(void *), void *arg,
                                     u32 prio, u8 domain, u8 is_idle)
{
    struct hyn_thread *t = pool_alloc();

    if (t == NULL) {
        return NULL;                             /* 池满：如实失败，不静默降级 */
    }
    memset(t, 0, (unsigned long)sizeof(*t));
    t->id         = ++g_thread_seq;
    t->entry      = entry;
    t->arg        = arg;
    t->prio       = prio;
    t->domain     = domain;
    t->is_idle    = is_idle;
    t->state      = (u8)HYN_THREAD_READY;
    t->exit_code  = 0;
    t->wake_result = HYN_SCHED_WOKE;
    strncpy_safe(t->name, name, HYN_THREAD_NAME_MAX);
    prepare_stack(t);
    return t;
}

/* ------------------------------------------------------------------ */
/* 调度核心                                                          */
/* ------------------------------------------------------------------ */

/*
 * 从 prev 之后一圈里挑"就绪且优先级最高"的线程（同优先级自然轮转）。
 *
 * prev 自身必须跳过：switch_away 会先把 prev 置成离场状态再调用本函数，但
 * "主动让出（READY）"这条路径下 prev 本身就是就绪的——若允许选中自己，当轮转
 * 扫到最后一个索引仍只有自己时，switch_to 会因 next == prev 直接返回，yield 变成
 * 空操作、yield_count 虚增、RUNNING 线程被错标成 READY。跳过 prev 从结构上排除这种自旋。
 */
static struct hyn_thread *pick_next(const struct hyn_thread *prev)
{
    u32 i, start = 0u;
    struct hyn_thread *best = NULL;

    if (prev != NULL) {
        for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
            if (&g_pool[i] == prev) {
                start = i + 1u;
                break;
            }
        }
    }
    for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
        struct hyn_thread *t = &g_pool[(start + i) % HYN_SCHED_MAX_THREADS];

        if (t == prev || t->state != (u8)HYN_THREAD_READY) {
            continue;
        }
        if (best == NULL || t->prio > best->prio) {
            best = t;
        }
    }
    return best;
}

static void switch_to(struct hyn_thread *next)
{
    struct hyn_thread *prev = g_current;

    if (next == NULL) {
        return;
    }
    if (next == prev) {
        /*
         * 自己切自己：只有空闲线程会走到这里——池里没有别的就绪线程时它就是唯一候选，
         * 而 switch_away 已经先把它的状态写成了"就绪"。必须改回运行中，否则会出现
         * "明明在跑却标着就绪"的状态污染：ready_count 虚增，且抢占判定
         * （要求当前线程 RUNNING）会整体失效，空闲线程再也不会把 CPU 让给新就绪的线程。
         */
        next->state = (u8)HYN_THREAD_RUNNING;
        return;
    }
    next->state = (u8)HYN_THREAD_RUNNING;
    next->run_count++;
    if (next->is_idle) {
        g_idle_run_count++;
    }
    g_current = next;
    g_switch_count++;
    arch_set_kernel_stack(next->kstack_top);      /* Ring3 陷入时用的内核栈 */
    hyn_cpu_switch(&prev->rsp, next->rsp);
}

/*
 * 让出/阻塞的统一出口：new_state 是本次离场状态（READY / BLOCKED / DEAD）。
 * 全程关中断：状态、current、内核栈、栈的切换必须是原子的。
 */
static void switch_away(u8 new_state)
{
    struct hyn_thread *prev = g_current;
    struct hyn_thread *next;
    uptr fl;

    if (!g_ready || prev == NULL) {
        return;
    }
    fl = arch_irq_save();
    prev->state = new_state;
    next = pick_next(prev);
    if (next == NULL) {
        next = g_idle;                            /* 兜底：至少还有空闲线程 */
    }
    switch_to(next);
    arch_irq_restore(fl);                         /* 本线程被重新调度回来后才走到这里 */
}

/*
 * 时钟节拍触发的抢占：只在"有更高或同级优先级的就绪线程"时切走；已经在中断上下文里，
 * 无需再关中断。优先级比较是这条纪律的另一半：低优先级线程就绪不构成抢占理由，
 * 否则短时间片会被低优先级线程反复打断，时间片的分配顺序就无从核对。
 */
static void preempt_from_tick(void)
{
    struct hyn_thread *prev = g_current;
    struct hyn_thread *best = NULL;
    u32 i;
    uptr fl;

    if (prev == NULL || prev->state != (u8)HYN_THREAD_RUNNING) {
        return;
    }
    for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
        struct hyn_thread *t = &g_pool[i];

        if (t->state == (u8)HYN_THREAD_READY && !t->is_idle &&
            t->prio >= prev->prio &&
            (best == NULL || t->prio > best->prio)) {
            best = t;
        }
    }
    if (best == NULL) {
        return;                                   /* 只有空闲线程在等：让当前线程继续跑 */
    }
    fl = arch_irq_save();
    prev->state = (u8)HYN_THREAD_READY;
    g_preempt_count++;
    switch_to(best);
    arch_irq_restore(fl);
}

static void idle_body(void *arg)
{
    HYN_UNUSED(arg);
    for (;;) {
        hyn_sched_yield();                       /* 有活就切走 */
        arch_cpu_relax();                        /* 没活就 hlt 等下一次中断 */
    }
}

void hyn_thread_trampoline(void)
{
    struct hyn_thread *t = g_current;

    if (t != NULL && t->entry != NULL) {
        t->entry(t->arg);
    }
    hyn_thread_exit(0);
}

void hyn_thread_exit(int code)
{
    struct hyn_thread *t = g_current;

    if (t == NULL) {
        arch_halt_forever();
    }
    t->exit_code = (i32)code;
    (void)hyn_sched_wake_chan(t, 0u);             /* 唤醒等本线程退出的等待者 */
    switch_away((u8)HYN_THREAD_DEAD);             /* 不会再被选中，故不会返回 */
    arch_halt_forever();                          /* 防御性兜底 */
}

hyn_status_t hyn_sched_init(void)
{
    struct hyn_thread *t;

    if (g_ready) {
        return HYN_EBUSY;
    }
    memset(g_pool, 0, (unsigned long)sizeof(g_pool));
    g_current = NULL; g_main = NULL; g_idle = NULL;
    g_thread_seq = 0ull; g_ticks = 0ull;
    g_switch_count = 0u; g_preempt_count = 0u; g_idle_run_count = 0u;
    g_preempt = 0; g_ready = 0;

    /* 主线程 = "现在正在跑这段代码的执行流"：它已经在引导栈上跑着了 */
    t = &g_pool[0];
    t->id         = ++g_thread_seq;
    t->state      = (u8)HYN_THREAD_RUNNING;
    t->prio       = HYN_THREAD_PRIO_NORMAL;
    t->domain     = (u8)HYN_DOMAIN_CORE;
    t->kstack_top = (u64)(uptr)_boot_stack_top;
    t->stack_low  = (u64)(uptr)_boot_stack_bottom;
    t->stack_high = (u64)(uptr)_boot_stack_top;
    t->wake_result = HYN_SCHED_WOKE;
    strncpy_safe(t->name, "kernel.main", HYN_THREAD_NAME_MAX);
    g_main = t;
    g_current = t;

    /* 空闲线程：保证"任何时刻都有东西可切"，且它只在没人就绪时才跑 */
    g_idle = thread_new("kernel.idle", idle_body, NULL,
                        HYN_THREAD_PRIO_IDLE, (u8)HYN_DOMAIN_CORE, 1u);
    if (g_idle == NULL) {
        return HYN_ENOMEM;
    }

    arch_set_kernel_stack(g_current->kstack_top);
    g_ready = 1;
    return HYN_OK;
}

int hyn_sched_ready(void)
{
    return g_ready;
}

hyn_thread_t *hyn_thread_create(const char *name, void (*entry)(void *), void *arg,
                                u32 prio, u8 domain)
{
    if (!g_ready) {
        return NULL;
    }
    return thread_new((name != NULL) ? name : "thread", entry, arg, prio, domain, 0u);
}

hyn_status_t hyn_thread_bind_port(hyn_thread_t *t, u64 port_id)
{
    if (t == NULL || port_id == 0ull) {
        return HYN_EINVAL;
    }
    t->port_id = port_id;
    return HYN_OK;
}

/* ------------------------------------------------------------------ */
/* 让出、节拍、阻塞与唤醒                                             */
/* ------------------------------------------------------------------ */

void hyn_sched_yield(void)
{
    struct hyn_thread *t = g_current;

    if (!g_ready || t == NULL) {
        return;
    }
    t->yield_count++;
    switch_away((u8)HYN_THREAD_READY);
}

void hyn_sched_tick(void)
{
    u32 i;

    if (!g_ready) {
        g_ticks++;
        return;
    }
    g_ticks++;

    /* 睡眠/等待超时到期：置回就绪并记录"是超时醒的" */
    for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
        struct hyn_thread *t = &g_pool[i];

        if (t->state == (u8)HYN_THREAD_BLOCKED && t->wake_tick != 0ull &&
            g_ticks >= t->wake_tick) {
            t->state       = (u8)HYN_THREAD_READY;
            t->chan        = NULL;
            t->wake_tick   = 0ull;
            t->wake_result = HYN_SCHED_TIMEOUT;
        }
    }

    if (g_preempt) {
        preempt_from_tick();                      /* 可能会切走并在稍后被切回来 */
    }
}

void hyn_sched_set_preempt(int on)
{
    g_preempt = (on != 0) ? 1 : 0;
}

int hyn_sched_preempt_enabled(void)
{
    return g_preempt;
}

int hyn_sched_block_timeout(void *chan, u64 timeout_ticks)
{
    struct hyn_thread *t = g_current;

    if (!g_ready || t == NULL || t->is_idle || chan == NULL) {
        return HYN_SCHED_TIMEOUT;                 /* 不该发生的阻塞：直接返回，不挂住内核 */
    }
    t->chan        = chan;
    t->wake_result = HYN_SCHED_WOKE;
    t->wake_tick   = (timeout_ticks == 0ull) ? 0ull : (g_ticks + timeout_ticks);
    switch_away((u8)HYN_THREAD_BLOCKED);
    return t->wake_result;
}

u32 hyn_sched_wake_chan(void *chan, u32 max)
{
    u32 i, n = 0u;

    if (chan == NULL) {
        return 0u;
    }
    for (i = 0u; i < HYN_SCHED_MAX_THREADS && (max == 0u || n < max); i++) {
        struct hyn_thread *t = &g_pool[i];

        if (t->state == (u8)HYN_THREAD_BLOCKED && t->chan == chan) {
            t->state       = (u8)HYN_THREAD_READY;
            t->chan        = NULL;
            t->wake_tick   = 0ull;
            t->wake_result = HYN_SCHED_WOKE;
            n++;
        }
    }
    return n;
}

int hyn_sched_wait_thread(hyn_thread_t *t, u64 timeout_ticks)
{
    u64 deadline;

    if (t == NULL) {
        return HYN_SCHED_TIMEOUT;
    }
    deadline = (timeout_ticks == 0ull) ? 0ull : (g_ticks + timeout_ticks);

    while (t->state != (u8)HYN_THREAD_DEAD) {
        u64 left = 0ull;

        if (deadline != 0ull) {
            if (g_ticks >= deadline) {
                return HYN_SCHED_TIMEOUT;
            }
            left = deadline - g_ticks;
        }
        (void)hyn_sched_block_timeout(t, left);   /* chan = 目标线程：它退出时唤醒 */
    }
    return HYN_SCHED_WOKE;
}

/* ------------------------------------------------------------------ */
/* 取证访问器（自检与汇报用）                                         */
/* ------------------------------------------------------------------ */

hyn_thread_t *hyn_thread_current(void)          { return g_current; }
u64 hyn_thread_id(const hyn_thread_t *t)        { return (t != NULL) ? t->id : 0ull; }
u64 hyn_thread_port_id(const hyn_thread_t *t)   { return (t != NULL) ? t->port_id : 0ull; }
u32 hyn_thread_prio(const hyn_thread_t *t)      { return (t != NULL) ? t->prio : 0u; }
u32 hyn_thread_run_count(const hyn_thread_t *t) { return (t != NULL) ? t->run_count : 0u; }
u32 hyn_thread_yield_count(const hyn_thread_t *t){ return (t != NULL) ? t->yield_count : 0u; }
int hyn_thread_exit_code(const hyn_thread_t *t) { return (t != NULL) ? (int)t->exit_code : -1; }
u64 hyn_thread_stack_low(const hyn_thread_t *t) { return (t != NULL) ? t->stack_low : 0ull; }
u64 hyn_thread_stack_high(const hyn_thread_t *t){ return (t != NULL) ? t->stack_high : 0ull; }

/* 线程归属域：故障域异常捕获的判据（CORE 线程出错一律如实停机） */
u8 hyn_thread_domain(const hyn_thread_t *t)     { return (t != NULL) ? t->domain : (u8)HYN_DOMAIN_CORE; }

const char *hyn_thread_name(const hyn_thread_t *t)
{
    return (t != NULL) ? t->name : "(无)";
}

hyn_thread_state_t hyn_thread_state(const hyn_thread_t *t)
{
    return (t != NULL) ? (hyn_thread_state_t)t->state : HYN_THREAD_UNUSED;
}

const char *hyn_thread_state_name(hyn_thread_state_t s)
{
    u32 i = (u32)s;

    return (i < 5u) ? g_state_names[i] : "未知";
}

u64 hyn_sched_ticks(void)          { return g_ticks; }
u32 hyn_sched_switch_count(void)   { return g_switch_count; }
u32 hyn_sched_preempt_count(void)  { return g_preempt_count; }
u32 hyn_sched_idle_run_count(void) { return g_idle_run_count; }

u32 hyn_sched_ready_count(void)
{
    u32 i, n = 0u;

    for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
        if (g_pool[i].state == (u8)HYN_THREAD_READY) {
            n++;
        }
    }
    return n;
}

u32 hyn_sched_live_count(void)
{
    u32 i, n = 0u;

    for (i = 0u; i < HYN_SCHED_MAX_THREADS; i++) {
        u8 s = g_pool[i].state;

        if (s != (u8)HYN_THREAD_UNUSED && s != (u8)HYN_THREAD_DEAD) {
            n++;
        }
    }
    return n;
}