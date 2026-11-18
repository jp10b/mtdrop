using System.Diagnostics;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using Mtdrop.Protocol;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;

namespace Mtdrop.App;

/// <summary>
/// Small always-on-top consent card (Accept / Decline). It never takes keyboard focus when it appears and has no
/// default button, so a key press meant for another application cannot accept a transfer.
/// </summary>
public partial class PromptWindow : Window, ICard
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
        CardTheme.Apply(Resources);
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
    internal static string Clean(string? text, int max)
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
}
