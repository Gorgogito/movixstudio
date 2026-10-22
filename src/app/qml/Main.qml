import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

ApplicationWindow {
    id: window

    property url initialFile
    property bool closeConfirmed: false
    property var pendingAction: null // acción que espera a resolver los cambios sin guardar

    width: 1440
    height: 900
    minimumWidth: 1100
    minimumHeight: 680
    visible: true
    color: Theme.background
    title: (project.modified ? "• " : "") + project.projectName + " — MovixStudio"

    palette {
        window: Theme.background
        windowText: Theme.text
        base: Theme.surfaceRaised
        alternateBase: Theme.surface
        text: Theme.text
        button: Theme.surfaceRaised
        buttonText: Theme.text
        highlight: Theme.accent
        highlightedText: "#FFFFFF"
        toolTipBase: Theme.surfaceRaised
        toolTipText: Theme.text
        placeholderText: Theme.textMuted
        mid: Theme.border
        dark: Theme.background
        light: Theme.surfaceHover
    }

    ProjectController {
        id: project
        onErrorOccurred: (message) => banner.show(message, true)
        onNotice: (message) => banner.show(message, false)
        onRecoveryAvailable: (when) => {
            recoveryDialog.when = when
            recoveryDialog.open()
        }
    }

    AiController {
        id: ai
        project: project
        onErrorOccurred: (message) => banner.show(message, true)
        onNotice: (message) => banner.show(message, false)
    }

    UpdateController {
        id: updates
        onErrorOccurred: (message) => banner.show(message, true)
        onNotice: (message) => banner.show(message, false)
    }

    readonly property TimelinePlayer activePlayer: viewer.activePlayer

    Component.onCompleted: {
        project.checkRecovery()
        updates.checkAtStartup()
        if (initialFile.toString() === "")
            return
        // Con una copia por recuperar, se decide antes de abrir el archivo: recuperarla lo sustituiría.
        if (recoveryDialog.visible)
            recoveryDialog.pendingFile = initialFile
        else
            project.importFiles([initialFile])
    }

    // --- Cambios sin guardar ---
    function guard(action) {
        if (!project.modified) {
            action()
            return
        }
        pendingAction = action
        unsavedDialog.open()
    }
    function runPending() {
        const action = pendingAction
        pendingAction = null
        if (action)
            action()
    }
    function saveProject(after) {
        if (project.fileUrl.toString() === "") {
            saveDialog.after = after
            saveDialog.open()
        } else if (project.save() && after) {
            after()
        }
    }

    onClosing: (close) => {
        if (closeConfirmed || !project.modified) {
            project.shutdown()
            return
        }
        close.accepted = false
        guard(() => {
            closeConfirmed = true
            window.close()
        })
    }

    // --- Atajos de teclado ---
    Shortcut { sequences: [StandardKey.New]; onActivated: window.guard(() => project.newProject()) }
    Shortcut { sequences: [StandardKey.Open]; onActivated: window.guard(() => openDialog.open()) }
    Shortcut { sequences: [StandardKey.Save]; onActivated: window.saveProject(null) }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: saveDialog.open() }
    Shortcut { sequence: "Ctrl+I"; onActivated: importDialog.open() }
    Shortcut { sequence: "Ctrl+G"; onActivated: project.createCompound() }
    Shortcut {
        sequence: "Ctrl+Shift+G"
        onActivated: if (project.selection.length === 1) project.breakCompound(project.selection[0])
    }
    Shortcut { sequence: "Ctrl+E"; onActivated: exportDialog.open() }
    Shortcut { sequences: [StandardKey.Undo]; onActivated: project.undo() }
    Shortcut { sequences: ["Ctrl+Y", "Ctrl+Shift+Z"]; onActivated: project.redo() }
    Shortcut { sequences: ["Ctrl+B", "S"]; onActivated: { viewer.showTimeline(); project.splitAtPlayhead() } }
    Shortcut { sequence: "Delete"; onActivated: library.wordSelectionActive ? library.deleteWordSelection() : project.deleteSelection(false) }
    Shortcut { sequence: "Shift+Delete"; onActivated: project.deleteSelection(true) }
    Shortcut { sequence: "Ctrl+A"; onActivated: project.selectAll() }
    Shortcut { sequences: [StandardKey.Copy]; onActivated: project.copySelection() }
    Shortcut { sequences: [StandardKey.Cut]; onActivated: project.cutSelection() }
    Shortcut { sequences: [StandardKey.Paste]; onActivated: { viewer.showTimeline(); project.paste() } }
    Shortcut { sequence: "T"; onActivated: { viewer.showTimeline(); project.addTitle(0) } }
    Shortcut { sequence: "Escape"; onActivated: project.clearSelection() }
    Shortcut { sequence: "Ctrl+M"; onActivated: project.addMarker() }
    Shortcut { sequence: "Alt+Right"; onActivated: project.jumpToMarker(true) }
    Shortcut { sequence: "Alt+Left"; onActivated: project.jumpToMarker(false) }
    Shortcut { sequence: "Space"; onActivated: window.activePlayer.togglePlayPause() }
    Shortcut { sequence: "K"; onActivated: window.activePlayer.pause() }
    Shortcut { sequence: "L"; onActivated: window.activePlayer.play() }
    Shortcut { sequence: "Left"; autoRepeat: true; onActivated: window.activePlayer.stepFrames(-1) }
    Shortcut { sequence: "Right"; autoRepeat: true; onActivated: window.activePlayer.stepFrames(1) }
    Shortcut { sequence: "Shift+Left"; autoRepeat: true; onActivated: window.activePlayer.seek(window.activePlayer.position - 1) }
    Shortcut { sequence: "Shift+Right"; autoRepeat: true; onActivated: window.activePlayer.seek(window.activePlayer.position + 1) }
    Shortcut { sequence: "Home"; onActivated: window.activePlayer.seek(0) }
    Shortcut { sequence: "End"; onActivated: window.activePlayer.seek(window.activePlayer.duration) }
    Shortcut { sequence: "M"; onActivated: window.activePlayer.muted = !window.activePlayer.muted }
    Shortcut { sequences: ["+", "="]; onActivated: timeline.zoomBy(1.5) }
    Shortcut { sequence: "-"; onActivated: timeline.zoomBy(1 / 1.5) }
    Shortcut { sequence: "Shift+Z"; onActivated: timeline.fitAll() }
    Shortcut { sequence: "N"; onActivated: timeline.snapping = !timeline.snapping }
    // Multicámara: 1–9 cortan al ángulo en el cabezal (también reproduciendo).
    Instantiator {
        model: 9
        delegate: Shortcut {
            required property int index
            sequence: String(index + 1)
            enabled: project.multicamView
            onActivated: { viewer.showTimeline(); project.switchAngle(index) }
        }
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&Archivo")
            MenuItem { text: qsTr("&Nuevo proyecto") + "\tCtrl+N"; onTriggered: window.guard(() => project.newProject()) }
            MenuItem { text: qsTr("&Abrir proyecto…") + "\tCtrl+O"; onTriggered: window.guard(() => openDialog.open()) }
            MenuItem { text: qsTr("&Guardar") + "\tCtrl+S"; onTriggered: window.saveProject(null) }
            MenuItem { text: qsTr("Guardar &como…") + "\tCtrl+Mayús+S"; onTriggered: saveDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("&Importar medios…") + "\tCtrl+I"; onTriggered: importDialog.open() }
            MenuItem { text: qsTr("&Exportar video…") + "\tCtrl+E"; onTriggered: exportDialog.open() }
            MenuItem { text: qsTr("Importar línea de tiempo (&OTIO)…"); onTriggered: window.guard(() => otioImportDialog.open()) }
            MenuItem { text: qsTr("Exportar línea de tiempo (O&TIO)…"); onTriggered: otioExportDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("Ajustes del &proyecto…"); onTriggered: settingsDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("&Salir"); onTriggered: window.close() }
        }
        Menu {
            title: qsTr("&Editar")
            MenuItem {
                text: (project.canUndo ? qsTr("&Deshacer: %1").arg(project.undoText) : qsTr("&Deshacer")) + "\tCtrl+Z"
                enabled: project.canUndo
                onTriggered: project.undo()
            }
            MenuItem {
                text: (project.canRedo ? qsTr("&Rehacer: %1").arg(project.redoText) : qsTr("&Rehacer")) + "\tCtrl+Y"
                enabled: project.canRedo
                onTriggered: project.redo()
            }
            MenuSeparator {}
            MenuItem { text: qsTr("&Dividir en el cabezal") + "\tCtrl+B"; onTriggered: project.splitAtPlayhead() }
            MenuItem { text: qsTr("&Eliminar") + "\t" + qsTr("Supr"); enabled: project.selection.length > 0; onTriggered: project.deleteSelection(false) }
            MenuItem { text: qsTr("Eliminar y &cerrar hueco") + "\t" + qsTr("Mayús+Supr"); enabled: project.selection.length > 0; onTriggered: project.deleteSelection(true) }
            MenuSeparator {}
            MenuItem { text: qsTr("Co&piar") + "\tCtrl+C"; enabled: project.selection.length > 0; onTriggered: project.copySelection() }
            MenuItem { text: qsTr("Cor&tar") + "\tCtrl+X"; enabled: project.selection.length > 0; onTriggered: project.cutSelection() }
            MenuItem { text: qsTr("Pe&gar en el cabezal") + "\tCtrl+V"; enabled: project.canPaste; onTriggered: project.paste() }
            MenuItem { text: qsTr("Modo &inserción"); checkable: true; checked: project.insertMode; onTriggered: project.insertMode = checked }
            MenuSeparator {}
            MenuItem { text: qsTr("Añadir tít&ulo") + "\tT"; onTriggered: project.addTitle(0) }
            MenuSeparator {}
            MenuItem {
                text: qsTr("Crear clip c&ompuesto") + "\tCtrl+G"
                enabled: project.selection.length > 0
                onTriggered: project.createCompound()
            }
            MenuItem {
                text: qsTr("Volver a la secuencia anterior")
                enabled: project.inCompound
                onTriggered: project.openSequenceLevel(project.sequencePath.length - 2)
            }
            MenuItem { text: qsTr("Seleccionar &todo") + "\tCtrl+A"; onTriggered: project.selectAll() }
            MenuItem { text: qsTr("Añadir pista de &video"); onTriggered: project.addTrack("video") }
            MenuItem { text: qsTr("Añadir pista de &audio"); onTriggered: project.addTrack("audio") }
            MenuSeparator {}
            MenuItem { text: qsTr("Añadir &marcador") + "\tCtrl+M"; onTriggered: project.addMarker() }
            MenuItem { text: qsTr("Quitar los marcadores"); enabled: project.markers.length > 0; onTriggered: project.clearMarkers() }
        }
        Menu {
            title: qsTr("&Reproducción")
            MenuItem {
                text: (window.activePlayer.playing ? qsTr("&Pausar") : qsTr("&Reproducir")) + "\t" + qsTr("Espacio")
                onTriggered: window.activePlayer.togglePlayPause()
            }
            MenuItem { text: qsTr("&Detener"); onTriggered: window.activePlayer.stop() }
            MenuSeparator {}
            MenuItem { text: qsTr("Fotograma &anterior") + "\t←"; onTriggered: window.activePlayer.stepFrames(-1) }
            MenuItem { text: qsTr("Fotograma &siguiente") + "\t→"; onTriggered: window.activePlayer.stepFrames(1) }
            MenuItem { text: qsTr("Ir al &inicio") + "\t" + qsTr("Inicio"); onTriggered: window.activePlayer.seek(0) }
            MenuItem { text: qsTr("Ir al &final") + "\t" + qsTr("Fin"); onTriggered: window.activePlayer.seek(window.activePlayer.duration) }
            MenuSeparator {}
            MenuItem { text: qsTr("Si&lenciar") + "\tM"; checkable: true; checked: window.activePlayer.muted; onTriggered: window.activePlayer.muted = checked }
        }
        Menu {
            title: qsTr("&Ver")
            MenuItem { text: qsTr("&Acercar la línea de tiempo") + "\t+"; onTriggered: timeline.zoomBy(1.5) }
            MenuItem { text: qsTr("A&lejar la línea de tiempo") + "\t−"; onTriggered: timeline.zoomBy(1 / 1.5) }
            MenuItem { text: qsTr("Ajustar &todo") + "\tMayús+Z"; onTriggered: timeline.fitAll() }
            MenuItem { text: qsTr("&Imán") + "\tN"; checkable: true; checked: timeline.snapping; onTriggered: timeline.snapping = checked }
            MenuSeparator {}
            Menu {
                title: qsTr("&Idioma")
                // Los nombres de idioma se muestran siempre en su propio idioma.
                MenuItem { text: "Español"; checkable: true; checked: Language.current === "es"; onTriggered: Language.current = "es" }
                MenuItem { text: "English"; checkable: true; checked: Language.current === "en"; onTriggered: Language.current = "en" }
            }
        }
        Menu {
            title: qsTr("&IA")
            MenuItem {
                text: qsTr("Generar &subtítulos…")
                enabled: !ai.busy && project.durationFrames > 0
                onTriggered: {
                    subtitlesDialog.createTrack = true
                    subtitlesDialog.open()
                }
            }
            MenuItem {
                text: qsTr("&Transcribir para editar por texto…")
                enabled: !ai.busy && project.durationFrames > 0
                onTriggered: {
                    subtitlesDialog.createTrack = false
                    subtitlesDialog.open()
                }
            }
            MenuItem { text: qsTr("Eliminar si&lencios…"); enabled: !ai.busy && project.durationFrames > 0; onTriggered: silenceDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("Marcar los &pulsos de la música"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.markBeats() }
            MenuItem { text: qsTr("Dividir en &escenas"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.splitScenes() }
            MenuItem { text: qsTr("Separar &voz y música"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.separateVoice() }
            MenuSeparator {}
            MenuItem { text: qsTr("Quitar el fondo (&personas)"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.removeBackground("portrait") }
            MenuItem { text: qsTr("Quitar el fondo (cualquier &sujeto)"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.removeBackground("general") }
            MenuItem { text: qsTr("Aumentar la resolución ×&2"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.upscale(2) }
            MenuItem { text: qsTr("Aumentar la resolución ×&4"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.upscale(4) }
            MenuItem { text: qsTr("Cámara lenta suave ×2"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.slowMotion(2) }
            MenuItem { text: qsTr("Cámara lenta suave ×4"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.slowMotion(4) }
            MenuItem { text: qsTr("Seguir el &movimiento de la máscara"); enabled: !ai.busy && project.selection.length > 0; onTriggered: ai.trackMotion() }
            Menu {
                title: qsTr("&Reencuadre automático")
                enabled: !ai.busy && project.selection.length > 0
                MenuItem { text: qsTr("Vertical 9:16"); onTriggered: ai.autoReframe("9:16") }
                MenuItem { text: qsTr("Cuadrado 1:1"); onTriggered: ai.autoReframe("1:1") }
                MenuItem { text: qsTr("Retrato 4:5"); onTriggered: ai.autoReframe("4:5") }
            }
            MenuSeparator {}
            MenuItem { text: qsTr("Te&xto a voz…"); enabled: !ai.busy; onTriggered: ttsDialog.open() }
            MenuItem { text: qsTr("T&raducir subtítulos…"); enabled: !ai.busy && project.durationFrames > 0; onTriggered: translateDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("&Importar subtítulos (SRT)…"); onTriggered: srtImportDialog.open() }
            MenuItem { text: qsTr("&Exportar subtítulos (SRT)…"); enabled: project.durationFrames > 0 && ai.hasSubtitleTrack(); onTriggered: srtExportDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("&Modelos de IA…"); onTriggered: modelsDialog.open() }
        }
        Menu {
            title: qsTr("Ay&uda")
            MenuItem { text: qsTr("&Buscar actualizaciones"); enabled: updates.enabled && !updates.checking; onTriggered: updates.check() }
            MenuItem {
                text: qsTr("Buscar actualizaciones al &iniciar")
                enabled: updates.enabled
                checkable: true
                checked: updates.autoCheck
                onTriggered: updates.autoCheck = checked
            }
            MenuSeparator {}
            MenuItem { text: qsTr("&Acerca de MovixStudio"); onTriggered: aboutDialog.open() }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Vertical

        SplitView {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 300
            orientation: Qt.Horizontal

            LibraryPanel {
                id: library
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 220
                project: project
                ai: ai
                onTranscribeRequested: {
                    subtitlesDialog.createTrack = false
                    subtitlesDialog.open()
                }
                onTimelineRequested: viewer.showTimeline()
                onImportRequested: importDialog.open()
                onPreviewRequested: (mediaId) => {
                    project.previewMedia(mediaId)
                    viewer.showSource()
                }
            }
            ViewerPanel {
                id: viewer
                SplitView.fillWidth: true
                SplitView.minimumWidth: 400
                project: project
                overlayMode: inspector.overlayMode
                overlayMaskIndex: inspector.maskIndex
                onKeyPicked: inspector.pickingKey = false
                onObjectPicked: (u, v) => {
                    inspector.pickingObject = false
                    ai.segmentObject(u, v)
                }
            }
            Inspector {
                id: inspector
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 260
                project: project
                ai: ai
                onSettingsRequested: settingsDialog.open()
            }
        }

        Timeline {
            id: timeline
            SplitView.preferredHeight: 320
            SplitView.minimumHeight: 200
            project: project
            onActivated: viewer.showTimeline()
        }
    }

    // Archivos arrastrados desde el explorador: se importan (o se abre el proyecto).
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (drop.hasUrls) {
                project.importFiles(drop.urls)
                drop.accept()
            }
        }
    }

    Banner { id: banner; anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 12 }
    UpdateBanner {
        updates: updates
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: banner.visible ? banner.height + 20 : 12
        // Cerrar con los cambios resueltos y abrir el instalador (que sustituye esta versión).
        onInstallRequested: window.guard(() => {
            if (updates.launchInstaller()) {
                window.closeConfirmed = true
                window.close()
            }
        })
    }

    // --- Diálogos ---
    FileDialog {
        id: importDialog
        title: qsTr("Importar medios")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("Medios compatibles") + " (*.mp4 *.mov *.m4v *.mkv *.webm *.avi *.wmv *.flv *.mts *.m2ts *.ts *.mxf *.mpg *.mpeg *.3gp *.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus *.wma *.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)",
            qsTr("Todos los archivos") + " (*)"
        ]
        onAccepted: project.importFiles(selectedFiles)
    }
    FileDialog {
        id: openDialog
        title: qsTr("Abrir proyecto")
        nameFilters: [qsTr("Proyectos de MovixStudio") + " (*.mxproj)"]
        onAccepted: project.openProject(selectedFile)
    }
    FileDialog {
        id: saveDialog
        property var after: null
        title: qsTr("Guardar proyecto")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mxproj"
        nameFilters: [qsTr("Proyectos de MovixStudio") + " (*.mxproj)"]
        onAccepted: {
            const next = after
            after = null
            if (project.saveAs(selectedFile) && next)
                next()
        }
        onRejected: after = null
    }

    Dialog {
        id: unsavedDialog
        title: qsTr("Cambios sin guardar")
        modal: true
        anchors.centerIn: parent
        width: 460
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("¿Quieres guardar los cambios de «%1»?").arg(project.projectName)
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Guardar")
                highlighted: true
                onClicked: {
                    unsavedDialog.close()
                    window.saveProject(() => window.runPending())
                }
            }
            Button {
                text: qsTr("No guardar")
                onClicked: {
                    unsavedDialog.close()
                    window.runPending()
                }
            }
            Button {
                text: qsTr("Cancelar")
                onClicked: {
                    window.pendingAction = null
                    unsavedDialog.close()
                }
            }
        }
    }

    Dialog {
        id: recoveryDialog
        property string when
        property url pendingFile // archivo con el que se abrió la aplicación, a la espera de la decisión
        readonly property string pendingName: pendingFile.toString() === ""
                                              ? "" : decodeURIComponent(pendingFile.toString().split("/").pop())
        title: qsTr("Recuperar proyecto")
        modal: true
        anchors.centerIn: parent
        width: 480
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: recoveryDialog.pendingName === ""
                  ? qsTr("MovixStudio no se cerró correctamente. Hay una copia automática del %1.\n¿Quieres recuperarla?").arg(recoveryDialog.when)
                  : qsTr("MovixStudio no se cerró correctamente. Hay una copia automática del %1.\n¿Quieres recuperarla en lugar de abrir «%2»?").arg(recoveryDialog.when).arg(recoveryDialog.pendingName)
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Recuperar")
                highlighted: true
                onClicked: {
                    recoveryDialog.pendingFile = ""
                    recoveryDialog.close()
                    project.recoverAutosave()
                }
            }
            Button {
                text: recoveryDialog.pendingName === "" ? qsTr("Descartar") : qsTr("Descartar y abrir «%1»").arg(recoveryDialog.pendingName)
                onClicked: {
                    project.discardAutosave()
                    recoveryDialog.close()
                }
            }
        }
        // Al descartar (o al cerrar con Esc, que conserva la copia para la próxima vez) se abre el archivo.
        onClosed: {
            if (pendingFile.toString() === "")
                return
            const file = pendingFile
            pendingFile = ""
            project.importFiles([file])
        }
    }

    ExportDialog { id: exportDialog; project: project }
    ProjectSettingsDialog { id: settingsDialog; project: project }
    SubtitlesDialog { id: subtitlesDialog; ai: ai }
    ModelsDialog { id: modelsDialog; ai: ai }
    SilenceDialog { id: silenceDialog; ai: ai }
    TtsDialog { id: ttsDialog; ai: ai }
    TranslateDialog { id: translateDialog; ai: ai }
    FileDialog {
        id: srtImportDialog
        title: qsTr("Importar subtítulos")
        nameFilters: [qsTr("Subtítulos SubRip") + " (*.srt)", qsTr("Todos los archivos") + " (*)"]
        onAccepted: ai.importSrt(selectedFile)
    }
    FileDialog {
        id: otioImportDialog
        title: qsTr("Importar línea de tiempo")
        nameFilters: [qsTr("OpenTimelineIO") + " (*.otio)"]
        onAccepted: project.importOtio(selectedFile)
    }
    FileDialog {
        id: otioExportDialog
        title: qsTr("Exportar línea de tiempo")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "otio"
        nameFilters: [qsTr("OpenTimelineIO") + " (*.otio)"]
        onAccepted: project.exportOtio(selectedFile)
    }
    FileDialog {
        id: srtExportDialog
        title: qsTr("Exportar subtítulos")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "srt"
        nameFilters: [qsTr("Subtítulos SubRip") + " (*.srt)"]
        onAccepted: ai.exportSrt(selectedFile)
    }

    AiStatus {
        ai: ai
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
    }

    Dialog {
        id: aboutDialog
        title: qsTr("Acerca de MovixStudio")
        modal: true
        anchors.centerIn: parent
        width: 480
        standardButtons: Dialog.Close

        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            RowLayout {
                spacing: 14
                Image { source: "qrc:/qt/qml/MovixStudio/icons/logo.svg"; sourceSize: Qt.size(48, 48) }
                ColumnLayout {
                    spacing: 2
                    Text { text: "MovixStudio"; color: Theme.text; font.pixelSize: 20; font.bold: true }
                    Text { text: qsTr("Versión %1").arg(Qt.application.version); color: Theme.textMuted }
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                text: qsTr("Editor de video libre y de código abierto, sin suscripciones ni marcas de agua. Versión de desarrollo: edición multipista con keyframes, títulos, transiciones, corrección de color, composición e IA local de audio, texto y video (fase F5).")
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall + 1
                text: qsTr("Distribuido bajo la Licencia Pública General de GNU, versión 3 o posterior. Usa Qt (LGPLv3), FFmpeg (GPL), miniaudio (dominio público), OpenColorIO (BSD), whisper.cpp (MIT), ONNX Runtime (MIT), eSpeak NG (GPLv3) y SentencePiece (Apache 2.0). Los modelos de IA se descargan aparte, cada uno con su licencia.")
            }
        }
    }
}
