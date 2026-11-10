# Development setup

Status: 2026-11-09. Only the protocol tools need a setup today. The driver section is a checklist to complete **before** the first driver experiment; it is not a set of instructions to run yet.

## Protocol tools (safe, no admin rights)

- Python 3.11+ with `pytest` and `cryptography`.
- `python -m pytest tools`

## .NET parts (M2)

- .NET SDK 10. `dotnet test mtdrop.sln`.

## Before the first driver swap (M3, spike S1 in ADR-0002)

Nothing below is automated. Every command that changes driver, boot or network-adapter state is shown together with its rollback command and run only after explicit confirmation by the machine's owner.

- [x] System Restore point created and named. (`scripts/prepare-driver-work.ps1`, 2026-11-10: "mtdrop: before driver work")
- [x] Vendor Wi-Fi driver package exported to a folder outside the repository. (same script, 2026-11-10: `oem12.inf`, version 3.5.0.1349, 21 files)
- [ ] Device Encryption / BitLocker state re-checked; recovery key saved off the machine if enabled.
- [ ] Internet without Wi-Fi available and tested (USB-C Ethernet adapter, or USB tethering from a second phone).
- [ ] Restore procedure rehearsed once: Device Manager → MediaTek adapter → update driver from the exported folder.
- [x] Toolchain installed (2026-11-10): Visual Studio 2026 (v18.10) with the MSVC x64 tools, the "Windows Driver Kit" component (`Component.Microsoft.Windows.DriverKit`) and the Spectre-mitigated libraries; WDK from NuGet (`Microsoft.Windows.WDK.x64` 10.0.28000.2526 plus the `Microsoft.Windows.SDK.CPP` packages it pulls in) restored to `D:\mtdrop-tools\packages`.

### Building the proxy driver

```powershell
& "C:\Program Files\Microsoft Visual Studio8\Community\MSBuild\Current\Binmd64\MSBuild.exe" `
  driver\proxy\mtdrop_proxy.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo /v:m
```

- Restore the packages once: `nuget.exe install Microsoft.Windows.WDK.x64 -Version 10.0.28000.2526 -OutputDirectory D:\mtdrop-tools\packages`. Another location: `/p:WdkPkgRoot=<folder>\`.
- `dotnet build` does not work with the WDK.
- The build does not sign, create certificates or install anything. Output: `driver\proxyd\Release\` (git-ignored).

## Things that never go into the repository

Firmware files, raw captures, TLS key logs, private keys, device names, MAC addresses, account identifiers. `.gitignore` blocks the common file types; it does not replace care.
