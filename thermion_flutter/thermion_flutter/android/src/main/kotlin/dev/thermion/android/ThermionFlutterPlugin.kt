package dev.thermion.android

import android.app.Activity
import android.content.res.AssetManager
import android.graphics.*
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Choreographer
import android.view.Surface
import androidx.annotation.NonNull
import androidx.annotation.RequiresApi
import androidx.lifecycle.Lifecycle
import io.flutter.FlutterInjector
import io.flutter.embedding.engine.FlutterJNI
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.embedding.engine.plugins.lifecycle.HiddenLifecycleReference
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.view.TextureRegistry
import java.io.File
import java.util.*

class NativeWindowHelper {
    companion object {
      external fun getNativeWindowFromSurface(surface: Surface): Long
      external fun releaseNativeWindow(window: Long)
    }
}

class ThermionFlutterPlugin: FlutterPlugin, MethodCallHandler, ActivityAware {

  companion object {
      const val CHANNEL_NAME = "dev.thermion.flutter/event"
      const val TAG = "FilamentPlugin"

      // Frames a destroyed texture's Surface/SurfaceProducer outlive the
      // Dart-side swapchain destroy. Mirrors the Windows 5-frame delay for
      // superseded render targets (native_texture_surface_manager.dart): the
      // GLES driver and Impeller's Vulkan import can still hold fences on
      // ImageReader buffers for a frame or two after eglDestroySurface.
      const val RELEASE_DELAY_FRAMES = 5
      const val RELEASE_FALLBACK_MS = 250L
  }

  private lateinit var channel : MethodChannel

  private lateinit var flutterPluginBinding : FlutterPlugin.FlutterPluginBinding

  private var lifecycle: Lifecycle? = null

  private data class TextureEntry(
      val surfaceProducer: TextureRegistry.SurfaceProducer?,
      val surfaceTextureEntry: TextureRegistry.SurfaceTextureEntry?,
      var surface: Surface?,
      var nativeWindow: Long
  ) {
      fun release() {
          surfaceProducer?.setCallback(null)
          surface?.release()
          surface = null
          NativeWindowHelper.releaseNativeWindow(nativeWindow)
          nativeWindow = 0L
          surfaceProducer?.release()
          surfaceTextureEntry?.release()
      }
  }

  var _surface: Surface? = null
  private val textures: MutableMap<Long, TextureEntry> = mutableMapOf()

  // Destroyed on the Dart side but not yet released (see
  // RELEASE_DELAY_FRAMES); flushed immediately on engine detach. Identity
  // set: TextureEntry is a data class with mutable fields.
  private val pendingReleases: MutableSet<TextureEntry> =
      Collections.newSetFromMap(IdentityHashMap())

  private fun releaseAfterFrames(entry: TextureEntry, frames: Int) {
      if (!pendingReleases.contains(entry)) return
      if (frames <= 0) {
          pendingReleases.remove(entry)
          entry.release()
          return
      }
      Choreographer.getInstance().postFrameCallback {
          releaseAfterFrames(entry, frames - 1)
      }
  }

  // Frame callbacks never fire while the app is backgrounded; release on a
  // timer too. Whichever path runs first wins, the other sees the entry gone.
  private fun scheduleRelease(entry: TextureEntry) {
      pendingReleases.add(entry)
      releaseAfterFrames(entry, RELEASE_DELAY_FRAMES)
      Handler(Looper.getMainLooper()).postDelayed({
          releaseAfterFrames(entry, 0)
      }, RELEASE_FALLBACK_MS)
  }

  private lateinit var activity:Activity

