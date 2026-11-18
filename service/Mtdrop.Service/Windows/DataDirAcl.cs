using System.Security.AccessControl;
using System.Security.Principal;

namespace Mtdrop.Service.Windows;

/// <summary>
/// %ProgramData%\mtdrop holds files the service acts on as SYSTEM: the watchdog task XML handed to schtasks, saved
/// Wi-Fi profiles re-added with netsh, the vendor driver backup installed with pnputil, the state file. ProgramData
/// lets every user create files and folders, so the directory must not inherit that: SYSTEM and Administrators only,
/// owned by Administrators (ADR-0004 decision 3). The installer sets this first; the service re-applies it at start.
/// </summary>
public static class DataDirAcl
{
    private static readonly SecurityIdentifier System = new(WellKnownSidType.LocalSystemSid, null);
    private static readonly SecurityIdentifier Admins = new(WellKnownSidType.BuiltinAdministratorsSid, null);

    public static DirectorySecurity Security()
    {
        var s = new DirectorySecurity();
        s.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        s.SetOwner(Admins);
        const InheritanceFlags all = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
        s.AddAccessRule(new FileSystemAccessRule(System, FileSystemRights.FullControl, all, PropagationFlags.None, AccessControlType.Allow));
        s.AddAccessRule(new FileSystemAccessRule(Admins, FileSystemRights.FullControl, all, PropagationFlags.None, AccessControlType.Allow));
        return s;
    }

    /// <summary>Creates the directory if needed and replaces its DACL and owner; children inherit the new entries.</summary>
    public static void Apply(string dir)
    {
        Directory.CreateDirectory(dir);
        new DirectoryInfo(dir).SetAccessControl(Security());
    }
}
