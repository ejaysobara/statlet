// Statlet island: an always-on-top window at the top center of the primary display that never takes focus.
// It springs open while hovered or after a click (click again to let it close).
//
// The window keeps one fixed size; the island's shape is a composition clip, so moving it costs no window work.
// The compositor runs the spring and the content fades, so the app is asleep while the island animates. A cursor
// poll keeps a model of the same spring for hit testing, and makes the window click-through outside the shape.
//
// Benchmark mode, for bench/run-bench.ps1 (env vars):
//   ISLAND_BENCH_OUT=<file>   write results here
//   ISLAND_BENCH=startup      write the time the first frame was composed, then exit
//   STATLET_TOP_OFFSET=<epx> override the gap above the island
//   ISLAND_BENCH=cycle        toggle open every ISLAND_BENCH_TOGGLE_MS (default 1000) for ISLAND_BENCH_SECONDS (default 20)
//
// Outside benchmarks the island reads %LOCALAPPDATA%\Statlet\settings.json (settings.h), watches its folder and
// rebuilds its visuals when the file changes, so the settings app's changes show at once.
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <winrt/Windows.UI.ViewManagement.h>
#include "events.h"
#include "gfx.h"
#include "log.h"
#include "bluetooth.h"
#include "capture.h"
#include "clock.h"
#include "downloads.h"
#include "media.h"
#include "placement.h"
#include "power.h"
#include "settings.h"
#include "widgets.h"
#include "shape_math.h"
#include "stats.h"
#include "takeover.h"
#include "tray_masks.h"

using namespace std::chrono_literals;
using winrt::Windows::UI::ViewManagement::UIColorType;
using winrt::Windows::UI::ViewManagement::UISettings;

enum : UINT { WM_APP_SETTINGS = WM_APP + 1, WM_APP_TRAY, WM_APP_VOLUME, WM_APP_NET, WM_APP_MEDIA, WM_APP_HOLD, WM_APP_BT, WM_APP_SHOT };
enum : UINT_PTR { TimerHover = 1, TimerClock, TimerStats, TimerBenchToggle, TimerBenchEnd, TimerTakeover, TimerNetDebounce, TimerFocusEnd, TimerDemo, TimerPeek, TimerMediaHide, TimerMediaClock, TimerReload, TimerRevalidate, TimerAwayReturn, TimerAwayRecheck, TimerDownload, TimerStatusPeek, TimerShot };
enum : UINT { MenuQuit = 1, MenuFocus, MenuSettings, MenuCopyShot, MenuSaveShot };

static Tokens t;
static HWND hwnd;
static UINT taskbarCreated;
static float scale;
// The window fits the largest open size, the spring's ~1% overshoot and the shadow (it may reach into the top gap).
static float hostW, hostH;

static wuc::Compositor compositor{nullptr};
static wuc::ContainerVisual root{nullptr}, collapsedFace{nullptr}, expandedFace{nullptr};
static wuc::CompositionPropertySet motion{nullptr};  // W, H: the sprung size in epx; S: the DPI scale
// The shape's X, Y, W, H, R, derived from motion. Held here: animations on a property set stop once the app releases it.
static wuc::CompositionPropertySet props{nullptr};
static std::unique_ptr<Gfx> gfx;
static std::unique_ptr<RoundedShape> shape;
static std::unique_ptr<Acrylic> acrylic;
static std::unique_ptr<Text> clockText, focusText;  // the rest line's date and time; "Focus 25m" in accent
static std::unique_ptr<Text> measureText;           // scratch text in the clock's style, for the day-fitted pill
// The status slot (v2 items 6-8): one Fluent glyph before the rest line's content. A persistent glyph (download running) and a
// peek (Bluetooth, download done, screenshot) share it; a peek wins until it ends, then the persistent glyph returns.
static std::unique_ptr<Text> statusIcon;
static wchar_t statusPersistent, statusPeek, slotShown;  // 0: none; slotShown is what the slot draws now
static wchar_t StatusGlyph() { return statusPeek ? statusPeek : statusPersistent; }

