import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Visor con dos fuentes: la línea de tiempo (programa) o un medio de la biblioteca (vista previa).
Rectangle {
    id: panel

    required property ProjectController project
    property bool sourceMode: false
    property bool scopesVisible: false
    property string overlayMode: "transform" // ver TransformOverlay
    property int overlayMaskIndex: 0
    signal keyPicked()
    signal objectPicked(real u, real v)
    readonly property TimelinePlayer activePlayer: sourceMode ? project.sourcePlayer : project.player

    function showTimeline() {
        if (sourceMode) {
            project.sourcePlayer.pause()
            sourceMode = false
        }
    }
    function showSource() {
        project.player.pause()
        sourceMode = true
    }

    color: Theme.viewer

    component ModeButton: AbstractButton {
        id: mode
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 28
        implicitWidth: label.implicitWidth + 24
        contentItem: Text {
            id: label
            text: mode.text
            color: mode.active ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
            font.bold: mode.active
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.radius
            color: mode.active ? Theme.surfaceHover : (mode.hovered ? Theme.surfaceRaised : "transparent")
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 12
                spacing: 4
                ModeButton {
                    text: qsTr("Línea de tiempo")
                    active: !panel.sourceMode
                    onClicked: panel.showTimeline()
                }
                ModeButton {
                    text: panel.project.sourceName ? qsTr("Medio: %1").arg(panel.project.sourceName) : qsTr("Medio")
                    active: panel.sourceMode
                    enabled: panel.project.sourceName !== ""
                    Layout.maximumWidth: 260
                    onClicked: panel.showSource()
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "%1 × %2 · %3 fps".arg(panel.project.canvasWidth).arg(panel.project.canvasHeight).arg(panel.project.frameRateText)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    visible: !panel.sourceMode
                }
                ModeButton {
                    text: qsTr("Scopes")
                    active: panel.scopesVisible
                    onClicked: panel.scopesVisible = !panel.scopesVisible
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            VideoView {
                id: videoView
                Layout.fillWidth: true
                Layout.fillHeight: true
                player: panel.activePlayer

                MouseArea {
                    anchors.fill: parent
                    onClicked: panel.activePlayer.togglePlayPause()
                }
                Text {
                    anchors.centerIn: parent
                    visible: !panel.sourceMode && !panel.project.player.hasContent
                    text: qsTr("Arrastra medios a la línea de tiempo para empezar a editar")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontNormal
                }
                TransformOverlay {
                    anchors.fill: parent
                    visible: !panel.sourceMode && !panel.activePlayer.playing
                    project: panel.project
                    mode: panel.overlayMode
                    maskIndex: panel.overlayMaskIndex
                    onPicked: panel.keyPicked()
                    onObjectPicked: (u, v) => panel.objectPicked(u, v)
                }
            }

            ScopesPanel {
                visible: panel.scopesVisible
                Layout.fillHeight: true
                Layout.preferredWidth: Math.min(320, panel.width * 0.35)
                view: videoView
            }
        }

        TransportBar {
            Layout.fillWidth: true
            player: panel.activePlayer
        }
    }
}
