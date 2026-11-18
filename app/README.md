# app

Tray application (WPF, .NET 10). Prototype from M2; receives over AWDL since M6 (F-024).

- Tray icon with **Start receiving (2 min)** / **Stop receiving**, **Open download folder**, **Exit**. Double-click toggles.
- **Start receiving** asks the mtdrop service (`service/`, ADR-0004) for a radio session over the control pipe `\.\pipe\mtdrop-control` (`start {minutes}`, 1–10 min), then follows it once a second (`status`). The first menu line shows what the service is doing ("switching the Wi-Fi card to mtdrop", "starting the radio", ...); the icon is amber meanwhile.
- The app then waits for the AWDL adapter (`awdl0`, created by `awdld`). The wait counts against the receive window. Once the adapter has an IPv6 link-local address the icon turns green and the PC is announced on it.
- **Stop receiving**, the end of the window or **Exit** send `stop`; while the service gives the card back the menu shows "Turning Wi-Fi back on..." and Start is disabled. When the service session ends (deadline, error, radio stopped) the receiver stops too; an error is shown as a notice and stays in the first menu line.
- If the service is not installed or not running, a notice says so (install: `scripts/install-service.ps1`).
- While receiving it hosts `Mtdrop.Protocol.ReceiveSession` in-process (unelevated).
- Each `/Ask` opens the app's own small always-on-top prompt window (not a toast: a consent decision must not depend on the notification system). It appears at the bottom-right of the primary screen's work area, does not take keyboard focus and is not in the taskbar. Several prompts stack upward and re-flow when one closes.
- The prompt shows the sender name (plain text, one line, ellipsised, labelled "Sender name is not verified"), the file description, **Accept** / **Decline** and a countdown. Only a click on Accept accepts; Decline, closing the window, Escape, 60 s timeout, stopping the receive window or a cancelled request all decline.
- There is no default button, so Enter/Space typed in another app cannot accept. Buttons ignore input (and look disabled) for 500 ms after the prompt appears or moves.
- After a transfer: a notice with **Open folder**, or a notice that nothing was saved. Notices are Windows toasts when toasts can be seen; when the user has switched notifications off (Settings > System > Notifications, `ToastEnabled = 0`, F-025) or for this app, or a toast call fails, the app shows its own card instead (same look and corner as the prompt, closes after 30 s, **Open folder** / **Close**). Notices are informational only and never block anything.
- The receive window ends by itself; a running transfer is allowed to finish first.

```
mtdrop.exe [--interface awdl0] [--dir <folder>] [--minutes 2] [--name <shown name>] [--receive] [--manual]
           [--decline-status 401] [--decline-keep-open]
mtdrop.exe --test-prompt
mtdrop.exe --test-notice
```

Log: `%LOCALAPPDATA%\mtdrop\logs\app.log` (one older file kept as `app.1.log`, rolled at 1 MB): start options, menu/icon changes, the service states it follows, when `awdl0` was ready, every `/Discover`, `/Ask` (file count and size, the decision and how long it took, the status sent back), `/Upload`, transfer results and notices. Sender names and file names are not written.

`--decline-status` (400–599, default 401) and `--decline-keep-open` (answer without `Connection: close` and keep the connection) are for experiments: `401` + close makes the iPhone show "Declined" (F-026), `403` leaves it on "Waiting" (F-025).

`--test-notice` (developer switch) shows the card used instead of toasts and exits when it closes.

`--test-prompt` (developer switch) shows one sample prompt ("Test Phone", `mtdrop-test.png`, 104177 bytes) without any tray icon or network session, writes `accepted` or `declined` to `%LOCALAPPDATA%\mtdrop\last-prompt.txt` and exits. Use it to check the window by hand.

`--interface` defaults to `awdl0`. For a local demo without the radio use any adapter with an IPv6 link-local address and `tools/demo/fake_sender.py` on the same interface. `--name` sets the name shown in the iPhone's share sheet (default: the computer name), e.g. for a recording that should not show the real one.

`--minutes` is capped at 10 with the service (its limit). With the service the radio ends at the service's deadline, so a transfer still running then is cut off.

Hand-started hardware session (M6 style, without the service): start `mtdrop.exe --manual --minutes 5` in your own desktop session, click **Start receiving** (it only waits for `awdl0`), then run the session script with `-Airdrop tray` (adds temporary firewall rules for `mtdrop.exe`; the script runs as SYSTEM, where prompts and toasts would be invisible, so it does not start the app itself).

Toasts use `Microsoft.Toolkit.Uwp.Notifications` (unpackaged desktop apps, no Windows App SDK runtime); every toast call is wrapped so a failure cannot crash or block the app.

Start with Windows: `scripts/install-service.ps1` adds a Start menu shortcut and (unless `-NoAutostart`) a per-user autostart entry.

Not done: settings UI, history, extending the window for a running transfer, cancelling the prompt when the sender gives up.
