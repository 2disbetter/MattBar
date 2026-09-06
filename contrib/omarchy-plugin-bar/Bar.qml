import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Optional QS-hosted plugin strip. Same window/auto-hide family as the
// 1.40 row that was known-visible: MattBar on the screen edge, this bar
// on the desktop-facing side. L/C/R/M zones live inside that shell.
// Overlay/panel/menu plugins stay on MattBar's Plugins chip.
//
// Host injection (configureBar, after construction — do NOT mark these
// required or a third-party bar never loads):
//   omarchyPath, barWidgetRegistry, barConfig, shell, manifest, pluginRegistry
//
// Hover family lives in two runtime files MattBar owns:
//   $XDG_RUNTIME_DIR/mattbar.plugin-bar
//     on=0|1  expanded=0|1  pinned=0|1  position=top|bottom|left|right
//     offset=<px inward to clear MattBar>  height=<row thickness>
//   $XDG_RUNTIME_DIR/mattbar.plugin-bar-hover
//     0 or 1, written here on pointer enter/leave so MattBar hold_open()
//     treats the row as part of the same auto-hide family.
Item {
  id: root

  property var shell: null
  property var manifest: null
  property var barConfig: null
  property var barWidgetRegistry: null
  property var pluginRegistry: null
  property string omarchyPath: ""

  // Notifications read barHidden first and skip barSize when true.
  // Keep barSize as the real row height (1.40 did this and the strip
  // mapped). Do not set exclusiveZone on the PanelWindow.
  readonly property bool barHidden: true
  readonly property int barSize: Math.max(1, rowHeight)
  readonly property int widgetSize: Math.max(1, rowHeight)
  property string fontFamily: "JetBrainsMono Nerd Font"
  property color foreground: Qt.rgba(0.870, 0.880, 0.910, 1)
  property color background: Qt.rgba(0.071, 0.075, 0.094, 0.94)
  property color urgent: Qt.rgba(0.940, 0.380, 0.360, 1)
  property color barForeground: foreground
  property color themeForeground: foreground
  property color themeContrastForeground: Qt.rgba(1, 1, 1, 1)

  // Defaults on so the first frame is visible if the state file is late.
  // parseState() ignores empty FileView payloads so a missing file cannot
  // flip these back to hidden.
  property bool rowOn: true
  property bool expanded: true
  property bool pinned: false
  property string position: "top"
  property int offset: 32
  property int rowHeight: 28
  property var leftIds: []
  property var centerIds: []
  property var rightIds: []
  property var moreIds: []
  property var widgetIds: []
  property var activePopout: null
  property bool hoverLatched: false
  property bool moreOpen: false

  readonly property bool vertical: position === "left" || position === "right"
  readonly property bool mounted: rowOn
  readonly property bool showRow: mounted && (expanded || pinned)

  onShowRowChanged: if (!showRow) moreOpen = false
  onMoreOpenChanged: if (moreOpen) writeHover(true)

  function runtimeDir() {
    var d = Quickshell.env("XDG_RUNTIME_DIR")
    return d && d.length ? d : "/tmp"
  }

  function parseColor(s, fallback) {
    if (!s) return fallback
    var p = String(s).split(",")
    if (p.length < 3) return fallback
    var r = parseFloat(p[0]), g = parseFloat(p[1]), b = parseFloat(p[2])
    var a = p.length > 3 ? parseFloat(p[3]) : 1
    if (isNaN(r) || isNaN(g) || isNaN(b) || isNaN(a)) return fallback
    return Qt.rgba(r, g, b, a)
  }

  function parseState(text) {
    text = String(text || "")
    if (!text.length) return
    var map = ({})
    text.split("\n").forEach(function (line) {
      var i = line.indexOf("=")
      if (i <= 0) return
      map[line.slice(0, i).trim()] = line.slice(i + 1).trim()
    })
    if (!("on" in map) && !("expanded" in map)) return
    function num(k, fallback) {
      var n = parseInt(map[k], 10)
      return isNaN(n) ? fallback : n
    }
    if ("on" in map) rowOn = map["on"] === "1"
    if ("expanded" in map) expanded = map["expanded"] === "1"
    if ("pinned" in map) pinned = map["pinned"] === "1"
    var pos = map["position"] || ""
    if (pos === "top" || pos === "bottom" || pos === "left" || pos === "right")
      position = pos
    if ("offset" in map) offset = Math.max(0, num("offset", offset))
    var h = num("height", 0)
    if (h > 0) rowHeight = h
    if (map["font"]) fontFamily = map["font"]
    if (map["fg"]) foreground = parseColor(map["fg"], foreground)
    if (map["bg"]) background = parseColor(map["bg"], background)
    if (map["urgent"]) urgent = parseColor(map["urgent"], urgent)
    if (!showRow) moreOpen = false
  }

  function takeIds(arr) {
    var ids = []
    if (!arr) return ids
    if (typeof arr === "string") {
      arr.split(",").forEach(function (s) {
        s = String(s).trim()
        if (s) ids.push(s)
      })
      return ids
    }
    if (!arr.length) return ids
    for (var i = 0; i < arr.length; i++) {
      var e = arr[i]
      if (e === null || e === undefined) continue
      if (typeof e === "string") {
        if (e) ids.push(e)
        continue
      }
      var id = e.id || e.plugin || e.module || ""
      if (id) ids.push(String(id))
    }
    return ids
  }

  function dedupe(list) {
    var seen = ({})
    var out = []
    for (var i = 0; i < list.length; i++) {
      var id = String(list[i])
      if (!id || seen[id]) continue
      seen[id] = true
      out.push(id)
    }
    return out
  }

  function refreshWidgets() {
    var cfg = barConfig
    var layout = cfg && cfg.layout ? cfg.layout : {}
    leftIds = dedupe(takeIds(layout.left))
    centerIds = dedupe(takeIds(layout.center))
    rightIds = dedupe(takeIds(layout.right))
    moreIds = dedupe(takeIds(cfg && cfg.more))
    // 1.40 also accepted a flat widgets list — keep that fallback so a
    // sidecar that only filled layout.left still renders.
    if (leftIds.length + centerIds.length + rightIds.length + moreIds.length === 0)
      leftIds = dedupe(takeIds(cfg && cfg.widgets))
    widgetIds = dedupe(leftIds.concat(centerIds).concat(rightIds).concat(moreIds))
    if (moreIds.length === 0) moreOpen = false
  }

  function registryComponent(id) {
    if (!barWidgetRegistry || !barWidgetRegistry.widgets) return null
    var entry = barWidgetRegistry.widgets[String(id)]
    return entry && entry.component ? entry.component : null
  }

  function findPanelWidget(pluginId) {
    var id = String(pluginId || "")
    if (!id) return null
    for (var i = 0; i < moduleSlots.length; i++) {
      var slot = moduleSlots[i]
      if (!slot || !slot.activeItem) continue
      if (slot.moduleName !== id) continue
      var item = slot.activeItem
      if (typeof item.open !== "function" || typeof item.close !== "function")
        continue
      if (item.opened === undefined) continue
      return item
    }
    return null
  }

  function summonBarWidget(pluginId) {
    var item = findPanelWidget(pluginId)
    if (!item || typeof item.open !== "function") return false
    item.open()
    return true
  }

  function hideBarWidget(pluginId) {
    var item = findPanelWidget(pluginId)
    if (!item || typeof item.close !== "function") return false
    item.close()
    return true
  }

  function isBarWidgetOpen(pluginId) {
    var item = findPanelWidget(pluginId)
    return !!item && item.opened === true
  }

  function run(command) {
    if (!command) return
    Quickshell.execDetached(["sh", "-c", String(command)])
  }

  function showTooltip(target, text) { }
  function hideTooltip(target) { }

  function requestPopout(owner) {
    if (activePopout && activePopout !== owner) {
      if (typeof activePopout.closeForPopoutSwitch === "function")
        activePopout.closeForPopoutSwitch()
      else if (typeof activePopout.close === "function")
        activePopout.close()
    }
    activePopout = owner
  }

  function releasePopout(owner) {
    if (activePopout === owner) activePopout = null
  }

  property var moduleSlots: []

  function registerSlot(slot) {
    if (!slot) return
    var next = moduleSlots.slice()
    if (next.indexOf(slot) < 0) next.push(slot)
    moduleSlots = next
  }

  function unregisterSlot(slot) {
    moduleSlots = moduleSlots.filter(function (s) { return s !== slot })
  }

  function writeHover(on) {
    if (hoverLatched === !!on) return
    hoverLatched = !!on
    hoverFile.setText(on ? "1" : "0")
  }

  function parkMargin(edge) {
    if (!root.mounted) return 0
    if (root.position !== edge) return 0
    if (root.showRow) return root.offset
    return -(root.rowHeight + 8)
  }

  onBarConfigChanged: refreshWidgets()
  onBarWidgetRegistryChanged: refreshWidgets()
  Component.onCompleted: {
    refreshWidgets()
    if (barConfig && barConfig.position)
      position = String(barConfig.position)
    stateFile.reload()
  }
  Component.onDestruction: writeHover(false)

  FileView {
    id: stateFile
    path: root.runtimeDir() + "/mattbar.plugin-bar"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.parseState(typeof text === "function" ? text() : text)
  }

  FileView {
    id: hoverFile
    path: root.runtimeDir() + "/mattbar.plugin-bar-hover"
    preload: false
    printErrors: false
    atomicWrites: false
  }

  Timer {
    interval: 750
    running: true
    repeat: true
    onTriggered: {
      root.refreshWidgets()
      stateFile.reload()
    }
  }

  Variants {
    model: Quickshell.screens
    delegate: Component {
      PanelWindow {
        id: win
        required property var modelData
        screen: modelData

        visible: root.mounted
        implicitWidth: root.vertical
                       ? (root.showRow ? root.rowHeight : 1)
                       : (modelData && modelData.width ? modelData.width : 1920)
        implicitHeight: root.vertical
                        ? (modelData && modelData.height ? modelData.height : 1080)
                        : (root.showRow ? root.rowHeight : 1)

        anchors {
          top: root.position === "top" || root.vertical
          bottom: root.position === "bottom" || root.vertical
          left: root.position === "left" || !root.vertical
          right: root.position === "right" || !root.vertical
        }
        margins {
          top: root.parkMargin("top")
          bottom: root.parkMargin("bottom")
          left: root.parkMargin("left")
          right: root.parkMargin("right")
        }

        exclusionMode: ExclusionMode.Ignore
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "mattbar-plugin-bar"
        color: "transparent"

        HoverHandler {
          onHoveredChanged: root.writeHover(hovered || root.moreOpen)
          Component.onDestruction: if (hovered) root.writeHover(false)
        }

        Rectangle {
          anchors.fill: parent
          color: root.background
          visible: root.showRow
        }

        Item {
          id: rowBox
          visible: !root.vertical && root.showRow
          anchors.fill: parent
          anchors.leftMargin: 8
          anchors.rightMargin: 8

          Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Repeater {
              model: root.leftIds
              delegate: Slot {
                height: root.rowHeight
                width: Math.max(implicitWidth, 16)
                moduleName: modelData
              }
            }
          }
          Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Repeater {
              model: root.centerIds
              delegate: Slot {
                height: root.rowHeight
                width: Math.max(implicitWidth, 16)
                moduleName: modelData
              }
            }
          }
          Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Repeater {
              model: root.rightIds
              delegate: Slot {
                height: root.rowHeight
                width: Math.max(implicitWidth, 16)
                moduleName: modelData
              }
            }
            MoreChip {
              visible: root.moreIds.length > 0
              width: 22
              height: root.rowHeight
            }
          }
        }

        Item {
          id: colBox
          visible: root.vertical && root.showRow
          anchors.fill: parent
          anchors.topMargin: 8
          anchors.bottomMargin: 8

          Column {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Repeater {
              model: root.leftIds
              delegate: Slot {
                width: root.rowHeight
                height: Math.max(implicitHeight, 16)
                moduleName: modelData
              }
            }
          }
          Column {
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Repeater {
              model: root.centerIds
              delegate: Slot {
                width: root.rowHeight
                height: Math.max(implicitHeight, 16)
                moduleName: modelData
              }
            }
          }
          Column {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            MoreChip {
              visible: root.moreIds.length > 0
              width: root.rowHeight
              height: 22
            }
            Repeater {
              model: root.rightIds
              delegate: Slot {
                width: root.rowHeight
                height: Math.max(implicitHeight, 16)
                moduleName: modelData
              }
            }
          }
        }

        // Overflow stays inside this layer-shell window. PopupWindow is
        // what broke the 1.41 load on some QS builds.
        Rectangle {
          id: morePop
          visible: root.moreOpen && root.showRow && root.moreIds.length > 0
          anchors.left: parent.left
          anchors.right: parent.right
          height: Math.max(32, root.moreIds.length * 28)
          y: root.position === "bottom" ? -height : parent.height
          color: root.background
          radius: 8
          HoverHandler {
            onHoveredChanged: root.writeHover(hovered || root.moreOpen)
          }
          Column {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 4
            Repeater {
              model: root.moreIds
              delegate: Slot {
                width: parent ? parent.width : 24
                height: 24
                moduleName: modelData
              }
            }
          }
        }
      }
    }
  }

  component MoreChip: Item {
    implicitWidth: 22
    implicitHeight: 22
    Text {
      anchors.centerIn: parent
      text: "\u22EF"
      color: root.moreOpen ? root.urgent : root.foreground
      font.family: root.fontFamily
      font.pixelSize: Math.max(11, root.rowHeight - 12)
    }
    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      onClicked: root.moreOpen = !root.moreOpen
    }
  }

  component Slot: Item {
    id: slot
    property string moduleName: ""
    property var activeItem: loader.item

    implicitWidth: loader.item ? (loader.item.implicitWidth || loader.item.width || 24) : 24
    implicitHeight: loader.item ? (loader.item.implicitHeight || loader.item.height || 24) : 24

    readonly property var registryComponent: {
      var w = root.barWidgetRegistry ? root.barWidgetRegistry.widgets : null
      if (!w) return null
      var entry = w[String(slot.moduleName)]
      return entry && entry.component ? entry.component : null
    }

    Loader {
      id: loader
      anchors.fill: parent
      active: slot.registryComponent !== null
      sourceComponent: slot.registryComponent
      onLoaded: slot.inject()
    }

    function inject() {
      var target = loader.item
      if (!target) return
      if ("bar" in target) target.bar = root
      if ("moduleName" in target) target.moduleName = slot.moduleName
      if ("settings" in target) target.settings = ({})
      if ("shell" in target) target.shell = root.shell
    }

    Component.onCompleted: root.registerSlot(slot)
    Component.onDestruction: root.unregisterSlot(slot)
  }
}
