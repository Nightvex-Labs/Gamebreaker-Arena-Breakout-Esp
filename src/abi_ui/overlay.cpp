#include "overlay.hpp"
#include "obf.hpp"
#include "api_resolver.hpp"  // v0.9.472 A7
#include "runlog.hpp"
#include "crash_marker.hpp"
#include "../ah_reader_thread.h"   // v0.9.455: ah_reader_state()
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
#include <cstdint>
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

// v0.9.454: consecutive Present DEVICE_LOST count. Reset on any successful
// Present so a recovered TDR doesn't count towards the exit gate. Exit only
// after 3 consecutive lost frames — modern GPU drivers recover within one.
static std::atomic<int>  g_present_lost_streak{0};

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

// Force-set foreground even under foreground-lock rules. Attach our input
// queue to the current foreground's thread, then SetForegroundWindow is
// treated as sanctioned. Detach immediately. No global hook, no injection.
static bool force_set_foreground(HWND target) {
    if (!target || !IsWindow(target)) return false;
    HWND cur = GetForegroundWindow();
    if (cur == target) return true;
    DWORD cur_tid = cur ? GetWindowThreadProcessId(cur, nullptr) : 0;
    DWORD our_tid = GetCurrentThreadId();
    BOOL attached = FALSE;
    if (cur_tid && cur_tid != our_tid) {
        attached = AttachThreadInput(our_tid, cur_tid, TRUE);
    }
    BOOL ok = SetForegroundWindow(target);
    if (attached) AttachThreadInput(our_tid, cur_tid, FALSE);
    return ok != FALSE;
}

void Overlay::set_input_capture(bool on) {
    // v0.9.414: WH_MOUSE_LL system-wide hook REMOVED (ACE aimbot signature).
    // v0.9.418: while panel is open we also clear WS_EX_TRANSPARENT on the
    // overlay HWND so mouse events land in our WndProc → ImGui backend
    // gets clicks + wheel natively (no polled path needed for wheel/scroll,
    // no global hook). Clicks stop passing through to the game — acceptable
    // while user is configuring the panel; they're not aiming at that moment.
    // On close we restore WS_EX_TRANSPARENT so gameplay clicks pass through.
    //
    // v0.9.452 wheel-routing fix: SetForegroundWindow(hwnd_) on open moved
    // keyboard focus to us. Windows routes WM_MOUSEWHEEL to the focused
    // window by default (SPI_GETMOUSEWHEELROUTING = MOUSEWHEEL_ROUTING_FOCUS),
    // so after the panel closed the game NEVER got wheel events back — the
    // 2x/4x/7x variable-zoom scope scroll was dead the whole session. Now
    // stash prev foreground on open and restore it on close (with the
    // AttachThreadInput bypass so foreground-lock can't refuse us).
    input_capture_.store(on);
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        if (hwnd_) {
            HWND cur_fg = GetForegroundWindow();
            if (cur_fg && cur_fg != hwnd_) prev_fg_hwnd_ = cur_fg;
            LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
            force_set_foreground(hwnd_);
        }
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        g_wheel_accum.store(0.0f);
        if (hwnd_) {
            LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
            // Give focus back to whoever had it before panel opened (the
            // game, in practice). Without this the overlay stays focused
            // even with WS_EX_TRANSPARENT restored, so mouse-wheel goes
            // to us and gets swallowed — scope zoom scroll dead in game.
            HWND target = (prev_fg_hwnd_ && IsWindow(prev_fg_hwnd_))
                            ? prev_fg_hwnd_ : find_game_hwnd();
            if (target && target != hwnd_) {
                force_set_foreground(target);
            }
            prev_fg_hwnd_ = nullptr;
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

// v1.0.26 GPU probe — logs vendor/device/name of every adapter for triage.
// Cheap, no side effects, no config change; only DXGI enum.
static void log_gpu_adapters() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        LOG("gpu_probe: CreateDXGIFactory1 FAILED — DXGI subsystem broken");
        return;
    }
    IDXGIAdapter1* adapter = nullptr;
    UINT idx = 0;
    for (; factory->EnumAdapters1(idx, &adapter) != DXGI_ERROR_NOT_FOUND; ++idx) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            char name8[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name8, sizeof(name8)-1, nullptr, nullptr);
            LOG("gpu_probe: adapter[%u] vendor=0x%04x device=0x%04x rev=0x%04x flags=0x%x vram=%lluMB name=\"%s\"",
                idx, d.VendorId, d.DeviceId, d.Revision, d.Flags,
                (unsigned long long)(d.DedicatedVideoMemory / (1024ULL*1024ULL)), name8);
        }
        adapter->Release();
    }
    if (idx == 0) LOG("gpu_probe: NO adapters enumerated — headless / display-driver dead");
    factory->Release();
}