// Event sounds (item 9): Windows' own sounds through the user's sound scheme and volume, so "No sounds" in Windows silences
// them and no sound files ship. One switch each in the settings; Bluetooth has none (Windows plays its own).
// STATLET_SOUND_SILENT=1 logs the sound without playing it (the tests set it).
enum class Event { DownloadDone, Screenshot, FocusDone };
static void PlayEventSound(Event e) {
    bool on = e == Event::Screenshot ? t.soundScreenshot : e == Event::DownloadDone ? t.soundDownloadDone : t.soundFocusDone;
    if (!on) return;
    Log("sound %s", e == Event::Screenshot ? "screenshot" : e == Event::DownloadDone ? "download" : "focus");
    if (getenv("STATLET_SOUND_SILENT")) return;
    PlaySoundW(e == Event::FocusDone ? L"Notification.Reminder" : L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}
static std::wstring widestTime;                     // the widest time at that style (see MeasureWidestTime)
static float dayExtra = 0;                          // how much wider the day's widest line is than the current one

// The widest time the clock can show. Measured when the island is built (text scale and DPI come with a rebuild), not every minute.
static void MeasureWidestTime() {
    measureText = std::make_unique<Text>(*gfx, TextStyle{t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    float best = -1;
    for (auto& s : WidestTimeSamples()) {
        measureText->Set(s);
        if (measureText->Width() > best) best = measureText->Width(), widestTime = s;
    }
}
static std::unique_ptr<Bar> cpuBar, ramBar;         // the rest line's mini bars
static wuc::ContainerVisual statsFace{nullptr}, cardFace{nullptr};  // the open faces: hovered usage, takeover card
struct StatRow {  // one labeled usage row of the hovered face
    std::unique_ptr<Text> label, mark, value;
    std::unique_ptr<Bar> bar;
};
static StatRow cpuRow, ramRow, mediaCpu, mediaRam;  // the hovered rows; the media card's compact pair
static std::unique_ptr<Text> icon, title, body, detail, cardValue;  // takeover card
static std::unique_ptr<Bar> bar;
static wuc::ContainerVisual restRow{nullptr};               // date/time text plus bars, centered as one group
static Rgba accent = t.accentFallback;
static CpuSampler cpuSampler;
static Sustain sustain(t.sustainAbove, t.sustainSeconds);
static float cpuPct = -1, ramPct = -1;  // whole percents; -1 until the first sample
static bool alert;                      // usage stayed above the sustain threshold: the island opens by itself

// Takeovers: a card (volume, battery, connection, Focus) opens the island for its holdMs, then it returns to the rest line.
// A newer card replaces the one showing. The open face is a card or the idle content (showingCard).
static bool takeover, showingCard;
static BatteryWatch battery(t.batteryLowAt, t.batteryCritAt);
static NetWatch netWatch;
static FocusTimer focus;
static double focusSeconds = t.focusMinutes * 60.0;
static int lastVolume = -1;
static bool lastMuted;

static Spring springW(t.collapsedMinW,t.springStiffness, t.springDamping), springH(t.collapsedH, t.springStiffness, t.springDamping);
static double modelTime;
static bool hovering, pinned, open, clickThrough;
static int held;  // a view the settings app holds open while its Appearance page shows: 0 none, 1 hover, 2 card, 3 music

// Now playing (media.h). Music shows on the island while a player plays, and for mediaHideMs after it pauses. A new track
// peeks for mediaPeekMs; afterwards the rest line follows Tokens::afterPeek. Hovering opens the media card.
static MediaInfo media;
static double mediaReadAt;  // Now() when `media` was read
static bool mediaShown, peeking;
static MediaRow shownRow = MediaRow::Rest;
static std::wstring trackKey, peekedKey;  // title and artist of the track showing, and of the last one peeked
static int artRequest;         // drops art that arrives after a newer track
static std::unique_ptr<Picture> artSmall, artLarge;
static std::unique_ptr<Text> peekTitle, peekArtist, mediaTitle, mediaArtist, elapsedText, remainingText;
static wuc::ContainerVisual peekRow{nullptr}, equalizer{nullptr}, mediaFace{nullptr};
static std::vector<wuc::SpriteVisual> eqBars;
static wuc::CompositionColorBrush mediaAccent{nullptr};  // equalizer, progress fill, stop dot, play button
static wuc::CompositionPropertySet mediaProps{nullptr};  // P: track progress 0..1, keyframed to the end while the card shows
static wuc::SpriteVisual waveSprite{nullptr};
static wuc::CompositionDrawingSurface waveSurface{nullptr};
static wuc::ShapeVisual flatFill{nullptr};
static wuc::ShapeVisual knobVisual{nullptr};                 // the playhead dot; null when Tokens::knob is Never
static wuc::CompositionColorGradientStop gradientStart{nullptr}, gradientEnd{nullptr};  // Gradient style: the played end, tracks the media colour
static Rgba artColor{};                                      // dominant colour of the current track's art
static bool haveArt, knobHot;
static bool eqRunning, driftRunning, waveShowing = true;
struct TransportButton {
    std::unique_ptr<Text> glyph;
    float cx;  // center x within the media face
};
static TransportButton transport[3];  // previous, play/pause, next
static UISettings uiSettings{nullptr};

static double Now() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / f.QuadPart;
}

static wuc::ExpressionAnimation Expr(const wchar_t* expression) {
    auto animation = compositor.CreateExpressionAnimation(expression);
    animation.SetReferenceParameter(L"m", motion);
    animation.SetScalarParameter(L"hw", hostW);
    return animation;
}

// Builds the visual tree: shape props derived from the sprung size, the glass, and two placeholder faces.
static void BuildIsland() {
    root = compositor.CreateContainerVisual();
    root.Size({hostW, hostH});
    motion = compositor.CreatePropertySet();
    motion.InsertScalar(L"W", t.collapsedMinW);
    motion.InsertScalar(L"H", t.collapsedH);
    motion.InsertScalar(L"S", scale);

    props = compositor.CreatePropertySet();
    for (auto name : {L"X", L"W", L"H", L"R"}) props.InsertScalar(name, 0);
    props.InsertScalar(L"Y", t.topOffset);
    props.StartAnimation(L"X", Expr(L"(hw - m.W) / 2"));
    props.StartAnimation(L"W", Expr(L"m.W"));
    props.StartAnimation(L"H", Expr(L"m.H"));
    auto radius = Expr(L"rc + (re - rc) * Clamp((m.H - hc) / (he - hc), 0, 1)");  // IslandRadius() in shape_math.h
    radius.SetScalarParameter(L"rc", t.collapsedR);
    radius.SetScalarParameter(L"re", t.expandedR);
    radius.SetScalarParameter(L"hc", t.collapsedH);
    radius.SetScalarParameter(L"he", t.expandedH);
    props.StartAnimation(L"R", radius);

    shape = std::make_unique<RoundedShape>(*gfx, props, t, t.expandedR);
    acrylic = std::make_unique<Acrylic>(*gfx, t);
    shape->content.Children().InsertAtTop(acrylic->root);
    root.Children().InsertAtTop(shape->root);

    // Collapsed: the idle clock, centered. Offsets are rounded to whole physical pixels so text stays crisp at rest.
    clockText = std::make_unique<Text>(*gfx, TextStyle{t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    MeasureWidestTime();
    focusText = std::make_unique<Text>(*gfx, TextStyle{t.typeClock, DWRITE_FONT_WEIGHT_SEMI_BOLD, accent, true});
    cpuBar = std::make_unique<Bar>(*gfx, t.restBarW, t.restBarH, t.track, accent);
    ramBar = std::make_unique<Bar>(*gfx, t.restBarW, t.restBarH, t.track, accent);
    statusIcon = MakeIcon(*gfx, t.glyphDownload, t.statusGlyph, accent);
    statusIcon->visual.IsVisible(false);
    slotShown = 0;  // the rebuild starts with an empty slot; LayoutCollapsed shows whatever is current
    restRow = compositor.CreateContainerVisual();
    for (auto v : {focusText->visual.as<wuc::Visual>(), clockText->visual.as<wuc::Visual>(), cpuBar->visual.as<wuc::Visual>(), ramBar->visual.as<wuc::Visual>(),
                   statusIcon->visual.as<wuc::Visual>()})
        restRow.Children().InsertAtTop(v);
    collapsedFace = compositor.CreateContainerVisual();
    collapsedFace.Children().InsertAtTop(restRow);
    auto centered = Expr(L"Vector3(Round((hw - c.Size.X) / 2 * m.S) / m.S, Round((y + (m.H - c.Size.Y) / 2) * m.S) / m.S, 0)");
    centered.SetReferenceParameter(L"c", restRow);
    centered.SetScalarParameter(L"y", t.topOffset);
    collapsedFace.StartAnimation(L"Offset", centered);

    // Open faces share one container (faded as a unit, placed at the open shape's padding); one shows at a time.
    expandedFace = compositor.CreateContainerVisual();

    // Hovered: CPU over RAM as labeled rows, in the same order as the rest line's bars, at the stats padding.
    statsFace = compositor.CreateContainerVisual();
    statsFace.Offset({t.statsPad - t.padX, t.statsPad - t.padY, 0});
    float rowY = 0;
    // Columns are measured, not guessed: [mark][label][bar][value]. The mark column is always reserved so the label never
    // moves (or runs into the bar) when a warning mark appears; the bar takes what the widest label and "100%" leave.
    for (auto [row, name] : {std::pair{&cpuRow, L"CPU"}, std::pair{&ramRow, L"RAM"}}) {
        row->label = std::make_unique<Text>(*gfx, TextStyle{t.typeCaption, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink2});
        row->label->Set(name);
        row->mark = MakeIcon(*gfx, 0xE7BA, 10, t.warn);  // Warning: high usage reads without relying on color
        row->value = std::make_unique<Text>(*gfx, TextStyle{t.typeLabel, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink, true});
        row->value->Set(L"100%");
    }
    float barX = cpuRow.mark->Width() + 3 + std::max(cpuRow.label->Width(), ramRow.label->Width()) + 8;
    float barW = t.statsW - 2 * t.statsPad - barX - 8 - cpuRow.value->Width();
    for (auto [row, name] : {std::pair{&cpuRow, L"CPU"}, std::pair{&ramRow, L"RAM"}}) {
        row->bar = std::make_unique<Bar>(*gfx, barW, t.statsBarH, t.track, accent);
        float barY = std::round((t.statsRowH - t.statsBarH) / 2);
        row->bar->visual.Offset({barX, barY, 0});
        auto notch = compositor.CreateSpriteVisual();  // marks warnAt on the track
        notch.Brush(compositor.CreateColorBrush(ToColor(t.ink2)));
        notch.Size({1, t.statsBarH + 4});
        notch.Offset({barX + std::round(barW * t.warnAt / 100), barY - 2, 0});
        auto v = compositor.CreateContainerVisual();
        v.Offset({0, rowY, 0});
        for (auto child : {row->label->visual.as<wuc::Visual>(), row->mark->visual.as<wuc::Visual>(), row->bar->visual.as<wuc::Visual>(),
                           notch.as<wuc::Visual>(), row->value->visual.as<wuc::Visual>()})
            v.Children().InsertAtTop(child);
        statsFace.Children().InsertAtTop(v);
        rowY += t.statsRowH + t.statsRowGap;
    }
    expandedFace.Children().InsertAtTop(statsFace);

    // Takeover card: icon and title; the detail beside the title when there is a bar (the bar and its value sit below),
    // else under it.
    float innerW = t.expandedW - 2 * t.padX;
    icon = MakeIcon(*gfx, 0xE767, t.typeTitle, accent);
    title = std::make_unique<Text>(*gfx, TextStyle{t.typeTitle, DWRITE_FONT_WEIGHT_BOLD, t.ink}, innerW - 26);
    body = std::make_unique<Text>(*gfx, TextStyle{t.typeBody, DWRITE_FONT_WEIGHT_NORMAL, t.ink2}, innerW);
    detail = std::make_unique<Text>(*gfx, TextStyle{t.typeBody, DWRITE_FONT_WEIGHT_NORMAL, t.ink2}, 180.f);
    cardValue = std::make_unique<Text>(*gfx, TextStyle{t.typeBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink, true});
    bar = std::make_unique<Bar>(*gfx, innerW - 52, 6.f, t.track, accent);
    title->Set(L"Volume");
    icon->visual.Offset({0, std::round((title->Height() - icon->Height()) / 2), 0});
    title->visual.Offset({26, 0, 0});
    body->visual.Offset({0, title->Height() + 4, 0});
    cardFace = compositor.CreateContainerVisual();
    for (auto v : {icon->visual.as<wuc::Visual>(), title->visual.as<wuc::Visual>(), body->visual.as<wuc::Visual>(), detail->visual.as<wuc::Visual>(),
                   bar->visual.as<wuc::Visual>(), cardValue->visual.as<wuc::Visual>()})
        cardFace.Children().InsertAtTop(v);
    cardFace.IsVisible(false);
    expandedFace.Children().InsertAtTop(cardFace);
    auto topLeft = Expr(L"Vector3(Round(((hw - m.W) / 2 + px) * m.S) / m.S, Round((y + py) * m.S) / m.S, 0)");
    topLeft.SetScalarParameter(L"px", t.padX);
    topLeft.SetScalarParameter(L"py", t.padY);
    topLeft.SetScalarParameter(L"y", t.topOffset);
    expandedFace.StartAnimation(L"Offset", topLeft);
    expandedFace.Opacity(0);

    shape->content.Children().InsertAtTop(collapsedFace);
    shape->content.Children().InsertAtTop(expandedFace);
}

static void Retarget();

static void Fade(wuc::Visual const& v, bool in);
static bool AnimationsEnabled();
static void DrawWave();

static float EqWidth() { return 4 * t.eqBarW + 3 * t.eqGap; }
static float SnapPx(float v) { return std::round(v * scale) / scale; }  // whole physical pixels keep text crisp

static void MoveTo(wuc::Visual const& v, wuc::ContainerVisual const& parent) {
    if (auto p = v.Parent()) {
        if (p == parent) return;
        p.Children().Remove(v);
    }
    parent.Children().InsertAtTop(v);
}

// Puts `g` in the status slot (0 empties it). A download breathes, a peek fades in over 150 ms, and with Windows animations
// off both are steady. A peek's fade-out is EndStatusPeek's: it keeps the slot (and its width) until the glyph is gone.
static void AnimateSlot(wchar_t g) {
    auto v = statusIcon->visual;
    v.StopAnimation(L"Opacity");
    v.IsVisible(g != 0);
    v.Opacity(1);
    if (g) statusIcon->Set({&g, 1});
    if (!g || !AnimationsEnabled()) return;
    auto a = compositor.CreateScalarKeyFrameAnimation();
    if (g == t.glyphDownload) {
        auto ease = compositor.CreateCubicBezierEasingFunction({0.37f, 0}, {0.63f, 1});
        a.InsertKeyFrame(0, 0.55f);  // starts dim: after a peek's fade-out the icon rises instead of popping to full
        a.InsertKeyFrame(0.5f, 1, ease);
        a.InsertKeyFrame(1, 0.55f, ease);
        a.Duration(std::chrono::milliseconds(t.breatheMs));
        a.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
    } else {
        a.InsertKeyFrame(0, 0);
        a.InsertKeyFrame(1, 1);
        a.Duration(std::chrono::milliseconds(150));
    }
    v.StartAnimation(L"Opacity", a);
}

// The collapsed rest line is the rest row (status slot, Focus label, clock, CPU/RAM bars; plus the art before and the
// equalizer after when tucked) or the music row (art, title, artist, equalizer), cross-faded. The collapsed width fits whichever
// shows. The slot leads the rest row and the Music row (the title gives up its room); the song-start peek has none.
static void LayoutCollapsed() {
    MediaRow row = RowFor(mediaShown, peeking, t.afterPeek);
    bool tucked = row == MediaRow::Tucked, music = row == MediaRow::Peek || row == MediaRow::Music, slotInMusic = row == MediaRow::Music;
    wchar_t slot = row == MediaRow::Peek ? 0 : StatusGlyph();
    if (slot != slotShown) AnimateSlot(slotShown = slot);
    if (slot) MoveTo(statusIcon->visual, slotInMusic ? peekRow : restRow);
    MoveTo(artSmall->visual, music ? peekRow : restRow);
    MoveTo(equalizer, music ? peekRow : restRow);
    artSmall->visual.IsVisible(tucked || music);
    equalizer.IsVisible(tucked || music);

    bool f = focus.Running();
    focusText->visual.IsVisible(f);
    float h = std::max(clockText->Height(), tucked ? t.artSmall : 0.f), x = 0;
    auto mid = [&](float itemH) { return std::round((h - itemH) / 2); };
    float slotX = slot ? t.statusSlotW + t.restGap : 0;
    // The blocks in the user's order (restOrder), each followed by restGap; the last gap is taken back. The width is the sum
    // of the same blocks whatever the order, and the clock block's extra (dayExtra) stays with the clock.
    for (RestItem item : RestSequence(t.restOrder, tucked)) switch (item) {
        case RestItem::Status:
            if (!slot || slotInMusic) break;
            statusIcon->visual.Offset({x + std::round((t.statusSlotW - statusIcon->Width()) / 2), mid(statusIcon->Height()), 0});
            x += slotX;
            break;
        case RestItem::Art:
            artSmall->visual.Offset({x, mid(t.artSmall), 0});
            x += t.artSmall + t.restGap;
            break;
        case RestItem::Clock:
            if (f) {
                focusText->visual.Offset({x, mid(focusText->Height()), 0});
                x += focusText->Width() + t.restGap;
            }
            clockText->visual.Offset({x, mid(clockText->Height()), 0});
            x += clockText->Width() + t.restGap;
            break;
        case RestItem::Usage: {
            float gap = 3, barsY = mid(2 * t.restBarH + gap);
            cpuBar->visual.Offset({x, barsY, 0});
            ramBar->visual.Offset({x, barsY + t.restBarH + gap, 0});
            x += t.restBarW + t.restGap;
            break;
        }
        case RestItem::Equalizer:
            equalizer.Offset({x, mid(t.eqH), 0});
            x += EqWidth() + t.restGap;
            break;
        }
    if (x > 0) x -= t.restGap;
    restRow.Size({x, h});

    // Music row: the title gets the room the width cap leaves; the artist follows (peek only) when a few letters fit.
    float limit = (row == MediaRow::Music ? t.musicMaxW : t.collapsedMaxW) - 2 * t.mediaPadX;
    float artX = slotInMusic ? slotX : 0, textX = artX + t.artSmall + t.restGap, room = limit - textX - t.restGap - EqWidth();
    if (slotInMusic) statusIcon->visual.Offset({std::round((t.statusSlotW - statusIcon->Width()) / 2), std::round((t.artSmall - statusIcon->Height()) / 2), 0});
    peekTitle->SetMaxWidth(room);
    float px = textX + peekTitle->Width(), artistRoom = room - peekTitle->Width() - t.restGap, ph = t.artSmall;
    bool artist = row == MediaRow::Peek && !media.artist.empty() && artistRoom >= 24;
    peekArtist->visual.IsVisible(artist);
    if (music) artSmall->visual.Offset({artX, 0, 0});
    peekTitle->visual.Offset({textX, std::round((ph - peekTitle->Height()) / 2), 0});
    if (artist) {
        peekArtist->SetMaxWidth(artistRoom);
        peekArtist->visual.Offset({px + t.restGap, std::round((ph - peekArtist->Height()) / 2), 0});
        px += t.restGap + peekArtist->Width();
    }
    if (music) equalizer.Offset({px + t.restGap, std::round((ph - t.eqH) / 2), 0});
    peekRow.Size({px + t.restGap + EqWidth(), ph});
    auto rs = restRow.Size(), ps = peekRow.Size();  // collapsedFace centers the rest row; center the music row on it
    peekRow.Offset({SnapPx((rs.x - ps.x) / 2), SnapPx((rs.y - ps.y) / 2), 0});

    bool wasMusic = shownRow == MediaRow::Peek || shownRow == MediaRow::Music;
    shownRow = row;
    if (wasMusic == music) return;
    if (AnimationsEnabled()) {
        Fade(music ? restRow : peekRow, false);
        Fade(music ? peekRow : restRow, true);
    } else {
        restRow.Opacity(music ? 0.f : 1.f);
        peekRow.Opacity(music ? 1.f : 0.f);
    }
}

static float CollapsedWidth() {
    switch (shownRow) {
    case MediaRow::Peek: return FitCollapsedWidth(peekRow.Size().x, t, t.mediaPadX);
    case MediaRow::Music: return std::min(FitCollapsedWidth(peekRow.Size().x, t, t.mediaPadX), t.musicMaxW);
    case MediaRow::Tucked: return FitCollapsedWidth(restRow.Size().x + dayExtra, t, t.mediaPadX);
    default: return FitCollapsedWidth(restRow.Size().x + dayExtra, t);
    }
}

static wchar_t restLine[96];  // the rest line's text, for the accessible name

// The window name is what screen readers announce: the rest line plus both usage figures and their level.
static void SetAccessibleName() {
    auto level = [](float pct) { auto l = LevelFor(pct, t); return l == Level::Critical ? L" critical" : l == Level::Warn ? L" high" : L""; };
    wchar_t name[200];
    if (cpuPct < 0) swprintf(name, 200, L"Statlet, %s", restLine);
    else swprintf(name, 200, L"Statlet, %s, CPU %d%%%s, memory %d%%%s", restLine, int(cpuPct), level(cpuPct), int(ramPct), level(ramPct));
    std::wstring full = name;
    if (mediaShown) full += (media.playing ? L", now playing " : L", paused ") + media.title + (media.artist.empty() ? L"" : L" by " + media.artist);
    wchar_t g = StatusGlyph();  // each status has its own shape and its own words
    if (g == t.glyphDownload) full += L", downloading";
    else if (g == t.glyphDone) full += L", download finished";
    else if (g == t.glyphBluetooth) full += L", Bluetooth device connected";
    else if (g == t.glyphCamera) full += L", screenshot taken";
    SetWindowTextW(hwnd, full.c_str());
}

static void UpdateTimers();

static bool ClockRunning();

static void UpdateClock() {
    wchar_t text[64];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, text, 64);
    std::wstring date = RestDate(nullptr, t.showWeekday);
    if (t.showWeekday && !focus.Running() && RowFor(mediaShown, peeking, t.afterPeek) == MediaRow::Tucked) {
        // Music (with the status slot when it shows) and the widest date together must stay under the width cap; if they would
        // not, the date shrinks to the weekday for this row instead of clipping.
        measureText->Set(date + L"  " + widestTime);
        if (TuckedRowTooWide(t, StatusGlyph() != 0, measureText->Width())) date = ShortDay(nullptr);
    }
    wchar_t line[96];
    if (focus.Running()) {  // the date steps aside for the countdown
        wchar_t label[32];
        swprintf(label, 32, L"Focus %dm", focus.MinutesLeft(Now()));
        focusText->Set(label);
        clockText->Set(text);
        swprintf(line, 96, L"%s  %s", label, text);
    } else if (RowFor(mediaShown, peeking, t.afterPeek) == MediaRow::Tucked && !t.showWeekday) {  // the art and equalizer take the date's room
        swprintf(line, 96, L"%s", text);
        clockText->Set(line);
    } else {
        swprintf(line, 96, L"%s  %s", date.c_str(), text);
        clockText->Set(line);
    }
    // Day-fitted pill: sized to the day's widest time, so the width changes at midnight and never during the day.
    dayExtra = 0;
    if (t.showWeekday && !focus.Running()) {
        measureText->Set(date + L"  " + widestTime);
        dayExtra = std::max(0.f, measureText->Width() - clockText->Width());
    }
    wcscpy_s(restLine, line);
    LayoutCollapsed();
    Retarget();  // the row's width may have changed
    SetAccessibleName();
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (ClockRunning()) SetTimer(hwnd, TimerClock, (60 - now.wSecond) * 1000 - now.wMilliseconds + 20, nullptr);
}

// One hovered row: past warnAt a warning mark leads the label, so the level reads without color; critical also colors the value.
static void ShowRow(StatRow& r, float pct) {
    Level l = LevelFor(pct, t);
    Rgba c = LevelColor(l, accent, t);
    r.bar->SetProgress(pct / 100);
    r.bar->SetFill(c);
    float mid = t.statsRowH / 2;
    r.mark->visual.IsVisible(l != Level::Normal);
    r.mark->SetColor(c);
    r.mark->visual.Offset({0, std::round(mid - r.mark->Height() / 2), 0});
    r.label->visual.Offset({r.mark->Width() + 3, std::round(mid - r.label->Height() / 2), 0});
    wchar_t v[8];
    swprintf(v, 8, L"%d%%", int(pct));
    r.value->Set(v);
    r.value->SetColor(l == Level::Critical ? t.critical : t.ink);
    r.value->visual.Offset({t.statsW - 2 * t.statsPad - r.value->Width(), std::round(mid - r.value->Height() / 2), 0});
}

// One compact media-card row, centered on its row's y: the label at 0, the bar, the value ending at mediaUsageW.
static void ShowMiniRow(StatRow& r, float pct) {
    Level l = LevelFor(pct, t);
    r.bar->SetProgress(pct / 100);
    r.bar->SetFill(LevelColor(l, accent, t));
    wchar_t v[8];
    swprintf(v, 8, L"%d%%", int(pct));
    r.value->Set(v);
    r.value->SetColor(l == Level::Critical ? t.critical : t.ink);
    r.value->visual.Offset({t.mediaUsageW - r.value->Width(), std::round(-r.value->Height() / 2), 0});
}

// Shows the current usage on every widget that has it. Bars only animate through compositor properties; text redraws only on change.
static void ShowStats() {
    auto color = [](float pct) { return LevelColor(LevelFor(pct, t), accent, t); };
    cpuBar->SetProgress(cpuPct / 100);
    cpuBar->SetFill(color(cpuPct));
    ramBar->SetProgress(ramPct / 100);
    ramBar->SetFill(color(ramPct));
    ShowRow(cpuRow, cpuPct);
    ShowRow(ramRow, ramPct);
    ShowMiniRow(mediaCpu, cpuPct);
    ShowMiniRow(mediaRam, ramPct);
    SetAccessibleName();
}

// Runs every statsPollMs. Redraws only when a whole percent changed; the sustain clock advances either way.
static void SampleStats() {
    float c = std::round(cpuSampler.Percent()), r = std::round(MemoryPercent());
    if (const char* fake = getenv("STATLET_FAKE_STATS")) sscanf(fake, "%f,%f", &c, &r);  // for screenshots: STATLET_FAKE_STATS=97,100
    bool changed = c != cpuPct || r != ramPct;
    cpuPct = c;
    ramPct = r;
    if (changed) ShowStats();
    bool high = sustain.Update(std::max(c, r), Now()) && t.openOnHighUsage;
    if (high != alert) {
        alert = high;
        Retarget();
    }
}

static void UpdateTrayIcon();

// The music card's colour: the Windows accent, or the album art's dominant colour (lifted to read on dark glass).
static Rgba MediaColor() { return t.artAccent && haveArt ? LiftForGlass(artColor) : accent; }

// Applies MediaColor() to everything on the music card that uses it (equalizer, bar, stop dot, play button, wave, gradient).
static void SetMediaColor() {
    Rgba c = MediaColor();
    if (mediaAccent.Color() == ToColor(c)) return;
    mediaAccent.Color(ToColor(c));
    if (gradientEnd) {
        gradientEnd.Color(ToColor(c));
        Rgba dim = GradientStart(c);
        gradientStart.Color(ToColor(dim));
    }
    DrawWave();
}

// Accent and material follow Windows settings: the accent color, transparency effects and battery saver.
static void ApplySettings() {
    accent = t.accentFallback;
    if (t.followAccent) try {
        auto c2 = uiSettings.GetColorValue(UIColorType::AccentLight2), c3 = uiSettings.GetColorValue(UIColorType::AccentLight3);
        accent = PickAccent({255, c2.R, c2.G, c2.B}, {255, c3.R, c3.G, c3.B}, t.glassTint);
    } catch (...) {
    }
    UpdateTrayIcon();
    if (!showingCard) icon->SetColor(accent);
    focusText->SetColor(accent);
    statusIcon->SetColor(accent);
    SetMediaColor();
    if (cpuPct >= 0) ShowStats();
    else {
        cpuBar->SetFill(accent);
        ramBar->SetFill(accent);
    }
    SYSTEM_POWER_STATUS power{};
    bool saver = GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1;
    bool effects = true;
    try {
        effects = uiSettings.AdvancedEffectsEnabled();
    } catch (...) {
    }
    acrylic->SetEffectsEnabled(effects && !saver);
}

static Away away = Away::No;
static float AwayOffset() { return -(hostH * scale + 4); }  // root's Offset.Y while slid or hidden

static BOOL CALLBACK AddMonitor(HMONITOR m, HDC, LPRECT, LPARAM list) {
    MONITORINFO mi{sizeof mi};
    Monitor one;  // stays empty (unusable) when a reading fails
    UINT dpi = 0, dpiY;
    if (GetMonitorInfoW(m, &mi)) one = {mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, 0, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0};
    if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dpi, &dpiY))) one.dpi = dpi;
    reinterpret_cast<std::vector<Monitor>*>(list)->push_back(one);
    return TRUE;
}

