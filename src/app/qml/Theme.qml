pragma Singleton
import QtQuick

// Paleta y medidas de la identidad visual de MovixStudio.
QtObject {
    readonly property color background: "#0F1115"
    readonly property color surface: "#161920"
    readonly property color surfaceRaised: "#1D2129"
    readonly property color surfaceHover: "#262B35"
    readonly property color border: "#2A2F3A"
    readonly property color viewer: "#08090C"
    readonly property color text: "#E8EAF0"
    readonly property color textMuted: "#99A1B3"
    readonly property color textDisabled: "#5B6273"
    readonly property color accent: "#7C6CFF"
    readonly property color accentHover: "#9688FF"
    readonly property color accentPressed: "#6555F0"
    readonly property color danger: "#FF5C6C"
    readonly property color dangerSurface: "#3A1A20"
    readonly property color noticeSurface: "#1E2440"

    // Línea de tiempo
    readonly property color trackVideo: "#141823"
    readonly property color trackAudio: "#121A18"
    readonly property color clipVideo: "#4B57C9"
    readonly property color clipVideoDark: "#343E99"
    readonly property color clipAudio: "#1E8A68"
    readonly property color clipAudioDark: "#166650"
    readonly property color clipTitle: "#7A4AB0"
    readonly property color clipCompound: "#B0662E"
    readonly property color clipSelected: "#FFC266"
    readonly property color playhead: "#FF5C8A"
    readonly property color snapGuide: "#FFC266"

    readonly property int radius: 6
    readonly property int spacing: 8
    readonly property string monoFont: "Cascadia Mono"
    readonly property int fontSmall: 11
    readonly property int fontNormal: 13
}
