/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared between radiod's command files (main.c, serve.c). */
#ifndef RADIOD_H
#define RADIOD_H

#include <stdint.h>

#include "hal_win.h"
#include "mt792x/mt792x.h"

extern struct mt792x g_dev;

/* Everything a run needs before the first chip access: validated firmware, DMA block, proxy with the
 * stop list armed, library initialised. */
struct session {
    struct mt792x_fw fw;
    uint8_t *patch, *ram;
    struct mt792x_patch_info *pinfo;
    struct mt792x_ram_info *rinfo;
    struct halwin *h;
};

int session_open(struct session *s, const char *patch_path, const char *ram_path);
void session_close(struct session *s);

/* Options every transmitting command shares. */
struct tx_cli {
    struct mt792x_options extra; /* only the TX / vif fields are used */
    uint8_t rate;
    int debug;
};

void tx_cli_init(struct tx_cli *c);
int tx_cli_parse(struct tx_cli *c, int argc, char **argv, int *i);
int tx_start(struct session *s, const char *patch_path, const char *ram_path, long chl, const struct tx_cli *c,
             uint8_t foreign_sa, const uint8_t *mac);
const char *tx_power_desc(const struct tx_cli *c);
int tx_channel_ok(uint8_t ch);

uint8_t band_of(uint8_t ch);
void print_masked_addr(const uint8_t *a);
int random_control_mac(uint8_t *mac, const uint8_t *own);

int serve(int argc, char **argv);

#endif
