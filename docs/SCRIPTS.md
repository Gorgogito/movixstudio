# Scripts de MovixStudio

Los scripts automatizan tareas de edición con JavaScript. Están en el menú **Scripts**: los que
trae MovixStudio y los tuyos, que se guardan en la carpeta que abre **Scripts → Abrir la carpeta de
scripts** (`%APPDATA%\MovixStudio\MovixStudio\scripts`). Un archivo `.js` nuevo aparece en el menú la
próxima vez que lo abras.

- Todo lo que hace un script es **un solo paso de deshacer** (Ctrl+Z lo revierte entero).
- Si el script falla (un error o `throw new Error("…")`), **no cambia nada** y se muestra el error
  con su línea.
- Un script no puede leer ni escribir archivos ni usar la red: solo ve el proyecto a través de
  `movix`. Si tarda más de 10 s, se detiene.

## Cabecera

```js
// @name Nombre en el menú
// @description Lo que hace (aparece al pasar el ratón por encima)
```

## Unidades

Las posiciones y duraciones son **fotogramas** del proyecto. Para convertir:
`movix.secondsToFrames(s)` y `movix.framesToSeconds(f)`. Los ids son números.

## Consultas

| Función | Devuelve |
|---|---|
| `movix.info()` | `{width, height, fps, duration, playhead, selection}` (`selection`: ids de los clips elegidos) |
| `movix.tracks()` | Pistas de video (de abajo arriba) y de audio: `{id, kind: "video"/"audio", name, muted, hidden, locked, clips}` |
| `movix.clip(id)` | El clip o `null`: `{id, trackId, start, duration, end, mediaId, name, enabled, speed, sourceIn, isTitle, isCompound, volume, opacity}` |
| `movix.media()` | Medios del proyecto: `{id, name, path, duration (s), hasVideo, hasAudio}` |
| `movix.markers()` | `{frame, label}` |

## Ediciones

| Función | Hace |
|---|---|
| `movix.split(clipId, frame)` | Divide el clip; devuelve el id de la parte derecha (−1 si no se pudo) |
| `movix.splitAll(frame)` | Divide todas las pistas desbloqueadas; devuelve los ids nuevos |
| `movix.remove(ids, ripple)` | Elimina clips (un id o una lista); con `ripple`, cierra el hueco |
| `movix.move(ids, delta, trackOffset)` | Desplaza fotogramas y pistas |
| `movix.trim(clipId, "start"/"end", frame)` | Recorta un borde; devuelve dónde quedó |
| `movix.place(mediaId, trackId, frame, duración)` | Coloca un medio (duración 0 = entero) |
| `movix.addTitle(texto, frame, duración, trackId)` | Título (sin pista: la de arriba si está libre o una nueva) |
| `movix.addTrack("video"/"audio")` | Pista nueva; devuelve su id |
| `movix.addMarker(frame, etiqueta)` / `movix.removeMarker(frame)` | Marcadores |
| `movix.removeRanges([[desde, hasta], …])` | Quita tramos de todas las pistas desbloqueadas cerrando huecos |
| `movix.set(clipId, propiedad, valor)` | `volume`, `opacity`, `scale`, `x`, `y`, `rotation`, `speed`, `enabled` o `text` (títulos) |
| `movix.log(texto)` | Se muestra al terminar |

## Ejemplo

```js
// @name Silenciar los clips cortos
// @description Pone a cero el volumen de los clips de menos de un segundo.
const second = movix.secondsToFrames(1)
let n = 0
for (const t of movix.tracks())
    for (const c of t.clips)
        if (c.duration < second && movix.set(c.id, "volume", 0))
            ++n
movix.log(n + " clips")
```

Los scripts incluidos (en `src/app/scripts`) son buenos puntos de partida.
