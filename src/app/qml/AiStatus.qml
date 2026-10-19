import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Progreso de la tarea de IA en curso (descarga o transcripción), con opción de cancelar.
Rectangle {
    id: status

    required property AiController ai

    visible: ai.busy
    z: 90
    width: 380
    height: layout.implicitHeight + 20
    radius: Theme.radius
    color: Theme.surfaceRaised
    border.color: Theme.accent

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: status.ai.status
                elide: Text.ElideRight
            }
            Button {
                text: qsTr("Cancelar")
                flat: true
                onClicked: status.ai.cancel()
            }
        }
        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 1
            value: Math.max(0, status.ai.progress)
            indeterminate: status.ai.progress < 0
        }
    }
}
