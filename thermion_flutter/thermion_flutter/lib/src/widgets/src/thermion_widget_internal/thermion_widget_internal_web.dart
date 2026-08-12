import 'package:flutter/material.dart' hide View;
import 'dart:ui_web' as ui_web;
import 'package:web/web.dart' as web;

import '../../../platform/src/thermion_flutter_plugin_web.dart';

/// Composites the engine's `#thermion_canvas` into the Flutter widget tree as
/// an HTML platform view.
///
/// The canvas element is created (and transferred to the render pthread) by
/// [ThermionFlutterPluginImpl.initialize] long before any widget mounts, so
/// this widget does not create a canvas — it *adopts* the existing placeholder
/// element into the platform-view slot, and re-parks it back onto
/// `document.body` (hidden behind the page) when the widget is disposed. The
/// OffscreenCanvas link survives reparenting, so presented frames keep
/// flowing to wherever the placeholder element currently lives.
///
/// One engine canvas exists per page, so at most one [ThermionCanvasView]
/// can be mounted at a time; a second concurrent mount would steal the
/// canvas from the first slot.
class ThermionCanvasView extends StatefulWidget {
  const ThermionCanvasView({super.key});

  @override
  State<ThermionCanvasView> createState() => _ThermionCanvasViewState();
}

class _ThermionCanvasViewState extends State<ThermionCanvasView> {
  static const _viewType = 'thermion-canvas';
  static bool _factoryRegistered = false;

  @override
  void initState() {
    super.initState();
    if (!_factoryRegistered) {
      ui_web.platformViewRegistry.registerViewFactory(_viewType, (int _) {
        final canvas = _resolveCanvas();
        if (canvas == null) {
          // The engine has not been initialized yet — nothing to show.
          // The slot gets an inert element; the next mount (after
          // initialize) adopts the real canvas.
          return web.document.createElement('div');
        }
        _adopt(canvas);
        return canvas;
      });
      _factoryRegistered = true;
    }
  }

  static web.HTMLCanvasElement? _resolveCanvas() =>
      ThermionFlutterPluginImpl.canvasElement ??
      web.document.getElementById('thermion_canvas')
          as web.HTMLCanvasElement?;

  /// Clear the parked-behind-the-page placement; inside the platform-view
  /// slot the canvas is positioned by the slot itself. The plugin keeps
  /// owning the CSS pixel dimensions (logical px, backing store = physical
  /// px), which always match the widget bounds that produced them.
  static void _adopt(web.HTMLCanvasElement canvas) {
    canvas.style
      ..position = ''
      ..zIndex = ''
      // Input stays a Flutter concern: gestures must reach the framework's
      // event capture layer, not the DOM canvas.
      ..pointerEvents = 'none'
      ..display = 'block';
  }

  /// Return the canvas to its hidden parking spot so `getElementById`
  /// keeps finding it and the engine can keep presenting into a live
  /// element between widget mounts.
  static void _park(web.HTMLCanvasElement canvas) {
    canvas.style
      ..position = 'fixed'
      ..zIndex = '-1';
    web.document.body?.appendChild(canvas);
  }

  @override
  void dispose() {
    final canvas = _resolveCanvas();
    if (canvas != null) {
      _park(canvas);
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return const HtmlElementView(viewType: _viewType);
  }
}
