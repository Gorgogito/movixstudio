import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Ajuste no animable (color, croma): etiqueta, valor editable y deslizador. Doble clic en el
// deslizador vuelve al valor por defecto. El valor se muestra multiplicado por `displayScale`.
ColumnLayout {
    id: row

    property string label
    property real from: -1
    property real to: 1
    property real value: 0
    property real defaultValue: 0
    property real displayScale: 100
    property int decimals: 0
    property string suffix: ""
    signal moved(real value)
    signal released()

    spacing: 0
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: row.label
            color: row.value !== row.defaultValue ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        TextField {
            Layout.preferredWidth: 52
            Layout.preferredHeight: 22
            topPadding: 2
            bottomPadding: 2
            horizontalAlignment: Text.AlignRight
            font.pixelSize: Theme.fontSmall + 1
            text: (row.value * row.displayScale).toFixed(row.decimals)
            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
            onEditingFinished: {
                const v = Number(text.replace(",", "."))
                if (!isNaN(v)) {
                    row.moved(Math.max(row.from, Math.min(row.to, v / row.displayScale)))
                    row.released()
                }
            }
        }
        Text {
            text: row.suffix
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            Layout.preferredWidth: 14
        }
    }
    MxSlider {
        Layout.fillWidth: true
        Layout.preferredHeight: 22
        from: row.from
        to: row.to
        value: row.value
        onMoved: row.moved(value)
        onPressedChanged: if (!pressed) row.released()
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: {
                row.moved(row.defaultValue)
                row.released()
            }
        }
    }
}
