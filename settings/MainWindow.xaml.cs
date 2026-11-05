using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace StatletSettings;

public sealed partial class MainWindow : Window
{
    readonly Dictionary<string, UserControl> pages = new();

    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr hwnd);

    public MainWindow()
    {
        InitializeComponent();
        SystemBackdrop = new Microsoft.UI.Xaml.Media.MicaBackdrop();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleBar);
        AppWindow.SetIcon(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "statlet-app.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;  // Resize takes physical pixels
        AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(1120 * scale), (int)(820 * scale)));
        Closed += (_, _) =>
        {
            Store.Hold(0);  // let the island close again
            Store.Save();   // a change still waiting on the save delay
        };
        Scroller.SizeChanged += (_, _) => Fit();
        Nav.SelectedItem = GeneralItem;
    }

    void OnNavigate(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is not NavigationViewItem { Tag: string tag } || Page is null) return;
        if (!pages.TryGetValue(tag, out var page))
            pages[tag] = page = tag switch { "media" => new NowPlayingPage(), "look" => new AppearancePage(), _ => new GeneralPage() };
        Page.Content = page;
        bool own = page is AppearancePage;  // it scrolls its controls under a fixed preview
        Scroller.VerticalScrollMode = own ? ScrollMode.Disabled : ScrollMode.Enabled;
        Scroller.VerticalScrollBarVisibility = own ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto;
        Scroller.ChangeView(null, 0, null, disableAnimation: true);
        Fit();
    }

    // The page is laid out at most 1000 DIP wide; past that it scales up by width/1000, capped at 1.4.
    // The nav scales with it at 3/4 the rate (sn), so its 260 DIP pane grows to 338 at s = 1.4.
    void Fit()
    {
        double h = Scroller.ActualHeight - 48;  // minus Scroller padding
        if (Nav.ActualWidth <= 0 || h <= 0) return;
        // s from the window, not the Scroller: solves 1000s = W - 80 - 260sn with sn = 1 + 0.75(s - 1), so pane and page settle in one pass
        double s = Math.Clamp((Nav.ActualWidth - 145) / 1195, 1.0, 1.4), sn = 1 + (s - 1) * 0.75;
        double w = Nav.ActualWidth - 260 * sn - 80;
        if (w <= 0) return;
        Nav.OpenPaneLength = 260 * sn;
        foreach (var item in Nav.MenuItems.OfType<NavigationViewItem>())
        {
            item.FontSize = 14 * sn;
            item.Height = sn == 1 ? double.NaN : 36 * sn;  // NaN keeps the template's 36 DIP minimum
            if (Find<Viewbox>(item, "IconBox") is { } icon) icon.Height = 16 * sn;
        }
        Page.Width = Math.Min(w, 1000);
        Page.Height = Page.Content is AppearancePage ? h / s : double.NaN;  // fills the viewport once scaled
        Zoom.Width = Page.Width * s;
    }

    static T? Find<T>(DependencyObject root, string name) where T : FrameworkElement
    {
        for (int i = 0; i < Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChild(root, i);
            if (child is T t && t.Name == name) return t;
            if (Find<T>(child, name) is { } hit) return hit;
        }
        return null;
    }
}
