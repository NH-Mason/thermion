import 'dart:async';
import 'dart:typed_data';

import 'thermion_dart_js_interop.g.dart';
import 'dart:js_interop';

export 'dart:typed_data';

export 'thermion_dart_js_interop.g.dart';
export 'dart:js_interop';
export 'dart:js_interop_unsafe';

const FILAMENT_SINGLE_THREADED = true;
const FILAMENT_WASM = true;
const IS_WINDOWS = false;

extension type _NativeLibrary(NativeLibrary _) implements JSObject {
  static _NativeLibrary get instance =>
      NativeLibrary.instance as _NativeLibrary;

  external void _execute_queue();

  /// Emscripten's `malloc`/`free` exports.
  ///
  /// `ffigen_js` declares these on [NativeLibrary] but keeps them library
  /// private, and its public `malloc` helper does not register the result in
  /// the allocation set that `Pointer.free()` consults — so a pointer obtained
  /// that way can never be released. Re-declaring them here gives this library
  /// a heap allocation it fully owns.
  @JS('_malloc')
  external Pointer<Uint8> mallocBytes(int numBytes);

  @JS('_free')
  external void freeBytes(Pointer<Uint8> ptr);
}

/// A WASM-heap staging buffer holding a private copy of some Dart bytes.
///
/// Every byte handed to a *render thread* native entry point must live in the
/// WASM heap — never on the WASM stack — and must stay valid until native
/// signals that it is done with it. See [Texture_setImageFromBuffer] for why
/// `Uint8List.address` cannot provide that guarantee.
class _StagedBytes {
  final Pointer<Uint8> pointer;
  final int size;
  bool _released = false;

  _StagedBytes._(this.pointer, this.size);

  /// Allocates `source.lengthInBytes` on the WASM heap and copies [source]
  /// into it. The caller owns the result and must call [release].
  static _StagedBytes copyOf(Uint8List source) {
    final size = source.lengthInBytes;
    if (size == 0) {
      return _StagedBytes._(nullptr.cast<Uint8>(), 0);
    }
    final pointer = _NativeLibrary.instance.mallocBytes(size);
    if (pointer.address == 0) {
      throw StateError(
        'Thermion: failed to allocate a $size byte WASM staging buffer',
      );
    }
    NativeLibrary.instance.writeArrayToMemory(source.toJS, pointer);
    return _StagedBytes._(pointer, size);
  }

  void release() {
    if (_released) {
      return;
    }
    _released = true;
    if (size != 0) {
      _NativeLibrary.instance.freeBytes(pointer);
    }
  }
}

typedef IntPtrList = Int32List;
typedef Utf8 = Char;
typedef Float = Float32;
typedef Double = Float64;
typedef Bool = bool;

class FinalizableUint8List {
  final Pointer name;
  final Uint8List data;

  FinalizableUint8List(this.name, this.data);
}

class CallbackHolder<T extends Function> {
  final Pointer<NativeFunction<T>> pointer;

  CallbackHolder(this.pointer);

  void dispose() {
    pointer.dispose();
  }
}

extension DPCF on DartPickCallbackFunction {
  CallbackHolder<DartPickCallbackFunction> asCallback() {
    final ptr = addFunction<DartPickCallbackFunction>(this.toJS, "viiffff");
    final cbh = CallbackHolder(ptr);
    return cbh;
  }
}

extension GPFBP on void Function(int, double, double, double) {
  CallbackHolder<GizmoPickCallbackFunction> asCallback() {
    final ptr = addFunction<GizmoPickCallbackFunction>(this.toJS, "viddd");
    return CallbackHolder(ptr);
  }
}

extension VFCB on void Function() {
  CallbackHolder<void Function()> asCallback() {
    final ptr = addFunction<void Function()>(this.toJS, "v");
    return CallbackHolder(ptr);
  }
}

int _lastRequestId = 0;
final _completers = <int, Completer>{};
void Function(int) _voidCallback = (int requestId) {
  _completers[requestId]!.complete();
  _completers.remove(requestId);
};

final _voidCallbackPtr = _voidCallback.addFunction();

Future<void> withVoidCallback(
  Function(int, Pointer<NativeFunction<Void Function(int)>>) func,
) async {
  final completer = Completer();
  var requestId = _lastRequestId;
  _lastRequestId++;

  _completers[requestId] = completer;

  func.call(requestId, _voidCallbackPtr.cast());
  while (!completer.isCompleted) {
    _NativeLibrary.instance._execute_queue();
    await Future.delayed(Duration(milliseconds: 1));
  }
  await completer.future;
}

