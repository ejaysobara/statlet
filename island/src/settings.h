#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <windows.h>
#include "tokens.h"

// The shared settings file (%LOCALAPPDATA%\Statlet\settings.json), written by the settings app and read by the island.
// One flat JSON object; a missing or malformed key keeps its default, and every value is clamped (the file is
// hand-editable). Example:
//   {"topOffset":10,"focusMinutes":25,"warnAt":80,"critAt":95,"openOnHighUsage":true,"afterPeek":"tuck",
//    "showWeekday":true,"progressStyle":"wave","playheadKnob":"never","artAccent":false,"mediaPeekMs":4000,"mediaHideMs":30000,"glassTint":"#1C1E24","tintOpacity":0.52,
//    "followAccent":true,"accentColor":"#4CC2FF","collapsedR":11,"expandedR":22,"textScale":1,"bounce":40,"speed":60}

// The folder holding settings.json and island.log: STATLET_DATA_DIR when set (tests), else %LOCALAPPDATA%\Statlet.
// The settings app does the same (settings\Settings.cs), so test launches never touch the real files.
inline std::wstring DataDir() {
    wchar_t buf[MAX_PATH];
    if (DWORD n = GetEnvironmentVariableW(L"STATLET_DATA_DIR", buf, MAX_PATH); n > 0 && n < MAX_PATH) return buf;
    return GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) ? std::wstring(buf) + L"\\Statlet" : L".";
}

// The raw value after "key": up to the next comma or brace, quotes stripped. Empty when the key is absent.
// shortcut: no escapes or nesting, which the settings app never writes; use a real parser if values grow strings.
inline std::string_view SettingRaw(std::string_view json, std::string_view key) {
    std::string quoted = "\"" + std::string(key) + "\"";
    size_t at = json.find(quoted);
    if (at == json.npos) return {};
    at = json.find(':', at + quoted.size());
    if (at == json.npos) return {};
    size_t end = json.find_first_of(",}", at + 1);
    std::string_view v = json.substr(at + 1, end == json.npos ? json.npos : end - at - 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n' || v.front() == '"')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n' || v.back() == '"')) v.remove_suffix(1);
    return v;
}

// A quoted string value, commas allowed (SettingRaw stops at the first one). Empty when absent or not a string.
inline std::string_view SettingString(std::string_view json, std::string_view key) {
    std::string quoted = "\"" + std::string(key) + "\"";
    size_t at = json.find(quoted);
    if (at == json.npos) return {};
    at = json.find(':', at + quoted.size());
    if (at == json.npos) return {};
    size_t open = json.find_first_not_of(" \t\r\n", at + 1);
    if (open == json.npos || json[open] != '"') return {};
    size_t close = json.find('"', open + 1);
    return close == json.npos ? std::string_view{} : json.substr(open + 1, close - open - 1);
}

// "status,clock,usage" in any order. Anything that is not exactly the three names once each leaves `out` as it was.
inline void SettingRestOrder(std::string_view json, std::array<Tokens::RestBlock, 3>& out) {
    auto text = SettingString(json, "restOrder");
    std::array<Tokens::RestBlock, 3> got{};
    int n = 0;
    unsigned seen = 0;
    while (!text.empty() && n < 3) {
        size_t comma = text.find(',');
        auto word = text.substr(0, comma);
        text = comma == text.npos ? std::string_view{} : text.substr(comma + 1);
        Tokens::RestBlock b;
        if (word == "status") b = Tokens::RestBlock::Status;
        else if (word == "clock") b = Tokens::RestBlock::Clock;
        else if (word == "usage") b = Tokens::RestBlock::Usage;
        else return;
        if (seen & (1u << int(b))) return;
        seen |= 1u << int(b);
        got[n++] = b;
    }
    if (n == 3 && text.empty()) out = got;
}

inline void SettingNumber(std::string_view json, std::string_view key, float& out, float lo, float hi) {
    std::string raw(SettingRaw(json, key));
    char* end = nullptr;
    float v = std::strtof(raw.c_str(), &end);
    if (!raw.empty() && end && *end == 0 && std::isfinite(v)) out = std::clamp(v, lo, hi);
}

inline void SettingInt(std::string_view json, std::string_view key, int& out, int lo, int hi) {
    float v = float(out);
    SettingNumber(json, key, v, float(lo), float(hi));
    out = int(std::lround(v));
}

inline void SettingBool(std::string_view json, std::string_view key, bool& out) {
    auto raw = SettingRaw(json, key);
    if (raw == "true") out = true;
    else if (raw == "false") out = false;
}

inline void SettingColor(std::string_view json, std::string_view key, Rgba& out) {  // "#RRGGBB", opaque
    auto raw = SettingRaw(json, key);
    if (raw.size() != 7 || raw[0] != '#') return;
    char* end = nullptr;
    std::string hex(raw.substr(1));
    unsigned long v = std::strtoul(hex.c_str(), &end, 16);
    if (end && *end == 0) out = Argb(0xFF000000u | uint32_t(v));
}

// Bounce and speed (0-100) map onto the spring; the defaults 40 and 60 give the designed stiffness 420, damping 34.
// Speed doubles the stiffness every 40 steps; bounce lowers the damping ratio from 1 (no overshoot) to 0.57.
inline void SpringFromFeel(float bounce, float speed, float& stiffness, float& damping) {
    stiffness = 420.f * std::exp2((speed - 60.f) / 40.f);
    float ratio = 1.f - bounce / 100.f * 0.4256f;
    damping = ratio * 2.f * std::sqrt(stiffness);
}

// The island's own small state (state.json beside settings.json). The island never edits settings.json, so it cannot
// race the settings app. Today: which time the right-hand label shows.
// Writes state.json whole, then renames it over; a failure removes the temp file and returns false (settings.json is never touched).
inline bool WriteState(const std::wstring& dir, bool timeTotal) {
    if (dir.empty()) return true;  // benchmarks run without a data folder
    std::wstring file = dir + L"\\state.json", tmp = file + L".tmp";
    bool ok = false;
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        ok = fputs(timeTotal ? "{\"timeTotal\":true}" : "{\"timeTotal\":false}", f) >= 0;
        ok = fclose(f) == 0 && ok;
    }
    ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        DWORD why = GetLastError();  // the caller logs this; the cleanup below must not replace it
        DeleteFileW(tmp.c_str());
        SetLastError(why);
    }
    return ok;
}

