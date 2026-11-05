using System.Diagnostics;

namespace StatletSettings;

// Opening Settings starts the island when it is not running. The island never starts Settings by itself
// (only its tray menu item does, on a click), so the two can never launch each other in a loop.
public static class IslandLauncher
{
    public const string MutexName = @"Local\Statlet.Island";  // held by the running island (island/src/main.cpp)

    public static bool IsRunning(string mutexName = MutexName)
    {
        if (!Mutex.TryOpenExisting(mutexName, out var held)) return false;
        held.Dispose();
        return true;
    }

    // Starts the island unless it is running or its exe is missing; returns whether it launched one.
    // `launch` is a seam for tests; by default the exe is started through explorer.exe, so it is not a child of this app
    // (it keeps running when Settings closes, and does not inherit this process's environment).
    public static bool EnsureRunning(string exePath, string mutexName = MutexName, Action<string>? launch = null)
    {
        if (IsRunning(mutexName) || !File.Exists(exePath)) return false;
        (launch ?? StartViaExplorer)(exePath);
        return true;
    }

    static void StartViaExplorer(string exePath) =>
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{exePath}\"") { UseShellExecute = true })?.Dispose();
}
