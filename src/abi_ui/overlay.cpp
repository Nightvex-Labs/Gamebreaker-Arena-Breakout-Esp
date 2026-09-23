#include "overlay.hpp"
#include "obf.hpp"
#include "api_resolver.hpp"  // v0.9.472 A7
#include "runlog.hpp"
#include "crash_marker.hpp"
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include "icons.hpp"
#include "image_loader.hpp"
#include "control_panel.hpp"
#include <psapi.h>
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <string>
#pragma comment(lib, "psapi.lib")

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")

#ifndef DWMWA_EXCLUDED_FROM_CAPTURE
#define DWMWA_EXCLUDED_FROM_CAPTURE 0x00000011
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace abi {

// fwd — defined below, needed by set_input_capture() to restore game focus
static HWND find_game_hwnd();

// v0.9.436: set to true in create_d3d() when WARP is selected.  Read by
// per-frame fps-cap clamp below to hard-limit renderer to 30fps under WARP.
static bool g_using_warp = false;

Overlay* Overlay::s_instance = nullptr;

Overlay::Overlay() { s_instance = this; }
Overlay::~Overlay() { shutdown(); s_instance = nullptr; }

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool input_on = s_instance ? s_instance->input_capture_.load() : false;
    const bool take_input = input_on;

    if (!take_input) {
        // Click-through mode — route events to game beneath us.
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
        bool is_mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        if (!is_mouse) {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
                return 1;
        }
    } else {
        // Capture-input mode — full ImGui interaction.
        if (msg == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return 1;
    }
    switch (msg) {
    case WM_SIZE:
        if (s_instance && s_instance->d3d_device_ && wp != SIZE_MINIMIZED) {
            s_instance->cleanup_rtv();
            s_instance->swapchain_->ResizeBuffers(
                0, (UINT)LOWORD(lp), (UINT)HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            s_instance->create_rtv();
        }
        return 0;
    // Swallow WM_CLOSE — DefWindowProc would translate it to WM_DESTROY +
    // exit. Alt+F4 / X-button / SC_CLOSE ALL reach us as WM_CLOSE. Overlay
    // exits ONLY via game-death detector (running_=false in overlay::run)
    // or user-initiated Quit menu. Prevents accidental Alt+F4 kill when
    // overlay has focus (menu open).
    case WM_CLOSE:
        return 0;
    // WM_SYSCOMMAND SC_CLOSE — Alt+Space→Close menu, taskbar close. Same
    // policy: ignore. Other SC_* (SC_MINIMIZE etc.) fall through to Def.
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) return 0;
        break;
    case WM_DESTROY:
        if (s_instance) s_instance->running_ = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// v0.9.437: set_fuser_mode() removed — 1PC-only project, transparent overlay always.

// v0.9.410 mouse-wheel: WS_EX_TRANSPARENT blocks WM_MOUSEWHEEL delivery to
// our overlay (event goes to the game/window underneath). To feed scroll
// into ImGui we install a WH_MOUSE_LL system hook while the panel is open
// and unhook when it closes. The hook is per-thread but low-level hooks
// run in the caller's context — cheap for the seconds the panel is up.
static std::atomic<float> g_wheel_accum{0.0f};
static HHOOK             g_mouse_hook = nullptr;

static LRESULT CALLBACK ll_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        auto* p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        short delta = (short)HIWORD(p->mouseData);
        float steps = (float)delta / (float)WHEEL_DELTA;
        for (;;) {
            float cur = g_wheel_accum.load();
            if (g_wheel_accum.compare_exchange_weak(cur, cur + steps)) break;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void Overlay::set_input_capture(bool on) {
    // v0.9.414: WH_MOUSE_LL system-wide hook REMOVED (ACE aimbot signature).
    // v0.9.418: while panel is open we also clear WS_EX_TRANSPARENT on the
    // overlay HWND so mouse events land in our WndProc → ImGui backend
    // gets clicks + wheel natively (no polled path needed for wheel/scroll,
    // no global hook). Clicks stop passing through to the game — acceptable
    // while user is configuring the panel; they're not aiming at that moment.
    // On close we restore WS_EX_TRANSPARENT so gameplay clicks pass through.
    input_capture_.store(on);
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        if (hwnd_) {
            LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
            // Force user-mode focus so wheel events route to us, not to whatever
            // window had focus before (game). SetForegroundWindow can be denied
            // by foreground-lock rules — a quick AttachThreadInput escape hatch
            // guarantees the swap. Cheap since panel opens once per session.
            SetForegroundWindow(hwnd_);
        }
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        g_wheel_accum.store(0.0f);
        if (hwnd_) {
            LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
        }
    }
}

bool Overlay::init(int sw, int sh) {
    sw_ = sw; sh_ = sh;
    LOG("overlay::init enter sw=%d sh=%d", sw, sh);
    LOG("overlay::init calling create_window()");
    if (!create_window()) { LOG("overlay::init create_window FAIL"); return false; }
    LOG("overlay::init create_window OK hwnd=%p", hwnd_);
    LOG("overlay::init calling create_d3d()");
    if (!create_d3d())    { LOG("overlay::init create_d3d FAIL"); return false; }
    LOG("overlay::init create_d3d OK device=%p ctx=%p", d3d_device_, d3d_ctx_);
    LOG("overlay::init calling protect_from_capture()");
    protect_from_capture();
    LOG("overlay::init protect_from_capture returned");

    LOG("overlay::init IMGUI_CHECKVERSION + CreateContext");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Transparent click-through overlay — start with mouse disabled; enabled
    // dynamically via set_input_capture() when panel opens.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_WindowBg]  = ImVec4(0,0,0,0);
    st.Colors[ImGuiCol_MenuBarBg] = ImVec4(0,0,0,0);

    // Combined Latin + Cyrillic + Simplified-Chinese range so we can display
    // any player nickname regardless of alphabet.
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    // Cover extras common in gamertags — arrows, symbols, extended Latin.
    static const ImWchar extra_range[] = { 0x2000, 0x206F, 0x2100, 0x21FF, 0x2600, 0x26FF, 0 };
    builder.AddRanges(extra_range);
    static ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    ImFontConfig cfg;
    cfg.MergeMode = false;
    cfg.PixelSnapH = true;
    // Simple single-font loader (baseline pre-bundle).
    const char* font_candidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    for (const char* p : font_candidates) {
        if (io.Fonts->AddFontFromFileTTF(p, 15.0f, &cfg, ranges.Data)) break;
    }
    if (io.Fonts->Fonts.Size == 0) io.Fonts->AddFontDefault();
    LOG("overlay::init font slots loaded: %d", io.Fonts->Fonts.Size);

    LOG("overlay::init calling ImGui_ImplWin32_Init");
    ImGui_ImplWin32_Init(hwnd_);
    LOG("overlay::init calling ImGui_ImplDX11_Init");
    ImGui_ImplDX11_Init(d3d_device_, d3d_ctx_);
    LOG("overlay::init ImGui backends OK");

    // Rasterise the mock's SVG icons into D3D11 textures. Each glyph
    // becomes an ImTextureID that control_panel.cpp blits with per-
    // call colour modulation. Any new icon = one line in
    // icons_data.hpp — no font atlas fiddling.
    LOG("overlay::init calling icons::init");
    icons::init(d3d_device_, 32);
    LOG("overlay::init icons::init done");

    // v0.9.410 Panel wallpaper + operator silhouette — DISABLED in prod for
    // now. WIC / GetEnvironmentVariableW / file I/O in the hollowed explorer
    // context crashed at this point (log truncated right after icons::init
    // done). Assets are cosmetic; the panel falls back to text placeholders
    // without them. Reintroduce once wrapped in SEH + CoInitializeEx guard.
    LOG("overlay::init skipping asset load (prod build)");

    // v0.9.359 hollow-safe: ShowWindowAsync posts WM_SHOWWINDOW via message
    // queue instead of the inline win32u.dll activation path that faults
    // 0xc0000005 in hollowed elevated Session 2. UpdateWindow skipped — first
    // WM_PAINT arrives naturally through the message loop below.
    LOG("overlay::init ShowWindowAsync (SW_SHOWNOACTIVATE)");
    ShowWindowAsync(hwnd_, SW_SHOWNOACTIVATE);
    LOG("overlay::init returning true");
    return true;
}

// Randomised class name so bulk-detect rules keyed on a single fixed literal
// (e.g. "Chrome_WidgetWin_1 at exactly game resolution, topmost, transparent")
// miss us. Pool is real Windows-native class prefixes, with an ATL-style hex
// suffix appended so RegisterClassExW never collides with an app that's
// legitimately running that class. Class stays stable for the process
// lifetime but changes on every launch.
static std::wstring pick_overlay_class_name() {
    static const wchar_t* pool[] = {
        L"Chrome_WidgetWin",     L"Edge_WidgetWin",
        L"MozillaWindowClass",   L"Qt5QWindowIcon",
        L"SunAwtFrame",          L"TApplication",
        L"ApplicationFrameWindow", L"Windows.UI.Core.CoreWindow",
        L"ATL",                  L"OleMainThreadWndClass",
        L"MSCTFIME UI",          L"IME",
        L"NVIDIA_Overlay",       L"RTSSHooksClass",
    };
    LARGE_INTEGER li{}; QueryPerformanceCounter(&li);
    uint64_t seed = (uint64_t)li.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 16);
    auto rng = [&]() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(seed >> 32); };
    std::wstring name = pool[rng() % (sizeof(pool)/sizeof(pool[0]))];
    // Append :{hex} — mirrors the ATL and MFC WSCLASS auto-suffix pattern.
    name += L":";
    static const wchar_t hex[] = L"0123456789abcdef";
    for (int i = 0; i < 8; ++i) name += hex[rng() % 16];
    return name;
}

bool Overlay::create_window() {
    // Compute once, keep alive for the process — CreateWindowExW dereferences
    // the pointer LATER during window init, so the storage must outlive the call.
    static std::wstring class_name = pick_overlay_class_name();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = nullptr;   // no cursor; transparent click-through to game
    wc.hbrBackground = nullptr;
    wc.lpszClassName = class_name.c_str();
    RegisterClassExW(&wc);

    // v0.9.437: 1PC transparent click-through overlay only (fuser removed).
    // Clicks pass through to game via WS_EX_TRANSPARENT.  Panel input handled
    // by direct GetCursorPos / GetAsyncKeyState polling into ImGui each frame
    // (see feed_polled_mouse_to_imgui in overlay::run) — bypasses Windows
    // hit-testing, foreground, SetCapture entirely.  set_input_capture() is a
    // pure atomic flip that gates the polling — no USER32 calls, hollow-safe.
    DWORD ex_style = WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW
                   | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    wc.hCursor = nullptr;
    hwnd_ = CreateWindowExW(
        ex_style,
        class_name.c_str(), L"",
        WS_POPUP,
        0, 0, sw_, sh_,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    return true;
}

// Detect GPUs known to crash D3D11CreateDevice HARDWARE on Win11 25H2.
// AMD Polaris family (RX 4xx, RX 5x0 non-XT, R9 380/390) is on the legacy
// driver branch and reliably faults inside amdxc64.dll on 26200.8875+.
// String-match by adapter description; VendorId 0x1002 (AMD) + DeviceId
// range would be more precise but the string form is compiler-friendly
// and easier to extend as new bad configs surface.
static bool is_polaris_or_legacy_amd() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    bool legacy = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            // Skip the software adapter (WARP / Basic Display Adapter)
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                const wchar_t* name = desc.Description;
                // Polaris RX 400/500 series (RX 460/470/480/550/560/570/580/590)
                if (wcsstr(name, L"RX 460") || wcsstr(name, L"RX 470") ||
                    wcsstr(name, L"RX 480") || wcsstr(name, L"RX 490") ||
                    wcsstr(name, L"RX 550") || wcsstr(name, L"RX 560") ||
                    wcsstr(name, L"RX 570") || wcsstr(name, L"RX 580") ||
                    wcsstr(name, L"RX 590"))
                    legacy = true;
                // Older GCN — R9 380/390, R7 370, R9 285 (Tonga/Hawaii)
                if (wcsstr(name, L"R9 380") || wcsstr(name, L"R9 390") ||
                    wcsstr(name, L"R9 290") || wcsstr(name, L"R9 285") ||
                    wcsstr(name, L"R7 370") || wcsstr(name, L"R7 360"))
                    legacy = true;
            }
        }
        adapter->Release();
    }
    factory->Release();
    return legacy;
}

