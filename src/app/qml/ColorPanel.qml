import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MovixStudio

// Corrección de color del clip: básica, ruedas, curvas, HSL por gamas y LUT .cube. Se aplica en la
// GPU como una LUT 3D, igual en el visor y al exportar.
ColumnLayout {
    id: panel

    required property ProjectController project
    required property var clipId
    property var grade: ({})
    property int hslBand: 0

    function refresh() {
        grade = clipId !== undefined ? project.colorGrade(clipId) : ({})
    }
    function set(key, value) { project.setColorValue(clipId, key, value) }
    function done() { project.endInteraction() }

    spacing: 6
    Layout.fillWidth: true
    Component.onCompleted: refresh()
    onClipIdChanged: refresh()
    Connections {
        target: panel.project
        function onHistoryChanged() { panel.refresh() }
    }
    Connections { // con keyframes, los valores cambian con el cabezal
        target: panel.project.player
        enabled: panel.grade.animated === true
        function onPositionChanged() { panel.refresh() }
    }

    // Los componentes en línea no ven los id de este archivo: avisan con señales.
    component SectionHeader: RowLayout {
        id: header
        property string text
        signal reset()
        Layout.fillWidth: true
        Layout.topMargin: 12
        Text {
            text: header.text
            color: Theme.accentHover
            font.pixelSize: Theme.fontSmall
            font.bold: true
            font.letterSpacing: 0.8
            font.capitalization: Font.AllUppercase
            Layout.fillWidth: true
        }
        IconButton {
            size: 20
            text: "↺"
            display: AbstractButton.TextOnly
            tip: qsTr("Restablecer esta sección")
            onClicked: header.reset()
        }
    }

    FileDialog {
        id: lutDialog
        title: qsTr("Cargar LUT")
        nameFilters: [qsTr("LUT de color (*.cube)"), qsTr("Todos los archivos (*)")]
        onAccepted: panel.project.setColorLut(panel.clipId, selectedFile)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Button {
            text: qsTr("Copiar")
            focusPolicy: Qt.NoFocus
            Layout.fillWidth: true
            onClicked: panel.project.copyColor(panel.clipId)
        }
        Button {
            text: qsTr("Pegar")
            enabled: panel.project.canPasteColor
            focusPolicy: Qt.NoFocus
            Layout.fillWidth: true
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Pega la corrección copiada en los clips seleccionados")
            onClicked: panel.project.pasteColor()
        }
        Button {
            text: qsTr("Restablecer")
            enabled: panel.grade.identity === false
            focusPolicy: Qt.NoFocus
            Layout.fillWidth: true
            onClicked: panel.project.resetColor(panel.clipId, "all")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: panel.grade.animated ? qsTr("Color animado: los cambios van al keyframe del cabezal")
                                       : qsTr("Keyframe de color")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        IconButton {
            size: 22
            text: "‹"
            display: AbstractButton.TextOnly
            enabled: panel.grade.hasPrev === true
            tip: qsTr("Keyframe anterior")
            onClicked: panel.project.jumpToKeyframe(panel.clipId, false)
        }
        AbstractButton {
            id: colorKey
            implicitWidth: 22
            implicitHeight: 22
            focusPolicy: Qt.NoFocus
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: panel.grade.keyHere ? qsTr("Quitar keyframe de color en el cabezal") : qsTr("Añadir keyframe de color en el cabezal")
            onClicked: panel.project.toggleColorKeyframe(panel.clipId)
            contentItem: Item {
                Rectangle {
                    anchors.centerIn: parent
                    width: 9
                    height: 9
                    rotation: 45
                    color: panel.grade.keyHere ? Theme.clipSelected : "transparent"
                    border.width: 1.5
                    border.color: panel.grade.animated ? Theme.clipSelected : (colorKey.hovered ? Theme.text : Theme.textMuted)
                }
            }
        }
        IconButton {
            size: 22
            text: "›"
            display: AbstractButton.TextOnly
            enabled: panel.grade.hasNext === true
            tip: qsTr("Keyframe siguiente")
            onClicked: panel.project.jumpToKeyframe(panel.clipId, true)
        }
    }

    // ---- Espacio de entrada (OpenColorIO) ----
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        Text {
            text: qsTr("Material de cámara")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
        }
        ComboBox {
            id: inputCombo
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            font.pixelSize: Theme.fontSmall + 1
            property var spaces: []
            Component.onCompleted: spaces = panel.project.ocioInputSpaces()
            model: [qsTr("Ninguno (ya es la señal del proyecto)")].concat(spaces.map(s => s.name + "  ·  " + s.family.replace("Input/", "")))
            currentIndex: Math.max(0, spaces.findIndex(s => s.name === (panel.grade.inputSpace ?? "")) + 1)
            onActivated: (i) => panel.project.setColorInputSpace(panel.clipId, i === 0 ? "" : spaces[i - 1].name)
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Convierte el perfil logarítmico de la cámara a la señal del proyecto con OpenColorIO (ACES).")
        }
    }

    // ---- Básica ----
    SectionHeader { text: qsTr("Básica"); onReset: panel.project.resetColor(panel.clipId, "basic") }
    Repeater {
        model: [
            { key: "exposure", label: qsTr("Exposición"), from: -4, to: 4, scale: 1, decimals: 2, suffix: "EV" },
            { key: "contrast", label: qsTr("Contraste") }, { key: "saturation", label: qsTr("Saturación") },
            { key: "vibrance", label: qsTr("Intensidad") }, { key: "temperature", label: qsTr("Temperatura") },
            { key: "tint", label: qsTr("Matiz") }, { key: "highlights", label: qsTr("Altas luces") },
            { key: "shadows", label: qsTr("Sombras") }, { key: "whites", label: qsTr("Blancos") },
            { key: "blacks", label: qsTr("Negros") }
        ]
        SliderRow {
            required property var modelData
            label: modelData.label
            from: modelData.from ?? -1
            to: modelData.to ?? 1
            displayScale: modelData.scale ?? 100
            decimals: modelData.decimals ?? 0
            suffix: modelData.suffix ?? ""
            value: panel.grade[modelData.key] ?? 0
            onMoved: (v) => panel.set(modelData.key, v)
            onReleased: panel.done()
        }
    }

    // ---- Ruedas ----
    SectionHeader { text: qsTr("Ruedas de color"); onReset: panel.project.resetColor(panel.clipId, "wheels") }
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: [{ key: "lift", label: qsTr("Sombras") }, { key: "gamma", label: qsTr("Medios") }, { key: "gain", label: qsTr("Luces") }]
            ColorWheel {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                label: modelData.label
                values: panel.grade[modelData.key] ?? [0, 0, 0, 0]
                onWheelMoved: (r, g, b) => panel.project.setColorWheel(panel.clipId, modelData.key, r, g, b)
                onMasterMoved: (v) => panel.set(modelData.key + ".master", v)
                onReleased: panel.done()
            }
        }
    }

    // ---- Curvas ----
    SectionHeader { text: qsTr("Curvas"); onReset: panel.project.resetColor(panel.clipId, "curves") }
    CurveEditor {
        Layout.fillWidth: true
        curves: panel.grade.curves ?? ({})
        onCurveEdited: (channel, points) => panel.project.setColorCurve(panel.clipId, channel, points)
        onReleased: panel.done()
    }

    // ---- HSL ----
    SectionHeader { text: qsTr("HSL"); onReset: panel.project.resetColor(panel.clipId, "hsl") }
    RowLayout {
        Layout.fillWidth: true
        spacing: 0
        Repeater {
            model: [
                { color: "#FF4040", name: qsTr("Rojos") }, { color: "#FF9A30", name: qsTr("Naranjas") },
                { color: "#F5E040", name: qsTr("Amarillos") }, { color: "#40D060", name: qsTr("Verdes") },
                { color: "#40D8D8", name: qsTr("Aguamarinas") }, { color: "#4070FF", name: qsTr("Azules") },
                { color: "#9A50FF", name: qsTr("Morados") }, { color: "#FF50D0", name: qsTr("Magentas") }
            ]
            Item {
                id: bandItem
                required property int index
                required property var modelData
                readonly property var band: (panel.grade.hsl ?? [])[index] ?? [0, 0, 0]
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                Rectangle {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    radius: 9
                    color: bandItem.modelData.color
                    border.width: panel.hslBand === bandItem.index ? 2 : 0
                    border.color: "white"
                    Rectangle { // marca de gama modificada
                        visible: bandItem.band[0] !== 0 || bandItem.band[1] !== 0 || bandItem.band[2] !== 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.bottom
                        anchors.topMargin: 1
                        width: 4; height: 4; radius: 2
                        color: Theme.text
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: panel.hslBand = bandItem.index
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: bandItem.modelData.name
                }
            }
        }
    }
    SliderRow {
        readonly property var band: (panel.grade.hsl ?? [])[panel.hslBand] ?? [0, 0, 0]
        label: qsTr("Tono")
        from: -30; to: 30; displayScale: 1; suffix: "°"
        value: band[0]
        onMoved: (v) => panel.set("hsl." + panel.hslBand + ".hue", v)
        onReleased: panel.done()
    }
    SliderRow {
        readonly property var band: (panel.grade.hsl ?? [])[panel.hslBand] ?? [0, 0, 0]
        label: qsTr("Saturación")
        value: band[1]
        onMoved: (v) => panel.set("hsl." + panel.hslBand + ".saturation", v)
        onReleased: panel.done()
    }
    SliderRow {
        readonly property var band: (panel.grade.hsl ?? [])[panel.hslBand] ?? [0, 0, 0]
        label: qsTr("Luminancia")
        value: band[2]
        onMoved: (v) => panel.set("hsl." + panel.hslBand + ".luminance", v)
        onReleased: panel.done()
    }

    // ---- LUT ----
    SectionHeader { text: qsTr("LUT"); onReset: panel.project.resetColor(panel.clipId, "lut") }
    RowLayout {
        Layout.fillWidth: true
        Text {
            Layout.fillWidth: true
            text: panel.grade.lutName || qsTr("Ninguna")
            color: panel.grade.lutName ? Theme.text : Theme.textMuted
            elide: Text.ElideMiddle
            font.pixelSize: Theme.fontSmall + 1
        }
        Button {
            text: qsTr("Cargar…")
            focusPolicy: Qt.NoFocus
            onClicked: lutDialog.open()
        }
    }
    SliderRow {
        visible: (panel.grade.lutName ?? "") !== ""
        label: qsTr("Intensidad")
        from: 0; to: 1; defaultValue: 1; suffix: "%"
        value: panel.grade.lutIntensity ?? 1
        onMoved: (v) => panel.set("lutIntensity", v)
        onReleased: panel.done()
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDisabled
        font.pixelSize: Theme.fontSmall
        text: qsTr("Doble clic en un control lo devuelve a su valor neutro.")
    }
}
