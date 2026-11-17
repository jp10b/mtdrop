using System.IO;
﻿using System.Drawing;
using System.Drawing.Drawing2D;
using System.Net;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Toolkit.Uwp.Notifications;
using Mtdrop.Protocol;
using WinForms = System.Windows.Forms;

namespace Mtdrop.App;

/// <summary>
/// Tray application: "Receive" toggle with a time window, Accept/Decline prompt window, saved-file toasts.
/// The AWDL adapter (awdl0) exists only while the radio session runs, so "Start receiving" first waits for it
/// (amber icon), within the same time window, and then announces the PC on it.
///   mtdrop.exe [--interface awdl0] [--dir folder] [--minutes 2] [--name shown-name] [--receive]
///   mtdrop.exe --test-prompt     (developer: show one sample prompt and exit)
/// </summary>
public partial class App : System.Windows.Application
{
    /// <summary>How long a consent prompt waits before it declines by itself (also the server-side limit).</summary>
    private static readonly TimeSpan ConsentLimit = TimeSpan.FromSeconds(60);

    private readonly Prompts _prompts = new();
    private readonly DispatcherTimer _window = new();
    private WinForms.NotifyIcon _tray = null!;
    private WinForms.ToolStripMenuItem _toggle = null!;
    private Mutex? _single;
    private ReceiveSession? _session;
    private CancellationTokenSource? _waiting;
    private int _activeTransfers;
    private string _interface = "awdl0";
    private string _destination = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
    private TimeSpan _duration = TimeSpan.FromMinutes(2);
    private string _shownName = Environment.MachineName;

    private static string? Arg(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args.Contains("--test-prompt"))
        {
            _ = RunPromptTestAsync();       // developer switch: no tray, no network session
            return;
        }
        Prompts.InitToasts();
        _single = new Mutex(true, "Local\\mtdrop-tray", out bool first);
        if (!first)
        {
            Shutdown();
            return;
        }
        _interface = Arg(e.Args, "--interface") ?? _interface;
        _destination = Arg(e.Args, "--dir") ?? _destination;
        _shownName = Arg(e.Args, "--name") is { Length: > 0 } name ? name : _shownName;
        if (double.TryParse(Arg(e.Args, "--minutes"), System.Globalization.CultureInfo.InvariantCulture, out double minutes) && minutes is > 0 and <= 60)
            _duration = TimeSpan.FromMinutes(minutes);

        _toggle = new WinForms.ToolStripMenuItem("Start receiving", null, async (_, _) => await ToggleAsync());
        var menu = new WinForms.ContextMenuStrip();
        menu.Items.Add(_toggle);
        menu.Items.Add("Open download folder", null, (_, _) => Shell.RevealInExplorer(_destination));
        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add("Exit", null, async (_, _) => await ExitAsync());
        _tray = new WinForms.NotifyIcon { ContextMenuStrip = menu, Visible = true };
        _tray.DoubleClick += async (_, _) => await ToggleAsync();
        _window.Tick += async (_, _) => await OnWindowElapsedAsync();
        UpdateTray();

