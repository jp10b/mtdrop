# app

Tray application (WPF, .NET 10). Prototype for M2.

- Tray icon with **Start receiving (2 min)** / **Stop receiving**, **Open download folder**, **Exit**. Double-click toggles.
- While receiving it hosts `Mtdrop.Protocol.ReceiveSession` in-process (unelevated).
- Each `/Ask` opens the app's own small always-on-top prompt window (not a toast: a consent decision must not depend on the notification system). It appears at the bottom-right of the primary screen's work area, does not take keyboard focus and is not in the taskbar. Several prompts stack upward and re-flow when one closes.
- The prompt shows the sender name (plain text, one line, ellipsised, labelled "Sender name is not verified"), the file description, **Accept** / **Decline** and a countdown. Only a click on Accept accepts; Decline, closing the window, Escape, 60 s timeout, stopping the receive window or a cancelled request all decline.
- There is no default button, so Enter/Space typed in another app cannot accept. Buttons ignore input (and look disabled) for 500 ms after the prompt appears or moves.
- After a transfer: a toast with **Open folder**, or a notice that nothing was saved. Toasts are informational only; if they fail (unpackaged-app registration, Do Not Disturb) nothing else is affected.
- The receive window ends by itself; a running transfer is allowed to finish first.

```
mtdrop.exe [--interface awdl0] [--dir <folder>] [--minutes 2] [--receive]
mtdrop.exe --test-prompt
```

`--test-prompt` (developer switch) shows one sample prompt ("Test Phone", `mtdrop-test.png`, 104177 bytes) without any tray icon or network session, writes `accepted` or `declined` to `%LOCALAPPDATA%\mtdrop\last-prompt.txt` and exits. Use it to check the window by hand.

`--interface` exists because the AWDL adapter is not there yet. For a local demo use any adapter with an IPv6 link-local address and `tools/demo/fake_sender.py` on the same interface.

Result toasts use `Microsoft.Toolkit.Uwp.Notifications` (unpackaged desktop apps, no Windows App SDK runtime); every toast call is wrapped so a failure cannot crash or block the app.

Not done: settings UI, history, start with Windows, talking to the elevated service (M7), cancelling the prompt when the sender gives up.