// Sizes the window for the primary monitor's work area and DPI (rules in placement.h). Called at start and on any display change.
static void Place(const char* why) {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, LPARAM(&monitors));
    Placement p;
    if (!PlaceOn(monitors, hostW, hostH, p)) {
        Log("place (%s): %zu monitor(s), none usable: kept the last placement", why, monitors.size());
        if (scale) return;
        p = PlacementFor(0, 0, GetSystemMetrics(SM_CXSCREEN), GetDpiForSystem(), hostW, hostH);  // first placement: something sane
    }
    BOOL placed = SetWindowPos(hwnd, HWND_TOPMOST, p.x, p.y, p.w, p.h, SWP_NOACTIVATE);
    Log("place (%s): %zu monitor(s), dpi %u window %d,%d %dx%d%s", why, monitors.size(), unsigned(std::lround(p.scale * 96)), p.x, p.y, p.w, p.h,
        placed ? "" : " FAILED");
    if (p.scale != scale) {
        scale = p.scale;
        root.Scale({scale, scale, 1});
        motion.InsertScalar(L"S", scale);
        gfx->SetScale(scale);
        if (away != Away::No) {  // the slide offset is in physical px: at a larger scale the old one leaves the window's bottom on screen
            root.StopAnimation(L"Offset.Y");
            root.Offset({0, AwayOffset(), 0});
        }
    }
}

static void StartSpring(const wchar_t* name, Spring& model) {
    auto spring = compositor.CreateSpringScalarAnimation();
    spring.DampingRatio(float(SpringDampingRatio(t.springStiffness, t.springDamping)));
    spring.Period(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::duration<double>(SpringPeriod(t.springStiffness))));
    spring.InitialVelocity(float(model.velocity));  // a retarget mid-flight keeps its momentum
    spring.FinalValue(float(model.target));
    motion.StartAnimation(name, spring);
}

static void Fade(wuc::Visual const& v, bool in) {
    auto fade = compositor.CreateScalarKeyFrameAnimation();
    if (in) {  // starts from the current opacity, so a quick re-hover does not flash
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction({0, 0}, {0.58f, 1}));
        fade.Duration(std::chrono::milliseconds(int(t.contentInMs)));
        fade.DelayTime(std::chrono::milliseconds(int(t.contentOutMs)));  // after the outgoing content is gone
        fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    } else {
        fade.InsertKeyFrame(1, 0);
        fade.Duration(std::chrono::milliseconds(int(t.contentOutMs)));
    }
    v.StartAnimation(L"Opacity", fade);
}

// Advances the hit-test model of the spring to now.
static void AdvanceModel() {
    double now = Now();
    springW.Step(now - modelTime);
    springH.Step(now - modelTime);
    modelTime = now;
}

static bool AnimationsEnabled() {
    try {
        return uiSettings.AnimationsEnabled();
    } catch (...) {
        return true;
    }
}

// The media card shows while music shows (unless usage is alerting); else the usage rows.
static bool ShowingMedia() { return !showingCard && mediaShown && !alert; }

static void ShowIdle() {
    showingCard = false;
    cardFace.IsVisible(false);
    statsFace.IsVisible(!ShowingMedia());
    mediaFace.IsVisible(ShowingMedia());
}

static void SyncProgress();
static void UpdateMediaMotion();

static void ShowCard(const Card& c) {
    showingCard = true;
    statsFace.IsVisible(false);
    mediaFace.IsVisible(false);
    cardFace.IsVisible(true);
    icon->Set({&c.glyph, 1});
    icon->SetColor(c.color);
    title->Set(c.title);
    bool hasBar = c.progress >= 0;
    body->visual.IsVisible(!hasBar);
    detail->visual.IsVisible(hasBar);
    bar->visual.IsVisible(hasBar);
    cardValue->visual.IsVisible(hasBar);
    if (!hasBar) {
        body->Set(c.body);
        return;
    }
    detail->Set(c.body);  // on the title's line, right-aligned, bottoms lined up with the title
    float innerW = t.expandedW - 2 * t.padX, innerH = t.cardH - 2 * t.padY;
    detail->visual.Offset({innerW - detail->Width(), std::round(title->Height() - detail->Height() - 1), 0});
    cardValue->Set(c.value);
    float rowH = cardValue->Height(), rowY = innerH - rowH;
    cardValue->visual.Offset({innerW - cardValue->Width(), rowY, 0});
    bar->visual.Offset({0, std::round(rowY + (rowH - 6) / 2), 0});
    bar->SetProgress(c.progress);
    bar->SetFill(c.color);
}

