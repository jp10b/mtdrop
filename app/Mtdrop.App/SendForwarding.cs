using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;

namespace Mtdrop.App;

/// <summary>
/// Hands files from a second start of the app (Explorer "Send to > iPhone (mtdrop)", or <c>mtdrop.exe --send</c>
/// while the tray app runs) to the running tray app. A named pipe per user and Windows session, created with
/// <see cref="PipeOptions.CurrentUserOnly"/>, so only processes of the same user can hand over files. The message is
/// the list of full paths, one per line; the receiving side checks every path again (OutgoingFile.FromPath) and
/// still asks the user to pick the receiver, so nothing is ever sent without the user.
/// </summary>
public static class SendForwarding
{
    private const int MaxMessageBytes = 256 * 1024;
    private const int MaxFiles = 256;

    private static string PipeName => $"mtdrop-tray-send-{Process.GetCurrentProcess().SessionId}";

    /// <summary>Second instance: sends the paths to the running one. False if it could not be reached.</summary>
    public static bool Forward(IReadOnlyList<string> paths)
    {
        try
        {
            using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.Out, PipeOptions.CurrentUserOnly);
            pipe.Connect(3000);
            byte[] message = Encoding.UTF8.GetBytes(string.Join("\n", paths.Take(MaxFiles).Select(Path.GetFullPath)));
            if (message.Length > MaxMessageBytes) return false;
            pipe.Write(message);
            pipe.Flush();
            return true;
        }
        catch (Exception e) when (e is IOException or TimeoutException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            return false;
        }
    }

    /// <summary>Running tray app: accepts hand-overs for the rest of the process lifetime.</summary>
    public static void Listen(Func<IReadOnlyList<string>, object> onFiles, Action<string> log)
    {
        _ = Task.Run(async () =>
        {
            int failures = 0;
            while (true)
            {
                if (failures > 20)
                {
                    log("send hand-over disabled after repeated failures (Explorer \"Send to\" needs a restart of the tray app)");
                    return;
                }
                try
                {
                    await using var pipe = new NamedPipeServerStream(PipeName, PipeDirection.In, 1, PipeTransmissionMode.Byte,
                        PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                    await pipe.WaitForConnectionAsync();
                    using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                    using var ms = new MemoryStream();
                    byte[] buf = new byte[16 * 1024];
                    int n;
                    while ((n = await pipe.ReadAsync(buf, cts.Token)) > 0)
                    {
                        if (ms.Length + n > MaxMessageBytes) throw new InvalidDataException("hand-over too large");
                        ms.Write(buf, 0, n);
                    }
                    string[] paths = Encoding.UTF8.GetString(ms.ToArray())
                        .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                        .Take(MaxFiles).ToArray();
                    failures = 0;
                    log($"send: {paths.Length} file(s) handed over by a second start");
                    if (paths.Length > 0) onFiles(paths);
                }
                catch (Exception e) when (e is IOException or OperationCanceledException or InvalidDataException or UnauthorizedAccessException)
                {
                    failures++;
                    log($"send hand-over failed: {e.GetType().Name}");
                    await Task.Delay(500);
                }
            }
        });
    }
}
