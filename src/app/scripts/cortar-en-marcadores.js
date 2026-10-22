// @name Cortar en los marcadores
// @description Divide todas las pistas desbloqueadas en cada marcador (por ejemplo, en los pulsos de la música).

const markers = movix.markers()
if (markers.length === 0)
    throw new Error("No hay marcadores en la línea de tiempo.")
let cuts = 0
for (const m of markers)
    cuts += movix.splitAll(m.frame).length
movix.log("Cortes: " + cuts)
