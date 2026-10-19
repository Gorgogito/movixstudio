import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Línea de tiempo multipista: regla, cabeceras de pista, clips, cabezal, imán y zoom.
Rectangle {
    id: timeline

    required property ProjectController project
    readonly property TimelinePlayer player: project.player
    signal activated() // el usuario trabaja en la línea de tiempo: el visor debe mostrarla

    property real pxPerFrame: 2
    readonly property real minPxPerFrame: 0.02
    readonly property real maxPxPerFrame: 40
    readonly property int trackHeight: 58
    readonly property int headerWidth: 168
    readonly property int rulerHeight: 28
    property bool snapping: true
    readonly property real snapThresholdPx: 10

    // Estado compartido del arrastre de clips (vista previa).
    property bool dragging: false
    property real dragDelta: 0
    property int dragRowOffset: 0
    property string dragKind: ""
    property real snapGuideFrame: -1
    property var snapCache: []

    function beginDrag() {
        snapCache = project.snapPoints(project.selection)
        dragKind = project.selectionKind()
    }
    function snapFrame(frame) {
        snapGuideFrame = -1
        if (!snapping)
            return frame
        let best = frame
        let bestDist = snapThresholdPx / pxPerFrame
        for (const p of snapCache) {
            const d = Math.abs(p - frame)
            if (d <= bestDist) {
                bestDist = d
                best = p
            }
        }
        if (best !== frame || bestDist === 0)
            snapGuideFrame = best
        return best
    }
    function updateDrag(clip, dx, dy) {
        let delta = Math.round(dx / pxPerFrame)
        snapGuideFrame = -1
        if (snapping) {
            // El imán actúa sobre cualquiera de los dos bordes del clip que se arrastra.
            let best = Infinity
            for (const p of snapCache) {
                for (const edge of [clip.start, clip.start + clip.duration]) {
                    const d = p - (edge + delta)
                    if (Math.abs(d) <= snapThresholdPx / pxPerFrame && Math.abs(d) < Math.abs(best)) {
                        best = d
                        snapGuideFrame = p
                    }
                }
            }
            if (best !== Infinity)
                delta += best
        }
        delta = Math.max(delta, -clip.start)
        // Cambio de pista: solo dentro del mismo tipo (video con video, audio con audio).
        let rows = dragKind === "mixed" ? 0 : Math.round(dy / trackHeight)
        const firstRow = clip.kind === "video" ? 0 : project.videoTrackCount
        const lastRow = clip.kind === "video" ? project.videoTrackCount - 1 : project.videoTrackCount + project.audioTrackCount - 1
        rows = Math.max(firstRow - clip.trackRow, Math.min(lastRow - clip.trackRow, rows))
        dragDelta = delta
        dragRowOffset = rows
        dragging = true
    }
    function commitDrag() {
        // En video la fila de pantalla crece hacia abajo pero el índice de pista (V1, V2...) hacia arriba.
        const trackOffset = dragKind === "video" ? -dragRowOffset : dragRowOffset
        if (dragging && (dragDelta !== 0 || dragRowOffset !== 0))
            project.moveClips(project.selection, dragDelta, trackOffset)
        cancelDrag()
    }
    function cancelDrag() {
        dragging = false
        dragDelta = 0
        dragRowOffset = 0
        snapGuideFrame = -1
    }

    function zoomTo(newPx, anchorViewX) {
        newPx = Math.max(minPxPerFrame, Math.min(maxPxPerFrame, newPx))
        const frameAtAnchor = (view.contentX + anchorViewX) / pxPerFrame
        pxPerFrame = newPx
        view.contentX = Math.max(0, frameAtAnchor * pxPerFrame - anchorViewX)
    }
    function zoomBy(factor) {
        const playheadX = player.positionFrame * pxPerFrame - view.contentX
        zoomTo(pxPerFrame * factor, playheadX >= 0 && playheadX <= view.width ? playheadX : view.width / 2)
    }
    function fitAll() {
        const frames = Math.max(project.durationFrames, project.frameRate * 10)
        pxPerFrame = Math.max(minPxPerFrame, Math.min(maxPxPerFrame, (view.width - 40) / frames))
        view.contentX = 0
    }
    function trackIdAtRow(row) {
        const item = headerRepeater.itemAt(row)
        return item ? item.trackId : -1
    }

    color: Theme.background

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Barra de herramientas ---
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 2

                IconButton {
                    iconName: "undo"; size: 32
                    tip: timeline.project.canUndo ? qsTr("Deshacer: %1 (Ctrl+Z)").arg(timeline.project.undoText) : qsTr("Deshacer (Ctrl+Z)")
                    enabled: timeline.project.canUndo
                    onClicked: timeline.project.undo()
                }
                IconButton {
                    iconName: "redo"; size: 32
                    tip: timeline.project.canRedo ? qsTr("Rehacer: %1 (Ctrl+Y)").arg(timeline.project.redoText) : qsTr("Rehacer (Ctrl+Y)")
                    enabled: timeline.project.canRedo
                    onClicked: timeline.project.redo()
                }
                Rectangle { width: 1; height: 22; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
                IconButton {
                    iconName: "scissors"; size: 32
                    tip: qsTr("Dividir en el cabezal (Ctrl+B)")
                    enabled: timeline.project.durationFrames > 0
                    onClicked: timeline.project.splitAtPlayhead()
                }
                IconButton {
                    iconName: "trash"; size: 32
                    tip: qsTr("Eliminar selección (Supr)")
                    enabled: timeline.project.selection.length > 0
                    onClicked: timeline.project.deleteSelection(false)
                }
                IconButton {
                    iconName: "magnet"; size: 32
                    tip: timeline.snapping ? qsTr("Imán activado (N)") : qsTr("Imán desactivado (N)")
                    icon.color: timeline.snapping ? Theme.clipSelected : Theme.textMuted
                    onClicked: timeline.snapping = !timeline.snapping
                }
                Button {
                    text: qsTr("Insertar")
                    checkable: true
                    checked: timeline.project.insertMode
                    focusPolicy: Qt.NoFocus
                    onToggled: timeline.project.insertMode = checked
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Modo inserción: al soltar, los clips posteriores se desplazan en lugar de sobrescribirse")
                }
                // Ruta de secuencias al editar un clip compuesto: cada nivel vuelve a él.
                Row {
                    visible: timeline.project.inCompound
                    Layout.leftMargin: 12
                    spacing: 2
                    Repeater {
                        model: timeline.project.sequencePath
                        Row {
                            required property int index
                            required property string modelData
                            readonly property bool last: index === timeline.project.sequencePath.length - 1
                            spacing: 2
                            Text {
                                visible: index > 0
                                text: "›"
                                color: Theme.textMuted
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            AbstractButton {
                                id: crumb
                                enabled: !parent.last
                                focusPolicy: Qt.NoFocus
                                implicitHeight: 26
                                implicitWidth: crumbText.implicitWidth + 14
                                onClicked: timeline.project.openSequenceLevel(parent.index)
                                contentItem: Text {
                                    id: crumbText
                                    text: modelData
                                    color: crumb.parent.last ? Theme.clipCompound : (crumb.hovered ? Theme.text : Theme.textMuted)
                                    font.bold: crumb.parent.last
                                    font.pixelSize: Theme.fontSmall + 1
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: Theme.radius
                                    color: crumb.hovered && crumb.enabled ? Theme.surfaceHover : "transparent"
                                }
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                IconButton { iconName: "film"; size: 32; tip: qsTr("Añadir pista de video"); onClicked: timeline.project.addTrack("video") }
                IconButton { iconName: "music"; size: 32; tip: qsTr("Añadir pista de audio"); onClicked: timeline.project.addTrack("audio") }
                Rectangle { width: 1; height: 22; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
                IconButton { iconName: "zoom-out"; size: 32; tip: qsTr("Alejar (−)"); onClicked: timeline.zoomBy(1 / 1.5) }
                MxSlider {
                    Layout.preferredWidth: 120
                    from: Math.log(timeline.minPxPerFrame)
                    to: Math.log(timeline.maxPxPerFrame)
                    value: Math.log(timeline.pxPerFrame)
                    onMoved: timeline.zoomTo(Math.exp(value), view.width / 2)
                }
                IconButton { iconName: "zoom-in"; size: 32; tip: qsTr("Acercar (+)"); onClicked: timeline.zoomBy(1.5) }
                IconButton { iconName: "fit"; size: 32; tip: qsTr("Ajustar todo a la vista (Mayús+Z)"); onClicked: timeline.fitAll() }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        }

        // --- Cuerpo ---
        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // Esquina: posición del cabezal.
            Rectangle {
                width: timeline.headerWidth
                height: timeline.rulerHeight
                color: Theme.surface
                Text {
                    anchors.centerIn: parent
                    text: timeline.project.timecode(timeline.player.positionFrame)
                    color: Theme.playhead
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.fontNormal
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
            }

            TimeRuler {
                x: timeline.headerWidth
                width: body.width - timeline.headerWidth
                height: timeline.rulerHeight
                project: timeline.project
                pxPerFrame: timeline.pxPerFrame
                contentX: view.contentX
                onSeekRequested: (frame) => {
                    timeline.activated()
                    timeline.player.pause()
                    timeline.player.seekFrame(frame)
                }
            }

            // Cabeceras de pista (se desplazan en vertical con las pistas).
            Item {
                x: 0
                y: timeline.rulerHeight
                width: timeline.headerWidth
                height: body.height - timeline.rulerHeight
                clip: true
                Column {
                    y: -view.contentY
                    width: parent.width
                    Repeater {
                        id: headerRepeater
                        model: timeline.project.tracks
                        delegate: TrackHeader {
                            project: timeline.project
                            trackHeight: timeline.trackHeight
                            kindCount: kind === "video" ? timeline.project.videoTrackCount : timeline.project.audioTrackCount
                        }
                    }
                }
            }

            Flickable {
                id: view
                x: timeline.headerWidth
                y: timeline.rulerHeight
                width: body.width - timeline.headerWidth
                height: body.height - timeline.rulerHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: false // el desplazamiento es con la rueda y las barras, no arrastrando
                contentWidth: Math.max(width, (timeline.project.durationFrames + timeline.project.frameRate * 30) * timeline.pxPerFrame)
                contentHeight: Math.max(height, headerRepeater.count * timeline.trackHeight)
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOn }
                ScrollBar.vertical: ScrollBar {}

                // Fondos de pista.
                Column {
                    Repeater {
                        model: timeline.project.tracks
                        Rectangle {
                            required property string kind
                            required property bool locked
                            width: view.contentWidth
                            height: timeline.trackHeight
                            color: kind === "video" ? Theme.trackVideo : Theme.trackAudio
                            opacity: locked ? 0.6 : 1
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                        }
                    }
                }

                // Clic en zona vacía: deselecciona y mueve el cabezal.
                MouseArea {
                    width: view.contentWidth
                    height: view.contentHeight
                    onPressed: (mouse) => {
                        timeline.activated()
                        timeline.project.clearSelection()
                        timeline.player.pause()
                        timeline.player.seekFrame(Math.max(0, Math.round(mouse.x / timeline.pxPerFrame)))
                    }
                }

                Repeater {
                    model: timeline.project.clips
                    delegate: TimelineClip { owner: timeline }
                }

                // Soltar medios desde la biblioteca.
                DropArea {
                    id: drop
                    width: view.contentWidth
                    height: view.contentHeight
                    keys: ["application/x-movix-media", "application/x-movix-title", "application/x-movix-transition"]
                    property real frame: -1
                    property int row: -1
                    onPositionChanged: (d) => {
                        frame = timeline.snapFrame(Math.max(0, Math.round(d.x / timeline.pxPerFrame)))
                        row = Math.floor(d.y / timeline.trackHeight)
                    }
                    onEntered: (d) => {
                        timeline.snapCache = timeline.project.snapPoints([])
                        frame = Math.max(0, Math.round(d.x / timeline.pxPerFrame))
                        row = Math.floor(d.y / timeline.trackHeight)
                    }
                    onExited: {
                        frame = -1
                        timeline.snapGuideFrame = -1
                    }
                    onDropped: (d) => {
                        const trackId = timeline.trackIdAtRow(row)
                        if (d.formats.indexOf("application/x-movix-transition") >= 0)
                            timeline.project.dropTransition(d.getDataAsString("application/x-movix-transition"), trackId, frame)
                        else if (d.formats.indexOf("application/x-movix-title") >= 0)
                            timeline.project.addTitle(Number(d.getDataAsString("application/x-movix-title")),
                                                      row < timeline.project.videoTrackCount ? trackId : -1, frame)
                        else if (trackId >= 0)
                            timeline.project.placeMedia(Number(d.getDataAsString("application/x-movix-media")), trackId, frame)
                        timeline.activated()
                        frame = -1
                        timeline.snapGuideFrame = -1
                        d.acceptProposedAction()
                    }
                    Rectangle {
                        visible: drop.containsDrag && drop.row >= 0
                        x: 0
                        y: drop.row * timeline.trackHeight
                        width: view.contentWidth
                        height: timeline.trackHeight
                        color: "#227C6CFF"
                    }
                    Rectangle {
                        visible: drop.containsDrag && drop.frame >= 0
                        x: drop.frame * timeline.pxPerFrame
                        width: 2
                        height: view.contentHeight
                        color: Theme.accentHover
                    }
                }
            }

            // Guía del imán.
            Rectangle {
                visible: timeline.snapGuideFrame >= 0
                x: timeline.headerWidth + timeline.snapGuideFrame * timeline.pxPerFrame - view.contentX
                y: timeline.rulerHeight
                width: 1
                height: body.height - timeline.rulerHeight
                color: Theme.snapGuide
            }

            // Cabezal.
            Item {
                readonly property real xInView: timeline.player.positionFrame * timeline.pxPerFrame - view.contentX
                visible: xInView >= -1 && xInView <= view.width
                x: timeline.headerWidth + xInView - 6
                width: 13
                height: body.height
                Rectangle { x: 6; width: 2; height: parent.height; color: Theme.playhead }
                Rectangle {
                    width: 13
                    height: 12
                    radius: 3
                    color: Theme.playhead
                }
            }

            // Zoom con Ctrl+rueda; desplazamiento horizontal con Mayús+rueda.
            WheelHandler {
                target: null
                acceptedModifiers: Qt.ControlModifier
                onWheel: (event) => {
                    const anchor = Math.max(0, event.x - timeline.headerWidth)
                    timeline.zoomTo(timeline.pxPerFrame * (event.angleDelta.y > 0 ? 1.25 : 0.8), anchor)
                }
            }
            WheelHandler {
                target: null
                acceptedModifiers: Qt.NoModifier
                onWheel: (event) => {
                    // En vertical si hay más pistas de las que caben; si no, recorre el tiempo.
                    if (view.contentHeight > view.height)
                        view.contentY = Math.max(0, Math.min(view.contentHeight - view.height, view.contentY - event.angleDelta.y / 2))
                    else
                        view.contentX = Math.max(0, Math.min(view.contentWidth - view.width, view.contentX - event.angleDelta.y))
                }
            }
            WheelHandler {
                target: null
                acceptedModifiers: Qt.ShiftModifier
                onWheel: (event) => {
                    view.contentX = Math.max(0, Math.min(view.contentWidth - view.width, view.contentX - event.angleDelta.y))
                }
            }
        }
    }

    // Si cambian los fps del proyecto, se conserva la escala de tiempo (píxeles por segundo).
    property real lastFrameRate: project.frameRate
    Connections {
        target: timeline.project
        function onSettingsChanged() {
            if (timeline.lastFrameRate > 0 && timeline.project.frameRate > 0)
                timeline.pxPerFrame *= timeline.lastFrameRate / timeline.project.frameRate
            timeline.lastFrameRate = timeline.project.frameRate
        }
    }

    // Durante la reproducción, el cabezal arrastra la vista para no salirse por la derecha.
    Connections {
        target: timeline.player
        function onPositionChanged() {
            if (!timeline.player.playing)
                return
            const x = timeline.player.positionFrame * timeline.pxPerFrame
            if (x > view.contentX + view.width * 0.92 || x < view.contentX)
                view.contentX = Math.max(0, Math.min(view.contentWidth - view.width, x - view.width * 0.1))
        }
    }
}
