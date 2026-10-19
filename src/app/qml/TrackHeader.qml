import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import MovixStudio

// Cabecera de una pista: nombre y conmutadores de visibilidad, sonido y bloqueo.
Rectangle {
    id: header

    required property var trackId
    required property string name
    required property string kind
    required property bool muted
    required property bool hidden
    required property bool locked
    property ProjectController project
    property int trackHeight: 56
    property int kindCount: 1

    width: parent ? parent.width : 160
    height: trackHeight
    color: Theme.surface

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    component Toggle: IconButton {
        property bool on: false
        size: 26
        icon.color: on ? Theme.clipSelected : Theme.textMuted
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 6
        spacing: 2

        IconImage {
            source: "qrc:/qt/qml/MovixStudio/icons/" + (header.kind === "video" ? "film" : "music") + ".svg"
            sourceSize: Qt.size(16, 16)
            color: Theme.textMuted
        }
        Text {
            text: header.name
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1 // sin ancho implícito: el nombre largo se recorta, no empuja los botones
            Layout.leftMargin: 6
        }
        Toggle {
            visible: header.kind === "video"
            iconName: header.hidden ? "eye-off" : "eye"
            on: header.hidden
            tip: header.hidden ? qsTr("Mostrar pista") : qsTr("Ocultar pista")
            onClicked: header.project.setTrackFlag(header.trackId, "hidden", !header.hidden)
        }
        Toggle {
            iconName: header.muted ? "volume-mute" : "volume"
            on: header.muted
            tip: header.muted ? qsTr("Activar sonido de la pista") : qsTr("Silenciar pista")
            onClicked: header.project.setTrackFlag(header.trackId, "muted", !header.muted)
        }
        Toggle {
            iconName: header.locked ? "lock" : "lock-open"
            on: header.locked
            tip: header.locked ? qsTr("Desbloquear pista") : qsTr("Bloquear pista")
            onClicked: header.project.setTrackFlag(header.trackId, "locked", !header.locked)
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        z: -1
        onClicked: menu.popup()
    }
    Menu {
        id: menu
        MenuItem {
            text: qsTr("Eliminar pista")
            enabled: header.kindCount > 1 && !header.locked
            onTriggered: header.project.removeTrack(header.trackId)
        }
    }
}
