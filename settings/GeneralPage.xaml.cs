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
        FillOrder();
    }

    // The Order list: labels in the island's order; RestOrder holds the keys.
    static readonly Dictionary<string, string> BlockLabels = new() { ["status"] = "Status icon", ["clock"] = "Date and time", ["usage"] = "CPU and memory bars" };
    readonly System.Collections.ObjectModel.ObservableCollection<string> order = [];

    void FillOrder()
    {
        order.Clear();
        foreach (string key in S.RestOrder.Split(',')) order.Add(BlockLabels[key]);
        OrderList.ItemsSource = order;
    }

    void SaveOrder() => S.RestOrder = string.Join(',', order.Select(label => BlockLabels.First(p => p.Value == label).Key));

    void OnOrderDragged(ListViewBase sender, DragItemsCompletedEventArgs e) => SaveOrder();

    void OnOrderSelected(object sender, SelectionChangedEventArgs e)
    {
        OrderUp.IsEnabled = OrderList.SelectedIndex > 0;
        OrderDown.IsEnabled = OrderList.SelectedIndex >= 0 && OrderList.SelectedIndex < order.Count - 1;
    }

    void MoveSelected(int by)
    {
        int from = OrderList.SelectedIndex, to = from + by;
        if (from < 0 || to < 0 || to >= order.Count) return;
        order.Move(from, to);
        OrderList.SelectedIndex = to;
        SaveOrder();
    }

    void OnOrderUp(object sender, RoutedEventArgs e) => MoveSelected(-1);
    void OnOrderDown(object sender, RoutedEventArgs e) => MoveSelected(1);

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
