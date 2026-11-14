#pragma once
// Now playing, from Windows' media sessions (GlobalSystemMediaTransportControls: Spotify, Apple Music, browsers and any
// player that shows in the Windows media flyout). Session events only post a window message; the app then reads the
// session on its own thread. Nothing polls.
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <string>
#include <winrt/base.h>
#include "tokens.h"

struct MediaInfo {
    bool present = false, playing = false;
    std::wstring title, artist;
    double position = 0, duration = 0;  // seconds; position as of the read
};

// Posts `msg` when the current session's track, playback state or timeline changes (wParam 0: call ReadMedia), or when
// Windows picks a different current session (wParam 1: call RebindMedia, which then posts wParam 0).
void WatchMedia(HWND hwnd, UINT msg);
void RebindMedia();
void ReadMedia(std::function<void(MediaInfo)> done);                       // async; `done` runs on the calling (window) thread
void ReadArt(std::function<void(winrt::com_ptr<IWICBitmapSource>)> done);  // the last read track's art, or null
enum class MediaCommand { Previous, PlayPause, Next };
void SendMedia(MediaCommand c);
bool ArtColor(IWICBitmapSource* art, Rgba& out);  // dominant colour of the cover, scaled to at most 64x64; false when it has none

// --- Rules, header-only so the tests can drive them ---

// m:ss, or h:mm:ss from an hour.
inline std::wstring FormatTime(double seconds) {
    int s = std::max(0, int(seconds)), h = s / 3600, m = s / 60 % 60;
    wchar_t text[16];
    if (h) swprintf(text, 16, L"%d:%02d:%02d", h, m, s % 60);
    else swprintf(text, 16, L"%d:%02d", m, s % 60);
    return text;
}

// Which rest line the collapsed island shows.
enum class MediaRow { Rest, Tucked, Peek, Music };
inline MediaRow RowFor(bool shown, bool peeking, Tokens::AfterPeek mode) {
    if (!shown) return MediaRow::Rest;
    if (peeking) return MediaRow::Peek;
    return mode == Tokens::AfterPeek::Tuck ? MediaRow::Tucked : mode == Tokens::AfterPeek::Music ? MediaRow::Music : MediaRow::Rest;
}

// A peek follows a track that has not peeked yet (players may report a new track paused for a moment before it plays),
// or playback starting while the music was hidden; never a resume of the song already showing.
inline bool ShouldPeek(bool unpeeked, bool playing, bool wasShown) { return playing && (unpeeked || !wasShown); }

// Dominant colour of premultiplied BGRA pixels: 4-bit-per-channel buckets, each pixel weighted by its chroma so a vivid
// sliver beats a large grey field; the answer is the mean colour of the winning bucket. False when nothing is opaque.
inline bool DominantColor(const uint8_t* bgra, size_t n, Rgba& out) {
    struct Bucket {
        double w = 0;
        uint64_t r = 0, g = 0, b = 0, n = 0;
    };
    std::unordered_map<uint32_t, Bucket> buckets;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = bgra + i * 4;
        if (p[3] < 128) continue;
        int r = std::min(p[2] * 255 / p[3], 255), g = std::min(p[1] * 255 / p[3], 255), b = std::min(p[0] * 255 / p[3], 255);  // un-premultiply
        int chroma = std::max({r, g, b}) - std::min({r, g, b});
        Bucket& k = buckets[uint32_t(r >> 4) << 8 | uint32_t(g >> 4) << 4 | uint32_t(b >> 4)];
        k.w += 1 + chroma * chroma / 256.0;
        k.r += r, k.g += g, k.b += b, k.n++;
    }
    const Bucket* best = nullptr;
    for (auto& entry : buckets)
        if (!best || entry.second.w > best->w) best = &entry.second;
    if (!best) return false;
    out = {255, uint8_t(best->r / best->n), uint8_t(best->g / best->n), uint8_t(best->b / best->n)};
    return true;
}

// The art colour sits on dark glass: lift it toward white until its luma reaches minLuma (0..255); untouched when brighter.
inline Rgba LiftForGlass(Rgba c, int minLuma = 140) {
    double luma = (299 * c.r + 587 * c.g + 114 * c.b) / 1000.0;
    if (luma >= minLuma) return c;
    double k = (minLuma - luma) / (255 - luma);
    auto up = [k](uint8_t v) { return uint8_t(std::lround(v + (255 - v) * k)); };
    return {255, up(c.r), up(c.g), up(c.b)};
}

// The Gradient bar ramps from this opaque, darker shade of the media colour up to the colour itself. (A see-through
// start looked dim and teal over the glass at low progress.)
inline Rgba GradientStart(Rgba c) { return {255, uint8_t(c.r * 6 / 10), uint8_t(c.g * 6 / 10), uint8_t(c.b * 6 / 10)}; }

// The right-hand time of the progress row: the time left (with a minus) or the track length.
inline std::wstring RightTime(double duration, double position, bool total) {
    return total ? FormatTime(duration) : L"-" + FormatTime(duration - position);
}
