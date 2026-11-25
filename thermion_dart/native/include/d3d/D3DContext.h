#pragma once

#include "windows/import.h"

#include <d3d.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3d11_4.h>

#include <wrl.h>
#include <wrl/client.h>

#include "d3d/D3DTexture.h"

namespace thermion::windows::d3d { 

    class DLL_EXPORT D3DContext {
        public:
            D3DContext(const uint8_t *vulkanLUID); 
            ~D3DContext();
            void Flush();
            std::unique_ptr<D3DTexture> CreateTexture(uint32_t width, uint32_t height);

            // Ends a D3D11_QUERY_EVENT on this device's immediate context and
            // flushes, so the query completes once every command this device
            // has issued so far has finished on the GPU. Null if the query
            // could not be created. Not thread-safe (immediate context):
            // callers serialise with every other use of this context.
            Microsoft::WRL::ComPtr<ID3D11Query> InsertEventQuery();

            // Non-blocking (D3D11_ASYNC_GETDATA_DONOTFLUSH). True once the
            // query has completed, for a null query, or when the device is
            // lost (nothing on it can still be pending).
            bool IsQueryDone(ID3D11Query *query);

            // Copies the Vulkan-side texture into the presentation texture on
            // this device and records `texture->copyDone`. Caller must know
            // Vulkan's write has completed (blit fence) and serialise with
            // every other use of this context.
            bool CopyForPresentation(D3DTexture *texture);

            // Polls `query` (flushing) for up to `timeoutMs`. True when done.
            bool WaitQuery(ID3D11Query *query, int timeoutMs);
            
            bool IsValid() {
                return _D3D11Device;
            }


        private:
            ID3D11DeviceContext* _D3D11DeviceContext = nullptr;
            ID3D11Device* _D3D11Device = nullptr;
            Microsoft::WRL::ComPtr<ID3D11Device5> _D3D11Device5;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext4> _D3D11DeviceContext4;
            Microsoft::WRL::ComPtr<ID3D11Fence> _sharedFence;


    };
}