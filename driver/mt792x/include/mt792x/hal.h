/*
 * mt792x/hal.h - hardware abstraction layer for the portable MT7921 logic.
 *
 * The library in src/ never touches the OS or the hardware directly. Whoever
 * embeds it (a UMDF proxy, a KMDF driver, a Linux test rig, a unit test with a
 * mock) fills in a struct mt_hal and passes it in. Keeping this boundary thin
 * is what lets the chip logic be unit-tested on any host.
 */
#ifndef MT792X_HAL_H
#define MT792X_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Log severity, lower value = more severe. */
enum mt_log_level { MT_LOG_ERROR = 0, MT_LOG_WARN = 1, MT_LOG_INFO = 2, MT_LOG_DEBUG = 3 };

struct mt_hal {
    void *ctx;                                                    /* passed back to every callback */
    uint32_t (*rr)(void *ctx, uint32_t bar_offset);               /* read a 32-bit register at a BAR0 offset */
    void (*wr)(void *ctx, uint32_t bar_offset, uint32_t value);   /* write a 32-bit register at a BAR0 offset */
    void (*sleep_us)(void *ctx, uint32_t microseconds);
    uint64_t (*now_us)(void *ctx);                                /* monotonic clock */
    void (*log)(void *ctx, enum mt_log_level level, const char *message);
    uint8_t *dma_virt;                                            /* one DMA-capable region: CPU address ... */
    uint32_t dma_bus;                                             /* ... its 32-bit bus address ... */
    uint32_t dma_len;                                             /* ... and its length in bytes */
    /* Optional. NULL: the region is physically contiguous and starts at dma_bus.
     * Non-NULL: the region is made of scattered 4 KiB pages; entry i is the bus address of the page
     * at CPU offset i * 4096 (dma_len / 4096 entries) and dma_bus is ignored. The library then never
     * hands the chip an object that crosses a page boundary. */
    const uint32_t *dma_page_bus;
};

#ifdef __cplusplus
}
#endif

#endif /* MT792X_HAL_H */