bool Overlay::create_d3d() {
    // Fallback ladder — driver crashes on some AMD legacy configs bypass SEH
    // via RaiseFailFastException / TerminateProcess from the driver DLL side,
    // so a first-time try can be uncatchable. Two mechanisms combined:
    //   1. Known-bad GPU detection — skip HW on RX 4xx/5x0 series (AMD legacy
    //      driver branch, always crashes on 25H2 26200.8875+).
    //   2. Persistent marker file — if HW attempt died last launch, next launch
    //      sees the file and goes straight to WARP even on unknown-GPU rigs.
    static const wchar_t* PREFER_WARP_FLAG = L"C:\\ProgramData\\.abi_gpu_warp_only";
    bool prefer_warp = (GetFileAttributesW(PREFER_WARP_FLAG) != INVALID_FILE_ATTRIBUTES);
    // v0.9.428: Polaris HW-driver crash only reproduces on Win 11 25H2
    // (build 26200+).  On older Windows (Win 10 22H2 / Win 11 22H2 / 24H2)
    // the legacy AMD driver works fine and WARP causes visible ESP lag.
    // Only force WARP when we're on 25H2+ AND the GPU is Polaris.
    if (!prefer_warp && is_polaris_or_legacy_amd()) {
        typedef LONG(WINAPI *pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
        // v0.9.472 A7: hash-based lookup.
        pfnRtlGetVersion pGV = api::resolve<pfnRtlGetVersion>(
            api::NTDLL, api::hash("RtlGetVersion"));
        RTL_OSVERSIONINFOW ov{sizeof(ov)};
        if (pGV && pGV(&ov) == 0 && ov.dwBuildNumber >= 26200) {
            LOG("create_d3d: Polaris + Win 11 25H2 (build %lu) — WARP forced (HW driver bug)",
                (unsigned long)ov.dwBuildNumber);
            prefer_warp = true;
        } else {
            LOG("create_d3d: Polaris on non-25H2 build — attempting HW (driver bug is 25H2-only)");
        }
    }
    LOG("create_d3d: prefer_warp=%d", prefer_warp ? 1 : 0);

    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = E_FAIL;

    if (!prefer_warp) {
        // Drop a "we're about to try HW" marker. If this launch dies inside
        // D3D11CreateDevice HW, next launch sees the marker → skips HW.
        // On successful HW init we DELETE the marker below.
        HANDLE marker = CreateFileW(PREFER_WARP_FLAG,
                                     GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
        LOG("create_d3d: marker dropped; attempt HARDWARE + BGRA_SUPPORT");
        __try {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                fls, 2, D3D11_SDK_VERSION,
                &d3d_device_, &fl, &d3d_ctx_);
            LOG("create_d3d: HW attempt returned hr=0x%08lx featureLevel=0x%04x", hr, (unsigned)fl);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("create_d3d: HW attempt CRASHED (SEH code=0x%08lx)", GetExceptionCode());
            hr = E_FAIL;
            if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
            if (d3d_ctx_)    { d3d_ctx_->Release();    d3d_ctx_ = nullptr; }
        }

        if (SUCCEEDED(hr)) {
            // HW worked — drop the marker so next launch tries HW again.
            DeleteFileW(PREFER_WARP_FLAG);
            LOG("create_d3d: HW OK, marker cleared");
        }
    } else {
        LOG("create_d3d: SKIPPING HW attempt — prior launch marked this system WARP-only");
    }

    // WARP fallback — Microsoft software rasterizer, always works on Win ≥ 8.
    if (FAILED(hr)) {
        LOG("create_d3d: attempting WARP (software rasterizer)");
        g_using_warp = true;   // v0.9.436: signal per-frame loop to clamp fps
        __try {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                fls, 2, D3D11_SDK_VERSION,
                &d3d_device_, &fl, &d3d_ctx_);
            LOG("create_d3d: WARP returned hr=0x%08lx featureLevel=0x%04x", hr, (unsigned)fl);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("create_d3d: WARP crashed (impossible) code=0x%08lx", GetExceptionCode());
            hr = E_FAIL;
        }
    }

    if (FAILED(hr)) { LOG("create_d3d: all D3D11CreateDevice paths FAILED, aborting"); return false; }
    LOG("create_d3d: D3D11 device created, featureLevel=0x%04x", (unsigned)fl);

    // Step 2: DXGI factory + composition swap chain
    LOG("create_d3d: QueryInterface IDXGIDevice");
    IDXGIDevice* dxgi_dev = nullptr;
    d3d_device_->QueryInterface(IID_PPV_ARGS(&dxgi_dev));
    LOG("create_d3d: dxgi_dev=%p", dxgi_dev);
    IDXGIAdapter* dxgi_adapter = nullptr;
    if (dxgi_dev) dxgi_dev->GetAdapter(&dxgi_adapter);
    LOG("create_d3d: dxgi_adapter=%p", dxgi_adapter);
    IDXGIFactory2* dxgi_factory = nullptr;
    if (dxgi_adapter) dxgi_adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));
    LOG("create_d3d: dxgi_factory=%p", dxgi_factory);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = sw_;
    sd.Height           = sh_;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SampleDesc.Count = 1;
    sd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    LOG("create_d3d: calling CreateSwapChainForComposition %dx%d", sd.Width, sd.Height);
    hr = dxgi_factory->CreateSwapChainForComposition(d3d_device_, &sd, nullptr, &swapchain_);
    LOG("create_d3d: CreateSwapChainForComposition hr=0x%08lx swapchain=%p", hr, swapchain_);
    dxgi_factory->Release();
    dxgi_adapter->Release();

    // Step 3: DirectComposition: device, target, visual
    hr = DCompositionCreateDevice(dxgi_dev, IID_PPV_ARGS(&dcomp_dev_));
    dxgi_dev->Release();
    if (FAILED(hr)) return false;

    hr = dcomp_dev_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_tgt_);
    if (FAILED(hr)) return false;
    hr = dcomp_dev_->CreateVisual(&dcomp_vis_);
    if (FAILED(hr)) return false;
    dcomp_vis_->SetContent(swapchain_);
    dcomp_tgt_->SetRoot(dcomp_vis_);
    dcomp_dev_->Commit();

    create_rtv();
    return true;
}

