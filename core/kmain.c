/*
 * 混元智能分层混合内核 —— 内核主入口
 *
 * 本文件只做"主线串联"：
 *   清零 BSS -> 保存引导信息 -> 拉起控制台 -> 初始化 HAL(设备登记)
 *   -> 拉起硬核底座的 GDT/IDT 与中断异常框架
 *   -> 物理页分配器 / PML4 虚拟内存 / 内核对象管理器 / ALPC 消息总线
 *   -> 进程/线程调度器（静态池 + 优先级 + 时钟节拍抢占）
 *   -> 跑自检 -> 如实汇报模块状态
 *
 * 分层纪律（不可违反）：
 *   · 硬核底座内不放驱动、文件系统、网络代码
 *   · 跨故障域通信强制走 ALPC 消息，禁止跨域直接函数调用
 *   · syscall 是 Ring3 进入内核的唯一入口
 */
#include <hyn/types.h>
#include <hyn/bootinfo.h>
#include <hyn/kprintf.h>
#include <hyn/kstring.h>
#include <hyn/hal.h>
#include <hyn/arch.h>
#include <hyn/cpu.h>
#include <hyn/gdt.h>
#include <hyn/idt.h>
#include <hyn/trap.h>
#include <hyn/timer.h>
#include <hyn/mmu.h>
#include <hyn/pmm.h>
#include <hyn/vmm.h>
#include <hyn/object.h>
#include <hyn/alpc.h>
#include <hyn/thread.h>
#include <hyn/syscall.h>
#include <hyn/ring3.h>
#include <hyn/fault.h>
#include <hyn/bus.h>
#include <hyn/net.h>
#include <hyn/vfs.h>
#include <hyn/userprog.h>
#include <hyn/buildstamp.h>

/*
 * 用户程序平铺镜像（由 tools/mkuserimg.py 在构建期从 shell.elf / agent.elf 的段表生成）。
 * 它必须在 kmain.c 里被 include 一次——镜像就是内核 .rodata 里的一段 const 数据，
 * 运行期整段拷到动态分配的用户页上执行，不依赖任何文件系统。
 */
#include <hyn/userimg.h>

extern u8 __bss_start[];
extern u8 __bss_end[];
extern u8 _boot_stack_bottom[];
extern u8 _boot_stack_top[];

/* 受控探测桩（arch/x86_64/trap_selftest.S） */
extern void hyn_trap_probe_breakpoint(void);
extern void hyn_trap_probe_pagefault(u64 bad_addr);
extern u8   hyn_trap_probe_pagefault_after[];

/* 引导层调用；此处前向声明，便于自检引用"内核代码地址"做恒等映射核对 */
void kernel_main(boot_info_t *bi);

static boot_info_t g_boot;
static int         g_boot_valid;
static u32         g_checks_total;
static u32         g_checks_passed;

/* 对象管理器自检：对象体与类型清理回调 */
static u8  g_obj_demo_body[64];
static u32 g_obj_demo_destroyed;
static const u32 HYN_OBJ_TYPE_SELFTEST = 0x20u;

/* ALPC 自检：派发统计与"服务侧"处理逻辑 */
static u32 g_alpc_dispatched;
static u32 g_alpc_reply_sent;

const boot_info_t *hyn_boot_info(void) { return &g_boot; }
int hyn_boot_info_valid(void) { return g_boot_valid; }

static void clear_bss(void)
{
    memset(__bss_start, 0, (u64)(__bss_end - __bss_start));
}

static void check(const char *name, int ok, const char *detail)
{
    g_checks_total++;
    if (ok) {
        g_checks_passed++;
    }
    kprintf("  [%s] %s %s\n", ok ? "通过" : "失败", name, detail);
}

static void print_banner(void)
{
    kputs("\n");
    kputs("=============================================================\n");
    kputs("  混元智能分层混合内核 (Hunyuan Layered Hybrid Kernel)\n");
    kputs("  硬核底座 + 快通道分组故障域 / 运行于 Ring0 内核态\n");
    kprintf("  架构: %s  构建时间戳: %s\n", arch_name(), HYN_BUILD_STAMP);
    kputs("=============================================================\n");
}

static void report_boot(void)
{
    static const char *const boot_methods[] = { "BIOS/MBR", "UEFI/GPT+ESP" };
    const char *method = "未知";
    u64 mem_mib = (u64)g_boot.total_mem_bytes / (1024u * 1024u);

    if (g_boot.boot_method < 2u) {
        method = boot_methods[g_boot.boot_method];
    }

    kprintf("[引导] 内核镜像  : %u 字节 / %u 扇区\n",
            (u32)g_boot.kernel_bytes, (u32)g_boot.kernel_sectors);
    kprintf("[引导] 物理内存  : %u 字节 (约 %u MiB)\n",
            (u32)g_boot.total_mem_bytes, (u32)mem_mib);
    kprintf("[引导] 引导栈    : %p .. %p (64KB)\n",
            (void *)_boot_stack_bottom, (void *)_boot_stack_top);
    kprintf("[引导] BSS 区间  : %p .. %p (%u 字节)\n",
            (void *)__bss_start, (void *)__bss_end,
            (u32)(u64)(__bss_end - __bss_start));
    kprintf("[引导] 信息结构  : magic=0x%x (%s)\n", (u32)g_boot.magic,
            g_boot_valid ? "有效" : "缺失，使用兜底值");
    kprintf("[引导] 引导方式  : %s (boot_method=%u)\n", method,
            (u32)g_boot.boot_method);
}

static const char *const g_dev_kind_names[] = {
    "未知", "帧缓冲", "USB", "HDA 音频", "NVMe", "网卡"
};
static const char *const g_dev_state_names[] = {
    "空", "已登记", "就绪", "失败", "静默(故障域已重置)"
};

static void device_report_cb(hal_device_t *dev, void *ctx)
{
    HYN_UNUSED(ctx);
    kprintf("[HAL ]   %s : %s / %s\n", dev->name,
            g_dev_kind_names[dev->kind], g_dev_state_names[dev->state]);
}

static void report_devices(void)
{
    kprintf("[HAL ] 设备登记完成，共 %u 个设备\n", hal_device_count());
    hal_foreach_device(device_report_cb, NULL);
}

/* ---- 自检：异常捕获与恢复 ---- */
static void selftest_traps(void)
{
    u64 before;
    int hit;

    kputs("\n---- 硬核底座自检：异常捕获 ----\n");

    /* #BP 断点异常：CPU 保存的 RIP 已指向下一条指令，无需修正 */
    hyn_trap_expect(3, 0);
    before = hyn_trap_count(3);
    hyn_trap_probe_breakpoint();
    hit = hyn_trap_expect_hit(3);
    check("#BP 断点异常捕获并恢复",
          (hit == 1) && (hyn_trap_count(3) == before + 1),
          "");

    /* #PF 页错误：读取未映射地址，捕获后把 RIP 修正到故障指令之后 */
    hyn_trap_expect(14, (u64)(uptr)hyn_trap_probe_pagefault_after);
    before = hyn_trap_count(14);
    hyn_trap_probe_pagefault(0x0000123456789ABCull);
    hit = hyn_trap_expect_hit(14);
    check("#PF 页错误捕获并修正 RIP 后继续",
          (hit == 1) && (hyn_trap_count(14) == before + 1),
          "");
}

/* ---- 自检：中断投递与时钟节拍 ---- */
static void selftest_interrupts(void)
{
    u64 t0, hal0;
    u64 spins = 0;
    int ok;

    kputs("\n---- 硬核底座自检：中断投递 ----\n");

    t0   = hyn_timer_ticks();
    hal0 = hal_ticks();

    arch_irq_enable();
    while (hyn_timer_ticks() < t0 + 5u && spins < 200000000ull) {
        arch_cpu_relax();
        spins++;
    }
    ok = (hyn_timer_ticks() >= t0 + 5u) && (hal_ticks() >= hal0 + 5u);
    arch_irq_disable();

    kprintf("  时钟节拍: arch=%u 次, HAL=%u 次, 等待自旋=%u\n",
            (u32)(hyn_timer_ticks() - t0), (u32)(hal_ticks() - hal0), (u32)spins);
    check("IRQ0 时钟中断投递到内核与 HAL", ok, "");
}

/* ---- 自检：物理页分配与 PML4 虚拟内存 ---- */
static void selftest_memory(void)
{
    const u64 stamp = 0x48594E55534D4D31ull;   /* 自检写入标记 */
    u64 p1, p2, pa_win, va_win = 0ull, va_win2 = 0ull, root, before, nonzero = 0ull;
    u32 free0, free_alloc, k;
    hyn_status_t st;
    int hit;

    kputs("\n---- 硬核底座自检：物理页分配与 PML4 虚拟内存 ----\n");

    /* ① 物理页分配：4K 对齐、内容清零（不泄漏上一任数据）、互不重叠、账目一致 */
    free0 = pmm_free_page_count();
    p1 = pmm_alloc_page();
    p2 = pmm_alloc_page();
    free_alloc = pmm_free_page_count();
    for (k = 0; k < 4096u; k++) {
        nonzero |= (u64)((volatile u8 *)(uptr)p1)[k];
    }
    check("物理页分配：4K 对齐 / 零填充 / 页不重叠 / 账目一致",
          (p1 != 0ull) && (p2 != 0ull) && (p1 != p2) &&
          ((p1 & 0xFFFull) == 0ull) && ((p2 & 0xFFFull) == 0ull) &&
          (nonzero == 0ull) && (free_alloc + 2u == free0), "");

    /* ② 归还与重复释放拦截 */
    check("物理页归还，且重复释放/越界释放被拒绝",
          (pmm_free_page(p1) == 1) && (pmm_free_page_count() + 1u == free0) &&
          (pmm_free_page(p1) == 0) && (pmm_free_page(0x1234000ull) == 0), "");
    (void)pmm_free_page(p2);

    /* ③ PML4 四级页表生效：CR3 指向新根，内核自身地址恒等可翻译，窗口尚未映射 */
    root = vmm_kernel_root();
    check("PML4 页表生效：CR3=新根 / 内核地址恒等翻译 / 窗口未映射",
          (root != 0ull) && (arch_mmu_current_root() == root) &&
          (vmm_translate((u64)(uptr)kernel_main) == (u64)(uptr)kernel_main) &&
          (vmm_translate(HYN_VMM_WINDOW_BASE) == 0ull), "");

    /* ④ 窗口映射：虚拟地址与物理地址解耦；写 VA、读 PA 命中的是同一个物理页 */
    {
        u64 readback = 0ull;
        st = vmm_alloc_window(&va_win);
        pa_win = vmm_translate(va_win);
        if (pa_win != 0ull) {
            *(volatile u64 *)(uptr)va_win = stamp;
            readback = *(volatile u64 *)(uptr)pa_win;
        }
        check("窗口映射：VA≠PA，写 VA 读 PA 命中同一物理页",
              (st == HYN_OK) && (pa_win != 0ull) && (pa_win != va_win) &&
              (readback == stamp), "");
    }

    /* ⑤ 取消映射：翻译归零，且硬件层真实访问被拦下（#PF，由陷阱框架捕获并修正 RIP） */
    before = hyn_trap_count(14);
    st = vmm_free_window(va_win);
    hyn_trap_expect(14, (u64)(uptr)hyn_trap_probe_pagefault_after);
    hyn_trap_probe_pagefault(va_win);
    hit = hyn_trap_expect_hit(14);
    check("取消映射后翻译归零，硬件层真实访问触发 #PF 并被捕获",
          (st == HYN_OK) && (vmm_translate(va_win) == 0ull) &&
          (hit == 1) && (hyn_trap_count(14) == before + 1u), "");

    /* ⑥ 取消映射后槽位可复用：说明资源真的被回收（不是只改了查询表） */
    st = vmm_alloc_window(&va_win2);
    check("窗口槽位取消映射后可复用（资源真实回收）",
          (st == HYN_OK) && (va_win2 == va_win), "");
    (void)vmm_free_window(va_win2);

    kprintf("  物理页: 受管 %u MiB / 可用 %u MiB / 累计分配 %u 次 / 页大小 4KiB\n",
            (u32)(pmm_managed_bytes() >> 20), (u32)(pmm_free_bytes() >> 20),
            pmm_alloc_count());
    kprintf("  页表  : PML4 根=%p / 恒等映射 %u MiB / 动态窗口=%p (%u 页)\n",
            (void *)(uptr)root, (u32)(HYN_VMM_IDENTITY_LIMIT >> 20),
            (void *)(uptr)HYN_VMM_WINDOW_BASE, HYN_VMM_WINDOW_PAGES);
}

/* ---- 自检：内核对象管理器 ---- */
static void obj_demo_on_destroy(void *body, void *ctx)
{
    HYN_UNUSED(ctx);
    if (body != NULL) {
        g_obj_demo_destroyed++;
    }
}

