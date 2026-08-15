#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "c_api/FrameSchedulerApi.h"
#include "c_api/TRenderManager.h"

#include "rendering/RenderThread.hpp"
#include "rendering/RenderManager.hpp"
#include "rendering/FrameScheduler.hpp"

// Dart API for port-based frame scheduling (hot restart safe)
#ifndef __EMSCRIPTEN__
#include "dart/dart_api_dl.h" // needed for FrameScheduler_initDartApi
#endif

using namespace thermion;

extern "C"
{

  static thermion::FrameScheduler* _frameScheduler = nullptr;

  // --- Multi-client registry (two engines in one process) -------------------
  // The native scheduler is a process-wide singleton, but each Flutter engine
  // (primary window, popout windows) runs its own Dart isolate with its own
  // FrameScheduler facade. Clients register through FrameScheduler_start /
  // FrameScheduler_startWithPort; one shared scheduler fans frames out to all
  // of them via _fanoutFrameCallback. A dead port (its isolate died in a hot
  // restart or a closed window) is pruned on its first failed post. The
  // scheduler is torn down when the last client deregisters through
  // FrameScheduler_removePort / FrameScheduler_removeCallback.
  //
  // Lock order: _schedulerLifecycleMutex (outer) -> _frameClientsMutex
  // (inner). The dispatch thread only ever takes the inner mutex, so a
  // lifecycle holder joining the dispatch thread cannot deadlock.
  //
  // Only PORT clients may register from a Flutter engine (any build mode):
  // an isolate can die without deregistering — hot restart, or a popout
  // window closing without a clean FilamentApp.destroy — and a dead port is
  // detectable (Dart_PostCObject_DL returns false) while a dead
  // NativeCallable trampoline is not. Direct-callback clients have no
  // liveness check and exist for non-Flutter embedders that own their
  // process lifetime.
  struct FrameClient {
    FrameCallback callback; // nullptr for port clients
    int64_t port;           // 0 for callback clients
  };
  // Function-local lazy statics (never destroyed): nothing runs at image
  // load, which keeps the plugin free of static-initialization order and
  // CRT-mix hazards — a namespace-scope std::mutex here fast-failed the
  // DEBUG Windows runner at process init (0xC0000409 before wWinMain).
  static std::mutex& _schedulerLifecycleMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
  }
  static std::mutex& _frameClientsMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
  }
  static std::vector<FrameClient>& _frameClients() {
    static std::vector<FrameClient>* v = new std::vector<FrameClient>();
    return *v;
  }

  static void _fanoutFrameCallback(uint64_t frameTimeNanos) {
#ifndef __EMSCRIPTEN__
    std::lock_guard<std::mutex> lock(_frameClientsMutex());
    for (auto it = _frameClients().begin(); it != _frameClients().end();) {
      if (it->callback) {
        it->callback(frameTimeNanos);
        ++it;
        continue;
      }
      if (Dart_PostCObject_DL == nullptr) {
        // Dart_InitializeApiDL never succeeded; this port can never be
        // reached. Drop the client instead of dereferencing null.
        it = _frameClients().erase(it);
        continue;
      }
      Dart_CObject msg;
      msg.type = Dart_CObject_kInt64;
      msg.value.as_int64 = static_cast<int64_t>(frameTimeNanos);
      if (Dart_PostCObject_DL(it->port, &msg)) {
        ++it;
      } else {
        // The receiving isolate is gone (hot restart / closed window).
        it = _frameClients().erase(it);
      }
    }
    // Never stop/delete the scheduler from inside its own dispatch: platform
    // stop() implementations join the dispatch thread. An empty registry just
    // dispatches to nobody until the next registration or an explicit
    // FrameScheduler_stop().
#endif
  }


  // Native render loop state
  static TRenderManager* _nativeRenderManager = nullptr;
  static RenderThread* _renderThread = nullptr;  // non-owning; owned by ThermionDartRenderThreadApi
  static std::atomic<bool> _nativeRenderInProgress{false};
  static PostRenderCallback _postRenderCallback = nullptr;
  static void* _postRenderUserData = nullptr;

  // Process-wide desired cap. It survives scheduler stop/start so lifecycle
  // transitions do not silently discard the user's setting. 0 = unlimited.
  static std::atomic<int> _targetFpsLimit{0};

  // Pacing state for the Linux Flutter-synced request-render path, which
  // bypasses FrameScheduler::dispatchFrame. These fields are only touched on
  // Flutter's UI thread.
  static int _requestAppliedFpsLimit = 0;
  static uint64_t _nextRequestRenderNs = 0;

  static void applyTargetFps(FrameScheduler* scheduler) {
    scheduler->setTargetFps(_targetFpsLimit.load(std::memory_order_relaxed));
  }

  // Caller must hold _schedulerLifecycleMutex.
  static void _ensureSchedulerStartedLocked(int targetFps) {
#ifndef __EMSCRIPTEN__
    if (_frameScheduler) return; // already running with the fan-out callback
    _frameScheduler = thermion::FrameScheduler::create(targetFps);
    applyTargetFps(_frameScheduler);
    _frameScheduler->start(_fanoutFrameCallback);
#endif
  }

  // Caller must hold _schedulerLifecycleMutex.
  static void _stopSchedulerLocked() {
#ifndef __EMSCRIPTEN__
    if (!_frameScheduler) return;
    _frameScheduler->stop();
    delete _frameScheduler;
    _frameScheduler = nullptr;
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_start(FrameCallback callback, int targetFps) {
#ifndef __EMSCRIPTEN__
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    {
      std::lock_guard<std::mutex> lock(_frameClientsMutex());
      bool present = false;
      for (const auto& client : _frameClients()) {
        if (client.callback == callback) {
          present = true;
          break;
        }
      }
      if (!present) {
        _frameClients().push_back(FrameClient{callback, 0});
      }
    }
    _ensureSchedulerStartedLocked(targetFps);
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_stop() {
#ifndef __EMSCRIPTEN__
    // Legacy remove-everything teardown. With more than one live engine this
    // stops every window's frame loop — per-client removal goes through
    // FrameScheduler_removePort / FrameScheduler_removeCallback instead.
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    {
      std::lock_guard<std::mutex> lock(_frameClientsMutex());
      _frameClients().clear();
    }
    _stopSchedulerLocked();

    // No scheduler callback can be admitted after stop() returns. Wait for
    // the final accepted render before invalidating any pointer it captured.
    while (_nativeRenderInProgress.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    _nativeRenderManager = nullptr;
    _renderThread = nullptr;
    _postRenderCallback = nullptr;
    _postRenderUserData = nullptr;
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_removePort(int64_t port) {
#ifndef __EMSCRIPTEN__
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    bool empty;
    {
      std::lock_guard<std::mutex> lock(_frameClientsMutex());
      _frameClients().erase(
          std::remove_if(_frameClients().begin(), _frameClients().end(),
                         [port](const FrameClient& client) {
                           return client.callback == nullptr &&
                                  client.port == port;
                         }),
          _frameClients().end());
      empty = _frameClients().empty();
    }
    if (empty) {
      _stopSchedulerLocked();
    }
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_removeCallback(FrameCallback callback) {
#ifndef __EMSCRIPTEN__
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    bool empty;
    {
      std::lock_guard<std::mutex> lock(_frameClientsMutex());
      _frameClients().erase(
          std::remove_if(_frameClients().begin(), _frameClients().end(),
                         [callback](const FrameClient& client) {
                           return client.callback == callback;
                         }),
          _frameClients().end());
      empty = _frameClients().empty();
    }
    if (empty) {
      _stopSchedulerLocked();
    }
#endif
  }

  // Port-based frame scheduler (hot restart safe)
  // When the Dart isolate dies (hot restart), Dart_PostCObject_DL silently
  // drops messages instead of crashing.

#ifndef __EMSCRIPTEN__
  static bool _dartApiInitialized = false;
#endif

  // Returns steady_clock microseconds — call from Dart to measure port transit time
  EMSCRIPTEN_KEEPALIVE int64_t FrameScheduler_steadyClockUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  EMSCRIPTEN_KEEPALIVE int FrameScheduler_initDartApi(void* data) {
#ifndef __EMSCRIPTEN__
    if (!_dartApiInitialized && data != nullptr) {
      intptr_t result = Dart_InitializeApiDL(data);
      _dartApiInitialized = (result == 0);
      return _dartApiInitialized ? 0 : -1;
    }
    return _dartApiInitialized ? 0 : -1;
#else
    return -1;
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_startWithPort(int64_t port, int targetFps) {
#ifndef __EMSCRIPTEN__
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    {
      std::lock_guard<std::mutex> lock(_frameClientsMutex());
      bool present = false;
      for (const auto& client : _frameClients()) {
        if (client.callback == nullptr && client.port == port) {
          present = true;
          break;
        }
      }
      if (!present) {
        _frameClients().push_back(FrameClient{nullptr, port});
      }
    }
    // The scheduler always runs in callback mode with the fan-out trampoline;
    // ports are posted to from the trampoline, not by the scheduler itself.
    _ensureSchedulerStartedLocked(targetFps);
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_setTargetFps(int fps) {
    int normalizedFps = fps > 0 ? fps : 0;
    _targetFpsLimit.store(normalizedFps, std::memory_order_relaxed);
#ifndef __EMSCRIPTEN__
    // The cap is process-wide: with two engines registered the shared
    // scheduler dispatches at one rate for both windows.
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    if (_frameScheduler) {
      _frameScheduler->setTargetFps(normalizedFps);
    }
#endif
  }

  // === Native render loop (bypasses Dart event loop) ===

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_setRenderThread(void* renderThread) {
#ifndef __EMSCRIPTEN__
    _renderThread = static_cast<RenderThread*>(renderThread);
#endif
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_setRenderManager(TRenderManager* rm) {
    _nativeRenderManager = rm;
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_setPostRenderCallback(PostRenderCallback callback, void* userData) {
    _postRenderCallback = callback;
    _postRenderUserData = userData;
  }

  // Frame callback for native render loop — runs on scheduler thread
  static bool _nativeFrameCallback(uint64_t frameTimeNanos) {
    auto* renderManager = _nativeRenderManager;
    auto* renderThread = _renderThread;
    auto postRenderCallback = _postRenderCallback;
    auto* postRenderUserData = _postRenderUserData;
    if (!renderManager || !renderThread) return false;
    if (_nativeRenderInProgress.exchange(true, std::memory_order_acq_rel)) {
      return false; // skip if still rendering
    }

    renderThread->addDetachedTask([
      renderManager,
      frameTimeNanos,
      postRenderCallback,
      postRenderUserData
    ]() {
      RenderManager_render(renderManager, frameTimeNanos);

      if (postRenderCallback) {
        postRenderCallback(postRenderUserData);
      }

      _nativeRenderInProgress.store(false, std::memory_order_release);
    });
    return true;
  }

  // Adapter for FrameScheduler::Callback, whose return type is void.
  static void _nativeScheduledFrameCallback(uint64_t frameTimeNanos) {
    _nativeFrameCallback(frameTimeNanos);
  }

  // Request a single render frame (called from Dart's frame callback).
  // Non-blocking: queues the render to the render thread and returns.
  // Throttled here (not in _nativeFrameCallback) so the Linux Flutter-synced
  // path — which bypasses the FrameScheduler/dispatchFrame throttle — still
  // honors the target fps. display-link/timer/DXGI paths pace themselves in
  // dispatchFrame and don't go through requestRender.
  EMSCRIPTEN_KEEPALIVE bool FrameScheduler_requestRender(uint64_t frameTimeNanos) {
    int fps = _targetFpsLimit.load(std::memory_order_relaxed);
    if (fps > 0) {
      const uint64_t interval = std::max<uint64_t>(
          1, 1000000000ULL / static_cast<uint64_t>(fps));
      const uint64_t tolerance = 1000000ULL; // 1 ms

      if (_requestAppliedFpsLimit != fps || _nextRequestRenderNs == 0) {
        _requestAppliedFpsLimit = fps;
        _nextRequestRenderNs = frameTimeNanos;
      }

      if (_nextRequestRenderNs > frameTimeNanos &&
          _nextRequestRenderNs - frameTimeNanos > tolerance) {
        return false;
      }

      if (!_nativeFrameCallback(frameTimeNanos)) {
        return false;
      }

      if (_nextRequestRenderNs <= frameTimeNanos) {
        const uint64_t missedIntervals =
            (frameTimeNanos - _nextRequestRenderNs) / interval + 1;
        _nextRequestRenderNs += missedIntervals * interval;
      } else {
        _nextRequestRenderNs += interval;
      }
      return true;
    } else {
      _requestAppliedFpsLimit = 0;
      _nextRequestRenderNs = 0;
    }
    return _nativeFrameCallback(frameTimeNanos);
  }

  EMSCRIPTEN_KEEPALIVE void FrameScheduler_startNativeRenderLoop(int targetFps) {
#ifndef __EMSCRIPTEN__
    // Standalone native-loop mode (single client by construction); replaces
    // whatever scheduler exists.
    std::lock_guard<std::mutex> lifecycle(_schedulerLifecycleMutex());
    _stopSchedulerLocked();
    _frameScheduler = new thermion::TimerFrameScheduler(
        targetFps > 0 ? targetFps : 60);
    applyTargetFps(_frameScheduler);
    _frameScheduler->start(_nativeScheduledFrameCallback);
#endif
  }

}
