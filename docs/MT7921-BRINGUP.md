# MT7921 (PCIe) bring-up specification for a polling user-mode host

Source: Linux mt76, clone `third_party/openwrt_mt76` @ `0936617dc529ff9da0385221d3fa6de732231177`.
All `file:line` references are relative to that clone root. Files used are `SPDX: BSD-3-Clause-Clear`.

Conventions:

- **FACT** (default): read directly from the cited source line(s).
- **ASSUMPTION**: inferred, not stated by the source.
- **TODO-VERIFY**: could not be determined from the source.
- **MEASURED**: value read on the target laptop right after the vendor Windows driver unloaded (given as input to this document, not from mt76).
- "BAR" = offset into BAR0 (1 MiB). "logical" = the address mt76 passes to `mt76_rr/wr/rmw`.
- All multi-byte fields are little-endian unless marked BE.
- `set X` = `reg |= X`, `clear X` = `reg &= ~X`, `rmw(mask,val)` = `reg = (reg & ~mask) | val` (`mmio.c:26-31`, `mt76.h:1217-1224`).

Target: PCI `14c3:7961`, `mt76_chip == 0x7961`, which makes `is_connac2()` true (`mt76_connac.h:281-285`) and `is_mt7925/7927/7928/7902()` false (`mt76_connac.h:246-274`). Every chip-conditional branch below was resolved with that.

---

## 1. Register access model

### 1.1 Translation function

`__mt7921_reg_addr(addr)` (`mt7921/pci.c:70-145`), applied by `mt7921_rr/wr/rmw` (`mt7921/pci.c:147-169`) to every `mt76_rr/wr/rmw/set/clear/poll` call:

1. If `addr < 0x100000`: return `addr` unchanged (direct BAR offset) (`mt7921/pci.c:120-121`).
2. Else walk `fixed_map[]` in table order; for the first entry with `addr >= phys` and `(addr - phys) <= size`, return `maps + (addr - phys)` (`mt7921/pci.c:123-134`). Note the comparison is `ofs > size → skip`, i.e. `ofs == size` is accepted (off-by-one in the source; do not rely on it).
3. Else if `addr` is in `[0x18000000,0x18c00000)`, `[0x70000000,0x78000000)` or `[0x7c000000,0x7c400000)`: L1 remap (1.3) (`mt7921/pci.c:136-139`).
4. Else: error, returns 0 (`mt7921/pci.c:141-144`).

Ring registers (`desc_base/ring_size/cpu_idx/dma_idx`) are accessed with raw `readl/writel` on `mmio.regs + ring_base + idx*0x10`, bypassing translation; they are direct BAR offsets anyway (`dma.h:106-118`, `dma.c:838`).

### 1.2 Complete `fixed_map` (`mt7921/pci.c:72-117`, in table order)

| # | logical base (`phys`) | BAR base (`maps`) | size | block |
|---|---|---|---|---|
| 0 | 0x820d0000 | 0x30000 | 0x10000 | WF_LMAC_TOP (WF_WTBLON) |
| 1 | 0x820ed000 | 0x24800 | 0x00800 | WF_LMAC_TOP BN0 (WF_MIB) |
| 2 | 0x820e4000 | 0x21000 | 0x00400 | BN0 WF_TMAC |
| 3 | 0x820e7000 | 0x21e00 | 0x00200 | BN0 WF_DMA |
| 4 | 0x820eb000 | 0x24200 | 0x00400 | BN0 WF_LPON |
| 5 | 0x820e2000 | 0x20800 | 0x00400 | BN0 WF_AGG |
| 6 | 0x820e3000 | 0x20c00 | 0x00400 | BN0 WF_ARB |
| 7 | 0x820e5000 | 0x21400 | 0x00800 | BN0 WF_RMAC |
| 8 | 0x00400000 | 0x80000 | 0x10000 | WF_MCU_SYSRAM |
| 9 | 0x00410000 | 0x90000 | 0x10000 | WF_MCU_SYSRAM (configure register) |
| 10 | 0x40000000 | 0x70000 | 0x10000 | WF_UMAC_SYSRAM |
| 11 | 0x54000000 | 0x02000 | 0x01000 | WFDMA PCIE0 MCU DMA0 |
| 12 | 0x55000000 | 0x03000 | 0x01000 | WFDMA PCIE0 MCU DMA1 |
| 13 | 0x58000000 | 0x06000 | 0x01000 | WFDMA PCIE1 MCU DMA0 (MEM_DMA) |
| 14 | 0x59000000 | 0x07000 | 0x01000 | WFDMA PCIE1 MCU DMA1 |
| 15 | 0x7c000000 | 0xf0000 | 0x10000 | CONN_INFRA |
| 16 | 0x7c020000 | 0xd0000 | 0x10000 | CONN_INFRA, WFDMA |
| 17 | 0x7c060000 | 0xe0000 | 0x10000 | CONN_INFRA, conn_host_csr_top |
| 18 | 0x80020000 | 0xb0000 | 0x10000 | WF_TOP_MISC_OFF |
| 19 | 0x81020000 | 0xc0000 | 0x10000 | WF_TOP_MISC_ON |
| 20 | 0x820c0000 | 0x08000 | 0x04000 | WF_UMAC_TOP (PLE) |
| 21 | 0x820c8000 | 0x0c000 | 0x02000 | WF_UMAC_TOP (PSE) |
| 22 | 0x820cc000 | 0x0e000 | 0x01000 | WF_UMAC_TOP (PP) |
| 23 | 0x820cd000 | 0x0f000 | 0x01000 | WF_MDP_TOP |
| 24 | 0x74030000 | 0x10000 | 0x10000 | PCIE_MAC_IREG |
| 25 | 0x820ce000 | 0x21c00 | 0x00200 | WF_LMAC_TOP (WF_SEC) |
| 26 | 0x820cf000 | 0x22000 | 0x01000 | WF_LMAC_TOP (WF_PF) |
| 27 | 0x820e0000 | 0x20000 | 0x00400 | BN0 WF_CFG |
| 28 | 0x820e1000 | 0x20400 | 0x00200 | BN0 WF_TRB |
| 29 | 0x820e9000 | 0x23400 | 0x00200 | BN0 WF_WTBLOFF |
| 30 | 0x820ea000 | 0x24000 | 0x00200 | BN0 WF_ETBF |
| 31 | 0x820ec000 | 0x24600 | 0x00200 | BN0 WF_INT |
| 32 | 0x820f0000 | 0xa0000 | 0x00400 | BN1 WF_CFG |
| 33 | 0x820f1000 | 0xa0600 | 0x00200 | BN1 WF_TRB |
| 34 | 0x820f2000 | 0xa0800 | 0x00400 | BN1 WF_AGG |
| 35 | 0x820f3000 | 0xa0c00 | 0x00400 | BN1 WF_ARB |
| 36 | 0x820f4000 | 0xa1000 | 0x00400 | BN1 WF_TMAC |
| 37 | 0x820f5000 | 0xa1400 | 0x00800 | BN1 WF_RMAC |
| 38 | 0x820f7000 | 0xa1e00 | 0x00200 | BN1 WF_DMA |
| 39 | 0x820f9000 | 0xa3400 | 0x00200 | BN1 WF_WTBLOFF |
| 40 | 0x820fa000 | 0xa4000 | 0x00200 | BN1 WF_ETBF |
| 41 | 0x820fb000 | 0xa4200 | 0x00400 | BN1 WF_LPON |
| 42 | 0x820fc000 | 0xa4600 | 0x00200 | BN1 WF_INT |
| 43 | 0x820fd000 | 0xa4800 | 0x00800 | BN1 WF_MIB |

### 1.3 L1 remap (`mt7921_reg_map_l1`, `mt7921/mt7921.h:231-242`)

Constants (`mt7921/regs.h:63-70`): `MT_HIF_REMAP_L1` = BAR `0xfe24c` (direct); `MT_HIF_REMAP_L1_MASK` = `0x0000ffff`; `MT_HIF_REMAP_L1_OFFSET` = `GENMASK(15,0)` (applied to the *address*); `MT_HIF_REMAP_L1_BASE` = `GENMASK(31,16)` (applied to the *address*); `MT_HIF_REMAP_BASE_L1` = `0x40000`.

Procedure for logical `addr`:

1. `base = addr >> 16` (16 bits), `offset = addr & 0xffff`.
2. `v = read(0xfe24c); write(0xfe24c, (v & 0xffff0000) | base)` — only bits 15:0 of the register are replaced; **bits 31:16 are preserved** (`mt7921/mt7921.h:237`).
3. `read(0xfe24c)` and discard ("use read to push write") (`mt7921/mt7921.h:238-239`).
4. Access BAR `0x40000 + offset`. The window is BAR `0x40000..0x4ffff` (64 KiB) and shows logical `base<<16 .. +0xffff`.

The remap register is rewritten on **every** access to an L1 address (no caching), so each L1 read costs 2 reads + 1 write + the access; an L1 `rmw` costs two full remaps. mt76 has no lock around this; a single-threaded host must simply never interleave two L1 accesses.

MEASURED `0xfe24c = 0x18451800`: low 16 bits `0x1800` = window currently on logical `0x1800xxxx`; high 16 bits `0x1845` must be kept. ASSUMPTION: the high half is a second remap base used by the vendor driver (mt76 never writes it).

### 1.4 Registers used in this document

| name | logical | BAR | how | source |
|---|---|---|---|---|
| `MT_HIF_REMAP_L1` | 0xfe24c | 0xfe24c | direct | `mt7921/regs.h:63-66` |
| `MT_PCIE_MAC_INT_ENABLE` | 0x10188 | 0x10188 | direct | `mt7921/regs.h:81-83` |
| `MT_PCIE_MAC_PM` | 0x10194 | 0x10194 | direct | `mt7921/regs.h:84` |
| `MT_CONN_ON_LPCTL` | 0x7c060010 | 0xe0010 | fixed #17 | `mt792x_regs.h:500` |
| `MT_CONN_ON_MISC` | 0x7c0600f0 | 0xe00f0 | fixed #17 | `mt792x_regs.h:505` |
| `MT_WFSYS_SW_RST_B` | 0x18000140 | 0x40140, remap base 0x1800 | L1 | `mt792x_dma.c:605`, `mt7921/regs.h:72` |
| `MT_HW_BOUND` | 0x70010020 | 0x40020, remap base 0x7001 | L1 (explicit `mt7921_l1_rr`) | `mt792x_regs.h:428` |
| `MT_HW_CHIPID` | 0x70010200 | 0x40200, remap base 0x7001 | L1 | `mt792x_regs.h:429` |
| `MT_HW_REV` | 0x70010204 | 0x40204, remap base 0x7001 | L1 | `mt792x_regs.h:430` |
| `MT_TOP_LPCR_HOST_BAND0` | 0x18060010 | 0x40010, remap base 0x1806 | L1 (explicit) | `mt792x_regs.h:404-407` |
| `MT_HW_EMI_CTL` | 0x18011100 | 0x41100, remap base 0x1801 | L1 — **not used by mt7921**, see 2.4 | `mt792x_regs.h:432` |
| `MT_SWDEF_MODE` | 0x41f23c | 0x9f23c | fixed #9 | `mt792x_regs.h:397-399` |
| `MT_WFDMA_DUMMY_CR` | 0x54000120 | 0x02120 | fixed #11 | `mt792x_regs.h:414-417` |
| `MT_DMASHDL_SW_CONTROL` | 0x7c026004 | 0xd6004 | fixed #16 | `mt792x_regs.h:437-438` |
| `MT_WFDMA0_*` (base `MT_WFDMA0_BASE` = 0xd4000) | 0xd4xxx | 0xd4xxx | direct | `mt792x_regs.h:262-263` |

WFDMA0 block (all direct; `mt792x_regs.h:265-383`, `mt7921/regs.h:31,61`):

| name | BAR |
|---|---|
| `MT_WFDMA0_RST` | 0xd4100 |
| `MT_WFDMA0_BUSY_ENA` | 0xd413c |
| `MT_MCU_CMD` | 0xd41f0 |
| `MT_MCU2HOST_SW_INT_ENA` | 0xd41f4 |
| `MT_WFDMA0_HOST_INT_STA` | 0xd4200 |
| `MT_WFDMA0_HOST_INT_ENA` | 0xd4204 |
| `MT_WFDMA0_GLO_CFG` | 0xd4208 |
| `MT_WFDMA0_RST_DTX_PTR` | 0xd420c |
| `MT_WFDMA0_RST_DRX_PTR` | 0xd4280 |
| `MT_WFDMA0_INT_RX_PRI` / `INT_TX_PRI` | 0xd4298 / 0xd429c |
| `MT_WFDMA0_GLO_CFG_EXT0` / `EXT1` | 0xd42b0 / 0xd42b4 |
| `MT_WFDMA0_PRI_DLY_INT_CFG0` | 0xd42f0 |
| `MT_TX_RING_BASE` | 0xd4300 |
| `MT_RX_EVENT_RING_BASE` | 0xd4500 |
| `MT_RX_DATA_RING_BASE` | 0xd4520 |
| `MT_WFDMA0_TX_RINGn_EXT_CTRL` | 0xd4600 + 4·n |
| `MT_WFDMA0_RX_RINGn_EXT_CTRL` | 0xd4680 + 4·n |

### 1.5 Poll helpers (used everywhere below)

- `mt76_poll_msec(reg, mask, val, timeout_ms)` = `____mt76_poll_msec(..., tick=10)`; `mt76_poll_msec_tick(..., timeout_ms, tick)` (`mt76.h:1236-1240`).
- Loop (`util.c:27-43`): `n = timeout/tick; do { if ((read(reg) & mask) == val) return true; sleep(tick ms .. 2·tick ms); } while (n-- > 0); return false;` → `timeout/tick + 1` reads.
- `mt76_poll(reg, mask, val, timeout_us)` (`util.c:9-25`): same with `udelay(10)`, `timeout/10 + 1` reads.

---

## 2. Probe-time sequence (PCIe MT7921), in execution order

### 2.1 `mt7921_pci_probe` (`mt7921/pci.c:259-452`)

| step | operation | value / mask | purpose | source |
|---|---|---|---|---|
| P1 | PCI config: enable device; set `PCI_COMMAND_MEMORY` if clear; `pci_set_master` (bus-master enable) | config space | MMIO decode + DMA | `mt7921/pci.c:315-324` |
| P2 | allocate 1 IRQ vector (any type); `dma_set_mask(32 bit)` | — | 32-bit DMA addresses only | `mt7921/pci.c:326-332` |
| P3 | optional ASPM disable (module param, default off) | config space `LNKCTL` | — | `mt7921/pci.c:334-335`, `pci.c:9-52` |
| P4 | read RAM firmware file from disk to extract feature byte (no hardware access) | — | CNM capability | `mt7921/pci.c:337-338`, `mt792x_core.c:861-929` |
| P5 | map BAR0; install translated bus ops | — | section 1 | `mt7921/pci.c:354-398` |
| P6 | `aspm_supported = true` if ASPM is enabled on the device or its parent link | config space `LNKCTL` | adds a delay in P8 | `mt7921/pci.c:400-401`, `pci.c:55-75` |
| P7 | `mt792xe_mcu_fw_pmctrl` (2.2) | | hand ownership to firmware first | `mt7921/pci.c:403-405` |
| P8 | `__mt792xe_mcu_drv_pmctrl` (2.3) | | take driver ownership | `mt7921/pci.c:407-409` |
| P9 | L1 read `MT_HW_CHIPID` (BAR 0x40200, remap 0x7001) → `chipid` (u16) | | chip id | `mt7921/pci.c:411` |
| P10 | if `chipid == 0x7961`: L1 read `MT_HW_BOUND` (BAR 0x40020); if bit `BIT(7)` set → `chipid = 0x7920` | mask 0x80 | MT7920 detection | `mt7921/pci.c:412-413` |
| P11 | L1 read `MT_HW_REV` (BAR 0x40204); `rev = (chipid << 16) \| (val & 0xff)` | | "ASIC revision" | `mt7921/pci.c:414-416` |
| P12 | `mt792x_wfsys_reset` (2.5) | | reset Wi-Fi subsystem | `mt7921/pci.c:418-420` |
| P13 | write `MT_WFDMA0_HOST_INT_ENA` (0xd4204) = 0x0 | | mask WFDMA interrupts | `mt7921/pci.c:422` |
| P14 | write `MT_PCIE_MAC_INT_ENABLE` (0x10188) = 0xff | | PCIe MAC interrupt enable | `mt7921/pci.c:424` |
| P15 | `devm_request_irq` | — | — | `mt7921/pci.c:426-429` |
| P16 | `mt7921_dma_init` (2.6) | | rings + enable DMA | `mt7921/pci.c:431-433` |
| P17 | `mt7921_register_device` → queues `init_work` (2.9) | | MCU init + firmware | `mt7921/pci.c:435`, `mt7921/init.c:252` |

MEASURED vs P9–P11: chip id 0x7961, `HW_REV` 0x8a10 → `0x8a10 & 0xff = 0x10` → rev `0x79610010`, matching the ASIC revision in the project context.

### 2.2 `mt792xe_mcu_fw_pmctrl` — give ownership to firmware (`mt792x_core.c:1085-1110`)

Bits (`mt792x_regs.h:500-503`): `PCIE_LPCR_HOST_SET_OWN` = `BIT(0)` (0x1), `PCIE_LPCR_HOST_CLR_OWN` = `BIT(1)` (0x2), `PCIE_LPCR_HOST_OWN_SYNC` = `BIT(2)` (0x4).

```
for i in 0 .. 9:                                   # MT792x_DRV_OWN_RETRY_COUNT = 10 (mt792x.h:43)
    write(0xe0010, 0x1)                            # SET_OWN
    if poll((read(0xe0010) & 0x4) == 0x4, timeout 50 ms, tick 1 ms): break   # 51 reads, 1–2 ms apart
if i == 10: fail "firmware own failed" (-EIO)
```

### 2.3 `__mt792xe_mcu_drv_pmctrl` — take ownership (`mt792x_core.c:1040-1061`)

```
for i in 0 .. 9:
    write(0xe0010, 0x2)                            # CLR_OWN
    if aspm_supported: sleep 2–3 ms               # mt792x_core.c:1047-1048
    if poll((read(0xe0010) & 0x4) == 0x0, timeout 50 ms, tick 1 ms): break
if i == 10: fail "driver own failed" (-EIO)
```

`OWN_SYNC` (bit 2) = 1 means firmware owns, 0 means driver owns. The non-`__` wrapper `mt792xe_mcu_drv_pmctrl` additionally calls `mt792x_wpdma_reinit_cond` (3.9) (`mt792x_core.c:1064-1083`); probe uses the `__` variant.

A host without config-space access cannot evaluate `aspm_supported`. ASSUMPTION: always sleeping 2–3 ms after `CLR_OWN` is harmless.

### 2.4 `MT_HW_EMI_CTL`

`MT_HW_EMI_CTL` (logical 0x18011100) / `MT_HW_EMI_CTL_SLPPROT_EN` (`BIT(1)`) are defined in the shared header (`mt792x_regs.h:432-433`) but the only user in the tree is the **MT7925** probe (`mt7925/pci.c:740`). The MT7921 PCIe path never reads or writes it. Do not write it for MT7921.

### 2.5 `mt792x_wfsys_reset` → `mt792x_wfsys_reset_default` (`mt792x_dma.c:603-624`)

Register: `is_connac2 ? 0x18000140 : 0x7c000140` → MT7921 uses logical **0x18000140** (`mt792x_dma.c:605`) = L1, remap base 0x1800, BAR **0x40140**. Bits (`mt792x_regs.h:515-516`): `WFSYS_SW_RST_B` = `BIT(0)`, `WFSYS_SW_INIT_DONE` = `BIT(4)`.