static void selftest_objects(void)
{
    hyn_status_t st;
    u64 obj_id = 0ull;
    hyn_handle_t h1 = 0ull, h2 = 0ull;
    hyn_handle_info_t info;
    hyn_object_id_t looked = 0ull;
    void *body = NULL;
    u32 size = 0u, k, leftover = 0u;

    kputs("\n---- 硬核底座自检：内核对象管理器 ----\n");

    st = hyn_class_register(HYN_OBJ_TYPE_SELFTEST, "自检对象", obj_demo_on_destroy, NULL);
    if (st != HYN_OK) {
        kprintf("  类型注册失败: status=%u\n", st);
    }
    memset(g_obj_demo_body, 0xAA, (unsigned long)sizeof(g_obj_demo_body));  /* 先塞脏数据 */

    st = hyn_object_create(HYN_OBJ_TYPE_SELFTEST, "core.selftest.object", HYN_DOMAIN_CORE,
                           g_obj_demo_body, (u32)sizeof(g_obj_demo_body), &obj_id, &h1);
    check("对象创建：返回对象 id 与内核句柄",
          (st == HYN_OK) && (obj_id != 0ull) && (h1 != 0ull), "");

    /* 句柄 -> 对象 -> 对象体 链路必须一致 */
    st = hyn_object_lookup(h1, &looked);
    check("句柄映射到唯一对象，对象体地址/容量/类型/域一致",
          (st == HYN_OK) && (looked == obj_id) &&
          (hyn_object_body(h1, &body, &size) == HYN_OK) &&
          (body == (void *)g_obj_demo_body) && (size == (u32)sizeof(g_obj_demo_body)) &&
          (hyn_handle_info(h1, &info) == HYN_OK) &&
          (info.type == HYN_OBJ_TYPE_SELFTEST) &&
          (info.domain == (u8)HYN_DOMAIN_CORE), "");

    /* 按名打开得到独立句柄；有句柄开着时销毁被拒绝（防悬挂引用） */
    st = hyn_object_open("core.selftest.object", &h2);
    check("按名打开得到独立句柄；存在未关闭句柄时销毁被拒绝",
          (st == HYN_OK) && (h2 != 0ull) && (h2 != h1) &&
          (hyn_object_destroy(obj_id) == HYN_EBUSY), "");

    /* 关闭全部句柄后销毁成功，类型清理回调被调用 */
    g_obj_demo_destroyed = 0u;
    (void)hyn_object_close(h1);
    (void)hyn_object_close(h2);
    st = hyn_object_destroy(obj_id);

    for (k = 0; k < sizeof(g_obj_demo_body); k++) {
        leftover += g_obj_demo_body[k];
    }
    check("销毁成功（清理回调触发），陈旧句柄与陈旧 id 全部失效",
          (st == HYN_OK) && (g_obj_demo_destroyed == 1u) &&
          (hyn_object_lookup(h1, &looked) == HYN_ENOENT) &&
          (hyn_object_body(h1, &body, &size) == HYN_ENOENT) &&
          (hyn_object_destroy(obj_id) == HYN_ENOENT), "");
    check("销毁后对象体已清零（脏数据不留给下一个使用者）", leftover == 0u, "");

    kprintf("  对象: 存活 %u / 累计创建 %u / 打开句柄 %u\n",
            hyn_object_count(), hyn_object_total(), hyn_handle_count());
}

/* ---- 自检：ALPC 端口消息总线 ---- */

/* 扮演"网络组服务"：收到请求后把应答发回请求方端口 */
static void alpc_selftest_handler(u64 port_id, const hyn_alpc_msg_t *msg, void *ctx)
{
    hyn_alpc_msg_t rep;
    hyn_status_t st;

    HYN_UNUSED(port_id);
    HYN_UNUSED(ctx);
    g_alpc_dispatched++;
    if (msg->type != HYN_ALPC_MSG_REQUEST || msg->reply_to == 0ull) {
        return;
    }
    hyn_alpc_msg_init(&rep, HYN_ALPC_MSG_REPLY, msg->msg_id,
                      msg->target_domain, msg->sender_domain, "OK", 2u);
    rep.sender_id = hyn_alpc_endpoint_of(port_id);  /* 回信同样要带发送端点，投递口据此校验许可 */
    st = hyn_alpc_send(msg->reply_to, &rep);
    if (st == HYN_OK) {
        g_alpc_reply_sent++;
    }
}

static void selftest_alpc(void)
{
    u64 agent = 0ull, svc = 0ull, reopened = 0ull, bogus = 0ull, rogue = 0ull;
    hyn_domain_t reopened_domain = HYN_DOMAIN_MAX;
    hyn_alpc_msg_t req, back, probe_msg;
    hyn_status_t st, st2;
    u32 n, i, ok_sends;

    kputs("\n---- 硬核底座自检：ALPC 端口消息总线 ----\n");

    /* ① 端口创建：core 域（Agent 侧）与 net 域（网络组服务侧） */
    st  = hyn_alpc_port_create("core.selftest.agent", HYN_DOMAIN_CORE, &agent);
    st2 = hyn_alpc_port_create("net.selftest.service", HYN_DOMAIN_NET, &svc);
    check("ALPC 跨域端口创建（core.* / net.*）",
          (st == HYN_OK) && (st2 == HYN_OK) &&
          (agent != 0ull) && (svc != 0ull) && (agent != svc), "");

    /* ② 命名域纪律：名字里声明的域与归属域不一致必须拒绝；同名重复创建必须拒绝 */
    check("端口名域与归属域不一致被拒绝 / 同名重复创建被拒绝",
          (hyn_alpc_port_create("net.selftest.bad", HYN_DOMAIN_CORE, &bogus) == HYN_EINVAL) &&
          (hyn_alpc_port_create("core.selftest.agent", HYN_DOMAIN_CORE, &bogus) == HYN_EBUSY), "");

    /* ③ 按名打开：拿到同一对象，归属域可核对 */
    st = hyn_alpc_port_open("net.selftest.service", &reopened, &reopened_domain);
    check("按名打开端口：对象 id 一致、归属域正确",
          (st == HYN_OK) && (reopened == svc) && (reopened_domain == HYN_DOMAIN_NET), "");

    /* ④ 跨域建链：请求 CORE->NET 与应答 NET->CORE 各需一条许可，故双向各建一条 */
    st  = hyn_alpc_connect(agent, svc);
    st2 = hyn_alpc_connect(svc, agent);
    check("跨域建链（CORE <-> NET），连接状态可查询",
          (st == HYN_OK) && (st2 == HYN_OK) &&
          (hyn_alpc_is_connected(agent, svc) == 1) &&
          (hyn_alpc_is_connected(svc, agent) == 1), "");

    /* ⑤ 跨域调用纪律：跨域非 ALPC 被拒绝；跨域经 ALPC / 同域直调放行 */
    check("跨域直接函数调用被拒绝，跨域经 ALPC 与同域直调放行",
          (hyn_domain_call_guard(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, 0) == HYN_EDOMAIN) &&
          (hyn_domain_call_guard(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, 1) == HYN_OK) &&
          (hyn_domain_call_guard(HYN_DOMAIN_NET, HYN_DOMAIN_NET, 0) == HYN_OK) &&
          (hyn_domain_blocked_count() >= 1u), "");

    /* ⑥ 跨域请求：CORE 域 agent -> NET 域服务，经派发被处理并回信 */
    hyn_alpc_msg_init(&req, HYN_ALPC_MSG_REQUEST, 0x4141u,
                      (u8)HYN_DOMAIN_CORE, (u8)HYN_DOMAIN_NET, "PING", 4u);
    req.sender_id = hyn_alpc_endpoint_of(agent);   /* 端点号由内核发号，调用方不自行推算 */
    req.reply_to  = agent;
    st = hyn_alpc_send(svc, &req);
    g_alpc_dispatched = 0u;
    g_alpc_reply_sent = 0u;
    n = hyn_alpc_dispatch(alpc_selftest_handler, NULL);
    check("跨域请求经 ALPC 投递、被服务侧处理并回信",
          (st == HYN_OK) && (n >= 1u) &&
          (g_alpc_dispatched == 1u) && (g_alpc_reply_sent == 1u), "");

    /* ⑦ 应答回流到请求方端口：序号/类型/负载/来源域逐项核对 */
    st = hyn_alpc_receive(agent, &back);
    check("应答回流到请求方端口，序号/类型/负载/来源域正确",
          (st == HYN_OK) && (back.type == HYN_ALPC_MSG_REPLY) &&
          (back.msg_id == 0x4141u) && (back.len == 2u) &&
          (back.data[0] == (u8)'O') && (back.data[1] == (u8)'K') &&
          (back.sender_domain == (u8)HYN_DOMAIN_NET) &&
          (back.target_domain == (u8)HYN_DOMAIN_CORE), "");
    check("空队列非阻塞收取返回 ENOENT（阻塞语义由 hyn_alpc_receive_wait 提供）",
          hyn_alpc_receive(agent, &back) == HYN_ENOENT, "");

    /* ⑧ 背压：队列深度 8，第 9 条投递必须被拒绝，而不是静默丢弃 */
    ok_sends = 0u;
    st2 = HYN_OK;
    for (i = 0u; i < HYN_ALPC_QUEUE_CAP + 1u; i++) {
        hyn_status_t s = hyn_alpc_send(svc, &req);
        if (s == HYN_OK) {
            ok_sends++;
        } else {
            st2 = s;
        }
    }
    check("队列满时投递返回 EBUSY（背压语义，不静默丢弃）",
          (ok_sends == HYN_ALPC_QUEUE_CAP) && (st2 == HYN_EBUSY), "");
    (void)hyn_alpc_dispatch(alpc_selftest_handler, NULL);
    while (hyn_alpc_receive(agent, &back) == HYN_OK) { }   /* 清空上一轮产生的应答 */

    /* ⑨ 连接许可的强度：跨域未建链投递被拒绝；同域（本就允许直调）不强制建链 */
    {
        hyn_status_t st_cross, st_same;
        hyn_alpc_msg_t sink;

        st = hyn_alpc_port_create("core.selftest.rogue", HYN_DOMAIN_CORE, &rogue);
        hyn_alpc_msg_init(&probe_msg, HYN_ALPC_MSG_NOTIFY, 0x5151u,
                          (u8)HYN_DOMAIN_CORE, (u8)HYN_DOMAIN_NET, "R", 1u);
        probe_msg.sender_id = hyn_alpc_endpoint_of(rogue);
        st_cross = hyn_alpc_send(svc, &probe_msg);        /* rogue 从未与 svc 建链 */

        hyn_alpc_msg_init(&probe_msg, HYN_ALPC_MSG_NOTIFY, 0x5152u,
                          (u8)HYN_DOMAIN_CORE, (u8)HYN_DOMAIN_CORE, "S", 1u);
        probe_msg.sender_id = hyn_alpc_endpoint_of(rogue);
        st_same = hyn_alpc_send(agent, &probe_msg);       /* 同域：不要求建链 */

        check("跨域投递需先建链（未建链被拒绝），同域不强制建链",
              (st == HYN_OK) && (st_cross == HYN_EDOMAIN) && (st_same == HYN_OK) &&
              (hyn_alpc_receive(agent, &sink) == HYN_OK) &&
              (hyn_alpc_permit_denied_count() >= 1u) && (hyn_alpc_pending(svc) == 0u), "");
        (void)hyn_object_destroy(rogue);
    }

    /* ⑩ 对象销毁与陈旧端点：端口被重置后端点立即失效、连接许可被撤销 */
    n = hyn_alpc_pending(svc);
    check("端口对象销毁：陈旧端点失效、连接许可撤销、消息不再可投递",
          (n == 0u) && (hyn_object_destroy(svc) == HYN_OK) &&
          (hyn_alpc_port_alive(svc) == 0) &&
          (hyn_alpc_port_domain(svc) == HYN_DOMAIN_MAX) &&
          (hyn_alpc_send(svc, &req) == HYN_ENOENT) &&
          (hyn_alpc_is_connected(agent, svc) == 0), "");

    kprintf("  端口: 存活 %u / 累计投递消息 %u 条 / 跨域直调拦截 %u 次 / 未建链投递拦截 %u 次\n",
            hyn_alpc_port_count(), hyn_alpc_msg_count(), hyn_domain_blocked_count(),
            hyn_alpc_permit_denied_count());
}

/* ==================================================================== */
/* 自检：进程/线程调度器                                                */
/* ==================================================================== */

/*
 * 主线程等条件就绪的统一姿势：一次只睡 1 个节拍，醒来检查一次。
 * 这样做的好处是"等待"完全不依赖当前线程的优先级——被等的线程（无论优先级高低）
 * 都能在空闲线程的节拍里正常推进；只睡 1 拍也保证不会因为抢先而漏掉任何状态。
 */
static int sched_wait_until(volatile u32 *flag, u32 want, u64 timeout_ticks)
{
    u64 deadline = hyn_sched_ticks() + timeout_ticks;

    while (*flag < want) {
        if (hyn_sched_ticks() >= deadline) {
            return 0;
        }
        (void)hyn_sched_block_timeout((void *)flag, 1ull);
    }
    return 1;
}

/* 优先级顺序自检：先建的 LOW 线程与后建的 HIGH 线程各自记录"我是第几个跑的" */
static volatile u32 g_order;
static u32 g_high_mark;
static u32 g_low_mark;
static const u8 g_prio_ids[2] = { 0u, 1u };     /* 0=LOW 先建，1=HIGH 后建 */

static void thread_prio_body(void *arg)
{
    u8 id = *(const u8 *)arg;

    if (id == g_prio_ids[0]) {
        g_low_mark  = ++g_order;                /* LOW：优先级低、后建 */
    } else {
        g_high_mark = ++g_order;                /* HIGH：优先级高、后建 */
    }
    hyn_thread_exit((id == g_prio_ids[0]) ? 0x1011 : 0x2022);
}

/* 合作式让出自检：主动让出 3 次后退出 */
static u32 g_coop_yields;

