# driver

Radio driver for MT7921, following [`docs/adr/0002-driver-strategy.md`](../docs/adr/0002-driver-strategy.md).

| Path | Content | State |
|---|---|---|
| `proxy/` | UMDF 2 driver that binds to the PCI function, maps BAR0 and relays batched register reads/writes to an elevated client. No DMA, no interrupts. Derived from the winawdl register proxy (MIT) | Builds. **Never installed yet** |
| `mt792x/` | Portable chip library (firmware download, MCU, DMA, sniffer, channel, TX) | Not started |

Building: see [`docs/DEV-SETUP.md`](../docs/DEV-SETUP.md). The build output is not signed and the build installs nothing.

Installing the proxy on the Wi-Fi card replaces the vendor driver for as long as it is installed (no Wi-Fi). It is done only with scripts that print the rollback first and ask for confirmation.
