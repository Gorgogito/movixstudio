# MovixStudio

Editor de video de escritorio **libre y de código abierto** (GPLv3): sin suscripciones, sin marcas
de agua y con herramientas de IA que se ejecutarán en el equipo del usuario, sin servicios de pago.

> **Estado: fase F6 en curso** — instalador MSI, ZIP portable y actualizaciones dentro de la
> aplicación, sobre la IA visual de F5 y la de audio y texto de F4, con aceleración por GPU 100 %
> libre (Vulkan/WebGPU). Consulta la [hoja de ruta](docs/ROADMAP.md).

**Novedades de F6 (en curso)**
- Instalador MSI en español (menú Inicio, escritorio, proyectos `.mxproj` asociados, actualización
  sobre la versión anterior) y ZIP portable; ningún componente propietario en la distribución.
- Búsqueda de versiones nuevas al iniciar (desactivable) o desde Ayuda: con el instalador, descarga
  la actualización, comprueba su SHA-256 y la instala.
- Intercambio de la línea de tiempo con otros editores (DaVinci Resolve, Premiere, Kdenlive…) en
  formato **OpenTimelineIO** (`.otio`): importar y exportar pistas, clips, transiciones, velocidad,
  títulos, compuestos y marcadores.
- **Multicámara**: varias cámaras (y un micrófono aparte) sincronizadas automáticamente por el
  audio; vista de todos los ángulos en cuadrícula y corte en directo con las teclas 1–9.

**Novedades de F5** — en local, en la GPU cuando la hay (cualquier GPU con Vulkan)
- Quitar el fondo sin pantalla verde (personas o cualquier sujeto) o recortar un objeto con un
  clic sobre el visor (SAM 2), seguido a lo largo del clip.
- Seguir el movimiento de una máscara y mover con ella títulos o imágenes.
- Reencuadre automático a vertical (9:16), cuadrado o 4:5 siguiendo al sujeto.
- Aumentar la resolución ×2/×4 (Real-ESRGAN) y cámara lenta suave ×2/×4 (RIFE).
- Transcripción con Whisper en la GPU por Vulkan (~4× más rápida).

**Novedades de F4** — todo se ejecuta en el equipo; el audio no sale de él
- Subtítulos automáticos con Whisper (Tiny, Base, Small o Large v3 Turbo) en una pista editable;
  importar, exportar y **traducir** subtítulos (español ↔ inglés, Opus-MT).
- Pestaña Texto: la transcripción de la línea de tiempo; borrar palabras corta el video.
- Eliminar silencios (Silero VAD) y **reducción de ruido** por clip (RNNoise) con intensidad.
- **Texto a voz** con Kokoro (5 voces, español e inglés) y **separación de voz y música** (HTDemucs).
- **Marcar los pulsos** de la música como marcadores y **dividir un clip en escenas**.
- Gestor de modelos con descarga verificada (SHA-256); el trabajo pesado corre en un proceso aparte.

**Novedades de F3**
- Pestaña Color en el inspector: ajustes básicos, ruedas de sombras/medios/luces, curvas general y
  RGB, HSL por 8 gamas de color y LUT `.cube` con intensidad, todo con keyframes. Copiar y pegar la
  corrección. Material de cámara (S-Log3, V-Log, LogC…) convertido con OpenColorIO (ACES).
- Pestaña Efectos: clave de croma con cuentagotas sobre el visor y hasta 8 máscaras por clip
  (rectángulo, elipse, lineal y forma libre) que se suman, restan o intersecan, con keyframes.
- 14 modos de fusión (multiplicar, trama, superponer, luz suave…) y recorte animable por borde.
- Clips compuestos (Ctrl+G): agrupan clips en una secuencia que se abre con doble clic.
- HDR: proyectos HDR10 (PQ) o HLG en Rec.2020, material de 10 bits y exportación de 10 bits con
  metadatos HDR10; el visor muestra la conversión a SDR.
- Scopes junto al visor: forma de onda, desfile RGB, vectorscopio e histograma en tiempo real.
- Todo se calcula en la GPU con el mismo compositor en la vista previa y en la exportación.

**Novedades de F2**
- Transformación por clip (posición, escala, rotación, opacidad) con keyframes (lineal, mantener,
  suave), editable en el inspector o arrastrando el recuadro sobre el visor.
- Títulos con fuente, tamaño, color, negrita/cursiva, alineación, contorno, sombra y fondo, a partir
  de cuatro plantillas; imágenes fijas (PNG/JPEG/WebP… con transparencia).