static void Retarget() {
    bool shouldOpen = hovering || pinned || alert || takeover || held;
    if (shouldOpen && !open && !takeover && showingCard) ShowIdle();  // a card left behind by a takeover that has since closed
    if (!showingCard) ShowIdle();                                       // music may have started or stopped
    // Hovered usage opens small; takeover and media cards open larger. A swap between faces while open resizes in place.
    float w, h;
    if (!shouldOpen) w = CollapsedWidth(), h = t.collapsedH;
    else if (showingCard) w = t.expandedW, h = t.cardH;
    else if (ShowingMedia()) w = t.expandedW, h = t.expandedH;
    else w = t.statsW, h = t.statsH;
    bool toggled = shouldOpen != open;
    if (toggled) open = shouldOpen, SyncProgress(), UpdateMediaMotion();  // the card's progress and motion run only while it shows
    if (!toggled && h == springH.target && w == springW.target) return;
    open = shouldOpen;
    bool started = modelTime != 0;  // the first layout at startup jumps straight to its size
    AdvanceModel();
    springW.target = w;
    springH.target = h;
    if (started && AnimationsEnabled() && !t.instantOpen) {
        StartSpring(L"W", springW);
        StartSpring(L"H", springH);
        if (toggled) {
            Fade(open ? collapsedFace : expandedFace, false);
            Fade(open ? expandedFace : collapsedFace, true);
        }
    } else {  // Windows animations off: jump straight to the new state
        motion.StopAnimation(L"W");
        motion.StopAnimation(L"H");
        collapsedFace.StopAnimation(L"Opacity");
        expandedFace.StopAnimation(L"Opacity");
        springW.Snap();
        springH.Snap();
        motion.InsertScalar(L"W", float(springW.value));
        motion.InsertScalar(L"H", float(springH.value));
        collapsedFace.Opacity(open ? 0.f : 1.f);
        expandedFace.Opacity(open ? 1.f : 0.f);
    }
}

static void Take(const Card& c) {
    ShowCard(c);
    takeover = true;
    SetTimer(hwnd, TimerTakeover, c.holdMs, nullptr);
    Retarget();
}

static void EndTakeover() {
    KillTimer(hwnd, TimerTakeover);
    takeover = false;
    if (hovering || pinned || alert || held) ShowIdle();  // stays open: show the usual content again
    Retarget();
}

// --- Now playing ---

static float ProgressW() { return t.expandedW - 2 * t.padX - 2 * 44; }  // between the elapsed and remaining times

// The wave is one long stroke drawn once (accent, round caps), a wavelength wider than the bar so drifting it by one
// wavelength loops seamlessly. Redrawn only on accent, DPI or device changes.
// shortcut: the amplitude stays full near 0% and 100% (no taper into the start and the stop dot); taper it with a
// second, flat-ended surface if the curl at the very start reads as a glitch.
static void DrawWave() {
    Rgba mc = MediaColor();
    float w = ProgressW() + t.waveLength, h = 12, cy = h / 2;
    gfx->Draw(waveSurface, UINT(std::ceil(w * gfx->scale)), UINT(std::ceil(h * gfx->scale)), [&](ID2D1DeviceContext* ctx) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(gfx->d2dFactory->CreatePathGeometry(path.put()));
        winrt::check_hresult(path->Open(sink.put()));
        auto y = [&](float x) { return cy - t.waveAmp * std::sin(2 * 3.14159265f * x / t.waveLength); };
        sink->BeginFigure({0, y(0)}, D2D1_FIGURE_BEGIN_HOLLOW);
        for (float x = 1; x <= w; x += 1) sink->AddLine({x, y(x)});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        winrt::com_ptr<ID2D1StrokeStyle> round;
        winrt::check_hresult(gfx->d2dFactory->CreateStrokeStyle(
            {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0}, nullptr, 0, round.put()));
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        winrt::check_hresult(ctx->CreateSolidColorBrush({mc.r / 255.f, mc.g / 255.f, mc.b / 255.f, 1}, brush.put()));
        ctx->DrawGeometry(path.get(), brush.get(), t.waveStroke, round.get());
    });
}

// Builds the music row (in the collapsed face), the equalizer and the media card (in the open face).
static void BuildMedia() {
    mediaAccent = compositor.CreateColorBrush(ToColor(MediaColor()));
    artSmall = std::make_unique<Picture>(*gfx, t.artSmall, t.artSmallR, t.buttonFill);
    artLarge = std::make_unique<Picture>(*gfx, t.artLarge, t.artLargeR, t.buttonFill);

    equalizer = compositor.CreateContainerVisual();
    equalizer.Size({EqWidth(), t.eqH});
    for (int i = 0; i < 4; i++) {
        auto b = compositor.CreateSpriteVisual();
        b.Brush(mediaAccent);
        b.Size({t.eqBarW, t.eqH});
        b.Offset({i * (t.eqBarW + t.eqGap), 0, 0});
        b.CenterPoint({t.eqBarW / 2, t.eqH / 2, 0});
        b.Scale({1, 0.3f, 1});
        equalizer.Children().InsertAtTop(b);
        eqBars.push_back(b);
    }

    peekTitle = std::make_unique<Text>(*gfx, TextStyle{t.typeClock, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink});
    peekArtist = std::make_unique<Text>(*gfx, TextStyle{t.typeClock, DWRITE_FONT_WEIGHT_NORMAL, t.ink2});
    peekRow = compositor.CreateContainerVisual();
    peekRow.Children().InsertAtTop(peekTitle->visual);
    peekRow.Children().InsertAtTop(peekArtist->visual);
    peekRow.Opacity(0);
    collapsedFace.Children().InsertAtTop(peekRow);

    // Media card: art, title and artist; the progress row (elapsed, bar, remaining); previous, play/pause, next.
    float innerW = t.expandedW - 2 * t.padX, innerH = t.expandedH - 2 * t.padY, textX = t.artLarge + 14;
    mediaFace = compositor.CreateContainerVisual();
    mediaTitle = std::make_unique<Text>(*gfx, TextStyle{t.typeTitle, DWRITE_FONT_WEIGHT_BOLD, t.ink}, innerW - textX);
    mediaArtist = std::make_unique<Text>(*gfx, TextStyle{t.typeBody, DWRITE_FONT_WEIGHT_NORMAL, t.ink2}, innerW - textX);
    elapsedText = std::make_unique<Text>(*gfx, TextStyle{t.typeCaption, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    remainingText = std::make_unique<Text>(*gfx, TextStyle{t.typeCaption, DWRITE_FONT_WEIGHT_NORMAL, t.ink2, true});
    for (auto v : {artLarge->visual.as<wuc::Visual>(), mediaTitle->visual.as<wuc::Visual>(), mediaArtist->visual.as<wuc::Visual>(),
                   elapsedText->visual.as<wuc::Visual>(), remainingText->visual.as<wuc::Visual>()})
        mediaFace.Children().InsertAtTop(v);

    // Progress bar: the played part (wave or flat line) is clipped to P; the track starts a gap after it and ends at a stop dot.
    // Segmented swaps all that for rows of ticks and Hairline thins the line; everything stays inside the same 12 epx slot.
    using PS = Tokens::ProgressStyle;
    bool seg = t.progressStyle == PS::Segmented, hair = t.progressStyle == PS::Hairline, grad = t.progressStyle == PS::Gradient;
    float th = hair ? t.hairH : t.waveStroke;
    float barW = ProgressW(), barY = t.artLarge + 12;
    mediaProps = compositor.CreatePropertySet();
    mediaProps.InsertScalar(L"P", 0);
    auto progress = compositor.CreateContainerVisual();
    progress.Size({barW, 12});
    progress.Offset({44, barY, 0});
    auto played = compositor.CreateContainerVisual();
    played.Size({barW, 12});
    auto clip = compositor.CreateInsetClip();
    // Segmented shows whole ticks only: Floor(P * n) of them, the last one without its trailing gap.
    float segU = (barW + t.segGap) / t.segCount;
    auto inset = compositor.CreateExpressionAnimation(seg ? L"Min(w - Floor(m.P * n) * u + g, w)" : L"w * (1 - m.P)");
    inset.SetReferenceParameter(L"m", mediaProps);
    inset.SetScalarParameter(L"w", barW);
    inset.SetScalarParameter(L"n", float(t.segCount));
    inset.SetScalarParameter(L"u", segU);
    inset.SetScalarParameter(L"g", t.segGap);
    clip.StartAnimation(L"RightInset", inset);
    played.Clip(clip);
    waveSprite = compositor.CreateSpriteVisual();
    waveSprite.Size({barW + t.waveLength, 12});
    DrawWave();
    auto waveBrush = compositor.CreateSurfaceBrush(waveSurface);
    waveBrush.Stretch(wuc::CompositionStretch::Fill);
    waveSprite.Brush(waveBrush);
    gfx->OnRedraw(&waveSurface, DrawWave);
    flatFill = compositor.CreateShapeVisual();
    flatFill.Size({barW, 12});
    auto flatGeometry = compositor.CreateRoundedRectangleGeometry();
    flatGeometry.Size({barW, th});
    flatGeometry.Offset({0, 6 - th / 2});
    flatGeometry.CornerRadius({th / 2, th / 2});
    auto flatShape = compositor.CreateSpriteShape(flatGeometry);
    gradientEnd = nullptr;
    if (grad) {  // a ramp from a dim start to the media colour, spanning only the played part (EndPoint follows P)
        auto ramp = compositor.CreateLinearGradientBrush();
        ramp.MappingMode(wuc::CompositionMappingMode::Absolute);  // the default, Relative, would read EndPoint as a multiple of the bar width
        ramp.StartPoint({0, 0});
        auto end = compositor.CreateExpressionAnimation(L"Vector2(Max(w * m.P, 1), 0)");
        end.SetReferenceParameter(L"m", mediaProps);
        end.SetScalarParameter(L"w", barW);
        ramp.StartAnimation(L"EndPoint", end);
        Rgba mc = MediaColor(), dim = GradientStart(mc);
        gradientStart = compositor.CreateColorGradientStop(0, ToColor(dim));
        ramp.ColorStops().Append(gradientStart);
        gradientEnd = compositor.CreateColorGradientStop(1, ToColor(mc));
        ramp.ColorStops().Append(gradientEnd);
        flatShape.FillBrush(ramp);
    } else flatShape.FillBrush(mediaAccent);
    flatFill.Shapes().Append(flatShape);
    bool wave = t.progressStyle == PS::Wave;
    waveSprite.Opacity(wave ? 1.f : 0.f);
    flatFill.Opacity(wave || seg ? 0.f : 1.f);
    waveShowing = wave;
    auto tickRow = [&](wuc::CompositionBrush brush) {
        auto row = compositor.CreateShapeVisual();
        row.Size({barW, 12});
        for (int i = 0; i < t.segCount; i++) {
            auto tick = compositor.CreateRoundedRectangleGeometry();
            tick.Size({segU - t.segGap, t.segH});
            tick.Offset({i * segU, 6 - t.segH / 2});
            tick.CornerRadius({1, 1});
            auto tickShape = compositor.CreateSpriteShape(tick);
            tickShape.FillBrush(brush);
            row.Shapes().Append(tickShape);
        }
        return row;
    };
    if (seg) played.Children().InsertAtTop(tickRow(mediaAccent));
    played.Children().InsertAtTop(flatFill);
    played.Children().InsertAtTop(waveSprite);
    auto rest = compositor.CreateShapeVisual();
    rest.Size({barW, 12});
    auto track = compositor.CreateRoundedRectangleGeometry();
    track.CornerRadius({th / 2, th / 2});
    auto trackOffset = compositor.CreateExpressionAnimation(L"Vector2(Min(w * m.P + g, w), y)");
    auto trackSize = compositor.CreateExpressionAnimation(L"Vector2(Max(w * (1 - m.P) - g - d, 0), s)");
    for (auto a : {trackOffset, trackSize}) {
        a.SetReferenceParameter(L"m", mediaProps);
        a.SetScalarParameter(L"w", barW);
        a.SetScalarParameter(L"g", t.waveGap);
        a.SetScalarParameter(L"d", th + t.waveGap);  // room for the stop dot
        a.SetScalarParameter(L"y", 6 - th / 2);
        a.SetScalarParameter(L"s", th);
    }
    track.StartAnimation(L"Offset", trackOffset);
    track.StartAnimation(L"Size", trackSize);
    auto trackShape = compositor.CreateSpriteShape(track);
    trackShape.FillBrush(compositor.CreateColorBrush(ToColor(t.track)));
    auto dot = compositor.CreateEllipseGeometry();
    dot.Center({barW - th / 2, 6});
    dot.Radius({th / 2, th / 2});
    auto dotShape = compositor.CreateSpriteShape(dot);
    dotShape.FillBrush(mediaAccent);
    if (!seg) rest.Shapes().Append(trackShape);
    if (!seg && !hair) rest.Shapes().Append(dotShape);  // a hairline ends without a dot
    if (seg) progress.Children().InsertAtTop(tickRow(compositor.CreateColorBrush(ToColor(t.track))));
    progress.Children().InsertAtTop(rest);
    progress.Children().InsertAtTop(played);
    knobVisual = nullptr;
    if (t.knob != Tokens::Knob::Never) {  // a dot, or a thin tick on the hairline and segmented bars; it rides P
        float half = hair || seg ? 1.f : t.knobR;
        knobVisual = compositor.CreateShapeVisual();
        knobVisual.Size({2 * half, 12});
        auto knobShape = [&]() -> wuc::CompositionSpriteShape {
            if (hair || seg) {
                auto pip = compositor.CreateRoundedRectangleGeometry();
                pip.Size({2, 8});
                pip.Offset({0, 2});
                pip.CornerRadius({1, 1});
                return compositor.CreateSpriteShape(pip);
            }
            auto ball = compositor.CreateEllipseGeometry();
            ball.Center({half, 6});
            ball.Radius({half, half});
            return compositor.CreateSpriteShape(ball);
        }();
        knobShape.FillBrush(compositor.CreateColorBrush(ToColor(t.ink)));
        knobVisual.Shapes().Append(knobShape);
        auto ride = compositor.CreateExpressionAnimation(L"w * m.P - h");
        ride.SetReferenceParameter(L"m", mediaProps);
        ride.SetScalarParameter(L"w", barW);
        ride.SetScalarParameter(L"h", half);
        knobVisual.StartAnimation(L"Offset.X", ride);
        knobVisual.Opacity(t.knob == Tokens::Knob::Always ? 1.f : 0.f);
        knobHot = false;
        progress.Children().InsertAtTop(knobVisual);
    }
    mediaFace.Children().InsertAtTop(progress);

    // Transport: round buttons centered under the bar; play/pause is accent-filled.
    float r = t.transportSize / 2, cy = innerH - r;
    for (int i = 0; i < 3; i++) {
        auto& b = transport[i];
        b.cx = innerW / 2 + (i - 1) * (t.transportSize + t.transportGap);
        auto circle = compositor.CreateEllipseGeometry();
        circle.Center({r, r});
        circle.Radius({r, r});
        auto fill = compositor.CreateSpriteShape(circle);
        fill.FillBrush(i == 1 ? mediaAccent : compositor.CreateColorBrush(ToColor(t.buttonFill)));
        auto button = compositor.CreateShapeVisual();
        button.Size({t.transportSize, t.transportSize});
        button.Offset({b.cx - r, cy - r, 0});
        button.Shapes().Append(fill);
        b.glyph = MakeIcon(*gfx, i == 0 ? 0xE892 : i == 1 ? 0xE768 : 0xE893, 14, i == 1 ? t.onAccent : t.ink);  // Previous, Play, Next
        b.glyph->visual.Offset({std::round(r - b.glyph->Width() / 2), std::round(r - b.glyph->Height() / 2), 0});
        button.Children().InsertAtTop(b.glyph->visual);
        mediaFace.Children().InsertAtTop(button);
    }
    // Usage either side of the transport, so it stays in reach while music shows: CPU from the left edge, RAM to the right.
    // Columns are measured: [label][bar][value]; the bar takes what the widest label and "100%" leave of mediaUsageW.
    for (auto [row, name] : {std::pair{&mediaCpu, L"CPU"}, std::pair{&mediaRam, L"RAM"}}) {
        row->label = std::make_unique<Text>(*gfx, TextStyle{t.typeCaption, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink2});
        row->label->Set(name);
        row->value = std::make_unique<Text>(*gfx, TextStyle{t.typeCaption, DWRITE_FONT_WEIGHT_SEMI_BOLD, t.ink, true});
        row->value->Set(L"100%");
    }
    float miniBarX = std::max(mediaCpu.label->Width(), mediaRam.label->Width()) + 6;
    float miniBarW = t.mediaUsageW - miniBarX - 6 - mediaCpu.value->Width();
    for (auto [row, name] : {std::pair{&mediaCpu, L"CPU"}, std::pair{&mediaRam, L"RAM"}}) {
        row->label->visual.Offset({0, std::round(-row->label->Height() / 2), 0});
        row->bar = std::make_unique<Bar>(*gfx, miniBarW, 4.f, t.track, accent);
        row->bar->visual.Offset({miniBarX, -2, 0});
        auto v = compositor.CreateContainerVisual();
        v.Offset({row == &mediaRam ? innerW - t.mediaUsageW : 0, cy, 0});
        for (auto child : {row->label->visual.as<wuc::Visual>(), row->bar->visual.as<wuc::Visual>(), row->value->visual.as<wuc::Visual>()})
            v.Children().InsertAtTop(child);
        mediaFace.Children().InsertAtTop(v);
    }
    mediaFace.IsVisible(false);
    expandedFace.Children().InsertAtTop(mediaFace);
}

static double MediaPosition() {
    double p = media.playing ? media.position + (Now() - mediaReadAt) : media.position;
    return media.duration > 0 ? std::min(p, media.duration) : p;
}

static void UpdateTimes() {
    bool known = media.duration > 0;
    elapsedText->visual.IsVisible(known);
    remainingText->visual.IsVisible(known);
    if (!known) return;
    double pos = MediaPosition();
    elapsedText->Set(FormatTime(pos));
    remainingText->Set(RightTime(media.duration, pos, t.timeTotal));
    float innerW = t.expandedW - 2 * t.padX, y = t.artLarge + 12 + std::round((12 - elapsedText->Height()) / 2);
    elapsedText->visual.Offset({0, y, 0});
    remainingText->visual.Offset({innerW - remainingText->Width(), y, 0});
}

// The bar's P runs to the end on the compositor while the media card shows and plays; otherwise it holds still.
static void SyncProgress() {
    bool showing = open && ShowingMedia();
    double d = media.duration, pos = MediaPosition();
    float p = d > 0 ? float(pos / d) : 0;
    mediaProps.StopAnimation(L"P");
    mediaProps.InsertScalar(L"P", p);
    if (showing && media.playing && d - pos > 0.05) {
        auto run = compositor.CreateScalarKeyFrameAnimation();
        run.InsertKeyFrame(0, p);
        run.InsertKeyFrame(1, 1, compositor.CreateLinearEasingFunction());
        run.Duration(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::duration<double>(d - pos)));
        mediaProps.StartAnimation(L"P", run);
    }
    UpdateTimes();
    if (showing && media.playing) SetTimer(hwnd, TimerMediaClock, 1000, nullptr);  // the times tick; nothing else does
    else KillTimer(hwnd, TimerMediaClock);
}

// Hover mode: the playhead dot fades in while the cursor is near the bar (6 epx slop) and out when it leaves or the card closes.
static void SetKnobHot(bool hot) {
    if (hot == knobHot || !knobVisual || t.knob != Tokens::Knob::Hover) return;
    knobHot = hot;
    auto fade = compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1, hot ? 1.f : 0.f);
    fade.Duration(std::chrono::milliseconds(AnimationsEnabled() ? 120 : 1));
    knobVisual.StartAnimation(L"Opacity", fade);
}

