# Architecture

Status: 2026-11-09. Reflects the *proposed* ADR-0001 and ADR-0002; it changes if they are not accepted.

## Components

```
 iPhone (share sheet, "Everyone for 10 Minutes")
        │ 802.11 frames (AWDL)
        ▼
 ┌──────────────────────────────────────────────┐
 │ MT7921 (PCIe) + MediaTek firmware            │
 └───────────────┬──────────────────────────────┘
                 │ registers, interrupts, DMA
 ┌───────────────▼──────────────────────────────┐  elevated
 │ driver/proxy     thin UMDF driver            │  (only while "Receive" is on)
 │ driver/mt792x    chip library (C, portable)  │
 │ radiod           hosts the chip library      │
 └───────────────┬──────────────────────────────┘
                 │ radio interface (RX / TX / channel / stats)
 ┌───────────────▼──────────────────────────────┐  elevated
 │ awdld            OWL core + Windows backend  │
 │                  Wintun adapter "awdl0"      │
 └───────────────┬──────────────────────────────┘
                 │ IPv6 link-local on awdl0
 ┌───────────────▼──────────────────────────────┐  unelevated
 │ airdropd         mDNS responder, TLS/HTTP,   │
 │ (library)        dvzip + cpio, safe save     │
 │ app              tray icon, toast, settings  │
 └──────────────────────────────────────────────┘
 ┌──────────────────────────────────────────────┐  elevated, always installed
 │ service          driver swap, watchdog that  │
 │                  always gives Wi-Fi back     │
 └──────────────────────────────────────────────┘
```

| Component | Language | Responsibility | Trust |
|---|---|---|---|
| `driver/proxy` | C (UMDF 2) | Bind to the PCI device, map registers, deliver interrupts, replay "stop DMA" on handle close | Elevated; tiny |
| `driver/mt792x` | C | Firmware download, MCU commands, DMA rings, sniffer mode, channel, raw TX. Talks to hardware only through a HAL | No OS dependencies |
| `radiod` | C | Hosts the chip library, exposes the radio interface | Elevated |
| `awdld` | C | AWDL sync, election, channel sequence, peers, data frames; `awdl0` adapter | Elevated |
| `airdropd` | C# | Service discovery, `/Discover`, `/Ask`, `/Upload`, extraction | **Unelevated**; parses untrusted network input |
| `app` | C# | "Receive" toggle, Accept/Decline toast, save location, history | Unelevated |
| `service` | C# | Swap vendor driver ↔ ours, start/stop `radiod` and `awdld`, watchdog | Elevated |

## Interfaces

1. **HAL** (`driver/mt792x` ↔ backend): `reg_read32`, `reg_write32`, `dma_alloc`, `irq_wait`, `sleep`. Lets the same chip code run over a UMDF proxy or a thin KMDF driver (ADR-0002).
2. **Radio interface** (`awdld` ↔ `radiod`): `open/close` with a lease, `get_caps`, `set_mac`, `set_channel`, `rx` (frame + per-frame channel, RSSI, timestamp, FCS status), `tx` (rate, ACK wanted, retries → honest status), `set_rx_filter`, `get_stats`. Chip-family independent; this is where another chipset plugs in.
3. **Network** (`awdld` ↔ `airdropd`): ordinary sockets on the `awdl0` adapter, selected by interface index.
4. **Control** (`app` ↔ `service`): local named pipe; commands "start receive (duration)", "stop", "status". The service never accepts file paths or frames from the app.

## Receive session lifecycle

1. User clicks **Receive**. `app` asks `service` to start a session with a time limit (default 2 minutes).
2. `service` swaps the driver (the vendor driver backup was made at install time), starts `radiod` and `awdld`, arms the watchdog.
3. `awdld` creates `awdl0`. `airdropd` starts announcing on it.
4. The phone lists the PC, the user taps it, `airdropd` raises the Accept/Decline toast, then saves the file.
5. On timeout, on "stop", or when any process dies: `service` stops DMA, removes our driver from the device, rescans, and verifies Wi-Fi is back. The watchdog and a boot-time task do the same if `service` itself is gone.

## Design rules

- **The network-facing parser runs unelevated.** Elevated parts never parse HTTP, plists or archives.
- **Every bring-up step has a hardware-observable check.** "No error returned" is not evidence.
- **Per-frame truth.** The radio reports the channel each frame arrived on; nothing trusts a cached "current channel".
- **Giving Wi-Fi back has priority over everything else**, including finishing a transfer.
- **No sender identifiers in logs** (record data, pseudonym, push token).
- Firmware is loaded from the vendor driver package already on the machine; it is never shipped or committed.

## Deliberately not here

Contacts-Only mode, Wi-Fi Aware, internet over Wi-Fi during receive (first version), sending (M8).
