import QtQuick
import QtQuick.Controls
import MovixStudio

// Botón del keyframe en el cabezal: clic lo añade o lo quita; clic derecho sobre un keyframe elige
// cómo se llega al siguiente. La forma indica la interpolación: rombo (lineal), cuadrado (mantener)
// o círculo (suave).
AbstractButton {
    id: button

    property bool keyHere: false
    property bool animated: false
    property string interp: "linear"
    property string addTip: qsTr("Añadir keyframe en el cabezal")
    property string removeTip: qsTr("Quitar keyframe en el cabezal")
    signal toggled()
    signal interpolationChosen(string interp)

    implicitWidth: 22
    implicitHeight: 22
    focusPolicy: Qt.NoFocus
    ToolTip.visible: hovered
    ToolTip.delay: 600
    ToolTip.text: keyHere ? removeTip + qsTr(" (clic derecho: interpolación)") : addTip
    onClicked: toggled()

    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: button.keyHere
        onTapped: menu.popup()
    }

    contentItem: Item {
        Rectangle {
            anchors.centerIn: parent
            width: button.interp === "smooth" && button.keyHere ? 10 : 9
            height: width
            radius: button.interp === "smooth" && button.keyHere ? width / 2 : 0
            rotation: button.interp === "linear" || !button.keyHere ? 45 : 0
            color: button.keyHere ? Theme.clipSelected : "transparent"
            border.width: 1.5
            border.color: button.animated ? Theme.clipSelected : (button.hovered ? Theme.text : Theme.textMuted)
        }
    }

    Menu {
        id: menu
        MenuItem { text: qsTr("Lineal"); checkable: true; checked: button.interp === "linear"; onTriggered: button.interpolationChosen("linear") }
        MenuItem { text: qsTr("Suave (acelera y frena)"); checkable: true; checked: button.interp === "smooth"; onTriggered: button.interpolationChosen("smooth") }
        MenuItem { text: qsTr("Mantener (salto al siguiente)"); checkable: true; checked: button.interp === "hold"; onTriggered: button.interpolationChosen("hold") }
    }
}
