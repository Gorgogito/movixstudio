import QtQuick
import QtQuick.Controls
import MovixStudio

// Regla de tiempo con marcas adaptadas al zoom. Pulsar o arrastrar mueve el cabezal.
Rectangle {
    id: ruler

    required property ProjectController project
    property real pxPerFrame: 1
    property real contentX: 0
    signal seekRequested(real frame)

    readonly property int fpsInt: Math.max(1, Math.round(project.frameRate))
    // Fotogramas entre marcas con etiqueta: el primer paso que deja al menos 90 px entre ellas.
    readonly property int step: {
        const f = fpsInt
        const candidates = [1, 2, 5, 10, f, 2 * f, 5 * f, 10 * f, 15 * f, 30 * f, 60 * f, 120 * f, 300 * f, 600 * f, 1800 * f, 3600 * f]
        for (const c of candidates)
            if (c * pxPerFrame >= 90)
                return c
        return candidates[candidates.length - 1]
    }
    readonly property real stepPx: step * pxPerFrame
    readonly property int firstTick: Math.floor(contentX / stepPx)

    function label(frame) {
        const tc = project.timecode(frame) // HH:MM:SS:FF
        if (step >= fpsInt)
            return tc.startsWith("00:") ? tc.substring(3, 8) : tc.substring(0, 8)
        return tc.substring(3)
    }

    color: Theme.surface
    clip: true

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }

    Repeater {
        model: Math.ceil(ruler.width / ruler.stepPx) + 2
        delegate: Item {
            required property int index
            readonly property int tick: ruler.firstTick + index
            x: tick * ruler.stepPx - ruler.contentX
            width: ruler.stepPx
            height: ruler.height

            Rectangle { width: 1; height: 10; anchors.bottom: parent.bottom; color: Theme.textMuted }
            // Marcas intermedias.
            Repeater {
                model: 4
                Rectangle {
                    required property int index
                    x: (index + 1) * parent.width / 5
                    width: 1
                    height: 5
                    anchors.bottom: parent.bottom
                    color: Theme.textDisabled
                }
            }
            Text {
                x: 4
                y: 3
                text: ruler.label(parent.tick * ruler.step)
                color: Theme.textMuted
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
        }
    }

    // Marcadores: triángulo en la regla (clic derecho lo quita).
    Repeater {
        model: ruler.project.markers
        delegate: Item {
            id: marker
            required property var modelData
            x: modelData.frame * ruler.pxPerFrame - ruler.contentX - 5
            visible: x > -10 && x < ruler.width
            width: 10
            height: ruler.height
            z: 2
            Canvas {
                anchors.bottom: parent.bottom
                width: 10
                height: 9
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = marker.modelData.color
                    ctx.beginPath()
                    ctx.moveTo(0, 0)
                    ctx.lineTo(10, 0)
                    ctx.lineTo(5, 9)
                    ctx.closePath()
                    ctx.fill()
                }
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                onClicked: (mouse) => {
                    if (mouse.button === Qt.RightButton)
                        ruler.project.removeMarker(marker.modelData.frame)
                    else
                        ruler.seekRequested(marker.modelData.frame)
                }
                ToolTip.visible: containsMouse && marker.modelData.label !== ""
                ToolTip.text: marker.modelData.label
                hoverEnabled: true
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        preventStealing: true
        onPressed: (mouse) => ruler.seekRequested(Math.max(0, Math.round((mouse.x + ruler.contentX) / ruler.pxPerFrame)))
        onPositionChanged: (mouse) => {
            if (pressed)
                ruler.seekRequested(Math.max(0, Math.round((mouse.x + ruler.contentX) / ruler.pxPerFrame)))
        }
    }
}