- Transiciones en los cortes: disolver, fundido a negro, barridos, empujes y deslizamiento (con
  fundido cruzado de audio de potencia constante).
- Velocidad por clip de 0,25× a 8× con el audio conservando el tono; fundidos de video y de audio;
  volumen animable por clip y volumen por pista; separar el audio de un clip de video.
- Modo inserción, copiar/cortar/pegar en el cabezal.
- Exportación en cadena de tres hilos (decodificar → componer → codificar): ~94 fps en 1080p60 con NVENC.

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
| Crear / deshacer clip compuesto | `Ctrl+G` / `Ctrl+Mayús+G` | Abrir clip compuesto | doble clic |

## Arquitectura

```
src/
  media/     movix_media     FFmpeg: sondeo, decodificación (D3D11VA/CPU), codificación, color
  core/      movix_core      modelo del proyecto, ediciones, deshacer/rehacer, .mxproj, evaluación
  color/     movix_color     matemática del color (corrección, curvas, HSL, LUT .cube), croma, fusión, scopes
  engine/    movix_engine    lectores por clip, composición de capas por fotograma, mezcla de audio
  render/    movix_render    compositor GPU (Qt RHI) compartido por visor y exportación
  export/    movix_export    exportación (compositor fuera de pantalla + mezcla + codificador)
  playback/  movix_playback  reproductor de la línea de tiempo y salida de audio (miniaudio)
  ai/        movix_ai        modelos de IA (catálogo, descarga), cliente del proceso de IA, Whisper, VAD,
                             Kokoro, Opus-MT, HTDemucs, MODNet, BiRefNet, SAM 2, Real-ESRGAN, RIFE
                             (ONNX Runtime, CPU o GPU por WebGPU), seguimiento, ritmo y escenas
  aiworker/  movix-ai-worker proceso que ejecuta los trabajos de IA (JSON por stdin/stdout)
  app/       MovixStudio     interfaz Qt Quick, modelos, miniaturas, formas de onda, controladores
tests/       Catch2 + medios sintéticos generados con FFmpeg
i18n/        traducciones (el español es el idioma fuente)
```

El núcleo no depende de la interfaz: cada edición es una función pura sobre el proyecto, el
historial guarda instantáneas (deshacer exacto por construcción) y el reproductor y la exportación
trabajan sobre instantáneas inmutables.

Tecnologías: C++20 · Qt 6.8 (Qt Quick, RHI) · FFmpeg 9 · OpenColorIO 2.6 · whisper.cpp 1.8 (ggml, CPU y Vulkan) · ONNX Runtime 1.31 (CPU y WebGPU/Dawn) · eSpeak NG 1.52 · SentencePiece · miniaudio · CMake + Ninja · vcpkg · Catch2.

## Compilar (Windows)

Requisitos: Visual Studio 2022/2026 Build Tools con C++, CMake ≥ 3.25, Qt 6.8 (MSVC 64 bits, con
`qtshadertools`), FFmpeg *shared* con headers (p. ej. `ffmpeg-n9.0-latest-win64-gpl-shared` de BtbN),
vcpkg y el ejecutable `ffmpeg` (para generar los medios de prueba). Las rutas están en
`CMakePresets.json` (`VCPKG_ROOT`, `QT_ROOT`, `FFMPEG_ROOT`).

```powershell
./scripts/build.ps1                          # Debug
./scripts/build.ps1 -Preset release -Test    # Release + pruebas
./scripts/run.ps1 -Preset release -- "C:\ruta\proyecto.mxproj"

# Carpeta distribuible autocontenida (ejecutables, Qt, FFmpeg, IA y licencias):
cmake --install build/release --prefix "$PWD/dist/MovixStudio"

# Instalador MSI y ZIP portable (build/release/MovixStudio-<versión>-win64.*):
./scripts/build.ps1 -Preset release -Package
```