// x, y: cursor in epx in the window
static void UpdateKnobHover(double x, double y) {
    double fx = x - ((hostW - springW.value) / 2 + t.padX), fy = y - (t.topOffset + t.padY), barY = t.artLarge + 12;
    SetKnobHot(open && ShowingMedia() && fx >= 44 - 6 && fx <= 44 + ProgressW() + 6 && fy >= barY - 6 && fy <= barY + 12 + 6);
}

// Loops run only where they are seen: the equalizer while collapsed with music showing, the wave's drift while the card shows.
static void UpdateMediaMotion() {
    bool anim = AnimationsEnabled(), playing = mediaShown && media.playing;
    bool eq = playing && !open && shownRow != MediaRow::Rest && anim;
    if (eq != eqRunning) {
        eqRunning = eq;
        for (size_t i = 0; i < eqBars.size(); i++) {
            if (!eq) {
                eqBars[i].StopAnimation(L"Scale.Y");
                eqBars[i].Scale({1, 0.3f, 1});
                continue;
            }
            auto bounce = compositor.CreateScalarKeyFrameAnimation();
            bounce.InsertKeyFrame(0, 0.35f);
            bounce.InsertKeyFrame(0.5f, 1);
            bounce.InsertKeyFrame(1, 0.35f);
            bounce.Duration(std::chrono::milliseconds(t.eqLoopMs));
            bounce.DelayTime(std::chrono::milliseconds(t.eqLoopMs * int(i * 3 % 4) / 4));  // staggered
            bounce.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            eqBars[i].StartAnimation(L"Scale.Y", bounce);
        }
    }
    if (!open || !ShowingMedia()) SetKnobHot(false);
    bool wave = t.progressStyle == Tokens::ProgressStyle::Wave && media.playing;  // paused: the wave flattens into a line
    bool seg = t.progressStyle == Tokens::ProgressStyle::Segmented;
    if (wave != waveShowing) {
        waveShowing = wave;
        for (auto [v, on] : {std::pair{waveSprite.as<wuc::Visual>(), wave}, std::pair{flatFill.as<wuc::Visual>(), !wave && !seg}}) {
            auto fade = compositor.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(1, on ? 1.f : 0.f);
            fade.Duration(std::chrono::milliseconds(anim ? t.waveFlattenMs : 1));
            v.StartAnimation(L"Opacity", fade);
        }
    }
    bool drift = wave && open && ShowingMedia() && anim;
    if (drift != driftRunning) {
        driftRunning = drift;
        if (drift) {
            auto slide = compositor.CreateScalarKeyFrameAnimation();
            slide.InsertKeyFrame(0, 0);
            slide.InsertKeyFrame(1, -t.waveLength, compositor.CreateLinearEasingFunction());
            slide.Duration(std::chrono::milliseconds(t.waveDriftMs));
            slide.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            waveSprite.StartAnimation(L"Offset.X", slide);
        } else waveSprite.StopAnimation(L"Offset.X");
    }
}

static void RefreshMedia() {
    artSmall->visual.Opacity(media.playing ? 1.f : 0.5f);
    transport[1].glyph->Set(media.playing ? L"\xE769" : L"\xE768");  // Pause, Play
    UpdateClock();  // lays out the rest line (tucked drops the date) and retargets
    SyncProgress();
    UpdateMediaMotion();
}

static void HideMedia() {
    mediaShown = peeking = false;
    KillTimer(hwnd, TimerPeek);
    KillTimer(hwnd, TimerMediaHide);
    RefreshMedia();
}

static void EndPeek() {
    KillTimer(hwnd, TimerPeek);
    peeking = false;
    RefreshMedia();
}

// A fresh reading of the current media session.
static bool sampleMedia;  // the settings app's Music preview is showing a made-up track

static void OnMedia(MediaInfo m) {
    bool has = m.present && !m.title.empty();  // players announce a new track empty first; wait for its title
    if (sampleMedia && !has) return;           // no real player ended the sample
    std::wstring key = has ? m.title + L"\n" + m.artist : L"";
    bool newTrack = has && key != trackKey, wasShown = mediaShown, wasPlaying = media.playing;
    bool unpeeked = has && key != peekedKey;  // a player may report a new track paused for a moment before it plays
    media = std::move(m);
    mediaReadAt = Now();
    if (!has) {
        trackKey.clear();
        if (mediaShown || peeking) HideMedia();
        return;
    }
    if (newTrack) {
        trackKey = key;
        peekTitle->Set(media.title);
        peekArtist->Set(media.artist.empty() ? L"" : L"·  " + media.artist);
        mediaTitle->Set(media.title);
        mediaArtist->Set(media.artist);
        float textX = t.artLarge + 14, top = std::round((t.artLarge - mediaTitle->Height() - 2 - mediaArtist->Height()) / 2);
        mediaTitle->visual.Offset({textX, top, 0});
        mediaArtist->visual.Offset({textX, top + mediaTitle->Height() + 2, 0});
        artSmall->Set(nullptr);  // the plain tile until the new art arrives
        artLarge->Set(nullptr);
        haveArt = false;  // the old track's colour must not outlive it: the accent shows until the new art arrives
        SetMediaColor();
        int request = ++artRequest;
        ReadArt([request](winrt::com_ptr<IWICBitmapSource> art) {
            if (request != artRequest) return;
            artSmall->Set(art);
            artLarge->Set(art);
            haveArt = ArtColor(art.get(), artColor);  // once per track
            SetMediaColor();
        });
    }
    if (media.playing) {
        KillTimer(hwnd, TimerMediaHide);
        mediaShown = true;
    } else if (mediaShown && (wasPlaying || newTrack) && t.mediaHideMs > 0) SetTimer(hwnd, TimerMediaHide, t.mediaHideMs, nullptr);
    if (ShouldPeek(unpeeked, media.playing, wasShown) && !open) {
        peeking = true;
        peekedKey = key;
        SetTimer(hwnd, TimerPeek, t.mediaPeekMs, nullptr);
    }
    RefreshMedia();
}

// A click on the media card's buttons sends the command instead of pinning the island. x, y: epx in the window.
static bool ClickTransport(double x, double y) {
    if (!open || !ShowingMedia()) return false;
    double fx = x - ((hostW - springW.value) / 2 + t.padX), fy = y - (t.topOffset + t.padY);
    double cy = t.expandedH - 2 * t.padY - t.transportSize / 2;
    for (int i = 0; i < 3; i++) {
        double dx = fx - transport[i].cx, dy = fy - cy;
        if (dx * dx + dy * dy > t.transportSize * t.transportSize / 4) continue;
        SendMedia(MediaCommand(i));
        return true;
    }
    return false;
}

