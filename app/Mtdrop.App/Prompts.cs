using System.IO;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Toolkit.Uwp.Notifications;
using Microsoft.Win32;
using Mtdrop.Protocol;

namespace Mtdrop.App;

/// <summary>
/// The consent prompt is the app's own always-on-top window (<see cref="PromptWindow"/>), because a consent
/// decision must not depend on the notification system. Non-critical messages (saved / failed / info) go to a
/// Windows toast when toasts are visible, otherwise to the app's own <see cref="NoticeWindow"/> card.
/// A prompt that is closed, ignored or times out counts as a decline.
/// </summary>
public sealed class Prompts
{
    /// <summary>Gap between the card and the screen edge, and between stacked cards (card to card).</summary>
    private const double Edge = 8;
    private const double Gap = 8;

    /// <summary>Open prompt windows, oldest first (= lowest). Touched on the UI thread only.</summary>
    private readonly List<Window> _open = [];

    /// <summary>Hooks the "Open folder" button of the result toast. Optional: failure only loses that button.</summary>
    public static void InitToasts()
    {
        try
        {
            ToastNotificationManagerCompat.OnActivated += e =>
            {
                try
                {
                    ToastArguments args = ToastArguments.Parse(e.Argument);
                    if (args.TryGetValue("action", out string action) && action == "open" && args.TryGetValue("path", out string path))
                        Shell.RevealInExplorer(path);
                }
                catch (Exception) { /* malformed toast arguments */ }
            };
        }
        catch (Exception) { /* toasts unavailable (unpackaged app registration); the app works without them */ }
    }

    public static string Describe(AskRequest ask)
    {
        string size = FormatSize(ask.TotalSize);
        return ask.Files.Count == 1
            ? $"{ask.Files[0].Name} ({size})"
            : $"{ask.Files.Count} files ({size})";
    }

    public static string FormatSize(long bytes) => bytes switch
    {
        <= 0 => "unknown size",
        < 1024 => $"{bytes} B",
        < 1024 * 1024 => $"{bytes / 1024.0:0.#} KB",
        < 1024L * 1024 * 1024 => $"{bytes / (1024.0 * 1024):0.#} MB",
        _ => $"{bytes / (1024.0 * 1024 * 1024):0.##} GB",
    };

    /// <summary>
    /// Shows the prompt and waits for the answer; true only if the user clicked Accept. Timeout, closing the
    /// window, Escape and <see cref="DeclineAll"/> give false. Cancelling <paramref name="ct"/> (shutdown, or the
    /// server's own consent timeout) closes the window and cancels the task, as the toast version did;
    /// <c>AirDropServer.HandleAskAsync</c> treats that as a decline unless the whole session is stopping.
    /// Safe to call from any thread.
    /// </summary>
    public async Task<bool> AskAsync(AskRequest ask, CancellationToken ct, TimeSpan timeout)
    {
        ct.ThrowIfCancellationRequested();
        Dispatcher? ui = System.Windows.Application.Current?.Dispatcher;
        if (ui is null) return false;

        // The answer is a TCS completed from the window's Closed event, so nothing blocks a thread-pool thread.
        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        PromptWindow? window = null;
        _ = ui.InvokeAsync(() =>
        {
            if (tcs.Task.IsCompleted) return;       // cancelled before the UI thread got to it
            try
            {
                window = Open(ask, timeout, tcs);
            }
            catch (Exception)
            {
                tcs.TrySetResult(false);            // cannot show the prompt: decline rather than crash or hang
            }
        });

        // Dispatcher work is FIFO, so the close below always runs after the window was created.
        await using (ct.Register(() =>
        {
            tcs.TrySetCanceled(ct);
            _ = ui.InvokeAsync(() => window?.Close());
        }))
            return await tcs.Task.ConfigureAwait(false);
    }

    private PromptWindow Open(AskRequest ask, TimeSpan timeout, TaskCompletionSource<bool> tcs)
    {
        var w = new PromptWindow(ask, timeout);
        w.Closed += (_, _) =>
        {
            _open.Remove(w);
            Reflow();                               // closing one re-flows the others
            tcs.TrySetResult(w.Accepted);
        };
        w.SizeChanged += (_, _) => Reflow();
        _open.Add(w);
        w.Show();                                   // ShowActivated=false: no focus steal
        Reflow();
        return w;
    }

