using System.Text.Json;
using Microsoft.Win32;
using Xunit;

namespace StatletSettings.Tests;

public class ReleaseTests
{
    static string Root
    {
        get
        {
            var d = new DirectoryInfo(AppContext.BaseDirectory);
            while (d != null && !File.Exists(Path.Combine(d.FullName, "VERSION"))) d = d.Parent;
            return d?.FullName ?? throw new FileNotFoundException("VERSION");
        }
    }

    [Fact]
    public void The_version_file_is_a_plain_version_number_and_the_installer_reads_it_from_there()
    {
        string version = File.ReadAllText(Path.Combine(Root, "VERSION")).Trim();
        Assert.Matches(@"^\d+\.\d+\.\d+$", version);
        string iss = File.ReadAllText(Path.Combine(Root, "installer", "statlet.iss"));
        Assert.Contains("{#AppVersion}", iss);
        Assert.DoesNotMatch(@"AppVersion=\d", iss);  // never a number typed into the script
    }

    [Fact]
    public void The_releases_links_point_at_the_statlet_repository_over_https()
    {
        Assert.Equal("https://github.com/ejaysobara/statlet/releases", Links.Releases);
        Assert.StartsWith(Links.Releases, Links.Notes);
    }

    [Fact]
    public void The_question_is_asked_once_and_only_while_the_switch_is_off()
    {
        var s = new Settings();
        Assert.True(s.ShouldAskAutostart(false));
        Assert.False(s.ShouldAskAutostart(true));
        s.AutostartAsked = true;
        Assert.False(s.ShouldAskAutostart(false));
    }

    [Fact]
    public void Keys_this_version_does_not_know_survive_a_save()
    {
        var s = Store.Parse("{\"topOffset\":12,\"fromTheFuture\":{\"a\":1},\"autostartAsked\":true}");
        Assert.True(s.AutostartAsked);
        string text = JsonSerializer.Serialize(s, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        Assert.Contains("\"fromTheFuture\":{\"a\":1}", text);
        Assert.Contains("\"autostartAsked\":true", text);
    }

    static string TempKey() => @"Software\StatletTests\" + Guid.NewGuid().ToString("N");

    [Fact]
    public void An_old_run_path_is_rewritten_and_a_missing_one_is_left_alone()
    {
        string key = TempKey();
        try
        {
            Store.RepairRunPath(key, @"C:\new\statlet.exe");  // no key, no value: nothing created
            Assert.Null(Registry.CurrentUser.OpenSubKey(key));
            using (var k = Registry.CurrentUser.CreateSubKey(key)) k.SetValue("Statlet", "\"D:\\old zip\\statlet.exe\"");
            Store.RepairRunPath(key, @"C:\new\statlet.exe");
            using var r = Registry.CurrentUser.OpenSubKey(key)!;
            Assert.Equal("\"C:\\new\\statlet.exe\"", r.GetValue("Statlet"));
        }
        finally { Registry.CurrentUser.DeleteSubKeyTree(key, throwOnMissingSubKey: false); }
    }

    [Theory]
    [InlineData(new byte[] { 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, false)]  // enabled
    [InlineData(new byte[] { 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, false)]  // enabled by the user
    [InlineData(new byte[] { 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, true)]   // switched off in Startup Apps
    public void Windows_startup_apps_switch_is_read_from_the_first_byte(byte[] data, bool blocked)
    {
        string key = TempKey();
        try
        {
            using (var k = Registry.CurrentUser.CreateSubKey(key)) k.SetValue("Statlet", data, RegistryValueKind.Binary);
            Assert.Equal(blocked, Store.IsBlocked(key));
        }
        finally { Registry.CurrentUser.DeleteSubKeyTree(key, throwOnMissingSubKey: false); }
    }

    [Fact]
    public void No_startup_apps_entry_means_not_blocked() => Assert.False(Store.IsBlocked(TempKey()));
}
