#pragma once
// OS notification sources for the takeover widgets. The OS calls back on its own threads; each callback only posts a
// message to the window, so the app sleeps between events and does no polling.
#include <windows.h>
#include <string>

// Posts `msg` with wParam = volume percent and lParam = muted whenever the default output device's volume or mute changes.
// When the default device itself changes, posts it once with wParam = UINT_MAX: call RebindVolume on the window thread.
void WatchVolume(HWND hwnd, UINT msg);
void RebindVolume();
std::wstring OutputDeviceName();  // the default output's name, read when it was bound

// Posts `msg` with wParam = 1 when Windows reports internet access, 0 when it does not, on every change.
void WatchNetwork(HWND hwnd, UINT msg);
bool InternetOnline();
