import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Gestor de modelos de IA: qué hay instalado, qué ocupa y dónde está.
Dialog {
    id: dlg

    required property AiController ai

    title: qsTr("Modelos de IA")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 560
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Repeater {
            model: dlg.ai.models
            delegate: Rectangle {
                id: row
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: content.implicitHeight + 16
                radius: Theme.radius
                color: Theme.surfaceRaised
                border.color: Theme.border

                RowLayout {
                    id: content
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: row.modelData.name; font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.description + "  ·  " + qsTr("%1 MB").arg(Math.round(row.modelData.sizeMB))
                                  + "  ·  " + qsTr("licencia %1").arg(row.modelData.license)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                    Label {
                        visible: row.modelData.installed
                        text: qsTr("Instalado")
                        color: Theme.accentHover
                        font.pixelSize: Theme.fontSmall
                    }
                    Button {
                        text: row.modelData.installed ? qsTr("Eliminar") : (row.modelData.downloading ? qsTr("Descargando…") : qsTr("Descargar"))
                        enabled: row.modelData.installed || (!dlg.ai.busy && !row.modelData.downloading)
                        onClicked: row.modelData.installed ? dlg.ai.removeModel(row.modelData.id) : dlg.ai.downloadModel(row.modelData.id)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Carpeta: %1").arg(dlg.ai.modelsFolder)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
            }
            Button { text: qsTr("Abrir carpeta"); onClicked: dlg.ai.openModelsFolder() }
        }
    }
}
