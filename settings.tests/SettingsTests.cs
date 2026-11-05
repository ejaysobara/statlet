using System.Runtime.CompilerServices;
using System.Text.Json;
using Xunit;

namespace StatletSettings.Tests;

static class SafeStore
{
    public static readonly string Dir = Path.Combine(Path.GetTempPath(), "statlet-settings-tests-" + Environment.ProcessId);

    // Runs before any test touches Store, so its file lives in a temp folder and never in %LOCALAPPDATA%\Statlet.
    [ModuleInitializer]
    internal static void Init() => Environment.SetEnvironmentVariable("STATLET_DATA_DIR", Dir);
}

public class SettingsTests
{
    [Fact]
    public void Defaults_match_the_island()
    {
        var s = new Settings();
        Assert.Equal(10, s.TopOffset);
        Assert.Equal(25, s.FocusMinutes);
        Assert.Equal((80, 95), (s.WarnAt, s.CritAt));
        Assert.Equal("tuck", s.AfterPeek);
        Assert.Equal(40, s.Bounce);
        Assert.Equal(60, s.Speed);
    }

    [Theory]
    [InlineData("")]
    [InlineData("not json")]
    [InlineData("{\"topOffset\":")]
    [InlineData("null")]
    public void Broken_text_gives_the_defaults(string text) => Assert.Equal(10, Store.Parse(text).TopOffset);

    [Fact]
    public void Parse_reads_camelCase_keys_and_numbers_written_as_strings()
    {
        var s = Store.Parse("{\"topOffset\":\"20\",\"afterPeek\":\"music\",\"followAccent\":false}");
        Assert.Equal(20, s.TopOffset);
        Assert.Equal("music", s.AfterPeek);
        Assert.False(s.FollowAccent);
        Assert.Equal(25, s.FocusMinutes);  // a key that is missing keeps its default
    }

    [Fact]
    public void Raising_WarnAt_keeps_CritAt_five_above()
    {
        var s = new Settings { WarnAt = 90 };
        Assert.Equal(95, s.CritAt);
        s.WarnAt = 95;
        Assert.Equal(100, s.CritAt);
        s.WarnAt = 60;
        Assert.Equal(100, s.CritAt);  // lowering it leaves CritAt alone
    }

    [Fact]
    public void ResetAppearance_leaves_the_other_pages_alone()
    {
        var s = new Settings { TextScale = 1.2, Speed = 10, InstantOpen = true, FocusMinutes = 50 };
        s.ResetAppearance();
        Assert.Equal((1.0, 60.0, false), (s.TextScale, s.Speed, s.InstantOpen));
        Assert.Equal(50, s.FocusMinutes);
    }

    [Fact]
    public void Changes_are_notified_once_and_only_when_the_value_differs()
    {
        var s = new Settings();
        var names = new List<string?>();
        s.PropertyChanged += (_, e) => names.Add(e.PropertyName);
        s.TopOffset = 10;  // same as the default
        s.TopOffset = 12;
        Assert.Equal(new[] { "TopOffset" }, names);
    }

    [Fact]
    public void Save_writes_to_the_override_folder_and_reads_back()
    {
        Store.Current.TopOffset = 33;
        Store.Save();
        string file = Path.Combine(SafeStore.Dir, "settings.json");
        Assert.True(File.Exists(file));
        Assert.Equal(33, Store.Parse(File.ReadAllText(file)).TopOffset);
        Assert.False(File.Exists(file + ".tmp"));
        using var doc = JsonDocument.Parse(File.ReadAllText(file));
        Assert.True(doc.RootElement.TryGetProperty("topOffset", out _));  // camelCase, the keys the island reads
    }
}
