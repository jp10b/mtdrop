using Mtdrop.Service.Windows;

namespace Mtdrop.Service.Tests;

public sealed class GiveBackTests : IDisposable
{
    private readonly Rig _rig = new();

    public void Dispose() => _rig.Dispose();

    [Fact]
    public void Removes_our_package_and_waits_for_the_vendor_driver()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;

        Assert.True(_rig.GiveBack().Run());
        Assert.Equal(["driver:delete oem99.inf", "wifi:restore"], _rig.Calls);
        Assert.Equal(0, _rig.Lock.Held);
    }

    [Fact]
    public void Give_back_never_switches_the_running_device_to_the_vendor_package()
    {
        // F-032: switching the driver on the running device hung Windows; the package is always removed with /uninstall.
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        _rig.Driver.VendorInStore = true;

        Assert.True(_rig.GiveBack().Run());
        Assert.Equal(["driver:delete oem99.inf", "wifi:restore"], _rig.Calls);
    }

    [Fact]
    public void Parses_vendor_packages_from_pnputil_xml()
    {
        const string xml = """
            <PnpUtil><Driver DriverName="oem12.inf"><OriginalName>mtkwl6ex.inf</OriginalName></Driver>
            <Driver DriverName="oem121.inf"><OriginalName>mtdrop_proxy.inf</OriginalName></Driver></PnpUtil>
            """;
        Assert.Equal(["oem12.inf"], Mtdrop.Service.Windows.DriverControl.ParsePackages(xml, ServiceConfig.VendorInfName));
        Assert.Equal(["oem121.inf"], Mtdrop.Service.Windows.DriverControl.ParseOurPackages(xml));
    }

    [Fact]
    public void Nudges_a_connect_when_wifi_does_not_reconnect_by_itself()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        _rig.Wifi.Connected = false;

        Assert.True(_rig.GiveBack().Run());
        Assert.Equal(["driver:delete oem99.inf", "wifi:restore", "wifi:nudge"], _rig.Calls);
        Assert.True(_rig.Log.Has("did not connect by itself; nudged (test)"));
        Assert.True(_rig.Log.Has("give-back: Wi-Fi is connected"));
    }

    [Fact]
    public void Reports_wifi_still_disconnected_after_the_nudge()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        _rig.Wifi.Connected = false;
        _rig.Wifi.NudgeConnects = false;

        Assert.True(_rig.GiveBack().Run());       // the card is back; only the connect is missing
        Assert.Single(_rig.Calls, "wifi:nudge");
        Assert.True(_rig.Log.Has("back but not connected yet"));
    }

    [Fact]
    public void Installs_the_vendor_driver_from_the_backup_if_it_does_not_come_back()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        _rig.Driver.VendorReturnsAfterDelete = false;
        _rig.AddVendorBackup();

        Assert.True(_rig.GiveBack().Run());
        Assert.Equal("driver:scan", _rig.Calls[1]);               // rescan only when the vendor driver did not come back
        Assert.Contains("driver:install-vendor", _rig.Calls);
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
    }

    [Fact]
    public void Reports_failure_without_a_backup()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        _rig.Driver.VendorReturnsAfterDelete = false;

        Assert.False(_rig.GiveBack().Run());
        Assert.True(_rig.Log.Has("backup was not found"));
    }

    [Fact]
    public void Reports_failure_when_the_backup_does_not_help()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        _rig.Driver.VendorReturnsAfterDelete = false;
        _rig.Driver.BackupInstallWorks = false;
        _rig.AddVendorBackup();

        Assert.False(_rig.GiveBack().Run());
        Assert.True(_rig.Log.Has("give-back FAILED"));
    }

    [Fact]
    public void Is_harmless_when_the_card_is_already_back()
    {
        Assert.True(_rig.GiveBack().Run());
        Assert.DoesNotContain(_rig.Calls, c => c.StartsWith("driver:delete", StringComparison.Ordinal));
    }

    [Fact]
    public void A_card_on_our_package_is_not_vendor_bound_even_if_the_provider_says_MediaTek()
    {
        _rig.Driver.PackageInStore = true;
        var card = new CardInfo("x", "MediaTek, Inc.", "oem99.inf", "1", true, 0);
        Assert.False(_rig.GiveBack().IsVendorBound(card));
        Assert.True(_rig.GiveBack().IsProxyBound(card));
    }
}

