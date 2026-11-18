using System.IO;
using System.Text;

namespace Mtdrop.App;

/// <summary>
/// Timestamped log of what the tray app saw and did: %LOCALAPPDATA%\mtdrop\logs\app.log (one previous file kept as
/// app.1.log). Private to the user; sender names and file names are not written. Never throws.
/// </summary>
public static class AppLog
{
    private const long MaxBytes = 1024 * 1024;
    private static readonly object Gate = new();
    private static string? _path;

    public static string Directory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "mtdrop", "logs");

    public static void Open()
    {
        try
        {
            System.IO.Directory.CreateDirectory(Directory);
            string path = Path.Combine(Directory, "app.log");
            if (File.Exists(path) && new FileInfo(path).Length > MaxBytes)
                File.Move(path, Path.Combine(Directory, "app.1.log"), overwrite: true);
            _path = path;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _path = null;
        }
    }

    public static void Write(string message)
    {
        string? path = _path;
        if (path is null) return;
        string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}  {message}{Environment.NewLine}";
        lock (Gate)
        {
            try { File.AppendAllText(path, line, Encoding.UTF8); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { /* logging is best effort */ }
        }
    }
}
