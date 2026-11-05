using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace StatletSettings;

public sealed partial class GeneralPage : UserControl
{
    static readonly int[] FocusChoices = [15, 25, 45, 60];

    public Settings S => Store.Current;

    public GeneralPage()
    {
        InitializeComponent();
        Startup.IsOn = Store.StartsWithWindows && !Store.BlockedByWindows;  // never On while Windows blocks it
        ShowStartupNote();
        foreach (int m in FocusChoices) FocusLength.Items.Add($"{m} minutes");
        FocusLength.SelectedIndex = Math.Max(0, Array.IndexOf(FocusChoices, (int)S.FocusMinutes));
    }

    public string Px(double v) => $"{v:0} px";
    public string Pct(double v) => $"{v:0}%";
    public double CritMin(double warn) => Math.Min(100, warn + 5);

    void OnStartup(object sender, RoutedEventArgs e)
    {
        try { Store.StartsWithWindows = Startup.IsOn; }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException) { Startup.IsOn = Store.StartsWithWindows; }
        ShowStartupNote();
    }

    void ShowStartupNote()
    {
        bool blocked = Store.StartsWithWindows && Store.BlockedByWindows;
        StartupNote.Text = blocked ? "Windows has turned this off in Startup Apps." : "The island only appears while it is running. Keep this on so it is back after a restart.";
        StartupApps.Visibility = blocked ? Visibility.Visible : Visibility.Collapsed;
    }

    void OnFocus(object sender, SelectionChangedEventArgs e) => S.FocusMinutes = FocusChoices[Math.Max(0, FocusLength.SelectedIndex)];
}
