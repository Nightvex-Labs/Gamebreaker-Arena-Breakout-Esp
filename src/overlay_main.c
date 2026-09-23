// arenahack overlay entry — delegates to full ABIFINAL abi::Overlay stack
// dropped into src/abi_ui/. Menu = abi::render_control_panel(cfg) each frame.
// C++ bootstrap lives in overlay_boot.cpp.
#include <stdio.h>

extern void DhInitHardening(void);   // src/hardening/dh_amsi_etw.c
extern int  AhOverlayRun(void);

int wmain(int argc, wchar_t** argv) {
    (void)argc; (void)argv;
    // FIRST — anti-debug + AMSI/ETW patch before anything else runs.
    // Silent ExitProcess if a debugger is attached.
    DhInitHardening();
    // NO printf — spawned from detached/no-console launcher, stdout invalid.
    return AhOverlayRun();
}