// A click on the right-hand time flips time left / track length and remembers it in state.json. x, y: epx in the window.
static void SaveState();
static bool ClickTime(double x, double y) {
    if (!open || !ShowingMedia() || media.duration <= 0) return false;
    double innerW = t.expandedW - 2 * t.padX, fx = x - ((hostW - springW.value) / 2 + t.padX), fy = y - (t.topOffset + t.padY), barY = t.artLarge + 12;
    if (fx < innerW - 44 || fx > innerW + 4 || fy < barY - 6 || fy > barY + 12 + 6) return false;
    t.timeTotal = !t.timeTotal;
    UpdateTimes();
    SaveState();
    return true;
}

static void OnVolume(int percent, bool muted) {
    if (percent == lastVolume && muted == lastMuted) return;
    lastVolume = percent;
    lastMuted = muted;
    Take(VolumeCard(percent, muted, accent, t, OutputDeviceName()));
}

static void OnBattery(int percent, bool onAc, int secondsLeft = -1) {
    auto e = battery.Update(percent, onAc);
    if (e != BatteryEvent::None) Take(BatteryCard(e, percent, accent, t, secondsLeft));
}

// Reads the power state after any power notification. Machines without a battery never raise an event.
static void CheckBattery() {
    SYSTEM_POWER_STATUS p{};
    if (!GetSystemPowerStatus(&p) || (p.BatteryFlag & 128) || p.BatteryLifePercent > 100 || p.ACLineStatus > 1) return;  // none or unknown
    OnBattery(p.BatteryLifePercent, p.ACLineStatus == 1, p.BatteryLifeTime == DWORD(-1) ? -1 : int(p.BatteryLifeTime));
}

static void SettleNet(bool online) {
    auto e = netWatch.Settle(online);
    if (e != NetEvent::None) Take(NetCard(e, accent, t));
}

static void ToggleFocus() {
    if (focus.Running()) {
        focus.Stop();
        KillTimer(hwnd, TimerFocusEnd);
        UpdateClock();
        return;
    }
    focus.Start(Now(), focusSeconds);
    SetTimer(hwnd, TimerFocusEnd, UINT(focusSeconds * 1000), nullptr);  // the one wake-up until the countdown ends
    UpdateClock();
    Take(FocusCard(false, t.focusMinutes, accent, t));
}

static void FinishFocus() {
    KillTimer(hwnd, TimerFocusEnd);
    focus.Stop();
    PlayEventSound(Event::FocusDone);
    UpdateClock();
    Take(FocusCard(true, t.focusMinutes, accent, t));
}

// STATLET_DEMO=battery|net|focus plays that takeover on a schedule, for screenshots and the live check; volume needs no demo
// (press a volume key). Battery and net go through the same watchers as real events, with made-up readings.
static void Demo(const char* what) {
    static int step;
    if (!strcmp(what, "battery")) {
        static const struct { int percent; bool ac; } s[] = {{25, false}, {18, false}, {9, false}, {40, true}};
        if (step < 4) OnBattery(s[step].percent, s[step].ac);
    } else if (!strcmp(what, "net")) {
        if (step < 2) SettleNet(step == 1);
    } else if (!strcmp(what, "focus")) {
        if (step == 0) ToggleFocus();
    } else if (!strcmp(what, "bt")) {  // rehearses a device connecting, without the headphones
        if (step == 0) PostMessageW(hwnd, WM_APP_BT, 0, 0);
    }
    if (++step >= 4) KillTimer(hwnd, TimerDemo);
}

// Outside the island's shape the window lets clicks through to whatever is below it.
static void SetClickThrough(bool on) {
    if (on == clickThrough) return;
    clickThrough = on;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, on ? ex | WS_EX_TRANSPARENT : ex & ~WS_EX_TRANSPARENT);
}

// --- Getting out of the way of maximized and fullscreen windows (event-driven: WinEvent hooks, no polling) ---

static HWND frontWindow;  // the window FrontWindowState judged, for the log and the location hook

// Walks the z-order from the front and judges the frontmost window on the primary (FrontOnPrimary and AwayFor in
// shape_math.h), so focus on another monitor does not matter.
static Away FrontWindowState() {
    frontWindow = nullptr;
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(primary, &mi)) return Away::No;
    std::vector<ZWindow> z;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
        RECT r{};
        if (w == hwnd || !IsWindowVisible(w) || IsIconic(w) || (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)) || !GetWindowRect(w, &r) || IsRectEmpty(&r))
            continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) continue;  // other desktops, suspended apps
        wchar_t cls[32] = L"";
        GetClassNameW(w, cls, 32);
        if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) continue;  // the taskbar sits over maximized windows
        ZWindow zw;
        zw.id = w;
        zw.topmost = (ex & WS_EX_TOPMOST) != 0;
        FrontWindow& f = zw.f;
        f.usable = true;
        f.shell = !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
        f.onPrimary = MonitorFromWindow(w, MONITOR_DEFAULTTONULL) == primary;
        f.covers = f.onPrimary && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
        f.zoomed = IsZoomed(w);
        f.caption = (GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) != 0;
        z.push_back(zw);
        if (f.onPrimary && !zw.topmost) break;  // nothing below it can be in front
    }
    const ZWindow* front = FrontOnPrimary(z);
    if (!front) return Away::No;
    frontWindow = HWND(const_cast<void*>(front->id));
    return AwayFor(front->f);
}

// One log line for an away change: the window FrontWindowState judged, the foreground window, and what Windows itself says.
static void LogAway(Away was, Away now) {
    static const char* names[] = {"no", "slid", "hidden"};
    HWND fg = frontWindow;
    char cls[64] = "";
    RECT r{};
    DWORD cloaked = 0;
    QUERY_USER_NOTIFICATION_STATE quns{};
    if (fg) {
        GetClassNameA(fg, cls, sizeof cls);
        GetWindowRect(fg, &r);
        DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    }
    SHQueryUserNotificationState(&quns);
    Log("away %s -> %s: front %p (fg %p) class '%s' rect %ld,%ld-%ld,%ld style 0x%08X ex 0x%08X zoomed %d visible %d cloaked %lu notif %d", names[int(was)],
        names[int(now)], (void*)fg, (void*)GetForegroundWindow(), cls, r.left, r.top, r.right, r.bottom, fg ? unsigned(GetWindowLongW(fg, GWL_STYLE)) : 0u,
        fg ? unsigned(GetWindowLongW(fg, GWL_EXSTYLE)) : 0u, fg ? IsZoomed(fg) : 0, fg ? IsWindowVisible(fg) : 0, cloaked, int(quns));
}

static AwayGate awayGate;  // shape_math.h: a return to No waits AwayReturnMs, so Alt+Tab does not bounce the island
constexpr UINT AwayReturnMs = 120;

// settle: the return wait ran out (TimerAwayReturn).
static void UpdateAway(bool settle = false) {
    bool waiting = awayGate.returning;
    bool changed = settle ? awayGate.Settle(FrontWindowState()) : awayGate.See(FrontWindowState());
    if (awayGate.returning && !waiting) SetTimer(hwnd, TimerAwayReturn, AwayReturnMs, nullptr);
    if (!awayGate.returning) KillTimer(hwnd, TimerAwayReturn);
    if (!changed) return;
    Away was = away, now = awayGate.shown;
    away = now;
    LogAway(was, now);
    UpdateTimers();
    if (was == Away::Hidden) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (now == Away::Hidden) ShowWindow(hwnd, SW_HIDE);
    auto slide = compositor.CreateScalarKeyFrameAnimation();
    slide.InsertKeyFrame(1, now == Away::No ? 0.f : AwayOffset(), compositor.CreateCubicBezierEasingFunction({0.3f, 0}, {0.2f, 1}));
    slide.Duration(std::chrono::milliseconds(now == Away::Hidden || was == Away::Hidden ? 1 : 300));
    root.StartAnimation(L"Offset.Y", slide);
    if (now != Away::No && hovering) {
        hovering = false;
        Retarget();
    }
}

// Checks everything that can go stale while the island was not told: placement, away state, clock, usage, battery.
// Called after display, DPI and work-area changes, resume, display on, lid open, unlock, session connect and an Explorer restart.
static void Revalidate(const char* why) {
    Place(why);
    UpdateAway();
    UpdateClock();
    SampleStats();
    CheckBattery();
}

// Display, DPI and work-area messages arrive in bursts (a cable plug sends several): revalidate once, 200 ms after the
// last one, naming every reason that asked.
static std::string pendingWhy;
static void RevalidateSoon(const char* why) {
    if (("+" + pendingWhy + "+").find("+" + std::string(why) + "+") == std::string::npos) pendingWhy += (pendingWhy.empty() ? "" : "+") + std::string(why);
    SetTimer(hwnd, TimerRevalidate, 200, nullptr);
}

// Logs every power broadcast with its setting and value (Modern Standby sends different ones than classic sleep), and
// revalidates when the machine or its display comes back.
static PowerState powerState;
static bool displayOff, locked;
static void OnPower(WPARAM wp, LPARAM lp) {
    if (wp != PBT_POWERSETTINGCHANGE || !lp) {
        Log("power %u", unsigned(wp));  // 4 suspend, 7 resume suspend, 18 resume automatic, 10 power status
        if (const char* why = PowerReason(UINT(wp), PowerSetting::Other, 0, powerState)) RevalidateSoon(why);
        return;
    }
    auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
    DWORD value = s->DataLength >= sizeof(DWORD) ? *reinterpret_cast<const DWORD*>(s->Data) : 0;
    const GUID& g = s->PowerSetting;
    PowerSetting kind = g == GUID_CONSOLE_DISPLAY_STATE ? PowerSetting::Display : g == GUID_LIDSWITCH_STATE_CHANGE ? PowerSetting::Lid : PowerSetting::Other;
    const char* name = kind == PowerSetting::Display ? "display" : kind == PowerSetting::Lid ? "lid" : g == GUID_ACDC_POWER_SOURCE ? "acdc"
                     : g == GUID_POWER_SAVING_STATUS ? "saver" : g == GUID_BATTERY_PERCENTAGE_REMAINING ? "battery" : "other";
    Log("power setting %s %lu", name, value);
    if (const char* why = PowerReason(PBT_POWERSETTINGCHANGE, kind, value, powerState)) RevalidateSoon(why);
    if (kind == PowerSetting::Display) {
        displayOff = value == 0;  // dimmed (2) still shows the island
        UpdateTimers();
    }
}

// Starts and stops the polls as the island becomes seen or unseen (rules in power.h). Coming back catches up at once;
// display on and unlock also revalidate.
static void StartHover() { SetTimer(hwnd, TimerHover, t.hoverPollMs, nullptr); }
static void StopHover() { KillTimer(hwnd, TimerHover); }
static void StartClock() { UpdateClock(); }  // re-arms the clock timer
static void StopClock() { KillTimer(hwnd, TimerClock); }
static void StartStats() {
    SetTimer(hwnd, TimerStats, t.statsPollMs, nullptr);
    SampleStats();
}
static void StopStats() { KillTimer(hwnd, TimerStats); }

// --- Status slot and downloads (downloads.h, v2 items 7 and 11) ---

static void SetStatus(wchar_t g) {  // the persistent glyph (0: none)
    if (g == statusPersistent) return;
    statusPersistent = g;
    UpdateClock();  // lays the row out, re-sizes the pill and renames the window
}
// A peek takes the slot for ms: it fades in over 150 ms and out over 150 ms, and only then does the slot (and its width) go.
// A newer peek replaces it. With Windows animations off it is steady and the slot goes at once. Items 6, 7 and 8 all use this.
static bool peekFading;  // the fade-out is running: the next timer tick ends the peek
static void PeekStatus(wchar_t g, int ms) {
    if (peekFading) slotShown = 0;  // a peek that was fading out starts over (fades in again)
    peekFading = false;
    statusPeek = g;
    SetTimer(hwnd, TimerStatusPeek, ms, nullptr);
    UpdateClock();
}
static void EndStatusPeek() {
    KillTimer(hwnd, TimerStatusPeek);
    if (!peekFading && statusPeek && slotShown == statusPeek && AnimationsEnabled()) {
        auto fade = compositor.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1, 0);  // from wherever the opacity is now
        fade.Duration(std::chrono::milliseconds(150));
        statusIcon->visual.StartAnimation(L"Opacity", fade);
        peekFading = true;
        SetTimer(hwnd, TimerStatusPeek, 150, nullptr);
        return;
    }
    peekFading = false;
    statusPeek = 0;
    UpdateClock();
}

static DownloadTracker downloads;
static std::wstring downloadsDir;
static HANDLE downloadChange = INVALID_HANDLE_VALUE;  // folder change notification, waited on by the message loop

static std::wstring DownloadsDir() {
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"STATLET_DOWNLOADS_DIR", env, MAX_PATH);  // tests point this at a temp folder
    if (n > 0 && n < MAX_PATH) return env;
    PWSTR known = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &known))) dir = known;
    CoTaskMemFree(known);
    return dir;
}

