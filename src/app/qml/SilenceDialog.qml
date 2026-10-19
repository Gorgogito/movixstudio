import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Eliminar silencios: detecta la voz (Silero VAD, en local) y corta los tramos sin ella en todas
// las pistas desbloqueadas, cerrando los huecos.
Dialog {
    id: dlg

    required property AiController ai

    title: qsTr("Eliminar silencios")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 460
    property double downloadMB: 0

    onAboutToShow: downloadMB = ai.pendingDownloadMB("")

    footer: DialogButtonBox {
        Button {
            text: qsTr("Eliminar silencios")
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button { text: qsTr("Cancelar"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: ai.removeSilences(sensitivity.value, minSilence.value, padding.value)

    GridLayout {
        anchors.fill: parent
        columns: 3
        columnSpacing: 12
        rowSpacing: 10

        Label { text: qsTr("Silencio mínimo") }
        Slider { id: minSilence; Layout.fillWidth: true; from: 200; to: 3000; stepSize: 50; value: 600 }
        Label { text: qsTr("%1 s").arg((minSilence.value / 1000).toLocaleString(Qt.locale(), "f", 2)); Layout.minimumWidth: 52 }

        Label { text: qsTr("Margen") }
        Slider { id: padding; Layout.fillWidth: true; from: 0; to: 500; stepSize: 10; value: 120 }
        Label { text: qsTr("%1 s").arg((padding.value / 1000).toLocaleString(Qt.locale(), "f", 2)) }

        Label { text: qsTr("Umbral de voz") }
        Slider { id: sensitivity; Layout.fillWidth: true; from: 0.2; to: 0.8; stepSize: 0.05; value: 0.5 }
        Label { text: Math.round(sensitivity.value * 100) + " %" }

        Label {
            Layout.columnSpan: 3
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            text: qsTr("Se cortan las pausas más largas que el silencio mínimo, dejando el margen antes y después de la voz. Un umbral más alto considera silencio la voz débil o el ruido de fondo. Las pistas bloqueadas no se tocan. Se puede deshacer.")
                  + (dlg.downloadMB > 0 ? " " + qsTr("Se descargará el modelo de detección de voz (%1 MB).").arg(dlg.downloadMB.toLocaleString(Qt.locale(), "f", 1)) : "")
        }
    }
}
