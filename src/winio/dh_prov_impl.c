// DeltaHack — provider runtime implementation.
//
// DhProviderTry — attempts to install + start + open + smoke-test a provider.
// DhProviderSelect — iterates providers by priority, returns first success.
// DhProviderPhysRead/Write — dispatches through the active provider protocol.
//
// Provider-specific protocol details (request struct layout) live in the
// per-family PhysRead/Write handlers below. All WinIo family drivers share
// the same request struct layout; ASUSIO / UCOREW / REDFOX differ slightly.
#include "../../inc/dh_provider.h"
#include "../../inc/dh_scm.h"
#include "../../inc/dh_dbunpack.h"

#include <windows.h>
#include <shlwapi.h>
#include <string.h>

// Global "active" provider — set by main after DhProviderSelect succeeds.
// PhysRead/PhysWrite in dh_phys.c consult this to route IOCTLs correctly.
const DH_PROVIDER* g_active_provider = NULL;

// DbUnpack declared in dh_dbunpack.h: BOOL DbUnpack(const void*, DWORD, void**, DWORD*)

// -----------------------------------------------------------------------------
// Request struct layouts (per protocol family)
// -----------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct {                        // WINIO / ASUSIO family (shared)
    ULONG_PTR ViewSize;
    ULONG_PTR BusAddress;
    HANDLE    SectionHandle;
    PVOID     BaseAddress;
    PVOID     ReferencedObject;
} WINIO_REQ;

typedef struct {                        // REDFOX family (inpoutx64) — 32 bytes
    HANDLE    SectionHandle;    // +0x00
    ULONG_PTR ViewSize;         // +0x08
    ULONG_PTR BusAddress;       // +0x10 — MUST be page-aligned
    PVOID     BaseAddress;      // +0x18 — mapped VA
} REDFOX_REQ;

typedef struct {                        // UCOREW64 family
    ULONG     Size;
    ULONG_PTR BusAddress;
    PVOID     BaseAddress;
    HANDLE    SectionHandle;
    PVOID     ReferencedObject;
} UCOREW_REQ;

#pragma pack(pop)

// PHYMEM struct: NO pack(1) — original phymem.h uses default alignment.
// On x64 = {PVOID @+0, ULONG @+8, padding to +16} = 16 bytes total.
typedef struct {
    PVOID pvAddr;
    ULONG dwSize;
} PHYMEM_REQ;

static BOOL CallIoctl(HANDLE hDev, DWORD ioctl, void* buf, DWORD sz)
{
    DWORD bytes = 0;
    return DeviceIoControl(hDev, ioctl, buf, sz, buf, sz, &bytes, NULL);
}

// -----------------------------------------------------------------------------
// Per-protocol phys map / unmap
// -----------------------------------------------------------------------------

static PVOID MapWinIo(HANDLE hDev, DWORD ioctl_map, u64 phys, u32 size,
                     HANDLE* sec, PVOID* refObj)
{
    WINIO_REQ req = {0};
    req.ViewSize = size;
    req.BusAddress = (ULONG_PTR)phys;
    if (!CallIoctl(hDev, ioctl_map, &req, sizeof(req))) return NULL;
    *sec = req.SectionHandle;
    *refObj = req.ReferencedObject;
    return req.BaseAddress;
}

static void UnmapWinIo(HANDLE hDev, DWORD ioctl_unmap, PVOID base,
                       HANDLE sec, PVOID refObj)
{
    WINIO_REQ req = {0};
    req.BaseAddress = base;
    req.SectionHandle = sec;
    req.ReferencedObject = refObj;
    CallIoctl(hDev, ioctl_unmap, &req, sizeof(req));
}

// REDFOX driver requires page-aligned bus address. Returns page-aligned
// mapped VA — caller must add page_offset to get desired physical byte.
// out_offset receives the intra-page offset for caller convenience.
static PVOID MapRedFox(HANDLE hDev, DWORD ioctl_map, u64 phys, u32 size,
                      HANDLE* sec, PVOID* refObj, u32* out_offset)
{
    u64 pageBase = phys & ~(u64)0xFFF;
    u32 pageOff  = (u32)(phys & 0xFFF);
    u32 mapSize  = ((pageOff + size + 0xFFF) & ~(u32)0xFFF);

    REDFOX_REQ req = {0};
    req.BusAddress = (ULONG_PTR)pageBase;
    req.ViewSize   = mapSize;
    if (!CallIoctl(hDev, ioctl_map, &req, sizeof(req))) return NULL;

    *sec = req.SectionHandle;
    *refObj = NULL;
    if (out_offset) *out_offset = pageOff;
    return req.BaseAddress;
}

