import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Subtítulos automáticos: transcribe con Whisper (en local) el audio de la línea de tiempo y crea
// una pista de subtítulos editables (clips de título).
Dialog {
    id: dlg

    required property AiController ai
    // false = solo transcribir (para la edición por texto), sin crear la pista de subtítulos.
    property bool createTrack: true

    title: createTrack ? qsTr("Generar subtítulos") : qsTr("Transcribir la línea de tiempo")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 480

    readonly property var transcriptionModels: ai.models.filter(m => m.task === "transcription")
    readonly property string modelId: transcriptionModels.length > 0 ? transcriptionModels[modelCombo.currentIndex].id : ""
    property double downloadMB: 0

    function refreshDownload() { downloadMB = modelId !== "" ? ai.pendingDownloadMB(modelId) : 0 }

    onAboutToShow: {
        // Por defecto, el modelo instalado más preciso; si no hay ninguno, el recomendado (Base).
        let best = transcriptionModels.findIndex(m => m.id === "whisper-base")
        for (let i = 0; i < transcriptionModels.length; ++i)
            if (transcriptionModels[i].installed)
                best = i
        modelCombo.currentIndex = Math.max(0, best)
        refreshDownload()
    }
    onModelIdChanged: refreshDownload()

    footer: DialogButtonBox {
        Button {
            text: dlg.downloadMB > 0 ? qsTr("Descargar y continuar") : (dlg.createTrack ? qsTr("Generar") : qsTr("Transcribir"))
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button { text: qsTr("Cancelar"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: ai.transcribe(modelId, ai.languages()[languageCombo.currentIndex].code, retranscribe.checked, createTrack,
                              charsBox.value, linesBox.value)

    GridLayout {
        anchors.fill: parent
        columns: 2
        columnSpacing: 14
        rowSpacing: 10

        Label { text: qsTr("Modelo") }
        ComboBox {
            id: modelCombo
            Layout.fillWidth: true
            model: dlg.transcriptionModels
            textRole: "name"
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: ListView.view.width
                highlighted: modelCombo.highlightedIndex === index
                contentItem: RowLayout {
                    Label { text: modelData.name; Layout.fillWidth: true }
                    Label {
                        text: modelData.installed ? qsTr("instalado") : qsTr("%1 MB").arg(Math.round(modelData.sizeMB))
                        color: modelData.installed ? Theme.accentHover : Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
        }
        Item { width: 1 }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            text: dlg.transcriptionModels.length > 0 ? dlg.transcriptionModels[modelCombo.currentIndex].description : ""
        }

        Label { text: qsTr("Idioma") }
        ComboBox {
            id: languageCombo
            Layout.fillWidth: true
            model: dlg.ai.languages()
            textRole: "name"
        }

        Label { text: qsTr("Caracteres por línea"); visible: dlg.createTrack }
        SpinBox { id: charsBox; from: 16; to: 80; value: 42; editable: true; visible: dlg.createTrack }

        Label { text: qsTr("Líneas"); visible: dlg.createTrack }
        SpinBox { id: linesBox; from: 1; to: 3; value: 2; visible: dlg.createTrack }

        Item { width: 1 }
        CheckBox {
            id: retranscribe
            text: qsTr("Volver a transcribir los medios ya transcritos")
        }

        Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: dlg.downloadMB > 0 ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            text: (dlg.downloadMB > 0
                   ? qsTr("Se descargarán %1 MB de modelos (una sola vez). ").arg(Math.round(dlg.downloadMB))
                   : "")
                  + qsTr("Todo se procesa en este equipo: el audio no sale de él. La transcripción se guarda en el proyecto.")
                  + " " + (dlg.createTrack ? qsTr("Los subtítulos se crean en una pista nueva y se editan como títulos.")
                                           : qsTr("Después podrás editar el video desde la pestaña Texto."))
        }
    }
}
