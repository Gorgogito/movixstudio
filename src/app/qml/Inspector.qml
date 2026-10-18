import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

// Propiedades del clip seleccionado o, sin selección, del proyecto.
Rectangle {
    id: inspector

    required property ProjectController project
    signal settingsRequested()

    property var info: ({})
    readonly property int selectedCount: project.selection.length

    function refresh() {
        info = selectedCount === 1 ? project.clipInfo(project.selection[0]) : ({})
    }

    color: Theme.surface
    Component.onCompleted: refresh()

    Connections {
        target: inspector.project
        function onSelectionChanged() { inspector.refresh() }
        function onHistoryChanged() { inspector.refresh() }
    }

    component SectionTitle: Text {
        Layout.topMargin: 12
        color: Theme.accentHover
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 6

            Text {
                text: qsTr("Propiedades")
                color: Theme.text
                font.pixelSize: Theme.fontNormal + 1
                font.bold: true
            }

            // --- Un clip seleccionado ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: inspector.info.clipId !== undefined
                spacing: 6

                SectionTitle { text: qsTr("Clip") }
                InfoRow { label: qsTr("Medio"); value: inspector.info.name ?? "" }
                InfoRow { label: qsTr("Pista"); value: inspector.info.track ?? "" }
                InfoRow { label: qsTr("Inicio"); value: inspector.project.timecode(inspector.info.start ?? 0) }
                InfoRow { label: qsTr("Duración"); value: inspector.project.timecode(inspector.info.duration ?? 0) }
                InfoRow { label: qsTr("Desde el medio"); value: Format.timecode(inspector.info.sourceIn ?? 0, inspector.project.frameRate) }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    Text { text: qsTr("Clip activo"); color: Theme.textMuted; Layout.fillWidth: true; font.pixelSize: Theme.fontSmall + 1 }
                    Switch {
                        focusPolicy: Qt.NoFocus
                        checked: inspector.info.enabled ?? true
                        onToggled: inspector.project.setClipEnabled(inspector.info.clipId, checked)
                    }
                }

                SectionTitle { text: qsTr("Audio"); visible: inspector.info.hasAudio ?? false }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: inspector.info.hasAudio ?? false
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: qsTr("Volumen"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall + 1; Layout.fillWidth: true }
                        Text {
                            readonly property real gain: gainSlider.value
                            text: gain <= 0.0001 ? qsTr("Silencio") : (20 * Math.log10(gain) >= 0 ? "+" : "") + (20 * Math.log10(gain)).toFixed(1) + " dB"
                            color: Theme.text
                            font.family: Theme.monoFont
                            font.pixelSize: Theme.fontSmall + 1
                        }
                    }
                    MxSlider {
                        id: gainSlider
                        Layout.fillWidth: true
                        from: 0
                        to: 2
                        value: inspector.info.gain ?? 1
                        onMoved: inspector.project.setClipGain(inspector.info.clipId, value)
                        onPressedChanged: if (!pressed) inspector.project.endInteraction()
                    }
                    Button {
                        text: qsTr("Restablecer (0 dB)")
                        focusPolicy: Qt.NoFocus
                        onClicked: {
                            inspector.project.setClipGain(inspector.info.clipId, 1)
                            inspector.project.endInteraction()
                        }
                    }
                }
            }

            // --- Varios clips ---
            Text {
                Layout.topMargin: 10
                visible: inspector.selectedCount > 1
                text: qsTr("%n clips seleccionados", "", inspector.selectedCount)
                color: Theme.textMuted
            }

            // --- Sin selección: proyecto ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: inspector.selectedCount === 0
                spacing: 6
                SectionTitle { text: qsTr("Proyecto") }
                InfoRow { label: qsTr("Nombre"); value: inspector.project.projectName }
                InfoRow { label: qsTr("Resolución"); value: inspector.project.canvasWidth + " × " + inspector.project.canvasHeight }
                InfoRow { label: qsTr("Fotogramas/s"); value: inspector.project.frameRateText }
                InfoRow { label: qsTr("Duración"); value: inspector.project.timecode(inspector.project.durationFrames) }
                Button {
                    Layout.topMargin: 8
                    text: qsTr("Ajustes del proyecto…")
                    icon.source: "qrc:/qt/qml/MovixStudio/icons/settings.svg"
                    focusPolicy: Qt.NoFocus
                    onClicked: inspector.settingsRequested()
                }
                Text {
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    wrapMode: Text.WordWrap
                    color: Theme.textDisabled
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("Selecciona un clip de la línea de tiempo para ver y cambiar sus propiedades.")
                }
            }
        }
    }
}
