import QtQuick
import QtQuick.Controls
import MovixStudio

// Deslizador con la identidad visual de MovixStudio (pista fina y control circular).
Slider {
    id: control

    focusPolicy: Qt.NoFocus

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: Theme.border

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.enabled ? Theme.accent : Theme.textDisabled
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 14
        height: 14
        radius: 7
        color: control.pressed ? Theme.accentHover : Theme.text
        visible: control.enabled
    }
}
