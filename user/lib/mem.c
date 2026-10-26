/*
 * HynOS —— 用户态极小内存原语（兜底）
 *
 * 用户程序以 -ffreestanding -fno-builtin 编译，正常不会产生对这些符号的调用；
 * 这里仍然实现一份，是为了"万一编译器把某个循环/结构体拷贝识别成 memcpy 调用"时，
 * 链接阶段不会因为缺符号而失败——宁可多一份 20 行兜底，也不让构建在语义之外断掉。
 *
 * 用 --gc-sections 链接：没有人引用时它们不会进入最终镜像。
 */
typedef unsigned long hyn_size_t;

void *memcpy(void *dst, const void *src, hyn_size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    hyn_size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, hyn_size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    hyn_size_t i;

    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (i = n; i > 0u; i--) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

void *memset(void *dst, int c, hyn_size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    hyn_size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}