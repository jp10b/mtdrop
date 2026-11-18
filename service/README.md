# service

The elevated mtdrop service (ADR-0004): runs receive sessions on request of the tray app and always gives the Wi-Fi card back to the MediaTek driver.

| project | what |
|---|---|
| `Mtdrop.Control` | control-pipe protocol and client (`\.\pipe\mtdrop-control`), shared with the tray app |
| `Mtdrop.Service` | `mtdrop-service.exe`: session state machine, give-back, Windows implementations, service host |
| `Mtdrop.Service.Tests` | state machine, give-back and protocol tests against fakes (no hardware, no elevation) |

## What it does

- **Service start** (boot, crash restart): if a session left the card on our proxy driver, give it back; then serve the pipe. Otherwise idle.
- **`start {minutes}`** (1–10): check files, card on the MediaTek driver, `radiod dma-probe`; export the card's saved Wi-Fi networks through the Native Wifi API (refuses to go on if Wi-Fi is connected but nothing could be exported: the adapter comes back with a new interface GUID and no saved networks, F-025); arm the watchdog task, write the state file, put the proxy on the card; start `radiod serve 44 … --mac random` and `awdld -c 44 -S intersect` from the install directory; add temporary firewall rules for the tray app; report `Receiving` once `awdl0` is up.
- **End** (deadline, `stop`, a child exiting, an error, service stop, system shutdown): end `awdld` (radiod follows through its lease), remove the firewall rules, give the card back, then clear the state file and the watchdog task – only once the MediaTek driver is verified back.
- **Watchdog**: one-shot SYSTEM task `mtdrop\watchdog` at deadline + 2 min runs `mtdrop-service.exe --give-back`, in case the service hangs or dies. Children (`radiod`, `awdld`) are in a kill-on-close job, so they die with the service.

The pipe accepts only `start`, `stop` and `status` with fixed fields; programs, arguments and paths all come from the service's own install directory. ACL: SYSTEM and Administrators full control, signed-in (INTERACTIVE) users read/write, network logons denied.

## Command line

```
mtdrop-service.exe --check       read-only: card driver, proxy packages, state file, awdl0, saved networks, required files
mtdrop-service.exe --give-back   give the card back once (what the watchdog task runs); exit 0 = MediaTek driver back
mtdrop-service.exe --console     run the service in a console for debugging (elevated, Ctrl+C stops)
```

## Install

`scripts/install-service.ps1` (M7.2). Without `-Apply` it checks its inputs and prints every change with its undo; nothing is touched. Build the inputs first (unelevated):

```
dotnet publish service\Mtdrop.Service -c Release -o D:\mtdrop-tools\build\service
dotnet publish app\Mtdrop.App -c Release -o D:\mtdrop-tools\build\app
.\scripts\install-service.ps1                 # plan
.\scripts\install-service.ps1 -Apply          # elevated: install or update
.\scripts\install-service.ps1 -Uninstall      # plan of the removal; -Uninstall -Apply removes (-RemoveData: also ProgramData)
```

Layout: `C:\Program Files\mtdrop\` (service, `radiod.exe`, `awdld.exe` + `wintun.dll`, `driver\` with the proxy package signed once at install by a local certificate `CN=mtdrop local driver signing (service)`, `firmware\`, `app\`) and `C:\ProgramData\mtdrop\` (`logs\`, `wifi-profiles\`, `vendor-driver-backup\`, `state\`). The data directory is SYSTEM and Administrators only, not inherited from ProgramData (where every user may create files): the service hands files from it to `schtasks`, the WLAN service and `pnputil` as SYSTEM. Saved networks are kept as numbered files (`000.xml`, ... in preference order, keys encrypted for this machine); the give-back re-adds the ones the adapter came back without, and `scripts/wifi-giveback.ps1` uses them too. The service re-applies that ACL at every start. The install does not touch the Wi-Fi card; the service only checks it at start.

`scripts/wifi-giveback.ps1` stays the manual fallback until the C# give-back has passed hardware sessions (M7.3, M7.4).
