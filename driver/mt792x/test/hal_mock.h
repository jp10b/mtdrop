/*
 * hal_mock.h - simulated MT7921 register space and environment for unit tests.
 *
 * Provides a struct mt_hal whose registers live in memory, with a virtual clock
 * (no real sleeping), an access trace, scriptable device behaviour through
 * hooks, captured log output and one zeroed, 4096-aligned "DMA" region.
 *
 * Test code only - this file may use OS/libc headers, src/ may not.
 */
#ifndef MT792X_TEST_HAL_MOCK_H
#define MT792X_TEST_HAL_MOCK_H

#include <stddef.h>
#include <stdint.h>

#include "mt792x/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_BAR_SIZE   0x100000u   /* 1 MiB BAR0 window */
#define MOCK_TRACE_CAP  (1u << 20)  /* access trace capacity (entries) */
#define MOCK_LOG_KEEP   16u         /* number of log messages remembered */
#define MOCK_LOG_LEN    256u        /* max stored length of one message */
#define MOCK_DMA_BUS    0x10000000u /* fake bus address of the DMA region */

struct mock_access {
    int is_write;       /* 1 = write, 0 = read */
    uint32_t offset;
    uint32_t value;     /* value written, or value returned to the reader */
};

struct mock {
    struct mt_hal hal;

    /* register space (allocated by mock_init) */
    uint32_t *regs;
    uint8_t *present;           /* 1 if the register was ever written/set */
    uint32_t default_value;     /* what never-written registers read as */

    /* bounds checking: any access outside BAR0 or not 4-byte aligned */
    unsigned bad_access;        /* number of such accesses */
    uint32_t bad_offset;        /* offset of the most recent one */

    /* optional device-behaviour hooks, called with hook_user */
    void (*on_write)(void *user, uint32_t offset, uint32_t value);   /* after the store */
    void (*on_read)(void *user, uint32_t offset, uint32_t *value);   /* may override the value */
    void *hook_user;

    /* access trace */
    struct mock_access *trace;
    uint32_t trace_len;
    int trace_overflow;         /* set if more than MOCK_TRACE_CAP accesses happened */

    /* virtual clock */
    uint64_t clock_us;
    uint64_t slept_us;          /* total time spent in sleep_us */

    /* log capture (ring buffer) */
    char log_text[MOCK_LOG_KEEP][MOCK_LOG_LEN];
    enum mt_log_level log_level[MOCK_LOG_KEEP];
    unsigned log_total;         /* total messages logged (not capped) */
    int verbose;                /* print messages to stderr (MT_TEST_VERBOSE) */
    const char *log_watch;      /* if set: count messages containing this substring */
    unsigned log_watch_hits;
    unsigned log_errors;        /* messages logged at MT_LOG_ERROR */

    /* DMA region */
    void *dma_raw;              /* raw allocation, for freeing */
};

/* Lifecycle. mock_init returns 0 on success, -1 on allocation failure. */
int mock_init(struct mock *m, uint32_t dma_len);
void mock_free(struct mock *m);
struct mt_hal *mock_hal(struct mock *m);

/* Register space, bypassing hooks, trace and bounds flag. */
void mock_set_reg(struct mock *m, uint32_t offset, uint32_t value);
uint32_t mock_get_reg(const struct mock *m, uint32_t offset);
void mock_set_default(struct mock *m, uint32_t value);

/* Trace helpers. Indices are positions in m->trace[]. */
void mock_trace_clear(struct mock *m);
unsigned mock_count_writes(const struct mock *m, uint32_t offset);
unsigned mock_count_reads(const struct mock *m, uint32_t offset);
/* Index of the first write to offset at or after 'from', or -1 if none. */
int mock_find_write(const struct mock *m, uint32_t offset, uint32_t from);

/* Virtual clock. */
void mock_advance(struct mock *m, uint64_t us);

/* Log capture. Index 0 = most recent message, up to MOCK_LOG_KEEP - 1. */
const char *mock_log_get(const struct mock *m, unsigned index_from_newest);

#ifdef __cplusplus
}
#endif

#endif /* MT792X_TEST_HAL_MOCK_H */
