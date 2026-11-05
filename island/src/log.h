#pragma once
// One-line event log at %LOCALAPPDATA%\Statlet\island.log, for finding out why the island went blank. Rare events
// only (no per-frame lines); the file keeps its last 256 KB. Never throws: a failed write is dropped.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>

inline wchar_t logPath[MAX_PATH];  // empty until LogOpen: tests and benchmarks log nowhere
constexpr long LogCap = 256 * 1024;

// Keeps the file's last `keep` bytes, starting at a line.
inline void LogTrim(long keep) noexcept {
    FILE* f = _wfopen(logPath, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= keep) return (void)fclose(f);
    std::string tail;
    try {
        tail.resize(size_t(keep));
    } catch (...) {
        return (void)fclose(f);
    }
    fseek(f, size - keep, SEEK_SET);
    tail.resize(fread(tail.data(), 1, tail.size(), f));
    fclose(f);
    size_t nl = tail.find('\n');
    if (FILE* w = _wfopen(logPath, L"wb")) {
        if (nl != std::string::npos) fwrite(tail.data() + nl + 1, 1, tail.size() - nl - 1, w);
        fclose(w);
    }
}

inline void LogOpen(const wchar_t* dir) noexcept {
    if (swprintf(logPath, MAX_PATH, L"%s\\island.log", dir) < 0) logPath[0] = 0;
    LogTrim(LogCap);
}

inline void Log(const char* fmt, ...) noexcept {
    if (!logPath[0]) return;
    char line[600];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, sizeof line - n - 1, fmt, args);
    va_end(args);
    FILE* f = _wfopen(logPath, L"ab");
    if (!f) return;
    fprintf(f, "%s\n", line);
    long size = ftell(f);
    fclose(f);
    if (size > LogCap) LogTrim(LogCap * 3 / 4);  // trims in steps, so a long session does not rewrite on every line
}
