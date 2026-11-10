/* Tests of the mock HAL itself. */
#include "test.h"
#include "hal_mock.h"
#include "mt792x/mt792x.h"

#include <string.h>

TEST(version_is_set)
{
    CHECK(strcmp(mt792x_version(), "0.0.1") == 0);
}

TEST(mock_register_read_write)
{
    struct mock m;
    struct mt_hal *h;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);

    CHECK_EQ_U32(h->rr(h->ctx, 0x1000), 0);          /* unknown register reads 0 */
    h->wr(h->ctx, 0x1000, 0xDEADBEEFu);
    CHECK_EQ_U32(h->rr(h->ctx, 0x1000), 0xDEADBEEFu);
    CHECK_EQ_U32(mock_get_reg(&m, 0x1000), 0xDEADBEEFu);

    mock_set_reg(&m, 0x2000, 0x12345678u);
    CHECK_EQ_U32(h->rr(h->ctx, 0x2000), 0x12345678u);

    mock_set_default(&m, 0xFFFFFFFFu);
    CHECK_EQ_U32(h->rr(h->ctx, 0x3000), 0xFFFFFFFFu);  /* default for untouched */
    CHECK_EQ_U32(h->rr(h->ctx, 0x1000), 0xDEADBEEFu);  /* written value kept */

    CHECK_EQ_U32(m.bad_access, 0);
    mock_free(&m);
}

TEST(mock_bounds_flag)
{
    struct mock m;
    struct mt_hal *h;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);

    CHECK_EQ_U32(m.bad_access, 0);
    (void)h->rr(h->ctx, MOCK_BAR_SIZE);               /* one past the end */
    CHECK_EQ_U32(m.bad_access, 1);
    CHECK_EQ_U32(m.bad_offset, MOCK_BAR_SIZE);
    h->wr(h->ctx, MOCK_BAR_SIZE + 4u, 1);
    CHECK_EQ_U32(m.bad_access, 2);
    (void)h->rr(h->ctx, 0x1002);                      /* unaligned */
    CHECK_EQ_U32(m.bad_access, 3);
    (void)h->rr(h->ctx, MOCK_BAR_SIZE - 4u);          /* last valid register */
    CHECK_EQ_U32(m.bad_access, 3);

    mock_free(&m);
}

static unsigned g_write_hook_calls;

static void write_hook(void *user, uint32_t offset, uint32_t value)
{
    struct mock *m = (struct mock *)user;
    g_write_hook_calls++;
    /* "after writing 0xA5 to register 0x100, register 0x200 reads 0x77" */
    if (offset == 0x100u && value == 0xA5u) mock_set_reg(m, 0x200u, 0x77u);
}

static void read_hook(void *user, uint32_t offset, uint32_t *value)
{
    (void)user;
    if (offset == 0x300u) *value = 0xC0FFEEu;
}

TEST(mock_hooks)
{
    struct mock m;
    struct mt_hal *h;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);
    g_write_hook_calls = 0;

    m.on_write = write_hook;
    m.on_read = read_hook;
    m.hook_user = &m;

    CHECK_EQ_U32(h->rr(h->ctx, 0x200), 0);
    h->wr(h->ctx, 0x100, 0x11);                       /* wrong value: no effect */
    CHECK_EQ_U32(h->rr(h->ctx, 0x200), 0);
    h->wr(h->ctx, 0x100, 0xA5);
    CHECK_EQ_U32(h->rr(h->ctx, 0x200), 0x77);
    CHECK_EQ_U32(g_write_hook_calls, 2);
    CHECK_EQ_U32(h->rr(h->ctx, 0x300), 0xC0FFEEu);    /* read hook overrides */
    CHECK_EQ_U32(mock_get_reg(&m, 0x300), 0);         /* ... but not the storage */

    mock_free(&m);
}

