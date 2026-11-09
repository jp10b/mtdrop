using System.IO;
using System.Collections.Concurrent;
using Microsoft.Toolkit.Uwp.Notifications;
using Mtdrop.Protocol;

namespace Mtdrop.App;

/// <summary>
/// Accept/Decline prompts as Windows toast notifications. A prompt that is ignored,
/// dismissed or times out counts as a decline.
/// </summary>
public sealed class Prompts
{
    private readonly ConcurrentDictionary<string, TaskCompletionSource<bool>> _pending = new();

    public Prompts()
    {
        ToastNotificationManagerCompat.OnActivated += e =>
        {
            ToastArguments args = ToastArguments.Parse(e.Argument);
            if (!args.TryGetValue("action", out string action)) return;
            switch (action)
            {
                case "accept" or "decline" when args.TryGetValue("id", out string id):
                    if (_pending.TryRemove(id, out TaskCompletionSource<bool>? tcs)) tcs.TrySetResult(action == "accept");
                    break;
                case "open" when args.TryGetValue("path", out string path):
                    Shell.RevealInExplorer(path);
                    break;
            }
        };
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

    /// <summary>Shows the prompt and waits for the answer. Cancellation (timeout, shutdown) removes the toast.</summary>
    public async Task<bool> AskAsync(AskRequest ask, CancellationToken ct)
    {
        string id = Guid.NewGuid().ToString("N");
        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        new ToastContentBuilder()
            .SetToastScenario(ToastScenario.Reminder)        // stays on screen until answered
            .AddText($"{ask.SenderName} wants to send you a file")
            .AddText(Describe(ask))
            .AddAttributionText("Sender name is not verified")
            .AddButton(new ToastButton().SetContent("Accept").AddArgument("action", "accept").AddArgument("id", id)
                .SetBackgroundActivation())
            .AddButton(new ToastButton().SetContent("Decline").AddArgument("action", "decline").AddArgument("id", id)
                .SetBackgroundActivation())
            .Show(t =>
            {
                t.Tag = id;
                t.Group = "ask";
                t.ExpirationTime = DateTimeOffset.Now.AddMinutes(2);
            });
        try
        {
            await using (ct.Register(() => tcs.TrySetCanceled(ct)))
                return await tcs.Task.ConfigureAwait(false);
        }
        finally
        {
            _pending.TryRemove(id, out _);
            try { ToastNotificationManagerCompat.History.Remove(id, "ask"); } catch (Exception) { /* toast already gone */ }
        }
    }

    public static void ShowSaved(TransferResult result)
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
    }

    public static void ShowFailed(TransferResult result) =>
        new ToastContentBuilder()
            .AddText($"Transfer from {result.Ask.SenderName} did not complete")
            .AddText("Nothing was saved. The sender may retry automatically.")
            .Show(t => t.Group = "result");

    public static void ShowMessage(string title, string detail) =>
        new ToastContentBuilder().AddText(title).AddText(detail).Show(t => t.Group = "info");

    public void DeclineAll()
    {
        foreach (string id in _pending.Keys)
            if (_pending.TryRemove(id, out TaskCompletionSource<bool>? tcs)) tcs.TrySetResult(false);
    }
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