inline void LoadState(Tokens& t, std::string_view json) { SettingBool(json, "timeTotal", t.timeTotal); }

inline void LoadSettings(Tokens& t, std::string_view json) {
    SettingNumber(json, "topOffset", t.topOffset, 0, 48);
    SettingInt(json, "focusMinutes", t.focusMinutes, 1, 180);
    SettingNumber(json, "warnAt", t.warnAt, 50, 95);
    SettingNumber(json, "critAt", t.critAt, 55, 100);
    t.critAt = std::max(t.critAt, t.warnAt + 5);
    SettingBool(json, "openOnHighUsage", t.openOnHighUsage);
    SettingBool(json, "instantOpen", t.instantOpen);
    SettingBool(json, "showWeekday", t.showWeekday);
    SettingBool(json, "downloadIcon", t.downloadIcon);
    SettingBool(json, "btIcon", t.btIcon);
    SettingBool(json, "soundScreenshot", t.soundScreenshot);
    SettingBool(json, "soundDownloadDone", t.soundDownloadDone);
    SettingBool(json, "soundFocusDone", t.soundFocusDone);
    SettingRestOrder(json, t.restOrder);
    auto peek = SettingRaw(json, "afterPeek");
    if (peek == "tuck") t.afterPeek = Tokens::AfterPeek::Tuck;
    else if (peek == "clock") t.afterPeek = Tokens::AfterPeek::Clock;
    else if (peek == "music") t.afterPeek = Tokens::AfterPeek::Music;
    auto progress = SettingRaw(json, "progressStyle");
    if (progress == "wave") t.progressStyle = Tokens::ProgressStyle::Wave;
    else if (progress == "line") t.progressStyle = Tokens::ProgressStyle::Line;
    else if (progress == "segmented") t.progressStyle = Tokens::ProgressStyle::Segmented;
    else if (progress == "hairline") t.progressStyle = Tokens::ProgressStyle::Hairline;
    else if (progress == "gradient") t.progressStyle = Tokens::ProgressStyle::Gradient;
    auto knob = SettingRaw(json, "playheadKnob");
    if (knob == "never") t.knob = Tokens::Knob::Never;
    else if (knob == "hover") t.knob = Tokens::Knob::Hover;
    else if (knob == "always") t.knob = Tokens::Knob::Always;
    SettingBool(json, "artAccent", t.artAccent);
    SettingInt(json, "mediaPeekMs", t.mediaPeekMs, 1000, 15000);
    SettingInt(json, "mediaHideMs", t.mediaHideMs, 0, 600000);  // 0: never hide a paused player
    SettingColor(json, "glassTint", t.glassTint);
    t.fallback = t.glassTint;
    t.fallback.a = 0xF0;
    SettingNumber(json, "tintOpacity", t.tintOpacity, 0.2f, 1);
    SettingBool(json, "followAccent", t.followAccent);
    SettingColor(json, "accentColor", t.accentFallback);
    SettingNumber(json, "collapsedR", t.collapsedR, 4, t.collapsedH / 2);
    SettingNumber(json, "expandedR", t.expandedR, 8, t.statsH / 2);
    float scale = 1;
    SettingNumber(json, "textScale", scale, 0.85f, 1.2f);
    for (float* size : {&t.typeTimerBig, &t.typeTitle, &t.typeTimerMini, &t.typeBody, &t.typeClock, &t.typeLabel, &t.typeCaption})
        *size = std::round(*size * scale);
    float bounce = 40, speed = 60;
    SettingNumber(json, "bounce", bounce, 0, 100);
    SettingNumber(json, "speed", speed, 0, 100);
    SpringFromFeel(bounce, speed, t.springStiffness, t.springDamping);
    float pace = std::sqrt(Tokens{}.springStiffness / t.springStiffness);  // the content fades keep step with the spring
    t.contentOutMs *= pace;
    t.contentInMs *= pace;
}
