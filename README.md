# mtdrop

**AirDrop-compatible file sharing between an iPhone and a Windows 11 laptop with a built-in MediaTek MT7921 Wi-Fi card.**

The PC shows up in the iPhone's native share sheet, the same way a Mac does, and it can send files back to the iPhone. You don't need an app on the iPhone, a USB dongle, a Linux VM or WSL.

> **Status: working alpha, tested on one machine.** Receiving and sending work end to end on the maintainer's laptop
> (Acer Swift X, MT7921, Windows 11 25H2) with an iPhone 15 Pro on iOS 26.5.2. There is no installer for other people
> yet: you build it from source and install it with a script. Read [Limitations](#limitations) before you try it.

mtdrop is an independent interoperability project. It is not affiliated with, sponsored by or endorsed by Apple Inc. AirDrop, iPhone and Mac are trademarks of Apple Inc. They appear here only to say what mtdrop works with.

## What it does

- **Receive:** click *Receive* in the tray app. Your PC appears in the iPhone's AirDrop sheet. You accept or decline on the PC, and files land in `Downloads`.
- **Send:** right-click files in Explorer and choose *Send to > iPhone (mtdrop)*, or use the tray menu. Nearby iPhones are listed, you pick one, and the owner accepts on the phone.
- **Gives Wi-Fi back by itself:** a session takes over the Wi-Fi card for up to 2 minutes. A Windows service always hands it back to the MediaTek driver afterwards, even if a process crashes or the laptop restarts.

### Measured on the test machine

| | |
|---|---|
| Click on *Send* until the radio is up | ~6 s |
| 5.4 MB photo PC → iPhone, upload | ~4 s (~1.4 MB/s); ~15 s from the click to "Delivered" |
| 1.8 MB photo iPhone → PC | ~350–590 kB/s (measured before the faster data path) |
| End of transfer until Wi-Fi is connected again | ~16 s |

Every number comes from a numbered experiment in [`docs/FINDINGS.md`](docs/FINDINGS.md).

## Limitations

- **No Wi-Fi internet during a session.** mtdrop replaces the Wi-Fi driver while it runs, for at most about 2 minutes. Ethernet and USB tethering are not affected.
- **One chip only: MediaTek MT7921 (PCIe, `PCI\VEN_14C3&DEV_7961`).** Other cards need their own radio backend. See [`docs/PORTING.md`](docs/PORTING.md).
- **"Everyone for 10 Minutes" only.** "Contacts Only" needs an identity signed by Apple, which cannot be created outside Apple devices. This will not change.
- **Bluetooth must be on to send.** An iPhone lists a sender only after a Bluetooth LE signal from it.
- **Developer install.** The driver package is signed with a certificate created on your own machine. Builds need Visual Studio with the WDK, MinGW-w64 and the .NET 10 SDK. Tested only with Secure Boot off (TODO-VERIFY with Secure Boot on).
- **Tested on one laptop and one iPhone.** Apple can change the protocol in any iOS update.
- **Firmware is not included.** mtdrop loads the MT7961 firmware from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git), because the Windows vendor firmware ignores the commands it needs. See [`docs/FIRMWARE.md`](docs/FIRMWARE.md).

## How it works

```
iPhone ──AWDL (802.11)──▶ radiod ──▶ awdld ──▶ airdropd ──▶ tray app
                          MT7921     AWDL       mDNS +       Accept / Decline,
                          via UMDF   + awdl0    HTTPS        Send to iPhone
                          proxy      (Wintun)   (/Discover, /Ask, /Upload)
                                 ▲
                   mtdrop service: swaps the Wi-Fi driver, watchdog, gives Wi-Fi back
```

- **`driver/`**: the radio backend.
  - A small UMDF driver (`proxy/`) binds to the Wi-Fi card and relays register access.
  - A portable chip library (`mt792x/`, ported from the Linux mt76 driver) boots the firmware and handles DMA, raw RX/TX and the channel.
  - `radiod` serves raw 802.11 frames to one client over a named pipe ([`radio_link.h`](driver/radiod/radio_link.h)).
