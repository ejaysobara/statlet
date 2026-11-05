#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Where the island's host window sits: centered at the top of the primary monitor's work area, sized for its DPI.
// Plain numbers in, plain numbers out, so it is tested without a monitor (Place in main.cpp reads the monitor and calls this).
struct Placement {
    int x = 0, y = 0, w = 0, h = 0;
    float scale = 1;  // DPI / 96
};

inline Placement PlacementFor(int workLeft, int workTop, int workRight, unsigned dpi, float hostW, float hostH) {
    Placement p;
    p.scale = dpi / 96.f;
    p.w = int(std::ceil(hostW * p.scale));
    p.h = int(std::ceil(hostH * p.scale));
    p.x = workLeft + (workRight - workLeft - p.w) / 2;
    p.y = workTop;
    return p;
}

// One display as Windows reports it (EnumDisplayMonitors): its work area in physical px, its effective DPI (0 when the
// DPI query failed) and whether it is the primary.
struct Monitor {
    int left = 0, top = 0, right = 0, bottom = 0;
    unsigned dpi = 0;
    bool primary = false;
};

// Places the island on the primary, or on the first usable monitor while none is flagged primary (mid-switch). False
// (keep the last good placement) when the primary's readings failed or there is no monitor at all.
inline bool PlaceOn(const std::vector<Monitor>& monitors, float hostW, float hostH, Placement& p) {
    auto usable = [](const Monitor& m) { return m.right > m.left && m.bottom > m.top && m.dpi > 0; };
    const Monitor* pick = nullptr;
    for (auto& m : monitors) if (m.primary) pick = &m;
    if (!pick) for (auto& m : monitors) if (usable(m)) { pick = &m; break; }
    if (!pick || !usable(*pick)) return false;
    p = PlacementFor(pick->left, pick->top, pick->right, pick->dpi, hostW, hostH);
    p.x = std::max(p.x, pick->left);  // a window wider than the work area starts at its left edge, never off it
    return true;
}