// v1.0.26 marker layout — 16-byte header replacing empty touch-file.
// Backwards-compatible: any file smaller than sizeof(WarpMarker) is treated as
// legacy touch (created_ft=0, warp_fail_count=0 → HW retry eligible).
#pragma pack(push, 1)
struct WarpMarker {
    uint32_t magic;              // 'AWMK' (0x4B4D5741 LE)
    uint32_t warp_fail_count;    // number of WARP init failures with marker present
    uint64_t created_ft;         // FILETIME 100ns UTC when marker first dropped
};
#pragma pack(pop)
static_assert(sizeof(WarpMarker) == 16, "WarpMarker size drift");

static const uint32_t WARP_MARKER_MAGIC = 0x4B4D5741; // 'AWMK'
static const uint64_t WARP_MARKER_TTL_100NS = 72ULL * 3600ULL * 10000000ULL; // 72h
static const uint32_t WARP_MARKER_MAX_FAILS = 3;

static bool read_warp_marker(const wchar_t* path, WarpMarker* out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    BOOL ok = ReadFile(h, out, sizeof(*out), &got, nullptr);
    CloseHandle(h);
    return ok && got == sizeof(*out) && out->magic == WARP_MARKER_MAGIC;
}

static void write_warp_marker(const wchar_t* path, const WarpMarker* m) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, m, sizeof(*m), &wrote, nullptr);
    CloseHandle(h);
}

static uint64_t filetime_now_100ns() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

