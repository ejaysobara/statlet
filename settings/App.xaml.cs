using Microsoft.UI.Xaml;

namespace StatletSettings;

public partial class App : Application
{
    Window? window;

    public App()
    {
        InitializeComponent();
        // A crash leaves its reason next to the settings file, since this app has no console.
        UnhandledException += (_, e) => File.WriteAllText(
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Statlet", "settings-crash.txt"), e.Exception.ToString());
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try { Store.RepairRunPath(); }
        catch (Exception e) when (e is UnauthorizedAccessException or System.Security.SecurityException) { }
        window = new MainWindow();
        window.Activate();
        StartIsland();
        // A second launch (Program.cs hands it to us) brings this window forward, and restarts the island if it has quit.
        var dispatcher = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        Microsoft.Windows.AppLifecycle.AppInstance.GetCurrent().Activated += (_, _) => dispatcher.TryEnqueue(() =>
        {
            window.Activate();
            StartIsland();
        });
    }

    static void StartIsland()
    {
        try { IslandLauncher.EnsureRunning(Store.IslandExe); }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception or InvalidOperationException) { }  // no island is not worth a crash
    }
}
