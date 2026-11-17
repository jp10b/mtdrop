using System.Text.Json;
using System.Text.Json.Serialization;

namespace Mtdrop.Control;

/// <summary>Where a receive session is (ADR-0004 decision 5).</summary>
[JsonConverter(typeof(JsonStringEnumConverter<SessionState>))]
public enum SessionState
{
    /// <summary>Nothing running; Wi-Fi is on the MediaTek driver.</summary>
    Idle,
    /// <summary>Checking preconditions, memory for DMA, saving Wi-Fi networks. Wi-Fi still on.</summary>
    Preparing,
    /// <summary>Putting our driver on the card. Wi-Fi goes down.</summary>
    TakingCard,
    /// <summary>Radio and AWDL daemon starting; waiting for the awdl0 adapter.</summary>
    Starting,
    /// <summary>awdl0 is up: the PC can be found.</summary>
    Receiving,
    /// <summary>Stopping the radio and giving the card back to the MediaTek driver.</summary>
    RestoringWifi,
}

/// <summary>One request per pipe connection. Commands: start (with minutes), stop, status.</summary>
public sealed record ControlRequest(string Command, int? Minutes = null);

/// <summary>The answer to every request: the session state after handling it.</summary>
public sealed record ControlResponse(
    bool Ok,
    SessionState State,
    int SecondsLeft,
    string? Message = null,
    string? Error = null);

public static class ControlProtocol
{
    /// <summary>\\.\pipe\mtdrop-control</summary>
    public const string PipeName = "mtdrop-control";

    /// <summary>Largest request or response in bytes; anything longer is refused.</summary>
    public const int MaxMessageBytes = 4096;

    public const int MinMinutes = 1;
    public const int MaxMinutes = 10;

    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static byte[] Encode(ControlRequest request) => JsonSerializer.SerializeToUtf8Bytes(request, Options);

    public static byte[] Encode(ControlResponse response) => JsonSerializer.SerializeToUtf8Bytes(response, Options);

    /// <summary>
    /// Parses and validates a request from an untrusted local client. Returns null and a reason for anything that is
    /// not exactly one of the three commands with valid arguments.
    /// </summary>
    public static ControlRequest? ParseRequest(ReadOnlySpan<byte> bytes, out string? error)
    {
        error = null;
        if (bytes.Length == 0 || bytes.Length > MaxMessageBytes)
        {
            error = "bad request size";
            return null;
        }
        ControlRequest? r;
        try
        {
            r = JsonSerializer.Deserialize<ControlRequest>(bytes, Options);
        }
        catch (JsonException)
        {
            error = "malformed request";
            return null;
        }
        switch (r?.Command)
        {
            case "start" when r.Minutes is >= MinMinutes and <= MaxMinutes:
                return r;
            case "start":
                error = $"minutes must be {MinMinutes}..{MaxMinutes}";
                return null;
            case "stop" or "status" when r.Minutes is null:
                return r;
            default:
                error = "unknown command";
                return null;
        }
    }

    public static ControlResponse? ParseResponse(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length == 0 || bytes.Length > MaxMessageBytes) return null;
        try
        {
            return JsonSerializer.Deserialize<ControlResponse>(bytes, Options);
        }
        catch (JsonException)
        {
            return null;
        }
    }
}
