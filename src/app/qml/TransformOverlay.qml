import QtQuick
import MovixStudio

// Recuadro de transformación sobre el visor para el clip seleccionado: arrastrar el interior lo
// mueve y arrastrar una esquina lo escala. Los cambios van a la posición/escala en el cabezal
// (como keyframe si la propiedad está animada).
Item {
    id: overlay

    required property ProjectController project
    property var clipId: project.selection.length === 1 ? project.selection[0] : undefined
    property var geo: ({})
    // Rectángulo del lienzo dentro del visor (mismo ajuste que hace el compositor).
    readonly property real canvasAspect: project.canvasWidth / Math.max(1, project.canvasHeight)
    readonly property rect canvas: {
        const w = width, h = height
        if (w <= 0 || h <= 0)
            return Qt.rect(0, 0, 0, 0)
        if (w / h > canvasAspect)
            return Qt.rect((w - h * canvasAspect) / 2, 0, h * canvasAspect, h)
        return Qt.rect(0, (h - w / canvasAspect) / 2, w, w / canvasAspect)
    }

    function refresh() {
        geo = clipId !== undefined ? project.clipGeometry(clipId) : ({})
    }
    onClipIdChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: overlay.project
        function onHistoryChanged() { overlay.refresh() }
    }
    Connections {
        target: overlay.project.player
        function onPositionChanged() { overlay.refresh() }
    }

    Item {
        id: box
        visible: overlay.geo.visible === true
        readonly property real cx: overlay.canvas.x + (overlay.geo.cx ?? 0.5) * overlay.canvas.width
        readonly property real cy: overlay.canvas.y + (overlay.geo.cy ?? 0.5) * overlay.canvas.height
        readonly property real hw: (overlay.geo.hw ?? 0) * overlay.canvas.width
        readonly property real hh: (overlay.geo.hh ?? 0) * overlay.canvas.height
        x: cx - hw
        y: cy - hh
        width: 2 * hw
        height: 2 * hh
        rotation: overlay.geo.rotation ?? 0

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: 1.5
            border.color: Theme.clipSelected
        }

        // Mover.
        MouseArea {
            anchors.fill: parent
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.SizeAllCursor
            property point origin
            property real startX: 0
            property real startY: 0
            onPressed: (m) => {
                origin = mapToItem(overlay, m.x, m.y)
                startX = (overlay.geo.cx ?? 0.5) - 0.5
                startY = (overlay.geo.cy ?? 0.5) - 0.5
            }
            onPositionChanged: (m) => {
                const p = mapToItem(overlay, m.x, m.y)
                overlay.project.setClipProperty(overlay.clipId, "x", startX + (p.x - origin.x) / overlay.canvas.width)
                overlay.project.setClipProperty(overlay.clipId, "y", startY + (p.y - origin.y) / overlay.canvas.height)
            }
            onReleased: overlay.project.endInteraction()
        }

        // Escalar desde las esquinas (proporcional a la distancia al centro).
        Repeater {
            model: [Qt.point(0, 0), Qt.point(1, 0), Qt.point(0, 1), Qt.point(1, 1)]
            Rectangle {
                required property point modelData
                x: modelData.x * box.width - 6
                y: modelData.y * box.height - 6
                width: 12
                height: 12
                radius: 2
                color: Theme.clipSelected
                border.color: "#80000000"
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.SizeFDiagCursor
                    property real startScale: 1
                    property real startDist: 1
                    function dist(m) {
                        const p = mapToItem(overlay, m.x, m.y)
                        return Math.max(1, Math.hypot(p.x - box.cx, p.y - box.cy))
                    }
                    onPressed: (m) => {
                        startScale = overlay.project.propertyState(overlay.clipId, "scale").value
                        startDist = dist(m)
                    }
                    onPositionChanged: (m) => overlay.project.setClipProperty(overlay.clipId, "scale", startScale * dist(m) / startDist)
                    onReleased: overlay.project.endInteraction()
                }
            }
        }
    }
}
