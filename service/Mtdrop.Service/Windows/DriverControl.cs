using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Xml;

namespace Mtdrop.Service.Windows;

/// <summary>
/// Driver store and device binding: pnputil for packages (as scripts/s3-bringup.ps1 and wifi-giveback.ps1),
/// newdev!UpdateDriverForPlugAndPlayDevices for the swap, CfgMgr32 for the live device state (the CIM view lags
/// behind a swap by seconds, F-004).
/// </summary>
public sealed partial class DriverControl(IProcessRunner runner, ILog log) : IDriverControl
{
    private static readonly string PnpUtil = Path.Combine(Environment.SystemDirectory, "pnputil.exe");
    private static readonly TimeSpan PnpTimeout = TimeSpan.FromMinutes(2);

    public CardInfo? FindCard()
    {
        string? id = FindInstanceId();
        if (id is null) return null;
        if (CM_Locate_DevNodeW(out uint dev, id, 0) != CR_SUCCESS) return null;
        bool started = CM_Get_DevNode_Status(out uint status, out uint problem, dev, 0) == CR_SUCCESS &&
                       (status & DN_STARTED) != 0 && (status & DN_HAS_PROBLEM) == 0;
        return new CardInfo(id,
            GetString(dev, DriverProperty(9)),     // DEVPKEY_Device_DriverProvider
            GetString(dev, DriverProperty(5)),     // DEVPKEY_Device_DriverInfPath
            GetString(dev, DriverProperty(3)),     // DEVPKEY_Device_DriverVersion
            started, (int)problem);
    }

    public IReadOnlyList<string> OurPackages() => ParsePackages(EnumDrivers(), ServiceConfig.ProxyInfName);

    public IReadOnlyList<string> VendorPackages() => ParsePackages(EnumDrivers(), ServiceConfig.VendorInfName);

    private string EnumDrivers()
    {
        (int code, string xml) = runner.Run(PnpUtil, ["/enum-drivers", "/format", "xml"], PnpTimeout);
        if (code != 0) throw new InvalidOperationException($"pnputil /enum-drivers failed ({code})");
        return xml;
    }

    /// <summary>Published names of driver store packages whose original INF is ours (pnputil's XML: Driver@DriverName, OriginalName).</summary>
    public static IReadOnlyList<string> ParseOurPackages(string xml) => ParsePackages(xml, ServiceConfig.ProxyInfName);

    public static IReadOnlyList<string> ParsePackages(string xml, string originalName)
    {
        var doc = new XmlDocument { XmlResolver = null };
        using (XmlReader reader = XmlReader.Create(new StringReader(xml), SafeXml))
            doc.Load(reader);
        var result = new List<string>();
        foreach (XmlElement d in doc.SelectNodes("//Driver")!.OfType<XmlElement>())
            if (string.Equals(d["OriginalName"]?.InnerText.Trim(), originalName, StringComparison.OrdinalIgnoreCase) &&
                d.GetAttribute("DriverName") is { Length: > 0 } name)
                result.Add(name);
        return result;
    }

    /// <summary>No DTDs, no external entities: the XML comes from a tool's output, but parse it defensively anyway.</summary>
    public static readonly XmlReaderSettings SafeXml = new() { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };

    public void AddProxyPackage(string inf)
    {
        (int code, string output) = runner.Run(PnpUtil, ["/add-driver", inf], PnpTimeout);
        if (code != 0)
        {
            log.Warn($"pnputil /add-driver: {output.Trim()}");
            throw new SessionException($"Windows did not accept the mtdrop driver package (pnputil {code})");
        }
        log.Info("proxy package added to the driver store");
    }

    public void BindProxy(string inf)
    {
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, ServiceConfig.HardwareId, inf, INSTALLFLAG_FORCE, out bool reboot))
            throw new SessionException($"Windows refused to put the mtdrop driver on the card ({new Win32Exception(Marshal.GetLastWin32Error()).Message})");
        if (reboot) throw new SessionException("Windows asks for a restart to switch the driver; not continuing");
    }

    public int DeletePackage(string published, bool uninstall) =>
        runner.Run(PnpUtil, uninstall ? ["/delete-driver", published, "/uninstall", "/force"] : ["/delete-driver", published, "/force"],
            PnpTimeout).ExitCode;

    public void ScanDevices() => runner.Run(PnpUtil, ["/scan-devices"], PnpTimeout);

    public int InstallVendorDriver(string inf) => runner.Run(PnpUtil, ["/add-driver", inf, "/install"], PnpTimeout).ExitCode;

    private static string? FindInstanceId()
    {
        const uint flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
        if (CM_Get_Device_ID_List_SizeW(out uint len, "PCI", flags) != CR_SUCCESS || len == 0) return null;
        char[] buffer = new char[len];
        if (CM_Get_Device_ID_ListW("PCI", buffer, len, flags) != CR_SUCCESS) return null;
        return new string(buffer).Split('\0', StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault(id => id.StartsWith(ServiceConfig.InstancePrefix, StringComparison.OrdinalIgnoreCase));
    }

    private static DEVPROPKEY DriverProperty(uint pid) => new() { fmtid = new Guid("a8b865dd-2e3d-4094-ad97-e593a70c75d6"), pid = pid };

    private static string? GetString(uint dev, DEVPROPKEY key)
    {
        uint size = 0;
        int cr = CM_Get_DevNode_PropertyW(dev, ref key, out uint type, null, ref size, 0);
        if (cr != CR_BUFFER_SMALL || size == 0) return null;
        byte[] buffer = new byte[size];
        if (CM_Get_DevNode_PropertyW(dev, ref key, out type, buffer, ref size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING) return null;
        return System.Text.Encoding.Unicode.GetString(buffer, 0, (int)size).TrimEnd('\0');
    }

    private const int CR_SUCCESS = 0, CR_BUFFER_SMALL = 0x1A;
    private const uint CM_GETIDLIST_FILTER_ENUMERATOR = 0x1, CM_GETIDLIST_FILTER_PRESENT = 0x100;
    private const uint DN_STARTED = 0x8, DN_HAS_PROBLEM = 0x400;
    private const uint DEVPROP_TYPE_STRING = 0x12;
    private const uint INSTALLFLAG_FORCE = 0x1;

    [StructLayout(LayoutKind.Sequential)]
    private struct DEVPROPKEY
    {
        public Guid fmtid;
        public uint pid;
    }

    [LibraryImport("cfgmgr32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int CM_Get_Device_ID_List_SizeW(out uint len, string? filter, uint flags);

    [LibraryImport("cfgmgr32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int CM_Get_Device_ID_ListW(string? filter, [Out] char[] buffer, uint len, uint flags);

    [LibraryImport("cfgmgr32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int CM_Locate_DevNodeW(out uint devInst, string id, uint flags);

    [LibraryImport("cfgmgr32.dll")]
    private static partial int CM_Get_DevNode_Status(out uint status, out uint problem, uint devInst, uint flags);

    [LibraryImport("cfgmgr32.dll")]
    private static partial int CM_Get_DevNode_PropertyW(uint devInst, ref DEVPROPKEY key, out uint type, [Out] byte[]? buffer, ref uint size, uint flags);

    [LibraryImport("newdev.dll", StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent, string hardwareId, string fullInfPath, uint flags,
        [MarshalAs(UnmanagedType.Bool)] out bool rebootRequired);
}
