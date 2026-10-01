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

// v1.0.38: session-scope disable flag. Set by AhUspaceMarkBroken() when
// reads start failing mid-session — indicates ACE ObRegisterCallbacks
// revoked PROCESS_VM_READ after ~10s (classic pattern). Once tripped
// USPACE stays off for the rest of this overlay process life; reader
// re-probe falls through to kdu path (survives VM_READ revoke because
// it uses physical memory + CR3 translation instead of NtReadVirtualMemory).
static BOOL                  g_uspace_disabled = FALSE;

BOOL AhUspaceDisabled(void) { return g_uspace_disabled; }

void AhUspaceMarkBroken(void) {
    // Callers already log via ah_diag; no local diag call needed here.
    g_uspace_disabled = TRUE;
    if (g_hproc) { CloseHandle(g_hproc); g_hproc = NULL; }
    g_pid = 0; g_base = 0;
}

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

// v1.0.36: HARD WHITELIST. Exact basename match, no substring fuzz. The
// substring matcher used up to v1.0.35 collided on 'arena_breakout' because
// arena_breakout_infinite_launcher.exe contains it — is_launcher_or_wrapper
// caught the specific case but any future launcher with a different name
// (arena_breakout_setup.exe, arena_breakout_installer.exe, ...) would slip
// through. Exact whitelist ends the whole class of bug.
static BOOL is_target_process(const wchar_t* exe) {
    if (!exe) return FALSE;
    static const wchar_t* WHITELIST[] = {
        L"UAGame.exe",           // Global retail Tencent build (primary)
        L"UAGameShipping.exe",   // CN / dev build variant
        NULL
    };
    for (int i = 0; WHITELIST[i]; i++) {
        if (_wcsicmp(exe, WHITELIST[i]) == 0) return TRUE;
    }
    return FALSE;
}

static DWORD find_pid_via_toolhelp(const char* procName) {
    (void)procName;   // v1.0.36: whitelist is fixed, procName parameter is ignored
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    DWORD hit = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (is_target_process(pe.szExeFile)) {
                hit = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return hit;
}

// v1.0.36: fetch both base and size in one Toolhelp pass. UAGame.exe has
// SizeOfImage in the 200-500 MB range; launchers are <10 MB. Size gate is
// the second belt (base < 4GB gate is the first).
static BOOL module_info_via_toolhelp(DWORD pid, u64* out_base, u64* out_size) {
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W me = { .dwSize = sizeof(me) };
    BOOL ok = FALSE;
    if (Module32FirstW(snap, &me)) {
        // First module = main executable in every Windows version.
        if (out_base) *out_base = (u64)(uintptr_t)me.modBaseAddr;
        if (out_size) *out_size = (u64)me.modBaseSize;
        ok = TRUE;
    }
    CloseHandle(snap);
    return ok;
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

    // First belt (v1.0.36): IsWow64Process. UE4 shipping (UAGame.exe) is
    // always x64 native. arena_breakout_infinite_launcher.exe is a 32-bit
    // .NET loader running under WOW64. Field data: Task Manager shows it
    // as "(32 bit)". Any WOW64 process cannot be UAGame — reject outright.
    // This is the strongest single gate; the belts below stay as belt+
    // suspenders.
    {
        BOOL wow64 = FALSE;
        if (IsWow64Process(h, &wow64) && wow64) {
            diag_log("USPACE: REJECT PID=%lu WOW64 32-bit process (not UAGame x64)", pid);
            CloseHandle(h);
            return FALSE;
        }
    }

    // v1.0.38.1: Module32-fallback. ACE strips
    // CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) access on the game
    // process — every Module32FirstW returns base=0/size=0. We used to
    // reject on that, which killed USPACE on every attach.
    //
    // Falling back to fixed UE4 shipping base 0x140000000: the MZ probe
    // below still validates that we're pointed at a real UAGame image;
    // the IsWow64 gate above already killed 32-bit wrappers; and Toolhelp
    // process-name whitelist (UAGame.exe / UAGameShipping.exe) locked
    // the identity before we got here. Losing the SizeOfImage sanity
    // check is a tolerable trade — no size-legit-but-wrong-process
    // scenario survives the previous belts.
    u64 base = 0, msize = 0;
    (void)module_info_via_toolhelp(pid, &base, &msize);
    if (!base) {
        diag_log("USPACE: Module32 access stripped by ACE — using fixed UE4 base 0x140000000, MZ probe will validate");
        base = 0x140000000ULL;
        msize = 0;
    }

    // UE4 shipping links at 0x140000000. Anything below 4 GB is a
    // wrapper (arena_breakout_infinite_launcher.exe sits at 0x400000).
    if (base < 0x100000000ULL) {
        diag_log("USPACE: REJECT wrapper PID=%lu base=0x%llX (< 4GB)",
                 pid, (unsigned long long)base);
        CloseHandle(h);
        return FALSE;
    }

    // SizeOfImage sanity ONLY if we got a real value from Module32.
    // When Module32 was stripped (msize=0) we skip this belt — MZ probe
    // below is enough to validate the image identity.
    if (msize > 0 && msize < 100ULL * 1024ULL * 1024ULL) {
        diag_log("USPACE: REJECT PID=%lu ModSize=%llu MB (< 100 MB, not UAGame)",
                 pid, (unsigned long long)(msize / (1024ULL * 1024ULL)));
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
    diag_log("USPACE: ATTACH OK pid=%lu base=0x%llX size=%llu MB (UAGame confirmed)",
             pid, (unsigned long long)base,
             (unsigned long long)(msize / (1024ULL * 1024ULL)));
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
