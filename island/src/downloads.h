#pragma once
// Download activity in the Downloads folder (v2 item 7). Browsers write `name.ext.crdownload` (or .part and friends) and
// rename it when done, so activity is visible without reading anything: no percent, no speed. Header-only for the tests.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The partial-file extensions browsers use (Chromium, Firefox, Opera, Safari-style), compared case-insensitively.
inline std::size_t PartialExtLen(std::wstring_view name) {
    for (std::wstring_view ext : {L".crdownload", L".part", L".partial", L".download", L".opdownload"}) {
        if (name.size() <= ext.size()) continue;
        std::wstring_view tail = name.substr(name.size() - ext.size());
        bool same = true;
        for (std::size_t i = 0; i < ext.size() && same; ++i) same = (tail[i] | 0x20) == ext[i];  // the extensions are ASCII letters and a dot
        if (same && tail[0] == L'.') return ext.size();
    }
    return 0;
}
inline bool IsPartialName(std::wstring_view name) { return PartialExtLen(name) != 0; }

struct PartialFile {
    std::wstring name;
    std::uint64_t size = 0;
};

// Idle -> Active (a partial grew in the last staleMs) -> Done (the partials are gone and the finished file is there; shown
// doneMs) -> Idle. A partial that has not grown for staleMs is ignored, so a crashed browser does not leave the icon on.
class DownloadTracker {
public:
    enum class State { Idle, Active, Done };
    explicit DownloadTracker(std::uint64_t staleMs = 60000, std::uint64_t doneMs = 2000) : staleMs(staleMs), doneMs(doneMs) {}

    // `partials` is the folder's partial files now; `finishedExists(name)` says whether the file a partial turns into
    // (its name without the partial extension) is there. Returns the new state.
    template <class Exists>
    State Observe(std::uint64_t now, const std::vector<PartialFile>& partials, Exists finishedExists) {
        now += staleMs;  // all times are shifted by staleMs so "stale at seed time" never needs a negative time (first minute after boot)
        bool finished = false;
        for (auto it = seen.begin(); it != seen.end();) {  // partials that vanished: renamed (done) or deleted (cancelled)
            bool gone = true;
            for (auto& p : partials) gone = gone && p.name != it->name;
            if (!gone) { ++it; continue; }
            finished = finished || finishedExists(it->name.substr(0, it->name.size() - PartialExtLen(it->name)));
            it = seen.erase(it);
        }
        for (auto& p : partials) {
            Entry* e = nullptr;
            for (auto& s : seen) if (s.name == p.name) e = &s;
            if (!e) seen.push_back({p.name, p.size, now});  // a new partial is a download starting
            else if (p.size > e->size) e->size = p.size, e->grew = now;
        }
        bool active = false;
        for (auto& s : seen) active = active || now - s.grew < staleMs;
        if (active) state = State::Active;
        else if (state == State::Active) {
            state = finished ? State::Done : State::Idle;
            doneAt = now;
        } else if (state == State::Done && now - doneAt >= doneMs) state = State::Idle;
        return state;
    }
    // Partials already there when watching starts count as old: a crashed browser's leftovers must not light the icon.
    void Seed(std::uint64_t now, const std::vector<PartialFile>& partials) {
        for (auto& p : partials) seen.push_back({p.name, p.size, now});  // shifted time: `now` here is already `now + staleMs - staleMs`
    }
    State state = State::Idle;
    // The 1 Hz timer runs while this holds. A stale partial does not count: it would keep the island waking for a dead download;
    // the folder watcher notices if it grows again.
    bool Busy(std::uint64_t now) const {
        now += staleMs;
        for (auto& s : seen) if (now - s.grew < staleMs) return true;
        return state == State::Done;
    }

private:
    struct Entry {
        std::wstring name;
        std::uint64_t size, grew;
    };
    std::vector<Entry> seen;
    std::uint64_t staleMs, doneMs, doneAt = 0;
};
