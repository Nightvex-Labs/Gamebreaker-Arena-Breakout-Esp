// Load a PNG/JPG file into a D3D11 shader-resource view using WIC
// (Windows Imaging Component — ships with every desktop Windows install,
// no third-party image library needed). Only used by abi_preview.exe to
// feed the ESP-Preview panel a real operator.png.

#pragma once

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace abi::image_loader {

// Load `path` into a fresh D3D11 texture + SRV. Returns true on success and
// fills *out_srv (caller keeps the ref) and pixel dimensions. Caller must
// Release the SRV eventually. Failure paths leave *out_srv = nullptr.
bool load_png(ID3D11Device* device, const wchar_t* path,
              ID3D11ShaderResourceView** out_srv,
              int* out_w, int* out_h);

// Memory-buffer variant: decodes `data`/`size` (PNG/JPG/BMP — any WIC-
// supported format). Removes the runtime file dependency so an image can be
// embedded into the PE and used without a sidecar file on disk.
bool load_png(ID3D11Device* device, const void* data, size_t size,
              ID3D11ShaderResourceView** out_srv,
              int* out_w, int* out_h);

}
