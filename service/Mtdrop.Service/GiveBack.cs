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

        foreach (string package in driver.OurPackages())
        {
            int code = driver.DeletePackage(package);
            log.Info($"give-back: removed proxy package {package} (pnputil exit {code})");
            if (code == 3010) log.Warn("give-back: Windows asks for a restart to finish removing the proxy driver");
        }
        driver.ScanDevices();
        card = WaitForVendor();

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
        int missing = wifi.RestoreMissingProfiles();
        log.Info(missing == 0 ? "give-back: saved Wi-Fi networks are all present" : $"give-back: re-added {missing} saved network(s)");
        DateTime until = clock.UtcNow.AddSeconds(30);
        while (!wifi.AdapterConnected() && clock.UtcNow < until) clock.Sleep(TimeSpan.FromSeconds(2));
        log.Info(wifi.AdapterConnected() ? "give-back: Wi-Fi is connected" : "give-back: Wi-Fi adapter is back but not connected yet");
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
