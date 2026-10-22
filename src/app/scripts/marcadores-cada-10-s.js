// @name Un marcador cada 10 segundos
// @description Añade un marcador cada 10 segundos a lo largo de la línea de tiempo (útil para revisar o cortar en partes iguales).

const step = movix.secondsToFrames(10)
const duration = movix.info().duration
let added = 0
for (let f = step; f < duration; f += step)
    if (movix.addMarker(f, movix.framesToSeconds(f) + " s"))
        ++added
movix.log("Marcadores: " + added)
