import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MovixStudio

// Efectos de composición del clip: clave de croma (fondo verde/azul) y máscara de forma.
ColumnLayout {
    id: panel

    required property ProjectController project
    required property var info
    property bool picking: false        // cuentagotas activo sobre el visor
    signal pickToggled(bool on)

    readonly property var clipId: info.clipId
    readonly property var key: info.chromaKey ?? ({})
    readonly property var masks: info.masks ?? []
    property int maskIndex: 0         // máscara seleccionada (la que se edita en el visor)
    signal maskSelected(int index)

    function shapeLabel(shape) {
        return { rectangle: qsTr("Rectángulo"), ellipse: qsTr("Elipse"), linear: qsTr("Lineal"), path: qsTr("Forma libre") }[shape] ?? shape
    }
    function reshape(smooth) {
        const geo = project.clipGeometry(clipId)
        const m = (geo.masks ?? [])[maskIndex]
        if (m && m.points)
            project.setMaskPoints(clipId, maskIndex, project.smoothMaskPoints(m.points, smooth))
        project.endInteraction()
    }

    function setKey(field, value) {
        const m = {}
        m[field] = value
        project.setChromaKey(clipId, m)
    }

    spacing: 6
    Layout.fillWidth: true

    ColorDialog {
        id: keyColorDialog
        onAccepted: {
            panel.setKey("color", selectedColor.toString())
            panel.project.endInteraction()
        }
    }

    Text {
        Layout.topMargin: 6
        text: qsTr("Clave de croma")
        color: Theme.accentHover
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }
    RowLayout {
        Layout.fillWidth: true
        Text { text: qsTr("Quitar un color de fondo"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall + 1; Layout.fillWidth: true }
        Switch {
            focusPolicy: Qt.NoFocus
            checked: panel.key.enabled ?? false
            onToggled: {
                panel.setKey("enabled", checked)
                panel.project.endInteraction()
            }
        }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.key.enabled === true
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Text { text: qsTr("Color clave"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall + 1; Layout.fillWidth: true }
            Rectangle {
                width: 44; height: 22; radius: 4
                color: panel.key.color ?? "#00ff00"
                border.color: Theme.border
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        keyColorDialog.selectedColor = parent.color
                        keyColorDialog.open()
                    }
                }
            }
            Button {
                text: panel.picking ? qsTr("Haz clic en el visor") : qsTr("Cuentagotas")
                checkable: true
                checked: panel.picking
                focusPolicy: Qt.NoFocus
                onToggled: panel.pickToggled(checked)
            }
        }
        SliderRow {
            label: qsTr("Tolerancia")
            from: 0; to: 1; defaultValue: 0.25; suffix: "%"
            value: panel.key.tolerance ?? 0.25
            onMoved: (v) => panel.setKey("tolerance", v)
            onReleased: panel.project.endInteraction()
        }
        SliderRow {
            label: qsTr("Suavidad del borde")
            from: 0; to: 1; defaultValue: 0.1; suffix: "%"
            value: panel.key.softness ?? 0.1
            onMoved: (v) => panel.setKey("softness", v)
            onReleased: panel.project.endInteraction()
        }
        SliderRow {
            label: qsTr("Quitar derrame de color")
            from: 0; to: 1; defaultValue: 0.5; suffix: "%"
            value: panel.key.spill ?? 0.5
            onMoved: (v) => panel.setKey("spill", v)
            onReleased: panel.project.endInteraction()
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSmall
            text: qsTr("Coloca el clip en una pista superior: lo que se quite deja ver las pistas de debajo.")
        }
    }

    // ---- Máscaras ----
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 14
        Text {
            text: qsTr("Máscaras")
            color: Theme.accentHover
            font.pixelSize: Theme.fontSmall
            font.bold: true
            font.letterSpacing: 0.8
            font.capitalization: Font.AllUppercase
            Layout.fillWidth: true
        }
        Button {
            text: qsTr("Añadir")
            enabled: panel.masks.length < 8
            focusPolicy: Qt.NoFocus
            onClicked: addMenu.popup()
            Menu {
                id: addMenu
                Repeater {
                    model: [{ shape: "rectangle", label: qsTr("Rectángulo") }, { shape: "ellipse", label: qsTr("Elipse") },
                            { shape: "linear", label: qsTr("Lineal") }, { shape: "path", label: qsTr("Forma libre") }]
                    MenuItem {
                        required property var modelData
                        text: modelData.label
                        onTriggered: {
                            const i = panel.project.addMask(panel.clipId, modelData.shape)
                            if (i >= 0)
                                panel.maskSelected(i)
                        }
                    }
                }
            }
        }
    }
    Text {
        visible: panel.masks.length === 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDisabled
        font.pixelSize: Theme.fontSmall
        text: qsTr("Sin máscaras: se ve toda la capa.")
    }
    Repeater {
        model: panel.masks
        Rectangle {
            id: maskRow
            required property int index
            required property var modelData
            readonly property bool selected: panel.maskIndex === index
            Layout.fillWidth: true
            implicitHeight: rowLayout.implicitHeight + 8
            radius: Theme.radius
            color: selected ? Theme.surfaceHover : Theme.surfaceRaised
            border.width: selected ? 1 : 0
            border.color: Theme.accent
            MouseArea {
                anchors.fill: parent
                onClicked: panel.maskSelected(maskRow.index)
            }
            RowLayout {
                id: rowLayout
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                Text {
                    text: (maskRow.index + 1) + ". " + panel.shapeLabel(maskRow.modelData.shape)
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall + 1
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                ComboBox {
                    Layout.preferredWidth: 96
                    font.pixelSize: Theme.fontSmall
                    readonly property var modes: ["add", "subtract", "intersect"]
                    model: [qsTr("Sumar"), qsTr("Restar"), qsTr("Intersecar")]
                    currentIndex: Math.max(0, modes.indexOf(maskRow.modelData.mode))
                    onActivated: (i) => panel.project.setMaskOptions(panel.clipId, maskRow.index, modes[i], maskRow.modelData.invert)
                }
                CheckBox {
                    text: qsTr("Inv.")
                    checked: maskRow.modelData.invert
                    focusPolicy: Qt.NoFocus
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Invertir la máscara")
                    onToggled: panel.project.setMaskOptions(panel.clipId, maskRow.index, maskRow.modelData.mode, checked)
                }
                IconButton {
                    size: 22
                    iconName: "trash"
                    tip: qsTr("Quitar la máscara")
                    onClicked: {
                        panel.project.removeMask(panel.clipId, maskRow.index)
                        panel.maskSelected(Math.max(0, maskRow.index - 1))
                    }
                }
            }
        }
    }
    ColumnLayout {
        id: maskProps
        Layout.fillWidth: true
        visible: panel.maskIndex < panel.masks.length
        readonly property var mask: panel.masks[panel.maskIndex] ?? ({})
        readonly property string base: "mask." + panel.maskIndex + "."
        spacing: 8
        PropertyRow { project: panel.project; clipId: panel.clipId; prop: maskProps.base + "x"; label: qsTr("Centro X"); from: 0; to: 1; displayScale: 100; suffix: "%" }
        PropertyRow { project: panel.project; clipId: panel.clipId; prop: maskProps.base + "y"; label: qsTr("Centro Y"); from: 0; to: 1; displayScale: 100; suffix: "%" }
        PropertyRow {
            visible: maskProps.mask.shape !== "linear"
            project: panel.project; clipId: panel.clipId; prop: maskProps.base + "width"; label: qsTr("Ancho"); from: 0; to: 2; displayScale: 100; suffix: "%"
        }
        PropertyRow {
            visible: maskProps.mask.shape !== "linear"
            project: panel.project; clipId: panel.clipId; prop: maskProps.base + "height"; label: qsTr("Alto"); from: 0; to: 2; displayScale: 100; suffix: "%"
        }
        PropertyRow { project: panel.project; clipId: panel.clipId; prop: maskProps.base + "rotation"; label: qsTr("Rotación"); from: -180; to: 180; suffix: "°" }
        PropertyRow { project: panel.project; clipId: panel.clipId; prop: maskProps.base + "feather"; label: qsTr("Difuminado"); from: 0; to: 0.5; displayScale: 100; suffix: "%" }
        RowLayout {
            visible: maskProps.mask.shape === "path"
            Layout.fillWidth: true
            Button {
                text: qsTr("Suavizar")
                focusPolicy: Qt.NoFocus
                Layout.fillWidth: true
                onClicked: panel.reshape(true)
            }
            Button {
                text: qsTr("Esquinas")
                focusPolicy: Qt.NoFocus
                Layout.fillWidth: true
                onClicked: panel.reshape(false)
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSmall
            text: maskProps.mask.shape === "path"
                  ? qsTr("En el visor: arrastra los puntos o el centro; doble clic añade un punto y clic derecho en un punto lo quita.")
                  : qsTr("Arrastra la máscara en el visor para moverla; sus valores admiten keyframes.")
        }
    }
}
