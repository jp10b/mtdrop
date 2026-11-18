using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using Mtdrop.Control;

namespace Mtdrop.Service.Tests;

public sealed class ControlProtocolTests
{
    private static ControlRequest? Parse(string json, out string? error) => ControlProtocol.ParseRequest(Encoding.UTF8.GetBytes(json), out error);

    [Theory]
    [InlineData("""{"command":"start","minutes":2}""", "start", 2)]
    [InlineData("""{"command":"start","minutes":10}""", "start", 10)]
    [InlineData("""{"command":"stop"}""", "stop", null)]
    [InlineData("""{"command":"status"}""", "status", null)]
    public void Accepts_the_three_commands(string json, string command, int? minutes)
    {
        ControlRequest? r = Parse(json, out string? error);
        Assert.Null(error);
        Assert.Equal(new ControlRequest(command, minutes), r);
    }

    [Theory]
    [InlineData("""{"command":"start"}""", "minutes must be")]
    [InlineData("""{"command":"start","minutes":0}""", "minutes must be")]
    [InlineData("""{"command":"start","minutes":11}""", "minutes must be")]
    [InlineData("""{"command":"stop","minutes":2}""", "unknown command")]
    [InlineData("""{"command":"install","minutes":2}""", "unknown command")]
    [InlineData("""{"command":"start","minutes":2,"exe":"C:\\evil.exe"}""", "malformed")]
    [InlineData("""{"command":"start","minutes":"2"}""", "malformed")]
    [InlineData("""not json""", "malformed")]
    [InlineData("""null""", "unknown command")]
    [InlineData("""{}""", "unknown command")]
    public void Refuses_anything_else(string json, string reason)
    {
        Assert.Null(Parse(json, out string? error));
        Assert.Contains(reason, error);
    }

    [Fact]
    public void Refuses_empty_and_oversized_requests()
    {
        Assert.Null(ControlProtocol.ParseRequest([], out _));
        byte[] big = Encoding.UTF8.GetBytes($$"""{"command":"status","x":"{{new string('a', ControlProtocol.MaxMessageBytes)}}"}""");
        Assert.Null(ControlProtocol.ParseRequest(big, out string? error));
        Assert.Equal("bad request size", error);
    }

    [Fact]
    public void Response_round_trips()
    {
        var r = new ControlResponse(true, SessionState.Receiving, 93, "visible to nearby iPhones");
        Assert.Equal(r, ControlProtocol.ParseResponse(ControlProtocol.Encode(r)));
        Assert.Contains("\"state\":\"Receiving\"", Encoding.UTF8.GetString(ControlProtocol.Encode(r)));
    }
}

public sealed class ControlServerTests : IDisposable
{
    private readonly Rig _rig = new();

    public void Dispose() => _rig.Dispose();

    [Fact]
    public void Pipe_acl_denies_network_and_gives_interactive_users_read_write_only()
    {
        PipeSecurity s = ControlServer.Security();
        var rules = s.GetAccessRules(true, false, typeof(SecurityIdentifier)).Cast<PipeAccessRule>().ToList();
        PipeAccessRule interactive = rules.Single(r => r.IdentityReference.Value == "S-1-5-4");
        Assert.Equal(AccessControlType.Allow, interactive.AccessControlType);
        Assert.Equal(0, (int)(interactive.PipeAccessRights & (PipeAccessRights.ChangePermissions | PipeAccessRights.TakeOwnership | PipeAccessRights.CreateNewInstance)));
        Assert.Contains(rules, r => r.IdentityReference.Value == "S-1-5-2" && r.AccessControlType == AccessControlType.Deny);
        Assert.DoesNotContain(rules, r => r.IdentityReference.Value == "S-1-1-0");   // Everyone
    }

    [Fact]
    public async Task Request_and_answer_over_a_real_pipe()
    {
        string name = $"mtdrop-control-test-{Guid.NewGuid():N}";
        var server = new ControlServer(_rig.Session, _rig.Log, name);
        using var cts = new CancellationTokenSource();
        Task serving = server.RunAsync(cts.Token);

        ControlResponse status = await ControlClient.SendAsync(new ControlRequest("status"), pipeName: name);
        Assert.True(status.Ok);
        Assert.Equal(SessionState.Idle, status.State);

        ControlResponse bad = await ControlClient.SendAsync(new ControlRequest("start", 99), pipeName: name);
        Assert.False(bad.Ok);
        Assert.Contains("minutes must be", bad.Error);
        Assert.Empty(_rig.Calls);

        cts.Cancel();
        await serving.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Missing_service_is_reported_as_unavailable()
    {
        await Assert.ThrowsAsync<ServiceUnavailableException>(() =>
            ControlClient.SendAsync(new ControlRequest("status"), connectTimeoutMs: 200, pipeName: $"mtdrop-nobody-{Guid.NewGuid():N}"));
    }

    [Fact]
    public async Task Start_command_reaches_the_session()
    {
        var server = new ControlServer(_rig.Session, _rig.Log);
        ControlResponse r = server.Handle(new ControlRequest("start", 2), null);
        Assert.True(r.Ok);
        Assert.Equal(SessionState.Preparing, r.State);
        _rig.Session.Stop();
        await _rig.Session.Completion.WaitAsync(TimeSpan.FromSeconds(20));
    }
}
