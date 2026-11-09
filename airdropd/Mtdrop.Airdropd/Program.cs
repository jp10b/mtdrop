// Development host for the AirDrop-compatible receiver: console prompts instead of a toast.
// Usage:
//   Mtdrop.Airdropd --interface <name-or-index> [--dir <folder>] [--name <shown name>] [--port 8771] [--yes]
//   Mtdrop.Airdropd --list-interfaces
// The tray app (app/) replaces this for normal use.

using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;
using Mtdrop.Protocol;

string? Arg(string name)
{
    int i = Array.IndexOf(args, name);
    return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
}

static IEnumerable<(NetworkInterface Nic, int Index, IPAddress? LinkLocal)> Interfaces()
{
    foreach (NetworkInterface nic in NetworkInterface.GetAllNetworkInterfaces())
    {
        IPv6InterfaceProperties? v6;
        try { v6 = nic.GetIPProperties().GetIPv6Properties(); }
        catch (NetworkInformationException) { continue; }
        if (v6 is null) continue;
        IPAddress? ll = nic.GetIPProperties().UnicastAddresses
            .Select(a => a.Address)
            .FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetworkV6 && (a.IsIPv6LinkLocal || IPAddress.IsLoopback(a)));
        yield return (nic, v6.Index, ll);
    }
}

if (args.Contains("--list-interfaces"))
{
    foreach (var (nic, index, ll) in Interfaces())
        Console.WriteLine($"{index,4}  {nic.OperationalStatus,-6} {(ll is null ? "no IPv6 link-local" : "IPv6 link-local ok"),-20} {nic.Name}");
    return 0;
}

string? wanted = Arg("--interface");
if (wanted is null)
{
    Console.Error.WriteLine("Missing --interface <name-or-index>. Use --list-interfaces to see candidates.");
    return 2;
}
var match = Interfaces().FirstOrDefault(i =>
    i.Index.ToString() == wanted || i.Nic.Name.Equals(wanted, StringComparison.OrdinalIgnoreCase));
if (match.Nic is null || match.LinkLocal is null)
{
    Console.Error.WriteLine($"Interface '{wanted}' not found or it has no IPv6 link-local address yet.");
    return 2;
}

string dataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "mtdrop");
string destination = Arg("--dir") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
string shownName = Arg("--name") ?? Environment.MachineName;
int port = int.TryParse(Arg("--port"), out int p) ? p : 8771;
bool autoAccept = args.Contains("--yes");

Directory.CreateDirectory(dataDir);
string idFile = Path.Combine(dataDir, "service-id");
string serviceId = File.Exists(idFile) ? File.ReadAllText(idFile).Trim() : "";
if (!System.Text.RegularExpressions.Regex.IsMatch(serviceId, "^[0-9a-f]{12}$"))
{
    serviceId = AirDropServiceRecords.NewServiceId();
    File.WriteAllText(idFile, serviceId);
}
X509Certificate2 certificate = ServerIdentity.LoadOrCreate(Path.Combine(dataDir, "identity.pfx"));

void Log(string m) => Console.WriteLine($"{DateTime.Now:HH:mm:ss} {m}");
var consoleLock = new SemaphoreSlim(1);

await using var server = new AirDropServer(new AirDropServerOptions
{
    ComputerName = shownName,
    DestinationDirectory = destination,
    Port = port,
    BindAddress = IPAddress.IsLoopback(match.LinkLocal) ? IPAddress.IPv6Loopback : IPAddress.IPv6Any,
    RequiredScopeId = IPAddress.IsLoopback(match.LinkLocal) ? null : match.Index,
    Log = Log,
    Consent = async (ask, ct) =>
    {
        string what = ask.Files.Count == 1
            ? $"{ask.Files[0].Name} ({ask.Files[0].Size:N0} bytes)"
            : $"{ask.Files.Count} files ({ask.TotalSize:N0} bytes)";
        Log($"\"{ask.SenderName}\" wants to send {what}");
        if (autoAccept) return true;
        await consoleLock.WaitAsync(ct);
        try
        {
            Console.Write("Accept? [y/N] ");
            string? answer = await Task.Run(Console.ReadLine, ct);
            return answer?.Trim().ToLowerInvariant() is "y" or "yes";
        }
        finally { consoleLock.Release(); }
    },
}, certificate);
server.TransferFinished += r =>
{
    if (r.Succeeded) foreach (string f in r.SavedFiles) Log($"saved {f}");
};
server.Start();

await using var mdns = new MdnsResponder(new AirDropServiceRecords
{
    ServiceId = serviceId,
    Address = match.LinkLocal,
    Port = server.Port,
}, match.Index, log: Log);
mdns.Start();

Log($"receiving as \"{shownName}\" on {match.Nic.Name} (index {match.Index}), port {server.Port}; saving to {destination}");
Log(autoAccept ? "auto-accept is ON (--yes): every transfer is accepted" : "each transfer asks for confirmation");
Log("press Ctrl+C to stop");

var done = new TaskCompletionSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; done.TrySetResult(); };
AppDomain.CurrentDomain.ProcessExit += (_, _) => done.TrySetResult();
if (int.TryParse(Arg("--run-seconds"), out int seconds)) _ = Task.Delay(TimeSpan.FromSeconds(seconds)).ContinueWith(_ => done.TrySetResult());
await done.Task;
Log("stopping");
return 0;
