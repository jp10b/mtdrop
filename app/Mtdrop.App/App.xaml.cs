using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Net;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Toolkit.Uwp.Notifications;
using Mtdrop.Control;
using Mtdrop.Protocol;
using WinForms = System.Windows.Forms;

namespace Mtdrop.App;

/// <summary>
/// Tray application: "Receive" toggle with a time window, Accept/Decline prompt window, saved-file toasts.
/// "Start receiving" asks the mtdrop service (ADR-0004) to run a radio session over the control pipe, follows its
/// state, waits for the AWDL adapter (awdl0) it brings up and then announces the PC on it. The wait counts against the
/// same time window. With --manual the radio session is started by hand instead (scripts/s3-bringup.ps1 -Airdrop tray)
/// and the app only waits for awdl0.
///   mtdrop.exe [--interface awdl0] [--dir folder] [--minutes 2] [--name shown-name] [--receive] [--manual]
///              [--decline-status 403] [--decline-keep-open]     (Decline experiments, docs/PROTOCOL.md §10 item 1)
///   mtdrop.exe --test-prompt     (developer: show one sample prompt and exit)
///   mtdrop.exe --test-notice     (developer: show the card used instead of toasts and exit when it closes)
/// Log: %LOCALAPPDATA%\mtdrop\logs\app.log.
/// </summary>
public partial class App : System.Windows.Application
{
    /// <summary>How long a consent prompt waits before it declines by itself (also the server-side limit).</summary>
    private static readonly TimeSpan ConsentLimit = TimeSpan.FromSeconds(60);
    /// <summary>Status requests that may fail in a row before the app stops following the service session.</summary>
    private const int PollFailureLimit = 3;

