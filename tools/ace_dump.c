// arenahack — ACE decrypt chain static-dump utility.
//
// Purpose: snapshot UAGame.exe memory around the new ACE encryption wrapper
// (reversed by hand, entry @ 0x228CE5E40 in field-captured disasm) to disk
// so we can RE the whole call chain + rip-rel data table layout offline.
//
// Approach: use the overlay's own kdu RPM primitives to read UAGame memory.
// Writes:
//   ace_dump/function_body.bin      — 4 KiB window around 0x228CE5E40
//   ace_dump/data_tables.bin        — concatenated windows around every
//                                     rip-rel ref seen in the entry function
//   ace_dump/manifest.txt           — human-readable (region, size, source VA)
//
// Build: adds to tools/ in build.bat as a separate console exe. Static-linked
// against dh_rpm.c + provider setup so we can run it before the overlay is
// running (no shmem contention).

#include <windows.h>
#include <stdio.h>
#include "../inc/dh_common.h"
#include "../inc/dh_rpm.h"
#include "../inc/dh_provider.h"

extern const DH_PROVIDER* DhProviderSelect(HANDLE* devOut, u32* flagsOut);

// ---- VAs captured from Maik's hand-RE (addresses are UAGame's VMP-decoded
// region ≥ 0x228000000 — not standard .text, likely a VMProtect section or
// Hyperion-mapped page). The decrypt entry is at 0x228CE5E40.
#define ACE_FN_ENTRY     0x228CE5E40ULL
#define ACE_FN_BODY_SIZE 0x1000              // 4 KiB window — covers full fn + a few callees

// Known data refs (every lea/mov with rip-rel disp32 observed in the paste):
static const u64 DATA_REFS[] = {
    0x228D56550ULL,   // qword-table[ecx*8] used for info-type dispatch
    0x228D41CF8ULL,   // state ptr (rdi = *[this])
    0x228D41BE1ULL,   // init flag byte
    0x228D55C50ULL,   // dispatch base — [228D55C50] added to magic, then jmp r10
    0x228D55CE8ULL,   // used in next-stage dispatch (r8 = *[this])
    0x228D55CA8ULL,   // additional dispatch const
    0x228D55D10ULL,   // another dispatch const
    0x228D55D48ULL,   // another dispatch const
};
#define DATA_WINDOW_PRE   0x40
#define DATA_WINDOW_POST  0x200             // 512B past each ref — tables usually bounded

static void mkdir_p(const char* path) { CreateDirectoryA(path, NULL); }

int main(int argc, char** argv) {
    HANDLE dev = NULL; u32 flags = 0;
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (!p || !dev) {
        fprintf(stderr, "DhProviderSelect FAIL\n");
        return 1;
    }
    fprintf(stdout, "provider: kdu#%u %s\n", p->kdu_id, p->name);

    u64 sysCR3 = 0;
    if (!RpmFindSystemCR3(dev, &sysCR3)) {
        fprintf(stderr, "RpmFindSystemCR3 FAIL\n");
        return 2;
    }
    fprintf(stdout, "sysCR3: 0x%llX\n", (unsigned long long)sysCR3);

    u64 procCR3 = 0, eproc = 0;
    if (!RpmFindProcess(dev, sysCR3, "UAGame.exe", &procCR3, &eproc)) {
        fprintf(stderr, "RpmFindProcess UAGame.exe FAIL — launch game first\n");
        return 3;
    }
    fprintf(stdout, "UAGame.exe procCR3: 0x%llX eproc: 0x%llX\n",
            (unsigned long long)procCR3, (unsigned long long)eproc);

    mkdir_p("ace_dump");

    // --- fn body
    void* fn_buf = HeapAlloc(GetProcessHeap(), 0, ACE_FN_BODY_SIZE);
    if (!fn_buf) return 4;
    BOOL ok_fn = RpmReadVirtual(dev, procCR3, ACE_FN_ENTRY, fn_buf, ACE_FN_BODY_SIZE);
    FILE* f = fopen("ace_dump/function_body.bin", "wb");
    if (f) { fwrite(fn_buf, 1, ACE_FN_BODY_SIZE, f); fclose(f); }
    fprintf(stdout, "fn_body: 0x%llX+%u → ace_dump/function_body.bin (read=%d)\n",
            (unsigned long long)ACE_FN_ENTRY, (unsigned)ACE_FN_BODY_SIZE, (int)ok_fn);

    // --- data refs
    FILE* mf = fopen("ace_dump/manifest.txt", "w");
    FILE* df = fopen("ace_dump/data_tables.bin", "wb");
    u64 total_data = 0;
    for (size_t i = 0; i < sizeof(DATA_REFS)/sizeof(DATA_REFS[0]); i++) {
        u64 va = DATA_REFS[i];
        u64 start = va - DATA_WINDOW_PRE;
        u32 size  = DATA_WINDOW_PRE + DATA_WINDOW_POST;
        u8 buf[0x240] = {0};
        BOOL ok = RpmReadVirtual(dev, procCR3, start, buf, size);
        if (df) fwrite(buf, 1, size, df);
        if (mf) fprintf(mf,
            "ref[%zu] = 0x%llX  dumped=[0x%llX..0x%llX]  size=%u  rd=%d\n",
            i, (unsigned long long)va,
            (unsigned long long)start, (unsigned long long)(start + size),
            size, (int)ok);
        total_data += size;
    }
    if (df) fclose(df);
    if (mf) {
        fprintf(mf, "\ntotal data bytes: %llu\n", (unsigned long long)total_data);
        fprintf(mf, "function body:    0x%llX + 0x%X\n",
                (unsigned long long)ACE_FN_ENTRY, (unsigned)ACE_FN_BODY_SIZE);
        fclose(mf);
    }
    fprintf(stdout, "data_tables: %llu bytes → ace_dump/data_tables.bin\n",
            (unsigned long long)total_data);

    HeapFree(GetProcessHeap(), 0, fn_buf);
    CloseHandle(dev);
    return 0;
}
