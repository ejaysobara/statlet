#pragma once
#include <cstdint>

// Every look value of the island, from the Island Design Spec (https://claude.ai/artifact/SSheiA7NfUQKoceC71focw).
// Sizes are effective pixels (epx); display scaling applies on top. The shared settings file overrides the user-facing
// ones (settings.h).
struct Rgba {
    uint8_t a, r, g, b;  // same order as winrt::Windows::UI::Color
};

constexpr Rgba Argb(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }

struct Tokens {
    // Shape and size
    float collapsedH = 34, collapsedR = 11;
    // Collapsed width fits what the rest line shows: content plus padding, rounded up to widthStep, kept in min..max
    float collapsedMinW = 124, collapsedMaxW = 260, collapsedPadX = 12, widthStep = 4;
    float expandedW = 368, expandedH = 168, expandedR = 22;
    float cardH = 104;                     // open height of takeover cards
    // Hovered rest view: CPU over RAM, each a labeled row (label, bar with a notch at warnAt, value)
    float statsW = 244, statsH = 68, statsPad = 14, statsRowH = 16, statsRowGap = 8, statsBarH = 6;
    float maxOpenW = 400, maxOpenH = 200;  // largest open size any widget may use; the fixed window is sized from it
    float topOffset = 10;                  // gap below the top of the primary work area

    // Material: dark acrylic in both Windows light and dark mode
    Rgba glassTint = Argb(0xFF1C1E24);
    float tintOpacity = 0.52f, luminosityOpacity = 0.65f;
    Rgba fallback = Argb(0xF01C1E24);  // transparency off, battery saver, or no backdrop
    Rgba edge = Argb(0x24FFFFFF);      // 1 epx inner stroke, white at 14%
    float edgeThickness = 1;
    Rgba shadow = Argb(0x47000000);    // 0, 8, 24 blur, black at 28%
    float shadowY = 8, shadowBlur = 24;

    // Color
    Rgba ink = Argb(0xFFF5F6F8);
    Rgba ink2 = Argb(0x9EF5F6F8);       // 62%
    Rgba track = Argb(0x33F5F6F8);      // 20%
    Rgba buttonFill = Argb(0x24F5F6F8); // 14%
    Rgba accentFallback = Argb(0xFF4CC2FF);  // also the user's own accent when followAccent is off
    bool followAccent = true;                // accent from the Windows accent color
    Rgba onAccent = Argb(0xFF101114);

    // CPU/RAM rest-line widget: usage colors, thresholds in percent, polling interval, and the sustained-high rule that opens the island
    Rgba warn = Argb(0xFFFFB020), critical = Argb(0xFFFF5A52);
    float warnAt = 80, critAt = 95, sustainAbove = 90, sustainSeconds = 30;
    bool openOnHighUsage = true;
    bool instantOpen = false;  // the "Instant" motion step: jump to size, as when Windows animations are off
    int statsPollMs = 2000;
    float restBarW = 26, restBarH = 4, restGap = 8;

    // Takeover widgets: how long each card stays (ms), battery thresholds in percent, the wait before "offline" shows, and the Focus length
    int takeoverVolumeMs = 1500, takeoverBatteryMs = 3500, takeoverNetMs = 3000, takeoverFocusMs = 4000;
    int batteryLowAt = 20, batteryCritAt = 10, netLostDelayMs = 3000, focusMinutes = 25;

    // Media (now playing). A song start or track change peeks for mediaPeekMs (art, title, artist, equalizer); afterwards the
    // rest line shows afterPeek: Tuck = art and equalizer beside the clock, Clock = plain rest line, Music = art, title and
    // equalizer in place of the clock. A paused player stays mediaHideMs. These two choices are user settings.
    enum class AfterPeek { Tuck, Clock, Music };
    enum class ProgressStyle { Wave, Line };
    AfterPeek afterPeek = AfterPeek::Tuck;
    ProgressStyle progressStyle = ProgressStyle::Wave;
    int mediaPeekMs = 4000, mediaHideMs = 30000;
    float mediaPadX = 7, musicMaxW = 210;                               // rest-line padding before the art; width cap for Music
    float artSmall = 22, artSmallR = 6, artLarge = 62, artLargeR = 14;  // album art, collapsed and open
    float eqBarW = 3, eqGap = 2, eqH = 14;                              // equalizer: 4 bars
    int eqLoopMs = 900;
    // Wavy progress: a stroke waving waveAmp either side of the centerline, one wave per waveLength, drifting one
    // wavelength per waveDriftMs while playing; it flattens when paused. The rest of the track is a flat line after a gap.
    float waveAmp = 3, waveLength = 32, waveStroke = 4, waveGap = 4;
    int waveDriftMs = 1600, waveFlattenMs = 240;
    float transportSize = 34, transportGap = 14;
    float mediaUsageW = 90;  // CPU and RAM either side of the transport: label, bar and value share this width

    // Typography (Manrope is bundled; Segoe UI Variable is the fallback)
    float typeTimerBig = 32, typeTitle = 16, typeTimerMini = 14, typeBody = 13, typeClock = 12, typeLabel = 12, typeCaption = 11;

    // Motion: one spring (mass 1) drives width, height and corner radius together
    float springStiffness = 420, springDamping = 34;
    float contentOutMs = 110, contentInMs = 140;
    int hoverPollMs = 40;

    // Open-state padding
    float padX = 18, padY = 16, rowGap = 12;
};