void Overlay::create_rtv() {
    ID3D11Texture2D* bb = nullptr;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        d3d_device_->CreateRenderTargetView(bb, nullptr, &rtv_);
        bb->Release();
    }
}

void Overlay::cleanup_rtv() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void Overlay::cleanup_d3d() {
    cleanup_rtv();
    if (dcomp_vis_)  { dcomp_vis_->Release();  dcomp_vis_  = nullptr; }
    if (dcomp_tgt_)  { dcomp_tgt_->Release();  dcomp_tgt_  = nullptr; }
    if (dcomp_dev_)  { dcomp_dev_->Release();  dcomp_dev_  = nullptr; }
    if (swapchain_)  { swapchain_->Release();  swapchain_  = nullptr; }
    if (d3d_ctx_)    { d3d_ctx_->Release();    d3d_ctx_    = nullptr; }
    if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
}

void Overlay::protect_from_capture() {
    // Anti-screencap ON — blocks OBS/ShadowPlay/Discord screenshots.
    set_capture_protection(true);
}

void Overlay::set_capture_protection(bool on) {
    if (!hwnd_) return;
    BOOL exc = on ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_CAPTURE, &exc, sizeof(exc));
    SetWindowDisplayAffinity(hwnd_, on ? WDA_EXCLUDEFROMCAPTURE : 0 /*WDA_NONE*/);
}

