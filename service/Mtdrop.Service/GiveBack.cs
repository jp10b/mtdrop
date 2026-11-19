namespace Mtdrop.Service;

/// <summary>
/// Gives the Wi-Fi card back to the MediaTek driver. Port of scripts/wifi-giveback.ps1 (used in every hardware
/// session since F-004), which stays as the manual fallback. Safe to run at any time, any number of times.
/// </summary>
public sealed class GiveBack(ServiceConfig config, IDriverControl driver, IWifi wifi, ICardLock cardLock, IClock clock, ILog log)
{
    /// <summary>Returns true when the MediaTek driver is verified bound and started.</summary>
    public bool Run()
    {
        using IDisposable _ = cardLock.Acquire(TimeSpan.FromMinutes(3));
        CardInfo? card = driver.FindCard();
        log.Info($"give-back: card driver now: {card?.ToString() ?? "card not found"}");

        // Always remove our package with /uninstall: Windows takes the device down and enumerates it again, so the
        // MediaTek driver starts on a freshly reset function. Switching the driver on the running device instead
        // (UpdateDriverForPlugAndPlayDevices with the vendor INF, tried in F-032) took 33 s once and hung Windows the
        // next time - do not bring it back without a reset of the function first.
        foreach (string package in driver.OurPackages())
        {
            int code = driver.DeletePackage(package, uninstall: true);
            log.Info($"give-back: removed proxy package {package} (pnputil exit {code})");
            if (code == 3010) log.Warn("give-back: Windows asks for a restart to finish removing the proxy driver");
        }
        // pnputil /uninstall returns once the MediaTek driver has started on the device (F-037), so look first and
        // rescan only when it is not there.
        card = driver.FindCard();
        if (!IsVendorBound(card))
        {
            driver.ScanDevices();
            card = WaitForVendor();
        }

        if (!IsVendorBound(card))
        {
            string? inf = FindVendorInf();
            if (inf is null)
            {
                log.Warn("give-back: the MediaTek driver did not come back and its backup was not found");
                return false;
            }
            log.Warn("give-back: the MediaTek driver did not come back by itself; installing it from the backup");
            int code = driver.InstallVendorDriver(inf);
            log.Info($"give-back: vendor driver install exit {code}");
            driver.ScanDevices();
            card = WaitForVendor();
        }
        if (!IsVendorBound(card))
        {
            log.Warn($"give-back FAILED: card driver: {card?.ToString() ?? "card not found"}");
            return false;
        }
        log.Info($"give-back: card driver back: {card}");

        // No fixed pause: RestoreMissingProfiles waits for the Wi-Fi interface itself.
        ProfileRestore profiles = wifi.RestoreMissingProfiles();
        if (profiles.Saved == 0) log.Warn("give-back: no saved Wi-Fi networks were exported; Wi-Fi may need a manual connect");
        else if (profiles.Missing == 0) log.Info($"give-back: all {profiles.Saved} saved Wi-Fi network(s) are present");
        else log.Info($"give-back: re-added {profiles.Restored} of {profiles.Missing} missing saved network(s) ({profiles.Saved} exported)");
        if (profiles.Failed > 0) log.Warn($"give-back: {profiles.Failed} saved network(s) could not be re-added");
        // Windows connected 2-5 s after the networks were back in F-026, but not at all after an abrupt end of radiod
        // (F-027), after networks re-added right away (F-037: nudged after 16 s, connected 1 s later) or after a plain
        // disable/enable of the card (F-037 baseline: ~100 s). Nudge it the way the user would after a short wait.
        if (!WaitConnected(NudgeAfter))
        {
            log.Info($"give-back: Wi-Fi did not connect by itself; {wifi.NudgeConnect()}");
            WaitConnected(TimeSpan.FromSeconds(25));
        }
        log.Info(wifi.AdapterConnected() ? "give-back: Wi-Fi is connected" : "give-back: Wi-Fi adapter is back but not connected yet");
        return true;
    }

    /// <summary>How long Windows gets to connect by itself before the nudge (F-037).</summary>
    public static readonly TimeSpan NudgeAfter = TimeSpan.FromSeconds(3);

    private bool WaitConnected(TimeSpan wait)
    {
        DateTime until = clock.UtcNow + wait;
        while (!wifi.AdapterConnected())
        {
            if (clock.UtcNow >= until) return false;
            clock.Sleep(TimeSpan.FromSeconds(2));
        }
        return true;
    }

    /// <summary>The card runs on a MediaTek driver package that is not ours.</summary>
    public bool IsVendorBound(CardInfo? card) =>
        card is { Started: true } &&
        card.Provider?.StartsWith("MediaTek", StringComparison.OrdinalIgnoreCase) == true &&
        !driver.OurPackages().Contains(card.InfName ?? "", StringComparer.OrdinalIgnoreCase);

    /// <summary>Our proxy package is the one bound to the card.</summary>
    public bool IsProxyBound(CardInfo? card) =>
        card?.InfName is { } inf && driver.OurPackages().Contains(inf, StringComparer.OrdinalIgnoreCase);

    private CardInfo? WaitForVendor()
    {
        DateTime until = clock.UtcNow + config.VendorWait;
        CardInfo? card;
        while (true)
        {
            clock.Sleep(TimeSpan.FromMilliseconds(700));
            card = driver.FindCard();
            if (IsVendorBound(card) || clock.UtcNow >= until) return card;
        }
    }

    private string? FindVendorInf()
    {
        if (!Directory.Exists(config.VendorBackupDir)) return null;
        return Directory.EnumerateFiles(config.VendorBackupDir, ServiceConfig.VendorInfName, SearchOption.AllDirectories).FirstOrDefault();
    }
}
