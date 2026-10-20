import QtQuick
import QtQuick.Shapes
import MovixStudio

// Recuadro de transformación sobre el visor para el clip seleccionado: arrastrar el interior lo
// mueve y arrastrar una esquina lo escala. Los cambios van a la posición/escala en el cabezal
// (como keyframe si la propiedad está animada). Modos: "transform", "mask" (dibuja la máscara y
// arrastrarla la mueve) y "pick" (cuentagotas de la clave de croma).
Item {
    id: overlay

    required property ProjectController project
    property string mode: "transform"
    signal picked()
    signal objectPicked(real u, real v)
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

    property int maskIndex: 0
    readonly property var mask: (geo.masks ?? [])[maskIndex] ?? null

    // Sistema de una máscara (−0,5..0,5 de su caja, sin girar) ↔ píxeles de la caja de la capa.
    function localToBox(m, lx, ly) {
        const bw = m.width * box.width, bh = m.height * box.height
        const a = m.rotation * Math.PI / 180, c = Math.cos(a), s = Math.sin(a)
        const x = lx * bw, y = ly * bh
        return Qt.point(m.x * box.width + c * x - s * y, m.y * box.height + s * x + c * y)
    }
    function boxToLocal(m, bx, by) {
        const a = m.rotation * Math.PI / 180, c = Math.cos(a), s = Math.sin(a)
        const dx = bx - m.x * box.width, dy = by - m.y * box.height
        const x = c * dx + s * dy, y = -s * dx + c * dy
        return Qt.point(x / Math.max(1e-6, m.width * box.width), y / Math.max(1e-6, m.height * box.height))
    }
    // Contorno cerrado de Bézier de una máscara de forma libre, en píxeles de la caja.
    function svgPath(m) {
        const pts = m.points ?? []
        if (pts.length < 3)
            return ""
        const at = (x, y) => { const q = localToBox(m, x, y); return q.x.toFixed(2) + " " + q.y.toFixed(2) }
        let d = "M " + at(pts[0][0], pts[0][1])
        for (let i = 1; i <= pts.length; ++i) {
            const a = pts[i - 1], b = pts[i % pts.length]
            d += " C " + at(a[0] + a[4], a[1] + a[5]) + " " + at(b[0] + b[2], b[1] + b[3]) + " " + at(b[0], b[1])
        }
        return d + " Z"
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
            border.color: overlay.mode === "transform" ? Theme.clipSelected : "#80FFFFFF"
        }

        // Máscaras (en coordenadas de la capa: se mueven y giran con ella). La seleccionada se resalta.
        Repeater {
            model: overlay.mode === "mask" ? (overlay.geo.masks ?? []) : []
            Item {
                id: maskEntry
                required property int index
                required property var modelData
                readonly property var m: modelData
                readonly property color stroke: index === overlay.maskIndex ? Theme.accentHover : "#70FFFFFF"
                anchors.fill: parent
                Item {
                    id: maskItem
                    readonly property bool linear: maskEntry.m.shape === "linear"
                    x: maskEntry.m.x * box.width - width / 2
                    y: maskEntry.m.y * box.height - height / 2
                    width: linear ? box.width * 3 : maskEntry.m.width * box.width
                    height: linear ? 0 : maskEntry.m.height * box.height
                    rotation: maskEntry.m.rotation
                    Rectangle {
                        anchors.fill: parent
                        visible: maskEntry.m.shape === "rectangle"
                        color: "transparent"
                        border.width: 1.5
                        border.color: maskEntry.stroke
                    }
                    Shape {
                        anchors.fill: parent
                        visible: maskEntry.m.shape === "ellipse"
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath {
                            strokeColor: maskEntry.stroke
                            strokeWidth: 1.5
                            fillColor: "transparent"
                            PathAngleArc {
                                centerX: maskItem.width / 2
                                centerY: maskItem.height / 2
                                radiusX: maskItem.width / 2
                                radiusY: maskItem.height / 2
                                startAngle: 0
                                sweepAngle: 360
                            }
                        }
                    }
                    Rectangle { // línea de la máscara lineal
                        visible: maskItem.linear
                        anchors.verticalCenter: parent.top
                        width: parent.width
                        height: 1.5
                        color: maskEntry.stroke
                    }
                }
                Shape { // forma libre: contorno ya transformado, en coordenadas de la caja
                    anchors.fill: parent
                    visible: maskEntry.m.shape === "path"
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: maskEntry.stroke
                        strokeWidth: 1.5
                        fillColor: "transparent"
                        PathSvg { path: overlay.svgPath(maskEntry.m) }
                    }
                }
            }
        }
        // Mover la máscara seleccionada (y, en la de forma libre, doble clic añade un punto).
        MouseArea {
            anchors.fill: parent
            enabled: overlay.mode === "mask" && overlay.mask !== null
            visible: enabled
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.SizeAllCursor
            property point origin
            property real startX: 0.5
            property real startY: 0.5
            onPressed: (m) => {
                origin = Qt.point(m.x, m.y)
                startX = overlay.mask.x
                startY = overlay.mask.y
            }
            onPositionChanged: (m) => {
                const base = "mask." + overlay.maskIndex + "."
                overlay.project.setClipProperty(overlay.clipId, base + "x", startX + (m.x - origin.x) / box.width)
                overlay.project.setClipProperty(overlay.clipId, base + "y", startY + (m.y - origin.y) / box.height)
            }
            onReleased: overlay.project.endInteraction()
            onDoubleClicked: (m) => {
                if (overlay.mask.shape !== "path")
                    return
                const pts = overlay.mask.points.map(q => q.slice())
                const local = overlay.boxToLocal(overlay.mask, m.x, m.y)
                // Se inserta en el lado cuyo punto medio está más cerca.
                let best = 0, bestDist = 1e9
                for (let i = 0; i < pts.length; ++i) {
                    const q = pts[(i + 1) % pts.length]
                    const d = Math.hypot((pts[i][0] + q[0]) / 2 - local.x, (pts[i][1] + q[1]) / 2 - local.y)
                    if (d < bestDist) { bestDist = d; best = i }
                }
                pts.splice(best + 1, 0, [local.x, local.y, 0, 0, 0, 0])
                overlay.project.setMaskPoints(overlay.clipId, overlay.maskIndex, pts)
                overlay.project.endInteraction()
            }
        }
        Rectangle { // centro de la máscara seleccionada
            visible: overlay.mode === "mask" && overlay.mask !== null
            x: (overlay.mask?.x ?? 0.5) * box.width - 7
            y: (overlay.mask?.y ?? 0.5) * box.height - 7
            width: 14
            height: 14
            radius: 7
            color: Theme.accent
            border.color: "white"
        }
        // Puntos de la forma libre: arrastrar los mueve; clic derecho los quita (mínimo tres).
        Repeater {
            // Modelo numérico: así los tiradores no se recrean (ni se pierde el arrastre) en cada cambio.
            model: overlay.mode === "mask" && overlay.mask?.shape === "path" ? overlay.mask.points.length : 0
            Rectangle {
                id: handle
                required property int index
                readonly property var pt: overlay.mask?.points?.[index] ?? [0, 0]
                readonly property point pos: overlay.mask ? overlay.localToBox(overlay.mask, pt[0], pt[1]) : Qt.point(0, 0)
                x: pos.x - 6
                y: pos.y - 6
                width: 12
                height: 12
                radius: 2
                rotation: 45
                color: "white"
                border.color: Theme.accent
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    preventStealing: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: (m) => {
                        if (m.button !== Qt.RightButton)
                            return
                        const pts = overlay.mask.points.map(q => q.slice())
                        if (pts.length > 3) {
                            pts.splice(handle.index, 1)
                            overlay.project.setMaskPoints(overlay.clipId, overlay.maskIndex, pts)
                            overlay.project.endInteraction()
                        }
                    }
                    onPositionChanged: (m) => {
                        if (!(pressedButtons & Qt.LeftButton))
                            return
                        const p = mapToItem(box, m.x, m.y)
                        const local = overlay.boxToLocal(overlay.mask, p.x, p.y)
                        const pts = overlay.mask.points.map(q => q.slice())
                        pts[handle.index][0] = local.x
                        pts[handle.index][1] = local.y
                        overlay.project.setMaskPoints(overlay.clipId, overlay.maskIndex, pts)
                    }
                    onReleased: overlay.project.endInteraction()
                }
            }
        }
        // Cuentagotas: toma el color del clip en el punto (coordenadas de la capa).
        MouseArea {
            anchors.fill: parent
            enabled: overlay.mode === "pick"
            visible: enabled
            cursorShape: Qt.CrossCursor
            onClicked: (m) => {
                if (overlay.project.pickKeyColor(overlay.clipId, m.x / box.width, m.y / box.height))
                    overlay.picked()
            }
        }

        // Señalar un objeto (SAM 2): coordenadas de la capa.
        MouseArea {
            anchors.fill: parent
            enabled: overlay.mode === "pickObject"
            visible: enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: (m) => overlay.objectPicked(m.x / box.width, m.y / box.height)
        }

        // Mover.
        MouseArea {
            anchors.fill: parent
            enabled: overlay.mode === "transform"
            visible: enabled
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
                visible: overlay.mode === "transform"
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
