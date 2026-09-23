#pragma once
// Minimal file-based diagnostic log for shipping builds.
//
// Goal: when RtkService silently fails (Defender quarantine, kdu -map BSOD
// recovery, non-elevated launch, driver device absent), the operator can
// look at %TEMP%\abi-run.log and see exactly which step went sideways
// without a debugger. Overwrites on every process start so it never grows.
//
// Not for release-post PR display — this leaves a plaintext trace on disk.
// Compiles out entirely when ABI_NO_RUNLOG is defined; keep on until we
// have a self-shipped in-process debug console.

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <windows.h>

namespace runlog {

inline HANDLE& handle() {
    static HANDLE h = INVALID_HANDLE_VALUE;
    return h;
}

// Set once by shutdown() so subsequent write() calls stay silent instead
// of re-opening the file via init(). Needed for destroy_self teardown.
inline bool& disabled_flag() {
    static bool b = false;
    return b;
}

// Path the current-session runlog resolved to. Empty until init() runs.
// Read by R3.7 boot-time sweep in self_destruct.cpp to skip the current
// file when glob-deleting prior sessions' orphan runlogs.
inline std::wstring& current_path() {
    static std::wstring p;
    return p;
}

inline void init() {
#ifndef ABI_NO_RUNLOG
    if (disabled_flag()) return;
    if (handle() != INVALID_HANDLE_VALUE) return;
    wchar_t path[MAX_PATH];
#ifdef ABI_DIAG_LOG_ROOT_C
    // Diagnostic build — route logs to C:\ root with clear names so a tester
    // finds them without hunting through %TEMP%. Per-pid suffix keeps parallel
    // launches from clobbering each other. Requires the target process to be
    // elevated (C:\ root writes need admin) — payload runs elevated post-UAC.
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"C:\\abi_diag_payload_%lu.log",
                 (unsigned long)GetCurrentProcessId());
#else
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH) return;
    // Randomised filename per boot — bulk-detect grep on the fixed literal
    // `abi-run.log` in %TEMP% misses. Prefix mimics real Windows temp-log
    // shapes so a manual review of %TEMP% doesn't stand out either.
    static const wchar_t* prefixes[] = {
        L"DXError", L"MpCmdRun", L"WPDNSE", L"WERD1F0", L"CBS",
        L"WinMSIPC", L"WU_E", L"DirectX", L"OneDrive", L"RtkNGUI",
    };
    LARGE_INTEGER li{}; QueryPerformanceCounter(&li);
    uint64_t seed = (uint64_t)li.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 16);
    auto rng = [&]() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(seed >> 32); };
    wchar_t leaf[64] = {};
    static const wchar_t hexchars[] = L"0123456789abcdef";
    _snwprintf_s(leaf, 64, _TRUNCATE, L"%s_%c%c%c%c%c%c.log",
                 prefixes[rng() % (sizeof(prefixes)/sizeof(prefixes[0]))],
                 hexchars[rng() % 16], hexchars[rng() % 16], hexchars[rng() % 16],
                 hexchars[rng() % 16], hexchars[rng() % 16], hexchars[rng() % 16]);
    wcsncat_s(path, MAX_PATH, leaf, _TRUNCATE);
#endif
    // TRUNCATE_EXISTING so operator always sees CURRENT run, no leftover noise.
    handle() = CreateFileW(path,
        FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle() != INVALID_HANDLE_VALUE) {
        current_path() = path;   // record so R3.7 sweep can skip us
    }
#endif
}

// Close the runlog handle so destroy_self can then delete the file.
// After this, LOG() becomes no-op (handle stays INVALID; init() would
// reopen but we're past shutdown point). Safe to call from any thread
// once no more LOG calls are expected.
inline void shutdown() {
#ifndef ABI_NO_RUNLOG
    HANDLE h = handle();
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
        handle() = INVALID_HANDLE_VALUE;
    }
    disabled_flag() = true;   // prevent write() from re-init'ing
#endif
}

inline void write(const char* fmt, ...) {
#ifndef ABI_NO_RUNLOG
    if (handle() == INVALID_HANDLE_VALUE) init();
    if (handle() == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st; GetLocalTime(&st);
    char buf[1024];
    int hdr = std::snprintf(buf, sizeof(buf),
        "%02u:%02u:%02u.%03u  ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap; va_start(ap, fmt);
    int n = std::vsnprintf(buf + hdr, sizeof(buf) - hdr - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    int total = hdr + n;
    if (total > (int)sizeof(buf) - 2) total = (int)sizeof(buf) - 2;
    buf[total++] = '\r';
    buf[total++] = '\n';

    DWORD written = 0;
    WriteFile(handle(), buf, (DWORD)total, &written, nullptr);
    FlushFileBuffers(handle());
#endif
}

}  // namespace runlog

// Macro must swallow args (not just call an empty function) — otherwise the
// format-string literals are still emitted to .rdata even with ABI_NO_RUNLOG.
// `((void)0)` doesn't evaluate __VA_ARGS__, so the literals become dead
// references and the linker drops them via /OPT:REF.
#ifdef ABI_NO_RUNLOG
#  define LOG(...) ((void)0)
#else
#  define LOG(...) ::runlog::write(__VA_ARGS__)
#endif
