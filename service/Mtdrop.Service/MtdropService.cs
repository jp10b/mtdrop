using System.ServiceProcess;
using Mtdrop.Service.Windows;

namespace Mtdrop.Service;

/// <summary>The real implementations wired together.</summary>
public sealed record Host(
    ServiceConfig Config, ILog Log, IProcessRunner Runner, IDriverControl Driver, IWifi Wifi, IFirewall Firewall,
    IWatchdogTask Watchdog, IStateFile State, ICardLock CardLock, INetworkProbe Network, IClock Clock)
{
    public GiveBack GiveBack => new(Config, Driver, Wifi, CardLock, Clock, Log);

    public Session NewSession() => new(Config, Driver, Wifi, Firewall, Watchdog, State, CardLock, Runner, Network, Clock, Log);

    public static Host Create(ServiceConfig config, ILog log)
    {
        var runner = new ProcessRunner(log);
        return new Host(config, log, runner, new DriverControl(runner, log), new Wifi(config, runner), new Firewall(log),
            new WatchdogTask(config, runner, log), new StateFile(config), new CardLock(), new NetworkProbe(), new SystemClock());
    }
}

/// <summary>
/// Windows service "mtdrop": recovery first (gives the card back if a session left it on our driver), then the
/// control pipe. Stop and system shutdown end a running session and give the card back before returning.
/// </summary>
public sealed class MtdropService : ServiceBase
{
    private static readonly TimeSpan StopBudget = TimeSpan.FromMinutes(2);
    private readonly ILog _log;
    private readonly ServiceConfig _config;
    private readonly Session _session;
    private readonly ControlServer _server;
    private readonly CancellationTokenSource _cts = new();
    private Task _serving = Task.CompletedTask;

    public MtdropService(ServiceConfig config, ILog log)
    {
        ServiceName = "mtdrop";
        CanStop = true;
        CanShutdown = true;
        _log = log;
        _config = config;
        _session = Host.Create(config, log).NewSession();
        _server = new ControlServer(_session, log);
    }

    protected override void OnStart(string[] args) => Begin();

    protected override void OnStop() => End();

    protected override void OnShutdown() => End();

    public void StartInConsole() => Begin();

    public void StopInConsole() => End();

    private void Begin()
    {
        _log.Info($"=== mtdrop service starting (pid {Environment.ProcessId}) ===");
        SecureDataDir(_config, _log);
        _ = _session.RecoverAsync();
        _serving = Task.Run(() => _server.RunAsync(_cts.Token));
    }

    private void End()
    {
        _log.Info("mtdrop service stopping");
        _cts.Cancel();
        try { RequestAdditionalTime((int)StopBudget.TotalMilliseconds); }
        catch (InvalidOperationException) { /* console mode: no service control manager */ }
        _session.ShutdownAsync(StopBudget).GetAwaiter().GetResult();
        _serving.Wait(TimeSpan.FromSeconds(5));
        _log.Info("mtdrop service stopped");
    }

    /// <summary>Re-applies the data directory's SYSTEM/Administrators-only ACL (see <see cref="DataDirAcl"/>).</summary>
    public static void SecureDataDir(ServiceConfig config, ILog log)
    {
        try { DataDirAcl.Apply(config.DataDir); }
        catch (Exception e) when (e is UnauthorizedAccessException or IOException or InvalidOperationException)
        {
            log.Warn($"could not secure {config.DataDir}: {e.Message}");
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _cts.Dispose();
        base.Dispose(disposing);
    }
}
