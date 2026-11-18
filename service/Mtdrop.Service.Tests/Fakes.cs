using Mtdrop.Service;

namespace Mtdrop.Service.Tests;

/// <summary>Time that only moves when the code under test waits: a 2-minute session runs in milliseconds.</summary>
public sealed class FakeClock : IClock
{
    public DateTime UtcNow { get; private set; } = new(2026, 11, 17, 20, 0, 0, DateTimeKind.Utc);

    public async Task Delay(TimeSpan d, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        lock (this) UtcNow += d;
        await Task.Yield();
        ct.ThrowIfCancellationRequested();
    }

    public void Sleep(TimeSpan d)
    {
        lock (this) UtcNow += d;
    }
}

public sealed class ListLog : ILog
{
    public List<string> Lines { get; } = [];
    public void Info(string message) { lock (Lines) Lines.Add(message); }
    public void Warn(string message) { lock (Lines) Lines.Add("WARN " + message); }
    public bool Has(string part) { lock (Lines) return Lines.Any(l => l.Contains(part, StringComparison.Ordinal)); }
}

/// <summary>A card that is on the MediaTek driver, or on our proxy, or on nothing (after a failed give-back).</summary>
public sealed class FakeDriver(List<string> calls) : IDriverControl
{
    public enum Binding { Vendor, Proxy, None }

    public Binding Bound { get; set; } = Binding.Vendor;
    public bool PackageInStore { get; set; }
    public bool Present { get; set; } = true;
    public bool VendorReturnsAfterDelete { get; set; } = true;
    public bool BackupInstallWorks { get; set; } = true;
    public bool FailBind { get; set; }

    public CardInfo? FindCard() => !Present ? null : Bound switch
    {
        Binding.Vendor => new CardInfo(@"PCI\VEN_14C3&DEV_7961&SUBSYS_X\4&1", "MediaTek, Inc.", "oem10.inf", "3.5.0.1349", true, 0),
        Binding.Proxy => new CardInfo(@"PCI\VEN_14C3&DEV_7961&SUBSYS_X\4&1", "mtdrop", "oem99.inf", "1.0", true, 0),
        _ => new CardInfo(@"PCI\VEN_14C3&DEV_7961&SUBSYS_X\4&1", null, null, null, false, 28),
    };

    public IReadOnlyList<string> OurPackages() => PackageInStore ? ["oem99.inf"] : [];

    public void AddProxyPackage(string inf)
    {
        calls.Add("driver:add");
        PackageInStore = true;
    }

    public void BindProxy(string inf)
    {
        calls.Add("driver:bind");
        if (FailBind) throw new SessionException("Windows refused to put the mtdrop driver on the card (test)");
        Bound = Binding.Proxy;
    }

    public int DeletePackage(string published)
    {
        calls.Add($"driver:delete {published}");
        PackageInStore = false;
        if (Bound == Binding.Proxy) Bound = VendorReturnsAfterDelete ? Binding.Vendor : Binding.None;
        return 0;
    }

    public void ScanDevices() => calls.Add("driver:scan");

    public int InstallVendorDriver(string inf)
    {
        calls.Add("driver:install-vendor");
        if (BackupInstallWorks) Bound = Binding.Vendor;
        return 0;
    }
}

public sealed class FakeWifi(List<string> calls) : IWifi
{
    public int Exported { get; set; } = 3;
    public bool ExportFails { get; set; }
    public bool Connected { get; set; } = true;

    public int ExportProfiles()
    {
        calls.Add("wifi:export");
        if (ExportFails) throw new IOException("WlanGetProfileList failed (test)");
        return Exported;
    }

    public ProfileRestore RestoreMissingProfiles() { calls.Add("wifi:restore"); return new ProfileRestore(Exported, 0, 0, 0); }
    public bool AdapterConnected() => Connected;
    public string SavedNetworksSummary() => $"{Exported} on the card";
}

public sealed class FakeFirewall(List<string> calls) : IFirewall
{
    public string? AllowedProgram { get; private set; }
    public void AllowReceiver(string program) { calls.Add("firewall:allow"); AllowedProgram = program; }
    public void RemoveReceiverRules() { calls.Add("firewall:remove"); AllowedProgram = null; }
}

public sealed class FakeWatchdog(List<string> calls) : IWatchdogTask
{
    public DateTime? ArmedFor { get; private set; }
    public void Arm(DateTime utcWhen) { calls.Add("watchdog:arm"); ArmedFor = utcWhen; }
    public void Disarm() { calls.Add("watchdog:disarm"); ArmedFor = null; }
}

public sealed class FakeState(List<string> calls) : IStateFile
{
    public bool CardTaken { get; set; }
    public void MarkCardTaken() { calls.Add("state:taken"); CardTaken = true; }
    public void Clear() { calls.Add("state:clear"); CardTaken = false; }
}