        if (e.Args.Contains("--receive")) _ = ToggleAsync();
    }

    /// <summary>
    /// <c>--test-prompt</c>: shows one sample prompt, writes "accepted" or "declined" to
    /// %LOCALAPPDATA%\mtdrop\last-prompt.txt and exits. Lets the window be checked by hand.
    /// </summary>
    private async Task RunPromptTestAsync()
    {
        try
        {
            var ask = new AskRequest
            {
                TransferId = "test",
                SenderName = "Test Phone",
                Files = [new AnnouncedFile("mtdrop-test.png", "public.png", 104177, false)],
                Remote = new IPEndPoint(IPAddress.IPv6Loopback, 0),
            };
            bool accepted = await _prompts.AskAsync(ask, CancellationToken.None, ConsentLimit);
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "mtdrop");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "last-prompt.txt"), accepted ? "accepted" : "declined");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { /* result file is best effort */ }
        finally
        {
            Shutdown();
        }
    }

    private static Icon MakeIcon(Color c)
    {
        using var bmp = new Bitmap(32, 32);
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            using var pen = new Pen(c, 3.2f);
            g.DrawArc(pen, 3, 3, 26, 26, 200, 140);
            g.DrawArc(pen, 9, 9, 14, 14, 200, 140);
            using var brush = new SolidBrush(c);
            g.FillEllipse(brush, 13, 19, 6, 6);
        }
        return Icon.FromHandle(bmp.GetHicon());
    }

    private void UpdateTray()
    {
        bool on = _session is not null;
        bool waiting = _waiting is not null;
        Icon? old = _tray.Icon;
        _tray.Icon = MakeIcon(on ? Color.FromArgb(0, 150, 136) : waiting ? Color.FromArgb(230, 150, 0) : Color.FromArgb(130, 130, 130));
        old?.Dispose();
        _tray.Text = on ? "mtdrop - receiving" : waiting ? $"mtdrop - waiting for {Shorten(_interface, 40)}" : "mtdrop - not receiving";
        _toggle.Text = on || waiting ? "Stop receiving" : $"Start receiving ({_duration.TotalMinutes:0.#} min)";
    }

    /// <summary>NotifyIcon.Text is limited to 127 characters.</summary>
    private static string Shorten(string s, int max) => s.Length <= max ? s : s[..(max - 1)] + "…";

    /// <summary>
    /// Polls until the interface exists and has an IPv6 link-local address, or the token fires (then null).
    /// Windows assigns the address a moment after the adapter appears, so a freshly seen adapter gets 1 s to settle.
    /// </summary>
    private static async Task<InterfaceInfo?> WaitForInterfaceAsync(string wanted, CancellationToken ct)
    {
        bool waited = false;
        try
        {
            while (true)
            {
                if (ReceiveSession.FindInterface(wanted) is { LinkLocal: not null })
                {
                    if (waited) await Task.Delay(TimeSpan.FromSeconds(1), ct);
                    if (ReceiveSession.FindInterface(wanted) is { LinkLocal: not null } nic) return nic;
                }
                waited = true;
                await Task.Delay(TimeSpan.FromMilliseconds(500), ct);
            }
        }
        catch (OperationCanceledException)
        {
            return null;
        }
    }

    private async Task ToggleAsync()
    {
        if (_session is not null || _waiting is not null)
        {
            await StopAsync();
            return;
        }
        // The wait for the adapter counts against the receive window: the PC is never visible longer than chosen.
        var waiting = new CancellationTokenSource(_duration);
        _waiting = waiting;
        DateTime started = DateTime.UtcNow;
        UpdateTray();
        InterfaceInfo? nic = await WaitForInterfaceAsync(_interface, waiting.Token);
        if (!ReferenceEquals(_waiting, waiting)) return;   // stopped (or exited) while waiting
        _waiting = null;
        waiting.Dispose();
        TimeSpan left = _duration - (DateTime.UtcNow - started);
        if (nic is null || left <= TimeSpan.Zero)
        {
            UpdateTray();
            Prompts.ShowMessage("Not receiving",
                $"The network adapter '{_interface}' did not appear within {_duration.TotalMinutes:0.#} min. Is the radio session running?");
            return;
        }
        try
        {
            _session = ReceiveSession.Start(new ReceiveSessionOptions
            {
                Interface = _interface,
                ComputerName = _shownName,
                DestinationDirectory = _destination,
                DataDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "mtdrop"),
                ConsentTimeout = ConsentLimit,
                Consent = async (ask, ct) =>
                {
                    bool accepted = await _prompts.AskAsync(ask, ct, ConsentLimit);
                    if (accepted) Interlocked.Increment(ref _activeTransfers);
                    return accepted;
                },
            });
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.Net.Sockets.SocketException or IOException)
        {
            Prompts.ShowMessage("Cannot start receiving", ex.Message);
            return;
        }
        _session.TransferFinished += r =>
        {
            Interlocked.Decrement(ref _activeTransfers);
            if (r.Succeeded) Prompts.ShowSaved(r);
            else Prompts.ShowFailed(r);
        };
        _window.Interval = left;
        _window.Start();
        UpdateTray();
    }

    private async Task OnWindowElapsedAsync()
    {
        if (Volatile.Read(ref _activeTransfers) > 0)
        {
            _window.Interval = TimeSpan.FromSeconds(10);    // let the running transfer finish
            return;
        }
        await StopAsync();
    }

    private async Task StopAsync()
    {
        CancellationTokenSource? waiting = _waiting;
        _waiting = null;
        waiting?.Cancel();
        waiting?.Dispose();
        _window.Stop();
        ReceiveSession? session = _session;
        _session = null;
        _prompts.DeclineAll();
        UpdateTray();
        if (session is not null) await session.DisposeAsync();
        Interlocked.Exchange(ref _activeTransfers, 0);
    }

    private async Task ExitAsync()
    {
        await StopAsync();
        _tray.Visible = false;
        _tray.Dispose();
        try { ToastNotificationManagerCompat.History.Clear(); } catch (Exception) { /* nothing to clear */ }
        Shutdown();
    }
}
