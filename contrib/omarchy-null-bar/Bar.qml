import QtQuick

// Null bar: creates no window/surfaces/timers. Selecting it unloads omarchy.bar.
// Properties mirror the real bar surface that other components read
// (guarded by `shell.bar && ...`): barHidden, barSize, fontFamily.
// summon/hide/isBarWidgetOpen intentionally absent (typeof-guarded no-ops).
Item {
  // set by shell.configureBar() when present; harmless to accept
  property var shell: null
  property var manifest: null
  property var barConfig: null
  property var barWidgetRegistry: null
  property var pluginRegistry: null
  property string omarchyPath: ""

  readonly property bool barHidden: true
  readonly property int barSize: 0
  readonly property string fontFamily: ""

  visible: false
  width: 0
  height: 0
}
