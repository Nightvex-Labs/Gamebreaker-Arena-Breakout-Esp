// arenahack probe — smoke-test the DeltaHack bypass against UAGame.exe.
//
// Pipeline:
//   1. driver_up: install inpoutx64 via SCM, open device.
//   2. RpmFindSystemCR3 — locate System EPROCESS DTB via low-stub scan.
//   3. RpmFindProcess("UAGame.exe") — walk ActiveProcessLinks, match ImageFileName.
//   4. Read PEB from EPROCESS to get UAGame ImageBase.
//   5. Read qword at base + GWORLD_RVA — if valid heap pointer, bypass works.
//   6. Bonus: deref UWorld to PersistentLevel + count Actors[] to prove reader chain.
//
// Success prints:
//   UAGame base=0x140000000  peb=0x...
//   GWorld @ 0xB29A608 -> 0x1AXXXXXXXX  (heap ptr, aligned)
//   PersistentLevel = 0x1BXXXXXXXX
//   Actors[] count = NNNN
//
// Failure prints exact stage that broke — that's what we investigate next.

#include "../inc/dh_common.h"
#include "../inc/dh_dbunpack.h"
#include "../inc/dh_scm.h"
#include "../inc/dh_phys.h"
#include "../inc/dh_provider.h"
#include "../inc/dh_rpm.h"
#include "../inc/ah_offsets.h"

#include <windows.h>
#include <stdio.h>

#define AH_DEV_NAME_LEGACY  L"EneIo"

// Set by driver_up so PhysRead/PhysWrite dispatch through the picked provider
// (correct IOCTL family). Without this, PhysRead uses hardcoded legacy WinIo
// IOCTL 0x80102040 which fails against inpoutx64 (needs REDFOX 0x9C40201C).
extern const DH_PROVIDER* g_active_provider;

static wchar_t g_svcName[64];
static const wchar_t* svc_name(void) {
    if (!g_svcName[0]) {
        _snwprintf(g_svcName, 64, L"ah_probe_%lu", GetCurrentProcessId());
    }
    return g_svcName;
}

static BOOL is_elevated(void) {
    BOOL ret = FALSE;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e = {0};
        DWORD n = 0;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n))
            ret = e.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }
    return ret;
}

// Unpack embedded EneIo64/inpoutx64 .bin from src/db/ to a random %TEMP% .sys.
// Uses provider registry to pick highest-priority driver on this host.
static BOOL prepare_driver_sys(wchar_t* outPath, DWORD cch) {
    // Look up inpoutx64.bin next to the exe. Simple probe: same dir as exe.
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    wchar_t binPath[MAX_PATH];
    _snwprintf(binPath, MAX_PATH, L"%s\\db\\inpoutx64.bin", exePath);
    if (GetFileAttributesW(binPath) == INVALID_FILE_ATTRIBUTES) {
        // fallback: same dir
        _snwprintf(binPath, MAX_PATH, L"%s\\inpoutx64.bin", exePath);
        if (GetFileAttributesW(binPath) == INVALID_FILE_ATTRIBUTES) {
            DH_ERROR("inpoutx64.bin not found next to probe.exe (looked at db\\ and same dir)");
            return FALSE;
        }
    }
    HANDLE h = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DH_ERROR("open inpoutx64.bin failed (gle=%lu)", GetLastError());
        return FALSE;
    }
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    void* in = HeapAlloc(GetProcessHeap(), 0, sz);
    if (!in || !ReadFile(h, in, sz, &rd, NULL) || rd != sz) {
        CloseHandle(h); DH_ERROR("read bin failed"); return FALSE;
    }
    CloseHandle(h);
    void* out = NULL; DWORD outSz = 0;
    if (!DbUnpack(in, sz, &out, &outSz)) {
        HeapFree(GetProcessHeap(), 0, in);
        DH_ERROR("DbUnpack failed"); return FALSE;
    }
    HeapFree(GetProcessHeap(), 0, in);

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    _snwprintf(outPath, cch, L"%s\\ah_%08lx.sys", tempDir, (unsigned long)GetTickCount());
    HANDLE ho = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (ho == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, out);
        DH_ERROR("open temp .sys failed (gle=%lu)", GetLastError());
        return FALSE;
    }
    DWORD wr = 0;
    if (!WriteFile(ho, out, outSz, &wr, NULL) || wr != outSz) {
        CloseHandle(ho); HeapFree(GetProcessHeap(), 0, out);
        DH_ERROR("write temp .sys failed"); return FALSE;
    }
    CloseHandle(ho);
    HeapFree(GetProcessHeap(), 0, out);
    DH_INFO("driver .sys unpacked -> %ls (%lu B)", outPath, outSz);
    return TRUE;
}

