// Development host for the AirDrop-compatible receiver: console prompts instead of a toast.
// Usage:
//   Mtdrop.Airdropd --interface <name-or-index> [--dir <folder>] [--name <shown name>] [--port 8771] [--yes]
//   Mtdrop.Airdropd --list-interfaces
// The tray app (app/) replaces this for normal use.

using Mtdrop.Protocol;

string? Arg(string name)
{
    int i = Array.IndexOf(args, name);
    return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
}

if (args.Contains("--list-interfaces"))
{
    foreach (InterfaceInfo i in ReceiveSession.ListInterfaces())
        Console.WriteLine($"{i.Index,4}  {(i.IsUp ? "up" : "down"),-5} {(i.LinkLocal is null ? "no IPv6 link-local" : "IPv6 link-local ok"),-20} {i.Name}");
    return 0;
}

string? wanted = Arg("--interface");
if (wanted is null)
{
    Console.Error.WriteLine("Missing --interface <name-or-index>. Use --list-interfaces to see candidates.");
    return 2;
}

string destination = Arg("--dir") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
string shownName = Arg("--name") ?? Environment.MachineName;
bool autoAccept = args.Contains("--yes");
var consoleLock = new SemaphoreSlim(1);

void Log(string m) => Console.WriteLine($"{DateTime.Now:HH:mm:ss} {m}");

ReceiveSession session;
try
{
    session = ReceiveSession.Start(new ReceiveSessionOptions
    {
        Interface = wanted,
        ComputerName = shownName,
        DestinationDirectory = destination,
        DataDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "mtdrop"),
        Port = int.TryParse(Arg("--port"), out int p) ? p : 8771,
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
    });
}
catch (Exception e) when (e is InvalidOperationException or System.Net.Sockets.SocketException)
{
    Console.Error.WriteLine(e.Message);
    return 2;
}

await using (session)
{
    session.TransferFinished += r =>
    {
        if (r.Succeeded) foreach (string f in r.SavedFiles) Log($"saved {f}");
    };
    Log($"receiving as \"{shownName}\" on {session.Interface.Name} (index {session.Interface.Index}), port {session.Port}; saving to {destination}");
    Log(autoAccept ? "auto-accept is ON (--yes): every transfer is accepted" : "each transfer asks for confirmation");
    Log("press Ctrl+C to stop");

    var done = new TaskCompletionSource();
    Console.CancelKeyPress += (_, e) => { e.Cancel = true; done.TrySetResult(); };
    if (int.TryParse(Arg("--run-seconds"), out int seconds))
        _ = Task.Delay(TimeSpan.FromSeconds(seconds)).ContinueWith(_ => done.TrySetResult());
    await done.Task;
    Log("stopping");
}
return 0;
