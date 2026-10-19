import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Texto a voz con Kokoro (en local): el audio generado se coloca en el cabezal.
Dialog {
    id: dlg

    required property AiController ai

    title: qsTr("Texto a voz")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 520
    property double downloadMB: 0

    onAboutToShow: downloadMB = ai.pendingDownloadMB("kokoro-82m")

    footer: DialogButtonBox {
        Button {
            text: dlg.downloadMB > 0 ? qsTr("Descargar y generar") : qsTr("Generar")
            highlighted: true
            enabled: textArea.text.trim().length > 0
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button { text: qsTr("Cancelar"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: ai.speak(textArea.text, ai.voices()[voiceCombo.currentIndex].id, speedSlider.value)

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            TextArea {
                id: textArea
                wrapMode: TextArea.Wrap
                placeholderText: qsTr("Escribe el texto que se leerá en voz alta…")
            }
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 12
            Label { text: qsTr("Voz") }
            ComboBox {
                id: voiceCombo
                Layout.fillWidth: true
                Layout.columnSpan: 2
                model: dlg.ai.voices()
                textRole: "name"
            }
            Label { text: qsTr("Velocidad") }
            Slider { id: speedSlider; Layout.fillWidth: true; from: 0.6; to: 1.6; stepSize: 0.05; value: 1.0 }
            Label { text: speedSlider.value.toLocaleString(Qt.locale(), "f", 2) + "×"; Layout.minimumWidth: 44 }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            text: qsTr("La voz se genera en este equipo con Kokoro y se añade en el cabezal, en una pista de audio libre.")
                  + (dlg.downloadMB > 0 ? " " + qsTr("Se descargará el modelo de voz (%1 MB).").arg(Math.round(dlg.downloadMB)) : "")
        }
    }
}
