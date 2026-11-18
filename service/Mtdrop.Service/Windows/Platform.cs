using System.ComponentModel;
using System.Diagnostics;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using System.Security;
using System.Text;
using System.Xml;

namespace Mtdrop.Service.Windows;

/// <summary>
/// Saved Wi-Fi networks through the Native Wifi API. Windows keeps them per WLAN interface GUID and the adapter gets a
/// new GUID every time the MediaTek driver binds again (FINDINGS F-025), so the give-back has to add them back.
/// Files are numbered in preference order (no network names in file names); keys stay encrypted for this machine.
/// </summary>
public sealed class Wifi(ServiceConfig config, ILog log) : IWifi
{
    private static readonly TimeSpan InterfaceWait = TimeSpan.FromSeconds(20);

    public int ExportProfiles()
    {
        Directory.CreateDirectory(config.ProfilesDir);
        foreach (string old in Directory.EnumerateFiles(config.ProfilesDir, "*.xml")) File.Delete(old);
        try
        {
            using WlanClient wlan = WlanClient.Open();
            Guid card = FindCard(wlan) ?? throw new IOException("the WLAN service does not list the MT7921 Wi-Fi interface");
            IReadOnlyList<(string Name, uint Flags)> profiles = wlan.Profiles(card);
            int saved = 0, skipped = 0, failed = 0;
            foreach ((string name, uint listFlags) in profiles)
            {
                if ((listFlags & (WlanClient.ProfileGroupPolicy | WlanClient.ProfileUser)) != 0) { skipped++; continue; }
                try
                {
                    (string xml, _) = wlan.Profile(card, name);
                    File.WriteAllText(Path.Combine(config.ProfilesDir, $"{saved:D3}.xml"), xml, new UTF8Encoding(false));
                    saved++;
                }
                catch (Win32Exception e)
                {
                    failed++;
                    log.Warn($"wifi: could not read saved network #{saved + skipped + failed}: {e.Message}");
                }
            }
            log.Info($"wifi: {profiles.Count} saved network(s) on the card, {saved} exported" +
                     (skipped > 0 ? $", {skipped} skipped (group policy or per-user)" : "") + (failed > 0 ? $", {failed} failed" : ""));
            if (saved == 0 && failed > 0) throw new IOException($"none of the {failed} saved Wi-Fi network(s) could be read");
            return saved;
        }
        catch (Win32Exception e)
        {
            throw new IOException(e.Message, e);
        }
    }

    public ProfileRestore RestoreMissingProfiles()
    {
        string[] files = Directory.Exists(config.ProfilesDir)
            ? Directory.EnumerateFiles(config.ProfilesDir, "*.xml").Order(StringComparer.Ordinal).ToArray()
            : [];
        if (files.Length == 0) return new ProfileRestore(0, 0, 0, 0);
        try
        {
            using WlanClient wlan = WlanClient.Open();
            Guid? card;
            DateTime until = DateTime.UtcNow + InterfaceWait;
            while ((card = FindCard(wlan)) is null && DateTime.UtcNow < until) Thread.Sleep(1000);
            if (card is not Guid id)
            {
                log.Warn("wifi: the MT7921 Wi-Fi interface did not appear; saved networks not re-added");
                return new ProfileRestore(files.Length, files.Length, 0, files.Length);
            }
            var known = wlan.Profiles(id).Select(p => p.Name).ToHashSet(StringComparer.Ordinal);
            int missing = 0, restored = 0, failed = 0;
            for (int i = 0; i < files.Length; i++)
            {
                string xml = File.ReadAllText(files[i]);
                string? name = ProfileName(xml);
                if (name is not null && known.Contains(name)) continue;
                missing++;
                string? error = name is null ? "unreadable profile file" : wlan.AddProfile(id, xml);
                if (error is null) restored++;
                else
                {
                    failed++;
                    log.Warn($"wifi: could not re-add saved network #{i + 1}: {error}");
                }
            }
            return new ProfileRestore(files.Length, missing, restored, failed);
        }
        catch (Win32Exception e)
        {
            log.Warn($"wifi: saved networks not re-added: {e.Message}");
            return new ProfileRestore(files.Length, files.Length, 0, files.Length);
        }
    }

    public bool AdapterConnected() => NetworkInterface.GetAllNetworkInterfaces().Any(n =>
        n.NetworkInterfaceType == NetworkInterfaceType.Wireless80211 &&
        n.Description.Contains("MT7921", StringComparison.OrdinalIgnoreCase) &&
        n.OperationalStatus == OperationalStatus.Up);