TEST(mock_trace)
{
    struct mock m;
    struct mt_hal *h;
    int idx;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);

    h->wr(h->ctx, 0x10, 1);
    (void)h->rr(h->ctx, 0x10);
    h->wr(h->ctx, 0x20, 2);
    h->wr(h->ctx, 0x10, 3);

    CHECK_EQ_U32(m.trace_len, 4);
    CHECK_EQ_U32(mock_count_writes(&m, 0x10), 2);
    CHECK_EQ_U32(mock_count_reads(&m, 0x10), 1);
    CHECK_EQ_U32(mock_count_writes(&m, 0x30), 0);

    idx = mock_find_write(&m, 0x10, 0);
    CHECK(idx == 0);
    idx = mock_find_write(&m, 0x10, 1);
    CHECK(idx == 3);
    CHECK_EQ_U32(m.trace[3].value, 3);
    CHECK(mock_find_write(&m, 0x10, 4) == -1);
    CHECK(mock_find_write(&m, 0x99, 0) == -1);

    /* mock_set_reg must not show up in the trace */
    mock_set_reg(&m, 0x40, 5);
    CHECK_EQ_U32(m.trace_len, 4);

    mock_trace_clear(&m);
    CHECK_EQ_U32(m.trace_len, 0);
    CHECK_EQ_U32(mock_count_writes(&m, 0x10), 0);
    CHECK_EQ_U32(mock_get_reg(&m, 0x10), 3);          /* registers survive */

    mock_free(&m);
}

TEST(mock_trace_capacity)
{
    struct mock m;
    struct mt_hal *h;
    uint32_t i;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);

    for (i = 0; i < MOCK_TRACE_CAP; i++) h->wr(h->ctx, 0x10, i);
    CHECK_EQ_U32(m.trace_len, MOCK_TRACE_CAP);
    CHECK(!m.trace_overflow);
    h->wr(h->ctx, 0x10, 0);
    CHECK(m.trace_overflow);
    CHECK_EQ_U32(m.trace_len, MOCK_TRACE_CAP);

    mock_free(&m);
}

TEST(mock_virtual_clock)
{
    struct mock m;
    struct mt_hal *h;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);

    CHECK(h->now_us(h->ctx) == 0);
    h->sleep_us(h->ctx, 150);
    CHECK(h->now_us(h->ctx) == 150);
    mock_advance(&m, 1000000);
    CHECK(h->now_us(h->ctx) == 1000150);
    CHECK(m.slept_us == 150);                         /* advance is not sleep */

    mock_free(&m);
}

TEST(mock_log_capture)
{
    struct mock m;
    struct mt_hal *h;
    char buf[32];
    unsigned i;
    CHECK(mock_init(&m, 0) == 0);
    h = mock_hal(&m);

    CHECK(mock_log_get(&m, 0) == NULL);
    h->log(h->ctx, MT_LOG_INFO, "first");
    h->log(h->ctx, MT_LOG_WARN, "second");
    CHECK(strcmp(mock_log_get(&m, 0), "second") == 0);
    CHECK(strcmp(mock_log_get(&m, 1), "first") == 0);
    CHECK(mock_log_get(&m, 2) == NULL);
    CHECK_EQ_U32(m.log_total, 2);

    for (i = 0; i < 40; i++) {
        buf[0] = 'm'; buf[1] = (char)('A' + (i % 26)); buf[2] = '\0';
        h->log(h->ctx, MT_LOG_DEBUG, buf);
    }
    CHECK_EQ_U32(m.log_total, 42);
    CHECK(strcmp(mock_log_get(&m, 0), "mN") == 0);    /* i = 39 -> 'A' + 13 */
    CHECK(mock_log_get(&m, MOCK_LOG_KEEP - 1u) != NULL);
    CHECK(mock_log_get(&m, MOCK_LOG_KEEP) == NULL);

    mock_free(&m);
}

TEST(mock_dma_region)
{
    struct mock m;
    struct mt_hal *h;
    uint8_t zeros[8192];
    CHECK(mock_init(&m, 8192) == 0);
    h = mock_hal(&m);

    CHECK(h->dma_virt != NULL);
    CHECK_EQ_U32(h->dma_bus, 0x10000000u);
    CHECK_EQ_U32(h->dma_len, 8192);
    CHECK((((uintptr_t)h->dma_virt) & 4095u) == 0);   /* 4096-aligned */

    memset(zeros, 0, sizeof(zeros));
    CHECK_MEM_EQ(h->dma_virt, zeros, 8192);           /* zeroed, full length usable */
    memset(h->dma_virt, 0xAB, 8192);                  /* must be writable end to end */
    CHECK_EQ_U32(h->dma_virt[8191], 0xAB);

    mock_free(&m);

    CHECK(mock_init(&m, 0) == 0);                     /* no DMA region requested */
    CHECK(mock_hal(&m)->dma_virt == NULL);
    CHECK_EQ_U32(mock_hal(&m)->dma_len, 0);
    mock_free(&m);
}
