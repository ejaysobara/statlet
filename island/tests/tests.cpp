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
#include <cstdarg>
#include <string>
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
    Timers seen = TimersFor(false, false, false, false), slid = TimersFor(true, false, false, false), hidden = TimersFor(true, true, false, false);
    CHECK(seen.hover && seen.rest, "on screen: every timer runs");
    CHECK(!slid.hover && slid.rest, "slid away: no cursor poll, the clock keeps the accessible name current");
    CHECK(!hidden.hover && !hidden.rest, "hidden: nothing runs");
    CHECK(!TimersFor(false, false, true, false).rest && !TimersFor(false, false, false, true).rest, "display off or locked: nothing runs");
    CHECK(SessionReason(WTS_SESSION_UNLOCK) &&SessionReason(WTS_CONSOLE_CONNECT) && SessionReason(WTS_REMOTE_CONNECT), "unlock and connects revalidate");
    CHECK(!SessionReason(WTS_SESSION_LOCK) && !SessionReason(WTS_CONSOLE_DISCONNECT), "lock and disconnect do not");
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
