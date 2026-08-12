import 'dart:async';

import 'package:flutter/material.dart' hide View;
import 'dart:ui_web' as ui_web;
import 'package:web/web.dart' as web;

import '../../../platform/src/thermion_flutter_plugin_web.dart';

/// Composites the engine's `#thermion_canvas` into the Flutter widget tree as
/// an HTML platform view.
///
/// The canvas element is created (and transferred to the render pthread) by
/// [ThermionFlutterPluginImpl.initialize] long before any widget mounts, and
/// there is exactly ONE canvas per page. Platform-view slots therefore never
/// receive the canvas directly: each slot gets its own stable host `div`, and
/// [_CanvasOwnership] moves the canvas between hosts so that
///
/// * the newest mounted view owns the canvas,
/// * unmounting an owner hands the canvas to the surviving most-recent host
///   (so a transient double-mount recovers instead of blanking the first
///   view),
/// * a stale dispose can never evict a newer owner (releases are by token),
/// * a host mounted before the engine initialized adopts the canvas when it
///   appears (or when the engine recreates it), instead of staying inert.
///
/// Input contract: the canvas is `pointer-events: none`. All input reaches
/// Flutter's event layer (ThermionListenerWidget etc.); DOM listeners attached
/// directly to the canvas will not fire while it is adopted.
class ThermionCanvasView extends StatefulWidget {
  const ThermionCanvasView({super.key});

  @override
  State<ThermionCanvasView> createState() => _ThermionCanvasViewState();
}

/// Moves the singleton engine canvas between platform-view host divs.
class _CanvasOwnership {
  static final List<int> _hostOrder = <int>[];
  static final Map<int, web.HTMLElement> _hosts = <int, web.HTMLElement>{};
  static StreamSubscription<web.HTMLCanvasElement?>? _canvasSubscription;

  static void registerHost(int token, web.HTMLElement host) {
    _hosts[token] = host;
    _hostOrder.remove(token);
    _hostOrder.add(token);
    _canvasSubscription ??= ThermionFlutterPluginImpl.canvasChanges.listen(
      (_) => _syncOwnership(),
    );
    _syncOwnership();
  }

  static void releaseHost(int token) {
    final wasCurrent = _hostOrder.isNotEmpty && _hostOrder.last == token;
    _hostOrder.remove(token);
    _hosts.remove(token);
    if (!wasCurrent) {
      // A stale release must not disturb the current owner.
      return;
    }
    _syncOwnership();
  }

  /// Adopt the current canvas into the newest live host, or park it when no
  /// host remains. Idempotent; safe to call on every ownership/canvas change.
  static void _syncOwnership() {
    final canvas = ThermionFlutterPluginImpl.canvasElement;
    if (canvas == null) {
      return;
    }
    if (_hostOrder.isEmpty) {
      _park(canvas);
      return;
    }
    final owner = _hosts[_hostOrder.last]!;
    if (canvas.parentElement == owner) {
      return;
    }
    canvas.style
      ..position = ''
      ..zIndex = ''
      // Input stays a Flutter concern: gestures must reach the framework's
      // event capture layer, not the DOM canvas.
      ..pointerEvents = 'none'
      ..display = 'block'
      ..width = '100%'
      ..height = '100%';
    owner.appendChild(canvas);
  }

  /// Return the canvas to its hidden parking spot on `document.body` so
  /// `getElementById` keeps finding it and the engine keeps presenting into a
  /// live element between widget mounts.
  static void _park(web.HTMLCanvasElement canvas) {
    canvas.style
      ..position = 'fixed'
      ..zIndex = '-1'
      ..pointerEvents = 'none';
    web.document.body?.appendChild(canvas);
  }
}

class _ThermionCanvasViewState extends State<ThermionCanvasView> {
  static const _viewType = 'thermion-canvas';
  static bool _factoryRegistered = false;
  static int _nextToken = 0;

  late final int _token = _nextToken++;

  @override
  void initState() {
    super.initState();
    if (!_factoryRegistered) {
      ui_web.platformViewRegistry.registerViewFactory(_viewType, (
        int viewId, {
        Object? params,
      }) {
        final host = web.document.createElement('div') as web.HTMLElement;
        host.style
          ..width = '100%'
          ..height = '100%';
        final token = params is int ? params : -1;
        _CanvasOwnership.registerHost(token, host);
        return host;
      });
      _factoryRegistered = true;
    }
  }

  @override
  void dispose() {
    _CanvasOwnership.releaseHost(_token);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return HtmlElementView(viewType: _viewType, creationParams: _token);
  }
}
