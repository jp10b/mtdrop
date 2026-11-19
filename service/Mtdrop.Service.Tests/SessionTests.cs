using Mtdrop.Control;

namespace Mtdrop.Service.Tests;

public sealed class SessionTests : IDisposable
{
    private readonly Rig _rig = new();

    public void Dispose() => _rig.Dispose();

    private async Task<ControlResponse> RunToEnd(int minutes = 2)
    {
        ControlResponse started = _rig.Session.Start(minutes);
        Assert.True(started.Ok, started.Error);
        await _rig.Session.Completion.WaitAsync(TimeSpan.FromSeconds(20));
        return _rig.Session.Status();
    }

    [Fact]
    public async Task Receives_until_the_deadline_then_gives_the_card_back()
    {
        ControlResponse end = await RunToEnd();

        Assert.Equal(SessionState.Idle, end.State);
        Assert.Null(end.Error);
        Assert.True(_rig.Log.Has("session: Receiving"));
        Assert.True(_rig.Log.Has("time window ended"));
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
        Assert.False(_rig.Driver.PackageInStore);
        Assert.False(_rig.State.CardTaken);
        Assert.Null(_rig.Watchdog.ArmedFor);
        Assert.Null(_rig.Firewall.AllowedProgram);
        Assert.Equal(0, _rig.Lock.Held);
    }

    [Fact]
    public async Task Watchdog_and_state_file_are_in_place_before_the_card_is_taken()
    {
        await RunToEnd();

        int arm = _rig.IndexOf("watchdog:arm"), taken = _rig.IndexOf("state:taken");
        int add = _rig.IndexOf("driver:add"), bind = _rig.IndexOf("driver:bind");
        Assert.True(arm >= 0 && taken > arm && add > taken && bind > add, string.Join(", ", _rig.Calls));
        Assert.True(_rig.IndexOf("run:radiod.exe dma-probe") < arm);
        Assert.True(_rig.IndexOf("wifi:export") < bind);
        // cleared only after the MediaTek driver is back
        Assert.True(_rig.IndexOf("state:clear") > _rig.IndexOf("driver:delete oem99.inf"));
    }

    [Fact]
    public async Task Watchdog_fires_after_the_session_deadline_plus_grace()
    {
        DateTime start = _rig.Clock.UtcNow;
        DateTime? armed = null;
        _rig.Network.OnCall = _ => armed ??= _rig.Watchdog.ArmedFor;
        _rig.Session.Start(3);
        await _rig.Session.Completion.WaitAsync(TimeSpan.FromSeconds(20));
        Assert.Equal(start.AddMinutes(3) + _rig.Config.WatchdogGrace, armed);
    }

    [Fact]
    public async Task Starts_radiod_and_awdld_with_fixed_arguments_from_the_install_directory()
    {
        await RunToEnd(2);

        (string exe, IReadOnlyList<string> args) = _rig.Runner.Started[0];
        Assert.Equal(_rig.Config.Radiod, exe);
        // 120 s window - 2 s settle + 30 s margin
        Assert.Equal(["--live-log", Path.Combine(_rig.Config.LogDir, "radiod-live.log"), "serve", "44",
            _rig.Config.FirmwarePatch, _rig.Config.FirmwareRam, "--max-seconds", "148", "--mac", "random", "--wait-client", "60",
            "--data-rate", "54"], args);
        Assert.Equal(_rig.Config.Awdld, _rig.Runner.Started[1].Exe);
        Assert.Equal(["-c", "44", "-S", "intersect"], _rig.Runner.Started[1].Args);
        Assert.True(_rig.Runner.Children["awdld"].Killed);
        Assert.False(_rig.Runner.Children["radiod"].Killed);   // ended by itself after awdld went (lease)
        Assert.True(_rig.IndexOf("firewall:allow") > _rig.IndexOf("start:awdld"));
    }

