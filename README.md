# MovixStudio

Editor de video de escritorio **libre y de código abierto** (GPLv3): sin suscripciones, sin marcas
de agua y con herramientas de IA que se ejecutarán en el equipo del usuario, sin servicios de pago.

> **Estado: fase F0 completada** — motor de medios y reproductor. La línea de tiempo y la edición
> llegan en la fase F1. Consulta la [hoja de ruta](docs/ROADMAP.md).

## Qué funciona hoy (F0)

- Apertura de medios mediante FFmpeg 9. Cubiertos por pruebas automáticas: MP4, MKV, WebM y WAV con
  H.264, HEVC 10 bits, VP9, AAC, Opus, FLAC y PCM. El resto de formatos que admite FFmpeg (MOV, AVI,
  MXF, AV1, ProRes, MP3…) debería funcionar por la misma vía, pero aún no tiene pruebas propias.
- Decodificación por GPU con **D3D11VA** (NVIDIA, AMD, Intel) y respaldo automático a CPU multihilo.
- Visor en GPU (Qt RHI / Direct3D 11) con conversión YUV→RGB por shader (BT.601/709/2020, rango
  limitado y completo) y relación de aspecto correcta.
- Reproducción con **el audio como reloj maestro**, sincronía A/V verificada (< 1 ms en los datos,
  fotograma correcto en el 100 % de las muestras), cadencia sin descartes a 60 fps / 60 Hz.
- Búsqueda precisa al fotograma, avance/retroceso fotograma a fotograma, arrastre de la barra de tiempo.
- Interfaz en **español (predeterminado) e inglés**, cambio de idioma en caliente.
- Atajos: `Espacio` reproducir/pausar · `K`/`L` pausa/reproducir · `←`/`→` fotograma · `Mayús+←/→` ±1 s ·
  `Inicio`/`Fin` · `M` silenciar · `I` panel de información · `Ctrl+O` abrir · doble clic pantalla completa.
- Arrastrar y soltar archivos sobre el visor.

## Arquitectura

```
src/
  media/     movix_media     FFmpeg: sondeo, decodificación de video (NV12) y audio (float), color
  playback/  movix_playback  hilos de decodificación, salida de audio (miniaudio), reloj A/V
  app/       MovixStudio     interfaz Qt Quick (QML), visor RHI, idioma
tests/       Catch2 + medios sintéticos generados con FFmpeg
i18n/        traducciones (el español es el idioma fuente)
```

Tecnologías: C++20 · Qt 6.8 (Qt Quick, RHI) · FFmpeg 9 (libav\*) · miniaudio · CMake + Ninja · vcpkg · Catch2.

## Compilar (Windows)

Requisitos: Visual Studio 2022/2026 Build Tools con C++, CMake ≥ 3.25, Qt 6.8 (MSVC 64 bits, con
`qtshadertools`), FFmpeg *shared* con headers (p. ej. `ffmpeg-n9.0-latest-win64-gpl-shared` de BtbN),
vcpkg y el ejecutable `ffmpeg` (para generar los medios de prueba).

Las rutas se configuran en `CMakePresets.json` (`VCPKG_ROOT`, `QT_ROOT`, `FFMPEG_ROOT`).

```powershell
./scripts/build.ps1                    # Debug
./scripts/build.ps1 -Preset release -Test   # Release + pruebas
./scripts/run.ps1 -Preset release -- "C:\ruta\video.mp4"

# Carpeta distribuible autocontenida (ejecutable + Qt + FFmpeg):
cmake --install build/release --prefix "$PWD/dist/MovixStudio"
```

Diagnóstico de la reproducción (fotogramas mostrados/descartados, huecos de render, saltos del reloj):

```powershell
$env:QT_LOGGING_RULES = "movix.player.stats.debug=true"; $env:QT_FORCE_STDERR_LOGGING = "1"
```

## Pruebas

`ctest --preset debug` genera primero los medios sintéticos (destellos y pitidos alineados para medir
la sincronía, 1080p60 con B-frames para la búsqueda, HEVC 10 bits, VP9/Opus, WAV mono) y después ejecuta:

| Área | Qué se verifica |
|---|---|
| Color | Matrices YUV→RGB con valores de referencia (negro, blanco, primarios) |
| Sondeo | Códecs, resolución, fps, duración, archivos sin video o inexistentes |
| Video | 180/180 fotogramas con timestamps exactos; búsqueda idéntica al bit a la decodificación secuencial; GPU ≡ CPU (PSNR > 45 dB); HEVC 10 bits y VP9; más rápido que tiempo real |
| Audio | Remuestreo 44,1 → 48 kHz sin pérdida de duración ni nivel; búsqueda exacta a la muestra; AAC/Opus/FLAC |
| Sincronía | Desfase entre destello de video y pitido de audio < 5 ms |
| Reproductor | Avance en tiempo real, fotograma correcto en cada instante, pausa, búsqueda, paso a paso, fin del medio, solo video, solo audio, errores, cadencia 60 fps a 60 Hz |

## Licencia

GPL-3.0-or-later. Ver [LICENSE](LICENSE). Componentes de terceros: Qt (LGPLv3), FFmpeg (GPL, compilado
con x264/x265), miniaudio (dominio público / MIT-0), Catch2 (BSL-1.0, solo pruebas).

MovixStudio no está afiliado a Wondershare ni a Filmora; no usa su código, marca ni recursos.
