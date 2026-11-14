#pragma once
#include <string>
#include <vector>
#include <windows.h>

// The rest line's date and the time samples the day-fitted pill is sized to (v2 item 5, ADR-0002). Header-only for the tests.

// "Thu Nov 13" (weekday) or "Nov 13", for `when` or today when null, in the user's locale.
inline std::wstring RestDate(const SYSTEMTIME* when, bool weekday, const wchar_t* locale = LOCALE_NAME_USER_DEFAULT) {
    wchar_t buf[32];
    GetDateFormatEx(locale, 0, when, weekday ? L"ddd MMM d" : L"MMM d", buf, 32, nullptr);
    return buf;
}

// Just the weekday ("Thu"): what the date shrinks to when the status slot leaves no room for the whole date.
inline std::wstring ShortDay(const SYSTEMTIME* when, const wchar_t* locale = LOCALE_NAME_USER_DEFAULT) {
    wchar_t buf[32];
    GetDateFormatEx(locale, 0, when, L"ddd", buf, 32, nullptr);
    return buf;
}

// The clock's time at minute 00 and minute 59 of every hour: every text the time can take, for measuring the widest.
inline std::vector<std::wstring> WidestTimeSamples() {
    std::vector<std::wstring> out;
    for (WORD h = 0; h < 24; ++h)
        for (WORD m : {WORD(0), WORD(59)}) {
            SYSTEMTIME st{};
            st.wHour = h;
            st.wMinute = m;
            wchar_t buf[64];
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 64);
            out.push_back(buf);
        }
    return out;
}
