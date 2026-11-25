#pragma once

#include "windows/import.h"

#include <functional>
#include <mutex>

#include <d3d.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>  
#include <d3d11_4.h>
#include <wrl.h>


namespace thermion::windows::d3d {

class DLL_EXPORT D3DTexture {
  public:
    D3DTexture(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTexture2D,
        HANDLE d3dTexture2DHandle,
        uint32_t width,
        uint32_t height
    );    
    ~D3DTexture();

    void Flush();
    // The texture Flutter's compositor (ANGLE, its own D3D11 device) opens.
    // Only ever written by OUR D3D11 device (CopyResource).
    HANDLE GetTextureHandle();

    // The texture Vulkan imports and blits into. Never opened by ANGLE, so
    // no Vulkan work ever touches a resource the compositor samples.
    void SetVulkanSide(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, HANDLE handle) {
      _vulkanSideTexture = texture;
      _vulkanSideHandle = handle;
    }
    HANDLE GetVulkanImportHandle() { return _vulkanSideHandle; }
    ID3D11Texture2D *GetVulkanSideTexture() { return _vulkanSideTexture.Get(); }
    ID3D11Texture2D *GetPresentationTexture() { return _d3dTexture2D.Get(); }

    // Completion of the last presentation copy (reads the Vulkan side).
    Microsoft::WRL::ComPtr<ID3D11Query> copyDone;

    // CPU-readable copy of the presentation texture, created on first use by
    // the pixel-buffer presentation path (D3DContext::ReadbackRGBA).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
       
    void SaveToBMP(const char* filename);

    uint32_t GetWidth() {
      return _width;
    }

    uint32_t GetHeight() {
      return _height;
    }
      
  private:
    uint32_t _width = 0;
    uint32_t _height = 0;
    
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _d3dTexture2D;
    HANDLE _d3dTexture2DHandle = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _vulkanSideTexture;
    HANDLE _vulkanSideHandle = nullptr;

    bool SaveTextureAsBMP(ID3D11Texture2D* texture, const char* filename);

    
};

}
