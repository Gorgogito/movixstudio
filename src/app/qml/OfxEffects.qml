import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Efectos OpenFX del clip: añadir un plugin instalado, activarlo, reordenarlo, quitarlo y ajustar
// sus parámetros (los controles se generan a partir de lo que declara el plugin). Los valores son
// los del cabezal; los parámetros animables admiten keyframes. Con `kind` "generator" o
// "transition" muestra solo los parámetros del generador del clip o de su transición de entrada.
ColumnLayout {
    id: section

    required property ProjectController project
    required property var info
    property string kind: "effects"
    readonly property bool list: kind === "effects"
    readonly property var clipId: info.clipId
    readonly property var plugins: project.ofxPlugins
    property var effects: []

    // Los repetidores dependen solo del número de efectos y de parámetros: al cambiar un valor no se
    // rehacen los controles (eso cortaría el arrastre de un deslizador).
    function refresh() {
        // info.clipId y no clipId: al llegar onInfoChanged, el enlace de clipId puede no haberse
        // recalculado todavía.
        effects = info.clipId !== undefined ? project.ofxEffects(info.clipId, kind) : []
    }
    onInfoChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: section.project
        function onHistoryChanged() { section.refresh() }
    }
    Connections {
        target: section.project.player
        function onPositionChanged() { if (section.effects.length > 0) section.refresh() }
    }

    spacing: 6
    Layout.fillWidth: true

    function componentLabel(type, i) {
        if (type === "rgb" || type === "rgba")
            return ["R", "G", "B", "A"][i]
        return ["X", "Y", "Z"][i]
    }

    Text {
        Layout.topMargin: 10
        visible: section.list || section.kind === "generator"
        text: section.list ? qsTr("Efectos OpenFX") : qsTr("Generador OpenFX")
        color: Theme.accentHover
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }
    Text {
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        visible: section.list && section.plugins.length === 0
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        text: qsTr("No hay plugins OpenFX instalados. Instálalos en la carpeta de plugins de MovixStudio o en la carpeta común de OpenFX del sistema y pulsa «Buscar de nuevo».")
    }
    ComboBox {
        Layout.fillWidth: true
        visible: section.list && section.plugins.length > 0
        focusPolicy: Qt.NoFocus
        displayText: qsTr("Añadir efecto…")
        model: section.plugins.map(p => (p.group ? p.group + " › " : "") + p.label)
        onActivated: (i) => {
            section.project.addOfxEffect(section.clipId, section.plugins[i].id)
            currentIndex = -1
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: section.list
        Button {
            text: qsTr("Carpeta de plugins")
            focusPolicy: Qt.NoFocus
            onClicked: section.project.openOfxFolder()
        }
        Button {
            text: qsTr("Buscar de nuevo")
            focusPolicy: Qt.NoFocus
            onClicked: section.project.rescanOfx()
        }
    }

    Repeater {
        model: section.effects.length
        delegate: Rectangle {
            id: card
            required property int index
            readonly property var fx: section.effects[index] ?? ({ params: [] })
            readonly property int fxIndex: fx.index ?? index
            Layout.fillWidth: true
            implicitHeight: body.implicitHeight + 16
            radius: Theme.radius
            color: Theme.surfaceRaised
            opacity: fx.enabled === false ? 0.6 : 1.0

            ColumnLayout {
                id: body
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Switch {
                        visible: section.list
                        focusPolicy: Qt.NoFocus
                        checked: card.fx.enabled === true
                        onToggled: section.project.setOfxEnabled(section.clipId, card.fxIndex, checked)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: (card.fx.label ?? "") + (card.fx.installed === false ? qsTr(" (no instalado)") : "")
                        color: card.fx.installed === false ? Theme.danger : Theme.text
                        font.pixelSize: Theme.fontSmall + 1
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    ToolButton {
                        visible: section.list
                        text: "↑"
                        focusPolicy: Qt.NoFocus
                        enabled: card.fxIndex > 0
                        onClicked: section.project.moveOfxEffect(section.clipId, card.fxIndex, card.fxIndex - 1)
                    }
                    ToolButton {
                        visible: section.list
                        text: "↓"
                        focusPolicy: Qt.NoFocus
                        enabled: card.fxIndex < section.effects.length - 1
                        onClicked: section.project.moveOfxEffect(section.clipId, card.fxIndex, card.fxIndex + 1)
                    }
                    ToolButton {
                        visible: section.list
                        text: "✕"
                        focusPolicy: Qt.NoFocus
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Quitar el efecto")
                        onClicked: section.project.removeOfxEffect(section.clipId, card.fxIndex)
                    }
                }

                Repeater {
                    model: card.fx.params.length
                    delegate: ColumnLayout {
                        id: param
                        required property int index
                        readonly property var p: card.fx.params[index] ?? ({ value: [], defaults: [], options: [] })
                        readonly property bool numeric: ["double", "int", "rgb", "rgba", "double2d", "int2d", "double3d", "int3d"].indexOf(p.type) >= 0
                        Layout.fillWidth: true
                        spacing: 2
                        ToolTip.visible: hint.hovered && (p.hint ?? "") !== ""
                        ToolTip.delay: 600
                        ToolTip.text: p.hint ?? ""
                        HoverHandler { id: hint }

                        function setComponent(i, v) {
                            const value = param.p.value.slice()
                            value[i] = v
                            section.project.setOfxParam(section.clipId, card.fxIndex, param.p.name, value)
                        }

                        // Nombre y keyframes en el cabezal (anterior, alternar, siguiente).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: param.p.label ?? ""
                                color: param.p.animated ? Theme.text : Theme.textMuted
                                font.pixelSize: Theme.fontSmall + 1
                                elide: Text.ElideRight
                            }
                            IconButton {
                                visible: param.p.animates === true
                                size: 22
                                text: "‹"
                                display: AbstractButton.TextOnly
                                enabled: param.p.hasPrev === true
                                tip: qsTr("Keyframe anterior")
                                onClicked: section.project.jumpToKeyframe(section.clipId, false)
                            }
                            KeyframeButton {
                                visible: param.p.animates === true
                                keyHere: param.p.keyHere === true
                                animated: param.p.animated === true
                                interp: param.p.interp || "linear"
                                onToggled: section.project.toggleOfxKeyframe(section.clipId, card.fxIndex, param.p.name)
                                onInterpolationChosen: (mode) => section.project.setOfxKeyInterpolation(section.clipId, card.fxIndex, param.p.name, mode)
                            }
                            IconButton {
                                visible: param.p.animates === true
                                size: 22
                                text: "›"
                                display: AbstractButton.TextOnly
                                enabled: param.p.hasNext === true
                                tip: qsTr("Keyframe siguiente")
                                onClicked: section.project.jumpToKeyframe(section.clipId, true)
                            }
                        }

                        // Números (y cada componente de los colores y vectores).
                        Repeater {
                            model: param.numeric ? param.p.value.length : 0
                            delegate: SliderRow {
                                required property int index
                                label: param.p.value.length > 1 ? section.componentLabel(param.p.type, index) : ""
                                from: param.p.type.startsWith("rgb") ? 0 : param.p.min
                                to: param.p.type.startsWith("rgb") ? 1 : param.p.max
                                value: param.p.value[index] ?? 0
                                defaultValue: param.p.defaults[index] ?? 0
                                displayScale: 1
                                decimals: param.p.type.startsWith("int") ? 0 : 2
                                onMoved: (v) => param.setComponent(index, param.p.type.startsWith("int") ? Math.round(v) : v)
                                onReleased: section.project.endInteraction()
                            }
                        }
                        Switch {
                            visible: param.p.type === "bool"
                            focusPolicy: Qt.NoFocus
                            checked: param.p.value[0] !== 0
                            onToggled: param.setComponent(0, checked ? 1 : 0)
                        }
                        ComboBox {
                            visible: param.p.type === "choice"
                            Layout.fillWidth: true
                            focusPolicy: Qt.NoFocus
                            model: param.p.options
                            currentIndex: param.p.value[0] ?? 0
                            onActivated: (i) => param.setComponent(0, i)
                        }
                    }
                }
            }
        }
    }
}
