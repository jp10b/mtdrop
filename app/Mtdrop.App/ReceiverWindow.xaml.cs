using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Button = System.Windows.Controls.Button;
using HorizontalAlignment = System.Windows.HorizontalAlignment;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;

namespace Mtdrop.App;

/// <summary>
/// "Send to which iPhone?" (M8.4): receivers appear as buttons while they are found; a click chooses one.
/// Closing, Cancel or Escape choose none. Names are self-reported by the receivers and shown as plain text.
/// Used on the UI thread only.
/// </summary>
public partial class ReceiverWindow : Window, IReceiverPicker
{
    private readonly TaskCompletionSource<string?> _choice = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly HashSet<string> _keys = [];

    public ReceiverWindow(string title)
    {
        InitializeComponent();
        CardTheme.Apply(Resources);
        TitleText.Text = PromptWindow.Clean(title, 80);
        StatusText.Text = "Looking for iPhones nearby...";
        Closed += (_, _) => _choice.TrySetResult(null);
        PreviewKeyDown += OnPreviewKeyDown;
        MouseLeftButtonDown += (_, e) => { if (e.ButtonState == MouseButtonState.Pressed) DragMove(); };
    }

    /// <summary>The key of the chosen receiver; null when the window was cancelled or closed.</summary>
    public Task<string?> Choice => _choice.Task;

    public int ReceiverCount => Receivers.Children.Count;

    public void SetStatus(string text) => StatusText.Text = PromptWindow.Clean(text, 200);

    /// <summary>Adds a receiver once per key.</summary>
    public void Add(string key, string name, string? model)
    {
        if (!_keys.Add(key) || _choice.Task.IsCompleted) return;
        var button = new Button
        {
            Style = (Style)FindResource("PromptButton"),
            Margin = new Thickness(0, 0, 0, 6),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Left,
            Content = new TextBlock
            {
                Text = PromptWindow.Clean(name, 60) + (model is { Length: > 0 } ? $"  ({PromptWindow.Clean(model, 30)})" : ""),
                TextTrimming = TextTrimming.CharacterEllipsis,
            },
            ToolTip = "Send to this device",
        };
        button.Click += (_, _) =>
        {
            if (_choice.TrySetResult(key)) Close();
        };
        Receivers.Children.Add(button);
        StatusText.Text = Receivers.Children.Count == 1 ? "Found 1 device. Still looking..." : $"Found {Receivers.Children.Count} devices. Still looking...";
    }

    private void OnCancelClick(object sender, RoutedEventArgs e) => Close();

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape) return;
        e.Handled = true;
        Close();
    }
}