```
L1-rmw(0x18000140): clear 0x1        # assert reset            (mt792x_dma.c:607)
sleep 50 ms                           # msleep(50)              (mt792x_dma.c:608)
L1-rmw(0x18000140): set 0x1          # release reset           (mt792x_dma.c:609)
poll((L1-read(0x18000140) & 0x10) == 0x10, timeout 500 ms, tick 10 ms)   # 51 reads, 10–20 ms apart (mt792x_dma.c:611-612)
on timeout: -ETIMEDOUT
```

The poll goes through `__mt76_poll_msec` → translated read, so the remap register is rewritten on every poll iteration.

### 2.6 `mt7921_dma_init` (`mt7921/pci.c:171-257`)

| step | operation | source |
|---|---|---|
| D1 | L1 read `MT_HW_CHIPID`; `== 0x7902` selects the MT7902 layout (false here) | `mt7921/pci.c:182` |
| D2 | `mt792x_dma_disable(force = true)` (2.7) | `mt7921/pci.c:198` |
| D3 | allocate + program TX data ring: hw idx 0 (`MT7921_TXQ_BAND0`), 2048 descriptors, base `MT_TX_RING_BASE` (ring registers at 0xd4300) | `mt7921/pci.c:203-205`, `mt7921/mt7921.h:17,118` |
| D4 | write `MT_WFDMA0_TX_RING0_EXT_CTRL` (0xd4600) = 0x4 (later overwritten by D11) | `mt7921/pci.c:209` |
| D5 | MCU command ring `MT_MCUQ_WM`: hw idx 17 (`MT7921_TXQ_MCU_WM`), 256 desc (ring registers at 0xd4410) | `mt7921/pci.c:212-213`, `mt7921/mt7921.h:18,120-121` |
| D6 | firmware download ring `MT_MCUQ_FWDL`: hw idx 16 (`MT7921_TXQ_FWDL`), 128 desc (0xd4400) | `mt7921/pci.c:218-219`, `mt7921/mt7921.h:19,120` |
| D7 | RX MCU event ring `MT_RXQ_MCU`: idx 0, 8 desc, buf 2048, base `MT_RX_EVENT_RING_BASE` (0xd4500) | `mt7921/pci.c:224-227`, `mt7921/mt7921.h:22,127` |
| D8 | RX "MCU_WA" ring `MT_RXQ_MCU_WA`: idx 0, 512 desc, buf 2048, base `MT_WFDMA0(0x540)` (0xd4540 = hardware RX ring 4) | `mt7921/pci.c:231-236`, `mt7921/mt7921.h:23` |
| D9 | RX data ring `MT_RXQ_MAIN`: idx 0 (`MT7921_RXQ_BAND0`), 1536 desc, buf 2048, base `MT_RX_DATA_RING_BASE` (0xd4520 = hardware RX ring 2) | `mt7921/pci.c:242-244`, `mt7921/mt7921.h:21,125` |
| D10 | `mt76_init_queues`: fill every RX ring with buffers and kick (3.5) | `mt7921/pci.c:248`, `dma.c:1127-1133,1166-1171` |
| D11 | `mt792x_dma_enable` (2.8) | `mt7921/pci.c:256` |

Per ring allocation, `mt76_dma_alloc_queue` → `mt76_dma_queue_reset(reset_idx = true)` performs, in this order (`dma.c:882`, `dma.c:221-245`, `dma.c:201-219`):

1. every descriptor: `ctrl = MT_DMA_CTL_DMA_DONE` (0x80000000) (`dma.c:232-233`);
2. write `cpu_idx` = 0, write `dma_idx` = 0 (`dma.c:241-242`);
3. write `ring_size` = ndesc (`dma.c:208`);
4. write `desc_base` = physical address of descriptor array (32 bit) (`dma.c:215`);
5. read `dma_idx` → `head = tail =` that value (clamped to 0 if ≥ ndesc) (`dma.c:217-218`, `dma.c:193-199`).

### 2.7 `mt792x_dma_disable(force)` (`mt792x_dma.c:447-485`)

| step | operation | value | source |
|---|---|---|---|
| 1 | clear in `MT_WFDMA0_GLO_CFG` (0xd4208) | `TX_DMA_EN`(b0) \| `RX_DMA_EN`(b2) \| `CSR_DISP_BASE_PTR_CHAIN_EN`(b15) \| `OMIT_TX_INFO`(b28) \| `OMIT_RX_INFO`(b27) \| `OMIT_RX_INFO_PFET2`(b21) = **0x18208005** | `mt792x_dma.c:450-455` |
| 2 | poll `(GLO_CFG & (TX_DMA_BUSY(b1) \| RX_DMA_BUSY(b3))) == 0`, i.e. mask 0xa, timeout 100 ms, tick 1 ms; timeout → `-ETIMEDOUT` | | `mt792x_dma.c:457-460` |
| 3 | (MT7927 only: `RST_DTX_PTR`/`RST_DRX_PTR`) — skipped | | `mt792x_dma.c:462-465` |
| 4 | clear in `MT_WFDMA0_GLO_CFG_EXT0` (0xd42b0): `CSR_TX_DMASHDL_EN` = `BIT(6)` (0x40) | | `mt792x_dma.c:468-469`, `mt792x_regs.h:346` |
| 5 | set in `MT_DMASHDL_SW_CONTROL` (logical 0x7c026004, BAR 0xd6004): `MT_DMASHDL_DMASHDL_BYPASS` = `BIT(28)` (0x10000000) | | `mt792x_dma.c:470`, `mt792x_regs.h:437-439` |
| 6 | if `force`: clear in `MT_WFDMA0_RST` (0xd4100): `DMASHDL_ALL_RST`(b5) \| `LOGIC_RST`(b4) = 0x30 | | `mt792x_dma.c:474-476`, `mt792x_regs.h:265-267` |
| 7 | if `force`: set in `MT_WFDMA0_RST`: 0x30 | | `mt792x_dma.c:478-480` |

The source ends the force reset with the two bits **set** (clear-then-set, no delay in between).

### 2.8 `mt792x_dma_enable` (`mt792x_dma.c:280-360`), non-mt7925 branch

| step | operation | source |
|---|---|---|
| 1 | prefetch: 14 writes, table in 3.4 | `mt792x_dma.c:260-277,285` |
| 2 | write `MT_WFDMA0_RST_DTX_PTR` (0xd420c) = 0xffffffff | `mt792x_dma.c:288` |
| 3 | (`RST_DRX_PTR` is written only for mt7925/mt7928 — **not** for MT7921) | `mt792x_dma.c:289-290` |
| 4 | write `MT_WFDMA0_PRI_DLY_INT_CFG0` (0xd42f0) = 0x0 | `mt792x_dma.c:293` |
| 5 | set in `GLO_CFG` (0xd4208): **0x5020b870** (bits in 3.7) | `mt792x_dma.c:327-336` |
| 6 | set in `GLO_CFG`: `TX_DMA_EN` \| `RX_DMA_EN` = 0x5 | `mt792x_dma.c:339-340` |
| 7 | set in `MT_WFDMA_DUMMY_CR` (logical 0x54000120, BAR 0x02120): `MT_WFDMA_NEED_REINIT` = `BIT(1)` | `mt792x_dma.c:349` |
| 8 | `mt76_connac_irq_enable(tx.all \| rx.all \| MT_INT_MCU_CMD)`: updates the software mask only (address argument 0 → no register write) and schedules the IRQ tasklet, which then writes `HOST_INT_ENA` (2.10) | `mt792x_dma.c:352-355`, `mt76_connac.h:429-433`, `mmio.c:91-108` |
| 9 | set in `MT_MCU2HOST_SW_INT_ENA` (0xd41f4): `MT_MCU_CMD_WAKE_RX_PCIE` = `BIT(0)` | `mt792x_dma.c:356`, `mt792x_regs.h:274-283` |

### 2.9 `init_work` → `mt7921_init_hardware` (`mt7921/init.c:84-168`)

Retry wrapper: up to `MT792x_MCU_INIT_RETRY_COUNT` = 10 attempts of `__mt7921_init_hardware`; between failed attempts `mt792x_init_reset` = `mt792x_wpdma_reset(dev, true)` (2.11) (`mt7921/init.c:115-121`, `mt792x.h:44`, `mt7921/pci.c:39-42`).

`__mt7921_init_hardware` (`mt7921/init.c:84-107`):

| step | operation | source |
|---|---|---|
| H1 | write `MT_SWDEF_MODE` (logical 0x41f23c, BAR 0x9f23c) = `MT_SWDEF_NORMAL_MODE` (0) — "force firmware operation mode into normal state, which should be set before firmware download stage" | `mt7921/init.c:88-91`, `mt792x_regs.h:397-400` |
| H2 | `mt7921e_mcu_init` (below) | `mt7921/init.c:92` |
| H3 | `mt76_eeprom_override` (host-side only) | `mt7921/init.c:96` |
| H4 | `mt7921_mcu_set_eeprom` (9.1) | `mt7921/init.c:100` |
| H5 | `mt7921_mac_init` (9.2) | `mt7921/init.c:104` |

`mt7921e_mcu_init` (`mt7921/pci_mcu.c:41-63`):

| step | operation | source |
|---|---|---|
| M1 | `mt7921e_driver_own`: `reg = L1-map(MT_TOP_LPCR_HOST_BAND0 = 0x18060010)` (remap base 0x1806, BAR 0x40010); write `reg` = `MT_TOP_LPCR_HOST_DRV_OWN` (`BIT(1)`, 0x2); poll `(read(reg) & MT_TOP_LPCR_HOST_FW_OWN (BIT(0))) == 0`, timeout 500 ms, tick 10 ms; timeout → `-EIO` "Timeout for driver own". The remap is written once; the write and the polls use BAR 0x40010 directly, so no other L1 access may intervene. | `mt7921/pci_mcu.c:7-19`, `mt792x_regs.h:404-409` |
| M2 | `rmw_field(MT_PCIE_MAC_PM (0x10194), MT_PCIE_MAC_PM_L0S_DIS = BIT(8), 1)` → set bit 8 (0x100): disable L0s | `mt7921/pci_mcu.c:56`, `mt792x_regs.h:435` |
| M3 | `mt7921_run_firmware` (sections 4–6, 9) | `mt7921/pci_mcu.c:58` |
| M4 | `tx_cleanup` of the FWDL ring (host bookkeeping) | `mt7921/pci_mcu.c:60` |

Two different ownership registers are used: `MT_CONN_ON_LPCTL` (0xe0010) in probe and `MT_TOP_LPCR_HOST_BAND0` (0x40010 via L1) in `mcu_init`. Both are written by mt76; ASSUMPTION: they are two views of the same low-power control (the bit layout is the same: bit 0 = FW own, bit 1 = DRV own request). TODO-VERIFY by reading both.

### 2.10 Interrupt path (for reference; a polling host does not use it)

- ISR: write `HOST_INT_ENA` = 0, schedule tasklet (`mt792x_dma.c:11-25`).
- Tasklet (`mt792x_dma.c:28-71`): write `HOST_INT_ENA` = 0; `intr = read(MT_WFDMA0_HOST_INT_STA 0xd4200) & irqmask`; write `HOST_INT_STA` = `intr` (write-1-to-clear ack); if `intr & MT_INT_MCU_CMD (BIT(29))`: `sw = read(MT_MCU_CMD 0xd41f0)`; write `MT_MCU_CMD` = `sw` (ack); if `sw & MT_MCU_CMD_WAKE_RX_PCIE (BIT(0))` treat as RX-data interrupt; then re-enable remaining mask bits by writing `HOST_INT_ENA`.
- Interrupt bits for MT7921 (`mt7921/regs.h:31-59`, `mt792x_regs.h:311-331`): RX data = `BIT(2)`; RX MCU (ring 0) = `BIT(0)`; RX MCU ring 4 ("WM2") = `BIT(22)`; TX done data ring 0 = `BIT(4)`; TX done FWDL (ring 16) = `BIT(26)`; TX done MCU (ring 17) = `BIT(27)`; `MT_INT_TX_DONE_ALL` = `BIT(27) | BIT(4) | GENMASK(18,4)`; `MT_INT_MCU_CMD` = `BIT(29)`.

### 2.11 `mt792x_wpdma_reset(force)` (`mt792x_dma.c:386-414`) and `mt792x_dma_reset` (`mt792x_dma.c:362-384`)

1. Host: flush all TX queues and MCU queues (`tx_cleanup(flush = true)`), flush all RX queues (`rx_cleanup`) (`mt792x_dma.c:391-398`). `tx_cleanup(flush)` ends with `mt76_dma_sync_idx` + kick: rewrites `ring_size`, `desc_base`, re-reads `dma_idx` into head/tail and writes `cpu_idx = head` (`dma.c:452-457`).
2. If `force`: `mt792x_wfsys_reset` (2.5) (`mt792x_dma.c:400-404`).
3. `mt792x_dma_reset(force)`: `mt792x_dma_disable(force)` (2.7); for every TX, MCU and RX queue `mt76_queue_reset(reset_idx = true)` (the 5 steps listed in 2.6); `mt792x_dma_enable` (2.8) (`mt792x_dma.c:367-383`).
4. For every RX queue `mt76_queue_rx_reset`: all descriptors `ctrl = DMA_DONE`, free buffers, `sync_idx`, refill + kick (`mt792x_dma.c:409-410`, `dma.c:931-971`).

---

## 3. WFDMA / DMA rings

### 3.1 Descriptor (`struct mt76_desc`, 16 bytes, 4-byte aligned, LE) (`dma.h:120-125`)

| offset | field | content |
|---|---|---|
| 0x0 | `buf0` | physical address of segment 0, bits 31:0 |
| 0x4 | `ctrl` | see below |
| 0x8 | `buf1` | physical address of segment 1 (TX, optional); on RX rings written as 0 (64-bit kernels put address bits 35:32 of buf0 in bits 3:0, `MT_DMA_CTL_SDP0_H`) (`dma.c:271-273,305`) |
| 0xc | `info` | TX: caller-supplied `tx_info` (0 for MCU/FWDL) plus, on 64-bit kernels, high address bits: `MT_DMA_CTL_SDP0_H` = `GENMASK(3,0)`, `MT_DMA_CTL_SDP1_H` = `GENMASK(19,16)`. RX: written 0 by host; read back and passed up as `info` (`dma.c:354-365,307,527-529`) |

`ctrl` bits (`dma.h:14-19`):

| bits | name | meaning |
|---|---|---|
| 13:0 | `MT_DMA_CTL_SD_LEN1` | length of segment 1 (`buf1`) |
| 14 | `MT_DMA_CTL_LAST_SEC1` | segment 1 is the last of the packet |
| 15 | `MT_DMA_CTL_M_DONE` | WED only, unused here |
| 29:16 | `MT_DMA_CTL_SD_LEN0` | length of segment 0 (`buf0`); TX: bytes to send; RX: host writes buffer size, chip overwrites with received length |
| 30 | `MT_DMA_CTL_LAST_SEC0` | segment 0 is the last of the packet (TX: host sets; RX: chip sets; clear on RX = more fragments follow in next descriptor) |
| 31 | `MT_DMA_CTL_DMA_DONE` | set by chip when it has finished with the descriptor (enabled by `TX_WB_DDONE`/`RX_WB_DDONE` in `GLO_CFG` — ASSUMPTION from the bit names); host clears it by writing `ctrl` without bit 31 |

Other `MT_DMA_CTL_*` definitions in `dma.h:20-39` (`TO_HOST`, `TOKEN`, `DROP`, `PN_CHK_FAIL`, `VER_MASK`, magic counters, RRO) are WED/RRO/other-chip features. On RX, mt76 still evaluates `drop = ctrl & (MT_DMA_CTL_TO_HOST_A BIT(12) | MT_DMA_CTL_DROP BIT(14))` (`dma.h:188-209`) — for MT7921 these overlap `SD_LEN1`, which the chip is not expected to set on RX (ASSUMPTION; if frames vanish, check this).

The 32-bit `buf0` plus the fact that mt76 sets a 32-bit DMA mask (`mt7921/pci.c:330`) means a block below 4 GiB with `buf1`/`info` high bits = 0 is exactly the mt76 configuration.

### 3.2 Ring registers (`struct mt76_queue_regs`, `mt76.h:236-241`)

Per ring, at `ring_base + hw_idx * 0x10` (`MT_RING_SIZE` = 0x10, `dma.h:12`, `dma.c:838`):

| offset | register | access |
|---|---|---|
| +0x0 | `desc_base` | W: physical address of descriptor array |
| +0x4 | `ring_size` | W: number of descriptors |
| +0x8 | `cpu_idx` | W: host index (TX: next descriptor the host will fill; RX: last+1 descriptor made available) |
| +0xc | `dma_idx` | R (W 0 at init): chip index |

### 3.3 Rings used by MT7921

| purpose | mt76 queue | hw ring | registers (BAR) base/size/cpu/dma | mt76 size | buffer | source |
|---|---|---|---|---|---|---|
| TX data | `q_tx[*]` (all ACs alias ring 0) | TX 0 | 0xd4300 / 0xd4304 / 0xd4308 / 0xd430c | 2048 | — | `mt7921/pci.c:203-205`, `mt76_connac_mac.c:261-275` |
| TX firmware download | `MT_MCUQ_FWDL` | TX 16 | 0xd4400 / 0xd4404 / 0xd4408 / 0xd440c | 128 | — | `mt7921/pci.c:218-219` |
| TX MCU command (WM) | `MT_MCUQ_WM` | TX 17 | 0xd4410 / 0xd4414 / 0xd4418 / 0xd441c | 256 | — | `mt7921/pci.c:212-213` |
| RX MCU event ("event from WM before firmware download") | `MT_RXQ_MCU` | RX 0 | 0xd4500 / 0xd4504 / 0xd4508 / 0xd450c | 8 | 2048 | `mt7921/pci.c:223-227` |
| RX data | `MT_RXQ_MAIN` | RX 2 | 0xd4520 / 0xd4524 / 0xd4528 / 0xd452c | 1536 | 2048 | `mt7921/pci.c:241-244` |
| RX "MCU_WA" ("Change mcu queue after firmware download"; header comment: "Rx mcu after fw downloaded") | `MT_RXQ_MCU_WA` | RX 4 | 0xd4540 / 0xd4544 / 0xd4548 / 0xd454c | 512 | 2048 | `mt7921/pci.c:231-236`, `mt792x_regs.h:288` |

Notes:

