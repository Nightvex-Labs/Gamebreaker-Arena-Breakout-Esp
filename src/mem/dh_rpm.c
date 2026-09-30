#include "../../inc/dh_rpm.h"
#include "../../inc/dh_phys.h"
#include "../../inc/ah_uspace_read.h"   // v1.0.34: userspace attach + read path
#include "../../inc/ah_test_trace.h"    // TEST-REMOVE: instrumentation
#include <windows.h>
#include <stdio.h>

// v1.0.34: sentinel CR3 value that means "read via userspace HANDLE, not phys".
// Real CR3 is always page-aligned (>= 0x1000) so 1 is safe as a marker.
#define AH_CR3_USPACE 1ULL

// v1.0.22 diag override — DH_INFO/DH_ERROR usually compile to nothing in
// DH_RELEASE. Under AH_DIAG we want early-exit visibility in RpmFindProcess,
// so redirect them to append to the public procs log.
#ifdef AH_DIAG
static void ah_procs_log(const char* fmt, ...) {
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
}
#undef DH_ERROR
#undef DH_INFO
#define DH_ERROR(fmt, ...) ah_procs_log("[ERR] " fmt, ##__VA_ARGS__)
#define DH_INFO(fmt, ...)  ah_procs_log("[INF] " fmt, ##__VA_ARGS__)
#endif
// VMProtect SDK not needed here — hide functions have multi-return, wrapped
// at project level via VMProtect_Con.exe function-address list instead.

// Cached System EPROCESS pointer (PsInitialSystemProcess). Populated by
// RpmFindProcess on first successful walk; consumed by RpmHideOwnProcess.
u64 g_rpm_psisp = 0;

// Direct-syscall exports (see src/hardening/dh_syscalls.{asm,c}).
extern DWORD g_ssn_NtQSI;
extern NTSTATUS DhDirectNtQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);

#define PHY_MASK          0x000FFFFFFFFFF000ull
#define PHY_MASK_1G       0x000FFFFFC0000000ull
#define PHY_MASK_2M       0x000FFFFFFFE00000ull
#define VA_MASK_1G        0x000000003FFFFFFFull
#define VA_MASK_2M        0x00000001FFFFFull
#define VA_MASK_4K        0x0000000000000FFFull
#define PTE_PRESENT       1ull
#define PTE_LARGE_PAGE    0x80ull

// ---- CR3 discovery via PROCESSOR_START_BLOCK in first 1MB ----

// PROCESSOR_START_BLOCK signature: E9 xx 06 00 01 00 00 00
// Mask: 0xffffffffffff00ff, Value: 0x00000001000600E9
#define PSB_SIG_MASK  0xFFFFFFFFFFFF00FFull
#define PSB_SIG_VAL   0x00000001000600E9ull

// Offsets within PROCESSOR_START_BLOCK (x64, Win10+):
// LmTarget = 0x70, ProcessorState.SpecialRegisters.Cr3 varies by build.
// Safe approach: ProcessorState starts after SelfMap+MsrPat+MsrEFER.
// Known offset of Cr3 within the block: computed from struct layout.
// On Win11 24H2/25H2 (26100/26200), the PSB CR3 offset = 0xC0.
// Cross-reference: MemProcFS vmmwininit.c uses PSB+0xA0 for older builds,
// newer builds shifted to 0xC0 due to added fields.
// We try both offsets.

static u64 TryPSBCr3(const u8* page, u32 offset)
{
    u64 val = *(const u64*)(page + offset);
    // CR3 must be page-aligned and in low physical range (< 512 GB)
    if ((val & 0xFFF) == 0 && val != 0 && val < 0x8000000000ull)
        return val;
    return 0;
}

BOOL RpmFindSystemCR3(HANDLE hDev, u64* cr3Out)
{
    *cr3Out = 0;

    // Map first 1MB physical
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, 0x100000);
    if (!buf) return FALSE;

    BOOL ok = PhysRead(hDev, 0, buf, 0x100000);
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, buf);
        DH_ERROR("failed to read first 1MB physical");
        return FALSE;
    }

    u64 cr3 = 0;
    for (u32 off = 0x1000; off < 0x100000; off += 0x1000) {
        u64 sig = *(u64*)(buf + off);
        if ((sig & PSB_SIG_MASK) != PSB_SIG_VAL)
            continue;

        // Validate LmTarget is a canonical kernel address
        u64 lmTarget = *(u64*)(buf + off + 0x70);
        if ((lmTarget >> 47) != 0x1FFFF && (lmTarget >> 47) != 0x0)
            continue; // not canonical

        // Try known CR3 offsets
        static const u32 cr3_offsets[] = { 0xA0, 0xC0, 0xB0 };
        for (u32 i = 0; i < DH_ARR_LEN(cr3_offsets); i++) {
            cr3 = TryPSBCr3(buf + off, cr3_offsets[i]);
            if (cr3) {
                DH_INFO("CR3 found at PSB+0x%X: 0x%llX (stub @ phys 0x%X)",
                        cr3_offsets[i], cr3, off);
                break;
            }
        }
        if (cr3) break;
    }

    HeapFree(GetProcessHeap(), 0, buf);
    if (!cr3) {
        DH_ERROR("no valid CR3 found in low stub");
        return FALSE;
    }
    *cr3Out = cr3;
    return TRUE;
}

// ---- Page walk: PML4 → PDPT → PD → PT ----

// v1.0.25: MMIO / non-RAM PA guard. When ACE tampers UAGame's page tables
// (25H2 + REDFOX provider is where this bites), a forged PTE can steer the
// walk into device-memory regions:
//   0xFEC00000-0xFECFFFFF  I/O APIC
//   0xFED00000-0xFED0FFFF  HPET
//   0xFEE00000-0xFEEFFFFF  Local APIC       ← side-effectful reads hang bus
//   0xE0000000-0xFFFFFFFF  Broad PCI(e) MMIO / BAR window
//   0xA0000-0xFFFFF        VGA aperture + video BIOS shadow (uncached)
//   0-0xFFFFF              real-mode legacy region (best-effort skip)
// Provider's MmMapIoSpace + memcpy against LAPIC/IOAPIC MMIO can wedge the
// CPU with no bugcheck — arbitration/priority-register reads change device
// state. That matches the "PC just freezes, no BSOD" report pattern.
// This guard returns FALSE early so the caller's RPM sees a normal miss.
static inline BOOL ah_is_bad_pa(u64 pa) {
    if (pa < 0x100000ULL)                                return TRUE; // legacy 0-1MB (VGA/BIOS)
    if (pa >= 0xFEC00000ULL && pa <  0xFEF00000ULL)      return TRUE; // I/O + HPET + LAPIC
    if (pa >= 0xF0000000ULL && pa <  0x100000000ULL)     return TRUE; // upper PCI(e) MMIO
    return FALSE;
}

BOOL RpmVirtToPhys(HANDLE hDev, u64 cr3, u64 va, u64* paOut)
{
    *paOut = 0;
    u64 table = cr3 & PHY_MASK;
    if (ah_is_bad_pa(table)) return FALSE;

    for (int level = 0; level < 4; level++) {
        int shift = 39 - level * 9;
        u64 idx = (va >> shift) & 0x1FF;
        u64 entry = 0;

        u64 pte_pa = table + idx * 8;
        if (ah_is_bad_pa(pte_pa)) return FALSE;      // PT itself lives in MMIO — bail
        if (!PhysRead(hDev, pte_pa, &entry, 8))
            return FALSE;

        if (!(entry & PTE_PRESENT))
            return FALSE;

        table = entry & PHY_MASK;

        if (entry & PTE_LARGE_PAGE) {
            // v1.0.25: large-page bit only valid at PDPT (shift=30) and PD
            // (shift=21). PML4 (shift=39) and PT (shift=12) with PS set is
            // an illegal PTE — treat as miss instead of returning a garbage
            // PA whose only the low 30/21 bits are meaningful.
            if (shift == 30) { // 1GB page
                u64 out = (entry & PHY_MASK_1G) + (va & VA_MASK_1G);
                if (ah_is_bad_pa(out)) return FALSE;
                *paOut = out;
                return TRUE;
            }
            if (shift == 21) { // 2MB page
                u64 out = (entry & PHY_MASK_2M) + (va & VA_MASK_2M);
                if (ah_is_bad_pa(out)) return FALSE;
                *paOut = out;
                return TRUE;
            }
            return FALSE;   // PS bit at illegal level → tampered PTE
        }
    }

    // 4KB page
    u64 out = table + (va & VA_MASK_4K);
    if (ah_is_bad_pa(out)) return FALSE;
    *paOut = out;
    return TRUE;
}

// ---- Read virtual memory ----

// VA→PA translation cache. Each hit saves 4 page-table PhysReads (= 8 IOCTLs).
// Keyed by (cr3 << 12) ^ (va & ~0xFFF). Generation-based invalidation — cache
// valid only within one main-tick generation. Caller bumps g_rpm_generation
// at tick start to invalidate. No race conditions possible within a generation
// since PT walks are deterministic for a given (cr3, va) at any single moment.
#define VA_PA_CACHE_SLOTS 4096
volatile u64 g_rpm_generation = 1;
static struct { u64 key; u64 pa_page; u64 gen; } g_vapa_cache[VA_PA_CACHE_SLOTS] = {0};

void RpmBumpGeneration(void) {
    g_rpm_generation++;
    if (g_rpm_generation == 0) g_rpm_generation = 1;   // never 0
}

BOOL RpmReadVirtual(HANDLE hDev, u64 cr3, u64 va, void* buf, u32 size)
{
    // TEST-REMOVE: RPM is 50K+/sec; sample every 5000 calls (~0.1s).
    static unsigned long long tt_rpm_ctr = 0;
    tt_rpm_ctr++;
    if ((tt_rpm_ctr % 5000) == 0) {
        ah_test_trace_write("RpmReadVirtual #%llu cr3=0x%llX va=0x%llX size=%u",
            tt_rpm_ctr, (unsigned long long)cr3, (unsigned long long)va, (unsigned)size);
    }
    // v1.0.34: userspace fast-path — sentinel CR3 means the reader thread
    // successfully opened a PROCESS_VM_READ handle to the game via Toolhelp
    // + NtOpenProcess (see ah_uspace_read.c). All reads go through
    // NtReadVirtualMemory instead of kdu phys + CR3 walk. This is the
    // ABIFINAL model — works on rigs where ACE unlinks EPROCESS from
    // PsActiveProcessLinks (Pattern-A cold-start failures).
    if (cr3 == AH_CR3_USPACE) {
        (void)hDev;
        return AhUspaceRead(va, buf, size);
    }

    u8* dst = (u8*)buf;
    u32 remaining = size;

    while (remaining > 0) {
        u32 pageOff = (u32)(va & 0xFFF);
        u32 chunk   = 0x1000 - pageOff;
        if (chunk > remaining) chunk = remaining;

        // VA→PA cache lookup — key by cr3 + page-aligned VA.
        u64 va_page = va & ~0xFFFULL;
        u64 key = (cr3 << 12) ^ va_page;
        u32 slot = (u32)((key * 0x9E3779B97F4A7C15ULL) >> 52) & (VA_PA_CACHE_SLOTS - 1);
        u64 pa = 0;
        u64 cur_gen = g_rpm_generation;
        // Double-check pattern — read key/gen twice to defend against
        // concurrent writer torn read.
        u64 k1 = g_vapa_cache[slot].key;
        u64 pa_page = g_vapa_cache[slot].pa_page;
        u64 g1 = g_vapa_cache[slot].gen;
        u64 k2 = g_vapa_cache[slot].key;
        if (k1 == key && k1 == k2 && g1 == cur_gen) {
            pa = pa_page | pageOff;
        } else {
            if (!RpmVirtToPhys(hDev, cr3, va, &pa))
                return FALSE;
            g_vapa_cache[slot].key     = key;
            g_vapa_cache[slot].pa_page = pa & ~0xFFFULL;
            g_vapa_cache[slot].gen     = cur_gen;
        }
        if (!PhysRead(hDev, pa, dst, chunk))
            return FALSE;

        dst       += chunk;
        va        += chunk;
        remaining -= chunk;
    }
    return TRUE;
}

// Virtual-write mirror — walks page tables per VA, splits per-page write.
// Only used for kernel-space writes (PID unlink, handle strip, etc).
BOOL RpmWriteVirtual(HANDLE hDev, u64 cr3, u64 va, const void* buf, u32 size)
{
    const u8* src = (const u8*)buf;
    u32 remaining = size;

    while (remaining > 0) {
        u32 pageOff = (u32)(va & 0xFFF);
        u32 chunk   = 0x1000 - pageOff;
        if (chunk > remaining) chunk = remaining;

        u64 pa = 0;
        if (!RpmVirtToPhys(hDev, cr3, va, &pa))
            return FALSE;
        if (!PhysWrite(hDev, pa, src, chunk))
            return FALSE;

        src       += chunk;
        va        += chunk;
        remaining -= chunk;
    }
    return TRUE;
}

