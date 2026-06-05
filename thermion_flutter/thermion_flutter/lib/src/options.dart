import 'package:thermion_dart/thermion_dart.dart';

class ThermionFlutterOptions {
  final String? uberarchivePath;

  final WebOptions webOptions;
  final NativeOptions nativeOptions;

  const ThermionFlutterOptions(
      {this.webOptions = const WebOptions(),
      this.nativeOptions = const NativeOptions(),
      this.uberarchivePath});
}

class NativeOptions {
  final Backend? backend;

  /// The format to use for the default render target color attachment.
  /// Currently only applicable on iOS/macOS.
  final TextureFormat renderTargetColorTextureFormat;

  /// The format to use for the default render target depth attachment.
  /// Currently only applicable on iOS/macOS.
  final TextureFormat renderTargetDepthTextureFormat;

  /// If true, create the highlight overlay system at viewer initialization.
  /// The overlay persists for the lifetime of the viewer.
  final bool createOverlay;

  const NativeOptions(
      {this.backend,
      this.renderTargetColorTextureFormat = TextureFormat.RGBA8,
      this.renderTargetDepthTextureFormat = TextureFormat.DEPTH24_STENCIL8,
      this.createOverlay = false});
}

class WebOptions {
  final bool createCanvas;
  final bool importCanvasAsWidget;
  final String jsPath;

  /// Filament backend to use on the web. If null, defaults to [Backend.OPENGL]
  /// (WebGL2). Pass [Backend.WEBGPU] to use the WebGPU backend; the caller is
  /// responsible for verifying availability first via [WebGpu.isSupported].
  final Backend? backend;

  const WebOptions(
      {this.importCanvasAsWidget = false,
      this.createCanvas = true,
      this.jsPath = "./thermion_dart.js",
      this.backend});
}
