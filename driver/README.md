# driver

Radio backend for the MT7921, following [`docs/adr/0002-driver-strategy.md`](../docs/adr/0002-driver-strategy.md). To port it to another card, see [`docs/PORTING.md`](../docs/PORTING.md).

| Path | Content |
|---|---|
| `proxy/` | UMDF 2 driver. It binds to the PCI function, maps BAR0, relays batched register reads and writes to one elevated client, and replays a stop list when that client's handle closes. It has no DMA and no interrupts of its own. Derived from the winawdl register proxy (MIT). |
| `mt792x/` | Portable chip library ported from Linux mt76 (BSD-3-Clause-Clear): firmware download, MCU, DMA rings, sniffer, channel, raw TX, TX status, station records. Builds and tests on Windows and Linux without hardware. |
| `radiod/` | The radio server: Windows HAL (proxy + locked DMA pages), hardware test commands (`bringup`, `sniff`, `txtest`, `acktest`), and `serve`, which speaks the radio link ([`radio_link.h`](radiod/radio_link.h)) to awdld. |
| `tools/` | Small hardware probes used during bring-up. |

Building: see [`docs/DEV-SETUP.md`](../docs/DEV-SETUP.md) and the READMEs of the subdirectories. The build does not sign and does not install anything. `scripts/install-service.ps1` installs and signs with a local certificate.

Installing the proxy on the Wi-Fi card replaces the vendor driver while it is installed, so there is no Wi-Fi during that time. The mtdrop service does this only for a session and always gives the card back.