// ---- EPROCESS walk ----

// EPROCESS offsets vary by Windows build. Germanium (24H2/25H2) did a major
// layout realign — Cobalt/Nickel (22H2/23H2) uses a very different set.
// Source: Vergilius Project (Microsoft PDB).
//
// KPROCESS.DirectoryTableBase is stable at 0x28 across every x64 build from
// Vista through 25H2 (KPROCESS header layout hasn't shifted). Everything else
// moves.

typedef struct {
    u32 min_build;
    u32 max_build;
    u32 dtb;         // KPROCESS.DirectoryTableBase
    u32 pid;         // EPROCESS.UniqueProcessId
    u32 links;       // EPROCESS.ActiveProcessLinks (LIST_ENTRY)
    u32 imgname;     // EPROCESS.ImageFileName (15-char)
    u32 peb_default; // EPROCESS.Peb — starting guess; DiscoverPebOffset() may refine
    const char* label;
} DhEprocLayout;

static const DhEprocLayout kEprocLayouts[] = {
    // Cobalt / Nickel / Vibranium — pre-Germanium layout. Range covers common
    // LTSC / preview / SAC releases inside each named version so a patched
    // build like 22000.4123 doesn't kick the operator into "not in known-good
    // table" error even though the EPROCESS layout hasn't drifted.
    // Win10 20H1..22H2 (19041..19045) share the exact same EPROCESS layout
    // as Win11 21H2 (22000) — the kernel body was only realigned in Germanium
    // (24H2+), so the same DTB/PID/LINKS/IMGNAME/PEB offsets work verbatim.
    { 19041, 19045, 0x28, 0x440, 0x448, 0x5A8, 0x550, "Win10 20H1-22H2" },
    { 22000, 22000, 0x28, 0x440, 0x448, 0x5A8, 0x550, "Win11 21H2" },
    { 22621, 22621, 0x28, 0x440, 0x448, 0x5A8, 0x550, "Win11 22H2" },
    { 22631, 22631, 0x28, 0x440, 0x448, 0x5A8, 0x550, "Win11 23H2" },
    // Germanium — realigned layout (Win11 24H2 / 25H2 / 26H2).
    // Range 26100..30000 covers every current + upcoming Germanium build:
    //   26100  = 24H2 initial   (PID=0x1D0 LINKS=0x1D8 PEB=0x2E0 — verified)
    //   26200  = 25H2 launch    (PID=0x1D0 LINKS=0x1D8 PEB=0x2E0 — verified)
    //   28000  = 26H2 preview   (PID=0x1D0 LINKS=0x1D8 PEB=0x2E0 — verified)
    //   catch-all up to 30000 for KB updates & Insider drift.
    // Source: github.com/I3r1h0n/eprocess_offsets + live probe on client
    // machines (DiscoverPebOffset returned +0x2E0 on 26100 + 26200).
    // DTB (KPROCESS.DirectoryTableBase) = 0x28 has been stable since Vista.
    // ImageFileName = 0x338 across Germanium (private probe on 26200 client).
    // DiscoverPebOffset() is a belt-and-suspenders refinement — with the
    // correct default 0x2E0 baked here it becomes a no-op instead of the
    // fallback lookup path.
    { 26100, 30000, 0x28, 0x1D0, 0x1D8, 0x338, 0x2E0, "Win11 24H2/25H2/26H2" },
};

// Populated by EprocInit() on first RpmFindProcess. Zero = uninitialised.
u32 g_eproc_dtb     = 0;   // non-static: reader thread needs liveness poll
u32 g_eproc_pid     = 0;
u32 g_eproc_links   = 0;
u32 g_eproc_imgname = 0;

// EPROCESS.Peb offset. Verified values from live dumps + public database:
//   Win10 20H1..22H2 / Win11 21H2..23H2 (Cobalt/Vibranium) = 0x550
//   Win11 24H2 / 25H2 / 26H2         (Germanium)          = 0x2E0
// EprocInit() sets the correct default from the kEprocLayouts row, and
// DiscoverPebOffset() is only used as a safety net if a future update
// drifts the offset inside a range.
u32 g_eproc_peb_off = 0x550;

// Reads OUR own PEB via TEB (gs:[0x60] on x64), returns non-zero on success.
static u64 GetOwnPeb(void) {
#if defined(_M_X64) || defined(__x86_64__)
    // NT_TIB.Self at gs:[0x30], PEB at gs:[0x60]
    return (u64)__readgsqword(0x60);
#else
    return 0;
#endif
}

// Walk EPROCESS list once, find OUR process by PID, scan offsets 0x300-0x700
// for one that matches our own known PEB. Caches into g_eproc_peb_off.
// Called lazily on first RpmFindProcess. Returns TRUE on success.
static BOOL DiscoverPebOffset(HANDLE hDev, u64 sysCR3, u64 psisp) {
    u64 our_peb = GetOwnPeb();
    u64 our_pid = (u64)GetCurrentProcessId();
    DH_INFO("DiscoverPebOffset: self PID=%llu PEB=0x%llX (via gs:[0x60])",
            our_pid, our_peb);
    if (!our_peb) {
        DH_ERROR("DiscoverPebOffset: own PEB via gs:[0x60] is 0 - cannot calibrate");
        return FALSE;
    }

    u64 head = psisp + g_eproc_links;
    u64 cur;
    if (!RpmRead64(hDev, sysCR3, head, &cur)) {
        DH_ERROR("DiscoverPebOffset: first flink read failed");
        return FALSE;
    }

    int count = 0;
    while (cur != head && count < 2048) {
        u64 eproc = cur - g_eproc_links;
        u64 pid = 0;
        RpmRead64(hDev, sysCR3, eproc + g_eproc_pid, &pid);
        if (pid == our_pid) {
            DH_INFO("DiscoverPebOffset: found self EPROCESS @ 0x%llX (scanned %d)",
                    eproc, count);
            // Scan wider — 25H2 may place PEB at 0x5A8 or higher
            for (u32 off = 0x200; off < 0xA00; off += 8) {
                u64 candidate = 0;
                if (RpmRead64(hDev, sysCR3, eproc + off, &candidate)
                    && candidate == our_peb) {
                    g_eproc_peb_off = off;
                    DH_INFO("DiscoverPebOffset: EPROCESS.Peb = +0x%X (matched self PEB=0x%llX)",
                            off, our_peb);
                    return TRUE;
                }
            }
            // Dump every 8-byte value in the plausible range so we see WHERE
            // our_peb landed (or if it landed at all)
            DH_WARN("DiscoverPebOffset: PEB=0x%llX not found in 0x200-0xA00 — dumping candidates:",
                    our_peb);
            for (u32 off = 0x200; off < 0xA00; off += 8) {
                u64 v = 0;
                if (RpmRead64(hDev, sysCR3, eproc + off, &v) &&
                    v > 0x00007FF000000000ULL && v < 0x00008000000000ULL) {
                    DH_INFO("  eproc+0x%03X = 0x%llX (user-space candidate)", off, v);
                }
            }
            return FALSE;
        }
        u64 flink = 0;
        if (!RpmRead64(hDev, sysCR3, cur, &flink) || flink == cur) break;
        cur = flink;
        count++;
    }
    DH_ERROR("DiscoverPebOffset: self PID=%llu not found in EPROCESS list (%d scanned)",
             our_pid, count);
    return FALSE;
}

// Fill g_eproc_* from the layout table for the running Windows build.
// Called lazily on first RpmFindProcess. Idempotent — re-calls short-circuit.
static BOOL EprocInit(void) {
    if (g_eproc_links) return TRUE;   // already initialised

    typedef LONG (WINAPI *pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
    pfnRtlGetVersion pRGV = (pfnRtlGetVersion)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW vi = { sizeof(vi) };
    if (pRGV) pRGV(&vi);
    u32 build = vi.dwBuildNumber;

    for (int i = 0; i < (int)DH_ARR_LEN(kEprocLayouts); i++) {
        const DhEprocLayout* l = &kEprocLayouts[i];
        if (build >= l->min_build && build <= l->max_build) {
            g_eproc_dtb     = l->dtb;
            g_eproc_pid     = l->pid;
            g_eproc_links   = l->links;
            g_eproc_imgname = l->imgname;
            g_eproc_peb_off = l->peb_default;
            DH_INFO("EprocInit: %s (build=%u) — DTB=0x%X PID=0x%X LINKS=0x%X IMGNAME=0x%X PEB=0x%X",
                    l->label, build,
                    l->dtb, l->pid, l->links, l->imgname, l->peb_default);
            return TRUE;
        }
    }
    DH_ERROR("EprocInit: Windows build %u not in known-good table. "
             "Supported: Win10 20H1-22H2 (19041..19045), "
             "Win11 21H2/22H2/23H2 (22000, 22621, 22631), "
             "Win11 24H2/25H2/26H1 (26100..30000). "
             "Add a row to kEprocLayouts for other builds.",
             build);
    return FALSE;
}

// ---- Direct-syscall NtQuerySystemInformation (ntdll-hook bypass) ---------
//
// ACE (and other AC/EDR products) install user-mode hooks on ntdll's
// NtQuerySystemInformation. Symptom on this build: NtQSI(64)
// (SystemExtendedHandleInformation) size-probe returns need_h≈56 instead
// of the real ~10-100 MB; second call fails; the handle-table fallback path
// finds nothing. We rebuild a fresh unhooked syscall stub at runtime by:
//   1. Mapping C:\Windows\System32\ntdll.dll from disk (read-only view).
//   2. Parsing its export table, resolving NtQuerySystemInformation RVA.
//   3. Reading the u32 SSN out of the on-disk stub prologue
//      (4C 8B D1 B8 <SSN u32> ...).
//   4. Building a 12-byte { mov r10,rcx; mov eax,SSN; syscall; ret } into
//      a PAGE_EXECUTE_READWRITE page and casting to a function pointer.
// This gives us an unhooked NtQuerySystemInformation regardless of what
// ACE patched into the loaded ntdll.
typedef NTSTATUS (NTAPI *dh_pfnNQSI)(ULONG, PVOID, ULONG, PULONG);

static DWORD dh_rva_to_file_off(const IMAGE_NT_HEADERS64* nt, DWORD rva)
{
    const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION((PIMAGE_NT_HEADERS64)nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD va = sh[i].VirtualAddress;
        DWORD sz = sh[i].Misc.VirtualSize;
        if (rva >= va && rva < va + sz)
            return rva - va + sh[i].PointerToRawData;
    }
    return 0;
}

static ULONG dh_read_nqsi_ssn_from_disk(void)
{
    HANDLE f = CreateFileW(L"C:\\Windows\\System32\\ntdll.dll",
                           GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) { CloseHandle(f); return 0; }
    const BYTE* base = (const BYTE*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    CloseHandle(f);
    if (!base) return 0;

    ULONG ssn = 0;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) goto done;
        const IMAGE_NT_HEADERS64* nt =
            (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) goto done;

        DWORD expRVA = nt->OptionalHeader
                         .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
                         .VirtualAddress;
        if (!expRVA) goto done;
        DWORD expOff = dh_rva_to_file_off(nt, expRVA);
        if (!expOff) goto done;
        const IMAGE_EXPORT_DIRECTORY* ed =
            (const IMAGE_EXPORT_DIRECTORY*)(base + expOff);
        DWORD namesOff = dh_rva_to_file_off(nt, ed->AddressOfNames);
        DWORD ordsOff  = dh_rva_to_file_off(nt, ed->AddressOfNameOrdinals);
        DWORD funcsOff = dh_rva_to_file_off(nt, ed->AddressOfFunctions);
        if (!namesOff || !ordsOff || !funcsOff) goto done;

        const DWORD*  nameRvas = (const DWORD*)(base + namesOff);
        const USHORT* ords     = (const USHORT*)(base + ordsOff);
        const DWORD*  funcs    = (const DWORD*)(base + funcsOff);

        for (DWORD i = 0; i < ed->NumberOfNames; i++) {
            DWORD noff = dh_rva_to_file_off(nt, nameRvas[i]);
            if (!noff) continue;
            const char* name = (const char*)(base + noff);
            if (strcmp(name, "NtQuerySystemInformation") != 0) continue;
            DWORD foff = dh_rva_to_file_off(nt, funcs[ords[i]]);
            if (!foff) break;
            const BYTE* stub = base + foff;
            // Expect: 4C 8B D1 B8 <ssn u32> ...
            if (stub[0] == 0x4C && stub[1] == 0x8B &&
                stub[2] == 0xD1 && stub[3] == 0xB8)
                ssn = *(const ULONG*)(stub + 4);
            break;
        }
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ssn = 0;
    }
    UnmapViewOfFile((LPCVOID)base);
    return ssn;
}

