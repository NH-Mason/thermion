import 'package:flutter/material.dart' hide View;

import '../../../platform/src/platform_texture_descriptor.dart';
import 'package:thermion_flutter/thermion_flutter.dart'
    hide Pointer, NativeType, nullptr, NativeLibrary;

import 'thermion_widget_internal_web.dart';

Widget surfaceWidgetBuilder(PlatformTextureDescriptor? descriptor) {
  if (descriptor == null) {
    // No surface allocated yet — hold the layout without painting.
    return const SizedBox.expand();
  }
  if (ThermionFlutterPlugin.instance.options.webOptions.importCanvasAsWidget) {
    return const ThermionCanvasView();
  }
  // Legacy path: the canvas stays parked on document.body behind the page;
  // the widget contributes nothing visible.
  return Container(color: Colors.transparent);
}