    [Theory]
    [InlineData("12", "12")]
    [InlineData(" 24\r\n", "24")]
    [InlineData("55", "54")]                      // invalid: ignored, default
    [InlineData("54 --max-seconds 900", "54")]    // cannot smuggle options
    public async Task Data_rate_setting_overrides_the_default_only_when_valid(string content, string expected)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_rig.Config.DataRateFile)!);
        File.WriteAllText(_rig.Config.DataRateFile, content);
        await RunToEnd(2);

        IReadOnlyList<string> args = _rig.Runner.Started[0].Args;
        Assert.Equal(expected, args[args.ToList().IndexOf("--data-rate") + 1]);
        Assert.Equal(14, args.Count);
        Assert.Equal(expected != content.Trim(), _rig.Log.Has("ignoring"));
    }

    [Theory]
    [InlineData("station", true)]
    [InlineData(" Station\r\n", true)]
    [InlineData("altx", false)]
    [InlineData("station --max-seconds 900", false)]  // invalid: ignored, default, cannot smuggle options
    [InlineData("", false)]
    public async Task Tx_path_setting_adds_sta_data_only_for_station(string content, bool expected)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_rig.Config.TxPathFile)!);
        File.WriteAllText(_rig.Config.TxPathFile, content);
        await RunToEnd(2);

        IReadOnlyList<string> args = _rig.Runner.Started[0].Args;
        Assert.Equal(expected, args.Contains("--sta-data"));
        Assert.Equal(expected ? 15 : 14, args.Count);
        Assert.Equal(content.Trim() is not ("station" or "Station" or "altx"), _rig.Log.Has("ignoring"));
    }

    [Fact]
    public async Task Firewall_rule_is_for_the_tray_app_only()
    {
        string? allowed = null;
        _rig.Network.OnCall = _ => allowed ??= _rig.Firewall.AllowedProgram;
        await RunToEnd();
        Assert.Equal(_rig.Config.TrayApp, allowed);
    }

    [Fact]
    public async Task Too_little_memory_changes_nothing()
    {
        _rig.Runner.DmaProbeExit = 1;
        ControlResponse end = await RunToEnd();

        Assert.Contains("not enough free memory", end.Error);
        Assert.DoesNotContain("driver:bind", _rig.Calls);
        Assert.DoesNotContain("watchdog:arm", _rig.Calls);
        Assert.DoesNotContain("state:taken", _rig.Calls);
        Assert.Empty(_rig.Runner.Started);
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
    }

    [Fact]
    public async Task Connected_wifi_without_an_exported_network_changes_nothing()
    {
        _rig.Wifi.Exported = 0;
        ControlResponse end = await RunToEnd();

        Assert.Contains("could not be exported", end.Error);
        Assert.DoesNotContain("watchdog:arm", _rig.Calls);
        Assert.DoesNotContain("driver:bind", _rig.Calls);
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
    }

    [Fact]
    public async Task Failed_export_changes_nothing()
    {
        _rig.Wifi.ExportFails = true;
        ControlResponse end = await RunToEnd();

        Assert.Contains("could not save the Wi-Fi networks", end.Error);
        Assert.DoesNotContain("state:taken", _rig.Calls);
        Assert.DoesNotContain("driver:bind", _rig.Calls);
    }

    [Fact]
    public async Task Disconnected_wifi_without_saved_networks_may_receive()
    {
        _rig.Wifi.Exported = 0;
        _rig.Wifi.Connected = false;
        ControlResponse end = await RunToEnd();

        Assert.Null(end.Error);
        Assert.Contains("driver:bind", _rig.Calls);
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
        Assert.True(_rig.Log.Has("no saved Wi-Fi networks were exported"));
    }

    [Fact]
    public async Task Refuses_when_the_card_is_not_on_the_vendor_driver()
    {
        _rig.Driver.Bound = FakeDriver.Binding.None;
        ControlResponse end = await RunToEnd();

        Assert.Contains("not on the MediaTek driver", end.Error);
        Assert.DoesNotContain("driver:bind", _rig.Calls);
    }

    [Fact]
    public async Task Refuses_when_a_file_is_missing()
    {
        File.Delete(_rig.Config.Awdld);
        ControlResponse end = await RunToEnd();

        Assert.Contains("missing awdld.exe", end.Error);
        Assert.DoesNotContain("driver:add", _rig.Calls);
    }

    [Fact]
    public async Task A_failed_bind_still_gives_the_card_back()
    {
        _rig.Driver.FailBind = true;
        ControlResponse end = await RunToEnd();

        Assert.Contains("refused to put the mtdrop driver", end.Error);
        Assert.Contains("driver:delete oem99.inf", _rig.Calls);
        Assert.False(_rig.Driver.PackageInStore);
        Assert.False(_rig.State.CardTaken);
        Assert.Null(_rig.Watchdog.ArmedFor);
        Assert.Empty(_rig.Runner.Started);
    }

    [Fact]
    public async Task Radio_ending_early_ends_the_session_and_gives_the_card_back()
    {
        _rig.Network.UpOnCall = 0;
        _rig.Network.OnCall = n =>
        {
            if (n == 5) { FakeChild r = _rig.Runner.Children["radiod"]; r.HasExited = true; r.ExitCode = 3; }
        };
        ControlResponse end = await RunToEnd();

        Assert.Contains("the radio stopped (exit code 3)", end.Error);
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
        Assert.False(_rig.State.CardTaken);
        Assert.True(_rig.Runner.Children["awdld"].Killed);
    }

    [Fact]
    public async Task Awdld_ending_early_ends_the_session()
    {
        _rig.Network.OnCall = n =>
        {
            if (n == 4) { FakeChild a = _rig.Runner.Children["awdld"]; a.HasExited = true; a.ExitCode = 1; }
        };
        ControlResponse end = await RunToEnd();

        Assert.Contains("AWDL daemon stopped", end.Error);
        Assert.False(_rig.State.CardTaken);
    }

    [Fact]
    public async Task Stop_request_ends_the_session_early()
    {
        DateTime start = _rig.Clock.UtcNow;
        _rig.Network.OnCall = n => { if (n == 10) _rig.Session.Stop(); };
        ControlResponse end = await RunToEnd(10);

        Assert.Null(end.Error);
        Assert.True(_rig.Log.Has("stopped on request"));
        Assert.True(_rig.Clock.UtcNow - start < TimeSpan.FromMinutes(5));
        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
    }

    [Fact]
    public async Task Radiod_that_does_not_end_is_killed()
    {
        _rig.Network.OnCall = n => { if (n == 1) _rig.Runner.Children["radiod"].EndsWhenWaitedFor = false; };
        await RunToEnd();

        Assert.True(_rig.Runner.Children["radiod"].Killed);
        Assert.True(_rig.Log.Has("radiod did not end within 15 s"));
        Assert.False(_rig.State.CardTaken);
    }

    [Fact]
    public async Task Failed_give_back_leaves_watchdog_and_state_file_for_the_retry()
    {
        _rig.Driver.VendorReturnsAfterDelete = false;   // and no vendor backup in DataDir
        ControlResponse end = await RunToEnd();

        Assert.Contains("could not be restored automatically", end.Error);
        Assert.True(_rig.State.CardTaken);
        Assert.NotNull(_rig.Watchdog.ArmedFor);
        Assert.Equal(SessionState.Idle, end.State);
    }

    [Fact]
    public async Task Second_start_while_running_is_refused()
    {
        ControlResponse second = default!;
        _rig.Network.OnCall = n => { if (n == 2) second = _rig.Session.Start(2); };
        await RunToEnd();

        Assert.False(second.Ok);
        Assert.Contains("already running", second.Error);
        Assert.Single(_rig.Calls, "driver:bind");
    }

    [Theory]
    [InlineData(0)]
    [InlineData(11)]
    [InlineData(-5)]
    public void Minutes_out_of_range_are_refused(int minutes)
    {
        ControlResponse r = _rig.Session.Start(minutes);
        Assert.False(r.Ok);
        Assert.Equal(SessionState.Idle, r.State);
        Assert.Empty(_rig.Calls);
    }

    [Fact]
    public async Task Status_reports_seconds_left_while_receiving()
    {
        ControlResponse? during = null;
        _rig.Network.OnCall = n => { if (n == 6) during = _rig.Session.Status(); };
        await RunToEnd(2);

        Assert.NotNull(during);
        Assert.Equal(SessionState.Receiving, during.State);
        Assert.InRange(during.SecondsLeft, 100, 120);
    }

    [Fact]
    public async Task Recovery_gives_back_a_card_left_taken()
    {
        _rig.State.CardTaken = true;
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;

        await _rig.Session.RecoverAsync().WaitAsync(TimeSpan.FromSeconds(20));

        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
        Assert.False(_rig.State.CardTaken);
        Assert.Contains("kill-strays", _rig.Calls);
        Assert.Equal(SessionState.Idle, _rig.Session.Status().State);
    }

    [Fact]
    public async Task Recovery_gives_back_a_proxy_bound_card_even_without_the_state_file()
    {
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;

        await _rig.Session.RecoverAsync().WaitAsync(TimeSpan.FromSeconds(20));

        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
    }

    [Fact]
    public async Task Recovery_does_nothing_when_the_card_is_on_the_vendor_driver()
    {
        await _rig.Session.RecoverAsync().WaitAsync(TimeSpan.FromSeconds(20));

        Assert.DoesNotContain(_rig.Calls, c => c.StartsWith("driver:", StringComparison.Ordinal));
        Assert.Null(_rig.Session.Status().Error);
    }

    [Fact]
    public async Task Start_is_refused_while_recovery_runs()
    {
        _rig.State.CardTaken = true;
        _rig.Driver.Bound = FakeDriver.Binding.Proxy;
        _rig.Driver.PackageInStore = true;
        Task recovery = _rig.Session.RecoverAsync();
        ControlResponse r = _rig.Session.Start(2);
        await recovery.WaitAsync(TimeSpan.FromSeconds(20));

        if (!r.Ok) Assert.Contains("already running", r.Error);
        else Assert.Equal(SessionState.Preparing, r.State);   // recovery had already finished
    }

    [Fact]
    public async Task Shutdown_mid_session_gives_the_card_back()
    {
        _rig.Network.OnCall = n => { if (n == 3) _ = _rig.Session.ShutdownAsync(TimeSpan.FromSeconds(10)); };
        ControlResponse end = await RunToEnd(10);

        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
        Assert.False(_rig.State.CardTaken);
        Assert.Equal(SessionState.Idle, end.State);
    }

    [Fact]
    public async Task System_shutdown_stops_the_radio_and_leaves_the_card_for_boot_recovery()
    {
        _rig.Network.OnCall = n => { if (n == 3) _ = _rig.Session.ShutdownForSystemAsync(TimeSpan.FromSeconds(4)); };
        ControlResponse end = await RunToEnd(10);

        Assert.True(_rig.Runner.Children["awdld"].Killed);
        Assert.True(_rig.Runner.Children["radiod"].HasExited);
        Assert.Null(_rig.Firewall.AllowedProgram);
        Assert.Equal(FakeDriver.Binding.Proxy, _rig.Driver.Bound);       // no give-back during shutdown
        Assert.Equal(-1, _rig.IndexOf("driver:delete oem99.inf"));
        Assert.True(_rig.State.CardTaken);                                 // boot recovery will see it
        Assert.NotNull(_rig.Watchdog.ArmedFor);                            // and the watchdog stays armed
        Assert.Equal(0, _rig.Lock.Held);
        Assert.Equal(SessionState.Idle, end.State);
    }

    [Fact]
    public async Task Radio_ending_just_before_the_shutdown_notice_still_leaves_the_card()
    {
        // F-027 T5: Windows ends radiod about a second before the service hears of the shutdown.
        _rig.Network.OnCall = n =>
        {
            if (n != 3) return;
            _rig.Runner.Children["radiod"].HasExited = true;
            _rig.Runner.Children["radiod"].ExitCode = 0;
            _ = _rig.Session.ShutdownForSystemAsync(TimeSpan.FromSeconds(4));
        };
        await RunToEnd(10);

        Assert.Equal(FakeDriver.Binding.Proxy, _rig.Driver.Bound);
        Assert.True(_rig.State.CardTaken);
        Assert.True(_rig.Log.Has("card stays on our driver"));
    }

    [Fact]
    public async Task Recovery_after_a_shutdown_mid_session_gives_the_card_back()
    {
        _rig.Network.OnCall = n => { if (n == 3) _ = _rig.Session.ShutdownForSystemAsync(TimeSpan.FromSeconds(4)); };
        await RunToEnd(10);

        Session booted = _rig.NewSession();     // next boot: a new service instance on the same machine state
        await booted.RecoverAsync().WaitAsync(TimeSpan.FromSeconds(20));

        Assert.Equal(FakeDriver.Binding.Vendor, _rig.Driver.Bound);
        Assert.False(_rig.State.CardTaken);
        Assert.Null(_rig.Watchdog.ArmedFor);
        Assert.Equal(SessionState.Idle, booted.Status().State);
    }
}