static dh_pfnNQSI dh_get_direct_nqsi(void)
{
    static dh_pfnNQSI cached = NULL;
    static int tried = 0;
    if (cached) return cached;
    if (tried) return NULL;
    tried = 1;

    ULONG ssn = dh_read_nqsi_ssn_from_disk();
    if (!ssn) return NULL;

    BYTE stub[] = {
        0x4C, 0x8B, 0xD1,             // mov r10, rcx
        0xB8, 0x00, 0x00, 0x00, 0x00, // mov eax, imm32 (SSN — patched below)
        0x0F, 0x05,                   // syscall
        0xC3                          // ret
    };
    *(ULONG*)(stub + 4) = ssn;

    void* page = VirtualAlloc(NULL, sizeof(stub),
                              MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!page) return NULL;
    memcpy(page, stub, sizeof(stub));
    FlushInstructionCache(GetCurrentProcess(), page, sizeof(stub));
    cached = (dh_pfnNQSI)page;
#ifdef AH_DIAG
    ah_procs_log("dh_get_direct_nqsi: unhooked stub built at %p SSN=0x%lX",
                 page, (unsigned long)ssn);
#endif
    return cached;
}

// v1.0.38.11: PspCidTable direct walk. Bulletproof against ACE DKOM-unlink
// from PsActiveProcessLinks — PspCidTable is what NtOpenProcess itself uses
// to resolve PID → EPROCESS, so ACE cannot remove UAGame from it without
// breaking the game's own kernel handle operations.
//
// Method:
//   1. Locate PspCidTable VA in ntoskrnl by disassembling
//      PsLookupProcessByProcessId (exported). First few bytes contain
//      `LEA rcx, [PspCidTable]` = 48 8D 0D disp32.
//   2. Read HANDLE_TABLE pointer at PspCidTable.
//   3. Decode HANDLE_TABLE.TableCode: bits 0-2 = level (0/1/2), rest = root.
//   4. Walk tree for index = target_pid >> 2.
//   5. Leaf HANDLE_TABLE_ENTRY has Object = EPROCESS with lock bits in low 3.
//   6. Read DTB from EPROCESS+g_eproc_dtb.
//
// Returns TRUE on success, fills *out_eproc / *out_dtb.
static BOOL RpmFindEprocViaPspCidTable(HANDLE hDev, u64 sysCR3, u64 ntBase,
                                        u64 target_pid,
                                        u64* out_eproc, u64* out_dtb)
{
    if (out_eproc) *out_eproc = 0;
    if (out_dtb)   *out_dtb   = 0;
    if (!target_pid || !ntBase) return FALSE;

    // --- Step 1: find PsLookupProcessByProcessId export in ntoskrnl. ---
    IMAGE_DOS_HEADER dh = {0};
    if (!RpmReadVirtual(hDev, sysCR3, ntBase, &dh, sizeof(dh))
        || dh.e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    IMAGE_NT_HEADERS64 nh = {0};
    if (!RpmReadVirtual(hDev, sysCR3, ntBase + dh.e_lfanew, &nh, sizeof(nh))
        || nh.Signature != IMAGE_NT_SIGNATURE) return FALSE;
    DWORD expRVA  = nh.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!expRVA) return FALSE;
    IMAGE_EXPORT_DIRECTORY ed = {0};
    if (!RpmReadVirtual(hDev, sysCR3, ntBase + expRVA, &ed, sizeof(ed))) return FALSE;

    const char* target = "PsLookupProcessByProcessId";
    DWORD targetLen = (DWORD)strlen(target);
    DWORD psl_rva = 0;
    for (DWORD i = 0; i < ed.NumberOfNames; i++) {
        DWORD nameRVA = 0;
        if (!RpmReadVirtual(hDev, sysCR3,
            ntBase + ed.AddressOfNames + i * 4, &nameRVA, 4)) continue;
        char name[64] = {0};
        if (!RpmReadVirtual(hDev, sysCR3, ntBase + nameRVA, name, 63)) continue;
        if (strncmp(name, target, targetLen) == 0 && name[targetLen] == 0) {
            USHORT ordIdx = 0;
            RpmReadVirtual(hDev, sysCR3,
                ntBase + ed.AddressOfNameOrdinals + i * 2, &ordIdx, 2);
            RpmReadVirtual(hDev, sysCR3,
                ntBase + ed.AddressOfFunctions + ordIdx * 4, &psl_rva, 4);
            break;
        }
    }
    if (!psl_rva) {
        DH_ERROR("PspCidTable: PsLookupProcessByProcessId not in exports");
        return FALSE;
    }

    // --- Step 2: read first 96 bytes of the function, find LEA rcx,[rip+disp32]
    // which loads PspCidTable's address. Pattern: 48 8D 0D XX XX XX XX.
    // Modern ntoskrnl builds sometimes use rax first (`48 8D 05 ..`) — accept
    // both r/m destinations by masking the ModR/M byte low bits.
    u8 code[96] = {0};
    if (!RpmReadVirtual(hDev, sysCR3, ntBase + psl_rva, code, sizeof(code))) return FALSE;
    u64 pspcid_va = 0;
    for (int i = 0; i < (int)sizeof(code) - 7; i++) {
        // 48 8D <ModR/M with mod=00 and rm=101> XX XX XX XX
        if (code[i] == 0x48 && code[i+1] == 0x8D
            && (code[i+2] & 0xC7) == 0x05)
        {
            int32_t disp = *(int32_t*)(code + i + 3);
            u64 next_ip = ntBase + psl_rva + (u32)i + 7;
            pspcid_va = next_ip + (int64_t)disp;
            DH_INFO("PspCidTable: LEA @ nt+0x%X (offset+%d) disp=%+d → PspCidTable VA=0x%llX",
                    (unsigned)psl_rva, i, (int)disp,
                    (unsigned long long)pspcid_va);
            break;
        }
    }
    if (!pspcid_va) {
        DH_ERROR("PspCidTable: LEA pattern not found in first %zu bytes of PsLookupProcessByProcessId",
                 sizeof(code));
        return FALSE;
    }

    // --- Step 3: PspCidTable is `PHANDLE_TABLE PspCidTable` — read pointer. ---
    u64 ht_va = 0;
    if (!RpmRead64(hDev, sysCR3, pspcid_va, &ht_va) || !ht_va) {
        DH_ERROR("PspCidTable: read of pointer at 0x%llX failed", (unsigned long long)pspcid_va);
        return FALSE;
    }
    if ((ht_va >> 48) != 0xFFFF) {
        DH_ERROR("PspCidTable: HANDLE_TABLE VA 0x%llX not canonical kernel", (unsigned long long)ht_va);
        return FALSE;
    }

    // --- Step 4: read HANDLE_TABLE.TableCode at +8 (stable Win10/11). ---
    u64 table_code = 0;
    if (!RpmRead64(hDev, sysCR3, ht_va + 8, &table_code) || !table_code) {
        DH_ERROR("PspCidTable: TableCode read failed at HT=0x%llX+8", (unsigned long long)ht_va);
        return FALSE;
    }
    u32 level    = (u32)(table_code & 3);
    u64 root_va  = table_code & ~(u64)7;
    DH_INFO("PspCidTable: HT=0x%llX TableCode=0x%llX level=%u root=0x%llX",
            (unsigned long long)ht_va, (unsigned long long)table_code,
            level, (unsigned long long)root_va);

    // --- Step 5: walk tree for index = target_pid / 4.
    // Leaf entries: 16 bytes each (Object + HighValue) → 256 entries per 4 KB page.
    // Intermediate: 8-byte pointers → 512 entries per 4 KB page.
    u64 index = target_pid >> 2;
    u64 entry_va = 0;

    if (level == 0) {
        // 1-level: root is directly array of HANDLE_TABLE_ENTRY.
        entry_va = root_va + index * 16;
    } else if (level == 1) {
        // 2-level: root is array of pointers to leaf arrays (256 entries each).
        u64 hi = index >> 8;
        u64 lo = index & 0xFF;
        u64 sub_va = 0;
        if (!RpmRead64(hDev, sysCR3, root_va + hi * 8, &sub_va) || !sub_va) {
            DH_ERROR("PspCidTable: L1 sub-page read failed hi=%llu",
                     (unsigned long long)hi);
            return FALSE;
        }
        entry_va = sub_va + lo * 16;
    } else if (level == 2) {
        // 3-level: root is array of pointers to mid-arrays (512 each),
        // each mid points to leaf arrays (256 each).
        u64 hi  = index >> 17;              // 512 * 256 = 131072 = 2^17
        u64 mid = (index >> 8) & 0x1FF;
        u64 lo  = index & 0xFF;
        u64 mid_va = 0, sub_va = 0;
        if (!RpmRead64(hDev, sysCR3, root_va + hi * 8, &mid_va) || !mid_va) {
            DH_ERROR("PspCidTable: L2 mid read failed hi=%llu", (unsigned long long)hi);
            return FALSE;
        }
        if (!RpmRead64(hDev, sysCR3, mid_va + mid * 8, &sub_va) || !sub_va) {
            DH_ERROR("PspCidTable: L2 sub read failed mid=%llu", (unsigned long long)mid);
            return FALSE;
        }
        entry_va = sub_va + lo * 16;
    } else {
        DH_ERROR("PspCidTable: unknown level %u", level);
        return FALSE;
    }

    // --- Step 6: read HANDLE_TABLE_ENTRY.Object. For CID table, Object is
    // the raw EPROCESS/KTHREAD pointer with lock bits in low 3 bits.
    u64 obj_raw = 0;
    if (!RpmRead64(hDev, sysCR3, entry_va, &obj_raw)) {
        DH_ERROR("PspCidTable: leaf entry read failed at 0x%llX",
                 (unsigned long long)entry_va);
        return FALSE;
    }
    u64 eproc = obj_raw & ~(u64)7;
    if ((eproc >> 48) != 0xFFFF) {
        DH_ERROR("PspCidTable: entry Object 0x%llX not canonical kernel (pid=%llu index=%llu entry=0x%llX)",
                 (unsigned long long)obj_raw, (unsigned long long)target_pid,
                 (unsigned long long)index, (unsigned long long)entry_va);
        return FALSE;
    }

    // --- Step 7: sanity — read PID field from the resolved EPROCESS,
    // must match target_pid. Guards against dangling / freed pool.
    u64 pid_check = 0;
    if (!RpmRead64(hDev, sysCR3, eproc + g_eproc_pid, &pid_check)) {
        DH_ERROR("PspCidTable: PID field read at eproc=0x%llX+0x%X failed",
                 (unsigned long long)eproc, g_eproc_pid);
        return FALSE;
    }
    if (pid_check != target_pid) {
        DH_ERROR("PspCidTable: PID mismatch — eproc.pid=%llu target=%llu (dangling entry?)",
                 (unsigned long long)pid_check, (unsigned long long)target_pid);
        return FALSE;
    }

    // --- Step 8: read DTB.
    u64 dtb = 0;
    if (!RpmRead64(hDev, sysCR3, eproc + g_eproc_dtb, &dtb) || !dtb) {
        DH_ERROR("PspCidTable: DTB read failed at eproc=0x%llX+0x%X",
                 (unsigned long long)eproc, g_eproc_dtb);
        return FALSE;
    }

    DH_INFO("PspCidTable: RESOLVED pid=%llu → eproc=0x%llX DTB=0x%llX",
            (unsigned long long)target_pid,
            (unsigned long long)eproc,
            (unsigned long long)dtb);
    if (out_eproc) *out_eproc = eproc;
    if (out_dtb)   *out_dtb   = dtb;
    return TRUE;
}

