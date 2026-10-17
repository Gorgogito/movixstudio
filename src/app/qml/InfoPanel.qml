import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

// Panel lateral con las propiedades técnicas del medio y el estado de la reproducción.
Rectangle {
    id: panel

    required property Player player
    readonly property var info: player.mediaInfo

    color: Theme.surface

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: Theme.border
    }

    component SectionTitle: Text {
        Layout.topMargin: 14
        Layout.bottomMargin: 2
        color: Theme.accentHover
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 14
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 6

            Text {
                text: qsTr("Información del medio")
                color: Theme.text
                font.pixelSize: Theme.fontNormal + 1
                font.bold: true
            }

            Text {
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: !panel.player.hasMedia
                text: qsTr("Abre un archivo para ver sus propiedades.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall + 1
                wrapMode: Text.WordWrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.player.hasMedia
                spacing: 6

                SectionTitle { text: qsTr("Archivo") }
                InfoRow { label: qsTr("Nombre"); value: Format.fileName(panel.player.source) }
                InfoRow { label: qsTr("Contenedor"); value: panel.info.container ?? "—" }
                InfoRow { label: qsTr("Duración"); value: Format.duration(panel.player.duration) }
                InfoRow { label: qsTr("Tasa de bits"); value: Format.bitRate(panel.info.bitRate) }

                SectionTitle { text: qsTr("Video"); visible: panel.player.hasVideo }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: panel.player.hasVideo
                    spacing: 6
                    InfoRow { label: qsTr("Códec"); value: (panel.info.videoCodec ?? "").toUpperCase() }
                    InfoRow { label: qsTr("Resolución"); value: (panel.info.width ?? 0) + " × " + (panel.info.height ?? 0) }
                    InfoRow {
                        label: qsTr("Fotogramas/s")
                        value: Number(panel.info.frameRate ?? 0).toLocaleString(Qt.locale(Language.current === "es" ? "es_ES" : "en_US"), "f", 3)
                    }
                    InfoRow { label: qsTr("Formato de píxel"); value: panel.info.pixelFormat ?? "—" }
                    InfoRow { label: qsTr("Decodificador"); value: panel.player.videoDecoder || "—" }
                }

                SectionTitle { text: qsTr("Audio"); visible: panel.player.hasAudio }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: panel.player.hasAudio
                    spacing: 6
                    InfoRow { label: qsTr("Códec"); value: (panel.info.audioCodec ?? "").toUpperCase() }
                    InfoRow { label: qsTr("Frecuencia"); value: (panel.info.sampleRate ?? 0) + " Hz" }
                    InfoRow { label: qsTr("Canales"); value: String(panel.info.channels ?? 0) }
                    InfoRow { label: qsTr("Salida"); value: panel.player.audioDevice || "—" }
                }

                SectionTitle { text: qsTr("Reproducción") }
                InfoRow { label: qsTr("Mostrados/s"); value: panel.player.presentedFps.toFixed(1) }
                InfoRow { label: qsTr("Descartados"); value: String(panel.player.droppedFrames) }
            }
        }
    }
}
