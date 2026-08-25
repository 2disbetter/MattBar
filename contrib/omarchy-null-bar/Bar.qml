import QtQuick

// A bar that isn't there. Creates no window, no surfaces, no timers —
// selecting this as the shell's bar option unloads omarchy.bar entirely.
//
// The properties below mirror the parts of the real bar's surface that
// other shell components read (guarded by `shell.bar && ...`):
//   - notifications: `!shell.bar.barHidden ? Math.max(0, shell.bar.barSize)
//     : defaultBarSize` — barHidden:true routes them to their default edge
//     margin and keeps barSize from ever being read (undefined would NaN
//     the anchor math).
//   - notifications also read fontFamily for popup text.
// summonBarWidget/hideBarWidget/isBarWidgetOpen are intentionally absent;
// their call sites are typeof-guarded and no-op cleanly.
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
