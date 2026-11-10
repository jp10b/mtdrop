using System.Diagnostics;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using Mtdrop.Protocol;
using Color = System.Windows.Media.Color;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;

namespace Mtdrop.App;

/// <summary>
/// Small always-on-top consent card (Accept / Decline). It never takes keyboard focus when it appears and has no
/// default button, so a key press meant for another application cannot accept a transfer.
/// </summary>
public partial class PromptWindow : Window
{
    /// <summary>Transparent margin around the card (room for the shadow); matches the Border margin in the XAML.</summary>
    public const double ShadowPad = 12;

    /// <summary>Clicks are ignored for this long after the window appears or moves under the pointer.</summary>
    private static readonly TimeSpan ArmDelay = TimeSpan.FromMilliseconds(500);

    private readonly TimeSpan _timeout;
    private readonly Stopwatch _clock = new();
    private readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private readonly DispatcherTimer _arm = new() { Interval = ArmDelay };
    private bool _armed;
    private bool _placed;
    private int _shownSeconds = -1;

    /// <summary>True only if the user clicked Accept. Every other way of closing the window means decline.</summary>
    public bool Accepted { get; private set; }

    public PromptWindow(AskRequest ask, TimeSpan timeout)
    {
        InitializeComponent();
        ApplyTheme(IsDarkTheme());
        _timeout = timeout;

        string name = Clean(ask.SenderName, 40);
        NameText.Text = name;
        NameText.ToolTip = name;
        DetailText.Text = Clean(Prompts.Describe(ask), 200);
        UpdateCountdown(timeout);
        SetArmed(false);

        _tick.Tick += (_, _) => OnTick();
        _arm.Tick += (_, _) =>
        {
            _arm.Stop();
            SetArmed(true);
        };
        // Loaded (not ContentRendered) so the timers always start once the window exists.
        Loaded += (_, _) =>
        {
            _clock.Start();
            _tick.Start();
            Rearm();
        };
        Closed += (_, _) =>
        {
            _tick.Stop();
            _arm.Stop();
        };
        PreviewKeyDown += OnPreviewKeyDown;
    }

    /// <summary>Moves the card; if it jumps while already visible, buttons are disabled again for a moment.</summary>
    public void PlaceAt(double left, double top)
    {
        bool moved = _placed && Math.Abs(Top - top) > 0.5;
        Left = left;
        Top = top;
        _placed = true;
        if (moved) Rearm();     // a card sliding under the pointer must not catch a double click meant for the one above it
    }

    private void Rearm()
    {
        SetArmed(false);
        _arm.Stop();
        _arm.Start();
    }

    private void SetArmed(bool armed)
    {
        _armed = armed;
        AcceptButton.IsEnabled = armed;
        DeclineButton.IsEnabled = armed;
    }

    private void OnTick()
    {
        TimeSpan left = _timeout - _clock.Elapsed;
        if (left <= TimeSpan.Zero)
        {
            Close();            // timeout = decline
            return;
        }
        UpdateCountdown(left);
    }

    private void UpdateCountdown(TimeSpan left)
    {
        int seconds = (int)Math.Ceiling(left.TotalSeconds);
        if (seconds == _shownSeconds) return;
        _shownSeconds = seconds;
        CountdownText.Text = $"Declines automatically in {seconds} s";
    }

    private void OnAcceptClick(object sender, RoutedEventArgs e)
    {
        if (!_armed) return;
        Accepted = true;
        Close();
    }

    private void OnDeclineClick(object sender, RoutedEventArgs e)
    {
        if (!_armed) return;
        Close();
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape) return;
        e.Handled = true;
        Close();
    }

    /// <summary>
    /// Makes untrusted text safe to show on one line: control characters and invisible/bidi-override
    /// characters become spaces, whitespace is collapsed, length is capped.
    /// </summary>
    private static string Clean(string? text, int max)
    {
        if (string.IsNullOrEmpty(text)) return "";
        var sb = new StringBuilder(text.Length);
        foreach (char c in text)
        {
            bool invisible = char.IsControl(c) || c is >= '​' and <= '‏' or >= '‪' and <= '‮'
                or >= '⁦' and <= '⁩' or '﻿';
            sb.Append(invisible ? ' ' : c);
        }
        string s = string.Join(' ', sb.ToString().Split(' ', StringSplitOptions.RemoveEmptyEntries));
        if (s.Length <= max) return s;
        int cut = char.IsHighSurrogate(s[max - 1]) ? max - 1 : max;
        return s[..cut].TrimEnd() + "…";
    }

    private static bool IsDarkTheme()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("AppsUseLightTheme") is int light && light == 0;
        }
        catch (Exception)
        {
            return false;       // unreadable preference: neutral light style
        }
    }

    private void ApplyTheme(bool dark)
    {
        static SolidColorBrush MakeBrush(string hex)
        {
            var b = new SolidColorBrush((Color)System.Windows.Media.ColorConverter.ConvertFromString(hex));
            b.Freeze();
            return b;
        }
        // Set after InitializeComponent: the XAML replaces Resources, and DynamicResource looks these up lazily.
        Resources["CardBrush"] = MakeBrush(dark ? "#2B2B2B" : "#FAFAFA");
        Resources["EdgeBrush"] = MakeBrush(dark ? "#4D4D4D" : "#D0D0D0");
        Resources["TextBrush"] = MakeBrush(dark ? "#F3F3F3" : "#1B1B1B");
        Resources["MutedBrush"] = MakeBrush(dark ? "#A6A6A6" : "#6B6B6B");
        Resources["ButtonBrush"] = MakeBrush(dark ? "#3C3C3C" : "#E6E6E6");
        Resources["AccentBrush"] = MakeBrush(dark ? "#60CDFF" : "#0067C0");
        Resources["AccentTextBrush"] = MakeBrush(dark ? "#000000" : "#FFFFFF");
    }
}