static void thread_coop_body(void *arg)
{
    u32 i;

    HYN_UNUSED(arg);
    for (i = 0u; i < 3u; i++) {
        g_coop_yields++;
        hyn_sched_yield();
    }
    hyn_thread_exit(0x1234);
}

/* 时间片抢占自检：低优先级自旋线程，只能被更高优先级线程抢占下去 */
static volatile u32 g_probe_spins;
static volatile u32 g_probe_stop;

static void thread_spin_body(void *arg)
{
    HYN_UNUSED(arg);
    while (!g_probe_stop) {
        g_probe_spins++;
    }
    hyn_thread_exit(0x4321);
}

static void selftest_scheduler(void)
{
    hyn_thread_t *high, *low, *yielder, *spinner;
    int woke_low, woke_high, woke_coop, woke_spin;
    u32 sw0, spins0;
    u64 t0;

    kputs("\n---- 硬核底座自检：进程/线程调度 ----\n");

    check("调度器初始化完成：内核主线程在册 + 空闲线程就绪（静态线程池）",
          (hyn_sched_ready() == 1) && (hyn_sched_live_count() == 2u) &&
          (hyn_sched_ready_count() == 1u), "");

    /* ① 线程创建：内核栈边界可核对、优先级与故障域随线程走 */
    high = hyn_thread_create("selftest.high", thread_prio_body, (void *)&g_prio_ids[1],
                             HYN_THREAD_PRIO_HIGH, (u8)HYN_DOMAIN_CORE);
    low  = hyn_thread_create("selftest.low",  thread_prio_body, (void *)&g_prio_ids[0],
                             HYN_THREAD_PRIO_LOW,  (u8)HYN_DOMAIN_CORE);
    check("创建内核线程：优先级/故障域/独立内核栈（%uKiB/线程）齐备",
          (high != NULL) && (low != NULL) && (high != low) &&
          (hyn_thread_prio(high) == HYN_THREAD_PRIO_HIGH) &&
          (hyn_thread_prio(low) == HYN_THREAD_PRIO_LOW) &&
          (hyn_thread_state(high) == HYN_THREAD_READY) &&
          ((hyn_thread_stack_high(high) - hyn_thread_stack_low(high)) == HYN_THREAD_STACK_SIZE) &&
          ((hyn_thread_stack_high(high) & 0xFull) == 0ull), "");

    /* ② 优先级：LOW 先建、HIGH 后建，先跑的必须是 HIGH */
    g_order = 0u; g_high_mark = 0u; g_low_mark = 0u;
    if (!sched_wait_until(&g_order, 2u, 200ull)) {
        kprintf("  优先级顺序等待超时：order=%u high=%u low=%u\n",
                g_order, g_high_mark, g_low_mark);
    }
    woke_high = hyn_sched_wait_thread(high, 200ull);
    woke_low  = hyn_sched_wait_thread(low, 200ull);
    check("优先级生效：后建的高优先级线程先于低优先级线程被调度",
          (g_high_mark == 1u) && (g_low_mark == 2u) &&
          (woke_high == HYN_SCHED_WOKE) && (woke_low == HYN_SCHED_WOKE) &&
          (hyn_thread_exit_code(high) == 0x2022) && (hyn_thread_exit_code(low) == 0x1011) &&
          (hyn_thread_state(high) == HYN_THREAD_DEAD) &&
          (hyn_thread_state(low) == HYN_THREAD_DEAD), "");

    /* ③ 合作式让出与"等线程退出"：让出 3 次 -> 运行 4 次 -> 退出码回传 */
    g_coop_yields = 0u;
    yielder = hyn_thread_create("selftest.yield", thread_coop_body, NULL,
                                HYN_THREAD_PRIO_NORMAL, (u8)HYN_DOMAIN_CORE);
    woke_coop = hyn_sched_wait_thread(yielder, 300ull);
    check("合作式让出：yield 3 次、被调度 4 次；等线程退出被唤醒并取回退出码",
          (yielder != NULL) && (woke_coop == HYN_SCHED_WOKE) &&
          (hyn_thread_yield_count(yielder) == 3u) &&
          (hyn_thread_run_count(yielder) >= 4u) &&
          (hyn_thread_exit_code(yielder) == 0x1234), "");

    /* ④ 时钟节拍抢占：低优先级自旋线程被更高优先级线程抢下去，而不是一直霸占 CPU */
    sw0 = hyn_sched_switch_count();
    t0  = hyn_timer_ticks();
    g_probe_spins = 0u;
    g_probe_stop  = 0u;
    spinner = hyn_thread_create("selftest.spin", thread_spin_body, NULL,
                                HYN_THREAD_PRIO_LOW, (u8)HYN_DOMAIN_CORE);
    hyn_sched_set_preempt(1);
    if (spinner != NULL) {
        hyn_sched_yield();      /* 让自旋线程先上 CPU；它开着中断，节拍会把它抢下来 */
        /*
         * 被抢占回来时至少已经过一个节拍。若自旋线程还活着，再等它被抢占一次，
         * 用节拍数兜底（节拍不足则说明抢占没发生，下面的断言会如实报出来）。
         */
        while (hyn_sched_preempt_count() == 0u && hyn_timer_ticks() < t0 + 20u) {
            (void)hyn_sched_block_timeout((void *)&g_probe_spins, 1ull);
        }
        g_probe_stop = 1u;
        spins0 = g_probe_spins;
        woke_spin = hyn_sched_wait_thread(spinner, 300ull);
    } else {
        spins0 = 0u;
        woke_spin = HYN_SCHED_TIMEOUT;
    }
    hyn_sched_set_preempt(0);
    {
        int ok_preempt = (spinner != NULL) && (hyn_sched_preempt_count() >= 1u) &&
                         (woke_spin == HYN_SCHED_WOKE) &&
                         (hyn_thread_run_count(spinner) >= 2u) &&
                         (hyn_thread_yield_count(spinner) == 0u) &&
                         (hyn_thread_exit_code(spinner) == 0x4321) &&
                         (hyn_sched_switch_count() > sw0) && (spins0 > 0u);

        if (!ok_preempt) {
            kprintf("  抢占明细：句柄%s 抢占计数=%u 唤醒=%d 自旋线程运行次数=%u "
                    "让出次数=%u 退出码=0x%x 切换 %u->%u 自旋次数=%u\n",
                    (spinner != NULL) ? "可用" : "为空", hyn_sched_preempt_count(), woke_spin,
                    (spinner != NULL) ? hyn_thread_run_count(spinner) : 0u,
                    (spinner != NULL) ? hyn_thread_yield_count(spinner) : 0u,
                    (spinner != NULL) ? (u32)hyn_thread_exit_code(spinner) : 0u,
                    sw0, hyn_sched_switch_count(), spins0);
        }
        check("时钟节拍抢占：更高优先级线程把低优先级自旋线程抢下去（时间片真的生效）",
              ok_preempt, "");
    }

    kprintf("  调度: 在册线程 %u / 切换 %u 次 / 抢占 %u 次 / 空闲线程运行 %u 次 / 节拍 %u\n",
            hyn_sched_live_count(), hyn_sched_switch_count(), hyn_sched_preempt_count(),
            hyn_sched_idle_run_count(), (u32)hyn_sched_ticks());
}

/* ==================================================================== */
/* 自检：syscall 唯一入口 与 Ring3 派发到 ALPC                          */
/* ==================================================================== */

/* Ring3 自检夹具：用户页 / 端口 / 线程 */
static u64 g_r3_good_rec, g_r3_good_code, g_r3_good_stack;
static u64 g_r3_fault_rec, g_r3_fault_code, g_r3_fault_stack;
static u64 g_r3_user_port, g_r3_svc_port;
static hyn_thread_t *g_r3_good_t, *g_r3_fault_t, *g_r3_svc_t;
static volatile u32 g_r3_svc_stop;
static u32 g_r3_svc_served;

/* 交给 Ring3 的栈顶参数块：与 arch/x86_64/ring3_prog.S 的 0/8/16(%rsp) 一一对应 */
typedef struct {
    u64 target_port;      /* 0(%rsp)  请求投递目标端口 */
    u64 record_va;        /* 8(%rsp)  记录页基址（程序写、内核读） */
    u64 self_port;        /* 16(%rsp) 自身身份端口 */
} ring3_args_t;

/* 线程入口用的发射参数：把"从哪进用户态"这件事留在内核侧 */
typedef struct {
    u64 entry_rip;
    u64 user_rsp;
} ring3_launch_t;

static ring3_launch_t g_r3_launch_good;
static ring3_launch_t g_r3_launch_fault;

/* 取一个用户页（U=1，已清零）；失败返回 0，由调用方按 0 判失败 */
static u64 user_page(void)
{
    u64 va = 0ull;

    if (vmm_alloc_user_page(&va) != HYN_OK) {
        return 0ull;
    }
    return va;
}

static u64 ring3_push_args(u64 stack_top, u64 target_port, u64 record_va, u64 self_port)
{
    u64 sp = (stack_top - 0x40ull) & ~0xFull;
    ring3_args_t *a = (ring3_args_t *)(uptr)sp;

    a->target_port = target_port;
    a->record_va   = record_va;
    a->self_port   = self_port;
    return sp;
}

/* 用户线程的入口：布置好现场后 iretq 进 Ring3 —— 之后的一切都在 CPL=3 里发生 */
static void ring3_user_body(void *arg)
{
    const ring3_launch_t *l = (const ring3_launch_t *)arg;

    hyn_arch_enter_user(l->entry_rip, l->user_rsp);      /* 不返回 */
}

/*
 * 扮演"网络组服务线程"：在 ALPC 语义下收取跨域请求并回信。
 * 用带超时的阻塞收取而不是死等，是为了让"服务线程该退出时能退出"——
 * 不依赖外部强杀，也不需要额外的唤醒通道。
 */
static void ring3_service_body(void *arg)
{
    u64 port = (u64)(uptr)arg;
    hyn_alpc_msg_t msg, rep;
    u8 echo[4];
    u32 i;

    for (;;) {
        hyn_status_t st = hyn_alpc_receive_wait(port, &msg, 50ull);

        if (st == HYN_OK && msg.type == HYN_ALPC_MSG_REQUEST && msg.len >= 4u) {
            for (i = 0u; i < 4u; i++) {
                echo[i] = msg.data[i];                    /* 原样回声，便于用户侧逐字节核对 */
            }
            hyn_alpc_msg_init(&rep, HYN_ALPC_MSG_REPLY, msg.msg_id,
                              (u8)HYN_DOMAIN_NET, (u8)HYN_DOMAIN_CORE, echo, 4u);
            rep.sender_id = hyn_alpc_endpoint_of(port);
            if (hyn_alpc_send(msg.reply_to, &rep) == HYN_OK) {
                g_r3_svc_served++;
            }
        }
        if (g_r3_svc_stop) {
            break;
        }
    }
}

