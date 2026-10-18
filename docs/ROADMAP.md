# Hoja de ruta de MovixStudio

Decisiones de base: GPLv3 · Windows primero (Linux/macOS en F6) · interfaz en español por defecto e
inglés · C++20 + Qt 6 (QML, RHI) + FFmpeg + miniaudio · IA local en un proceso separado (ONNX Runtime,
whisper.cpp) · cada módulo se entrega probado; nada simulado se presenta como terminado.

## F0 — Fundaciones ✅

Repositorio, compilación, decodificación de video (D3D11VA/CPU) y audio, salida de audio con reloj,
reproductor con sincronía A/V, visor en GPU, interfaz básica, i18n, pruebas, instalación.

## F1 — Editor mínimo funcional ✅

- [x] Modelo de proyecto, ediciones puras con invariantes, deshacer/rehacer por instantáneas
- [x] Archivo `.mxproj` versionado con rutas relativas, autoguardado y recuperación
- [x] Motor de línea de tiempo: lectores por clip, composición por fotograma, mezclador de audio
- [x] Compositor GPU único para visor y exportación (Qt RHI, también fuera de pantalla)
- [x] Codificador: H.264/HEVC/AV1/VP9/ProRes por CPU y NVENC/AMF/QSV, AAC/Opus/PCM, MP4/MOV/WebM/MKV
- [x] Reproductor de la línea de tiempo (sustituye al de F0) con ediciones en caliente
- [x] Interfaz de editor: biblioteca de medios, visor programa/medio, inspector, línea de tiempo
      multipista (mover, recortar, dividir, imán, zoom, pistas), exportación, ajustes del proyecto
- [x] 49 casos de prueba (Debug y Release) + verificación manual de la app con capturas

### Correcciones que salieron de las pruebas

- Pre-roll en la búsqueda de audio (sin transitorios al buscar) y reapertura cerca del inicio para
  no perder el primer bloque de un AAC (afectaba a la vista previa y a la exportación).
- Al ajustar el proyecto al primer clip, la posición de soltado se convierte de fps.

### Limitaciones conocidas (pasan a F2)

- La decodificación por GPU copia cada fotograma a memoria del sistema (sin ruta de copia cero).
- La exportación es secuencial (decodificar → componer → codificar): ~50 fps en 1080p60 en el
  equipo de desarrollo; paralelizar las etapas debería acercarse a ~120 fps.
- Las imágenes fijas no se pueden usar todavía en la línea de tiempo.
- Soltar o mover siempre sobrescribe (no hay modo de inserción con desplazamiento) y el audio de
  un clip de video no se puede separar a una pista de audio.
- Falta copiar/pegar, selección por recuadro y edición de varios clips a la vez en el inspector.
- El contenido de 10 bits se muestra a 8 bits; sin HDR.
- Los mensajes de error del motor de medios aún no son traducibles.
- La carpeta distribuible ocupa ~250 MB (sin depurar plugins de Qt ni reducir FFmpeg).

## F2 — Edición completa

Transformaciones (posición, escala, rotación, recorte) y opacidad con keyframes, transiciones,
velocidad y *speed ramping*, títulos y texto animado, fundidos y mezcla de audio por pista, imágenes
fijas, modo inserción y audio separado, copiar/pegar, atajos configurables, proxies automáticos,
decodificación de copia cero y exportación en paralelo.

## F3 — Color y composición

Corrección primaria, ruedas, curvas, HSL, LUT `.cube`, OpenColorIO, scopes, máscaras, croma,
modos de fusión, clips compuestos.

## F4 — IA de audio y texto (local)

Proceso `ai-worker`, gestor de modelos; subtítulos con Whisper (whisper.cpp), edición por texto,
eliminación de silencios (Silero VAD), reducción de ruido (DeepFilterNet/RNNoise), separación de voz
(Demucs), detección de beats y escenas, TTS (Kokoro), traducción (Opus-MT).

## F5 — IA visual (local)

Eliminación de fondo (BiRefNet/MODNet), máscaras y seguimiento con SAM 2, *motion tracking*,
encuadre automático, escalado (Real-ESRGAN), interpolación de fotogramas (RIFE).

## F6 — Pulido y ecosistema

Multicámara, plantillas propias, OpenTimelineIO, plugins/scripting, OpenFX, instalador y
actualizaciones, Linux y macOS.
