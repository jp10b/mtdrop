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
}
