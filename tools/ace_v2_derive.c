// arenahack — ACE v2 key-derivation PoC.
//
// Reproduces the one-shot avalanche mix at UAGame 0x228CE5E40 before the
// real decrypt body @ 0x228CF3BC0. If state_ptr reads correctly and the
// derived key is non-trivially shaped, we have a path to port the full
// chain next.
//
// Build (standalone, links against overlay's RPM primitives):
//   cl /nologo /W3 /O2 /GS- /MD /DUNICODE /D_UNICODE /DAH_DIAG /Iinc \
//      tools/ace_v2_derive.c src/log.c src/ah_stubs.c \
//      src/db/dh_dbunpack.c src/svc/dh_scm.c \
//      src/winio/dh_phys.c src/winio/dh_prov_registry.c \
//      src/winio/dh_prov_impl.c src/mem/dh_rpm.c \
//      src/mem/ah_uspace_read.c \
//      /Fe:build/ace_v2_derive.exe /link /SUBSYSTEM:CONSOLE \
//      Advapi32.lib User32.lib Shlwapi.lib

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "../inc/dh_common.h"
#include "../inc/dh_rpm.h"
#include "../inc/dh_provider.h"

// ---- Constants extracted from disassembly @ 0x228CE5EA4..0x228CE5EE8
#define ACE_V2_K1      0x72C31EEB5421C33AULL
#define ACE_V2_K2      0x7E5302E2AFD48A54ULL
#define ACE_V2_K3      0xBB486BC0UL
#define ACE_V2_K4      0xE1F4D56F2491335EULL
#define ACE_V2_K5      0xF3AD984F99E14D48ULL

// ---- RVA offsets within UAGame image. Base in field is NOT 0x140000000 —
// the disassembly addresses are inside a 0x228000000+ segment (likely VMP
// section or Hyperion-mapped page). Treat these as absolute VA and read
// via procCR3 translation.
#define ACE_V2_STATE_PTR_VA   0x228D41CF8ULL
#define ACE_V2_INIT_FLAG_VA   0x228D41BE1ULL
#define ACE_V2_DISPATCH_VA    0x228D56550ULL
#define ACE_V2_FN_ENTRY_VA    0x228CE5E40ULL

static inline uint64_t ror64(uint64_t x, int n) { n &= 63; return (x >> n) | (x << (64 - n)); }
static inline uint64_t rol64(uint64_t x, int n) { n &= 63; return (x << n) | (x >> (64 - n)); }

// Direct C port of the derivation. Takes the state pointer VALUE (what sits
// at [0x228D41CF8]) and produces the derived key.
static uint64_t ace_v2_derive_key(uint64_t state_ptr) {
    uint64_t a = state_ptr;
    a ^= ACE_V2_K1;                               // xor rdi, K1
    a = ror64(a, 1);                              // ror rdi, 1
    uint64_t b = ACE_V2_K2 + a;                   // add r10, rdi
    uint32_t c32 = ACE_V2_K3 ^ (uint32_t)b;       // xor r11d, r10d (32-bit)
    uint64_t c = (uint64_t)c32;                   // zero-extend to 64
    uint64_t d = ACE_V2_K4 + c;                   // add r10, r11
    d = rol64(d, 0x39);                           // rol r10, 0x39
    uint64_t e = ACE_V2_K5 ^ d;                   // xor rdi, r10
    return e;
}

extern const DH_PROVIDER* DhProviderSelect(HANDLE* devOut, u32* flagsOut);

