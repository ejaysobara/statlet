// Tests for the island's math and its five drawing pieces. Run out\statlet-tests.exe from the island folder;
// exit code 0 means everything passed.
//
// - Unit tests: spring timing, hit testing, corner radius, accent contrast.
// - Golden images (Text, Icon): drawn offscreen with the same Paint code the island uses, compared with
//   tests\golden\<name>@<dpi>.png. A missing golden is written from the current output; look at it, then keep it.
// - Screenshot checks (RoundedShape, Acrylic, Bar, Text on screen): a test island over a solid magenta window,
//   captured from the screen, with pixel assertions and a golden. Also checks device-loss recovery.
// - `--e2e` also launches out\statlet.exe and moves the cursor to test click-through (it takes over the mouse briefly).
#include <windows.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include "../src/gfx.h"
#include "../src/shape_math.h"
#include "../src/stats.h"
#include "../src/takeover.h"
#include "../src/media.h"
#include "../src/placement.h"
#include "../src/power.h"
#include "../src/settings.h"
#include "../src/bluetooth.h"
#include "../src/capture.h"
#include "../src/clock.h"
#include "../src/downloads.h"
#include "../src/widgets.h"
#include <cstdarg>
#include <string>
// doctest's thread_local stream is freed after the heap at CRT exit (0xC0000374, ~7 in 100 runs); tests are single-threaded.
#define DOCTEST_CONFIG_NO_MULTITHREADING
#define DOCTEST_CONFIG_IMPLEMENT
#pragma warning(push, 0)
#include "vendor/doctest.h"
#pragma warning(pop)

using winrt::check_hresult;
using winrt::com_ptr;

static float scale = 1;

static std::string Fmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    return buf;
}
// doctest's CHECK, keeping the printf-style message the tests were written with.
#undef CHECK
#define CHECK(cond, ...) DOCTEST_CHECK_MESSAGE(bool(cond), Fmt(__VA_ARGS__))

struct Image {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;  // 0xAARRGGBB
    uint32_t At(UINT x, UINT y) const { return px[y * w + x]; }
};

static int Channel(uint32_t c, int shift) { return int((c >> shift) & 0xFF); }
static int R(uint32_t c) { return Channel(c, 16); }
static int G(uint32_t c) { return Channel(c, 8); }
static int B(uint32_t c) { return Channel(c, 0); }
static int Diff(uint32_t a, uint32_t b) { return std::max({std::abs(R(a) - R(b)), std::abs(G(a) - G(b)), std::abs(B(a) - B(b))}); }

static com_ptr<IWICImagingFactory> Wic() {
    static com_ptr<IWICImagingFactory> wic;
    if (!wic) check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.put_void()));
    return wic;
}

static void SavePng(const std::wstring& path, const Image& img) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    com_ptr<IWICStream> stream;
    com_ptr<IWICBitmapEncoder> encoder;
    com_ptr<IWICBitmapFrameEncode> frame;
    check_hresult(Wic()->CreateStream(stream.put()));
    check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    check_hresult(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize(img.w, img.h));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    check_hresult(frame->WritePixels(img.h, img.w * 4, img.w * img.h * 4, (BYTE*)img.px.data()));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());
}

static bool LoadPng(const std::wstring& path, Image& img) {
    com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put()))) return false;
    com_ptr<IWICBitmapFrameDecode> frame;
    com_ptr<IWICFormatConverter> converter;
    check_hresult(decoder->GetFrame(0, frame.put()));
    check_hresult(Wic()->CreateFormatConverter(converter.put()));
    check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    check_hresult(converter->GetSize(&img.w, &img.h));
    img.px.resize(img.w * img.h);
    check_hresult(converter->CopyPixels(nullptr, img.w * 4, img.w * img.h * 4, (BYTE*)img.px.data()));
    return true;
}

// Compares with tests\golden\<name>@<dpi>.png: at most `share` of pixels may differ by more than `tolerance` per channel.
static void Golden(const char* name, const Image& actual, int tolerance, double share) {
    wchar_t file[128];
    swprintf(file, 128, L"%hs@%d.png", name, int(std::lround(scale * 96)));
    std::wstring golden = std::wstring(L"tests\\golden\\") + file, out = std::wstring(L"tests\\out\\") + file;
    SavePng(out, actual);
    Image expected;
    if (!LoadPng(golden, expected)) {  // a missing golden fails; STATLET_NEW_GOLDEN=1 writes it from the current output
        bool write = getenv("STATLET_NEW_GOLDEN") != nullptr;
        if (write) SavePng(golden, actual);
        CHECK(false, "%s: golden %ls is missing%s", name, golden.c_str(), write ? " (written; review it)" : " (rerun with STATLET_NEW_GOLDEN=1, then review the image)");
        return;
    }
    CHECK(expected.w == actual.w && expected.h == actual.h, "%s: golden is %ux%u, got %ux%u", name, expected.w, expected.h, actual.w, actual.h);
    if (expected.w != actual.w || expected.h != actual.h) return;
    size_t off = 0;
    for (size_t i = 0; i < actual.px.size(); i++) off += Diff(actual.px[i], expected.px[i]) > tolerance;
    CHECK(off <= share * actual.px.size(), "%s: %zu of %zu pixels differ from the golden (see %ls)", name, off, actual.px.size(), out.c_str());
}

// --- Unit tests ---

TEST_SUITE_BEGIN("unit");

TEST_CASE("math") {
    Tokens t;
    CHECK(std::fabs(SpringPeriod(t.springStiffness) - 0.0488) < 0.001,"period %f", SpringPeriod(t.springStiffness));
    CHECK(std::fabs(SpringDampingRatio(t.springStiffness, t.springDamping) - 0.83) < 0.01, "ratio %f", SpringDampingRatio(t.springStiffness, t.springDamping));

    // Spec: settles to within 2% in about 0.24 s with a barely visible overshoot.
    Spring s(t.collapsedH, t.springStiffness, t.springDamping);
    s.target = t.expandedH;
    double travel = t.expandedH - t.collapsedH, peak = 0, settle = -1;
    for (int i = 1; i <= 1000; i++) {
        s.Step(0.001);
        peak = std::max(peak, s.value);
        if (std::fabs(s.target - s.value) > 0.02 * travel) settle = -1;
        else if (settle < 0) settle = i / 1000.0;
    }
    CHECK(settle > 0.18 && settle < 0.32, "settle %f s", settle);
    CHECK(peak - t.expandedH < 0.02 * travel, "overshoot %f", peak - t.expandedH);
    CHECK(s.value == s.target && s.velocity == 0, "not at rest after 1 s");

    // One long step (the cursor poll after idle) lands at rest instead of exploding.
    Spring jump(0, t.springStiffness, t.springDamping);
    jump.target = 100;
    jump.Step(5);
    CHECK(jump.value == 100, "long step %f", jump.value);

    CHECK(InRoundedRect(50, 17, 0, 0, 100, 34, 11), "center");
    CHECK(!InRoundedRect(1, 1, 0, 0, 100, 34, 11), "corner outside the arc");
    CHECK(InRoundedRect(4, 4, 0, 0, 100, 34, 11), "corner inside the arc");
    CHECK(InRoundedRect(0, 17, 0, 0, 100, 34, 11) && !InRoundedRect(100, 17, 0, 0, 100, 34, 11), "edges are half-open");
    CHECK(!InRoundedRect(50, -1, 0, 0, 100, 34, 11), "above");

    CHECK(IslandRadius(t, t.collapsedH) == t.collapsedR && IslandRadius(t, t.expandedH) == t.expandedR, "radius ends");
    CHECK(IslandRadius(t, 500) == t.expandedR, "radius clamps on overshoot");

    CHECK(FitCollapsedWidth(121, t) == 148 && FitCollapsedWidth(123.5f, t) == 148, "content plus padding, rounded up to 4");
    CHECK(FitCollapsedWidth(10, t) == t.collapsedMinW && FitCollapsedWidth(900, t) == t.collapsedMaxW, "width clamps to the spec range");

    CHECK(std::fabs(Contrast(Argb(0xFFFFFFFF), Argb(0xFF000000)) - 21) < 0.01, "white on black");
    Rgba dark = Argb(0xFF203040), light = Argb(0xFF99CCFF);
    CHECK(PickAccent(dark, light, t.glassTint).r == light.r, "dark accent steps lighter");
    CHECK(PickAccent(light, dark, t.glassTint).r == light.r, "readable accent kept");
    CHECK(Contrast(t.ink2, t.glassTint) > 7, "Ink2 contrast %f", Contrast(t.ink2, t.glassTint));
}

TEST_CASE("away") {  // AwayFor in shape_math.h, with windows seen in island.log on Ejay's machine
    auto make = [](bool covers, bool zoomed, bool caption) {
        FrontWindow f;
        f.usable = f.onPrimary = true;
        f.covers = covers, f.zoomed = zoomed, f.caption = caption;
        return f;
    };
    CHECK(AwayFor(make(true, true, true)) == Away::Slid, "maximized Chrome (rect -9,-9 to 2569,1449, caption) slides");
    CHECK(AwayFor(make(false, false, true)) == Away::No, "a normal window leaves the island alone");
    CHECK(AwayFor(make(true, false, false)) == Away::Hidden, "a borderless fullscreen game hides it");
    CHECK(AwayFor(make(true, true, false)) == Away::Hidden, "maximized without a title bar hides it");
    CHECK(AwayFor(make(true, false, true)) == Away::Hidden, "Snipping Tool's overlay (0,0 to 2560,1440, not zoomed) hides it");
    FrontWindow f = make(true, false, false);
    f.shell = true;
    CHECK(AwayFor(f) == Away::No, "the desktop never hides it");
    f = make(true, false, false);
    f.onPrimary = false;
    CHECK(AwayFor(f) == Away::No, "fullscreen on another monitor leaves it");
    f.usable = false;
    CHECK(AwayFor(f) == Away::No, "no foreground window leaves it");
}

TEST_CASE("front window on the primary") {  // FrontOnPrimary in shape_math.h over fake z-orders, front first
    auto win = [](bool onPrimary, bool covers, bool zoomed, bool caption, bool topmost = false, bool shell = false) {
        ZWindow w;
        w.f.usable = true, w.f.onPrimary = onPrimary, w.f.covers = covers, w.f.zoomed = zoomed, w.f.caption = caption, w.f.shell = shell;
        w.topmost = topmost;
        return w;
    };
    auto judge = [](std::vector<ZWindow> z) { const ZWindow* f = FrontOnPrimary(z); return f ? AwayFor(f->f) : Away::No; };
    ZWindow focusedOnLaptop = win(false, false, false, true), fullscreen = win(true, true, false, false), normal = win(true, false, false, true);
    ZWindow maximized = win(true, true, true, true), desktop = win(true, true, false, false, false, true);
    CHECK(judge({focusedOnLaptop, fullscreen}) == Away::Hidden, "focus on the laptop panel, fullscreen video on the primary: hidden");
    CHECK(judge({focusedOnLaptop, maximized}) == Away::Slid, "focus on the laptop panel, maximized window on the primary: slid");
    CHECK(judge({normal, maximized}) == Away::No, "a normal window in front on the primary: no");
    CHECK(judge({win(true, false, false, false, true), maximized}) == Away::Slid, "a small always-on-top window (Alt+Tab, picture-in-picture) is looked past");
    CHECK(judge({win(true, true, false, false, true), normal}) == Away::Hidden, "an always-on-top window covering the primary counts");
    CHECK(judge({win(false, true, false, false), desktop}) == Away::No, "fullscreen on the laptop panel only, desktop on the primary: no");
    CHECK(judge({}) == Away::No, "nothing on the primary: no");
}

