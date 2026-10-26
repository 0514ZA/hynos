/*
 * HynOS —— COM1(16550) 串口后端
 *
 * 只做最简初始化与逐字符输出，供串口日志与自检结果抓取使用。
 * 该后端不是故障域设备，而是内核日志通道（内核底座自身可依赖）。
 */
#include <hyn/hal.h>
#include <hyn/arch.h>

#define COM1_PORT 0x3F8u

#define UART_DATA        0
#define UART_IER         1
#define UART_FCR         2
#define UART_LCR         3
#define UART_MCR         4
#define UART_LSR         5
#define UART_DLL         0
#define UART_DLM         1

#define UART_LSR_THR_EMPTY 0x20u

static int g_serial_ready;

hyn_status_t hal_serial_init(void)
{
    hyn_outb(COM1_PORT + UART_IER, 0x00);      /* 关闭串口中断，日志走轮询 */
    hyn_outb(COM1_PORT + UART_LCR, 0x80);      /* 打开 DLAB */
    hyn_outb(COM1_PORT + UART_DLL, 0x01);      /* 115200 / 1 = 115200 */
    hyn_outb(COM1_PORT + UART_DLM, 0x00);
    hyn_outb(COM1_PORT + UART_LCR, 0x03);      /* 8 位数据、无校验、1 位停止 */
    hyn_outb(COM1_PORT + UART_FCR, 0xC7);      /* 使能并清空 FIFO */
    hyn_outb(COM1_PORT + UART_MCR, 0x0B);      /* DTR/RTS/OUT2 */
    g_serial_ready = 1;
    return HYN_OK;
}

int hal_serial_ready(void)
{
    return g_serial_ready;
}

static void serial_raw_putc(char c)
{
    u32 guard = 1000000u;
    while (((hyn_inb(COM1_PORT + UART_LSR) & UART_LSR_THR_EMPTY) == 0) && guard--) {
        /* 等待发送保持寄存器就绪 */
    }
    hyn_outb(COM1_PORT + UART_DATA, (u8)c);
}

void hal_serial_putc(char c)
{
    if (c == '\n') {
        serial_raw_putc('\r');
    }
    serial_raw_putc(c);
}