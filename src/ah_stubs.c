// arenahack — stubs for hardening symbols that dh_rpm.c pulls in.
// Real DeltaHack ships direct syscall wrappers (SysWhispers2-style) in
// dh_syscalls.c + dh_syscalls.asm. For probe MVP we let dh_rpm.c fall
// through to the GetProcAddress path (safe, just leaves ntdll hooks
// visible to EDR — fine for local smoke test).
//
// Full syscall hardening lands when we promote probe → daemon.

#include <windows.h>

DWORD g_ssn_NtQSI = 0;   // 0 = direct-syscall disabled → dh_rpm.c uses GetProcAddress

LONG __stdcall DhDirectNtQuerySystemInformation(
    ULONG cls, PVOID buf, ULONG len, PULONG needed)
{
    (void)cls; (void)buf; (void)len; (void)needed;
    return (LONG)0xC0000002L;   // STATUS_NOT_IMPLEMENTED — never called (SSN=0)
}
