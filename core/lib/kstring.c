/* 混元 OS —— 字符串/内存操作实现（纯 C，架构无关） */
#include <hyn/kstring.h>

void *memcpy(void *dst, const void *src, unsigned long n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    u8 *d = (u8 *)dst;
    while (n--) {
        *d++ = (u8)c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    while (n--) {
        if (*x != *y) {
            return (int)(*x) - (int)(*y);
        }
        x++;
        y++;
    }
    return 0;
}

unsigned long strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, unsigned long n)
{
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return r;
}

char *strncpy_safe(char *dst, const char *src, unsigned long cap)
{
    if (cap == 0) {
        return dst;
    }
    unsigned long i = 0;
    while ((i + 1) < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

u64 kstr_hash(const char *s)
{
    /* FNV-1a 64 位 */
    u64 h = 0xCBF29CE484222325ULL;
    while (*s) {
        h ^= (u64)(u8)*s++;
        h *= 0x100000001B3ULL;
    }
    return h;
}