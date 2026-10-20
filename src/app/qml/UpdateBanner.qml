import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Aviso de versión nueva: novedades, instalar (descarga y verifica el MSI) u omitir la versión.
Rectangle {
    id: bar

    required property UpdateController updates
    signal installRequested() // el instalador está listo: la ventana debe cerrarse para abrirlo

    property bool dismissed: false

    visible: updates.available && !dismissed
    z: 95
    width: Math.min(parent.width - 48, layout.implicitWidth + 24)
    height: layout.implicitHeight + 16
    radius: Theme.radius
    color: Theme.noticeSurface
    border.color: Theme.accent

    readonly property bool installerReady: updates.installerReady
    onInstallerReadyChanged: if (installerReady) installRequested()

    RowLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 8
        anchors.leftMargin: 14
        spacing: 6

        Label {
            text: bar.updates.downloading
                  ? qsTr("Descargando MovixStudio %1… %2 %").arg(bar.updates.latestVersion).arg(Math.round(Math.max(0, bar.updates.progress) * 100))
                  : qsTr("MovixStudio %1 está disponible.").arg(bar.updates.latestVersion)
            color: Theme.text
        }
        ProgressBar {
            visible: bar.updates.downloading
            Layout.preferredWidth: 120
            from: 0
            to: 1
            value: Math.max(0, bar.updates.progress)
            indeterminate: bar.updates.progress < 0
        }
        Button {
            text: qsTr("Novedades")
            flat: true
            visible: !bar.updates.downloading && bar.updates.pageUrl.toString() !== ""
            onClicked: Qt.openUrlExternally(bar.updates.pageUrl)
        }
        Button {
            text: bar.updates.installerReady ? qsTr("Instalar ahora") : qsTr("Actualizar")
            highlighted: true
            visible: bar.updates.canInstall && !bar.updates.downloading
            onClicked: bar.updates.installerReady ? bar.installRequested() : bar.updates.install()
        }
        Button {
            text: qsTr("Descargar")
            highlighted: true
            visible: !bar.updates.canInstall
            onClicked: Qt.openUrlExternally(bar.updates.pageUrl)
        }
        Button {
            text: qsTr("Cancelar")
            flat: true
            visible: bar.updates.downloading
            onClicked: bar.updates.cancelDownload()
        }
        Button {
            text: qsTr("Omitir esta versión")
            flat: true
            visible: !bar.updates.downloading
            onClicked: bar.updates.skipVersion()
        }
        Button {
            text: "✕"
            flat: true
            visible: !bar.updates.downloading
            onClicked: bar.dismissed = true
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Recordármelo más tarde")
        }
    }
}
