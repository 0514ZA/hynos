/*
 * HynOS —— PIT(8253/8254) 时钟实装（x86_64）
 *
 * 通道 0 工作在模式 3（方波），输出接到 8259 的 IRQ0。
 */
#include <hyn/timer.h>
#include <hyn/arch.h>

#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43
#define PIT_BASE_HZ  1193182u

static u32          g_hz;
static u64          g_ticks;
static void       (*g_tick_cb)(void);

hyn_status_t hyn_timer_init(u32 hz)
{
    u32 divisor;

    if (hz == 0 || hz > 1000u) {
        return HYN_EINVAL;
    }
    divisor = PIT_BASE_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    g_hz = PIT_BASE_HZ / divisor;
    g_ticks = 0;

    hyn_outb(PIT_CMD, 0x36);                                 /* 通道0 | 读写低高字节 | 模式3 | 二进制 */
    hyn_outb(PIT_CH0_DATA, (u8)(divisor & 0xFFu));
    hyn_outb(PIT_CH0_DATA, (u8)((divisor >> 8) & 0xFFu));
    return HYN_OK;
}

u32 hyn_timer_hz(void) { return g_hz; }
u64 hyn_timer_ticks(void) { return g_ticks; }

void hyn_timer_attach_tick(void (*cb)(void))
{
    g_tick_cb = cb;
}

void hyn_timer_tick(void)
{
    g_ticks++;
    if (g_tick_cb != NULL) {
        g_tick_cb();
    }
}