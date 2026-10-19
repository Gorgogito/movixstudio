import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Fila de una propiedad animable: deslizador, valor, keyframe en el cabezal y navegación.
// El valor se muestra multiplicado por `displayScale` (p. ej. 100 para porcentajes).
ColumnLayout {
    id: row

    required property ProjectController project
    required property var clipId
    required property string prop
    property string label
    property real from: 0
    property real to: 1
    property real displayScale: 1
    property int decimals: 0
    property string suffix: ""
    property bool animatable: true

    property var state_: ({})
    function refresh() {
        state_ = clipId !== undefined ? project.propertyState(clipId, prop) : ({})
    }

    spacing: 2
    Layout.fillWidth: true
    Component.onCompleted: refresh()
    onClipIdChanged: refresh()

    Connections {
        target: row.project
        function onHistoryChanged() { row.refresh() }
    }
    Connections {
        target: row.project.player
        function onPositionChanged() { row.refresh() }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: row.label
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
            Layout.fillWidth: true
        }
        TextField {
            id: valueField
            Layout.preferredWidth: 64
            horizontalAlignment: Text.AlignRight
            font.pixelSize: Theme.fontSmall + 1
            text: row.state_.value !== undefined ? (row.state_.value * row.displayScale).toFixed(row.decimals) : ""
            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
            onEditingFinished: {
                const v = Number(text.replace(",", "."))
                if (!isNaN(v)) {
                    row.project.setClipProperty(row.clipId, row.prop, v / row.displayScale)
                    row.project.endInteraction()
                }
            }
        }
        Text {
            text: row.suffix
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            Layout.preferredWidth: 16
        }
        // Keyframes: anterior, alternar en el cabezal, siguiente.
        IconButton {
            visible: row.animatable
            size: 22
            text: "‹"
            display: AbstractButton.TextOnly
            enabled: row.state_.hasPrev === true
            tip: qsTr("Keyframe anterior")
            onClicked: row.project.jumpToKeyframe(row.clipId, false)
        }
        AbstractButton {
            id: keyButton
            visible: row.animatable
            implicitWidth: 22
            implicitHeight: 22
            focusPolicy: Qt.NoFocus
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: row.state_.keyHere ? qsTr("Quitar keyframe en el cabezal") : qsTr("Añadir keyframe en el cabezal")
            onClicked: row.project.toggleKeyframe(row.clipId, row.prop)
            contentItem: Item {
                Rectangle {
                    anchors.centerIn: parent
                    width: 9
                    height: 9
                    rotation: 45
                    color: row.state_.keyHere ? Theme.clipSelected : "transparent"
                    border.width: 1.5
                    border.color: row.state_.animated ? Theme.clipSelected : (keyButton.hovered ? Theme.text : Theme.textMuted)
                }
            }
        }
        IconButton {
            visible: row.animatable
            size: 22
            text: "›"
            display: AbstractButton.TextOnly
            enabled: row.state_.hasNext === true
            tip: qsTr("Keyframe siguiente")
            onClicked: row.project.jumpToKeyframe(row.clipId, true)
        }
        IconButton {
            size: 22
            text: "↺"
            display: AbstractButton.TextOnly
            tip: qsTr("Restablecer")
            onClicked: row.project.resetClipProperty(row.clipId, row.prop)
        }
    }
    MxSlider {
        Layout.fillWidth: true
        from: row.from
        to: row.to
        value: row.state_.value !== undefined ? row.state_.value : row.from
        onMoved: row.project.setClipProperty(row.clipId, row.prop, value)
        onPressedChanged: if (!pressed) row.project.endInteraction()
    }
}