Future<Pointer<T>> withPointerCallback<T extends NativeType>(
  Function(Pointer<NativeFunction<Void Function(Pointer<T>)>>) func,
) async {
  final completer = Completer<Pointer<T>>();
  // ignore: prefer_function_declarations_over_variables
  void Function(Pointer<T>) callback = (Pointer<T> ptr) {
    completer.complete(ptr.cast<T>());
  };

  final onComplete_interopFnPtr = callback.addFunction();

  func.call(onComplete_interopFnPtr.cast());

  while (!completer.isCompleted) {
    _NativeLibrary.instance._execute_queue();
    await Future.delayed(Duration(milliseconds: 1));
  }

  var ptr = await completer.future;
  onComplete_interopFnPtr.dispose();

  return ptr;
}

/// Uploads [buffer] into [tTexture] on the render thread.
///
/// This wrapper exists because a `Uint8List` reaches native memory in a
/// fundamentally different way on each platform, and only the web path needs
/// explicit staging:
///
///  * native (`ffi.dart`): `Uint8List.address` is dart:ffi's zero-copy address
///    of the Dart-heap backing store. It is only legal as a direct argument to
///    a leaf `@Native` call, so the call site cannot be factored any further.
///  * web (here): the bytes must be copied into the WASM heap by hand.
///
/// `Uint8List.address` (`ffigen_js` `Uint8ListExtension`) is *not* usable for a
/// render-thread call, for two independent reasons:
///
///  1. For buffers < 32 KiB, `ffigen_js._getPointer` allocates on the WASM
///     *stack*. `Texture_setImageRenderThread`
///     (`native/src/c_api/ThermionDartRenderThreadApi.cpp`) captures `data` by
///     value into a `std::packaged_task` and the copy into native storage does
///     not happen until that task runs (`TTexture.cpp`, `Texture_setImage`).
///     Handing deferred native work a stack address is only accidentally safe
///     today because nothing in the generated interop ever calls
///     `stackRestore`; it becomes a use-after-free the moment any caller does.
///  2. For buffers >= 32 KiB it takes the `malloc` branch, and the pointer can
///     only be released through `Pointer.free()` — which is a silent no-op for
///     anything `_getPointer` did not register.
///
/// So stage an explicitly owned heap copy instead. Releasing it once the
/// callback resolves is correct: `Texture_setImage` copies `data` into its own
/// `std::vector<uint8_t>` — "the texture upload is async, so we need to copy
/// the buffer" — *before* invoking `onComplete`, so the callback is the
/// ownership boundary even though the copy itself is deferred.
///
/// (Historical note: an earlier version of this comment claimed the native
/// copy was synchronous with the call, and that the heap was built with
/// `-sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=512mb`. Both are false. The copy
/// is deferred to the render thread as described above, and since 2026-08-12
/// `native/web/CMakeLists.txt` builds with `-sALLOW_MEMORY_GROWTH=1
/// -sINITIAL_MEMORY=512mb -sMAXIMUM_MEMORY=2gb`.)
Future<bool> Texture_setImageFromBuffer(
  Pointer<TEngine> tEngine,
  Pointer<TTexture> tTexture,
  int level,
  Uint8List buffer,
  int xOffset,
  int yOffset,
  int zOffset,
  int width,
  int height,
  int depth,
  int bufferFormat,
  int pixelDataType,
) async {
  final staged = _StagedBytes.copyOf(buffer);
  final size = staged.size;
  try {
    return await withBoolCallback((cb) {
      Texture_setImageRenderThread(
        tEngine,
        tTexture,
        level,
        staged.pointer,
        size,
        xOffset,
        yOffset,
        zOffset,
        width,
        height,
        depth,
        bufferFormat,
        pixelDataType,
        cb,
      );
    });
  } finally {
    // Native has finished copying out of the staging buffer by the time the
    // callback resolves, so the heap copy can go back now.
    staged.release();
  }
}

var _ktx2StagedInputBytes = 0;

/// Creates a KTX2 texture from [data] on the render thread.
///
/// Like [Texture_setImageFromBuffer], this stages an explicitly owned copy on
/// the WASM heap rather than using `Uint8List.address`, so that the buffer is
/// never WASM-stack-resident and can actually be released afterwards.
///
/// Unlike `Texture_setImageRenderThread`, the native entry point here
/// (`Ktx2Reader_createTextureRenderThread`) really does copy into a
/// `std::vector<uint8_t>` *on the calling thread*, before the task is queued —
/// it is the in-repo example of the pattern `Texture_setImageRenderThread`
/// lacks. Releasing after the callback is therefore conservative but correct.
Future<Pointer<TKtx2LoadResult>> Ktx2Reader_createTextureFromBuffer(
  Pointer<TEngine> tEngine,
  Uint8List data,
  int transferFunction,
  int maxMipLevels,
) async {
  final staged = _StagedBytes.copyOf(data);
  final size = staged.size;
  _ktx2StagedInputBytes += size;
  try {
    return await withPointerCallback<TKtx2LoadResult>((cb) {
      Ktx2Reader_createTextureRenderThread(
        tEngine,
        staged.pointer,
        size,
        transferFunction,
        maxMipLevels,
        cb,
      );
    });
  } finally {
    staged.release();
    _ktx2StagedInputBytes -= size;
  }
}

