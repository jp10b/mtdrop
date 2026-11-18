using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;
using Color = System.Windows.Media.Color;

namespace Mtdrop.App;

/// <summary>A card window stacked by <see cref="Prompts"/> in the bottom-right corner.</summary>
public interface ICard
{
    void PlaceAt(double left, double top);
}

/// <summary>Light or dark brushes for the card windows, following the Windows app theme (light or dark).</summary>
public static class CardTheme
{
    private static bool IsDark()
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

    /// <summary>Puts the card brushes into a window's resources (after InitializeComponent).</summary>
    public static void Apply(ResourceDictionary resources)
    {
        bool dark = IsDark();
        static SolidColorBrush MakeBrush(string hex)
        {
            var b = new SolidColorBrush((Color)System.Windows.Media.ColorConverter.ConvertFromString(hex));
            b.Freeze();
            return b;
        }
        // Set after InitializeComponent: the XAML replaces Resources, and DynamicResource looks these up lazily.
        resources["CardBrush"] = MakeBrush(dark ? "#2B2B2B" : "#FAFAFA");
        resources["EdgeBrush"] = MakeBrush(dark ? "#4D4D4D" : "#D0D0D0");
        resources["TextBrush"] = MakeBrush(dark ? "#F3F3F3" : "#1B1B1B");
        resources["MutedBrush"] = MakeBrush(dark ? "#A6A6A6" : "#6B6B6B");
        resources["ButtonBrush"] = MakeBrush(dark ? "#3C3C3C" : "#E6E6E6");
        resources["AccentBrush"] = MakeBrush(dark ? "#60CDFF" : "#0067C0");
        resources["AccentTextBrush"] = MakeBrush(dark ? "#000000" : "#FFFFFF");
    }
}
