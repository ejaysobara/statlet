#pragma once
// Rules and card contents for the takeover widgets (volume, battery, connection, Focus): each briefly opens the island.
// Header-only and free of drawing and OS calls so the tests can drive it directly.
#include <algorithm>
#include <cmath>
#include <string>
#include "tokens.h"

// What the island shows while a takeover is open: an icon glyph (Segoe Fluent Icons), a title and a detail line, and
// optionally a bar with its value beside it (progress < 0: no bar; the detail then sits under the title instead of beside it).
struct Card {
    wchar_t glyph;
    std::wstring title, body;
    float progress;
    Rgba color;
    int holdMs;
    std::wstring value;  // shown at the end of the bar
};

// `device`: the output's name, shown as the detail.
inline Card VolumeCard(int percent, bool muted, Rgba accent, const Tokens& t, std::wstring device = {}) {
    return {wchar_t(muted ? 0xE74F : 0xE767), L"Volume", std::move(device), muted ? 0.f : percent / 100.f, muted ? t.ink2 : accent, t.takeoverVolumeMs,
            muted ? L"Muted" : std::to_wstring(percent) + L"%"};
}

// "About 40 min left" or "About 1 h 20 min left". Right after unplugging Windows has no estimate yet (seconds < 0) and
// the card says so, as the Windows battery flyout does.
inline std::wstring TimeLeft(int seconds) {
    if (seconds < 0) return L"Estimating time left";
    int minutes = std::max(1, seconds / 60), hours = minutes / 60;
    return L"About " + (hours ? std::to_wstring(hours) + L" h " + std::to_wstring(minutes % 60) : std::to_wstring(minutes)) + L" min left";
}

enum class BatteryEvent { None, Low, Critical, Charging, Unplugged };

// Edge-triggered: Low once when the charge falls to lowAt on battery, Critical once at critAt, Charging when the charger is plugged in.
// Plugging in re-arms both warnings. Feed it every power notification; unchanged input yields None.
class BatteryWatch {
public:
    BatteryWatch(int lowAt, int critAt) : lowAt(lowAt), critAt(critAt) {}
    BatteryEvent Update(int percent, bool onAc) {
        BatteryEvent e = BatteryEvent::None;
        if (onAc) {
            if (haveAc && !wasAc) e = BatteryEvent::Charging;
            warned = 0;
        } else if (haveAc && wasAc) {
            e = BatteryEvent::Unplugged;  // any low/critical warning follows on the next notification
        } else if (percent <= critAt && warned < 2) {
            warned = 2;
            e = BatteryEvent::Critical;
        } else if (percent <= lowAt && warned < 1) {
            warned = 1;
            e = BatteryEvent::Low;
        }
        wasAc = onAc;
        haveAc = true;
        return e;
    }

private:
    int lowAt, critAt, warned = 0;  // warned: 0 none, 1 low shown, 2 critical shown
    bool wasAc = false, haveAc = false;
};

// `secondsLeft`: Windows' estimate of the time on battery (-1 when unknown).
inline Card BatteryCard(BatteryEvent e, int percent, Rgba accent, const Tokens& t, int secondsLeft = -1) {
    std::wstring pct = std::to_wstring(percent) + L"%";
    if (e == BatteryEvent::Charging) return {wchar_t(0xE83E), L"Charging", L"Plugged in", percent / 100.f, accent, t.takeoverBatteryMs, pct};
    if (e == BatteryEvent::Unplugged) return {wchar_t(0xE850 + 9), L"On battery", TimeLeft(secondsLeft), percent / 100.f, accent, t.takeoverBatteryMs, pct};
    bool crit = e == BatteryEvent::Critical;
    return {wchar_t(0xE850), crit ? L"Battery critical" : L"Battery low", TimeLeft(secondsLeft), percent / 100.f, crit ? t.critical : t.warn,
            t.takeoverBatteryMs, pct};
}

enum class NetEvent { None, Lost, Back };

// Call Settle once the connection has stayed in a state long enough (the app waits netLostDelayMs before settling on offline).
// "Back" only follows a "Lost" that was shown, so a blip shorter than the delay says nothing.
class NetWatch {
public:
    NetEvent Settle(bool online) {
        if (!online && !lost) return lost = true, NetEvent::Lost;
        if (online && lost) return lost = false, NetEvent::Back;
        return NetEvent::None;
    }

private:
    bool lost = false;
};

inline Card NetCard(NetEvent e, Rgba accent, const Tokens& t) {
    return e == NetEvent::Lost ? Card{wchar_t(0xE783), L"Offline", L"No internet connection", -1.f, t.warn, t.takeoverNetMs}
                               : Card{wchar_t(0xE701), L"Back online", L"Internet connection restored", -1.f, accent, t.takeoverNetMs};
}

// The Focus countdown. The app's one-shot timer ends it; this only answers "how many minutes are left" for the rest line.
class FocusTimer {
public:
    void Start(double now, double seconds) { end = now + seconds; }
    void Stop() { end = -1; }
    bool Running() const { return end >= 0; }
    int MinutesLeft(double now) const { return Running() ? std::max(0, int(std::ceil((end - now) / 60))) : 0; }

private:
    double end = -1;
};

inline Card FocusCard(bool done, int minutes, Rgba accent, const Tokens& t) {
    std::wstring len = std::to_wstring(minutes) + L" min";
    return {wchar_t(0xE916), done ? L"Focus complete" : L"Focus started", done ? len + L" done. Take a break." : len + L", one thing at a time", -1.f, accent,
            t.takeoverFocusMs};
}