static BOOL driver_up(DH_DRIVER* drv) {
    ZeroMemory(drv, sizeof(*drv));

    // 1. Try provider registry (priority-sorted) — kdu-family probe.
    HANDLE dev = NULL;
    u32 flags = 0;
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (p && dev) {
        drv->hDevice = dev;
        _snwprintf(drv->devPath, 128, L"\\\\.\\%s", p->dev_name);
        g_active_provider = p;   // route PhysRead/Write through this provider
        DH_INFO("active provider: kdu#%u %s (flags=0x%X)", p->kdu_id, p->name, flags);
        return TRUE;
    }

    // 2. Legacy fallback: prepare inpoutx64 by hand, install service.
    DH_WARN("provider registry failed — falling back to legacy inpoutx64 install");
    _snwprintf(drv->devPath, 128, L"\\\\.\\%s", AH_DEV_NAME_LEGACY);
    HANDLE probe = CreateFileW(drv->devPath, GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (probe != INVALID_HANDLE_VALUE) {
        drv->hDevice = probe;
        DH_INFO("device already resident, reusing: %ls", drv->devPath);
        return TRUE;
    }

    wchar_t sysPath[MAX_PATH];
    if (!prepare_driver_sys(sysPath, MAX_PATH)) return FALSE;
    if (!DhDrvInstall(drv, svc_name(), AH_DEV_NAME_LEGACY, sysPath)) return FALSE;
    if (!DhDrvStart(drv)) return FALSE;
    if (!DhDrvOpenDevice(drv)) return FALSE;
    DH_INFO("device open (legacy): %ls", drv->devPath);
    return TRUE;
}

static void driver_down(DH_DRIVER* drv) {
    if (drv->hDevice && !drv->hSvc) {
        CloseHandle(drv->hDevice);
        drv->hDevice = NULL;
        return;
    }
    DhDrvStop(drv);
    DhDrvUninstall(drv);
    DhDrvCleanup(drv);
}

int wmain(int argc, wchar_t** argv) {
    (void)argc; (void)argv;

    printf("arenahack probe — smoke-test bypass against UAGame.exe\n\n");

    if (!is_elevated()) {
        DH_ERROR("not elevated — need admin (right-click, Run as administrator)");
        return 3;
    }

    DH_DRIVER drv;
    if (!driver_up(&drv)) {
        DH_ERROR("[STAGE 1] driver_up FAILED — BYOVD chain broken");
        return 10;
    }
    DH_INFO("[STAGE 1] OK — driver up");

    u64 sysCR3 = 0;
    if (!RpmFindSystemCR3(drv.hDevice, &sysCR3)) {
        DH_ERROR("[STAGE 2] RpmFindSystemCR3 FAILED — low-stub scan broken");
        driver_down(&drv);
        return 11;
    }
    DH_INFO("[STAGE 2] OK — sysCR3 = 0x%llX", sysCR3);

    u64 procCR3 = 0, eproc = 0;
    if (!RpmFindProcess(drv.hDevice, sysCR3, AH_PROC_NAME, &procCR3, &eproc)) {
        DH_ERROR("[STAGE 3] RpmFindProcess(\"%s\") FAILED — UAGame not running? "
                 "start Arena Breakout Infinite first.", AH_PROC_NAME);
        driver_down(&drv);
        return 12;
    }
    DH_INFO("[STAGE 3] OK — UAGame EPROCESS=0x%llX  CR3=0x%llX", eproc, procCR3);

    u64 peb = 0;
    if (!RpmRead64(drv.hDevice, sysCR3, eproc + g_eproc_peb_off, &peb) || !peb) {
        DH_ERROR("[STAGE 4] read EPROCESS.Peb (@+0x%X) FAILED — layout table wrong?",
                 g_eproc_peb_off);
        driver_down(&drv);
        return 13;
    }
    DH_INFO("[STAGE 4] OK — PEB VA=0x%llX", peb);

    u64 base = 0, size = 0;
    if (!RpmGetMainImageBase(drv.hDevice, procCR3, peb, &base, &size)) {
        DH_ERROR("[STAGE 5] RpmGetMainImageBase FAILED — PEB traversal broken");
        driver_down(&drv);
        return 14;
    }
    DH_INFO("[STAGE 5] OK — UAGame base=0x%llX  size=0x%llX", base, size);

    // -- The payoff: read GWorld pointer. --
    u64 gWorld = 0;
    if (!RpmRead64(drv.hDevice, procCR3, base + AH_RVA_GWORLD, &gWorld)) {
        DH_ERROR("[STAGE 6] read base+GWORLD_RVA (0x%llX) FAILED — probe RPM broken",
                 (unsigned long long)AH_RVA_GWORLD);
        driver_down(&drv);
        return 15;
    }
    if (gWorld < 0x10000ULL || gWorld > 0x00007FFFFFFFFFFFULL || (gWorld & 7)) {
        DH_ERROR("[STAGE 6] GWorld=0x%llX looks INVALID — not aligned or not heap",
                 gWorld);
        driver_down(&drv);
        return 16;
    }
    DH_INFO("[STAGE 6] OK — GWorld=0x%llX  (valid heap ptr, 8-aligned)", gWorld);

    // -- Bonus: chase to PersistentLevel and count Actors[] --
    u64 persistentLevel = 0;
    if (RpmRead64(drv.hDevice, procCR3, gWorld + AH_UW_PERSISTENTLVL, &persistentLevel)
        && persistentLevel > 0x10000ULL) {
        DH_INFO("[STAGE 7] PersistentLevel=0x%llX", persistentLevel);

        u64 actorsData = 0;
        u32 actorsNum = 0;
        RpmRead64(drv.hDevice, procCR3, persistentLevel + AH_ULEVEL_ACTORS, &actorsData);
        RpmReadVirtual(drv.hDevice, procCR3, persistentLevel + AH_ULEVEL_ACTORS + 8, &actorsNum, 4);
        DH_INFO("[STAGE 7] Actors[] data=0x%llX  count=%u", actorsData, actorsNum);
        if (actorsNum > 0 && actorsNum < 100000 && actorsData > 0x10000ULL) {
            DH_INFO("=== BYPASS FULLY LIVE — read chain reaches game world state ===");
        } else {
            DH_WARN("Actors[] out of sane range — GWorld may be pre-raid stub");
        }
    } else {
        DH_WARN("[STAGE 7] PersistentLevel read failed or NULL — pre-raid state?");
    }

    printf("\n=== PROBE COMPLETE — bypass VERIFIED against UAGame.exe ===\n");
    printf("Next: swap probe for full reader loop (Actor walker + ACE decrypt).\n\n");

    driver_down(&drv);
    return DH_OK;
}
