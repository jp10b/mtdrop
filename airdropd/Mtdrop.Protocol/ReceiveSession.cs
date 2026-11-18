using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;
using System.Text.RegularExpressions;

namespace Mtdrop.Protocol;

public sealed record InterfaceInfo(string Name, int Index, bool IsUp, IPAddress? LinkLocal, bool IsLoopback);

public sealed class ReceiveSessionOptions
{
    /// <summary>Interface name (as shown by Windows) or numeric IPv6 interface index.</summary>
    public required string Interface { get; init; }
    public required string ComputerName { get; init; }
    public required string DestinationDirectory { get; init; }

    /// <summary>Where the TLS identity and the service id are kept between runs.</summary>
    public required string DataDirectory { get; init; }
    public required Func<AskRequest, CancellationToken, Task<bool>> Consent { get; init; }
    public int Port { get; init; } = 8771;
    public TimeSpan ConsentTimeout { get; init; } = TimeSpan.FromSeconds(60);
    /// <summary>See <see cref="AirDropServerOptions.DeclineStatus"/>.</summary>
    public int DeclineStatus { get; init; } = 401;
    /// <summary>See <see cref="AirDropServerOptions.DeclineKeepsConnection"/>.</summary>
    public bool DeclineKeepsConnection { get; init; }
    public Action<string>? Log { get; init; }
}

/// <summary>
/// One "Receive is on" period: the HTTPS endpoints plus the mDNS announcements on
/// one interface. Disposing it sends the mDNS goodbye and closes every connection.
/// </summary>
public sealed class ReceiveSession : IAsyncDisposable
{
    private readonly AirDropServer _server;
    private readonly MdnsResponder _mdns;

    public InterfaceInfo Interface { get; }
    public int Port => _server.Port;
    public event Action<TransferResult>? TransferFinished;

    private ReceiveSession(AirDropServer server, MdnsResponder mdns, InterfaceInfo nic)
    {
        _server = server;
        _mdns = mdns;
        Interface = nic;
        _server.TransferFinished += r => TransferFinished?.Invoke(r);
    }

    public static IReadOnlyList<InterfaceInfo> ListInterfaces()
    {
        var list = new List<InterfaceInfo>();
        foreach (NetworkInterface nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            try
            {
                IPInterfaceProperties props = nic.GetIPProperties();
                IPv6InterfaceProperties? v6 = props.GetIPv6Properties();
                if (v6 is null) continue;
                bool loopback = nic.NetworkInterfaceType == NetworkInterfaceType.Loopback;
                IPAddress? address = props.UnicastAddresses.Select(a => a.Address).FirstOrDefault(a =>
                    a.AddressFamily == AddressFamily.InterNetworkV6 && (loopback ? IPAddress.IsLoopback(a) : a.IsIPv6LinkLocal));
                list.Add(new InterfaceInfo(nic.Name, v6.Index, nic.OperationalStatus == OperationalStatus.Up, address, loopback));
            }
            catch (NetworkInformationException)
            {
                // Interface without IPv6 or one that disappeared while enumerating.
            }
        }
        return list;
    }

    public static InterfaceInfo? FindInterface(string nameOrIndex) =>
        ListInterfaces().FirstOrDefault(i =>
            i.Index.ToString() == nameOrIndex || i.Name.Equals(nameOrIndex, StringComparison.OrdinalIgnoreCase));

    /// <exception cref="InvalidOperationException">The interface is missing or has no usable IPv6 address.</exception>
    public static ReceiveSession Start(ReceiveSessionOptions o)
    {
        InterfaceInfo nic = FindInterface(o.Interface)
            ?? throw new InvalidOperationException($"Network interface '{o.Interface}' was not found.");
        if (nic.LinkLocal is null)
            throw new InvalidOperationException($"Network interface '{nic.Name}' has no IPv6 link-local address.");

        Directory.CreateDirectory(o.DataDirectory);
        X509Certificate2 certificate = ServerIdentity.LoadOrCreate(Path.Combine(o.DataDirectory, "identity.pfx"));
        string serviceId = LoadOrCreateServiceId(Path.Combine(o.DataDirectory, "service-id"));

        var server = new AirDropServer(new AirDropServerOptions
        {
            ComputerName = o.ComputerName,
            DestinationDirectory = o.DestinationDirectory,
            Port = o.Port,
            BindAddress = nic.IsLoopback ? IPAddress.IPv6Loopback : IPAddress.IPv6Any,
            RequiredScopeId = nic.IsLoopback ? null : nic.Index,
            Consent = o.Consent,
            ConsentTimeout = o.ConsentTimeout,
            DeclineStatus = o.DeclineStatus,
            DeclineKeepsConnection = o.DeclineKeepsConnection,
            Log = o.Log,
        }, certificate);
        server.Start();
        MdnsResponder mdns;
        try
        {
            mdns = new MdnsResponder(new AirDropServiceRecords
            {
                ServiceId = serviceId,
                Address = nic.LinkLocal,
                Port = server.Port,
            }, nic.Index, log: o.Log);
        }
        catch
        {
            server.DisposeAsync().AsTask().GetAwaiter().GetResult();
            throw;
        }
        mdns.Start();
        return new ReceiveSession(server, mdns, nic);
    }

    /// <summary>The 12-hex id this machine announces as a receiver and uses as SenderID when sending.</summary>
    public static string LoadOrCreateServiceId(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        try
        {
            string existing = File.ReadAllText(path).Trim();
            if (Regex.IsMatch(existing, "^[0-9a-f]{12}$")) return existing;
        }
        catch (IOException) { }
        string id = AirDropServiceRecords.NewServiceId();
        File.WriteAllText(path, id);
        return id;
    }

    public async ValueTask DisposeAsync()
    {
        await _mdns.DisposeAsync().ConfigureAwait(false);
        await _server.DisposeAsync().ConfigureAwait(false);
    }
}
