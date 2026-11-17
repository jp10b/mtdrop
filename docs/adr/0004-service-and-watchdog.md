# ADR-0004: Receive sessions run by an elevated service, with a watchdog that always restores Wi-Fi

- **Status:** Accepted (2026-11-17, maintainer; session start allowed for signed-in users, decision 2)
- **Deciders:** maintainer
- **Inputs:** CLAUDE.md §6 (driver switching), §7 (M7), §9 (safety); `docs/ARCHITECTURE.md` (control plane, session flow); ADR-0001 (C# for `app` and `service`); ADR-0002 (UMDF proxy + user-mode radio, SYSTEM for SeLockMemoryPrivilege, F-005); ADR-0003 (`radiod serve` lease, `awdld`); `scripts/s3-bringup.ps1` and `scripts/wifi-giveback.ps1` (the swap and give-back used in every hardware session since F-004); `docs/FINDINGS.md` F-011 (system hang), F-022–F-024 (end-to-end receive)

## Context

M6 works end to end, but a receive session still means: an elevated PowerShell, `run-as-system.ps1`, `s3-bringup.ps1` with a dozen arguments, the tray app started separately, and a firewall rule added by the script. The user story (CLAUDE.md §1) is one click on **Start receiving**.

What a session needs today (FACT, from `s3-bringup.ps1`):

1. Preconditions: the card is on the MediaTek driver, the vendor driver backup exists, firmware files exist, `radiod dma-probe` finds a DMA block (else: too little free RAM).
2. Saved Wi-Fi networks exported (the adapter can come back without them).
3. The proxy driver package catalogued (Inf2Cat) and signed with a local certificate trusted machine-wide (private key deleted at once) – today on **every** session.
4. A watchdog process that gives the card back after N seconds whatever happens.
5. `pnputil /add-driver` + `UpdateDriverForPlugAndPlayDevices` → the proxy owns the card, Wi-Fi is down.
6. `radiod serve 44 <patch> <ram> --max-seconds N --mac random` as SYSTEM (SeLockMemoryPrivilege), `awdld -c 44 -S intersect` (creates `awdl0` through Wintun, needs admin).
7. Temporary inbound firewall rules for the receiver (TCP 8771, UDP 5353): `awdl0` is an unidentified public network.
8. Give-back: `pnputil /delete-driver /uninstall /force` for our package, `/scan-devices`, wait for the MediaTek driver, reinstall it from the backup if it does not come back, re-add missing Wi-Fi profiles, wait for the adapter to connect.

Gaps for normal use: nothing restores Wi-Fi after a reboot or power loss mid-session (the watchdog process dies with the machine and the proxy stays bound); the tray app cannot start anything; the binaries run as SYSTEM from `D:\mtdrop-tools`, which the user account can write (any program running as the user could replace `radiod.exe` and get SYSTEM).

## Options

### 1. Who runs the session

| | A: Windows service (LocalSystem) | B: scheduled task per session (today) | C: tray app elevated by UAC |
|---|---|---|---|
| One click, no UAC prompt | yes | no (needs elevation to register the task) | no (prompt every time) |
| Survives the tray app crashing | yes | yes | no – the elevated app *is* the session |
| Boot-time recovery | yes (automatic start) | needs a second task | no |
| SeLockMemoryPrivilege | LocalSystem has it | SYSTEM has it | admin token lacks it (F-005) |
| Attack surface | a pipe that any allowed user can call | none permanent | none permanent |

### 2. Orchestration code

| | C# port of the session and give-back logic | service shells out to the PowerShell scripts |
|---|---|---|
| Tested on hardware | not yet (port) | ~20 sessions |
| Cancellation, status, timeouts | natural | awkward (parse output, kill trees) |
| Dependencies | none beyond .NET | PowerShell + execution policy + PnP cmdlets |
| Unit-testable | yes, behind interfaces | no |

### 3. Watchdog

| layer | protects against |
|---|---|
| service timer (deadline) | normal end, user Stop, tray app gone |
| `radiod --max-seconds` (deadline + margin) and the radio lease (ADR-0003) | the service losing track of `radiod`; `awdld` dying |
| one-shot scheduled task at deadline + 2 min running `mtdrop-service.exe --give-back` | the service process hung or killed |
| give-back at every service start (automatic start, recovery action "restart") | reboot, power loss, service crash |
| `scripts/wifi-giveback.ps1` by hand, vendor driver backup, restore point | all of the above failing |

## Decision

1. **A Windows service, `mtdrop` (option 1A), LocalSystem, automatic (delayed) start, recovery: restart after 5 s.** On every start it first runs the **give-back if the proxy owns the card** (boot and crash recovery), then waits for requests. It is idle otherwise (no radio activity, no open ports).
2. **Control pipe `\\.\pipe\mtdrop-control`**, ACL: SYSTEM and Administrators full, **INTERACTIVE** (signed-in desktop users) read/write; network logons denied. Requests are JSON, one per message, fixed vocabulary: `start {minutes}` (1–10), `stop`, `status`. **No paths, arguments, frames or names from the client** – every program, argument and file the service uses comes from its own install directory. A local signed-in user can therefore take Wi-Fi down for at most 10 minutes per request; that is the accepted cost of a one-click receive (the same user can switch Wi-Fi off in the quick settings anyway).
3. **Install layout (admin-only writable):** `C:\Program Files\mtdrop\` holds `mtdrop-service.exe`, `radiod.exe`, `awdld.exe`, `wintun.dll`, the tray app, and `driver\` (the proxy package, **catalogued and signed once at install**, private key deleted). `C:\ProgramData\mtdrop\` (SYSTEM + Administrators only) holds logs, the exported Wi-Fi profiles and the session state file. Firmware and the vendor driver backup are copied at install from where the maintainer has them (`mtdrop-private`); neither is redistributed (CLAUDE.md §9). Nothing the service executes lives in a user-writable directory.
4. **Orchestration in C# (option 2, port)**, behind small interfaces (`IDriverControl`, `IProcessRunner`, `IFirewall`, `IClock`) so the state machine is unit-tested without hardware. `wifi-giveback.ps1` stays as the manual fallback and is not changed. The PowerShell session script stays for experiments.
5. **Session state machine:** `Idle → Preparing` (preconditions, DMA probe, profiles export) `→ Swapping` (watchdog task armed *before* the swap) `→ Starting` (`radiod serve`, `awdld`, firewall rules for the tray app) `→ Receiving` (`awdl0` up) `→ Stopping` (`awdld` and `radiod` end; give-back; rules and watchdog task removed) `→ Idle`. Any error, any child exiting early, or the deadline → `Stopping`. A state file in `ProgramData` records "card taken" before the swap and is cleared only after the MediaTek driver is verified back.
6. **The tray app stays unelevated** and keeps the receiver (`airdropd` in-process, ADR-0001): **Start receiving** sends `start`, shows the service state (preparing / Wi-Fi off / receiving / restoring Wi-Fi), waits for `awdl0` as now; **Stop** sends `stop`. If the service is not installed, the app says so instead of failing silently.
7. **Install/uninstall by script** (`scripts/install-service.ps1`, elevated, prints every change and its undo first): copy files, sign the driver package, register the service and its recovery actions, set the directory ACLs. `-Uninstall` stops and deletes the service, runs the give-back, removes the files, the watchdog task, the firewall rules and the signing certificate. A real installer is M9.

## Consequences

- Wi-Fi comes back after a crash, a kill of any process, or a reboot mid-session, without the user doing anything (M7 acceptance).
- One more always-running SYSTEM process (idle) and one pipe reachable by signed-in users. The pipe accepts only three commands; the service is the only place that touches drivers.
- The trusted local signing certificate stays installed between sessions (as today). Its private key never exists after install, so it cannot sign anything else.
- Binaries move from `D:\mtdrop-tools` to `Program Files` for normal use; development builds are copied in by re-running the install script.
- The give-back is ported from PowerShell to C#. Until the port has passed hardware sessions, every M7 session keeps `wifi-giveback.ps1` as the documented fallback.

## Steps and gates

| step | what | gate |
|---|---|---|
| M7.1 | `service/` C# project: state machine, pipe server, give-back and swap via SetupAPI/CfgMgr/pnputil, watchdog task; unit tests with fakes | tests pass offline |
| M7.2 | `install-service.ps1` (+ `-Uninstall`); tray app talks to the service | install, `status`, uninstall on the maintainer's machine – no radio session yet |
| M7.3 | hardware: one click on Start receiving → photo received → Wi-Fi back | as F-024, started from the tray only |
| M7.4 | robustness: kill `awdld`, kill `radiod`, kill the service, reboot – each mid-session | Wi-Fi back each time without manual steps (M7 acceptance) |

## Open points

- Whether `pnputil /delete-driver` of our package is still the best way back when the package stays signed in `Program Files` (alternative: `UpdateDriverForPlugAndPlayDevices` with the vendor INF). Keep the current method first, it is the tested one.
- Fast Startup / hibernation mid-session: does the proxy come back bound on resume? TODO-VERIFY in M7.4.
- Multiple signed-in users (fast user switching): the second user's tray app would see the session as busy. Not handled beyond that in v0.1.
