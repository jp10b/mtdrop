using System.Security.Cryptography;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Storage.Streams;

namespace Mtdrop.App;

/// <summary>
/// The Bluetooth LE advertisement an Apple device sends before an AirDrop (ADR-0005 decision 2). Without it an
/// iPhone does not announce itself as a receiver (jedbillyb/airdrop-mt7921 FINDINGS §27; winawdl DESIGN-NOTES
/// "Sending"). Apple manufacturer data (company 0x004C), Continuity type 0x05, 18 bytes: 8 zero bytes, version 1,
/// four 2-byte contact hashes, a zero byte. The hashes are random: a receiver set to "Everyone" does not check them,
/// and no contact detail of the user is sent. Runs only while a send is looking for or talking to a receiver.
/// </summary>
public sealed class BleWake : IDisposable
{
    public const ushort AppleCompanyId = 0x004C;
    private const int MaxRestarts = 10;
    private readonly Action<string> _log;
    private readonly object _gate = new();
    private BluetoothLEAdvertisementPublisher? _publisher;
    private bool _stopped;
    private int _restarts;
    private volatile bool _radioOff;

    public BleWake(Action<string> log) => _log = log;

    /// <summary>True once Windows refused the advert because the Bluetooth radio is off.</summary>
    public bool RadioOff => _radioOff;

    /// <summary>
    /// Whether the Bluetooth radio is switched on in Windows; null when Windows does not say (no radio listed, or
    /// the query failed). Read before a send so Wi-Fi is not turned off for nothing: with Bluetooth off the advert is
    /// refused (RadioNotAvailable) and no iPhone lists this PC (2026-11-20).
    /// </summary>
    public static async Task<bool?> IsBluetoothOnAsync()
    {
        try
        {
            var radios = await Windows.Devices.Radios.Radio.GetRadiosAsync();
            var bluetooth = radios.Where(r => r.Kind == Windows.Devices.Radios.RadioKind.Bluetooth).ToList();
            if (bluetooth.Count == 0) return null;
            return bluetooth.Any(r => r.State == Windows.Devices.Radios.RadioState.On);
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>Type 0x05 payload with random hashes (winawdl scripts/airdrop-beacon.ps1, mt7921 tools/blewake.sh).</summary>
    public static byte[] Payload()
    {
        byte[] p = new byte[20];
        p[0] = 0x05;                                // Continuity type: AirDrop
        p[1] = 0x12;                                // 18 bytes follow
        p[10] = 0x01;                               // after 8 zero bytes: version 1
        RandomNumberGenerator.Fill(p.AsSpan(11, 8));
        return p;                                   // last byte stays 0
    }

    public void Start()
    {
        lock (_gate)
        {
            if (_stopped) return;
            var data = new BluetoothLEManufacturerData { CompanyId = AppleCompanyId };
            using (var writer = new DataWriter())
            {
                writer.WriteBytes(Payload());
                data.Data = writer.DetachBuffer();
            }
            var publisher = new BluetoothLEAdvertisementPublisher();
            publisher.Advertisement.ManufacturerData.Add(data);
            publisher.StatusChanged += OnStatusChanged;
            _publisher = publisher;
            try
            {
                publisher.Start();
            }
            catch (Exception e)
            {
                _log($"ble: could not start advertising: {e.GetType().Name}: {e.Message}");
            }
        }
    }

    private void OnStatusChanged(BluetoothLEAdvertisementPublisher sender, BluetoothLEAdvertisementPublisherStatusChangedEventArgs args)
    {
        _log($"ble: advertisement {args.Status}{(args.Error != Windows.Devices.Bluetooth.BluetoothError.Success ? $" ({args.Error})" : "")}");
        if (args.Status != BluetoothLEAdvertisementPublisherStatus.Aborted) return;
        if (args.Error == Windows.Devices.Bluetooth.BluetoothError.RadioNotAvailable) _radioOff = true;
        // Aborted: Bluetooth was switched off or reset (on a combo chip a Wi-Fi reconfiguration can do that,
        // mt7921 FINDINGS §36). Try again a moment later, a bounded number of times.
        lock (_gate)
        {
            if (_stopped || !ReferenceEquals(sender, _publisher) || ++_restarts > MaxRestarts) return;
        }
        _ = Task.Delay(TimeSpan.FromSeconds(2)).ContinueWith(_ => Start(), TaskScheduler.Default);
    }

    public void Dispose()
    {
        lock (_gate)
        {
            _stopped = true;
            if (_publisher is null) return;
            try { _publisher.Stop(); } catch (Exception) { /* already stopped or Bluetooth gone */ }
            _publisher.StatusChanged -= OnStatusChanged;
            _publisher = null;
        }
        _log("ble: advertisement stopped");
    }
}
