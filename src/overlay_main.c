// arenahack overlay entry — delegates to full ABIFINAL abi::Overlay stack
// dropped into src/abi_ui/. Menu = abi::render_control_panel(cfg) each frame.
// C++ bootstrap lives in overlay_boot.cpp.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <dbghelp.h>
#include "../inc/ah_test_trace.h"    // TEST-REMOVE: instrumentation

#pragma comment(lib, "Dbghelp.lib")

extern void DhInitHardening(void);   // src/hardening/dh_amsi_etw.c
extern int  AhOverlayRun(void);

// v1.0.24: Vectored Exception Handler — catches faults BEFORE SEH filter,
// including fast-fails (RaiseFailFastException, /GS cookie, CFG violation,
// heap corruption) which bypass SEH entirely. Writes a text crash summary
// plus a full MiniDump to %TEMP% so the launcher stub picks them up and
// ships to koenflow.com telemetry. This is the only path to see WHY
// marker=0 crashes happen (the SEH filter never runs for fast-fails).
static LONG CALLBACK veh_crash_dump(EXCEPTION_POINTERS* ep) {
    // TEST-REMOVE: fire on ANY VEH entry, even benign ones — we want to see
    // every exception the process observes, filtered or not.
    if (ep && ep->ExceptionRecord) {
        ah_test_trace_write("VEH ENTER code=0x%08lX flags=0x%lX addr=%p",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            (unsigned long)ep->ExceptionRecord->ExceptionFlags,
            ep->ExceptionRecord->ExceptionAddress);
    } else {
        ah_test_trace_write("VEH ENTER (null ep)");
    }
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Only serious crashes — skip C++ exceptions (0xE06D7363), debug breaks,
    // stack single-steps, etc. that don't kill the process.
    switch (code) {
        case 0xC0000005:  // ACCESS_VIOLATION
        case 0xC0000006:  // IN_PAGE_ERROR
        case 0xC00000FD:  // STACK_OVERFLOW
        case 0xC0000374:  // HEAP_CORRUPTION
        case 0xC0000409:  // STACK_BUFFER_OVERRUN (fast-fail bucket)
        case 0xC000041D:  // FATAL_USER_CALLBACK_EXCEPTION
        case 0xC0000420:  // ASSERTION_FAILURE
        case 0xC0000602:  // FAIL_FAST_EXCEPTION
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

    // Best-effort — nothing here should throw or block long. Reentrancy is
    // avoided by TerminateProcess at the end (VEH runs before OS teardown).
    wchar_t tmp[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn >= MAX_PATH - 32) TerminateProcess(GetCurrentProcess(), code);

    // 1. Write text crash-meta: exception code, RIP, thread id, first
    //    ~8 stack frames (via RtlCaptureStackBackTrace), module base for RIP.
    {
        wchar_t meta_path[MAX_PATH];
        wcscpy_s(meta_path, MAX_PATH, tmp);
        wcsncat_s(meta_path, MAX_PATH, L".abi_crash_meta", _TRUNCATE);
        HANDLE mh = CreateFileW(meta_path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
        if (mh != INVALID_HANDLE_VALUE) {
            char meta[2048];
            int mn = _snprintf_s(meta, sizeof(meta), _TRUNCATE,
                "code=0x%08lX\r\nflags=0x%08lX\r\nrip=0x%llX\r\ntid=%lu\r\npid=%lu\r\n",
                (unsigned long)code,
                (unsigned long)ep->ExceptionRecord->ExceptionFlags,
                (unsigned long long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,
                GetCurrentThreadId(), GetCurrentProcessId());
            // Backtrace (best-effort).
            void* frames[16];
            USHORT nf = RtlCaptureStackBackTrace(0, 16, frames, NULL);
            for (USHORT i = 0; i < nf && mn < (int)sizeof(meta) - 24; i++) {
                int adv = _snprintf_s(meta + mn, sizeof(meta) - mn, _TRUNCATE,
                    "bt[%d]=0x%llX\r\n", (int)i, (unsigned long long)(uintptr_t)frames[i]);
                if (adv > 0) mn += adv;
            }
            // Module for RIP.
            HMODULE hmod = NULL;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hmod) && hmod) {
                wchar_t mn2[MAX_PATH];
                if (GetModuleFileNameW(hmod, mn2, MAX_PATH)) {
                    char utf8[MAX_PATH * 2];
                    int ul = WideCharToMultiByte(CP_UTF8, 0, mn2, -1,
                                                 utf8, sizeof(utf8), NULL, NULL);
                    if (ul > 0 && mn < (int)sizeof(meta) - ul - 16) {
                        mn += _snprintf_s(meta + mn, sizeof(meta) - mn, _TRUNCATE,
                                          "mod=%s\r\nmod_base=0x%llX\r\n",
                                          utf8, (unsigned long long)(uintptr_t)hmod);
                    }
                }
            }
            DWORD w = 0; WriteFile(mh, meta, (DWORD)mn, &w, NULL);
            FlushFileBuffers(mh);
            CloseHandle(mh);
        }
    }

    // 2. Full MiniDump — thread stacks + module list. Enough to reconstruct
    //    any crash off-line in WinDbg with matching PDBs. Size ~200-500 KB.
    {
        wchar_t dmp_path[MAX_PATH];
        wcscpy_s(dmp_path, MAX_PATH, tmp);
        wcsncat_s(dmp_path, MAX_PATH, L".abi_crash_dump.dmp", _TRUNCATE);
        HANDLE dh = CreateFileW(dmp_path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
        if (dh != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers    = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                dh,
                MiniDumpWithThreadInfo | MiniDumpWithHandleData |
                MiniDumpWithFullMemoryInfo,
                &mei, NULL, NULL);
            CloseHandle(dh);
        }
    }

    // Also write the SEH marker so the parent stub still gets a signal even
    // if the SEH filter downstream never fires (fast-fails skip it).
    {
        wchar_t seh_path[MAX_PATH];
        wcscpy_s(seh_path, MAX_PATH, tmp);
        wcsncat_s(seh_path, MAX_PATH, L".abi_lc", _TRUNCATE);
        HANDLE sh = CreateFileW(seh_path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
        if (sh != INVALID_HANDLE_VALUE) {
            uint8_t byte = 15;   // crash_marker::SEH_UNHANDLED
            DWORD w = 0;
            WriteFile(sh, &byte, 1, &w, NULL);
            FlushFileBuffers(sh);
            CloseHandle(sh);
        }
    }

    // Continue exception processing — SEH filter still gets a shot for
    // logging, then process terminates with the exception code as exit
    // status. For fast-fails Windows will terminate anyway; our writes
    // above already landed.
    return EXCEPTION_CONTINUE_SEARCH;
}

// v0.9.455: last-chance SEH filter. Any unhandled exception in overlay/render/
// reader (heap corruption, NULL deref, D3D fail) would otherwise land in
// WerFault → exit_code=0x40010004 with an empty crash_marker. This handler
// writes SEH_UNHANDLED (=15) to %TEMP%\.abi_lc so the parent launcher_stub's
// crash_upload_after_child() sees a non-empty marker and we can distinguish
// unhandled-exception deaths from Task Manager kills in server-side reports.
// Ends by returning EXCEPTION_EXECUTE_HANDLER so the process terminates
// cleanly with the actual exception code as exit status.
static LONG WINAPI ah_overlay_unhandled_seh(EXCEPTION_POINTERS* ep) {
    // TEST-REMOVE: trace immediately so we see the SEH filter got invoked
    // even if subsequent WriteFile of marker fails.
    if (ep && ep->ExceptionRecord) {
        ah_test_trace_write("SEH FILTER ENTER code=0x%08lX addr=%p",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    } else {
        ah_test_trace_write("SEH FILTER ENTER (null ep)");
    }
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n && n < MAX_PATH) {
        wcsncat_s(path, MAX_PATH, L".abi_lc", _TRUNCATE);
        HANDLE h = CreateFileW(path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            uint8_t byte = 15;   // crash_marker::SEH_UNHANDLED
            DWORD w = 0;
            WriteFile(h, &byte, 1, &w, NULL);
            FlushFileBuffers(h);
            CloseHandle(h);
        }
    }
    (void)ep;
    return EXCEPTION_EXECUTE_HANDLER;
}

// v1.0.38.15 state-leak fix: register provider teardown with CRT atexit so
// EVERY normal return path from wmain triggers stop+delete of the kdu SCM
// service + driver image unload + .sys blob delete. Covers: (1) wmain
// returning rv from AhOverlayRun, (2) C runtime exit() on main thread, (3)
// any detached reader thread that didn't reach its own cleanup before
// process termination. ExitProcess() does NOT invoke atexit — those paths
// (GWORLD-STUCK self-restart) already call DhProviderShutdownAll directly.
extern void DhProviderShutdownAll(void);
static void ah_atexit_teardown(void) {
    ah_test_trace_write("ah_atexit_teardown: provider shutdown");   // TEST-REMOVE
    DhProviderShutdownAll();
}

int wmain(int argc, wchar_t** argv) {
    (void)argc; (void)argv;
    ah_test_trace_write("wmain ENTER argc=%d", argc);   // TEST-REMOVE
    atexit(ah_atexit_teardown);
    // v1.0.24: VEH FIRST — before hardening, before SEH filter. VEH is the
    // only handler that catches fast-fails (RaiseFailFastException, /GS
    // cookie, CFG violation, heap corruption). These bypass SEH entirely,
    // so any of them without VEH = marker=0 with zero forensic evidence.
    AddVectoredExceptionHandler(1 /* CALL_FIRST */, veh_crash_dump);
    ah_test_trace_write("wmain VEH installed");   // TEST-REMOVE

    // Anti-debug + AMSI/ETW patch. Silent ExitProcess if a debugger is
    // attached. If ntdll patching AVs (Win11 25H2 HVCI edge case), VEH
    // above will catch it and drop a dump before the process dies.
    ah_test_trace_write("wmain calling DhInitHardening");   // TEST-REMOVE
    DhInitHardening();
    ah_test_trace_write("wmain DhInitHardening returned");   // TEST-REMOVE

    // v0.9.455: install last-chance SEH filter for exceptions that DO go
    // through normal exception dispatching (not fast-fails). Writes marker=15.
    SetUnhandledExceptionFilter(ah_overlay_unhandled_seh);
    ah_test_trace_write("wmain SEH filter installed, calling AhOverlayRun");   // TEST-REMOVE
    // NO printf — spawned from detached/no-console launcher, stdout invalid.
    int rv = AhOverlayRun();
    ah_test_trace_write("wmain AhOverlayRun returned rv=%d — normal exit", rv);   // TEST-REMOVE
    return rv;
}
