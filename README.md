# mtdrop

**AirDrop-compatible receiving on Windows 11 for laptops with a built-in MediaTek MT7921 Wi-Fi card.**
The PC is meant to show up in the iPhone's native share sheet, like a Mac does. No app on the iPhone, no dongle, no VM.

> **Status: early development. Nothing here works on Windows yet.**
> What exists today: research, protocol notes, a proof on Linux that the hardware can do it, and the protocol service with tests. The radio driver, which everything depends on, is not started.

mtdrop is an independent interoperability project. It is not affiliated with or endorsed by Apple. AirDrop is a trademark of Apple Inc.

## What has been shown so far

| Step | Result |
|---|---|
| The MT7921 in the target laptop receives a photo from an iPhone 15 Pro (iOS 26.5.2) under Linux | Done – [`docs/FINDINGS.md`](docs/FINDINGS.md) F-001 |
| What iOS 26.5.2 sends, decoded from our own capture | Done – F-002, [`docs/PROTOCOL.md`](docs/PROTOCOL.md) |
| Protocol service on Windows (`airdropd`): discovery records, TLS, `/Discover`, `/Ask`, `/Upload`, safe extraction | Works against tests and against a replay of the real capture – F-003. Not yet tested over the air (needs the radio) |
| Windows radio driver | In progress: a user-mode driver boots the MediaTek firmware on Windows without test-signing (F-004 to F-006). Receives raw frames, including the iPhone's AWDL frames, with ~10 ms channel switches (F-007 to F-009; needs the linux-firmware image). Nothing transmitted yet |
| End-to-end receive on Windows | Not started (M6) |

## Honest limitations

- **"Everyone for 10 Minutes" only.** "Contacts Only" needs an Apple-signed identity that cannot be created outside Apple devices. This is permanent.
- **No Wi-Fi internet while receiving** in the first version. The project's driver takes over the Wi-Fi card for the receive window and gives it back afterwards.
- **Slow.** About 70 kB/s on this chip under Linux: a typical photo takes around half a minute.
- **One chip family.** MT7921 first; MT7922 later. Other vendors need their own radio backend.
- **Fragile by nature.** The protocols are undocumented and Apple can change them in any iOS update.
- Receiving only. Sending to an iPhone is a later milestone.

## How it is meant to work

```
iPhone ──AWDL (802.11)──▶ radio backend (MT7921) ──▶ awdld (AWDL, IPv6 link) ──▶ airdropd (mDNS + HTTPS) ──▶ tray app (Accept / Decline, save)
```

Details: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Decisions and their reasons: [`docs/adr/`](docs/adr/).

## Repository layout

| Path | Content |
|---|---|
| `docs/` | Architecture, protocol notes, prior-art research, lab notebook, decisions |
| `driver/` | Radio driver for MT7921 (not started) |
| `awdld/` | AWDL daemon based on OWL (not started) |
| `airdropd/` | AirDrop protocol service (C#): library, tests, console host |
| `app/` | Tray app prototype: Receive toggle, Accept/Decline toast |
| `service/` | Elevated helper (not started) |
| `tools/` | Capture analysis and reference codecs (Python) |
| `tests/fixtures/` | Synthetic, redacted protocol fixtures only |

## Building and testing

```
dotnet test mtdrop.sln
python -m pip install pytest cryptography
python -m pytest tools
```

## Safety

This project replaces a Wi-Fi driver while it runs. Read [`docs/DEV-SETUP.md`](docs/DEV-SETUP.md) before running anything from `driver/` or `scripts/`. Captures, TLS key logs, firmware and anything identifying a device or person are never committed.

## Credits

Built on the published work of others, in particular [OWL](https://github.com/seemoo-lab/owl) and [OpenDrop](https://github.com/seemoo-lab/opendrop) (Secure Mobile Networking Lab, TU Darmstadt), [airdrop-mt7921](https://github.com/jedbillyb/airdrop-mt7921), [winawdl](https://github.com/raph559/winawdl), [AirDrop-for-Windows](https://github.com/21Suspect/AirDrop-for-Windows), [linux-airdrop](https://github.com/Foglio24/linux-airdrop) and the Linux [mt76](https://github.com/openwrt/mt76) driver. See [`docs/RESEARCH.md`](docs/RESEARCH.md) and [`third_party/NOTICE`](third_party/NOTICE).

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