BOOL RpmFindProcess(HANDLE hDev, u64 sysCR3,
                    const char* procName, u64* procCR3, u64* eprocessOut)
{
    ah_test_trace_write("RpmFindProcess ENTER target='%s' sysCR3=0x%llX",   // TEST-REMOVE
        procName ? procName : "(null)", (unsigned long long)sysCR3);
    *procCR3 = 0;
    if (eprocessOut) *eprocessOut = 0;

    // v1.0.34: userspace attach FIRST. Toolhelp32 walks PspCidTable, not
    // PsActiveProcessLinks — works even when ACE has unlinked the game's
    // EPROCESS (Pattern-A cold-start failures). OpenProcess(VM_READ)
    // succeeds on ~95% of consumer rigs (HVCI off, no strict handle strip).
    // Only fall back to the kdu phys/CR3 walk if userspace fails.
    if (AhUspaceOk()) {
        // Already attached from a previous call; just re-verify the handle
        // is still good (game may have exited or ACE may have revoked
        // PROCESS_VM_READ post-open).
        if (AhUspaceVerify()) {
            *procCR3 = AH_CR3_USPACE;
            if (eprocessOut) *eprocessOut = 0;
#ifdef AH_DIAG
            ah_procs_log("USPACE: re-verified existing handle, using userspace path");
#endif
            return TRUE;
        }
        AhUspaceDetach();   // stale — drop it
    }
    {
        DWORD u_pid = 0; u64 u_base = 0;
        if (AhUspaceAttach(procName, &u_pid, &u_base)) {
            *procCR3 = AH_CR3_USPACE;
            if (eprocessOut) *eprocessOut = 0;
#ifdef AH_DIAG
            ah_procs_log("USPACE: attached PID=%lu base=0x%llX — userspace primary path in use",
                         u_pid, (unsigned long long)u_base);
#endif
            return TRUE;
        }
    }

#ifdef AH_DIAG
    {
        HANDLE lh = CreateFileA("C:\\Users\\Public\\ah_procs.log",
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lh != INVALID_HANDLE_VALUE) {
            SetFilePointer(lh, 0, NULL, FILE_END);
            char line[128];
            int n = _snprintf(line, sizeof(line),
                "== RpmFindProcess CALL sysCR3=0x%llX target='%s' ==\r\n",
                (unsigned long long)sysCR3, procName);
            DWORD w = 0;
            WriteFile(lh, line, (DWORD)n, &w, NULL);
            CloseHandle(lh);
        }
    }
#endif
    if (!EprocInit()) {
#ifdef AH_DIAG
        HANDLE lh = CreateFileA("C:\\Users\\Public\\ah_procs.log",
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lh != INVALID_HANDLE_VALUE) {
            SetFilePointer(lh, 0, NULL, FILE_END);
            const char* m = "EprocInit FAILED -- unsupported Windows build?\r\n";
            DWORD w = 0;
            WriteFile(lh, m, (DWORD)strlen(m), &w, NULL);
            CloseHandle(lh);
        }
#endif
        return FALSE;
    }

    // Step 1: find PsInitialSystemProcess via ntoskrnl export
    // Simpler approach: use NtQuerySystemInformation to get ntoskrnl base,
    // then walk PE exports to find PsInitialSystemProcess, read via phys.

    // Get ntoskrnl base from SystemModuleInformation
    typedef struct {
        PVOID  Reserved[2];
        PVOID  Base;
        ULONG  Size;
        ULONG  Flags;
        USHORT Index;
        USHORT Unknown;
        USHORT LoadCount;
        USHORT NameOffset;
        CHAR   Name[256];
    } RTL_SYSTEM_MODULE;

    typedef struct {
        ULONG Count;
        RTL_SYSTEM_MODULE Modules[1];
    } RTL_SYSTEM_MODULES;

    typedef NTSTATUS (NTAPI *pfnNtQuerySystemInformation)(
        ULONG, PVOID, ULONG, PULONG);

    // Direct syscall stub built at runtime from a fresh disk-mapped ntdll.
    // Bypasses whatever user-mode hook ACE / EDR patched into the loaded
    // ntdll's NtQuerySystemInformation. Falls back to GetProcAddress only
    // if the disk-parse-and-build path fails (fresh install without ntdll
    // on disk — shouldn't happen).
    pfnNtQuerySystemInformation pNtQSI =
        (pfnNtQuerySystemInformation)dh_get_direct_nqsi();
    if (!pNtQSI) {
        pNtQSI = (pfnNtQuerySystemInformation)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    }
    if (!pNtQSI) {
        DH_ERROR("NtQuerySystemInformation not found");
        return FALSE;
    }

    // v1.0.22: on Win11 25H2 with VBS/HVCI, NtQuerySystemInformation(11)
    // returns Module.Base=0 unless the caller holds SeDebugPrivilege
    // (KASLR-base disclosure mitigation). Enable it here — overlay is
    // elevated, so AdjustTokenPrivileges succeeds.
    {
        HANDLE hTok = NULL;
        if (OpenProcessToken(GetCurrentProcess(),
                             TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok))
        {
            LUID luid;
            if (LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &luid)) {
                TOKEN_PRIVILEGES tp = {0};
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hTok, FALSE, &tp, sizeof(tp), NULL, NULL);
#ifdef AH_DIAG
                DWORD gle = GetLastError();
                DH_INFO("SeDebugPrivilege enable gle=%lu", gle);
#endif
            }
            CloseHandle(hTok);
        }
    }

    ULONG needed = 0;
    pNtQSI(11 /*SystemModuleInformation*/, NULL, 0, &needed);
    if (!needed) return FALSE;

    RTL_SYSTEM_MODULES* mods = (RTL_SYSTEM_MODULES*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
    if (!mods) return FALSE;

    NTSTATUS st = pNtQSI(11, mods, needed, &needed);
    if (st < 0 || mods->Count == 0) {
        HeapFree(GetProcessHeap(), 0, mods);
        DH_ERROR("NtQuerySystemInformation(11) failed: 0x%lX", st);
        return FALSE;
    }

    // v1.0.22: on Win11 25H2 with VBS/HVCI, Modules[0] is hvix64.sys /
    // Secure Kernel — NOT ntoskrnl. Walk all modules and pick the one
    // whose filename starts with "ntoskrnl", "ntkrnl", "ntkrpamp", or
    // "ntkrla57" (any x64 kernel variant).
    u64 ntBase = 0;
    for (ULONG mi = 0; mi < mods->Count; mi++) {
        const char* fname = mods->Modules[mi].Name + mods->Modules[mi].NameOffset;
        if (_strnicmp(fname, "ntoskrnl", 8) == 0 ||
            _strnicmp(fname, "ntkrnl",    6) == 0 ||
            _strnicmp(fname, "ntkrla",    6) == 0 ||
            _strnicmp(fname, "ntkrpamp",  8) == 0)
        {
            ntBase = (u64)mods->Modules[mi].Base;
            DH_INFO("ntoskrnl match at Modules[%lu] '%s' base=0x%llX",
                    (unsigned long)mi, fname, ntBase);
            break;
        }
    }
    if (!ntBase) {
        DH_INFO("ntoskrnl NOT FOUND in %lu modules — fallback to Modules[0]",
                (unsigned long)mods->Count);
        ntBase = (u64)mods->Modules[0].Base;
    }
    DH_INFO("ntoskrnl base: 0x%llX", ntBase);
    HeapFree(GetProcessHeap(), 0, mods);

    // Step 2: read ntoskrnl PE to find PsInitialSystemProcess export
    IMAGE_DOS_HEADER dos;
    if (!RpmReadVirtual(hDev, sysCR3, ntBase, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        DH_ERROR("failed to read ntoskrnl DOS header");
        return FALSE;
    }

    IMAGE_NT_HEADERS64 nt;
    if (!RpmReadVirtual(hDev, sysCR3, ntBase + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        DH_ERROR("failed to read ntoskrnl NT header");
        return FALSE;
    }

    DWORD expRVA  = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD expSize = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (!expRVA || !expSize) {
        DH_ERROR("no export directory in ntoskrnl");
        return FALSE;
    }

    IMAGE_EXPORT_DIRECTORY expDir;
    if (!RpmReadVirtual(hDev, sysCR3, ntBase + expRVA, &expDir, sizeof(expDir)))
        return FALSE;

    u64 psisp = 0;
    const char* target = "PsInitialSystemProcess";
    DWORD targetLen = (DWORD)strlen(target);

    // Walk export name table
    for (DWORD i = 0; i < expDir.NumberOfNames; i++) {
        DWORD nameRVA = 0;
        if (!RpmReadVirtual(hDev, sysCR3,
            ntBase + expDir.AddressOfNames + i * 4, &nameRVA, 4))
            continue;

        char name[64] = {0};
        if (!RpmReadVirtual(hDev, sysCR3, ntBase + nameRVA, name, 63))
            continue;

        if (strncmp(name, target, targetLen) == 0 && name[targetLen] == 0) {
            USHORT ordIdx = 0;
            RpmReadVirtual(hDev, sysCR3,
                ntBase + expDir.AddressOfNameOrdinals + i * 2, &ordIdx, 2);

            DWORD funcRVA = 0;
            RpmReadVirtual(hDev, sysCR3,
                ntBase + expDir.AddressOfFunctions + ordIdx * 4, &funcRVA, 4);

            // PsInitialSystemProcess is a pointer — read the pointer value
            RpmRead64(hDev, sysCR3, ntBase + funcRVA, &psisp);
            break;
        }
    }

    if (!psisp) {
        DH_ERROR("PsInitialSystemProcess not found in exports");
        return FALSE;
    }

    DH_INFO("PsInitialSystemProcess -> EPROCESS @ 0x%llX", psisp);

    // Cache for later reuse (RpmHideOwnProcess doesn't need to re-resolve).
    g_rpm_psisp = psisp;

    // Sanity-check the layout table row: read System's KPROCESS.DirectoryTableBase
    // from g_eproc_dtb — must equal the CR3 we walked in with. Mismatch = wrong
    // offsets for this build, bail with actionable error instead of walking a
    // wrong LIST_ENTRY chain into garbage.
    {
        u64 sys_dtb = 0;
        if (!RpmRead64(hDev, sysCR3, psisp + g_eproc_dtb, &sys_dtb)) {
            DH_ERROR("layout probe: read System EPROCESS+DTB(0x%X) failed",
                     g_eproc_dtb);
            return FALSE;
        }
        if ((sys_dtb & ~0xFFFULL) != (sysCR3 & ~0xFFFULL)) {
            DH_ERROR("layout probe FAILED: EPROCESS+0x%X = 0x%llX, expected sysCR3=0x%llX. "
                     "kEprocLayouts row for this build is wrong — verify offsets against "
                     "Vergilius Project.",
                     g_eproc_dtb, sys_dtb, sysCR3);
            return FALSE;
        }
        DH_INFO("layout probe OK: System DTB @ +0x%X matches sysCR3", g_eproc_dtb);
    }

    // Lazy-discover EPROCESS.Peb offset from OUR OWN process. First call per
    // run pays a one-time list-walk cost. If discovery fails, g_eproc_peb_off
    // keeps its default (0x550) which is right on 26100 but wrong on 26200.
    static BOOL s_peb_off_discovered = FALSE;
    if (!s_peb_off_discovered) {
        s_peb_off_discovered = TRUE;
        DiscoverPebOffset(hDev, sysCR3, psisp);
    }

    // v1.0.38.3: authoritative PID resolution via NtQSI(5) BEFORE the
    // ActiveProcessLinks walk. NtQSI(5) enumerates PspCidTable, which ACE
    // can't cheaply forge — the PID it returns is the live PID even if
    // ACE has scrubbed EPROCESS.ImageFileName or DKOM-unlinked the game
    // from ActiveProcessLinks. Once known, target_pid drives an extra
    // accept criterion in the bidirectional walker below: name-match OR
    // pid==target_pid. For a PID-authoritative match, the MZ probe is
    // skipped (freshly-spawned process may not have its PE header
    // page-resident through the DTB yet — this is why the previous
    // walker classified all 7 UAGame hits as "decoys" and never latched
    // onto the real one).
    u64 target_pid = 0;
    {
        typedef struct _SYS_PROC_INF_LOCAL {
            ULONG NextEntryOffset;
            ULONG NumberOfThreads;
            BYTE Reserved1[48];
            UNICODE_STRING ImageName;
            LONG BasePriority;
            HANDLE UniqueProcessId;
        } SYS_PROC_INF_LOCAL;
        ULONG need_p = 0;
        pNtQSI(5, NULL, 0, &need_p);
        if (need_p) {
            need_p += 0x10000;
            PVOID pbuf = VirtualAlloc(NULL, need_p, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
            if (pbuf) {
                NTSTATUS stp = pNtQSI(5, pbuf, need_p, &need_p);
                if (stp >= 0) {
                    SYS_PROC_INF_LOCAL* pi = (SYS_PROC_INF_LOCAL*)pbuf;
                    while (1) {
                        if (pi->ImageName.Buffer && pi->ImageName.Length) {
                            size_t nchars = pi->ImageName.Length / sizeof(WCHAR);
                            static const wchar_t* WL_UP[] = {
                                L"UAGame.exe",
                                L"UAGameShipping.exe",
                                NULL
                            };
                            for (int _wi = 0; !target_pid && WL_UP[_wi]; _wi++) {
                                size_t wl = wcslen(WL_UP[_wi]);
                                if (nchars != wl) continue;
                                int ok_m = 1;
                                for (size_t k = 0; k < wl; k++) {
                                    wchar_t a = pi->ImageName.Buffer[k];
                                    wchar_t b = WL_UP[_wi][k];
                                    if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
                                    if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
                                    if (a != b) { ok_m = 0; break; }
                                }
                                if (ok_m) target_pid = (u64)(uintptr_t)pi->UniqueProcessId;
                            }
                        }
                        if (target_pid || !pi->NextEntryOffset) break;
                        pi = (SYS_PROC_INF_LOCAL*)((BYTE*)pi + pi->NextEntryOffset);
                    }
                }
                VirtualFree(pbuf, 0, MEM_RELEASE);
            }
        }
#ifdef AH_DIAG
        ah_procs_log("target_pid via NtQSI(5) = %llu (0 = unknown; walker matches by name only)",
                     (unsigned long long)target_pid);
#endif
    }

    // Step 3: BIDIRECTIONAL walk of ActiveProcessLinks (Flink + Blink).
    // Observed on Win11 25H2 (26220) with live ACE: forward Flink chain
    // gets a poisoned pointer ~1100 nodes deep — RpmRead64 rejects it
    // and the walk terminates before reaching the live UAGame's
    // EPROCESS. UAGame is spawned late, so it lives near tail in Flink
    // order = near head->Blink in Blink order. Walking Blink from head
    // first reaches the newest EPROCESSes (including UAGame) within a
    // handful of steps; the forward Flink walk still runs after to
    // cover older processes ACE hasn't tampered with. Visited set is
    // shared across both directions — if backward walk meets forward
    // walk, coverage is complete.
    u64 head = psisp + g_eproc_links;
    u32 count = 0;
    u64 flink = 0;   // scratch used inside the inner loop
    // v1.0.22 diag: dump every walked ImageFileName to ah_procs.log so
    // we can see what UAGame is really called in EPROCESS on Win11 25H2.
#ifdef AH_DIAG
    u32 diag_read_fail = 0;
    u32 diag_read_ok   = 0;
    u32 diag_cycle_at  = 0;   // step at which a FLINK-cycle was detected (0 = none)
#endif
    // Cycle detection: ACE forges FLINK chains that loop through synthetic
    // decoy EPROCESS structures (verified on this build via the 4096-step
    // scan exhausting on all-Acer decoys). Keep a visited-EPROCESS set;
    // if the walker sees the same eproc twice, we're in a cycle — bail so
    // we don't burn the whole budget spinning in fake nodes.
    const u32 VIS_CAP = 8192;
    u64* visited = (u64*)HeapAlloc(GetProcessHeap(), 0, VIS_CAP * sizeof(u64));
    u32 visited_n = 0;
    int cycle_hit = 0;
    for (int dir = 0; dir < 2; dir++) {
        // LIST_ENTRY layout: Flink at +0, Blink at +8. dir 0 walks
        // forward via Flink, dir 1 walks backward via Blink starting
        // from head. Both share `visited` — a hit on backward means
        // we met forward's territory (coverage complete).
        const u32 link_off = (dir == 0) ? 0u : 8u;
        u64 cur = 0;
        if (!RpmRead64(hDev, sysCR3, head + link_off, &cur)) continue;
    while (cur != head && count < VIS_CAP) {
        u64 eproc = cur - g_eproc_links;
        // Cycle probe — O(n) linear scan; n <= 8192 so worst-case ~32M ops
        // ≈ tens of ms one-off per FindProcess call. Fine.
        {
            int seen = 0;
            for (u32 vi = 0; vi < visited_n; vi++) {
                if (visited[vi] == eproc) { seen = 1; break; }
            }
            if (seen) {
                cycle_hit = 1;
#ifdef AH_DIAG
                diag_cycle_at = count;
#endif
                break;
            }
            if (visited_n < VIS_CAP) visited[visited_n++] = eproc;
        }

        char imgName[16] = {0};
        BOOL name_ok = RpmReadVirtual(hDev, sysCR3, eproc + g_eproc_imgname, imgName, 15);
        if (!name_ok) {
#ifdef AH_DIAG
            diag_read_fail++;
#endif
            goto next;
        }
#ifdef AH_DIAG
        diag_read_ok++;
        // Dump every walked name — even garbage; helps see what walker sees.
        {
            u64 pid_dbg = 0;
            RpmRead64(hDev, sysCR3, eproc + g_eproc_pid, &pid_dbg);
            HANDLE lh = CreateFileA("C:\\Users\\Public\\ah_procs.log",
                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (lh != INVALID_HANDLE_VALUE) {
                SetFilePointer(lh, 0, NULL, FILE_END);
                // Sanitize non-printables in name to '.' so log stays readable.
                char safe[16] = {0};
                for (int q = 0; q < 15; q++) {
                    unsigned char c = (unsigned char)imgName[q];
                    safe[q] = (c >= 32 && c < 127) ? (char)c : '.';
                }
                char line[128];
                int n = _snprintf(line, sizeof(line),
                    "walk#%u eproc=0x%llX pid=%llu name='%s'\r\n",
                    count, (unsigned long long)eproc,
                    (unsigned long long)pid_dbg, safe);
                if (n > 0) {
                    DWORD w = 0;
                    WriteFile(lh, line, (DWORD)n, &w, NULL);
                }
                CloseHandle(lh);
            }
        }
#endif

        // v1.0.36: HARD WHITELIST. Exact ImageFileName match (case-insensitive,
        // NUL-terminated). EPROCESS.ImageFileName is truncated at 15 chars.
        //   "UAGame.exe"        (10 chars) → fits
        //   "UAGameShipping.exe"(18 chars) → truncated to "UAGameShipping."
        // Substring 'arena_breakout' matched arena_breakout_infinite_launcher.
        // via ImageFileName truncation and let the reader latch on the .NET
        // launcher wrapper (base=0x400000). Killed the substring; if Steam
        // ships an actual UAGame.exe under a different basename we'll add
        // it explicitly by name.
        static const char* AH_WHITELIST[] = {
            "UAGame.exe",
            "UAGameShipping.",   // 15-char truncation of UAGameShipping.exe
            NULL
        };
        int _matched = 0;
        for (int _wi = 0; AH_WHITELIST[_wi]; _wi++) {
            if (_stricmp(imgName, AH_WHITELIST[_wi]) == 0) { _matched = 1; break; }
        }
        (void)procName;   // Whitelist is fixed; caller's procName is ignored.

        // v1.0.38.3: authoritative-PID accept. NtQSI(5) at the top of
        // this function resolved target_pid (from PspCidTable, which ACE
        // can't cheaply spoof); if this eproc's PID matches, it IS the
        // live game — even if ImageFileName is scrubbed or ImageBase
        // pages haven't been faulted in yet through the DTB.
        u64 pid_here = 0;
        RpmRead64(hDev, sysCR3, eproc + g_eproc_pid, &pid_here);
        int pid_match_target = (target_pid && pid_here == target_pid);

        if (_matched || pid_match_target) {
            u64 dtb = 0;
            RpmRead64(hDev, sysCR3, eproc + g_eproc_dtb, &dtb);
            if (dtb) {
                u64 pid = pid_here;   // keep DH_INFO variable name below
                // Decoy filter — ACE sometimes spawns duplicate processes
                // whose DTB translates to garbage / all-zeros to defeat
                // external walkers. Real game: the one whose PE header
                // is readable through its own DTB. We accept ANY canonical
                // ImageBase (PEB.ImageBaseAddress varies with ASLR on
                // Win11 25H2), not the hardcoded 0x140000000 — fetch PEB
                // + ImageBase to sanity-check, and if the value looks
                // canonical, verify MZ magic through DTB there.
                //
                // Fallback: if PEB probe fails, still allow through as a
                // last-resort (better than never finding UAGame on 25H2).
                BOOL is_real = FALSE;
                u64  probe_base = 0;
                {
                    u64 peb = 0;
                    if (RpmRead64(hDev, sysCR3, eproc + g_eproc_peb_off, &peb)
                        && peb && (peb >> 48) == 0)
                    {
                        u64 image_base = 0;
                        if (RpmRead64(hDev, dtb, peb + 0x10, &image_base)
                            && image_base && (image_base >> 48) == 0)
                        {
                            probe_base = image_base;
                            u16 mz = 0;
                            if (RpmReadVirtual(hDev, dtb, image_base, &mz, 2)
                                && mz == 0x5A4D)
                                is_real = TRUE;
                        }
                    }
                }
                if (!is_real) {
                    // Legacy path — try hardcoded 0x140000000 too.
                    u16 mz140 = 0;
                    if (RpmReadVirtual(hDev, dtb, 0x140000000ULL, &mz140, 2)
                        && mz140 == 0x5A4D)
                    {
                        probe_base = 0x140000000ULL;
                        is_real = TRUE;
                    }
                }
                // v1.0.38.3: PID-authoritative accept — MZ probe can miss
                // a freshly-launched process whose PE header hasn't been
                // page-faulted in through the game DTB yet. NtQSI(5) can't
                // lie about a live PID (reads PspCidTable), so if pid_here
                // equals target_pid, treat this EPROCESS as real. Wrapper
                // filter below still runs on probe_base and rejects if it
                // resolved to a low VA (< 4 GB).
                if (!is_real && pid_match_target) {
#ifdef AH_DIAG
                    ah_procs_log("PID-AUTH accept: PID=%llu (target from NtQSI(5)) — skipping MZ probe",
                                 (unsigned long long)pid);
#endif
                    is_real = TRUE;
                }
                // Wrapper filter: Steam ships arena_breakout.exe as a small
                // bootstrap PE with ASLR ImageBase in low VA (~0x570000).
                // Real UE4 shipping build (UAGame.exe) links at 0x140000000
                // and stays high even with ASLR — always >= 4 GB. Reject
                // anything below that threshold so a wrapper never wins over
                // the real game when both are alive.
                if (is_real && probe_base && probe_base < 0x100000000ULL) {
#ifdef AH_DIAG
                    ah_procs_log("REJECT wrapper: PID=%llu ImageBase=0x%llX (too low, not UE4 game)",
                                 (unsigned long long)pid, (unsigned long long)probe_base);
#endif
                    is_real = FALSE;
                }
                if (is_real) {
                    DH_INFO("found REAL '%s' PID=%llu EPROCESS=0x%llX CR3=0x%llX "
                            "ImageBase=0x%llX (past decoy filter)",
                            imgName, pid, eproc, dtb, probe_base);
                    *procCR3 = dtb;
                    if (eprocessOut) *eprocessOut = eproc;
                    HeapFree(GetProcessHeap(), 0, visited);
                    return TRUE;
                }
                DH_INFO("skip decoy '%s' PID=%llu CR3=0x%llX "
                        "(no MZ at PEB.ImageBase nor 0x140000000)",
                        imgName, pid, dtb);
            }
        }

    next:
        if (!RpmRead64(hDev, sysCR3, cur + link_off, &flink) || flink == cur)
            break;
        cur = flink;
        count++;
    }   // end inner while (per-direction walk)
    }   // end outer for (bidirectional Flink/Blink)
    HeapFree(GetProcessHeap(), 0, visited);
#ifdef AH_DIAG
    {
        HANDLE lh = CreateFileA("C:\\Users\\Public\\ah_procs.log",
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lh != INVALID_HANDLE_VALUE) {
            SetFilePointer(lh, 0, NULL, FILE_END);
            char line[192];
            int n = _snprintf(line, sizeof(line),
                "== BIDIR WALK END: total=%u reads_ok=%u reads_fail=%u cycle_at=%u %s ==\r\n",
                count, diag_read_ok, diag_read_fail, diag_cycle_at,
                cycle_hit ? "cycle-hit-or-met" : "natural-exit");
            DWORD w = 0;
            WriteFile(lh, line, (DWORD)n, &w, NULL);
            CloseHandle(lh);
        }
    }
#endif

    DH_ERROR("process '%s' not found in EPROCESS list (%u scanned) — "
             "all candidates were ACE decoys",
             procName, count);

    // v1.0.38.11: PspCidTable direct walk — bulletproof against DKOM
    // unlinking from ActiveProcessLinks. If target_pid is known
    // (NtQSI(5) resolved it at function top), walk PspCidTable directly:
    // it's what NtOpenProcess itself uses, ACE cannot remove UAGame's
    // entry from it without breaking every kernel handle op on the
    // game process.
    if (target_pid) {
        u64 cid_eproc = 0, cid_dtb = 0;
        if (RpmFindEprocViaPspCidTable(hDev, sysCR3, ntBase, target_pid,
                                        &cid_eproc, &cid_dtb))
        {
            // Verify PE header via CR3 — same wrapper filter as walker.
            u64 peb_cid = 0, ib_cid = 0;
            RpmRead64(hDev, sysCR3, cid_eproc + g_eproc_peb_off, &peb_cid);
            if (peb_cid && (peb_cid >> 48) == 0)
                RpmRead64(hDev, cid_dtb, peb_cid + 0x10, &ib_cid);
            if (ib_cid && ib_cid < 0x100000000ULL) {
                DH_ERROR("PspCidTable resolved eproc but ImageBase=0x%llX < 4 GB (wrapper?) — reject",
                         (unsigned long long)ib_cid);
            } else {
                DH_INFO("PspCidTable RESOLVED: eproc=0x%llX DTB=0x%llX ImgBase=0x%llX pid=%llu (bypassed DKOM)",
                        (unsigned long long)cid_eproc,
                        (unsigned long long)cid_dtb,
                        (unsigned long long)ib_cid,
                        (unsigned long long)target_pid);
                *procCR3 = cid_dtb;
                if (eprocessOut) *eprocessOut = cid_eproc;
                return TRUE;
            }
        } else {
            DH_ERROR("PspCidTable walk FAILED for target_pid=%llu — falling to NtQSI(64) fallback",
                     (unsigned long long)target_pid);
        }
    }

    // v1.0.22 fallback for Win11 25H2 where ACE DKOM-unlinks UAGame from
    // ActiveProcessLinks. Path:
    //   1. NtQuerySystemInformation(5) enum in user mode — DKOM can't hide
    //      from this API since it reads a different list (PspCidTable).
    //   2. For each hit whose name matches procName, take PID.
    //   3. NtQuerySystemInformation(64 = SystemExtendedHandleInformation)
    //      returns every handle in the system with its kernel Object ptr.
    //   4. Find any Process handle where the target EPROCESS's UniqueProcessId
    //      (RPM at Object + g_eproc_pid) matches our PID. Object IS the EPROCESS.
    //   5. Read DTB from that EPROCESS via sysCR3 → done.
    typedef struct _SYS_PROC {
        ULONG NextEntryOffset;
        ULONG NumberOfThreads;
        BYTE Reserved1[48];
        UNICODE_STRING ImageName;
        LONG BasePriority;
        HANDLE UniqueProcessId;
    } SYS_PROC;
    typedef NTSTATUS (NTAPI *pNtQSI2)(ULONG, PVOID, ULONG, PULONG);
    // Prefer the disk-built direct syscall stub — the loaded ntdll's
    // NtQuerySystemInformation is hooked by ACE (evidence: NtQSI(64) size
    // probe returns need_h=56 through the hooked path, but the real handle
    // table is 10-100 MB). GetProcAddress path stays only as a last resort.
    pNtQSI2 nqsi = (pNtQSI2)dh_get_direct_nqsi();
    if (!nqsi) {
        nqsi = (pNtQSI2)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                        "NtQuerySystemInformation");
    }
    if (!nqsi) return FALSE;

    // ---- Step 1-2: find UAGame PID via SystemProcessInformation. ----
    ULONG need_p = 0;
    nqsi(5, NULL, 0, &need_p);
    if (!need_p) return FALSE;
    need_p += 0x10000;
    PVOID pbuf = VirtualAlloc(NULL, need_p, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!pbuf) return FALSE;
    NTSTATUS st_p = nqsi(5, pbuf, need_p, &need_p);
    u64 uagame_pid = 0;
    if (st_p >= 0) {
        SYS_PROC* pi = (SYS_PROC*)pbuf;
        while (1) {
            if (pi->ImageName.Buffer && pi->ImageName.Length) {
                size_t nchars = pi->ImageName.Length / sizeof(WCHAR);
                // v1.0.36: HARD WHITELIST. NtQSI(5)'s ImageName is the full
                // basename with .exe extension (not the 15-char EPROCESS
                // truncation). Exact case-insensitive match against a fixed
                // list — no substrings, no procName-driven fuzz.
                static const wchar_t* AH_WL[] = {
                    L"UAGame.exe",
                    L"UAGameShipping.exe",
                    NULL
                };
                for (int _wi = 0; !uagame_pid && AH_WL[_wi]; _wi++) {
                    size_t wl = wcslen(AH_WL[_wi]);
                    if (nchars != wl) continue;
                    int ok = 1;
                    for (size_t k = 0; k < wl; k++) {
                        wchar_t a = pi->ImageName.Buffer[k];
                        wchar_t b = AH_WL[_wi][k];
                        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
                        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
                        if (a != b) { ok = 0; break; }
                    }
                    if (ok) uagame_pid = (u64)(uintptr_t)pi->UniqueProcessId;
                }
                (void)procName;
            }
            if (uagame_pid || !pi->NextEntryOffset) break;
            pi = (SYS_PROC*)((BYTE*)pi + pi->NextEntryOffset);
        }
    }
    VirtualFree(pbuf, 0, MEM_RELEASE);
#ifdef AH_DIAG
    ah_procs_log("FALLBACK: NtQSI(5) UAGame PID=%llu (st=0x%lX)",
                 (unsigned long long)uagame_pid, (unsigned long)st_p);
#endif
    if (!uagame_pid) return FALSE;

    // ---- Step 2.5: EPROCESS retry, match by PID.
    // First walk (line 655+) matches by ImageFileName. Anti-cheats (Vanguard
    // on Steam-ABInfinite in particular) sometimes zero / mangle ImageFileName
    // in the game's EPROCESS so a name-based scan misses it, but leave the
    // EPROCESS in ActiveProcessLinks with everything else intact. Since we now
    // know the real PID (from NtQSI(5) above), redo the walk matching pid_check
    // == uagame_pid instead. If the EPROCESS is still linked, we find it here
    // and skip the handle-table fallback entirely — which is important because
    // NtQSI(64) is one of the first calls Vanguard tears down at userland.
    {
        u64 psisp_cache = g_rpm_psisp;
        if (psisp_cache) {
            u64 head2 = psisp_cache + g_eproc_links;
            u64 cur2 = 0;
            if (RpmRead64(hDev, sysCR3, head2, &cur2)) {
                // Same forged-FLINK-cycle problem as the primary walker —
                // ACE loops the chain through decoy EPROCESS nodes so a
                // naive count-cap scan never reaches the real UAGame. Track
                // visited pointers, break on a repeat.
                const int SCAN2_CAP = 8192;
                u64* visited2 = (u64*)HeapAlloc(GetProcessHeap(), 0,
                                                SCAN2_CAP * sizeof(u64));
                int visited2_n = 0;
                int scan2 = 0;
                int cycle2 = 0;
                while (cur2 && cur2 != head2 && scan2 < SCAN2_CAP) {
                    u64 eproc2 = cur2 - g_eproc_links;
                    {
                        int seen2 = 0;
                        for (int vi = 0; vi < visited2_n; vi++) {
                            if (visited2[vi] == eproc2) { seen2 = 1; break; }
                        }
                        if (seen2) { cycle2 = 1; break; }
                        if (visited2_n < SCAN2_CAP) visited2[visited2_n++] = eproc2;
                    }
                    u64 pid2 = 0;
                    if (RpmRead64(hDev, sysCR3, eproc2 + g_eproc_pid, &pid2)
                        && pid2 == uagame_pid) {
                        u64 dtb2 = 0;
                        if (RpmRead64(hDev, sysCR3, eproc2 + g_eproc_dtb, &dtb2)
                            && dtb2) {
                            // Wrapper filter (same reasoning as primary walk):
                            // Steam ships arena_breakout.exe as a small bootstrap
                            // PE at ImageBase ~0x570000. NtQSI(5) substring match
                            // returns its PID for "arena_breakout" alt-name, and
                            // this walk would happily hand back its DTB. Reject
                            // if ImageBase < 4 GB — real UAGame is always at
                            // 0x140000000+.
                            u64 peb_p = 0, ib_p = 0;
                            RpmRead64(hDev, sysCR3, eproc2 + g_eproc_peb_off, &peb_p);
                            if (peb_p && (peb_p >> 48) == 0)
                                RpmRead64(hDev, dtb2, peb_p + 0x10, &ib_p);
                            if (ib_p && ib_p < 0x100000000ULL) {
#ifdef AH_DIAG
                                ah_procs_log("FALLBACK-C REJECT wrapper: pid=%llu ImageBase=0x%llX (too low)",
                                             (unsigned long long)uagame_pid,
                                             (unsigned long long)ib_p);
#endif
                            } else {
#ifdef AH_DIAG
                                ah_procs_log("FALLBACK-C: EPROCESS by PID hit pid=%llu eproc=0x%llX DTB=0x%llX ImgBase=0x%llX",
                                             (unsigned long long)uagame_pid,
                                             (unsigned long long)eproc2,
                                             (unsigned long long)dtb2,
                                             (unsigned long long)ib_p);
#endif
                                *procCR3 = dtb2;
                                if (eprocessOut) *eprocessOut = eproc2;
                                HeapFree(GetProcessHeap(), 0, visited2);
                                return TRUE;
                            }
                        }
                    }
                    u64 flink2 = 0;
                    if (!RpmRead64(hDev, sysCR3, cur2, &flink2) || flink2 == cur2) break;
                    cur2 = flink2;
                    scan2++;
                }
                HeapFree(GetProcessHeap(), 0, visited2);
#ifdef AH_DIAG
                ah_procs_log("FALLBACK-C: EPROCESS-by-PID scan exhausted pid=%llu scanned=%d cycle=%d",
                             (unsigned long long)uagame_pid, scan2, cycle2);
#endif
            }
        }
    }

    // ---- Step 3-4: enum handles, find one whose Object.UniqueProcessId == PID. ----
    typedef struct _SYS_HANDLE_EX {
        PVOID    Object;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR HandleValue;
        ULONG    GrantedAccess;
        USHORT   CreatorBackTraceIndex;
        USHORT   ObjectTypeIndex;
        ULONG    HandleAttributes;
        ULONG    Reserved;
    } SYS_HANDLE_EX;
    typedef struct _SYS_HANDLE_INFO_EX {
        ULONG_PTR NumberOfHandles;
        ULONG_PTR Reserved;
        SYS_HANDLE_EX Handles[1];
    } SYS_HANDLE_INFO_EX;

    ULONG need_h = 0;
    NTSTATUS st_probe = nqsi(64 /*SystemExtendedHandleInformation*/, NULL, 0, &need_h);
#ifdef AH_DIAG
    ah_procs_log("FALLBACK: NtQSI(64) size-probe st=0x%lX need_h=%lu",
                 (unsigned long)st_probe, (unsigned long)need_h);
#endif
    if (!need_h) return FALSE;
    need_h += 0x100000;   // handle table can grow between probes
    PVOID hbuf = VirtualAlloc(NULL, need_h, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!hbuf) return FALSE;
    NTSTATUS st_h = nqsi(64, hbuf, need_h, &need_h);
    BOOL ok = FALSE;
    if (st_h >= 0) {
        SYS_HANDLE_INFO_EX* hi = (SYS_HANDLE_INFO_EX*)hbuf;
#ifdef AH_DIAG
        ah_procs_log("FALLBACK: NtQSI(64) numHandles=%llu",
                     (unsigned long long)hi->NumberOfHandles);
#endif
        u64 last_object = 0;
        for (ULONG_PTR i = 0; i < hi->NumberOfHandles; i++) {
            SYS_HANDLE_EX* h = &hi->Handles[i];
            if (!h->Object) continue;
            if ((u64)h->Object == last_object) continue;   // handle-dupe skip
            last_object = (u64)h->Object;
            // Probe the Object as if it were an EPROCESS. Read its
            // UniqueProcessId at g_eproc_pid. If it equals our target,
            // read DTB from g_eproc_dtb — that IS the game's CR3.
            u64 pid_check = 0;
            if (!RpmRead64(hDev, sysCR3, (u64)h->Object + g_eproc_pid, &pid_check))
                continue;
            if (pid_check != uagame_pid) continue;
            u64 dtb = 0;
            if (!RpmRead64(hDev, sysCR3, (u64)h->Object + g_eproc_dtb, &dtb)
                || !dtb) continue;
            // Sanity — try MZ at PEB.ImageBase.
            u64 peb2 = 0;
            u64 image_base2 = 0;
            RpmRead64(hDev, sysCR3, (u64)h->Object + g_eproc_peb_off, &peb2);
            if (peb2 && (peb2 >> 48) == 0)
                RpmRead64(hDev, dtb, peb2 + 0x10, &image_base2);
#ifdef AH_DIAG
            ah_procs_log("FALLBACK: found EPROCESS=0x%llX PID=%llu DTB=0x%llX ImgBase=0x%llX",
                         (unsigned long long)h->Object,
                         (unsigned long long)pid_check,
                         (unsigned long long)dtb,
                         (unsigned long long)image_base2);
#endif
            *procCR3 = dtb;
            if (eprocessOut) *eprocessOut = (u64)h->Object;
            ok = TRUE;
            break;
        }
    }
    VirtualFree(hbuf, 0, MEM_RELEASE);
    return ok;
}

// ---- Userspace PEB.Ldr walking ----
//
// PEB layout (x64, stable across Win10/11):
//   +0x018  Ldr:  PPEB_LDR_DATA
//   +0x010  ImageBaseAddress: PVOID
//
// PEB_LDR_DATA layout (x64, stable):
//   +0x010  InLoadOrderModuleList: LIST_ENTRY
//
// LDR_DATA_TABLE_ENTRY layout (x64, stable Win10+):
//   +0x000  InLoadOrderLinks: LIST_ENTRY
//   +0x030  DllBase: PVOID
//   +0x040  SizeOfImage: ULONG
//   +0x058  BaseDllName: UNICODE_STRING { u16 Length, u16 MaxLength, u32 pad, PWSTR Buffer }

#define PEB_LDR_OFFSET              0x018
#define PEB_IMAGEBASE_OFFSET        0x010
#define PEB_LDR_INLOAD_OFFSET       0x010
#define LDR_ENTRY_DLLBASE           0x030
#define LDR_ENTRY_SIZEOFIMAGE       0x040
#define LDR_ENTRY_BASEDLLNAME       0x058

BOOL RpmGetMainImageBase(HANDLE hDev, u64 procCR3, u64 pebVA,
                         u64* baseOut, u64* sizeOut)
{
    if (baseOut) *baseOut = 0;
    if (sizeOut) *sizeOut = 0;
    if (!pebVA) return FALSE;

    u64 base = 0;
    if (!RpmRead64(hDev, procCR3, pebVA + PEB_IMAGEBASE_OFFSET, &base) || !base)
        return FALSE;
    if (baseOut) *baseOut = base;

    if (sizeOut) {
        // PE size from OptionalHeader.SizeOfImage — read IMAGE_NT_HEADERS64
        DWORD e_lfanew = 0;
        if (!RpmReadVirtual(hDev, procCR3, base + 0x3C, &e_lfanew, 4))
            return TRUE;
        u32 sizeOfImage = 0;
        if (!RpmReadVirtual(hDev, procCR3, base + e_lfanew + 0x50, &sizeOfImage, 4))
            return TRUE;
        *sizeOut = sizeOfImage;
    }
    return TRUE;
}

// ANSI-fold WCHAR for case-insensitive short-name compare (module names are ASCII).
static int wcs_ieq_ascii(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

BOOL RpmEnumModules(HANDLE hDev, u64 procCR3, u64 pebVA,
                    RpmModuleCb cb, void* ctx)
{
    if (!pebVA) return FALSE;

    u64 ldr = 0;
    if (!RpmRead64(hDev, procCR3, pebVA + PEB_LDR_OFFSET, &ldr) || !ldr)
        return FALSE;

    u64 head = ldr + PEB_LDR_INLOAD_OFFSET;
    u64 flink = 0;
    if (!RpmRead64(hDev, procCR3, head, &flink) || flink == head)
        return FALSE;

    u64 cur = flink;
    u32 count = 0;
    while (cur != head && count < 512) {
        u64 entry = cur;  // InLoadOrderLinks is at offset 0

        u64 dllBase = 0;
        u32 sizeOfImage = 0;
        u16 nameLen = 0;
        u64 nameBuf = 0;

        RpmRead64(hDev, procCR3, entry + LDR_ENTRY_DLLBASE, &dllBase);
        RpmReadVirtual(hDev, procCR3, entry + LDR_ENTRY_SIZEOFIMAGE, &sizeOfImage, 4);
        RpmReadVirtual(hDev, procCR3, entry + LDR_ENTRY_BASEDLLNAME, &nameLen, 2);
        RpmRead64(hDev, procCR3, entry + LDR_ENTRY_BASEDLLNAME + 8, &nameBuf);

        wchar_t name[128] = {0};
        u32 copyLen = nameLen;
        if (copyLen > sizeof(name) - 2) copyLen = sizeof(name) - 2;
        if (nameBuf && copyLen)
            RpmReadVirtual(hDev, procCR3, nameBuf, name, copyLen);

        if (dllBase) {
            if (!cb(name, dllBase, sizeOfImage, ctx))
                return TRUE;
        }

        u64 next = 0;
        if (!RpmRead64(hDev, procCR3, cur, &next) || next == cur)
            break;
        cur = next;
        count++;
    }
    return TRUE;
}

typedef struct {
    const wchar_t* target;
    u64 base;
    u64 size;
    BOOL found;
} FindModCtx;

static BOOL find_mod_cb(const wchar_t* name, u64 base, u64 size, void* ctxp) {
    FindModCtx* ctx = (FindModCtx*)ctxp;
    if (wcs_ieq_ascii(name, ctx->target)) {
        ctx->base = base;
        ctx->size = size;
        ctx->found = TRUE;
        return FALSE;
    }
    return TRUE;
}

BOOL RpmFindModule(HANDLE hDev, u64 procCR3, u64 pebVA,
                   const wchar_t* dllName, u64* baseOut, u64* sizeOut)
{
    if (baseOut) *baseOut = 0;
    if (sizeOut) *sizeOut = 0;

    FindModCtx ctx = { dllName, 0, 0, FALSE };
    RpmEnumModules(hDev, procCR3, pebVA, find_mod_cb, &ctx);
    if (!ctx.found) return FALSE;

    if (baseOut) *baseOut = ctx.base;
    if (sizeOut) *sizeOut = ctx.size;
    return TRUE;
}

// ---- UE4 UObject / FUObjectArray access ----

BOOL RpmGetUObjectByIndex(HANDLE hDev, u64 procCR3, u64 gObjectsVA,
                          i32 index, u64* outObj)
{
    *outObj = 0;
    if (index < 0) return FALSE;

    u64 chunksPtr = 0;
    if (!RpmRead64(hDev, procCR3, gObjectsVA + GOBJ_CHUNKS_PTR, &chunksPtr) || !chunksPtr)
        return FALSE;

    i32 chunkIdx = index / GOBJ_ELEMENTS_PER_CHUNK;
    i32 inChunk  = index % GOBJ_ELEMENTS_PER_CHUNK;

    u64 chunk = 0;
    if (!RpmRead64(hDev, procCR3, chunksPtr + (u64)chunkIdx * 8, &chunk) || !chunk)
        return FALSE;

    u64 itemVA = chunk + (u64)inChunk * GOBJ_ITEM_SIZE;
    u64 obj = 0;
    if (!RpmRead64(hDev, procCR3, itemVA, &obj))
        return FALSE;

    *outObj = obj;
    return TRUE;
}

// ---- FNamePool reader (Delta UE4.24 with UE5-style header + XOR 0xFF) ----
//
// Empirically-verified Delta FNamePool layout (from live dump on 25H2):
//   +0x00: 8 zero bytes (header/pad)
//   +0x08: uint8* Blocks[N]   — pointer array of ~1MB block pointers
//
// FNameEntry header (UE5-style, 16 bits little-endian):
//   bit  0    : bIsWide
//   bits 1..5 : LowercaseProbeHash (ignored)
//   bits 6..15: Length (10 bits, max 1023)
//
// FNameEntry layout:
//   +0x00: uint16 Header
//   +0x02: chars (ANSI 1B each, Length count) — XORed with 0xFF
//   Next entry is 2-byte aligned.
//
// FName encoding: ComparisonIndex = (BlockIdx << 18) | SlotIdx
//   Delta uses 14-bit block index, 18-bit slot index (verified by
//   cold-RE of FName::AppendString @ 0x150D47780).

#define FNAMEPOOL_BLOCKS_OFF   0x08
#define FNAMEPOOL_STRIDE       2      // FNameEntryHandle stride (each unit = 2 bytes)
#define FNAME_ENTRY_HEADER_SZ  2

// Delta's FName cipher — length-derived XOR key. Ported from DErDYAST1R
// binary dump (2025-12-17). Same key XORed against every byte of the name.
// key ends up in {0x7F, 0xFF} across all 9 branches.
static u8 DeltaFNameXorKey(u32 length)
{
    u32 r9d = (u32)length;
    // Fast div-by-9: (0x38E38E39 * r9d) >> 32 >> 1 == r9d / 9.
    u32 quot = (u32)(((u64)0x38E38E39ULL * (u64)r9d) >> 32) >> 1;
    u32 rem  = r9d - quot * 9;
    u8  k;
    switch (rem) {
        case 0: k = (u8)((r9d & 0x1F) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
        case 1: k = (u8)((r9d ^ 0xDF) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
        case 2: k = (u8)((r9d | 0xCF) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
        case 3: {
            int8_t s = (int8_t)r9d;
            k = (u8)((u8)(s * 0x21) + 0x80);
            return k | 0x7F;
        }
        case 4: k = (u8)(((u8)r9d >> 2) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
        case 5: {
            u8 base = (u8)(r9d - 0x29);
            k = (u8)(base + base + base);
            return k | 0x7F;
        }
        case 6: k = (u8)((((u8)r9d << 2) | 0x05) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
        case 7: k = (u8)((((u8)r9d >> 4) | 0x07) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
        case 8: k = (u8)(((u8)r9d ^ 0x0C) + 0x80); k = (u8)(k + r9d); return k | 0x7F;
    }
    return 0xFF;   // unreachable — rem is always in [0..8]
}

BOOL RpmResolveFName(HANDLE hDev, u64 procCR3, u64 gNamesVA,
                     u32 comparisonIndex, char* out, u32 outSize)
{
    if (!out || outSize == 0) return FALSE;
    out[0] = 0;
    if (comparisonIndex == 0) { strcpy_s(out, outSize, "None"); return TRUE; }

    // Delta: 14-bit block index, 18-bit slot index (NOT 16/16).
    u32 blockIdx = comparisonIndex >> 18;
    u32 offset   = (comparisonIndex & 0x3FFFF) * FNAMEPOOL_STRIDE;

    u64 blockPtr = 0;
    if (!RpmRead64(hDev, procCR3, gNamesVA + FNAMEPOOL_BLOCKS_OFF + blockIdx * 8, &blockPtr)
        || !blockPtr)
        return FALSE;

    u64 entryVA = blockPtr + offset;

    u16 header = 0;
    if (!RpmReadVirtual(hDev, procCR3, entryVA, &header, 2))
        return FALSE;

    u8  bIsWide = header & 1;
    u16 nameLen = header >> 6;   // UE5-style: bits 6..15 = Length (10 bits)

    if (nameLen == 0) { strcpy_s(out, outSize, ""); return TRUE; }
    if (nameLen >= outSize) nameLen = (u16)(outSize - 1);
    // Engine caps FName Length at 1023 (10-bit field). We cap at 128 —
    // covers every class / weapon / operator / map name in DFM. Bump
    // caller buffers to match if you expect longer.
    if (nameLen > 256) nameLen = 256;

    u8 xorKey = DeltaFNameXorKey((u32)nameLen);

    if (bIsWide) {
        wchar_t wbuf[512];
        if (nameLen > 511) nameLen = 511;
        if (!RpmReadVirtual(hDev, procCR3, entryVA + FNAME_ENTRY_HEADER_SZ,
                            wbuf, nameLen * 2))
            return FALSE;
        // Cold-RE @ 0x150D4B7D0: 'add eax, 2 ; xor [r8+rax*2], dx' — the
        // decrypt only XORs every OTHER wchar (indices 0, 2, 4, ...).
        // Value is a 16-bit widened xorKey (same key both bytes).
        u16 wideKey = (u16)((u16)xorKey | ((u16)xorKey << 8));
        for (u16 i = 0; i < nameLen; i += 2) wbuf[i] ^= wideKey;
        wbuf[nameLen] = 0;
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, outSize, NULL, NULL);
    } else {
        if (!RpmReadVirtual(hDev, procCR3, entryVA + FNAME_ENTRY_HEADER_SZ,
                            out, nameLen))
            return FALSE;
        u32 nBytes = nameLen;
        if (nBytes > 0x400) nBytes = 0x400;
        for (u32 i = 0; i < nBytes; i++) out[i] ^= xorKey;
        out[nameLen] = 0;
    }
    return TRUE;
}

BOOL RpmGetObjectName(HANDLE hDev, u64 procCR3, u64 gNamesVA,
                      u64 obj, char* out, u32 outSize)
{
    if (!obj || !out || !outSize) return FALSE;
    out[0] = 0;

    u32 fname[2] = {0};
    if (!RpmReadVirtual(hDev, procCR3, obj + UOBJ_NAME, fname, 8))
        return FALSE;

    return RpmResolveFName(hDev, procCR3, gNamesVA, fname[0], out, outSize);
}

BOOL RpmGetObjectClassName(HANDLE hDev, u64 procCR3, u64 gNamesVA,
                           u64 obj, char* out, u32 outSize)
{
    if (!obj || !out || !outSize) return FALSE;
    out[0] = 0;

    u64 classObj = 0;
    if (!RpmRead64(hDev, procCR3, obj + UOBJ_CLASS, &classObj) || !classObj)
        return FALSE;

    return RpmGetObjectName(hDev, procCR3, gNamesVA, classObj, out, outSize);
}

// ---- FEncVector decoder ----

// ---- GObjects class-name lookup + SuperStruct-chain filter ----
//
// The pickup class filter uses these two primitives:
//   1. RpmFindUClassByName — lazy-init locator for "PickupBase" UClass*.
//   2. RpmIsClassDescendantOf — per-actor ancestry check against the base.
// Together they form a structural gate that admits ONLY actors whose class
// inherits from APickupBase (native subclasses + BP InventoryPickup_C).
// This replaces the field-shape heuristic which admitted AActor descendants
// whose 0xFC0..0x1020 band happened to hold pickup-like flag/int/float
// patterns (the density-cluster FP case).

u64 RpmFindUClassByName(HANDLE hDev, u64 procCR3, u64 gObjectsVA,
                       u64 gNamesVA, const char* targetName)
{
    if (!gObjectsVA || !gNamesVA || !targetName || !targetName[0]) return 0;

    // Read FUObjectArray header: Chunks@+0x10, NumElements@+0x04.
    // (Delta uses +0x04 for live element count — verified by walk-gobjects
    // resolving 479k here; the old +0x24 offset was wrong and aborted us.)
    u8 hdr[0x30] = {0};
    if (!RpmReadVirtual(hDev, procCR3, gObjectsVA, hdr, sizeof(hdr))) return 0;
    u64 chunksPtr = *(u64*)(hdr + GOBJ_CHUNKS_PTR);
    i32 numElem   = *(i32*)(hdr + 0x04);
    if (!chunksPtr || numElem <= 0 || numElem > 0x400000) return 0;

    i32 numChunks = (numElem + (i32)GOBJ_ELEMENTS_PER_CHUNK - 1) /
                    (i32)GOBJ_ELEMENTS_PER_CHUNK;
    if (numChunks <= 0 || numChunks > 128) numChunks = numChunks > 128 ? 128 : 0;
    if (!numChunks) return 0;

    u64 chunkPtrs[128] = {0};
    if (!RpmReadVirtual(hDev, procCR3, chunksPtr, chunkPtrs,
                        (u32)(numChunks * (i32)sizeof(u64)))) return 0;

    // 1.5 MB static scratch — one full chunk of FUObjectItems at a time.
    static u8 s_items[GOBJ_ELEMENTS_PER_CHUNK * GOBJ_ITEM_SIZE];
    u64 found = 0;
    for (i32 ci = 0; ci < numChunks && !found; ci++) {
        u64 chunk = chunkPtrs[ci];
        if (!chunk) continue;
        i32 lo = ci * (i32)GOBJ_ELEMENTS_PER_CHUNK;
        i32 hi = lo + (i32)GOBJ_ELEMENTS_PER_CHUNK;
        if (hi > numElem) hi = numElem;
        u32 nItems = (u32)(hi - lo);
        if (!RpmReadVirtual(hDev, procCR3, chunk, s_items,
                            nItems * (u32)GOBJ_ITEM_SIZE)) continue;
        for (u32 ii = 0; ii < nItems && !found; ii++) {
            u64 obj = *(u64*)(s_items + ii * GOBJ_ITEM_SIZE);
            if (!obj) continue;
            u32 nameIdx = 0;
            if (!RpmReadVirtual(hDev, procCR3, obj + UOBJ_NAME, &nameIdx, 4))
                continue;
            if (!nameIdx) continue;
            char nameBuf[64] = {0};
            if (!RpmResolveFName(hDev, procCR3, gNamesVA, nameIdx,
                                 nameBuf, sizeof(nameBuf))) continue;
            if (nameBuf[0] && strcmp(nameBuf, targetName) == 0)
                found = obj;
        }
    }
    return found;
}

BOOL RpmIsClassDescendantOf(HANDLE hDev, u64 procCR3,
                            u64 classPtr, u64 ancestorClass)
{
    if (!classPtr || !ancestorClass) return FALSE;
    u64 cur = classPtr;
    for (int hop = 0; hop < 8; hop++) {
        if (cur == ancestorClass) return TRUE;
        u64 sup = 0;
        if (!RpmRead64(hDev, procCR3, cur + USTRUCT_SUPER, &sup) || !sup)
            return FALSE;
        cur = sup;
    }
    return FALSE;
}

BOOL RpmReadEncVector(HANDLE hDev, u64 procCR3, u64 vecVA,
                      float* xOut, float* yOut, float* zOut,
                      u8* bEncryptedOut)
{
    u8 buf[16] = {0};
    if (!RpmReadVirtual(hDev, procCR3, vecVA, buf, 16)) return FALSE;

    float x = *(float*)(buf + 0);
    float y = *(float*)(buf + 4);
    float z = *(float*)(buf + 8);
    // FEncHandler @ +0x0C: u16 Index, i8 bEncrypted, u8 flags
    u8 bEnc = buf[14];

    if (bEncryptedOut) *bEncryptedOut = bEnc;

    if (bEnc != 0) {
        // TODO: real decrypt algo — for now just return raw floats.
        // The algorithm lives in UKismetMathLibrary::DecVector.
        // Live RE will fill this in; for now the caller inspects bEncrypted.
    }

    if (xOut) *xOut = x;
    if (yOut) *yOut = y;
    if (zOut) *zOut = z;
    return TRUE;
}

// ---------------------------------------------------------------------------
// RpmHideOwnProcess — unlink our own EPROCESS from ActiveProcessLinks so
// Task Manager, tasklist.exe, Process Hacker and every tool walking
// SystemProcessInformation cannot see us.
//
// Requires:
//   - EprocInit() has been called (g_eproc_* populated)
//   - RpmFindProcess() has run at least once (g_rpm_psisp cached)
//   - Kernel virtual-write path works (RpmWriteVirtual → PhysWrite via kdu)
//
// Mechanics:
//   LIST_ENTRY layout: { Flink, Blink }
//   our.Flink → &next.LIST_ENTRY
//   our.Blink → &prev.LIST_ENTRY
//   To unlink:
//     write our.Flink at address our.Blink  → prev.Flink = our.Flink
//     write our.Blink at address our.Flink+8 → next.Blink = our.Blink
//   Post-unlink: our.Flink/Blink point to self so any late deref stays safe.
// ---------------------------------------------------------------------------

BOOL RpmHideOwnProcess(HANDLE hDev, u64 sysCR3)
{
    // NOTE: VMProtect markers not applied here — multiple return paths make
    // single-basic-block requirement impossible without heavy refactor.
    // Function is disabled anyway (see main.c) so lower priority.
    if (!g_rpm_psisp || !g_eproc_links) {
        DH_ERROR("HideOwnProcess: psisp/eproc not initialized");
        return FALSE;
    }

    u64 our_pid = (u64)GetCurrentProcessId();
    u64 head = g_rpm_psisp + g_eproc_links;
    u64 cur = 0;
    if (!RpmRead64(hDev, sysCR3, head, &cur)) {
        DH_ERROR("HideOwnProcess: read first Flink failed");
        return FALSE;
    }

    u64 our_eproc = 0;
    int scanned = 0;
    while (cur != head && scanned < 4096) {
        u64 eproc = cur - g_eproc_links;
        u64 pid = 0;
        if (RpmRead64(hDev, sysCR3, eproc + g_eproc_pid, &pid) && pid == our_pid) {
            our_eproc = eproc;
            break;
        }
        u64 flink = 0;
        if (!RpmRead64(hDev, sysCR3, cur, &flink) || flink == cur) break;
        cur = flink;
        scanned++;
    }
    if (!our_eproc) {
        DH_ERROR("HideOwnProcess: own PID=%llu not found (%d scanned)",
                 our_pid, scanned);
        return FALSE;
    }

    u64 our_links = our_eproc + g_eproc_links;
    u64 our_flink = 0, our_blink = 0;
    if (!RpmRead64(hDev, sysCR3, our_links, &our_flink) ||
        !RpmRead64(hDev, sysCR3, our_links + 8, &our_blink)) {
        DH_ERROR("HideOwnProcess: read own Flink/Blink failed");
        return FALSE;
    }
    DH_INFO("HideOwnProcess: PID=%llu eproc=0x%llX Flink=0x%llX Blink=0x%llX",
            our_pid, our_eproc, our_flink, our_blink);

    // Write prev.Flink = our.Flink   (address = our.Blink)
    if (!RpmWrite64(hDev, sysCR3, our_blink, our_flink)) {
        DH_ERROR("HideOwnProcess: write prev.Flink failed — kernel write "
                 "probably not supported by provider");
        return FALSE;
    }
    // Write next.Blink = our.Blink   (address = our.Flink + 8)
    if (!RpmWrite64(hDev, sysCR3, our_flink + 8, our_blink)) {
        DH_ERROR("HideOwnProcess: write next.Blink failed");
        // Try to relink prev so we don't leave list corrupted.
        RpmWrite64(hDev, sysCR3, our_blink, our_links);
        return FALSE;
    }
    // Point our own Flink/Blink at ourselves — safe if kernel dereferences
    // during our process teardown.
    RpmWrite64(hDev, sysCR3, our_links, our_links);
    RpmWrite64(hDev, sysCR3, our_links + 8, our_links);

    DH_INFO("HideOwnProcess: unlinked from ActiveProcessLinks — invisible to "
            "Task Manager / tasklist / Process Hacker");
    return TRUE;
}

// ---------------------------------------------------------------------------
// RpmHideAllByImageName — walk ActiveProcessLinks, find every EPROCESS
// whose ImageFileName starts with `nameA` (case-insensitive), and unlink
// each one from the list. Returns number of processes newly hidden.
//
// Design: daemon calls this periodically. Own EPROCESS is already unlinked
// (from RpmHideOwnProcess) so it doesn't re-appear in the walk. Overlay
// (same exe name, different PID) gets hidden the first time daemon spots it.
// Late-arriving instances also get caught on the next call.
// ---------------------------------------------------------------------------

int RpmHideAllByImageName(HANDLE hDev, u64 sysCR3, const char* nameA)
{
    if (!g_rpm_psisp || !g_eproc_links || !nameA || !nameA[0]) return 0;

    size_t nameLen = strlen(nameA);
    if (nameLen > 14) nameLen = 14;   // ImageFileName is 15 chars incl. NUL

    u64 head = g_rpm_psisp + g_eproc_links;
    u64 cur = 0;
    if (!RpmRead64(hDev, sysCR3, head, &cur)) return 0;

    int hidden = 0;
    int scanned = 0;
    while (cur != head && scanned < 4096) {
        u64 eproc = cur - g_eproc_links;
        u64 next_flink = 0;
        // Cache next link BEFORE we unlink current — the write invalidates cur.
        if (!RpmRead64(hDev, sysCR3, cur, &next_flink) || next_flink == cur) break;

        char imgName[16] = {0};
        if (!RpmReadVirtual(hDev, sysCR3, eproc + g_eproc_imgname, imgName, 15)) {
            cur = next_flink; scanned++; continue;
        }
        imgName[15] = 0;

        if (_strnicmp(imgName, nameA, nameLen) == 0) {
            u64 our_links = eproc + g_eproc_links;
            u64 our_flink = 0, our_blink = 0;
            if (RpmRead64(hDev, sysCR3, our_links, &our_flink) &&
                RpmRead64(hDev, sysCR3, our_links + 8, &our_blink)) {
                u64 pid = 0;
                RpmRead64(hDev, sysCR3, eproc + g_eproc_pid, &pid);
                // Unlink: prev.Flink = our.Flink, next.Blink = our.Blink
                if (RpmWrite64(hDev, sysCR3, our_blink, our_flink) &&
                    RpmWrite64(hDev, sysCR3, our_flink + 8, our_blink)) {
                    // Self-loop own links for safe teardown.
                    RpmWrite64(hDev, sysCR3, our_links, our_links);
                    RpmWrite64(hDev, sysCR3, our_links + 8, our_links);
                    DH_INFO("HideAllByName: hid PID=%llu name=\"%s\"",
                            pid, imgName);
                    hidden++;
                }
            }
        }
        cur = next_flink;
        scanned++;
    }
    return hidden;
}
