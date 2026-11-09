using System.Text;

namespace Mtdrop.Protocol;

/// <summary>Turns archive member names from an untrusted sender into safe Windows file names.</summary>
public static class SafeName
{
    public const int MaxLength = 200;

    private static readonly HashSet<string> Reserved = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    /// <summary>
    /// Maps a member name to a bare file name (the directory structure is flattened),
    /// or null if the member must be skipped (root entry, AppleDouble sidecar,
    /// Live Photo metadata, empty after cleaning).
    /// Throws <see cref="ProtocolFormatException"/> for names that try to escape:
    /// absolute paths, "..", backslashes, drive letters, control characters.
    /// </summary>
    public static string? FromArchiveMember(string member)
    {
        if (member.Length == 0) throw new ProtocolFormatException("empty member name");
        if (member.StartsWith('/') || member.Contains('\\') || member.Contains(':'))
            throw new ProtocolFormatException("member name is not a plain relative path");
        foreach (char c in member)
            if (char.IsControl(c)) throw new ProtocolFormatException("control character in member name");
        string[] parts = member.Split('/', StringSplitOptions.RemoveEmptyEntries);
        foreach (string part in parts)
            if (part == "..") throw new ProtocolFormatException("member name contains '..'");
        string[] real = [.. parts.Where(p => p != ".")];
        if (real.Length == 0) return null;
        string leaf = real[^1];
        if (leaf.StartsWith("._", StringComparison.Ordinal)) return null;
        if (real.Length >= 2 && real[^2].EndsWith(".pvt", StringComparison.OrdinalIgnoreCase)
            && leaf.Equals("metadata.plist", StringComparison.OrdinalIgnoreCase)) return null;
        return Clean(leaf);
    }

    /// <summary>Cleans a single path component. Returns null if nothing usable is left.</summary>
    public static string? Clean(string leaf)
    {
        leaf = leaf.Normalize(NormalizationForm.FormC);
        var sb = new StringBuilder(leaf.Length);
        foreach (char c in leaf)
        {
            if (char.IsControl(c) || c is '<' or '>' or ':' or '"' or '/' or '\\' or '|' or '?' or '*') continue;
            // Bidirectional overrides and zero-width characters can disguise an extension.
            if (c is (>= '​' and <= '‏') or (>= '‪' and <= '‮') or (>= '⁦' and <= '⁩') or '﻿') continue;
            sb.Append(c);
        }
        string s = sb.ToString().TrimStart('.', ' ').TrimEnd('.', ' ');
        if (s.Length == 0) return null;
        if (s.Length > MaxLength)
        {
            string ext = Path.GetExtension(s);
            if (ext.Length > 20) ext = "";
            s = s[..(MaxLength - ext.Length)].TrimEnd('.', ' ') + ext;
            if (char.IsHighSurrogate(s[^1])) s = s[..^1];
        }
        int dot = s.IndexOf('.');
        string stem = dot >= 0 ? s[..dot] : s;
        if (Reserved.Contains(stem.TrimEnd(' '))) s = "_" + s;
        return s;
    }
}

public sealed class UploadLimits
{
    /// <summary>Maximum number of archive members (of any kind).</summary>
    public int MaxMembers { get; init; } = 512;

    /// <summary>Maximum total bytes of member data read from the archive.</summary>
    public long MaxTotalBytes { get; init; } = 4L * 1024 * 1024 * 1024;
}

/// <summary>
/// Decodes an /Upload body (dvzip → cpio odc) and saves the regular files it contains.
/// Nothing is left behind on failure. Existing files are never overwritten.
/// </summary>
public static class UploadReceiver
{
    /// <param name="body">Decoded HTTP body (chunked framing already removed).</param>
    /// <returns>Full paths of the saved files, in archive order.</returns>
    public static async Task<IReadOnlyList<string>> ReceiveAsync(
        Stream body, string destinationDirectory, UploadLimits limits, CancellationToken ct)
    {
        Directory.CreateDirectory(destinationDirectory);
        var cpio = new CpioOdcReader(new DvzipStream(body));
        var saved = new List<string>();
        string? temp = null;
        long budget = limits.MaxTotalBytes;
        int members = 0;
        byte[] buf = new byte[128 * 1024];
        try
        {
            while (await cpio.NextAsync(ct).ConfigureAwait(false) is { } entry)
            {
                if (++members > limits.MaxMembers) throw new ProtocolFormatException("too many archive members");
                if (entry.Kind == CpioEntryKind.Directory) continue;
                if (entry.Kind != CpioEntryKind.RegularFile || entry.LinkCount > 1)
                    throw new ProtocolFormatException("archive contains a link or special file");
                budget -= entry.Size;
                if (budget < 0) throw new ProtocolFormatException("archive larger than announced");
                string? name = SafeName.FromArchiveMember(entry.Name);
                if (name is null)
                {
                    await cpio.SkipDataAsync(ct).ConfigureAwait(false);
                    continue;
                }
                temp = Path.Combine(destinationDirectory, $".mtdrop-{Guid.NewGuid():N}.part");
                await using (var fs = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, useAsync: true))
                {
                    int n;
                    while ((n = await cpio.ReadDataAsync(buf, ct).ConfigureAwait(false)) > 0)
                        await fs.WriteAsync(buf.AsMemory(0, n), ct).ConfigureAwait(false);
                }
                string final = MoveWithoutOverwrite(temp, destinationDirectory, name);
                temp = null;
                saved.Add(final);
                MarkAsDownloaded(final);
            }
            if (saved.Count == 0) throw new ProtocolFormatException("archive contains no files");
            return saved;
        }
        catch
        {
            TryDelete(temp);
            foreach (string f in saved) TryDelete(f);
            throw;
        }
    }

    private static string MoveWithoutOverwrite(string temp, string directory, string name)
    {
        string stem = Path.GetFileNameWithoutExtension(name), ext = Path.GetExtension(name);
        for (int i = 0; i < 10_000; i++)
        {
            string candidate = Path.Combine(directory, i == 0 ? name : $"{stem} ({i}){ext}");
            if (File.Exists(candidate) || Directory.Exists(candidate)) continue;
            try
            {
                File.Move(temp, candidate, overwrite: false);
                return candidate;
            }
            catch (IOException) when (File.Exists(candidate))
            {
                // Lost a race with another writer; try the next name.
            }
        }
        throw new IOException("could not find a free file name");
    }

    /// <summary>Adds the "came from another computer" mark (Zone.Identifier, zone 3) like browsers do.</summary>
    private static void MarkAsDownloaded(string path)
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            File.WriteAllText(path + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            // File systems without alternate data streams (FAT, some network shares).
        }
    }

    private static void TryDelete(string? path)
    {
        if (path is null) return;
        try { File.Delete(path); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
