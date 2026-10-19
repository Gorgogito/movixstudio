import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

// Propiedades del clip seleccionado (transformación, fundidos, velocidad, transición, título,
// audio) o, sin selección, del proyecto.
Rectangle {
    id: inspector

    required property ProjectController project
    signal settingsRequested()

    property var info: ({})
    readonly property int selectedCount: project.selection.length
    readonly property bool hasClip: info.clipId !== undefined
    readonly property var title: info.title ?? ({})

    function refresh() {
        info = selectedCount === 1 ? project.clipInfo(project.selection[0]) : ({})
    }
    function setTitle(key, value) {
        const s = {}
        s[key] = value
        project.setTitleStyle(info.clipId, s)
    }

    color: Theme.surface
    Component.onCompleted: refresh()

    Connections {
        target: inspector.project
        function onSelectionChanged() { inspector.refresh() }
        function onHistoryChanged() { inspector.refresh() }
    }

    component SectionTitle: Text {
        Layout.topMargin: 14
        Layout.fillWidth: true
        color: Theme.accentHover
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }
    component SmallLabel: Text {
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall + 1
    }
    // Campo de segundos (fundidos, transiciones).
    component SecondsField: SpinBox {
        id: secs
        property real seconds: 0
        signal edited(real value)
        from: 0
        to: 6000
        stepSize: 5
        editable: true
        value: Math.round(seconds * 100)
        focusPolicy: Qt.ClickFocus
        Layout.preferredWidth: 104
        textFromValue: (v, locale) => Number(v / 100).toLocaleString(locale, "f", 2) + " s"
        valueFromText: (t, locale) => Math.round(Number.fromLocaleString(locale, t.replace(" s", "")) * 100)
        onValueModified: edited(value / 100)
    }

    ColorDialog {
        id: colorDialog
        property string key
        onAccepted: inspector.setTitle(key, selectedColor.toString().length === 7 ? "#ff" + selectedColor.toString().substring(1) : selectedColor.toString())
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 6

            Text {
                text: qsTr("Propiedades")
                color: Theme.text
                font.pixelSize: Theme.fontNormal + 1
                font.bold: true
            }

            // ======== Un clip seleccionado ========
            ColumnLayout {
                Layout.fillWidth: true
                visible: inspector.hasClip
                spacing: 6

                SectionTitle { text: inspector.info.isTitle ? qsTr("Título") : qsTr("Clip") }
                InfoRow { label: qsTr("Medio"); value: inspector.info.name ?? ""; visible: !inspector.info.isTitle }
                InfoRow { label: qsTr("Pista"); value: inspector.info.track ?? "" }
                InfoRow { label: qsTr("Inicio"); value: inspector.project.timecode(inspector.info.start ?? 0) }
                InfoRow { label: qsTr("Duración"); value: inspector.project.timecode(inspector.info.duration ?? 0) }
                RowLayout {
                    Layout.fillWidth: true
                    SmallLabel { text: qsTr("Clip activo"); Layout.fillWidth: true }
                    Switch {
                        focusPolicy: Qt.NoFocus
                        checked: inspector.info.enabled ?? true
                        onToggled: inspector.project.setClipEnabled(inspector.info.clipId, checked)
                    }
                }

                // ---- Texto del título ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: inspector.info.isTitle === true
                    spacing: 6
                    TextArea {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 64
                        wrapMode: TextEdit.Wrap
                        text: inspector.title.text ?? ""
                        placeholderText: qsTr("Escribe el texto del título")
                        onTextChanged: if (activeFocus && text !== inspector.title.text) inspector.setTitle("text", text)
                        onActiveFocusChanged: if (!activeFocus) inspector.project.endInteraction()
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1 // que no imponga el ancho del nombre de fuente más largo
                        model: Qt.fontFamilies()
                        currentIndex: Math.max(0, model.indexOf(inspector.title.font ?? ""))
                        onActivated: inspector.setTitle("font", currentText)
                    }
                    RowLayout {
                        SmallLabel { text: qsTr("Tamaño") }
                        MxSlider {
                            Layout.fillWidth: true
                            from: 0.02
                            to: 0.3
                            value: inspector.title.size ?? 0.09
                            onMoved: inspector.setTitle("size", value)
                            onPressedChanged: if (!pressed) inspector.project.endInteraction()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Button {
                            text: qsTr("Negrita")
                            font.bold: true
                            checkable: true
                            checked: inspector.title.bold ?? false
                            focusPolicy: Qt.NoFocus
                            Layout.fillWidth: true
                            onToggled: inspector.setTitle("bold", checked)
                        }
                        Button {
                            text: qsTr("Cursiva")
                            font.italic: true
                            checkable: true
                            checked: inspector.title.italic ?? false
                            focusPolicy: Qt.NoFocus
                            Layout.fillWidth: true
                            onToggled: inspector.setTitle("italic", checked)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: [qsTr("Izquierda"), qsTr("Centro"), qsTr("Derecha")]
                            Button {
                                required property int index
                                required property string modelData
                                text: modelData
                                checkable: true
                                checked: (inspector.title.align ?? 1) === index
                                focusPolicy: Qt.NoFocus
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                onClicked: inspector.setTitle("align", index)
                            }
                        }
                    }
                    RowLayout {
                        SmallLabel { text: qsTr("Color"); Layout.fillWidth: true }
                        Rectangle {
                            width: 44; height: 22; radius: 4
                            color: inspector.title.color ?? "white"
                            border.color: Theme.border
                            MouseArea { anchors.fill: parent; onClicked: { colorDialog.key = "color"; colorDialog.selectedColor = parent.color; colorDialog.open() } }
                        }
                    }
                    RowLayout {
                        SmallLabel { text: qsTr("Contorno") }
                        MxSlider {
                            Layout.fillWidth: true
                            from: 0
                            to: 0.25
                            value: inspector.title.outline ?? 0
                            onMoved: inspector.setTitle("outline", value)
                            onPressedChanged: if (!pressed) inspector.project.endInteraction()
                        }
                        Rectangle {
                            width: 22; height: 22; radius: 4
                            color: inspector.title.outlineColor ?? "black"
                            border.color: Theme.border
                            MouseArea { anchors.fill: parent; onClicked: { colorDialog.key = "outlineColor"; colorDialog.selectedColor = parent.color; colorDialog.open() } }
                        }
                    }
                    RowLayout {
                        CheckBox { text: qsTr("Sombra"); checked: inspector.title.shadow ?? false; focusPolicy: Qt.NoFocus; onToggled: inspector.setTitle("shadow", checked) }
                        Item { Layout.fillWidth: true }
                        CheckBox {
                            text: qsTr("Fondo")
                            checked: (inspector.title.background ?? "") !== ""
                            focusPolicy: Qt.NoFocus
                            onToggled: inspector.setTitle("background", checked ? "#b0101420" : "")
                        }
                        Rectangle {
                            visible: (inspector.title.background ?? "") !== ""
                            width: 22; height: 22; radius: 4
                            color: inspector.title.background || "transparent"
                            border.color: Theme.border
                            MouseArea { anchors.fill: parent; onClicked: { colorDialog.key = "background"; colorDialog.selectedColor = parent.color; colorDialog.open() } }
                        }
                    }
                }

                // ---- Transformación (pistas de video) ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: inspector.info.isVideoTrack === true
                    spacing: 8
                    SectionTitle { text: qsTr("Transformación") }
                    PropertyRow { project: inspector.project; clipId: inspector.info.clipId; prop: "x"; label: qsTr("Posición X"); from: -1; to: 1; displayScale: 100; suffix: "%" }
                    PropertyRow { project: inspector.project; clipId: inspector.info.clipId; prop: "y"; label: qsTr("Posición Y"); from: -1; to: 1; displayScale: 100; suffix: "%" }
                    PropertyRow { project: inspector.project; clipId: inspector.info.clipId; prop: "scale"; label: qsTr("Escala"); from: 0.05; to: 4; displayScale: 100; suffix: "%" }
                    PropertyRow { project: inspector.project; clipId: inspector.info.clipId; prop: "rotation"; label: qsTr("Rotación"); from: -180; to: 180; suffix: "°" }
                    PropertyRow { project: inspector.project; clipId: inspector.info.clipId; prop: "opacity"; label: qsTr("Opacidad"); from: 0; to: 1; displayScale: 100; suffix: "%" }

                    SectionTitle { text: qsTr("Fundidos") }
                    RowLayout {
                        SmallLabel { text: qsTr("Entrada"); Layout.fillWidth: true }
                        SecondsField {
                            seconds: inspector.info.fadeIn ?? 0
                            onEdited: (v) => inspector.project.setFades(inspector.info.clipId, v, inspector.info.fadeOut, false)
                        }
                    }
                    RowLayout {
                        SmallLabel { text: qsTr("Salida"); Layout.fillWidth: true }
                        SecondsField {
                            seconds: inspector.info.fadeOut ?? 0
                            onEdited: (v) => inspector.project.setFades(inspector.info.clipId, inspector.info.fadeIn, v, false)
                        }
                    }
                }

                // ---- Velocidad ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !inspector.info.isTitle && !inspector.info.isStill
                    SectionTitle { text: qsTr("Velocidad") }
                    RowLayout {
                        SmallLabel { text: qsTr("Velocidad"); Layout.fillWidth: true }
                        ComboBox {
                            id: speedCombo
                            readonly property var speeds: [0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4, 8]
                            model: speeds.map(s => (s + "×").replace(".", ","))
                            currentIndex: Math.max(0, speeds.indexOf(inspector.info.speed ?? 1))
                            displayText: (inspector.info.speed ?? 1).toLocaleString(Qt.locale(), "f", 2).replace(/,?0+$/, "") + "×"
                            onActivated: (i) => inspector.project.setClipSpeed(inspector.info.clipId, speeds[i])
                        }
                    }
                    SmallLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.textDisabled
                        text: qsTr("El audio conserva el tono a cualquier velocidad.")
                    }
                }

                // ---- Transición de entrada ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: inspector.info.hasCut === true
                    SectionTitle { text: qsTr("Transición de entrada") }
                    ComboBox {
                        id: transitionCombo
                        Layout.fillWidth: true
                        readonly property var types: ["none", "dissolve", "dipToBlack", "wipeLeft", "wipeRight", "pushLeft", "pushRight", "slideLeft"]
                        model: [qsTr("Ninguna"), qsTr("Disolver"), qsTr("Fundido a negro"), qsTr("Barrido a la izquierda"),
                                qsTr("Barrido a la derecha"), qsTr("Empujar a la izquierda"), qsTr("Empujar a la derecha"), qsTr("Deslizar")]
                        currentIndex: Math.max(0, types.indexOf(inspector.info.transition ?? "none"))
                        onActivated: (i) => inspector.project.setTransition(inspector.info.clipId, types[i], inspector.info.transitionSeconds > 0 ? inspector.info.transitionSeconds : 1.0)
                    }
                    RowLayout {
                        visible: (inspector.info.transition ?? "none") !== "none"
                        SmallLabel { text: qsTr("Duración"); Layout.fillWidth: true }
                        SecondsField {
                            seconds: inspector.info.transitionSeconds ?? 0
                            onEdited: (v) => inspector.project.setTransition(inspector.info.clipId, inspector.info.transition, v)
                        }
                    }
                }

                // ---- Audio ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: inspector.info.hasAudio === true
                    spacing: 8
                    SectionTitle { text: qsTr("Audio") }
                    PropertyRow { project: inspector.project; clipId: inspector.info.clipId; prop: "volume"; label: qsTr("Volumen"); from: 0; to: 2; displayScale: 100; suffix: "%" }
                    RowLayout {
                        SmallLabel { text: qsTr("Fundido de entrada"); Layout.fillWidth: true }
                        SecondsField {
                            seconds: inspector.info.audioFadeIn ?? 0
                            onEdited: (v) => inspector.project.setFades(inspector.info.clipId, v, inspector.info.audioFadeOut, true)
                        }
                    }
                    RowLayout {
                        SmallLabel { text: qsTr("Fundido de salida"); Layout.fillWidth: true }
                        SecondsField {
                            seconds: inspector.info.audioFadeOut ?? 0
                            onEdited: (v) => inspector.project.setFades(inspector.info.clipId, inspector.info.audioFadeIn, v, true)
                        }
                    }
                    RowLayout {
                        SmallLabel { text: qsTr("Volumen de la pista %1").arg(inspector.info.track ?? ""); Layout.fillWidth: true }
                        Text { text: Math.round((inspector.info.trackVolume ?? 1) * 100) + " %"; color: Theme.text; font.pixelSize: Theme.fontSmall + 1 }
                    }
                    MxSlider {
                        Layout.fillWidth: true
                        from: 0
                        to: 2
                        value: inspector.info.trackVolume ?? 1
                        onMoved: inspector.project.setTrackVolume(inspector.info.trackId, value)
                        onPressedChanged: if (!pressed) inspector.project.endInteraction()
                    }
                }
                Button {
                    visible: inspector.info.canDetachAudio === true
                    text: qsTr("Separar el audio a una pista")
                    focusPolicy: Qt.NoFocus
                    onClicked: inspector.project.detachAudio(inspector.info.clipId)
                }
            }

            // ======== Varios clips ========
            Text {
                Layout.topMargin: 10
                visible: inspector.selectedCount > 1
                text: qsTr("%n clips seleccionados", "", inspector.selectedCount)
                color: Theme.textMuted
            }

            // ======== Sin selección: proyecto ========
            ColumnLayout {
                Layout.fillWidth: true
                visible: inspector.selectedCount === 0
                spacing: 6
                SectionTitle { text: qsTr("Proyecto") }
                InfoRow { label: qsTr("Nombre"); value: inspector.project.projectName }
                InfoRow { label: qsTr("Resolución"); value: inspector.project.canvasWidth + " × " + inspector.project.canvasHeight }
                InfoRow { label: qsTr("Fotogramas/s"); value: inspector.project.frameRateText }
                InfoRow { label: qsTr("Duración"); value: inspector.project.timecode(inspector.project.durationFrames) }
                Button {
                    Layout.topMargin: 8
                    text: qsTr("Ajustes del proyecto…")
                    icon.source: "qrc:/qt/qml/MovixStudio/icons/settings.svg"
                    focusPolicy: Qt.NoFocus
                    onClicked: inspector.settingsRequested()
                }
                Text {
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    wrapMode: Text.WordWrap
                    color: Theme.textDisabled
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("Selecciona un clip de la línea de tiempo para ver y cambiar sus propiedades.")
                }
            }
        }
    }
}
