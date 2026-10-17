import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MovixStudio
import "Format.js" as Format

ApplicationWindow {
    id: window

    property url initialFile
    property bool showInfo: true

    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    color: Theme.background
    title: player.hasMedia ? qsTr("%1 — MovixStudio").arg(Format.fileName(player.source)) : "MovixStudio"

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

    Player {
        id: player
        onErrorOccurred: (message) => errorBanner.show(message)
    }

    Component.onCompleted: {
        if (initialFile.toString() !== "")
            player.open(initialFile)
    }

    // --- Atajos de teclado (válidos en toda la ventana) ---
    Shortcut { sequences: [StandardKey.Open]; onActivated: openDialog.open() }
    Shortcut { sequences: [StandardKey.Quit]; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; enabled: player.hasMedia; onActivated: player.togglePlayPause() }
    Shortcut { sequence: "K"; enabled: player.hasMedia; onActivated: player.pause() }
    Shortcut { sequence: "L"; enabled: player.hasMedia; onActivated: player.play() }
    Shortcut { sequence: "Left"; enabled: player.hasVideo; autoRepeat: true; onActivated: player.stepFrames(-1) }
    Shortcut { sequence: "Right"; enabled: player.hasVideo; autoRepeat: true; onActivated: player.stepFrames(1) }
    Shortcut { sequence: "Shift+Left"; enabled: player.hasMedia; autoRepeat: true; onActivated: player.seek(player.position - 1) }
    Shortcut { sequence: "Shift+Right"; enabled: player.hasMedia; autoRepeat: true; onActivated: player.seek(player.position + 1) }
    Shortcut { sequence: "Home"; enabled: player.hasMedia; onActivated: player.seek(0) }
    Shortcut { sequence: "End"; enabled: player.hasMedia; onActivated: player.seek(player.duration) }
    Shortcut { sequence: "M"; onActivated: player.muted = !player.muted }
    Shortcut { sequence: "I"; onActivated: window.showInfo = !window.showInfo }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&Archivo")
            MenuItem { text: qsTr("&Abrir medio…") + "\tCtrl+O"; onTriggered: openDialog.open() }
            MenuItem { text: qsTr("&Cerrar medio"); enabled: player.hasMedia; onTriggered: player.close() }
            MenuSeparator {}
            MenuItem { text: qsTr("&Salir") + "\tCtrl+Q"; onTriggered: Qt.quit() }
        }
        Menu {
            title: qsTr("&Reproducción")
            MenuItem {
                text: (player.playing ? qsTr("&Pausar") : qsTr("&Reproducir")) + "\t" + qsTr("Espacio")
                enabled: player.hasMedia
                onTriggered: player.togglePlayPause()
            }
            MenuItem { text: qsTr("&Detener"); enabled: player.hasMedia; onTriggered: player.stop() }
            MenuSeparator {}
            MenuItem { text: qsTr("Fotograma &anterior") + "\t←"; enabled: player.hasVideo; onTriggered: player.stepFrames(-1) }
            MenuItem { text: qsTr("Fotograma &siguiente") + "\t→"; enabled: player.hasVideo; onTriggered: player.stepFrames(1) }
            MenuItem { text: qsTr("Ir al &inicio") + "\t" + qsTr("Inicio"); enabled: player.hasMedia; onTriggered: player.seek(0) }
            MenuItem { text: qsTr("Ir al &final") + "\t" + qsTr("Fin"); enabled: player.hasMedia; onTriggered: player.seek(player.duration) }
            MenuSeparator {}
            MenuItem { text: qsTr("Si&lenciar") + "\tM"; checkable: true; checked: player.muted; onTriggered: player.muted = checked }
        }
        Menu {
            title: qsTr("&Ver")
            MenuItem { text: qsTr("Panel de &información") + "\tI"; checkable: true; checked: window.showInfo; onTriggered: window.showInfo = checked }
            Menu {
                title: qsTr("&Idioma")
                // Los nombres de idioma se muestran siempre en su propio idioma.
                MenuItem { text: "Español"; checkable: true; checked: Language.current === "es"; onTriggered: Language.current = "es" }
                MenuItem { text: "English"; checkable: true; checked: Language.current === "en"; onTriggered: Language.current = "en" }
            }
        }
        Menu {
            title: qsTr("Ay&uda")
            MenuItem { text: qsTr("&Acerca de MovixStudio"); onTriggered: aboutDialog.open() }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                id: viewer
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.viewer

                VideoView {
                    anchors.fill: parent
                    player: player
                    visible: player.hasVideo
                }

                // Estado vacío: invitación a abrir un archivo.
                Column {
                    anchors.centerIn: parent
                    spacing: 16
                    visible: !player.hasMedia

                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        source: "qrc:/qt/qml/MovixStudio/icons/logo.svg"
                        sourceSize: Qt.size(72, 72)
                        opacity: dropArea.containsDrag ? 1 : 0.85
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: dropArea.containsDrag ? qsTr("Suelta el archivo para abrirlo") : qsTr("Arrastra un video o un audio aquí")
                        color: Theme.text
                        font.pixelSize: 18
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("o usa Archivo → Abrir medio (Ctrl+O)")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontNormal
                    }
                    Button {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Abrir medio…")
                        icon.source: "qrc:/qt/qml/MovixStudio/icons/folder-open.svg"
                        focusPolicy: Qt.NoFocus
                        onClicked: openDialog.open()
                    }
                }

                // Medio de solo audio.
                Column {
                    anchors.centerIn: parent
                    spacing: 10
                    visible: player.hasMedia && !player.hasVideo
                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        source: "qrc:/qt/qml/MovixStudio/icons/volume.svg"
                        sourceSize: Qt.size(56, 56)
                        opacity: 0.6
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Archivo de solo audio")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontNormal + 2
                    }
                }

                // Clic para reproducir/pausar; doble clic para pantalla completa.
                MouseArea {
                    anchors.fill: parent
                    enabled: player.hasMedia
                    onClicked: player.togglePlayPause()
                    onDoubleClicked: {
                        player.togglePlayPause() // deshace el clic simple
                        window.visibility = window.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
                    }
                }

                DropArea {
                    id: dropArea
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    onDropped: (drop) => {
                        if (drop.hasUrls) {
                            player.open(drop.urls[0])
                            drop.accept()
                        }
                    }
                }

                // Aviso de error no bloqueante.
                Rectangle {
                    id: errorBanner

                    function show(message) {
                        errorText.text = message
                        visible = true
                        hideTimer.restart()
                    }

                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 16
                    width: Math.min(parent.width - 32, errorText.implicitWidth + 32)
                    height: errorText.implicitHeight + 20
                    radius: Theme.radius
                    color: Theme.dangerSurface
                    border.color: Theme.danger
                    visible: false

                    Text {
                        id: errorText
                        anchors.centerIn: parent
                        width: parent.width - 32
                        color: Theme.text
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Timer {
                        id: hideTimer
                        interval: 6000
                        onTriggered: errorBanner.visible = false
                    }
                }
            }

            InfoPanel {
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                visible: window.showInfo && window.visibility !== Window.FullScreen
                player: player
            }
        }

        TransportBar {
            Layout.fillWidth: true
            player: player
            visible: window.visibility !== Window.FullScreen
        }
    }

    FileDialog {
        id: openDialog
        title: qsTr("Abrir medio")
        nameFilters: [
            qsTr("Medios compatibles") + " (*.mp4 *.mov *.m4v *.mkv *.webm *.avi *.wmv *.flv *.mts *.m2ts *.ts *.mxf *.mpg *.mpeg *.3gp *.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus *.wma)",
            qsTr("Todos los archivos") + " (*)"
        ]
        onAccepted: player.open(selectedFile)
    }

    Dialog {
        id: aboutDialog
        title: qsTr("Acerca de MovixStudio")
        modal: true
        anchors.centerIn: parent
        width: 460
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
                text: qsTr("Editor de video libre y de código abierto, sin suscripciones ni marcas de agua. Versión de desarrollo: reproductor y motor de medios (fase F0).")
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall + 1
                text: qsTr("Distribuido bajo la Licencia Pública General de GNU, versión 3 o posterior. Usa Qt (LGPLv3), FFmpeg (GPL) y miniaudio (dominio público).")
            }
        }
    }
}
