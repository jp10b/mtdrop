using System.IO;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Toolkit.Uwp.Notifications;
using Mtdrop.Protocol;

namespace Mtdrop.App;

/// <summary>
/// The consent prompt is the app's own always-on-top window (<see cref="PromptWindow"/>), because a consent
/// decision must not depend on the notification system. Toasts are used only for non-critical messages
/// (saved / failed / info) and every toast call is allowed to fail silently.
/// A prompt that is closed, ignored or times out counts as a decline.
/// </summary>
public sealed class Prompts
{
    /// <summary>Gap between the card and the screen edge, and between stacked cards (card to card).</summary>
    private const double Edge = 8;
    private const double Gap = 8;

    /// <summary>Open prompt windows, oldest first (= lowest). Touched on the UI thread only.</summary>
    private readonly List<PromptWindow> _open = [];

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
        foreach (PromptWindow w in _open)
        {
            double height = w.ActualHeight, width = w.ActualWidth;
            if (height <= 0 || width <= 0) continue;    // not laid out yet; SizeChanged will call us again
            double top = Math.Max(bottom - height, area.Top - pad);
            w.PlaceAt(area.Right - Edge + pad - width, top);
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
        foreach (PromptWindow w in _open.ToArray()) w.Close();
    }

    // ---- Toasts: informational only, never allowed to throw. ----

    private static void TryToast(Action show)
    {
        try { show(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"toast failed: {ex.Message}"); }
    }

    public static void ShowSaved(TransferResult result) => TryToast(() =>
    {
        string first = result.SavedFiles[0];
        string text = result.SavedFiles.Count == 1
            ? Path.GetFileName(first)
            : $"{result.SavedFiles.Count} files";
        var builder = new ToastContentBuilder()
            .AddText($"Received from {result.Ask.SenderName}")
            .AddText($"{text} saved to {Path.GetDirectoryName(first)}")
            .AddArgument("action", "open").AddArgument("path", first)
            .AddButton(new ToastButton().SetContent("Open folder").AddArgument("action", "open").AddArgument("path", first)
                .SetBackgroundActivation());
        builder.Show(t => t.Group = "result");
    });

    public static void ShowFailed(TransferResult result) => TryToast(() =>
        new ToastContentBuilder()
            .AddText($"Transfer from {result.Ask.SenderName} did not complete")
            .AddText("Nothing was saved. The sender may retry automatically.")
            .Show(t => t.Group = "result"));

    public static void ShowMessage(string title, string detail) => TryToast(() =>
        new ToastContentBuilder().AddText(title).AddText(detail).Show(t => t.Group = "info"));
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
