#include "d3d/D3DContext.h"
#include <chrono>
#include <iostream>
#include <thread>

namespace thermion::windows::d3d
{

    D3DContext::D3DContext(const uint8_t *vulkanLUID)
    {
        IDXGIAdapter *adapter_ = nullptr;

        auto feature_levels = {
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
        };

        IDXGIFactory1 *dxgi = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgi);
        if (FAILED(hr))
        {
            std::cout << "Failed to create DXGI 1.1 factory" << std::endl;
            return;
        }

         // Find the adapter matching the Vulkan LUID passed above
        UINT i = 0;
        IDXGIAdapter* currAdapter = nullptr;
        bool found = false;

        while (dxgi->EnumAdapters(i, &currAdapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC desc;
            currAdapter->GetDesc(&desc);

            //std::wcout << "Checking D3D adapter LUID" << desc.AdapterLuid << std::endl;

            // Compare the 8 bytes of LUID
            // LUID structure in Windows is exactly 8 bytes (DWORD + LONG)
            if (memcmp(&desc.AdapterLuid, vulkanLUID, 8) == 0) {
                adapter_ = currAdapter;
                std::wcout << L"[INFO] Found matching D3D adapter: " << desc.Description << std::endl;
                found = true;
                break;
            }
            currAdapter->Release();
            i++;
        }
    
        dxgi->Release();

        
        if (!found || !adapter_) {
            std::cout << "[ERROR] Could not find D3D adapter matching Vulkan device LUID!" << std::endl;
            return;
        }

        Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(dxgi->QueryInterface(IID_PPV_ARGS(&factory2))))
        {
            std::cout << "DXGI 1.2 or higher supported" << std::endl;
        }

        DXGI_ADAPTER_DESC adapter_desc_;
        adapter_->GetDesc(&adapter_desc_);
        std::wcout << L"D3D adapter description: " << adapter_desc_.Description
                   << std::endl;

        hr = ::D3D11CreateDevice(
            // No driver worker threads on this device: the NVIDIA crash is a
            // race between threaded-driver consumers of a shared resource,
            // and this device only issues one small copy per frame.
            adapter_, D3D_DRIVER_TYPE_UNKNOWN, 0,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
            feature_levels.begin(),
            static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION,
            &_D3D11Device, 0, &_D3D11DeviceContext);

        if (FAILED(hr))
        {
            std::cout << "Failed to create D3D device" << std::endl;
            return;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device = nullptr;
        auto dxgi_device_success = _D3D11Device->QueryInterface(
            __uuidof(IDXGIDevice), (void **)&dxgi_device);
        if (SUCCEEDED(dxgi_device_success) && dxgi_device != nullptr)
        {
            dxgi_device->SetGPUThreadPriority(5); // Must be in interval [-7, 7].
        }

        auto level = _D3D11Device->GetFeatureLevel();
        std::cout << "Direct3D Feature Level: "
                  << (((unsigned)level) >> 12) << "_"
                  << ((((unsigned)level) >> 8) & 0xf) << std::endl;

        hr = _D3D11Device->QueryInterface(__uuidof(ID3D11Device5), (void**)&_D3D11Device5);

        if (FAILED(hr)) {
            std::cout << "Failed to query ID3D11Device5 (Windows 10 Creator's Update required)" << std::endl;
            return;
        }

        // Query for ID3D11DeviceContext4
        hr = _D3D11DeviceContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&_D3D11DeviceContext4);