TEST_CASE("away gate") {  // AwayGate in shape_math.h, replaying island.log 18:22:40 (Alt+Tab over a maximized window)
    AwayGate g;
    int changes = 0;
    g.shown = Away::Slid;
    changes += g.See(Away::No);    // .127 the taskbar is foreground
    changes += g.See(Away::No);    // an empty window: still waiting, the wait does not restart
    changes += g.See(Away::Slid);  // .205 the maximized window again
    CHECK(changes == 0 && g.shown == Away::Slid && !g.returning, "Alt+Tab through the taskbar: no animation (%d changes)", changes);
    CHECK(!g.Settle(Away::Slid), "a stale timer after the return was dropped does nothing");
    CHECK(!g.See(Away::No) && g.returning && g.Settle(Away::No) && g.shown == Away::No, "a real return applies after the wait");
    CHECK(g.See(Away::Hidden) && g.shown == Away::Hidden, "going away is instant");
    CHECK(g.See(Away::Slid) && g.shown == Away::Slid, "hidden to slid is instant too");
    CHECK(!g.See(Away::No) && g.Settle(Away::Hidden) && g.shown == Away::Hidden, "back to No, then a fullscreen window by the time the wait ends: hidden");
}

TEST_CASE("stats") {  // levels, sustain rule and samplers from stats.h
    Tokens t;
    CHECK(LevelFor(79.9f, t) == Level::Normal && LevelFor(80, t) == Level::Warn, "amber starts at 80");
    CHECK(LevelFor(94.9f, t) == Level::Warn && LevelFor(95, t) == Level::Critical, "red starts at 95");
    CHECK(LevelColor(Level::Normal, t.ink, t).r == t.ink.r && LevelColor(Level::Warn, t.ink, t).r == t.warn.r &&
              LevelColor(Level::Critical, t.ink, t).r == t.critical.r, "level colors");

    // Opens after 30 s strictly above 90; exactly 90 and any dip restart the clock.
    Sustain s(t.sustainAbove, t.sustainSeconds);
    CHECK(!s.Update(91, 0) && !s.Update(91, 29.9), "not yet at 29.9 s");
    CHECK(s.Update(91, 30) && s.Update(99, 45), "open at 30 s and while still high");
    CHECK(!s.Update(90, 50), "90 is not above 90");
    CHECK(!s.Update(91, 51) && !s.Update(91, 80.9) && s.Update(91, 81), "the clock restarts after a dip");
    CHECK(!s.Update(50, 90) && !s.Update(95, 91) && !s.Update(95, 120), "a dip resets, 29 s later is still closed");

    CpuSampler cpu;
    CHECK(cpu.Percent() == 0, "first sample is 0");
    Sleep(100);
    float c = cpu.Percent(), m = MemoryPercent();
    CHECK(c >= 0 && c <= 100, "cpu %f", c);
    CHECK(m > 0 && m <= 100, "ram %f", m);
}

TEST_CASE("takeover") {  // rules and card contents from takeover.h
    Tokens t;
    Rgba accent = t.accentFallback;

    BatteryWatch b(t.batteryLowAt, t.batteryCritAt);
    CHECK(b.Update(80, false) == BatteryEvent::None && b.Update(21, false) == BatteryEvent::None, "no warning above 20");
    CHECK(b.Update(20, false) == BatteryEvent::Low && b.Update(19, false) == BatteryEvent::None, "low fires once at 20");
    CHECK(b.Update(10, false) == BatteryEvent::Critical && b.Update(9, false) == BatteryEvent::None, "critical fires once at 10");
    CHECK(b.Update(9, true) == BatteryEvent::Charging && b.Update(10, true) == BatteryEvent::None, "charging fires once on plug-in");
    CHECK(b.Update(15, false) == BatteryEvent::Unplugged && b.Update(15, false) == BatteryEvent::Low, "unplugging says so, then re-arms the warnings");
    BatteryWatch first(20, 10);
    CHECK(first.Update(60, true) == BatteryEvent::None, "already charging at start says nothing");
    BatteryWatch jump(20, 10);
    CHECK(jump.Update(8, false) == BatteryEvent::Critical, "starting below critical goes straight to critical");

    NetWatch n;
    CHECK(n.Settle(true) == NetEvent::None, "online at start says nothing");
    CHECK(n.Settle(false) == NetEvent::Lost && n.Settle(false) == NetEvent::None, "lost once");
    CHECK(n.Settle(true) == NetEvent::Back && n.Settle(true) == NetEvent::None, "back once, only after lost");

    FocusTimer f;
    CHECK(!f.Running() && f.MinutesLeft(0) == 0, "idle");
    f.Start(100, 25 * 60);
    CHECK(f.Running() && f.MinutesLeft(100) == 25 && f.MinutesLeft(100 + 60) == 24, "counts down in whole minutes");
    CHECK(f.MinutesLeft(100 + 24 * 60 + 1) == 1 && f.MinutesLeft(100 + 25 * 60 + 5) == 0, "rounds up, never below 0");
    f.Stop();
    CHECK(!f.Running(), "stopped");

    auto v = VolumeCard(42, false, accent, t, L"Speakers");
    CHECK(v.value == L"42%" && v.body == L"Speakers" && v.progress == 0.42f && v.holdMs == t.takeoverVolumeMs, "volume card");
    auto m = VolumeCard(42, true, accent, t);
    CHECK(m.value == L"Muted" && m.progress == 0 && m.glyph != v.glyph, "muted volume card");
    auto low = BatteryCard(BatteryEvent::Low, 18, accent, t, 40 * 60), crit = BatteryCard(BatteryEvent::Critical, 9, accent, t);
    CHECK(low.title == L"Battery low" && low.value == L"18%" && low.body == L"About 40 min left" && low.color.r == t.warn.r, "low card is amber");
    CHECK(crit.body == L"Estimating time left" && TimeLeft(80 * 60) == L"About 1 h 20 min left" && TimeLeft(20) == L"About 1 min left", "time left");
    CHECK(crit.title == L"Battery critical" && crit.color.r == t.critical.r && crit.color.g == t.critical.g, "critical card is red");
    CHECK(BatteryCard(BatteryEvent::Charging, 63, accent, t).title == L"Charging", "charging card");
    CHECK(NetCard(NetEvent::Lost, accent, t).title == L"Offline" && NetCard(NetEvent::Back, accent, t).title == L"Back online", "connection cards");
    CHECK(NetCard(NetEvent::Lost, accent, t).progress < 0 && FocusCard(false, 25, accent, t).progress < 0, "cards without a bar");
    CHECK(FocusCard(true, 25, accent, t).title == L"Focus complete" && FocusCard(false, 25, accent, t).title == L"Focus started", "focus cards");
}

TEST_CASE("media") {  // rules from media.h
    using AP = Tokens::AfterPeek;
    CHECK(FormatTime(84) == L"1:24" && FormatTime(5) == L"0:05" && FormatTime(3725) == L"1:02:05" && FormatTime(-3) == L"0:00", "times");
    CHECK(RowFor(false, true, AP::Tuck) == MediaRow::Rest, "no music: the rest line");
    CHECK(RowFor(true, true, AP::Clock) == MediaRow::Peek, "a peek shows whatever the setting");
    CHECK(RowFor(true, false, AP::Tuck) == MediaRow::Tucked && RowFor(true, false, AP::Clock) == MediaRow::Rest &&
              RowFor(true, false, AP::Music) == MediaRow::Music, "after the peek: the setting");
    CHECK(ShouldPeek(true, true, true) && ShouldPeek(false, true, false), "new track, or playing again after hiding");
    CHECK(!ShouldPeek(false, true, true) && !ShouldPeek(true, false, false), "not a resume of the same song, not while paused");
    Rgba c{};
    auto fill = [](std::vector<uint8_t>& v, size_t from, size_t to, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        for (size_t i = from; i < to; i++) v[i * 4] = uint8_t(b * a / 255), v[i * 4 + 1] = uint8_t(g * a / 255), v[i * 4 + 2] = uint8_t(r * a / 255), v[i * 4 + 3] = a;
    };
    std::vector<uint8_t> px(100 * 4);
    fill(px, 0, 100, 200, 30, 40);
    CHECK(DominantColor(px.data(), 100, c) && c.r == 200 && c.g == 30 && c.b == 40, "dominant: a solid cover");
    fill(px, 0, 60, 20, 90, 200), fill(px, 60, 100, 230, 200, 10);
    CHECK(DominantColor(px.data(), 100, c) && c.r == 20 && c.b == 200, "dominant: the larger of two colours");
    fill(px, 0, 90, 128, 128, 128), fill(px, 90, 100, 230, 20, 60);
    CHECK(DominantColor(px.data(), 100, c) && c.r == 230 && c.g == 20, "dominant: a vivid sliver beats a grey field");
    fill(px, 0, 100, 128, 128, 128);
    CHECK(DominantColor(px.data(), 100, c) && c.r == 128 && c.g == 128, "dominant: all grey is grey");
    fill(px, 0, 100, 255, 0, 0, 0);
    CHECK(!DominantColor(px.data(), 100, c) && !DominantColor(px.data(), 0, c), "dominant: transparent or empty has none");
    std::vector<uint8_t> art(64 * 64 * 4);  // the largest size ArtColor hands to DominantColor
    for (size_t i = 0; i < art.size(); i++) art[i] = uint8_t(i * 37 + i / 64);
    for (size_t i = 3; i < art.size(); i += 4) art[i] = 255;
    auto t0 = std::chrono::steady_clock::now();
    DominantColor(art.data(), 64 * 64, c);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 5, "colour pick of a 64x64 cover takes %.2f ms", ms);
    Rgba g0 = GradientStart({255, 100, 200, 250});
    CHECK(g0.a == 255 && g0.r == 60 && g0.g == 120 && g0.b == 150, "the gradient starts from an opaque 60%% shade, not a see-through colour");
    Rgba dark = LiftForGlass({255, 10, 10, 30}), bright = LiftForGlass({255, 250, 240, 200});
    CHECK(std::abs((299 * dark.r + 587 * dark.g + 114 * dark.b) / 1000 - 140) <= 1 && dark.b > 30, "a dark colour lifts to luma 140");
    CHECK(bright.r == 250 && bright.g == 240 && bright.b == 200, "a bright colour is untouched");
    CHECK(RightTime(200, 50, false) == L"-2:30" && RightTime(200, 50, true) == L"3:20", "right-hand time: left or total");
    Tokens t;
    CHECK(FitCollapsedWidth(130, t, t.mediaPadX) == 144 && FitCollapsedWidth(130, t) == 156, "media rows use the smaller pad");
}

TEST_CASE("settings") {  // the shared settings file, settings.h
    Tokens d, t;
    LoadSettings(t, "");
    CHECK(t.topOffset == d.topOffset && t.afterPeek == d.afterPeek && t.typeTitle == d.typeTitle, "empty file keeps defaults");
    CHECK(std::fabs(t.springStiffness - 420) < 0.5f && std::fabs(t.springDamping - 34) < 0.1f, "default feel is the designed spring: %.2f %.2f", t.springStiffness, t.springDamping);
    LoadSettings(t, R"({"topOffset": 20, "afterPeek":"music", "progressStyle":"line", "followAccent":false, "accentColor":"#FF7AB6",
                       "glassTint":"#2A1F33", "openOnHighUsage":false, "mediaHideMs":0, "textScale":1.15, "warnAt":90, "critAt":60})");
    CHECK(t.topOffset == 20 && t.afterPeek == Tokens::AfterPeek::Music && t.progressStyle == Tokens::ProgressStyle::Line, "values apply");
    CHECK(!t.followAccent && t.accentFallback.r == 0xFF && t.accentFallback.g == 0x7A && t.glassTint.r == 0x2A && t.fallback.a == 0xF0 && t.fallback.b == 0x33, "colors");
    CHECK(!t.openOnHighUsage && t.mediaHideMs == 0 && t.typeTitle == 18, "flags and text scale");
    CHECK(t.critAt == 95, "critical stays above warning: %.0f", t.critAt);
    Tokens u;
    LoadSettings(u, R"({"topOffset":"oops","collapsedR":99,"expandedR":-5,"glassTint":"#12","speed":100,"bounce":0})");
    CHECK(u.topOffset == d.topOffset && u.collapsedR == u.collapsedH / 2 && u.expandedR == 8 && u.glassTint.r == d.glassTint.r, "bad values clamp or keep defaults");
    CHECK(std::fabs(u.springDamping - 2 * std::sqrt(u.springStiffness)) < 0.01f && u.springStiffness > 800, "no bounce is critically damped, fast is stiffer");
    CHECK(!u.instantOpen, "animated by default");
    LoadSettings(u, R"({"instantOpen":true})");
    CHECK(u.instantOpen, "instant step reads");
    CHECK(u.contentInMs < d.contentInMs * 0.75f && u.contentOutMs < d.contentOutMs * 0.75f,"fast fades the content faster");
}