    public string SavedNetworksSummary()
    {
        try
        {
            using WlanClient wlan = WlanClient.Open();
            if (FindCard(wlan) is not Guid id) return "MT7921 interface not listed by the WLAN service";
            IReadOnlyList<(string Name, uint Flags)> profiles = wlan.Profiles(id);
            int readable = profiles.Count(p =>
            {
                try { return ProfileName(wlan.Profile(id, p.Name).Xml) == p.Name; }
                catch (Win32Exception) { return false; }
            });
            (WlanClient.Radio sw, WlanClient.Radio hw) = wlan.RadioState(id);
            IReadOnlyList<WlanClient.AvailableNetwork> seen = wlan.AvailableNetworks(id);
            int inRange = seen.Where(n => n.ProfileName.Length > 0 && n.Connectable).Select(n => n.ProfileName).Distinct().Count();
            return $"{profiles.Count} on the card, {readable} readable; radio software {sw}, hardware {hw}; " +
                   $"last scan: {seen.Count} entries, {inRange} saved in range, connected: {seen.Any(n => n.Connected)}";
        }
        catch (Win32Exception e)
        {
            return $"unavailable: {e.Message}";
        }
    }

    public string NudgeConnect()
    {
        try
        {
            using WlanClient wlan = WlanClient.Open();
            if (FindCard(wlan) is not Guid card) return "the MT7921 interface is not listed by the WLAN service";
            (WlanClient.Radio sw, WlanClient.Radio hw) = wlan.RadioState(card);
            string radio = $"radio software {sw}, hardware {hw}";
            if (sw != WlanClient.Radio.On || hw != WlanClient.Radio.On) return $"{radio}; not connecting (the radio is off)";
            wlan.Scan(card);
            Thread.Sleep(TimeSpan.FromSeconds(4));                  // scan results take a few seconds
            List<string> order = wlan.Profiles(card).Select(p => p.Name).ToList();
            IReadOnlyList<WlanClient.AvailableNetwork> seen = wlan.AvailableNetworks(card);
            List<int> inRange = seen.Where(n => n.ProfileName.Length > 0 && n.Connectable)
                .Select(n => order.IndexOf(n.ProfileName)).Where(i => i >= 0).Distinct().Order().ToList();
            string what = $"{radio}; {seen.Count} scan entr{(seen.Count == 1 ? "y" : "ies")}, {inRange.Count} saved network(s) in range";
            if (seen.Any(n => n.Connected)) return $"{what}; already connecting/connected";
            if (inRange.Count == 0) return $"{what}; nothing to connect to";
            string? error = wlan.Connect(card, order[inRange[0]]);
            return error is null
                ? $"{what}; asked to connect to saved network #{inRange[0] + 1} of {order.Count} (preference order)"
                : $"{what}; connect request failed: {error}";
        }
        catch (Win32Exception e)
        {
            return $"could not ask the WLAN service: {e.Message}";
        }
    }

    private static Guid? FindCard(WlanClient wlan) =>
        wlan.Interfaces().FirstOrDefault(i => i.Description.Contains("MT7921", StringComparison.OrdinalIgnoreCase))?.Id;

    private static string? ProfileName(string xml)
    {
        try
        {
            var doc = new XmlDocument { XmlResolver = null };
            using (XmlReader reader = XmlReader.Create(new StringReader(xml), DriverControl.SafeXml))
                doc.Load(reader);
            return doc.DocumentElement?.ChildNodes.OfType<XmlElement>().FirstOrDefault(e => e.LocalName == "name")?.InnerText;
        }
        catch (XmlException)
        {
            return null;
        }
    }
}

/// <summary>Temporary inbound rules for the receiver through the firewall's COM API (no command-line quoting).</summary>
public sealed class Firewall(ILog log) : IFirewall
{
    private const string Group = "mtdrop receive session";
    private static readonly (string Name, int Protocol, string Port)[] Rules =
    [
        ("mtdrop-receiver-tcp", 6, "8771"),     // HTTPS: /Discover, /Ask, /Upload
        ("mtdrop-receiver-mdns", 17, "5353"),   // mDNS on awdl0
    ];

    public void AllowReceiver(string program)
    {
        RemoveReceiverRules();
        dynamic policy = Create("HNetCfg.FwPolicy2");
        foreach ((string name, int protocol, string port) in Rules)
        {
            dynamic rule = Create("HNetCfg.FWRule");
            rule.Name = name;
            rule.Grouping = Group;
            rule.Description = "Added by the mtdrop service for one receive session; removed when it ends.";
            rule.ApplicationName = program;
            rule.Protocol = protocol;
            rule.LocalPorts = port;
            rule.Direction = 1;          // NET_FW_RULE_DIR_IN
            rule.Action = 1;             // NET_FW_ACTION_ALLOW
            rule.Profiles = 0x7FFFFFFF;  // all profiles: awdl0 is an unidentified (public) network
            rule.Enabled = true;
            policy.Rules.Add(rule);
        }
        log.Info($"firewall: receiver rules added for {Path.GetFileName(program)}");
    }

