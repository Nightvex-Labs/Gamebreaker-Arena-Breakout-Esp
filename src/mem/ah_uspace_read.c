// ah_uspace_read.c — Toolhelp32 + NtOpenProcess + ReadProcessMemory attach
// path. See ah_uspace_read.h for rationale.
//
// Direct-syscall path is NOT used here on purpose: we are the OVERLAY
// process, not the game. ACE does not hook OUR ntdll (the overlay isn't
// UAGame — ACE's in-process protection stays in the game). Straight
// GetProcAddress + NtOpenProcess + ReadProcessMemory work.

#include "../../inc/ah_uspace_read.h"
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "Advapi32.lib")

typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0)

typedef NTSTATUS (NTAPI *pNtReadVirtualMemory)(
    HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
    SIZE_T NumberOfBytesToRead, PSIZE_T NumberOfBytesRead);

static HANDLE                g_hproc = NULL;
static u64                   g_base  = 0;
static DWORD                 g_pid   = 0;
static pNtReadVirtualMemory  g_ntrvm = NULL;

static void diag_log(const char* fmt, ...) {
#ifdef AH_DIAG
    HANDLE h = CreateFileA("C:\\Users\\Public\\ah_procs.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 2;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
#else
    (void)fmt;
#endif
}

// Try to enable SeDebugPrivilege so OpenProcess(PROCESS_VM_READ) succeeds
// on games that filter the standard access mask. Overlay launcher runs
// elevated, so this typically succeeds. Best-effort; safe to fail.
static void enable_debug_privilege(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    LUID luid;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp = {0};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(tok);
}

// Multi-name substring matcher — same names accepted by the kdu-side
// RpmFindProcess so users on Steam distribution (arena_breakout.exe) or
// CN/dev builds (UAGameShipping.exe) still attach with the same overlay.
static BOOL name_matches(const wchar_t* exe, const char* primary) {
    static const char* ALT[] = {
        "arena_breakout",
        "UAGameShipping",
        NULL,
    };
    // Lowercase compare, substring, length-capped to 14 (Toolhelp gives us
    // full basename so we don't have the 15-char EPROCESS truncation issue).
    // Primary: first 6 chars — covers "UAGame" prefix for all UAGame*.exe.
    if (!exe || !primary) return FALSE;
    size_t elen = wcslen(exe);

    const char* cands[4] = { primary, NULL, NULL, NULL };
    size_t      clens[4] = { 0 };
    clens[0] = strlen(primary); if (clens[0] > 6) clens[0] = 6;
    int cn = 1;
    for (int i = 0; ALT[i] && cn < 4; i++) {
        cands[cn] = ALT[i];
        clens[cn] = strlen(ALT[i]);
        if (clens[cn] > 14) clens[cn] = 14;
        cn++;
    }
    for (int ci = 0; ci < cn; ci++) {
        size_t cl = clens[ci];
        if (elen < cl) continue;
        for (size_t i = 0; i + cl <= elen; i++) {
            int ok = 1;
            for (size_t k = 0; k < cl; k++) {
                wchar_t w = exe[i + k];
                char    c = cands[ci][k];
                if (w >= L'A' && w <= L'Z') w = (wchar_t)(w + 32);
                if (c >= 'A'  && c <= 'Z')  c = (char)(c + 32);
                if ((int)w != (int)(unsigned char)c) { ok = 0; break; }
            }
            if (ok) return TRUE;
        }
    }
    return FALSE;
}

// v1.0.34.1: reject known launcher/wrapper process names outright. The
// substring matcher would happily latch onto arena_breakout_infinite_launcher
// which contains "arena_breakout" but is a small .NET loader — its module
// base is at 0x400000, not 0x140000000, and every read into the real game's
// VA space returns garbage.
static BOOL is_launcher_or_wrapper(const wchar_t* exe) {
    if (!exe) return FALSE;
    static const wchar_t* BAD[] = {
        L"launcher",     // arena_breakout_infinite_launcher.exe, NightvexLauncher.exe
        L"bootstrap",
        L"updater",
        L"crashhandler",
        NULL
    };
    for (int i = 0; BAD[i]; i++) {
        size_t blen = wcslen(BAD[i]);
        size_t elen = wcslen(exe);
        if (elen < blen) continue;
        for (size_t j = 0; j + blen <= elen; j++) {
            int ok = 1;
            for (size_t k = 0; k < blen; k++) {
                wchar_t a = exe[j + k], b = BAD[i][k];
                if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
                if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
                if (a != b) { ok = 0; break; }
            }
            if (ok) return TRUE;
        }
    }
    return FALSE;
}

static DWORD find_pid_via_toolhelp(const char* procName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    DWORD hit = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (is_launcher_or_wrapper(pe.szExeFile)) continue;
            if (name_matches(pe.szExeFile, procName)) {
                hit = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return hit;
}

static u64 module_base_via_toolhelp(DWORD pid, const char* procName) {
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = { .dwSize = sizeof(me) };
    u64 base = 0;
    if (Module32FirstW(snap, &me)) {
        // First module = main executable in every Windows version.
        base = (u64)(uintptr_t)me.modBaseAddr;
        (void)procName;
    }
    CloseHandle(snap);
    return base;
}

BOOL AhUspaceAttach(const char* procName, DWORD* out_pid, u64* out_base) {
    if (g_hproc) AhUspaceDetach();
    enable_debug_privilege();

    DWORD pid = find_pid_via_toolhelp(procName ? procName : "UAGame.exe");
    if (!pid) {
        diag_log("USPACE: Toolhelp did not find %s", procName ? procName : "UAGame.exe");
        return FALSE;
    }

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if (!h) {
        diag_log("USPACE: OpenProcess PID=%lu failed gle=%lu", pid, GetLastError());
        return FALSE;
    }

    u64 base = module_base_via_toolhelp(pid, procName);
    if (!base) {
        diag_log("USPACE: Module32 base=0 for PID=%lu", pid);
        CloseHandle(h);
        return FALSE;
    }

    // v1.0.34.1: reject wrappers. UE4 shipping build (UAGame.exe) links at
    // 0x140000000 and stays high even with ASLR — always >= 4 GB.
    // arena_breakout_infinite_launcher and similar .NET loaders sit at
    // 0x400000 (2 GB below). Same filter as the kdu path uses. Without
    // this the reader latches on the launcher, every gworld/pawn read
    // hits unmapped VA, and CR3-STALE watchdog re-attaches forever.
    if (base < 0x100000000ULL) {
        diag_log("USPACE: REJECT wrapper PID=%lu ImageBase=0x%llX (too low, not UE4 game)",
                 pid, (unsigned long long)base);
        CloseHandle(h);
        return FALSE;
    }

    // Resolve NtReadVirtualMemory once. ReadProcessMemory works too but
    // wraps NtReadVirtualMemory; direct call skips one frame and avoids
    // the SetLastError overhead on hot reads.
    if (!g_ntrvm) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) {
            g_ntrvm = (pNtReadVirtualMemory)GetProcAddress(nt, "NtReadVirtualMemory");
        }
    }

    g_hproc = h;
    g_pid   = pid;
    g_base  = base;

    // Probe read at module base to confirm VM_READ wasn't stripped.
    uint16_t mz = 0;
    if (!AhUspaceRead(base, &mz, 2) || mz != 0x5A4D) {
        diag_log("USPACE: MZ probe failed at base=0x%llX (mz=0x%04X) — read stripped",
                 (unsigned long long)base, mz);
        AhUspaceDetach();
        return FALSE;
    }

    if (out_pid)  *out_pid  = pid;
    if (out_base) *out_base = base;
    diag_log("USPACE: ATTACH OK pid=%lu base=0x%llX", pid, (unsigned long long)base);
    return TRUE;
}

BOOL AhUspaceRead(u64 va, void* dst, uint32_t n) {
    if (!g_hproc || !dst || !n) return FALSE;
    SIZE_T got = 0;
    if (g_ntrvm) {
        NTSTATUS st = g_ntrvm(g_hproc, (PVOID)(uintptr_t)va, dst, n, &got);
        return (st == STATUS_SUCCESS) && (got == n);
    }
    return ReadProcessMemory(g_hproc, (LPCVOID)(uintptr_t)va, dst, n, &got)
           && got == n;
}

void AhUspaceDetach(void) {
    if (g_hproc) {
        CloseHandle(g_hproc);
        g_hproc = NULL;
    }
    g_pid  = 0;
    g_base = 0;
}

BOOL AhUspaceOk(void) {
    return g_hproc != NULL;
}

DWORD AhUspacePid(void) {
    return g_pid;
}

u64 AhUspaceBase(void) {
    return g_base;
}

BOOL AhUspaceVerify(void) {
    if (!g_hproc || !g_base) return FALSE;
    uint16_t mz = 0;
    return AhUspaceRead(g_base, &mz, 2) && mz == 0x5A4D;
}