  private fun notifyDartBeforeReleasingSurface(
      method: String,
      arguments: Any,
      surface: Surface?,
      nativeWindow: Long
  ) {
      fun releaseOld() {
          surface?.release()
          NativeWindowHelper.releaseNativeWindow(nativeWindow)
      }
      channel.invokeMethod(method, arguments, object : MethodChannel.Result {
          override fun success(result: Any?) {
              releaseOld()
          }

          override fun error(errorCode: String, errorMessage: String?, errorDetails: Any?) {
              Log.e(
                  TAG,
                  "$method failed before releasing the Android surface: " +
                      "$errorCode ${errorMessage ?: ""}"
              )
              releaseOld()
          }

          override fun notImplemented() {
              Log.e(TAG, "$method was not handled before releasing the Android surface")
              releaseOld()
          }
      })
  }

  override fun onAttachedToEngine(@NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
    this.flutterPluginBinding = flutterPluginBinding
    channel = MethodChannel(flutterPluginBinding.binaryMessenger, CHANNEL_NAME)
    channel.setMethodCallHandler(this)
    Log.d("thermion_flutter", "Loading library")
    System.loadLibrary("thermion_flutter_android")
    Log.d("thermion_flutter", "Loaded")
  }

  override fun onAttachedToActivity(binding: ActivityPluginBinding) {
    lifecycle = (binding.lifecycle as? HiddenLifecycleReference)?.lifecycle
    activity = binding.activity
    activity.window.setFormat(PixelFormat.RGBA_8888)
  }

