// @name Quitar los huecos
// @description Elimina los tramos en que ninguna pista tiene nada (video y audio siguen sincronizados).

const clips = []
for (const t of movix.tracks())
    for (const c of t.clips)
        clips.push([c.start, c.end])
clips.sort((a, b) => a[0] - b[0])

// Tramos sin ningún clip entre el inicio y el final del último.
const gaps = []
let reached = 0
for (const [start, end] of clips) {
    if (start > reached)
        gaps.push([reached, start])
    reached = Math.max(reached, end)
}
const removed = movix.removeRanges(gaps)
movix.log("Fotogramas eliminados: " + removed)
