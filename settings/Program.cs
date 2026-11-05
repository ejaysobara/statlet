using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppLifecycle;

namespace StatletSettings;

// Our own entry point (DISABLE_XAML_GENERATED_MAIN in the project) so a second launch can hand over to the first and exit:
// one Settings window, brought forward, however many times it is opened (tray menu, shortcut, double-click).
public static class Program
{
    const string InstanceKey = "StatletSettings";

    [STAThread]
    static int Main()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        var instance = AppInstance.FindOrRegisterForKey(InstanceKey);
        if (!instance.IsCurrent)
        {
            // Off the UI thread: the redirect needs a message pump this thread does not have yet.
            var args = AppInstance.GetCurrent().GetActivatedEventArgs();
            Task.Run(async () => await instance.RedirectActivationToAsync(args)).Wait();
            return 0;
        }
        Microsoft.UI.Xaml.Application.Start(_ =>
        {
            SynchronizationContext.SetSynchronizationContext(new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
            new App();
        });
        return 0;
    }
}
