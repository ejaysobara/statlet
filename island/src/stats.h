#pragma once
// System usage for the CPU/RAM rest-line widget: sampling, severity levels and the sustained-high-usage rule.
// Header-only and free of drawing code so the tests can drive it directly.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include "tokens.h"

enum class Level { Normal, Warn, Critical };

// Amber from warnAt percent, red from critAt percent (Tokens::warnAt, critAt).
constexpr Level LevelFor(float percent, const Tokens& t) {
    return percent >= t.critAt ? Level::Critical : percent >= t.warnAt ? Level::Warn : Level::Normal;
}

inline Rgba LevelColor(Level l, Rgba normal, const Tokens& t) { return l == Level::Critical ? t.critical : l == Level::Warn ? t.warn : normal; }

// True once usage has stayed above `threshold` for `holdSeconds` without a dip; stays true while above, clears on the first dip.
// `now` is any monotonic clock in seconds, and samples may arrive at any interval.
class Sustain {
public:
    Sustain(float threshold, double holdSeconds) : threshold(threshold), hold(holdSeconds) {}
    bool Update(float percent, double now) {
        if (percent <= threshold) {
            since = -1;
            return false;
        }
        if (since < 0) since = now;
        return now - since >= hold;
    }

private:
    float threshold;
    double hold, since = -1;
};

// CPU use between two consecutive calls, from the kernel's cumulative times. The first call returns 0.
class CpuSampler {
public:
    float Percent() {
        auto u64 = [](FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
        ULONGLONG i = u64(idle), total = u64(kernel) + u64(user);  // kernel time includes idle
        float result = 0;
        if (havePrev && total > prevTotal) result = 100.f * float((total - prevTotal) - (i - prevIdle)) / float(total - prevTotal);
        prevIdle = i;
        prevTotal = total;
        havePrev = true;
        return std::clamp(result, 0.f, 100.f);
    }

private:
    ULONGLONG prevIdle = 0, prevTotal = 0;
    bool havePrev = false;
};

inline float MemoryPercent() {
    MEMORYSTATUSEX m{sizeof m};
    return GlobalMemoryStatusEx(&m) ? float(m.dwMemoryLoad) : 0.f;
}