public sealed class PlatformTests
{
    [Fact]
    public void Finds_our_packages_in_pnputil_xml()
    {
        const string xml = """
            <?xml version="1.0" encoding="utf-8"?>
            <PnpUtil Version="10.0.26200" Command="/enum-drivers /format xml">
                <Driver DriverName="oem3.inf"><OriginalName>acerairplanemodecontroller.inf</OriginalName></Driver>
                <Driver DriverName="oem97.inf"><OriginalName>mtdrop_proxy.inf</OriginalName></Driver>
                <Driver DriverName="oem10.inf"><OriginalName>mtkwl6ex.inf</OriginalName></Driver>
                <Driver DriverName="oem98.inf"><OriginalName>MTDROP_PROXY.INF</OriginalName></Driver>
            </PnpUtil>
            """;
        Assert.Equal(["oem97.inf", "oem98.inf"], DriverControl.ParseOurPackages(xml));
    }

    [Fact]
    public void Pnputil_xml_with_a_doctype_is_not_resolved()
    {
        const string xml = """<?xml version="1.0"?><!DOCTYPE x SYSTEM "http://127.0.0.1:1/x.dtd"><PnpUtil/>""";
        Assert.ThrowsAny<System.Xml.XmlException>(() => DriverControl.ParseOurPackages(xml));
    }

    [Fact]
    public void Watchdog_task_xml_runs_the_service_exe_as_system_at_local_time()
    {
        string xml = WatchdogTask.TaskXml(new DateTime(2026, 11, 17, 22, 5, 9), @"C:\Program Files\mtdrop\mtdrop-service.exe");
        var doc = new System.Xml.XmlDocument();
        doc.LoadXml(xml.Replace("encoding=\"UTF-16\"", ""));
        Assert.Contains("<StartBoundary>2026-11-17T22:05:09</StartBoundary>", xml);
        Assert.Contains("<UserId>S-1-5-18</UserId>", xml);
        Assert.Contains(@"<Command>C:\Program Files\mtdrop\mtdrop-service.exe</Command>", xml);
        Assert.Contains("<Arguments>--give-back</Arguments>", xml);
        Assert.Contains("<StartWhenAvailable>true</StartWhenAvailable>", xml);
    }

    [Fact]
    public void Watchdog_task_xml_escapes_the_path()
    {
        string xml = WatchdogTask.TaskXml(DateTime.Now, @"C:\a&b<c>\mtdrop-service.exe");
        Assert.Contains(@"C:\a&amp;b&lt;c&gt;\mtdrop-service.exe", xml);
    }

    [Fact]
    public void Data_dir_acl_is_system_and_admins_only_and_not_inherited()
    {
        System.Security.AccessControl.DirectorySecurity s = DataDirAcl.Security();
        Assert.True(s.AreAccessRulesProtected);
        Assert.Equal("S-1-5-32-544", s.GetOwner(typeof(System.Security.Principal.SecurityIdentifier))!.Value);
        var rules = s.GetAccessRules(true, true, typeof(System.Security.Principal.SecurityIdentifier))
            .Cast<System.Security.AccessControl.FileSystemAccessRule>().ToList();
        Assert.Equal(["S-1-5-18", "S-1-5-32-544"], rules.Select(r => r.IdentityReference.Value).Order());
        Assert.All(rules, r => Assert.Equal(System.Security.AccessControl.AccessControlType.Allow, r.AccessControlType));
    }
}