void Overlay::shutdown() {
    if (d3d_device_) {
        icons::shutdown();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    cleanup_d3d();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// Resolve the game window (matched by exe name) so the overlay can mirror
// its minimize/restore state. Also accepts common shipping-build variants.
static HWND find_game_hwnd() {
    struct Ctx { HWND hwnd; } ctx{};
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        // Do NOT gate on IsWindowVisible — a minimized UE4 window can lose
        // WS_VISIBLE and we still need to find it.
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        if (!pid) return TRUE;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return TRUE;
        wchar_t path[MAX_PATH] = L"";
        DWORD len = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(h, 0, path, &len);
        CloseHandle(h);
        if (!ok) return TRUE;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        bool hit = _wcsicmp(base, OBFW(L"UAGame.exe")) == 0
                || _wcsnicmp(base, OBFW(L"UAGame"), 6) == 0
                || _wcsnicmp(base, OBFW(L"ArenaBreakout"), 13) == 0;
        if (!hit) return TRUE;
        // Prefer a top-level window with a non-empty rect (skip hidden helpers).
        RECT r{}; GetWindowRect(hw, &r);
        if ((r.right - r.left) < 100 || (r.bottom - r.top) < 100) return TRUE;
        reinterpret_cast<Ctx*>(lp)->hwnd = hw;
        return FALSE;
    }, (LPARAM)&ctx);
    if (ctx.hwnd) return ctx.hwnd;
    // Fallback: UE4 shipping windows use class "UnrealWindow".
    return FindWindowW(L"UnrealWindow", nullptr);
}

