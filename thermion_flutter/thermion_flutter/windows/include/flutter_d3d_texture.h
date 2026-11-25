#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <flutter/texture_registrar.h>
#include <flutter_texture_registrar.h>

namespace thermion::tflutter::windows {

    class FlutterD3DTexture {
        public:
            // `pixelBuffer`: register a CPU pixel-buffer texture instead of a
            // DXGI shared handle, so Flutter's ANGLE device never opens a
            // texture shared with another device (experiment, see plugin).
            FlutterD3DTexture(HANDLE d3dTexture2DHandle, uint32_t width, uint32_t height,
                              bool pixelBuffer = false);
            ~FlutterD3DTexture();
            ::flutter::TextureVariant* GetFlutterTexture();
            HANDLE GetD3DTextureHandle();
            int64_t GetFlutterTextureId();
            void SetFlutterTextureId(int64_t textureId);

            // The registrar that registered this texture. Texture records are
            // process-global while registrars are per-engine, and Flutter texture
            // IDs are pointer-derived and therefore recyclable — so an ID alone
            // does not identify a texture. Lookups must match owner AND ID.
            ::flutter::TextureRegistrar* GetOwner();
            void SetOwner(::flutter::TextureRegistrar* owner);

            // Atomically swap the underlying D3D shared handle.
            // Flutter's GpuSurfaceTexture callback returns the descriptor
            // pointer each frame; after this call it will see the new handle.
            void SwapDescriptor(HANDLE newHandle, uint32_t width, uint32_t height);

            // How many times Flutter's raster thread has fetched the
            // descriptor. Incremented under the descriptor lock, so a fetch
            // counted after a SwapDescriptor is guaranteed to see the new
            // handle (and ANGLE then drops its surface for the old one).
            uint64_t GetFetchCount();

            // Pixel-buffer mode only. The blit worker fills Scratch() (it
            // is the only writer) and CommitScratch() publishes it.
            bool IsPixelBuffer() const { return _pixelBufferMode; }
            std::vector<uint8_t> &Scratch() { return _scratch; }
            void CommitScratch(uint32_t width, uint32_t height);
        private:
            const FlutterDesktopPixelBuffer *AcquirePixels();
            static void ReleasePixels(void *context);
            bool _pixelBufferMode = false;
            std::mutex _pixelMutex;  // held from AcquirePixels to ReleasePixels
            std::vector<uint8_t> _pixels;
            std::vector<uint8_t> _scratch;
            FlutterDesktopPixelBuffer _pixelBuffer{};
            uint32_t _width;
            uint32_t _height;
            std::unique_ptr<FlutterDesktopGpuSurfaceDescriptor> _textureDescriptor = nullptr;
            std::unique_ptr<::flutter::TextureVariant> _texture;
            int64_t _flutterTextureId = -1;
            ::flutter::TextureRegistrar* _owner = nullptr;
            HANDLE _d3dTexture2DHandle;
            std::mutex _descriptorMutex;
            std::atomic<uint64_t> _fetchCount{0};
    };
}

