# MovixStudio

Editor de video de escritorio **libre y de código abierto** (GPLv3): sin suscripciones, sin marcas
de agua y con herramientas de IA que se ejecutarán en el equipo del usuario, sin servicios de pago.

> **Estado: fase F1 completada** — editor multipista básico con exportación. Consulta la
> [hoja de ruta](docs/ROADMAP.md) para lo que viene (efectos, títulos, color, IA local…).

## Qué se puede hacer hoy

**Proyecto**
- Crear, abrir y guardar proyectos (`.mxproj`, JSON legible y versionado). Si se mueve la carpeta
  del proyecto junto con sus medios, estos se encuentran por ruta relativa.
- Autoguardado cada 30 s y recuperación tras un cierre inesperado; aviso de cambios sin guardar.
- Ajustes de resolución y fotogramas/s (cambiar los fps conserva los instantes de los clips). Al
  añadir el primer clip, el proyecto adopta su resolución y velocidad.
- Deshacer/rehacer ilimitado (300 pasos) de todas las ediciones.

**Medios y línea de tiempo**
- Importar archivos (diálogo o arrastrando desde el explorador) en segundo plano, con miniaturas.
- Vista previa de cualquier medio en el visor sin tocar la línea de tiempo.
- Línea de tiempo multipista (video y audio), con pistas que se pueden añadir, eliminar, ocultar,
  silenciar y bloquear.
- Arrastrar medios a las pistas, mover clips (también entre pistas), recortar desde los bordes,
  dividir en el cabezal, eliminar con o sin cierre de hueco, activar/desactivar clips.
- Imán a bordes de clips y al cabezal, zoom (rueda + Ctrl) y desplazamiento, miniaturas en los clips
  de video y formas de onda en los de audio.
- Volumen por clip (en dB, hasta +6 dB). El audio de los clips de video suena desde su pista.

**Reproducción y exportación**
- Visor en GPU con la composición de todas las capas; audio de todas las pistas mezclado en tiempo
  real como reloj maestro; las ediciones se ven y se oyen al instante, incluso reproduciendo.
- Exportación a MP4/MOV/WebM/MKV con H.264, HEVC, AV1, VP9 o ProRes 422 HQ, por CPU o por GPU
  (NVENC/AMF/Quick Sync: solo se ofrecen los que funcionan en el equipo), resolución, fps y calidad
  configurables, con progreso, tiempo restante, etapa más lenta y cancelación.
- La exportación usa el mismo compositor que la vista previa: lo que se ve es lo que se exporta.

**Interfaz**: español (predeterminado) e inglés, cambio en caliente.

### Atajos de teclado

| Acción | Atajo | Acción | Atajo |
|---|---|---|---|
| Reproducir / pausa | `Espacio` (`K` pausa, `L` reproduce) | Dividir en el cabezal | `Ctrl+B` o `S` |
| Fotograma anterior / siguiente | `←` / `→` | Eliminar / cerrar hueco | `Supr` / `Mayús+Supr` |
| ±1 segundo | `Mayús+←` / `Mayús+→` | Seleccionar todo / nada | `Ctrl+A` / `Esc` |
| Inicio / final | `Inicio` / `Fin` | Deshacer / rehacer | `Ctrl+Z` / `Ctrl+Y` |
| Acercar / alejar / ajustar | `+` / `-` / `Mayús+Z` | Imán | `N` |
| Nuevo / abrir / guardar | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | Importar / exportar | `Ctrl+I` / `Ctrl+E` |

## Arquitectura

```
src/
  media/     movix_media     FFmpeg: sondeo, decodificación (D3D11VA/CPU), codificación, color
  core/      movix_core      modelo del proyecto, ediciones, deshacer/rehacer, .mxproj, evaluación
  engine/    movix_engine    lectores por clip, composición de capas por fotograma, mezcla de audio
  render/    movix_render    compositor GPU (Qt RHI) compartido por visor y exportación
  export/    movix_export    exportación (compositor fuera de pantalla + mezcla + codificador)
  playback/  movix_playback  reproductor de la línea de tiempo y salida de audio (miniaudio)
  app/       MovixStudio     interfaz Qt Quick, modelos, miniaturas, formas de onda, controladores
tests/       Catch2 + medios sintéticos generados con FFmpeg
i18n/        traducciones (el español es el idioma fuente)
```

El núcleo no depende de la interfaz: cada edición es una función pura sobre el proyecto, el
historial guarda instantáneas (deshacer exacto por construcción) y el reproductor y la exportación
trabajan sobre instantáneas inmutables.

Tecnologías: C++20 · Qt 6.8 (Qt Quick, RHI) · FFmpeg 9 · miniaudio · CMake + Ninja · vcpkg · Catch2.

## Compilar (Windows)

Requisitos: Visual Studio 2022/2026 Build Tools con C++, CMake ≥ 3.25, Qt 6.8 (MSVC 64 bits, con
`qtshadertools`), FFmpeg *shared* con headers (p. ej. `ffmpeg-n9.0-latest-win64-gpl-shared` de BtbN),
vcpkg y el ejecutable `ffmpeg` (para generar los medios de prueba). Las rutas están en
`CMakePresets.json` (`VCPKG_ROOT`, `QT_ROOT`, `FFMPEG_ROOT`).

```powershell
./scripts/build.ps1                          # Debug
./scripts/build.ps1 -Preset release -Test    # Release + pruebas
./scripts/run.ps1 -Preset release -- "C:\ruta\proyecto.mxproj"

# Carpeta distribuible autocontenida (ejecutable + Qt + FFmpeg):
cmake --install build/release --prefix "$PWD/dist/MovixStudio"
```

Diagnóstico de la reproducción: `$env:QT_LOGGING_RULES = "movix.player.stats.debug=true"; $env:QT_FORCE_STDERR_LOGGING = "1"`.

## Pruebas

`ctest --preset debug` genera los medios sintéticos y ejecuta 49 casos (Debug y Release):

| Área | Qué se verifica |
|---|---|
| Medios | Sondeo; 180/180 fotogramas con timestamps exactos; búsqueda idéntica al bit; GPU ≡ CPU; HEVC 10 bits, VP9; remuestreo y búsqueda de audio exactos; sincronía A/V < 5 ms |
| Núcleo | Conversión fotograma↔tiempo (también 29,97); cada edición y sus límites; cambio de fps; 3000 ediciones aleatorias con invariantes y deshacer/rehacer exactos; ida y vuelta del `.mxproj`, rutas relativas y archivos dañados |
| Motor | Fotograma de fuente correcto por capa y decodificación en orden; mezcla exacta a la muestra, volumen, suma y silenciado |
| Compositor | Colores BT.709, ajuste de aspecto con bandas, apilado de capas y orientación (render fuera de pantalla) |
| Exportación | Fidelidad frente a la fuente (PSNR), sincronía A/V tras exportar, cada códec disponible, cambio de resolución/aspecto, cancelación |
| Reproductor | Tiempo real, fotograma correcto, pausa/búsqueda/paso, 60 fps a 60 Hz sin descartes, varios clips y huecos, edición en plena reproducción, solo audio, proyecto vacío |
| Modelos de la UI | 600 ediciones aleatorias validadas con `QAbstractItemModelTester` |

## Licencia

GPL-3.0-or-later. Ver [LICENSE](LICENSE). Componentes de terceros: Qt (LGPLv3), FFmpeg (GPL, compilado
con x264/x265), miniaudio (dominio público / MIT-0), Catch2 (BSL-1.0, solo pruebas).

MovixStudio no está afiliado a Wondershare ni a Filmora; no usa su código, marca ni recursos.
