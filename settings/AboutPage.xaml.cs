using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Win32;

namespace StatletSettings;

public sealed partial class AboutPage : UserControl
{
    // Inno Setup's uninstall entry for the installer's fixed AppId (installer/statlet.iss).
    const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E1B7C52-3A0D-4F0B-9B83-5D6C1E2A7F41}_is1";

    public AboutPage()
    {
        InitializeComponent();
        VersionText.Text = "Statlet " + Version;
        InstalledText.Text = InstallDate() is { } d ? "Installed " + d.ToString("d MMMM yyyy", CultureInfo.CurrentCulture) : "Not installed with the setup program";
    }

    // The VERSION file's number, without the git hash the SDK appends.
    internal static string Version =>
        (Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "").Split('+')[0];

    static DateTime? InstallDate()
    {
        using var k = Registry.CurrentUser.OpenSubKey(UninstallKey);
        return k?.GetValue("InstallDate") is string s && DateTime.TryParseExact(s, "yyyyMMdd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var d) ? d : null;
    }

    static void Open(string url) => Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })?.Dispose();
    void OnReleases(object sender, RoutedEventArgs e) => Open(Links.Releases);
    void OnNotes(object sender, RoutedEventArgs e) => Open(Links.Notes);
}
