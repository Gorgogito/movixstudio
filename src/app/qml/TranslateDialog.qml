import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Traducción de subtítulos con Opus-MT (en local): crea una pista traducida con los mismos tiempos.
Dialog {
    id: dlg

    required property AiController ai

    title: qsTr("Traducir subtítulos")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 440
    readonly property var pairs: [
        { from: "es", to: "en", name: qsTr("Español → inglés"), model: "opus-mt-es-en" },
        { from: "en", to: "es", name: qsTr("Inglés → español"), model: "opus-mt-en-es" }
    ]
    property double downloadMB: 0
    function refresh() { downloadMB = ai.pendingDownloadMB(pairs[pairCombo.currentIndex].model) }

    onAboutToShow: refresh()

    footer: DialogButtonBox {
        Button {
            text: dlg.downloadMB > 0 ? qsTr("Descargar y traducir") : qsTr("Traducir")
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button { text: qsTr("Cancelar"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: ai.translateSubtitles(pairs[pairCombo.currentIndex].from, pairs[pairCombo.currentIndex].to)

    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        RowLayout {
            Label { text: qsTr("Idiomas") }
            ComboBox {
                id: pairCombo
                Layout.fillWidth: true
                model: dlg.pairs
                textRole: "name"
                onActivated: dlg.refresh()
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            text: qsTr("Se traduce la pista del subtítulo seleccionado (o la de subtítulos superior) a una pista nueva con los mismos tiempos y estilo; la original se oculta.")
                  + (dlg.downloadMB > 0 ? " " + qsTr("Se descargará el modelo de traducción (%1 MB).").arg(Math.round(dlg.downloadMB)) : "")
        }
    }
}
