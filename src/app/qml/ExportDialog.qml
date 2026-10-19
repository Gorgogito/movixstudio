import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

// Exportación: formato, resolución, fotogramas/s, calidad y audio; después, progreso y resultado.
Dialog {
    id: dlg

    required property ProjectController project
    readonly property var exporter: project.exporter

    property var codecs: []
    property url outputUrl
    property string state_: "settings" // settings | running | done
    property bool succeeded: false
    property string resultMessage: ""

    title: qsTr("Exportar video")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(600, parent ? parent.width - 40 : 600)
    closePolicy: state_ === "running" ? Popup.NoAutoClose : Popup.CloseOnEscape

    readonly property var resolutions: [
        { label: qsTr("Como el proyecto (%1 × %2)").arg(project.canvasWidth).arg(project.canvasHeight), shortSide: 0 },
        { label: "2160p (4K UHD)", shortSide: 2160 },
        { label: "1440p (QHD)", shortSide: 1440 },
        { label: "1080p (Full HD)", shortSide: 1080 },
        { label: "720p (HD)", shortSide: 720 },
        { label: "480p (SD)", shortSide: 480 }
    ]
    readonly property var rates: [
        { label: qsTr("Como el proyecto (%1)").arg(project.frameRateText), num: project.frameRateNum, den: project.frameRateDen },
        { label: "23,976", num: 24000, den: 1001 }, { label: "24", num: 24, den: 1 }, { label: "25", num: 25, den: 1 },
        { label: "29,97", num: 30000, den: 1001 }, { label: "30", num: 30, den: 1 }, { label: "50", num: 50, den: 1 },
        { label: "59,94", num: 60000, den: 1001 }, { label: "60", num: 60, den: 1 }
    ]

    function currentCodec() { return codecs[codecCombo.currentIndex] }
    function outputSize() {
        const w = project.canvasWidth, h = project.canvasHeight
        const s = resolutions[resolutionCombo.currentIndex].shortSide
        if (!s)
            return [w & ~1, h & ~1]
        const f = s / Math.min(w, h)
        return [Math.round(w * f / 2) * 2, Math.round(h * f / 2) * 2]
    }
    function updateExtension() {
        const codec = currentCodec()
        if (!codec)
            return
        let path = outputUrl.toString()
        if (path === "")
            outputUrl = exporter.suggestedFile(project.projectName, codec.extension)
        else
            outputUrl = path.replace(/\.[^./\\]+$/, "." + codec.extension)
    }
    function startExport() {
        const size = outputSize()
        const rate = rates[rateCombo.currentIndex]
        exporter.start({
            path: outputUrl,
            width: size[0],
            height: size[1],
            fpsNum: rate.num,
            fpsDen: rate.den,
            codec: currentCodec().name,
            quality: qualityHigh.checked ? "alta" : qualityMedium.checked ? "media" : "baja",
            includeAudio: audioCheck.checked,
            tenBit: tenBitCheck.checked || dlg.project.colorMode !== "sdr",
            audioKbps: [128, 192, 256, 320][audioRate.currentIndex]
        })
        state_ = "running"
    }

    onAboutToShow: {
        codecs = exporter.videoCodecs()
        if (codecCombo.currentIndex < 0 && codecs.length > 0)
            codecCombo.currentIndex = 0
        outputUrl = ""
        updateExtension()
        state_ = "settings"
    }

    Connections {
        target: dlg.exporter
        function onFinished(ok, message, file) {
            dlg.succeeded = ok
            dlg.resultMessage = ok ? qsTr("Se exportó «%1».").arg(Format.fileName(file)) : message
            dlg.state_ = "done"
        }
    }

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        title: qsTr("Guardar el video como")
        currentFile: dlg.outputUrl
        nameFilters: dlg.currentCodec() ? [qsTr("Video") + " (*." + dlg.currentCodec().extension + ")"] : []
        onAccepted: {
            dlg.outputUrl = selectedFile
            dlg.updateExtension()
        }
    }

    // Solo el bloque del estado actual es visible (la altura del diálogo se adapta a él).
    contentItem: ColumnLayout {
        // --- Ajustes ---
        GridLayout {
            visible: dlg.state_ === "settings"
            columns: 2
            columnSpacing: 14
            rowSpacing: 10

            Label { text: qsTr("Archivo") }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    Layout.fillWidth: true
                    readOnly: true
                    text: decodeURIComponent(dlg.outputUrl.toString().replace(/^file:\/\/\//, ""))
                }
                Button { text: qsTr("Cambiar…"); onClicked: saveDialog.open() }
            }

            Label { text: qsTr("Formato") }
            ComboBox {
                id: codecCombo
                Layout.fillWidth: true
                model: dlg.codecs
                textRole: "label"
                onActivated: dlg.updateExtension()
            }

            Label { text: qsTr("Resolución") }
            RowLayout {
                ComboBox {
                    id: resolutionCombo
                    Layout.fillWidth: true
                    model: dlg.resolutions
                    textRole: "label"
                }
                Label {
                    text: dlg.outputSize()[0] + " × " + dlg.outputSize()[1]
                    color: Theme.textMuted
                }
            }

            Label { text: qsTr("Fotogramas/s") }
            ComboBox {
                id: rateCombo
                Layout.fillWidth: true
                model: dlg.rates
                textRole: "label"
            }

            Label { text: qsTr("Calidad") }
            RowLayout {
                enabled: dlg.currentCodec() ? dlg.currentCodec().name !== "prores_ks" : true
                RadioButton { id: qualityHigh; text: qsTr("Alta"); checked: true }
                RadioButton { id: qualityMedium; text: qsTr("Media") }
                RadioButton { id: qualityLow; text: qsTr("Baja (archivo más pequeño)") }
            }

            Label { text: qsTr("Profundidad") }
            CheckBox {
                id: tenBitCheck
                text: dlg.project.colorMode !== "sdr" ? qsTr("10 bits (obligatorio en HDR)") : qsTr("10 bits (menos bandas en degradados)")
                checked: dlg.project.colorMode !== "sdr"
                enabled: dlg.project.colorMode === "sdr"
            }

            Label { text: qsTr("Audio") }
            RowLayout {
                CheckBox { id: audioCheck; text: qsTr("Incluir audio"); checked: true }
                ComboBox {
                    id: audioRate
                    enabled: audioCheck.checked && !!dlg.currentCodec() && dlg.currentCodec().audioCodec !== "pcm_s16le"
                    model: ["128 kb/s", "192 kb/s", "256 kb/s", "320 kb/s"]
                    currentIndex: 1
                }
            }

            Label {
                Layout.columnSpan: 2
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                text: dlg.project.durationFrames > 0
                      ? qsTr("Duración: %1. Los códecs marcados como GPU usan el codificador por hardware de tu tarjeta gráfica.").arg(dlg.project.timecode(dlg.project.durationFrames))
                      : qsTr("La línea de tiempo está vacía: añade clips antes de exportar.")
            }
        }

        // --- En curso ---
        ColumnLayout {
            visible: dlg.state_ === "running"
            spacing: 10
            Label { text: qsTr("Exportando…"); font.bold: true }
            ProgressBar { Layout.fillWidth: true; from: 0; to: 1; value: dlg.exporter.progress }
            Label {
                text: qsTr("%1 % · %2 fotogramas/s · quedan %3")
                      .arg(Math.floor(dlg.exporter.progress * 100))
                      .arg(dlg.exporter.speed.toFixed(0))
                      .arg(Format.duration(dlg.exporter.remaining))
                color: Theme.textMuted
            }
            Label {
                visible: dlg.exporter.bottleneck !== ""
                text: qsTr("Etapa más lenta: %1").arg(dlg.exporter.bottleneck)
                color: Theme.textDisabled
                font.pixelSize: Theme.fontSmall
            }
        }

        // --- Resultado ---
        ColumnLayout {
            visible: dlg.state_ === "done"
            spacing: 10
            Label {
                text: dlg.succeeded ? qsTr("Exportación terminada") : qsTr("La exportación no se completó")
                font.bold: true
                color: dlg.succeeded ? Theme.text : Theme.danger
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: dlg.resultMessage
                color: Theme.textMuted
            }
        }
    }

    footer: DialogButtonBox {
        Button {
            visible: dlg.state_ === "settings"
            text: qsTr("Exportar")
            highlighted: true
            enabled: dlg.codecs.length > 0 && dlg.project.durationFrames > 0
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: dlg.startExport()
        }
        Button {
            visible: dlg.state_ === "done" && dlg.succeeded
            text: qsTr("Mostrar en la carpeta")
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: dlg.exporter.revealInExplorer(dlg.outputUrl)
        }
        Button {
            text: dlg.state_ === "running" ? qsTr("Cancelar exportación") : dlg.state_ === "done" ? qsTr("Cerrar") : qsTr("Cancelar")
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: dlg.state_ === "running" ? dlg.exporter.cancel() : dlg.close()
        }
    }
}
