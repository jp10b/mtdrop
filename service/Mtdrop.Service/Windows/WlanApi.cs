using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Mtdrop.Service.Windows;

/// <summary>
/// Minimal Native Wifi API (wlanapi.dll) client: interfaces, saved profiles (as XML) and adding profiles.
/// Profile XML is read without WLAN_PROFILE_GET_PLAINTEXT_KEY, so key material stays encrypted for this machine,
/// exactly as <c>netsh wlan export profile</c> without <c>key=clear</c> writes it.
/// </summary>
public sealed class WlanClient : IDisposable
{
    /// <summary>WLAN_PROFILE_GROUP_POLICY: set by policy, cannot be added back by us.</summary>
    public const uint ProfileGroupPolicy = 0x1;
    /// <summary>WLAN_PROFILE_USER: per-user profile of the calling user.</summary>
    public const uint ProfileUser = 0x2;

    private const uint ErrorAlreadyExists = 183;
    private const int InterfaceInfoSize = 16 + 256 * 2 + 4;     // GUID, WCHAR[256] description, WLAN_INTERFACE_STATE
    private const int ProfileInfoSize = 256 * 2 + 4;            // WCHAR[256] name, DWORD flags
    // WLAN_AVAILABLE_NETWORK: WCHAR[256] profile (0), DOT11_SSID (512, 36 bytes), bss type (548), bssids (552),
    // connectable (556), reason (560), phy count (564), phy types[8] (568), more phys (600), signal (604),
    // security (608), auth (612), cipher (616), flags (620), reserved (624)
    private const int AvailableNetworkSize = 628;
    private const uint NetworkConnected = 0x1;                  // WLAN_AVAILABLE_NETWORK_CONNECTED
    private const uint NetworkHasProfile = 0x2;                 // WLAN_AVAILABLE_NETWORK_HAS_PROFILE
    private const uint OpcodeRadioState = 4;                    // wlan_intf_opcode_radio_state

    private IntPtr _handle;

    private WlanClient(IntPtr handle) => _handle = handle;

    /// <exception cref="Win32Exception">The WLAN AutoConfig service is not running or refused the client.</exception>
    public static WlanClient Open()
    {
        uint rc = WlanOpenHandle(2, IntPtr.Zero, out _, out IntPtr handle);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanOpenHandle failed ({rc})");
        return new WlanClient(handle);
    }

    public sealed record Interface(Guid Id, string Description, int State);

    public IReadOnlyList<Interface> Interfaces()
    {
        uint rc = WlanEnumInterfaces(_handle, IntPtr.Zero, out IntPtr list);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanEnumInterfaces failed ({rc})");
        try
        {
            int count = Marshal.ReadInt32(list);
            var result = new List<Interface>(count);
            for (int i = 0; i < count; i++)
            {
                IntPtr item = list + 8 + i * InterfaceInfoSize;
                var id = Marshal.PtrToStructure<Guid>(item);
                string description = Marshal.PtrToStringUni(item + 16) ?? "";
                int state = Marshal.ReadInt32(item + 16 + 512);
                result.Add(new Interface(id, description, state));
            }
            return result;
        }
        finally
        {
            WlanFreeMemory(list);
        }
    }

    /// <summary>Profile names on one interface, in connection preference order, with their WLAN_PROFILE_* flags.</summary>
    public IReadOnlyList<(string Name, uint Flags)> Profiles(Guid iface)
    {
        uint rc = WlanGetProfileList(_handle, ref iface, IntPtr.Zero, out IntPtr list);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanGetProfileList failed ({rc})");
        try
        {
            int count = Marshal.ReadInt32(list);
            var result = new List<(string, uint)>(count);
            for (int i = 0; i < count; i++)
            {
                IntPtr item = list + 8 + i * ProfileInfoSize;
                result.Add((Marshal.PtrToStringUni(item) ?? "", (uint)Marshal.ReadInt32(item + 512)));
            }
            return result;
        }
        finally
        {
            WlanFreeMemory(list);
        }
    }

    /// <summary>The profile's XML (keys encrypted) and its flags.</summary>
    public (string Xml, uint Flags) Profile(Guid iface, string name)
    {
        uint flags = 0;
        uint rc = WlanGetProfile(_handle, ref iface, name, IntPtr.Zero, out IntPtr xml, ref flags, out _);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanGetProfile failed ({rc})");
        try
        {
            return (Marshal.PtrToStringUni(xml) ?? "", flags);
        }
        finally
        {
            WlanFreeMemory(xml);
        }
    }

    /// <summary>Adds an all-user profile; null on success (or if it already exists), else the error text.</summary>
    public string? AddProfile(Guid iface, string xml)
    {
        uint rc = WlanSetProfile(_handle, ref iface, 0, xml, null, false, IntPtr.Zero, out uint reason);
        if (rc == 0 || rc == ErrorAlreadyExists) return null;
        var text = new StringBuilder(256);
        string why = WlanReasonCodeToString(reason, text.Capacity, text, IntPtr.Zero) == 0 ? text.ToString() : $"reason {reason}";
        return $"error {rc}: {why}";
    }

    /// <summary>DOT11_RADIO_STATE.</summary>
    public enum Radio { Unknown = 0, On = 1, Off = 2 }

