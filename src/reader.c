// arenahack reader MVP — enumerate enemies from GameState.PlayerArray,
// decrypt positions via ACE, print name/HP/xyz to stdout each tick.
//
// Fresh C from scratch. References C:\ABIFINAL\src\reader.cpp for offsets
// + walk pattern only. Loop rate 5 Hz for MVP (readable output). Runs
// forever, Ctrl+C to stop.
//
// Pipeline:
//   driver_up (BYOVD via kdu inpoutx64) → sysCR3 → RpmFindProcess("UAGame.exe")
//   → PEB → ImageBase → GWorld → UGameState → PlayerArray[]
//   → for each PS: pawn = PS+0x378, root = pawn+0x170,
//                  pos = AhAceDecrypt(root), hp = pawn+0x1C5C,
//                  name = FString @ PS+0x3F8

#include "../inc/dh_common.h"
#include "../inc/dh_scm.h"
#include "../inc/dh_provider.h"
#include "../inc/dh_rpm.h"
#include "../inc/ah_offsets.h"
#include "../inc/ah_ace.h"

#include <windows.h>
#include <stdio.h>

extern const DH_PROVIDER* g_active_provider;

static wchar_t g_svcName[64];
static const wchar_t* svc_name(void) {
    if (!g_svcName[0]) _snwprintf(g_svcName, 64, L"ah_reader_%lu", GetCurrentProcessId());
    return g_svcName;
}

static BOOL is_elevated(void) {
    BOOL r = FALSE; HANDLE t = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        TOKEN_ELEVATION e = {0}; DWORD n = 0;
        if (GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n))
            r = e.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(t);
    }
    return r;
}

static BOOL driver_up(DH_DRIVER* drv) {
    ZeroMemory(drv, sizeof(*drv));
    HANDLE dev = NULL; u32 flags = 0;
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (!p || !dev) return FALSE;
    drv->hDevice = dev;
    _snwprintf(drv->devPath, 128, L"\\\\.\\%s", p->dev_name);
    g_active_provider = p;
    DH_INFO("active provider: kdu#%u %s", p->kdu_id, p->name);
    return TRUE;
}

static void driver_down(DH_DRIVER* drv) {
    if (drv->hDevice && !drv->hSvc) { CloseHandle(drv->hDevice); return; }
    DhDrvStop(drv); DhDrvUninstall(drv); DhDrvCleanup(drv);
}

