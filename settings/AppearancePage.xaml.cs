using System.ComponentModel;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace StatletSettings;

// The appearance lab. The preview here is drawn in XAML from the same values the island reads; the island itself
// also rebuilds on every change and holds the picked view open while this page shows.
public sealed partial class AppearancePage : UserControl
{
    static readonly (string Name, string Hex)[] TintChoices = [("Graphite (default)", "#1C1E24"), ("Ink", "#0B0D12"), ("Plum", "#2A1F33"), ("Pine", "#17271F"), ("Navy", "#141F33")];
    static readonly (string Name, string Hex)[] AccentChoices = [("Sky (default)", "#4CC2FF"), ("Mint", "#6EE7A0"), ("Amber", "#FFB020"), ("Rose", "#FF7AB6"), ("Lilac", "#B69CFF")];
    // Open sizes in epx, as in island/src/tokens.h: resting, hover, card, music
    static readonly (double W, double H)[] Sizes = [(176, 34), (244, 68), (368, 104), (368, 168)];

    public Settings S => Store.Current;
    int view;
    Storyboard? story;
    int replays;  // a newer replay or view pick cancels an older replay's return

    public AppearancePage()
    {
        InitializeComponent();
        foreach (var (name, hex) in TintChoices) Tints.Children.Add(MakeSwatch(name, hex, () => S.GlassTint = hex));
        foreach (var (name, hex) in AccentChoices) Accents.Children.Add(MakeSwatch(name, hex, () => S.AccentColor = hex));
        PropertyChangedEventHandler changed = (_, _) => UpdatePreview(animate: false);
        Loaded += (_, _) => { S.PropertyChanged += changed; Store.Hold(view); UpdatePreview(animate: false); };
        Unloaded += (_, _) => { S.PropertyChanged -= changed; Store.Hold(0); };
        ViewRest.IsChecked = true;
    }

    public string Percent(double v) => $"{v * 100:0}%";
    public string Px(double v) => $"{v:0} px";
    public Visibility Hidden(bool on) => on ? Visibility.Collapsed : Visibility.Visible;

    static Color ParseColor(string hex) =>
        Color.FromArgb(255, Convert.ToByte(hex[1..3], 16), Convert.ToByte(hex[3..5], 16), Convert.ToByte(hex[5..7], 16));

    ToggleButton MakeSwatch(string name, string hex, Action pick)
    {
        var button = new ToggleButton
        {
            Style = (Style)Resources["Swatch"],
            Content = new Ellipse { Width = 26, Height = 26, Fill = new SolidColorBrush(ParseColor(hex)), Stroke = new SolidColorBrush(Color.FromArgb(0x33, 255, 255, 255)) },
            Tag = hex,
        };
        AutomationProperties.SetName(button, name);
        ToolTipService.SetToolTip(button, name);
        button.Click += (_, _) => pick();
        return button;
    }

    static void CheckOnly(Panel group, Func<object, bool> isOn)
    {
        foreach (var b in group.Children.OfType<ToggleButton>()) b.IsChecked = isOn(b.Tag);
    }

    static double Num(object tag) => double.Parse((string)tag, System.Globalization.CultureInfo.InvariantCulture);

    // A value saved between steps (by the old sliders) lights the nearest step.
    static void CheckNearest(Panel group, double value)
    {
        var near = group.Children.OfType<ToggleButton>().Where(b => (string)b.Tag != "instant").MinBy(b => Math.Abs(Num(b.Tag) - value));
        CheckOnly(group, tag => (string)tag == (string?)near?.Tag);  // by text: each Tag read is a new string
    }

    void UpdatePreview(bool animate)
    {
        CheckOnly(Tints, tag => (string)tag == S.GlassTint);
        CheckOnly(Accents, tag => (string)tag == S.AccentColor);
        CheckOnly(TextSizes, tag => Math.Abs(double.Parse((string)tag, System.Globalization.CultureInfo.InvariantCulture) - S.TextScale) < 0.01);
        if (S.InstantOpen) CheckOnly(SpeedSteps, tag => (string)tag == "instant");
        else CheckNearest(SpeedSteps, S.Speed);
        CheckNearest(BounceSteps, S.Bounce);
        var path = Motion.SpringPath(0, 100, S.Speed, S.Bounce);
        int frames = path.FindLastIndex(x => Math.Abs(100 - x) > 2) + 1;  // until within 2%, as the spec's settle time
        MotionTime.Text = S.InstantOpen ? "Opens and closes at once, with no animation" : $"Opens in about {Math.Round(frames * 100 / 60.0) * 10:0} ms";

        var tint = ParseColor(S.GlassTint);
        Glass.TintColor = tint;
        Glass.TintOpacity = S.TintOpacity;
        Glass.FallbackColor = Color.FromArgb(0xF0, tint.R, tint.G, tint.B);
        var accent = S.FollowAccent ? (Color)Application.Current.Resources["SystemAccentColorLight2"] : ParseColor(S.AccentColor);
        var accentBrush = new SolidColorBrush(accent);
        var ink = new SolidColorBrush(Color.FromArgb(255, 0xF5, 0xF6, 0xF8));
        foreach (var b in new[] { RestCpu, RestRam, HoverCpu, HoverRam, MediaCpu, MediaRam }) b.Background = ink;
        foreach (var b in new[] { CardBar, MediaBar }) b.Background = accentBrush;
        RestFocus.Foreground = accentBrush;
        CardIcon.Foreground = accentBrush;

        // Text sizes as in tokens.h (clock 12, caption 11, label 12, body 13, title 16), scaled
        double s = S.TextScale;
        foreach (var t in new[] { RestTime, RestFocus }) t.FontSize = Math.Round(12 * s);
        foreach (var t in new[] { CpuLabel, RamLabel, MediaElapsed, MediaLeft }) t.FontSize = Math.Round(11 * s);
        foreach (var t in new[] { CpuValue, RamValue }) t.FontSize = Math.Round(12 * s);
        foreach (var t in new[] { CardValue, CardDetail, MediaArtist }) t.FontSize = Math.Round(13 * s);
        foreach (var t in new[] { CardTitle, MediaTitle }) t.FontSize = Math.Round(16 * s);

        RestFace.Visibility = view == 0 ? Visibility.Visible : Visibility.Collapsed;
        HoverFace.Visibility = view == 1 ? Visibility.Visible : Visibility.Collapsed;
        CardFace.Visibility = view == 2 ? Visibility.Visible : Visibility.Collapsed;
        MediaFace.Visibility = view == 3 ? Visibility.Visible : Visibility.Collapsed;
        // The island's radius runs from resting to open with its height (IslandRadius in island/src/shape_math.h)
        double grow = Math.Clamp((Sizes[view].H - Sizes[0].H) / (Sizes[3].H - Sizes[0].H), 0, 1);
        Island.CornerRadius = new CornerRadius(S.CollapsedR + (S.ExpandedR - S.CollapsedR) * grow);
        double width = Sizes[view].W;
        if (view == 0)  // the resting island fits its line: content plus 12 each side, in steps of 4, 124 to 260
        {
            RestFace.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            width = Math.Clamp(Math.Ceiling((RestFace.DesiredSize.Width + 24) / 4) * 4, 124, 260);
        }
        Resize(width, Sizes[view].H, animate);
    }

