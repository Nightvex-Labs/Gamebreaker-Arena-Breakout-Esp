// WIC → D3D11 texture loader (see header).
//
// Uses IWICImagingFactory to decode the file and convert to 32bppPBGRA,
// which is the exact format ImGui expects when we hand an SRV back through
// AddImage(). Nothing exotic: OpenFile → GetFrame(0) → Convert → CopyPixels
// → CreateTexture2D + CreateShaderResourceView. If any step fails we clean
// up and return false so the caller can fall back to a placeholder.

#include "image_loader.hpp"

#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace abi::image_loader {

static IWICImagingFactory* g_wic = nullptr;

static bool ensure_wic() {
    if (g_wic) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Already-initialised (S_FALSE / RPC_E_CHANGED_MODE) is fine.
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) return false;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    return SUCCEEDED(hr) && g_wic != nullptr;
}

bool load_png(ID3D11Device* device, const wchar_t* path,
              ID3D11ShaderResourceView** out_srv,
              int* out_w, int* out_h)
{
    if (out_srv) *out_srv = nullptr;
    if (out_w)   *out_w = 0;
    if (out_h)   *out_h = 0;
    if (!device || !path || !ensure_wic()) return false;

    IWICBitmapDecoder*     dec = nullptr;
    IWICBitmapFrameDecode* frm = nullptr;
    IWICFormatConverter*   cnv = nullptr;
    ID3D11Texture2D*       tex = nullptr;
    bool ok = false;

    do {
        if (FAILED(g_wic->CreateDecoderFromFilename(
                path, nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frm))) break;
        if (FAILED(g_wic->CreateFormatConverter(&cnv))) break;
        if (FAILED(cnv->Initialize(frm, GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) break;

        UINT w = 0, h = 0;
        if (FAILED(cnv->GetSize(&w, &h)) || w == 0 || h == 0) break;

        const UINT stride = w * 4;
        std::vector<unsigned char> pixels((size_t)stride * h);
        if (FAILED(cnv->CopyPixels(nullptr, stride,
                                    (UINT)pixels.size(), pixels.data()))) break;

        D3D11_TEXTURE2D_DESC td{};
        td.Width          = w;
        td.Height         = h;
        td.MipLevels      = 1;
        td.ArraySize      = 1;
        td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage          = D3D11_USAGE_DEFAULT;
        td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ pixels.data(), stride, 0 };
        if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) break;
        if (FAILED(device->CreateShaderResourceView(tex, nullptr, out_srv))) break;

        if (out_w) *out_w = (int)w;
        if (out_h) *out_h = (int)h;
        ok = true;
    } while (0);

    if (tex) tex->Release();
    if (cnv) cnv->Release();
    if (frm) frm->Release();
    if (dec) dec->Release();
    return ok;
}

// Memory-buffer overload. Uses IWICStream::InitializeFromMemory so the same
// decode/convert/upload pipeline can consume an embedded byte array with no
// filesystem I/O. WIC copies the header on decoder-create but expects the
// caller-owned buffer to stay alive across CopyPixels; the const_cast on
// `data` is safe — WIC only reads from the stream.
bool load_png(ID3D11Device* device, const void* data, size_t size,
              ID3D11ShaderResourceView** out_srv,
              int* out_w, int* out_h)
{
    if (out_srv) *out_srv = nullptr;
    if (out_w)   *out_w = 0;
    if (out_h)   *out_h = 0;
    if (!device || !data || size == 0 || !ensure_wic()) return false;

    IWICStream*            strm = nullptr;
    IWICBitmapDecoder*     dec  = nullptr;
    IWICBitmapFrameDecode* frm  = nullptr;
    IWICFormatConverter*   cnv  = nullptr;
    ID3D11Texture2D*       tex  = nullptr;
    bool ok = false;

    do {
        if (FAILED(g_wic->CreateStream(&strm))) break;
        if (FAILED(strm->InitializeFromMemory(
                static_cast<BYTE*>(const_cast<void*>(data)),
                static_cast<DWORD>(size)))) break;
        if (FAILED(g_wic->CreateDecoderFromStream(
                strm, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frm))) break;
        if (FAILED(g_wic->CreateFormatConverter(&cnv))) break;
        if (FAILED(cnv->Initialize(frm, GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) break;

        UINT w = 0, h = 0;
        if (FAILED(cnv->GetSize(&w, &h)) || w == 0 || h == 0) break;

        const UINT stride = w * 4;
        std::vector<unsigned char> pixels((size_t)stride * h);
        if (FAILED(cnv->CopyPixels(nullptr, stride,
                                    (UINT)pixels.size(), pixels.data()))) break;

        D3D11_TEXTURE2D_DESC td{};
        td.Width          = w;
        td.Height         = h;
        td.MipLevels      = 1;
        td.ArraySize      = 1;
        td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage          = D3D11_USAGE_DEFAULT;
        td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ pixels.data(), stride, 0 };
        if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) break;
        if (FAILED(device->CreateShaderResourceView(tex, nullptr, out_srv))) break;

        if (out_w) *out_w = (int)w;
        if (out_h) *out_h = (int)h;
        ok = true;
    } while (0);

    if (tex)  tex->Release();
    if (cnv)  cnv->Release();
    if (frm)  frm->Release();
    if (dec)  dec->Release();
    if (strm) strm->Release();
    return ok;
}

} // namespace abi::image_loader