/// Bytes currently staged by pending web KTX2 calls.
///
/// Exposed for ownership tests so successful and unsupported loads can assert
/// that their staging allocation returned to baseline.
int get debugKtx2StagedInputBytes => _ktx2StagedInputBytes;

Future<bool> withBoolCallback(
  Function(Pointer<NativeFunction<Void Function(Bool)>>) func,
) async {
  final completer = Completer<bool>();
  // ignore: prefer_function_declarations_over_variables
  void Function(int) callback = (int result) {
    completer.complete(result == 1);
  };

  final onComplete_interopFnPtr = callback.addFunction();

  try {
    func.call(onComplete_interopFnPtr.cast());

    while (!completer.isCompleted) {
      _NativeLibrary.instance._execute_queue();
      await Future.delayed(Duration(milliseconds: 1));
    }

    return await completer.future;
  } finally {
    // `addFunction` installs a wrapper in the WASM function table; without a
    // matching `removeFunction` every awaited call permanently consumes a
    // table slot (and its JS trampoline). `withPointerCallback` already does
    // this; the bool/float/int variants did not, which leaks once per
    // `Texture.setImage`, per material-parameter write, etc.
    onComplete_interopFnPtr.dispose();
  }
}

Future<double> withFloatCallback(
  void Function(Pointer<NativeFunction<void Function(double)>>) func,
) async {
  final completer = Completer<double>();
  // ignore: prefer_function_declarations_over_variables
  void Function(double) callback = (double result) {
    completer.complete(result);
  };
  var ptr = callback.addFunction();
  try {
    func.call(ptr);
    while (!completer.isCompleted) {
      _NativeLibrary.instance._execute_queue();
      await Future.delayed(Duration(milliseconds: 1));
    }
    return await completer.future;
  } finally {
    ptr.dispose();
  }
}

Future<int> withIntCallback(
  Function(Pointer<NativeFunction<void Function(int)>>) func,
) async {
  final completer = Completer<int>();
  // ignore: prefer_function_declarations_over_variables
  void Function(int) callback = (int result) {
    completer.complete(result);
  };
  var ptr = callback.addFunction();
  try {
    func.call(ptr);
    while (!completer.isCompleted) {
      _NativeLibrary.instance._execute_queue();
      await Future.delayed(Duration(milliseconds: 1));
    }
    return await completer.future;
  } finally {
    ptr.dispose();
  }
}

Pointer<T> allocate<T extends NativeType>(int count) {
  switch (T) {
    case PointerClass:
      return malloc(count * 4);
    case Char:
      return malloc(count);
    default:
      throw Exception(T.toString());
  }
}

Future<int> withUInt32Callback(
  Function(Pointer<NativeFunction<Void Function(int)>>) func,
) async {
  final completer = Completer<int>();
  // ignore: prefer_function_declarations_over_variables
  void Function(int) callback = (int result) {
    completer.complete(result);
  };
  final ptr = callback.addFunction();
  try {
    func.call(ptr.cast());
    return await completer.future;
  } finally {
    ptr.dispose();
  }
}

Future<String> withCharPtrCallback(
  Function(Pointer<NativeFunction<Void Function(Pointer<Char>)>>) func,
) async {
  throw UnimplementedError();
}

extension DartBigIntExtension on int {
  BigInt get toBigInt {
    return BigInt.from(this);
  }
}

Pointer stackSave() => NativeLibrary.instance.stackSave();

void stackRestore(Pointer ptr) =>
    NativeLibrary.instance.stackRestore(ptr.cast());

/// Runs [body] and reclaims every WASM-stack allocation it made.
///
/// The WASM stack pointer only ever moves *down*: `ffigen_js` routes anything
/// under 32 KiB through emscripten's `stackAlloc` (`types.dart`,
/// `_getPointer`, `Struct.stackAlloc`, `make*List`), and `Pointer.free()` is a
/// silent no-op for those addresses. Nothing in the generated interop calls
/// `stackRestore`, so without an explicit scope like this one every small
/// staged argument permanently consumes stack against the fixed
/// `-sSTACK_SIZE` — an unbounded, monotonic leak that aborts the module once
/// it crosses the guard page.
///
/// Contract, identical to the hand-rolled save/restore pairs this replaces:
/// [body] MUST be synchronous, and MUST NOT return or retain anything backed
/// by the stack. In particular a generated struct out-param (`double4x4`,
/// `Aabb3`, `TViewport`, …) is a *view* over WASM memory, not a copy, so it
/// must be converted to a Dart value inside [body] — that is exactly why the
/// generated bindings cannot bracket themselves and the scope has to live
/// here, at the wrapper layer.
T withStackScope<T>(T Function() body) {
  final stackPtr = stackSave();
  try {
    return body();
  } finally {
    stackRestore(stackPtr);
  }
}
