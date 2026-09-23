#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <functional>
#include <atomic>

namespace abi {

class Overlay {
public:
    Overlay();
    ~Overlay();

    bool init(int screen_w, int screen_h);
    void shutdown();

    // Drives the message loop; calls `frame_fn` once per frame.
    void run(const std::function<void()>& frame_fn);

    HWND                  hwnd()     const { return hwnd_; }
    // Dynamically toggle mouse input capture (for control panel).
    void                  set_input_capture(bool on);
    int                   width()    const { return sw_; }
    int                   height()   const { return sh_; }
    ID3D11Device*         device()         { return d3d_device_; }
    ID3D11DeviceContext*  context()        { return d3d_ctx_; }
    IDXGISwapChain1*      swapchain()      { return swapchain_; }

private:
    bool create_window();
    bool create_d3d();
    void cleanup_d3d();
    void create_rtv();
    void cleanup_rtv();
    void protect_from_capture();

public:
    // Toggle screen-capture exclusion at runtime. Off = screenshots/OBS can
    // see the overlay (for making UI captures); on = anti-cheat / recorders
    // cannot capture it.
    void set_capture_protection(bool on);

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    HWND                       hwnd_{nullptr};
    int                        sw_{0}, sh_{0};
    std::atomic<bool>          input_capture_{false};
    ID3D11Device*              d3d_device_{nullptr};
    ID3D11DeviceContext*       d3d_ctx_{nullptr};
    IDXGISwapChain1*           swapchain_{nullptr};
    ID3D11RenderTargetView*    rtv_{nullptr};
    IDCompositionDevice*       dcomp_dev_{nullptr};
    IDCompositionTarget*       dcomp_tgt_{nullptr};
    IDCompositionVisual*       dcomp_vis_{nullptr};
    std::atomic<bool>          running_{true};

    static Overlay* s_instance;
};

}  // namespace abi
