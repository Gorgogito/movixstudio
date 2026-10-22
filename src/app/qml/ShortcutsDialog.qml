import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Atajos de teclado: pulsa el atajo de una acción y luego la combinación nueva (Esc cancela). Si
// otra acción la usaba, se le quita y se avisa.
Dialog {
    id: dlg

    required property ShortcutController keys
    property string capturing: "" // acción que espera su atajo nuevo
    property string message: ""

    title: qsTr("Atajos de teclado")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 560
    height: Math.min(680, Overlay.overlay ? Overlay.overlay.height - 80 : 680)
    onClosed: { capturing = ""; message = "" }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Restablecer todos")
            DialogButtonBox.buttonRole: DialogButtonBox.ResetRole
            onClicked: { dlg.keys.resetAll(); dlg.message = "" }
        }
        Button { text: qsTr("Cerrar"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }

    // Recoge la combinación: se adelanta a los atajos de la ventana (ShortcutOverride).
    Item {
        id: catcher
        focus: dlg.capturing !== ""
        Keys.onShortcutOverride: (event) => event.accepted = dlg.capturing !== ""
        Keys.onPressed: (event) => {
            if (dlg.capturing === "")
                return
            event.accepted = true
            if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                dlg.capturing = ""
                return
            }
            const seq = dlg.keys.sequenceFromKey(event.key, event.modifiers)
            if (seq === "")
                return // solo un modificador: se espera la tecla
            const other = dlg.keys.conflict(dlg.capturing, seq)
            dlg.keys.assign(dlg.capturing, seq)
            dlg.message = other !== "" ? qsTr("«%1» se quitó de «%2».").arg(dlg.keys.display(seq)).arg(other) : ""
            dlg.capturing = ""
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: dlg.capturing !== "" ? qsTr("Pulsa la combinación nueva (Esc para cancelar).")
                                       : qsTr("Haz clic en un atajo para cambiarlo.")
        }
        Label {
            Layout.fillWidth: true
            visible: dlg.message !== ""
            wrapMode: Text.WordWrap
            color: Theme.accentHover
            text: dlg.message
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dlg.keys.actions
            section.property: "group"
            section.delegate: Text {
                required property string section
                text: section
                topPadding: 8
                bottomPadding: 2
                color: Theme.accentHover
                font.pixelSize: Theme.fontSmall
                font.bold: true
                font.capitalization: Font.AllUppercase
            }
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                id: rowItem
                required property var modelData
                width: ListView.view.width - 12
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: rowItem.modelData.label
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall + 1
                    elide: Text.ElideRight
                }
                Button {
                    Layout.preferredWidth: 150
                    focusPolicy: Qt.NoFocus
                    highlighted: dlg.capturing === rowItem.modelData.id
                    text: dlg.capturing === rowItem.modelData.id ? qsTr("Pulsa una tecla…")
                          : (rowItem.modelData.shortcut !== "" ? rowItem.modelData.shortcut : qsTr("(ninguno)"))
                    font.bold: !rowItem.modelData.isDefault
                    onClicked: { dlg.message = ""; dlg.capturing = rowItem.modelData.id; catcher.forceActiveFocus() }
                }
                ToolButton {
                    text: "✕"
                    focusPolicy: Qt.NoFocus
                    enabled: rowItem.modelData.shortcut !== ""
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Sin atajo")
                    onClicked: dlg.keys.assign(rowItem.modelData.id, "")
                }
                ToolButton {
                    text: "↺"
                    focusPolicy: Qt.NoFocus
                    enabled: !rowItem.modelData.isDefault
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Restablecer")
                    onClicked: dlg.keys.reset(rowItem.modelData.id)
                }
            }
        }
    }
}
