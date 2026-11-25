#pragma once

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>
#include <flutter_texture_registrar.h>

#include "flutter_d3d_texture.h"

namespace thermion::tflutter::windows
{

  FlutterD3DTexture::FlutterD3DTexture(HANDLE d3dTexture2DHandle, uint32_t width, uint32_t height,
                                       bool pixelBuffer)
      : _width(width), _height(height), _pixelBufferMode(pixelBuffer)
  {
    _textureDescriptor = std::make_unique<FlutterDesktopGpuSurfaceDescriptor>();
    _textureDescriptor->struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    _textureDescriptor->handle = d3dTexture2DHandle;
    _textureDescriptor->width = _textureDescriptor->visible_width = width;
    _textureDescriptor->height = _textureDescriptor->visible_height = height;
    _textureDescriptor->release_context = nullptr;
    _textureDescriptor->release_callback = [](void *release_context) {

    };
    _textureDescriptor->format = kFlutterDesktopPixelFormatBGRA8888;

    if (_pixelBufferMode) {
      _texture = std::make_unique<::flutter::TextureVariant>(::flutter::PixelBufferTexture(
          [this](size_t, size_t) { return AcquirePixels(); }));
      return;
    }

    _texture =
        std::make_unique<::flutter::TextureVariant>(::flutter::GpuSurfaceTexture::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [&](size_t width, size_t height)
            {
              std::lock_guard<std::mutex> lock(_descriptorMutex);
              _fetchCount.fetch_add(1);
              return _textureDescriptor.get();
            }));
  }

  uint64_t FlutterD3DTexture::GetFetchCount() {
    return _fetchCount.load();
  }

  // Raster thread. The lock is released by ReleasePixels, which Flutter calls
  // on the same thread once glTexImage2D has consumed the buffer.
  const FlutterDesktopPixelBuffer *FlutterD3DTexture::AcquirePixels() {
    _pixelMutex.lock();
    _fetchCount.fetch_add(1);
    if (_pixels.empty()) {
      _pixelMutex.unlock();
      return nullptr;
    }
    _pixelBuffer.buffer = _pixels.data();
    _pixelBuffer.width = _pixelBuffer.width ? _pixelBuffer.width : _width;
    _pixelBuffer.height = _pixelBuffer.height ? _pixelBuffer.height : _height;
    _pixelBuffer.release_callback = &FlutterD3DTexture::ReleasePixels;
    _pixelBuffer.release_context = this;
    return &_pixelBuffer;
  }

  void FlutterD3DTexture::ReleasePixels(void *context) {
    static_cast<FlutterD3DTexture *>(context)->_pixelMutex.unlock();
  }

  void FlutterD3DTexture::CommitScratch(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(_pixelMutex);
    _pixels.swap(_scratch);
    _pixelBuffer.width = width;
    _pixelBuffer.height = height;
  }

  FlutterD3DTexture::~FlutterD3DTexture() {

  }
  
  ::flutter::TextureVariant* FlutterD3DTexture::GetFlutterTexture() {
      return _texture.get();
  }

  HANDLE FlutterD3DTexture::GetD3DTextureHandle() {
    std::lock_guard<std::mutex> lock(_descriptorMutex);
    return _textureDescriptor->handle;
  }

  void FlutterD3DTexture::SwapDescriptor(HANDLE newHandle, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(_descriptorMutex);
    _textureDescriptor->handle = newHandle;
    _textureDescriptor->width = _textureDescriptor->visible_width = width;
    _textureDescriptor->height = _textureDescriptor->visible_height = height;
    _width = width;
    _height = height;
  }

  void FlutterD3DTexture::SetFlutterTextureId(int64_t textureId) {
    _flutterTextureId = textureId;
  }

  ::flutter::TextureRegistrar* FlutterD3DTexture::GetOwner() {
    return _owner;
  }

  void FlutterD3DTexture::SetOwner(::flutter::TextureRegistrar* owner) {
    _owner = owner;
  }

  int64_t FlutterD3DTexture::GetFlutterTextureId()
  {
    return _flutterTextureId;
  }

}
