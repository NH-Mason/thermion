#pragma once

#include "APIBoundaryTypes.h"
#include "TRenderManager.h"

#ifdef __cplusplus
namespace thermion
{
    extern "C"
    {
#endif
        typedef void (*FrameCallback)(uint64_t frameTimeNanos);
        typedef void (*PostRenderCallback)(void* userData);

        EMSCRIPTEN_KEEPALIVE void FrameScheduler_start(FrameCallback callback, int targetFps);
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_stop();

        // Native render loop
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_setRenderThread(void* renderThread);
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_setRenderManager(TRenderManager* rm);
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_setPostRenderCallback(PostRenderCallback callback, void* userData);
        // Request a single render frame (non-blocking, queues to render thread).
        // Returns true only when a render was actually queued.
        EMSCRIPTEN_KEEPALIVE bool FrameScheduler_requestRender(uint64_t frameTimeNanos);
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_startNativeRenderLoop(int targetFps);

        // Port-based frame scheduler (hot restart safe)
        // Initialize Dart API DL - must be called once before using port mode
        EMSCRIPTEN_KEEPALIVE int FrameScheduler_initDartApi(void* data);
        // Start frame scheduler in port mode - posts frame timestamps to Dart port
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_startWithPort(int64_t port, int targetFps);

        // Multi-client deregistration (two engines in one process).
        // FrameScheduler_start / FrameScheduler_startWithPort REGISTER a client
        // with one process-shared native scheduler (created on first
        // registration). These remove exactly one client; the scheduler is
        // destroyed when the last client deregisters. FrameScheduler_stop()
        // remains the legacy remove-everything teardown and must not be called
        // while another engine's client is registered.
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_removePort(int64_t port);
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_removeCallback(FrameCallback callback);

        // Cap the dispatch rate to [fps] by skipping frames at the source.
        // fps <= 0 means unlimited (dispatch every vsync). The value is retained
        // and applied to schedulers created after this call as well.
        EMSCRIPTEN_KEEPALIVE void FrameScheduler_setTargetFps(int fps);

        // Returns steady_clock microseconds
        EMSCRIPTEN_KEEPALIVE int64_t FrameScheduler_steadyClockUs();

#ifdef __cplusplus
    }
}
#endif