El MSI necesita [WiX](https://wixtoolset.org) 4 o posterior y su extensión de interfaz
(`dotnet tool install --global wix`, `wix extension add --global WixToolset.UI.wixext/<versión>`).
Las DLL que hacen falta se resuelven al instalar (una que falte detiene la instalación) y la
biblioteca de ejecución de MSVC va junto al programa. La dirección de la que se leen las versiones
nuevas se cambia con `-DMOVIX_UPDATE_URL=...` (formato de la API de versiones de GitHub; vacía = sin
actualizaciones) o, para pruebas, con la variable de entorno del mismo nombre.

Diagnóstico de la reproducción: `$env:QT_LOGGING_RULES = "movix.player.stats.debug=true"; $env:QT_FORCE_STDERR_LOGGING = "1"`.

## Pruebas

`ctest --preset debug` genera los medios sintéticos, descarga una vez los modelos de prueba
(Whisper Base, Silero VAD, RNNoise, Kokoro, Opus-MT, HTDemucs, MODNet, BiRefNet, SAM 2, Real-ESRGAN y
RIFE, ~1,3 GB verificados con SHA-256, en `build/test-models`) y ejecuta 102 casos
(Debug y Release). Además de los de F0–F2, los de F3 comparan la GPU con una referencia en la CPU
(corrección de color, croma, cada modo de fusión, máscaras, conversiones HDR) y comprueban LUT
`.cube`, curvas, scopes, OCIO, clips compuestos y la exportación HDR10; los de F4 transcriben voz
real (tiempos por palabra dentro de ±0,15 s), detectan sus tramos de voz, usan el proceso de IA
(cola, cancelación, errores) y verifican subtítulos, SRT, silencios y cortes por texto:

| Área | Qué se verifica |
|---|---|
| Medios | Sondeo; 180/180 fotogramas con timestamps exactos; búsqueda idéntica al bit; GPU ≡ CPU; HEVC 10 bits, VP9; remuestreo y búsqueda de audio exactos; sincronía A/V < 5 ms |
| Núcleo | Conversión fotograma↔tiempo (también 29,97); cada edición y sus límites; cambio de fps; 3000 ediciones aleatorias con invariantes y deshacer/rehacer exactos; ida y vuelta del `.mxproj`, rutas relativas y archivos dañados; ida y vuelta de OTIO y un `.otio` escrito por la biblioteca oficial (23,976 fps, código de tiempo, audio separado) |
| Motor | Fotograma de fuente correcto por capa y decodificación en orden; mezcla exacta a la muestra, volumen, suma y silenciado |
| Compositor | Colores BT.709, ajuste de aspecto con bandas, apilado de capas y orientación (render fuera de pantalla) |
| Exportación | Fidelidad frente a la fuente (PSNR), sincronía A/V tras exportar, cada códec disponible, cambio de resolución/aspecto, cancelación |
| Reproductor | Tiempo real, fotograma correcto, pausa/búsqueda/paso, 60 fps a 60 Hz sin descartes, varios clips y huecos, edición en plena reproducción, solo audio, proyecto vacío |
| Modelos de la UI | 600 ediciones aleatorias validadas con `QAbstractItemModelTester` |
| IA (F4) | Whisper sobre voz sintetizada (idioma, palabras, tiempos), Silero VAD, proceso de IA con cola/cancelación/caída, descarga multiarchivo verificada, subtítulos y SRT, silencios y cortes por tramos, RNNoise (−15 dB de ruido sin desfase), ritmo a 92/120/150 BPM (±35 ms), cortes de escena exactos, Kokoro → Whisper (ida y vuelta), traducción es↔en, HTDemucs (voz/percusión separadas, correlación > 0,8), integración con los controladores reales |
| IA visual (F5) | BiRefNet y SAM 2 separan sujeto/fondo (1,0 / 0,0) y lo siguen por el clip; mate en el motor (invertible, en el `.mxproj`); rastreador con error < 1 px; Real-ESRGAN recupera la nitidez del original; RIFE crea una sola bola nítida en la posición intermedia (±6 px); reencuadre, escalado y cámara lenta de punta a punta con los controladores |
| Multicámara (F6) | Sincronía por audio ±1 ms (sintética) y ±10 ms con dos cámaras AAC y un micrófono reales; sin falsos positivos con grabaciones ajenas o mudas; cambio de ángulo, audio fijo o que sigue al video, romper, `.mxproj` y OTIO; la cuadrícula en la GPU coincide celda a celda con cada ángulo; controlador real de punta a punta |

## Licencia

GPL-3.0-or-later. Ver [LICENSE](LICENSE). Los componentes de terceros que se distribuyen, sus
licencias y dónde obtener su código están en [packaging/AVISOS_DE_TERCEROS.txt](packaging/AVISOS_DE_TERCEROS.txt);
la instalación copia el texto de cada licencia en `licenses/`. Catch2 (BSL-1.0) solo se usa en las pruebas.

MovixStudio no está afiliado a Wondershare ni a Filmora; no usa su código, marca ni recursos.