// PHYMEM: request={addr, size}, output=PVOID (mapped VA). Distinct buffers,
// distinct sizes — CallIoctl's in==out shortcut doesn't work here.
static PVOID MapPhyMem(HANDLE hDev, DWORD ioctl_map, u64 phys, u32 size)
{
    PHYMEM_REQ req = {0};
    req.pvAddr = (PVOID)(ULONG_PTR)phys;
    req.dwSize = size;
    PVOID mapped = NULL;
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(hDev, ioctl_map,
                              &req, sizeof(req),
                              &mapped, sizeof(mapped),
                              &bytes, NULL);
    if (!ok) {
        static int once = 0;
        if (!once++) {
            DH_ERROR("PhyMem MAP: DeviceIoControl gle=%lu ioctl=0x%X reqSz=%u phys=0x%llX size=%u",
                     GetLastError(), ioctl_map, (unsigned)sizeof(req),
                     (unsigned long long)phys, size);
        }
        return NULL;
    }
    return mapped;
}

static void UnmapPhyMem(HANDLE hDev, DWORD ioctl_unmap, PVOID mapped, u32 size)
{
    PHYMEM_REQ req = {0};
    req.pvAddr = mapped;
    req.dwSize = size;
    DWORD bytes = 0;
    DeviceIoControl(hDev, ioctl_unmap, &req, sizeof(req), NULL, 0, &bytes, NULL);
}

static void UnmapRedFox(HANDLE hDev, DWORD ioctl_unmap, PVOID base,
                        HANDLE sec, PVOID refObj)
{
    (void)refObj;
    REDFOX_REQ req = {0};
    req.BaseAddress = base;
    req.SectionHandle = sec;
    CallIoctl(hDev, ioctl_unmap, &req, sizeof(req));
}

// -----------------------------------------------------------------------------
// Public: phys R/W dispatch
// -----------------------------------------------------------------------------

