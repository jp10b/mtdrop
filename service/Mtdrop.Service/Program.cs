// mtdrop service (ADR-0004): runs receive sessions on request of the tray app and always gives the Wi-Fi card back.
// Usage:
//   mtdrop-service.exe               run as the Windows service "mtdrop" (registered by scripts/install-service.ps1)
//   mtdrop-service.exe --console     the same in a console, for debugging (elevated; stop with Ctrl+C)
//   mtdrop-service.exe --give-back   give the card back once and exit (watchdog task, manual recovery); exit 0 = Wi-Fi back
//   mtdrop-service.exe --check       read-only: card driver, our packages in the driver store, required files, awdl0

using System.ServiceProcess;
using Mtdrop.Service;
using Mtdrop.Service.Windows;

ServiceConfig config = ServiceConfig.Default();

if (args.Contains("--check"))
{
    var log = new ConsoleOnlyLog();
    Host host = Host.Create(config, log);
    CardInfo? card = host.Driver.FindCard();
    Console.WriteLine($"install dir:      {config.InstallDir}");
    Console.WriteLine($"data dir:         {config.DataDir}");
    Console.WriteLine($"card:             {card?.InstanceId ?? "not found"}");
    Console.WriteLine($"card driver:      {card?.ToString() ?? "-"}");
    string packages = string.Join(", ", host.Driver.OurPackages());
    Console.WriteLine($"proxy packages:   {(packages.Length > 0 ? packages : "none")}");
    Console.WriteLine($"on vendor driver: {host.GiveBack.IsVendorBound(card)}");
    Console.WriteLine($"state file:       {(host.State.CardTaken ? "card TAKEN" : "clear")}");
    Console.WriteLine($"awdl0 up:         {host.Network.AwdlUp()}");
    Console.WriteLine($"Wi-Fi connected:  {host.Wifi.AdapterConnected()}");
    foreach (string f in config.RequiredFiles.Append(config.TrayApp))
        Console.WriteLine($"{(File.Exists(f) ? "ok     " : "MISSING")} {f}");
    return 0;
}

if (args.Contains("--give-back"))
{
    var log = new FileLog(Path.Combine(config.LogDir, "service.log"), echo: true);
    log.Info("=== give-back (watchdog task or manual) ===");
    Host host = Host.Create(config, log);
    host.Runner.KillStrays([config.Radiod, config.Awdld]);
    host.Firewall.RemoveReceiverRules();
    bool ok;
    try { ok = host.GiveBack.Run(); }
    catch (Exception e) { log.Warn($"give-back threw: {e.Message}"); ok = false; }
    if (ok) host.State.Clear();
    log.Info($"give-back result: {(ok ? "OK" : "FAILED")}");
    return ok ? 0 : 1;
}

if (args.Contains("--console"))
{
    var log = new FileLog(Path.Combine(config.LogDir, "service.log"), echo: true);
    using var service = new MtdropService(config, log);
    service.StartInConsole();
    var done = new TaskCompletionSource();
    Console.CancelKeyPress += (_, e) => { e.Cancel = true; done.TrySetResult(); };
    await done.Task;
    service.StopInConsole();
    return 0;
}

if (Environment.UserInteractive)
{
    Console.Error.WriteLine("mtdrop-service runs as the Windows service \"mtdrop\". Options: --check | --console | --give-back");
    return 2;
}

ServiceBase.Run(new MtdropService(config, new FileLog(Path.Combine(config.LogDir, "service.log"), echo: false)));
return 0;

/// <summary>--check prints its own lines; anything the platform code logs goes to the console too.</summary>
internal sealed class ConsoleOnlyLog : ILog
{
    public void Info(string message) => Console.WriteLine($"  log: {message}");
    public void Warn(string message) => Console.WriteLine($"  log WARN: {message}");
}
