using Mtdrop.Control;

namespace Mtdrop.Service;

/// <summary>A failure with a message meant for the user (shown by the tray app).</summary>
public sealed class SessionException(string message) : Exception(message);

/// <summary>
/// The receive session state machine (ADR-0004 decision 5): Idle → Preparing → TakingCard → Starting → Receiving →
/// RestoringWifi → Idle. Any error, a child exiting, the deadline or Stop leads to RestoringWifi; the card is given
/// back whenever it was taken. One session at a time.
/// </summary>
public sealed class Session(
    ServiceConfig config,
    IDriverControl driver,
    IWifi wifi,
    IFirewall firewall,
    IWatchdogTask watchdog,
    IStateFile stateFile,
    ICardLock cardLock,
    IProcessRunner runner,
    INetworkProbe network,
    IClock clock,
    ILog log)
{
    private readonly GiveBack _giveBack = new(config, driver, wifi, cardLock, clock, log);
    private readonly object _gate = new();
    private SessionState _state = SessionState.Idle;
    private string? _message;
    private string? _lastError;
    private DateTime _deadline;
    private CancellationTokenSource? _cts;
    private Task _run = Task.CompletedTask;
    private bool _leaveCard;

    public static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(500);
    /// <summary>Wait after a child ended by itself before the give-back, in case the system is shutting down.</summary>
    public static readonly TimeSpan ShutdownGrace = TimeSpan.FromSeconds(3);

    /// <summary>The running session or recovery (completed when idle).</summary>
    public Task Completion { get { lock (_gate) return _run; } }

    public ControlResponse Status()
    {
        lock (_gate)
        {
            int left = _state is SessionState.Idle or SessionState.RestoringWifi
                ? 0
                : (int)Math.Max(0, Math.Ceiling((_deadline - clock.UtcNow).TotalSeconds));
            return new ControlResponse(true, _state, left, _message, _lastError);
        }
    }

    public ControlResponse Start(int minutes)
    {
        if (minutes is < ControlProtocol.MinMinutes or > ControlProtocol.MaxMinutes)
            return Status() with { Ok = false, Error = $"minutes must be {ControlProtocol.MinMinutes}..{ControlProtocol.MaxMinutes}" };
        lock (_gate)
        {
            if (_state != SessionState.Idle)
                return new ControlResponse(false, _state, 0, _message, "a receive session is already running");
            _state = SessionState.Preparing;
            _message = "checking";
            _lastError = null;
            _deadline = clock.UtcNow.AddMinutes(minutes);
            var cts = new CancellationTokenSource();
            _cts = cts;
            _run = Task.Run(() => RunAsync(cts.Token));
        }
        log.Info($"session: start requested for {minutes} min");
        return Status();
    }

    public ControlResponse Stop()
    {
        lock (_gate)
        {
            if (_cts is { IsCancellationRequested: false } cts)
            {
                log.Info("session: stop requested");
                cts.Cancel();
            }
        }
        return Status();
    }

    /// <summary>
    /// At service start (boot, crash restart): if a session left the card on our driver, give it back
    /// (ADR-0004 decision 1). Start requests are refused until this has finished.
    /// </summary>
    public Task RecoverAsync()
    {
        lock (_gate)
        {
            if (_state != SessionState.Idle) return _run;
            _state = SessionState.RestoringWifi;
            _message = "checking the Wi-Fi card";
            _run = Task.Run(Recover);
            return _run;
        }
    }

    /// <summary>Service stop: end the session and give the card back before returning.</summary>
    public async Task ShutdownAsync(TimeSpan timeout)
    {
        Stop();
        Task run = Completion;
        await Task.WhenAny(run, Task.Delay(timeout));
    }

    /// <summary>
    /// System shutdown or restart: stop the radio but leave the card, the state file and the watchdog task as they
    /// are. Windows gives services only seconds here, so a give-back (about 25 s) would be cut off half way, and a
    /// give-back that finishes while WLAN AutoConfig is already stopping cannot re-add the saved networks but would
    /// clear the state file. The give-back runs in full after the next boot instead: service recovery
    /// (<see cref="RecoverAsync"/>) or the watchdog task, whichever comes first (FINDINGS F-027).
    /// </summary>
    public async Task ShutdownForSystemAsync(TimeSpan timeout)
    {
        lock (_gate) _leaveCard = true;
        await ShutdownAsync(timeout);
    }

    private void Set(SessionState state, string? message)
    {
        lock (_gate)
        {
            _state = state;
            _message = message;
        }
        log.Info($"session: {state}{(message is null ? "" : $" - {message}")}");
    }

    private void Fail(string error)
    {
        lock (_gate) _lastError ??= error;
        log.Warn($"session: {error}");
    }

    private void Recover()
    {
        try
        {
            runner.KillStrays([config.Radiod, config.Awdld]);
            firewall.RemoveReceiverRules();
            CardInfo? card = driver.FindCard();
            if (stateFile.CardTaken || _giveBack.IsProxyBound(card))
            {
                log.Warn($"recovery: the card was left on our driver ({card?.ToString() ?? "card not found"}); giving it back");
                if (_giveBack.Run())
                {
                    stateFile.Clear();
                    watchdog.Disarm();
                }
                else
                {
                    Fail("Wi-Fi could not be restored automatically. Run scripts\\wifi-giveback.ps1 as administrator.");
                }
            }
            else
            {
                watchdog.Disarm();
                log.Info($"recovery: nothing to do (card: {card?.ToString() ?? "not found"})");
            }
        }
        catch (Exception e)
        {
            Fail($"recovery failed: {e.Message}");
        }
        finally
        {
            Set(SessionState.Idle, null);
        }
    }

    private async Task RunAsync(CancellationToken ct)
    {
        bool armed = false, taken = false, childEnded = false;
        IChildProcess? radiod = null, awdld = null;
        try
        {
            // ---- Preparing: nothing on the machine changes before all of these hold --------------------------
            foreach (string f in config.RequiredFiles)
                if (!File.Exists(f)) throw new SessionException($"missing {Path.GetFileName(f)} - reinstall mtdrop");
            CardInfo card = driver.FindCard() ?? throw new SessionException("the MT7921 Wi-Fi card was not found");
            if (!_giveBack.IsVendorBound(card))
                throw new SessionException($"the Wi-Fi card is not on the MediaTek driver ({card})");
            log.Info($"session: card driver before: {card}");
            ct.ThrowIfCancellationRequested();

            Set(SessionState.Preparing, "reserving memory for the radio");
            (int probe, string probeOut) = runner.Run(config.Radiod, ["dma-probe"], TimeSpan.FromSeconds(60));
            foreach (string line in probeOut.Split('\n', StringSplitOptions.RemoveEmptyEntries)) log.Info($"  dma-probe: {line.TrimEnd()}");
            if (probe != 0) throw new SessionException("not enough free memory for the radio - close large programs and try again");
            // Every swap gives the adapter a new interface GUID without saved networks (F-025): without the export,
            // Wi-Fi would not reconnect by itself afterwards. Nothing has changed on the machine yet.
            int exported;
            try { exported = wifi.ExportProfiles(); }
            catch (IOException e) { throw new SessionException($"could not save the Wi-Fi networks ({e.Message}) - not switching the card"); }
            log.Info($"session: saved Wi-Fi networks exported: {exported}");
            if (exported == 0 && wifi.AdapterConnected())
                throw new SessionException("Wi-Fi is connected but its saved network could not be exported - not switching the card, Wi-Fi would not come back by itself");
            ct.ThrowIfCancellationRequested();

            // ---- TakingCard: watchdog first, then the swap -------------------------------------------------
            Set(SessionState.TakingCard, "switching the Wi-Fi card to mtdrop (Wi-Fi goes off)");
            DateTime deadline;
            lock (_gate) deadline = _deadline;
            watchdog.Arm(deadline + config.WatchdogGrace);
            armed = true;
            using (cardLock.Acquire(TimeSpan.FromMinutes(1)))
            {
                stateFile.MarkCardTaken();
                taken = true;
                driver.AddProxyPackage(config.ProxyInf);
                driver.BindProxy(config.ProxyInf);
            }
            log.Info($"session: card driver during the session: {driver.FindCard()?.ToString() ?? "card not found"}");

            // ---- Starting: radio server, AWDL daemon, firewall for the receiver ----------------------------
            Set(SessionState.Starting, "starting the radio");
            await clock.Delay(TimeSpan.FromSeconds(2), ct);
            int seconds = (int)Math.Clamp(Math.Ceiling((deadline - clock.UtcNow + config.RadiodMargin).TotalSeconds), 5, 900);
            Directory.CreateDirectory(config.LogDir);
            radiod = runner.Start(config.Radiod,
                ["--live-log", Path.Combine(config.LogDir, "radiod-live.log"), "serve", $"{ServiceConfig.Channel}",
                 config.FirmwarePatch, config.FirmwareRam, "--max-seconds", $"{seconds}", "--mac", "random", "--wait-client", "60"],
                Path.Combine(config.LogDir, "radiod.log"), config.InstallDir);
            awdld = runner.Start(config.Awdld, ["-c", $"{ServiceConfig.Channel}", "-S", "intersect"],
                Path.Combine(config.LogDir, "awdld.log"), config.InstallDir);
            firewall.AllowReceiver(config.TrayApp);
            Set(SessionState.Starting, "waiting for the AWDL adapter");

            // ---- Receiving until the deadline, Stop, or a child ending -------------------------------------
            bool warnedGone = false;
            while (true)
            {
                if (radiod.HasExited) { childEnded = true; throw new SessionException($"the radio stopped (exit code {radiod.ExitCode})"); }
                if (awdld.HasExited) { childEnded = true; throw new SessionException($"the AWDL daemon stopped (exit code {awdld.ExitCode})"); }
                bool up = network.AwdlUp();
                SessionState now = Status().State;
                if (now == SessionState.Starting && up)
                    Set(SessionState.Receiving, "visible to nearby iPhones");
                else if (now == SessionState.Receiving && !up && !warnedGone)
                {
                    log.Warn("session: the awdl0 adapter is gone while awdld still runs");
                    warnedGone = true;
                }
                if (clock.UtcNow >= deadline)
                {
                    log.Info("session: time window ended");
                    break;
                }
                await clock.Delay(PollInterval, ct);
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested)
        {
            log.Info("session: stopped on request");
        }
        catch (SessionException e)
        {
            Fail(e.Message);
        }
        catch (Exception e)
        {
            Fail($"unexpected error: {e.Message}");
        }
        finally
        {
            if (taken || radiod is not null || awdld is not null)
                Set(SessionState.RestoringWifi, "turning Wi-Fi back on");
            await StopChildrenAsync(radiod, awdld);
            Safe(firewall.RemoveReceiverRules, "removing the firewall rules");
            // At a restart Windows ends radiod (console shutdown event) about a second before the service hears of the
            // shutdown (F-027 T5); give OnShutdown that moment before starting a give-back that would be cut off.
            if (taken && childEnded) await clock.Delay(ShutdownGrace, CancellationToken.None);
            bool leave;
            lock (_gate) leave = _leaveCard;
            if (taken && leave)
            {
                log.Warn("session: system shutdown - the card stays on our driver; it is given back after the next boot");
            }
            else if (taken)
            {
                bool back = false;
                try { back = _giveBack.Run(); }
                catch (Exception e) { log.Warn($"give-back threw: {e.Message}"); }
                if (back)
                {
                    Safe(stateFile.Clear, "clearing the state file");
                    Safe(watchdog.Disarm, "removing the watchdog task");
                }
                else
                {
                    Fail("Wi-Fi could not be restored automatically; the watchdog will retry in a few minutes. " +
                         "If it stays off, run scripts\\wifi-giveback.ps1 as administrator.");
                }
            }
            else if (armed)
            {
                Safe(watchdog.Disarm, "removing the watchdog task");
            }
            lock (_gate)
            {
                _cts?.Dispose();
                _cts = null;
            }
            Set(SessionState.Idle, null);
        }
    }

    private async Task StopChildrenAsync(IChildProcess? radiod, IChildProcess? awdld)
    {
        // awdld first: radiod then sees its client go, stops transmitting and shuts the chip down in order
        // (lease, ADR-0003). Killing radiod is the last resort; the proxy then replays its stop list.
        if (awdld is not null)
        {
            if (!awdld.HasExited) awdld.Kill();
            await awdld.WaitForExitAsync(TimeSpan.FromSeconds(5));
            awdld.Dispose();
        }
        if (radiod is not null)
        {
            if (!await radiod.WaitForExitAsync(TimeSpan.FromSeconds(15)))
            {
                log.Warn("session: radiod did not end within 15 s; killing it");
                radiod.Kill();
                await radiod.WaitForExitAsync(TimeSpan.FromSeconds(5));
            }
            log.Info($"session: radiod exit code {radiod.ExitCode?.ToString() ?? "?"}");
            radiod.Dispose();
        }
    }

    private void Safe(Action action, string what)
    {
        try { action(); }
        catch (Exception e) { log.Warn($"session: {what} failed: {e.Message}"); }
    }
}
