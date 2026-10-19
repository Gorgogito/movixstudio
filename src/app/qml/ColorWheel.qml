import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Rueda de color de sombras, medios o luces. El punto empuja la imagen hacia un tono: su posición
// es un desplazamiento en el plano Cb/Cr (el mismo que el vectorscopio), convertido a un
// desplazamiento RGB que no cambia la luminancia. El deslizador de abajo sube o baja el nivel.
ColumnLayout {
    id: wheel

    property string label
    property var values: [0, 0, 0, 0] // r, g, b, master
    signal wheelMoved(real r, real g, real b)
    signal masterMoved(real value)
    signal released()

    readonly property real chromaScale: 0.3 // magnitud de (Cb, Cr) en el borde de la rueda
    readonly property point puck: {
        const cb = (values[2] ?? 0) / 1.8556
        const cr = (values[0] ?? 0) / 1.5748
        return Qt.point(cb / chromaScale, -cr / chromaScale)
    }
    readonly property bool neutral: (values[0] ?? 0) === 0 && (values[1] ?? 0) === 0 && (values[2] ?? 0) === 0 && (values[3] ?? 0) === 0

    function emitPuck(px, py) {
        const len = Math.hypot(px, py)
        if (len > 1) {
            px /= len
            py /= len
        }
        const cb = px * chromaScale, cr = -py * chromaScale
        const r = 1.5748 * cr, b = 1.8556 * cb
        const g = -(0.2126 * r + 0.0722 * b) / 0.7152
        wheelMoved(r, g, b)
    }

    spacing: 2

    Item {
        id: disk
        Layout.fillWidth: true
        Layout.preferredHeight: width
        readonly property real radius: Math.min(width, height) / 2 - 2

        Image {
            anchors.centerIn: parent
            width: Math.max(8, Math.round(disk.radius * 2))
            height: width
            sourceSize: Qt.size(width, height)
            source: "image://colorwheel/" + width
            smooth: true
        }
        Rectangle { // cruz central
            anchors.centerIn: parent
            width: 1; height: disk.radius * 0.3
            color: "#80FFFFFF"
        }
        Rectangle {
            anchors.centerIn: parent
            width: disk.radius * 0.3; height: 1
            color: "#80FFFFFF"
        }
        Rectangle {
            id: puckItem
            width: 12
            height: 12
            radius: 6
            x: disk.width / 2 + wheel.puck.x * disk.radius - width / 2
            y: disk.height / 2 + wheel.puck.y * disk.radius - height / 2
            color: "transparent"
            border.width: 2
            border.color: "white"
        }
        MouseArea {
            anchors.fill: parent
            preventStealing: true
            function update(m) {
                wheel.emitPuck((m.x - disk.width / 2) / disk.radius, (m.y - disk.height / 2) / disk.radius)
            }
            onPressed: (m) => update(m)
            onPositionChanged: (m) => { if (pressed) update(m) }
            onReleased: wheel.released()
            onDoubleClicked: {
                wheel.wheelMoved(0, 0, 0)
                wheel.released()
            }
        }
    }
    Text {
        Layout.alignment: Qt.AlignHCenter
        text: wheel.label
        color: wheel.neutral ? Theme.textMuted : Theme.text
        font.pixelSize: Theme.fontSmall
    }
    MxSlider {
        Layout.fillWidth: true
        Layout.preferredHeight: 20
        from: -1
        to: 1
        value: wheel.values[3] ?? 0
        onMoved: wheel.masterMoved(value)
        onPressedChanged: if (!pressed) wheel.released()
        TapHandler {
            onDoubleTapped: {
                wheel.masterMoved(0)
                wheel.released()
            }
        }
    }
}
