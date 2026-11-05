#pragma once
// Now playing, from Windows' media sessions (GlobalSystemMediaTransportControls: Spotify, Apple Music, browsers and any
// player that shows in the Windows media flyout). Session events only post a window message; the app then reads the
// session on its own thread. Nothing polls.
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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
