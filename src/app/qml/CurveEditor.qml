import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MovixStudio

// Editor de curvas (general y por canal). Clic fuera de un punto añade uno, arrastrar lo mueve y
// doble clic lo quita; los extremos solo se mueven en vertical. La curva se dibuja con la misma
// interpolación monótona (Fritsch–Carlson) que aplica el motor.
ColumnLayout {
    id: editor

    property var curves: ({})
    property string channel: "master"
    signal curveEdited(string channel, var points)
    signal released()

    readonly property var channelColors: ({ master: "#E8EAF0", red: "#FF5C5C", green: "#4CD964", blue: "#5C8DFF" })
    function pointsOf(ch) {
        const c = curves[ch] ?? []
        return c.length ? c : [[0, 0], [1, 1]]
    }
    property var work: null // copia mientras se arrastra
    readonly property var points: work ?? pointsOf(channel)

    // Curva monótona: devuelve una función x → y (misma fórmula que color::Curve).
    function interpolator(input) {
        let pts = input.slice().sort((a, b) => a[0] - b[0])
        if (pts[0][0] > 1e-4) pts.unshift([0, 0])
        if (pts[pts.length - 1][0] < 1 - 1e-4) pts.push([1, 1])
        const n = pts.length
        const d = [], m = []
        for (let i = 0; i + 1 < n; ++i)
            d.push((pts[i + 1][1] - pts[i][1]) / Math.max(1e-9, pts[i + 1][0] - pts[i][0]))
        m[0] = d[0]
        m[n - 1] = d[n - 2]
        for (let i = 1; i + 1 < n; ++i)
            m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2
        for (let i = 0; i + 1 < n; ++i) {
            if (d[i] === 0) { m[i] = 0; m[i + 1] = 0; continue }
            const a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b
            if (s > 9) { const t = 3 / Math.sqrt(s); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i] }
        }
        return function (x) {
            let seg = 0
            while (seg + 2 < n && x > pts[seg + 1][0]) ++seg
            const h = pts[seg + 1][0] - pts[seg][0]
            const t = h > 0 ? Math.max(0, Math.min(1, (x - pts[seg][0]) / h)) : 0
            const t2 = t * t, t3 = t2 * t
            const y = (2 * t3 - 3 * t2 + 1) * pts[seg][1] + (t3 - 2 * t2 + t) * h * m[seg]
                    + (-2 * t3 + 3 * t2) * pts[seg + 1][1] + (t3 - t2) * h * m[seg + 1]
            return Math.max(0, Math.min(1, y))
        }
    }

    onPointsChanged: plot.requestPaint()
    onCurvesChanged: plot.requestPaint()
    spacing: 4

    RowLayout {
        Layout.fillWidth: true
        spacing: 2
        Repeater {
            model: [{ key: "master", label: qsTr("General") }, { key: "red", label: "R" }, { key: "green", label: "G" }, { key: "blue", label: "B" }]
            Button {
                required property var modelData
                text: modelData.label
                checkable: true
                checked: editor.channel === modelData.key
                focusPolicy: Qt.NoFocus
                Layout.fillWidth: true
                Layout.preferredWidth: modelData.key === "master" ? 2 : 1
                palette.buttonText: editor.channelColors[modelData.key]
                onClicked: editor.channel = modelData.key
            }
        }
    }

    Item {
        id: area
        Layout.fillWidth: true
        Layout.preferredHeight: width
        readonly property real inset: 6
        readonly property real size: width - 2 * inset
        function toPx(p) { return Qt.point(inset + p[0] * size, inset + (1 - p[1]) * size) }
        function fromPx(x, y) { return [Math.max(0, Math.min(1, (x - inset) / size)), Math.max(0, Math.min(1, 1 - (y - inset) / size))] }

        Canvas {
            id: plot
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const s = area.size, o = area.inset
                ctx.fillStyle = "#0C0D10"
                ctx.fillRect(o, o, s, s)
                ctx.strokeStyle = "#2A2F3A"
                ctx.lineWidth = 1
                for (let i = 1; i < 4; ++i) {
                    ctx.beginPath(); ctx.moveTo(o + s * i / 4, o); ctx.lineTo(o + s * i / 4, o + s); ctx.stroke()
                    ctx.beginPath(); ctx.moveTo(o, o + s * i / 4); ctx.lineTo(o + s, o + s * i / 4); ctx.stroke()
                }
                ctx.strokeStyle = "#3A404D"
                ctx.beginPath(); ctx.moveTo(o, o + s); ctx.lineTo(o + s, o); ctx.stroke()
                const draw = (pts, color, width) => {
                    const f = editor.interpolator(pts)
                    ctx.strokeStyle = color
                    ctx.lineWidth = width
                    ctx.beginPath()
                    for (let i = 0; i <= 64; ++i) {
                        const x = i / 64
                        const p = area.toPx([x, f(x)])
                        if (i === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y)
                    }
                    ctx.stroke()
                }
                for (const ch of ["master", "red", "green", "blue"])
                    if (ch !== editor.channel && (editor.curves[ch] ?? []).length)
                        draw(editor.pointsOf(ch), Qt.alpha(editor.channelColors[ch], 0.35), 1)
                draw(editor.points, editor.channelColors[editor.channel], 1.6)
                ctx.fillStyle = editor.channelColors[editor.channel]
                for (const pt of editor.points) {
                    const p = area.toPx(pt)
                    ctx.beginPath(); ctx.arc(p.x, p.y, 4, 0, 2 * Math.PI); ctx.fill()
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            preventStealing: true
            property int dragIndex: -1
            function nearest(x, y) {
                let best = -1, bestDist = 10
                const pts = editor.points
                for (let i = 0; i < pts.length; ++i) {
                    const p = area.toPx(pts[i])
                    const d = Math.hypot(p.x - x, p.y - y)
                    if (d < bestDist) { bestDist = d; best = i }
                }
                return best
            }
            onPressed: (m) => {
                let pts = editor.points.map(p => [p[0], p[1]])
                dragIndex = nearest(m.x, m.y)
                if (dragIndex < 0) {
                    const p = area.fromPx(m.x, m.y)
                    pts.push(p)
                    pts.sort((a, b) => a[0] - b[0])
                    dragIndex = pts.findIndex(q => q[0] === p[0] && q[1] === p[1])
                }
                editor.work = pts
                editor.curveEdited(editor.channel, pts)
            }
            onPositionChanged: (m) => {
                if (dragIndex < 0 || !editor.work)
                    return
                const pts = editor.work.map(p => [p[0], p[1]])
                const p = area.fromPx(m.x, m.y)
                const last = pts.length - 1
                if (dragIndex === 0 && pts[0][0] <= 1e-4) p[0] = 0
                else if (dragIndex === last && pts[last][0] >= 1 - 1e-4) p[0] = 1
                else {
                    const lo = dragIndex > 0 ? pts[dragIndex - 1][0] + 0.01 : 0
                    const hi = dragIndex < last ? pts[dragIndex + 1][0] - 0.01 : 1
                    p[0] = Math.max(lo, Math.min(hi, p[0]))
                }
                pts[dragIndex] = p
                editor.work = pts
                editor.curveEdited(editor.channel, pts)
            }
            onReleased: {
                dragIndex = -1
                editor.work = null
                editor.released()
            }
            onDoubleClicked: (m) => {
                const i = nearest(m.x, m.y)
                const pts = editor.pointsOf(editor.channel).map(p => [p[0], p[1]])
                if (i > 0 && i < pts.length - 1) {
                    pts.splice(i, 1)
                    editor.curveEdited(editor.channel, pts)
                    editor.released()
                }
            }
        }
    }
}
