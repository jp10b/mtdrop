#include "hal_mock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_WORDS (MOCK_BAR_SIZE / 4u)
#define DMA_ALIGN 4096u

static int in_range(struct mock *m, uint32_t offset)
{
    if (offset >= MOCK_BAR_SIZE || (offset & 3u) != 0) {
        m->bad_access++;
        m->bad_offset = offset;
        return 0;
    }
    return 1;
}

static void trace_add(struct mock *m, int is_write, uint32_t offset, uint32_t value)
{
    if (m->trace_len >= MOCK_TRACE_CAP) {
        m->trace_overflow = 1;
        return;
    }
    m->trace[m->trace_len].is_write = is_write;
    m->trace[m->trace_len].offset = offset;
    m->trace[m->trace_len].value = value;
    m->trace_len++;
}

static uint32_t hal_rr(void *ctx, uint32_t offset)
{
    struct mock *m = (struct mock *)ctx;
    uint32_t value;

    if (!in_range(m, offset)) return 0;
    value = m->present[offset / 4u] ? m->regs[offset / 4u] : m->default_value;
    if (m->on_read) m->on_read(m->hook_user, offset, &value);
    trace_add(m, 0, offset, value);
    return value;
}

static void hal_wr(void *ctx, uint32_t offset, uint32_t value)
{
    struct mock *m = (struct mock *)ctx;

    if (!in_range(m, offset)) return;
    m->regs[offset / 4u] = value;
    m->present[offset / 4u] = 1;
    trace_add(m, 1, offset, value);
    if (m->on_write) m->on_write(m->hook_user, offset, value);
}

static void hal_sleep_us(void *ctx, uint32_t us)
{
    struct mock *m = (struct mock *)ctx;
    m->clock_us += us;
    m->slept_us += us;
}

static uint64_t hal_now_us(void *ctx)
{
    return ((struct mock *)ctx)->clock_us;
}

static void hal_log(void *ctx, enum mt_log_level level, const char *message)
{
    struct mock *m = (struct mock *)ctx;
    unsigned slot = m->log_total % MOCK_LOG_KEEP;
    size_t n = message ? strlen(message) : 0;

    if (n >= MOCK_LOG_LEN) n = MOCK_LOG_LEN - 1;
    if (n) memcpy(m->log_text[slot], message, n);
    m->log_text[slot][n] = '\0';
    m->log_level[slot] = level;
    m->log_total++;
    if (m->verbose) {
        static const char *const names[] = { "ERROR", "WARN", "INFO", "DEBUG" };
        fprintf(stderr, "[mock %s] %s\n", names[(unsigned)level & 3u], m->log_text[slot]);
    }
}

int mock_init(struct mock *m, uint32_t dma_len)
{
    memset(m, 0, sizeof(*m));

    m->regs = (uint32_t *)calloc(MOCK_WORDS, sizeof(uint32_t));
    m->present = (uint8_t *)calloc(MOCK_WORDS, 1);
    m->trace = (struct mock_access *)calloc(MOCK_TRACE_CAP, sizeof(struct mock_access));
    if (!m->regs || !m->present || !m->trace) {
        mock_free(m);
        return -1;
    }

    if (dma_len) {
        /* Over-allocate and align manually: no OS-specific aligned_alloc needed. */
        size_t total = (size_t)dma_len + DMA_ALIGN;
        uintptr_t p;
        m->dma_raw = calloc(1, total);
        if (!m->dma_raw) {
            mock_free(m);
            return -1;
        }
        p = ((uintptr_t)m->dma_raw + (DMA_ALIGN - 1u)) & ~(uintptr_t)(DMA_ALIGN - 1u);
        m->hal.dma_virt = (uint8_t *)p;
        m->hal.dma_bus = MOCK_DMA_BUS;
        m->hal.dma_len = dma_len;
    }

    m->hal.ctx = m;
    m->hal.rr = hal_rr;
    m->hal.wr = hal_wr;
    m->hal.sleep_us = hal_sleep_us;
    m->hal.now_us = hal_now_us;
    m->hal.log = hal_log;

    m->verbose = getenv("MT_TEST_VERBOSE") != NULL;
    return 0;
}

void mock_free(struct mock *m)
{
    free(m->regs);
    free(m->present);
    free(m->trace);
    free(m->dma_raw);
    memset(m, 0, sizeof(*m));
}

struct mt_hal *mock_hal(struct mock *m)
{
    return &m->hal;
}

void mock_set_reg(struct mock *m, uint32_t offset, uint32_t value)
{
    if (!in_range(m, offset)) return;
    m->regs[offset / 4u] = value;
    m->present[offset / 4u] = 1;
}

uint32_t mock_get_reg(const struct mock *m, uint32_t offset)
{
    if (offset >= MOCK_BAR_SIZE || (offset & 3u) != 0) return 0;
    return m->present[offset / 4u] ? m->regs[offset / 4u] : m->default_value;
}

void mock_set_default(struct mock *m, uint32_t value)
{
    m->default_value = value;
}

void mock_trace_clear(struct mock *m)
{
    m->trace_len = 0;
    m->trace_overflow = 0;
}

static unsigned count_kind(const struct mock *m, int is_write, uint32_t offset)
{
    unsigned n = 0;
    uint32_t i;
    for (i = 0; i < m->trace_len; i++) {
        if (m->trace[i].is_write == is_write && m->trace[i].offset == offset) n++;
    }
    return n;
}

unsigned mock_count_writes(const struct mock *m, uint32_t offset)
{
    return count_kind(m, 1, offset);
}

unsigned mock_count_reads(const struct mock *m, uint32_t offset)
{
    return count_kind(m, 0, offset);
}

int mock_find_write(const struct mock *m, uint32_t offset, uint32_t from)
{
    uint32_t i;
    for (i = from; i < m->trace_len; i++) {
        if (m->trace[i].is_write && m->trace[i].offset == offset) return (int)i;
    }
    return -1;
}

void mock_advance(struct mock *m, uint64_t us)
{
    m->clock_us += us;
}

const char *mock_log_get(const struct mock *m, unsigned index_from_newest)
{
    unsigned stored = m->log_total < MOCK_LOG_KEEP ? m->log_total : MOCK_LOG_KEEP;
    if (index_from_newest >= stored) return NULL;
    return m->log_text[(m->log_total - 1u - index_from_newest) % MOCK_LOG_KEEP];
}
