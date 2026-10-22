import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Pide un nombre (por ejemplo, el de una plantilla). `named` se emite con el nombre sin espacios
// sobrantes; con un nombre vacío no se puede aceptar.
Dialog {
    id: dlg

    property string label: qsTr("Nombre")
    property string acceptText: qsTr("Guardar")
    property string initialName: ""
    signal named(string name)

    modal: true
    anchors.centerIn: Overlay.overlay
    width: 400
    onAboutToShow: {
        field.text = initialName
        field.selectAll()
        field.forceActiveFocus()
    }
    onAccepted: named(field.text.trim())

    footer: DialogButtonBox {
        Button {
            text: dlg.acceptText
            highlighted: true
            enabled: field.text.trim().length > 0
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button { text: qsTr("Cancelar"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: dlg.label
            wrapMode: Text.WordWrap
        }
        TextField {
            id: field
            Layout.fillWidth: true
            onAccepted: if (text.trim().length > 0) dlg.accept()
        }
    }
}
