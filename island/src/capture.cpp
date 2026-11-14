#include "capture.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <winrt/base.h>

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir;
}

std::wstring ShotDir() {
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"STATLET_SCREENSHOT_DIR", env, MAX_PATH);  // tests point this at a temp folder
    return ResolveShotDir(n > 0 && n < MAX_PATH ? env : L"", KnownFolder(FOLDERID_Screenshots), KnownFolder(FOLDERID_Pictures));
}

// BitBlt of the monitor from the screen DC (physical pixels; the island is per-monitor DPI aware). The island leaves the picture
// for the moment of the capture (display affinity), and DwmFlush waits for the screen to be composed without it.
bool CaptureMonitorAt(HWND island, POINT at, Shot& out) {
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &mi)) return false;
    int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w <= 0 || h <= 0) return false;
    bool hidden = SetWindowDisplayAffinity(island, WDA_EXCLUDEFROMCAPTURE) != 0;
    if (hidden) DwmFlush();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};  // negative height: rows top-down
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib) {
        HGDIOBJ old = SelectObject(mem, dib);
        ok = BitBlt(mem, 0, 0, w, h, screen, mi.rcMonitor.left, mi.rcMonitor.top, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(mem, old);
        if (ok) {
            out.w = w, out.h = h;
            out.bgra.assign(static_cast<const std::uint8_t*>(bits), static_cast<const std::uint8_t*>(bits) + size_t(w) * h * 4);
            for (size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 0xFF;  // BitBlt leaves alpha 0; the PNG is written as BGRA
        }
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (hidden) SetWindowDisplayAffinity(island, WDA_NONE);
    return ok;
}

bool EncodePng(const Shot& shot, std::vector<std::uint8_t>& png) {
    try {
        auto wic = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
        winrt::check_hresult(frame->Initialize(nullptr));
        winrt::check_hresult(frame->SetSize(UINT(shot.w), UINT(shot.h)));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;  // a PNG encoder takes this one as is; alpha is 0xFF everywhere
        winrt::check_hresult(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat32bppBGRA) return false;  // the encoder wanted something else: do not write skewed rows
        winrt::check_hresult(frame->WritePixels(UINT(shot.h), UINT(shot.w) * 4, UINT(shot.bgra.size()), const_cast<BYTE*>(shot.bgra.data())));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
        STATSTG stat{};
        winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
        png.resize(size_t(stat.cbSize.QuadPart));
        LARGE_INTEGER zero{};
        winrt::check_hresult(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
        ULONG got = 0;
        winrt::check_hresult(stream->Read(png.data(), ULONG(png.size()), &got));
        return got == png.size();
    } catch (...) {
        return false;
    }
}

bool SaveShot(const Shot& shot, std::wstring& name) {
    std::wstring dir = ShotDir();
    std::vector<std::uint8_t> png;
    if (dir.empty() || !EncodePng(shot, png)) return false;
    int made = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) return false;
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::wstring base = ShotFileName(now), stem = base.substr(0, base.size() - 4);
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int n = 1; n <= 20 && f == INVALID_HANDLE_VALUE; ++n) {  // two saves in one second: "... (2).png"
        name = n == 1 ? base : stem + L" (" + std::to_wstring(n) + L").png";
        f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);  // CREATE_NEW: never overwrite
        if (f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, png.data(), DWORD(png.size()), &wrote, nullptr) && wrote == png.size();
    CloseHandle(f);
    if (!ok) DeleteFileW((dir + L"\\" + name).c_str());
    return ok;
}

static HGLOBAL GlobalCopy(const void* data, size_t bytes, size_t headBytes = 0, const void* head = nullptr) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, headBytes + bytes);
    if (!g) return nullptr;
    auto* p = static_cast<std::uint8_t*>(GlobalLock(g));
    if (!p) {
        GlobalFree(g);
        return nullptr;
    }
    if (head) memcpy(p, head, headBytes);
    memcpy(p + headBytes, data, bytes);
    GlobalUnlock(g);
    return g;
}

bool CopyShot(HWND owner, const Shot& shot) {
    std::vector<std::uint8_t> png;
    if (!EncodePng(shot, png)) return false;
    // CF_DIB is bottom-up: flip the rows.
    size_t stride = size_t(shot.w) * 4;
    std::vector<std::uint8_t> flipped(shot.bgra.size());
    for (int y = 0; y < shot.h; ++y) memcpy(&flipped[size_t(shot.h - 1 - y) * stride], &shot.bgra[size_t(y) * stride], stride);
    BITMAPINFOHEADER head{sizeof head, shot.w, shot.h, 1, 32, BI_RGB};
    head.biSizeImage = DWORD(flipped.size());
    HGLOBAL dib = GlobalCopy(flipped.data(), flipped.size(), sizeof head, &head);
    HGLOBAL pngMem = GlobalCopy(png.data(), png.size());
    bool ok = false;
    if (!dib) {  // out of memory (a very large screen): nothing to put on the clipboard, and no point retrying
        if (pngMem) GlobalFree(pngMem);
        return false;
    }
    for (int tries = 0; tries < 5 && !ok; ++tries) {  // another app may hold the clipboard for a moment
        if (OpenClipboard(owner)) {
            EmptyClipboard();
            ok = dib && SetClipboardData(CF_DIB, dib) != nullptr;
            if (ok) dib = nullptr;  // the clipboard owns it now
            if (pngMem && SetClipboardData(RegisterClipboardFormatW(L"PNG"), pngMem)) pngMem = nullptr;
            CloseClipboard();
        } else Sleep(20);
    }
    if (dib) GlobalFree(dib);
    if (pngMem) GlobalFree(pngMem);
    return ok;
}
