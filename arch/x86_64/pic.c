/*
 * 混元 OS —— 8259A 中断控制器实装（x86_64）
 */
#include <hyn/pic.h>
#include <hyn/arch.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

#define ICW1_INIT  0x11     /* 初始化 + 需要 ICW4 */
#define ICW4_8086  0x01

static void pic_io_wait(void)
{
    hyn_outb(0x80, 0);
}

void hyn_pic_init(void)
{
    /* 主片 */
    hyn_outb(PIC1_CMD, ICW1_INIT);      pic_io_wait();
    hyn_outb(PIC1_DATA, 0x20);          pic_io_wait();   /* 向量基址 32 */
    hyn_outb(PIC1_DATA, 0x04);          pic_io_wait();   /* 从片挂在 IRQ2 */
    hyn_outb(PIC1_DATA, ICW4_8086);     pic_io_wait();
    /* 从片 */
    hyn_outb(PIC2_CMD, ICW1_INIT);      pic_io_wait();
    hyn_outb(PIC2_DATA, 0x28);          pic_io_wait();   /* 向量基址 40 */
    hyn_outb(PIC2_DATA, 0x02);          pic_io_wait();   /* 级联标识 */
    hyn_outb(PIC2_DATA, ICW4_8086);     pic_io_wait();

    /* 初始全部屏蔽，由各子系统按需放开 */
    hyn_outb(PIC1_DATA, 0xFF);
    hyn_outb(PIC2_DATA, 0xFF);
}

void hyn_pic_mask(u32 irq)
{
    if (irq < 8) {
        hyn_outb(PIC1_DATA, (u8)(hyn_inb(PIC1_DATA) | (1u << irq)));
    } else if (irq < HYN_PIC_IRQ_COUNT) {
        hyn_outb(PIC2_DATA, (u8)(hyn_inb(PIC2_DATA) | (1u << (irq - 8))));
    }
}

void hyn_pic_unmask(u32 irq)
{
    if (irq < 8) {
        hyn_outb(PIC1_DATA, (u8)(hyn_inb(PIC1_DATA) & (u8)~(1u << irq)));
    } else if (irq < HYN_PIC_IRQ_COUNT) {
        hyn_outb(PIC2_DATA, (u8)(hyn_inb(PIC2_DATA) & (u8)~(1u << (irq - 8))));
        hyn_outb(PIC1_DATA, (u8)(hyn_inb(PIC1_DATA) & (u8)~(1u << 2)));   /* 放开级联 */
    }
}

void hyn_pic_eoi(u32 irq)
{
    if (irq >= 8) {
        hyn_outb(PIC2_CMD, PIC_EOI);
    }
    hyn_outb(PIC1_CMD, PIC_EOI);
}