public sealed class FakeLock : ICardLock
{
    public int Held { get; private set; }
    public IDisposable Acquire(TimeSpan timeout)
    {
        Held++;
        return new Releaser(this);
    }

    private sealed class Releaser(FakeLock owner) : IDisposable
    {
        public void Dispose() => owner.Held--;
    }
}

public sealed class FakeChild(string name) : IChildProcess
{
    public string Name { get; } = name;
    public bool HasExited { get; set; }
    public int? ExitCode { get; set; }
    public bool Killed { get; private set; }
    /// <summary>radiod ends by itself once its client (awdld) is gone, as the lease does.</summary>
    public bool EndsWhenWaitedFor { get; set; } = true;

    public Task<bool> WaitForExitAsync(TimeSpan timeout)
    {
        if (!HasExited && EndsWhenWaitedFor) { HasExited = true; ExitCode ??= 0; }
        return Task.FromResult(HasExited);
    }

    public void Kill() { Killed = true; HasExited = true; ExitCode ??= -1; }
    public void Dispose() { }
}

public sealed class FakeRunner(List<string> calls) : IProcessRunner
{
    public int DmaProbeExit { get; set; }
    public List<(string Exe, IReadOnlyList<string> Args)> Started { get; } = [];
    public Dictionary<string, FakeChild> Children { get; } = [];

    public (int ExitCode, string Output) Run(string exe, IReadOnlyList<string> args, TimeSpan timeout)
    {
        calls.Add($"run:{Path.GetFileName(exe)} {string.Join(' ', args)}");
        return args is ["dma-probe"] ? (DmaProbeExit, "dma-probe: test\n") : (0, "");
    }

    public IChildProcess Start(string exe, IReadOnlyList<string> args, string logPath, string? workingDirectory = null)
    {
        string name = Path.GetFileNameWithoutExtension(exe);
        calls.Add($"start:{name}");
        Started.Add((exe, args));
        var child = new FakeChild(name);
        Children[name] = child;
        return child;
    }

    public void KillStrays(IReadOnlyList<string> exePaths) => calls.Add("kill-strays");
}

public sealed class FakeNetwork : INetworkProbe
{
    public int Calls { get; private set; }
    /// <summary>awdl0 appears on this call (0 = never).</summary>
    public int UpOnCall { get; set; } = 3;
    public Action<int>? OnCall { get; set; }

    public bool AwdlUp()
    {
        Calls++;
        OnCall?.Invoke(Calls);
        return UpOnCall > 0 && Calls >= UpOnCall;
    }
}

/// <summary>A session wired to fakes, with an install directory that has every required file.</summary>
public sealed class Rig : IDisposable
{
    public List<string> Calls { get; } = [];
    public string Root { get; } = Directory.CreateTempSubdirectory("mtdrop-svc-test-").FullName;
    public ServiceConfig Config { get; }
    public FakeDriver Driver { get; }
    public FakeWifi Wifi { get; }
    public FakeFirewall Firewall { get; }
    public FakeWatchdog Watchdog { get; }
    public FakeState State { get; }
    public FakeLock Lock { get; } = new();
    public FakeRunner Runner { get; }
    public FakeNetwork Network { get; } = new();
    public FakeClock Clock { get; } = new();
    public ListLog Log { get; } = new();
    public Session Session { get; }

    public Rig()
    {
        Config = new ServiceConfig { InstallDir = Path.Combine(Root, "install"), DataDir = Path.Combine(Root, "data") };
        foreach (string f in Config.RequiredFiles)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(f)!);
            File.WriteAllText(f, "test");
        }
        Driver = new FakeDriver(Calls);
        Wifi = new FakeWifi(Calls);
        Firewall = new FakeFirewall(Calls);
        Watchdog = new FakeWatchdog(Calls);
        State = new FakeState(Calls);
        Runner = new FakeRunner(Calls);
        Session = new Session(Config, Driver, Wifi, Firewall, Watchdog, State, Lock, Runner, Network, Clock, Log);
    }

    public GiveBack GiveBack() => new(Config, Driver, Wifi, Lock, Clock, Log);

    /// <summary>A second service instance on the same machine state (service restart, next boot).</summary>
    public Session NewSession() => new(Config, Driver, Wifi, Firewall, Watchdog, State, Lock, Runner, Network, Clock, Log);

    public void AddVendorBackup()
    {
        string dir = Path.Combine(Config.VendorBackupDir, "oem12-3.5.0.1349");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, ServiceConfig.VendorInfName), "test");
    }

    public int IndexOf(string call) => Calls.IndexOf(call);

    public void Dispose()
    {
        try { Directory.Delete(Root, recursive: true); } catch (IOException) { }
    }
}