- **`awdld/`**: Apple Wireless Direct Link, based on [OWL](https://github.com/seemoo-lab/owl), with a Wintun `awdl0` adapter for IPv6.
- **`airdropd/`**: the AirDrop protocol in C#: DNS-SD, TLS, `/Discover`, `/Ask`, `/Upload`, and safe archive extraction.
- **`service/`**: the elevated Windows service. It swaps drivers, runs the session state machine, keeps a watchdog task and restores Wi-Fi.
- **`app/`**: the tray app (WPF): Receive, Send, the receiver list and the Accept/Decline prompt.

Details: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/PROTOCOL.md`](docs/PROTOCOL.md). Decisions and their reasons: [`docs/adr/`](docs/adr/).

## Building and installing (developers)

> This swaps the driver of your Wi-Fi card. Before the first install:
> - create a System Restore point;
> - export the MediaTek driver (`scripts/prepare-driver-work.ps1`);
> - make sure you can reach the internet without Wi-Fi.
>
> Every script that changes driver or network state prints its plan and how to undo it. Without `-Apply` it changes nothing.

1. Toolchains:
   - Visual Studio 2026 with the MSVC x64 tools and the Windows Driver Kit;
   - CMake and Ninja;
   - MinGW-w64 GCC (for `awdld`: OWL is GNU C);
   - .NET 10 SDK;
   - Python 3.11+ (tools and tests only).
2. Build, in this order:
   - the proxy driver: `driver/proxy`, see [`docs/DEV-SETUP.md`](docs/DEV-SETUP.md);
   - `radiod`: [`driver/radiod/README.md`](driver/radiod/README.md);
   - `awdld`: [`awdld/README.md`](awdld/README.md);
   - `dotnet publish` of `service/Mtdrop.Service` and `app/Mtdrop.App`.
3. Get the MT7961 firmware from linux-firmware: [`docs/FIRMWARE.md`](docs/FIRMWARE.md).
4. In an elevated PowerShell:
   - `.\scripts\install-service.ps1` shows the plan;
   - `.\scripts\install-service.ps1 -Apply` installs. `-ToolsRoot` and `-FirmwareDir` point it at your build output.
5. Start *mtdrop* from the Start menu. On the iPhone, set AirDrop to *Everyone for 10 Minutes*.

To undo: `.\scripts\install-service.ps1 -Uninstall -Apply`. If the card is ever left on the mtdrop driver, `.\scripts\wifi-giveback.ps1` puts the MediaTek driver back.

Tests that need no hardware:

```
dotnet test mtdrop.sln
python -m pytest tools
cmake -S driver/mt792x -B build/mt792x && cmake --build build/mt792x && ctest --test-dir build/mt792x
```

## Privacy and safety

- Nothing is sent to a receiver before you choose it. Sending needs a confirmation on the iPhone, and receiving needs one on the PC.
- Logs stay on the PC. The service logs can be read only by administrators. Logs never contain receiver names or Apple account data. The AWDL log does contain the random, regularly changing link addresses of nearby devices.
- The repository never contains firmware, packet captures, TLS key logs or private keys (CI refuses such files).
- The Bluetooth wake signal carries random hashes, not your contact details.

## Other Wi-Fi cards

Everything above `radiod` is chip-independent. To bring mtdrop to another card (Intel, Realtek, Qualcomm, MT7922, …), read [`docs/PORTING.md`](docs/PORTING.md). It lists what the radio must be able to do, how to check that on Linux first, and the test ladder this project used for the MT7921.

## Credits

Built on the published work of others:
- [OWL](https://github.com/seemoo-lab/owl) and [OpenDrop](https://github.com/seemoo-lab/opendrop) (Secure Mobile Networking Lab, TU Darmstadt);
- [airdrop-mt7921](https://github.com/jedbillyb/airdrop-mt7921);
- [winawdl](https://github.com/raph559/winawdl);
- [AirDrop-for-Windows](https://github.com/21Suspect/AirDrop-for-Windows);
- [linux-airdrop](https://github.com/Foglio24/linux-airdrop);
- the Linux [mt76](https://github.com/openwrt/mt76) driver.

The origin and licence of every reused file is in [`third_party/NOTICE`](third_party/NOTICE). The survey of prior art is in [`docs/RESEARCH.md`](docs/RESEARCH.md).

## Licence

GPL-3.0-or-later, see [`LICENSE`](LICENSE). Some files carry MIT or BSD-3-Clause-Clear headers because they come from MIT- or BSD-licensed sources. See their headers and `third_party/NOTICE`.
