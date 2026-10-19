import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

// Biblioteca de medios del proyecto: importar, previsualizar y arrastrar a la línea de tiempo.
Rectangle {
    id: bin

    required property ProjectController project
    signal importRequested()
    signal previewRequested(var mediaId)

    color: Theme.surface

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 6

            Text {
                text: qsTr("Medios del proyecto")
                color: Theme.text
                font.pixelSize: Theme.fontNormal + 1
                font.bold: true
                Layout.fillWidth: true
            }
            BusyIndicator {
                running: bin.project.importing > 0
                visible: running
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
            }
            IconButton {
                iconName: "import"
                tip: qsTr("Importar medios (Ctrl+I)")
                size: 30
                onClicked: bin.importRequested()
            }
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 4
            clip: true
            model: bin.project.media
            cellWidth: Math.max(120, Math.floor(width / Math.max(1, Math.floor(width / 150))))
            cellHeight: cellWidth * 0.56 + 40
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: tile
                required property var mediaId
                required property string name
                required property string path
                required property real duration
                required property bool hasVideo
                required property bool hasAudio
                required property string thumbnail
                required property int usage
                required property int index

                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    id: card
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: Theme.radius
                    color: hover.hovered || dragArea.drag.active ? Theme.surfaceHover : Theme.surfaceRaised
                    border.color: grid.currentIndex === tile.index ? Theme.accent : "transparent"
                    border.width: 2

                    Drag.active: dragArea.drag.active
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.CopyAction
                    Drag.mimeData: { "application/x-movix-media": String(tile.mediaId) }
                    Drag.imageSource: tile.thumbnail

                    Rectangle {
                        id: thumbBox
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 4
                        height: width * 0.56
                        radius: 4
                        color: Theme.viewer
                        clip: true

                        Image {
                            anchors.fill: parent
                            source: tile.thumbnail
                            sourceSize.width: 320
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            visible: tile.hasVideo
                        }
                        IconImage {
                            anchors.centerIn: parent
                            source: "qrc:/qt/qml/MovixStudio/icons/music.svg"
                            sourceSize: Qt.size(36, 36)
                            visible: !tile.hasVideo
                            color: Theme.clipAudio
                        }
                        Rectangle {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 4
                            width: durationText.implicitWidth + 8
                            height: durationText.implicitHeight + 4
                            radius: 3
                            color: "#B0000000"
                            Text {
                                id: durationText
                                anchors.centerIn: parent
                                text: Format.duration(tile.duration)
                                color: "white"
                                font.pixelSize: 10
                            }
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 4
                            width: 8
                            height: 8
                            radius: 4
                            color: Theme.clipSelected
                            visible: tile.usage > 0
                            ToolTip.visible: usageHover.hovered
                            ToolTip.text: qsTr("En uso en la línea de tiempo")
                            HoverHandler { id: usageHover }
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: thumbBox.bottom
                        anchors.margins: 6
                        text: tile.name
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall + 1
                        elide: Text.ElideMiddle
                    }

                    HoverHandler { id: hover }
                    ToolTip.visible: hover.hovered && !dragArea.drag.active
                    ToolTip.delay: 800
                    ToolTip.text: tile.path

                    MouseArea {
                        id: dragArea
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        drag.target: dragProxy
                        onPressed: grid.currentIndex = tile.index
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton)
                                contextMenu.popup()
                        }
                        onDoubleClicked: bin.previewRequested(tile.mediaId)
                    }
                    Item { id: dragProxy }

                    Menu {
                        id: contextMenu
                        MenuItem { text: qsTr("Vista previa"); onTriggered: bin.previewRequested(tile.mediaId) }
                        MenuItem { text: qsTr("Añadir a la línea de tiempo"); onTriggered: bin.project.appendToTimeline(tile.mediaId) }
                        MenuSeparator {}
                        MenuItem {
                            text: tile.usage > 0 ? qsTr("Quitar del proyecto (y sus clips)") : qsTr("Quitar del proyecto")
                            onTriggered: bin.project.removeMedia(tile.mediaId)
                        }
                    }
                }
            }

            // Estado vacío.
            Column {
                anchors.centerIn: parent
                spacing: 10
                visible: grid.count === 0
                width: parent.width - 32
                IconImage {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "qrc:/qt/qml/MovixStudio/icons/import.svg"
                    sourceSize: Qt.size(40, 40)
                    color: Theme.textMuted
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Importa videos, audio o imágenes, o arrástralos aquí desde el explorador.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall + 1
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Importar medios…")
                    focusPolicy: Qt.NoFocus
                    onClicked: bin.importRequested()
                }
            }
        }
    }
}
