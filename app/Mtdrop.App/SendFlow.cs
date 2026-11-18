using System.Diagnostics;
using System.IO;
using System.Net;
using System.Security.Cryptography.X509Certificates;
using Mtdrop.Protocol;

namespace Mtdrop.App;

/// <summary>What became of one send.</summary>
/// <param name="ReceiverName">Name the receiver gave in /Discover (self-reported); null if none was chosen.</param>
public sealed record SendSummary(string? ReceiverName, SendResult? Result);

/// <summary>
/// One send on a radio session that is already up (ADR-0005): Bluetooth wake, browse for <c>_airdrop._tcp</c> on
/// awdl0, ask each receiver for its name (/Discover), let the user confirm the receiver, then /Ask + /Upload.
/// The user always confirms the receiver: other people's iPhones nearby wake up too.
/// Receiver names are never written to the log (they are often a person's name): <paramref name="status"/> gets the
/// text for the tray and a second text without the name for the log.
/// </summary>
public sealed class SendFlow(
    InterfaceInfo nic,
    IReadOnlyList<OutgoingFile> files,
    string senderName,
    string dataDirectory,
    Func<string, string?, Task<bool>> confirmReceiver,
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

        var nextTry = new Dictionary<string, TimeSpan>(StringComparer.OrdinalIgnoreCase);   // by service id
        var refused = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        int receivers = 0;
        while (true)
        {
            ct.ThrowIfCancellationRequested();
            foreach (AirDropServiceEndpoint e in browser.Current)
            {
                if (e.ServiceId.Equals(ourId, StringComparison.OrdinalIgnoreCase) || refused.Contains(e.ServiceId)) continue;
                if (nextTry.TryGetValue(e.ServiceId, out TimeSpan at) && clock.Elapsed < at) continue;
                if (!nextTry.ContainsKey(e.ServiceId))
                    log($"send: receiver #{++receivers} announced {clock.Elapsed.TotalSeconds:0.0} s after the advert started ({e.Txt})");
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
                    refused.Add(e.ServiceId);
                    continue;
                }
                log($"send: /Discover answered {clock.Elapsed.TotalSeconds:0.0} s after the advert started (model {who.Value.Model ?? "?"})");
                if (!await confirmReceiver(who.Value.Name, who.Value.Model))
                {
                    log("send: the user did not choose this receiver");
                    refused.Add(e.ServiceId);
                    continue;
                }

                status($"Waiting for {who.Value.Name} to accept", "Waiting for the receiver to accept");
                long sent = 0, total = Math.Max(1, files.Sum(f => f.Size));
                int lastPercent = -1;
                var progress = new ActionProgress(n =>
                {
                    int percent = (int)(100 * Interlocked.Add(ref sent, n) / total);
                    if (percent == Interlocked.Exchange(ref lastPercent, percent)) return;
                    status($"Sending to {who.Value.Name}: {percent} %", $"Sending: {percent} %");
                });
                SendResult result = await client.SendAsync(e.EndPoint, files, progress, ct);
                log($"send: {result.Outcome} after {clock.Elapsed.TotalSeconds:0.0} s ({result.Detail})");
                return new SendSummary(who.Value.Name, result);
            }
            await Task.Delay(TimeSpan.FromMilliseconds(500), ct);
        }
    }

    private sealed class ActionProgress(Action<long> report) : IProgress<long>
    {
        public void Report(long value) => report(value);
    }
}