// --- Offscreen golden images for Text and Icon ---

static Image RenderOffscreen(Gfx& gfx, float wEpx, float hEpx, Rgba background, const std::function<void(ID2D1DeviceContext*)>& paint) {
    com_ptr<ID2D1DeviceContext> ctx;
    check_hresult(gfx.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, ctx.put()));
    Image img;
    img.w = UINT(std::ceil(wEpx * gfx.scale)), img.h = UINT(std::ceil(hEpx * gfx.scale));
    D2D1_BITMAP_PROPERTIES1 props{{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96 * gfx.scale, 96 * gfx.scale, D2D1_BITMAP_OPTIONS_TARGET};
    com_ptr<ID2D1Bitmap1> target, cpu;
    check_hresult(ctx->CreateBitmap({img.w, img.h}, nullptr, 0, props, target.put()));
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    check_hresult(ctx->CreateBitmap({img.w, img.h}, nullptr, 0, props, cpu.put()));
    ctx->SetTarget(target.get());
    ctx->SetDpi(96 * gfx.scale, 96 * gfx.scale);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ctx->BeginDraw();
    ctx->Clear({background.r / 255.f, background.g / 255.f, background.b / 255.f, 1});
    paint(ctx.get());
    check_hresult(ctx->EndDraw());
    check_hresult(cpu->CopyFromBitmap(nullptr, target.get(), nullptr));
    D2D1_MAPPED_RECT mapped;
    check_hresult(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped));
    img.px.resize(img.w * img.h);
    for (UINT y = 0; y < img.h; y++) memcpy(&img.px[y * img.w], mapped.bits + y * mapped.pitch, img.w * 4);
    cpu->Unmap();
    return img;
}

TEST_CASE("placement") {  // PlacementFor in placement.h: centered at the top of the work area, sized for the DPI
    Placement p = PlacementFor(0, 0, 2560, 96, 400, 200);
    CHECK(p.scale == 1 && p.w == 400 && p.h == 200, "100%%: %dx%d at scale %f", p.w, p.h, p.scale);
    CHECK(p.x == 1080 && p.y == 0, "centered at the top: %d,%d", p.x, p.y);
    p = PlacementFor(0, 40, 2560, 144, 400, 200);  // 150%, work area starts below a top taskbar
    CHECK(p.scale == 1.5f && p.w == 600 && p.h == 300 && p.y == 40, "150%%: %dx%d at y %d", p.w, p.h, p.y);
    p = PlacementFor(-1920, 0, 0, 96, 400, 200);  // primary monitor placed left of the origin: negative coordinates
    CHECK(p.x == -1160, "negative work area: x %d", p.x);
    p = PlacementFor(0, 0, 1920, 120, 333, 101);  // 125%: fractional sizes round up so the shape is never clipped
    CHECK(p.w == 417 && p.h == 127, "rounds up: %dx%d", p.w, p.h);
}

TEST_CASE("placement over fake topologies") {  // PlaceOn in placement.h; work areas in physical px
    const float w = 456, h = 536;  // the host window at Tokens' defaults, epx
    auto inside = [](const Placement& p, const Monitor& m) { return p.x >= m.left && p.y >= m.top && p.x + p.w <= m.right; };
    Placement p, last;
    Monitor external{0, 0, 2560, 1392, 120, true}, laptop{2560, 0, 4480, 1032, 144, false};
    CHECK(PlaceOn({laptop, external}, w, h, p) && p.scale == 1.25f && inside(p, external), "external primary at 120 dpi beside the laptop at 144: on the external at 125%%");
    Monitor alone = laptop;
    alone.primary = true, alone.left = 0, alone.right = 1920;
    CHECK(PlaceOn({alone}, w, h, p) && p.scale == 1.5f && inside(p, alone), "laptop only: on the laptop at 150%%");
    Monitor left{-2560, 0, 0, 1392, 120, false};
    CHECK(PlaceOn({left, alone}, w, h, p) && inside(p, alone), "primary swapped to the laptop, external left of it: on the laptop");
    Monitor top = external;
    top.top = 48;
    CHECK(PlaceOn({top}, w, h, p) && p.y == 48, "taskbar on top: just under it, y %d", p.y);
    for (unsigned dpi : {96u, 120u, 144u, 192u}) {
        Monitor m = external;
        m.dpi = dpi;
        CHECK(PlaceOn({m}, w, h, p) && inside(p, m) && p.scale == dpi / 96.f, "%u dpi: x %d w %d inside the work area", dpi, p.x, p.w);
    }
    last = p;
    p = last;
    CHECK(!PlaceOn({}, w, h, p) && p.x == last.x && p.scale == last.scale, "no monitor (mid-unplug): keep the last placement");
    Monitor failed = external;
    failed.dpi = 0;
    CHECK(!PlaceOn({failed, laptop}, w, h, p), "the primary's DPI query failed: keep the last placement, do not guess 96");
    failed = external;
    failed.right = failed.left;
    CHECK(!PlaceOn({failed}, w, h, p), "a zero-size work area (failed GetMonitorInfo): keep the last placement");
    Monitor orphan = left;
    CHECK(PlaceOn({orphan}, w, h, p) && inside(p, orphan) && p.x < 0, "no primary flagged, one monitor left of the origin: on it, x %d", p.x);
    Monitor tiny{0, 0, 400, 300, 96, true};
    CHECK(PlaceOn({tiny}, w, h, p) && p.x == 0, "work area narrower than the window: pinned to its left edge, never off it (x %d)", p.x);
}

TEST_CASE("power and session reasons") {  // PowerReason and SessionReason in power.h
    PowerState s;
    CHECK(PowerReason(PBT_APMRESUMEAUTOMATIC, PowerSetting::Other, 0, s) && !PowerReason(PBT_APMSUSPEND, PowerSetting::Other, 0, s), "resume revalidates, suspend does not");
    CHECK(!PowerReason(PBT_APMRESUMESUSPEND, PowerSetting::Other, 0, s), "resume-suspend always follows resume-automatic: one revalidate");
    CHECK(!PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Display, 1, s), "the first reading after registering only seeds");
    CHECK(!PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Display, 2, s), "on to dimmed: nothing");
    CHECK(!PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Display, 0, s), "off: nothing");
    const char* why = PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Display, 1, s);
    CHECK(why && !strcmp(why, "display on"), "off to on revalidates");
    CHECK(!PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Display, 1, s), "on to on: nothing");
    CHECK(!PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Lid, 0, s) && !PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Lid, 0, s), "lid closed: nothing");
    why = PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Lid, 1, s);
    CHECK(why && !strcmp(why, "lid open"), "lid opened revalidates");
    CHECK(!PowerReason(PBT_POWERSETTINGCHANGE, PowerSetting::Other, 0, s), "battery, AC and saver settings: nothing");
    WidgetState seen{false, false, false, false}, slid{true, false, false, false}, hidden{true, true, false, false};
    CHECK(WantHover(seen) && WantRest(seen), "on screen: every timer runs");
    CHECK(!WantHover(slid) && WantRest(slid), "slid away: no cursor poll, the clock keeps the accessible name current");
    CHECK(!WantHover(hidden) && !WantRest(hidden), "hidden: nothing runs");
    CHECK(!WantRest({false, false, true, false}) && !WantRest({false, false, false, true}), "display off or locked: nothing runs");
    CHECK(SessionReason(WTS_SESSION_UNLOCK) &&SessionReason(WTS_CONSOLE_CONNECT) && SessionReason(WTS_REMOTE_CONNECT), "unlock and connects revalidate");
    CHECK(!SessionReason(WTS_SESSION_LOCK) && !SessionReason(WTS_CONSOLE_DISCONNECT), "lock and disconnect do not");
}

static int starts, stops;
TEST_CASE("widget registry starts a widget only while it is wanted") {
    starts = stops = 0;
    std::array<Widget, 2> ws{{
        {"a", WantRest, [] { ++starts; }, [] { ++stops; }},
        {"b", WantRest, [] { ++starts; }, [] { ++stops; }, false},  // turned off: never runs
    }};
    WidgetState on{false, false, false, false}, off{false, false, false, true};
    CHECK(Reconcile(ws, on) == 1, "starts the wanted one");
    CHECK(Reconcile(ws, on) == 0, "idempotent: no second start");
    CHECK((ws[0].running && !ws[1].running && starts == 1), "only the enabled one runs");
    CHECK(Reconcile(ws, off) == 1, "locked: stops it");
    CHECK((!ws[0].running && stops == 1), "stopped once");
    CHECK(Reconcile(ws, off) == 0, "no second stop");
}

TEST_CASE("status slot glyphs and sizes (item 11)") {  // the four approved icons; four distinct shapes, one 16 epx slot
    Tokens k;
    CHECK((k.glyphBluetooth == 0xE702 && k.glyphDownload == 0xE896 && k.glyphDone == 0xE73E && k.glyphCamera == 0xE722), "approved code points");
    CHECK((k.glyphBluetooth != k.glyphDownload && k.glyphDownload != k.glyphDone && k.glyphDone != k.glyphCamera && k.glyphCamera != k.glyphBluetooth), "distinct");
    CHECK((k.statusGlyph <= k.statusSlotW && k.statusSlotW + k.restGap == 24), "14 epx glyph in a 16 epx slot, 24 px of width");
}

TEST_CASE("rest-line order (item 9)") {  // RestSequence in shape_math.h and the restOrder setting
    using B = Tokens::RestBlock;
    using I = RestItem;
    std::array<B, 3> order = {B::Status, B::Clock, B::Usage};
    CHECK((RestSequence(order, false) == std::vector<I>{I::Status, I::Clock, I::Usage}), "default, no music");
    CHECK((RestSequence(order, true) == std::vector<I>{I::Status, I::Art, I::Clock, I::Usage, I::Equalizer}), "default with music: art and equalizer frame the date and bars, as today");
    CHECK((RestSequence({B::Clock, B::Usage, B::Status}, true) == std::vector<I>{I::Art, I::Clock, I::Usage, I::Equalizer, I::Status}), "status last: it stays outside the frame");
    CHECK((RestSequence({B::Usage, B::Status, B::Clock}, true) == std::vector<I>{I::Art, I::Usage, I::Status, I::Clock, I::Equalizer}), "status between: inside the frame");
    int perms = 0;  // every permutation keeps the same blocks (so the width is the same) and frames them with the music
    std::array<B, 3> p = {B::Status, B::Clock, B::Usage};
    std::sort(p.begin(), p.end());
    do {
        ++perms;
        auto plain = RestSequence(p, false), tucked = RestSequence(p, true);
        CHECK(plain.size() == 3 && tucked.size() == 5, "sizes for permutation %d", perms);
        CHECK((std::count(tucked.begin(), tucked.end(), I::Art) == 1 && std::count(tucked.begin(), tucked.end(), I::Equalizer) == 1), "one art and one equalizer, permutation %d", perms);
        std::vector<I> without;
        for (auto i : tucked) if (i != I::Art && i != I::Equalizer) without.push_back(i);
        CHECK(without == plain, "the music only adds items, permutation %d", perms);
        auto art = std::find(tucked.begin(), tucked.end(), I::Art), eq = std::find(tucked.begin(), tucked.end(), I::Equalizer);
        CHECK(art < eq, "art before equalizer, permutation %d", perms);
    } while (std::next_permutation(p.begin(), p.end()));
    CHECK(perms == 6, "six orders");

    auto parse = [](const char* json) { Tokens t; LoadSettings(t, json); return t.restOrder; };
    CHECK((parse(R"({"restOrder":"clock,usage,status"})") == std::array<B, 3>{B::Clock, B::Usage, B::Status}), "valid order applies");
    CHECK((parse(R"({"topOffset":10,"restOrder":"usage,status,clock","speed":60})") == std::array<B, 3>{B::Usage, B::Status, B::Clock}), "valid order among other keys");
    for (const char* bad : {R"({"restOrder":"clock,clock,usage"})", R"({"restOrder":"clock,usage"})", R"({"restOrder":"clock,usage,status,status"})", R"({"restOrder":"clock,usage,time"})",
                            R"({"restOrder":""})", R"({"restOrder":5})", R"({})"})
        CHECK((parse(bad) == order), "bad value keeps the default: %s", bad);
    Tokens d, on;
    CHECK((d.soundScreenshot && !d.soundDownloadDone && d.soundFocusDone), "sound defaults: screenshot on, download off, focus on");
    LoadSettings(on, R"({"soundScreenshot":false,"soundDownloadDone":true,"soundFocusDone":false})");
    CHECK((!on.soundScreenshot && on.soundDownloadDone && !on.soundFocusDone), "sound switches apply");
}

