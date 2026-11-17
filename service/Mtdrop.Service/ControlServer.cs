using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using Mtdrop.Control;

namespace Mtdrop.Service;

/// <summary>
/// \\.\pipe\mtdrop-control (ADR-0004 decision 2): SYSTEM and Administrators full control, INTERACTIVE (signed-in
/// desktop users) read/write, NETWORK denied. One request per connection, three commands, nothing else accepted.
/// </summary>
public sealed class ControlServer(Session session, ILog log, string pipeName = ControlProtocol.PipeName)
{
    private const int MaxInstances = 4;

    public static PipeSecurity Security()
    {
        var security = new PipeSecurity();
        // The process that serves (SYSTEM in the service) needs FILE_CREATE_PIPE_INSTANCE for every further instance.
        security.AddAccessRule(new PipeAccessRule(WindowsIdentity.GetCurrent().User!, PipeAccessRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.NetworkSid, null),
            PipeAccessRights.FullControl, AccessControlType.Deny));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
            PipeAccessRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
            PipeAccessRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.InteractiveSid, null),
            PipeAccessRights.ReadWrite | PipeAccessRights.Synchronize, AccessControlType.Allow));
        return security;
    }

    public async Task RunAsync(CancellationToken ct)
    {
        PipeSecurity security = Security();
        bool first = true;
        while (!ct.IsCancellationRequested)
        {
            NamedPipeServerStream pipe;
            try
            {
                // The first instance must be ours: if another process already owns the name, fail rather than
                // serve next to it (pipe squatting).
                PipeOptions options = PipeOptions.Asynchronous | (first ? PipeOptions.FirstPipeInstance : PipeOptions.None);
                pipe = NamedPipeServerStreamAcl.Create(pipeName, PipeDirection.InOut, MaxInstances,
                    PipeTransmissionMode.Message, options, ControlProtocol.MaxMessageBytes, ControlProtocol.MaxMessageBytes, security);
                first = false;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                log.Warn($"control pipe: cannot create an instance ({e.Message}); retrying");
                await Task.Delay(1000, ct);
                continue;
            }
            try
            {
                await pipe.WaitForConnectionAsync(ct);
            }
            catch (OperationCanceledException)
            {
                await pipe.DisposeAsync();
                break;
            }
            _ = HandleAsync(pipe, ct);
        }
    }

    private async Task HandleAsync(NamedPipeServerStream pipe, CancellationToken ct)
    {
        await using (pipe)
        {
            try
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                timeout.CancelAfter(TimeSpan.FromSeconds(5));
                byte[] buffer = new byte[ControlProtocol.MaxMessageBytes + 1];
                int total = 0;
                do
                {
                    int n = await pipe.ReadAsync(buffer.AsMemory(total), timeout.Token);
                    if (n == 0) break;
                    total += n;
                } while (!pipe.IsMessageComplete && total < buffer.Length);

                ControlRequest? request = ControlProtocol.ParseRequest(buffer.AsSpan(0, total), out string? error);
                ControlResponse response = Handle(request, error);
                await pipe.WriteAsync(ControlProtocol.Encode(response), timeout.Token);
                await pipe.FlushAsync(timeout.Token);
                pipe.WaitForPipeDrain();
            }
            catch (Exception e) when (e is IOException or OperationCanceledException)
            {
                // Client went away or was too slow; nothing to answer.
            }
        }
    }

    /// <summary>Maps one validated request onto the session. Public for tests.</summary>
    public ControlResponse Handle(ControlRequest? request, string? parseError)
    {
        if (request is null)
        {
            log.Warn($"control pipe: refused a request ({parseError})");
            return session.Status() with { Ok = false, Error = parseError };
        }
        return request.Command switch
        {
            "start" => session.Start(request.Minutes!.Value),
            "stop" => session.Stop(),
            _ => session.Status(),
        };
    }
}
