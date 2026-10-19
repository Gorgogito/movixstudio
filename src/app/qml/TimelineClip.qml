import QtQuick
import QtQuick.Controls
import MovixStudio

// Clip en la línea de tiempo: miniaturas o forma de onda, mover arrastrando (con imán y cambio
// de pista), recortar desde los bordes y menú contextual. La vista previa del arrastre es solo
// visual; la edición se aplica al soltar.
Item {
    id: clipItem // no «clip»: chocaría con la propiedad Item.clip dentro de los hijos

    required property var clipId
    required property int trackRow
    required property var start
    required property var duration
    required property string name
    required property string path
    required property real sourceIn
    required property real mediaDuration
    required property string kind
    required property bool hasAudio
    required property bool hasVideo
    required property bool clipEnabled
    required property real gain
    required property bool selected
    required property bool isTitle
    required property bool isCompound
    required property bool isStill
    required property real speed
    required property var transitionFrames
    required property var fadeIn
    required property var fadeOut
    property var owner // la línea de tiempo que contiene el clip

    readonly property real px: owner.pxPerFrame
    readonly property real fps: owner.project.frameRate
    readonly property bool movingWithDrag: selected && owner.dragging
    property bool trimming: false
    property real trimStart: 0
    property real trimEnd: 0
    readonly property real visualStart: trimming ? trimStart : start + (movingWithDrag ? owner.dragDelta : 0)
    readonly property real visualEnd: trimming ? trimEnd : start + duration + (movingWithDrag ? owner.dragDelta : 0)
    readonly property int visualRow: trackRow + (movingWithDrag ? owner.dragRowOffset : 0)
    // Límites del medio disponibles para recortar (imágenes y títulos no tienen límite).
    readonly property bool unlimited: isTitle || isStill
    readonly property real minStart: unlimited ? 0 : Math.max(0, start - Math.floor(sourceIn * fps / speed + 1e-6))
    readonly property real maxEnd: unlimited ? 1e9 : start + Math.floor((mediaDuration - sourceIn) * fps / speed + 0.001)
    readonly property real visibleSourceIn: sourceIn + (trimming ? (trimStart - start) / fps * speed : 0)
    // Keyframes del clip (solo se muestran si está seleccionado).
    property var keyframes: []
    function refreshKeyframes() {
        keyframes = selected ? owner.project.keyframeFrames(clipId) : []
    }
    onSelectedChanged: refreshKeyframes()
    Connections {
        target: clipItem.owner.project
        enabled: clipItem.selected
        function onHistoryChanged() { clipItem.refreshKeyframes() }
    }

    x: visualStart * px
    y: visualRow * owner.trackHeight + 3
    width: Math.max(3, (visualEnd - visualStart) * px)
    height: owner.trackHeight - 6
    z: movingWithDrag || trimming ? 10 : (selected ? 2 : 1)

    Rectangle {
        id: body
        anchors.fill: parent
        radius: 4
        clip: true
        color: clipItem.isCompound ? Theme.clipCompound : clipItem.isTitle ? Theme.clipTitle
                                   : clipItem.kind === "video" ? Theme.clipVideoDark : Theme.clipAudioDark
        opacity: clipItem.clipEnabled ? 1 : 0.45
        border.width: clipItem.selected ? 2 : 1
        border.color: clipItem.selected ? Theme.clipSelected : Qt.lighter(color, 1.35)

        // Tira de miniaturas (clips de video).
        Item {
            id: filmstrip
            visible: clipItem.kind === "video" && clipItem.hasVideo && !clipItem.isTitle && !clipItem.isCompound
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: clipItem.hasAudio ? body.height * 0.62 : body.height - 2
            clip: true
            readonly property real tileWidth: Math.max(24, height * 16 / 9)

            Repeater {
                model: filmstrip.visible ? Math.min(30, Math.ceil(filmstrip.width / filmstrip.tileWidth)) : 0
                Image {
                    required property int index
                    // Instante de la miniatura, redondeado a 1/4 s para reutilizar la caché.
                    readonly property real t: clipItem.isStill ? 0 : Math.round((clipItem.visibleSourceIn + index * filmstrip.tileWidth / clipItem.px / clipItem.fps * clipItem.speed) * 4) / 4
                    x: index * filmstrip.tileWidth
                    width: filmstrip.tileWidth
                    height: filmstrip.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    sourceSize.height: Math.max(24, Math.round(filmstrip.height))
                    source: "image://thumbs/" + Math.round(t * 1e6) + "/" + encodeURIComponent(clipItem.path)
                    opacity: 0.85
                }
            }
        }

        WaveformItem {
            visible: clipItem.hasAudio
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: clipItem.kind === "video" ? body.height * 0.38 - 2 : body.height - 18
            source: clipItem.path
            sourceIn: clipItem.visibleSourceIn
            duration: (clipItem.visualEnd - clipItem.visualStart) / clipItem.fps * clipItem.speed
            color: clipItem.kind === "video" ? "#B0C8D0FF" : "#C0B8FFE0"
        }

        // Fundidos: degradados hacia negro en los extremos.
        Rectangle {
            visible: clipItem.fadeIn > 0
            width: Math.min(body.width, clipItem.fadeIn * clipItem.px)
            height: body.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#C0000000" }
                GradientStop { position: 1; color: "transparent" }
            }
        }
        Rectangle {
            visible: clipItem.fadeOut > 0
            anchors.right: parent.right
            width: Math.min(body.width, clipItem.fadeOut * clipItem.px)
            height: body.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "transparent" }
                GradientStop { position: 1; color: "#C0000000" }
            }
        }

        // Rombos de los keyframes del clip seleccionado.
        Repeater {
            model: clipItem.keyframes
            Rectangle {
                required property var modelData
                x: (modelData - clipItem.start) * clipItem.px - 4
                y: body.height - 12
                width: 8
                height: 8
                rotation: 45
                color: Theme.clipSelected
                border.color: "#80000000"
                visible: x > -4 && x < body.width
            }
        }

        Rectangle {
            x: 3
            y: 3
            width: Math.min(label.implicitWidth + 8, body.width - 6)
            height: label.implicitHeight + 2
            radius: 3
            color: "#99000000"
            visible: body.width > 24
            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                x: 4
                width: parent.width - 8
                text: (clipItem.isTitle ? "T  " : clipItem.isCompound ? "▣  " : "") + clipItem.name
                      + (clipItem.speed !== 1 ? "  ·  " + clipItem.speed.toLocaleString(Qt.locale(), "f", 2).replace(/,?0+$/, "") + "×" : "")
                      + (clipItem.gain >= 0 && clipItem.gain !== 1 ? "  ·  " + Math.round(clipItem.gain * 100) + " %" : "")
                color: "white"
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }
    }

    // Transición de entrada: ocupa [corte - mitad, corte + mitad), centrada en el inicio del clip.
    Rectangle {
        visible: clipItem.transitionFrames > 0 && !clipItem.trimming && !clipItem.movingWithDrag
        x: -Math.floor(clipItem.transitionFrames / 2) * clipItem.px
        y: 2
        z: 5
        width: Math.max(10, clipItem.transitionFrames * clipItem.px)
        height: 14
        radius: 7
        color: "#D07C6CFF"
        border.color: Theme.accentHover
        Text {
            anchors.centerIn: parent
            text: "⧓"
            color: "white"
            font.pixelSize: 10
        }
    }

    // --- Mover ---
    MouseArea {
        id: moveArea
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true
        cursorShape: clipItem.movingWithDrag ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property point pressPos
        property bool moved: false

        onPressed: (mouse) => {
            clipItem.owner.activated()
            if (mouse.button === Qt.RightButton) {
                if (!clipItem.selected)
                    clipItem.owner.project.select(clipItem.clipId, false)
                contextMenu.popup()
                return
            }
            const additive = (mouse.modifiers & Qt.ControlModifier) !== 0
            if (additive || !clipItem.selected)
                clipItem.owner.project.select(clipItem.clipId, additive)
            pressPos = mapToItem(clipItem.parent, mouse.x, mouse.y)
            moved = false
            clipItem.owner.beginDrag()
        }
        onPositionChanged: (mouse) => {
            if (!pressed || !(mouse.buttons & Qt.LeftButton))
                return
            const p = mapToItem(clipItem.parent, mouse.x, mouse.y)
            const dx = p.x - pressPos.x
            const dy = p.y - pressPos.y
            if (!moved && Math.abs(dx) < 4 && Math.abs(dy) < 4)
                return
            moved = true
            clipItem.owner.updateDrag(clipItem, dx, dy)
        }
        onReleased: moved ? clipItem.owner.commitDrag() : clipItem.owner.cancelDrag()
        onCanceled: clipItem.owner.cancelDrag()
        // Doble clic: en un compuesto, abre su secuencia; en los demás, lleva el cabezal al inicio.
        onDoubleClicked: clipItem.isCompound ? clipItem.owner.project.openCompound(clipItem.clipId)
                                             : clipItem.owner.player.seekFrame(clipItem.start)
    }

    // --- Recortar ---
    component TrimHandle: MouseArea {
        id: handle
        property bool startEdge: true
        width: 7
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        hoverEnabled: true
        property real pressX: 0

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 3
            anchors.bottomMargin: 3
            radius: 2
            color: Theme.clipSelected
            opacity: handle.containsMouse || handle.pressed ? 0.9 : 0
        }

        onPressed: (mouse) => {
            clipItem.owner.activated()
            if (!clipItem.selected)
                clipItem.owner.project.select(clipItem.clipId, false)
            pressX = mapToItem(clipItem.parent, mouse.x, 0).x
            clipItem.trimStart = clipItem.start
            clipItem.trimEnd = clipItem.start + clipItem.duration
            clipItem.trimming = true
            clipItem.owner.beginDrag()
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            const dx = mapToItem(clipItem.parent, mouse.x, 0).x - pressX
            if (startEdge) {
                let f = clipItem.owner.snapFrame(clipItem.start + Math.round(dx / clipItem.px))
                clipItem.trimStart = Math.max(clipItem.minStart, Math.min(f, clipItem.trimEnd - 1))
            } else {
                let f = clipItem.owner.snapFrame(clipItem.start + clipItem.duration + Math.round(dx / clipItem.px))
                clipItem.trimEnd = Math.min(clipItem.maxEnd, Math.max(f, clipItem.trimStart + 1))
            }
        }
        onReleased: {
            if (startEdge && clipItem.trimStart !== clipItem.start)
                clipItem.owner.project.trimClip(clipItem.clipId, true, clipItem.trimStart)
            else if (!startEdge && clipItem.trimEnd !== clipItem.start + clipItem.duration)
                clipItem.owner.project.trimClip(clipItem.clipId, false, clipItem.trimEnd)
            clipItem.trimming = false
            clipItem.owner.cancelDrag()
        }
        onCanceled: {
            clipItem.trimming = false
            clipItem.owner.cancelDrag()
        }
    }
    TrimHandle { anchors.left: parent.left; startEdge: true }
    TrimHandle { anchors.right: parent.right; startEdge: false }

    Menu {
        id: contextMenu
        MenuItem { text: qsTr("Dividir en el cabezal") + "\tCtrl+B"; onTriggered: clipItem.owner.project.splitAtPlayhead() }
        MenuItem { text: qsTr("Eliminar") + "\t" + qsTr("Supr"); onTriggered: clipItem.owner.project.deleteSelection(false) }
        MenuItem { text: qsTr("Eliminar y cerrar hueco") + "\t" + qsTr("Mayús+Supr"); onTriggered: clipItem.owner.project.deleteSelection(true) }
        MenuSeparator {}
        MenuItem { text: qsTr("Copiar") + "\tCtrl+C"; onTriggered: clipItem.owner.project.copySelection() }
        MenuItem { text: qsTr("Cortar") + "\tCtrl+X"; onTriggered: clipItem.owner.project.cutSelection() }
        MenuItem { text: qsTr("Pegar en el cabezal") + "\tCtrl+V"; enabled: clipItem.owner.project.canPaste; onTriggered: clipItem.owner.project.paste() }
        MenuSeparator {}
        Menu {
            title: qsTr("Velocidad")
            enabled: !clipItem.unlimited
            Repeater {
                model: [0.25, 0.5, 1, 1.5, 2, 4]
                MenuItem {
                    required property real modelData
                    text: String(modelData).replace(".", ",") + "×"
                    checkable: true
                    checked: Math.abs(clipItem.speed - modelData) < 1e-6
                    onTriggered: clipItem.owner.project.setClipSpeed(clipItem.clipId, modelData)
                }
            }
        }
        MenuItem {
            text: qsTr("Crear clip compuesto") + "\tCtrl+G"
            onTriggered: clipItem.owner.project.createCompound()
        }
        MenuItem {
            visible: clipItem.isCompound
            height: visible ? implicitHeight : 0
            text: qsTr("Abrir clip compuesto")
            onTriggered: clipItem.owner.project.openCompound(clipItem.clipId)
        }
        MenuItem {
            visible: clipItem.isCompound
            height: visible ? implicitHeight : 0
            text: qsTr("Deshacer clip compuesto") + "\tCtrl+Mayús+G"
            onTriggered: clipItem.owner.project.breakCompound(clipItem.clipId)
        }
        MenuItem {
            text: qsTr("Separar audio")
            enabled: clipItem.kind === "video" && clipItem.hasAudio
            onTriggered: clipItem.owner.project.detachAudio(clipItem.clipId)
        }
        MenuSeparator {}
        MenuItem {
            text: clipItem.clipEnabled ? qsTr("Desactivar clip") : qsTr("Activar clip")
            onTriggered: clipItem.owner.project.setClipEnabled(clipItem.clipId, !clipItem.clipEnabled)
        }
        MenuItem { text: qsTr("Ir al inicio del clip"); onTriggered: clipItem.owner.player.seekFrame(clipItem.start) }
    }
}
