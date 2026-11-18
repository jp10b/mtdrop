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

        // Fast path (M9 B1, F-032): switch the card straight back to the MediaTek package already in the driver store,
        // the way the session switched it to ours (~3 s), and remove our package only once Wi-Fi is back. The old
        // path - delete our package with /uninstall, which reinstalls the card (~15 s) - stays as the fallback.
        bool fast = false;
        if (IsProxyBound(card) && driver.VendorPackages() is [string vendor, ..])
        {
            log.Info($"give-back: switching the card back to {vendor}");
            if (driver.BindVendor(vendor))
            {
                card = WaitForVendor();
                fast = IsVendorBound(card);
            }
            if (!fast) log.Warn("give-back: the quick switch did not bring the MediaTek driver back; removing our package instead");
        }
        if (!fast)
        {
            foreach (string package in driver.OurPackages())
            {
                int code = driver.DeletePackage(package, uninstall: true);
                log.Info($"give-back: removed proxy package {package} (pnputil exit {code})");
                if (code == 3010) log.Warn("give-back: Windows asks for a restart to finish removing the proxy driver");
            }
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

        clock.Sleep(TimeSpan.FromSeconds(3));
        ProfileRestore profiles = wifi.RestoreMissingProfiles();
        if (profiles.Saved == 0) log.Warn("give-back: no saved Wi-Fi networks were exported; Wi-Fi may need a manual connect");
        else if (profiles.Missing == 0) log.Info($"give-back: all {profiles.Saved} saved Wi-Fi network(s) are present");
        else log.Info($"give-back: re-added {profiles.Restored} of {profiles.Missing} missing saved network(s) ({profiles.Saved} exported)");
        if (profiles.Failed > 0) log.Warn($"give-back: {profiles.Failed} saved network(s) could not be re-added");
        // Normally Windows connects 2-5 s after the networks are back (F-026). After an abrupt end of radiod it did
        // not try at all (F-027), so nudge it the way the user would, then wait again.
        if (!WaitConnected(TimeSpan.FromSeconds(12)))
        {
            log.Info($"give-back: Wi-Fi did not connect by itself; {wifi.NudgeConnect()}");
            WaitConnected(TimeSpan.FromSeconds(25));
        }
        log.Info(wifi.AdapterConnected() ? "give-back: Wi-Fi is connected" : "give-back: Wi-Fi adapter is back but not connected yet");
        // Our package is no longer on any device: remove it from the store without touching the card.
        foreach (string package in driver.OurPackages())
        {
            int code = driver.DeletePackage(package, uninstall: false);
            log.Info($"give-back: removed proxy package {package} from the driver store (pnputil exit {code})");
        }
        return true;
    }

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
