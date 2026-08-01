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
/// fundamentally different way on each platform, and only the web path needs a
/// release step:
///
///  * native (`ffi.dart`): `Uint8List.address` is dart:ffi's zero-copy address
///    of the Dart-heap backing store. It is only legal as a direct argument to
///    a leaf `@Native` call, so the call site cannot be factored any further.
///  * web (here): `Uint8List.address` (`ffigen_js` `Uint8ListExtension`) copies
///    the bytes into the WASM heap. For buffers >= 32 KiB — i.e. every real
///    texture — `_getPointer` takes the `malloc` branch. Nothing else ever
///    releases that allocation, and the WASM heap is built with
///    `-sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=512mb`, so a media-rate upload
///    loop exhausts the whole heap in seconds.
///
/// Releasing after the callback resolves is safe: `Texture_setImage`
/// (`native/src/c_api/TTexture.cpp`) copies `data` into its own
/// `std::vector<uint8_t>` *synchronously* — "the texture upload is async, so we
/// need to copy the buffer" — and only then does the render-thread task invoke
/// `onComplete`. By the time this future completes, native no longer refers to
/// the staged copy.
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
  final ptr = buffer.address;
  final size = buffer.lengthInBytes;
  try {
    return await withBoolCallback((cb) {
      Texture_setImageRenderThread(
        tEngine,
        tTexture,
        level,
        ptr,
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
    // No-op for the < 32 KiB stack-allocated branch (the pointer is not in
    // `_heapAllocations`); frees the malloc'd copy otherwise.
    ptr.free();
  }
}

Future<bool> withBoolCallback(
  Function(Pointer<NativeFunction<Void Function(Bool)>>) func,
) async {
  final completer = Completer<bool>();
  // ignore: prefer_function_declarations_over_variables
  void Function(int) callback = (int result) {
    completer.complete(result == 1);
  };

  final onComplete_interopFnPtr = callback.addFunction();

  func.call(onComplete_interopFnPtr.cast());

  while (!completer.isCompleted) {
    _NativeLibrary.instance._execute_queue();
    await Future.delayed(Duration(milliseconds: 1));
  }

  return completer.future;
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
  func.call(ptr);
  while (!completer.isCompleted) {
    _NativeLibrary.instance._execute_queue();
    await Future.delayed(Duration(milliseconds: 1));
  }
  return completer.future;
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
  func.call(ptr);
  while (!completer.isCompleted) {
    _NativeLibrary.instance._execute_queue();
    await Future.delayed(Duration(milliseconds: 1));
  }
  return completer.future;
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
  func.call(ptr.cast());
  await completer.future;
  return completer.future;
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
