using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Win32;

namespace StatletSettings;

// The shared settings file, %LOCALAPPDATA%\Statlet\settings.json. The island reads the same keys (island/src/settings.h)
// and rebuilds itself when the file changes, so every change here is saved at once (no Save button).
public sealed class Settings : INotifyPropertyChanged
{
    double topOffset = 10, focusMinutes = 25, warnAt = 80, critAt = 95;
    bool openOnHighUsage = true, followAccent = true, instantOpen;
    string afterPeek = "tuck", progressStyle = "wave", glassTint = "#1C1E24", accentColor = "#4CC2FF";
    double mediaPeekMs = 4000, mediaHideMs = 30000, tintOpacity = 0.52, collapsedR = 11, expandedR = 22, textScale = 1, bounce = 40, speed = 60;

    public double TopOffset { get => topOffset; set => Set(ref topOffset, value); }
    public double FocusMinutes { get => focusMinutes; set => Set(ref focusMinutes, value); }
    public double WarnAt { get => warnAt; set { Set(ref warnAt, value); if (CritAt < value + 5) CritAt = Math.Min(100, value + 5); } }
    public double CritAt { get => critAt; set => Set(ref critAt, value); }
    public bool OpenOnHighUsage { get => openOnHighUsage; set => Set(ref openOnHighUsage, value); }
    public string AfterPeek { get => afterPeek; set => Set(ref afterPeek, value); }
    public string ProgressStyle { get => progressStyle; set => Set(ref progressStyle, value); }
    public double MediaPeekMs { get => mediaPeekMs; set => Set(ref mediaPeekMs, value); }
    public double MediaHideMs { get => mediaHideMs; set => Set(ref mediaHideMs, value); }
    public string GlassTint { get => glassTint; set => Set(ref glassTint, value); }
    public double TintOpacity { get => tintOpacity; set => Set(ref tintOpacity, value); }
    public bool FollowAccent { get => followAccent; set => Set(ref followAccent, value); }
    public string AccentColor { get => accentColor; set => Set(ref accentColor, value); }
    public double CollapsedR { get => collapsedR; set => Set(ref collapsedR, value); }
    public double ExpandedR { get => expandedR; set => Set(ref expandedR, value); }
    public double TextScale { get => textScale; set => Set(ref textScale, value); }
    public double Bounce { get => bounce; set => Set(ref bounce, value); }
    public double Speed { get => speed; set => Set(ref speed, value); }
    public bool InstantOpen { get => instantOpen; set => Set(ref instantOpen, value); }

    public event PropertyChangedEventHandler? PropertyChanged;

    void Set<T>(ref T field, T value, [CallerMemberName] string name = "")
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    // Puts the appearance keys back to their defaults; the other pages keep theirs.
    public void ResetAppearance()
    {
        var d = new Settings();
        GlassTint = d.GlassTint; TintOpacity = d.TintOpacity; FollowAccent = d.FollowAccent; AccentColor = d.AccentColor;
        CollapsedR = d.CollapsedR; ExpandedR = d.ExpandedR; TextScale = d.TextScale; Bounce = d.Bounce; Speed = d.Speed; InstantOpen = d.InstantOpen;
    }
}

public static class Store
{
    // STATLET_DATA_DIR replaces %LOCALAPPDATA%\Statlet so tests never touch the real settings file.
    static readonly string Dir = Environment.GetEnvironmentVariable("STATLET_DATA_DIR") is { Length: > 0 } over
        ? over
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Statlet");
    static readonly string FilePath = Path.Combine(Dir, "settings.json");
    static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, NumberHandling = JsonNumberHandling.AllowReadingFromString };
    static CancellationTokenSource? pending;

    public static Settings Current { get; } = Load();

    // Broken or empty text gives the defaults.
    internal static Settings Parse(string text)
    {
        try { return JsonSerializer.Deserialize<Settings>(text, Json) ?? new(); }
        catch (JsonException) { return new(); }
    }

    static Settings Load()
    {
        Settings s;
        try { s = Parse(File.ReadAllText(FilePath)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { s = new(); }  // missing: defaults
        s.PropertyChanged += (_, _) => SaveSoon();
        return s;
    }

    // Slider drags change values many times a second: write once they pause, so the island rebuilds once.
    static async void SaveSoon()
    {
        pending?.Cancel();
        var mine = pending = new CancellationTokenSource();
        try { await Task.Delay(150, mine.Token); } catch (TaskCanceledException) { return; }
        Save();
    }

    // Written to a temp file and moved over, so the island never reads half a file.
    public static void Save()
    {
        // The island reads the file the moment the temp file appears and its handle blocks the move, so retry: giving up
        // here silently lost the change and the island rebuilt from the old file.
        for (int attempt = 0; attempt < 10; attempt++)
        {
            try
            {
                Directory.CreateDirectory(Dir);
                string temp = FilePath + ".tmp";
                File.WriteAllText(temp, JsonSerializer.Serialize(Current, Json));
                File.Move(temp, FilePath, overwrite: true);
                return;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Thread.Sleep(40); }
        }
    }

    // --- Start with Windows: the per-user Run key points at the island beside this app's folder ---

    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    internal static string IslandExe => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "statlet.exe"));

    public static bool StartsWithWindows
    {
        get { using var k = Registry.CurrentUser.OpenSubKey(RunKey); return k?.GetValue("Statlet") is string; }
        set
        {
            using var k = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value) k.SetValue("Statlet", $"\"{IslandExe}\"");
            else k.DeleteValue("Statlet", throwOnMissingValue: false);
        }
    }

    // --- Preview hold: asks the island to keep a view open (0 none, 1 hover, 2 card, 3 music) ---

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string cls, IntPtr title);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    const uint WmAppHold = 0x8000 + 6;  // WM_APP_HOLD in island/src/main.cpp

    public static void Hold(int view)
    {
        IntPtr island = FindWindowW("StatletIsland", IntPtr.Zero);
        if (island != IntPtr.Zero) PostMessageW(island, WmAppHold, view, 0);
    }
}
