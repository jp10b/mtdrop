# app

Tray application (WPF, .NET 10). Prototype from M2; receives over AWDL since M6 (F-024).

- Tray icon with **Start receiving (2 min)** / **Stop receiving**, **Open download folder**, **Exit**. Double-click toggles.
- **Start receiving** first waits for the AWDL adapter (`awdl0`, created by `awdld` while the radio session runs): amber icon, "waiting for awdl0". The wait counts against the receive window. Once the adapter has an IPv6 link-local address the icon turns green and the PC is announced on it. If it never appears, a notice says so.
- While receiving it hosts `Mtdrop.Protocol.ReceiveSession` in-process (unelevated).
- Each `/Ask` opens the app's own small always-on-top prompt window (not a toast: a consent decision must not depend on the notification system). It appears at the bottom-right of the primary screen's work area, does not take keyboard focus and is not in the taskbar. Several prompts stack upward and re-flow when one closes.
- The prompt shows the sender name (plain text, one line, ellipsised, labelled "Sender name is not verified"), the file description, **Accept** / **Decline** and a countdown. Only a click on Accept accepts; Decline, closing the window, Escape, 60 s timeout, stopping the receive window or a cancelled request all decline.
- There is no default button, so Enter/Space typed in another app cannot accept. Buttons ignore input (and look disabled) for 500 ms after the prompt appears or moves.
- After a transfer: a toast with **Open folder**, or a notice that nothing was saved. Toasts are informational only; if they fail (unpackaged-app registration, Do Not Disturb) nothing else is affected.
- The receive window ends by itself; a running transfer is allowed to finish first.

```
mtdrop.exe [--interface awdl0] [--dir <folder>] [--minutes 2] [--name <shown name>] [--receive]
mtdrop.exe --test-prompt
```

`--test-prompt` (developer switch) shows one sample prompt ("Test Phone", `mtdrop-test.png`, 104177 bytes) without any tray icon or network session, writes `accepted` or `declined` to `%LOCALAPPDATA%\mtdrop\last-prompt.txt` and exits. Use it to check the window by hand.

`--interface` defaults to `awdl0`. For a local demo without the radio use any adapter with an IPv6 link-local address and `tools/demo/fake_sender.py` on the same interface. `--name` sets the name shown in the iPhone's share sheet (default: the computer name), e.g. for a recording that should not show the real one.

Hardware session (M6, until the elevated service of M7 exists): start `mtdrop.exe --minutes 5` in your own desktop session, click **Start receiving**, then run the session script with `-Airdrop tray` (adds temporary firewall rules for `mtdrop.exe`; the script runs as SYSTEM, where prompts and toasts would be invisible, so it does not start the app itself).

Result toasts use `Microsoft.Toolkit.Uwp.Notifications` (unpackaged desktop apps, no Windows App SDK runtime); every toast call is wrapped so a failure cannot crash or block the app.

Not done: settings UI, history, start with Windows, talking to the elevated service (M7), cancelling the prompt when the sender gives up.
