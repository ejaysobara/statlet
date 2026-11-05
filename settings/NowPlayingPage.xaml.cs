using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Animation;

namespace StatletSettings;

public sealed partial class NowPlayingPage : UserControl
{
    static readonly (double Ms, string Label)[] PeekChoices = [(2000, "2 seconds"), (4000, "4 seconds"), (6000, "6 seconds"), (8000, "8 seconds")];
    static readonly (double Ms, string Label)[] HideChoices = [(10000, "10 seconds"), (30000, "30 seconds"), (120000, "2 minutes"), (0, "Never")];

    static Settings S => Store.Current;

    public NowPlayingPage()
    {
        InitializeComponent();
        Select(Modes, S.AfterPeek);
        Select(Bars, S.ProgressStyle);
        Fill(Peek, PeekChoices, S.MediaPeekMs);
        Fill(Hide, HideChoices, S.MediaHideMs);
        // The sample wave drifts one wavelength per 1.6 s, as on the island; it stays still when Windows animations are off.
        if (new Windows.UI.ViewManagement.UISettings().AnimationsEnabled)
        {
            var drift = new DoubleAnimation { From = 0, To = -32, Duration = TimeSpan.FromMilliseconds(1600), RepeatBehavior = RepeatBehavior.Forever };
            Storyboard.SetTarget(drift, Drift);
            Storyboard.SetTargetProperty(drift, "X");
            var story = new Storyboard();
            story.Children.Add(drift);
            Loaded += (_, _) => story.Begin();
            Unloaded += (_, _) => story.Stop();
        }
    }

    static void Select(GridView view, string tag) =>
        view.SelectedItem = view.Items.OfType<GridViewItem>().FirstOrDefault(i => (string)i.Tag == tag) ?? view.Items[0];

    static void Fill(ComboBox box, (double Ms, string Label)[] choices, double current)
    {
        foreach (var c in choices) box.Items.Add(c.Label);
        int i = Array.FindIndex(choices, c => c.Ms == current);
        box.SelectedIndex = i >= 0 ? i : 1;
    }

    void OnMode(object sender, SelectionChangedEventArgs e) { if (Modes.SelectedItem is GridViewItem { Tag: string t }) S.AfterPeek = t; }
    void OnBar(object sender, SelectionChangedEventArgs e) { if (Bars.SelectedItem is GridViewItem { Tag: string t }) S.ProgressStyle = t; }
    void OnPeek(object sender, SelectionChangedEventArgs e) { if (Peek.SelectedIndex >= 0) S.MediaPeekMs = PeekChoices[Peek.SelectedIndex].Ms; }
    void OnHide(object sender, SelectionChangedEventArgs e) { if (Hide.SelectedIndex >= 0) S.MediaHideMs = HideChoices[Hide.SelectedIndex].Ms; }
}
