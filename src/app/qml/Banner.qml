import QtQuick
import MovixStudio

// Aviso no bloqueante en la parte superior de la ventana (información o error).
Rectangle {
    id: banner

    function show(message, isError) {
        text.text = message
        error = isError
        visible = true
        hideTimer.interval = isError ? 9000 : 5000
        hideTimer.restart()
    }

    property bool error: false

    z: 100
    visible: false
    width: Math.min(parent.width - 48, text.implicitWidth + 40)
    height: text.implicitHeight + 20
    radius: Theme.radius
    color: error ? Theme.dangerSurface : Theme.noticeSurface
    border.color: error ? Theme.danger : Theme.accent

    Text {
        id: text
        anchors.centerIn: parent
        width: parent.width - 32
        color: Theme.text
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
    MouseArea {
        anchors.fill: parent
        onClicked: banner.visible = false
    }
    Timer {
        id: hideTimer
        onTriggered: banner.visible = false
    }
}
