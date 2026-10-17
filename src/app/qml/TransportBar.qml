import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

// Barra de transporte: posición, búsqueda y controles de reproducción y volumen.
Rectangle {
    id: bar

    required property Player player

    implicitHeight: 104
    color: Theme.surface

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
    }

    component TimeLabel: Text {
        color: Theme.text
        font.family: Theme.monoFont
        font.pixelSize: Theme.fontNormal
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 10
        anchors.bottomMargin: 10
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            TimeLabel { text: Format.timecode(bar.player.position, bar.player.frameRate) }

            MxSlider {
                id: seekSlider
                Layout.fillWidth: true
                enabled: bar.player.hasMedia
                from: 0
                to: Math.max(bar.player.duration, 0.001)
                onMoved: bar.player.seek(value)

                Binding on value {
                    when: !seekSlider.pressed
                    value: bar.player.position
                    restoreMode: Binding.RestoreNone
                }
            }

            TimeLabel {
                text: Format.timecode(bar.player.duration, bar.player.frameRate)
                color: Theme.textMuted
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Item { Layout.fillWidth: true; Layout.preferredWidth: 1 }

            IconButton {
                iconName: "skip-start"
                tip: qsTr("Ir al inicio (Inicio)")
                enabled: bar.player.hasMedia
                onClicked: bar.player.seek(0)
            }
            IconButton {
                iconName: "step-back"
                tip: qsTr("Fotograma anterior (←)")
                enabled: bar.player.hasVideo
                onClicked: bar.player.stepFrames(-1)
            }
            IconButton {
                iconName: bar.player.playing ? "pause" : "play"
                tip: bar.player.playing ? qsTr("Pausar (Espacio)") : qsTr("Reproducir (Espacio)")
                accent: true
                size: 46
                enabled: bar.player.hasMedia
                onClicked: bar.player.togglePlayPause()
            }
            IconButton {
                iconName: "step-forward"
                tip: qsTr("Fotograma siguiente (→)")
                enabled: bar.player.hasVideo
                onClicked: bar.player.stepFrames(1)
            }
            IconButton {
                iconName: "skip-end"
                tip: qsTr("Ir al final (Fin)")
                enabled: bar.player.hasMedia
                onClicked: bar.player.seek(bar.player.duration)
            }

            Item { Layout.fillWidth: true; Layout.preferredWidth: 1 }

            IconButton {
                iconName: bar.player.muted || bar.player.volume === 0 ? "volume-mute" : "volume"
                tip: bar.player.muted ? qsTr("Activar sonido (M)") : qsTr("Silenciar (M)")
                onClicked: bar.player.muted = !bar.player.muted
            }
            MxSlider {
                id: volumeSlider
                Layout.preferredWidth: 110
                from: 0
                to: 1
                value: bar.player.volume
                onMoved: {
                    bar.player.volume = value
                    bar.player.muted = false
                }
                ToolTip.visible: pressed
                ToolTip.text: Math.round(value * 100) + " %"
            }
        }
    }
}
