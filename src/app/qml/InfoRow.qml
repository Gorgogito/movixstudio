import QtQuick
import QtQuick.Layouts
import MovixStudio

// Fila "etiqueta: valor" del panel de información.
RowLayout {
    property alias label: labelText.text
    property alias value: valueText.text

    Layout.fillWidth: true
    spacing: Theme.spacing

    Text {
        id: labelText
        Layout.preferredWidth: 110
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall + 1
        elide: Text.ElideRight
    }
    Text {
        id: valueText
        Layout.fillWidth: true
        color: Theme.text
        font.pixelSize: Theme.fontSmall + 1
        wrapMode: Text.WrapAnywhere
        maximumLineCount: 3
    }
}
