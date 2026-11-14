#pragma once
// Widget registry (v2): each widget says when it is wanted, and runs only while it is. A widget that is turned off, or
// one whose output nobody can see, has no timer and no watcher, so it costs nothing. Reconcile() is cheap and idempotent:
// call it whenever an input to `wanted` changes (away, hidden, display, lock, settings).
#include <array>
#include <cstddef>

struct WidgetState {
    bool away, hidden, displayOff, locked;  // the island's visibility (power.h rules)
};

struct Widget {
    const char* name;
    bool (*wanted)(const WidgetState&);
    void (*start)();
    void (*stop)();
    bool enabled = true;  // the hook for a per-widget setting; every v1 widget is on
    bool running = false;
};

// Starts and stops widgets to match `s`. Returns how many changed.
template <std::size_t N>
int Reconcile(std::array<Widget, N>& widgets, const WidgetState& s) {
    int changed = 0;
    for (Widget& w : widgets) {
        bool want = w.enabled && w.wanted(s);
        if (want == w.running) continue;
        w.running = want;
        (want ? w.start : w.stop)();
        ++changed;
    }
    return changed;
}

// The v1 rules, unchanged: the cursor poll only while the island is on screen; the clock and usage polls while the
// display is on, the session unlocked and the island not hidden.
inline bool Seen(const WidgetState& s) { return !s.displayOff && !s.locked; }
inline bool WantHover(const WidgetState& s) { return Seen(s) && !s.away; }
inline bool WantRest(const WidgetState& s) { return Seen(s) && !s.hidden; }
