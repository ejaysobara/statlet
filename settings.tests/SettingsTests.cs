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
    public void Bar_style_keys_round_trip_and_unknown_keys_survive()
    {
        var s = Store.Parse("{\"progressStyle\":\"segmented\",\"playheadKnob\":\"hover\",\"artAccent\":true,\"futureKey\":7}");
        Assert.Equal(("segmented", "hover", true), (s.ProgressStyle, s.PlayheadKnob, s.ArtAccent));
        var d = new Settings();
        Assert.Equal(("wave", "never", false), (d.ProgressStyle, d.PlayheadKnob, d.ArtAccent));
        string json = JsonSerializer.Serialize(s, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        Assert.Contains("\"playheadKnob\":\"hover\"", json);
        Assert.Contains("\"artAccent\":true", json);
        Assert.Contains("\"futureKey\":7", json);
    }

    [Fact]
    public void ResetAppearance_clears_the_art_colour_switch()
    {
        var s = new Settings { ArtAccent = true, PlayheadKnob = "always" };
        s.ResetAppearance();
        Assert.False(s.ArtAccent);
        Assert.Equal("always", s.PlayheadKnob);  // a Now Playing choice, not an appearance one
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
    public void ShowWeekday_defaults_on_and_round_trips_as_showWeekday()
    {
        Assert.True(new Settings().ShowWeekday);
        Assert.False(Store.Parse("{\"showWeekday\":false}").ShowWeekday);
        Assert.True(Store.Parse("{\"showWeekday\":\"maybe\"}").ShowWeekday);  // a bad value keeps the default
        Store.Current.ShowWeekday = false;
        Store.Save();
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(SafeStore.Dir, "settings.json")));
        Assert.False(doc.RootElement.GetProperty("showWeekday").GetBoolean());
        Store.Current.ShowWeekday = true;
        Store.Save();
    }

    [Fact]
    public void V2_keys_default_like_the_island_and_round_trip()
    {
        var d = new Settings();
        Assert.Equal((true, true, true, false, true, "status,clock,usage"), (d.BtIcon, d.DownloadIcon, d.SoundScreenshot, d.SoundDownloadDone, d.SoundFocusDone, d.RestOrder));
        var s = Store.Parse("{\"btIcon\":false,\"downloadIcon\":false,\"soundScreenshot\":false,\"soundDownloadDone\":true,\"soundFocusDone\":false,\"restOrder\":\"clock,usage,status\"}");
        Assert.Equal((false, false, false, true, false, "clock,usage,status"), (s.BtIcon, s.DownloadIcon, s.SoundScreenshot, s.SoundDownloadDone, s.SoundFocusDone, s.RestOrder));
        string json = JsonSerializer.Serialize(s, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        foreach (string key in new[] { "btIcon", "downloadIcon", "soundScreenshot", "soundDownloadDone", "soundFocusDone", "restOrder" }) Assert.Contains($"\"{key}\":", json);
        Assert.Contains("\"restOrder\":\"clock,usage,status\"", json);
    }

    [Theory]
    [InlineData("clock,clock,usage")]
    [InlineData("clock,usage")]
    [InlineData("clock,usage,status,status")]
    [InlineData("clock,usage,time")]
    [InlineData("")]
    [InlineData("Status,clock,usage")]
    public void A_bad_restOrder_reads_as_the_default_order(string bad) =>
        Assert.Equal("status,clock,usage", Store.Parse($"{{\"restOrder\":\"{bad}\"}}").RestOrder);

    [Fact]
    public void Every_order_of_the_three_blocks_is_accepted()
    {
        string[] names = ["status", "clock", "usage"];
        var all = from a in names from b in names from c in names where new[] { a, b, c }.Distinct().Count() == 3 select $"{a},{b},{c}";
        Assert.Equal(6, all.Count());
        foreach (string o in all) Assert.Equal(o, Store.Parse($"{{\"restOrder\":\"{o}\"}}").RestOrder);
    }

    [Fact]
    public void Unknown_keys_survive_a_save_next_to_the_v2_keys()
    {
        var s = Store.Parse("{\"btIcon\":false,\"someFutureKey\":{\"a\":1}}");
        string json = JsonSerializer.Serialize(s, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        Assert.Contains("\"someFutureKey\":{\"a\":1}", json);
        Assert.Contains("\"btIcon\":false", json);
    }

    // tests/fixtures/settings-contract.json is also read by the island's tests (island/tests/tests.cpp): both sides must parse
    // every key to the same values, so a renamed or mistyped key fails here or there.
    [Fact]
    public void Matches_the_shared_settings_contract_fixture()
    {
        string text = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fixtures", "settings-contract.json"));
        var s = Store.Parse(text);
        Assert.Equal((false, false, false, false, true, false, "usage,status,clock"), (s.ShowWeekday, s.BtIcon, s.DownloadIcon, s.SoundScreenshot, s.SoundDownloadDone, s.SoundFocusDone, s.RestOrder));
        using var doc = JsonDocument.Parse(text);
        var props = typeof(Settings).GetProperties().Select(p => JsonNamingPolicy.CamelCase.ConvertName(p.Name)).ToHashSet();
        foreach (var key in doc.RootElement.EnumerateObject()) Assert.Contains(key.Name, props);  // every key in the file is a real Settings key
        Assert.Null(s.Unknown);
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
