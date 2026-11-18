# ADR-0006: M9 – what to improve first, and what this architecture cannot do

- **Status:** Accepted (2026-11-18, maintainer)
- **Deciders:** maintainer
- **Inputs:** CLAUDE.md §7 (M9 list), §2 (constraints); ADR-0002 (UMDF proxy replaces the vendor driver while we own the chip); ADR-0004 (service sessions); `docs/FINDINGS.md` F-023 (receive 350–590 kB/s), F-025–F-027 (session timing, give-back ~26 s), F-028/F-029 (send 72–87 kB/s); `jedbillyb/airdrop-mt7921` FINDINGS §24, §38–§41 (coexistence on Linux); winawdl DESIGN-NOTES "Sending" (rate ladder: 0.5 → 2.2 MB/s)

## Context

v0.1 works both ways and is robust (M6–M8). CLAUDE.md lists for M9: same-channel coexistence (keep internet), dual-channel research, throughput (`widen`), installer + attestation-signed driver, MT7922. Measured today (FACT):

| | value | source |
|---|---|---|
| Wi-Fi off per session | ~2.5 min (radio up 10–12 s after the click, give-back 26–30 s, of which 15 s is `pnputil /delete-driver`) | F-025, F-026, F-027 |
| receive | 350–590 kB/s | F-023 |
| send | 72–87 kB/s; bursts of ~270 kB/s; 12 % of unicast frames not ACKed, 26 refused | F-028, F-029, `radiod.log` |
| data rate | OFDM 12 Mbit/s for every data frame (OWL's radiotap rate; `radiod` supports CCK/OFDM only) | `driver/mt792x/src/tx.c` |
| channel | fixed 44, advertised sequence `intersect` (only the phone's channel-44 slots) | ADR-0003, F-018 |

## The items, honestly

### A. Throughput (proposed first)

Entirely in our code, measurable in one session, benefits every transfer. Levers, cheapest first:

1. **Data rate.** Unicast data at OFDM 24/36/54 Mbit/s instead of 12 cuts air time per frame 2–4.5×; with the phone on our channel only part of each 16-slot sequence, air time is what limits a send. winawdl went from 0.5 to 2.2 MB/s with a rate ladder. Step 1: a `radiod` option for the unicast data rate and a measurement at 12/24/54 (send and receive). HT/VHT rates need TXD work (later).
2. **What limits the send:** log the phone's advertised sequence as a receiver and our refused/not-ACKed frames per second; check whether TCP or slot overlap dominates.
3. **Channel strategy:** advertise a wider sequence on 44 while *we* have data (what the phone does as a sender, PROTOCOL §2) – `pin` broke receiving on iOS 26 (mt7921 §25), so only during a send and only measured.

### B. Shorter Wi-Fi outage

1. Keep the signed proxy package in the driver store between sessions (skip `pnputil /add-driver` and `/delete-driver /uninstall`, ~15 s of each give-back). Risk to check first: Windows must never pick our package for the card on its own (boot, driver update, hardware rescan). PnP ranks by signature and date; our package is signed by a local certificate, the vendor's by Microsoft (WHQL) – TODO-VERIFY with a rescan while it sits in the store.
2. Start the radio session on the click without the DMA probe when memory is plentiful (probe ~0.5 s – small).

### C. Keeping Wi-Fi internet during a session – **not possible in this architecture**

On Linux, mac80211 lets a monitor interface live next to an associated managed interface on the same chip (mt7921 §38). On Windows the MediaTek driver is a closed WDI miniport and our proxy **replaces** it while we own the chip: during a session nothing in Windows talks to the access point. Keeping the connection would need our own station mode: scanning, association, WPA2/WPA3 (SAE) handshakes, key installation in the firmware, power save towards the AP, an NDIS/WDI miniport so Windows sees a network adapter – and then the time sharing with AWDL that macOS does in its driver (mt7921 §41). That is a full Wi-Fi driver, far beyond everything in M3–M8, and a security-critical one. Recommendation: do not pursue; make the outage short (B) and state the limitation in the README (already in CLAUDE.md §2).

### D. Installer and signed driver for other users – needs the maintainer's decisions

- Driver: today `install-service.ps1` signs the proxy package with a **locally generated certificate trusted machine-wide** (root + trusted publisher, private key deleted). Fine for one developer machine, not something to ship. Other users need Microsoft attestation signing: an EV code-signing certificate (yearly cost) and a Partner Center hardware account. Decision: the maintainer's (money, identity).
- Firmware: the session needs linux-firmware's MT7961 images (F-007); redistribution terms TODO-VERIFY (CLAUDE.md §9 forbids committing them; an installer would download them from the official source).
- A real installer (MSI/MSIX) is straightforward once the two points above are settled.

### E. MT7922 – deferred

No hardware. The library follows mt76 where the two chips share code; support needs someone with the card.

## Decision (proposed)

1. **A first** (rate option + measurements, then the send-path analysis), **then B1** (after verifying the PnP ranking risk).
2. **C is declared out of scope** for this architecture; README states it.
3. **D waits** for the maintainer's decision on signing; MT7922 (E) waits for hardware.

## Consequences

- A and B change `radiod`, `awdld` options and the service only; the user-visible flow stays the same.
- Each experiment gets a FINDINGS entry with predictions first, as before.
