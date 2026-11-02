#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <stdlib.h>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <unistd.h>
#include <mimalloc.h>
#endif

#include <filament/LightManager.h>

#include "c_api/APIBoundaryTypes.h"
#include "c_api/TAnimationManager.h"
#include "c_api/TEngine.h"
#include "c_api/TRenderableManager.h"
#include "c_api/TTransformManager.h"
#include "c_api/TGizmo.h"
#include "c_api/TGltfAssetLoader.h"
#include "c_api/TGltfResourceLoader.h"
#include "c_api/TRenderer.h"
#include "c_api/TRenderManager.h"
#include "c_api/TRenderTarget.h"
#include "c_api/TScene.h"
#include "c_api/TSceneAsset.h"
#include "c_api/TFilamentAsset.h"
#include "c_api/TTexture.h"
#include "c_api/TView.h"
#include "c_api/TVertexBuffer.h"
#include "c_api/TIndexBuffer.h"
#include "c_api/ThermionDartRenderThreadApi.h"

#include "rendering/RenderThread.hpp"
#include "rendering/RenderManager.hpp"
#include "Log.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/proxying.h>
#include <emscripten/eventloop.h>
#include <emscripten/console.h>
#endif

using namespace thermion;
using namespace std::chrono_literals;
#include <time.h>
#include <cinttypes>

#if defined __EMSCRIPTEN__
#define PROXY(call)                                           \
  auto startTime = std::chrono::high_resolution_clock::now(); \
  TRACE("PROXYING");                                          \
  _renderThread->queue.proxySync(_renderThread->outer, [=]() { call; });
#else
#define PROXY(call) call
#endif
extern "C"
{

  std::unique_ptr<RenderThread> _renderThread;

  // Two-engine support: the render thread is a process global that may be
  // shared by multiple Dart isolates (the primary window and popout windows
  // each run their own engine). Acquire/release discipline:
  // RenderThread_create reuses a live thread and bumps the refcount;
  // RenderThread_destroy only destroys once the last reference is released.
  // A reference leaked by a dead isolate (hot restart) deliberately keeps the
  // thread alive for reuse — tearing down a thread another live engine still
  // renders on is the strictly worse failure.
  //
  // Scope of the lock: create/destroy only. The many other entry points in
  // this file deref the bare _renderThread unlocked, exactly as before this
  // change; refcounting narrows (not widens) their exposure, because the
  // thread is now only ever destroyed after every isolate has released its
  // reference — an isolate queueing tasks after releasing is a caller bug
  // that was equally fatal under the old single-owner model.
  //
  // PRECONDITION the narrowing argument rests on: create/destroy calls are
  // BALANCED per owner — release is an owner's last touch of the thread. An
  // unbalanced extra destroy releases a reference someone else still holds
  // and re-opens the use-after-free. The Dart side enforces this with a
  // reentrancy latch in FFIFilamentApp.destroy().
  // Function-local lazy static (never destroyed): nothing runs at image
  // load — a namespace-scope std::mutex here fast-failed the DEBUG Windows
  // runner at process init (0xC0000409 before wWinMain).
  static std::mutex& _renderThreadMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
  }
  static int _renderThreadRefs = 0;

  EMSCRIPTEN_KEEPALIVE void* RenderThread_create()
  {
    TRACE("RenderThread_create");
    std::lock_guard<std::mutex> lock(_renderThreadMutex());
    if (_renderThread)
    {
      _renderThreadRefs++;
      Log("RenderThread_create: reusing live render thread (refs=%d).",
          _renderThreadRefs);
      return _renderThread.get();
    }
    _renderThread = std::make_unique<RenderThread>();
    _renderThreadRefs = 1;
    TRACE("RenderThread created");
    return _renderThread.get();
  }

  EMSCRIPTEN_KEEPALIVE void RenderThread_destroy()
  {
    TRACE("RenderThread_destroy");
    std::lock_guard<std::mutex> lock(_renderThreadMutex());
    if (!_renderThread)
    {
      _renderThreadRefs = 0;
      return;
    }
    if (_renderThreadRefs > 1)
    {
      _renderThreadRefs--;
      Log("RenderThread_destroy: %d reference(s) remain; render thread stays "
          "alive.",
          _renderThreadRefs);
      return;
    }
    _renderThreadRefs = 0;
    _renderThread = nullptr;
  }

  EMSCRIPTEN_KEEPALIVE void RenderThread_addTask(void (*task)())
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          task();
        });
    auto fut = _renderThread->addTask(lambda, "RenderThread_addTask");
  }

  EMSCRIPTEN_KEEPALIVE void RenderManager_attachToRenderThread(TRenderManager *tRenderManager)
  {
#ifdef __EMSCRIPTEN__
    if (!_renderThread) {
      Log("WARNING - RenderManager_attachToRenderThread called with no RenderThread active.");
      return;
    }
    auto *rm = reinterpret_cast<RenderManager *>(tRenderManager);
    _renderThread->setRenderManager(rm);
#else
    (void)tRenderManager; // no-op on native
#endif
  }

  // Inverse of RenderManager_attachToRenderThread. Call before deleting a
  // RenderManager so the worker's iter() doesn't dereference a freed pointer
  // on its next tick.
  EMSCRIPTEN_KEEPALIVE void RenderManager_detachFromRenderThread()
  {
#ifdef __EMSCRIPTEN__
    if (_renderThread) {
      _renderThread->setRenderManager(nullptr);
    }
#endif
  }

  EMSCRIPTEN_KEEPALIVE void RenderManager_setRenderableRenderThread(
    TRenderManager *tRenderer,
    TSwapChain *tSwapChain,
    TView **tViews,
    uint8_t numViews,
    uint32_t requestId, VoidCallback onComplete) {

      std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderManager_setRenderable(tRenderer, tSwapChain, tViews, numViews);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderManager_setRenderable");

    }

  EMSCRIPTEN_KEEPALIVE void RenderManager_renderRenderThread(
    TRenderManager *tRenderManager,
    uint64_t frameTimeInNanos,
    uint32_t requestId,
    VoidCallback onComplete)
  {
    auto queuedAt = std::chrono::steady_clock::now();
    _renderThread->addDetachedTask([=]() mutable {
      auto startedAt = std::chrono::steady_clock::now();
      float queueMs = std::chrono::duration_cast<std::chrono::nanoseconds>(startedAt - queuedAt).count() / 1e6f;
      RenderManager_render(tRenderManager, frameTimeInNanos);
      static int cbCount = 0;
      cbCount++;
      if (queueMs > 5.0f) {
        TRACE("[QUEUE] render waited %.1fms in queue\n", queueMs);
      }
      if (cbCount <= 3 || cbCount % 300 == 0) {
        TRACE("[RenderCB] #%d completing requestId=%u queueWait=%.1fms\n", cbCount, requestId, queueMs);
      }
      PROXY(onComplete(requestId));
    }, "RenderManager_render");
  }

  EMSCRIPTEN_KEEPALIVE void RenderManager_addAnimationManagerRenderThread(
    TRenderManager *tRenderManager,
    TAnimationManager *tAnimationManager,
    uint32_t requestId,
    VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderManager_addAnimationManager(tRenderManager, tAnimationManager);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderManager_addAnimationManager");
  }

  EMSCRIPTEN_KEEPALIVE void RenderManager_removeAnimationManagerRenderThread(
    TRenderManager *tRenderManager,
    TAnimationManager *tAnimationManager,
    uint32_t requestId,
    VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderManager_removeAnimationManager(tRenderManager, tAnimationManager);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderManager_removeAnimationManager");
  }

  EMSCRIPTEN_KEEPALIVE void RenderManager_removeSwapChainRenderThread(
    TRenderManager *tRenderManager,
    TSwapChain *tSwapChain,
    uint32_t requestId,
    VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderManager_removeSwapChain(tRenderManager, tSwapChain);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderManager_removeSwapChain");
  }

  // Engine creation vs. window/engine teardown.
  //
  // The render thread is shared by every engine in the process, so a second
  // engine's Engine_create runs while the FIRST engine's window may be
  // closing (a popout watchdog firing mid-boot did exactly this and aborted a
  // plugin worker thread). Teardown paths take this lock through
  // EngineLifecycle_lock/unlock so a half-built engine can never be torn down
  // underneath its own creation.
  //
  // timed_mutex, not mutex: a create that never returns must not make a
  // window unclosable. Teardown proceeds with a warning after the budget.
  // Function-local lazy static for the same reason as _renderThreadMutex().
