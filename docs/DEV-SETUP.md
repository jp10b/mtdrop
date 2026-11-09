# Development setup

Status: 2026-11-09. Only the protocol tools need a setup today. The driver section is a checklist to complete **before** the first driver experiment; it is not a set of instructions to run yet.

## Protocol tools (safe, no admin rights)

- Python 3.11+ with `pytest` and `cryptography`.
- `python -m pytest tools`

## .NET parts (M2)

- .NET SDK 10. `dotnet build`, `dotnet test`.

## Before the first driver swap (M3, spike S1 in ADR-0002)

Nothing below is automated. Every command that changes driver, boot or network-adapter state is shown together with its rollback command and run only after explicit confirmation by the machine's owner.

- [ ] System Restore point created and named.
- [ ] Vendor Wi-Fi driver package exported to a folder outside the repository.
- [ ] Device Encryption / BitLocker state re-checked; recovery key saved off the machine if enabled.
- [ ] Internet without Wi-Fi available and tested (USB-C Ethernet adapter, or USB tethering from a second phone).
- [ ] Restore procedure rehearsed once: Device Manager → MediaTek adapter → update driver from the exported folder.
- [ ] Toolchain installed: Visual Studio with the C++ workload, Windows SDK and WDK of matching version.

## Things that never go into the repository

Firmware files, raw captures, TLS key logs, private keys, device names, MAC addresses, account identifiers. `.gitignore` blocks the common file types; it does not replace care.