static void selftest_syscall(void)
{
    hyn_trap_frame_t kf;
    hyn_ring3_record_t *rec;
    u64 need[6];
    u64 good_sz, fault_sz, i;
    u32 dpl3 = 0u, dpl3_vec = 0xFFFFFFFFu;
    u32 total0, user0, denied0, unknown0, spoof0, sent0;
    int woke_good, woke_fault, woke_svc;
    hyn_status_t st;

    kputs("\n---- 硬核底座自检：syscall 唯一入口与 Ring3 派发 ----\n");

    /* ① 唯一入口：逐项扫 IDT，DPL=3 的门有且仅有一个，且就是向量 0x80 */
    for (i = 0ull; i < (u64)HYN_TRAP_VECTORS; i++) {
        if (hyn_idt_gate_dpl((u32)i) == 3u) {
            dpl3++;
            dpl3_vec = (u32)i;
        }
    }
    check("IDT 全表扫描：DPL=3 的门有且仅有一个（0x80），其余一律 DPL=0",
          (dpl3 == 1u) && (dpl3_vec == HYN_TRAP_SYSCALL_VECTOR) &&
          (hyn_idt_gate_dpl(HYN_TRAP_SYSCALL_VECTOR) == 3u) &&
          (hyn_idt_get_handler(HYN_TRAP_SYSCALL_VECTOR) == (u64)(uptr)isr_128) &&
          (hyn_idt_get_attr(HYN_TRAP_SYSCALL_VECTOR) == HYN_IDT_ATTR_USER_INT_GATE) &&
          (hyn_idt_gate_dpl(3u) == 0u) && (hyn_idt_gate_dpl(14u) == 0u) &&
          (hyn_idt_gate_dpl(HYN_TRAP_IRQ_BASE) == 0u), "");

    /* ② 内核态调用"仅限 Ring3"的入口必须被拒；未知调用号必须有明确回音 */
    total0   = hyn_syscall_total();
    denied0  = hyn_syscall_kernel_denied();
    unknown0 = hyn_syscall_unknown_count();

    memset(&kf, 0, (unsigned long)sizeof(kf));
    kf.cs  = (u64)HYN_SEL_KCODE64;              /* 合成一个"内核态调用者"的陷阱帧 */
    kf.rax = (u64)HYN_SYS_ALPC_SEND;
    hyn_syscall_enter(&kf);
    {
        u64 denied_ret = kf.rax;

        memset(&kf, 0, (unsigned long)sizeof(kf));
        kf.cs  = (u64)HYN_SEL_KCODE64;
        kf.rax = 0x7777ull;                     /* 表中没有的调用号 */
        hyn_syscall_enter(&kf);
        check("内核态调用用户专属入口被拒（EPERM），未知调用号返回 ENOTSUP",
              (denied_ret == (u64)HYN_EPERM) && (kf.rax == (u64)HYN_ENOTSUP) &&
              (hyn_syscall_kernel_denied() == denied0 + 1u) &&
              (hyn_syscall_unknown_count() == unknown0 + 1u) &&
              (hyn_syscall_total() == total0 + 2u), "");
    }

    /* ③ 正向：内核态调用允许的入口（PING）——参数真到了内核，回显也真回来了 */
    memset(&kf, 0, (unsigned long)sizeof(kf));
    kf.cs  = (u64)HYN_SEL_KCODE64;
    kf.rax = (u64)HYN_SYS_PING;
    kf.rdi = (u64)HYN_SYS_PING_MAGIC;
    hyn_syscall_enter(&kf);
    check("内核态 PING：调用号派发到实现，参数与回显逐项一致",
          (kf.rax == (u64)HYN_OK) && (kf.rdx == (u64)HYN_SYS_PING_MAGIC), "");

    /* ④ 用户地址窗口：探针要的 6 个用户页全部映射（U=1），且不在内核恒等映射区内 */
    need[0] = g_r3_good_rec    = user_page();
    need[1] = g_r3_good_code   = user_page();
    need[2] = g_r3_good_stack  = user_page();
    need[3] = g_r3_fault_rec   = user_page();
    need[4] = g_r3_fault_code  = user_page();
    need[5] = g_r3_fault_stack = user_page();

    {
        /* 6 个页必须：非 0、4KiB 对齐、逐页连续（相邻差恰好 4096）、全落在用户窗口内、可翻译 */
        int win_ok = 1;

        for (i = 0ull; i < 6ull; i++) {
            if (need[i] == 0ull) {
                win_ok = 0;
                break;
            }
            if ((need[i] & 0xFFFull) != 0ull) {
                win_ok = 0;
                break;
            }
            if ((need[i] < (u64)HYN_USER_BASE) || (need[i] >= (u64)HYN_USER_TOP)) {
                win_ok = 0;
                break;
            }
            if (vmm_translate(need[i]) == 0ull) {
                win_ok = 0;
                break;
            }
            if ((i > 0ull) && (need[i] != need[i - 1ull] + 4096ull)) {
                win_ok = 0;
                break;
            }
        }
        check("Ring3 用户地址窗口：6 个页全部映射（U=1，落在 0x40_0000_0000 起）", win_ok != 0, "");
    }

    /* ⑤ 端口与服务线程：用户身份端口（CORE）与内核服务端口（NET）双向建链 */
    st = hyn_alpc_port_create("core.ring3.user", HYN_DOMAIN_CORE, &g_r3_user_port);
    if (st == HYN_OK) {
        st = hyn_alpc_port_create("net.ring3.service", HYN_DOMAIN_NET, &g_r3_svc_port);
    }
    if (st == HYN_OK) {
        st = hyn_alpc_connect(g_r3_user_port, g_r3_svc_port);
    }
    if (st == HYN_OK) {
        st = hyn_alpc_connect(g_r3_svc_port, g_r3_user_port);   /* 回信方向另需一条许可 */
    }
    g_r3_svc_served = 0u;
    g_r3_svc_t = hyn_thread_create("service.net", ring3_service_body,
                                   (void *)(uptr)g_r3_svc_port,
                                   HYN_THREAD_PRIO_NORMAL, (u8)HYN_DOMAIN_NET);
    check("Ring3 夹具就绪：用户身份端口 + NET 服务端口 + 服务线程（跨域双向建链）",
          (st == HYN_OK) && (g_r3_user_port != 0ull) && (g_r3_svc_port != 0ull) &&
          (g_r3_svc_t != NULL) &&
          (hyn_alpc_port_domain(g_r3_svc_port) == HYN_DOMAIN_NET) &&
          (hyn_alpc_is_connected(g_r3_user_port, g_r3_svc_port) == 1) &&
          (hyn_alpc_is_connected(g_r3_svc_port, g_r3_user_port) == 1), "");

    /* ⑥ 装填用户页：探针代码 + 记录页（清零）+ 用户栈参数块 */
    memset((void *)(uptr)g_r3_good_rec, 0, 4096ul);
    memset((void *)(uptr)g_r3_fault_rec, 0, 4096ul);
    /* 先转成整数再相减：两个符号是独立声明，指针相减属于未定义行为，编译器可能把结果折掉 */
    good_sz  = (u64)(uptr)__ring3_probe_good_end  - (u64)(uptr)__ring3_probe_good_start;
    fault_sz = (u64)(uptr)__ring3_probe_fault_end - (u64)(uptr)__ring3_probe_fault_start;
    kprintf("  [Ring3] 探针体积：正常流程 %llu 字节 / 越权访存 %llu 字节（各上限 4096）\n",
            good_sz, fault_sz);
    memcpy((void *)(uptr)g_r3_good_code,  __ring3_probe_good_start,  (unsigned long)good_sz);
    memcpy((void *)(uptr)g_r3_fault_code, __ring3_probe_fault_start, (unsigned long)fault_sz);
    check("Ring3 探针装载：两段代码体积明确、记录页已清零",
          (good_sz > 0ull) && (fault_sz > 0ull) &&
          (good_sz <= 4096ull) && (fault_sz <= 4096ull) &&
          (((volatile u64 *)(uptr)g_r3_good_rec)[0] == 0ull), "");

    /* ⑦ 创建两个 Ring3 用户线程：正常流程 + 越权访存 */
    g_r3_launch_good.entry_rip = g_r3_good_code;
    g_r3_launch_good.user_rsp  = ring3_push_args(g_r3_good_stack + 4096ull,
                                                g_r3_svc_port, g_r3_good_rec, g_r3_user_port);
    g_r3_launch_fault.entry_rip = g_r3_fault_code;
    g_r3_launch_fault.user_rsp  = ring3_push_args(g_r3_fault_stack + 4096ull,
                                                 g_r3_svc_port, g_r3_fault_rec, g_r3_user_port);

    user0 = hyn_syscall_user_count();
    g_r3_good_t  = hyn_thread_create("ring3.good", ring3_user_body, &g_r3_launch_good,
                                     HYN_THREAD_PRIO_NORMAL, (u8)HYN_DOMAIN_CORE);
    g_r3_fault_t = hyn_thread_create("ring3.fault", ring3_user_body, &g_r3_launch_fault,
                                     HYN_THREAD_PRIO_LOW, (u8)HYN_DOMAIN_CORE);
    if ((g_r3_good_t != NULL) && (st == HYN_OK)) {
        (void)hyn_thread_bind_port(g_r3_good_t, g_r3_user_port);   /* 用户线程的 ALPC 身份 */
        (void)hyn_thread_bind_port(g_r3_fault_t, g_r3_user_port);
    }
    check("创建 Ring3 用户线程：内核栈就绪、身份端口已绑定（用户无法伪造身份）",
          (g_r3_good_t != NULL) && (g_r3_fault_t != NULL) && (st == HYN_OK) &&
          (hyn_thread_port_id(g_r3_good_t) == g_r3_user_port), "");

    /* ⑧ 放行：开抢占，让 Ring3 线程与内核服务线程按优先级轮转 */
    hyn_sched_set_preempt(1);
    woke_good = hyn_sched_wait_thread(g_r3_good_t, 800ull);

    /* ⑨ 记录页 = 用户态自己写下的证据：程序读到的 %cs、内核给的线程 id */
    rec = (hyn_ring3_record_t *)(uptr)g_r3_good_rec;
    check("Ring3 记录页：程序自证 CPL=3（自读 %cs=0x2B），内核帧一致，线程 id 相符",
          (rec->magic == (u64)HYN_RING3_REC_MAGIC) &&
          (rec->cpl_arg == (u64)HYN_RING3_USER_CS) &&
          (rec->cpl_ret == 0ull) &&
          (rec->tid_ret == hyn_thread_id(g_r3_good_t)) &&
          (hyn_syscall_first_user_cs() == (u64)HYN_RING3_USER_CS) &&
          ((hyn_syscall_last_user_cs() & 3ull) == 3ull), "");

    /* ⑩ 负向：非法用户指针必须被拦（内核绝不按用户给的值去摸内存）；越权收他人端口被拒 */
    check("Ring3 负向：非法用户指针返回 EFAULT，越权收他人端口消息返回 EPERM",
          (rec->badptr_ret == (u64)HYN_EFAULT) &&
          (rec->foreign_ret == (u64)HYN_EPERM), "");

    /* ⑪ 正向：请求经 syscall 投进 ALPC -> 服务线程收到 -> 应答回流到用户端口 */
    check("Ring3 请求经 syscall 派发到 ALPC：服务侧应答复流，应答字段逐项正确",
          (rec->send_ret == 0ull) && (rec->recv_ret == 0ull) &&
          (rec->reply_type == (u64)HYN_ALPC_MSG_REPLY) &&
          (rec->reply_msg_id == 0x52513151ull) &&
          (rec->reply_domain == (u64)HYN_DOMAIN_NET) &&
          (rec->reply_len == 4ull) &&
          (rec->reply_word == 0x31305152ull) &&
          (rec->done == (u64)HYN_RING3_DONE) &&
          (g_r3_svc_served >= 1u) &&
          (woke_good == HYN_SCHED_WOKE) &&
          (hyn_thread_state(g_r3_good_t) == HYN_THREAD_DEAD), "");

    /* ⑫ 越权访存：只结束该用户线程 —— 退出码带向量、记录页没被写脏、内核继续跑 */
    woke_fault = hyn_sched_wait_thread(g_r3_fault_t, 300ull);
    check("Ring3 越权访存：只结束该线程（退出码带 #PF），内核与其余线程不受影响",
          (woke_fault == HYN_SCHED_WOKE) &&
          (hyn_thread_exit_code(g_r3_fault_t) == (int)(0x10000u | 14u)) &&
          (((volatile u64 *)(uptr)g_r3_fault_rec)[0] == 0ull) &&
          (hyn_user_fault_count() == 1u) &&
          (hyn_sched_ready() == 1) && (hyn_thread_current() != NULL), "");

    /* ⑬ 收尾：服务线程按自己的超时退出，不靠强杀 */
    g_r3_svc_stop = 1u;
    woke_svc = hyn_sched_wait_thread(g_r3_svc_t, 300ull);
    hyn_sched_set_preempt(0);
    check("ALPC 服务线程按超时轮询自行退出，等线程退出被唤醒",
          (woke_svc == HYN_SCHED_WOKE) && (hyn_thread_exit_code(g_r3_svc_t) == 0),
          "");

    /* ⑭ syscall 审计：次数、身份回填、越权与指针拦截逐项对得上 */
    spoof0 = 3u;                        /* 用户伪造了 sender_id / sender_domain / reply_to 三处 */
    sent0  = 1u;
    check("syscall 审计计数一致：用户 7 次 / 内核 3 次；身份回填 3 处；指针与越权各拦 1 次",
          (hyn_syscall_user_count() == user0 + 7u) &&
          (hyn_syscall_total() == total0 + 10u) &&
          (hyn_syscall_kernel_denied() == denied0 + 1u) &&
          (hyn_syscall_unknown_count() == unknown0 + 1u) &&
          (hyn_syscall_spoof_fixed() == spoof0) &&
          (hyn_syscall_alpc_sent() == sent0) &&
          (hyn_syscall_badptr_count() == 1u) &&
          (hyn_syscall_foreign_denied() == 1u) &&
          (hyn_syscall_noport_denied() == 0u), "");

    kprintf("  syscall: 调用 %u 次（用户 %u / 内核 %u）/ 身份回填 %u 处 / 非法指针拦截 %u 次\n",
            hyn_syscall_total(), hyn_syscall_user_count(),
            hyn_syscall_total() - hyn_syscall_user_count(),
            hyn_syscall_spoof_fixed(), hyn_syscall_badptr_count());
    kprintf("  Ring3  : 首帧 CS=0x%x RIP=%p / 用户线程异常 %u 次 / 经 syscall 投递 ALPC %u 条\n",
            (u32)hyn_syscall_first_user_cs(), (void *)(uptr)hyn_syscall_first_user_rip(),
            hyn_user_fault_count(), hyn_syscall_alpc_sent());
}

/* ---- T6：快通道分组故障域自检 ---- */

static u32 dom_state(hyn_domain_t d)
{
    const hyn_group_info_t *i = hyn_domain_info(d);

    return (i != NULL) ? i->state : (u32)HYN_DOMAIN_STATE_OFFLINE;
}