    // The size is always set as the local value; an animation runs from the current size and lets go when done,
    // since a held storyboard would hide every later size change (a resting width refit, a replay's collapse).
    void Resize(double w, double h, bool animate)
    {
        double fromW = Island.ActualWidth, fromH = Island.ActualHeight;
        story?.Stop();
        Island.Width = w;
        Island.Height = h;
        if (!animate || S.InstantOpen || !new Windows.UI.ViewManagement.UISettings().AnimationsEnabled) return;
        story = new Storyboard();
        foreach (var (prop, from, to) in new[] { ("Width", fromW, w), ("Height", fromH, h) })
        {
            var a = new DoubleAnimationUsingKeyFrames { EnableDependentAnimation = true, FillBehavior = FillBehavior.Stop };
            var path = Motion.SpringPath(from, to, S.Speed, S.Bounce);
            for (int i = 0; i < path.Count; i++)
                a.KeyFrames.Add(new LinearDoubleKeyFrame { Value = path[i], KeyTime = TimeSpan.FromSeconds((i + 1) / 60.0) });
            Storyboard.SetTarget(a, Island);
            Storyboard.SetTargetProperty(a, prop);
            story.Children.Add(a);
        }
        story.Begin();
    }

    void OnView(object sender, RoutedEventArgs e)
    {
        var picked = (ToggleButton)sender;
        view = int.Parse((string)picked.Tag);
        replays++;
        foreach (var b in ((Panel)picked.Parent).Children.OfType<ToggleButton>()) b.IsChecked = b == picked;
        Store.Hold(view);
        UpdatePreview(animate: true);
    }

    void OnWall(object sender, RoutedEventArgs e)
    {
        bool dark = ReferenceEquals(sender, WallDark);
        WallDark.IsChecked = dark;
        WallLight.IsChecked = !dark;
        Stage.Background = new SolidColorBrush(dark ? Color.FromArgb(255, 0x28, 0x30, 0x44) : Color.FromArgb(255, 0xE7, 0xE2, 0xD6));
        BlobA.Fill = new SolidColorBrush(dark ? Color.FromArgb(255, 0x3F, 0x5A, 0x7A) : Color.FromArgb(255, 0xF2, 0xB8, 0x80));
        BlobB.Fill = new SolidColorBrush(dark ? Color.FromArgb(255, 0x6B, 0x4A, 0x6E) : Color.FromArgb(255, 0x9C, 0xC7, 0xB4));
    }

    void OnSize(object sender, RoutedEventArgs e) =>
        S.TextScale = double.Parse((string)((ToggleButton)sender).Tag, System.Globalization.CultureInfo.InvariantCulture);

    void OnReset(object sender, RoutedEventArgs e) => S.ResetAppearance();

    // Picking a step plays it, so the choice is felt at once.
    void OnSpeed(object sender, RoutedEventArgs e)
    {
        var tag = ((ToggleButton)sender).Tag;
        S.InstantOpen = (string)tag == "instant";
        if (!S.InstantOpen) S.Speed = Num(tag);
        OnReplay(sender, e);
    }

    void OnBounce(object sender, RoutedEventArgs e)
    {
        S.Bounce = Num(((ToggleButton)sender).Tag);
        OnReplay(sender, e);
    }

    // Plays the open motion again: collapse, then reopen to the picked view.
    void OnReplay(object sender, RoutedEventArgs e)
    {
        int token = ++replays;
        if (view == 0)  // resting opens as a hover does, with the hover rows, then rests again
        {
            view = 1;
            UpdatePreview(animate: true);
            view = 0;
            DispatcherQueue.TryEnqueue(async () => { await Task.Delay(1400); if (token == replays) UpdatePreview(animate: true); });
            return;
        }
        Resize(Sizes[0].W, Sizes[0].H, animate: false);
        Island.UpdateLayout();  // the open animation starts from the resting size
        UpdatePreview(animate: true);
    }
}