BOOL DhProviderPhysRead(HANDLE hDev, const DH_PROVIDER* prov,
                        u64 phys, void* dst, u32 size)
{
    if (!size || !hDev || !prov) return FALSE;

    // WINIO family caps view size at ~one page per IOCTL — chunk bulk reads
    // through page-sized MapView calls (REDFOX handles arbitrary size in one
    // shot via its own page-batched mapper; not affected).
    if (prov->protocol == DH_PROTO_WINIO ||
        prov->protocol == DH_PROTO_ASUSIO ||
        prov->protocol == DH_PROTO_UCOREW ||
        prov->protocol == DH_PROTO_PHYMEM)
    {
        // Page-chunk + tolerate individual failures (phys page 0 = MMIO etc).
        u32 done = 0;
        u32 page_ok = 0, page_fail = 0;
        while (done < size) {
            u64 curPhys  = phys + done;
            u32 pageOff2 = (u32)(curPhys & 0xFFF);
            u32 chunkMax = 0x1000 - pageOff2;
            u32 chunk    = (size - done < chunkMax) ? (size - done) : chunkMax;

            HANDLE sec2 = NULL; PVOID refObj2 = NULL;
            PVOID mapped2 = NULL;
            u64 alignedPhys = curPhys & ~(u64)0xFFF;

            if (prov->protocol == DH_PROTO_PHYMEM) {
                mapped2 = MapPhyMem(hDev, prov->ioctl_map, alignedPhys, 0x1000);
            } else {
                mapped2 = MapWinIo(hDev, prov->ioctl_map, alignedPhys, 0x1000,
                                   &sec2, &refObj2);
            }
            if (!mapped2) {
                memset((u8*)dst + done, 0, chunk);
                page_fail++;
                done += chunk;
                continue;
            }

            BOOL cok = TRUE;
            __try {
                memcpy((u8*)dst + done, (u8*)mapped2 + pageOff2, chunk);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                cok = FALSE;
            }
            if (prov->protocol == DH_PROTO_PHYMEM) {
                UnmapPhyMem(hDev, prov->ioctl_unmap, mapped2, 0x1000);
            } else {
                UnmapWinIo(hDev, prov->ioctl_unmap, mapped2, sec2, refObj2);
            }
            if (!cok) {
                memset((u8*)dst + done, 0, chunk);
                page_fail++;
            } else {
                page_ok++;
            }
            done += chunk;
        }
        return (page_ok > 0);
    }

    // REDFOX family: single IOCTL handles arbitrary size (page-batched inside).
    HANDLE sec = NULL; PVOID refObj = NULL; PVOID mapped = NULL;
    u32 pageOff = 0;

    if (prov->protocol == DH_PROTO_REDFOX) {
        mapped = MapRedFox(hDev, prov->ioctl_map, phys, size, &sec, &refObj, &pageOff);
    } else {
        return FALSE;
    }
    if (!mapped) return FALSE;

    BOOL ok = TRUE;
    __try {
        memcpy(dst, (u8*)mapped + pageOff, size);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    UnmapRedFox(hDev, prov->ioctl_unmap, mapped, sec, refObj);
    return ok;
}

BOOL DhProviderPhysWrite(HANDLE hDev, const DH_PROVIDER* prov,
                         u64 phys, const void* src, u32 size)
{
    if (!size || !hDev || !prov) return FALSE;

    // Same page-chunking rationale as PhysRead for WINIO/PHYMEM family.
    if (prov->protocol == DH_PROTO_WINIO ||
        prov->protocol == DH_PROTO_ASUSIO ||
        prov->protocol == DH_PROTO_UCOREW ||
        prov->protocol == DH_PROTO_PHYMEM)
    {
        u32 done = 0;
        while (done < size) {
            u64 curPhys  = phys + done;
            u32 pageOff2 = (u32)(curPhys & 0xFFF);
            u32 chunkMax = 0x1000 - pageOff2;
            u32 chunk    = (size - done < chunkMax) ? (size - done) : chunkMax;

            HANDLE sec2 = NULL; PVOID refObj2 = NULL;
            PVOID mapped2 = NULL;
            u64 alignedPhys = curPhys & ~(u64)0xFFF;

            if (prov->protocol == DH_PROTO_PHYMEM) {
                mapped2 = MapPhyMem(hDev, prov->ioctl_map, alignedPhys, 0x1000);
            } else {
                mapped2 = MapWinIo(hDev, prov->ioctl_map, alignedPhys, 0x1000,
                                   &sec2, &refObj2);
            }
            if (!mapped2) return FALSE;

            BOOL cok = TRUE;
            __try {
                memcpy((u8*)mapped2 + pageOff2, (const u8*)src + done, chunk);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                cok = FALSE;
            }
            if (prov->protocol == DH_PROTO_PHYMEM) {
                UnmapPhyMem(hDev, prov->ioctl_unmap, mapped2, 0x1000);
            } else {
                UnmapWinIo(hDev, prov->ioctl_unmap, mapped2, sec2, refObj2);
            }
            if (!cok) return FALSE;
            done += chunk;
        }
        return TRUE;
    }

    HANDLE sec = NULL; PVOID refObj = NULL; PVOID mapped = NULL;
    u32 pageOff = 0;
    if (prov->protocol == DH_PROTO_REDFOX) {
        mapped = MapRedFox(hDev, prov->ioctl_map, phys, size, &sec, &refObj, &pageOff);
    } else {
        return FALSE;
    }
    if (!mapped) return FALSE;

    BOOL ok = TRUE;
    __try {
        memcpy((u8*)mapped + pageOff, src, size);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    UnmapRedFox(hDev, prov->ioctl_unmap, mapped, sec, refObj);
    return ok;
}

// -----------------------------------------------------------------------------
// Try one provider: unpack bin -> temp.sys -> SCM install+start -> open device
// -----------------------------------------------------------------------------

BOOL DhProviderTry(const DH_PROVIDER* prov, HANDLE* out_dev)
{
    if (!prov || !out_dev) return FALSE;

    // Locate .bin — try (in order):
    //   1. env %DH_INSTALL_DIR%\db\<name>.bin  — launcher-set install path
    //   2. <exeDir>\db\<name>.bin              — flat install layout
    //   3. <exeDir>\<name>.bin                 — flat next-to-exe
    //   4. <exeDir>\..\src\db\<name>.bin       — dev tree
    //   5. C:\DeltaHack\db\<name>.bin          — hardcoded fallback for
    //                                            launcher-spawned temp exec
    wchar_t exeDir[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);
    wchar_t binPath[MAX_PATH * 2];
    HANDLE h = INVALID_HANDLE_VALUE;

    // 1. Launcher-set install dir (set by App.exe before CreateProcess).
    {
        wchar_t installDir[MAX_PATH] = {0};
        DWORD envLen = GetEnvironmentVariableW(L"DH_INSTALL_DIR",
                                               installDir, MAX_PATH);
        if (envLen > 0 && envLen < MAX_PATH) {
            _snwprintf(binPath, MAX_PATH * 2, L"%ws\\db\\%ws",
                       installDir, prov->bin_filename);
            h = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        _snwprintf(binPath, MAX_PATH * 2, L"%ws\\db\\%ws", exeDir, prov->bin_filename);
        h = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        _snwprintf(binPath, MAX_PATH * 2, L"%ws\\%ws", exeDir, prov->bin_filename);
        h = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        _snwprintf(binPath, MAX_PATH * 2, L"%ws\\..\\src\\db\\%ws", exeDir, prov->bin_filename);
        h = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        _snwprintf(binPath, MAX_PATH * 2, L"C:\\DeltaHack\\db\\%ws", prov->bin_filename);
        h = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DH_ERROR("provider %s: bin not found (last tried: %ws)", prov->name, binPath);
        return FALSE;
    }
    DWORD binSize = GetFileSize(h, NULL);
    u8* raw = (u8*)HeapAlloc(GetProcessHeap(), 0, binSize);
    DWORD got = 0;
    if (!ReadFile(h, raw, binSize, &got, NULL) || got != binSize) {
        DH_ERROR("provider %s: read bin failed", prov->name);
        HeapFree(GetProcessHeap(), 0, raw);
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);

    // Optional pre-open callback (AsIO3 zombie proc, etc.)
    if (prov->pre_open && !prov->pre_open()) {
        DH_ERROR("provider %s: pre_open failed", prov->name);
        HeapFree(GetProcessHeap(), 0, raw);
        return FALSE;
    }

    void* decoded = NULL;
    DWORD decSize = 0;
    BOOL unpacked = DbUnpack(raw, binSize, &decoded, &decSize);
    HeapFree(GetProcessHeap(), 0, raw);
    if (!unpacked || !decoded) {
        DH_ERROR("provider %s: DbUnpack failed", prov->name);
        return FALSE;
    }

    // Write .sys to %TEMP%
    wchar_t sysPath[MAX_PATH * 2];
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    _snwprintf(sysPath, MAX_PATH * 2, L"%wsdh_%ws_%lu.sys",
               tempDir, prov->svc_name, GetCurrentProcessId());
    h = CreateFileW(sysPath, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DH_ERROR("provider %s: create sys failed", prov->name);
        HeapFree(GetProcessHeap(), 0, decoded);
        return FALSE;
    }
    WriteFile(h, decoded, decSize, &got, NULL);
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, decoded);

    // Fast-path: resident device from prior session. Reuse handle — even if
    // it's a different .sys build, IOCTL surface is defined by kdu family so
    // ours match if protocol tag is correct.
    wchar_t devPath[128];
    _snwprintf(devPath, 128, L"\\\\.\\%ws", prov->dev_name);
    HANDLE probeDev = CreateFileW(devPath, GENERIC_READ | GENERIC_WRITE,
                                   0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (probeDev != INVALID_HANDLE_VALUE) {
        DH_INFO("provider %s: device already resident, reusing %ws", prov->name, devPath);
        *out_dev = probeDev;
        return TRUE;
    }

    // SCM install + start + open device (fresh install)
    wchar_t svcName[64];
    _snwprintf(svcName, 64, L"%ws_%lu", prov->svc_name, GetCurrentProcessId());
    DH_DRIVER drv = {0};
    if (!DhDrvInstall(&drv, svcName, prov->dev_name, sysPath)) return FALSE;
    if (!DhDrvStart(&drv))  { DhDrvStop(&drv); return FALSE; }

    // Optional unlock handshake
    if (prov->unlock && !prov->unlock(drv.hDevice)) {
        DH_ERROR("provider %s: unlock failed", prov->name);
        DhDrvStop(&drv);
        return FALSE;
    }

    if (!DhDrvOpenDevice(&drv)) {
        DH_ERROR("provider %s: open device '\\\\.\\%ws' failed", prov->name, prov->dev_name);
        DhDrvStop(&drv);
        return FALSE;
    }

    // NO smoke test — sending wrong IOCTL to wrong driver family = BSOD risk.
    // Provider is considered "live" if it opened device successfully.
    // Actual IOCTL verification must happen on caller side after provider selection.
    DH_INFO("provider %s STARTED: dev=\\\\.\\%ws (no smoke test — safety)",
            prov->name, prov->dev_name);
    *out_dev = drv.hDevice;
    return TRUE;
}

// -----------------------------------------------------------------------------
// Iterate providers by priority, pick first that works
// -----------------------------------------------------------------------------

const DH_PROVIDER* DhProviderSelect(HANDLE* out_dev, u32* out_flags)
{
    if (out_flags) *out_flags = 0;
    // Sort providers by descending priority (static — done at init once)
    // For simplicity: try in registry order (priority pre-sorted in registry).
    for (int i = 0; i < g_provider_count; i++) {
        const DH_PROVIDER* p = &g_providers[i];
        DH_INFO("trying provider [#%u %s pri=%d]...", p->kdu_id, p->name, p->priority);
        if (DhProviderTry(p, out_dev)) {
            if (out_flags) *out_flags = p->avail_flags;
            return p;
        }
    }
    return NULL;
}