static ULONGLONG newestFinishedAgeMs = ~0ull;  // how long ago the newest non-partial file in the folder was written (set by ScanDownloads)

// The folder root's partial files with their sizes. The directory entry's size lags a file being written, so ask the file.
static std::vector<PartialFile> ScanDownloads() {
    std::vector<PartialFile> out;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW((downloadsDir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return out;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG nowTicks = (ULONGLONG(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
    newestFinishedAgeMs = ~0ull;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!IsPartialName(fd.cFileName)) {  // a browser may rename "Unconfirmed 123.crdownload" to the real name: note the newest finished file
            ULONGLONG written = (ULONGLONG(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (nowTicks >= written) newestFinishedAgeMs = std::min(newestFinishedAgeMs, (nowTicks - written) / 10000);
            continue;
        }
        PartialFile p{fd.cFileName, (std::uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow};
        HANDLE f = CreateFileW((downloadsDir + L"\\" + fd.cFileName).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size;
        if (f != INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(f, &size)) p.size = std::uint64_t(size.QuadPart);
            CloseHandle(f);
        }
        out.push_back(std::move(p));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

// Looks at the folder, moves the tracker, shows the result, and keeps the 1 Hz timer running only while a download is.
static void ScanDownloadsNow() {
    ULONGLONG now = GetTickCount64();
    auto before = downloads.state;
    auto partials = ScanDownloads();
    // Finished: the file the partial turns into is there, or (Chromium's "Unconfirmed NNN" names) some file was just written.
    auto after = downloads.Observe(now, partials, [](const std::wstring& stem) {
        return GetFileAttributesW((downloadsDir + L"\\" + stem).c_str()) != INVALID_FILE_ATTRIBUTES || newestFinishedAgeMs < 15000;
    });
    if (after != before) {
        Log("downloads %s", after == DownloadTracker::State::Active ? "active" : after == DownloadTracker::State::Done ? "done" : "idle");
        if (after == DownloadTracker::State::Active) SetStatus(t.glyphDownload);
        else {
            SetStatus(0);
            if (after == DownloadTracker::State::Done) {
                PeekStatus(t.glyphDone, t.peekDoneMs);
                PlayEventSound(Event::DownloadDone);
            }
        }
    }
    if (downloads.Busy(now)) SetTimer(hwnd, TimerDownload, 1000, nullptr);
    else KillTimer(hwnd, TimerDownload);
}

static void StartDownloads() {
    downloadsDir = DownloadsDir();
    if (downloadsDir.empty()) return;
    downloadChange = FindFirstChangeNotificationW(downloadsDir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    downloads.Seed(GetTickCount64(), ScanDownloads());
    if (downloadChange == INVALID_HANDLE_VALUE) Log("downloads folder cannot be watched (error %lu): the icon will not show", GetLastError());  // missing folder or offline drive
    else Log("downloads watching");
    ScanDownloadsNow();
}
static void StopDownloads() {
    if (downloadChange != INVALID_HANDLE_VALUE) FindCloseChangeNotification(downloadChange);
    downloadChange = INVALID_HANDLE_VALUE;
    KillTimer(hwnd, TimerDownload);
    downloads = DownloadTracker{};
    SetStatus(0);
}
static void OnDownloadChange() {  // while a download runs the 1 Hz timer does the looking; this catches a download starting
    FindNextChangeNotification(downloadChange);
    if (!downloads.Busy(GetTickCount64())) ScanDownloadsNow();
}
static bool WantDownloads(const WidgetState& s) { return Seen(s) && t.downloadIcon && !getenv("ISLAND_BENCH"); }
static void StartBluetooth() {
    WatchBluetooth(hwnd, WM_APP_BT);
    Log("bluetooth watching");
}
static bool WantBluetooth(const WidgetState& s) { return Seen(s) && t.btIcon && !getenv("ISLAND_BENCH"); }

// The polled widgets (widgets.h). Event-driven sources (volume, network, media, battery) cost nothing while idle.
static std::array<Widget, 5> widgets{{
    {"hover", WantHover, StartHover, StopHover},
    {"clock", WantRest, StartClock, StopClock},
    {"stats", WantRest, StartStats, StopStats},
    {"downloads", WantDownloads, StartDownloads, StopDownloads},
    {"bluetooth", WantBluetooth, StartBluetooth, StopBluetooth},
}};
static Widget& clockWidget = widgets[1];
static bool ClockRunning() { return clockWidget.running; }

static void UpdateTimers() {
    WidgetState s{away != Away::No, away == Away::Hidden, displayOff, locked};
    bool wasRest = clockWidget.running;
    Reconcile(widgets, s);
    if (clockWidget.running != wasRest) Log("timers %s", clockWidget.running ? "on" : "off");
}

static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND w, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (event >= EVENT_OBJECT_SHOW) {  // object events: only top-level windows
        if (idObject != OBJID_WINDOW || idChild != 0 || !w || GetAncestor(w, GA_ROOT) != w) return;
        if (event == EVENT_OBJECT_LOCATIONCHANGE && w != GetForegroundWindow() && w != frontWindow) return;
        // The show event can arrive before the window is in front in the z-order: look again shortly (one timer, coalesced).
        if (event != EVENT_OBJECT_LOCATIONCHANGE) SetTimer(hwnd, TimerAwayRecheck, 120, nullptr);
    }
    UpdateAway();
}

static void PollCursor() {
    POINT p;
    RECT wr;
    if (away != Away::No) return;
    if (!GetCursorPos(&p) || !GetWindowRect(hwnd, &wr)) return;
    AdvanceModel();
    double x = (p.x - wr.left) / scale, y = (p.y - wr.top) / scale, w = springW.value, h = springH.value;
    bool over = InRoundedRect(x, y, (hostW - w) / 2, t.topOffset, w, h, IslandRadius(t, h));
    SetClickThrough(!over);
    UpdateKnobHover(x, y);
    if (over != hovering) {
        hovering = over;
        Retarget();
    }
}

// --- Settings file ---

static wuc::Desktop::DesktopWindowTarget target{nullptr};
static std::wstring settingsDir;  // DataDir(): STATLET_DATA_DIR or %LOCALAPPDATA%\Statlet
static std::string settingsJson;  // the file's text as last applied

static std::string ReadDataFile(const wchar_t* name) {
    std::string text;
    if (settingsDir.empty()) return text;  // benchmarks run without a data folder
    if (FILE* f = _wfopen((settingsDir + L"\\" + name).c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    return text;
}

static std::string ReadSettingsFile() { return ReadDataFile(L"settings.json"); }

static void SaveState() {
    if (!WriteState(settingsDir, t.timeTotal)) Log("state.json write failed (error %lu); the time toggle will not survive a restart", GetLastError());
}

// The file over the defaults; environment overrides (tests, benchmarks) win.
static void LoadTokens(const std::string& json) {
    t = Tokens{};
    LoadSettings(t, json);
    LoadState(t, ReadDataFile(L"state.json"));
    if (const char* top = getenv("STATLET_TOP_OFFSET")) t.topOffset = float(atof(top));
    hostW = t.maxOpenW + 2 * t.shadowBlur + 8;
    hostH = t.topOffset + t.maxOpenH + t.shadowY + t.shadowBlur + 4;
    const char* secs = getenv("STATLET_FOCUS_SECONDS");
    focusSeconds = secs ? atof(secs) : t.focusMinutes * 60.0;
    springW = Spring(t.collapsedMinW, t.springStiffness, t.springDamping);  // the hit-test model follows the feel too
    springH = Spring(t.collapsedH, t.springStiffness, t.springDamping);
}

// Holds a view open for the settings app's preview. The card is a sample volume card; music is a made-up track when
// no player is playing.
// shortcut: if the settings app dies without releasing, the hold lasts until the island restarts; add a process
// watch if that ever happens in practice.
static void Hold(int view) {
    bool wasCard = held == 2;
    held = view;
    if (view == 2) {
        ShowCard(VolumeCard(lastVolume >= 0 ? lastVolume : 50, lastMuted, accent, t, OutputDeviceName()));
        takeover = true;
        KillTimer(hwnd, TimerTakeover);
    } else if (wasCard) EndTakeover();
    Retarget();  // opens first, so the sample track below does not peek
    if (view == 3 && !mediaShown) {
        sampleMedia = true;
        MediaInfo sample;
        sample.present = sample.playing = true;
        sample.title = L"Northern Lights";
        sample.artist = L"Aurora Field";
        sample.position = 72, sample.duration = 204;
        OnMedia(std::move(sample));
        Retarget();
    } else if (view != 3 && sampleMedia) {
        sampleMedia = false;
        trackKey.clear();
        OnMedia({});
        ReadMedia(OnMedia);
    }
}

// Rebuilds every visual from the new tokens. Running state (media, Focus, usage, a hold) carries over; a takeover
// card on screen is dropped.
static void ReloadSettings() {
    std::string json = ReadSettingsFile();
    if (json == settingsJson) return;  // something else in the folder changed
    settingsJson = json;
    LoadTokens(json);
    modelTime = 0;  // the first layout after the rebuild jumps straight to its size
    open = showingCard = takeover = eqRunning = driftRunning = false;
    waveShowing = true;
    KillTimer(hwnd, TimerTakeover);
    eqBars.clear();
    gfx->Forget(&waveSurface);
    waveSurface = nullptr;
    BuildIsland();
    BuildMedia();
    target.Root(root);
    Log("settings reloaded (%zu bytes), away %d", json.size(), int(away));
    scale = 0;  // forces Place to apply the monitor's scale
    Place("settings");
    if (away != Away::No) root.Offset({0, AwayOffset(), 0});  // the new root starts on screen, over the maximized window
    ApplySettings();
    UpdateClock();
    UpdateTimers();  // a changed downloadIcon starts or stops that watcher
    modelTime = Now();
    trackKey.clear();  // refills the song's text and art; peekedKey stays, so it does not peek again
    if (mediaShown) ReadMedia(OnMedia);
    if (sampleMedia) sampleMedia = mediaShown = peeking = false;  // the rebuild dropped the sample's text; Hold adds it again
    if (held) Hold(held);
}

// Opens the settings app (settings\StatletSettings.exe beside the island), or brings it forward if it is open.
static void OpenSettings() {
    if (HWND w = FindWindowW(nullptr, L"Statlet Settings")) {
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring app = exe;
    app = app.substr(0, app.find_last_of(L'\\') + 1) + L"settings\\StatletSettings.exe";
    HINSTANCE started = ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(started) <= 32) Log("Settings did not start (%ls, code %d): is the settings folder beside statlet.exe?", app.c_str(), int(reinterpret_cast<INT_PTR>(started)));  // was silent
}

static bool trayAdded;
static HICON trayIcon;

// The logo's S in white (dark taskbar) or ink (light taskbar) with its pill in the accent, tinted from the masks in
// tray_masks.h at the tray's icon size.
static HICON MakeTrayIcon() {
    DWORD light = 0, cb = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &cb);
    int want = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    const TrayMask* m = &trayMasks[std::size(trayMasks) - 1];
    for (auto& c : trayMasks) if (c.size >= want) { m = &c; break; }
    const uint32_t ink[3] = {light ? 0x1Bu : 255u, light ? 0x1Fu : 255u, light ? 0x27u : 255u};
    BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), m->size, -m->size, 1, 32, BI_RGB}};
    uint32_t* px;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
    HBITMAP maskBmp = CreateBitmap(m->size, m->size, 1, 1, nullptr);
    if (!color || !maskBmp) {
        if (color) DeleteObject(color);
        if (maskBmp) DeleteObject(maskBmp);
        return nullptr;
    }
    for (int i = 0; i < m->size * m->size; i++) {
        uint32_t a = m->pill[i], b = m->s[i];
        uint32_t o = a + b * (255 - a) / 255;  // pill over S
        auto mix = [&](uint32_t pc, uint32_t sc) { return o ? (pc * a + sc * b * (255 - a) / 255) / o : 0u; };
        px[i] = o << 24 | mix(accent.r, ink[0]) << 16 | mix(accent.g, ink[1]) << 8 | mix(accent.b, ink[2]);
    }
    ICONINFO ii{TRUE, 0, 0, maskBmp, color};
    HICON hi = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(maskBmp);
    return hi;
}

static void UpdateTrayIcon() {
    if (!trayAdded) return;
    HICON fresh = MakeTrayIcon();
    if (!fresh) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd;
    nid.uFlags = NIF_ICON;
    nid.hIcon = fresh;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (trayIcon) DestroyIcon(trayIcon);
    trayIcon = fresh;
}

static void AddTrayIcon() {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    if (trayIcon) DestroyIcon(trayIcon);
    trayIcon = MakeTrayIcon();
    nid.hIcon = trayIcon ? trayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Statlet");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    trayAdded = true;
}

// Screenshots (capture.h, v2 item 8): the menu has closed 150 ms after the click, so it is not in the picture.
static int pendingShot;  // MenuCopyShot or MenuSaveShot
static void StartShot(int kind) {
    pendingShot = kind;
    SetTimer(hwnd, TimerShot, 150, nullptr);
}
static void TakeShot() {
    KillTimer(hwnd, TimerShot);
    POINT at;
    GetCursorPos(&at);
    Shot shot;
    std::wstring name;
    bool save = pendingShot == MenuSaveShot;
    bool ok = CaptureMonitorAt(hwnd, at, shot) && (save ? SaveShot(shot, name) : CopyShot(hwnd, shot));
    if (!ok) {
        Log("screenshot failed (error %lu)", GetLastError());  // no peek: a camera that did not fire must not look as if it did
        return;
    }
    Log(save ? "screenshot saved %dx%d" : "screenshot copied %dx%d", shot.w, shot.h);
    PeekStatus(t.glyphCamera, t.peekShotMs);
    PlayEventSound(Event::Screenshot);
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    wchar_t start[48];
    swprintf(start, 48, L"Start focus timer (%d min)", t.focusMinutes);
    AppendMenuW(menu, MF_STRING, MenuFocus, focus.Running() ? L"Stop focus timer" : start);
    AppendMenuW(menu, MF_STRING, MenuCopyShot, L"Copy screenshot");
    AppendMenuW(menu, MF_STRING, MenuSaveShot, L"Save screenshot");
    AppendMenuW(menu, MF_STRING, MenuSettings, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuQuit, L"Quit Statlet");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);  // lets the menu close when the user clicks elsewhere
    UINT choice = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr));
    if (choice == MenuQuit) DestroyWindow(hwnd);
    else if (choice == MenuFocus) ToggleFocus();
    else if (choice == MenuCopyShot || choice == MenuSaveShot) StartShot(int(choice));
    else if (choice == MenuSettings) OpenSettings();
    DestroyMenu(menu);
}

static const char* benchMode;
static const char* benchOut;

static void WriteBench(const char* fmt, double value) {
    if (FILE* f = benchOut ? fopen(benchOut, "a") : nullptr) {
        fprintf(f, fmt, value);
        fclose(f);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE:  // only arrives while the cursor is on the window: re-check now instead of on the next poll
        PollCursor();
        return 0;
    case WM_LBUTTONDOWN:
        PollCursor();
        if (hovering && !ClickTransport(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale) && !ClickTime(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale)) {  // a click that beat the poll to a spot outside the shape is dropped
            pinned = !pinned;
            Retarget();
        }
        return 0;
    case WM_TIMER:
        if (wp == TimerHover) PollCursor();
        else if (wp == TimerClock) UpdateClock();
        else if (wp == TimerStats) SampleStats();
        else if (wp == TimerTakeover) EndTakeover();
        else if (wp == TimerFocusEnd) FinishFocus();
        else if (wp == TimerNetDebounce) {
            KillTimer(h, TimerNetDebounce);
            SettleNet(InternetOnline());
        } else if (wp == TimerDemo) Demo(getenv("STATLET_DEMO"));
        else if (wp == TimerPeek) EndPeek();
        else if (wp == TimerMediaHide) HideMedia();
        else if (wp == TimerMediaClock) UpdateTimes();
        else if (wp == TimerDownload) ScanDownloadsNow();
        else if (wp == TimerStatusPeek) EndStatusPeek();
        else if (wp == TimerShot) TakeShot();
        else if (wp == TimerReload) {
            KillTimer(h, TimerReload);
            ReloadSettings();
        } else if (wp == TimerAwayRecheck) {
            KillTimer(h, TimerAwayRecheck);
            UpdateAway();        } else if (wp == TimerAwayReturn) {
            KillTimer(h, TimerAwayReturn);
            UpdateAway(true);
        } else if (wp == TimerRevalidate) {
            KillTimer(h, TimerRevalidate);
            std::string why = std::move(pendingWhy);
            pendingWhy.clear();
            Revalidate(why.c_str());
        }
        else if (wp == TimerBenchToggle) { pinned = !pinned; Retarget(); }
        else if (wp == TimerBenchEnd) DestroyWindow(h);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        RevalidateSoon(msg == WM_DPICHANGED ? "dpi" : "display");
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) RevalidateSoon("work area");
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"intl") == 0) {  // a locale or 12/24-hour change: re-measure the widest time
            MeasureWidestTime();
            UpdateClock();
        }
        return 0;
    case WM_WTSSESSION_CHANGE:
        Log("session change %u", unsigned(wp));  // 7 lock, 8 unlock, 1/2 console connect/disconnect, 3/4 remote
        if (const char* why = SessionReason(wp)) RevalidateSoon(why);
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_UNLOCK) {
            locked = wp == WTS_SESSION_LOCK;
            UpdateTimers();
        }
        return 0;
    case WM_APP_VOLUME:
        if (wp == UINT_MAX) RebindVolume();  // the default output device changed
        else OnVolume(int(wp), lp != 0);
        return 0;
    case WM_APP_BT:  // a paired device connected (bluetooth.cpp); the icon only, no name, no sound (Windows plays its own)
        if (t.btIcon) {
            Log("bluetooth connected");
            PeekStatus(t.glyphBluetooth, t.peekBluetoothMs);
        }
        return 0;
    case WM_APP_SHOT:  // the menu items' action without the menu (the tests post this); wParam is MenuCopyShot or MenuSaveShot
        if (wp == MenuCopyShot || wp == MenuSaveShot) StartShot(int(wp));
        return 0;
    case WM_APP_MEDIA:
        if (wp) RebindMedia();  // a different player became current
        else ReadMedia(OnMedia);
        return 0;
    case WM_APP_NET:
        if (wp) {  // back: settle now; going offline waits out short blips
            KillTimer(h, TimerNetDebounce);
            SettleNet(true);
        } else SetTimer(h, TimerNetDebounce, t.netLostDelayMs, nullptr);
        return 0;
    case WM_POWERBROADCAST:
        OnPower(wp, lp);
        CheckBattery();
        [[fallthrough]];
    case WM_APP_SETTINGS:
        ApplySettings();
        return TRUE;
    case WM_APP_HOLD:  // from the settings app
        Hold(wp <= 3 ? int(wp) : 0);
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = h;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        Log("exit");
        PostQuitMessage(0);
        return 0;
    }
    }
    if (msg == taskbarCreated && msg) {  // Explorer restarted
        AddTrayIcon();
        RevalidateSoon("explorer");
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    benchMode = getenv("ISLAND_BENCH");
    benchOut = getenv("ISLAND_BENCH_OUT");
    if (!benchMode) {
        // STATLET_INSTANCE=<name> lets a test island (with its own STATLET_DATA_DIR) run beside the real one.
        wchar_t instance[32];
        std::wstring mutex = L"Local\\Statlet.Island";
        if (DWORD n = GetEnvironmentVariableW(L"STATLET_INSTANCE", instance, 32); n > 0 && n < 32) mutex += std::wstring(L".") + instance;
        CreateMutexW(nullptr, TRUE, mutex.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Started again (Start menu, shortcut): the island is already up, so show Settings instead of exiting unseen.
            // A test island (STATLET_INSTANCE) never opens windows.
            if (GetEnvironmentVariableW(L"STATLET_INSTANCE", instance, 32) == 0) OpenSettings();
            return 0;
        }
    }
    // Benchmarks keep the defaults. The reference app Dynamic Edge often occupies the top center; tests and benchmarks
    // move the island down with STATLET_TOP_OFFSET.
    HANDLE settingsChanged = INVALID_HANDLE_VALUE;
    if (!benchMode) {
        settingsDir = DataDir();
        CreateDirectoryW(settingsDir.c_str(), nullptr);
        LogOpen(settingsDir.c_str());
        Log("start pid %lu", GetCurrentProcessId());
        settingsJson = ReadSettingsFile();
        settingsChanged = FindFirstChangeNotificationW(settingsDir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    }
    LoadTokens(settingsJson);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto queue = CreateDispatcherQueue();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"StatletIsland";
    RegisterClassW(&wc);
    // Layered + transparent is what makes a window click-through for other processes; it starts that way until
    // the cursor is over the shape.
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                           wc.lpszClassName, L"Statlet", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    clickThrough = true;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring font = exe;
    font = font.substr(0, font.find_last_of(L'\\') + 1) + L"Manrope.ttf";

    compositor = wuc::Compositor();
    target = AttachCompositor(compositor, hwnd);
    scale = GetDpiForWindow(hwnd) / 96.f;
    gfx = std::make_unique<Gfx>(compositor, font, scale);
    BuildIsland();
    BuildMedia();
    target.Root(root);
    uiSettings = UISettings();
    uiSettings.ColorValuesChanged([](auto&&, auto&&) { PostMessageW(hwnd, WM_APP_SETTINGS, 0, 0); });
    uiSettings.AdvancedEffectsEnabledChanged([](auto&&, auto&&) { PostMessageW(hwnd, WM_APP_SETTINGS, 0, 0); });
    for (auto guid : {&GUID_POWER_SAVING_STATUS, &GUID_BATTERY_PERCENTAGE_REMAINING, &GUID_ACDC_POWER_SOURCE, &GUID_CONSOLE_DISPLAY_STATE,
                      &GUID_LIDSWITCH_STATE_CHANGE})
        RegisterPowerSettingNotification(hwnd, guid, DEVICE_NOTIFY_WINDOW_HANDLE);
    RegisterSuspendResumeNotification(hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);  // Modern Standby only tells windows that ask
    ApplySettings();
    scale = 0;  // forces Place to apply the monitor's scale
    Place("start");
    UpdateClock();
    modelTime = Now();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    // A window shown, hidden or cloaked on the primary without taking the focus (W4); the island's own are skipped.
    SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    UpdateAway();
    UpdateTimers();  // starts the cursor, usage and clock timers (and takes the first sample) unless the island starts unseen

    if (benchMode && !strcmp(benchMode, "startup")) {
        compositor.RequestCommitAsync().Completed([](auto&&, auto&&) {
            DwmFlush();  // committed; wait for DWM to compose it
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            WriteBench("%.0f\n", double((ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime));
            PostMessageW(hwnd, WM_CLOSE, 0, 0);  // this callback may run off the window's thread
        });
    } else if (benchMode && !strcmp(benchMode, "cycle")) {
        const char* toggle = getenv("ISLAND_BENCH_TOGGLE_MS");
        const char* secs = getenv("ISLAND_BENCH_SECONDS");
        SetTimer(hwnd, TimerBenchToggle, toggle ? atoi(toggle) : 1000, nullptr);
        SetTimer(hwnd, TimerBenchEnd, (secs ? atoi(secs) : 20) * 1000, nullptr);
        if (FILE* f = benchOut ? fopen(benchOut, "a") : nullptr) fclose(f);  // the compositor paces frames: no app frame times to report
    } else if (!benchMode) {
        AddTrayIcon();
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        WatchVolume(hwnd, WM_APP_VOLUME);
        WatchNetwork(hwnd, WM_APP_NET);
        WatchMedia(hwnd, WM_APP_MEDIA);
        CheckBattery();  // seeds the watcher; warns at once if already low on battery
        if (!InternetOnline()) SetTimer(hwnd, TimerNetDebounce, t.netLostDelayMs, nullptr);
        if (getenv("STATLET_DEMO")) SetTimer(hwnd, TimerDemo, 4000, nullptr);
    }

    MSG msg;
    int recoverTries = 0;
    for (;;) {
        HANDLE handles[3] = {gfx->deviceLost};  // then the settings folder and the Downloads folder, when they are watched
        DWORD count = 1, settingsAt = 0, downloadsAt = 0;
        if (settingsChanged != INVALID_HANDLE_VALUE) handles[settingsAt = count++] = settingsChanged;
        if (downloadChange != INVALID_HANDLE_VALUE) handles[downloadsAt = count++] = downloadChange;
        DWORD wait = MsgWaitForMultipleObjectsEx(count, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (downloadsAt && wait == WAIT_OBJECT_0 + downloadsAt) OnDownloadChange();
        if (settingsAt && wait == WAIT_OBJECT_0 + settingsAt) {  // a save often arrives as several writes: reload once they settle
            FindNextChangeNotification(settingsChanged);
            SetTimer(hwnd, TimerReload, 120, nullptr);
        }
        if (wait == WAIT_OBJECT_0) {
            bool ok = gfx->RecoverDevice();
            ++recoverTries;
            if (ok || recoverTries == 1 || recoverTries % 20 == 0) Log("device lost: recovery try %d %s", recoverTries, ok ? "ok" : "failed");
            if (ok) recoverTries = 0;
            else {
                Sleep(250);  // GPU still resetting: try again shortly, without spinning
                SetEvent(gfx->deviceLost);
            }
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return int(msg.wParam);
            DispatchMessageW(&msg);
        }
    }
}
