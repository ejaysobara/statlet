#pragma once
// Which power, display and session notifications make the island check itself again (Revalidate in main.cpp).
// Plain values in, so the rules are tested without sleeping the machine.
#include <windows.h>
#include <wtsapi32.h>

enum class PowerSetting { Other, Display, Lid };

struct PowerState {
    int display = -1, lid = -1;  // last GUID_CONSOLE_DISPLAY_STATE (0 off, 1 on, 2 dimmed) and lid (0 closed, 1 open); -1 unknown
};

// The reason to revalidate after a WM_POWERBROADCAST, or nullptr. Registering for a setting delivers its current value
// at once; that first reading only seeds the state.
inline const char* PowerReason(UINT event, PowerSetting kind, DWORD value, PowerState& s) {
    if (event == PBT_APMRESUMEAUTOMATIC) return "resume";  // always sent on resume, classic or Modern Standby
    if (event != PBT_POWERSETTINGCHANGE || kind == PowerSetting::Other) return nullptr;
    int& last = kind == PowerSetting::Display ? s.display : s.lid;
    bool back = last == 0 && value != 0;  // display off -> on or dimmed; lid closed -> open
    last = int(value);
    return back ? (kind == PowerSetting::Display ? "display on" : "lid open") : nullptr;
}

// Which timers run (W6: no wake-ups while nobody can see the island). The cursor poll only matters while the island is
// on screen; the clock and usage polls stop while the display is off, the session is locked or the island is hidden.
struct Timers {
    bool hover, rest;
};
inline Timers TimersFor(bool away, bool hidden, bool displayOff, bool locked) {
    bool seen = !displayOff && !locked;
    return {seen && !away, seen && !hidden};
}

inline const char* SessionReason(WPARAM change) {
    if (change == WTS_SESSION_UNLOCK) return "unlock";
    if (change == WTS_CONSOLE_CONNECT || change == WTS_REMOTE_CONNECT) return "connect";
    return nullptr;
}
