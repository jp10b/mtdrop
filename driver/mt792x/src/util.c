/* SPDX-License-Identifier: GPL-3.0-or-later */
/* util.c - memory helpers, DMA memory access and a tiny freestanding log formatter. */
#include "internal.h"

void mt_memset(void *dst, int value, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    size_t i;
    for (i = 0; i < len; i++) d[i] = (uint8_t)value;
}

void mt_memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < len; i++) d[i] = s[i];
}

static int host_is_little_endian(void)
{
    const uint16_t one = 1;
    return *(const uint8_t *)&one == 1;
}

static uint32_t swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

/* Callers only pass offsets inside the layout (checked once in mt_layout_init) and 4-aligned. */
uint32_t mt_dma_ld32(const struct mt792x *dev, uint32_t offset)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(const void *)(dev->hal->dma_virt + offset);
    uint32_t v = *p;
    return host_is_little_endian() ? v : swap32(v);
}

void mt_dma_st32(struct mt792x *dev, uint32_t offset, uint32_t value)
{
    volatile uint32_t *p = (volatile uint32_t *)(void *)(dev->hal->dma_virt + offset);
    *p = host_is_little_endian() ? value : swap32(value);
}

void mt_dma_read(const struct mt792x *dev, uint32_t offset, uint8_t *dst, uint32_t len)
{
    const volatile uint8_t *s = dev->hal->dma_virt + offset;
    uint32_t i;
    for (i = 0; i < len; i++) dst[i] = s[i];
}

void mt_dma_write(struct mt792x *dev, uint32_t offset, const uint8_t *src, uint32_t len)
{
    volatile uint8_t *d = dev->hal->dma_virt + offset;
    uint32_t i;
    for (i = 0; i < len; i++) d[i] = src[i];
}

/* ---- formatter ---- */

struct fmt_out {
    char *buf;
    size_t size;
    size_t pos;
};

static void out_c(struct fmt_out *o, char c)
{
    if (o->size && o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void out_num(struct fmt_out *o, uint64_t v, unsigned base, int upper, int neg, unsigned width, int zero)
{
    char tmp[24];
    unsigned n = 0, len;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v && n < sizeof(tmp));
    len = n + (neg ? 1u : 0u);
    if (neg && zero) out_c(o, '-');
    while (len < width) {
        out_c(o, zero ? '0' : ' ');
        len++;
    }
    if (neg && !zero) out_c(o, '-');
    while (n) out_c(o, tmp[--n]);
}

int mt_vformat(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct fmt_out o;
    o.buf = buf;
    o.size = size;
    o.pos = 0;

    while (*fmt) {
        unsigned width = 0;
        int zero = 0, longlong = 0;
        char c = *fmt++;
        if (c != '%') {
            out_c(&o, c);
            continue;
        }
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10u + (unsigned)(*fmt++ - '0');
        if (fmt[0] == 'l' && fmt[1] == 'l') {
            longlong = 1;
            fmt += 2;
        }
        c = *fmt;
        if (c == '\0') break;
        fmt++;
        switch (c) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n = 0;
            if (!s) s = "(null)";
            while (s[n]) n++;
            while (n < width) {
                out_c(&o, ' ');
                width--;
            }
            while (*s) out_c(&o, *s++);
            break;
        }
        case 'c':
            out_c(&o, (char)va_arg(ap, int));
            break;
        case 'd': {
            int64_t v = longlong ? (int64_t)va_arg(ap, long long) : (int64_t)va_arg(ap, int);
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            out_num(&o, mag, 10, 0, v < 0, width, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longlong ? (uint64_t)va_arg(ap, unsigned long long) : (uint64_t)va_arg(ap, unsigned int);
            out_num(&o, v, c == 'u' ? 10u : 16u, c == 'X', 0, width, zero);
            break;
        }
        case '%':
            out_c(&o, '%');
            break;
        default:
            out_c(&o, '%');
            out_c(&o, c);
            break;
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = '\0';
    return (int)o.pos;
}

int mt_format(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = mt_vformat(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void mt_logf(const struct mt792x *dev, enum mt_log_level level, const char *fmt, ...)
{
    char line[320];
    va_list ap;
    if (!dev || !dev->hal || !dev->hal->log) return;
    va_start(ap, fmt);
    mt_vformat(line, sizeof(line), fmt, ap);
    va_end(ap);
    dev->hal->log(dev->hal->ctx, level, line);
}
