import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Resolución y velocidad del proyecto. Cambiar los fps conserva los instantes de los clips.
Dialog {
    id: dlg

    required property ProjectController project

    title: qsTr("Ajustes del proyecto")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 460
    standardButtons: Dialog.Ok | Dialog.Cancel

    readonly property var presets: [
        { label: "1920 × 1080 (16:9)", w: 1920, h: 1080 },
        { label: "3840 × 2160 (4K 16:9)", w: 3840, h: 2160 },
        { label: "1280 × 720 (16:9)", w: 1280, h: 720 },
        { label: qsTr("1080 × 1920 (vertical 9:16)"), w: 1080, h: 1920 },
        { label: qsTr("1080 × 1080 (cuadrado)"), w: 1080, h: 1080 },
        { label: qsTr("1080 × 1350 (retrato 4:5)"), w: 1080, h: 1350 }
    ]
    readonly property var rates: [
        { label: "23,976", num: 24000, den: 1001 }, { label: "24", num: 24, den: 1 }, { label: "25", num: 25, den: 1 },
        { label: "29,97", num: 30000, den: 1001 }, { label: "30", num: 30, den: 1 }, { label: "50", num: 50, den: 1 },
        { label: "59,94", num: 60000, den: 1001 }, { label: "60", num: 60, den: 1 }
    ]

    onAboutToShow: {
        widthBox.value = project.canvasWidth
        heightBox.value = project.canvasHeight
        let idx = rates.findIndex(r => r.num === project.frameRateNum && r.den === project.frameRateDen)
        rateCombo.currentIndex = idx >= 0 ? idx : 4
    }
    onAccepted: {
        const r = rates[rateCombo.currentIndex]
        project.setProjectSettings(widthBox.value, heightBox.value, r.num, r.den)
    }

    GridLayout {
        anchors.fill: parent
        columns: 2
        columnSpacing: 14
        rowSpacing: 10

        Label { text: qsTr("Plantilla") }
        ComboBox {
            Layout.fillWidth: true
            model: dlg.presets
            textRole: "label"
            displayText: qsTr("Elegir…")
            onActivated: (i) => {
                widthBox.value = dlg.presets[i].w
                heightBox.value = dlg.presets[i].h
            }
        }

        Label { text: qsTr("Ancho") }
        SpinBox { id: widthBox; from: 16; to: 8192; stepSize: 2; editable: true }

        Label { text: qsTr("Alto") }
        SpinBox { id: heightBox; from: 16; to: 8192; stepSize: 2; editable: true }

        Label { text: qsTr("Fotogramas/s") }
        ComboBox { id: rateCombo; Layout.fillWidth: true; model: dlg.rates; textRole: "label" }

        Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            text: qsTr("Al añadir el primer clip a una línea de tiempo vacía, el proyecto adopta su resolución y velocidad automáticamente.")
        }
    }
}
