import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Scopes del visor: dos a la vez, cada uno con su tipo. Solo analizan mientras se ven.
Rectangle {
    id: panel

    required property VideoView view
    readonly property var typeNames: [qsTr("Forma de onda"), qsTr("Desfile RGB"), qsTr("Vectorscopio"), qsTr("Histograma")]

    color: Theme.surface
    onVisibleChanged: view.scopesActive = visible
    Component.onCompleted: view.scopesActive = visible
    Component.onDestruction: view.scopesActive = false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        Repeater {
            model: [0, 2]
            ColumnLayout {
                id: slot
                required property int modelData
                property int type: modelData
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 4
                ComboBox {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 26
                    font.pixelSize: Theme.fontSmall + 1
                    model: panel.typeNames
                    currentIndex: slot.type
                    onActivated: (i) => slot.type = i
                }
                ScopeView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    source: panel.view
                    type: slot.type
                }
            }
        }
    }
}