// The overlay should be visible only when the game process holds focus.
// Borderless UE4 windows don't reliably flip IsIconic/SW_SHOWMINIMIZED, so
// tracking foreground ownership is what actually works.
static bool game_owns_foreground(HWND game_hw, HWND self_hw) {
    if (!game_hw || !IsWindow(game_hw)) return true;   // no game found — leave overlay up
    if (IsIconic(game_hw)) return false;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == self_hw) return true;                    // our own control panel

    DWORD game_pid = 0, fg_pid = 0;
    GetWindowThreadProcessId(game_hw, &game_pid);
    GetWindowThreadProcessId(fg, &fg_pid);
    return game_pid && fg_pid && game_pid == fg_pid;
}

}  // close abi namespace so the extern binds to ::-scope global from main.cpp
extern std::atomic<bool> g_game_active;
extern std::atomic<int>  g_render_fps_cap;      // v0.9.427: 30/60/120
extern std::atomic<int>  g_render_fps_hidden;   // v0.9.427: 5..30

// v0.9.451: SEH-safe Present wrapper. Free function (no C++ objects with
// destructors) so it can host __try/__except, which Overlay::run cannot
// because it holds ImGui / D3D / std::function objects that require
// unwinding (C2712). Runtime cost: zero — one extra call frame.
static HRESULT safe_present(IDXGISwapChain1* sc) noexcept {
    __try {
        return sc ? sc->Present(0, 0) : E_POINTER;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }
}