int main(void) {
    printf("[+] arenahack ACE v2 key-derivation PoC\n\n");

    // 1. Static self-test with a dummy state_ptr so build sanity is clean.
    uint64_t test_in  = 0x00007FF6C0401000ULL;    // arbitrary UE4-ish ptr
    uint64_t test_out = ace_v2_derive_key(test_in);
    printf("    static test  in=0x%016llX  out=0x%016llX\n\n",
           (unsigned long long)test_in, (unsigned long long)test_out);

    // 2. Attach to UAGame and read live state_ptr.
    HANDLE dev = NULL; u32 flags = 0;
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (!p || !dev) { fprintf(stderr, "[-] DhProviderSelect FAIL\n"); return 1; }
    printf("[+] provider: kdu#%u %s\n", p->kdu_id, p->name);

    u64 sysCR3 = 0;
    if (!RpmFindSystemCR3(dev, &sysCR3)) { fprintf(stderr, "[-] sysCR3 fail\n"); return 2; }
    printf("[+] sysCR3: 0x%llX\n", (unsigned long long)sysCR3);

    u64 procCR3 = 0, eproc = 0;
    if (!RpmFindProcess(dev, sysCR3, "UAGame.exe", &procCR3, &eproc)) {
        fprintf(stderr, "[-] UAGame.exe not running\n"); return 3;
    }
    printf("[+] UAGame procCR3: 0x%llX eproc: 0x%llX\n\n",
           (unsigned long long)procCR3, (unsigned long long)eproc);

    // 3. Dump 32 bytes around state ptr + init flag + dispatch table head.
    uint8_t init_flag = 0;
    uint64_t state_ptr = 0;
    uint64_t disp_head[8] = {0};
    uint8_t fn_prologue[16] = {0};

    BOOL ok_init = RpmReadVirtual(dev, procCR3, ACE_V2_INIT_FLAG_VA, &init_flag, 1);
    BOOL ok_sp   = RpmRead64     (dev, procCR3, ACE_V2_STATE_PTR_VA, &state_ptr);
    BOOL ok_dh   = RpmReadVirtual(dev, procCR3, ACE_V2_DISPATCH_VA,  disp_head, sizeof(disp_head));
    BOOL ok_fn   = RpmReadVirtual(dev, procCR3, ACE_V2_FN_ENTRY_VA,  fn_prologue, sizeof(fn_prologue));

    printf("[probe] init_flag @ 0x%llX  read=%d  val=0x%02X  (expect 0x01)\n",
           (unsigned long long)ACE_V2_INIT_FLAG_VA, (int)ok_init, init_flag);
    printf("[probe] state_ptr @ 0x%llX  read=%d  val=0x%016llX\n",
           (unsigned long long)ACE_V2_STATE_PTR_VA, (int)ok_sp,
           (unsigned long long)state_ptr);
    printf("[probe] fn prologue @ 0x%llX  read=%d  bytes=", (unsigned long long)ACE_V2_FN_ENTRY_VA, (int)ok_fn);
    for (int i = 0; i < 16; i++) printf("%02X ", fn_prologue[i]);
    printf("\n           expect: 41 56 56 57 53 48 83 EC 28 81 F9 E9 00 00 00 0F\n");

    printf("[probe] dispatch head @ 0x%llX  read=%d\n",
           (unsigned long long)ACE_V2_DISPATCH_VA, (int)ok_dh);
    for (int i = 0; i < 8; i++)
        printf("           disp[%d] = 0x%016llX\n", i, (unsigned long long)disp_head[i]);
    printf("\n");

    // 4. If state_ptr looks like a sane user pointer, run derivation.
    if (ok_sp && state_ptr != 0 && state_ptr < 0x00008000000000ULL) {
        uint64_t key = ace_v2_derive_key(state_ptr);
        printf("[+] LIVE DERIVATION\n");
        printf("    state_ptr : 0x%016llX\n", (unsigned long long)state_ptr);
        printf("    derived   : 0x%016llX\n", (unsigned long long)key);
        printf("    stage 1 (xor K1)         = 0x%016llX\n", (unsigned long long)(state_ptr ^ ACE_V2_K1));
        printf("    stage 2 (ror 1)          = 0x%016llX\n", (unsigned long long)ror64(state_ptr ^ ACE_V2_K1, 1));
        uint64_t s2 = ror64(state_ptr ^ ACE_V2_K1, 1);
        printf("    stage 3 (K2 + s2)        = 0x%016llX\n", (unsigned long long)(ACE_V2_K2 + s2));
        uint64_t s3 = ACE_V2_K2 + s2;
        printf("    stage 4 (K3 ^ s3_lo)     = 0x%016llX\n", (unsigned long long)(uint64_t)(uint32_t)(ACE_V2_K3 ^ (uint32_t)s3));
    } else {
        printf("[!] state_ptr not sane — ACE encrypt region may not be mapped\n");
        printf("    OR UAGame hasn't initialized ACE yet. Launch into a raid and retry.\n");
    }

    CloseHandle(dev);
    return 0;
}