  @RequiresApi(Build.VERSION_CODES.M)
  override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
    when (call.method) {
        "createTexture" -> {
                val args = call.arguments as List<*>
                val width = args[0] as Int
                val height = args[1] as Int
                val useSurfaceProducer = args.getOrNull(4) as? Boolean ?: false
                if (width < 1 || height < 1) {
                    result.error("DIMENSION_MISMATCH", "Both dimensions must be greater than zero (you provided $width x $height)", null)
                    return
                }
                val producer: TextureRegistry.SurfaceProducer?
                val surfaceTextureEntry: TextureRegistry.SurfaceTextureEntry?
                val surface: Surface
                val flutterTextureId: Long

                if (useSurfaceProducer) {
                    Log.d("thermion_flutter", "Creating SurfaceProducer ${width}x${height}")

                    // SurfaceProducer fixes premultiplied-alpha compositing
                    // under Impeller, but Flutter's implementation is backed
                    // by an ImageReader on API 29+.
                    producer = flutterPluginBinding.textureRegistry.createSurfaceProducer(
                        TextureRegistry.SurfaceLifecycle.resetInBackground
                    )
                    producer.setSize(width, height)
                    surfaceTextureEntry = null
                    surface = producer.getSurface()
                    flutterTextureId = producer.id()
                } else {
                    Log.d("thermion_flutter", "Creating SurfaceTexture ${width}x${height}")

                    // SurfaceTexture avoids ImageReader acquisition on the
                    // Android main thread and is substantially faster on some
                    // devices. It is the default for opaque Thermion views.
                    producer = null
                    surfaceTextureEntry =
                        flutterPluginBinding.textureRegistry.createSurfaceTexture()
                    val surfaceTexture = surfaceTextureEntry.surfaceTexture()
                    surfaceTexture.setDefaultBufferSize(width, height)
                    surface = Surface(surfaceTexture)
                    flutterTextureId = surfaceTextureEntry.id()
                }

                if (!surface.isValid) {
                    surface.release()
                    producer?.release()
                    surfaceTextureEntry?.release()
                    result.error("SURFACE_INVALID", "Failed to create valid surface", null)
                    return
                }

                Log.d("thermion_flutter", "Loading library")
                System.loadLibrary("thermion_flutter_android")
                val nativeWindowPtr = NativeWindowHelper.getNativeWindowFromSurface(surface)
                if (nativeWindowPtr == 0L) {
                    surface.release()
                    producer?.release()
                    surfaceTextureEntry?.release()
                    result.error(
                        "NATIVE_WINDOW_UNAVAILABLE",
                        "Failed to acquire a native window from the surface",
                        null
                    )
                    return
                }

                val textureEntry = TextureEntry(
                    producer,
                    surfaceTextureEntry,
                    surface,
                    nativeWindowPtr
                )
                textures[flutterTextureId] = textureEntry
                producer?.setCallback(
                    object : TextureRegistry.SurfaceProducer.Callback {
                        override fun onSurfaceCleanup() {
                            val entry = textures[flutterTextureId] ?: return
                            val surfaceToRelease = entry.surface
                            val windowToRelease = entry.nativeWindow
                            entry.surface = null
                            entry.nativeWindow = 0L
                            notifyDartBeforeReleasingSurface(
                                "onSurfaceCleanup",
                                flutterTextureId,
                                surfaceToRelease,
                                windowToRelease
                            )
                        }

                        override fun onSurfaceAvailable() {
                            val entry = textures[flutterTextureId] ?: return
                            val newSurface = producer.getSurface()
                            if (!newSurface.isValid) {
                                newSurface.release()
                                channel.invokeMethod(
                                    "onSurfaceError",
                                    listOf(
                                        flutterTextureId,
                                        "Replacement surface is invalid"
                                    )
                                )
                                return
                            }

                            val newNativeWindowPtr =
                                NativeWindowHelper.getNativeWindowFromSurface(newSurface)
                            if (newNativeWindowPtr == 0L) {
                                newSurface.release()
                                channel.invokeMethod(
                                    "onSurfaceError",
                                    listOf(
                                        flutterTextureId,
                                        "Failed to acquire replacement native window"
                                    )
                                )
                                return
                            }

                            val surfaceToRelease = entry.surface
                            val windowToRelease = entry.nativeWindow
                            entry.surface = newSurface
                            entry.nativeWindow = newNativeWindowPtr
                            notifyDartBeforeReleasingSurface(
                                "onSurfaceAvailable",
                                listOf(
                                    flutterTextureId,
                                    newNativeWindowPtr
                                ),
                                surfaceToRelease,
                                windowToRelease
                            )
                        }
                    }
                )
                result.success(listOf(flutterTextureId, flutterTextureId, nativeWindowPtr))
            }
            "destroyTexture" -> {
                val textureId = (call.arguments as Int).toLong()
                val textureEntry = textures.remove(textureId)
                if (textureEntry != null) {
                    // Dart has destroyed (and, with the flush in
                    // FFIFilamentApp.destroySwapChain, drained) the swapchain.
                    // Stop surface callbacks now; free the buffers later.
                    textureEntry.surfaceProducer?.setCallback(null)
                    scheduleRelease(textureEntry)
                    result.success(true)
                } else {
                    result.error("TEXTURE_NOT_FOUND", "Texture with id $textureId not found", null)
                }
            }
            "markTextureFrameAvailable" -> {
                val textureId = (call.arguments as Int).toLong()
                val textureEntry = textures[textureId]
                if (textureEntry != null) {
                    result.success(null)
                } else {
                    result.error("TEXTURE_NOT_FOUND", "Texture with id $textureId not found", null)
                }
            }
        "getRenderingContext" -> {
          // The Dart registry reads the driver platform and the shared context
          // in ONE channel call so both are captured under a single native
          // lock on multi-engine platforms. Android's Filament OpenGL backend
          // creates its own context, so both values are null — the same
          // answers the legacy getDriverPlatform/getSharedContext handlers
          // below have always returned.
          result.success(listOf(null, null))
        }
        "getDriverPlatform" -> {
          result.success(null)
        }
        "getSharedContext" -> {
          result.success(null)
        }
        else -> {
          result.notImplemented()
        }
    }
}

  override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {
      channel.setMethodCallHandler(null)
        // Release all textures
        for ((_, textureEntry) in textures) {
            textureEntry.release()
        }
        textures.clear()
        for (entry in pendingReleases) {
            entry.release()
        }
        pendingReleases.clear()
  }

  
  override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
    onAttachedToActivity(binding)
  }

  override fun onDetachedFromActivityForConfigChanges() {
    onDetachedFromActivity()
  }

  override fun onDetachedFromActivity() {
    lifecycle = null
  }

}
