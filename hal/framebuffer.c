/*
 * 混元 OS —— Framebuffer HAL 设备（VGA 文本模式）
 *
 * 说明：BIOS 交出的 VGA 文本模式即一块真实的显示后端，直接作为 HAL 的
 * Framebuffer 设备实例实装（图形帧缓冲行列在后续图形驱动接入时替换内部实现，
 * 对上接口保持不变）。同时它也是内核日志的显示侧通道。
 */
#include <hyn/hal.h>
#include <hyn/kstring.h>

#define VGA_TEXT_BASE   0xB8000u
#define VGA_COLS        80u
#define VGA_ROWS        25u
#define VGA_ATTR        0x07u   /* 黑底浅灰字 */

#define CRTC_INDEX  0x3D4u
#define CRTC_DATA   0x3D5u

extern void hyn_outb(u16 port, u8 value);
extern u8   hyn_inb(u16 port);

static volatile u8 *const g_vga = (volatile u8 *)VGA_TEXT_BASE;
static u32 g_row;
static u32 g_col;
static int g_fb_ready;

static void vga_set_cursor(void)
{
    u16 pos = (u16)(g_row * VGA_COLS + g_col);
    hyn_outb(CRTC_INDEX, 0x0F);
    hyn_outb(CRTC_DATA, (u8)(pos & 0xFF));
    hyn_outb(CRTC_INDEX, 0x0E);
    hyn_outb(CRTC_DATA, (u8)((pos >> 8) & 0xFF));
}

static void vga_scroll(void)
{
    for (u32 r = 1; r < VGA_ROWS; r++) {
        u8 *dst = (u8 *)(g_vga + (r - 1) * VGA_COLS * 2);
        const u8 *src = (const u8 *)(g_vga + r * VGA_COLS * 2);
        memcpy(dst, src, VGA_COLS * 2);
    }
    for (u32 c = 0; c < VGA_COLS; c++) {
        g_vga[((VGA_ROWS - 1) * VGA_COLS + c) * 2 + 0] = ' ';
        g_vga[((VGA_ROWS - 1) * VGA_COLS + c) * 2 + 1] = VGA_ATTR;
    }
    g_row = VGA_ROWS - 1;
    g_col = 0;
}

void hal_fb_clear(void)
{
    for (u32 i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        g_vga[i * 2 + 0] = ' ';
        g_vga[i * 2 + 1] = VGA_ATTR;
    }
    g_row = 0;
    g_col = 0;
    if (g_fb_ready) {
        vga_set_cursor();
    }
}

void hal_fb_putc(char c)
{
    if (!g_fb_ready) {
        return;
    }
    if (c == '\n') {
        g_col = 0;
        g_row++;
    } else if (c == '\r') {
        g_col = 0;
    } else if (c == '\t') {
        g_col = (g_col + 4) & ~3u;
        if (g_col >= VGA_COLS) {
            g_col = 0;
            g_row++;
        }
    } else {
        if (g_col >= VGA_COLS) {
            g_col = 0;
            g_row++;
        }
        g_vga[(g_row * VGA_COLS + g_col) * 2 + 0] = (u8)c;
        g_vga[(g_row * VGA_COLS + g_col) * 2 + 1] = VGA_ATTR;
        g_col++;
    }
    while (g_row >= VGA_ROWS) {
        vga_scroll();
    }
    vga_set_cursor();
}

hyn_status_t hal_fb_init(void)
{
    if (g_fb_ready) {
        return HYN_OK;      /* 重复初始化不清屏，避免擦掉已输出的日志 */
    }
    g_fb_ready = 1;
    hal_fb_clear();
    return HYN_OK;
}

u32 hal_fb_width(void)  { return VGA_COLS; }
u32 hal_fb_height(void) { return VGA_ROWS; }

/* ---------------- HAL 设备实例 ---------------- */

static hyn_status_t fb_probe(hal_device_t *dev)
{
    (void)dev;
    if (hal_fb_init() != HYN_OK) {
        return HYN_EIO;
    }
    dev->state = HAL_DEV_STATE_READY;
    return HYN_OK;
}

static hyn_status_t fb_reset(hal_device_t *dev)
{
    hal_fb_clear();
    dev->state = HAL_DEV_STATE_READY;
    return HYN_OK;
}

static hyn_status_t fb_shutdown(hal_device_t *dev)
{
    hal_fb_clear();
    dev->state = HAL_DEV_STATE_QUIESCED;
    return HYN_OK;
}

hal_device_t hal_dev_framebuffer = {
    .name     = "framebuffer.vga-text",
    .kind     = HAL_DEV_FRAMEBUFFER,
    .state    = HAL_DEV_STATE_EMPTY,
    .domain   = (u8)HYN_DOMAIN_CORE,     /* 底座设备：不可被故障域静默/重置 */
    .probe    = fb_probe,
    .reset    = fb_reset,
    .shutdown = fb_shutdown,
    .priv     = NULL,
    .next     = NULL,
};