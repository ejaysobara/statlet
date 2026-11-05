#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "tokens.h"

// Pure math shared by the shell and the tests: the spring model, rounded-rect hit testing and accent contrast.

// Damped spring (mass 1). The compositor runs the real animation; this model tracks the same curve on demand so
// hit testing knows the island's shape mid-flight without the app doing any per-frame work.
struct Spring {
    double value, target, velocity = 0, stiffness, damping;

    Spring(double v, double k, double c) : value(v), target(v), stiffness(k), damping(c) {}

    bool Settled() const { return std::fabs(target - value) < 0.1 && std::fabs(velocity) < 1; }

    void Snap() { value = target; velocity = 0; }

    void Step(double dt) {
        for (double t = 0; t < dt && !(value == target && velocity == 0); t += 1 / 480.0) {
            double h = std::min(1 / 480.0, dt - t);
            velocity += (stiffness * (target - value) - damping * velocity) * h;
            value += velocity * h;
            if (Settled()) Snap();
        }
    }
};

// Windows.UI.Composition's spring takes a period and damping ratio; these convert the spec's stiffness and damping.
// Its Period acts as 1/omega, not one oscillation: measured on screen, 2*pi/omega ran the island about 6x too slow.
inline double SpringPeriod(double stiffness) { return 1 / std::sqrt(stiffness); }
inline double SpringDampingRatio(double stiffness, double damping) { return damping / (2 * std::sqrt(stiffness)); }

// Corner radius follows the island's height between the collapsed and expanded sizes.
inline double IslandRadius(const Tokens& t, double h) {
    double p = std::clamp((h - t.collapsedH) / (t.expandedH - t.collapsedH), 0.0, 1.0);
    return t.collapsedR + (t.expandedR - t.collapsedR) * p;
}

// The collapsed island's width for a rest line `content` epx wide, padded padX each side (default collapsedPadX).
// Rounding to a step keeps small text changes (a minute ticking over) from re-springing the width.
inline float FitCollapsedWidth(float content, const Tokens& t, float padX = -1) {
    float w = std::ceil((content + 2 * (padX < 0 ? t.collapsedPadX : padX)) / t.widthStep) * t.widthStep;
    return std::clamp(w, t.collapsedMinW, t.collapsedMaxW);
}

inline bool InRoundedRect(double x, double y, double left, double top, double w, double h, double r) {
    if (x < left || y < top || x >= left + w || y >= top + h) return false;
    r = std::min({r, w / 2, h / 2});
    double cx = std::clamp(x, left + r, left + w - r), cy = std::clamp(y, top + r, top + h - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

// WCAG relative luminance and contrast ratio, alpha ignored.
inline double Luminance(Rgba c) {
    auto lin = [](uint8_t v) { double s = v / 255.0; return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

inline double Contrast(Rgba a, Rgba b) {
    double la = Luminance(a), lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Getting out of the way: the island slides up for a maximized window and hides for a fullscreen one.
enum class Away { No, Slid, Hidden };

// What the island knows about the foreground window, read in main.cpp's FrontWindowState.
struct FrontWindow {
    bool usable = false;  // exists, is not the island, is visible and not minimized
    bool shell = false;   // the desktop or the taskbar
    bool onPrimary = false;
    bool covers = false;  // its rect covers the whole primary monitor
    bool zoomed = false, caption = false;
};

inline Away AwayFor(const FrontWindow& f) {
    if (!f.usable || f.shell || !f.onPrimary) return Away::No;
    if (f.covers && !f.zoomed) return Away::Hidden;   // borderless fullscreen games
    if (f.covers && !f.caption) return Away::Hidden;  // maximized without a title bar
    return f.zoomed ? Away::Slid : Away::No;
}

// One top-level window, front of the z-order first, as main.cpp's FrontWindowState reads it. Windows nobody looks at
// (hidden, minimized, cloaked, tool windows, click-through overlays, the island, the taskbar) are left out of the list.
struct ZWindow {
    FrontWindow f;  // f.usable is true
    bool topmost = false;
    const void* id = nullptr;  // the HWND, for the log
};

// The window the island stays out of the way of: the frontmost one on the primary, whichever monitor has the focus.
// Small always-on-top windows (menus, the Alt+Tab switcher, picture-in-picture) are looked past; a topmost window that
// covers the primary counts.
inline const ZWindow* FrontOnPrimary(const std::vector<ZWindow>& z) {
    for (auto& w : z)
        if (w.f.onPrimary && (!w.topmost || w.f.covers)) return &w;
    return nullptr;
}

// Going away applies at once (a fullscreen game hides the island instantly). Coming back to No waits for Settle(), called
// by a short timer, and is dropped if the window goes away again first: Alt+Tab passes the foreground through the
// taskbar and empty windows for a few ms (island.log: slid -> no -> slid within 55 ms).
struct AwayGate {
    Away shown = Away::No;
    bool returning = false;  // a return to No is waiting for Settle()

    bool See(Away now) {  // a new reading; true when `shown` changed
        returning = now == Away::No && shown != Away::No;
        if (returning || now == shown) return false;
        shown = now;
        return true;
    }
    bool Settle(Away now) {  // the wait ran out; `now` is a fresh reading
        if (!returning) return false;
        returning = false;
        if (now != Away::No) return See(now);
        shown = Away::No;
        return true;
    }
};

// The spec's accent rule: AccentLight2, stepping lighter until it has 3:1 against the glass tint.
inline Rgba PickAccent(Rgba light2, Rgba light3, Rgba glass) {
    return Contrast(light2, glass) >= 3 ? light2 : light3;
}