    public void RemoveReceiverRules()
    {
        dynamic policy = Create("HNetCfg.FwPolicy2");
        foreach ((string name, _, _) in Rules)
            for (int i = 0; i < 8 && Exists(policy, name); i++)
                policy.Rules.Remove(name);
    }

    private static bool Exists(dynamic policy, string name)
    {
        try
        {
            _ = policy.Rules.Item(name);
            return true;
        }
        catch (Exception e) when (e is COMException or FileNotFoundException)
        {
            return false;
        }
    }

    private static dynamic Create(string progId) =>
        Activator.CreateInstance(Type.GetTypeFromProgID(progId, throwOnError: true)!)!;
}

/// <summary>A one-shot SYSTEM task "mtdrop\watchdog" that runs <c>mtdrop-service.exe --give-back</c>.</summary>
public sealed class WatchdogTask(ServiceConfig config, IProcessRunner runner, ILog log) : IWatchdogTask
{
    public const string TaskName = @"mtdrop\watchdog";
    private static readonly string Schtasks = Path.Combine(Environment.SystemDirectory, "schtasks.exe");

    public void Arm(DateTime utcWhen)
    {
        string xml = TaskXml(utcWhen.ToLocalTime(), config.ServiceExe);
        string file = Path.Combine(config.DataDir, "state", "watchdog.xml");
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, xml, Encoding.Unicode);
        (int code, string output) = runner.Run(Schtasks, ["/Create", "/TN", TaskName, "/XML", file, "/F"], TimeSpan.FromSeconds(30));
        if (code != 0) throw new SessionException($"could not arm the watchdog task (schtasks {code}: {output.Trim()})");
        log.Info($"watchdog task armed for {utcWhen.ToLocalTime():HH:mm:ss}");
    }

    public void Disarm()
    {
        (int code, _) = runner.Run(Schtasks, ["/Delete", "/TN", TaskName, "/F"], TimeSpan.FromSeconds(30));
        if (code == 0) log.Info("watchdog task removed");
    }

    /// <summary>Task Scheduler XML: time trigger in local time, runs as SYSTEM, starts late if the PC was asleep.</summary>
    public static string TaskXml(DateTime localWhen, string exe) => $"""
        <?xml version="1.0" encoding="UTF-16"?>
        <Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
          <RegistrationInfo>
            <Description>mtdrop: gives the Wi-Fi card back to the MediaTek driver if a receive session did not end (ADR-0004).</Description>
          </RegistrationInfo>
          <Triggers>
            <TimeTrigger>
              <StartBoundary>{localWhen:yyyy-MM-ddTHH:mm:ss}</StartBoundary>
              <Enabled>true</Enabled>
            </TimeTrigger>
          </Triggers>
          <Principals>
            <Principal id="Author">
              <UserId>S-1-5-18</UserId>
              <RunLevel>HighestAvailable</RunLevel>
            </Principal>
          </Principals>
          <Settings>
            <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
            <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
            <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
            <StartWhenAvailable>true</StartWhenAvailable>
            <ExecutionTimeLimit>PT10M</ExecutionTimeLimit>
            <Enabled>true</Enabled>
          </Settings>
          <Actions Context="Author">
            <Exec>
              <Command>{SecurityElement.Escape(exe)}</Command>
              <Arguments>--give-back</Arguments>
            </Exec>
          </Actions>
        </Task>
        """;
}

public sealed class StateFile(ServiceConfig config) : IStateFile
{
    public bool CardTaken => File.Exists(config.StateFile);

    public void MarkCardTaken()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(config.StateFile)!);
        File.WriteAllText(config.StateFile, $"card taken {DateTime.Now:yyyy-MM-dd HH:mm:ss}\r\n");
    }

    public void Clear() => File.Delete(config.StateFile);
}

/// <summary>Global named mutex: the service and the watchdog task (another process) never swap at the same time.</summary>
public sealed class CardLock : ICardLock
{
    public IDisposable Acquire(TimeSpan timeout)
    {
        var mutex = new Mutex(false, @"Global\mtdrop-card");
        try
        {
            if (!mutex.WaitOne(timeout)) throw new TimeoutException("another mtdrop process is switching the Wi-Fi card");
        }
        catch (AbandonedMutexException)
        {
            // The previous owner died while holding it; we own it now.
        }
        return new Releaser(mutex);
    }

    private sealed class Releaser(Mutex mutex) : IDisposable
    {
        public void Dispose()
        {
            mutex.ReleaseMutex();
            mutex.Dispose();
        }
    }
}

