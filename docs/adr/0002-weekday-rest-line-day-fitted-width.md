# ADR-0002: Weekday on the rest line with a day-fitted pill width

**Date**: 2026-11-13
**Status**: accepted
**Deciders**: Ejay (owner), v2 Architect thread

## Context

v2 item 5 puts the weekday beside the clock. The rest line is one text run (`MMM d  h:mm tt`) and the collapsed pill is fitted to it every minute. v2 had a rule that the pill size must not change. Adding the weekday in front of the date makes the pill 24 px wider, and with music the line must also hold the album art, equalizer and CPU/RAM bars. Measured over every date and minute (Segoe UI Variable 12 px, en-US), the date part ranges 39 to 66 px and the time 39 to 48 px.

## Decision

The rest line shows `ddd MMM d  h:mm tt` (e.g. `Thu Nov 13  5:56 AM`), also beside the art and equalizer, behind the setting `showWeekday` (default on). The pill width is day-fitted: the day's date plus the widest time of any minute, so it is 152 to 180 px (clock only) or 200 to 224 px (with music), and changes only at midnight. Ejay lifted the no-resize rule for this item.

## Alternatives Considered

### Weekday replaces the month (`Thu 13`)
- **Pros**: pill size unchanged.
- **Cons**: loses the month.
- **Why not**: Ejay chose the full date.

### One fixed width (180 / 224 px)
- **Pros**: the pill never changes size.
- **Cons**: up to 28 px of empty space on short days (`Fri Jul 3`).
- **Why not**: looks loose; Ejay wanted every day to fit snugly.

### Fitted per minute (today's rule)
- **Pros**: no new code.
- **Cons**: the width springs during the day (e.g. 9:59 to 10:00).
- **Why not**: visible twitch on a status pill.

## Consequences

### Positive
- The weekday and the full date are always visible, music or not.
- No width change during the day; no new timers.

### Negative
- The pill is wider than v1: up to 180 px (was 148) and 224 px with music (was 152).
- `CollapsedWidth()` gains a measured "widest time" term, re-measured on settings and DPI changes.

### Risks
- Long localized abbreviations can hit the 260 px cap and clip. Mitigation: the existing clamp, plus a DirectWrite width test under en-US.

See `docs/v2-weekday-architecture.md` and `docs/mockups/weekday-day-fitted.png`.