// Read FString {TCHAR* Data; int32 Count; int32 Max} → UTF-8 ASCII truncate.
// FString layout for UE4.24: {data_ptr(8), count(4), max(4)} = 16 bytes.
static void read_fstring(HANDLE hDev, u64 procCR3, u64 fstrVA,
                         char* out, size_t outSz) {
    if (outSz == 0) return;
    out[0] = 0;
    struct { u64 data; i32 num; i32 max; } fs = {0};
    if (!RpmReadVirtual(hDev, procCR3, fstrVA, &fs, sizeof(fs))) return;
    if (!fs.data || fs.num <= 0 || fs.num > 256) return;
    wchar_t wbuf[128] = {0};
    u32 rd = (u32)fs.num;
    if (rd > 127) rd = 127;
    if (!RpmReadVirtual(hDev, procCR3, fs.data, wbuf, rd * 2)) return;
    for (u32 i = 0; i < rd && i + 1 < outSz; i++) {
        wchar_t c = wbuf[i];
        out[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    out[rd < outSz - 1 ? rd : outSz - 1] = 0;
}

typedef struct {
    u64 gworld;
    u64 world;
    u64 gamestate;
    u64 gameinst;
} AH_WORLD_CTX;

static BOOL resolve_world(HANDLE hDev, u64 procCR3, u64 imageBase, AH_WORLD_CTX* ctx) {
    if (!RpmRead64(hDev, procCR3, imageBase + AH_RVA_GWORLD, &ctx->gworld) ||
        ctx->gworld < 0x10000ULL) return FALSE;
    ctx->world = ctx->gworld;
    RpmRead64(hDev, procCR3, ctx->world + AH_UW_GAMESTATE, &ctx->gamestate);
    RpmRead64(hDev, procCR3, ctx->world + AH_UW_GAMEINSTANCE, &ctx->gameinst);
    return ctx->gamestate != 0;
}

typedef struct {
    u64  ps;
    u64  pawn;
    u64  root;
    float x, y, z;
    float hp;
    u32  team;
    char name[64];
    BOOL pos_ok;
} AH_ENTITY;

#define MAX_ENT 64

static u32 walk_players(HANDLE hDev, u64 procCR3, u64 imageBase,
                        u64 gamestate, AH_ENTITY* out, u32 maxN)
{
    struct { u64 data; i32 num; i32 max; } arr = {0};
    if (!RpmReadVirtual(hDev, procCR3, gamestate + AH_GS_PLAYERARRAY, &arr, sizeof(arr))) return 0;
    if (arr.num <= 0 || arr.num > 64 || !arr.data) return 0;
    u32 n = 0;
    for (i32 i = 0; i < arr.num && n < maxN; i++) {
        u64 ps = 0;
        if (!RpmRead64(hDev, procCR3, arr.data + (u64)i * 8, &ps) || !ps) continue;
        u64 pawn = 0;
        RpmRead64(hDev, procCR3, ps + AH_PS_PAWN, &pawn);
        if (!pawn) continue;
        u64 root = 0;
        RpmRead64(hDev, procCR3, pawn + AH_PAWN_ROOT, &root);

        AH_ENTITY* e = &out[n++];
        memset(e, 0, sizeof(*e));
        e->ps = ps; e->pawn = pawn; e->root = root;

        if (root) {
            e->pos_ok = AhAceDecrypt(hDev, procCR3, imageBase,
                                     root, &e->x, &e->y, &e->z);
        }
        // Direct HP float on SGCharacter (raw, ignores 7-limb aset).
        RpmReadVirtual(hDev, procCR3, pawn + AH_PAWN_HP_DIRECT, &e->hp, 4);
        RpmReadVirtual(hDev, procCR3, ps + AH_PS_TEAMINDEX, &e->team, 4);
        read_fstring(hDev, procCR3, ps + AH_PS_PLAYERNAME, e->name, sizeof(e->name));
    }
    return n;
}

int wmain(int argc, wchar_t** argv) {
    (void)argc; (void)argv;
    printf("arenahack reader MVP — enumerate PlayerArray, ACE decrypt, print xyz\n\n");

    if (!is_elevated()) { DH_ERROR("not elevated"); return 3; }

    DH_DRIVER drv;
    if (!driver_up(&drv)) { DH_ERROR("driver_up failed"); return 10; }

    u64 sysCR3 = 0;
    if (!RpmFindSystemCR3(drv.hDevice, &sysCR3)) {
        driver_down(&drv); DH_ERROR("sysCR3 failed"); return 11;
    }
    u64 procCR3 = 0, eproc = 0;
    if (!RpmFindProcess(drv.hDevice, sysCR3, AH_PROC_NAME, &procCR3, &eproc)) {
        driver_down(&drv); DH_ERROR("UAGame not found"); return 12;
    }
    u64 peb = 0;
    if (!RpmRead64(drv.hDevice, sysCR3, eproc + g_eproc_peb_off, &peb) || !peb) {
        driver_down(&drv); DH_ERROR("read PEB failed"); return 13;
    }
    u64 imageBase = 0, imageSize = 0;
    if (!RpmGetMainImageBase(drv.hDevice, procCR3, peb, &imageBase, &imageSize)) {
        driver_down(&drv); DH_ERROR("read image base failed"); return 14;
    }

    DH_INFO("UAGame base=0x%llX size=0x%llX", imageBase, imageSize);

    // Reader loop @ 5 Hz for MVP readability.
    AH_ENTITY ents[MAX_ENT];
    for (int tick = 0; ; tick++) {
        AH_WORLD_CTX wctx = {0};
        if (!resolve_world(drv.hDevice, procCR3, imageBase, &wctx)) {
            printf("[tick %d] world chain broken\n", tick);
            Sleep(200);
            continue;
        }
        u32 n = walk_players(drv.hDevice, procCR3, imageBase,
                             wctx.gamestate, ents, MAX_ENT);
        printf("\n[tick %d] world=%llX gs=%llX gi=%llX  players=%u\n",
               tick, wctx.world, wctx.gamestate, wctx.gameinst, n);
        for (u32 i = 0; i < n; i++) {
            const AH_ENTITY* e = &ents[i];
            printf("  [%2u] team=%u hp=%6.1f  ", i, e->team, e->hp);
            if (e->pos_ok)
                printf("pos=(%8.0f, %8.0f, %6.0f)  ", e->x, e->y, e->z);
            else
                printf("pos=<decrypt fail %d>          ", (int)AhAceLastFail());
            printf("%s\n", e->name[0] ? e->name : "<no name>");
        }
        Sleep(200);
        if (tick >= 25) break;   // ~5s of output for MVP smoke-test
    }

    driver_down(&drv);
    return 0;
}
