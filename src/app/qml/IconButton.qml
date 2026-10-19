import QtQuick
import QtQuick.Controls
import MovixStudio

// Botón de icono de la barra de transporte. No toma el foco para que los atajos
// de teclado (espacio, flechas) sigan funcionando en toda la ventana.
ToolButton {
    id: control

    property string iconName
    property string tip
    property bool accent: false
    property int size: 36

    implicitWidth: size
    implicitHeight: size
    focusPolicy: Qt.NoFocus
    display: AbstractButton.IconOnly
    icon.source: iconName ? "qrc:/qt/qml/MovixStudio/icons/" + iconName + ".svg" : ""
    icon.width: Math.round(size * 0.55)
    icon.height: Math.round(size * 0.55)
    icon.color: !enabled ? Theme.textDisabled : (accent ? "#FFFFFF" : Theme.text)

    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 600
    ToolTip.text: tip

    background: Rectangle {
        radius: control.accent ? control.width / 2 : Theme.radius
        color: {
            if (control.accent)
                return control.pressed ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accent)
            if (control.pressed)
                return Theme.border
            return control.hovered ? Theme.surfaceHover : "transparent"
        }
        opacity: control.enabled ? 1 : 0.5
    }
}
