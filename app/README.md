# app

Tray application (WPF, .NET 10). Prototype for M2.

- Tray icon with **Start receiving (2 min)** / **Stop receiving**, **Open download folder**, **Exit**. Double-click toggles.
- While receiving it hosts `Mtdrop.Protocol.ReceiveSession` in-process (unelevated).
- Each `/Ask` raises a toast with **Accept** / **Decline**. Ignoring it for 60 s declines. The sender name is shown as claimed and labelled unverified.
- After a transfer: a toast with **Open folder**, or a notice that nothing was saved.
- The receive window ends by itself; a running transfer is allowed to finish first.

```
mtdrop.exe [--interface awdl0] [--dir <folder>] [--minutes 2] [--receive]
```

`--interface` exists because the AWDL adapter is not there yet. For a local demo use any adapter with an IPv6 link-local address and `tools/demo/fake_sender.py` on the same interface.

Toasts use `Microsoft.Toolkit.Uwp.Notifications` (works for unpackaged desktop apps without the Windows App SDK runtime).

Not done: settings UI, history, start with Windows, talking to the elevated service (M7), cancelling the prompt when the sender gives up.