        adapter_->Release();
    }

    D3DContext::~D3DContext() { 
        if (_D3D11DeviceContext) {
            _D3D11DeviceContext->Release();
        }
        if (_D3D11Device) {
            _D3D11Device->Release();
        }
        std::cerr << "D3DContext destroyed" << std::endl;
    }

    std::unique_ptr<D3DTexture> D3DContext::CreateTexture(uint32_t width, uint32_t height)
    {
        // Create texture
        auto d3d11_texture2D_desc = D3D11_TEXTURE2D_DESC{0};
        d3d11_texture2D_desc.Width = width;
        d3d11_texture2D_desc.Height = height;
        d3d11_texture2D_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d3d11_texture2D_desc.MipLevels = 1;
        d3d11_texture2D_desc.ArraySize = 1;
        d3d11_texture2D_desc.SampleDesc.Count = 1;
        d3d11_texture2D_desc.SampleDesc.Quality = 0;
        d3d11_texture2D_desc.Usage = D3D11_USAGE_DEFAULT;
        d3d11_texture2D_desc.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d3d11_texture2D_desc.CPUAccessFlags = 0;
        d3d11_texture2D_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTexture2D;
        
        auto hr = _D3D11Device->CreateTexture2D(&d3d11_texture2D_desc, nullptr, &d3dTexture2D);

        if FAILED (hr)
        {
            std::cout << "Failed to create D3D texture (" << hr << ")" << std::endl;
            return nullptr;
        }
        auto resource = Microsoft::WRL::ComPtr<IDXGIResource1>{};
        hr = d3dTexture2D.As(&resource);

        if FAILED (hr)
        {
            std::cout << "Failed to create D3D texture" << std::endl;
            return nullptr;
        }
        HANDLE d3dTexture2DHandle = nullptr;
        //hr = resource->GetSharedHandle(&d3dTexture2DHandle);
        hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &d3dTexture2DHandle);
        if FAILED (hr)
        {
            std::cout << "Failed to get shared handle to external D3D texture" << std::endl;
            return nullptr;
        }

        d3dTexture2D->AddRef();

        Flush();
        
        std::cout << "Created external D3D texture " << width << "x" << height << std::endl;
        auto texture =  std::make_unique<D3DTexture>(d3dTexture2D, d3dTexture2DHandle, width, height);

        ID3D11RenderTargetView* rtv = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;

        hr = _D3D11Device->CreateRenderTargetView(d3dTexture2D.Get(), &rtvDesc, &rtv);
        if (FAILED(hr)) {
            std::cout << "Failed to create render target view" << std::endl;
            return std::nullptr_t();
        }

        float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        _D3D11DeviceContext->ClearRenderTargetView(rtv, black);
        rtv->Release();

        // Second texture, same description, for Vulkan to import and write.
        // Vulkan and ANGLE never share a resource: Vulkan -> (this) -> our
        // D3D11 CopyResource -> presentation texture -> ANGLE.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> vulkanSide;
        hr = _D3D11Device->CreateTexture2D(&d3d11_texture2D_desc, nullptr, &vulkanSide);
        if (FAILED(hr)) {
            std::cout << "Failed to create Vulkan-side D3D texture (" << hr << ")" << std::endl;
            return nullptr;
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> vulkanSideResource;
        HANDLE vulkanSideHandle = nullptr;
        if (FAILED(vulkanSide.As(&vulkanSideResource)) ||
            FAILED(vulkanSideResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                          &vulkanSideHandle))) {
            std::cout << "Failed to share Vulkan-side D3D texture" << std::endl;
            return nullptr;
        }
        texture->SetVulkanSide(vulkanSide, vulkanSideHandle);

        Flush();
        return texture;
    }

    bool D3DContext::CopyForPresentation(D3DTexture *texture)
    {
        if (!texture || !texture->GetVulkanSideTexture()) {
            return false;
        }
        _D3D11DeviceContext->CopyResource(texture->GetPresentationTexture(),
                                          texture->GetVulkanSideTexture());
        texture->copyDone = InsertEventQuery();
        return texture->copyDone != nullptr;
    }

    bool D3DContext::ReadbackRGBA(D3DTexture *texture, std::vector<uint8_t> &rgba, int timeoutMs)
    {
        auto *source = texture ? texture->GetPresentationTexture() : nullptr;
        if (!source) {
            return false;
        }
        if (!texture->staging) {
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            if (FAILED(_D3D11Device->CreateTexture2D(&desc, nullptr, &texture->staging))) {
                std::cerr << "D3DContext: staging texture creation failed" << std::endl;
                return false;
            }
        }
        _D3D11DeviceContext->CopyResource(texture->staging.Get(), source);
        auto query = InsertEventQuery();
        if (!WaitQuery(query.Get(), timeoutMs)) {
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(_D3D11DeviceContext->Map(texture->staging.Get(), 0, D3D11_MAP_READ,
                                            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            return false;
        }
        const uint32_t width = texture->GetWidth();
        const uint32_t height = texture->GetHeight();
        rgba.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t *src = static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t *dst = rgba.data() + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; x++) {
                // BGRA -> RGBA
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
        _D3D11DeviceContext->Unmap(texture->staging.Get(), 0);
        return true;
    }

    bool D3DContext::WaitQuery(ID3D11Query *query, int timeoutMs)
    {
        if (!query) {
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (true) {
            BOOL done = FALSE;
            const HRESULT hr = _D3D11DeviceContext->GetData(query, &done, sizeof(done), 0);
            if (hr == S_OK) {
                return done == TRUE;
            }
            if (FAILED(hr)) {
                return true; // device lost: nothing can still be pending
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
    }

    void D3DContext::Flush()
    {
        _D3D11DeviceContext->Flush();
    }

    Microsoft::WRL::ComPtr<ID3D11Query> D3DContext::InsertEventQuery()
    {
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        if (!_D3D11Device || !_D3D11DeviceContext) {
            return query;
        }
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_EVENT;
        if (FAILED(_D3D11Device->CreateQuery(&desc, &query))) {
            std::cerr << "D3DContext: CreateQuery(EVENT) failed" << std::endl;
            return nullptr;
        }
        _D3D11DeviceContext->End(query.Get());
        _D3D11DeviceContext->Flush();
        return query;
    }

    bool D3DContext::IsQueryDone(ID3D11Query *query)
    {
        if (!query || !_D3D11DeviceContext) {
            return true;
        }
        BOOL done = FALSE;
        const HRESULT hr = _D3D11DeviceContext->GetData(
            query, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) {
            return false;
        }
        if (FAILED(hr)) {
            // Device removed/reset: no work on this device can still run.
            return true;
        }
        return done == TRUE;
    }


}