#pragma once
// crash_marker — persistent one-byte "why did we die" marker.
//
// Written *just before* any silent process death (antidbg, watchdog,
// heartbeat fail, terminate_handler, DEVICE_REMOVED, etc.). Read once at
// the top of wWinMain and immediately deleted, so subsequent runs see
// only the code from the last death.
//
// Works even when RUNLOG is off (prod build): writes 1 byte to
// %TEMP%\.abi_lc, sets FILE_ATTRIBUTE_HIDDEN, no runlog dependency.
// Cost: a single CreateFileW+WriteFile at death time.
//
// Read at boot goes through runlog::write() so it shows up in the diag
// log for testers who have the diag build; in prod builds it's captured
// but nowhere-emitted (only useful when we hand them a diag build).

#include <cstdint>
#include <windows.h>

namespace crash_marker {

enum Reason : uint8_t {
    NONE                = 0,
    ANTIDBG_ISDBGPRESENT = 1,
    ANTIDBG_CHECKREMOTE  = 2,
    ANTIDBG_PEB_FLAGS    = 3,
    ANTIDBG_DEBUG_PORT   = 4,
    ANTIDBG_HW_BP        = 5,
    ANTIDBG_RDTSC        = 6,
    UAGAME_WATCHDOG_EXIT = 7,
    GUARD_HEARTBEAT_FAIL = 8,
    GUARD_VALIDATE_FAIL  = 9,
    READER_AV            = 10,
    OVERLAY_AV           = 11,
    OVERLAY_DEVICE_LOST  = 12,
    TERMINATE_HANDLER    = 13,
    SELFCHECK_INTEGRITY  = 14,
    SEH_UNHANDLED        = 15,
};

inline void marker_path(wchar_t out[MAX_PATH]) {
    // %TEMP%\.abi_lc — leading dot keeps it hidden from casual dir
    // listings, name is ambiguous (looks like a lock/cache stub).
    DWORD n = GetTempPathW(MAX_PATH, out);
    if (n == 0 || n >= MAX_PATH) { out[0] = 0; return; }
    wcsncat_s(out, MAX_PATH, L".abi_lc", _TRUNCATE);
}

// Called from any death path immediately before TerminateProcess/_Exit.
// Best-effort: never throws, never blocks, ignores all errors.
inline void write(uint8_t reason) {
    wchar_t path[MAX_PATH]; marker_path(path);
    if (!path[0]) return;
    HANDLE h = CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, &reason, 1, &w, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

// Called once from wWinMain top. Returns 0 (NONE) if there was no
// prior death or the marker was unreadable, else the recorded reason.
// Always deletes the marker after reading so we never confuse two runs.
inline uint8_t read_and_clear() {
    wchar_t path[MAX_PATH]; marker_path(path);
    if (!path[0]) return 0;
    HANDLE h = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint8_t b = 0; DWORD n = 0;
    ReadFile(h, &b, 1, &n, nullptr);
    CloseHandle(h);
    DeleteFileW(path);
    return (n == 1) ? b : 0;
}

inline const char* reason_name(uint8_t r) {
    switch (r) {
        case ANTIDBG_ISDBGPRESENT: return "antidbg::IsDebuggerPresent";
        case ANTIDBG_CHECKREMOTE:  return "antidbg::CheckRemoteDebuggerPresent";
        case ANTIDBG_PEB_FLAGS:    return "antidbg::peb_flags (NtGlobalFlag)";
        case ANTIDBG_DEBUG_PORT:   return "antidbg::debug_port (NtQueryInformationProcess)";
        case ANTIDBG_HW_BP:        return "antidbg::hw_bp (Dr0-3)";
        case ANTIDBG_RDTSC:        return "antidbg::rdtsc";
        case UAGAME_WATCHDOG_EXIT: return "uagame_watchdog::game_exited";
        case GUARD_HEARTBEAT_FAIL: return "guard::heartbeat_fail_closed";
        case GUARD_VALIDATE_FAIL:  return "guard::validate_fail_closed";
        case READER_AV:            return "reader::access_violation";
        case OVERLAY_AV:           return "overlay::access_violation";
        case OVERLAY_DEVICE_LOST:  return "overlay::device_removed";
        case TERMINATE_HANDLER:    return "std::terminate_handler";
        case SELFCHECK_INTEGRITY:  return "selfcheck::integrity_error";
        case SEH_UNHANDLED:        return "SEH::unhandled_exception";
        default:                   return "none";
    }
}

}  // namespace crash_marker
