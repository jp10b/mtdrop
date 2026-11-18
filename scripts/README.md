# scripts

Setup, build and run scripts (PowerShell). Any script that changes driver or adapter state must print its rollback command and ask for confirmation.

| script | what | changes the machine |
|---|---|---|
| `install-service.ps1` | installs / removes the mtdrop service (M7.2, ADR-0004). Without `-Apply` it only prints the plan, each change with its undo | only with `-Apply` (elevated) |
| `wifi-giveback.ps1` | gives the Wi-Fi card back to the MediaTek driver; safe any time | yes (elevated) |
| `s3-bringup.ps1` | hand-started hardware session as SYSTEM (via `run-as-system.ps1`) | yes, needs `-IUnderstandWifiGoesDown` |
