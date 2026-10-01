// TEST-REMOVE: whole file — heavy instrumentation for tomorrow's crash-hunt.
// Standalone log writer independent of ah_diag so any TU (reader C++, overlay
// C++, launcher C, hardening C) can drop TEST_TRACE calls without linkage
// gymnastics. Log path: C:\Users\Public\ah_test_trace.log. Grep after the
// hunt: `grep -rn "TEST_TRACE\|TEST-REMOVE" src/ inc/` → delete every hit.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "../inc/ah_test_trace.h"

static CRITICAL_SECTION s_tt_cs;
static volatile LONG    s_tt_init = 0;

static void tt_init_once(void) {
    // state 0 = unset, 1 = winner initializing, 2 = done.
    // Loser must wait for state=2, not state!=1 — otherwise it spins forever
    // because the winner never advanced the sentinel past 1.
    if (InterlockedCompareExchange(&s_tt_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_tt_cs);
        InterlockedExchange(&s_tt_init, 2);   // signal done
    } else {
        while (s_tt_init != 2) SwitchToThread();   // wait for winner
    }
}

void ah_test_trace_write(const char* fmt, ...) {
    if (!s_tt_init) tt_init_once();
    EnterCriticalSection(&s_tt_cs);
    HANDLE h = CreateFileA("C:\Users\Public\ah_test_trace.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LeaveCriticalSection(&s_tt_cs); return; }
    SetFilePointer(h, 0, NULL, FILE_END);
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[1024];
    int hdr = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[%02u:%02u:%02u.%03u tid=%lu pid=%lu] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        (unsigned long)GetCurrentThreadId(),
        (unsigned long)GetCurrentProcessId());
    if (hdr < 0) hdr = 0;
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(buf + hdr, sizeof(buf) - hdr - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - hdr - 2;
    size_t total = (size_t)hdr + (size_t)n;
    if (total > sizeof(buf) - 2) total = sizeof(buf) - 2;
    buf[total++] = '\r'; buf[total++] = '\n';
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)total, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    LeaveCriticalSection(&s_tt_cs);
}