bool Overlay::create_d3d() {
    // v1.0.26 rewritten WARP-marker logic with TTL + fail-count.
    //
    // Fallback ladder — driver crashes on some AMD legacy configs bypass SEH
    // via RaiseFailFastException / TerminateProcess from the driver DLL side,
    // so a first-time try can be uncatchable. Three-layer defense:
    //   1. Known-bad GPU detection — skip HW on RX 4xx/5x0 series (AMD legacy
    //      driver branch, always crashes on 25H2 26200.8875+).
    //   2. Persistent marker file with 72h TTL — if HW attempt died last launch,
    //      next launch (within 72h) sees the file and goes straight to WARP.
    //      After 72h expires we retry HW (driver may have been updated).
    //   3. WARP fail-counter — if WARP itself fails ≥3 times with marker set,
    //      the marker is deleted so next launch retries HW (WARP-only mode
    //      isn't helping this rig — better to try HW again than stay locked).
    log_gpu_adapters();

    static const wchar_t* PREFER_WARP_FLAG = L"C:\\ProgramData\\.abi_gpu_warp_only";
    WarpMarker mk{};
    bool marker_present = false;
    bool marker_valid = false;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        marker_present = GetFileAttributesExW(PREFER_WARP_FLAG, GetFileExInfoStandard, &fad) != 0;
        if (marker_present) {
            if (read_warp_marker(PREFER_WARP_FLAG, &mk)) {
                marker_valid = true;
                LOG("create_d3d: marker present magic=OK fails=%u created_ft=0x%016llx",
                    mk.warp_fail_count, (unsigned long long)mk.created_ft);
            } else {
                // Legacy empty file — synthesize marker with created_ft from filetime.
                mk.magic = WARP_MARKER_MAGIC;
                mk.warp_fail_count = 0;
                mk.created_ft = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32)
                              | fad.ftLastWriteTime.dwLowDateTime;
                marker_valid = true;
                LOG("create_d3d: marker present (legacy touch) — upgraded, created_ft=0x%016llx",
                    (unsigned long long)mk.created_ft);
            }
        }
    }

    bool prefer_warp = marker_valid;

    // TTL: marker older than 72h → invalidate, retry HW.
    if (prefer_warp) {
        uint64_t now = filetime_now_100ns();
        if (mk.created_ft && now > mk.created_ft && (now - mk.created_ft) > WARP_MARKER_TTL_100NS) {
            LOG("create_d3d: marker EXPIRED (>72h old) — deleting, retry HW");
            DeleteFileW(PREFER_WARP_FLAG);
            prefer_warp = false;
        }
    }

    // Fail-cap: WARP failed ≥3 times with marker set → marker is not helping.
    if (prefer_warp && mk.warp_fail_count >= WARP_MARKER_MAX_FAILS) {
        LOG("create_d3d: marker fail_count=%u exceeds cap %u — WARP-only mode not helping, deleting marker",
            mk.warp_fail_count, WARP_MARKER_MAX_FAILS);
        DeleteFileW(PREFER_WARP_FLAG);
        prefer_warp = false;
    }

    // v0.9.428: Polaris HW-driver crash only reproduces on Win 11 25H2
    // (build 26200+).  On older Windows (Win 10 22H2 / Win 11 22H2 / 24H2)
    // the legacy AMD driver works fine and WARP causes visible ESP lag.
    // Only force WARP when we're on 25H2+ AND the GPU is Polaris.
    if (!prefer_warp && is_polaris_or_legacy_amd()) {
        typedef LONG(WINAPI *pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
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
    LOG("create_d3d: prefer_warp=%d marker_present=%d marker_valid=%d",
        prefer_warp ? 1 : 0, marker_present ? 1 : 0, marker_valid ? 1 : 0);

    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = E_FAIL;

    if (!prefer_warp) {
        // Drop a "we're about to try HW" marker with fresh timestamp + 0 fails.
        WarpMarker fresh{ WARP_MARKER_MAGIC, 0, filetime_now_100ns() };
        write_warp_marker(PREFER_WARP_FLAG, &fresh);
        mk = fresh;
        LOG("create_d3d: marker dropped (fresh, 16B); attempt HARDWARE + BGRA_SUPPORT");
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
            // HW worked — clear marker so next launch tries HW again.
            DeleteFileW(PREFER_WARP_FLAG);
            LOG("create_d3d: HW OK, marker cleared");
        }
    } else {
        LOG("create_d3d: SKIPPING HW attempt — marker still valid (fails=%u age=%llums)",
            mk.warp_fail_count,
            (unsigned long long)((filetime_now_100ns() - mk.created_ft) / 10000ULL));
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
            LOG("create_d3d: WARP crashed code=0x%08lx", GetExceptionCode());
            hr = E_FAIL;
        }

        if (SUCCEEDED(hr)) {
            // WARP succeeded — reset fail_count on the marker (but keep marker,
            // HW is still known-bad).  Only relevant if marker already existed.
            if (prefer_warp && mk.warp_fail_count > 0) {
                mk.warp_fail_count = 0;
                write_warp_marker(PREFER_WARP_FLAG, &mk);
                LOG("create_d3d: WARP OK, fail_count reset to 0");
            }
        } else if (prefer_warp) {
            // WARP failed WITH marker set → increment fail count.
            // On third failure the marker is auto-deleted next launch (checked above).
            mk.warp_fail_count++;
            write_warp_marker(PREFER_WARP_FLAG, &mk);
            LOG("create_d3d: WARP FAILED with marker set — fail_count now %u (cap %u; auto-delete triggers at cap+1 next launch)",
                mk.warp_fail_count, WARP_MARKER_MAX_FAILS);
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
    // No UAGame window discovered → hide overlay. Player should never see the
    // overlay while the game is not running (or minimized); the reader thread
    // stays alive underneath so first frame after game foreground is instant.
    if (!game_hw || !IsWindow(game_hw)) {
        // Keep the control panel visible if the operator explicitly focused it
        // (rare cold-config path). Otherwise hide.
        HWND fg = GetForegroundWindow();
        return (fg && fg == self_hw);
    }
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

// v1.0.29: reader status HUD. Field triage of v1.0.28 showed multiple
// hwids where the overlay stared idle for 30-90s during cold-attach (60-
// 177 RpmFindProcess attempts + canary bailouts + wrong-CR3 retry). No
// visible signal to the user meant "software broken → kill via task-mgr".
// This HUD keeps a small, always-visible panel top-right with the reader
// state so the user knows WHY there's no ESP yet and how far along we are.
// Hides itself when everything is nominal (LIVE + entities visible + Hz
// healthy) so it doesn't clutter the actual gameplay.
static void draw_reader_status_hud() {
    int   state = ah_reader_state();
    float hz    = ah_reader_hz();
    AH_LIVE_SNAP snap{};
    ah_reader_snapshot(&snap);

    // Silent when everything is working — user already sees the ESP.
    bool everything_ok = (state == AH_READER_LIVE &&
                          snap.ent_n > 0 && hz > 5.0f);
    if (everything_ok) return;

    const char* state_str = "?";
    ImU32 state_col = IM_COL32(255, 255, 255, 255);
    switch (state) {
        case AH_READER_INIT:          state_str = "INIT";                     state_col = IM_COL32(200,200,200,255); break;
        case AH_READER_PROVIDER_OK:   state_str = "DRIVER OK";                state_col = IM_COL32(200,200,255,255); break;
        case AH_READER_CR3_OK:        state_str = "CR3 OK";                   state_col = IM_COL32(200,200,255,255); break;
        case AH_READER_WAITING_GAME:  state_str = "WAITING FOR GAME";         state_col = IM_COL32(255,220,100,255); break;
        case AH_READER_ATTACHED:      state_str = "ATTACHED — LOADING WORLD"; state_col = IM_COL32(255,220,100,255); break;
        case AH_READER_LIVE:          state_str = "LIVE";                     state_col = IM_COL32(100,255,100,255); break;
        case AH_READER_GAME_GONE:     state_str = "GAME GONE";                state_col = IM_COL32(255,100,100,255); break;
        case AH_READER_PROVIDER_FAIL: state_str = "DRIVER LOAD FAILED";       state_col = IM_COL32(255, 60, 60,255); break;
        case AH_READER_CR3_FAIL:      state_str = "CR3 SCAN FAILED";          state_col = IM_COL32(255, 60, 60,255); break;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f),
                             ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##ah_reader_status_hud", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, state_col);
        ImGui::Text("[ESP] %s", state_str);
        ImGui::PopStyleColor();
        ImGui::Text("Hz: %.1f  ent: %d  loot: %d",
                    hz, snap.ent_n, snap.loot_n);

        const char* hint = nullptr;
        ImU32 hint_col = IM_COL32(200, 200, 200, 255);
        switch (state) {
            case AH_READER_WAITING_GAME:
                hint = "Waiting for Arena Breakout: Infinite to launch"; break;
            case AH_READER_ATTACHED:
                if (snap.ent_n == 0)
                    hint = "Attached — waiting for world (menu or raid load)";
                break;
            case AH_READER_GAME_GONE:
                hint = "Game closed — overlay will exit";
                hint_col = IM_COL32(255, 120, 120, 255);
                break;
            case AH_READER_PROVIDER_FAIL:
                hint = "Driver failed to load — check HVCI/Hyper-V, then relaunch";
                hint_col = IM_COL32(255, 120, 120, 255);
                break;
            case AH_READER_CR3_FAIL:
                hint = "System CR3 scan failed — kdu or Windows build mismatch";
                hint_col = IM_COL32(255, 120, 120, 255);
                break;
            case AH_READER_LIVE:
                if (snap.ent_n == 0)
                    hint = "In menu — no players to render yet";
                break;
            default: break;
        }
        if (hint) {
            ImGui::PushStyleColor(ImGuiCol_Text, hint_col);
            ImGui::TextWrapped("%s", hint);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

namespace abi {

void Overlay::run(const std::function<void()>& frame_fn) {
    MSG msg{};
    HWND game_hwnd = nullptr;
    // Start hidden — init() showed the window for DirectComposition setup,
    // now gate visibility on UAGame foreground ownership from the first tick.
    // Prevents the "overlay drawn on desktop with no game" flash at startup.
    bool overlay_visible = false;
    ShowWindowAsync(hwnd_, SW_HIDE);
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
            // v0.9.454: raised from 3 → 30 (30 s at 60fps window). ABI window
            // gets short-lived pseudo-invisibility during: raid load screens,
            // fullscreen<->borderless swap, resolution change, alt+tab back-
            // to-desktop. All those trip the old 3-probe gate and kill the
            // whole process, discarding kdu state — user has to relaunch by
            // hand. 30 probes tolerates every legit transition; only a truly
            // gone UAGame (game exited) sticks past that.
            if (game_miss_streak >= 30) {
                LOG("overlay::run: game window gone for 30 probes — self-exit");
                running_ = false;
                break;
            }
        }

        // v0.9.455: reader-driven game-gone watchdog. Steam-wrapper holds the
        // UAGame window for 30-60s after close (game_owns_foreground stays
        // true), so game_miss_streak alone never trips and overlay hangs empty.
        // The reader thread sets AH_READER_GAME_GONE once gworld=0 sustains
        // for ~15s — much more reliable than HWND polling.
        if (ah_reader_state() == AH_READER_GAME_GONE) {
            LOG("overlay::run: reader reports GAME_GONE (gworld=0 held) — self-exit");
            running_ = false;
            break;
        }

        // v0.9.455 reader-freeze watchdog. If reader Hz was > 0 (thread was
        // ticking) then drops to 0 for 5 s straight — the reader-thread is
        // wedged on a blocking RPM read (kdu IOCTL hang, procCR3 stale). Auto-
        // reattach: signals stop, waits for the thread, respawns. Prevents the
        // "overlay shows frozen snapshot forever" symptom on flaky kdu paths.
        {
            using namespace std::chrono;
            static auto s_last_hz_check   = steady_clock::now();
            static auto s_first_zero_at   = steady_clock::time_point{};
            static bool s_ever_ticking    = false;
            auto now_tp = steady_clock::now();
            if (duration_cast<milliseconds>(now_tp - s_last_hz_check).count() >= 1000) {
                s_last_hz_check = now_tp;
                float hz = ah_reader_hz();
                if (hz > 0.5f) {
                    s_ever_ticking  = true;
                    s_first_zero_at = steady_clock::time_point{};
                } else if (s_ever_ticking) {
                    if (s_first_zero_at.time_since_epoch().count() == 0) {
                        s_first_zero_at = now_tp;
                    } else if (duration_cast<seconds>(now_tp - s_first_zero_at).count() >= 5) {
                        LOG("overlay::run: reader Hz=0 for 5s (frozen) — triggering ah_reader_reattach");
                        ah_reader_reattach();
                        s_first_zero_at = steady_clock::time_point{};
                        s_ever_ticking  = false;
                    }
                }
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

        // v1.0.29: status HUD before the main frame — auto-hides once
        // reader hits LIVE with entities visible.
        draw_reader_status_hud();

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
                // v0.9.454: 3-strike gate before pulling the plug. A single TDR
                // (Timeout Detection Recovery) fires during resolution swap,
                // fullscreen enter/leave, or heavy combat frames — the driver
                // recovers within one frame on modern GPUs. Killing the whole
                // overlay on the first strike loses kdu state and forces a
                // launcher relaunch. Sleep 100 ms between strikes so the driver
                // has a chance to reset; only after 3 consecutive lost frames
                // (~300 ms with no recovery) do we accept the device is dead.
                int strike = g_present_lost_streak.fetch_add(1) + 1;
                LOG("overlay: Present DEVICE_LOST hr=0x%08lx removedReason=0x%08lx streak=%d",
                    present_hr, rr, strike);
                if (strike >= 3) {
                    // v1.0.29: DEVICE_LOST recovery. Field triage of v1.0.28
                    // showed bd31e3dd8e2a producing marker=OVERLAY_DEVICE_LOST
                    // repeatedly (5x). Previously we just wrote the marker and
                    // died — user had to relaunch the overlay by hand, kdu
                    // state was lost. On modern rigs a TDR / Alt+Tab GPU reset
                    // / Discord-Steam-overlay hook conflict is transient and
                    // resolvable by a fresh D3D device + swapchain. Rebuild
                    // once, then only if THAT fails do we surface the marker.
                    static int s_recreate_streak = 0;
                    s_recreate_streak++;
                    LOG("overlay: DEVICE_LOST strike=%d — attempting D3D recreate #%d",
                        strike, s_recreate_streak);

                    ImGui_ImplDX11_Shutdown();
                    cleanup_d3d();

                    if (create_d3d()) {
                        ImGui_ImplDX11_Init(d3d_device_, d3d_ctx_);
                        LOG("overlay: D3D recreate OK — resuming render loop (recovery #%d)",
                            s_recreate_streak);
                        g_present_lost_streak.store(0);
                        // Don't zero s_recreate_streak here — a rig that TDRs
                        // over and over would spin forever silently rebuilding.
                        // We cap at 3 recreate attempts total per process life.
                        if (s_recreate_streak >= 3) {
                            LOG("overlay: >=3 device recreates this session — next DEVICE_LOST will exit");
                        }
                        Sleep(100);
                        continue;
                    }

                    LOG("overlay: D3D recreate FAILED — device truly dead, writing marker + exit");
                    crash_marker::write(crash_marker::OVERLAY_DEVICE_LOST);
                    running_ = false;
                    break;
                }
                Sleep(100);
                continue;
            }
            // Non-fatal Present failures (OCCLUDED, STATUS_UNSUCCESSFUL): log
            // once and keep going — usually resolves within a frame.
            static bool logged = false;
            if (!logged) {
                LOG("overlay: Present returned 0x%08lx (non-fatal, will retry silently)", present_hr);
                logged = true;
            }
        } else {
            g_present_lost_streak.store(0);
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