#ifndef __EMSCRIPTEN__
  static std::timed_mutex& _engineLifecycleMutex() {
    static std::timed_mutex* m = new std::timed_mutex();
    return *m;
  }
#endif

  // Blocks until no engine creation is in flight, or until the budget
  // expires. Returns true if the lock was acquired (caller must unlock).
  EMSCRIPTEN_KEEPALIVE bool EngineLifecycle_lock(int timeoutMs)
  {
#ifndef __EMSCRIPTEN__
    if (_engineLifecycleMutex().try_lock_for(std::chrono::milliseconds(timeoutMs)))
    {
      return true;
    }
    Log("EngineLifecycle_lock: engine creation still in flight after %d ms; "
        "proceeding with teardown unsynchronised.",
        timeoutMs);
    return false;
#else
    (void)timeoutMs;
    return false;
#endif
  }

  EMSCRIPTEN_KEEPALIVE void EngineLifecycle_unlock()
  {
#ifndef __EMSCRIPTEN__
    _engineLifecycleMutex().unlock();
#endif
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createRenderThread(
      TBackend backend,
      void *platform,
      void *sharedContext,
      uint8_t stereoscopicEyeCount,
      bool disableHandleUseAfterFreeCheck,
      void (*onComplete)(TEngine *))
  {

    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Log("Engine_createRenderThread: task started (platform=%p, "
              "sharedContext=%p).",
              platform, sharedContext);
          auto startedAt = std::chrono::steady_clock::now();
#ifndef __EMSCRIPTEN__
          std::lock_guard<std::timed_mutex> lifecycle(_engineLifecycleMutex());
#endif
          auto *engine = Engine_create(backend, platform, sharedContext, stereoscopicEyeCount, disableHandleUseAfterFreeCheck);
          auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startedAt)
                               .count();
          Log("Engine_createRenderThread: Engine_create returned %p after "
              "%lld ms.",
              (void *)engine, (long long)elapsedMs);
          PROXY(onComplete(engine));
        });
    Log("Engine_createRenderThread: queued (%zu task(s) ahead of it).",
        _renderThread->pendingTaskCount());
    auto fut = _renderThread->addTask(lambda, "Engine_create");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createRendererRenderThread(TEngine *tEngine, void (*onComplete)(TRenderer *))
  {

    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *renderer = Engine_createRenderer(tEngine);
          PROXY(onComplete(renderer));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_createRenderer");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createSwapChainRenderThread(TEngine *tEngine, void *window, uint64_t flags, void (*onComplete)(TSwapChain *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto swapChain = Engine_createSwapChain(tEngine, window, flags);
          PROXY(onComplete(swapChain));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_createSwapChain");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createHeadlessSwapChainRenderThread(TEngine *tEngine, uint32_t width, uint32_t height, uint64_t flags, void (*onComplete)(TSwapChain *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto swapChain = Engine_createHeadlessSwapChain(tEngine, width, height, flags);
          PROXY(onComplete(swapChain));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_createHeadlessSwapChain");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroySwapChainRenderThread(TEngine *tEngine, TSwapChain *tSwapChain, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroySwapChain(tEngine, tSwapChain);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroySwapChain");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyRendererRenderThread(TEngine *tEngine, TRenderer *tRenderer, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyRenderer(tEngine, tRenderer);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyRenderer");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyViewRenderThread(TEngine *tEngine, TView *tView, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyView(tEngine, tView);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyView");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroySceneRenderThread(TEngine *tEngine, TScene *tScene, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyScene(tEngine, tScene);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyScene");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createCameraRenderThread(TEngine *tEngine, EntityId entityId, void (*onComplete)(TCamera *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto camera = Engine_createCamera(tEngine, entityId);
          PROXY(onComplete(camera));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_createCamera");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyCameraRenderThread(TEngine *tEngine, TCamera *tCamera, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyCamera(tEngine, tCamera);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyCamera");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createViewRenderThread(TEngine *tEngine, void (*onComplete)(TView *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *view = Engine_createView(tEngine);
          PROXY(onComplete(view));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_createView");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyRenderThread(TEngine *tEngine, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroy(tEngine);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyTextureRenderThread(TEngine *engine, TTexture *tTexture, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyTexture(engine, tTexture);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyTexture");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroySkyboxRenderThread(TEngine *tEngine, TSkybox *tSkybox, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroySkybox(tEngine, tSkybox);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroySkybox");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyIndirectLightRenderThread(TEngine *tEngine, TIndirectLight *tIndirectLight, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyIndirectLight(tEngine, tIndirectLight);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyIndirectLight");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_buildMaterialRenderThread(TEngine *tEngine, const uint8_t *materialData, size_t length, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto material = Engine_buildMaterial(tEngine, materialData, length);
          PROXY(onComplete(material));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_buildMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyMaterialRenderThread(TEngine *tEngine, TMaterial *tMaterial, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyMaterial(tEngine, tMaterial);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyMaterialInstanceRenderThread(TEngine *tEngine, TMaterialInstance *tMaterialInstance, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyMaterialInstance(tEngine, tMaterialInstance);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyMaterialInstance");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_createFenceRenderThread(TEngine *tEngine, void (*onComplete)(TFence *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *fence = Engine_createFence(tEngine);
          PROXY(onComplete(fence));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_createFence");
  }

  EMSCRIPTEN_KEEPALIVE void Fence_waitAndDestroyRenderThread(TFence *tFence, uint32_t requestId, VoidCallback onComplete)
  {
    
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Fence_waitAndDestroy(tFence);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Fence_waitAndDestroy");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyFenceRenderThread(TEngine *tEngine, TFence *tFence, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_destroyFence(tEngine, tFence);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyFence");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_flushAndWaitRenderThread(TEngine *tEngine, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_flushAndWait(tEngine);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_flushAndWait");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_executeRenderThread(TEngine *tEngine, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Engine_execute(tEngine);
          std::packaged_task<void()> callback(
          [=]() mutable
          { 
            PROXY(onComplete(requestId));
          });
          _renderThread->addTask(callback, "Engine_execute.callback");
        });
    auto fut = _renderThread->addTask(lambda, "Engine_execute");
  }

  EMSCRIPTEN_KEEPALIVE void execute_queue()
  {
#ifdef __EMSCRIPTEN__
    _renderThread->queue.execute();
#endif
  }

  EMSCRIPTEN_KEEPALIVE void Engine_buildSkyboxRenderThread(TEngine *tEngine, TTexture *tTexture, void (*onComplete)(TSkybox *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *skybox = Engine_buildSkybox(tEngine, tTexture);
          PROXY(onComplete(skybox));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_buildSkybox");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_buildColoredSkyboxRenderThread(TEngine *tEngine, float r, float g, float b, float a, void (*onComplete)(TSkybox *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *skybox = Engine_buildColoredSkybox(tEngine, r, g, b, a);
          PROXY(onComplete(skybox));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_buildColoredSkybox");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_buildIndirectLightFromIrradianceTextureRenderThread(
    TEngine *tEngine,
    TTexture *tReflectionsTexture,
    TTexture* tIrradianceTexture,
    float intensity,
    void (*onComplete)(TIndirectLight *)) {
      std::packaged_task<void()> lambda(
          [=]() mutable
          {
            auto *indirectLight = Engine_buildIndirectLightFromIrradianceTexture(tEngine, tReflectionsTexture, tIrradianceTexture, intensity);
            PROXY(onComplete(indirectLight));
          });
      auto fut = _renderThread->addTask(lambda, "Engine_buildIndirectLightFromIrradianceTexture");
  }
  
  EMSCRIPTEN_KEEPALIVE void Engine_buildIndirectLightFromIrradianceHarmonicsRenderThread(
    TEngine *tEngine,
    TTexture *tReflectionsTexture,
    float *harmonics,
    float intensity,
    void (*onComplete)(TIndirectLight *)) {
      std::packaged_task<void()> lambda(
          [=]() mutable
          {
            auto *indirectLight = Engine_buildIndirectLightFromIrradianceHarmonics(tEngine, tReflectionsTexture, harmonics, intensity);
            PROXY(onComplete(indirectLight));
          });
      auto fut = _renderThread->addTask(lambda, "Engine_buildIndirectLightFromIrradianceHarmonics");
  }


  EMSCRIPTEN_KEEPALIVE void Renderer_beginFrameRenderThread(TRenderer *tRenderer, TSwapChain *tSwapChain, uint64_t frameTimeInNanos, void (*onComplete)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto result = Renderer_beginFrame(tRenderer, tSwapChain, frameTimeInNanos);
          PROXY(onComplete(result));
        });
    auto fut = _renderThread->addTask(lambda, "Renderer_beginFrame");
  }
  EMSCRIPTEN_KEEPALIVE void Renderer_endFrameRenderThread(TRenderer *tRenderer, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Renderer_endFrame(tRenderer);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Renderer_endFrame");
  }

  EMSCRIPTEN_KEEPALIVE void Renderer_renderRenderThread(TRenderer *tRenderer, TView *tView, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Renderer_render(tRenderer, tView);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Renderer_render");
  }

  EMSCRIPTEN_KEEPALIVE void Renderer_renderStandaloneViewRenderThread(TRenderer *tRenderer, TView *tView, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Renderer_renderStandaloneView(tRenderer, tView);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Renderer_renderStandaloneView");
  }

  EMSCRIPTEN_KEEPALIVE void Renderer_setClearOptionsRenderThread(
      TRenderer *tRenderer,
      double clearR,
      double clearG,
      double clearB,
      double clearA,
      uint8_t clearStencil,
      bool clear,
      bool discard, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Renderer_setClearOptions(tRenderer, clearR, clearG, clearB, clearA, clearStencil, clear, discard);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Renderer_setClearOptions");
  }

  EMSCRIPTEN_KEEPALIVE void Renderer_readPixelsRenderThread(
      TRenderer *tRenderer,
      uint32_t width, uint32_t height, uint32_t xOffset, uint32_t yOffset,
      TRenderTarget *tRenderTarget,
      TPixelDataFormat tPixelBufferFormat,
      TPixelDataType tPixelDataType,
      uint8_t *out,
      size_t outLength,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Renderer_readPixels(tRenderer, width, height, xOffset, yOffset, tRenderTarget, tPixelBufferFormat, tPixelDataType, out, outLength);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Renderer_readPixels");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createImageMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createImageMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createImageMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createGizmoMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createGizmoMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createGizmoMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createSilhouetteMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createSilhouetteMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createSilhouetteMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createEdgeOutlineMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createEdgeOutlineMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createEdgeOutlineMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createWireframeMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createWireframeMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createWireframeMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createTranslationAxisMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createTranslationAxisMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createTranslationAxisMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createBoneOverlayMaterialRenderThread(TEngine *tEngine, void (*onComplete)(TMaterial *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createBoneOverlayMaterial(tEngine);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createBoneOverlayMaterial");
  }

  EMSCRIPTEN_KEEPALIVE void Material_createInstanceRenderThread(TMaterial *tMaterial, void (*onComplete)(TMaterialInstance *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *instance = Material_createInstance(tMaterial);
          PROXY(onComplete(instance));
        });
    auto fut = _renderThread->addTask(lambda, "Material_createInstance");
  }

  EMSCRIPTEN_KEEPALIVE void MaterialInstance_setParameterTextureRenderThread(
      TMaterialInstance *tMaterialInstance,
      const char *propertyName,
      TTexture* tTexture,
      TTextureSampler* tSampler,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          MaterialInstance_setParameterTexture(tMaterialInstance, propertyName, tTexture, tSampler);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "MaterialInstance_setParameterTexture");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_destroyRenderThread(TSceneAsset *tSceneAsset, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          SceneAsset_destroy(tSceneAsset);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_destroy");
  }


  EMSCRIPTEN_KEEPALIVE void SceneAsset_createFromFilamentAssetRenderThread(
      TEngine *tEngine,
      TGltfAssetLoader *tAssetLoader,
      TNameComponentManager *tNameComponentManager,
      TFilamentAsset *tFilamentAsset,
      bool rebuildVertices,
      void (*onComplete)(TSceneAsset *))
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          auto sceneAsset = SceneAsset_createFromFilamentAsset(tEngine, tAssetLoader, tNameComponentManager, tFilamentAsset, rebuildVertices);
          PROXY(onComplete(sceneAsset));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_createFromFilamentAsset");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_createFromBuffersRenderThread(
      TEngine *tEngine,
      TVertexBuffer *tVertexBuffer,
      TIndexBuffer *tIndexBuffer,
      TMaterialInstance **materialInstances,
      int materialInstanceCount,
      TPrimitiveType tPrimitiveType,
      Aabb3 boundingBox,
      void (*callback)(TSceneAsset *))
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          auto sceneAsset = SceneAsset_createFromBuffers(tEngine, tVertexBuffer, tIndexBuffer, materialInstances, materialInstanceCount, tPrimitiveType, boundingBox);
          PROXY(callback(sceneAsset));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_createFromBuffers");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_createInstanceRenderThread(
      TSceneAsset *asset, TMaterialInstance **tMaterialInstances,
      int materialInstanceCount,
      void (*callback)(TSceneAsset *))
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          auto instanceAsset = SceneAsset_createInstance(asset, tMaterialInstances, materialInstanceCount);
          PROXY(callback(instanceAsset));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_createInstance");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_releaseSourceDataRenderThread(
      TSceneAsset *tSceneAsset, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          SceneAsset_releaseSourceData(tSceneAsset);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_releaseSourceData");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_setFlatShadingRenderThread(
      TSceneAsset *tSceneAsset, bool flatShading, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          SceneAsset_setFlatShading(tSceneAsset, flatShading);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_setFlatShading");
  }

  EMSCRIPTEN_KEEPALIVE void MaterialProvider_createMaterialInstanceRenderThread(
      TMaterialProvider *tMaterialProvider,
      bool doubleSided,
      bool unlit,
      bool hasVertexColors,
      bool hasBaseColorTexture,
      bool hasNormalTexture,
      bool hasOcclusionTexture,
      bool hasEmissiveTexture,
      bool useSpecularGlossiness,
      int alphaMode,
      bool enableDiagnostics,
      bool hasMetallicRoughnessTexture,
      uint8_t metallicRoughnessUV,
      bool hasSpecularGlossinessTexture,
      uint8_t specularGlossinessUV,
      uint8_t baseColorUV,
      bool hasClearCoatTexture,
      uint8_t clearCoatUV,
      bool hasClearCoatRoughnessTexture,
      uint8_t clearCoatRoughnessUV,
      bool hasClearCoatNormalTexture,
      uint8_t clearCoatNormalUV,
      bool hasClearCoat,
      bool hasTransmission,
      bool hasTextureTransforms,
      uint8_t emissiveUV,
      uint8_t aoUV,
      uint8_t normalUV,
      bool hasTransmissionTexture,
      uint8_t transmissionUV,
      bool hasSheenColorTexture,
      uint8_t sheenColorUV,
      bool hasSheenRoughnessTexture,
      uint8_t sheenRoughnessUV,
      bool hasVolumeThicknessTexture,
      uint8_t volumeThicknessUV,
      bool hasSheen,
      bool hasIOR,
      bool hasVolume,
      void (*callback)(TMaterialInstance *))
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          auto materialInstance = MaterialProvider_createMaterialInstance(
              tMaterialProvider,
              doubleSided,
              unlit,
              hasVertexColors,
              hasBaseColorTexture,
              hasNormalTexture,
              hasOcclusionTexture,
              hasEmissiveTexture,
              useSpecularGlossiness,
              alphaMode,
              enableDiagnostics,
              hasMetallicRoughnessTexture,
              metallicRoughnessUV,
              hasSpecularGlossinessTexture,
              specularGlossinessUV,
              baseColorUV,
              hasClearCoatTexture,
              clearCoatUV,
              hasClearCoatRoughnessTexture,
              clearCoatRoughnessUV,
              hasClearCoatNormalTexture,
              clearCoatNormalUV,
              hasClearCoat,
              hasTransmission,
              hasTextureTransforms,
              emissiveUV,
              aoUV,
              normalUV,
              hasTransmissionTexture,
              transmissionUV,
              hasSheenColorTexture,
              sheenColorUV,
              hasSheenRoughnessTexture,
              sheenRoughnessUV,
              hasVolumeThicknessTexture,
              volumeThicknessUV,
              hasSheen,
              hasIOR,
              hasVolume);
          PROXY(callback(materialInstance));
        });
    auto fut = _renderThread->addTask(lambda, "MaterialProvider_createMaterialInstance");
  }

  EMSCRIPTEN_KEEPALIVE void ColorGrading_createRenderThread(TEngine *tEngine, TToneMapper *toneMapper, void (*callback)(TColorGrading *))
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          auto cg = ColorGrading_create(tEngine, toneMapper);
          PROXY(callback(cg));
        });
    auto fut = _renderThread->addTask(lambda, "ColorGrading_create");
  }

  EMSCRIPTEN_KEEPALIVE void ColorGradingBuilder_createRenderThread(void (*onComplete)(TColorGradingBuilder *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *builder = ColorGradingBuilder_create();
          PROXY(onComplete(builder));
        });
    auto fut = _renderThread->addTask(lambda, "ColorGradingBuilder_create");
  }

  EMSCRIPTEN_KEEPALIVE void ColorGradingBuilder_buildRenderThread(TColorGradingBuilder *tBuilder, TEngine *tEngine, void (*onComplete)(TColorGrading *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *colorGrading = ColorGradingBuilder_build(tBuilder, tEngine);
          PROXY(onComplete(colorGrading));
        });
    auto fut = _renderThread->addTask(lambda, "ColorGradingBuilder_build");
  }

  EMSCRIPTEN_KEEPALIVE void ColorGradingBuilder_destroyRenderThread(TColorGradingBuilder *tBuilder, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          ColorGradingBuilder_destroy(tBuilder);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "ColorGradingBuilder_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createLinearRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createLinear(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createLinear");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createACESRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createACES(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createACES");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createACESLegacyRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createACESLegacy(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createACESLegacy");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createFilmicRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createFilmic(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createFilmic");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createPBRNeutralRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createPBRNeutral(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createPBRNeutral");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createAGXRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createAGX(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createAGX");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createAGXWithLookRenderThread(TEngine *tEngine, int look, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createAGXWithLook(tEngine, look);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createAGXWithLook");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createGenericRenderThread(TEngine *tEngine, float contrast, float midGrayIn, float midGrayOut, float hdrMax, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createGeneric(tEngine, contrast, midGrayIn, midGrayOut, hdrMax);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createGeneric");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_createDisplayRangeRenderThread(TEngine *tEngine, void (*onComplete)(TToneMapper *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *toneMapper = ToneMapper_createDisplayRange(tEngine);
          PROXY(onComplete(toneMapper));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_createDisplayRange");
  }

  EMSCRIPTEN_KEEPALIVE void ToneMapper_destroyRenderThread(TToneMapper *tToneMapper, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          ToneMapper_destroy(tToneMapper);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "ToneMapper_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void Engine_destroyColorGradingRenderThread(TEngine *tEngine, TColorGrading *tColorGrading, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          Engine_destroyColorGrading(tEngine, tColorGrading);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Engine_destroyColorGrading");
  }

  EMSCRIPTEN_KEEPALIVE void View_pickRenderThread(TView *tView, uint32_t requestId, uint32_t x, uint32_t y, PickCallback callback)
  {
    // Post to the render thread: View::pick links a query into a list that
    // FRenderer::renderJob reads twice per frame without a lock (structure
    // pass decision, then the Picking Resolve Pass). A query queued from
    // another thread between the two reads makes the resolve pass read a
    // never-created FrameGraph handle -> FrameGraph::assertValid panic.
    std::packaged_task<void()> lambda(
        [=]
        {
          auto *view = reinterpret_cast<View *>(tView);
          view->pick(x, y, [=](filament::View::PickingQueryResult const &result)
                     { PROXY(callback(requestId, utils::Entity::smuggle(result.renderable), result.depth, result.fragCoords.x, result.fragCoords.y, result.fragCoords.z)); });
        });
    auto fut = _renderThread->addTask(lambda, "View_pick");
  }

  EMSCRIPTEN_KEEPALIVE void View_setColorGradingRenderThread(TView *tView, TColorGrading *tColorGrading, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setColorGrading(tView, tColorGrading);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setColorGrading");
  }

  EMSCRIPTEN_KEEPALIVE void View_setBloomRenderThread(TView *tView, bool enabled, double strength, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setBloom(tView, enabled, strength);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setBloom");
  }

  EMSCRIPTEN_KEEPALIVE void View_setCameraRenderThread(TView *tView, TCamera *tCamera, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setCamera(tView, tCamera);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setCamera");
  }

  EMSCRIPTEN_KEEPALIVE void View_getNameRenderThread(TView *tView, void (*onComplete)(const char *))
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          auto name = View_getName(tView);
          PROXY(onComplete(name));
        });
    auto fut = _renderThread->addTask(lambda, "View_getName");
  }

  EMSCRIPTEN_KEEPALIVE void View_setNameRenderThread(TView *tView, const char *name, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setName(tView, name);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setName");
  }

  EMSCRIPTEN_KEEPALIVE void View_setViewportRenderThread(TView *tView, uint32_t width, uint32_t height, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setViewport(tView, width, height);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setViewport");
  }

  EMSCRIPTEN_KEEPALIVE void View_setRenderTargetRenderThread(TView *tView, TRenderTarget *tRenderTarget, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setRenderTarget(tView, tRenderTarget);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setRenderTarget");
  }

  EMSCRIPTEN_KEEPALIVE void View_setAntiAliasingRenderThread(TView *tView, bool msaa, bool fxaa, bool taa, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setAntiAliasing(tView, msaa, fxaa, taa);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setAntiAliasing");
  }

  EMSCRIPTEN_KEEPALIVE void View_setPostProcessingRenderThread(TView *tView, bool enabled, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setPostProcessing(tView, enabled);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setPostProcessing");
  }

  EMSCRIPTEN_KEEPALIVE void View_setFrustumCullingEnabledRenderThread(TView *tView, bool enabled, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setFrustumCullingEnabled(tView, enabled);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setFrustumCullingEnabled");
  }

  EMSCRIPTEN_KEEPALIVE void View_setStencilBufferEnabledRenderThread(TView *tView, bool enabled, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setStencilBufferEnabled(tView, enabled);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setStencilBufferEnabled");
  }

  EMSCRIPTEN_KEEPALIVE void View_setDitheringEnabledRenderThread(TView *tView, bool enabled, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setDitheringEnabled(tView, enabled);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setDitheringEnabled");
  }

  EMSCRIPTEN_KEEPALIVE void View_setRenderQualityRenderThread(TView *tView, int qualityLevel, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setRenderQuality(tView, static_cast<TQualityLevel>(qualityLevel));
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setRenderQuality");
  }

  EMSCRIPTEN_KEEPALIVE void View_setSceneRenderThread(TView *tView, TScene *tScene, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setScene(tView, tScene);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setScene");
  }

  EMSCRIPTEN_KEEPALIVE void View_setLayerEnabledRenderThread(TView *tView, int layer, bool visible, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setLayerEnabled(tView, layer, visible);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setLayerEnabled");
  }

  EMSCRIPTEN_KEEPALIVE void View_setBlendModeRenderThread(TView *tView, int blendMode, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setBlendMode(tView, static_cast<TBlendMode>(blendMode));
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setBlendMode");
  }

  EMSCRIPTEN_KEEPALIVE void View_setFogOptionsRenderThread(TView *tView, TFogOptions tFogOptions, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setFogOptions(tView, tFogOptions);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setFogOptions");
  }

  EMSCRIPTEN_KEEPALIVE void View_setAmbientOcclusionOptionsRenderThread(TView *tView, TAmbientOcclusionOptions options, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setAmbientOcclusionOptions(tView, options);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setAmbientOcclusionOptions");
  }

  EMSCRIPTEN_KEEPALIVE void View_setFrontFaceWindingInvertedRenderThread(TView *tView, bool inverted, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setFrontFaceWindingInverted(tView, inverted);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setFrontFaceWindingInverted");
  }

  EMSCRIPTEN_KEEPALIVE void View_setShadowsEnabledRenderThread(TView *tView, bool enabled, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setShadowsEnabled(tView, enabled);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setShadowsEnabled");
  }

  EMSCRIPTEN_KEEPALIVE void View_setShadowTypeRenderThread(TView *tView, int shadowType, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setShadowType(tView, shadowType);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setShadowType");
  }

  EMSCRIPTEN_KEEPALIVE void View_setSoftShadowOptionsRenderThread(TView *tView, TSoftShadowOptions options, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setSoftShadowOptions(tView, options);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setSoftShadowOptions");
  }

  EMSCRIPTEN_KEEPALIVE void View_setVsmShadowOptionsRenderThread(TView *tView, TVsmShadowOptions options, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setVsmShadowOptions(tView, options);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setVsmShadowOptions");
  }

  EMSCRIPTEN_KEEPALIVE void View_setTransparentPickingEnabledRenderThread(TView *tView, bool enabled, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]
        {
          View_setTransparentPickingEnabled(tView, enabled);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "View_setTransparentPickingEnabled");
  }

  EMSCRIPTEN_KEEPALIVE void AnimationManager_resetToRestPoseRenderThread(TAnimationManager *tAnimationManager, TSceneAsset *tSceneAsset, uint32_t requestId, VoidCallback onComplete) {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          AnimationManager_resetToRestPose(tAnimationManager, tSceneAsset);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "AnimationManager_resetToRestPose");
  }

  EMSCRIPTEN_KEEPALIVE void AnimationManager_createRenderThread(TEngine *tEngine, void (*onComplete)(TAnimationManager *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *animationManager = AnimationManager_create(tEngine);
          PROXY(onComplete(animationManager));
        });
    auto fut = _renderThread->addTask(lambda, "AnimationManager_create");
  }

  EMSCRIPTEN_KEEPALIVE void AnimationManager_destroyRenderThread(TAnimationManager *tAnimationManager, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          AnimationManager_destroy(tAnimationManager);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "AnimationManager_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void AnimationManager_updateRenderThread(TAnimationManager *tAnimationManager, uint64_t frameTimeInNanos, uint32_t requestId, VoidCallback onComplete) {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          AnimationManager_update(tAnimationManager, frameTimeInNanos);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "AnimationManager_update");
  }

  EMSCRIPTEN_KEEPALIVE void AnimationManager_updateBoneMatricesRenderThread(
      TAnimationManager *tAnimationManager,
      TSceneAsset *sceneAsset,
      void (*callback)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          bool result = AnimationManager_updateBoneMatrices(tAnimationManager, sceneAsset);
          PROXY(callback(result));
        });
    auto fut = _renderThread->addTask(lambda, "AnimationManager_updateBoneMatrices");
  }

  EMSCRIPTEN_KEEPALIVE void AnimationManager_setMorphTargetWeightsRenderThread(
      TAnimationManager *tAnimationManager,
      EntityId entityId,
      const float *const morphData,
      int numWeights,
      void (*callback)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          bool result = AnimationManager_setMorphTargetWeights(tAnimationManager, entityId, morphData, numWeights);
          PROXY(callback(result));
        });
    auto fut = _renderThread->addTask(lambda, "AnimationManager_setMorphTargetWeights");
  }

  EMSCRIPTEN_KEEPALIVE void Image_createEmptyRenderThread(uint32_t width, uint32_t height, uint32_t channel, void (*onComplete)(TLinearImage *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto image = Image_createEmpty(width, height, channel);
          PROXY(onComplete(image));
        });
    auto fut = _renderThread->addTask(lambda, "Image_createEmpty");
  }

  EMSCRIPTEN_KEEPALIVE void Image_decodeRenderThread(uint8_t *data, size_t length, const char *name, bool alpha, void (*onComplete)(TLinearImage *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto image = Image_decode(data, length, name, alpha);
          PROXY(onComplete(image));
        });
    auto fut = _renderThread->addTask(lambda, "Image_decode");
  }

  EMSCRIPTEN_KEEPALIVE void Image_getBytesRenderThread(TLinearImage *tLinearImage, void (*onComplete)(float *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto bytes = Image_getBytes(tLinearImage);
          PROXY(onComplete(bytes));
        });
    auto fut = _renderThread->addTask(lambda, "Image_getBytes");
  }

  EMSCRIPTEN_KEEPALIVE void Image_destroyRenderThread(TLinearImage *tLinearImage, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Image_destroy(tLinearImage);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Image_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void Image_getWidthRenderThread(TLinearImage *tLinearImage, void (*onComplete)(uint32_t))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto width = Image_getWidth(tLinearImage);
          PROXY(onComplete(width));
        });
    auto fut = _renderThread->addTask(lambda, "Image_getWidth");
  }

  EMSCRIPTEN_KEEPALIVE void Image_getHeightRenderThread(TLinearImage *tLinearImage, void (*onComplete)(uint32_t))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto height = Image_getHeight(tLinearImage);
          PROXY(onComplete(height));
        });
    auto fut = _renderThread->addTask(lambda, "Image_getHeight");
  }

  EMSCRIPTEN_KEEPALIVE void Image_getChannelsRenderThread(TLinearImage *tLinearImage, void (*onComplete)(uint32_t))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto channels = Image_getChannels(tLinearImage);
          PROXY(onComplete(channels));
        });
    auto fut = _renderThread->addTask(lambda, "Image_getChannels");
  }

  EMSCRIPTEN_KEEPALIVE void Texture_buildRenderThread(
      TEngine *tEngine,
      uint32_t width,
      uint32_t height,
      uint32_t depth,
      uint8_t levels,
      uint16_t tUsage,
      intptr_t import,
      TTextureSamplerType sampler,
      TTextureFormat format, void (*onComplete)(TTexture *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *texture = Texture_build(tEngine, width, height, depth, levels, tUsage, import, sampler, format);
          PROXY(onComplete(texture));
        });
    auto fut = _renderThread->addTask(lambda, "Texture_build");
  }

  EMSCRIPTEN_KEEPALIVE void Texture_setExternalImageRenderThread(TEngine *tEngine, TTexture *tTexture, void *externalImage, uint32_t requestId, VoidCallback onComplete) {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Texture_setExternalImage(tEngine, tTexture, externalImage);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Texture_setExternalImage");
  }

  EMSCRIPTEN_KEEPALIVE void Texture_generateMipMapsRenderThread(TTexture *tTexture, TEngine *tEngine, uint32_t requestId, VoidCallback onComplete) {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Texture_generateMipMaps(tTexture, tEngine);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Texture_generateMipMaps");
  }

  #ifdef EMSCRIPTEN
  static std::unordered_map<uint32_t, std::function<void(int32_t)>> _emscriptenWrappers;

  EMSCRIPTEN_KEEPALIVE static void Emscripten_voidCallback(int32_t requestId) {
      Log("Emscripten_voidCallback: requestId %d", requestId);

      auto it = _emscriptenWrappers.find(requestId);
      if (it != _emscriptenWrappers.end()) {
          it->second(requestId);
          _emscriptenWrappers.erase(it);
      } else {
        Log("SEVERE: failed to find request id %d", requestId);
      }
  }
  #endif

  
  // Web callers must go through this variant — Ktx2Reader::load reaches into
  // Filament Engine state (Texture::Builder().build, internal job queues) and
  // those calls assert / abort when invoked from anywhere except the engine's
  // render thread. We also copy the input bytes synchronously here so the
  // lambda doesn't dangle when data was stack-allocated by the caller (the
  // ffigen_js Uint8List.address path uses stackAlloc for buffers < 32KB).
  EMSCRIPTEN_KEEPALIVE void Ktx2Reader_createTextureRenderThread(
    TEngine *tEngine, uint8_t* data, size_t size, int32_t transferFunction, int32_t maxMipLevels,
    void (*onComplete)(TKtx2LoadResult *)) {
    std::vector<uint8_t> bytes(data, data + size);
    std::packaged_task<void()> lambda(
        [tEngine, bytes = std::move(bytes), transferFunction, maxMipLevels, onComplete]() mutable
        {
          auto *result = Ktx2Reader_createTexture(
              tEngine, bytes.data(), bytes.size(), transferFunction, maxMipLevels);
          PROXY(onComplete(result));
        });
    auto fut = _renderThread->addTask(lambda, "Ktx2Reader_createTexture");
  }

  EMSCRIPTEN_KEEPALIVE void Ktx1Reader_createTextureRenderThread(
    TEngine *tEngine, TKtx1Bundle *tBundle, uint32_t requestId, VoidCallback onTextureUploadComplete, void (*onComplete)(TTexture *)) {

      #ifdef EMSCRIPTEN
        if(onTextureUploadComplete) {
            _emscriptenWrappers[requestId] = [=](int32_t requestId) {
                PROXY(onTextureUploadComplete(requestId));
            };
        }
      #endif
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          #ifdef EMSCRIPTEN
            auto *texture = Ktx1Reader_createTexture(tEngine, tBundle, requestId, onTextureUploadComplete ? Emscripten_voidCallback : nullptr);
          #else
            auto *texture = Ktx1Reader_createTexture(tEngine, tBundle, requestId, onTextureUploadComplete);
          #endif          
          PROXY(onComplete(texture));
        });
    auto fut = _renderThread->addTask(lambda, "Ktx1Reader_createTexture");
  }


  EMSCRIPTEN_KEEPALIVE void Texture_loadImageRenderThread(TEngine *tEngine, TTexture *tTexture, TLinearImage *tImage,
                                                          TPixelDataFormat bufferFormat, TPixelDataType pixelDataType,
                                                          int level,
                                                          void (*onComplete)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          bool result = Texture_loadImage(tEngine, tTexture, tImage, bufferFormat, pixelDataType, level);
          PROXY(onComplete(result));
        });
    auto fut = _renderThread->addTask(lambda, "Texture_loadImage");
  }

  EMSCRIPTEN_KEEPALIVE void Texture_setImageRenderThread(
      TEngine *tEngine,
      TTexture *tTexture,
      uint32_t level,
      uint8_t *data,
      size_t size,
      uint32_t x_offset,
      uint32_t y_offset,
      uint32_t z_offset,
      uint32_t width,
      uint32_t height,
      uint32_t depth,
      uint32_t bufferFormat,
      uint32_t pixelDataType,
      void (*onComplete)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          bool result = Texture_setImage(
              tEngine,
              tTexture,
              level,
              data,
              size,
              x_offset,
              y_offset,
              z_offset,
              width,
              height,
              depth,
              bufferFormat,
              pixelDataType);
          PROXY(onComplete(result));
        });
    auto fut = _renderThread->addTask(lambda, "Texture_setImage");
  }

  EMSCRIPTEN_KEEPALIVE void RenderTarget_getColorTextureRenderThread(TRenderTarget *tRenderTarget, void (*onComplete)(TTexture *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto texture = RenderTarget_getColorTexture(tRenderTarget);
          PROXY(onComplete(texture));
        });
    auto fut = _renderThread->addTask(lambda, "RenderTarget_getColorTexture");
  }

  EMSCRIPTEN_KEEPALIVE void RenderTarget_createRenderThread(
      TEngine *tEngine,
      TTexture *tColor,
      TTexture *tDepth,
      void (*onComplete)(TRenderTarget *))
  {
    auto color = reinterpret_cast<filament::Texture *>(tColor);
    auto depth = reinterpret_cast<filament::Texture *>(tDepth);

    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto texture = RenderTarget_create(tEngine, tColor, tDepth);
          PROXY(onComplete(texture));
        });
    auto fut = _renderThread->addTask(lambda, "RenderTarget_create");
  }

  EMSCRIPTEN_KEEPALIVE void RenderTarget_destroyRenderThread(
      TEngine *tEngine,
      TRenderTarget *tRenderTarget,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderTarget_destroy(tEngine, tRenderTarget);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderTarget_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_createRenderThread(void (*onComplete)(TTextureSampler *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto sampler = TextureSampler_create();
          PROXY(onComplete(sampler));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_create");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_createWithFilteringRenderThread(
      TSamplerMinFilter minFilter,
      TSamplerMagFilter magFilter,
      TSamplerWrapMode wrapS,
      TSamplerWrapMode wrapT,
      TSamplerWrapMode wrapR,
      void (*onComplete)(TTextureSampler *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto sampler = TextureSampler_createWithFiltering(minFilter, magFilter, wrapS, wrapT, wrapR);
          PROXY(onComplete(sampler));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_createWithFiltering");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_createWithComparisonRenderThread(
      TSamplerCompareMode compareMode,
      TSamplerCompareFunc compareFunc,
      void (*onComplete)(TTextureSampler *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto sampler = TextureSampler_createWithComparison(compareMode, compareFunc);
          PROXY(onComplete(sampler));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_createWithComparison");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setMinFilterRenderThread(
      TTextureSampler *sampler,
      TSamplerMinFilter filter,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setMinFilter(sampler, filter);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setMinFilter");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setMagFilterRenderThread(
      TTextureSampler *sampler,
      TSamplerMagFilter filter,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setMagFilter(sampler, filter);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setMagFilter");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setWrapModeSRenderThread(
      TTextureSampler *sampler,
      TSamplerWrapMode mode,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setWrapModeS(sampler, mode);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setWrapModeS");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setWrapModeTRenderThread(
      TTextureSampler *sampler,
      TSamplerWrapMode mode,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setWrapModeT(sampler, mode);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setWrapModeT");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setWrapModeRRenderThread(
      TTextureSampler *sampler,
      TSamplerWrapMode mode,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setWrapModeR(sampler, mode);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setWrapModeR");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setAnisotropyRenderThread(
      TTextureSampler *sampler,
      double anisotropy,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setAnisotropy(sampler, anisotropy);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setAnisotropy");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_setCompareModeRenderThread(
      TTextureSampler *sampler,
      TSamplerCompareMode mode,
      TTextureSamplerCompareFunc func,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_setCompareMode(sampler, mode, func);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_setCompareMode");
  }

  EMSCRIPTEN_KEEPALIVE void TextureSampler_destroyRenderThread(
      TTextureSampler *sampler,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TextureSampler_destroy(sampler);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TextureSampler_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void GltfAssetLoader_createRenderThread(
    TEngine *tEngine,
    TMaterialProvider *tMaterialProvider,
    TNameComponentManager *tNameComponentManager,
    void (*callback)(TGltfAssetLoader *))
  {
    std::packaged_task<void()> lambda(
      [=]() mutable
      {
        auto loader = GltfAssetLoader_create(tEngine, tMaterialProvider, tNameComponentManager);
        PROXY(callback(loader));
      });
    auto fut = _renderThread->addTask(lambda, "GltfAssetLoader_create");
  }

  EMSCRIPTEN_KEEPALIVE void GltfAssetLoader_destroyRenderThread(
    TGltfAssetLoader *tAssetLoader,
    uint32_t requestId,
    VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
      [=]() mutable
      {
        GltfAssetLoader_destroy(tAssetLoader);
        PROXY(onComplete(requestId));
      });
    auto fut = _renderThread->addTask(lambda, "GltfAssetLoader_destroy");
  }
  
  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_createRenderThread(TEngine *tEngine, void (*callback)(TGltfResourceLoader *)) {
    std::packaged_task<void()> lambda(
      [=]() mutable
      {
        auto loader = GltfResourceLoader_create(tEngine);
        PROXY(callback(loader));
      });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_create");
  }


  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_destroyRenderThread(TEngine *tEngine, TGltfResourceLoader *tResourceLoader, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          GltfResourceLoader_destroy(tEngine, tResourceLoader);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_loadResourcesRenderThread(TGltfResourceLoader *tGltfResourceLoader, TFilamentAsset *tFilamentAsset, void (*callback)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto result = GltfResourceLoader_loadResources(tGltfResourceLoader, tFilamentAsset);
          PROXY(callback(result));
        });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_loadResources");
  }

  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_addResourceDataRenderThread(
      TGltfResourceLoader *tGltfResourceLoader,
      const char *uri,
      uint8_t *data,
      size_t length,
      uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          GltfResourceLoader_addResourceData(tGltfResourceLoader, uri, data, length);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_addResourceData");
  }

  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_asyncBeginLoadRenderThread(
      TGltfResourceLoader *tGltfResourceLoader,
      TFilamentAsset *tFilamentAsset,
      void (*callback)(bool))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto result = GltfResourceLoader_asyncBeginLoad(tGltfResourceLoader, tFilamentAsset);
          PROXY(callback(result));
        });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_asyncBeginLoad");
  }

  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_asyncUpdateLoadRenderThread(
      TGltfResourceLoader *tGltfResourceLoader)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          GltfResourceLoader_asyncUpdateLoad(tGltfResourceLoader);
        });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_asyncUpdateLoad");
  }

  EMSCRIPTEN_KEEPALIVE void GltfResourceLoader_asyncGetLoadProgressRenderThread(
      TGltfResourceLoader *tGltfResourceLoader,
      void (*callback)(float))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto result = GltfResourceLoader_asyncGetLoadProgress(tGltfResourceLoader);
          PROXY(callback(result));
        });
    auto fut = _renderThread->addTask(lambda, "GltfResourceLoader_asyncGetLoadProgress");
  }

  EMSCRIPTEN_KEEPALIVE void GltfAssetLoader_loadRenderThread(
      TEngine *tEngine,
      TGltfAssetLoader *tAssetLoader,
      uint8_t *data,
      size_t length,
      uint32_t numInstances,
      void (*callback)(TFilamentAsset *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto loader = GltfAssetLoader_load(tEngine, tAssetLoader, data, length, numInstances);
          PROXY(callback(loader));
        });
    auto fut = _renderThread->addTask(lambda, "GltfAssetLoader_load");
  }

  EMSCRIPTEN_KEEPALIVE void Scene_addFilamentAssetRenderThread(TScene *tScene, TFilamentAsset *tAsset, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Scene_addFilamentAsset(tScene, tAsset);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Scene_addFilamentAsset");
  }

  EMSCRIPTEN_KEEPALIVE void FilamentAsset_getWireframeRenderThread(TFilamentAsset *tFilamentAsset, void (*onComplete)(EntityId))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto entityId = FilamentAsset_getWireframe(tFilamentAsset);
          PROXY(onComplete(entityId));
        });
    auto fut = _renderThread->addTask(lambda, "FilamentAsset_getWireframe");
  }

  EMSCRIPTEN_KEEPALIVE void Scene_addEntityRenderThread(TScene *tScene, EntityId entityId, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Scene_addEntity(tScene, entityId);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Scene_addEntity");
  }

  EMSCRIPTEN_KEEPALIVE void Scene_removeEntityRenderThread(TScene *tScene, EntityId entityId, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Scene_removeEntity(tScene, entityId);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Scene_removeEntity");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_addToSceneRenderThread(TSceneAsset *tSceneAsset, TScene *tScene, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          SceneAsset_addToScene(tSceneAsset, tScene);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_addToScene");
  }

  EMSCRIPTEN_KEEPALIVE void SceneAsset_removeFromSceneRenderThread(TSceneAsset *tSceneAsset, TScene *tScene, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          SceneAsset_removeFromScene(tSceneAsset, tScene);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "SceneAsset_removeFromScene");
  }

  EMSCRIPTEN_KEEPALIVE void Scene_setSkyboxRenderThread(TScene *tScene, TSkybox *tSkybox, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Scene_setSkybox(tScene, tSkybox);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Scene_setSkybox");
  }

  EMSCRIPTEN_KEEPALIVE void Scene_setIndirectLightRenderThread(TScene *tScene, TIndirectLight *tIndirectLight, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          Scene_setIndirectLight(tScene, tIndirectLight);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "Scene_setIndirectLight");
  }

  EMSCRIPTEN_KEEPALIVE void Gizmo_createRenderThread(
      TEngine *tEngine,
      TGltfAssetLoader *tAssetLoader,
      TGltfResourceLoader *tGltfResourceLoader,
      TNameComponentManager *tNameComponentManager,
      TView *tView,
      TMaterial *tMaterial,
      TGizmoType tGizmoType,
      void (*callback)(TGizmo *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *gizmo = Gizmo_create(tEngine, tAssetLoader, tGltfResourceLoader, tNameComponentManager, tView, tMaterial, tGizmoType);
          PROXY(callback(gizmo));
        });
    auto fut = _renderThread->addTask(lambda, "Gizmo_create");
  }

  EMSCRIPTEN_KEEPALIVE void VertexBufferBuilder_buildRenderThread(
      TVertexBufferBuilder *tBuilder,
      TEngine *tEngine,
      void (*onComplete)(TVertexBuffer *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *vertexBuffer = VertexBufferBuilder_build(tBuilder, tEngine);
          PROXY(onComplete(vertexBuffer));
        });
    auto fut = _renderThread->addTask(lambda, "VertexBufferBuilder_build");
  }

  EMSCRIPTEN_KEEPALIVE void VertexBuffer_destroyRenderThread(
      TEngine *tEngine,
      TVertexBuffer *tBuffer,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          VertexBuffer_destroy(tEngine, tBuffer);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "VertexBuffer_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void VertexBuffer_setBufferAtRenderThread(
      TEngine* tEngine,
      TVertexBuffer* tBuffer,
      uint8_t bufferIndex,
      void* data,
      size_t sizeInBytes,
      uint32_t byteOffset,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    // Copy data using std::vector to ensure it remains valid after the function returns
    auto *buffer = new std::vector<uint8_t>(sizeInBytes);
    std::copy(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + sizeInBytes, buffer->begin());

    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          // Create a BufferDescriptor with a callback to delete the vector
          VertexBuffer::BufferDescriptor bufferDescriptor(
              buffer->data(),
              buffer->size(),
              [](void* buffer, size_t size, void* user) {
                delete reinterpret_cast<std::vector<uint8_t>*>(user);
              },
              buffer
          );

          // Call the original setBufferAt with our BufferDescriptor
          auto* engine = reinterpret_cast<filament::Engine*>(tEngine);
          auto* vertexBuffer = reinterpret_cast<filament::VertexBuffer*>(tBuffer);
          vertexBuffer->setBufferAt(*engine, bufferIndex, std::move(bufferDescriptor), byteOffset);

          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "VertexBuffer_setBufferAt");
  }

  EMSCRIPTEN_KEEPALIVE void IndexBufferBuilder_buildRenderThread(
      TIndexBufferBuilder *tBuilder,
      TEngine *tEngine,
      void (*onComplete)(TIndexBuffer *))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *indexBuffer = IndexBufferBuilder_build(tBuilder, tEngine);
          PROXY(onComplete(indexBuffer));
        });
    auto fut = _renderThread->addTask(lambda, "IndexBufferBuilder_build");
  }

  EMSCRIPTEN_KEEPALIVE void IndexBuffer_destroyRenderThread(
      TEngine *tEngine,
      TIndexBuffer *tBuffer,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          IndexBuffer_destroy(tEngine, tBuffer);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "IndexBuffer_destroy");
  }

  EMSCRIPTEN_KEEPALIVE void IndexBuffer_setBufferRenderThread(
      TEngine* tEngine,
      TIndexBuffer* tBuffer,
      void* data,
      size_t sizeInBytes,
      uint32_t byteOffset,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    // Copy data using std::vector to ensure it remains valid after the function returns
    auto *buffer = new std::vector<uint8_t>(sizeInBytes);
    std::copy(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + sizeInBytes, buffer->begin());

    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          // Create a BufferDescriptor with a callback to delete the vector
          IndexBuffer::BufferDescriptor bufferDescriptor(
              buffer->data(),
              buffer->size(),
              [](void* buffer, size_t size, void* user) {
                delete reinterpret_cast<std::vector<uint8_t>*>(user);
              },
              buffer
          );

          // Call the original setBuffer with our BufferDescriptor
          auto* engine = reinterpret_cast<filament::Engine*>(tEngine);
          auto* indexBuffer = reinterpret_cast<filament::IndexBuffer*>(tBuffer);
          indexBuffer->setBuffer(*engine, std::move(bufferDescriptor), byteOffset);

          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "IndexBuffer_setBuffer");
  }

  EMSCRIPTEN_KEEPALIVE void RenderableBuilder_buildRenderThread(
      TRenderableBuilder *tBuilder,
      TEngine *tEngine,
      EntityId entityId,
      void (*onComplete)(int))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto *builder = reinterpret_cast<filament::RenderableManager::Builder*>(tBuilder);
          auto *engine = reinterpret_cast<filament::Engine*>(tEngine);
          const auto &entity = utils::Entity::import(entityId);

          auto result = builder->build(*engine, entity);
          PROXY(onComplete(static_cast<int>(result)));
        });
    auto fut = _renderThread->addTask(lambda, "RenderableBuilder_build");
  }

  EMSCRIPTEN_KEEPALIVE void EntityManager_createEntityRenderThread(TEntityManager *tEntityManager, void (*onComplete)(EntityId))
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          auto entityId = EntityManager_createEntity(tEntityManager);
          PROXY(onComplete(entityId));
        });
    auto fut = _renderThread->addTask(lambda, "EntityManager_createEntity");
  }

  EMSCRIPTEN_KEEPALIVE void EntityManager_destroyEntityRenderThread(TEntityManager *tEntityManager, EntityId entityId, uint32_t requestId, VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          EntityManager_destroyEntity(tEntityManager, entityId);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "EntityManager_destroyEntity");
  }

  EMSCRIPTEN_KEEPALIVE void TransformManager_setTransformRenderThread(
      TTransformManager *tTransformManager,
      EntityId entityId,
      double4x4 transform,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TransformManager_setTransform(tTransformManager, entityId, transform);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TransformManager_setTransform");
  }

  EMSCRIPTEN_KEEPALIVE void TransformManager_setParentRenderThread(
      TTransformManager *tTransformManager,
      EntityId child,
      EntityId parent,
      bool preserveScaling,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TransformManager_setParent(tTransformManager, child, parent, preserveScaling);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TransformManager_setParent");
  }

  EMSCRIPTEN_KEEPALIVE void TransformManager_createComponentRenderThread(
      TTransformManager *tTransformManager,
      EntityId entityId,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TransformManager_createComponent(tTransformManager, entityId);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TransformManager_createComponent");
  }

  EMSCRIPTEN_KEEPALIVE void TransformManager_removeComponentRenderThread(
      TTransformManager *tTransformManager,
      EntityId entityId,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          TransformManager_removeComponent(tTransformManager, entityId);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "TransformManager_removeComponent");
  }

  EMSCRIPTEN_KEEPALIVE void RenderableManager_destroyEntityRenderThread(
      TRenderableManager *tRenderableManager,
      EntityId entityId,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderableManager_destroyEntity(tRenderableManager, entityId);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderableManager_destroyEntity");
  }

  EMSCRIPTEN_KEEPALIVE void RenderableManager_setBonesFromMat4RenderThread(
      TRenderableManager *tRenderableManager,
      EntityId entityId,
      const float *transforms,
      size_t boneCount,
      size_t offset,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderableManager_setBonesFromMat4(tRenderableManager, entityId, transforms, boneCount, offset);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderableManager_setBonesFromMat4");
  }

  EMSCRIPTEN_KEEPALIVE void RenderableManager_setBonesFromBoneRenderThread(
      TRenderableManager *tRenderableManager,
      EntityId entityId,
      const float *bones,
      size_t boneCount,
      size_t offset,
      uint32_t requestId,
      VoidCallback onComplete)
  {
    std::packaged_task<void()> lambda(
        [=]() mutable
        {
          RenderableManager_setBonesFromBone(tRenderableManager, entityId, bones, boneCount, offset);
          PROXY(onComplete(requestId));
        });
    auto fut = _renderThread->addTask(lambda, "RenderableManager_setBonesFromBone");
  }

}
