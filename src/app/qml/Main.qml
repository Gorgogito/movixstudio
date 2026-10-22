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

    // --- Atajos de teclado (configurables: Editar → Atajos de teclado) ---
    ShortcutController { id: keys }
    Shortcut { sequences: keys.keys.new; onActivated: window.guard(() => project.newProject()) }
    Shortcut { sequences: keys.keys.open; onActivated: window.guard(() => openDialog.open()) }
    Shortcut { sequences: keys.keys.save; onActivated: window.saveProject(null) }
    Shortcut { sequences: keys.keys.saveAs; onActivated: saveDialog.open() }
    Shortcut { sequences: keys.keys["import"]; onActivated: importDialog.open() }
    Shortcut { sequences: keys.keys.compound; onActivated: project.createCompound() }
    Shortcut {
        sequences: keys.keys.breakCompound
        onActivated: if (project.selection.length === 1) project.breakCompound(project.selection[0])
    }
    Shortcut { sequences: keys.keys["export"]; onActivated: exportDialog.open() }
    Shortcut { sequences: keys.keys.undo; onActivated: project.undo() }
    Shortcut { sequences: keys.keys.redo; onActivated: project.redo() }
    Shortcut { sequences: keys.keys.split; onActivated: { viewer.showTimeline(); project.splitAtPlayhead() } }
    Shortcut { sequences: keys.keys["delete"]; onActivated: library.wordSelectionActive ? library.deleteWordSelection() : project.deleteSelection(false) }
    Shortcut { sequences: keys.keys.rippleDelete; onActivated: project.deleteSelection(true) }
    Shortcut { sequences: keys.keys.selectAll; onActivated: project.selectAll() }
    Shortcut { sequences: keys.keys.copy; onActivated: project.copySelection() }
    Shortcut { sequences: keys.keys.cut; onActivated: project.cutSelection() }
    Shortcut { sequences: keys.keys.paste; onActivated: { viewer.showTimeline(); project.paste() } }
    Shortcut { sequences: keys.keys.title; onActivated: { viewer.showTimeline(); project.addTitle(0) } }
    Shortcut { sequences: keys.keys.deselect; onActivated: project.clearSelection() }
    Shortcut { sequences: keys.keys.marker; onActivated: project.addMarker() }
    Shortcut { sequences: keys.keys.nextMarker; onActivated: project.jumpToMarker(true) }
    Shortcut { sequences: keys.keys.prevMarker; onActivated: project.jumpToMarker(false) }
    Shortcut { sequences: keys.keys.playPause; onActivated: window.activePlayer.togglePlayPause() }
    Shortcut { sequences: keys.keys.pause; onActivated: window.activePlayer.pause() }
    Shortcut { sequences: keys.keys.play; onActivated: window.activePlayer.play() }
    Shortcut { sequences: keys.keys.stepBack; autoRepeat: true; onActivated: window.activePlayer.stepFrames(-1) }
    Shortcut { sequences: keys.keys.stepForward; autoRepeat: true; onActivated: window.activePlayer.stepFrames(1) }
    Shortcut { sequences: keys.keys.back1s; autoRepeat: true; onActivated: window.activePlayer.seek(window.activePlayer.position - 1) }
    Shortcut { sequences: keys.keys.forward1s; autoRepeat: true; onActivated: window.activePlayer.seek(window.activePlayer.position + 1) }
    Shortcut { sequences: keys.keys.home; onActivated: window.activePlayer.seek(0) }
    Shortcut { sequences: keys.keys.end; onActivated: window.activePlayer.seek(window.activePlayer.duration) }
    Shortcut { sequences: keys.keys.mute; onActivated: window.activePlayer.muted = !window.activePlayer.muted }
    Shortcut { sequences: keys.keys.zoomIn; onActivated: timeline.zoomBy(1.5) }
    Shortcut { sequences: keys.keys.zoomOut; onActivated: timeline.zoomBy(1 / 1.5) }
    Shortcut { sequences: keys.keys.fitAll; onActivated: timeline.fitAll() }
    Shortcut { sequences: keys.keys.snapping; onActivated: timeline.snapping = !timeline.snapping }
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
            width: 340 // cabe el atajo a la derecha de cada acción
            MenuItem { text: qsTr("&Nuevo proyecto") + "\t" + keys.text("new"); onTriggered: window.guard(() => project.newProject()) }
            MenuItem { text: qsTr("&Abrir proyecto…") + "\t" + keys.text("open"); onTriggered: window.guard(() => openDialog.open()) }
            Menu {
                id: newFromTemplateMenu
                title: qsTr("Nuevo desde p&lantilla")
                enabled: project.projectTemplates.length > 0
                Instantiator {
                    model: project.projectTemplates
                    delegate: MenuItem {
                        required property string modelData
                        text: modelData
                        onTriggered: window.guard(() => project.newFromTemplate(modelData))
                    }
                    onObjectAdded: (index, object) => newFromTemplateMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => newFromTemplateMenu.removeItem(object)
                }
            }
            MenuItem { text: qsTr("&Guardar") + "\t" + keys.text("save"); onTriggered: window.saveProject(null) }
            MenuItem { text: qsTr("Guardar &como…") + "\t" + keys.text("saveAs"); onTriggered: saveDialog.open() }
            MenuItem { text: qsTr("Guardar como plan&tilla…"); onTriggered: projectTemplateDialog.open() }
            Menu {
                id: removeTemplateMenu
                title: qsTr("Eliminar plantilla de proyecto")
                enabled: project.projectTemplates.length > 0
                Instantiator {
                    model: project.projectTemplates
                    delegate: MenuItem {
                        required property string modelData
                        text: modelData
                        onTriggered: project.removeProjectTemplate(modelData)
                    }
                    onObjectAdded: (index, object) => removeTemplateMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => removeTemplateMenu.removeItem(object)
                }
            }
            MenuSeparator {}
            MenuItem { text: qsTr("&Importar medios…") + "\t" + keys.text("import"); onTriggered: importDialog.open() }
            MenuItem { text: qsTr("&Exportar video…") + "\t" + keys.text("export"); onTriggered: exportDialog.open() }
            MenuItem { text: qsTr("Importar línea de tiempo (&OTIO)…"); onTriggered: window.guard(() => otioImportDialog.open()) }
            MenuItem { text: qsTr("Exportar línea de tiempo (O&TIO)…"); onTriggered: otioExportDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("Ajustes del &proyecto…"); onTriggered: settingsDialog.open() }
            MenuSeparator {}
            MenuItem { text: qsTr("&Salir"); onTriggered: window.close() }
        }
        Menu {
            title: qsTr("&Editar")
            width: 340
            MenuItem {
                text: (project.canUndo ? qsTr("&Deshacer: %1").arg(project.undoText) : qsTr("&Deshacer")) + "\t" + keys.text("undo")
                enabled: project.canUndo
                onTriggered: project.undo()
            }
            MenuItem {
                text: (project.canRedo ? qsTr("&Rehacer: %1").arg(project.redoText) : qsTr("&Rehacer")) + "\t" + keys.text("redo")
                enabled: project.canRedo
                onTriggered: project.redo()
            }
            MenuSeparator {}
            MenuItem { text: qsTr("&Dividir en el cabezal") + "\t" + keys.text("split"); onTriggered: project.splitAtPlayhead() }
            MenuItem { text: qsTr("&Eliminar") + "\t" + keys.text("delete"); enabled: project.selection.length > 0; onTriggered: project.deleteSelection(false) }
            MenuItem { text: qsTr("Eliminar y &cerrar hueco") + "\t" + keys.text("rippleDelete"); enabled: project.selection.length > 0; onTriggered: project.deleteSelection(true) }
            MenuSeparator {}
            MenuItem { text: qsTr("Co&piar") + "\t" + keys.text("copy"); enabled: project.selection.length > 0; onTriggered: project.copySelection() }
            MenuItem { text: qsTr("Cor&tar") + "\t" + keys.text("cut"); enabled: project.selection.length > 0; onTriggered: project.cutSelection() }
            MenuItem { text: qsTr("Pe&gar en el cabezal") + "\t" + keys.text("paste"); enabled: project.canPaste; onTriggered: project.paste() }
            MenuItem { text: qsTr("Modo &inserción"); checkable: true; checked: project.insertMode; onTriggered: project.insertMode = checked }
            MenuSeparator {}
            MenuItem { text: qsTr("Añadir tít&ulo") + "\t" + keys.text("title"); onTriggered: project.addTitle(0) }
            MenuSeparator {}
            MenuItem {
                text: qsTr("Crear clip c&ompuesto") + "\t" + keys.text("compound")
                enabled: project.selection.length > 0
                onTriggered: project.createCompound()
            }
            MenuItem {
                text: qsTr("Volver a la secuencia anterior")
                enabled: project.inCompound
                onTriggered: project.openSequenceLevel(project.sequencePath.length - 2)
            }
            MenuItem { text: qsTr("Seleccionar &todo") + "\t" + keys.text("selectAll"); onTriggered: project.selectAll() }
            MenuItem { text: qsTr("Añadir pista de &video"); onTriggered: project.addTrack("video") }
            MenuItem { text: qsTr("Añadir pista de &audio"); onTriggered: project.addTrack("audio") }
            MenuSeparator {}
            MenuItem { text: qsTr("Añadir &marcador") + "\t" + keys.text("marker"); onTriggered: project.addMarker() }
            MenuItem { text: qsTr("Quitar los marcadores"); enabled: project.markers.length > 0; onTriggered: project.clearMarkers() }
            MenuSeparator {}
            MenuItem { text: qsTr("Atajos de &teclado…"); onTriggered: shortcutsDialog.open() }
        }
        Menu {
            title: qsTr("&Reproducción")
            width: 300
            MenuItem {
                text: (window.activePlayer.playing ? qsTr("&Pausar") : qsTr("&Reproducir")) + "\t" + keys.text("playPause")
                onTriggered: window.activePlayer.togglePlayPause()
            }
            MenuItem { text: qsTr("&Detener"); onTriggered: window.activePlayer.stop() }
            MenuSeparator {}
            MenuItem { text: qsTr("Fotograma &anterior") + "\t" + keys.text("stepBack"); onTriggered: window.activePlayer.stepFrames(-1) }
            MenuItem { text: qsTr("Fotograma &siguiente") + "\t" + keys.text("stepForward"); onTriggered: window.activePlayer.stepFrames(1) }
            MenuItem { text: qsTr("Ir al &inicio") + "\t" + keys.text("home"); onTriggered: window.activePlayer.seek(0) }
            MenuItem { text: qsTr("Ir al &final") + "\t" + keys.text("end"); onTriggered: window.activePlayer.seek(window.activePlayer.duration) }
            MenuSeparator {}
            MenuItem { text: qsTr("Si&lenciar") + "\t" + keys.text("mute"); checkable: true; checked: window.activePlayer.muted; onTriggered: window.activePlayer.muted = checked }
        }
        Menu {
            title: qsTr("&Ver")
            width: 340
            MenuItem { text: qsTr("&Acercar la línea de tiempo") + "\t" + keys.text("zoomIn"); onTriggered: timeline.zoomBy(1.5) }
            MenuItem { text: qsTr("A&lejar la línea de tiempo") + "\t" + keys.text("zoomOut"); onTriggered: timeline.zoomBy(1 / 1.5) }
            MenuItem { text: qsTr("Ajustar &todo") + "\t" + keys.text("fitAll"); onTriggered: timeline.fitAll() }
            MenuItem { text: qsTr("&Imán") + "\t" + keys.text("snapping"); checkable: true; checked: timeline.snapping; onTriggered: timeline.snapping = checked }
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
            id: scriptsMenu
            title: qsTr("&Scripts")
            onAboutToShow: project.reloadScripts() // recoge los que se añadan a la carpeta
            Instantiator {
                model: project.scripts
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    ToolTip.visible: hovered && modelData.description !== ""
                    ToolTip.delay: 500
                    ToolTip.text: modelData.description
                    onTriggered: { viewer.showTimeline(); project.runScript(modelData.path) }
                }
                onObjectAdded: (index, object) => scriptsMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => scriptsMenu.removeItem(object)
            }
            MenuSeparator {}
            MenuItem { text: qsTr("Abrir la carpeta de &scripts"); onTriggered: project.openScriptsFolder() }
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
    ShortcutsDialog {
        id: shortcutsDialog
        keys: keys
    }
    NameDialog {
        id: projectTemplateDialog
        title: qsTr("Guardar como plantilla de proyecto")
        label: qsTr("Nombre de la plantilla. Guarda los ajustes, las pistas, los títulos, los marcadores y los medios del proyecto (con el mismo nombre se sustituye).")
        initialName: project.projectName
        onNamed: (name) => project.saveProjectTemplate(name)
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
