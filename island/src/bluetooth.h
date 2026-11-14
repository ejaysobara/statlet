#pragma once
// Bluetooth device connect events (v2 item 6): a paired device that goes from not connected to connected. Connect only; no
// disconnect, battery or name. The tracker is header-only for the tests; the watcher is in bluetooth.cpp.
#include <map>
#include <string>
#include <windows.h>

class BtTracker {
public:
    // A device's connected state as the watcher reports it (Added or Updated). True only for a false -> true change seen after
    // the first enumeration finished, so devices already connected when the island starts never peek.
    bool Update(const std::wstring& id, bool connected) {
        auto it = state.find(id);
        bool was = it != state.end() && it->second;
        state[id] = connected;
        return ready && connected && !was;
    }
    void Remove(const std::wstring& id) { state.erase(id); }
    void EnumerationDone() { ready = true; }

private:
    std::map<std::wstring, bool> state;
    bool ready = false;
};

// Posts `msg` (no parameters) to `hwnd` each time a paired Bluetooth device connects. The OS calls back on its own threads,
// so the callback does nothing but post. StopBluetooth ends the watch; a later WatchBluetooth starts a fresh one.
void WatchBluetooth(HWND hwnd, UINT msg);
void StopBluetooth();
