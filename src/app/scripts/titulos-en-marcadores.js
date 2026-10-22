// @name Títulos en los marcadores
// @description Pone un título de 3 segundos con el texto de cada marcador que tenga nombre.

const length = movix.secondsToFrames(3)
let added = 0
for (const m of movix.markers()) {
    if (m.label === "")
        continue
    if (movix.addTitle(m.label, m.frame, length) > 0)
        ++added
}
if (added === 0)
    throw new Error("Ningún marcador tiene nombre.")
movix.log("Títulos: " + added)
