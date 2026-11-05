using Xunit;

namespace StatletSettings.Tests;

public class IslandLauncherTests
{
    static string UniqueName() => @"Local\Statlet.Tests." + Guid.NewGuid().ToString("N");

    [Fact]
    public void A_held_mutex_means_the_island_is_running()
    {
        string name = UniqueName();
        Assert.False(IslandLauncher.IsRunning(name));
        using var held = new Mutex(true, name);
        Assert.True(IslandLauncher.IsRunning(name));
    }

    [Fact]
    public void Starts_the_island_when_the_mutex_is_absent()
    {
        string exe = Path.GetTempFileName();
        try
        {
            var started = new List<string>();
            Assert.True(IslandLauncher.EnsureRunning(exe, UniqueName(), started.Add));
            Assert.Equal(new[] { exe }, started);
        }
        finally { File.Delete(exe); }
    }

    [Fact]
    public void Does_not_start_a_second_island()
    {
        string exe = Path.GetTempFileName(), name = UniqueName();
        try
        {
            using var held = new Mutex(true, name);
            var started = new List<string>();
            Assert.False(IslandLauncher.EnsureRunning(exe, name, started.Add));
            Assert.Empty(started);
        }
        finally { File.Delete(exe); }
    }

    [Fact]
    public void A_missing_exe_starts_nothing()
    {
        var started = new List<string>();
        Assert.False(IslandLauncher.EnsureRunning(Path.Combine(Path.GetTempPath(), "no-such-statlet.exe"), UniqueName(), started.Add));
        Assert.Empty(started);
    }

    [Fact]
    public void The_default_mutex_name_is_the_one_the_island_creates()
    {
        string island = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "island", "src", "main.cpp"));
        Assert.Contains("L\"" + IslandLauncher.MutexName.Replace(@"\", @"\\") + "\"", island);
    }
}
