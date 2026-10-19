import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Biblioteca con pestañas: medios del proyecto, plantillas de título y transiciones.
Rectangle {
    id: library

    required property ProjectController project
    signal importRequested()
    signal previewRequested(var mediaId)

    color: Theme.surface

    component Tab: AbstractButton {
        id: tab
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 34
        Layout.fillWidth: true
        contentItem: Text {
            text: tab.text
            color: tab.active ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
            font.bold: tab.active
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: tab.hovered ? Theme.surfaceRaised : "transparent"
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: tab.active ? Theme.accent : "transparent"
            }
        }
    }

    // Tarjeta arrastrable de títulos y transiciones.
    component LibraryCard: Rectangle {
        id: card
        property string label
        property string mimeKey
        property string mimeValue
        property string glyph
        signal activated()
        implicitHeight: 74
        radius: Theme.radius
        color: hover.hovered ? Theme.surfaceHover : Theme.surfaceRaised
        Drag.active: dragArea.drag.active
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: {
            const m = {}
            m[card.mimeKey] = card.mimeValue
            return m
        }
        Column {
            anchors.centerIn: parent
            spacing: 6
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: card.glyph
                color: Theme.accentHover
                font.pixelSize: 22
                font.bold: true
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: card.label
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
            }
        }
        HoverHandler { id: hover }
        MouseArea {
            id: dragArea
            anchors.fill: parent
            drag.target: proxy
            onDoubleClicked: card.activated()
        }
        Item { id: proxy }
    }

    property int current: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            Tab { text: qsTr("Medios"); active: library.current === 0; onClicked: library.current = 0 }
            Tab { text: qsTr("Títulos"); active: library.current === 1; onClicked: library.current = 1 }
            Tab { text: qsTr("Transiciones"); active: library.current === 2; onClicked: library.current = 2 }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: library.current

            MediaBin {
                project: library.project
                onImportRequested: library.importRequested()
                onPreviewRequested: (id) => library.previewRequested(id)
            }

            ColumnLayout {
                spacing: 8
                Text {
                    Layout.margins: 10
                    Layout.bottomMargin: 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Arrastra un título a la línea de tiempo o haz doble clic para añadirlo en el cabezal.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall + 1
                }
                GridLayout {
                    Layout.margins: 10
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 8
                    Repeater {
                        model: library.project.titlePresets()
                        LibraryCard {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            label: modelData.name
                            glyph: "T"
                            mimeKey: "application/x-movix-title"
                            mimeValue: String(index)
                            onActivated: library.project.addTitle(index)
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }

            ColumnLayout {
                spacing: 8
                Text {
                    Layout.margins: 10
                    Layout.bottomMargin: 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Arrastra una transición al corte entre dos clips contiguos de la misma pista.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall + 1
                }
                GridLayout {
                    Layout.margins: 10
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 8
                    Repeater {
                        model: [
                            { type: "dissolve", name: qsTr("Disolver"), glyph: "◐" },
                            { type: "dipToBlack", name: qsTr("Fundido a negro"), glyph: "●" },
                            { type: "wipeLeft", name: qsTr("Barrido ←"), glyph: "▤" },
                            { type: "wipeRight", name: qsTr("Barrido →"), glyph: "▤" },
                            { type: "pushLeft", name: qsTr("Empujar ←"), glyph: "⇇" },
                            { type: "pushRight", name: qsTr("Empujar →"), glyph: "⇉" },
                            { type: "slideLeft", name: qsTr("Deslizar"), glyph: "⇐" }
                        ]
                        LibraryCard {
                            required property var modelData
                            Layout.fillWidth: true
                            label: modelData.name
                            glyph: modelData.glyph
                            mimeKey: "application/x-movix-transition"
                            mimeValue: modelData.type
                            // Doble clic: en el corte de entrada del clip seleccionado, si lo tiene.
                            onActivated: {
                                if (library.project.selection.length === 1)
                                    library.project.setTransition(library.project.selection[0], modelData.type, 1.0)
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