TEST_CASE("screenshot file name and folder (item 8)") {  // ShotFileName and ResolveShotDir in capture.h
    SYSTEMTIME st{};
    st.wYear = 2026, st.wMonth = 11, st.wDay = 13, st.wHour = 6, st.wMinute = 30, st.wSecond = 12;
    std::wstring name = ShotFileName(st);
    CHECK(name == L"Statlet 2026-11-13 06-30-12.png", "name: %ls", name.c_str());
    CHECK(name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos, "no character Windows rejects");
    CHECK(ResolveShotDir(L"C:\\t", L"C:\\Users\\a\\Pictures\\Screenshots", L"C:\\Users\\a\\Pictures") == L"C:\\t", "the test override wins");
    CHECK(ResolveShotDir(L"", L"C:\\Users\\a\\Pictures\\Screenshots", L"C:\\Users\\a\\Pictures") == L"C:\\Users\\a\\Pictures\\Screenshots", "the Screenshots folder");
    CHECK(ResolveShotDir(L"", L"", L"C:\\Users\\a\\Pictures") == L"C:\\Users\\a\\Pictures\\Screenshots", "falls back to Pictures\\Screenshots when it is missing");
    CHECK(ResolveShotDir(L"", L"", L"").empty(), "nothing known: empty, so the save fails cleanly");
}

TEST_CASE("bluetooth tracker (item 6)") {  // BtTracker in bluetooth.h: connect only, nothing during the first enumeration
    BtTracker b;
    CHECK(!b.Update(L"phone", true) && !b.Update(L"buds", false), "devices found while enumerating never peek, connected or not");
    b.EnumerationDone();
    CHECK(!b.Update(L"phone", true), "an update that repeats the state does not peek");
    CHECK(b.Update(L"buds", true), "false -> true peeks");
    CHECK(!b.Update(L"buds", true), "and only once");
    CHECK(!b.Update(L"buds", false), "disconnecting does not peek");
    CHECK(b.Update(L"buds", true), "connecting again peeks again");
    b.Remove(L"buds");
    CHECK(b.Update(L"buds", true), "removed, then re-added already connected: peeks (a fresh pairing)");
    CHECK(b.Update(L"new", true), "a device first seen after enumeration, connected, peeks");
    Tokens d, t, junk;
    CHECK(d.btIcon, "on by default");
    LoadSettings(t, R"({"btIcon":false})");
    CHECK(!t.btIcon, "false applies");
    LoadSettings(junk, R"({"btIcon":"maybe"})");
    CHECK(junk.btIcon, "a bad value keeps the default");
}

TEST_CASE("download partial names (item 7)") {  // IsPartialName in downloads.h
    for (auto n : {L"a.zip.crdownload", L"a.zip.part", L"a.zip.partial", L"a.zip.download", L"a.zip.opdownload", L"A.ZIP.CrDownload", L"x.PART"})
        CHECK(IsPartialName(n), "%ls should be partial", n);
    for (auto n : {L"a.zip", L"notes.txt", L"part", L".part", L"a.partx", L"crdownload", L"report-download.pdf", L""})
        CHECK(!IsPartialName(n), "%ls should not be partial", n);
    CHECK(PartialExtLen(L"a.zip.crdownload") == 11, "extension length");
}

TEST_CASE("download tracker (item 7)") {  // Idle -> Active -> Done -> Idle, with a fake clock in ms
    using S = DownloadTracker::State;
    auto exists = [](const std::wstring&) { return true; };
    auto gone = [](const std::wstring&) { return false; };
    DownloadTracker d;
    CHECK(d.Observe(1000, {}, exists) == S::Idle && !d.Busy(1000), "nothing there: idle");
    CHECK(d.Observe(2000, {{L"a.zip.crdownload", 10}}, exists) == S::Active && d.Busy(2000), "a new partial: active");
    CHECK(d.Observe(5000, {{L"a.zip.crdownload", 10}}, exists) == S::Active, "no growth for 3 s is still active");
    CHECK(d.Observe(6000, {{L"a.zip.crdownload", 20}}, exists) == S::Active, "growth: active");
    CHECK(d.Observe(7000, {}, exists) == S::Done && d.Busy(7000), "renamed to a.zip: done");
    CHECK(d.Observe(8500, {}, exists) == S::Done, "done shows 2 s");
    CHECK(d.Observe(9000, {}, exists) == S::Idle && !d.Busy(9000), "then idle");

    DownloadTracker c;  // cancelled: the partial is deleted, no finished file
    c.Observe(1000, {{L"b.zip.part", 5}}, gone);
    CHECK(c.Observe(2000, {}, gone) == S::Idle, "a deleted partial is idle, not done");

    DownloadTracker s;  // stalled: a crashed browser leaves its partial behind
    s.Observe(1000, {{L"c.zip.crdownload", 5}}, exists);
    CHECK(s.Observe(30000, {{L"c.zip.crdownload", 5}}, exists) == S::Active, "29 s without growth: still active");
    CHECK(s.Observe(61001, {{L"c.zip.crdownload", 5}}, exists) == S::Idle && !s.Busy(61001), "60 s without growth: ignored, timer off");
    CHECK(s.Observe(62000, {{L"c.zip.crdownload", 6}}, exists) == S::Active, "growing again: active");

    DownloadTracker two;  // two overlapping downloads: the icon stays until the last one ends
    two.Observe(1000, {{L"a.crdownload", 1}, {L"b.crdownload", 1}}, exists);
    CHECK(two.Observe(2000, {{L"b.crdownload", 2}}, exists) == S::Active, "one finished, one running: active");
    CHECK(two.Observe(3000, {}, exists) == S::Done, "last one done: done");

    DownloadTracker seed;  // partials present at start are old
    seed.Seed(100000, {{L"d.crdownload", 9}});
    CHECK(seed.Observe(100500, {{L"d.crdownload", 9}}, exists) == S::Idle && !seed.Busy(100500), "leftover at start: idle");
    CHECK(seed.Observe(101000, {{L"d.crdownload", 12}}, exists) == S::Active, "it grows: active");

    DownloadTracker early;  // the island starts in the first minute after boot (autostart): a leftover is still old
    early.Seed(5000, {{L"e.crdownload", 9}});
    CHECK(early.Observe(5500, {{L"e.crdownload", 9}}, exists) == S::Idle && !early.Busy(5500), "leftover seeded 5 s after boot: idle");
}

// tests\fixtures\settings-contract.json is also read by settings.tests (SettingsTests.cs): both sides parse every key to the
// same values, so a renamed or mistyped key fails on one side.
TEST_CASE("settings contract with the settings app (item 10)") {
    FILE* f = nullptr;
    fopen_s(&f, "..\\tests\\fixtures\\settings-contract.json", "rb");
    REQUIRE_MESSAGE(f, "tests\\fixtures\\settings-contract.json not found (run from the island folder)");
    std::string json(1 << 12, '\0');
    json.resize(fread(json.data(), 1, json.size(), f));
    fclose(f);
    Tokens d, t;
    LoadSettings(t, json);
    CHECK((t.showWeekday != d.showWeekday && t.btIcon != d.btIcon && t.downloadIcon != d.downloadIcon), "every icon key flips its default");
    CHECK((t.soundScreenshot != d.soundScreenshot && t.soundDownloadDone != d.soundDownloadDone && t.soundFocusDone != d.soundFocusDone), "every sound key flips its default");
    using B = Tokens::RestBlock;
    CHECK((t.restOrder == std::array<B, 3>{B::Usage, B::Status, B::Clock}), "restOrder usage,status,clock");
    CHECK((!t.showWeekday && !t.btIcon && !t.downloadIcon && !t.soundScreenshot && t.soundDownloadDone && !t.soundFocusDone), "the values the settings app reads from the same file");
}

TEST_CASE("spring parity with the settings app") {  // tests\fixtures\spring.json is also checked by settings.tests (Motion.cs)
    FILE* f = nullptr;
    fopen_s(&f, "..\\tests\\fixtures\\spring.json", "rb");
    REQUIRE_MESSAGE(f, "tests\\fixtures\\spring.json not found (run from the island folder)");
    std::string json(1 << 16, '\0');
    json.resize(fread(json.data(), 1, json.size(), f));
    fclose(f);
    int cases = 0;
    for (size_t at = json.find('{', json.find('[')); at != std::string::npos; at = json.find('{', at + 1)) {  // one object per case
        std::string_view one = std::string_view(json).substr(at, json.find('}', at) - at);
        float bounce = std::strtof(std::string(SettingRaw(one, "bounce")).c_str(), nullptr), speed = std::strtof(std::string(SettingRaw(one, "speed")).c_str(), nullptr);
        float stiffness = 0, damping = 0;
        SpringFromFeel(bounce, speed, stiffness, damping);
        float wantK = std::strtof(std::string(SettingRaw(one, "stiffness")).c_str(), nullptr), wantC = std::strtof(std::string(SettingRaw(one, "damping")).c_str(), nullptr);
        CHECK(std::fabs(stiffness - wantK) < 0.01f * std::max(1.f, wantK / 100), "speed %g bounce %g: stiffness %f, fixture %f", speed, bounce, stiffness, wantK);
        CHECK(std::fabs(damping - wantC) < 0.01f * std::max(1.f, wantC / 10), "speed %g bounce %g: damping %f, fixture %f", speed, bounce, damping, wantC);
        cases++;
    }
    CHECK(cases >= 10, "only %d fixture cases read", cases);
}

TEST_CASE("data dir honors STATLET_DATA_DIR") {  // so test launches never touch the real settings.json
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", L"C:\\temp\\statlet-test");
    CHECK(DataDir() == L"C:\\temp\\statlet-test", "override wins: %ls", DataDir().c_str());
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", nullptr);
    std::wstring d = DataDir();
    CHECK(d.size() > 8 && d.compare(d.size() - 8, 8, L"\\Statlet") == 0, "default ends in \\Statlet: %ls", d.c_str());
}

TEST_CASE("bar style settings") {
    Tokens d;
    CHECK(d.progressStyle == Tokens::ProgressStyle::Wave && d.knob == Tokens::Knob::Never && !d.artAccent && !d.timeTotal, "defaults");
    auto load = [](const char* json) { Tokens t; LoadSettings(t, json); return t; };
    using PS = Tokens::ProgressStyle;
    CHECK(load("{\"progressStyle\":\"segmented\"}").progressStyle == PS::Segmented && load("{\"progressStyle\":\"hairline\"}").progressStyle == PS::Hairline &&
              load("{\"progressStyle\":\"gradient\"}").progressStyle == PS::Gradient && load("{\"progressStyle\":\"line\"}").progressStyle == PS::Line, "styles");
    CHECK(load("{\"playheadKnob\":\"hover\"}").knob == Tokens::Knob::Hover && load("{\"playheadKnob\":\"always\"}").knob == Tokens::Knob::Always &&
              load("{\"playheadKnob\":\"never\"}").knob == Tokens::Knob::Never, "knob");
    CHECK(load("{\"artAccent\":true}").artAccent, "art accent");
    CHECK(load("{\"progressStyle\":\"zigzag\"}").progressStyle == PS::Wave, "an unknown style keeps the default");
    CHECK(load("{\"playheadKnob\":7}").knob == Tokens::Knob::Never && !load("{\"artAccent\":maybe}").artAccent, "bad knob and flag keep defaults");
    Tokens s;
    LoadState(s, "{\"timeTotal\":true}");
    CHECK(s.timeTotal, "state.json: time total");
    LoadState(s, "garbage");
    CHECK(s.timeTotal, "garbage state keeps the value");
}