namespace abi {

void Overlay::run(const std::function<void()>& frame_fn) {
    MSG msg{};
    HWND game_hwnd = nullptr;
    bool overlay_visible = true;
    int  game_probe_ctr  = 0;

    // v0.9.409 Alt+Tab hide via cached HWND compare. Only USER32 call is
    // GetForegroundWindow (safe, returns whatever HWND — no callback), plus
    // a pointer comparison against our own overlay HWND and the cached
    // UAGame HWND (populated by watchdog). No GetWindowThreadProcessId,
    // no OpenProcess, no EnumWindows — those were the v0.9.359 / v0.9.407
    // crash surfaces. Poll only every 6 frames (~50ms at 120fps) to reduce
    // exposure and system-wide GetForegroundWindow load.
    int  fg_poll_ctr = 0;
    while (running_) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running_ = false;
        }
        if (!running_) break;

        // ── Overlay visibility gate + game-death auto-exit ────────────────
        // Re-locate game HWND every ~1s (60 frames @60fps). Between probes
        // reuse cached handle. If game not foreground → hide overlay window.
        // If game process gone for 3 consecutive probes (~3s) → break loop
        // → self-exit → cage janitor wipes all artifacts.
        if (++game_probe_ctr >= 60) {
            game_probe_ctr = 0;
            HWND fresh = find_game_hwnd();
            if (fresh) { game_hwnd = fresh; }
            else if (game_hwnd && !IsWindow(game_hwnd)) game_hwnd = nullptr;
        }
        if (++fg_poll_ctr >= 6) {
            fg_poll_ctr = 0;
            bool should_show = game_owns_foreground(game_hwnd, hwnd_);
            if (should_show != overlay_visible) {
                overlay_visible = should_show;
                ShowWindowAsync(hwnd_, should_show ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
        }
        // Game process gone check — poll every ~60 frames. Only ARM the
        // death detector after we've seen the game HWND at least once
        // (user often opens overlay before launching game). 3 consecutive
        // misses → exit → cage janitor wipes payload.
        static bool game_seen_once = false;
        static int  game_miss_streak = 0;
        static int  game_death_ctr = 0;
        if (game_hwnd && IsWindow(game_hwnd)) game_seen_once = true;
        if (game_seen_once && ++game_death_ctr >= 60) {
            game_death_ctr = 0;
            bool alive = (game_hwnd && IsWindow(game_hwnd));
            if (!alive) {
                HWND probe = find_game_hwnd();
                alive = (probe && IsWindow(probe));
            }
            if (!alive) { game_miss_streak++; }
            else { game_miss_streak = 0; }
            if (game_miss_streak >= 3) {
                LOG("overlay::run: game window gone for 3 probes — self-exit");
                running_ = false;
                break;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // v0.9.367 polled mouse: window has WS_EX_TRANSPARENT set so all
        // clicks pass through to the game natively. When the settings panel
        // is open (input_capture_ = true), feed ImGui raw cursor position +
        // button state so the panel can be operated with mouse. GetCursorPos
        // and GetAsyncKeyState are cross-thread safe reads — no USER32 fault
        // class. Bypasses window-manager hittest, foreground, SetCapture.
        if (input_capture_.load() && hwnd_) {
            ImGuiIO& io = ImGui::GetIO();
            POINT pt{};
            if (GetCursorPos(&pt)) {
                ScreenToClient(hwnd_, &pt);
                io.AddMousePosEvent((float)pt.x, (float)pt.y);
            }
            io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
            // v0.9.410: drain wheel events collected by low-level hook.
            float steps = g_wheel_accum.exchange(0.0f);
            if (steps != 0.0f) io.AddMouseWheelEvent(0.0f, steps);
        }

        ImGui::NewFrame();

        frame_fn();

        ImGui::Render();
        const float clr[4] = { 0, 0, 0, 0 };   // v0.9.437: always transparent (fuser removed)
        d3d_ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        d3d_ctx_->ClearRenderTargetView(rtv_, clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // VSync off. Cap render loop at cfg.render_fps_cap (30/60/120).
        // When overlay is hidden (Alt+Tab / game not foreground), fall back
        // to cfg.render_fps_hidden — saves GPU on weak cards (RX 470/570).
        //
        // v0.9.451: check Present hr. TDR (GPU driver timeout recovery) or
        // any GPU stall during heavy in-game load returns DXGI_ERROR_
        // DEVICE_REMOVED / DEVICE_HUNG / DEVICE_RESET. Prior versions
        // ignored the hr, kept calling OMSetRenderTargets / RenderDrawData
        // on the dead device next frame, and the driver DLL would then
        // fast-fail the process (bypassing SEH entirely). This was the
        // "software just closes mid-game" report on weak / mobile GPUs
        // where TDR fires under intense combat scenes.
        HRESULT present_hr = safe_present(swapchain_);
        if (FAILED(present_hr)) {
            if (present_hr == DXGI_ERROR_DEVICE_REMOVED ||
                present_hr == DXGI_ERROR_DEVICE_HUNG    ||
                present_hr == DXGI_ERROR_DEVICE_RESET)
            {
                HRESULT rr = d3d_device_ ? d3d_device_->GetDeviceRemovedReason() : E_FAIL;
                LOG("overlay: Present DEVICE_LOST hr=0x%08lx removedReason=0x%08lx — stopping render loop",
                    present_hr, rr);
                // Drop the persistent marker so the next-boot postmortem
                // knows GPU driver crash killed the last session. Stop the
                // loop cleanly instead of continuing on a dead swapchain
                // and inviting the driver DLL fast-fail.
                crash_marker::write(crash_marker::OVERLAY_DEVICE_LOST);
                running_ = false;
                break;
            }
            // Non-fatal Present failures (OCCLUDED, STATUS_UNSUCCESSFUL): log
            // once and keep going — usually resolves within a frame.
            static bool logged = false;
            if (!logged) {
                LOG("overlay: Present returned 0x%08lx (non-fatal, will retry silently)", present_hr);
                logged = true;
            }
        }
        {
            using namespace std::chrono;
            static auto t_prev = steady_clock::now();
            auto t_now = steady_clock::now();
            int cap = overlay_visible
                        ? ::g_render_fps_cap.load(std::memory_order_relaxed)
                        : ::g_render_fps_hidden.load(std::memory_order_relaxed);
            if (cap < 5) cap = 5;
            if (cap > 240) cap = 240;
            // v0.9.436: hard cap when running under WARP (software renderer) or
            // on ≤4-thread CPU.  WARP at 120fps eats ~15-20% CPU per core on
            // Ryzen 3 1200 — kills game framerate.  Clamp to 30fps regardless
            // of user setting; visually indistinguishable for an ESP overlay
            // and gives back 3× CPU to the game.  Hidden cap goes down to 5
            // fps in the same conditions.
            static const int hw_cap = []() {
                SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
                bool weak_cpu = si.dwNumberOfProcessors <= 4;
                bool clamp = (weak_cpu || g_using_warp);
                if (clamp) {
                    LOG("fps-cap: WARP=%d weak_cpu=%d → hard cap 30fps visible / 5fps hidden",
                        (int)g_using_warp, (int)weak_cpu);
                }
                return clamp ? 30 : 240;
            }();
            if (cap > hw_cap) cap = hw_cap;
            if (!overlay_visible && hw_cap == 30 && cap > 5) cap = 5;
            const double target = 1.0 / (double)cap;
            double dt = duration<double>(t_now - t_prev).count();
            if (dt < target) {
                std::this_thread::sleep_for(duration<double>(target - dt));
            }
            t_prev = steady_clock::now();
        }
        if (dcomp_dev_) dcomp_dev_->Commit();
    }
}

}  // namespace abi