public sealed class NetworkProbe : INetworkProbe
{
    public bool AwdlUp() => NetworkInterface.GetAllNetworkInterfaces()
        .Any(n => n.Name.Equals("awdl0", StringComparison.OrdinalIgnoreCase) && n.OperationalStatus == OperationalStatus.Up);
}

/// <summary>Timestamped lines in %ProgramData%\mtdrop\logs\service.log (and the console in --console mode).</summary>
public sealed class FileLog(string path, bool echo) : ILog
{
    private readonly object _gate = new();

    public void Info(string message) => Write("    ", message);
    public void Warn(string message) => Write("WARN", message);

    private void Write(string level, string message)
    {
        string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {level} {message}";
        lock (_gate)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                if (File.Exists(path) && new FileInfo(path).Length > 5 * 1024 * 1024)
                    File.Move(path, path + ".1", overwrite: true);
                File.AppendAllText(path, line + Environment.NewLine);
            }
            catch (IOException)
            {
                // Logging must never take the service down.
            }
            if (echo) Console.WriteLine(line);
        }
    }
}

/// <summary>Child processes: hidden, output to a log file, in a job object that kills them if the service dies.</summary>
public sealed partial class ProcessRunner(ILog log) : IProcessRunner
{
    private static readonly Lazy<IntPtr> Job = new(CreateKillOnCloseJob);

    public (int ExitCode, string Output) Run(string exe, IReadOnlyList<string> args, TimeSpan timeout)
    {
        using var p = new Process { StartInfo = Info(exe, args, null) };
        p.Start();
        Task<string> stdout = p.StandardOutput.ReadToEndAsync();
        Task<string> stderr = p.StandardError.ReadToEndAsync();
        if (!p.WaitForExit(timeout))
        {
            try { p.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            log.Warn($"{Path.GetFileName(exe)} did not finish within {timeout.TotalSeconds:0} s; killed");
            return (-1, "");
        }
        p.WaitForExit();
        return (p.ExitCode, stdout.Result + stderr.Result);
    }

    public IChildProcess Start(string exe, IReadOnlyList<string> args, string logPath, string? workingDirectory = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
        var writer = new StreamWriter(logPath, append: false, Encoding.UTF8) { AutoFlush = true };
        var p = new Process { StartInfo = Info(exe, args, workingDirectory), EnableRaisingEvents = true };
        DataReceivedEventHandler sink = (_, e) =>
        {
            if (e.Data is null) return;
            lock (writer) writer.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {e.Data}");
        };
        p.OutputDataReceived += sink;
        p.ErrorDataReceived += sink;
        p.Start();
        if (!AssignProcessToJobObject(Job.Value, p.Handle))
            log.Warn($"{Path.GetFileName(exe)} could not be put into the job object (error {Marshal.GetLastWin32Error()})");
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        log.Info($"started {Path.GetFileName(exe)} (pid {p.Id})");
        return new Child(p, writer);
    }

    public void KillStrays(IReadOnlyList<string> exePaths)
    {
        foreach (string exe in exePaths)
            foreach (Process p in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(exe)))
                using (p)
                {
                    try
                    {
                        if (!string.Equals(p.MainModule?.FileName, exe, StringComparison.OrdinalIgnoreCase)) continue;
                        p.Kill(entireProcessTree: true);
                        log.Warn($"killed a leftover {Path.GetFileName(exe)} (pid {p.Id})");
                    }
                    catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
                }
    }

    private static ProcessStartInfo Info(string exe, IReadOnlyList<string> args, string? workingDirectory)
    {
        var info = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = workingDirectory ?? Path.GetDirectoryName(exe)!,
        };
        foreach (string a in args) info.ArgumentList.Add(a);
        return info;
    }

    private sealed class Child(Process p, StreamWriter writer) : IChildProcess
    {
        public string Name { get; } = p.ProcessName;
        public bool HasExited => p.HasExited;
        public int? ExitCode => p.HasExited ? p.ExitCode : null;

        public async Task<bool> WaitForExitAsync(TimeSpan timeout)
        {
            using var cts = new CancellationTokenSource(timeout);
            try
            {
                await p.WaitForExitAsync(cts.Token);
                return true;
            }
            catch (OperationCanceledException)
            {
                return false;
            }
        }

        public void Kill()
        {
            try { p.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
        }

        public void Dispose()
        {
            p.Dispose();
            lock (writer) writer.Dispose();
        }
    }

    private static IntPtr CreateKillOnCloseJob()
    {
        IntPtr job = CreateJobObjectW(IntPtr.Zero, null);
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>());
        return job;
    }

    private const int JobObjectExtendedLimitInformation = 9;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    private static partial IntPtr CreateJobObjectW(IntPtr attributes, string? name);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetInformationJobObject(IntPtr job, int infoClass, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, uint length);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool AssignProcessToJobObject(IntPtr job, IntPtr process);
}
