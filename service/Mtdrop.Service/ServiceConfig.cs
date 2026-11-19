namespace Mtdrop.Service;

/// <summary>
/// Every program, argument and file the service uses. Nothing comes from the control pipe (ADR-0004 decision 2).
/// Default layout (decision 3): binaries in the install directory (Program Files, admin-only writable), data in
/// %ProgramData%\mtdrop (SYSTEM + Administrators only).
/// </summary>
public sealed record ServiceConfig
{
    public required string InstallDir { get; init; }
    public required string DataDir { get; init; }

    public const string HardwareId = @"PCI\VEN_14C3&DEV_7961&CC_0280";
    public const string InstancePrefix = @"PCI\VEN_14C3&DEV_7961";
    public const string ProxyInfName = "mtdrop_proxy.inf";
    public const string VendorInfName = "mtkwl6ex.inf";
    public const int Channel = 44;

    public string Radiod => Path.Combine(InstallDir, "radiod.exe");
    public string Awdld => Path.Combine(InstallDir, "awdld.exe");
    public string Wintun => Path.Combine(InstallDir, "wintun.dll");
    public string ProxyInf => Path.Combine(InstallDir, "driver", ProxyInfName);
    /// <summary>linux-firmware's MT7961 images: the Windows vendor firmware ignores the commands we need (F-007).</summary>
    public string FirmwarePatch => Path.Combine(InstallDir, "firmware", "WIFI_MT7961_patch_mcu_1_2_hdr.bin");
    public string FirmwareRam => Path.Combine(InstallDir, "firmware", "WIFI_RAM_CODE_MT7961_1.bin");
    /// <summary>The tray app: the receiver that gets the temporary firewall rules.</summary>
    public string TrayApp => Path.Combine(InstallDir, "app", "mtdrop.exe");
    public string ServiceExe => Path.Combine(InstallDir, "mtdrop-service.exe");

    public string LogDir => Path.Combine(DataDir, "logs");
    public string ProfilesDir => Path.Combine(DataDir, "wifi-profiles");
    public string VendorBackupDir => Path.Combine(DataDir, "vendor-driver-backup");
    public string StateFile => Path.Combine(DataDir, "state", "card-taken");
    /// <summary>
    /// Setting (ADR-0006 A1, F-030): one OFDM rate in Mbit/s for unicast data, passed to radiod as --data-rate.
    /// Absent = <see cref="DefaultDataRate"/>. In the data directory, so only SYSTEM and Administrators can set it.
    /// </summary>
    public string DataRateFile => Path.Combine(DataDir, "settings", "radio-data-rate");

    /// <summary>54 Mbit/s: 35–45 % faster sends than OWL's 12 in F-030, F-031, F-032 and F-034, 2–3 % frames lost.</summary>
    public const int DefaultDataRate = 54;

    public static readonly IReadOnlySet<int> OfdmRates = new HashSet<int> { 6, 9, 12, 18, 24, 36, 48, 54 };

    /// <summary>The data rate setting, or null when absent; <paramref name="invalid"/> is set for a bad file.</summary>
    public int? ReadDataRate(out string? invalid)
    {
        invalid = null;
        string text;
        try { text = File.ReadAllText(DataRateFile).Trim(); }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { return null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { invalid = e.Message; return null; }
        if (int.TryParse(text, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out int mbps)
            && OfdmRates.Contains(mbps))
            return mbps;
        invalid = $"'{(text.Length > 20 ? text[..20] : text)}' is not one of {string.Join(" ", OfdmRates)}";
        return null;
    }

    /// <summary>
    /// Setting (ADR-0007 S3, F-035, F-036): <c>station</c> or absent = radiod registers each data receiver as a
    /// station and sends its data on the AC queue (<c>--sta-data</c>); <c>altx</c> = data on ALTX0 as from F-021 to
    /// F-035. In the data directory, so only SYSTEM and Administrators can set it.
    /// </summary>
    public string TxPathFile => Path.Combine(DataDir, "settings", "radio-tx-path");

    /// <summary>F-036: the 5.4 MB photo in 8.7 s (~620 kB/s, 0.1 % frames given up) against 45.6 s on ALTX0 (8.9 %).</summary>
    public const bool DefaultStationData = true;

    /// <summary>The TX path setting: true = station, false = altx, null when absent; <paramref name="invalid"/> is set for a bad file.</summary>
    public bool? ReadStationData(out string? invalid)
    {
        invalid = null;
        string text;
        try { text = File.ReadAllText(TxPathFile).Trim(); }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { return null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { invalid = e.Message; return null; }
        if (text.Equals("station", StringComparison.OrdinalIgnoreCase)) return true;
        if (text.Equals("altx", StringComparison.OrdinalIgnoreCase)) return false;
        invalid = $"'{(text.Length > 20 ? text[..20] : text)}' is not 'station' or 'altx'";
        return null;
    }

    /// <summary>How long the give-back waits for the MediaTek driver after a rescan, per attempt.</summary>
    public TimeSpan VendorWait { get; init; } = TimeSpan.FromSeconds(40);
    /// <summary>The watchdog task fires this long after the session deadline.</summary>
    public TimeSpan WatchdogGrace { get; init; } = TimeSpan.FromMinutes(2);
    /// <summary>radiod's own limit is the session plus this margin, so the service normally ends it first.</summary>
    public TimeSpan RadiodMargin { get; init; } = TimeSpan.FromSeconds(30);

    public IReadOnlyList<string> RequiredFiles => [Radiod, Awdld, Wintun, ProxyInf, FirmwarePatch, FirmwareRam];

    public static ServiceConfig Default() => new()
    {
        InstallDir = AppContext.BaseDirectory,
        DataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "mtdrop"),
    };
}
