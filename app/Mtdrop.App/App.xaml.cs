using System.IO;
﻿using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Toolkit.Uwp.Notifications;
using Mtdrop.Protocol;
using WinForms = System.Windows.Forms;

namespace Mtdrop.App;

/// <summary>
/// Tray application: "Receive" toggle with a time window, Accept/Decline toasts, saved-file toasts.
/// Prototype for M2: the network interface is given on the command line because the AWDL
/// adapter does not exist yet.
///   mtdrop.exe [--interface awdl0] [--dir folder] [--minutes 2] [--receive]
/// </summary>
public partial class App : System.Windows.Application
{
    private readonly Prompts _prompts = new();
    private readonly DispatcherTimer _window = new();
    private WinForms.NotifyIcon _tray = null!;
    private WinForms.ToolStripMenuItem _toggle = null!;
    private Mutex? _single;
    private ReceiveSession? _session;
    private int _activeTransfers;
    private string _interface = "awdl0";
    private string _destination = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
    private TimeSpan _duration = TimeSpan.FromMinutes(2);

    private static string? Arg(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _single = new Mutex(true, "Local\\mtdrop-tray", out bool first);
        if (!first)
        {
            Shutdown();
            return;
        }
        _interface = Arg(e.Args, "--interface") ?? _interface;
        _destination = Arg(e.Args, "--dir") ?? _destination;
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

    private static Icon MakeIcon(bool receiving)
    {
        using var bmp = new Bitmap(32, 32);
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            Color c = receiving ? Color.FromArgb(0, 150, 136) : Color.FromArgb(130, 130, 130);
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
        Icon? old = _tray.Icon;
        _tray.Icon = MakeIcon(on);
        old?.Dispose();
        _tray.Text = on ? "mtdrop - receiving" : "mtdrop - not receiving";
        _toggle.Text = on ? "Stop receiving" : $"Start receiving ({_duration.TotalMinutes:0.#} min)";
    }

    private async Task ToggleAsync()
    {
        if (_session is not null)
        {
            await StopAsync();
            return;
        }
        try
        {
            _session = ReceiveSession.Start(new ReceiveSessionOptions
            {
                Interface = _interface,
                ComputerName = Environment.MachineName,
                DestinationDirectory = _destination,
                DataDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "mtdrop"),
                Consent = async (ask, ct) =>
                {
                    bool accepted = await _prompts.AskAsync(ask, ct);
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
        _window.Interval = _duration;
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
