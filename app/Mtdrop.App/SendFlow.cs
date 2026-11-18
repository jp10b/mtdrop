using System.Diagnostics;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using Mtdrop.Protocol;

namespace Mtdrop.App;

/// <summary>What became of one send.</summary>
/// <param name="ReceiverName">Name the receiver gave in /Discover (self-reported); null if none was chosen.</param>
/// <param name="Result">Null when no receiver was chosen (cancelled, or the window ended).</param>
public sealed record SendSummary(string? ReceiverName, SendResult? Result);

/// <summary>Where the user sees the receivers found and chooses one (<see cref="ReceiverWindow"/>).</summary>
public interface IReceiverPicker
{
    void Add(string key, string name, string? model);
    void SetStatus(string text);
    /// <summary>Key of the chosen receiver, or null when the user cancelled.</summary>
    Task<string?> Choice { get; }
}

/// <summary>
/// One send on a radio session that is already up (ADR-0005): Bluetooth wake, browse for <c>_airdrop._tcp</c> on
/// awdl0, ask each receiver for its name (/Discover) and list it, wait for the user's choice, then /Ask + /Upload.
/// Nothing is sent before the user chose a receiver: other people's iPhones nearby wake up too.
/// Receiver names are never written to the log (they are often a person's name): <paramref name="status"/> gets the
/// text for the tray and a second text without the name for the log.
/// </summary>
public sealed class SendFlow(
    InterfaceInfo nic,
    IReadOnlyList<OutgoingFile> files,
    string senderName,
    string dataDirectory,
    IReceiverPicker picker,
    Action<string, string> status,
    Action<string> log)
{
    private static readonly TimeSpan Retry = TimeSpan.FromSeconds(3);

    public async Task<SendSummary> RunAsync(CancellationToken ct)
    {
        X509Certificate2 certificate = ServerIdentity.LoadOrCreate(Path.Combine(dataDirectory, "identity.pfx"));
        string ourId = ReceiveSession.LoadOrCreateServiceId(Path.Combine(dataDirectory, "service-id"));
        var client = new AirDropClient(new AirDropClientOptions
        {
            SenderName = senderName,
            SenderId = ourId,
            Log = m => log($"sender: {m}"),
        }, certificate);

        var clock = Stopwatch.StartNew();
        using var ble = new BleWake(log);
        ble.Start();
        await using var browser = new MdnsBrowser(nic.Index, log: m => log($"browse: {m}"));
        browser.Start();
        status("Looking for iPhones nearby", "Looking for iPhones nearby");
        log($"send: {files.Count} file(s), {files.Sum(f => f.Size)} bytes; browsing on {nic.Name} (index {nic.Index})");

        // ---- find receivers and list them until the user chooses one --------------------------------------
        var nextTry = new Dictionary<string, TimeSpan>(StringComparer.OrdinalIgnoreCase);              // by service id
        var named = new Dictionary<string, (AirDropServiceEndpoint Endpoint, string Name)>(StringComparer.OrdinalIgnoreCase);
        var hidden = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        int announced = 0;
        while (!picker.Choice.IsCompleted)
        {
            ct.ThrowIfCancellationRequested();
            foreach (AirDropServiceEndpoint e in browser.Current)
            {
                if (picker.Choice.IsCompleted) break;
                if (e.ServiceId.Equals(ourId, StringComparison.OrdinalIgnoreCase) || hidden.Contains(e.ServiceId)) continue;
                if (named.TryGetValue(e.ServiceId, out var known))
                {
                    if (!known.Endpoint.EndPoint.Equals(e.EndPoint)) named[e.ServiceId] = (e, known.Name);   // moved
                    continue;
                }
                if (nextTry.TryGetValue(e.ServiceId, out TimeSpan at) && clock.Elapsed < at) continue;
                if (!nextTry.ContainsKey(e.ServiceId))
                    log($"send: receiver #{++announced} announced {clock.Elapsed.TotalSeconds:0.0} s after the advert started ({e.Txt})");
                nextTry[e.ServiceId] = clock.Elapsed + Retry;

                (string Name, string? Model)? who;
                try
                {
                    who = await client.DiscoverAsync(e.EndPoint, ct);
                }
                catch (Exception ex) when (ex is IOException or System.Net.Sockets.SocketException or TimeoutException
                                           or System.Security.Authentication.AuthenticationException or ProtocolFormatException)
                {
                    log($"send: /Discover failed ({ex.GetType().Name}: {ex.Message}); retrying in {Retry.TotalSeconds:0} s");
                    continue;
                }
                if (who is null)
                {
                    log("send: a receiver answered /Discover without a name (not visible to us; Contacts Only?)");
                    hidden.Add(e.ServiceId);
                    continue;
                }
                log($"send: receiver listed {clock.Elapsed.TotalSeconds:0.0} s after the advert started (model {who.Value.Model ?? "?"})");
                named[e.ServiceId] = (e, who.Value.Name);
                picker.Add(e.ServiceId, who.Value.Name, who.Value.Model);
            }
            await Task.WhenAny(picker.Choice, Task.Delay(TimeSpan.FromMilliseconds(500), ct));
        }

        string? key = await picker.Choice;
        if (key is null || !named.TryGetValue(key, out var target))
        {
            log("send: no receiver chosen");
            return new SendSummary(null, null);
        }

        // ---- the send itself --------------------------------------------------------------------------------
        log($"send: receiver chosen {clock.Elapsed.TotalSeconds:0.0} s after the advert started");
        status($"Waiting for {target.Name} to accept", "Waiting for the receiver to accept");
        long sent = 0, total = Math.Max(1, files.Sum(f => f.Size));
        int lastPercent = -1;
        var progress = new ActionProgress(n =>
        {
            int percent = (int)(100 * Interlocked.Add(ref sent, n) / total);
            if (percent == Interlocked.Exchange(ref lastPercent, percent)) return;
            status($"Sending to {target.Name}: {percent} %", $"Sending: {percent} %");
        });
        using var probeStop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        Task<string> rtt = ProbeRttAsync(target.Endpoint.EndPoint.Address, probeStop.Token);
        SendResult result = await client.SendAsync(target.Endpoint.EndPoint, files, progress, ct);
        probeStop.Cancel();
        log($"send: {result.Outcome} after {clock.Elapsed.TotalSeconds:0.0} s ({result.Detail}); {await rtt}");
        return new SendSummary(target.Name, result);
    }

    /// <summary>
    /// Pings the receiver once a second during /Ask and /Upload (F-031): the round trip over AWDL bounds TCP when the
    /// phone shares our channel only part of its sequence (F-030). One small echo per second; summary for the log.
    /// </summary>
    private static async Task<string> ProbeRttAsync(System.Net.IPAddress address, CancellationToken ct)
    {
        var times = new List<long>();
        int lost = 0;
        using var ping = new System.Net.NetworkInformation.Ping();
        try
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    var reply = await ping.SendPingAsync(address, TimeSpan.FromSeconds(2), null, null, ct);
                    if (reply.Status == System.Net.NetworkInformation.IPStatus.Success) times.Add(reply.RoundtripTime);
                    else lost++;
                }
                catch (System.Net.NetworkInformation.PingException) { lost++; }
                await Task.Delay(TimeSpan.FromSeconds(1), ct);
            }
        }
        catch (OperationCanceledException) { }
        if (times.Count == 0) return $"RTT: no replies ({lost} lost)";
        times.Sort();
        return $"RTT during the transfer: {times.Count} replies, {lost} lost, min {times[0]} ms, median {times[times.Count / 2]} ms, " +
               $"max {times[^1]} ms";
    }

    private sealed class ActionProgress(Action<long> report) : IProgress<long>
    {
        public void Report(long value) => report(value);
    }
}