- `MT_RX_BUF_SIZE` = 2048 (`mt76.h:29`). The length mt76 actually writes into `SD_LEN0` is `SKB_WITH_OVERHEAD(2048)` (2048 minus the kernel's `skb_shared_info`, kernel-configuration dependent, < 2048) (`dma.c:779,801`). ASSUMPTION: any value ≤ the real buffer size works; use the full size of the buffer we allocate (2048).
- Which packet types arrive on RX ring 4 after the RAM firmware boots (MCU events, TX-free notifications `PKT_TYPE_TXRX_NOTIFY`, TX status) is not stated per ring; mt76 dispatches by packet type on all three RX rings identically (`mt7921/mac.c:578-621`). The MT7902 comment "RX Ring0 is larger and shared for event/TX-done … does not use the MCU_WA ring" (`mt7921/pci.c:184-189`) implies that on MT7921 TX-done and post-boot events use ring 4. ASSUMPTION — a polling host must poll rings 0, 2 and 4.
- Minimum valid ring sizes: **TODO-VERIFY**. Not stated anywhere in mt76. Evidence that sizes are host-chosen: MT7902 uses 512 instead of 8 for RX ring 0 with the same code (`mt7921/pci.c:190-194`), MT7925 uses 512 (`mt7925/mt7925.h:22`). The prefetch depth is 4 entries per ring (3.4); ASSUMPTION: sizes ≥ 8 and powers of two are safe. mt76's own sizes need 2048·16 + 256·16 + 128·16 + (8+512+1536)·(16+2048) bytes ≈ 4.1 MiB and do **not** fit in a 2 MiB block; RX data and RX ring 4 must be shrunk (e.g. 256 and 64 descriptors — ASSUMPTION).
- Does the firmware require rings that mt76 does not allocate? mt76 writes prefetch entries for RX 3, RX 5 and TX 1–6 (3.4) but allocates none of them. So only TX 0/16/17 and RX 0/2/4 have a `desc_base`.

### 3.4 Prefetch (`EXT_CTRL`) values, non-mt7925 branch (`mt792x_dma.c:155,260-277`)

`PREFETCH(base, depth) = (base << 16) | depth`. Field masks: `MT_WPDMA0_BASE_PTR_MASK` = `GENMASK(31,16)`, `MT_WPDMA0_MAX_CNT_MASK` = `GENMASK(7,0)` (`mt792x_regs.h:370-371`). Written in this order:

| # | register | BAR | value |
|---|---|---|---|
| 1 | `RX_RING0_EXT_CTRL` | 0xd4680 | 0x00000004 |
| 2 | `RX_RING2_EXT_CTRL` | 0xd4688 | 0x00400004 |
| 3 | `RX_RING3_EXT_CTRL` | 0xd468c | 0x00800004 |
| 4 | `RX_RING4_EXT_CTRL` | 0xd4690 | 0x00c00004 |
| 5 | `RX_RING5_EXT_CTRL` | 0xd4694 | 0x01000004 |
| 6 | `TX_RING0_EXT_CTRL` | 0xd4600 | 0x01400004 |
| 7 | `TX_RING1_EXT_CTRL` | 0xd4604 | 0x01800004 |
| 8 | `TX_RING2_EXT_CTRL` | 0xd4608 | 0x01c00004 |
| 9 | `TX_RING3_EXT_CTRL` | 0xd460c | 0x02000004 |
| 10 | `TX_RING4_EXT_CTRL` | 0xd4610 | 0x02400004 |
| 11 | `TX_RING5_EXT_CTRL` | 0xd4614 | 0x02800004 |
| 12 | `TX_RING6_EXT_CTRL` | 0xd4618 | 0x02c00004 |
| 13 | `TX_RING16_EXT_CTRL` | 0xd4640 | 0x03400004 |
| 14 | `TX_RING17_EXT_CTRL` | 0xd4644 | 0x03800004 |

(Base 0x0300 is skipped in the source.)

### 3.5 RX: handing buffers to the chip and re-arming

Initial fill (`mt76_dma_rx_fill_buf`, `dma.c:775-814`; `mt76_dma_add_rx_buf`, `dma.c:247-320`):

```
while queued < ndesc - 1:                 # one descriptor is always left unfilled
    d = &desc[head]
    d.buf0 = buffer physical address
    d.buf1 = 0
    d.ctrl = buf_len << 16                # SD_LEN0 only; DMA_DONE and LAST_SEC0 clear
    d.info = 0
    head = (head + 1) % ndesc; queued++
write cpu_idx = head                      # kick (dma.c:413-420, preceded by wmb())
```

After a fresh ring (`dma_idx` = 0) this leaves `head = ndesc - 1`, `cpu_idx = ndesc - 1`.

Receive (`mt76_dma_dequeue`, `dma.c:568-631`; `mt76_dma_get_buf`, `dma.c:506-566`; `mt76_dma_rx_process`, `dma.c:1000-1099`):

```
loop:
    if queued == 0: stop
    if !(desc[tail].ctrl & DMA_DONE): stop          # completion is detected by the DMA_DONE bit, NOT by dma_idx
    ctrl = desc[tail].ctrl
    len  = (ctrl >> 16) & 0x3fff
    more = !(ctrl & LAST_SEC0)                       # packet continues in next descriptor
    info = desc[tail].info
    tail = (tail + 1) % ndesc; queued--
    deliver / append fragment (drop if len > buffer size)
after the loop: refill exactly as in the initial fill (dma.c:1097) and write cpu_idx = head
```

For a host with one permanent buffer per descriptor slot: the slot being refilled is `head` (the one that was left empty), not the slot just consumed; copy the data out of slot `tail` before the next refill reaches it.

`dma_idx` for RX rings is only read at (re)initialisation by mt76 on this chip (`dma.c:1011-1016` applies to WED/RRO queues only).

### 3.6 TX: enqueue and completion

Enqueue of one raw buffer (MCU command or firmware chunk): `mt76_dma_tx_queue_skb_raw` (`dma.c:633-664`) → `mt76_dma_add_buf` with one segment (`dma.c:322-389`):

```
if queued + 1 >= ndesc - 1: fail (ring full)        # dma.c:643
d = &desc[head]
d.buf0 = physical address of the bytes
d.buf1 = 0
d.info = 0
d.ctrl = (len << 16) | LAST_SEC0 (0x40000000)       # written last; DMA_DONE clear
head = (head + 1) % ndesc; queued++
write cpu_idx = head                                 # after a write barrier
```

Completion (`mt76_dma_tx_cleanup`, `dma.c:422-461`): `last = read(dma_idx)`; every descriptor from `tail` up to (not including) `last` is complete. TX completion is detected by **index compare** (`tail != dma_idx`), not by the `DMA_DONE` bit. A value ≥ ndesc read from `dma_idx` is treated as a hung bus (`dma.c:189-199`).

For data frames with two segments (TXD+TXP in `buf0`, see 9.6) `ctrl = len0 << 16 | LAST_SEC0` when the count of segments is odd, or `len0 << 16 | len1 | LAST_SEC1` for an even count (`dma.c:353-373`).

### 3.7 `GLO_CFG` (0xd4208) bits (`mt792x_regs.h:292-309`)

| bit | name | set by `dma_enable` | meaning (from name; behaviour not documented in mt76 → ASSUMPTION unless noted) |
|---|---|---|---|
| 0 | `TX_DMA_EN` | yes (step 6) | TX DMA engine enable |
| 1 | `TX_DMA_BUSY` | RO | TX engine busy (polled in disable) |
| 2 | `RX_DMA_EN` | yes (step 6) | RX DMA engine enable |
| 3 | `RX_DMA_BUSY` | RO | RX engine busy |
| 5:4 | `DMA_SIZE` | value 3 | descriptor/burst size selector |
| 6 | `TX_WB_DDONE` | yes | write back `DMA_DONE` into TX descriptors |
| 9 | `FW_DWLD_BYPASS_DMASHDL` | no (MT7927 only) | — |
| 11 | `FIFO_DIS_CHECK` | yes | — |
| 12 | `FIFO_LITTLE_ENDIAN` | yes | — |
| 13 | `RX_WB_DDONE` | yes | write back `DMA_DONE` into RX descriptors |
| 15 | `CSR_DISP_BASE_PTR_CHAIN_EN` | yes | chained prefetch base pointers (pairs with 3.4) |
| 20 | `CSR_LBK_RX_Q_SEL_EN` | no | — |
| 21 | `OMIT_RX_INFO_PFET2` | yes | — |
| 26 | `ADDR_EXT_EN` | no | — |
| 27 | `OMIT_RX_INFO` | no (cleared in disable) | — |
| 28 | `OMIT_TX_INFO` | yes | — |
| 30 | `CLK_GAT_DIS` | yes | clock gating disable |

Step-5 mask: b30 + b28 + b21 + b15 + b13 + b12 + b11 + b6 + (3 << 4) = **0x5020b870**; step 6 adds 0x5.

MEASURED `GLO_CFG = 0x1010b870` decodes to b28, b20, b15, b13, b12, b11, b6, `DMA_SIZE` = 3; `TX_DMA_EN`/`RX_DMA_EN` and both BUSY bits are 0 → DMA is stopped. Expected values when replaying mt76 from that state: after `dma_disable` 0x00103870; after enable step 5 0x5030b870; after step 6 **0x5030b875** (bit 20 is inherited, mt76 neither sets nor clears it for MT7921).

### 3.8 Index reset registers

- `MT_WFDMA0_RST_DTX_PTR` (0xd420c): written 0xffffffff in `dma_enable` (`mt792x_dma.c:288`). ASSUMPTION: resets all TX `dma_idx` to 0.
- `MT_WFDMA0_RST_DRX_PTR` (0xd4280): **not written for MT7921** (`mt792x_dma.c:289-290`, only mt7925/mt7928; MT7927 in disable). RX indexes are zeroed by the explicit `dma_idx = 0` writes during queue reset (`dma.c:242`) and by the `MT_WFDMA0_RST` logic reset.
- Order matters: mt76 programs/refills rings first, then `RST_DTX_PTR`, then enables. Because `cpu_idx` for TX rings is 0 at that point, the reset is consistent. RX `cpu_idx` (= ndesc − 1) is written before enable and is not reset.

### 3.9 `MT_WFDMA_DUMMY_CR` / `MT_WFDMA_NEED_REINIT`

- Host sets bit 1 at BAR 0x02120 at the end of `dma_enable` (`mt792x_dma.c:349`).
- `mt792x_dma_need_reinit()` = bit 1 reads back **0** (`mt792x.h:458-461`).
- `mt792x_wpdma_reinit_cond` (`mt792x_dma.c:416-445`), called after every driver-own in normal operation (`mt792x_core.c:1074`): if the bit is 0: write `HOST_INT_ENA` = 0, write `MT_PCIE_MAC_INT_ENABLE` = 0, `mt792x_wpdma_reset(force = false)` (2.11), write `MT_PCIE_MAC_INT_ENABLE` = 0xff.
- ASSUMPTION: the chip clears the bit when WFDMA lost its configuration during firmware-owned low power. A host that never gives ownership back should still check it after any `CLR_OWN`.

### 3.10 Delay-interrupt configuration

Only `MT_WFDMA0_PRI_DLY_INT_CFG0` (0xd42f0) = 0 (`mt792x_dma.c:293`). `MT_WFDMA0_INT_RX_PRI`/`INT_TX_PRI` are touched only for mt7925/7927/7928 (`mt792x_dma.c:174-234`).

### 3.11 What a polling host must do to send one command and get its response

Preconditions: rings programmed, RX rings 0 and 4 filled, `GLO_CFG` enabled, `HOST_INT_ENA` = 0.

1. Build `TXD + payload` (section 4) in a DMA buffer; `seq` chosen per 4.3.
2. Check space: `queued + 1 < ndesc - 1` on TX ring 17.
3. Fill `desc[head]`: `buf0`, `buf1 = 0`, `info = 0`, then `ctrl = (len << 16) | 0x40000000`.
4. `head = (head + 1) % ndesc`; write `cpu_idx` (0xd4418) = `head`.
5. Poll `dma_idx` (0xd441c) until it equals `head` → chip consumed the command (TX completion). The buffer may be reused after that.
6. Poll RX ring 0 **and** RX ring 4: `desc[tail].ctrl & 0x80000000`. For each completed descriptor: read `len`, `LAST_SEC0`; copy out; advance `tail`; re-arm one descriptor at `head` (`ctrl = buf_len << 16`, `buf0`, `buf1 = 0`, `info = 0`), write that ring's `cpu_idx` = new `head`.
7. Parse the event (4.5). If `rxd.seq != seq` or it is an unsolicited event, keep polling (mt76 returns `-EAGAIN` and waits for the next one, `mcu.c:116-131`, `mt7921/mcu.c:39-40`).
8. Timeout: 3 s from send (`mt7921/pci_mcu.c:33`: `mcu.timeout = 3 * HZ`, overriding the 20 s set in `mt76_connac_mcu.c:3556`). No retry (`max_retry` unset in `mt7921_mcu_ops`, `mt7921/pci_mcu.c:43-47`; `mcu.c:118-124`). On timeout mt76 triggers a full chip reset (`mt7921/mcu.c:23-29`).
9. Optional hygiene: write-1-to-clear `HOST_INT_STA` (0xd4200) and `MT_MCU_CMD` (0xd41f0) as the tasklet does (2.10). ASSUMPTION: not required for data flow when `HOST_INT_ENA` = 0.

Firmware chunks: same as steps 2–5 on TX ring 16 (0xd4408 / 0xd440c), no TXD, no response.

---

## 4. MCU message formats

### 4.1 Command header `struct mt76_connac2_mcu_txd` (64 bytes) (`mt76_connac_mcu.h:55-72`)

| offset | size | field | value written by `mt76_connac2_mcu_fill_message` (`mt76_connac_mcu.c:3545-3618`) |
|---|---|---|---|
| 0x00 | 4 | `txd[0]` | `TX_BYTES(total_len) \| PKT_FMT(MT_TX_TYPE_CMD = 2) \| Q_IDX(MT_TX_MCU_PORT_RX_Q0 = 0x20)` = `0x41000000 \| total_len` |
| 0x04 | 4 | `txd[1]` | `MT_TXD1_LONG_FORMAT (BIT(31)) \| HDR_FORMAT(MT_HDR_FORMAT_CMD = 1)` = `0x80010000` |
| 0x08 | 24 | `txd[2..7]` | 0 |
| 0x20 | 2 | `len` | `total_len - 32` (everything after `txd[8]`) |
| 0x22 | 2 | `pq_id` | `MCU_PQ_ID(MT_TX_PORT_IDX_MCU = 1, MT_TX_MCU_PORT_RX_Q0 = 0x20)` = `(1 << 15) \| (0x20 << 10)` = **0x8000** |
| 0x24 | 1 | `cid` | command id (bits 7:0 of `cmd`) |
| 0x25 | 1 | `pkt_type` | `MCU_PKT_ID` = 0xa0 |
| 0x26 | 1 | `set_query` | see 4.3 |
| 0x27 | 1 | `seq` | see 4.3 |
| 0x28 | 1 | `uc_d2b0_rev` | 0 |
| 0x29 | 1 | `ext_cid` | bits 15:8 of `cmd` |
| 0x2a | 1 | `s2d_index` | `MCU_S2D_H2N` = 0 (host → WM); `MCU_S2D_H2C` = 2 only with the WA flag (never on MT7921) |
| 0x2b | 1 | `ext_cid_ack` | 1 if `ext_cid != 0`, else 0 |
| 0x2c | 20 | `rsv[5]` | 0 |
| 0x40 | n | payload | |

`total_len` = 64 + payload length. Field masks: `MT_TXD0_TX_BYTES` = `GENMASK(15,0)`, `MT_TXD0_PKT_FMT` = `GENMASK(24,23)`, `MT_TXD0_Q_IDX` = `GENMASK(31,25)`, `MT_TXD1_HDR_FORMAT` = `GENMASK(17,16)` (`mt76_connac2_mac.h:50-66`); enums `mt76_connac2_mac.h:7-19,345-356`; `MCU_PQ_ID`, `MCU_PKT_ID` `mt76_connac_mcu.h:52-53`.

The same `Q_IDX`/`PKT_FMT` is used for all commands; the FWDL ring is selected only by which TX ring the buffer is queued on. `MT_TX_MCU_PORT_RX_FWDL` (0x3e) exists (`mt76_connac2_mac.h:350`) but is used only by mt7615 (`mt7615/mcu.c:88`).

### 4.2 Unified command header `struct mt76_connac2_mcu_uni_txd` (48 bytes) (`mt76_connac_mcu.h:103-123`)

Used when the `cmd` value has `__MCU_CMD_FIELD_UNI` (`BIT(17)`) (`mt76_connac_mcu.c:3565,3577-3587`). Not needed for firmware download.

| offset | size | field | value |
|---|---|---|---|
| 0x00 | 32 | `txd[8]` | as in 4.1 (`txd[0] = 0x41000000 \| total_len`, `txd[1] = 0x80010000`) |
| 0x20 | 2 | `len` | `total_len - 32` |
| 0x22 | 2 | `cid` | command id (16 bit) |
| 0x24 | 1 | `rsv` | 0 |
| 0x25 | 1 | `pkt_type` | 0xa0 |
| 0x26 | 1 | `frag_n` | 0 |
| 0x27 | 1 | `seq` | see 4.3 |
| 0x28 | 2 | `checksum` | 0 |
| 0x2a | 1 | `s2d_index` | 0 |
| 0x2b | 1 | `option` | `MCU_CMD_UNI_EXT_ACK` = `ACK(BIT0) \| UNI(BIT1) \| SET(BIT2)` = 0x07 (always, also for queries, at this commit) (`mt76_connac_mcu.h:1212-1217`) |
| 0x2c | 4 | `rsv1` | 0 |
| 0x30 | n | payload | |

### 4.3 `mt76_connac2_mcu_fill_message` rules (`mt76_connac_mcu.c:3545-3618`)

`cmd` encoding (`mt76_connac_mcu.h:1220-1245`): bits 7:0 id, 15:8 ext id, `BIT(16)` QUERY, `BIT(17)` UNI, `BIT(18)` CE, `BIT(19)` WA, `BIT(20)` WM. `MCU_CMD(x)` = id only; `MCU_EXT_CMD(x)` = id 0xed + ext id; `MCU_CE_CMD(x)` = id + CE flag.

1. **Sequence**: `seq = ++msg_seq & 0xf; if (seq == 0) seq = ++msg_seq & 0xf;` → cycles 1..15, never 0. Incremented for **every** message, including `FW_SCATTER` chunks and messages sent without waiting (`mt76_connac_mcu.c:3558-3560`). `msg_seq` starts at 0 (ASSUMPTION: zero-initialised device struct), so the first command has seq 1.
2. **`FW_SCATTER`** (`cmd == MCU_CMD(FW_SCATTER)`, id 0xee): return immediately — **no header of any kind is prepended**; the buffer is raw firmware bytes (`mt76_connac_mcu.c:3562-3563`). Queued on the FWDL ring (TX 16) (`mt7921/pci_mcu.c:35-38`). (USB/SDIO add their own headers elsewhere; not applicable to PCIe.)
3. UNI commands: 48-byte header (4.2). All other commands: 64-byte header (4.1). There is no "short" format on this path.
4. `set_query` (`mt76_connac_mcu.c:3598-3606`, enum `mt76_connac_mcu.h:1118-1123`): if `ext_cid != 0` or the CE flag is set: `MCU_Q_QUERY` (0) when the QUERY flag is set, else `MCU_Q_SET` (1). Otherwise `MCU_Q_NA` (3). All firmware-download commands are plain `MCU_CMD` → 3.
5. All commands except `FW_SCATTER` go to TX ring 17 (`mt7921/pci_mcu.c:26,35-38`).

### 4.4 Command and event ids

Commands used for firmware download (`mt76_connac_mcu.h:1374-1389`):

| name | id |
|---|---|
| `MCU_CMD_TARGET_ADDRESS_LEN_REQ` | 0x01 |
| `MCU_CMD_FW_START_REQ` | 0x02 |
| `MCU_CMD_INIT_ACCESS_REG` | 0x03 |
| `MCU_CMD_NIC_POWER_CTRL` | 0x04 |
| `MCU_CMD_PATCH_START_REQ` | 0x05 |
| `MCU_CMD_PATCH_FINISH_REQ` | 0x07 |
| `MCU_CMD_PATCH_SEM_CONTROL` | 0x10 |
| `MCU_CMD_EXT_CID` | 0xed |
| `MCU_CMD_FW_SCATTER` | 0xee |
| `MCU_CMD_RESTART_DL_REQ` | 0xef |

Events (`mt76_connac_mcu.h:1053-1074`): `MCU_EVENT_TARGET_ADDRESS_LEN` / `FW_START` / `GENERIC` = 0x01, `ACCESS_REG` 0x02, `MT_PATCH_SEM` 0x04, `REG_ACCESS` 0x05, `LP_INFO` 0x07, `SCAN_DONE` 0x0d, `TX_DONE` 0x0f, `ROC` 0x10, `BSS_ABSENCE` 0x11, `BSS_BEACON_LOSS` 0x13, `CH_PRIVILEGE` 0x18, `SCHED_SCAN_DONE` 0x23, `DBG_MSG` 0x27, `RSSI_NOTIFY` 0x96, `TXPWR` 0xd0, `EXT` 0xed, `RESTART_DL` 0xef, `COREDUMP` 0xf0. mt76 does not check `eid` when matching a response; only `seq` (4.5).

### 4.5 Event format `struct mt76_connac2_mcu_rxd` (36-byte header) (`mt76_connac_mcu.h:125-143`)

The RX buffer starts directly with this structure (no extra prefix on PCIe; `skb->data` = start of DMA buffer, `dma.c:1073-1082`).

| offset | size | field |
|---|---|---|
| 0x00 | 24 | `rxd[6]` — hardware RX descriptor. `rxd[0]`: `MT_RXD0_LENGTH` = `GENMASK(15,0)`, `MT_RXD0_PKT_FLAG` = `GENMASK(19,16)`, `MT_RXD0_PKT_TYPE` = `GENMASK(31,27)` (`mt76_connac2_mac.h:185-187`) |
| 0x18 | 2 | `len` |
| 0x1a | 2 | `pkt_type_id` |
| 0x1c | 1 | `eid` |
| 0x1d | 1 | `seq` |
| 0x1e | 1 | `option` |
| 0x1f | 1 | `rsv` |
| 0x20 | 1 | `ext_eid` |
| 0x21 | 2 | `rsv1` |
| 0x23 | 1 | `s2d_index` |
| 0x24 | n | payload (`tlv[]`) |

Classification (`mt7921/mac.c:578-621`): `type = rxd[0] >> 27`; an MCU event has `type == PKT_TYPE_RX_EVENT` (7) and `PKT_FLAG != 1` (type 7 with flag 1 is re-typed to `PKT_TYPE_NORMAL_MCU` (8), i.e. an 802.11 frame). Packet types: `TXS` 0, `TXRXV` 1, `NORMAL` 2, `RX_DUP_RFB` 3, `RX_TMR` 4, `RETRIEVE` 5, `TXRX_NOTIFY` 6, `RX_EVENT` 7, `NORMAL_MCU` 8, `RX_FW_MONITOR` 0x0c (`mt76_connac.h:9-21`).

Solicited vs unsolicited (`mt7921/mcu.c:379-414`): event is dropped if shorter than 0x21 bytes. Unsolicited if `option & MCU_UNI_CMD_UNSOLICITED_EVENT (BIT(2))`. Otherwise, if `eid == 0x06` → response. Otherwise unsolicited if `ext_eid == MCU_EXT_EVENT_RATE_REPORT (0x87)` or `eid ∈ {0x13, 0x23, 0x96, 0x0d, 0x0f, 0x27, 0xf0, 0x07}` or **`seq == 0`**; everything else is a response.

Response matching and result (`mt7921_mcu_parse_response`, `mt7921/mcu.c:16-92`):

- no event within the timeout → `-ETIMEDOUT` + chip reset;
- `rxd.seq != seq` → `-EAGAIN` (discard, keep waiting);
- `PATCH_SEM_CONTROL` and `PATCH_FINISH_REQ`: minimum length 33 bytes; **result = the single byte at offset 0x20** (`sizeof(rxd) - 4`, i.e. the `ext_eid` position);
- UNI `DEV_INFO_UPDATE`, `BSS_INFO_UPDATE`, `STA_REC_UPDATE`, `HIF_CTRL`, `OFFLOAD`, `SUSPEND`: payload at 0x24 is `{ u8 cid; u8 pad[3]; le32 status; }` (`mt76_connac_mcu.h:1883-1887`); result = `status`; if `cid` ≠ command id → `-EAGAIN`;
- every other command (including `TARGET_ADDRESS_LEN_REQ`, `PATCH_START_REQ`, `FW_START_REQ`): minimum length 36 bytes; result 0; payload = bytes from 0x24. **No status field is checked** for these.

### 4.6 Worked examples (bytes as they appear in the DMA buffer; `ss` = sequence number)

Common 64-byte header skeleton, `LL` = low byte of `total_len`, `ll` = `total_len - 32`:

```
00: LL 00 00 41  00 00 01 80  00 00 00 00  00 00 00 00
10: 00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
20: ll 00 00 80  CC a0 03 ss  00 00 00 00  00 00 00 00
30: 00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
40: <payload>
```

(`txd[0]` = `0x410000LL` LE; `txd[1]` = `0x80010000` LE; `pq_id` = 0x8000 LE; `pkt_type` a0; `set_query` 03; `ext_cid` 00; `s2d_index` 00; `ext_cid_ack` 00.)

| command | `CC` | payload struct (source) | payload bytes | `total_len` → `LL`, `ll` | descriptor `ctrl` |
|---|---|---|---|---|---|
| `NIC_POWER_CTRL` (`mt76_connac_mcu_restart`), no response awaited | 04 | `{ u8 power_mode = 1; u8 rsv[3]; }` (`mt76_connac_mcu.c:2985-2996`) | `01 00 00 00` | 68 → `44`, `24` | 0x40440000 |
| `PATCH_SEM_CONTROL` get | 10 | `{ le32 op = PATCH_SEM_GET (1); }` (`mt76_connac_mcu.c:24-35`, `mt76_connac_mcu.h:1420-1423`) | `01 00 00 00` | 68 → `44`, `24` | 0x40440000 |
| `PATCH_SEM_CONTROL` release | 10 | `{ le32 op = PATCH_SEM_RELEASE (0); }` | `00 00 00 00` | 68 → `44`, `24` | 0x40440000 |
| `PATCH_START_REQ` | 05 | `{ le32 addr; le32 len; le32 mode; }` (`mt76_connac_mcu.c:54-78`) | e.g. addr 0x00900000, len 0x00012345, mode 0x80000000: `00 00 90 00 45 23 01 00 00 00 00 80` | 76 → `4c`, `2c` | 0x404c0000 |
| `TARGET_ADDRESS_LEN_REQ` | 01 | same struct | same layout | 76 → `4c`, `2c` | 0x404c0000 |
| `PATCH_FINISH_REQ` | 07 | `{ u8 check_crc = 0; u8 reserved[3]; }` (`mt76_connac_mcu.c:38-49`) | `00 00 00 00` | 68 → `44`, `24` | 0x40440000 |
| `FW_START_REQ` | 02 | `{ le32 option; le32 addr; }` (`mt76_connac_mcu.c:9-21`) | e.g. option 1, addr 0x00915000: `01 00 00 00 00 50 91 00`; no override: 8 × `00` | 72 → `48`, `28` | 0x40480000 |

Full dump, `PATCH_SEM_CONTROL` get with seq 2:

```
00: 44 00 00 41 00 00 01 80 00 00 00 00 00 00 00 00
10: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
20: 24 00 00 80 10 a0 03 02 00 00 00 00 00 00 00 00
30: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
40: 01 00 00 00
```

Choice between `PATCH_START_REQ` and `TARGET_ADDRESS_LEN_REQ` (`mt76_connac_mcu.c:68-75`), for MT7921: `PATCH_START_REQ` (0x05) if `addr == 0x200000` (`MCU_PATCH_ADDRESS`, `mt76_connac_mcu.c:52`) or `addr == 0x900000`; otherwise `TARGET_ADDRESS_LEN_REQ` (0x01). This rule is applied to patch sections **and** RAM regions alike.

`FW_SCATTER` chunk: no header. The DMA buffer contains only ≤ 4096 firmware bytes; descriptor on TX ring 16: `buf0` = address, `ctrl = (chunk_len << 16) | 0x40000000` (4096 bytes → `0x50000000`), `buf1 = 0`, `info = 0`. A sequence number is consumed per chunk but not transmitted.

---

## 5. Firmware files

File names (`mt792x.h:49,57,553-591`): RAM `mediatek/WIFI_RAM_CODE_MT7961_1.bin`, patch `mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin` (default branch of `mt792x_ram_name`/`mt792x_patch_name` for chip 0x7961).

### 5.1 ROM patch file

`struct mt76_connac2_patch_hdr` — 96 bytes at file offset 0, numeric fields **big-endian** (`mt76_connac_mcu.h:147-162`):

| offset | size | field |
|---|---|---|
| 0x00 | 16 | `build_date` (chars) |
| 0x10 | 4 | `platform` (chars) |
| 0x14 | 4 | `hw_sw_ver` BE32 |
| 0x18 | 4 | `patch_ver` BE32 |
| 0x1c | 2 | `checksum` BE16 |
| 0x1e | 2 | `rsv` |
| 0x20 | 4 | `desc.patch_ver` BE32 |
| 0x24 | 4 | `desc.subsys` BE32 |
| 0x28 | 4 | `desc.feature` BE32 |
| 0x2c | 4 | `desc.n_region` BE32 |
| 0x30 | 4 | `desc.crc` BE32 |
| 0x34 | 44 | `desc.rsv[11]` |

`struct mt76_connac2_patch_sec` — 64 bytes each, section `i` at file offset `96 + 64·i`, **big-endian** (`mt76_connac_mcu.h:164-178`, `mt76_connac_mcu.c:3375`):

| offset | size | field |
|---|---|---|
| 0x00 | 4 | `type` BE32 |
| 0x04 | 4 | `offs` BE32 — file offset of the section data |
| 0x08 | 4 | `size` BE32 |
| 0x0c | 4 | `info.addr` BE32 — load address |
| 0x10 | 4 | `info.len` BE32 — bytes to download |
| 0x14 | 4 | `info.sec_key_idx` BE32 |
| 0x18 | 4 | `info.align_len` BE32 |
| 0x1c | 36 | `info.rsv[9]` |

Validation (`mt76_connac2_patch_check`, `mt76_connac_mcu.c:3311-3335`): file ≥ 96 bytes; `n_region ≤ (size − 96) / 64`; each `offs ≤ size` and `len ≤ size − offs`.

Iteration (`mt76_connac2_load_patch`, `mt76_connac_mcu.c:3369-3400`), for `i = 0 .. n_region − 1`:

1. `(type & PATCH_SEC_TYPE_MASK (0xffff)) != PATCH_SEC_TYPE_INFO (2)` → abort `-EINVAL` (then release semaphore).
2. `addr = info.addr`, `len = info.len`, `data = file + offs`, `sec_info = info.sec_key_idx`.
3. `mode = mt76_connac2_get_data_mode(sec_info)` (`mt76_connac_mcu.c:3283-3309`, constants `mt76_connac_mcu.h:17-38`):
   - start with `DL_MODE_NEED_RSP` = `BIT(31)`;
   - if `sec_info == 0xffffffff` (`PATCH_SEC_NOT_SUPPORT`): done → `0x80000000`;
   - `enc = sec_info >> 24` (`PATCH_SEC_ENC_TYPE_MASK` = `GENMASK(31,24)`):
     - 0 (`PLAIN`): `0x80000000`;
     - 1 (`AES`): `|= DL_MODE_ENCRYPT (BIT0) | ((sec_info & 0xff) << 1) & DL_MODE_KEY_IDX (GENMASK(2,1)) | DL_MODE_RESET_SEC_IV (BIT3)`;
     - 2 (`SCRAMBLE`): `|= DL_MODE_ENCRYPT (BIT0) | DL_CONFIG_ENCRY_MODE_SEL (BIT6) | DL_MODE_RESET_SEC_IV (BIT3)` → `0x80000049`;
     - other: error message, mode unchanged.
4. Send init-download `{addr, len, mode}` (4.6 rule for the command id), wait for response.
5. Send `data[0..len)` as `FW_SCATTER` chunks of at most `max_len` bytes.

`max_len` = `mt76_is_sdio ? 2048 : 4096` → **4096 for PCIe** (`mt76_connac_mcu.c:3340`). Chunking (`__mt76_mcu_send_firmware`, `mcu.c:142-165`): `cur = min(4096, remaining)`; send without waiting for a response; then `tx_cleanup` of the FWDL ring (reap completed descriptors).

After all sections: `PATCH_FINISH_REQ` (wait; result byte must be 0, otherwise "Failed to start patch"), then **always** `PATCH_SEM_CONTROL` release, whose result must be `PATCH_REL_SEM_SUCCESS` (3) or the whole load fails with `-EAGAIN` (`mt76_connac_mcu.c:3402-3419`).

### 5.2 Patch semaphore result codes (`mt76_connac_mcu.h:1132-1137`; handling `mt76_connac_mcu.c:3344-3353,3407-3415`)

| value | name | on `get` | on `release` |
|---|---|---|---|
| 0 | `PATCH_NOT_DL_SEM_FAIL` | error "Failed to get patch semaphore", `-EAGAIN` | error |
| 1 | `PATCH_IS_DL` | patch already loaded: return success immediately, **skip the whole patch download, do not release the semaphore**, continue with the RAM image | error |
| 2 | `PATCH_NOT_DL_SEM_SUCCESS` | proceed with download | error |
| 3 | `PATCH_REL_SEM_SUCCESS` | error | success |
| other / negative | (timeout etc.) | error | error |

### 5.3 RAM firmware file

`struct mt76_connac2_fw_trailer` — the **last 36 bytes** of the file, LE (`mt76_connac_mcu.h:180-190`, `mt76_connac_mcu.c:3200`):

| offset (from `size − 36`) | size | field |
|---|---|---|
| 0x00 | 1 | `chip_id` |
| 0x01 | 1 | `eco_code` |
| 0x02 | 1 | `n_region` |
| 0x03 | 1 | `format_ver` |
| 0x04 | 1 | `format_flag` |
| 0x05 | 2 | `rsv` |
| 0x07 | 10 | `fw_ver` (chars) |
| 0x11 | 15 | `build_date` (chars) |
| 0x20 | 4 | `crc` LE32 |

`struct mt76_connac2_fw_region` — 40 bytes each, LE, stored immediately **before** the trailer; region `i` is at file offset `size − 36 − (n_region − i)·40` (`mt76_connac_mcu.h:192-202`, `mt76_connac_mcu.c:3094-3095`):

| offset | size | field |
|---|---|---|
| 0x00 | 4 | `decomp_crc` |
| 0x04 | 4 | `decomp_len` |
| 0x08 | 4 | `decomp_blk_sz` |
| 0x0c | 4 | `rsv` |
| 0x10 | 4 | `addr` LE32 |
| 0x14 | 4 | `len` LE32 |
| 0x18 | 1 | `feature_set` |
| 0x19 | 1 | `type` (`FW_TYPE_DEFAULT` 0, `FW_TYPE_CLC` 2; `mt76_connac_mcu.h:46-50`) |
| 0x1a | 14 | `rsv1` |

Validation (`mt76_connac2_fw_check`, `mt76_connac_mcu.c:3048-3078`): file ≥ 36 bytes; `n_region ≤ (size − 36)/40`; the sum of all `len` must not exceed `size − 36 − n_region·40`.

`feature_set` bits (`mt76_connac_mcu.h:11-15`): `FW_FEATURE_SET_ENCRYPT` `BIT(0)`, `FW_FEATURE_SET_KEY_IDX` `GENMASK(2,1)`, `FW_FEATURE_ENCRY_MODE` `BIT(4)`, `FW_FEATURE_OVERRIDE_ADDR` `BIT(5)`, `FW_FEATURE_NON_DL` `BIT(6)`.

Iteration (`mt76_connac_mcu_send_ram_firmware`, `mt76_connac_mcu.c:3081-3130`; called with `is_wa = false`, `mt76_connac_mcu.c:3204`; MT7921 passes no WA image, `mt792x_core.c:1149`):

```
offset = 0; override = 0
for i in 0 .. n_region-1:                      # ascending, region data is concatenated from file offset 0
    mode = gen_dl_mode(feature_set)
    len = region.len; addr = region.addr
    if feature_set & FW_FEATURE_NON_DL: goto next          # not downloaded (e.g. CLC table), but its bytes are skipped
    if feature_set & FW_FEATURE_OVERRIDE_ADDR: override = addr
    init_download(addr, len, mode)                          # wait for response; cmd id per 4.6 rule
    send file[offset .. offset+len) as FW_SCATTER chunks of ≤ 4096 bytes
next:
    offset += len
option = (override != 0) ? FW_START_OVERRIDE (BIT0) : 0    # FW_START_WORKING_PDA_CR4 (BIT2) only for WA
FW_START_REQ { option, addr = override }                    # wait for response
```

`gen_dl_mode` (`mt76_connac_mcu.h:1927-1943`), for MT7921 (connac2, `is_wa = false`):

```
mode  = (feature_set & BIT0) ? (DL_MODE_ENCRYPT BIT0 | DL_MODE_RESET_SEC_IV BIT3) : 0
mode |= (feature_set & BIT4) ? DL_CONFIG_ENCRY_MODE_SEL BIT6 : 0
mode |= ((feature_set >> 1) & 0x3) << 1          # DL_MODE_KEY_IDX GENMASK(2,1)
mode |= DL_MODE_NEED_RSP BIT31
```

`FW_START_REQ`: `option`/`addr` as above (`mt76_connac_mcu.c:3124-3129`); constants `FW_START_OVERRIDE` `BIT(0)`, `FW_START_WORKING_PDA_CR4` `BIT(2)`, `FW_START_WORKING_PDA_DSP` `BIT(3)` (`mt76_connac_mcu.h:25-27`). Note that an override address of 0 cannot be expressed (`if (override)`).

The trailer's `fw_ver` (10 chars) and `build_date` (15 chars) are what Linux prints as "WM Firmware Version" (`mt76_connac_mcu.c:3201-3202`).

---

## 6. Complete S3 sequence ("proxy bound, DMA block allocated" → "firmware ready")

Deviations from mt76 are marked **[HOST]**. Everything else is the mt76 order from sections 2–5.

### 6.0 What the MEASURED start state says

| measured | decode | consequence |
|---|---|---|
| `MT_CONN_ON_LPCTL` = 0x0 | `OWN_SYNC` (bit 2) = 0 → driver-owned | chip is awake; mt76 still does fw-own then drv-own (S1, S2) |
| `MT_CONN_ON_MISC` = 0x1 | `MT_TOP_MISC_FW_STATE` (`GENMASK(2,0)`) = 1 = `MT_TOP_MISC2_FW_PWR_ON`; `MT_TOP_MISC2_FW_N9_RDY` (`GENMASK(1,0)`) = 3 is the "firmware running" value | **RAM firmware is not running**; the value equals exactly the "MCU ready for firmware download" state mt76 waits for (`mt792x_core.c:1118-1121`). The vendor driver left the chip reset/ROM-idle. |
| `GLO_CFG` = 0x1010b870 | TX/RX DMA disabled, not busy (3.7) | no DMA in progress |
| `HOST_INT_ENA` = 0, `MT_PCIE_MAC_INT_ENABLE` = 0xff | same as mt76 after P13/P14 | nothing to undo |
| `MT_HIF_REMAP_L1` = 0x18451800 | window on 0x1800xxxx | vendor driver's last L1 access was in the 0x1800xxxx block (the WFSYS reset register lives there) |

Whether the ROM patch is still loaded after the vendor driver's exit cannot be read from these values. mt76 handles both cases through the semaphore (5.2): `PATCH_IS_DL` → skip the patch and go straight to RAM download. Since the sequence performs its own WFSYS reset (S4), ASSUMPTION: the semaphore will answer `PATCH_NOT_DL_SEM_SUCCESS` (2); implement both branches. `MT_CONN_STATUS` (logical 0x7c053c10, `MT_WIFI_PATCH_DL_STATE` = `BIT(0)`, `mt792x_regs.h:497-498`) is defined but not used by the PCIe path — TODO-VERIFY whether it reflects patch state on PCIe.

### 6.1 Steps

| # | action | success condition / proof |
|---|---|---|
| S0 | **[HOST]** Save `read(0xfe24c)` for restore at exit. Lay out the 2 MiB block: descriptor arrays (16 B × ndesc, 4-byte aligned) for TX 0/16/17 and RX 0/2/4, RX buffers (2048 B each), one command buffer, one 4096-byte chunk buffer. Zero everything. | — |
| S1 | fw-own (2.2): up to 10 × { write 0xe0010 = 0x1; poll `(0xe0010 & 0x4) == 0x4`, 50 ms / 1 ms } | bit 2 of 0xe0010 = 1 |
| S2 | drv-own (2.3): up to 10 × { write 0xe0010 = 0x2; sleep 2–3 ms; poll `(0xe0010 & 0x4) == 0`, 50 ms / 1 ms } | bit 2 of 0xe0010 = 0 |
| S3 | L1 read 0x70010200 → expect 0x7961; L1 read 0x70010020 bit 7 must be 0; L1 read 0x70010204 & 0xff → 0x10 | chip id / rev as MEASURED |
| S4 | WFSYS reset (2.5): L1 clear bit 0 @0x18000140; sleep 50 ms; L1 set bit 0; poll `(L1-read & 0x10) == 0x10`, 500 ms / 10 ms | `WFSYS_SW_INIT_DONE` = 1 |
| S5 | write 0xd4204 = 0 | `HOST_INT_ENA` reads 0 |
| S6 | write 0x10188 = 0xff (mt76). **[HOST]** alternative: leave/write as found; see section 8 R1 | — |
| S7 | DMA disable, force (2.7): clear 0x18208005 @0xd4208; poll `(0xd4208 & 0xa) == 0`, 100 ms / 1 ms; clear 0x40 @0xd42b0; set 0x10000000 @0xd6004; clear 0x30 @0xd4100; set 0x30 @0xd4100 | `GLO_CFG` bits 0–3 = 0 |
| S8 | For each ring in the order TX 0, TX 17, TX 16, RX 0, RX 4, RX 2: all descriptors `ctrl = 0x80000000`; write `cpu_idx` = 0; write `dma_idx` = 0; write `ring_size` = ndesc; write `desc_base` = phys; read `dma_idx` (expect 0) → `head = tail`. After TX 0: write 0xd4600 = 0x4. | `dma_idx` reads 0, `desc_base`/`ring_size` read back |
| S9 | Fill RX rings 2, 0, 4 (mt76 enum order `MAIN`, `MCU`, `MCU_WA`): ndesc − 1 buffers each (3.5); write each `cpu_idx` = ndesc − 1 | — |
| S10 | DMA enable (2.8): 14 prefetch writes (3.4); write 0xd420c = 0xffffffff; write 0xd42f0 = 0; set 0x5020b870 @0xd4208; set 0x5 @0xd4208; set 0x2 @0x02120. **[HOST]** skip the interrupt enable (keep 0xd4204 = 0). Set 0x1 @0xd41f4 (optional for a polling host, mt76 does it). | `GLO_CFG` reads 0x5030b875 (if bit 20 was set as MEASURED); 0x02120 bit 1 = 1 |
| S11 | write 0x9f23c (`MT_SWDEF_MODE`) = 0 | — |
| S12 | driver-own, second register (2.9 M1): L1-map 0x18060010 (remap base 0x1806); write 0x40010 = 0x2; poll `(0x40010 & 0x1) == 0`, 500 ms / 10 ms | bit 0 of 0x40010 = 0 |
| S13 | set 0x100 @0x10194 (`MT_PCIE_MAC_PM_L0S_DIS`) | bit 8 = 1 |
| S14 | send `NIC_POWER_CTRL {power_mode = 1}` on TX 17, do **not** wait for a response (`mt792x_core.c:1116`). **[HOST]** wait for `dma_idx(17) == cpu_idx(17)`. | TX 17 `dma_idx` advanced by 1 |
| S15 | poll `(0xe00f0 & 0x7) == 0x1`, 1000 ms / 10 ms. On timeout mt76 only warns "MCU is not ready for firmware download" and continues (`mt792x_core.c:1118-1121`). | `MT_CONN_ON_MISC` low 3 bits = 1 |
| S16 | `PATCH_SEM_CONTROL` get; wait ≤ 3 s for the event with the same `seq`; result = byte 0x20 of the event | 2 → S17; 1 → skip to S21 (no release); else fail |
| S17 | for each patch section: init-download `{addr, len, mode}` (id 0x05 if addr ∈ {0x200000, 0x900000} else 0x01), wait for response (seq match only) | response received |
| S18 | …then the section data as raw ≤ 4096-byte chunks on TX 16. **[HOST]** after each chunk wait until `dma_idx(16) == cpu_idx(16)` before reusing the chunk buffer (mt76 instead uses a fresh buffer per chunk and reaps after each send, `mcu.c:147-161`). | TX 16 `dma_idx` == `cpu_idx` |
| S19 | `PATCH_FINISH_REQ {check_crc = 0}`, wait | result byte 0x20 == 0 |
| S20 | `PATCH_SEM_CONTROL` release, wait (also on any failure in S17–S19) | result byte 0x20 == 3 |
| S21 | for each RAM region without `FW_FEATURE_NON_DL`: init-download (mode per 5.3), wait; then chunks as S18. Track `override`. | as S17/S18 |
| S22 | `FW_START_REQ {option, addr}`, wait ≤ 3 s. The response may arrive on RX ring 0 or 4 — poll both (TODO-VERIFY which). | response with matching `seq` |
| S23 | poll `(0xe00f0 & 0x3) == 0x3` (`MT_TOP_MISC2_FW_N9_RDY`), 1500 ms / 10 ms (151 reads); timeout → "Timeout for initializing firmware", `-EIO` (`mt792x_core.c:1153-1158`) | **`MT_CONN_ON_MISC` bits 1:0 = 3 → firmware ready** |

On any failure in S11–S23 mt76 retries the whole block up to 10 times after `mt792x_wpdma_reset(force = true)` = flush queues, S4, S7, S8, S10, RX re-fill (`mt7921/init.c:115-121`, 2.11).

Sequence-number bookkeeping for S14–S22: S14 uses seq 1, S16 seq 2, each init-download and each chunk consume one number each (wrap 15 → 1). Only the numbers of awaited commands matter.

---

## 7. Safe shutdown

### 7.1 What mt76 does on remove (`mt7921_pci_remove` → `mt7921e_unregister_device`, `mt7921/pci.c:44-68,454-467`)

Hardware-relevant operations in order:

| # | operation | source |
|---|---|---|
| 1 | (mac80211 unregister; if the interface was up: `MAC_INIT_CTRL` disable via `mt792x_stop`, `mt792x_core.c:244-248`) | `mt7921/pci.c:54` |
| 2 | `__mt792x_mcu_drv_pmctrl` (2.3): make sure the driver owns the chip | `mt7921/pci.c:62` |
| 3 | `mt792x_dma_cleanup` (`mt792x_dma.c:487-513`): clear 0x18208005 @0xd4208; poll `(0xd4208 & 0xa) == 0`, 100 ms / 1 ms (result ignored); clear 0x30 @0xd4100; set 0x30 @0xd4100; free host queues | `mt7921/pci.c:63` |
| 4 | `mt792x_wfsys_reset` (2.5): L1 clear bit 0 @0x18000140; 50 ms; set bit 0; poll bit 4, 500 ms | `mt7921/pci.c:64` |
| 5 | free IRQ, free device | `mt7921/pci.c:464-466` |

Not done on remove: **no `fw_pmctrl`** (the chip is left driver-owned), no write to `HOST_INT_ENA` or `MT_PCIE_MAC_INT_ENABLE`, no DMASHDL changes. The resulting state (driver-owned, WFSYS freshly reset, DMA disabled, `MT_PCIE_MAC_INT_ENABLE` = 0xff) is the same as the MEASURED state left by the vendor driver.

`fw_pmctrl` is used only on suspend (`mt7921_pci_suspend`, `mt7921/pci.c:469-550`), whose hardware tail is: poll `(0xd4208 & 0xa) == 0` (`mt76_poll`, 1000 µs); clear 0x5 @0xd4208; write 0xd4204 = 0; write 0x10188 = 0; fw-own (2.2).

### 7.2 Minimal "stop all DMA" replay list for the proxy (≤ 32 operations)

Operation types: `W` = write value, `S` = set bits, `C` = clear bits, all on BAR offsets.

| # | op | BAR | value | purpose | basis |
|---|---|---|---|---|---|
| 1 | W | 0xe0010 | 0x00000002 | request driver ownership so the following writes land (no-op if already owned) | `mt7921/pci.c:62`, 2.3 |
| 2 | W | 0xd4204 | 0x00000000 | mask WFDMA interrupts | `mt7921/pci.c:521` |
| 3 | C | 0xd4208 | 0x18208005 | **stop TX and RX DMA** (the essential write) | `mt792x_dma.c:490-496` |
| 4 | C | 0xd4208 | 0x18208005 | repeat (stands in for the busy poll the proxy cannot do) | [HOST] |
| 5 | C | 0xd4100 | 0x00000030 | WFDMA logic + DMASHDL reset… | `mt792x_dma.c:503-505` |
| 6 | S | 0xd4100 | 0x00000030 | …release | `mt792x_dma.c:507-509` |
| 7 | C | 0xfe24c | 0x0000ffff | L1 remap: clear base field, upper 16 bits preserved | 1.3 |
| 8 | S | 0xfe24c | 0x00001800 | L1 remap: window → 0x1800xxxx | 1.3 |
| 9 | C | 0x40140 | 0x00000001 | assert WFSYS reset: stops the firmware (and anything it does with host memory) | `mt792x_dma.c:607` |
| 10 | S | 0x40140 | 0x00000001 | release WFSYS reset | `mt792x_dma.c:609` |

Notes:

- Rows 3 and 9 are the two writes that matter. Row 3 alone disables the host-side DMA engines; row 9 kills the firmware. ASSUMPTION: the WFDMA host block is in CONN_INFRA and is not reset by the WFSYS reset (that is why mt76 resets it separately through `MT_WFDMA0_RST`), so row 3 must not be omitted.
- mt76 waits 50 ms between rows 9 and 10. If the proxy can delay, insert ≥ 50 ms. If it cannot: TODO-VERIFY whether releasing immediately is accepted; the alternative is to stop after row 9 (WFSYS held in reset) — safe against DMA, and mt76's own start (clear, 50 ms, set) recovers from it, but whether the **vendor** driver starts cleanly from that state is TODO-VERIFY.
- After row 8 the low half of `MT_HIF_REMAP_L1` is 0x1800, identical to the MEASURED value left by the vendor driver.
- The proxy must not free or unlock the DMA block before row 3 has executed.
- `MT_PCIE_MAC_INT_ENABLE` is left at 0xff and ownership is left with the driver, matching both mt76-after-remove and the MEASURED vendor state.
- If the chip was firmware-owned and asleep when the client died, row 1 without a wait may not be sufficient (mt76 polls up to 10 × 50 ms). ASSUMPTION: our host never issues `SET_OWN` after S1, so this cannot occur after S2.

### 7.3 Orderly shutdown by the client (preferred when the process exits normally)

1. Stop sending; (optional, if started) `MAC_INIT_CTRL {enable = 0}` and sniffer disable.
2. Section 7.1 rows 2–4 with the real polls and the 50 ms delay.
3. Restore `MT_HIF_REMAP_L1` low 16 bits to the value saved in S0.

---

## 8. Risks for a polling, user-mode, interrupt-less host

| # | risk | detail | source | mitigation / status |
|---|---|---|---|---|
| R1 | Unhandled interrupt | mt76 enables `HOST_INT_ENA` (TX/RX done, MCU cmd) via the tasklet after `dma_enable`, and writes `MT_PCIE_MAC_INT_ENABLE` = 0xff. A host with no ISR must never write a non-zero `HOST_INT_ENA`. Whether 0x10188 = 0xff with `HOST_INT_ENA` = 0 can still raise an interrupt from another source is unknown. | `mt792x_dma.c:352-356`, `mt7921/pci.c:424` | Keep 0xd4204 = 0. TODO-VERIFY whether 0x10188 should be written 0 while we own the device (mt76 does that only in suspend / reinit, `mt7921/pci.c:522`, `mt792x_dma.c:429`). |
| R2 | Bus mastering | mt76 calls `pci_set_master` and sets `PCI_COMMAND_MEMORY`. Without config-space access we cannot verify that Bus Master Enable is still set after the vendor driver unloaded and our UMDF driver started. If it is clear, register access works but no descriptor is ever fetched. | `mt7921/pci.c:319-324` | Symptom: TX `dma_idx` never moves after a kick. TODO-VERIFY on Windows. |
| R3 | DMA address width / IOMMU | mt76 sets a 32-bit DMA mask; descriptors carry 32-bit `buf0`. mt76 uses bus addresses from the kernel DMA API; a user-mode host uses raw physical addresses, which is only valid if no IOMMU/DMA-remapping sits between the device and RAM. | `mt7921/pci.c:330`, `dma.c:304,375` | Block below 4 GiB satisfies the width. DMA remapping (Kernel DMA Protection) is TODO-VERIFY on the target. |
| R4 | ASPM | `aspm_supported` (config space) adds a 2–3 ms sleep after `CLR_OWN`; L0s is disabled through BAR register 0x10194 bit 8 (accessible). Disabling ASPM through `LNKCTL` is not possible for us. | `mt7921/pci.c:400-401`, `mt792x_core.c:1047-1048`, `mt7921/pci_mcu.c:56` | Always sleep after `CLR_OWN`; set 0x10194 bit 8. |
| R5 | Runtime PM / deep sleep | mt76 enables runtime PM and deep sleep by default on PCIe (`pm.enable`, `pm.ds_enable`) and returns ownership to the firmware after `MT792x_PM_TIMEOUT` idle; every MCU/TX path first wakes the chip. In monitor mode it disables both and sends `KeepFullPwr 1`. | `mt7921/init.c:206-216`, `mt7921/main.c:602-618`, `mt76_connac_mcu.c:2060-2067` | Never issue `SET_OWN` after S1; send chip-config `KeepFullPwr 1` after boot (9.1). |
| R6 | WFDMA re-init | After a low-power exit the firmware may have dropped the WFDMA configuration; mt76 detects this with `MT_WFDMA_NEED_REINIT` and rebuilds rings. | `mt792x_dma.c:416-445`, `mt792x.h:458-461` | Check 0x02120 bit 1 periodically; if 0, redo S7–S10 without WFSYS reset. |
| R7 | RX starvation | mt76 is interrupt driven; with polling, RX ring 0 has only 8 descriptors in mt76's layout. If the host polls slowly the chip stalls or drops. Unsolicited events (firmware log, debug) arrive without a request. | `mt7921/mt7921.h:22` | Poll rings 0/2/4 in the same loop; consider a larger ring 0 (MT7902 uses 512). |
| R8 | TX completion without interrupt | mt76 reaps TX by `dma_idx` in NAPI/after each firmware chunk. A firmware image needs more chunks than the 128-entry FWDL ring if never reaped; ring-full is a hard error (`-ENOMEM`), not a wait. | `dma.c:643`, `mcu.c:157-160` | Wait for `dma_idx == cpu_idx` after each chunk. |
| R9 | MCU timeout semantics | Timeout is 3 s; mt76 reacts with a full reset (`mt792x_reset`), not a resend. Responses are matched by `seq` only; stale responses with another `seq` are silently skipped. | `mt7921/pci_mcu.c:33`, `mt7921/mcu.c:23-40` | Implement the same; never reuse a `seq` while an older response may still arrive. |
| R10 | Event ring switch | Events come on RX ring 0 before the RAM firmware runs and (per comments) on ring 4 afterwards. | `mt7921/pci.c:223,232`, `mt792x_regs.h:288` | Poll both; TODO-VERIFY the exact switch point. |
| R11 | L1 remap is stateful | Every L1 access rewrites 0xfe24c; `mt7921e_driver_own` then uses the mapped BAR offset for a 500 ms poll. Any concurrent L1 access (second thread, proxy-side replay) corrupts the window. | `mt7921/mt7921.h:231-242`, `mt7921/pci_mcu.c:7-19` | Single register-access thread; the proxy's death-replay must only run after the client is gone. |
| R12 | Sleep granularity | mt76 polls with 1–2 ms and 10–20 ms sleeps. Windows default timer granularity is coarser. | `util.c:38` | Use elapsed-time based timeouts, not iteration counts. |
| R13 | Write ordering | mt76 writes `ctrl` last and issues a write barrier before the `cpu_idx` kick. | `dma.c:375-378,415` | On x86 a normal store order suffices; write `ctrl` last, then the register. |
| R14 | Fragmented RX | A packet longer than the RX buffer spans several descriptors (`LAST_SEC0` clear). mt76 drops if `len` exceeds the buffer. | `dma.c:523-524,1052-1061` | Reassemble or drop consistently. |
| R15 | `MT_SWDEF_MODE` | Must be written 0 "before firmware download stage". | `mt7921/init.c:88-91` | S11. |
| R16 | No status check on download commands | mt76 does not inspect the payload of `PATCH_START_REQ`/`TARGET_ADDRESS_LEN_REQ`/`FW_START_REQ` responses; a firmware-side rejection is only visible later (finish result ≠ 0 or `N9_RDY` timeout). | `mt7921/mcu.c:87-89` | Log the raw response payload during bring-up (format TODO-VERIFY). |
| R17 | Combo chip | WFSYS reset is the Wi-Fi subsystem reset; effect on the USB Bluetooth half is not described in mt76. | — | TODO-VERIFY (project note: Wi-Fi reconfiguration resets BT on Linux). |
| R18 | AER / hung bus | mt76 treats a `dma_idx ≥ ndesc` read (e.g. 0xffffffff) as a hung bus. | `dma.c:189-199` | Treat all-ones register reads as fatal. |

---

## 9. After boot (brief)

### 9.1 First MCU commands after firmware start

`mt7921_run_firmware` (`mt7921/mcu.c:682-704`) then `__mt7921_init_hardware` (`mt7921/init.c:96-104`), in order:

| # | command | `cmd` macro → header fields | payload | wait | required? |
|---|---|---|---|---|---|
| 1 | NIC capability | `MCU_CE_CMD(GET_NIC_CAPAB)`: `cid` 0x8a, `set_query` 1 (CE flag, no QUERY flag), `ext_cid` 0 | none | yes | Source of the MAC address, antenna mask, band support, `chip_cap` (`mt7921/mcu.c:597-667`). ASSUMPTION: optional for the firmware itself, needed by us for the MAC address. |
| 2 | EEPROM byte read (inside `mt7921_load_clc`, only if CLC is supported) | `MCU_EXT_QUERY(EFUSE_ACCESS)`: `cid` 0xed, `ext_cid` 0x01, `set_query` 0, `ext_cid_ack` 1 | `{ le32 addr; le32 valid; u8 data[16]; }` with `addr` = offset rounded down to 16 (`mt7921/mcu.h:41-45`, `mt7921/mcu.c:95-123`) | yes | no (regulatory/CLC only) |
| 3 | thermal protection | `MCU_CE_CMD(CHIP_CONFIG)`: `cid` 0xca, `set_query` 1 | `struct mt76_connac_config` (328 bytes: `le16 id; u8 type; u8 resp_type = 0; le16 data_size; le16 resv; char data[320]`) with `data = "DowngradeTxStreamTemp 105 115"` (`mt76_connac_mcu.h:1874-1881`, `mt76_connac_mcu.c:2047-2057`, `mt7921/mcu.c:1521-1532`) | no | ASSUMPTION: no |
| 4 | firmware log to host | `MCU_CE_CMD(FWLOG_2_HOST)`: `cid` 0xc5, `set_query` 1 | `{ u8 ctrl_val = 1; u8 pad[3]; }` (`mt7921/mcu.c:669-680`) | no | no (produces `MCU_EVENT_DBG_MSG` 0x27 events) |
| 5 | EEPROM/efuse mode | `MCU_EXT_CMD(EFUSE_BUFFER_MODE)`: `cid` 0xed, `ext_cid` 0x21, `set_query` 1, `ext_cid_ack` 1 | `{ u8 buffer_mode = EE_MODE_EFUSE (0); u8 format = EE_FORMAT_WHOLE (1); le16 len = 0; }` (`mt7921/mcu.c:972-985`, `mt76_connac_mcu.h:1193-1202`) | yes | ASSUMPTION: yes (tells the firmware to use efuse calibration data) |
| 6 | `mt7921_mac_init` register writes (9.2), then `MCU_EXT_CMD(PROTECT_CTRL)` RTS threshold via `mt76_connac_mcu_set_rts_thresh(0x92b, 0)` | | (`mt7921/init.c:64-81`) | yes | TODO-VERIFY |
| 7 | deep sleep policy (end of `init_work`) | `MCU_CE_CMD(CHIP_CONFIG)` | `data = "KeepFullPwr 0"` when deep sleep enabled, `"KeepFullPwr 1"` to keep the chip awake (`mt7921/init.c:167`, `mt76_connac_mcu.c:2060-2067`) | no | **we want `KeepFullPwr 1`** (project finding: deep sleep kills monitor RX) |

`GET_NIC_CAPAB` response payload (after the 36-byte event header): `{ le16 n_element; u8 rsv[2]; }` then `n_element` × `{ le32 type; le32 len; u8 data[len]; }` (`mt7921/mcu.c:599-662`). Types (`mt76_connac_mcu.h:1471-1492`): `MT_NIC_CAP_MAC_ADDR` = 7 (6 bytes), `MT_NIC_CAP_PHY` = 8 (`{ht, vht, _5g, max_bw, nss, dbdc, tx_ldpc, rx_ldpc, tx_stbc, rx_stbc, hw_path, he}`; `antenna_mask = BIT(nss) − 1`; `hw_path` bit 0 = 2.4 GHz, bit 1 = 5 GHz), `MT_NIC_CAP_6G` = 0x18, `MT_NIC_CAP_CHIP_CAP` = 0x20 (u64).

When the interface is brought up (`__mt7921_start`, `mt7921/main.c:227-270`): `MCU_EXT_CMD(MAC_INIT_CTRL)` (ext 0x46) `{ u8 enable = 1; u8 band = 0; u8 rsv[2]; }`, wait (`mt76_connac_mcu.c:216-230`); `MCU_CE_CMD(SET_CHAN_DOMAIN)` (0x0f) (`mt76_connac_mcu.c:123-211`); `MCU_EXT_CMD(SET_RX_PATH)` (ext 0x4e) with the channel struct of 9.4; TX power (`MCU_CE_CMD(SET_RATE_TX_POWER)` 0x5d); `MCU_EXT_CMD(ID_RADIO_ON_OFF_CTRL)` (ext 0x05) `{ u8 ctrlid; u8 rsv[3]; }` with `ctrlid` 1 then 2, no wait (`mt7921/mcu.c:707-718`, `mt7921/mt7921.h:40-42`). ASSUMPTION: `MAC_INIT_CTRL` enable and a channel command are the minimum for RX; the rest is TODO-VERIFY.

### 9.2 `mt7921_mac_init` register writes (`mt7921/init.c:64-81`, `mt792x_mac.c:290-316`)

| register (logical → BAR) | operation |
|---|---|
| `MT_MDP_DCR1` 0x820cd004 → 0x0f004 | `rmw_field(MT_MDP_DCR1_MAX_RX_LEN GENMASK(15,3), 1536)` |
| `MT_MDP_DCR0` 0x820cd000 → 0x0f000 | set `MT_MDP_DCR0_DAMSDU_EN` `BIT(15)`; set `MT_MDP_DCR0_RX_HDR_TRANS_EN` `BIT(19)` (hardware RX header translation — a raw-802.11 host may prefer to leave this off; frames then carry `MT_RXD2_NORMAL_HDR_TRANS` = 0. TODO-VERIFY effect in sniffer mode) |
| `MT_WTBL_UPDATE` 0x820d4230 → 0x34230 | for idx 0..19: `rmw(MT_WTBL_UPDATE_WLAN_IDX GENMASK(9,0), idx \| MT_WTBL_UPDATE_ADM_COUNT_CLEAR BIT(12))`, poll `MT_WTBL_UPDATE_BUSY BIT(31) == 0`, 5000 µs (`mt7921/mac.c:19-26`, `mt7921/regs.h:74-79`) |
| per band 0/1: `MT_TMAC_CTCR0`, `MT_WF_RMAC_MIB_TIME0`, `MT_WF_RMAC_MIB_AIRTIME0`, `MT_MIB_SCR1`, `MT_DMA_DCR0` (`MAX_RX_LEN` = 1536, clear `RXD_G5_EN` `BIT(23)`), `MT_WTBLOFF_TOP_RSCR` | see `mt792x_mac.c:294-315`; addresses via `mt792x_regs.h:31-68,97-101,219-255` and the fixed map |

`MT_DMA_DCR0_RXD_G5_EN` is cleared "due to hw issues" → RXD group 5 (C-RXV) is normally absent (`mt792x_mac.c:308-309`).

### 9.3 Sniffer (monitor) enable

mt76 path: monitor vif → `mt76_connac_mcu_uni_add_dev` (9.7) at add-interface (`mt7921/main.c:323-325`); on `IEEE80211_CONF_CHANGE_MONITOR`: sniffer enable, PM and deep sleep off, beacon filter off (`mt7921/main.c:602-618,654-658`); on channel-context change for a monitor vif: sniffer enable + sniffer config (`mt7921/main.c:1428-1430`).

Both are UNI commands (48-byte header, 4.2), `cid` = `MCU_UNI_CMD_SNIFFER` = 0x24, waited:

Enable (`mt7921_mcu_set_sniffer`, `mt7921/mcu.c:1185-1213`), 12-byte payload:

| offset | field | value |
|---|---|---|
| 0 | `band_idx` | 0 |
| 1 | `pad[3]` | 0 |
| 4 | `tag` LE16 | 0 |
| 6 | `len` LE16 | 8 |
| 8 | `enable` | 1 / 0 |
| 9 | `pad[3]` | 0 |

Channel config (`mt7921_mcu_config_sniffer`, `mt7921/mcu.c:1215-1281`), 20-byte payload:

| offset | field | value |
|---|---|---|
| 0 | `band_idx`, `pad[3]` | 0 |
| 4 | `tag` LE16 | 1 |
| 6 | `len` LE16 | 16 |
| 8 | `aid` u16 | 0 |
| 10 | `ch_band` | 1 = 2.4 GHz, 2 = 5 GHz, 3 = 6 GHz |
| 11 | `bw` | 0 = 20/40 MHz, 1 = 80, 2 = 160, 3 = 80+80, 4 = 5, 5 = 10, 6 = 320 |
| 12 | `control_ch` | primary channel number |
| 13 | `sco` | 0 none, 1 if `control_ch < center_ch` (SCA), 3 if `control_ch > center_ch` (SCB) |
| 14 | `center_ch` | centre channel number |
| 15 | `center_ch2` | 0 (80+80 only) |
| 16 | `drop_err` | 1 = drop FCS-error frames (mt76: `!(rxfilter & MT7921_FILTER_FCSFAIL)`) |
| 17 | `pad[3]` | 0 |

RX filter (`mt7921_mcu_set_rxfilter`, `mt7921/mcu.c:1534-1554`): `MCU_CE_CMD(SET_RX_FILTER)` (0x0a), no wait, 68-byte payload `{ u8 rsv[4]; u8 mode; u8 rsv2[3]; le32 fif; le32 bit_map; u8 bit_op; u8 pad[51]; }`; `mode` = 1 when `fif != 0` (replace filter) else 2 (bit operation: `bit_op` 1 = set, 2 = clear bits of `bit_map`, which uses the `MT_WF_RFCR_DROP_*` bit numbering of `mt792x_regs.h:223-244`). `fif` flags: `MT7921_FILTER_FCSFAIL` `BIT(2)`, `MT7921_FILTER_CONTROL` `BIT(5)`, `MT7921_FILTER_OTHER_BSS` `BIT(6)`, `MT7921_FILTER_ENABLE` `BIT(31)` (`mt7921/mt7921.h:10-13`, `mt7921/main.c:666-691`).

### 9.4 Channel switch

`mt7921_set_channel` → `mt7921_mcu_set_chan_info(MCU_EXT_CMD(CHANNEL_SWITCH))` (`mt7921/main.c:477-499`, `mt7921/mcu.c:912-970`): `cid` 0xed, `ext_cid` 0x08, `set_query` 1, `ext_cid_ack` 1, waited; 76-byte payload:

| offset | field | value |
|---|---|---|
| 0 | `control_ch` | primary channel |
| 1 | `center_ch` | centre channel |
| 2 | `bw` | `mt76_connac_chan_bw()` (`mt76_connac.h:379`; 20 MHZ = `CMD_CBW_20MHZ`, `mt76_connac.h:56-60`) |
| 3 | `tx_streams_num` | popcount(antenna mask) |
| 4 | `rx_streams` | antenna **mask** for `SET_RX_PATH`; popcount for `CHANNEL_SWITCH` (`mt7921/mcu.c:960-961`) |
| 5 | `switch_reason` | `CH_SWITCH_NORMAL` 0 (monitor / `SET_RX_PATH`), `CH_SWITCH_SCAN_BYPASS_DPD` 9 (off-channel), `CH_SWITCH_DFS` 5 (`mt76_connac_mcu.h:1155-1162`) |
| 6 | `band_idx` | 0 |
| 7 | `center_ch2` | 0 |
| 8 | `cac_case` LE16 | 0 |
| 10 | `channel_band` | 0 = 2.4 GHz, 1 = 5 GHz, 2 = 6 GHz (nl80211 band, 6 GHz mapped to 2) |
| 11 | `rsv0` | 0 |
| 12 | `outband_freq` LE32 | 0 |
| 16 | `txpower_drop`, `ap_bw`, `ap_center_ch` | 0 |
| 19 | `rsv1[57]` | 0 |

After the command mt76 reprograms timing registers (`mt792x_mac_set_timeing`) and resets counters (`mt7921/main.c:488-490`). `MCU_EXT_CMD(SET_RX_PATH)` (ext 0x4e) uses the same struct (`mt7921/main.c:240`).

Which of {`CHANNEL_SWITCH`, sniffer config TLV} actually retunes the radio in sniffer mode, and the switch latency, are TODO-VERIFY (project finding from Linux: trust the frequency reported in RX frames, 9.5).

### 9.5 RX descriptor essentials (802.11 frames) (`mt7921_mac_fill_rx`, `mt7921/mac.c:168-436`; masks `mt76_connac2_mac.h:184-277`)

Frame buffers have `PKT_TYPE` 2 (`NORMAL`) or 8 (`NORMAL_MCU`, incl. type 7 with flag 1) (`mt7921/mac.c:587-612`).

| word | fields used |
|---|---|
| `rxd[0]` | `LENGTH` 15:0; `PKT_FLAG` 19:16; `IP_SUM` b23, `UDP_TCP_SUM` b24; `PKT_TYPE` 31:27 |
| `rxd[1]` | `WLAN_IDX` 9:0; `GROUP_1` b11, `GROUP_2` b12, `GROUP_3` b13, `GROUP_4` b14, `GROUP_5` b15; `SEC_MODE` 20:16; `KEY_ID` 22:21; `CM` b23; `CLM` b24; `ICV_ERR` b25; `TKIP_MIC_ERR` b26; **`FCS_ERR` b27**; `BAND_IDX` b28 (frame rejected if set) |
| `rxd[2]` | `BSSID` 5:0; `MAC_HDR_LEN` 12:8; **`HDR_TRANS` b13** (1 = payload is 802.3, not 802.11); **`HDR_OFFSET` 15:14** (padding = 2 × value bytes before the MAC header); `TID` 19:16; `AMSDU_ERR` b23 (rejected); `MAX_LEN_ERROR` b24 (rejected); `FRAG` b27; `NON_AMPDU` b30 |
| `rxd[3]` | `RXV_SEQ` 7:0; **`CH_FREQ` 15:8** = channel number the frame was received on (> 180 → 6 GHz: `(n − 181)·4 + 1`; > 14 → 5 GHz; else 2.4 GHz; `mt792x.h:444-456`); `ADDR_TYPE` 17:16 (`MT_RXD3_NORMAL_U2M` = 1 = unicast to me) |
| `rxd[4]` | `PAYLOAD_FORMAT` 1:0 (A-MSDU: 3 first, 2 mid, 1 last) |

Variable part after the 6 base words (24 bytes), in this order, each present only if its `rxd[1]` bit is set:

| order | group | size | content |
|---|---|---|---|
| 1 | `GROUP_4` | 4 words | `[0]` `FRAME_CONTROL` 15:0, `TA_LO` 31:16; `[1]` `TA_HI`; `[2]` `SEQ_CTRL` 15:0, `QOS_CTL` 31:16; `[3]` HT control |
| 2 | `GROUP_1` | 4 words | IV / PN bytes |
| 3 | `GROUP_2` | 2 words | `[0]` = timestamp (32 bit) |
| 4 | `GROUP_3` | 2 words | P-RXV: `[0]` rate (`MT_PRXV_TX_RATE` 6:0, `NSTS` 9:7, `HT_AD_CODE` b11, …), `[1]` RCPI0..3 in bytes 0..3 |
| 5 | `GROUP_5` (only evaluated if `GROUP_3`) | 18 words | C-RXV; "Monitor mode would use RCPI described in GROUP 5 instead": RCPI taken from the word at offset 6 of this group (`mt7921/mac.c:350-364`) |

Then `2 × HDR_OFFSET` pad bytes, then the 802.11 header (if `HDR_TRANS` = 0) (`mt7921/mac.c:380-402`). RSSI per chain = `(RCPI − 220) / 2` dBm (`mt7921/mt7921.h:114`). Whether the FCS is included in `LENGTH` is TODO-VERIFY.

Other RX packet types on the same rings: `PKT_TYPE_TXS` (0): 2 header words then 8-word TX status records (`mt7921/mac.c:605-609`); `PKT_TYPE_TXRX_NOTIFY` (6): TX-free notification `{ le16 rx_byte_cnt; le16 ctrl (MSDU count 9:0); le32 txd; }` + `count` × LE32 entries (`MT_TX_FREE_PAIR` b31 → wcid pair entry with `WLAN_ID` 23:14; otherwise `MSDU_ID` 30:16, `STATUS` 14:13 (0 = ok), `COUNT` 12:0) (`mt7921/mac.c:476-551`, `mt76_connac.h:233-237`, `mt76_connac2_mac.h:40-48`).

### 9.6 TX descriptor essentials for a raw 802.11 frame

PCIe data path (`mt7921e_tx_prepare_skb`, `mt7921/pci_mac.c:8-54`; `mt76_dma_tx_queue_skb`, `dma.c:666-745`): first DMA segment = 64-byte "TXWI" = 32-byte TXD (`MT_TXD_SIZE`, `mt76_connac.h:34`) + 32-byte `struct mt76_connac_hw_txp` (`mt76_connac.h:211-224`); the frame itself is **not** in the ring descriptor but referenced from the TXP. MT7921 uses the *hw* TXP format (`is_mt76_fw_txp()` false for 0x7961, `mt76_connac.h:361-377`).

TXP (`mt76_connac_write_hw_txp`, `mt76_connac_mac.c:162-199`): `msdu_id[0]` LE16 = `token | MT_MSDU_ID_VALID (BIT(15))`, `msdu_id[1..3]` = 0; `ptr[0].buf0` LE32 = frame physical address; `ptr[0].len0` LE16 = `(len & MT_TXD_LEN_MASK GENMASK(11,0)) | MT_TXD_LEN_LAST (BIT(15))` for the last buffer; `ptr[0].len1`, `ptr[0].buf1`, `ptr[1]` for further buffers. Ring descriptor: `buf0` = TXWI address, `ctrl = (64 << 16) | LAST_SEC0`, on TX ring 0. The token (0 … `MT7921_TOKEN_SIZE` − 1 = 8191, `mt7921/mt7921.h:29`) comes back in a `TXRX_NOTIFY` (9.5), which is how mt76 frees the frame — completion of the ring descriptor only means the TXWI was fetched.

TXD words (`mt76_connac2_mac_write_txwi`, `mt76_connac_mac.c:504-627`; 802.11 part `mt76_connac_mac.c:409-502`; masks `mt76_connac2_mac.h:50-128`):

| word | value for a management/raw frame |
|---|---|
| `txd[0]` | `TX_BYTES` = frame length + 32; `PKT_FMT` = `MT_TX_TYPE_CT` (0) on PCIe; `Q_IDX` = `MT_LMAC_ALTX0` (0x10) for the PSD/mgmt queue, or `wmm_idx·4 + (3 − ac)` for data ACs |
| `txd[1]` | `LONG_FORMAT` b31; `WLAN_IDX` 9:0 (wcid); `OWN_MAC` 29:24 (omac idx); `HDR_FORMAT` 17:16 = `MT_HDR_FORMAT_802_11` (2); `HDR_INFO` 15:11 = 802.11 header length / 2; `TID` 22:20 |
| `txd[2]` | `FRAME_TYPE` 5:4, `SUB_TYPE` 3:0 (from frame control); `MULTICAST` b10; **`FIX_RATE` b31** (mt76 sets it for every non-data or multicast frame); `HTC_VLD` b13 is set by mt76 whenever `FIX_RATE` is set |
| `txd[3]` | `REM_TX_COUNT` 15:11 = 15 (retry budget); **`NO_ACK` b0** (when no ACK is expected); `BA_DISABLE` b28 with fixed rate; for injected frames `SN_VALID` b31 + `SEQ` 27:16 (keeps the caller's sequence number) |
| `txd[5]` | `PID` 7:0; `TX_STATUS_HOST` b10 when `PID ≥ MT_PACKET_ID_FIRST (3)` → firmware reports a TX status (TXS) for this frame |
| `txd[6]` | with `FIX_RATE`: `FIXED_BW` b2; `TX_RATE` 29:16 = `NSS` 12:10 \| `MODE` 9:6 \| `IDX` 5:0 (`mt76_connac2_mac.h:133-139`); `BW` 1:0 |
| `txd[7]` | `TYPE` 21:20, `SUB_TYPE` 19:16 (PCIe); `SPE_IDX` 15:11 (antenna path selection; mt76 uses `24 + band` for the 2-chain duplicate fallback); `HW_AMSDU` b10 cleared |

Rate encoding (`mt76_connac_mac.c:359-365`, `mac80211.c:160-173`, `mt76.h:334-336,1182-1193`): `MODE` 0 = CCK, 1 = OFDM; legacy `IDX`: CCK 1/2/5.5/11 Mb/s = 0/1/2/3; OFDM 6/9/12/18/24/36/48/54 Mb/s = 11/15/10/14/9/13/8/12. Example OFDM 6 Mb/s: `TX_RATE` = `(1 << 6) | 11` = 0x4b.

ACK result: TX status record (8 words, `MT_TXS*`, `mt76_connac2_mac.h:141-174`): `txs[0]` `ACK_TIMEOUT` b16, `RTS_TIMEOUT` b17, `QUEUE_TIMEOUT` b18 (`MT_TXS0_ACK_ERROR_MASK` = `GENMASK(18,16)`; 0 = acked, `mt76_connac_mac.c:769`), `TX_RATE` 13:0; `txs[2]` `WCID` 25:16; `txs[3]` `PID` 31:24. Delivered as `PKT_TYPE_TXS` packets or inside `MCU_EVENT_TX_DONE` (0x0f) (`mt7921/mac.c:438-474,605-609`, `mt7921/mcu.c:277-286`).

Open points (TODO-VERIFY): whether TX works at all while the sniffer is enabled and which `WLAN_IDX`/`OWN_MAC` a frame needs when no station record exists (mt76 uses the per-vif wcid `MT792x_WTBL_RESERVED − idx` = 19 for vif 0, or the global wcid 0, `mt7921/main.c:332-335`, `mt792x_core.c:978-993`); whether `TXRX_NOTIFY` must be consumed for the firmware to keep transmitting.

### 9.7 Where "ACK frames addressed to our MAC" would be configured

What the code shows:

- **Own MAC address / OMAC entry**: `MCU_UNI_CMD(DEV_INFO_UPDATE)` (UNI `cid` 0x01), payload 16 bytes: `{ u8 omac_idx; u8 band_idx; le16 pad; }` + TLV `{ le16 tag = DEV_INFO_ACTIVE (0); le16 len = 12; u8 active; u8 link_idx; u8 omac_addr[6]; }` (`mt76_connac_mcu.c:1201-1225,1280`, `mt76_connac_mcu.h:1047-1050`). This is the only place in the bring-up path where the host gives the firmware a MAC address to own.
- **BSS entry**: immediately followed by `MCU_UNI_CMD(BSS_INFO_UPDATE)` (UNI `cid` 0x02) with a `UNI_BSS_INFO_BASIC` (tag 0) TLV: `omac_idx`, `band_idx`, `wmm_idx`, `active`, `bmc_tx_wlan_idx`, `sta_idx`, `conn_state = 1`, `conn_type` (`CONNECTION_INFRA_AP` for monitor/AP, `CONNECTION_INFRA_STA` for station) (`mt76_connac_mcu.c:1226-1301`). Order when enabling: DEV_INFO first, then BSS_INFO; reversed when disabling. Both are waited and checked through `{cid, status}` (4.5).
- mt76 sends both for a monitor vif too (`mt7921/main.c:323-325`, `mt76_connac_mcu.c:1254-1257`).
- mt792x declares `no_active_monitor = true` (`mt792x_core.c:824`) and `WANT_MONITOR_VIF` (`mt792x_core.c:841`): mt76 does not claim that the chip ACKs frames in monitor mode.
- RX filter bits relevant to unicast: `MT_WF_RFCR_DROP_OTHER_UC` `BIT(18)`, `MT_WF_RFCR_DROP_OTHER_BSS` `BIT(17)`, `MT_WF_RFCR_DROP_A3_MAC` `BIT(8)` (`mt792x_regs.h:231-241`), reachable through `SET_RX_FILTER` bit operations (9.3).

TODO-VERIFY (not answered by the source): whether the firmware ACKs unicast frames to the `DEV_INFO` address while the sniffer is enabled; whether a `STA_REC`/WTBL entry for the peer is required; whether an AP-type or STA-type BSS is needed for AWDL-style peer-to-peer unicast; how the Linux prior art (`airdrop-mt7921`) obtains ACKs (it uses a separate monitor interface next to a managed/P2P one).

---

## 10. Consolidated TODO-VERIFY list

1. Minimum valid ring sizes; whether non-power-of-two sizes are accepted (3.3).
2. Which RX ring carries MCU events after the RAM firmware starts, and where the `FW_START_REQ` response arrives (3.3, S22, R10).
3. Whether `MT_CONN_ON_LPCTL` and `MT_TOP_LPCR_HOST_BAND0` are the same control (2.9).
4. Whether Bus Master Enable survives the vendor-driver unload / UMDF start (R2).
5. DMA remapping / Kernel DMA Protection on the target (R3).
6. Whether `MT_PCIE_MAC_INT_ENABLE` = 0xff can raise an interrupt with `HOST_INT_ENA` = 0 (R1).
7. Behaviour when WFSYS reset is released without the 50 ms delay, or left asserted, and whether the vendor driver starts cleanly afterwards (7.2).
8. Whether the patch is still loaded after the vendor driver unloads (semaphore answer) (6.0); meaning of `MT_CONN_STATUS` on PCIe.
9. Payload format of the download-command responses (R16).
10. Effect of WFSYS reset on the Bluetooth half (R17).
11. Meaning of the upper 16 bits of `MT_HIF_REMAP_L1` (1.3).
12. Minimum command set for RX after boot; need for `EFUSE_BUFFER_MODE`, `SET_CHAN_DOMAIN`, TX power (9.1).
13. Which command retunes the radio in sniffer mode and its latency (9.4).
14. FCS presence in RX `LENGTH`; RX header translation in sniffer mode (9.2, 9.5).
15. Raw TX while sniffing: required wcid/OMAC, token/TX-free handling (9.6).
16. Firmware ACK of unicast frames to our MAC in sniffer mode (9.7).
17. Licence note carried over from CLAUDE.md: BSD-3-Clause-Clear compatibility with GPL-3.0 for any code derived from these files.

---

## 11. Post-boot and sniffer: verified details

Added for the M3 sniffer step (library `driver/mt792x`, files `src/postboot.c`, `src/rx.c`, `src/mcu.c`). Same clone and conventions as above. This section supersedes the brief section 9 where they differ; section 9 is left as written. Everything here was re-read from the source; places where the implementation deviates from mt76 are marked **[HOST]**, open questions **TODO-VERIFY** with the option that selects the alternative.

### 11.1 Command word and the three header formats

mt76 encodes every command as an `int cmd` (`mt76_connac_mcu.h:1225-1245`): bits 7:0 id, 15:8 ext id, `BIT(16)` QUERY, `BIT(17)` UNI, `BIT(18)` CE. `MCU_EXT_CMD(x)` = id `0xed` + ext id; `MCU_CE_CMD(x)` = id + CE flag; `MCU_UNI_CMD(x)` = id + UNI flag. `mt76_connac2_mcu_fill_message` (`mt76_connac_mcu.c:3545-3618`) builds:

| format | header | fields that differ |
|---|---|---|
| plain `MCU_CMD` (bring-up) | 64 bytes (4.1) | `set_query` 3 (`MCU_Q_NA`), `ext_cid` 0, `ext_cid_ack` 0 |
| `MCU_CE_CMD` | 64 bytes | `cid` = id, `set_query` = 0 if QUERY flag else 1, `ext_cid` 0, `ext_cid_ack` 0 (`mt76_connac_mcu.c:3598-3606`) |
| `MCU_EXT_CMD` | 64 bytes | `cid` 0xed, `ext_cid` = ext id, `set_query` 0/1 as CE, `ext_cid_ack` 1 |
| `MCU_UNI_CMD` | 48 bytes (4.2) | `len` = total − 32, `cid` LE16 at 0x22 (no `pq_id`), `pkt_type` 0xa0, `seq`, `s2d_index` 0, `option` 0x07 (`MCU_CMD_UNI_EXT_ACK`), rest 0 (`mt76_connac_mcu.c:3577-3587`) |

Every command except `FW_SCATTER` consumes one sequence number (1..15, never 0) and goes to TX ring 17. Response matching is by `seq` only (`mcu.c:116-131`); response handling per command (`mt7921/mcu.c:16-92`):

- `MCU_UNI_CMD(DEV_INFO_UPDATE / BSS_INFO_UPDATE / STA_REC_UPDATE / HIF_CTRL / OFFLOAD / SUSPEND)`: event ≥ 36 + 8 bytes, payload `{u8 cid; u8 pad[3]; le32 status}`; `cid` ≠ command id → `-EAGAIN` (keep waiting); result = `status` (0 = success). On a BSS **de**activation, status `0xc0000001` (`MT76_CONNAC_MCU_STATUS_WLAN_FAILURE`) is ignored (`mt76_connac_mcu.h:1963-1980`).
- every other command used here (CE, EXT, UNI `SNIFFER`): event ≥ 36 bytes, result 0, **no status checked**. The library logs the first payload word at INFO anyway (diagnostic only).

Timeout 3 s, no retry (4.5). [HOST] best-effort commands during shutdown use 500 ms and stop at the first failure. [HOST] while waiting, RX ring 2 is drained first (frames are counted as `discarded_in_cmd`, events are matched like on rings 0/4) once post-boot has completed, so the data ring cannot stall during commands.

### 11.2 Sequence after `FW_START` (stage 6 = S24–S31)

mt76 order: `mt7921_run_firmware` (`mt7921/mcu.c:682-704`) → `__mt7921_init_hardware` (`mt7921/init.c:96-104`) → end of `mt7921_init_work` (`mt7921/init.c:167`) → first command of `__mt7921_start` (`mt7921/main.c:232`).

| step | command | cmd | payload (bytes) | awaited | source |
|---|---|---|---|---|---|
| S24 | `GET_NIC_CAPAB` | CE 0x8a, set_query 1 | none | yes | `mt7921/mcu.c:597-667` |
| — | CLC (`EFUSE_ACCESS` query of `MT_EE_HW_TYPE`, CLC tables from the RAM file's `NON_DL` region of type 2, later `SET_CLC`) | — | — | — | **not sent**, 11.3 |
| S25 | `CHIP_CONFIG` "DowngradeTxStreamTemp 105 115" | CE 0xca | 328: `le16 id 0, u8 type 0, u8 resp_type 0, le16 data_size 0, le16 resv 0, char data[320]` (`mt76_connac_mcu.h:1874-1881`) | no | `mt7921/mcu.c:1521-1532`, `mt76_connac_mcu.c:2047-2057` |
| S26 | `FWLOG_2_HOST` | CE 0xc5 | 4: `{1, 0, 0, 0}` | no | `mt7921/mcu.c:669-680,703` |
| S27 | `EFUSE_BUFFER_MODE` | EXT 0x21 | 4: `{buffer_mode 0 = EE_MODE_EFUSE, format 1 = EE_FORMAT_WHOLE, le16 len 0}` | yes | `mt7921/mcu.c:972-985`, `mt76_connac_mcu.h:1193-1202` |
| S28 | `mt7921_mac_init` registers (11.4) | — | — | — | `mt7921/init.c:64-79` |
| S29 | `PROTECT_CTRL` (RTS threshold) | EXT 0x3e | 12: `{prot_idx 1, band 0, rsv[2], le32 0x92b, le32 pkt_thresh 2}` | yes | `mt7921/init.c:80`, `mt76_connac_mcu.c:255-272` |
| S30 | `CHIP_CONFIG` "KeepFullPwr 1" | CE 0xca | 328 as S25 | no | `mt76_connac_mcu.c:2060-2067`; **[HOST]** mt76 sends "KeepFullPwr 0" here (deep sleep on by default, `mt7921/init.c:210-216`); "1" is what mt76 sends for a monitor interface (`mt7921/main.c:611-614`) and what the project needs |
| S31 | `MAC_INIT_CTRL` enable | EXT 0x46 | 4: `{enable 1, band 0, rsv[2]}` | yes | `mt7921/main.c:232`, `mt76_connac_mcu.c:216-230` |

`mt76_eeprom_override` (`mt7921/init.c:96`) is host-only. `set_query` for every CE/EXT command above is 1 (no QUERY flag).

`GET_NIC_CAPAB` reply (after `skb_pull` of the 36-byte event header): `{le16 n_element; u8 rsv[2]}` then `n_element` × `{le32 type; le32 len; u8 data[len]}`. Parsing stops (no error) when fewer than 8 bytes remain for a TLV header or `len` exceeds what remains (`mt7921/mcu.c:620-634`). Types used (`mt76_connac_mcu.h:1471-1492`): `MT_NIC_CAP_HW_VER` 5 and `SW_VER` 6 (not parsed by mt76; the library keeps the first 16 raw bytes for the log), `MAC_ADDR` 7 (used if ≥ 6 bytes), `PHY` 8 (`struct mt7921_phy_cap` 12 bytes, used if ≥ 11: `antenna_mask = BIT(nss) − 1`, `hw_path` bit 0 = 2.4 GHz, bit 1 = 5 GHz; `mt7921/mcu.c:563-595`), `6G` 0x18 (byte 0), `CHIP_CAP` 0x20 (u64 if ≥ 8; bits `mt792x.h:29-36`, e.g. `BIT(3)` `WF_RF_PIN_CTRL_EVT_EN`). A reply shorter than its 4-byte header is an error (`-EINVAL`, `mt7921/mcu.c:613-616`).

### 11.3 What is deliberately not sent, and why

| mt76 action | why skipped | effect if wrong |
|---|---|---|
| `mt7921_load_clc` (`mt7921/mcu.c:442-531`; enabled on PCIe, `mt7921/regd.c:22-29`) | regulatory per-country channel/power tables for TX; the first thing it sends is an `EFUSE_ACCESS` query, the tables go out later as `SET_CLC` (`mt7921/mcu.c:1362-1487`) | none expected for RX; needed before transmitting (M4) |
| TX power, `mt76_connac_mcu_set_rate_txpower` (`mt7921/main.c:244,1294-1308`, `mt76_connac_mcu.c:2188-2352`) | per-rate TX power limits (SKU) | TX only (M4) |
| `mt792x_mac_reset_counters` (`mt7921/main.c:248,489`) | reads (clears) MIB counters | statistics only |
| `ID_RADIO_ON_OFF_CTRL` on start is **sent** (SN3); `WF_RF_PIN_CTRL` is sent only if `CHIP_CAP BIT(3)` (SN4), as mt76 | — | — |

### 11.4 `mt7921_mac_init` registers (S28)

All through the fixed map (1.2); written with read-modify-write as mt76 does (`mt76_rmw/set/clear` always write).

| register (logical → BAR) | operation | source |
|---|---|---|
| `MT_MDP_DCR1` 0x820cd004 → 0x0f004 | rmw `GENMASK(15,3)` = 1536 (0x3000) | `mt7921/init.c:68`, `mt7921/regs.h:16-17` |
| `MT_MDP_DCR0` 0x820cd000 → 0x0f000 | set `BIT(15)` DAMSDU_EN; set `BIT(19)` RX_HDR_TRANS_EN (option `no_rx_hdr_trans` skips the second) | `mt7921/init.c:70-72`, `mt7921/regs.h:12-14` |
| `MT_WTBL_UPDATE` 0x820d4230 → 0x34230, idx 0..19 | rmw(`GENMASK(9,0)`, idx \| `BIT(12)`); poll `BIT(31)` == 0, 5 ms, 10 µs tick; **poll result ignored** | `mt7921/init.c:74-76`, `mt7921/mac.c:19-26`, `mt7921/regs.h:74-79`, `mt792x_regs.h:159-162` |
| per band b = 0, 1 (`mt792x_mac_init_band`, `mt792x_mac.c:289-316`): | | |
| `MT_TMAC_CTCR0` 0x820e40f4/0x820f40f4 → 0x210f4/0xa10f4 | rmw `GENMASK(5,0)` = 0x3f; set `BIT(18)\|BIT(17)` | `mt792x_regs.h:48-51` |
| `MT_WF_RMAC_MIB_TIME0` +0x3c4 → 0x217c4/0xa17c4; `MT_WF_RMAC_MIB_AIRTIME0` +0x380 → 0x21780/0xa1780 | set `BIT(30)` | `mt792x_regs.h:220-259` |
| `MT_MIB_SCR1` 0x820ed004/0x820fd004 → 0x24804/0xa4804 | set `BIT(8)`, then set `BIT(9)` | `mt792x_regs.h:97-102` |
| `MT_DMA_DCR0` 0x820e7000/0x820f7000 → 0x21e00/0xa1e00 | rmw `GENMASK(15,3)` = 1536; clear `BIT(23)` RXD_G5_EN ("hw issues") | `mt792x_regs.h:56-61` |
| `MT_WTBLOFF_TOP_RSCR` +0x008 → 0x23408/0xa3408 | rmw mask 0xc3000000 = 0x03000000 (RCPI mode 0, param 3) | `mt792x_regs.h:64-69` |

Fixed-map check for 0x820cd000: entry #22 (0x820cc000, size 0x1000) matches through the `ofs == size` off-by-one and yields 0x0e000 + 0x1000 = 0x0f000, the same BAR offset as entry #23. Band-1 registers are written although MT7921 has one band, as mt76 does.

### 11.5 Monitor interface and sniffer start (SN1–SN13)

mt76 has no single "monitor start" function; with mac80211 the following callbacks run when a monitor interface comes up. **ASSUMPTION**: the order below (start → add_interface → configure_filter → config(MONITOR) → channel) is mac80211's; mt76 does not fix it.

| step | command | cmd | payload | awaited | source |
|---|---|---|---|---|---|
| SN1 | `SET_CHAN_DOMAIN` | CE 0x0f | 12-byte header `{alpha2[4], bw_2g 0, bw_5g 3, bw_6g 3, pad, n_2ch, n_5ch, n_6ch, pad2}` + per channel `{le16 hw_value, le16 pad, le32 flags}` | no | `mt7921/main.c:236`, `mt76_connac_mcu.c:123-213` |
| SN2 | `SET_RX_PATH` | EXT 0x4e | 76, 11.6 (rx_streams = antenna **mask**) | yes | `mt7921/main.c:240`, `mt7921/mcu.c:912-970` |
| — | TX power | — | — | — | not sent (11.3) |
| SN3 | `ID_RADIO_ON_OFF_CTRL` 1 (LED ctrl enable), then 2 (radio on LED) | EXT 0x05 | 4: `{ctrlid, rsv[3]}` | no | `mt7921/main.c:253-261`, `mt7921/mcu.c:707-718`, `mt7921/mt7921.h:40-42` |
| SN4 | `WF_RF_PIN_CTRL` init, only if `CHIP_CAP BIT(3)` | EXT 0xbd | 2: `{action 0, value 0}`; awaited only if action ≠ 0 | no | `mt7921/main.c:263-266`, `mt7921/mcu.c:1506-1519` |
| SN5 | `DEV_INFO_UPDATE` active | UNI 0x01 | 16: `{omac_idx 0, band_idx 0, le16 pad}` + `{le16 tag 0 (DEV_INFO_ACTIVE), le16 len 12, active 1, link_idx 0, omac_addr[6]}` | yes, status | `mt7921/main.c:311-325`, `mt76_connac_mcu.c:1194-1225,1280-1291` |
| SN6 | `BSS_INFO_UPDATE` basic | UNI 0x02 | 36: `{bss_idx 0, pad[3]}` + `struct mt76_connac_bss_basic_tlv` (32 bytes, `mt76_connac_mcu.h:1517-1545`): tag 0, len 32, active 1, omac 0, hw_bss_idx 0, band 0, `conn_type` LE32 0x00010002 (`CONNECTION_INFRA_AP` for `NL80211_IFTYPE_MONITOR`), conn_state 1, wmm 0, bssid 0, `bmc_tx_wlan_idx` 0, bcn_interval 0, dtim 0, phymode 0, `sta_idx` 0, rest 0 | yes, status | `mt76_connac_mcu.c:1226-1301` |
| SN7 | `mt7921_mac_wtbl_update(19, ADM_COUNT_CLEAR)` | register | — | — | `mt7921/main.c:332-340` |
| SN8 | `SET_RX_FILTER` mode 1 | CE 0x0a | 68: `{rsv[4], mode, rsv2[3], le32 fif, le32 bit_map, bit_op, pad[51]}`; `fif = BIT(31) ENABLE \| BIT(2) FCSFAIL? \| BIT(5) CONTROL? \| BIT(6) OTHER_BSS?` | no | `mt7921/main.c:666-691`, `mt7921/mcu.c:1534-1554`, `mt7921/mt7921.h:10-13` |
| SN9 | `SNIFFER` enable | UNI 0x24 | 12: `{band_idx 0, pad[3]}` + `{le16 tag 0, le16 len 8, enable 1, pad[3]}` | yes | `mt7921/main.c:610`, `mt7921/mcu.c:1185-1213` |
| SN10 | `CHIP_CONFIG` "KeepFullPwr 1" | CE 0xca | 328 | no | `mt7921/main.c:612-614` |
| SN11 | `SET_BSS_ABORT` | CE 0x17 | 4: `{bss_idx 0, pad[3]}` | no | `mt7921/main.c:616-617`, `mt7921/mcu.c:1145,1062-1097` |
| SN12 | `SET_RX_FILTER` mode 2 | CE 0x0a | `bit_op` 2 (`MT7921_FIF_BIT_CLR`), `bit_map` `BIT(11)` `MT_WF_RFCR_DROP_OTHER_BEACON` | no | `mt7921/mcu.c:1149-1151`, `mt792x_regs.h:234` |
| SN13 | tune (11.6) | | | | |

Notes, all FACT unless marked:

- The monitor vif gets index 0 = `omac_idx` 0, band 0, `wmm_idx` 0 (`mt7921/main.c:311-321`). `mt76_connac_mcu_uni_add_dev` is called **before** `wcid.idx` is set to `MT792x_WTBL_RESERVED − idx` = 19 (`mt7921/main.c:323-335`), so `bmc_tx_wlan_idx` and `sta_idx` carry the still-zero wcid index 0. The library reproduces that.
- `hw_bss_idx` = `omac_idx` (0) because it is not > `EXT_BSSID_START` 0x10 (`mt76_connac_mcu.c:1277-1278`, `mt76_connac.h:73,78`).
- The MAC address in `DEV_INFO` is the vif address (`bss_conf->addr`); for a monitor vif that is the hardware address, i.e. the one from `GET_NIC_CAPAB`. The library takes it from there unless the caller passes one.
- `mt792x_init_wiphy` sets `no_active_monitor` and `WANT_MONITOR_VIF` (`mt792x_core.c:824,841`): mt76 does not claim the chip ACKs in monitor mode (9.7 still applies).
- SN1 **[HOST]**: Linux fills the channel list from cfg80211 after regulatory processing (disabled channels left out, `flags` = cfg80211 channel flags, which are not defined in mt76). The library sends mt76's own band tables (`mac80211.c:30-82`: 2.4 GHz 1–14, 5 GHz 36–177; only bands `GET_NIC_CAPAB` reports) with `flags` 0 and `alpha2` "00" (mt7921's world regdomain name, `mt7921/regd.c:15`; `dev->alpha2` is `char[3]`, `mt76.h:1022`). `bw_*` values as in the source. **TODO-VERIFY** whether the firmware uses this for RX at all; option `skip_chan_domain` omits it. Not awaited, so a rejection is invisible.
- Sniffer stop (mt76 order): `SNIFFER` enable 0 (config `CHANGE_MONITOR`, `mt7921/main.c:610`), then `mt76_connac_mcu_uni_add_dev(enable = false)`: **BSS first, then DEV** (`mt76_connac_mcu.c:1282-1299`), both with `active` 0. mt76 would also send "KeepFullPwr 0" when monitor goes away (`mt7921/main.c:612-614`); **[HOST]** not sent. Interface stop (`mt7921_stop`, `mt7921/main.c:284-298` → `mt792x_stop`, `mt792x_core.c:232-252`): `ID_RADIO_ON_OFF_CTRL` 3 (radio off LED, not awaited), then `MAC_INIT_CTRL {enable 0}` (awaited). `mt792x_shutdown` runs these (spec 7.3 step 1) before the register part, only if the bus is alive; state flags are set **before** each enabling command so that a command whose response was lost is still undone.

### 11.6 Which command tunes the radio

The deciding bit is **not** in `GET_NIC_CAPAB`: it is `MT792x_FW_CAP_CNM` (`BIT(7)`, `mt792x.h:27`) of the feature byte that `mt792x_get_offload_capability` reads **from the RAM firmware file** at probe (`mt792x_core.c:861-929`, used in `mt792x_get_mac80211_ops`, `mt792x_core.c:938-976`, stored as `dev->fw_features`, `mt7921/pci.c:338,360`). File layout: after the concatenated region data (`offset` = sum of all region lengths, `mt76_connac2_fw_check`, `mt76_connac_mcu.c:3048-3078`) skip 16 bytes; then `struct mt792x_realease_info {le16 len; u8 pad_len; u8 tag}` whose `len` bytes contain entries of the same header + `len` data + `pad_len` padding; tag 4 (`MT792x_FW_TAG_FEATURE`) holds `struct mt792x_fw_features {u8 segment; u8 data; u8 rsv[14]}`, `data` is the feature byte (`mt792x.h:25-27,72-82`). A malformed block makes mt76 log "Invalid firmware" and use 0. The library parses this in `mt792x_ram_parse` (`fw_features`, `fw_features_status`).

- **Without CNM**: mac80211 emulates channel contexts (`ieee80211_emulate_*`, `mt792x_core.c:962-973`) and retunes through `config(IEEE80211_CONF_CHANGE_CHANNEL)` → `mt76_update_channel` → `mt7921_set_channel` → `MCU_EXT_CMD(CHANNEL_SWITCH)` (`mt7921/main.c:477-499,640-644`, `mac80211.c:1057-1118`), followed by `mt792x_mac_set_timeing` (11.6.1). The sniffer config TLV is never sent.
- **With CNM**: `assign_vif_chanctx` only records the context (`mt792x_core.c:463-480`, `mac80211.c:1043-1055`, no command); a channel change of a monitor vif goes through `mt7921_change_chanctx` → `SNIFFER` enable 1 + `SNIFFER` config (`mt7921/main.c:1411-1436`). No `CHANNEL_SWITCH`.
- In both cases `__mt7921_start` has already sent `SET_RX_PATH` with the current channel (SN2).

`CHANNEL_SWITCH` / `SET_RX_PATH` payload (76 bytes, `mt7921/mcu.c:917-941`), 20 MHz: `control_ch` = channel, `center_ch` = channel (center frequency of a 20 MHz channel), `bw` = `CMD_CBW_20MHZ` = `IEEE80211_STA_RX_BW_20` = 0 (`mt76_connac.h:56-60,379-397`; the enum value 0 is from `include/net/mac80211.h`, not mt76), `tx_streams_num` = popcount(antenna mask), `rx_streams` = mask for `SET_RX_PATH`, popcount for `CHANNEL_SWITCH` (`mt7921/mcu.c:960-961`), `switch_reason` 0 (`CH_SWITCH_NORMAL`: `SET_RX_PATH`, or monitor and not off-channel, `mt7921/mcu.c:948-951`), `band_idx` 0, `center_ch2` 0, `cac_case` 0, `channel_band` = nl80211 band (0 = 2.4 GHz, 1 = 5 GHz; uapi enum), rest 0.

`SNIFFER` config payload (20 bytes, `mt7921/mcu.c:1215-1281`): `{band_idx 0, pad[3]}` + `{le16 tag 1, le16 len 16, u16 aid 0, ch_band (1 = 2.4, 2 = 5, 3 = 6 GHz), bw (0 for 20/40), control_ch, sco (0 when control == center), center_ch, center_ch2 0, drop_err = !(rxfilter & FCSFAIL), pad[3]}`.

Library: option `tune_mode` = `MT792X_TUNE_AUTO` (mt76 rule above), `_CHANNEL_SWITCH`, `_SNIFFER_CONFIG`, `_BOTH`. `mt792x_sniffer_set_channel` measures from the first command sent to the last awaited response.

**MEASURED (file, not chip)**: the maintainer's vendor RAM image (build `20250922193119`, package 3.5.0.1349) contains a valid release-info block (132 bytes between the region data and the region table) **without** a feature tag → feature byte 0 → mt76 would use `CHANNEL_SWITCH`.

#### 11.6.1 `mt792x_mac_set_timeing` after `CHANNEL_SWITCH`

`mt792x_mac.c:35-74`, band 0, `coverage_class` 0, `slottime` 9 (`mt792x_core.c:771`): set `BIT(8)|BIT(9)` (TX/RX disable) in `MT_ARB_SCR` 0x820e3080 → BAR 0x20c80; 1 µs; write `MT_TMAC_CDTR` (0x21090) = 231 \| 48 << 16; `MT_TMAC_ODTR` (0x21094) = 60 \| 28 << 16; `MT_TMAC_ICR0` (0x210a4) = EIFS 360 \| RIFS 2 << 10 \| SIFS (10 at 2.4 GHz, 16 at 5 GHz) << 16 \| slot 9 << 24 (= 0x090a0968 / 0x09100968); rmw `MT_AGG_ACR0` (0x20884) `GENMASK(13,0)` = 0x49 (`MT792x_CFEND_RATE_DEFAULT`, slot < 20); clear `BIT(8)|BIT(9)` in `MT_ARB_SCR` (`mt792x_regs.h:37-46,179-215`). The CNM path never runs it. The library logs `MT_ARB_SCR` before the update: if the firmware leaves `RX_DISABLE` set, reception would depend on this function (**TODO-VERIFY**; option `skip_set_timing`).

### 11.7 RX packet classification (all RX rings)

`mt7921_queue_rx_skb` (`mt7921/mac.c:578-621`) dispatches by `RXD0` bits 31:27 on every ring alike: type 7 with `PKT_FLAG` 1 is re-typed to 8; 0 `TXS` → records of 8 words after 2 header words; 6 `TXRX_NOTIFY` → TX-free (`struct mt76_connac_tx_free {le16 rx_byte_cnt; le16 ctrl; le32 txd}`, `ctrl` bits 9:0 = MSDU count, then LE32 entries; an entry with `BIT(31)` is a wcid pair and does not count, `mt7921/mac.c:476-551`, `mt76_connac.h:233-237`, `mt76_connac2_mac.h:40-48`); 7 → MCU event (11.1, `mt7921/mcu.c:379-414`); 2 and 8 → 802.11 frame (11.8); anything else dropped. The library counts all of these (`mt792x_rx_counters`), returns only frames, logs unsolicited events at DEBUG with id and length (firmware log text of `MCU_EVENT_DBG_MSG` 0x27 type 3, `mt7921/mcu.c:237-261`, printed sanitised).

Rings: events arrived on RX ring 0 in F-006; the library polls rings 0, 4 and 2 and treats them identically. A descriptor without `LAST_SEC0` starts a multi-descriptor packet (`dma.c:523-524`); mt76 reassembles it into one skb for data queues; **[HOST]** the library drops the whole chain and counts it (`frag_packets`, `frag_descs`). With `MAX_RX_LEN` 1536 (11.4) and 2048-byte buffers this is not expected for frames; an event larger than 2048 bytes (e.g. a long `GET_NIC_CAPAB` reply) would be lost the same way and end in a timeout, with a WARN line naming the dropped chain.

TX-free notifications: mt76 uses them to free TX tokens; nothing in the source says the firmware needs them consumed beyond the ring being re-armed. With no TX they are not expected.

### 11.8 RX descriptor of a frame (`mt7921_mac_fill_rx`, `mt7921/mac.c:168-436`; masks `mt76_connac2_mac.h:184-300`)

Rejected (mt76 `-EINVAL`): `RXD1` `BAND_IDX` (b28); `RXD2` `AMSDU_ERR` (b23); `HDR_TRANS` (RXD2 b13) together with `CM` (RXD1 b23); `RXD2` `MAX_LEN_ERROR` (b24); a group that reaches the end of the packet; `HDR_TRANS` on a first fragment (needs a station, `mt7921/mac.c:381-392`). Flagged, not dropped: `FCS_ERR` (RXD1 b27), `ICV_ERR` (b25, mt76 `RX_FLAG_ONLY_MONITOR`), `TKIP_MIC_ERR` (b26), decrypted (`SEC_MODE` 20:16 ≠ 0 and no `CLM`/`CM`).

Channel: `RXD3` bits 15:8 = `CH_FREQ`, converted by `mt792x_get_status_freq_info` (`mt792x.h:444-456`): > 180 → 6 GHz channel `(n − 181) · 4 + 1`; > 14 → 5 GHz; else 2.4 GHz. `RXD3` bits 17:16 = 1 → unicast to us.

Optional groups after the 24-byte base, in this order, each present only if its `RXD1` bit is set; after each group mt76 requires at least one more byte (`(u8 *)rxd - skb->data >= skb->len` → `-EINVAL`):

| group | `RXD1` bit | size | used |
|---|---|---|---|
| 4 | b14 | 16 | word 0: frame control 15:0 |
| 1 | b11 | 16 | IV (decrypted frames only) |
| 2 | b12 | 8 | word 0 = timestamp |
| 3 (P-RXV) | b13 | 8 | word 0: rate (`TX_RATE` 6:0, `NSTS` 9:7, `HT_AD_CODE` b11, `FRAME_MODE` 14:12, `HT_SGI` 16:15, `HT_STBC` 23:22, `TX_MODE` 27:24); word 1: RCPI0..3 in bytes 0..3 |
| 5 (C-RXV) | b15, **only evaluated if group 3** | 72 | RCPI word = group-5 word 0 after skipping 6 words, i.e. byte 24 of the group ("Monitor mode would use RCPI described in GROUP 5 instead") |

Group 5 announced without group 3 is not skipped by mt76 either; the library flags it (`MT792X_RXF_G5_WITHOUT_G3`). `MT_DMA_DCR0_RXD_G5_EN` is cleared in S28, so group 5 should not appear.

Per-chain RSSI = `(RCPI − 220) / 2` (`mt7921/mt7921.h:114`); mt76 computes it in unsigned arithmetic and stores it in an `s8`, which rounds towards −∞ for odd negative differences; the library does the same. Combined RSSI = `mt76_rx_signal(antenna_mask, chains)` (`mac80211.c:1229-1256`): start −128, take the strongest chain, add 3 / 2 / 1 dB when the next chain is within 0 / ≤ 2 / ≤ 6 dB.

Frame start: base RXD + groups + `2 × HDR_OFFSET` (RXD2 15:14) bytes (`mt7921/mac.c:257,380`). If `HDR_TRANS` is clear and `RXD4` bits 1:0 (A-MSDU payload format) ≠ 0, mt76 moves the 802.11 header 2 bytes forward over two padding bytes (`memmove(data + 2, data, hdrlen)`, `mt7921/mac.c:397-400`); the library does the same for data frames (`hdrlen` = 24, +6 with both DS bits, +2 QoS, +4 HT control) and rejects the flag on other frame types.

**FCS**: mt76 never declares `IEEE80211_HW_RX_INCLUDES_FCS` (no occurrence in the tree), so mac80211 treats the delivered frame as **FCS-less**; no code strips 4 bytes. That is the library default (`MT792X_FCS_MT76`). TODO-VERIFY on hardware: every frame is checked whether its last 4 bytes are the CRC-32 of the rest (`MT792X_RXF_FCS_TRAILER`, counter `fcs_trailer`); options `MT792X_FCS_STRIP` and `MT792X_FCS_AUTO`.

**Header translation**: `mt7921_mac_init` enables `MT_MDP_DCR0_RX_HDR_TRANS_EN` globally; whether a frame is translated is controlled per station in the WTBL (`mt76_connac_mcu_sta_update_hdr_trans`, `mt76_connac_mcu.c:549-574`, called from `mt7921_sta_set_decap_offload`, `mt7921/main.c:1207-1227`). mt76 has nothing monitor-specific: it handles translated frames (`RX_FLAG_8023`) and raw ones. **ASSUMPTION**: frames from transmitters without a WTBL entry (everything a sniffer sees) arrive untranslated. The library returns translated frames flagged `MT792X_RXF_HDR_TRANS`, logs the first one at INFO and counts them; option `no_rx_hdr_trans` leaves the global enable off.

`RXD0` bits 15:0 should equal the received length; mt76 does not check it on PCIe; the library flags a difference (`MT792X_RXF_LEN_MISMATCH`). **[HOST]** a frame with fewer than 10 bytes (an ACK without FCS) is rejected.

### 11.9 Polling host specifics

- One `cpu_idx` write per up to 32 re-armed RX ring-2 descriptors, and always before `mt792x_rx_poll` returns 0 (mt76 refills after each NAPI batch, `dma.c:1097`). Rings 0 and 4 are re-armed immediately.
- Liveness every 250 ms inside `mt792x_rx_poll` (the data path itself reads no registers): `MT_CONN_ON_MISC` bits 1:0 must stay 3 (`MT792X_E_FW_DEAD` otherwise), `MT_WFDMA_DUMMY_CR` bit 1 must stay set (`MT792X_E_DMA_REINIT`, risk R6); an all-ones read is `MT792X_E_BUS_DEAD`.
- A ring that ran full is noticed when every armed descriptor is complete at poll time (`rx2_full`).

### 11.10 To be decided on hardware

| # | question | default (mt76) | alternative (option) | log line that tells |
|---|---|---|---|---|
| H1 | Which command retunes in sniffer mode on this firmware | `CHANNEL_SWITCH` (feature byte 0) | `tune_mode` = sniffer config / both | per-frame channel counts in `radiod sniff` vs. the tuned channel; `channel -> N via ...: X us` |
| H2 | FCS included in the frame | no | `fcs_mode` strip / auto | counter `fcs_trailer` vs. `frames` |
| H3 | Header translation in sniffer mode | enabled | `no_rx_hdr_trans` | "first frame with hardware header translation"; `S28 MAC init: MDP_DCR0 ... (RX header translation ON/off)` |
| H4 | `ARB_SCR` RX_DISABLE left set by the firmware | `set_timeing` runs after `CHANNEL_SWITCH` | `skip_set_timing` | "had RX_DISABLE set before the update" |
| H5 | Is `SET_CHAN_DOMAIN` with flags 0 accepted / needed | sent | `skip_chan_domain` | not awaited: only RX behaviour tells |
| H6 | Are `DEV_INFO`/`BSS_INFO` needed for the sniffer | sent | `skip_add_dev` | `MCU DEV_INFO_UPDATE ... result` |
| H7 | Events on ring 0 or 4 after post-boot | both polled | — | `response ring N` in every INFO command line |
| H8 | Firmware log volume | `FWLOG_2_HOST 1` | `skip_fw_log` | event counter 0x27 |
| H9 | Antenna mask when `GET_NIC_CAPAB` has no PHY TLV | 0 (mt76) | `antenna_mask` override | `S24 PHY ...` line |
