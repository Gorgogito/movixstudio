import QtQuick
import QtQuick.Controls.impl
import MovixStudio

// Vista multicámara sobre el visor: los ángulos del clip multicámara en el cabezal se ven en
// cuadrícula (la compone el reproductor); aquí se rotulan, se resalta el que está en el montaje
// y un clic en uno corta a él en el cabezal (también reproduciendo, como las teclas 1–9).
Item {
    id: overlay

    required property ProjectController project
    required property TimelinePlayer player
    property var info: ({})
    readonly property bool active: info.clipId !== undefined

    function refresh() { info = project.multicamAt(player.positionFrame) }
    Component.onCompleted: refresh()
    Connections {
        target: overlay.player
        function onPositionChanged() { overlay.refresh() }
    }
    Connections {
        target: overlay.project
        function onTimelineChanged() { overlay.refresh() }
    }

    // Rectángulo del lienzo dentro del visor (mismo ajuste que hace el compositor).
    readonly property real canvasAspect: project.canvasWidth / Math.max(1, project.canvasHeight)
    readonly property rect canvas: {
        const w = width, h = height
        if (w <= 0 || h <= 0)
            return Qt.rect(0, 0, 0, 0)
        if (w / h > canvasAspect)
            return Qt.rect((w - h * canvasAspect) / 2, 0, h * canvasAspect, h)
        return Qt.rect(0, (h - w / canvasAspect) / 2, w, w / canvasAspect)
    }

    // Lo que suena (un micrófono aparte no tiene celda).
    Rectangle {
        visible: overlay.active
        x: overlay.canvas.x + 8
        y: overlay.canvas.y + overlay.canvas.height - height - 8
        width: audioRow.implicitWidth + 12
        height: audioRow.implicitHeight + 6
        radius: 3
        color: "#B0000000"
        Row {
            id: audioRow
            anchors.centerIn: parent
            spacing: 4
            IconImage {
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/qt/qml/MovixStudio/icons/volume.svg"
                sourceSize: Qt.size(12, 12)
                color: "white"
            }
            Text {
                text: overlay.active ? qsTr("Audio: %1").arg(overlay.info.audioSources[overlay.info.audioAngle < 0 ? overlay.info.angle : overlay.info.audioAngle] ?? "")
                                     : ""
                color: "white"
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    Repeater {
        model: overlay.active ? overlay.info.cells : []
        delegate: Rectangle {
            id: cell
            required property var modelData
            required property int index
            readonly property bool current: overlay.info.angle === index
            readonly property bool sounding: (overlay.info.audioAngle < 0 ? overlay.info.angle : overlay.info.audioAngle) === index

            x: overlay.canvas.x + modelData[0] * overlay.canvas.width
            y: overlay.canvas.y + modelData[1] * overlay.canvas.height
            width: modelData[2] * overlay.canvas.width
            height: modelData[3] * overlay.canvas.height
            color: "transparent"
            border.width: current ? 3 : 1
            border.color: current ? Theme.accent : (hover.hovered ? "#C0FFFFFF" : "#50FFFFFF")

            Rectangle {
                x: 6
                y: 6
                width: label.implicitWidth + (speaker.visible ? speaker.width + 4 : 0) + 12
                height: label.implicitHeight + 6
                radius: 3
                color: cell.current ? Theme.accent : "#B0000000"
                Text {
                    id: label
                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: (cell.index < 9 ? (cell.index + 1) + "  " : "") + overlay.info.angles[cell.index]
                    color: "white"
                    font.pixelSize: Theme.fontSmall
                    font.bold: cell.current
                }
                IconImage {
                    id: speaker
                    anchors.left: label.right
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/qt/qml/MovixStudio/icons/volume.svg"
                    sourceSize: Qt.size(12, 12)
                    color: "white"
                    visible: cell.sounding
                }
            }

            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            MouseArea {
                anchors.fill: parent
                onClicked: overlay.project.switchAngle(cell.index)
            }
        }
    }
}
