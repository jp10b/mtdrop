using System.IO.Pipes;

namespace Mtdrop.Control;

/// <summary>Raised when the mtdrop service cannot be reached (not installed, not running, or no access).</summary>
public sealed class ServiceUnavailableException(string message, Exception? inner = null) : Exception(message, inner);

/// <summary>Client side of the control pipe, used by the tray app. One connection per request.</summary>
public static class ControlClient
{
    public static Task<ControlResponse> StartAsync(int minutes, CancellationToken ct = default) =>
        SendAsync(new ControlRequest("start", minutes), ct);

    public static Task<ControlResponse> StopAsync(CancellationToken ct = default) => SendAsync(new ControlRequest("stop"), ct);

    public static Task<ControlResponse> StatusAsync(CancellationToken ct = default) => SendAsync(new ControlRequest("status"), ct);

    public static async Task<ControlResponse> SendAsync(ControlRequest request, CancellationToken ct = default, int connectTimeoutMs = 2000,
        string pipeName = ControlProtocol.PipeName)
    {
        using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            await pipe.ConnectAsync(connectTimeoutMs, ct);
        }
        catch (TimeoutException e)
        {
            throw new ServiceUnavailableException("The mtdrop service is not running (or not installed).", e);
        }
        catch (UnauthorizedAccessException e)
        {
            throw new ServiceUnavailableException("No access to the mtdrop service (only signed-in users may use it).", e);
        }
        pipe.ReadMode = PipeTransmissionMode.Message;
        byte[] bytes = ControlProtocol.Encode(request);
        await pipe.WriteAsync(bytes, ct);
        await pipe.FlushAsync(ct);

        byte[] buffer = new byte[ControlProtocol.MaxMessageBytes + 1];
        int total = 0;
        do
        {
            int n = await pipe.ReadAsync(buffer.AsMemory(total), ct);
            if (n == 0) break;
            total += n;
        } while (!pipe.IsMessageComplete && total < buffer.Length);
        return ControlProtocol.ParseResponse(buffer.AsSpan(0, total))
            ?? throw new ServiceUnavailableException("The mtdrop service sent an invalid answer.");
    }
}
