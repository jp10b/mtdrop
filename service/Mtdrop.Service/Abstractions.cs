namespace Mtdrop.Service;

// Everything the session and the give-back do to the machine goes through these interfaces, so the state machine
// can be tested without hardware (ADR-0004 decision 4). The Windows implementations live in Windows/.

/// <summary>The Wi-Fi card as Plug and Play sees it now.</summary>
/// <param name="Started">The device is started (DN_STARTED) and has no problem code.</param>
public sealed record CardInfo(string InstanceId, string? Provider, string? InfName, string? Version, bool Started, int Problem)
{
    public override string ToString() => $"{Provider ?? "?"} / {InfName ?? "?"} / {Version ?? "?"}{(Started ? "" : $" (not started, problem {Problem})")}";
}

public interface IDriverControl
{
    /// <summary>The MT7921, or null if it is not present.</summary>
    CardInfo? FindCard();

    /// <summary>Published names (oemNN.inf) of our proxy packages in the driver store.</summary>
    IReadOnlyList<string> OurPackages();

    /// <summary>Adds the proxy package to the driver store (pnputil /add-driver). Throws on failure.</summary>
    void AddProxyPackage(string inf);

    /// <summary>Forces the proxy onto the card (UpdateDriverForPlugAndPlayDevices). Throws on failure or if a restart is needed.</summary>
    void BindProxy(string inf);

    /// <summary>pnputil /delete-driver &lt;published&gt; /uninstall /force; returns pnputil's exit code.</summary>
    int DeletePackage(string published);

    void ScanDevices();

    /// <summary>pnputil /add-driver &lt;inf&gt; /install with the vendor driver backup; returns pnputil's exit code.</summary>
    int InstallVendorDriver(string inf);
}

public interface IWifi
{
    /// <summary>Exports the saved Wi-Fi networks of the card (keys stay encrypted for this machine). Returns how many.</summary>
    /// <exception cref="IOException">The networks could not be read.</exception>
    int ExportProfiles();

    /// <summary>Re-adds exported networks the adapter came back without (it comes back with a new interface GUID).</summary>
    ProfileRestore RestoreMissingProfiles();

    /// <summary>The MT7921's network adapter is up (connected).</summary>
    bool AdapterConnected();

    /// <summary>Read-only one-liner for --check: how many saved networks the card has now.</summary>
    string SavedNetworksSummary();
}

/// <param name="Saved">Networks exported before the session.</param>
/// <param name="Missing">Of those, not on the adapter after the give-back.</param>
public sealed record ProfileRestore(int Saved, int Missing, int Restored, int Failed);

public interface IFirewall
{
    /// <summary>Temporary inbound rules for the receiver (TCP 8771, UDP 5353) for one program.</summary>
    void AllowReceiver(string program);

    /// <summary>Removes the temporary rules; safe to call when there are none.</summary>
    void RemoveReceiverRules();
}

/// <summary>A one-shot scheduled task that runs the give-back even if the service is hung or gone.</summary>
public interface IWatchdogTask
{
    void Arm(DateTime utcWhen);
    void Disarm();
}

/// <summary>Records "the card is ours" on disk before the swap; cleared only once the MediaTek driver is back.</summary>
public interface IStateFile
{
    bool CardTaken { get; }
    void MarkCardTaken();
    void Clear();
}

/// <summary>Machine-wide lock around every swap and give-back (the watchdog task runs in another process).</summary>
public interface ICardLock
{
    IDisposable Acquire(TimeSpan timeout);
}

public interface IChildProcess : IDisposable
{
    string Name { get; }
    bool HasExited { get; }
    int? ExitCode { get; }
    Task<bool> WaitForExitAsync(TimeSpan timeout);
    void Kill();
}

public interface IProcessRunner
{
    /// <summary>Runs a program to completion and returns its exit code and output.</summary>
    (int ExitCode, string Output) Run(string exe, IReadOnlyList<string> args, TimeSpan timeout);

    /// <summary>Starts a long-running child whose output goes to <paramref name="logPath"/>. Children die with the service.</summary>
    IChildProcess Start(string exe, IReadOnlyList<string> args, string logPath, string? workingDirectory = null);

    /// <summary>Kills leftovers of earlier service instances (radiod, awdld from the install directory).</summary>
    void KillStrays(IReadOnlyList<string> exePaths);
}

public interface INetworkProbe
{
    /// <summary>The AWDL adapter (awdl0) exists and is up.</summary>
    bool AwdlUp();
}

public interface IClock
{
    DateTime UtcNow { get; }
    Task Delay(TimeSpan d, CancellationToken ct = default);
    void Sleep(TimeSpan d);
}

public interface ILog
{
    void Info(string message);
    void Warn(string message);
}

public sealed class SystemClock : IClock
{
    public DateTime UtcNow => DateTime.UtcNow;
    public Task Delay(TimeSpan d, CancellationToken ct = default) => Task.Delay(d, ct);
    public void Sleep(TimeSpan d) => Thread.Sleep(d);
}