TEST_CASE("bar style edge cases") {  // QA additions: boundaries of the v2 bar-style helpers
    // RightTime: past the end shows -0:00, an hour or more gets hours, no duration is -0:00.
    CHECK(RightTime(200, 250, false) == L"-0:00", "position past the end clamps to -0:00");
    CHECK(RightTime(3725, 0, true) == L"1:02:05" && RightTime(3725, 25, false) == L"-1:01:40", "an hour or more shows hours");
    CHECK(RightTime(0, 0, false) == L"-0:00" && RightTime(0, 0, true) == L"0:00", "no duration");
    // LiftForGlass: black lifts to grey at the floor, the floor itself and white are untouched.
    Rgba black = LiftForGlass({255, 0, 0, 0}), floorGrey = LiftForGlass({255, 140, 140, 140}), white = LiftForGlass({255, 255, 255, 255});
    CHECK(black.r == 140 && black.g == 140 && black.b == 140, "black lifts to luma 140 grey");
    CHECK(floorGrey.r == 140 && floorGrey.g == 140 && floorGrey.b == 140, "luma exactly 140 is untouched");
    CHECK(white.r == 255 && white.g == 255 && white.b == 255, "white is untouched");
    // DominantColor: alpha 127 is ignored, 128 counts, and premultiplied pixels come back un-premultiplied.
    Rgba c{};
    auto fillA = [](std::vector<uint8_t>& v, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        for (size_t i = 0; i < v.size() / 4; i++) v[i * 4] = uint8_t(b * a / 255), v[i * 4 + 1] = uint8_t(g * a / 255), v[i * 4 + 2] = uint8_t(r * a / 255), v[i * 4 + 3] = a;
    };
    std::vector<uint8_t> px(100 * 4);
    fillA(px, 200, 30, 40, 127);
    CHECK(!DominantColor(px.data(), 100, c), "alpha 127 counts as transparent");
    fillA(px, 200, 30, 40, 128);
    CHECK(DominantColor(px.data(), 100, c) && std::abs(int(c.r) - 200) <= 2 && std::abs(int(c.g) - 30) <= 2 && std::abs(int(c.b) - 40) <= 2, "alpha 128 counts, colour un-premultiplied");
    // Settings: state.json owns timeTotal; settings.json must not set it; false and absent keys behave.
    Tokens t;
    LoadSettings(t, "{\"timeTotal\":true}");
    CHECK(!t.timeTotal, "a stray timeTotal in settings.json is ignored");
    LoadState(t, "{\"timeTotal\":true}");
    LoadState(t, "{}");
    CHECK(t.timeTotal, "state.json without the key keeps the value");
    LoadState(t, "{\"timeTotal\":false}");
    CHECK(!t.timeTotal, "state.json can turn it back off");
    auto load = [](const char* json) { Tokens k; LoadSettings(k, json); return k; };
    CHECK(load("{\"playheadKnob\":\"Hover\"}").knob == Tokens::Knob::Never && load("{\"progressStyle\":\"Hairline\"}").progressStyle == Tokens::ProgressStyle::Wave,
          "values are case-sensitive: a wrong case keeps the default");
}

