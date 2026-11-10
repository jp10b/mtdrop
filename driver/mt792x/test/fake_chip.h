/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fake_chip.h - a simulated MT7921 on top of the mock HAL, strict enough to catch
 * host mistakes. Behaviour follows docs/MT7921-BRINGUP.md (registers, rings, MCU
 * messages); every deviation by the host is recorded as a "violation".
 *
 * What it models:
 *  - LPCTL own/sync handshake, TOP_LPCR_HOST_BAND0 driver own (via L1)
 *  - the L1 remap window (BAR 0x40000-0x4ffff routed by MT_HIF_REMAP_L1 bits 15:0):
 *    chip id / bound / rev, WFSYS reset register with INIT_DONE after a delay
 *  - CONN_ON_MISC firmware state (1 after reset, 3 after FW_START)
 *  - GLO_CFG enable/busy bits, WFDMA logic reset, RST_DTX_PTR
 *  - ring registers of TX 0/16/17 and RX 0/2/4; consuming TX descriptors from ring memory by
 *    translating bus addresses back into the HAL region (violation if outside / misaligned /
 *    malformed); MCU command parsing per spec 4; patch semaphore and download state; events
 *    written into RX rings only if the host armed them correctly; DMA_DONE write-back and
 *    dma_idx like the real chip.
 */
#ifndef MT792X_TEST_FAKE_CHIP_H
#define MT792X_TEST_FAKE_CHIP_H

#include <stdint.h>

#include "hal_mock.h"

#define FAKE_MAX_DL        64u
#define FAKE_MAX_CMDS      512u
#define FAKE_MAX_ENABLING  65536u

struct fake_dl {
    int phase;          /* 0 = patch (semaphore held), 1 = RAM */
    uint8_t cmd;        /* 0x05 or 0x01 */
    uint32_t addr, len, mode;
    uint8_t *data;      /* received bytes (malloc'd, len bytes) */
    uint32_t got;
    uint32_t chunks;
};

struct fake_cmd {
    uint8_t cid, seq;
    uint32_t total_len;
    uint32_t ring;      /* 17 or 16 (16 = FW_SCATTER chunk) */
};

struct fake_chip {
    struct mock *m;

    /* ---- configuration / fault knobs (set after fake_init, before the run) ---- */
    uint32_t chip_id, hw_bound, hw_rev;
    uint32_t remap_initial;
    int fw_own_never;           /* SET_OWN never sets OWN_SYNC */
    unsigned drv_own_fail_count;/* the first N CLR_OWN requests are ignored */
    int drv_own_never;
    int wfsys_never_done;
    uint32_t wfsys_done_delay_us;
    int dma_busy_stuck;
    int drv_own2_never;
    int fw_state_never_ready;
    int sem_get_result;         /* -1 = natural behaviour */
    int sem_release_result;     /* -1 = natural behaviour */
    int patch_loaded;           /* semaphore answers PATCH_IS_DL */
    unsigned drop_response_nth; /* 1-based TX17 command index whose response is not sent; 0 = none */
    unsigned short_response_nth;/* 1-based TX17 command index answered with a 33-byte event */
    int finish_result;
    unsigned fw_start_ring;     /* RX ring for the FW_START response (0 or 4) */
    unsigned event_ring;        /* RX ring for all other responses */
    int rx_never_armed;         /* host RX cpu_idx writes have no effect */
    int n9_never;
    int tx_stall_ring;          /* -1 = none; kicks on this TX ring are never consumed */
    uint32_t bus_dead_after;    /* 0 = never; after this many register accesses the bus reads all ones */
    int inject_unsolicited;     /* before each response: DBG_MSG event with seq 0 */
    int inject_stale;           /* before each response: a response with another seq */
    int nic_power_responds;     /* answer NIC_POWER_CTRL although mt76 does not wait for it */

    /* ---- state ---- */
    uint32_t accesses;
    int dead;
    int dead_read_seen;
    unsigned writes_after_dead_read;
    int own_sync;
    int driver_owned_once;
    unsigned clr_own_requests;
    unsigned set_own_writes;
    int wfsys_asserted;
    uint64_t wfsys_assert_time, wfsys_release_time;
    int wfsys_released_once;
    unsigned wfsys_resets;
    int top_lpcr_fw_own;
    int nic_powered;
    int fw_started;
    uint32_t tx_dma[18];
    uint32_t rx_dma[8], rx_cpu[8];
    int sem_held, patch_done;
    uint32_t seq_counter;       /* emulates the host's msg_seq to check every sequence number */
    unsigned cmd_count;         /* TX17 commands */
    struct fake_cmd cmds[FAKE_MAX_CMDS];
    unsigned n_cmds;            /* TX17 commands and TX16 chunks, in order */
    struct fake_dl dl[FAKE_MAX_DL];
    unsigned n_dl;
    uint32_t fw_start_option, fw_start_addr;
    unsigned events_sent[8];
    unsigned events_dropped;
    unsigned responses_dropped_by_knob;
    uint32_t enabling_idx[FAKE_MAX_ENABLING]; /* trace indices of DMA-enabling writes */
    unsigned n_enabling;
    unsigned tx_kicks[18];
    uint32_t fault_idx;         /* trace index at which a knob fault was injected/observed; 0 = none */

    /* ---- violations ---- */
    unsigned violations;
    char first_violation[256];
};

/* Wires the fake into the mock (hooks + initial register values). Defaults model the
 * MEASURED start state: LPCTL 0, MISC 1, GLO_CFG 0x1010b870, remap 0x18451800. */
void fake_init(struct fake_chip *f, struct mock *m);
void fake_free(struct fake_chip *f);

/* Bytes the chip "has" for a load address after downloads (NULL if never downloaded). */
const struct fake_dl *fake_find_dl(const struct fake_chip *f, uint32_t addr);

/* Number of DMA-enabling writes with trace index > idx. */
unsigned fake_enabling_after(const struct fake_chip *f, uint32_t idx);

/* GLO_CFG as the chip holds it. */
uint32_t fake_glo_cfg(const struct fake_chip *f);
/* Current low-level firmware state register (CONN_ON_MISC). */
uint32_t fake_misc(const struct fake_chip *f);

#endif /* MT792X_TEST_FAKE_CHIP_H */
