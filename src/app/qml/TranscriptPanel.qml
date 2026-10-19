import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Edición por texto: las palabras que se oyen en la línea de tiempo. Clic = ir a la palabra;
// Mayús+clic = ampliar la selección; Supr = cortar del video lo que ocupan (en todas las pistas).
Item {
    id: panel

    required property AiController ai
    required property ProjectController project
    signal transcribeRequested()
    signal seekRequested()

    property int anchorIndex: -1
    property int focusIndex: -1
    readonly property int selFirst: Math.min(anchorIndex, focusIndex)
    readonly property int selLast: Math.max(anchorIndex, focusIndex)
    readonly property bool hasSelection: anchorIndex >= 0 && focusIndex >= 0
    readonly property int playing: ai.words.length > 0 ? ai.wordAt(project.player.positionFrame) : -1

    function clearSelection() { anchorIndex = -1; focusIndex = -1 }
    function deleteSelection() {
        if (!hasSelection)
            return
        ai.deleteWords(selFirst, selLast)
        clearSelection()
    }

    // Las palabras cambian con cualquier edición: la selección por índice deja de valer.
    Connections {
        target: panel.ai
        function onWordsChanged() { panel.clearSelection() }
    }
    // Seleccionar clips en la línea de tiempo devuelve el control (y la tecla Supr) a los clips.
    Connections {
        target: panel.project
        function onSelectionChanged() {
            if (panel.project.selection.length > 0)
                panel.clearSelection()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
            text: panel.ai.words.length > 0
                  ? qsTr("Haz clic en una palabra para ir a ella; Mayús+clic amplía la selección. Supr corta del video lo que ocupa.")
                  : qsTr("Transcribe el audio de la línea de tiempo para editar el video como un texto: borrar palabras corta el video.")
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.ai.words.length === 0 || panel.ai.untranscribed
            Button {
                text: panel.ai.words.length === 0 ? qsTr("Transcribir…") : qsTr("Transcribir lo que falta…")
                enabled: !panel.ai.busy && panel.project.durationFrames > 0
                onClicked: panel.transcribeRequested()
            }
            Item { Layout.fillWidth: true }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: flow.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Flow {
                id: flow
                width: flick.width - 10
                spacing: 3
                Repeater {
                    model: panel.ai.words
                    delegate: Rectangle {
                        id: word
                        required property var modelData
                        required property int index
                        readonly property bool selected: panel.hasSelection && index >= panel.selFirst && index <= panel.selLast
                        readonly property bool current: index === panel.playing
                        implicitWidth: label.implicitWidth + 6
                        implicitHeight: label.implicitHeight + 4
                        radius: 3
                        color: selected ? Theme.accent : (current ? Theme.surfaceHover : (hover.hovered ? Theme.surfaceRaised : "transparent"))
                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: word.modelData.text
                            color: word.selected ? "#FFFFFF" : (word.current ? Theme.clipSelected : Theme.text)
                            font.pixelSize: Theme.fontNormal
                        }
                        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                if ((mouse.modifiers & Qt.ShiftModifier) && panel.anchorIndex >= 0) {
                                    panel.focusIndex = word.index
                                } else {
                                    panel.project.clearSelection()
                                    panel.anchorIndex = word.index
                                    panel.focusIndex = word.index
                                }
                                panel.project.player.seek(word.modelData.start)
                                panel.seekRequested()
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.ai.words.length > 0
            Text {
                Layout.fillWidth: true
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                text: panel.hasSelection ? qsTr("%n palabras seleccionadas", "", panel.selLast - panel.selFirst + 1)
                                         : qsTr("%n palabras", "", panel.ai.words.length)
            }
            Button {
                text: qsTr("Cortar selección")
                enabled: panel.hasSelection
                onClicked: panel.deleteSelection()
            }
        }
    }
}
