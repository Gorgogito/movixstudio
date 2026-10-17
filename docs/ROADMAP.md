# Hoja de ruta de MovixStudio

Decisiones de base: GPLv3 · Windows primero (Linux/macOS en F6) · interfaz en español por defecto e
inglés · C++20 + Qt 6 (QML, RHI) + FFmpeg + miniaudio · IA local en un proceso separado (ONNX Runtime,
whisper.cpp) · cada módulo se entrega probado; nada simulado se presenta como terminado.

## F0 — Fundaciones ✅

- [x] Repositorio, CMake + presets, vcpkg, licencia GPLv3, i18n (es/en)
- [x] Sondeo de medios y decodificación de video (D3D11VA + CPU) a NV12
- [x] Decodificación y remuestreo de audio con búsqueda exacta a la muestra
- [x] Salida de audio (miniaudio) con reloj de reproducción interpolado
- [x] Reproductor multihilo: audio como reloj maestro, reloj de pared sin audio, descarte de fotogramas tardíos,
      tolerancia de refresco (sin descartes a 60 fps / 60 Hz)
- [x] Visor en GPU (Qt RHI) con conversión YUV→RGB por shader
- [x] Interfaz: visor, barra de transporte, panel de información, menús, atajos, arrastrar y soltar
- [x] 22 casos de prueba automáticos, en Debug y Release (medios sintéticos generados por CTest)
- [x] Instalación autocontenida (`cmake --install`)

### Limitaciones conocidas (se abordan en F1)

- La decodificación por GPU copia cada fotograma a memoria del sistema; falta la ruta sin copia
  (textura D3D11 compartida con el visor). Aun así decodifica 1080p60 a más de 200 fps.
- El contenido de 10 bits se reduce a 8 bits (NV12) para mostrarse; sin mapeo de tonos HDR.
- La latencia del dispositivo de audio es una estimación (±10 ms).
- En la primera reproducción tras abrir un archivo pueden descartarse 1–2 fotogramas por atascos
  puntuales del render al cargar elementos de la interfaz.
- Los mensajes de error del motor de medios aún no son traducibles (solo español).
- La carpeta distribuible ocupa ~250 MB (sin depurar plugins de Qt ni reducir FFmpeg).

## F1 — Editor mínimo funcional

- Modelo de proyecto (JSON versionado), autosave y recuperación
- Biblioteca de medios con miniaturas y formas de onda
- Línea de tiempo multipista: dividir, recortar, ripple, *snapping*, mover entre pistas
- Deshacer/rehacer basado en comandos
- Compositor GPU de la línea de tiempo (varias capas) y ruta de decodificación sin copia
- Exportación configurable (resolución, fps, calidad, códec: H.264/HEVC/AV1 por CPU o NVENC/AMF/QSV)

## F2 — Edición completa

Transformaciones y opacidad con keyframes, transiciones, velocidad y *speed ramping*, títulos y texto
animado, mezcla de audio básica, atajos configurables, proxies automáticos.

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
