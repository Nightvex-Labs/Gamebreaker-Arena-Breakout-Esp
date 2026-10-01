// ah_uspace_read.h — userspace-based attach + read primitive.
// v1.0.34: alternative to the kdu-based phys/CR3 path in dh_rpm.c.
//
// Rationale: field triage of Pattern-A hwids showed the game's EPROCESS
// gets unlinked from PsActiveProcessLinks by ACE, so our kernel-side walk
// fails on those systems. Toolhelp32 + NtOpenProcess uses PspCidTable
// (a completely different kernel table that ACE cannot afford to hook
// without breaking every Win32 tool including Task Manager), so it works
// on the same systems.
//
// This is what ABIFINAL does end-to-end — no kdu, no phys reads, no CR3.
// We keep the kdu path as a fallback for HVCI-on machines where the game
// process handle is stripped of PROCESS_VM_READ post-open.
//
// Contract: AhUspaceAttach() finds UAGame.exe (or alternate distribution
// names), opens a read-only handle, resolves the module base, and returns
// TRUE on success. Subsequent AhUspaceRead(va, dst, n) reads work through
// the cached handle. AhUspaceDetach() closes the handle.

#pragma once
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long u64;

// Attach to a running game process by executable name (matched with
// substring on the first 6 chars, plus a small alternate-names list —
// see impl for the current list). Returns TRUE if a process was found,
// opened with PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, and
// its main module base resolved.
//
// out_pid, out_base — filled on success.
BOOL AhUspaceAttach(const char* procName, DWORD* out_pid, u64* out_base);

// Read `n` bytes from the game's virtual address space. Returns TRUE on
// exact-length read, FALSE otherwise (short reads are treated as failure
// for parity with RpmReadVirtual).
BOOL AhUspaceRead(u64 va, void* dst, uint32_t n);

// Convenience — 8-byte read.
static inline BOOL AhUspaceRead64(u64 va, u64* dst) {
    return AhUspaceRead(va, dst, 8);
}

// Close handle. Idempotent.
void AhUspaceDetach(void);

// TRUE if a handle is currently held.
BOOL AhUspaceOk(void);

// Currently attached PID (0 if not attached).
DWORD AhUspacePid(void);

// Currently attached module base (0 if not attached).
u64 AhUspaceBase(void);

// Verify handle by touching the module base — some AC modules revoke
// PROCESS_VM_READ post-open, so we probe before publishing "attached".
BOOL AhUspaceVerify(void);

// v1.0.38: session-scope disable state. Set by AhUspaceMarkBroken() when
// reads start failing mid-session (ACE ObRegisterCallbacks revoked
// PROCESS_VM_READ ~10s after OpenProcess). Once TRUE, AhUspaceAttach()
// refuses, RpmFindProcess falls through to kdu path.
BOOL AhUspaceDisabled(void);
void AhUspaceMarkBroken(void);

#ifdef __cplusplus
}
#endif