static void selftest_domains(void)
{
    hyn_status_t        st;
    hyn_status_t        st2;
    u32                 ports0, calls0, mis0, fault0;
    u32                 epoch0[HYN_DOMAIN_MAX];
    u32                 reset0[HYN_DOMAIN_MAX];
    u32                 served_net0, len_actual;
    u32                 blocked0, denied0, echo, d;
    u64                 st_svc0;
    hyn_group_ping_t    ping;
    hyn_group_ping_t    ping2;
    hyn_storage_probe_t sp;
    hyn_storage_chain_t sc;
    hyn_net_probe_t     np;
    hyn_bus_probe_t     bp;

    /*
     * 三个读数结构体必须先清零：跨域调用一旦失败（返回错误码），本组不会写入任何字节，
     * 若不清零，后面打印出来的就是栈上的历史垃圾——看起来像"读到了硬件寄存器"，
     * 实际是伪造的硬件事实。清零后读数为 0 就明确表示"这次没读成"。
     */
    memset(&sp, 0, (unsigned long)sizeof(sp));
    memset(&sc, 0, (unsigned long)sizeof(sc));
    memset(&np, 0, (unsigned long)sizeof(np));
    memset(&bp, 0, (unsigned long)sizeof(bp));

    kputs("\n---- 快通道分组故障域：注册、跨域消息往返、单独重置 ----\n");

    /* 三组服务线程与"重置->重建服务线程"都要靠抢占轮转，本段自检期间打开抢占 */
    hyn_sched_set_preempt(1);

    ports0 = hyn_alpc_port_count();
    st     = hyn_domain_init();
    calls0 = hyn_domain_call_total();

    /* ① 分组结构：底座 + 三组在位；端口、服务端口域归属、服务线程逐项核对 */
    check("故障域初始化：硬核底座 + 存储/网络/硬件总线三组在位，7 条端口建链、三组服务线程就绪",
          (st == HYN_OK) &&
          (hyn_domain_registered(HYN_DOMAIN_CORE) == 1) &&
          (hyn_domain_registered(HYN_DOMAIN_STORAGE) == 1) &&
          (hyn_domain_registered(HYN_DOMAIN_NET) == 1) &&
          (hyn_domain_registered(HYN_DOMAIN_BUS) == 1) &&
          (dom_state(HYN_DOMAIN_CORE) == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (dom_state(HYN_DOMAIN_STORAGE) == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (dom_state(HYN_DOMAIN_NET) == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (dom_state(HYN_DOMAIN_BUS) == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (hyn_domain_svc_port(HYN_DOMAIN_CORE) == 0ull) &&   /* 底座不设服务端口，只发不接 */
          (hyn_alpc_port_domain(hyn_domain_svc_port(HYN_DOMAIN_STORAGE)) == HYN_DOMAIN_STORAGE) &&
          (hyn_alpc_port_domain(hyn_domain_svc_port(HYN_DOMAIN_NET)) == HYN_DOMAIN_NET) &&
          (hyn_alpc_port_domain(hyn_domain_svc_port(HYN_DOMAIN_BUS)) == HYN_DOMAIN_BUS) &&
          (hyn_domain_svc_thread(HYN_DOMAIN_STORAGE) != NULL) &&
          (hyn_domain_svc_thread(HYN_DOMAIN_NET) != NULL) &&
          (hyn_domain_svc_thread(HYN_DOMAIN_BUS) != NULL) &&
          (hyn_alpc_port_count() == ports0 + 7u), "");
    kprintf("  故障域: 底座=%s / 存储组=%s / 网络组=%s / 总线组=%s；端口 %u -> %u\n",
            hyn_domain_name(HYN_DOMAIN_CORE), hyn_domain_name(HYN_DOMAIN_STORAGE),
            hyn_domain_name(HYN_DOMAIN_NET), hyn_domain_name(HYN_DOMAIN_BUS),
            ports0, hyn_alpc_port_count());

    /* ② HAL 骨架：五类最简外设驱动均已登记并绑定到对应故障域 */
    {
        const hal_dev_kind_t kinds[5] = { HAL_DEV_FRAMEBUFFER, HAL_DEV_USB, HAL_DEV_HDA,
                                          HAL_DEV_NVME, HAL_DEV_NET };
        u32 present = 0u;
        u32 i;

        for (i = 0u; i < 5u; i++) {
            if (hal_device_by_kind(kinds[i]) != NULL) {
                present++;
            }
        }
        check("HAL 骨架：Framebuffer/USB/HDA/NVMe/网卡 五类设备均已登记",
              present == 5u, "");
        kprintf("  HAL  : 设备共 %u 个；按域 底座=%u 存储=%u 网络=%u 总线=%u；五类外设登记 %u/5\n",
                hal_device_count(),
                hal_device_count_domain(HYN_DOMAIN_CORE),
                hal_device_count_domain(HYN_DOMAIN_STORAGE),
                hal_device_count_domain(HYN_DOMAIN_NET),
                hal_device_count_domain(HYN_DOMAIN_BUS), present);
    }

    /* ③ 跨域消息往返：底座 -> 网络组，回声 / 纪元 / 状态 / 资源逐项一致 */
    served_net0 = hyn_domain_info(HYN_DOMAIN_NET)->msg_served;
    echo = 0x434F5245u;                                  /* 'CORE' */
    memset(&ping, 0, (unsigned long)sizeof(ping));
    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_PING,
                         &echo, 4u, &ping, (u32)sizeof(ping), &len_actual);
    check("跨域调用强制走 ALPC：底座 -> 网络组 PING，回声参数与组状态经消息往返逐项一致",
          (st == HYN_OK) && (len_actual == (u32)sizeof(ping)) &&
          (ping.op_echo == echo) &&
          (ping.domain == (u32)HYN_DOMAIN_NET) &&
          (ping.state == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (ping.epoch == 1u) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->msg_served == served_net0 + 1u) &&
          (hyn_domain_call_total() == calls0 + 1u) &&
          (hyn_domain_msg_discarded() == 0u), "");

    /* ④ 纪律的代码形态：同域直调放行；跨域直调拒绝；同域也不许占消息总线 */
    memset(&ping, 0, (unsigned long)sizeof(ping));
    st  = hyn_group_local_call(HYN_DOMAIN_NET, HYN_DOMAIN_NET, HYN_GROUP_OP_PING,
                               &echo, 4u, &ping, (u32)sizeof(ping), &len_actual);
    blocked0 = hyn_domain_blocked_count();
    st2 = hyn_group_local_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_PING,
                               &echo, 4u, &ping, (u32)sizeof(ping), &len_actual);
    check("域纪律：同域直调可用；跨域直调被拒并记账；同域走消息总线也被拒（EDOMAIN）",
          (st == HYN_OK) && (ping.op_echo == echo) &&
          (st2 == HYN_EDOMAIN) && (hyn_domain_blocked_count() == blocked0 + 1u) &&
          (hyn_domain_call(HYN_DOMAIN_BUS, HYN_DOMAIN_BUS, HYN_GROUP_OP_PING,
                           &echo, 4u, &ping, (u32)sizeof(ping), &len_actual) == HYN_EDOMAIN),
          "");

    /* ⑤ 存储组读数：全部来自真实寄存器与共享枚举结果，不做推导 */
    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_STORAGE, HYN_GROUP_OP_STORAGE_PROBE,
                         NULL, 0u, &sp, (u32)sizeof(sp), &len_actual);
    check("存储组读数：NVMe 类别号 0x0108 真读、BAR/版本/就绪位与 VFS 骨架节点一致",
          (st == HYN_OK) && (len_actual == (u32)sizeof(sp)) &&
          (sp.nvme_class == 0x0108u) &&
          (sp.nvme_found == ((hal_device_by_kind(HAL_DEV_NVME) != NULL) ? 1u : 0u)) &&
          (sp.vfs_nodes >= 4u) &&
          (sp.vfs_mounts >= sp.nvme_found) &&
          ((sp.nvme_found == 0u) || (sp.reg_ok == 1u)), "");
    kprintf("  存储组: NVMe=%u 类别=0x%04x 厂商=0x%04x 设备=0x%04x BAR=%u KiB @0x%x%08x 版本=0x%x CSTS=0x%x VFS 节点=%u 挂载=%u\n",
            sp.nvme_found, sp.nvme_class, (u32)sp.pci_vendor, (u32)sp.pci_device,
            sp.bar0_size / 1024u,
            (u32)vfs_nvme_regs()->bar0_base_hi, (u32)vfs_nvme_regs()->bar0_base_lo,
            sp.version, sp.csts, sp.vfs_nodes, sp.vfs_mounts);

    /* ⑥ 组到组也走 ALPC：存储组服务线程内部再调网络组 */
    served_net0 = hyn_domain_info(HYN_DOMAIN_NET)->msg_served;
    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_STORAGE, HYN_GROUP_OP_STORAGE_CHAIN,
                         NULL, 0u, &sc, (u32)sizeof(sc), &len_actual);
    check("组到组也走 ALPC：存储组内层调网络组成功、内层未被拒、网络组服务计数随之增长",
          (st == HYN_OK) && (len_actual == (u32)sizeof(sc)) &&
          (sc.chain_ok == (u32)HYN_OK) &&
          (sc.chain_denied_before == hyn_domain_call_denied()) &&
          (sc.net_ping.op_echo == 0x53544F52u) &&
          (sc.net_ping.domain == (u32)HYN_DOMAIN_NET) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->msg_served == served_net0 + 1u), "");

    /* ⑦ 单组重置：本组换代，底座与其他两组纪元/重置计数一律不变 */
    for (d = 0u; d < (u32)HYN_DOMAIN_MAX; d++) {
        epoch0[d] = hyn_domain_info((hyn_domain_t)d)->epoch;
        reset0[d] = hyn_domain_info((hyn_domain_t)d)->reset_count;
    }
    st_svc0 = hyn_thread_id(hyn_domain_svc_thread(HYN_DOMAIN_STORAGE));
    st = hyn_domain_reset(HYN_DOMAIN_STORAGE);
    check("单独重置存储组：quiesce->reset->probe 走全、本组纪元+1 与服务线程换代，底座与其余两组不受影响",
          (st == HYN_OK) &&
          (dom_state(HYN_DOMAIN_STORAGE) == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (hyn_domain_info(HYN_DOMAIN_STORAGE)->epoch == epoch0[HYN_DOMAIN_STORAGE] + 1u) &&
          (hyn_domain_info(HYN_DOMAIN_STORAGE)->reset_count == reset0[HYN_DOMAIN_STORAGE] + 1u) &&
          (hyn_domain_info(HYN_DOMAIN_STORAGE)->quiesce_count >= 1u) &&
          (hyn_domain_pending_reset(HYN_DOMAIN_STORAGE) == 0) &&
          (hyn_domain_svc_thread(HYN_DOMAIN_STORAGE) != NULL) &&
          (hyn_thread_id(hyn_domain_svc_thread(HYN_DOMAIN_STORAGE)) != st_svc0) &&
          (hyn_domain_info(HYN_DOMAIN_CORE)->epoch == epoch0[HYN_DOMAIN_CORE]) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->epoch == epoch0[HYN_DOMAIN_NET]) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->reset_count == reset0[HYN_DOMAIN_NET]) &&
          (hyn_domain_info(HYN_DOMAIN_BUS)->epoch == epoch0[HYN_DOMAIN_BUS]) &&
          (hyn_domain_info(HYN_DOMAIN_BUS)->reset_count == reset0[HYN_DOMAIN_BUS]) &&
          (hyn_alpc_port_count() == ports0 + 7u), "");

    memset(&ping, 0, (unsigned long)sizeof(ping));
    memset(&ping2, 0, (unsigned long)sizeof(ping2));
    st  = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_PING,
                          &echo, 4u, &ping, (u32)sizeof(ping), &len_actual);
    st2 = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_STORAGE, HYN_GROUP_OP_PING,
                          &echo, 4u, &ping2, (u32)sizeof(ping2), &len_actual);
    check("重置期间底座不停摆：重置后底座 -> 网络组、底座 -> 存储组均可继续通话（存储组纪元为 2）",
          (st == HYN_OK) && (st2 == HYN_OK) &&
          (ping.domain == (u32)HYN_DOMAIN_NET) && (ping.epoch == 1u) &&
          (ping2.domain == (u32)HYN_DOMAIN_STORAGE) && (ping2.epoch == 2u), "");

    /* ⑧ 网络组读数：网卡寄存器真读 + IPv4 首部校验和真算（与内核侧独立复算交叉核对） */
    {
        u8  hdr[20];
        u32 cksum_local;

        memset(hdr, 0, (unsigned long)sizeof(hdr));
        hdr[0]  = 0x45u; hdr[1] = 0x00u;
        hdr[2]  = 0x00u; hdr[3] = 0x14u;
        hdr[4]  = 0x1Cu; hdr[5] = 0x46u;
        hdr[6]  = 0x40u; hdr[7] = 0x00u;
        hdr[8]  = 64u;   hdr[9] = 6u;
        hdr[12] = 192u;  hdr[13] = 168u; hdr[14] = 1u;  hdr[15] = 10u;
        hdr[16] = 10u;   hdr[17] = 0u;   hdr[18] = 0u;  hdr[19] = 1u;
        cksum_local = net_ip_checksum(hdr, 20u);

        st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_NET_PROBE,
                             NULL, 0u, &np, (u32)sizeof(np), &len_actual);
        check("网络组读数：网卡寄存器真读、IPv4 校验和真算（回填后为 0，且与内核侧独立复算一致）、连接表无伪造",
              (st == HYN_OK) && (len_actual == (u32)sizeof(np)) &&
              (np.nic_found == ((hal_device_by_kind(HAL_DEV_NET) != NULL) ? 1u : 0u)) &&
              ((np.nic_found == 0u) || (np.reg_ok == 1u)) &&
              (np.cksum_expected == cksum_local) && (np.cksum_result == 0u) &&
              (np.conns == 3u), "");
        kprintf("  网络组: 网卡=%u 厂商=0x%04x 设备=0x%04x BAR=%u KiB @0x%x%08x 链路=%u MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                np.nic_found, (u32)np.pci_vendor, (u32)np.pci_device, np.bar0_size / 1024u,
                (u32)net_nic_regs()->bar0_base_hi, (u32)net_nic_regs()->bar0_base_lo,
                np.link_up,
                (u32)((np.mac_hi >> 8) & 0xFFu), (u32)(np.mac_hi & 0xFFu),
                (u32)((np.mac_lo >> 24) & 0xFFu), (u32)((np.mac_lo >> 16) & 0xFFu),
                (u32)((np.mac_lo >> 8) & 0xFFu),  (u32)(np.mac_lo & 0xFFu));
        kprintf("          校验和: 期望=0x%04x（内核复算 0x%04x）/ 回填后复算=0x%04x / LISTEN 槽位=%u / 已建立连接=%u（不伪造）\n",
                np.cksum_expected, cksum_local, np.cksum_result,
                np.conns, net_established_count());
    }

    /* ⑨ 硬件总线组读数：PCI 清点与共享枚举一致；ACPI 扫描确已执行 */
    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_BUS, HYN_GROUP_OP_BUS_ENUM,
                         NULL, 0u, &bp, (u32)sizeof(bp), &len_actual);
    {
        /*
         * 对照口径要选对：hal_pci_scan_count() 是"扫描次数"，不是"设备数"——
         * 拿它当设备数比，语义就是错的（且必然不等）。这里由硬核底座侧重新做一次
         * 盘点，与分组读数比对同一张共享枚举表的**设备数**；同时要求扫描次数 ≥1，
         * 证明分组的读数确实来自"扫过一遍"的真枚举，而不是编出来的数字。
         */
        u32 ref_devices = hal_pci_rescan();     /* 底座侧独立复清点：返回本次枚举到的设备数 */
        u32 ref_count   = 0u;

        (void)hal_pci_table(&ref_count);
        check("硬件总线组读数：PCI 清点与共享枚举结果一致；ACPI 扫描已执行且若找到则校验和必须通过",
              (st == HYN_OK) && (len_actual == (u32)sizeof(bp)) &&
              (bp.pci_devices >= 1u) && (bp.pci_devices == ref_count) &&
              (ref_devices == ref_count) && (hal_pci_scan_count() >= 1u) &&
              (bp.pci_bridges <= bp.pci_devices) &&
              ((bp.acpi_rsdp_found == 0u) ||
               ((bp.acpi_checksum_ok == 1u) && (bp.acpi_tables_ok == bp.acpi_tables))), "");
        kprintf("  总线组: PCI 设备=%u（桥 %u；底座独立复清点=%u，累计扫描 %u 次）；ACPI RSDP=%u 版本=%u 校验和=%u 表=%u/%u RSDT=0x%x OEM=%s 首表=%s\n",
                bp.pci_devices, bp.pci_bridges, ref_count, hal_pci_scan_count(),
                bp.acpi_rsdp_found, bp.acpi_revision,
                bp.acpi_checksum_ok, bp.acpi_tables_ok, bp.acpi_tables,
                bp.rsdt_address, bp.oem_id, bp.first_table);
    }

    /* ⑩ 分组异常捕获：网络组服务线程故意撞未映射地址 —— 只影响本组 */
    fault0      = hyn_domain_fault_total();
    denied0     = hyn_domain_call_denied();
    epoch0[HYN_DOMAIN_NET] = hyn_domain_info(HYN_DOMAIN_NET)->epoch;
    reset0[HYN_DOMAIN_NET] = hyn_domain_info(HYN_DOMAIN_NET)->reset_count;
    served_net0 = hyn_domain_info(HYN_DOMAIN_NET)->msg_served;
    /*
     * 就地隔离的基线必须紧贴故障注入点采集：存储组刚刚在 ⑦ 做过一次**计划内**重置，
     * 若沿用函数开头的老基线，就会把那次计划内重置误算成"被网络组的故障牵连"。
     * 这里把底座与另外两组的基线一并刷新，比对口径才与"故障是否外溢"这个问题对齐。
     */
    epoch0[HYN_DOMAIN_CORE]    = hyn_domain_info(HYN_DOMAIN_CORE)->epoch;
    reset0[HYN_DOMAIN_CORE]    = hyn_domain_info(HYN_DOMAIN_CORE)->reset_count;
    epoch0[HYN_DOMAIN_STORAGE] = hyn_domain_info(HYN_DOMAIN_STORAGE)->epoch;
    reset0[HYN_DOMAIN_STORAGE] = hyn_domain_info(HYN_DOMAIN_STORAGE)->reset_count;
    epoch0[HYN_DOMAIN_BUS]     = hyn_domain_info(HYN_DOMAIN_BUS)->epoch;
    reset0[HYN_DOMAIN_BUS]     = hyn_domain_info(HYN_DOMAIN_BUS)->reset_count;

    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_CRASH,
                         NULL, 0u, NULL, 0u, NULL);
    check("分组异常捕获：网络组服务线程触发内核态 #PF 后本组标记 FAULTED，调用方拿到“对方不可用”而非挂死",
          (st == HYN_EFAULT_FAULT) &&
          (dom_state(HYN_DOMAIN_NET) == (u32)HYN_DOMAIN_STATE_FAULTED) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->fault_count == 1u) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->last_fault_vector == 14u) &&
          (hyn_domain_fault_total() == fault0 + 1u) &&
          (hyn_domain_pending_reset(HYN_DOMAIN_NET) == 1), "");
    kprintf("  故障隔离: 网络组状态=%s 向量=%u 出错线程=%s；底座 %s(纪元%u) / 存储组 %s(纪元%u) 未受影响\n",
            hyn_domain_state_name((hyn_domain_state_t)dom_state(HYN_DOMAIN_NET)),
            hyn_domain_info(HYN_DOMAIN_NET)->last_fault_vector,
            hyn_domain_info(HYN_DOMAIN_NET)->last_fault_thread_name,
            hyn_domain_state_name((hyn_domain_state_t)dom_state(HYN_DOMAIN_CORE)),
            hyn_domain_info(HYN_DOMAIN_CORE)->epoch,
            hyn_domain_state_name((hyn_domain_state_t)dom_state(HYN_DOMAIN_STORAGE)),
            hyn_domain_info(HYN_DOMAIN_STORAGE)->epoch);

    st  = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_BUS, HYN_GROUP_OP_PING,
                          &echo, 4u, &ping, (u32)sizeof(ping), &len_actual);
    st2 = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_STORAGE, HYN_GROUP_OP_PING,
                          &echo, 4u, &ping, (u32)sizeof(ping), &len_actual);
    check("故障就地隔离：底座纪元不变、其余两组纪元与重置计数不变，且仍能正常互调",
          (hyn_domain_info(HYN_DOMAIN_CORE)->epoch == epoch0[HYN_DOMAIN_CORE]) &&
          (hyn_domain_info(HYN_DOMAIN_CORE)->reset_count == reset0[HYN_DOMAIN_CORE]) &&
          (hyn_domain_info(HYN_DOMAIN_STORAGE)->epoch == epoch0[HYN_DOMAIN_STORAGE]) &&
          (hyn_domain_info(HYN_DOMAIN_STORAGE)->reset_count == reset0[HYN_DOMAIN_STORAGE]) &&
          (hyn_domain_info(HYN_DOMAIN_BUS)->epoch == epoch0[HYN_DOMAIN_BUS]) &&
          (hyn_domain_info(HYN_DOMAIN_BUS)->reset_count == reset0[HYN_DOMAIN_BUS]) &&
          (st == HYN_OK) && (st2 == HYN_OK), "");
    kprintf("          就地隔离明细: 底座纪元%u->%u 重置%u->%u；存储组纪元%u->%u 重置%u->%u；总线组纪元%u->%u 重置%u->%u；隔离期互调 底座->总线 st=%u / 底座->存储 st=%u\n",
            epoch0[HYN_DOMAIN_CORE], hyn_domain_info(HYN_DOMAIN_CORE)->epoch,
            reset0[HYN_DOMAIN_CORE], hyn_domain_info(HYN_DOMAIN_CORE)->reset_count,
            epoch0[HYN_DOMAIN_STORAGE], hyn_domain_info(HYN_DOMAIN_STORAGE)->epoch,
            reset0[HYN_DOMAIN_STORAGE], hyn_domain_info(HYN_DOMAIN_STORAGE)->reset_count,
            epoch0[HYN_DOMAIN_BUS], hyn_domain_info(HYN_DOMAIN_BUS)->epoch,
            reset0[HYN_DOMAIN_BUS], hyn_domain_info(HYN_DOMAIN_BUS)->reset_count,
            (u32)st, (u32)st2);

    check("已故障的组明确拒绝服务：再调网络组返回 EFAULT_FAULT 并计入拒绝数（绝不假装成功）",
          (hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_PING,
                           &echo, 4u, &ping, (u32)sizeof(ping), &len_actual)
           == HYN_EFAULT_FAULT) &&
          (hyn_domain_call_denied() > denied0), "");

    /* ⑪ 安全点重置：故障组自行回到在线，硬件重新认领，消息服务恢复 */
    st = hyn_domain_service_pending();
    check("安全点重置故障组：quiesce->reset->probe 走全，网络组回到在线、纪元+1、待重置清零、故障史保留",
          (st == HYN_OK) &&
          (dom_state(HYN_DOMAIN_NET) == (u32)HYN_DOMAIN_STATE_ONLINE) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->epoch == epoch0[HYN_DOMAIN_NET] + 1u) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->reset_count == reset0[HYN_DOMAIN_NET] + 1u) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->fault_count == 1u) &&
          (hyn_domain_pending_reset(HYN_DOMAIN_NET) == 0) &&
          (hyn_domain_svc_thread(HYN_DOMAIN_NET) != NULL), "");

    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_NET_PROBE,
                         NULL, 0u, &np, (u32)sizeof(np), &len_actual);
    check("重置后网络组硬件重新认领成功、消息服务恢复（服务计数继续增长）",
          (st == HYN_OK) && (len_actual == (u32)sizeof(np)) &&
          ((np.nic_found == 0u) || (np.reg_ok == 1u)) &&
          (hyn_domain_info(HYN_DOMAIN_NET)->msg_served > served_net0), "");

    /* ⑫ 审计收尾：答应上了、没丢；底座不可重置不可静默 */
    mis0 = hyn_domain_msg_discarded();
    check("跨域审计汇总：往返成功多次、应答配对无丢弃、审计行可读，且底座不可重置/不可静默",
          (hyn_domain_call_total() >= 6u) && (mis0 == 0u) &&
          (hyn_domain_last_msg()[0] != '\0') &&
          (hyn_domain_reset(HYN_DOMAIN_CORE) == HYN_EDOMAIN) &&
          (hyn_domain_quiesce(HYN_DOMAIN_CORE) == HYN_EDOMAIN), "");
    kprintf("  跨域审计: 往返成功 %u 次 / 被拒 %u 次 / 跨域直调拦下 %u 次 / 应答丢弃 %u 条 / 最近: %s\n",
            hyn_domain_call_total(), hyn_domain_call_denied(),
            hyn_domain_blocked_count(), hyn_domain_msg_discarded(),
            hyn_domain_last_msg());

    hyn_sched_set_preempt(0);
}

