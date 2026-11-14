#pragma once
// Screenshot of the monitor under the cursor, to the clipboard or a PNG file (v2 item 8). Nothing runs until a menu item is
// clicked. The name and folder rules are header-only for the tests; the capture and encoding are in capture.cpp.
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

struct Shot {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;  // top-down, 4 bytes a pixel, alpha unused
};

// "Statlet 2026-11-13 06-30-12.png": local time, and no character Windows rejects in a file name (no colons).
inline std::wstring ShotFileName(const SYSTEMTIME& t) {
    wchar_t name[64];
    swprintf(name, 64, L"Statlet %04d-%02d-%02d %02d-%02d-%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return name;
}

// The folder a PNG goes to: the test override when set, else the user's Screenshots folder, else Pictures\Screenshots.
inline std::wstring ResolveShotDir(const std::wstring& override, const std::wstring& screenshots, const std::wstring& pictures) {
    if (!override.empty()) return override;
    if (!screenshots.empty()) return screenshots;
    return pictures.empty() ? std::wstring() : pictures + L"\\Screenshots";
}

std::wstring ShotDir();                                        // ResolveShotDir over the environment and the known folders
bool CaptureMonitorAt(HWND island, POINT at, Shot& out);       // the monitor under `at`, without the island in the picture
bool EncodePng(const Shot& shot, std::vector<std::uint8_t>& png);
bool SaveShot(const Shot& shot, std::wstring& name);           // a PNG in ShotDir(), created if missing; `name` is the file's name
bool CopyShot(HWND owner, const Shot& shot);                   // CF_DIB plus the registered "PNG" format
