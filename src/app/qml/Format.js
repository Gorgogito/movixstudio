.pragma library

// Utilidades de formato para la interfaz.

function pad(n, width) {
    var s = String(n)
    while (s.length < width)
        s = "0" + s
    return s
}

// Código de tiempo HH:MM:SS:FF (FF = fotograma dentro del segundo).
function timecode(seconds, fps) {
    if (!isFinite(seconds) || seconds < 0)
        seconds = 0
    var rate = fps > 0 ? fps : 25
    var totalFrames = Math.floor(seconds * rate + 1e-6)
    var frameRate = Math.round(rate)
    var ff = totalFrames % frameRate
    var totalSeconds = Math.floor(totalFrames / frameRate)
    // Para velocidades no enteras (29,97…) se calcula desde los segundos reales.
    if (Math.abs(rate - frameRate) > 0.001) {
        totalSeconds = Math.floor(seconds + 1e-6)
        ff = Math.min(frameRate - 1, Math.floor((seconds - totalSeconds) * rate + 1e-6))
    }
    var h = Math.floor(totalSeconds / 3600)
    var m = Math.floor(totalSeconds / 60) % 60
    var s = totalSeconds % 60
    return pad(h, 2) + ":" + pad(m, 2) + ":" + pad(s, 2) + ":" + pad(ff, 2)
}

// Duración legible: 1:23:45 o 3:07.
function duration(seconds) {
    if (!isFinite(seconds) || seconds < 0)
        seconds = 0
    var t = Math.round(seconds)
    var h = Math.floor(t / 3600)
    var m = Math.floor(t / 60) % 60
    var s = t % 60
    return (h > 0 ? h + ":" + pad(m, 2) : String(m)) + ":" + pad(s, 2)
}

function bitRate(bps) {
    if (!bps || bps <= 0)
        return "—"
    if (bps >= 1e6)
        return (bps / 1e6).toFixed(1) + " Mb/s"
    return Math.round(bps / 1e3) + " kb/s"
}

function fileName(url) {
    var s = decodeURIComponent(String(url))
    var i = Math.max(s.lastIndexOf("/"), s.lastIndexOf("\\"))
    return i >= 0 ? s.substring(i + 1) : s
}
