// arenahack — self position + yaw probe. Read chain identical to ABIFINAL:
//   GWorld → UGameInstance@+0x180 → LocalPlayers[0]@+0x38 → PC@+0x30
//   PC.Pawn@+0x348 → Root@+0x170 → ACE-decrypt → world x/y/z
//   PC.ControlRot@+0x380 → FRotator pitch/yaw/roll (plain, live per-tick)
//   PC.PlayerCameraManager@+0x3B0 → private cache FMinimalViewInfo @+0x2130
//                                 → LOC@+0, ROT@+0xC, FOV@+0x18
//
// Loop 30s @ 2Hz — walk around, look around, watch values change.

#include "../inc/dh_common.h"
#include "../inc/dh_scm.h"
#include "../inc/dh_provider.h"
#include "../inc/dh_rpm.h"
#include "../inc/ah_offsets.h"
#include "../inc/ah_ace.h"

#include <windows.h>
#include <stdio.h>

extern const DH_PROVIDER* g_active_provider;

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

int wmain(int argc, wchar_t** argv) {
    (void)argc; (void)argv;
    printf("arenahack self-probe — pos + yaw of local player @2Hz for 30s\n\n");

    if (!is_elevated()) { DH_ERROR("not elevated"); return 3; }

    DH_DRIVER drv = {0};
    HANDLE dev = NULL; u32 flags = 0;
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (!p || !dev) { DH_ERROR("driver_up failed"); return 10; }
    drv.hDevice = dev;
    g_active_provider = p;

    u64 sysCR3 = 0;
    if (!RpmFindSystemCR3(drv.hDevice, &sysCR3)) {
        DH_ERROR("sysCR3 fail"); CloseHandle(dev); return 11;
    }
    u64 procCR3 = 0, eproc = 0;
    if (!RpmFindProcess(drv.hDevice, sysCR3, AH_PROC_NAME, &procCR3, &eproc)) {
        DH_ERROR("UAGame not found — start the game first"); CloseHandle(dev); return 12;
    }
    u64 peb = 0;
    RpmRead64(drv.hDevice, sysCR3, eproc + g_eproc_peb_off, &peb);
    u64 base = 0, size = 0;
    RpmGetMainImageBase(drv.hDevice, procCR3, peb, &base, &size);
    DH_INFO("UAGame base=0x%llX  sysCR3=0x%llX  procCR3=0x%llX", base, sysCR3, procCR3);

    for (int tick = 0; tick < 60; tick++) {
        // Fresh chain walk every tick — no caching, so raid transitions self-heal.
        u64 gworld = 0;
        RpmRead64(drv.hDevice, procCR3, base + AH_RVA_GWORLD, &gworld);

        u64 gi = 0, lp_arr = 0, lp0 = 0, pc = 0, pawn = 0, root = 0, pcm = 0;
        if (gworld) RpmRead64(drv.hDevice, procCR3, gworld + AH_UW_GAMEINSTANCE, &gi);
        if (gi)     RpmRead64(drv.hDevice, procCR3, gi + AH_GI_LOCALPLAYERS, &lp_arr);
        if (lp_arr) RpmRead64(drv.hDevice, procCR3, lp_arr, &lp0);
        if (lp0)    RpmRead64(drv.hDevice, procCR3, lp0 + AH_LP_PC, &pc);
        if (pc) {
            RpmRead64(drv.hDevice, procCR3, pc + AH_PC_PAWN, &pawn);
            if (!pawn) RpmRead64(drv.hDevice, procCR3, pc + AH_PC_PAWN_ACK, &pawn);
            RpmRead64(drv.hDevice, procCR3, pc + AH_PC_CAMMGR, &pcm);
        }
        if (pawn) RpmRead64(drv.hDevice, procCR3, pawn + AH_PAWN_ROOT, &root);

        // POS via ACE decrypt of user_root.
        float px = 0, py = 0, pz = 0;
        BOOL pos_ok = FALSE;
        if (root) {
            pos_ok = AhAceDecrypt(drv.hDevice, procCR3, base, root, &px, &py, &pz);
        }

        // YAW from PC.ControlRot (@PC+0x380) — plain FRotator {pitch, yaw, roll}.
        float ctl[3] = {0};
        if (pc) RpmReadVirtual(drv.hDevice, procCR3, pc + AH_PC_CONTROLROT, ctl, 12);

        // Cross-check from PCM private cache (@PCM+0x213C for ROT).
        float pcm_rot[3] = {0};
        if (pcm) RpmReadVirtual(drv.hDevice, procCR3, pcm + AH_PCM_CACHE_ROT_PRIV, pcm_rot, 12);

        printf("[%2d]  pawn=%llx root=%llx pcm=%llx\n", tick,
               (unsigned long long)pawn, (unsigned long long)root, (unsigned long long)pcm);
        if (pos_ok)
            printf("      POS  x=%9.1f  y=%9.1f  z=%7.1f  (ACE decrypt %d)\n",
                   px, py, pz, (int)AhAceLastFail());
        else
            printf("      POS  <decrypt fail: %d>\n", (int)AhAceLastFail());
        printf("      PC.ControlRot   pitch=%7.2f  yaw=%7.2f  roll=%5.2f\n",
               ctl[0], ctl[1], ctl[2]);
        printf("      PCM priv POV    pitch=%7.2f  yaw=%7.2f  roll=%5.2f\n",
               pcm_rot[0], pcm_rot[1], pcm_rot[2]);

        Sleep(500);
    }

    CloseHandle(dev);
    return 0;
}
