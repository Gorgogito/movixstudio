import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Efectos OpenFX del clip: añadir un plugin instalado, activarlo, reordenarlo, quitarlo y ajustar
// sus parámetros (los controles se generan a partir de lo que declara el plugin).
ColumnLayout {
    id: section

    required property ProjectController project
    required property var info
    readonly property var clipId: info.clipId
    readonly property var plugins: project.ofxPlugins
    // Se vuelve a leer cuando cambia el clip (info se renueva con cada edición).
    readonly property var effects: { info; return clipId !== undefined ? project.ofxEffects(clipId) : [] }

    spacing: 6
    Layout.fillWidth: true

    function componentLabel(type, i) {
        if (type === "rgb" || type === "rgba")
            return ["R", "G", "B", "A"][i]
        return ["X", "Y", "Z"][i]
    }

    Text {
        Layout.topMargin: 10
        text: qsTr("Efectos OpenFX")
        color: Theme.accentHover
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }
    Text {
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        visible: section.plugins.length === 0
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        text: qsTr("No hay plugins OpenFX instalados. Instálalos en la carpeta de plugins de MovixStudio o en la carpeta común de OpenFX del sistema y pulsa «Buscar de nuevo».")
    }
    ComboBox {
        Layout.fillWidth: true
        visible: section.plugins.length > 0
        focusPolicy: Qt.NoFocus
        displayText: qsTr("Añadir efecto…")
        model: section.plugins.map(p => (p.group ? p.group + " › " : "") + p.label)
        onActivated: (i) => {
            section.project.addOfxEffect(section.clipId, section.plugins[i].id)
            currentIndex = -1
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Button {
            text: qsTr("Carpeta de plugins")
            focusPolicy: Qt.NoFocus
            onClicked: section.project.openOfxFolder()
        }
        Button {
            text: qsTr("Buscar de nuevo")
            focusPolicy: Qt.NoFocus
            onClicked: section.project.rescanOfx()
        }
    }

    Repeater {
        model: section.effects
        delegate: Rectangle {
            id: card
            required property var modelData
            readonly property int fxIndex: modelData.index
            Layout.fillWidth: true
            implicitHeight: body.implicitHeight + 16
            radius: Theme.radius
            color: Theme.surfaceRaised
            opacity: modelData.enabled ? 1.0 : 0.6

            ColumnLayout {
                id: body
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Switch {
                        focusPolicy: Qt.NoFocus
                        checked: card.modelData.enabled
                        onToggled: section.project.setOfxEnabled(section.clipId, card.fxIndex, checked)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: card.modelData.label + (card.modelData.installed ? "" : qsTr(" (no instalado)"))
                        color: card.modelData.installed ? Theme.text : Theme.danger
                        font.pixelSize: Theme.fontSmall + 1
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    ToolButton {
                        text: "↑"
                        focusPolicy: Qt.NoFocus
                        enabled: card.fxIndex > 0
                        onClicked: section.project.moveOfxEffect(section.clipId, card.fxIndex, card.fxIndex - 1)
                    }
                    ToolButton {
                        text: "↓"
                        focusPolicy: Qt.NoFocus
                        enabled: card.fxIndex < section.effects.length - 1
                        onClicked: section.project.moveOfxEffect(section.clipId, card.fxIndex, card.fxIndex + 1)
                    }
                    ToolButton {
                        text: "✕"
                        focusPolicy: Qt.NoFocus
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Quitar el efecto")
                        onClicked: section.project.removeOfxEffect(section.clipId, card.fxIndex)
                    }
                }

                Repeater {
                    model: card.modelData.params
                    delegate: ColumnLayout {
                        id: param
                        required property var modelData
                        readonly property bool numeric: ["double", "int", "rgb", "rgba", "double2d", "int2d", "double3d", "int3d"].indexOf(modelData.type) >= 0
                        Layout.fillWidth: true
                        spacing: 2
                        ToolTip.visible: hint.hovered && modelData.hint !== ""
                        ToolTip.delay: 600
                        ToolTip.text: modelData.hint
                        HoverHandler { id: hint }

                        function setComponent(i, v) {
                            const value = param.modelData.value.slice()
                            value[i] = v
                            section.project.setOfxParam(section.clipId, card.fxIndex, param.modelData.name, value)
                        }

                        // Números (y cada componente de los colores y vectores).
                        Repeater {
                            model: param.numeric ? param.modelData.value.length : 0
                            delegate: SliderRow {
                                required property int index
                                label: param.modelData.label + (param.modelData.value.length > 1 ? " " + section.componentLabel(param.modelData.type, index) : "")
                                from: param.modelData.type.startsWith("rgb") ? 0 : param.modelData.min
                                to: param.modelData.type.startsWith("rgb") ? 1 : param.modelData.max
                                value: param.modelData.value[index]
                                defaultValue: param.modelData.defaults[index] ?? 0
                                displayScale: 1
                                decimals: param.modelData.type.startsWith("int") ? 0 : 2
                                onMoved: (v) => param.setComponent(index, param.modelData.type.startsWith("int") ? Math.round(v) : v)
                                onReleased: section.project.endInteraction()
                            }
                        }
                        RowLayout {
                            visible: param.modelData.type === "bool"
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: param.modelData.label
                                color: Theme.text
                                font.pixelSize: Theme.fontSmall + 1
                            }
                            Switch {
                                focusPolicy: Qt.NoFocus
                                checked: param.modelData.value[0] !== 0
                                onToggled: param.setComponent(0, checked ? 1 : 0)
                            }
                        }
                        RowLayout {
                            visible: param.modelData.type === "choice"
                            Layout.fillWidth: true
                            Text {
                                text: param.modelData.label
                                color: Theme.text
                                font.pixelSize: Theme.fontSmall + 1
                            }
                            ComboBox {
                                Layout.fillWidth: true
                                focusPolicy: Qt.NoFocus
                                model: param.modelData.options
                                currentIndex: param.modelData.value[0]
                                onActivated: (i) => param.setComponent(0, i)
                            }
                        }
                    }
                }
            }
        }
    }
}