TEST_CASE("state.json write") {
    std::wstring dir = std::wstring(_wgetenv(L"TEMP")) + L"\\statlet-state-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    CHECK(WriteState(dir, true), "writes into an existing folder");
    FILE* f = _wfopen((dir + L"\\state.json").c_str(), L"rb");
    char buf[64] = {};
    if (f) fread(buf, 1, sizeof buf - 1, f), fclose(f);
    Tokens t;
    LoadState(t, buf);
    CHECK(t.timeTotal, "reads back what was written");
    CHECK(GetFileAttributesW((dir + L"\\state.json.tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "no temp file is left behind");
    std::wstring missing = dir + L"\\no\\such";
    CHECK(!WriteState(missing, true) && GetFileAttributesW((missing + L"\\state.json.tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "a failed write returns false and leaves no temp file");
    DeleteFileW((dir + L"\\state.json").c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("settings edge cases") {  // the file is hand-editable, so bad values keep the defaults
    Tokens d;
    auto load = [](const char* json) { Tokens t; LoadSettings(t, json); return t; };
    CHECK(load("not json at all").topOffset == d.topOffset, "garbage keeps the default");
    CHECK(load("{\"topOffset\":}").topOffset == d.topOffset, "a missing value keeps the default");
    CHECK(load("{\"topOffset\":nan}").topOffset == d.topOffset, "nan is ignored");
    CHECK(load("{\"topOffset\":1e999}").topOffset == d.topOffset, "an overflowing number is ignored");
    CHECK(load("{\"topOffset\":12px}").topOffset == d.topOffset, "trailing junk is ignored");
    CHECK(load("{\"topOffset\" : \"7\" }").topOffset == 7, "spaces and quotes around a number are fine");
    CHECK(load("{\"topOffset\":-3}").topOffset == 0 && load("{\"topOffset\":999}").topOffset == 48, "values are clamped");
    CHECK(load("{\"warnAt\":95,\"critAt\":50}").critAt == 100, "critAt stays at least 5 above warnAt");
    CHECK(load("{\"mediaHideMs\":0}").mediaHideMs == 0, "0 means never hide, and is allowed");
    CHECK(load("{\"openOnHighUsage\":yes}").openOnHighUsage == d.openOnHighUsage, "only true and false are booleans");
    Rgba tint = load("{\"glassTint\":\"#GGGGGG\"}").glassTint;
    CHECK(tint.r == d.glassTint.r && tint.g == d.glassTint.g && tint.b == d.glassTint.b, "a bad color keeps the default");
    CHECK(load("{\"afterPeek\":\"nonsense\"}").afterPeek == d.afterPeek, "an unknown choice keeps the default");
}

TEST_SUITE_END();

// One compositor and Gfx for the render and screen suites.
static Gfx& Rig() {
    static wuc::Compositor compositor;
    static auto* gfx = new Gfx(compositor,L"assets\\Manrope.ttf", scale);
    return *gfx;
}

TEST_SUITE_BEGIN("render");

TEST_CASE("weekday rest line") {  // clock.h and the showWeekday setting (v2 item 5, ADR-0002); en-US
    SYSTEMTIME thu{};
    thu.wYear = 2026, thu.wMonth = 11, thu.wDay = 12, thu.wDayOfWeek = 4;
    CHECK(RestDate(&thu, true, L"en-US") == L"Thu Nov 12" && RestDate(&thu, false, L"en-US") == L"Nov 12", "day text: %ls", RestDate(&thu, true, L"en-US").c_str());
    CHECK(WidestTimeSamples().size() == 48, "minute 00 and 59 of each hour: %zu", WidestTimeSamples().size());
    Tokens d, t, junk;
    CHECK(d.showWeekday, "on by default");
    LoadSettings(t, R"({"showWeekday":false})");
    CHECK(!t.showWeekday, "false applies");
    LoadSettings(junk, R"({"showWeekday":"maybe"})");
    CHECK(junk.showWeekday, "a bad value keeps the default");
}

TEST_CASE("day-fitted pill stays within the measured band for every day of 2026 and 2027") {  // ADR-0002 width guard, clock only
    Gfx& gfx = Rig();
    Tokens t;
    Text rest(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    float widest = -1;
    std::wstring widestTime;
    for (auto& s : WidestTimeSamples()) {
        rest.Set(s);
        if (rest.Width() > widest) widest = rest.Width(), widestTime = s;
    }
    FILETIME ft{};
    SYSTEMTIME start{};
    start.wYear = 2026, start.wMonth = 1, start.wDay = 1;
    SystemTimeToFileTime(&start, &ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
    int days = 0, outside = 0;
    float lo = 1e9f, hi = 0;
    for (; days < 730; ++days, u.QuadPart += 864000000000ULL) {  // 100 ns ticks per day
        FILETIME f{u.LowPart, u.HighPart};
        SYSTEMTIME st{};
        FileTimeToSystemTime(&f, &st);
        rest.Set(RestDate(&st, true) + L"  " + widestTime);
        float w = FitCollapsedWidth(rest.Width() + t.restGap + t.restBarW, t);
        lo = std::min(lo, w), hi = std::max(hi, w);
        if (w < 160 || w > 188) outside++;  // DirectWrite measured 160-188 for Segoe UI Variable 12 px; the spec's Pillow estimate was 152-180 (flagged to the Architect)
    }
    CHECK(days == 730 && outside == 0, "%d days, %d outside 160-188 (range %.0f to %.0f)", days, outside, lo, hi);
}

TEST_CASE("status slot keeps the pill under the cap for every day of 2026 and 2027 (item 7)") {  // plan: 212 clock only, 256 with music, cap 260
    Gfx& gfx = Rig();
    Tokens t;
    Text rest(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    float widest = -1;
    std::wstring widestTime;
    for (auto& s : WidestTimeSamples()) {
        rest.Set(s);
        if (rest.Width() > widest) widest = rest.Width(), widestTime = s;
    }
    float eq = 4 * t.eqBarW + 3 * t.eqGap, slot = t.statusSlotW + t.restGap;
    FILETIME ft{};
    SYSTEMTIME start{};
    start.wYear = 2026, start.wMonth = 1, start.wDay = 1;
    SystemTimeToFileTime(&start, &ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
    float plain = 0, music = 0;
    for (int days = 0; days < 730; ++days, u.QuadPart += 864000000000ULL) {
        FILETIME f{u.LowPart, u.HighPart};
        SYSTEMTIME st{};
        FileTimeToSystemTime(&f, &st);
        rest.Set(RestDate(&st, true) + L"  " + widestTime);
        float clock = rest.Width() + t.restGap + t.restBarW;
        plain = std::max(plain, FitCollapsedWidth(clock + slot, t));
        float tucked = t.artSmall + t.restGap + clock + t.restGap + eq;  // the row as LayoutCollapsed builds it
        music = std::max(music, std::ceil((tucked + slot + 2 * t.mediaPadX) / t.widthStep) * t.widthStep);  // unclamped, so a clip would show
    }
    CHECK(plain <= 212, "clock only with the slot: %.0f px", plain);
    CHECK(music <= 256 && music < t.collapsedMaxW, "music with the slot: %.0f px (cap %.0f)", music, t.collapsedMaxW);
}

// ---- QA (v2 item 5): 12h and 24h bands, and the rollover property. Not part of the dev branch. ----
static std::wstring QaTime(const wchar_t* loc, WORD h, WORD m) {
    SYSTEMTIME st{}; st.wHour = h; st.wMinute = m;
    wchar_t b[64]; GetTimeFormatEx(loc, TIME_NOSECONDS, &st, nullptr, b, 64); return b;
}
static SYSTEMTIME QaDay(int dayIndex) {  // dayIndex days after 2026-01-01
    FILETIME ft{}; SYSTEMTIME s{}; s.wYear = 2026, s.wMonth = 1, s.wDay = 1; SystemTimeToFileTime(&s, &ft);
    ULARGE_INTEGER u{}; u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime; u.QuadPart += 864000000000ULL * dayIndex;
    FILETIME f{u.LowPart, u.HighPart}; SYSTEMTIME o{}; FileTimeToSystemTime(&f, &o); return o;
}
TEST_CASE("QA weekday: pill width band, 12h and 24h, every day of 2026 and 2027") {
    Gfx& gfx = Rig(); Tokens t;
    Text rest(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    for (const wchar_t* loc : {L"en-US", L"en-GB"}) {
        float widest = -1; std::wstring widestTime;  // the true widest over all 1440 minutes
        for (WORD h = 0; h < 24; ++h) for (WORD m = 0; m < 60; ++m) { auto s = QaTime(loc, h, m); rest.Set(s); if (rest.Width() > widest) widest = rest.Width(), widestTime = s; }
        float lo = 1e9f, hi = 0; int outside = 0;
        const bool h12 = wcscmp(loc, L"en-US") == 0; const float bandLo = h12 ? 160.f : 140.f, bandHi = h12 ? 188.f : 168.f;  // Architect: 24h has its own band
        for (int d = 0; d < 730; ++d) {
            auto st = QaDay(d); rest.Set(RestDate(&st, true, loc) + L"  " + widestTime);
            float w = FitCollapsedWidth(rest.Width() + t.restGap + t.restBarW, t); lo = std::min(lo, w), hi = std::max(hi, w); if (w < bandLo || w > bandHi) outside++;
        }
        MESSAGE(Fmt("%ls widest time '%ls' pill range %.0f to %.0f, %d days outside %.0f-%.0f", loc, widestTime.c_str(), lo, hi, outside, bandLo, bandHi));
        CHECK(outside == 0, "%ls: %d days outside %.0f-%.0f (range %.0f to %.0f)", loc, outside, bandLo, bandHi, lo, hi);
    }
}
TEST_CASE("QA weekday: current-locale samples contain the true widest time (what MeasureWidestTime relies on)") {
    Gfx& gfx = Rig(); Tokens t;
    Text rest(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    float sampled = -1, all = -1;
    for (auto& s : WidestTimeSamples()) { rest.Set(s); sampled = std::max(sampled, rest.Width()); }
    for (WORD h = 0; h < 24; ++h) for (WORD m = 0; m < 60; ++m) { rest.Set(QaTime(LOCALE_NAME_USER_DEFAULT, h, m)); all = std::max(all, rest.Width()); }
    CHECK(sampled == all, "samples %.2f vs all 1440 minutes %.2f", sampled, all);
}
TEST_CASE("QA weekday: width never moves during a day (9:59 to 10:00, every minute), changes only with the date at midnight") {
    Gfx& gfx = Rig(); Tokens t;
    Text rest(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true}), meas(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    for (const wchar_t* loc : {L"en-US", L"en-GB"}) {
        float widest = -1; std::wstring wt;
        for (WORD h = 0; h < 24; ++h) for (WORD m : {WORD(0), WORD(59)}) { auto s = QaTime(loc, h, m); meas.Set(s); if (meas.Width() > widest) widest = meas.Width(), wt = s; }
        int moved = 0, clamped = 0, days = 0; float prevPill = -1; int dateChanges = 0;
        for (int d = 0; d < 14; ++d) {  // two weeks: every weekday twice
            auto st = QaDay(d + 140); auto date = RestDate(&st, true, loc);
            meas.Set(date + L"  " + wt); float dayW = meas.Width(), pill0 = -1;
            for (WORD h = 0; h < 24; ++h) for (WORD m = 0; m < 60; ++m) {
                rest.Set(date + L"  " + QaTime(loc, h, m));
                if (rest.Width() > dayW + 0.01f) clamped++;  // the line would be wider than the reserved width: dayExtra clamps to 0 and the pill would grow
                float extra = std::max(0.f, dayW - rest.Width());
                float pill = FitCollapsedWidth(rest.Width() + extra + t.restGap + t.restBarW, t);
                if (pill0 < 0) pill0 = pill; else if (pill != pill0) moved++;
            }
            if (prevPill >= 0 && pill0 != prevPill) dateChanges++;
            prevPill = pill0; days++;
        }
        MESSAGE(Fmt("%ls: %d days x 1440 minutes, minutes where the pill moved: %d, lines wider than reserved: %d, midnight width changes over %d day boundaries: %d", loc, days, moved, clamped, days - 1, dateChanges));
        CHECK(moved == 0, "%ls: the pill moved %d times inside a day", loc, moved);
        CHECK(clamped == 0, "%ls: %d minutes wider than the day's widest time", loc, clamped);
    }
}

TEST_CASE("the date shrinks to the weekday when the tucked row would pass the cap, at every text size (item 7 follow-up)") {
    Gfx& gfx = Rig();
    for (float textScale : {0.85f, 1.f, 1.1f, 1.2f}) {
        Tokens t;
        std::string json = Fmt("{\"textScale\":%g}", textScale);
        LoadSettings(t, json);
        Text rest(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
        float widest = -1;
        std::wstring widestTime;
        for (auto& s : WidestTimeSamples()) {
            rest.Set(s);
            if (rest.Width() > widest) widest = rest.Width(), widestTime = s;
        }
        for (bool slot : {false, true}) {
            int shrunk = 0, days = 0, clipped = 0;
            SYSTEMTIME st{};
            st.wYear = 2026, st.wMonth = 1, st.wDay = 1;
            FILETIME ft{};
            SystemTimeToFileTime(&st, &ft);
            ULARGE_INTEGER u{};
            u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
            for (; days < 365; ++days, u.QuadPart += 864000000000ULL) {
                FILETIME f{u.LowPart, u.HighPart};
                FileTimeToSystemTime(&f, &st);
                rest.Set(RestDate(&st, true) + L"  " + widestTime);
                float w = rest.Width();
                if (TuckedRowTooWide(t, slot, w)) {
                    ++shrunk;
                    rest.Set(ShortDay(&st) + L"  " + widestTime);  // what UpdateClock shows instead
                    w = rest.Width();
                }
                if (TuckedRowTooWide(t, slot, w)) ++clipped;  // even the weekday is too wide: nothing more to give
            }
            MESSAGE(Fmt("text scale %.2f, slot %d: date shortened on %d of %d days", textScale, int(slot), shrunk, days));
            CHECK(clipped == 0, "scale %.2f slot %d: %d days still pass the cap with the short date", textScale, int(slot), clipped);
            if (textScale == 1.f && !slot) CHECK(shrunk == 0, "100%% text, no slot: the full date always fits (%d days shortened)", shrunk);
            if (textScale == 1.2f && slot) CHECK(shrunk > 0, "120%% text with the slot should shorten the date on some days");
        }
    }
}

TEST_CASE("text and icon goldens") {
    Gfx& gfx = Rig();
    Tokens t;
    UINT32 index;
    BOOL exists = FALSE;
    CHECK(gfx.bundled && SUCCEEDED(gfx.bundled->FindFamilyName(L"Manrope", &index, &exists)) && exists, "bundled Manrope did not load");

    Text title(gfx, {t.typeTitle, DWRITE_FONT_WEIGHT_BOLD, t.ink});
    title.Set(L"Harbor Lights");
    Text clock(gfx, {t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    clock.Set(L"12:34 PM");
    Text label(gfx, {t.typeLabel, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink2, false, 0.08f});
    label.Set(L"FOCUS");
    Text trimmed(gfx, {t.typeBody, DWRITE_FONT_WEIGHT_NORMAL, t.ink2}, 120);
    trimmed.Set(L"A very long artist name that cannot fit");
    auto icon = MakeIcon(gfx, 0xE768, 16, t.accentFallback);  // Play

    CHECK(trimmed.Width() <= 120, "trimmed width %f", trimmed.Width());
    Text a(gfx, {t.typeTimerBig, DWRITE_FONT_WEIGHT_BOLD, t.ink, true}), b(gfx, {t.typeTimerBig, DWRITE_FONT_WEIGHT_BOLD, t.ink, true});
    a.Set(L"11:11");
    b.Set(L"00:00");
    CHECK(a.Width() == b.Width(), "tabular digits differ: %f vs %f", a.Width(), b.Width());
    CHECK(std::fabs(title.Height() - 22) < 3, "title line height %f", title.Height());
    CHECK(icon->Width() > 10 && icon->Height() > 10, "icon glyph missing");

    // Drawn offscreen at 100, 125, 150 and 200%, whatever this machine's DPI (the golden name carries the DPI).
    const float machine = scale;
    for (float s : {1.f, 1.25f, 1.5f, 2.f}) {
        scale = s;
        gfx.SetScale(s);
        for (auto [name, text] : {std::pair{"text-title", &title}, {"text-clock", &clock}, {"text-label", &label}, {"text-trimmed", &trimmed}, {"icon-play", icon.get()}})
            Golden(name, RenderOffscreen(gfx, text->Width(), text->Height(), t.glassTint, [&](ID2D1DeviceContext* ctx) { text->Paint(ctx); }), 24, 0.01);
        float side = RoundedShape::ShadowSide(t, t.expandedR);
        Golden("shadow", RenderOffscreen(gfx, side, side, Argb(0xFFFFFFFF), [&](ID2D1DeviceContext* ctx) { RoundedShape::PaintShadow(ctx, t, t.expandedR); }), 8, 0.01);
    }
    scale = machine;  // the screen suite draws at the machine's DPI
    gfx.SetScale(machine);
}

TEST_SUITE_END();

// --- Screenshot checks: the pieces composed on screen ---

static Image CaptureScreen(int x, int y, int w, int h) {
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void* bits;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
    Image img;
    img.w = w, img.h = h;
    img.px.assign((uint32_t*)bits, (uint32_t*)bits + w * h);
    for (auto& p : img.px) p |= 0xFF000000;
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

static void Pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(5);
    }
    DwmFlush();
    DwmFlush();
}

static HWND MakeWindow(const wchar_t* cls, DWORD ex, int x, int y, int w, int h, HBRUSH background) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = background;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | ex, cls, cls, WS_POPUP, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

TEST_SUITE_BEGIN("screen");

TEST_CASE("pieces on screen") {
    Gfx& gfx = Rig();
    Tokens t;
    const float W = 240, H = 90, X = 40, Y = 30, hostW = 320, hostH = 170;  // epx
    const int x0 = 120, y0 = 220, wpx = int(std::ceil(hostW * scale)), hpx = int(std::ceil(hostH * scale));
    const uint32_t magenta = 0xFFFF00FF;
    HWND back = MakeWindow(L"StatletTestBackdrop", 0, x0 - 40, y0 - 40, wpx + 80, hpx + 80, CreateSolidBrush(RGB(255, 0, 255)));
    HWND host = MakeWindow(L"StatletTestIsland", WS_EX_NOREDIRECTIONBITMAP, x0, y0, wpx, hpx, nullptr);
    auto target = AttachCompositor(gfx.compositor, host);

    auto c = gfx.compositor;
    auto root = c.CreateContainerVisual();
    root.Size({hostW, hostH});
    root.Scale({scale, scale, 1});
    auto props = c.CreatePropertySet();
    for (auto [name, value] : {std::pair{L"X", X}, {L"Y", Y}, {L"W", W}, {L"H", H}, {L"R", t.expandedR}}) props.InsertScalar(name, value);
    RoundedShape shape(gfx, props, t, t.expandedR);
    Acrylic glass(gfx, t);
    Bar bar(gfx, W - 40, 6, t.track, t.accentFallback);
    bar.SetProgress(0.5f);
    bar.visual.Offset({X + 20, Y + H - 22, 0});
    Text title(gfx, {t.typeTitle, DWRITE_FONT_WEIGHT_BOLD, t.ink});
    title.Set(L"Statlet");
    title.visual.Offset({X + 20, Y + 16, 0});
    shape.content.Children().InsertAtTop(glass.root);
    shape.content.Children().InsertAtTop(bar.visual);
    shape.content.Children().InsertAtTop(title.visual);
    root.Children().InsertAtTop(shape.root);
    target.Root(root);
    Pump(600);

    auto px = [&](float epx) { return UINT(std::floor(epx * scale)); };
    auto at = [&](const Image& img, float ex, float ey) { return img.At(px(ex), px(ey)); };
    Image shot = CaptureScreen(x0, y0, wpx, hpx);

    uint32_t clear = at(shot, 4, 4), corner = at(shot, X + 1, Y + 1), glassPx = at(shot, X + W / 2, Y + 42);
    CHECK(Diff(clear, magenta) <= 2, "outside the shape is untouched: %08X", clear);
    CHECK(R(corner) > 180 && B(corner) > 180, "corner outside the arc is not glass: %08X", corner);
    // DWM draws the live backdrop only while it is active (not when the desktop is locked or unattended). Then the
    // golden is meaningless, so say so and skip it; STATLET_REQUIRE_ACRYLIC=1 turns the skip into a failure.
    bool acrylicOn = R(glassPx) < 140 && G(glassPx) < 60;
    if (!acrylicOn && !getenv("STATLET_REQUIRE_ACRYLIC")) MESSAGE(Fmt("SKIPPED live-acrylic golden and glass checks: backdrop inactive (glass pixel %08X); the fallback golden below still runs", glassPx));
    else {
        Golden("screen-island", shot, 12, 0.02);
        CHECK(acrylicOn, "glass tints the backdrop dark: %08X", glassPx);
    }
    uint32_t edge = at(shot, X + W / 2, Y + 0.4f), inside = at(shot, X + W / 2, Y + 4);
    if (acrylicOn) CHECK(G(edge) > G(inside) + 4, "inner edge is lighter than the glass: %08X vs %08X", edge, inside);
    uint32_t below = at(shot, X + W / 2, Y + H + 10), farBelow = at(shot, X + W / 2, hostH - 2);
    CHECK(R(below) < R(farBelow) - 20, "shadow darkens below the shape: %08X vs %08X", below, farBelow);
    uint32_t fill = at(shot, X + 22, Y + H - 19), track = at(shot, X + W - 24, Y + H - 19);
    CHECK(Diff(fill, 0xFF000000 | (t.accentFallback.r << 16) | (t.accentFallback.g << 8) | t.accentFallback.b) <= 4, "bar fill is the accent: %08X", fill);
    if (acrylicOn) CHECK(G(track) > G(glassPx) + 10, "bar track is lighter than the glass: %08X vs %08X", track, glassPx);
    UINT textPixels = 0;
    for (UINT y = px(Y + 16); y < px(Y + 36); y++)
        for (UINT x = px(X + 20); x < px(X + 80); x++) textPixels += G(shot.At(x, y)) > 200;
    CHECK(textPixels > 40, "title text is drawn: %u bright pixels", textPixels);

    // Device loss: a new device must redraw every surface, so the capture matches the one before.
    gfx.RecoverDevice();
    Pump(400);
    Image recovered = CaptureScreen(x0, y0, wpx, hpx);
    size_t changed = 0;
    for (size_t i = 0; i < shot.px.size(); i++) changed += Diff(shot.px[i], recovered.px[i]) > 12;
    CHECK(changed < shot.px.size() / 200, "after device recovery %zu pixels changed", changed);

    // Transparency effects off: solid fallback (#1C1E24 at 94%) over the magenta backdrop.
    glass.SetEffectsEnabled(false);
    Pump(400);
    Image solid = CaptureScreen(x0, y0, wpx, hpx);
    Golden("screen-island-fallback", solid, 12, 0.02);
    auto blend = [&](int fg, int bg) { return (fg * t.fallback.a + bg * (255 - t.fallback.a)) / 255; };
    uint32_t want = 0xFF000000 | (blend(t.glassTint.r, 255) << 16) | (blend(t.glassTint.g, 0) << 8) | blend(t.glassTint.b, 255);
    CHECK(Diff(at(solid, X + W / 2, Y + 42), want) <= 4, "fallback color %08X, want %08X", at(solid, X + W / 2, Y + 42), want);

    target.Root(nullptr);
    DestroyWindow(host);
    DestroyWindow(back);
}

TEST_SUITE_END();

// --- End to end: the real island's click-through (moves the cursor) ---

TEST_SUITE_BEGIN("e2e");

TEST_CASE("click-through") {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi;
    SetEnvironmentVariableW(L"STATLET_TOP_OFFSET", L"300");
    SetEnvironmentVariableW(L"ISLAND_BENCH", L"e2e");  // skips the single-instance check and the tray icon
    wchar_t cmd[] = L"out\\statlet.exe";
    POINT saved;
    GetCursorPos(&saved);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int cx = (mi.rcWork.left + mi.rcWork.right) / 2, cy = mi.rcWork.top + int(317 * scale);  // middle of the collapsed island
    SetCursorPos(cx, cy + int(400 * scale));
    CHECK(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "could not start out\\statlet.exe");
    // The child has its copy: clear ours, or the "instance" suite's island inherits bench mode and writes no log.
    for (auto name : {L"ISLAND_BENCH", L"STATLET_TOP_OFFSET"}) SetEnvironmentVariableW(name, nullptr);
    Sleep(1500);
    HWND island = FindWindowW(L"StatletIsland", nullptr);
    CHECK(island, "island window not found");
    RECT wr;
    GetWindowRect(island, &wr);
    auto owner = [](int x, int y) { return GetAncestor(WindowFromPoint({x, y}), GA_ROOT); };
    CHECK(owner(wr.left + 4, wr.top + 4) != island, "clicks outside the shape go through");
    CHECK(owner(cx, cy) != island, "clicks go through while the cursor is elsewhere");
    SetCursorPos(cx, cy);
    Sleep(250);
    CHECK(owner(cx, cy) == island, "the island takes clicks once the cursor is on it");
    Sleep(500);
    SetCursorPos(wr.left + 4, wr.top + 4);  // the move reaches the island, which turns click-through at once
    Sleep(20);
    CHECK(owner(wr.left + 4, wr.top + 4) != island, "leaving the shape restores click-through before the next poll");
    SetCursorPos(saved.x, saved.y);
    PostMessageW(island, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

TEST_SUITE_END();

// --- A test island beside the real one (needs a desktop; does not touch the mouse or the real data folder) ---

TEST_SUITE_BEGIN("instance");

static HWND IslandOf(DWORD pid) {  // Ejay's own island may be running too, so FindWindow is not enough
    struct Find { DWORD pid; HWND found; } f{pid, nullptr};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* f = reinterpret_cast<Find*>(lp);
        DWORD p = 0;
        wchar_t cls[32] = L"";
        GetWindowThreadProcessId(w, &p);
        GetClassNameW(w, cls, 32);
        if (p != f->pid || wcscmp(cls, L"StatletIsland")) return TRUE;
        f->found = w;
        return FALSE;
    }, LPARAM(&f));
    return f.found;
}

static std::string ReadFile(const std::wstring& path, uintmax_t from = 0) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) < from || ec) from = 0;  // trimmed meanwhile: read it all
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        _fseeki64(f, __int64(from), SEEK_SET);
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    return text;
}

TEST_CASE("a test island runs beside the real one and revalidates on resume, unlock and display change") {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    auto dir = std::filesystem::path(self).parent_path();
    std::wstring data = (dir / L"instance-test").wstring(), log = data + L"\\island.log", real = DataDir() + L"\\island.log";
    std::filesystem::create_directories(data);
    std::error_code ec;
    uintmax_t before = std::filesystem::file_size(log, ec), realBefore = std::filesystem::file_size(real, ec);
    if (ec) realBefore = 0;
    SetEnvironmentVariableW(L"STATLET_INSTANCE", L"test");
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", data.c_str());
    SetEnvironmentVariableW(L"STATLET_TOP_OFFSET", L"300");
    std::wstring cmd = L"\"" + (dir / L"statlet.exe").wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (auto name : {L"STATLET_INSTANCE", L"STATLET_DATA_DIR", L"STATLET_TOP_OFFSET"}) SetEnvironmentVariableW(name, nullptr);
    REQUIRE_MESSAGE(started, "could not start statlet.exe beside the tests");
    HWND island = nullptr;
    for (int i = 0; i < 50 && !island; i++) Sleep(100), island = IslandOf(pi.dwProcessId);
    CHECK(island, "the test island did not start (the single-instance check may ignore STATLET_INSTANCE)");
    // The window exists before the island has installed its window-event hooks: wait for the line logged after them.
    for (int i = 0; i < 100 && island && ReadFile(log, before).find("timers on") == std::string::npos; i++) Sleep(50);
    if (island) {
        PostMessageW(island, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
        PostMessageW(island, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        PostMessageW(island, WM_DISPLAYCHANGE, 32, 0);
        PostMessageW(island, WM_DISPLAYCHANGE, 32, 0);  // a burst, as a cable plug sends
        Sleep(600);
        std::string text = ReadFile(log, before);
        CHECK(text.find(Fmt("start pid %lu", pi.dwProcessId)) != std::string::npos, "the test island logs to its own folder");
        size_t at = text.find("place (resume+unlock+display): "), places = 0;
        for (size_t i = text.find("place ("); i != std::string::npos; i = text.find("place (", i + 1)) places++;
        CHECK(at != std::string::npos, "no 'place (resume+unlock+display)' line in %ls", log.c_str());
        CHECK(places == 2, "the burst placed once (plus start): %zu place lines", places);

        // W4: a fullscreen window in front on the primary hides the island even though it never takes the focus, and
        // closing it brings the island back after the return wait. The screen shows black for under a second.
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"StatletTestFullscreen";
        RegisterClassW(&wc);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        RECT m = mi.rcMonitor;
        uintmax_t mark = std::filesystem::file_size(log, ec);
        // Topmost: a background process cannot raise a plain window above the foreground one, which made this flaky.
        HWND big = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST, wc.lpszClassName, L"Statlet test window", WS_POPUP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                                   nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowPos(big, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Sleep(400);
        std::string hid = ReadFile(log, mark);
        CHECK(big && IsWindowVisible(big), "the test window was not created or shown (error %lu)", GetLastError());
        CHECK(GetForegroundWindow() != big, "the test window took the focus; this check needs it not to");
        std::string top;  // what is in front, for the failure message
        auto hides = [](const std::string& s) { return s.find("-> hidden:") != std::string::npos; };  // from no, or from slid when a maximized window is behind
        if (!hides(hid)) {
            int n = 0;
            for (HWND w = GetTopWindow(nullptr); w && n < 6; w = GetWindow(w, GW_HWNDNEXT)) {
                if (!IsWindowVisible(w)) continue;
                wchar_t cls[48] = L"";
                RECT r{};
                GetClassNameW(w, cls, 48);
                GetWindowRect(w, &r);
                top += Fmt(" [%ls ex=%lX %ld,%ld-%ld,%ld%s]", cls, GetWindowLongW(w, GWL_EXSTYLE), r.left, r.top, r.right, r.bottom, w == big ? " TEST" : "");
                n++;
            }
        }
        CHECK(hides(hid), "a fullscreen window in front without the focus did not hide the island: '%s' z-order:%s", hid.c_str(), top.c_str());
        mark = std::filesystem::file_size(log, ec);
        DestroyWindow(big);
        Sleep(400);
        CHECK(ReadFile(log, mark).find("away hidden -> ") != std::string::npos, "closing it did not bring the island back");

        // W6: locking stops the clock and usage timers; unlocking starts them again.
        mark = std::filesystem::file_size(log, ec);
        PostMessageW(island, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
        Sleep(150);
        PostMessageW(island, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        Sleep(400);
        std::string lock = ReadFile(log, mark);
        size_t off = lock.find("timers off"), on = lock.find("timers on");
        CHECK(off != std::string::npos && on != std::string::npos && off < on, "lock then unlock should log timers off, then on: '%s'", lock.c_str());
        CHECK(ReadFile(real, realBefore).find(Fmt("start pid %lu", pi.dwProcessId)) == std::string::npos, "the real island.log was not written");
        PostMessageW(island, WM_CLOSE, 0, 0);
    }
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// Item 9 end to end: the island logs "sound <event>" when an event's switch is on and nothing when it is off. Here the download
// switch is turned on (default off) and the screenshot switch off (default on); focus keeps its default (on). The sounds are
// not played (STATLET_SOUND_SILENT), only logged.
TEST_CASE("a test island plays the sounds whose switches are on (item 9)") {
    constexpr WPARAM kSave = 5;  // main.cpp's MenuSaveShot
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    auto dir = std::filesystem::path(self).parent_path();
    std::wstring data = (dir / L"instance-sound").wstring(), dl = (dir / L"instance-sound-downloads").wstring(), shots = (dir / L"instance-sound-shots").wstring(), log = data + L"\\island.log";
    std::error_code ec;
    std::filesystem::remove_all(dl, ec);
    std::filesystem::remove_all(shots, ec);
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(dl);
    if (FILE* f = _wfopen((data + L"\\settings.json").c_str(), L"wb")) {
        fputs(R"({"soundScreenshot":false,"soundDownloadDone":true})", f);
        fclose(f);
    }
    uintmax_t before = std::filesystem::file_size(log, ec);
    if (ec) before = 0;
    SetEnvironmentVariableW(L"STATLET_INSTANCE", L"testsound");
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", data.c_str());
    SetEnvironmentVariableW(L"STATLET_DOWNLOADS_DIR", dl.c_str());
    SetEnvironmentVariableW(L"STATLET_SCREENSHOT_DIR", shots.c_str());
    SetEnvironmentVariableW(L"STATLET_SOUND_SILENT", L"1");
    SetEnvironmentVariableW(L"STATLET_FOCUS_SECONDS", L"2");
    SetEnvironmentVariableW(L"STATLET_DEMO", L"focus");  // starts a Focus timer 4 s in; it ends 2 s later
    SetEnvironmentVariableW(L"STATLET_TOP_OFFSET", L"300");
    std::wstring cmd = L"\"" + (dir / L"statlet.exe").wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (auto name : {L"STATLET_INSTANCE", L"STATLET_DATA_DIR", L"STATLET_DOWNLOADS_DIR", L"STATLET_SCREENSHOT_DIR", L"STATLET_SOUND_SILENT", L"STATLET_FOCUS_SECONDS", L"STATLET_DEMO", L"STATLET_TOP_OFFSET"})
        SetEnvironmentVariableW(name, nullptr);
    REQUIRE_MESSAGE(started, "could not start statlet.exe beside the tests");
    HWND island = nullptr;
    for (int i = 0; i < 50 && !island; i++) Sleep(100), island = IslandOf(pi.dwProcessId);
    REQUIRE_MESSAGE(island, "the test island did not start");
    auto waitFor = [&](const char* line, int ms) {
        for (int i = 0; i < ms / 50; i++) {
            if (ReadFile(log, before).find(line) != std::string::npos) return true;
            Sleep(50);
        }
        return false;
    };
    CHECK(waitFor("downloads watching", 6000), "the downloads watcher did not start");
    PostMessageW(island, WM_APP + 8, kSave, 0);
    CHECK(waitFor("screenshot saved", 4000), "the screenshot did not save");
    if (FILE* f = _wfopen((dl + L"\\a.zip.crdownload").c_str(), L"ab")) {
        fputs("data", f);
        fclose(f);
    }
    CHECK(waitFor("downloads active", 3000), "the partial file did not start a download");
    Sleep(300);
    std::filesystem::rename(dl + L"\\a.zip.crdownload", dl + L"\\a.zip", ec);
    CHECK(waitFor("sound download", 4000), "the download-done switch is on but no 'sound download' was logged");
    CHECK(waitFor("sound focus", 9000), "the focus switch is on by default but no 'sound focus' was logged");
    CHECK(ReadFile(log, before).find("sound screenshot") == std::string::npos, "the screenshot switch is off but 'sound screenshot' was logged");
    CHECK(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT, "the island exited");
    PostMessageW(island, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::filesystem::remove_all(dl, ec);
    std::filesystem::remove_all(shots, ec);
}

// Item 8 end to end: the island's two screenshot actions (posted as the tray menu items would) on the monitor under the
// cursor. Save: a PNG in the override folder, decoding at the monitor's pixel size. Copy: a CF_DIB of the same size on the
// clipboard (this replaces what is on the clipboard while the test runs). The island is excluded from the picture by display
// affinity; that is checked by eye (QA), not here.
TEST_CASE("a test island saves and copies a screenshot of the monitor under the cursor (item 8)") {
    constexpr WPARAM kCopy = 4, kSave = 5;  // main.cpp's MenuCopyShot and MenuSaveShot
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    auto dir = std::filesystem::path(self).parent_path();
    std::wstring data = (dir / L"instance-shot").wstring(), out = (dir / L"instance-shot-out").wstring(), log = data + L"\\island.log";
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
    std::filesystem::create_directories(data);
    uintmax_t before = std::filesystem::file_size(log, ec);
    if (ec) before = 0;
    SetEnvironmentVariableW(L"STATLET_INSTANCE", L"testshot");
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", data.c_str());
    SetEnvironmentVariableW(L"STATLET_SCREENSHOT_DIR", out.c_str());
    SetEnvironmentVariableW(L"STATLET_TOP_OFFSET", L"300");
    SetEnvironmentVariableW(L"STATLET_SOUND_SILENT", L"1");
    std::wstring cmd = L"\"" + (dir / L"statlet.exe").wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (auto name : {L"STATLET_INSTANCE", L"STATLET_DATA_DIR", L"STATLET_SCREENSHOT_DIR", L"STATLET_TOP_OFFSET", L"STATLET_SOUND_SILENT"}) SetEnvironmentVariableW(name, nullptr);
    REQUIRE_MESSAGE(started, "could not start statlet.exe beside the tests");
    HWND island = nullptr;
    for (int i = 0; i < 50 && !island; i++) Sleep(100), island = IslandOf(pi.dwProcessId);
    REQUIRE_MESSAGE(island, "the test island did not start");
    auto waitFor = [&](const char* line, int ms) {
        for (int i = 0; i < ms / 50; i++) {
            if (ReadFile(log, before).find(line) != std::string::npos) return true;
            Sleep(50);
        }
        return false;
    };
    for (int i = 0; i < 100 && ReadFile(log, before).find("timers on") == std::string::npos; i++) Sleep(50);
    POINT at;
    GetCursorPos(&at);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &mi);
    LONG mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;

    PostMessageW(island, WM_APP + 8, kSave, 0);  // WM_APP_SHOT: the 8th message after WM_APP (see main.cpp's enum)
    bool saved = waitFor("screenshot saved", 4000);
    CHECK(saved, "no 'screenshot saved' line: %s", ReadFile(log, before).c_str());
    std::wstring png;
    for (auto& e : std::filesystem::directory_iterator(out, ec)) png = e.path().wstring();
    CHECK(!png.empty(), "no PNG in %ls", out.c_str());
    if (!png.empty()) {
        auto name = std::filesystem::path(png).filename().wstring();
        CHECK((name.rfind(L"Statlet ", 0) == 0 && name.size() == 8 + 19 + 4), "file name format: %ls", name.c_str());
        auto wic = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        if (SUCCEEDED(wic->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) && SUCCEEDED(decoder->GetFrame(0, frame.put()))) frame->GetSize(&w, &h);
        CHECK((LONG(w) == mw && LONG(h) == mh), "PNG is %ux%u, the monitor is %ldx%ld", w, h, mw, mh);
    }

    PostMessageW(island, WM_APP + 8, kCopy, 0);
    CHECK(waitFor("screenshot copied", 4000), "no 'screenshot copied' line");
    bool clip = false;
    for (int i = 0; i < 10 && !clip; i++) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE g = GetClipboardData(CF_DIB)) {
                auto* head = static_cast<BITMAPINFOHEADER*>(GlobalLock(g));
                clip = head && head->biWidth == mw && head->biHeight == mh;
                GlobalUnlock(g);
            }
            CloseClipboard();
        }
        if (!clip) Sleep(50);
    }
    CHECK(clip, "the clipboard has no CF_DIB of %ldx%ld", mw, mh);
    CHECK(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT, "the island exited");
    PostMessageW(island, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::filesystem::remove_all(out, ec);
}

// Item 6 end to end: the Bluetooth watcher starts beside the real one without trouble, and the demo hook (a made-up connect)
// reaches the status slot. The real connect needs headphones; this is the part a machine can check.
TEST_CASE("a test island starts the Bluetooth watcher and peeks the icon on a connect (item 6)") {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    auto dir = std::filesystem::path(self).parent_path();
    std::wstring data = (dir / L"instance-bt").wstring(), log = data + L"\\island.log";
    std::error_code ec;
    std::filesystem::create_directories(data);
    uintmax_t before = std::filesystem::file_size(log, ec);
    if (ec) before = 0;
    SetEnvironmentVariableW(L"STATLET_INSTANCE", L"testbt");
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", data.c_str());
    SetEnvironmentVariableW(L"STATLET_DEMO", L"bt");
    SetEnvironmentVariableW(L"STATLET_TOP_OFFSET", L"300");
    std::wstring cmd = L"\"" + (dir / L"statlet.exe").wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (auto name : {L"STATLET_INSTANCE", L"STATLET_DATA_DIR", L"STATLET_DEMO", L"STATLET_TOP_OFFSET"}) SetEnvironmentVariableW(name, nullptr);
    REQUIRE_MESSAGE(started, "could not start statlet.exe beside the tests");
    auto waitFor = [&](const char* line, int ms) {
        for (int i = 0; i < ms / 50; i++) {
            if (ReadFile(log, before).find(line) != std::string::npos) return true;
            Sleep(50);
        }
        return false;
    };
    CHECK(waitFor("bluetooth watching", 6000), "the Bluetooth watcher did not start");
    CHECK(waitFor("bluetooth connected", 8000), "the demo connect did not reach the island");
    Sleep(400);
    CHECK(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT, "the island exited");
    PostMessageW(IslandOf(pi.dwProcessId), WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// Item 7 end to end: a test island watches a temp folder (STATLET_DOWNLOADS_DIR); a growing x.zip.crdownload turns the icon
// on, renaming it to x.zip shows done, then idle. The log lines are the island's own "downloads ..." records.
TEST_CASE("a test island sees a download in its Downloads folder (item 7)") {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    auto dir = std::filesystem::path(self).parent_path();
    std::wstring data = (dir / L"instance-dl").wstring(), dl = (dir / L"instance-dl-downloads").wstring(), log = data + L"\\island.log";
    std::error_code ec;
    std::filesystem::remove_all(dl, ec);
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(dl);
    uintmax_t before = std::filesystem::file_size(log, ec);
    if (ec) before = 0;
    SetEnvironmentVariableW(L"STATLET_INSTANCE", L"testdl");
    SetEnvironmentVariableW(L"STATLET_DATA_DIR", data.c_str());
    SetEnvironmentVariableW(L"STATLET_DOWNLOADS_DIR", dl.c_str());
    SetEnvironmentVariableW(L"STATLET_TOP_OFFSET", L"300");
    std::wstring cmd = L"\"" + (dir / L"statlet.exe").wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (auto name : {L"STATLET_INSTANCE", L"STATLET_DATA_DIR", L"STATLET_DOWNLOADS_DIR", L"STATLET_TOP_OFFSET"}) SetEnvironmentVariableW(name, nullptr);
    REQUIRE_MESSAGE(started, "could not start statlet.exe beside the tests");
    auto waitFor = [&](const char* line, int ms) {
        for (int i = 0; i < ms / 50; i++) {
            if (ReadFile(log, before).find(line) != std::string::npos) return true;
            Sleep(50);
        }
        return false;
    };
    auto write = [&](const wchar_t* name, size_t bytes) {
        if (FILE* f = _wfopen((dl + L"\\" + name).c_str(), L"ab")) {
            std::string chunk(bytes, 'x');
            fwrite(chunk.data(), 1, chunk.size(), f);
            fclose(f);
        }
    };
    bool watching = waitFor("downloads watching", 6000);
    CHECK(watching, "the island never started watching '%ls'", dl.c_str());
    if (watching) {
        write(L"x.zip.crdownload", 100);
        CHECK(waitFor("downloads active", 3000), "a new partial file did not show the download icon");
        Sleep(1100);
        write(L"x.zip.crdownload", 100);  // it grows
        Sleep(1100);
        std::filesystem::rename(dl + L"\\x.zip.crdownload", dl + L"\\x.zip", ec);
        CHECK(waitFor("downloads done", 3000), "renaming the partial to x.zip did not show done");
        CHECK(waitFor("downloads idle", 5000), "done did not hand the slot back after 2 s");
    }
    PostMessageW(IslandOf(pi.dwProcessId), WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::filesystem::remove_all(dl, ec);
}

TEST_SUITE_END();

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto queue = CreateDispatcherQueue();
    scale = GetDpiForSystem() / 96.f;

    // Our own flags: --e2e runs the "e2e" suite (it takes over the mouse), --ci skips what needs a desktop (render, screen, e2e).
    // Everything else goes to doctest, e.g. --test-suite=unit or --list-test-cases.
    bool e2e = false, ci = false;
    std::vector<std::string> args{"statlet-tests"};
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--e2e") e2e = true;
        else if (a == L"--ci") ci = true;
        else {
            std::string narrow;
            for (wchar_t c : a) narrow += char(c);  // doctest flags are ASCII
            args.push_back(narrow);
        }
    }
    std::vector<const char*> argvNarrow;
    for (auto& a : args) argvNarrow.push_back(a.c_str());
    doctest::Context context(int(argvNarrow.size()), argvNarrow.data());
    std::string exclude = e2e ? "" : "e2e";
    if (ci) exclude += (exclude.empty() ? "" : ",") + std::string("render,screen,e2e,instance");
    if (!exclude.empty()) context.addFilter("test-suite-exclude", exclude.c_str());
    int rc = context.run();
    if (!rc) printf("all passed\n");
    return rc;
}