/* ==================================================================== */
/* 自检：libhyn 用户态库与 Ring3 示例程序（T7）                          */
/*   完成条件：示例程序经 libhyn 发起的 ALPC 请求能被内核正确响应。      */
/*   做法：两份用户程序镜像整段装载到动态分配的用户页，作为真实 Ring3    */
/*   线程执行；用户侧只能经 int $0x80 收发 ALPC 消息，内核侧在安全点替   */
/*   core.req 应答（同域）、网络组服务线程应答（跨域）。不给用户态留后门。*/
/* ==================================================================== */

/*
 * 参数块：T5 的 ring3_args_t 是三字段（配 arch/x86_64/ring3_prog.S 探针），
 * 用户态库程序的入口桩 user/lib/entry.S 读的是四字段（hyn_abi_user_args_t，
 * 多一个 arena_va @0x18）。两份夹具刻意分开，互不复用、互不牵连。
 */
typedef struct {
    u64 target_port;   /* 0x00 请求投递目标：core.req（同域）/ net.svc（跨域） */
    u64 record_va;     /* 0x08 记录页：用户写、内核读 */
    u64 self_port;     /* 0x10 自身身份端口（内核绑定，用户改不了） */
    u64 arena_va;      /* 0x18 预留消息缓冲区页；当前恒 0，镜像按此断言 */
} ring3_args4_t;

static u64 g_lu_shell_code, g_lu_shell_stack, g_lu_shell_rec;
static u64 g_lu_agent_code, g_lu_agent_stack, g_lu_agent_rec;
static u64 g_lu_shell_port, g_lu_agent_port;
static hyn_thread_t *g_lu_shell_t;
static hyn_thread_t *g_lu_agent_t;
static ring3_launch_t g_lu_launch_shell, g_lu_launch_agent;
static u32 g_lu_served_core;   /* 底座安全点替 core.req 实际应答的条数 */
static u32 g_lu_net_growth;    /* 网络组服务计数增量（跨域往返的直接证据） */

/* 连续申请 n 个用户页；任一页失败、或地址不再连续（bump 分配下必然连续），一律返回 0 */
static u64 user_pages(u32 n)
{
    u64 first = 0ull, prev = 0ull, va;
    u32 i;

    for (i = 0u; i < n; i++) {
        va = user_page();
        if (va == 0ull) {
            return 0ull;
        }
        if (i == 0u) {
            first = va;
        } else if (va != prev + HYN_MMU_PAGE_SIZE) {
            return 0ull;
        }
        prev = va;
    }
    return first;
}