    /// <summary>Stacks the cards upward from the bottom-right corner of the primary work area (newest on top).</summary>
    private void Reflow()
    {
        Rect area = SystemParameters.WorkArea;      // primary screen without the taskbar, in DIPs
        double pad = PromptWindow.ShadowPad;
        double bottom = area.Bottom - Edge + pad;   // window bottom, so that the card's bottom is Edge above the work area edge
        foreach (Window w in _open)
        {
            double height = w.ActualHeight, width = w.ActualWidth;
            if (height <= 0 || width <= 0) continue;    // not laid out yet; SizeChanged will call us again
            double top = Math.Max(bottom - height, area.Top - pad);
            ((ICard)w).PlaceAt(area.Right - Edge + pad - width, top);
            bottom = top + 2 * pad - Gap;
        }
    }

    /// <summary>Declines every open prompt (receive window stopped). Safe to call from any thread.</summary>
    public void DeclineAll()
    {
        Dispatcher? ui = System.Windows.Application.Current?.Dispatcher;
        if (ui is null) return;
        if (ui.CheckAccess()) CloseAll();
        else _ = ui.InvokeAsync(CloseAll);
    }

    private void CloseAll()
    {
        foreach (PromptWindow w in _open.OfType<PromptWindow>().ToArray()) w.Close();     // notices stay
    }

    // ---- Notices: informational only, never allowed to throw. A Windows toast when toasts can be seen, otherwise
    // the app's own card (toasts switched off for the user, FINDINGS F-025, or the toast call failed). ----

    /// <summary>False when Windows would swallow our toasts: notifications off for the user or for this app.</summary>
    private static bool ToastsVisible()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\PushNotifications");
            if (key?.GetValue("ToastEnabled") is int enabled && enabled == 0) return false;
            return ToastNotificationManagerCompat.CreateToastNotifier().Setting == global::Windows.UI.Notifications.NotificationSetting.Enabled;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <param name="logAs">What app.log records: never the title or detail, which carry the sender's name and file names.</param>
    private void Notify(string title, string detail, string? reveal, Action<ToastContentBuilder>? extend, string group, string logAs)
    {
        if (ToastsVisible())
        {
            try
            {
                var builder = new ToastContentBuilder().AddText(title).AddText(detail);
                extend?.Invoke(builder);
                builder.Show(t => t.Group = group);
                AppLog.Write($"notice (toast): {logAs}");
                return;
            }
            catch (Exception ex)
            {
                AppLog.Write($"toast failed ({ex.GetType().Name}: {ex.Message}); showing the card instead");
            }
        }
        AppLog.Write($"notice (card): {logAs}");
        Dispatcher? ui = System.Windows.Application.Current?.Dispatcher;
        _ = ui?.InvokeAsync(() =>
        {
            try
            {
                var w = new NoticeWindow(title, detail, reveal);
                w.Closed += (_, _) =>
                {
                    _open.Remove(w);
                    Reflow();
                };
                w.SizeChanged += (_, _) => Reflow();
                _open.Add(w);
                w.Show();
                Reflow();
            }
            catch (Exception ex)
            {
                AppLog.Write($"notice card failed: {ex.GetType().Name}: {ex.Message}");
            }
        });
    }

    public void ShowSaved(TransferResult result)
    {
        string first = result.SavedFiles[0];
        string text = result.SavedFiles.Count == 1
            ? Path.GetFileName(first)
            : $"{result.SavedFiles.Count} files";
        Notify($"Received from {result.Ask.SenderName}", $"{text} saved to {Path.GetDirectoryName(first)}", first,
            b => b.AddArgument("action", "open").AddArgument("path", first)
                .AddButton(new ToastButton().SetContent("Open folder").AddArgument("action", "open").AddArgument("path", first)
                    .SetBackgroundActivation()),
            "result", $"saved {result.SavedFiles.Count} file(s)");
    }

    public void ShowFailed(TransferResult result) =>
        Notify($"Transfer from {result.Ask.SenderName} did not complete", "Nothing was saved. The sender may retry automatically.",
            null, null, "result", "transfer did not complete");

    /// <summary>For the app's own messages only: the title is logged.</summary>
    public void ShowMessage(string title, string detail) => Notify(title, detail, null, null, "info", title);
}

public static class Shell
{
    /// <summary>Opens Explorer with the file selected. The path comes from our own save result.</summary>
    public static void RevealInExplorer(string path)
    {
        try
        {
            if (File.Exists(path))
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe") { ArgumentList = { "/select,", path } });
            else if (Directory.Exists(path))
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe") { ArgumentList = { path } });
        }
        catch (System.ComponentModel.Win32Exception) { }
    }
}
