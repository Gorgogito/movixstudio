// @name Bajar la música de fondo
// @description Deja al 30 % el volumen de los clips de las pistas de audio (la música) sin tocar el sonido de los videos.

let changed = 0
for (const t of movix.tracks()) {
    if (t.kind !== "audio" || t.locked)
        continue
    for (const c of t.clips)
        if (movix.set(c.id, "volume", 0.3))
            ++changed
}
movix.log("Clips: " + changed)