    private readonly Prompts _prompts = new();
    private readonly DispatcherTimer _window = new();
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromSeconds(1) };
    private WinForms.NotifyIcon _tray = null!;
    private WinForms.ToolStripMenuItem _toggle = null!;
    private WinForms.ToolStripMenuItem _status = null!;
    private Mutex? _single;
    private ReceiveSession? _session;
    private CancellationTokenSource? _waiting;
    private int _activeTransfers;
    private string _interface = "awdl0";
    private string _destination = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
    private TimeSpan _duration = TimeSpan.FromMinutes(2);
    private string _shownName = Environment.MachineName;
    private bool _manual;
    private int _declineStatus = 403;
    private bool _declineKeepOpen;
    private string? _loggedStatus;
    private SessionState? _loggedServiceState;
    /// <summary>Last status of the service session this app started; null when none is being followed.</summary>
    private ControlResponse? _service;
    private bool _polling;
    private int _pollFailures;
    private string? _lastProblem;

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
        if (e.Args.Contains("--test-notice"))
        {
            // Developer switch: the card shown instead of a toast; exits when it closes (by itself after 30 s).
            var notice = new NoticeWindow("Received from Test Phone", "mtdrop-test.png saved to Downloads",
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"));
            notice.Closed += (_, _) => Shutdown();
            notice.Show();
            notice.PlaceAt(SystemParameters.WorkArea.Right - notice.ActualWidth, SystemParameters.WorkArea.Bottom - notice.ActualHeight);
            return;
        }
        Prompts.InitToasts();
        _single = new Mutex(true, "Local\\mtdrop-tray", out bool first);
        if (!first)
        {
            Shutdown();
            return;
        }
        AppLog.Open();
        _interface = Arg(e.Args, "--interface") ?? _interface;
        _destination = Arg(e.Args, "--dir") ?? _destination;
        _shownName = Arg(e.Args, "--name") is { Length: > 0 } name ? name : _shownName;
        _manual = e.Args.Contains("--manual");
        if (double.TryParse(Arg(e.Args, "--minutes"), System.Globalization.CultureInfo.InvariantCulture, out double minutes) && minutes is > 0 and <= 60)
            _duration = TimeSpan.FromMinutes(minutes);
        if (!_manual && _duration > TimeSpan.FromMinutes(ControlProtocol.MaxMinutes))
            _duration = TimeSpan.FromMinutes(ControlProtocol.MaxMinutes);     // the service's limit (ADR-0004)
        if (int.TryParse(Arg(e.Args, "--decline-status"), out int status) && status is >= 400 and <= 599)
            _declineStatus = status;
        _declineKeepOpen = e.Args.Contains("--decline-keep-open");
        AppLog.Write($"=== mtdrop tray app started (pid {Environment.ProcessId}, {(_manual ? "manual" : "service")} mode, " +
                     $"window {_duration.TotalMinutes:0.#} min, decline {_declineStatus}{(_declineKeepOpen ? " keep-open" : "")}) ===");

        _status = new WinForms.ToolStripMenuItem("Not receiving") { Enabled = false };
        _toggle = new WinForms.ToolStripMenuItem("Start receiving", null, async (_, _) => await ToggleAsync());
        var menu = new WinForms.ContextMenuStrip();
        menu.Items.Add(_status);
        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add(_toggle);
        menu.Items.Add("Open download folder", null, (_, _) => Shell.RevealInExplorer(_destination));
        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add("Exit", null, async (_, _) => await ExitAsync());
        _tray = new WinForms.NotifyIcon { ContextMenuStrip = menu, Visible = true };
        _tray.DoubleClick += async (_, _) => await ToggleAsync();
        _window.Tick += async (_, _) => await OnWindowElapsedAsync();
        _poll.Tick += async (_, _) => await PollServiceAsync();
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

    /// <summary>The service is ending a session (or has not reported Idle yet after Stop): Wi-Fi is on its way back.</summary>
    private bool Restoring => _service is not null && _session is null && _waiting is null;

    private void UpdateTray()
    {
        bool on = _session is not null;
        bool waiting = _waiting is not null;
        bool restoring = Restoring;
        string status =
            on ? "Receiving - visible to nearby iPhones" :
            waiting ? (_service?.Message is { Length: > 0 } m ? Capitalize(m) : $"Waiting for {_interface}") :
            restoring ? "Turning Wi-Fi back on" :
            _lastProblem is not null ? $"Last session: {_lastProblem}" :
            "Not receiving";
        Icon? old = _tray.Icon;
        _tray.Icon = MakeIcon(on ? Color.FromArgb(0, 150, 136) : waiting || restoring ? Color.FromArgb(230, 150, 0) : Color.FromArgb(130, 130, 130));
        old?.Dispose();
        _tray.Text = Shorten($"mtdrop - {(on || waiting || restoring ? status : "not receiving")}", 127);
        _status.Text = Shorten(status, 120);
        if (status != _loggedStatus)
        {
            _loggedStatus = status;
            AppLog.Write($"tray: {(on ? "green" : waiting || restoring ? "amber" : "grey")} - {status}");
        }
        _toggle.Text = restoring ? "Turning Wi-Fi back on..." : on || waiting ? "Stop receiving" : $"Start receiving ({_duration.TotalMinutes:0.#} min)";
        _toggle.Enabled = !restoring;
    }

    private static string Capitalize(string s) => s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];

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
            AppLog.Write("user: Stop receiving");
            await StopAsync();
            return;
        }
        if (_service is not null) return;      // the previous session is still giving the Wi-Fi card back
        AppLog.Write("user: Start receiving");
        _lastProblem = null;
        // The wait for the adapter counts against the receive window: the PC is never visible longer than chosen.
        var waiting = new CancellationTokenSource(_duration);
        _waiting = waiting;
        DateTime started = DateTime.UtcNow;
        UpdateTray();
        if (!_manual && !await StartServiceSessionAsync(waiting))
        {
            if (ReferenceEquals(_waiting, waiting))
            {
                _waiting = null;
                waiting.Dispose();
            }
            UpdateTray();
            return;
        }
        InterfaceInfo? nic = await WaitForInterfaceAsync(_interface, waiting.Token);
        if (!ReferenceEquals(_waiting, waiting)) return;   // stopped, exited or the service session ended meanwhile
        _waiting = null;
        waiting.Dispose();
        TimeSpan left = _duration - (DateTime.UtcNow - started);
        AppLog.Write(nic is null
            ? $"{_interface} did not appear within the window"
            : $"{_interface} ready (index {nic.Index}) {(DateTime.UtcNow - started).TotalSeconds:0.0} s after Start; {left.TotalSeconds:0} s of the window left");
        if (nic is null || left <= TimeSpan.Zero)
        {
            UpdateTray();
            _prompts.ShowMessage("Not receiving", _manual
                ? $"The network adapter '{_interface}' did not appear within {_duration.TotalMinutes:0.#} min. Is the radio session running?"
                : $"The radio did not come up within {_duration.TotalMinutes:0.#} min. The service is turning Wi-Fi back on.");
            if (!_manual) await SendStopAsync();
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
                DeclineStatus = _declineStatus,
                DeclineKeepsConnection = _declineKeepOpen,
                Log = m => AppLog.Write($"receiver: {m}"),
                Consent = async (ask, ct) =>
                {
                    bool accepted = await _prompts.AskAsync(ask, ct, ConsentLimit);
                    if (accepted) Interlocked.Increment(ref _activeTransfers);
                    return accepted;
                },
            });
            AppLog.Write($"receiver: announced on {_interface}, port {_session.Port}");
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.Net.Sockets.SocketException or IOException)
        {
            AppLog.Write($"receiver failed to start: {ex.Message}");
            _prompts.ShowMessage("Cannot start receiving", ex.Message);
            if (!_manual) await SendStopAsync();
            UpdateTray();
            return;
        }
        _session.TransferFinished += r =>
        {
            Interlocked.Decrement(ref _activeTransfers);
            AppLog.Write(r.Succeeded ? $"transfer done: {r.SavedFiles.Count} file(s) saved" : $"transfer failed: {r.Error}");
            if (r.Succeeded) _prompts.ShowSaved(r);
            else _prompts.ShowFailed(r);
        };
        _window.Interval = left;
        _window.Start();
        UpdateTray();
    }

    /// <summary>Asks the service for a radio session. False (with a notice) if it is not available or refuses.</summary>
    private async Task<bool> StartServiceSessionAsync(CancellationTokenSource waiting)
    {
        int minutes = (int)Math.Clamp(Math.Ceiling(_duration.TotalMinutes), ControlProtocol.MinMinutes, ControlProtocol.MaxMinutes);
        ControlResponse r;
        try
        {
            r = await ControlClient.StartAsync(minutes);
        }
        catch (ServiceUnavailableException ex)
        {
            _lastProblem = "service not available";
            AppLog.Write($"service not available: {ex.Message}");
            _prompts.ShowMessage("mtdrop service not available",
                $"{ex.Message} Install it with scripts\\install-service.ps1 (as administrator), or use --manual for a session started by hand.");
            return false;
        }
        catch (IOException ex)
        {
            _lastProblem = ex.Message;
            AppLog.Write($"service start request failed: {ex.Message}");
            _prompts.ShowMessage("Cannot start receiving", ex.Message);
            return false;
        }
        if (!r.Ok)
        {
            _lastProblem = r.Error ?? "the service refused";
            AppLog.Write($"service refused: {_lastProblem}");
            _prompts.ShowMessage("Cannot start receiving", _lastProblem);
            return false;
        }
        AppLog.Write($"service: session started ({minutes} min)");
        _service = r;
        _pollFailures = 0;
        _poll.Start();
        if (!ReferenceEquals(_waiting, waiting))
        {
            await SendStopAsync();          // Stop was clicked while the request was on its way
            return false;
        }
        UpdateTray();
        return true;
    }

    /// <summary>Follows the service session once a second; ends the local receiver when the radio goes away.</summary>
    private async Task PollServiceAsync()
    {
        if (_polling || _service is null) return;
        _polling = true;
        try
        {
            ControlResponse r;
            try
            {
                r = await ControlClient.StatusAsync();
            }
            catch (Exception ex) when (ex is ServiceUnavailableException or IOException or TimeoutException)
            {
                if (++_pollFailures < PollFailureLimit) return;
                AppLog.Write($"service stopped answering: {ex.Message}");
                await StopReceiverAsync();
                EndFollowing("The mtdrop service stopped answering. Its watchdog gives the Wi-Fi card back within a few minutes; " +
                             "if Wi-Fi stays off, run scripts\\wifi-giveback.ps1 as administrator.");
                return;
            }
            _pollFailures = 0;
            _service = r;
            if (r.State != _loggedServiceState)
            {
                _loggedServiceState = r.State;
                AppLog.Write($"service: {r.State}{(r.Message is { Length: > 0 } m ? $" - {m}" : "")}{(r.Error is { Length: > 0 } err ? $" (error: {err})" : "")}");
            }
            if (r.State is SessionState.RestoringWifi or SessionState.Idle)
                await StopReceiverAsync();          // awdl0 goes away with the radio
            if (r.State == SessionState.Idle)
                EndFollowing(r.Error);
            UpdateTray();
        }
        finally
        {
            _polling = false;
        }
    }

    private void EndFollowing(string? error)
    {
        _poll.Stop();
        _service = null;
        _loggedServiceState = null;
        _lastProblem = error;
        UpdateTray();
        if (error is not null) _prompts.ShowMessage("Receiving ended", error);
    }

    private async Task SendStopAsync()
    {
        if (_service is null) return;
        try
        {
            ControlResponse r = await ControlClient.StopAsync();
            _service = r;
        }
        catch (Exception ex) when (ex is ServiceUnavailableException or IOException or TimeoutException)
        {
            // The poll notices; the service and its watchdog give the card back on their own.
        }
        UpdateTray();
    }

    private async Task OnWindowElapsedAsync()
    {
        if (Volatile.Read(ref _activeTransfers) > 0 && (_manual || _service?.State == SessionState.Receiving))
        {
            AppLog.Write("window ended during a transfer; extending by 10 s");
            // Let the running transfer finish. With the service, its own deadline (same window) still ends the radio.
            _window.Interval = TimeSpan.FromSeconds(10);
            return;
        }
        AppLog.Write("window ended");
        await StopAsync();
    }

    /// <summary>User stop or end of the window: stop receiving and ask the service to end the radio session.</summary>
    private async Task StopAsync()
    {
        await StopReceiverAsync();
        await SendStopAsync();
    }

    /// <summary>Stops waiting for awdl0 and the local receiver; the service session (if any) is left alone.</summary>
    private async Task StopReceiverAsync()
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
        await StopAsync();          // the service gives the card back by itself; no need to wait for it
        _poll.Stop();
        _tray.Visible = false;
        _tray.Dispose();
        try { ToastNotificationManagerCompat.History.Clear(); } catch (Exception) { /* nothing to clear */ }
        Shutdown();
    }
}
