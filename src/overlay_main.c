// arenahack overlay entry — delegates to full ABIFINAL abi::Overlay stack
// dropped into src/abi_ui/. Menu = abi::render_control_panel(cfg) each frame.
// C++ bootstrap lives in overlay_boot.cpp.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

extern void DhInitHardening(void);   // src/hardening/dh_amsi_etw.c
extern int  AhOverlayRun(void);

// v0.9.455: last-chance SEH filter. Any unhandled exception in overlay/render/
// reader (heap corruption, NULL deref, D3D fail) would otherwise land in
// WerFault → exit_code=0x40010004 with an empty crash_marker. This handler
// writes SEH_UNHANDLED (=15) to %TEMP%\.abi_lc so the parent launcher_stub's
// crash_upload_after_child() sees a non-empty marker and we can distinguish
// unhandled-exception deaths from Task Manager kills in server-side reports.
// Ends by returning EXCEPTION_EXECUTE_HANDLER so the process terminates
// cleanly with the actual exception code as exit status.
static LONG WINAPI ah_overlay_unhandled_seh(EXCEPTION_POINTERS* ep) {
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

int wmain(int argc, wchar_t** argv) {
    (void)argc; (void)argv;
    // FIRST — anti-debug + AMSI/ETW patch before anything else runs.
    // Silent ExitProcess if a debugger is attached.
    DhInitHardening();
    // v0.9.455: install last-chance SEH filter BEFORE any other init runs so
    // even a fault inside AhOverlayRun's setup path lands here instead of
    // WerFault.
    SetUnhandledExceptionFilter(ah_overlay_unhandled_seh);
    // NO printf — spawned from detached/no-console launcher, stdout invalid.
    return AhOverlayRun();
}
