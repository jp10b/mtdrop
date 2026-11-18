using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;

namespace Mtdrop.App;

/// <summary>
/// Informational card (file saved, transfer failed, session problems) shown instead of a Windows toast when toasts
/// are switched off (FINDINGS F-025). Closes by itself after a while unless the pointer is on it.
/// </summary>
public partial class NoticeWindow : Window, ICard
{
    private static readonly TimeSpan Lifetime = TimeSpan.FromSeconds(30);

    private readonly string? _reveal;
    private readonly DispatcherTimer _life = new() { Interval = Lifetime };

    /// <param name="reveal">File or folder that "Open folder" shows in Explorer; null hides the button.</param>
    public NoticeWindow(string title, string detail, string? reveal)
    {
        InitializeComponent();
        CardTheme.Apply(Resources);
        _reveal = reveal;
        TitleText.Text = PromptWindow.Clean(title, 80);
        TitleText.ToolTip = TitleText.Text;
        DetailText.Text = PromptWindow.Clean(detail, 400);
        if (reveal is not null) OpenButton.Visibility = Visibility.Visible;

        _life.Tick += (_, _) =>
        {
            if (IsMouseOver) return;        // reading it: try again a lifetime later
            Close();
        };
        Loaded += (_, _) => _life.Start();
        Closed += (_, _) => _life.Stop();
        PreviewKeyDown += OnPreviewKeyDown;
    }

    public void PlaceAt(double left, double top)
    {
        Left = left;
        Top = top;
    }

    private void OnOpenClick(object sender, RoutedEventArgs e)
    {
        if (_reveal is not null) Shell.RevealInExplorer(_reveal);
        Close();
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => Close();

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape) return;
        e.Handled = true;
        Close();
    }
}
