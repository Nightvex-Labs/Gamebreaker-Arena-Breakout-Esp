// arenahack — AMSI/ETW patch + anti-debug prologue. Called once at wmain
// entry from overlay_main.c before any other init.
//
// AMSI: patches AmsiScanBuffer + AmsiScanString to return E_INVALIDARG
//   (0x80070057) — Defender/EDR AMSI probes on our process return "clean"
//   instantly, no behavior visible. Byte pattern uses XOR+ADD (unique) to
//   avoid the classic pub Yara pattern `B8 57 00 07 80 C3`.
//
// ETW: patches ntdll!Etw* + NtTraceEvent prologues to `XOR RAX,RAX ; RET`
//   — user-mode ETW event delivery silently dies for our process. Kernel
//   ETW still fires but never gets correlated user-space payload.
//
// Anti-debug: PEB.BeingDebugged + NtQueryInformationProcess(ProcessDebugPort=7,
//   ProcessDebugFlags=0x1F). Any positive hit → silent ExitProcess. Prevents
//   x64dbg / OllyDbg / WinDbg attachment for casual RE.
//
// Ported verbatim from C:/DeltaHack/loader/src/hardening/dh_amsi_etw.c
// (2026-09-23), minus VMProtect markers (overlay doesn't link SDK) and
// minus DhInitSyscalls (SysWhispers2 port pending).

#include <windows.h>
#include <stdio.h>
#include <intrin.h>
#include "../../inc/ah_test_trace.h"    // TEST-REMOVE: instrumentation

static BOOL PatchPrologue(HMODULE mod, const char* funcName,
                          const BYTE* patch, SIZE_T patchLen)
{
    if (!mod) return FALSE;
    FARPROC pfn = GetProcAddress(mod, funcName);
    if (!pfn) return FALSE;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)pfn, patchLen, PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    memcpy((void*)pfn, patch, patchLen);
    DWORD dummy = 0;
    VirtualProtect((LPVOID)pfn, patchLen, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)pfn, patchLen);
    return TRUE;
}

static BOOL IsBeingDebugged(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    unsigned char* peb = (unsigned char*)__readgsqword(0x60);
    if (peb && peb[0x02]) {
        ah_test_trace_write("IsBeingDebugged: PEB.BeingDebugged=1");   // TEST-REMOVE
        return TRUE;
    }
#endif
    typedef LONG (NTAPI *pfnNtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    pfnNtQIP p = (pfnNtQIP)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (p) {
        HANDLE port = NULL;
        ULONG ret = 0;
        LONG st1 = p(GetCurrentProcess(), 7 /*ProcessDebugPort*/,
                     &port, sizeof(port), &ret);
        if (st1 >= 0 && port != NULL) {
            ah_test_trace_write("IsBeingDebugged: ProcessDebugPort=%p → TRUE", port);   // TEST-REMOVE
            return TRUE;
        }
        // v1.0.38.2: ProcessDebugFlags (info class 0x1F) check DISABLED —
        // false-positive on 2PC (SSH-launched process, no debugger). Docs
        // are ambiguous (some say 0=debugged, others 0=default-normal); on
        // Windows 11 25H2 build 26200 it consistently returns 0 for
        // regular processes, so we can't distinguish. Rely on PEB flag +
        // ProcessDebugPort only.
        ah_test_trace_write("IsBeingDebugged: port=%p st1=0x%lX → FALSE",
                            port, (unsigned long)st1);   // TEST-REMOVE
    } else {
        ah_test_trace_write("IsBeingDebugged: NtQueryInformationProcess not found → FALSE");   // TEST-REMOVE
    }
    return FALSE;
}

void DhInitHardening(void)
{
    ah_test_trace_write("DhInitHardening ENTER");   // TEST-REMOVE
    if (IsBeingDebugged()) {
        // TEST-REMOVE: unmask silent exit so we know if a legit user gets
        // false-positive-flagged as debugged.
        ah_test_trace_write("DhInitHardening: IsBeingDebugged returned TRUE → ExitProcess(0)");
        // Silent exit — no log line (would signal detection).
        ExitProcess(0);
    }
    ah_test_trace_write("DhInitHardening not debugged, patching AMSI/ETW");   // TEST-REMOVE

    HMODULE amsi = LoadLibraryA("amsi.dll");
    if (amsi) {
        // XOR EAX,EAX ; ADD EAX,0x80070057 ; RET  (E_INVALIDARG return)
        static const BYTE amsi_patch[] = {
            0x31, 0xC0, 0x05, 0x57, 0x00, 0x07, 0x80, 0xC3
        };
        PatchPrologue(amsi, "AmsiScanBuffer", amsi_patch, sizeof(amsi_patch));
        PatchPrologue(amsi, "AmsiScanString", amsi_patch, sizeof(amsi_patch));
    }

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        // XOR RAX,RAX ; RET   (return 0 = success without emitting)
        static const BYTE etw_patch[] = {
            0x48, 0x33, 0xC0, 0xC3
        };
        PatchPrologue(ntdll, "EtwEventWrite",     etw_patch, sizeof(etw_patch));
        PatchPrologue(ntdll, "EtwEventWriteFull", etw_patch, sizeof(etw_patch));
        PatchPrologue(ntdll, "EtwEventWriteEx",   etw_patch, sizeof(etw_patch));
        PatchPrologue(ntdll, "NtTraceEvent",      etw_patch, sizeof(etw_patch));
    }
}