/* 一组用户页的硬校验：非 0 / 4KiB 对齐 / 全落在用户窗口内 / 每页都真的映射到物理页 */
static int user_pages_ok(u64 base, u32 n)
{
    u32 i;

    if ((base == 0ull) || ((base & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull)) {
        return 0;
    }
    if ((base < (u64)HYN_USER_BASE) ||
        (base + (u64)n * HYN_MMU_PAGE_SIZE > (u64)HYN_USER_TOP)) {
        return 0;
    }
    for (i = 0u; i < n; i++) {
        if (vmm_translate(base + (u64)i * HYN_MMU_PAGE_SIZE) == 0ull) {
            return 0;
        }
    }
    return 1;
}

/*
 * 记录页必须是全新的零页：内核不预填任何"答案"，
 * 记录里每个字段都只能由用户程序自己写出来。
 */
static int user_page_fresh(u64 va)
{
    const volatile u64 *p;

    if (va == 0ull) {
        return 0;
    }
    p = (const volatile u64 *)(uptr)va;
    return (p[0] == 0ull) && (p[1] == 0ull) && (p[2] == 0ull) && (p[3] == 0ull);
}

/* 按用户态入口桩的期望压四字段参数块，返回用户栈顶 */
static u64 ring3_push_args4(u64 stack_top, u64 target_port, u64 record_va,
                            u64 self_port, u64 arena_va)
{
    u64 sp = (stack_top - 0x40ull) & ~0xFull;
    ring3_args4_t *a = (ring3_args4_t *)(uptr)sp;

    a->target_port = target_port;
    a->record_va   = record_va;
    a->self_port   = self_port;
    a->arena_va    = arena_va;
    return sp;
}

static void selftest_libhyn(void)
{
    ring3_launch_t *ls = &g_lu_launch_shell;
    ring3_launch_t *la = &g_lu_launch_agent;
    hyn_abi_record_t *srec;
    hyn_abi_record_t *arec;
    hyn_net_probe_t np2;
    const hyn_group_info_t *ci;
    const hyn_group_info_t *ni;
    hyn_status_t st;
    u64 sz_shell, sz_agent, core_req, net_svc, deadline;
    u32 shell_pages, agent_pages, tmp_pages;
    u32 user0, denied0, badptr0, foreign0, noport0, spoof0, sent0, permit0, fault0, calls0;
    u32 served_core0, served_net0, served_core1, served_net1;
    u32 handled = 0u, len_actual = 0u, op = 0u, mid = 0u;
    u8  sdom = 0u;
    int ready, probe_ok;
    int woke_s = HYN_SCHED_TIMEOUT, woke_a = HYN_SCHED_TIMEOUT;

    kputs("\n---- libhyn 与 Ring3 示例：用户程序经 syscall + ALPC 与内核交互 ----\n");

    sz_shell = (u64)g_user_shell_img_size;
    sz_agent = (u64)g_user_agent_img_size;
    tmp_pages  = (u32)((sz_shell + HYN_MMU_PAGE_SIZE - 1ull) / HYN_MMU_PAGE_SIZE);
    shell_pages = tmp_pages;
    tmp_pages  = (u32)((sz_agent + HYN_MMU_PAGE_SIZE - 1ull) / HYN_MMU_PAGE_SIZE);
    agent_pages = tmp_pages;

    /* 每程序：代码页（镜像平铺）+ 2 页用户栈 + 1 页记录页 */
    g_lu_shell_code  = user_pages(shell_pages);
    g_lu_shell_stack = user_pages(2u);
    g_lu_shell_rec   = user_pages(1u);
    g_lu_agent_code  = user_pages(agent_pages);
    g_lu_agent_stack = user_pages(2u);
    g_lu_agent_rec   = user_pages(1u);

    srec = (hyn_abi_record_t *)(uptr)g_lu_shell_rec;
    arec = (hyn_abi_record_t *)(uptr)g_lu_agent_rec;

    /* 装载：镜像整段拷到代码页起始处（页本身是清零页，尾部自然留零） */
    if ((g_lu_shell_code != 0ull) && (sz_shell > 0ull)) {
        memcpy((void *)(uptr)g_lu_shell_code, g_user_shell_img, (unsigned long)sz_shell);
    }
    if ((g_lu_agent_code != 0ull) && (sz_agent > 0ull)) {
        memcpy((void *)(uptr)g_lu_agent_code, g_user_agent_img, (unsigned long)sz_agent);
    }

    check("libhyn 示例装载：两份用户程序镜像链入内核 .rodata 并整段写入动态用户页（页数为真实占用、记录页为全新零页）",
          (sz_shell > 0ull) && (sz_agent > 0ull) &&
          (shell_pages >= 1u) && (shell_pages <= HYN_USERPROG_CODE_PAGES_MAX) &&
          (agent_pages >= 1u) && (agent_pages <= HYN_USERPROG_CODE_PAGES_MAX) &&
          user_pages_ok(g_lu_shell_code, shell_pages) &&
          user_pages_ok(g_lu_agent_code, agent_pages) &&
          user_pages_ok(g_lu_shell_stack, 2u) && user_pages_ok(g_lu_shell_rec, 1u) &&
          user_pages_ok(g_lu_agent_stack, 2u) && user_pages_ok(g_lu_agent_rec, 1u) &&
          (user_page_fresh(g_lu_shell_rec) != 0) && (user_page_fresh(g_lu_agent_rec) != 0), "");

    core_req = hyn_domain_req_port(HYN_DOMAIN_CORE);
    net_svc  = hyn_domain_svc_port(HYN_DOMAIN_NET);

    st = hyn_alpc_port_create("core.lu.shell", HYN_DOMAIN_CORE, &g_lu_shell_port);
    if (st == HYN_OK) {
        st = hyn_alpc_port_create("core.lu.agent", HYN_DOMAIN_CORE, &g_lu_agent_port);
    }
    if ((st == HYN_OK) && ((core_req == 0ull) || (net_svc == 0ull))) {
        st = HYN_ENOENT;
    }
    /* 跨域必须双向各建一条许可：请求方向 agent->net.svc，回信方向 net.svc->agent */
    if (st == HYN_OK) {
        st = hyn_alpc_connect(g_lu_agent_port, net_svc);
    }
    if (st == HYN_OK) {
        st = hyn_alpc_connect(net_svc, g_lu_agent_port);
    }

    check("libhyn 示例端口与建链：两用户身份端口（底座域）创建成功；跨域 agent<->net.svc 双向许可就位（缺一条都发不出去）",
          (st == HYN_OK) && (g_lu_shell_port != 0ull) && (g_lu_agent_port != 0ull) &&
          (hyn_alpc_port_domain(g_lu_shell_port) == HYN_DOMAIN_CORE) &&
          (hyn_alpc_port_domain(g_lu_agent_port) == HYN_DOMAIN_CORE) &&
          (hyn_alpc_port_domain(core_req) == HYN_DOMAIN_CORE) &&
          (hyn_alpc_port_domain(net_svc) == HYN_DOMAIN_NET) &&
          (hyn_alpc_is_connected(g_lu_agent_port, net_svc) == 1) &&
          (hyn_alpc_is_connected(net_svc, g_lu_agent_port) == 1), "");

    ls->entry_rip = 0ull;
    ls->user_rsp  = 0ull;
    la->entry_rip = 0ull;
    la->user_rsp  = 0ull;
    if ((g_lu_shell_stack != 0ull) && (g_lu_agent_stack != 0ull) &&
        (g_lu_shell_code != 0ull) && (g_lu_agent_code != 0ull)) {
        ls->entry_rip = g_lu_shell_code;
        ls->user_rsp  = ring3_push_args4(g_lu_shell_stack + 2ull * HYN_MMU_PAGE_SIZE,
                                         core_req, g_lu_shell_rec, g_lu_shell_port, 0ull);
        la->entry_rip = g_lu_agent_code;
        la->user_rsp  = ring3_push_args4(g_lu_agent_stack + 2ull * HYN_MMU_PAGE_SIZE,
                                         net_svc, g_lu_agent_rec, g_lu_agent_port, 0ull);
    }
    ready = (st == HYN_OK) && (ls->entry_rip != 0ull) && (la->entry_rip != 0ull);

    user0    = hyn_syscall_user_count();
    denied0  = hyn_syscall_kernel_denied();
    badptr0  = hyn_syscall_badptr_count();
    foreign0 = hyn_syscall_foreign_denied();
    noport0  = hyn_syscall_noport_denied();
    spoof0   = hyn_syscall_spoof_fixed();
    sent0    = hyn_syscall_alpc_sent();
    permit0  = hyn_alpc_permit_denied_count();
    fault0   = hyn_user_fault_count();
    calls0   = hyn_domain_call_total();
    served_core0 = hyn_domain_info(HYN_DOMAIN_CORE)->msg_served;
    served_net0  = hyn_domain_info(HYN_DOMAIN_NET)->msg_served;

    hyn_sched_set_preempt(1);
    if (ready) {
        g_lu_shell_t = hyn_thread_create("lu.shell", ring3_user_body, ls,
                                         HYN_THREAD_PRIO_NORMAL, (u8)HYN_DOMAIN_CORE);
        g_lu_agent_t = hyn_thread_create("lu.agent", ring3_user_body, la,
                                         HYN_THREAD_PRIO_NORMAL, (u8)HYN_DOMAIN_CORE);
    }
    if ((g_lu_shell_t != NULL) && (g_lu_agent_t != NULL)) {
        (void)hyn_thread_bind_port(g_lu_shell_t, g_lu_shell_port);
        (void)hyn_thread_bind_port(g_lu_agent_t, g_lu_agent_port);
    }

    check("libhyn 示例线程：两个 Ring3 用户线程就绪，身份端口由内核绑定（用户程序自报的身份一律被纠正）",
          (g_lu_shell_t != NULL) && (g_lu_agent_t != NULL) &&
          (hyn_thread_port_id(g_lu_shell_t) == g_lu_shell_port) &&
          (hyn_thread_port_id(g_lu_agent_t) == g_lu_agent_port), "");

    /*
     * 推进：用户线程要等内核在安全点代服务才会被应答；主线程若死等线程退出就没人服务 core.req。
     * 所以"非阻塞代服务一条 + 1 节拍短睡"交替，直到两份记录页都写完 |done|。
     */
    if ((g_lu_shell_t != NULL) && (g_lu_agent_t != NULL)) {
        deadline = hyn_sched_ticks() + 2000ull;
        while ((((const volatile hyn_abi_record_t *)srec)->done != HYN_ABI_REC_DONE) ||
               (((const volatile hyn_abi_record_t *)arec)->done != HYN_ABI_REC_DONE)) {
            if (hyn_domain_serve_port(core_req, &op, &mid, &sdom) == HYN_OK) {
                handled++;
            }
            if (hyn_sched_ticks() >= deadline) {
                break;
            }
            (void)hyn_sched_block_timeout((void *)(uptr)core_req, 1ull);
        }
    }

    if (g_lu_shell_t != NULL) {
        woke_s = hyn_sched_wait_thread(g_lu_shell_t, 400ull);
    }
    if (g_lu_agent_t != NULL) {
        woke_a = hyn_sched_wait_thread(g_lu_agent_t, 400ull);
    }

    /* 内核侧独立复探一次网卡，作为 agent 读数的外部对照（同样走跨域消息，不是函数调用） */
    memset(&np2, 0, (unsigned long)sizeof(np2));
    st = hyn_domain_call(HYN_DOMAIN_CORE, HYN_DOMAIN_NET, HYN_GROUP_OP_NET_PROBE,
                         NULL, 0u, &np2, (u32)sizeof(np2), &len_actual);
    probe_ok = ((st == HYN_OK) && (len_actual == (u32)sizeof(np2))) ? 1 : 0;

    hyn_sched_set_preempt(0);

    ci = hyn_domain_info(HYN_DOMAIN_CORE);
    ni = hyn_domain_info(HYN_DOMAIN_NET);
    served_core1 = ci->msg_served;
    served_net1  = ni->msg_served;
    g_lu_served_core = handled;
    g_lu_net_growth  = (served_net1 > served_net0) ? (served_net1 - served_net0) : 0u;

    /* ① Shell（同域 core.req）：身份自证 + 两轮真实往返 */
    check("Shell 记录页：用户态自证 CPL=3、线程 id 相符、身份端口一致；PING 与分组 INFO 两轮往返均被应答，"
          "来源域=底座、序号原样回填、载荷长度与内核侧读数逐项一致",
          (srec != NULL) &&
          (srec->magic == HYN_ABI_REC_MAGIC) &&
          (srec->abi_version == (u64)HYN_ABI_VERSION) &&
          (srec->kind == (u64)HYN_ABI_KIND_SHELL) &&
          (srec->tid == hyn_thread_id(g_lu_shell_t)) &&
          (srec->cs == (u64)HYN_ABI_USER_CS) &&
          (srec->cpl_ret == (u64)HYN_ABI_OK) &&
          (srec->self_port == g_lu_shell_port) &&
          (srec->target_port == core_req) &&
          (srec->sys_ping_echo == (u64)HYN_ABI_PING_MAGIC) &&
          (srec->ping_ret == (u64)HYN_ABI_OK) &&
          (srec->ping_echo == (u64)HYN_ABI_PING_MAGIC) &&
          (srec->ping_domain == (u64)HYN_ABI_DOMAIN_CORE) &&
          (srec->ping_msg_id == 1ull) &&
          (srec->ping_state == (u64)HYN_ABI_DOMAIN_ONLINE) &&
          (srec->ping_epoch == (u64)ci->epoch) &&
          (srec->req2_ret == (u64)HYN_ABI_OK) &&
          (srec->req2_len == (u64)sizeof(hyn_abi_group_info_t)) &&
          (srec->req2_domain == (u64)HYN_ABI_DOMAIN_CORE) &&
          (srec->k0 == (u64)ci->devices_total) &&
          (srec->k1 == (u64)ci->devices_ready) &&
          (srec->k2 == (u64)ci->resources) &&
          (srec->k3 == (u64)ci->probe_count), "");

    /* ② Agent（跨域 net.svc）：跨域往返 + 网卡硬件事实 */
    check("Agent 记录页：跨域请求经 net.svc 被应答，来源域=网络组；NET_PROBE 取回的网卡事实"
          "（认领标志/VID:DID/校验和实测与期望）与内核独立复探逐项一致",
          probe_ok &&
          (arec != NULL) &&
          (arec->magic == HYN_ABI_REC_MAGIC) &&
          (arec->abi_version == (u64)HYN_ABI_VERSION) &&
          (arec->kind == (u64)HYN_ABI_KIND_AGENT) &&
          (arec->tid == hyn_thread_id(g_lu_agent_t)) &&
          (arec->cs == (u64)HYN_ABI_USER_CS) &&
          (arec->cpl_ret == (u64)HYN_ABI_OK) &&
          (arec->self_port == g_lu_agent_port) &&
          (arec->target_port == net_svc) &&
          (arec->sys_ping_echo == (u64)HYN_ABI_PING_MAGIC) &&
          (arec->ping_ret == (u64)HYN_ABI_OK) &&
          (arec->ping_echo == (u64)HYN_ABI_PING_MAGIC) &&
          (arec->ping_domain == (u64)HYN_ABI_DOMAIN_NET) &&
          (arec->ping_msg_id == 1ull) &&
          (arec->ping_state == (u64)HYN_ABI_DOMAIN_ONLINE) &&
          (arec->ping_epoch == (u64)ni->epoch) &&
          (arec->req2_ret == (u64)HYN_ABI_OK) &&
          (arec->req2_len == (u64)sizeof(hyn_abi_net_probe_t)) &&
          (arec->req2_domain == (u64)HYN_ABI_DOMAIN_NET) &&
          (arec->k0 == (u64)np2.nic_found) &&
          (arec->k1 == (((u64)np2.pci_vendor << 16) | (u64)np2.pci_device)) &&
          (arec->k2 == (u64)np2.cksum_expected) &&
          (arec->k3 == (u64)np2.cksum_result) &&
          /*
           * 不只比对数字一致——还核对数字本身站得住：造一个标准 IPv4 首部、真算校验和回填、
           * 再算一次必须为 0（"校验和是对的"的定义），故期望值非 0、结果必须为 0。
           * 这条挡的是"载荷整段是零也判过"的假通过。
           */
          (np2.cksum_expected != 0u) && (np2.cksum_result == 0u), "");

    /* ③ 负向对照：四类拒绝都必须如实回到用户态，且返回码就是内核语义 */
    check("用户侧负向对照如实反馈：Shell 空收超时=ETIMEDOUT、非法指针发送=EFAULT；"
          "Agent 收他人端口=EPERM、非法指针收取=EFAULT（拒绝理由不美化、不吞掉）",
          (srec->recv_to == (u64)HYN_ABI_ETIMEDOUT) &&
          (srec->badptr_ret == (u64)HYN_ABI_EFAULT) &&
          (arec->recv_to == (u64)HYN_ABI_EPERM) &&
          (arec->badptr_ret == (u64)HYN_ABI_EFAULT), "");

    /* ④ 全流程审计：用户态只经 syscall、内核只按语义计数，无一笔来源不明 */
    check("libhyn 全流程审计自洽：两程序各 10 次 syscall（用户调用 +20）；非法指针拦 2 次、越权收他人端口 1 次、"
          "身份回填 8 处、经 syscall 投递 4 条、跨域许可零拒绝、内核异常零接管；底座代服务 2 条；"
          "两线程均正常退出且退出码 0；底座服务计数 +2、网络组服务计数 +3（含内核复探 1 条）",
          (hyn_syscall_user_count() == user0 + 20u) &&
          (hyn_syscall_kernel_denied() == denied0) &&
          (hyn_syscall_badptr_count() == badptr0 + 2u) &&
          (hyn_syscall_foreign_denied() == foreign0 + 1u) &&
          (hyn_syscall_noport_denied() == noport0) &&
          (hyn_syscall_spoof_fixed() == spoof0 + 8u) &&
          (hyn_syscall_alpc_sent() == sent0 + 4u) &&
          (hyn_alpc_permit_denied_count() == permit0) &&
          (hyn_user_fault_count() == fault0) &&
          (handled == 2u) &&
          (woke_s == HYN_SCHED_WOKE) && (woke_a == HYN_SCHED_WOKE) &&
          (hyn_thread_state(g_lu_shell_t) == HYN_THREAD_DEAD) &&
          (hyn_thread_state(g_lu_agent_t) == HYN_THREAD_DEAD) &&
          (hyn_thread_exit_code(g_lu_shell_t) == 0) &&
          (hyn_thread_exit_code(g_lu_agent_t) == 0) &&
          (served_core1 == served_core0 + 2u) &&
          (served_net1 == served_net0 + 3u) &&
          (hyn_domain_call_total() == calls0 + 1u), "");

    if (srec != NULL) {
        kprintf("  Ring3 记录页: shell tid=%u ping_ret=%u 回声=0x%08X 组读回(规模=%u/就绪=%u/资源=%u/探测=%u) 超时=%u 非法指针=%u 完成=0x%X\n",
                (u32)srec->tid, (u32)srec->ping_ret, (u32)srec->ping_echo,
                (u32)srec->k0, (u32)srec->k1, (u32)srec->k2, (u32)srec->k3,
                (u32)srec->recv_to, (u32)srec->badptr_ret, (u32)srec->done);
    }
    if (arec != NULL) {
        kprintf("  Ring3 记录页: agent tid=%u 来源域=%u 网卡(认领=%u VID:DID=%04X:%04X 校验和=%u期望%u) 被拒=%u 非法指针=%u 完成=0x%X\n",
                (u32)arec->tid, (u32)arec->ping_domain,
                (u32)arec->k0, (u32)(arec->k1 >> 16), (u32)(arec->k1 & 0xFFFFull),
                (u32)arec->k3, (u32)arec->k2,
                (u32)arec->recv_to, (u32)arec->badptr_ret, (u32)arec->done);
    }
    kprintf("  libhyn 审计: 用户 syscall %u 次 / 非法指针挡下 %u 次 / 越权收他人端口 %u 次 / "
            "身份回填 %u 处 / 经 syscall 投递 %u 条 / 跨域许可被拒 %u 次\n",
            hyn_syscall_user_count(), hyn_syscall_badptr_count(),
            hyn_syscall_foreign_denied(), hyn_syscall_spoof_fixed(),
            hyn_syscall_alpc_sent(), hyn_alpc_permit_denied_count());
}

static void report_modules(void)
{
    kputs("\n[内核] 硬核底座模块状态\n");
    kputs("      · 中断/异常框架(GDT/IDT/PIC/PIT) : 已实装\n");
    kprintf("      · PML4 四级页表虚拟内存         : 已实装（位图物理页分配 + 4K 映射/取消映射 + 内核窗口 %u 处）\n",
            vmm_mapping_count());
    kputs("      · 内核对象管理器                : 已实装（命名对象/句柄代次/类型清理回调；对象池为静态池）\n");
    kprintf("      · ALPC 端口消息总线             : 已实装（%u 端口存活 / 队列深度 %u；跨域强制走消息；支持阻塞收取）\n",
            hyn_alpc_port_count(), HYN_ALPC_QUEUE_CAP);
    kprintf("      · 进程/线程调度与 syscall 入口  : 已实装（静态池 %u 线程 / 切换 %u 次 / 抢占 %u 次；用户线程经 syscall 调用 %u 次）\n",
            hyn_sched_live_count(), hyn_sched_switch_count(), hyn_sched_preempt_count(),
            hyn_syscall_user_count());
    kputs("      · 分组故障域(硬核底座隔离)      : 已实装（三组各自探测/静默/重置；单组重置 epoch+1）\n");
    kprintf("        - 存储组 %s 纪元%u / 网络组 %s 纪元%u / 总线组 %s 纪元%u\n",
            hyn_domain_state_name((hyn_domain_state_t)dom_state(HYN_DOMAIN_STORAGE)),
            hyn_domain_info(HYN_DOMAIN_STORAGE)->epoch,
            hyn_domain_state_name((hyn_domain_state_t)dom_state(HYN_DOMAIN_NET)),
            hyn_domain_info(HYN_DOMAIN_NET)->epoch,
            hyn_domain_state_name((hyn_domain_state_t)dom_state(HYN_DOMAIN_BUS)),
            hyn_domain_info(HYN_DOMAIN_BUS)->epoch);
    kprintf("        - 跨域消息往返 %u 次 / 被拒 %u 次 / 跨域直调拦下 %u 次 / 应答丢弃 %u 条 / 异常接管 %u 次\n",
            hyn_domain_call_total(), hyn_domain_call_denied(),
            hyn_domain_blocked_count(), hyn_domain_msg_discarded(),
            hyn_domain_fault_total());
    kprintf("      · libhyn 用户态库与 Ring3 示例   : 已实装（shell %u 字节 / agent %u 字节平铺镜像链入内核、运行期装载到动态用户页；"
            "两程序各 10 次 syscall 全走 int $0x80；底座安全点替 core.req 应答 %u 条；跨域 agent<->net.svc 往返使网络组服务计数 +%u）\n",
            (u32)g_user_shell_img_size, (u32)g_user_agent_img_size,
            g_lu_served_core, g_lu_net_growth);
}

/* ---- 时钟节拍：先喂 HAL，再喂调度器（PIT 单回调，故在此合流） ---- */
static void kernel_tick(void)
{
    hal_timer_tick();
    hyn_sched_tick();
}

void kernel_main(boot_info_t *bi)
{
    hyn_status_t st;

    clear_bss();

    if (bi != NULL && bi->magic == HYN_BOOTINFO_MAGIC) {
        g_boot = *bi;
        g_boot_valid = 1;
    } else {
        g_boot.magic = HYN_BOOTINFO_MAGIC;
        g_boot.total_mem_bytes = 128u * 1024u * 1024u;
        g_boot.kernel_bytes = 0;
        g_boot.kernel_sectors = 0;
        g_boot_valid = 0;
    }

    arch_irq_disable();
    (void)hal_console_init();
    kprintf_add_sink(hal_console_sink, NULL);

    print_banner();
    report_boot();

    st = hal_init();
    if (st != HYN_OK) {
        kpanic_status("HAL 初始化", st);
    }
    report_devices();

    kputs("\n---- 硬核底座初始化：分段与中断 ----\n");
    hyn_gdt_init();

    st = hyn_trap_init();
    hyn_timer_attach_tick(kernel_tick);
    check("GDT/IDT 与中断框架初始化", st == HYN_OK, "");

    selftest_traps();
    selftest_interrupts();

    kputs("\n---- 硬核底座初始化：内存、对象、消息总线与调度器 ----\n");

    st = pmm_init(g_boot.total_mem_bytes);
    check("物理页分配器初始化（位图 @ 0x200000，受管区 @ 0x400000 起）", st == HYN_OK, "");
    if (st != HYN_OK) {
        kpanic_status("物理页分配器初始化", st);
    }

    st = vmm_init();
    check("PML4 虚拟内存管理器初始化（新建页表并切换 CR3）", st == HYN_OK, "");
    if (st != HYN_OK) {
        kpanic_status("虚拟内存管理器初始化", st);
    }

    st = hyn_obj_init();
    check("内核对象管理器初始化", st == HYN_OK, "");
    if (st != HYN_OK) {
        kpanic_status("对象管理器初始化", st);
    }

    st = hyn_alpc_init();
    check("ALPC 端口消息总线初始化", st == HYN_OK, "");
    if (st != HYN_OK) {
        kpanic_status("ALPC 初始化", st);
    }

    /*
     * 调度器必须在任何线程被创建之前就位：它把"正在跑这段代码的执行流"登记成
     * 内核主线程，并造出空闲线程——此后 yield/阻塞/抢占才有"可切的对象"。
     */
    st = hyn_sched_init();
    check("进程/线程调度器初始化（主线程在册 + 空闲线程就绪）", st == HYN_OK, "");
    if (st != HYN_OK) {
        kpanic_status("调度器初始化", st);
    }

    selftest_memory();
    selftest_objects();
    selftest_alpc();
    selftest_scheduler();
    selftest_syscall();
    selftest_domains();
    selftest_libhyn();

    report_modules();

    kprintf("\n[自检] 结果：%u/%u 项通过\n", g_checks_passed, g_checks_total);
    if (g_checks_passed == g_checks_total) {
        kputs("[自检] 全部通过：中断/异常、PML4 虚拟内存、对象管理器、ALPC 消息总线、进程/线程调度与 syscall 唯一入口均正常。\n");
    } else {
        kputs("[自检] 存在未通过项，详见上方日志。\n");
    }

    /* 自检退出：QEMU 以 isa-debug-exit 设备接收退出码，便于自动化取证 */
    arch_qemu_exit((g_checks_passed == g_checks_total) ? 0x10u : 0x11u);
    arch_halt_forever();
}