    /// <summary>Software and hardware radio state of the first PHY (wlan_intf_opcode_radio_state).</summary>
    public (Radio Software, Radio Hardware) RadioState(Guid iface)
    {
        uint rc = WlanQueryInterface(_handle, ref iface, OpcodeRadioState, IntPtr.Zero, out uint size, out IntPtr data, IntPtr.Zero);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanQueryInterface(radio state) failed ({rc})");
        try
        {
            // WLAN_RADIO_STATE: DWORD dwNumberOfPhys; WLAN_PHY_RADIO_STATE { DWORD index; DOT11_RADIO_STATE sw, hw; }[64]
            if (size < 16 || Marshal.ReadInt32(data) < 1) return (Radio.Unknown, Radio.Unknown);
            return ((Radio)Marshal.ReadInt32(data + 8), (Radio)Marshal.ReadInt32(data + 12));
        }
        finally
        {
            WlanFreeMemory(data);
        }
    }

    /// <summary>Asks the WLAN service for a fresh scan; the results arrive a few seconds later.</summary>
    public void Scan(Guid iface)
    {
        uint rc = WlanScan(_handle, ref iface, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanScan failed ({rc})");
    }

    /// <param name="ProfileName">Saved profile for this network, empty for an entry without one.</param>
    public sealed record AvailableNetwork(string ProfileName, bool Connectable, bool Connected);

    /// <summary>Networks from the last scan (WlanGetAvailableNetworkList, flags 0).</summary>
    public IReadOnlyList<AvailableNetwork> AvailableNetworks(Guid iface)
    {
        uint rc = WlanGetAvailableNetworkList(_handle, ref iface, 0, IntPtr.Zero, out IntPtr list);
        if (rc != 0) throw new Win32Exception((int)rc, $"WlanGetAvailableNetworkList failed ({rc})");
        try
        {
            int count = Marshal.ReadInt32(list);
            var result = new List<AvailableNetwork>(count);
            for (int i = 0; i < count; i++)
            {
                IntPtr item = list + 8 + i * AvailableNetworkSize;
                string profile = Marshal.PtrToStringUni(item) ?? "";
                bool connectable = Marshal.ReadInt32(item + 556) != 0;
                uint flags = (uint)Marshal.ReadInt32(item + 620);
                result.Add(new AvailableNetwork((flags & NetworkHasProfile) != 0 ? profile : "", connectable, (flags & NetworkConnected) != 0));
            }
            return result;
        }
        finally
        {
            WlanFreeMemory(list);
        }
    }

    /// <summary>Connects with a saved profile, as "Connect" in the Wi-Fi menu does; null when the request was accepted.</summary>
    public string? Connect(Guid iface, string profileName)
    {
        var p = new ConnectionParameters { Mode = 0 /* wlan_connection_mode_profile */, Profile = profileName, BssType = 1 /* infrastructure */ };
        uint rc = WlanConnect(_handle, ref iface, ref p, IntPtr.Zero);
        return rc == 0 ? null : $"error {rc}";
    }

    public void Dispose()
    {
        if (_handle == IntPtr.Zero) return;
        WlanCloseHandle(_handle, IntPtr.Zero);
        _handle = IntPtr.Zero;
    }

    [DllImport("wlanapi.dll")]
    private static extern uint WlanOpenHandle(uint clientVersion, IntPtr reserved, out uint negotiatedVersion, out IntPtr clientHandle);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanCloseHandle(IntPtr clientHandle, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanEnumInterfaces(IntPtr clientHandle, IntPtr reserved, out IntPtr interfaceList);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanGetProfileList(IntPtr clientHandle, ref Guid interfaceGuid, IntPtr reserved, out IntPtr profileList);

    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)]
    private static extern uint WlanGetProfile(IntPtr clientHandle, ref Guid interfaceGuid, string profileName, IntPtr reserved,
        out IntPtr profileXml, ref uint flags, out uint grantedAccess);

    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)]
    private static extern uint WlanSetProfile(IntPtr clientHandle, ref Guid interfaceGuid, uint flags, string profileXml,
        string? allUserProfileSecurity, [MarshalAs(UnmanagedType.Bool)] bool overwrite, IntPtr reserved, out uint reasonCode);

    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)]
    private static extern uint WlanReasonCodeToString(uint reasonCode, int bufferSize, StringBuilder buffer, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern void WlanFreeMemory(IntPtr memory);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanQueryInterface(IntPtr clientHandle, ref Guid interfaceGuid, uint opCode, IntPtr reserved,
        out uint dataSize, out IntPtr data, IntPtr opcodeValueType);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanScan(IntPtr clientHandle, ref Guid interfaceGuid, IntPtr dot11Ssid, IntPtr ieData, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanGetAvailableNetworkList(IntPtr clientHandle, ref Guid interfaceGuid, uint flags, IntPtr reserved,
        out IntPtr availableNetworkList);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanConnect(IntPtr clientHandle, ref Guid interfaceGuid, ref ConnectionParameters parameters, IntPtr reserved);

    /// <summary>WLAN_CONNECTION_PARAMETERS.</summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ConnectionParameters
    {
        public int Mode;
        [MarshalAs(UnmanagedType.LPWStr)] public string Profile;
        public IntPtr Ssid;
        public IntPtr DesiredBssids;
        public int BssType;
        public uint Flags;
    }
}
