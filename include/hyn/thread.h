/*
 * 混元 OS —— 进程/线程模型与优先级调度接口（硬核底座，架构无关）
 *
 * 模型取舍（如实标注，不夸大）：
 *   · 线程 = 内核态执行流 + 独立内核栈；"进程"目前只有"一个内核地址空间 + Ring3
 *     用户线程"，进程级地址空间隔离与进程对象随故障域里程碑（T6）补；
 *   · 调度 = 优先级优先 + 同优先级轮转（Round-Robin）；时间片由时钟节拍触发，
 *     是否抢占可开关（避免自检期间被无关节拍打断）；
 *   · 阻塞/唤醒基于 chan（通道）标识，不做自旋等待——等消息时 CPU 应当被让出去。
 *
 * 与 syscall 的分工：
 *   syscall 是 Ring3 进入内核的唯一入口（向量 0x80 的 DPL=3 门）。Ring3 线程在
 *   syscall 层通过"身份端口"（port_id）被识别，消息发送者身份一律由内核回填。
 *
 * 线程栈：每线程 16KiB 静态栈（内核态不设页表隔离，故栈就必须靠边界与自检守住）。
 */
#ifndef HYN_THREAD_H
#define HYN_THREAD_H

#include <hyn/types.h>

/*
 * 线程池容量：故障域里程碑要 3 个分组服务线程（存储组/网络组/硬件总线组各一条），
 * 且故障域重置后要能重建线程——DEAD 槽位复用必须等"等它退出的人先醒来"，
 * 因此池容量要留出余量，不能让"组服务线程 + 自检夹具"把池占满。
 */
#define HYN_SCHED_MAX_THREADS  16u              /* 线程池容量（静态池，不动态分配） */
#define HYN_THREAD_STACK_SIZE  16384u           /* 每线程内核栈 16KiB */
#define HYN_THREAD_NAME_MAX    24u

/* 优先级：数值越大越优先；空闲线程固定最低（它只在没别的事可做时跑） */
#define HYN_THREAD_PRIO_IDLE   0u
#define HYN_THREAD_PRIO_LOW    4u
#define HYN_THREAD_PRIO_NORMAL 8u
#define HYN_THREAD_PRIO_HIGH   16u

/* 阻塞等待的返回原因 */
#define HYN_SCHED_WOKE    0   /* 被显式唤醒 */
#define HYN_SCHED_TIMEOUT 1   /* 等超时了 */

typedef struct hyn_thread hyn_thread_t;

typedef enum hyn_thread_state {
    HYN_THREAD_UNUSED  = 0,
    HYN_THREAD_READY   = 1,
    HYN_THREAD_RUNNING = 2,
    HYN_THREAD_BLOCKED = 3,
    HYN_THREAD_DEAD    = 4
} hyn_thread_state_t;

hyn_status_t hyn_sched_init(void);
int          hyn_sched_ready(void);

/* 创建内核线程；池满返回 NULL（不分配即失败，绝不静默降级） */
hyn_thread_t *hyn_thread_create(const char *name, void (*entry)(void *), void *arg,
                                u32 prio, u8 domain);

/* 绑定线程的 ALPC 身份端口：syscall 投递消息时以此为发送者身份（用户无法伪造） */
hyn_status_t hyn_thread_bind_port(hyn_thread_t *t, u64 port_id);

hyn_thread_t       *hyn_thread_current(void);
u64                 hyn_thread_id(const hyn_thread_t *t);
u64                 hyn_thread_port_id(const hyn_thread_t *t);
const char         *hyn_thread_name(const hyn_thread_t *t);
/*
 * 线程归属的故障域：异常捕获靠它判断"这次内核态异常该由哪个分组自己扛"；
 * 归属底座（CORE）的线程出错一律如实停机，绝不吞掉。
 */
u8                  hyn_thread_domain(const hyn_thread_t *t);
hyn_thread_state_t  hyn_thread_state(const hyn_thread_t *t);
const char         *hyn_thread_state_name(hyn_thread_state_t s);
u32                 hyn_thread_prio(const hyn_thread_t *t);
u32                 hyn_thread_run_count(const hyn_thread_t *t);
u32                 hyn_thread_yield_count(const hyn_thread_t *t);
int                 hyn_thread_exit_code(const hyn_thread_t *t);
u64                 hyn_thread_stack_low(const hyn_thread_t *t);
u64                 hyn_thread_stack_high(const hyn_thread_t *t);

/* 调度动作 */
void hyn_sched_yield(void);                    /* 主动让出，回到就绪队列 */
void hyn_sched_tick(void);                     /* 时钟节拍：睡眠到期 + 可选时间片轮转 */
void hyn_sched_set_preempt(int on);
int  hyn_sched_preempt_enabled(void);

/* 阻塞/唤醒：chan 必须非空（端口 id、线程句柄、事件地址均可）。
 * timeout_ticks = 0 表示不限时；返回 HYN_SCHED_WOKE / HYN_SCHED_TIMEOUT。 */
int  hyn_sched_block_timeout(void *chan, u64 timeout_ticks);
u32  hyn_sched_wake_chan(void *chan, u32 max);                  /* max=0 表示全部唤醒 */
int  hyn_sched_wait_thread(hyn_thread_t *t, u64 timeout_ticks); /* 阻塞到线程退出 */

/* 结束当前线程：唤醒等待者后永不返回 */
void hyn_thread_exit(int code) __attribute__((noreturn));

/* 新线程首次运行的着陆点（由 arch/x86_64/thread.S 中 switch 的 ret 进入） */
void hyn_thread_trampoline(void) __attribute__((noreturn));

u64  hyn_sched_ticks(void);
u32  hyn_sched_switch_count(void);
u32  hyn_sched_preempt_count(void);
u32  hyn_sched_idle_run_count(void);
u32  hyn_sched_ready_count(void);
u32  hyn_sched_live_count(void);          /* 除已退出者之外仍在册的线程数 */

#endif /* HYN_THREAD_H */