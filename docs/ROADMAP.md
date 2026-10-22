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

## F2 — Edición completa ✅

- [x] Valores animables con keyframes (lineal, mantener, suave) que se mueven con el clip
- [x] Transformación (posición, escala, rotación, opacidad) en el compositor y sobre el visor
- [x] Fundidos de video y audio; volumen animable por clip y volumen por pista
- [x] Velocidad constante por clip (0,25×–8×), audio con tono conservado (atempo)
- [x] Transiciones entre clips contiguos: disolver, fundido a negro, barridos, empujes, deslizar
- [x] Títulos (QPainter) e imágenes fijas con transparencia (capas RGBA)
- [x] Separar audio, modo inserción, copiar/cortar/pegar
- [x] Formato `.mxproj` v2 (abre los v1)
- [x] Exportación en cadena de tres hilos
- [x] 62 casos de prueba en Debug y Release, prueba aleatoria con 18 operaciones

### Correcciones que salieron de las pruebas

- `setTransition` con clips de un fotograma llamaba a `std::clamp` con límites invertidos.
- El reloj de audio se suaviza (sigue el tiempo real y se corrige hacia el audio): con dispositivos
  de periodo largo o irregular el video avanzaba a saltos. Tras una búsqueda no informa de un
  instante anterior al pedido.
- La prueba de cadencia usa el dispositivo de audio real y una espera precisa (el virtual y
  `sleep_until` dependen de la resolución del temporizador de Windows).

### Pendiente (pasa a F3 o posterior)

- *Speed ramping* (velocidad variable) y reproducción inversa.
- Elegir la interpolación de un keyframe desde la interfaz (el núcleo y el archivo ya la admiten).
- Recorte (crop) como propiedad del clip; animaciones de texto predefinidas.
- Atajos configurables y proxies automáticos.
- Decodificación de copia cero y conversión RGBA→YUV en la GPU para exportar (hoy la composición
  y esa conversión limitan a ~94 fps en 1080p60).
- Selección por recuadro y edición de varios clips a la vez en el inspector.

## F3 — Color y composición ✅

- [x] Compositor con lienzo propio en coma flotante (RGBA16F) y pasadas adicionales para los modos
      de fusión que leen el fondo; sin modos de fusión (ni scopes) dibuja directamente en el
      destino, en una sola pasada. Misma ruta en el visor y en la exportación
- [x] Corrección de color por clip aplicada en la GPU como LUT 3D (33³) generada en la CPU:
      exposición, contraste, saturación, intensidad, temperatura, matiz, altas luces, sombras,
      blancos, negros; ruedas lift/gamma/gain; curvas general y RGB (monótonas); HSL en 8 gamas;
      LUT `.cube` 1D/3D con intensidad; copiar/pegar la corrección
- [x] Corrección de color animable: keyframes con la corrección completa (lineal, mantener, suave);
      entre dos keyframes se mezclan sus LUT, así que se animan también curvas, HSL y LUT
- [x] OpenColorIO 2.6 con el config de estudio de ACES incluido: 28 espacios de cámara (S-Log3,
      V-Log, LogC, C-Log…) llevados a la señal del proyecto con la vista estándar de su pantalla,
      dentro de la misma LUT del clip
- [x] 10 bits y HDR: decodificación P016, señales PQ y HLG (Rec.2020) detectadas y convertidas a la
      de trabajo del proyecto (SDR Rec.709, HDR10 o HLG; SDR a 203 nits; mapeo de tonos a SDR);
      visor HDR mostrado en SDR; exportación de 10 bits con la señalización y los metadatos HDR10
- [x] Clave de croma (distancia en Cb/Cr sobre la señal de la fuente, borde suave, supresión del
      derrame) con cuentagotas
- [x] Hasta 8 máscaras por clip (rectángulo, elipse, lineal y forma libre de Bézier) que se suman,
      restan o intersecan, con difuminado, inversión y keyframes; edición de puntos sobre el visor
- [x] 14 modos de fusión (fórmulas de W3C) y recorte animable
- [x] Clips compuestos: secuencias anidadas que se crean desde la selección, se abren para
      editarlas (ruta de navegación), se recortan, cambian de velocidad, admiten efectos y se
      pueden deshacer; vídeo compuesto en una textura transparente y audio mezclado recursivamente
- [x] Scopes: forma de onda, desfile RGB, vectorscopio (con objetivos y línea de piel) e histograma
- [x] Formato `.mxproj` v4 (abre v1–v3)
- [x] 91 casos de prueba en Debug y Release; la GPU coincide con la referencia en la CPU
      (corrección ≤ 1 nivel, cada modo de fusión ≤ 2, PQ ≤ 0,03 %), exportación HDR10 verificada al
      decodificarla; prueba aleatoria de 3000 ediciones que incluye compuestos, máscaras y color

### Correcciones que salieron de las pruebas

- Con una capa normal y otra con fusión en la misma pasada, la segunda leía un fondo sin copiar.
- La lectura de la GPU para los scopes se completa en el fotograma siguiente: en pausa no llegaba
  nunca; ahora el visor pide otro refresco solo mientras hay una lectura o una imagen nueva.
- El disco de las ruedas de color se genera en C++ (el `Canvas` de QML no mostraba los píxeles
  escritos con `putImageData`).
- Al arrastrar un punto de una máscara de forma libre, la lista de tiradores se recreaba y se
  perdía el arrastre.
- En proyectos HDR la clave de croma se calculaba después de convertir la señal y no coincidía
  con el color clave (tomado de la fuente).

### Limitaciones conocidas

- El visor muestra los proyectos HDR convertidos a SDR (no usa la salida HDR de Windows).
- La corrección, los modos de fusión y la clave trabajan sobre la señal codificada (no en luz
  lineal); los modos de fusión dentro de un compuesto suponen un fondo opaco.
- Los puntos de las máscaras de forma libre no se animan (sí su posición, tamaño, giro y borde).
- Deshacer un clip compuesto exige velocidad 1× y sobrescribe lo que haya en su sitio.
- Pendientes de F2 que siguen abiertos: *speed ramping*, interpolación de keyframes en la interfaz,
  animaciones de texto, atajos configurables, proxies, copia cero, selección por recuadro.

## F4 — IA de audio y texto (local) ✅

- [x] Proceso `movix-ai-worker` aparte (un fallo o falta de memoria no cierra el editor): JSON por
      líneas en stdin/stdout, cola de trabajos, progreso, cancelación y detección de caídas
- [x] Gestor de modelos: catálogo con tamaño, licencia y SHA-256; descarga verificada sobre la
      marcha (`.part` → renombrado solo si coincide); carpeta en `%LOCALAPPDATA%\MovixStudio\models`
- [x] whisper.cpp 1.8 con el backend de CPU de ggml en variantes (SSE4.2…AVX-512) elegidas en tiempo
      de ejecución (*overlay port* de vcpkg): AVX2 en el equipo de desarrollo, ~6× más rápido que la
      compilación genérica
- [x] Transcripción con marcas por palabra (DTW sobre las cabezas de alineamiento del modelo) y
      bordes ajustados a los tramos de voz de Silero VAD; idioma automático o elegido; se guarda en
      el medio (`.mxproj` v5) y se reutiliza
- [x] Subtítulos automáticos: palabras llevadas a la línea de tiempo a través de los clips
      (recortes, velocidad, pistas silenciadas, audio separado, compuestos), agrupadas por pausas,
      frases, duración y longitud, en líneas equilibradas; pista de títulos editable; SRT
      importar/exportar
- [x] Edición por texto: pestaña Texto con las palabras de la línea de tiempo; borrar palabras corta
      ese tramo en todas las pistas desbloqueadas cerrando el hueco (sincronía intacta)
- [x] Eliminar silencios con Silero VAD: silencio mínimo, margen y umbral configurables
- [x] Reducción de ruido por clip con RNNoise (filtro `arnndn` de FFmpeg) con intensidad ajustable,
      en reproducción y exportación, sin desfase (el retardo de 10 ms se compensa y cada búsqueda
      precalienta la red 0,3 s)
- [x] Detección de ritmo (flujo espectral por bandas, tempo por autocorrelación y pulsos por
      programación dinámica): marca los pulsos de la música como marcadores
- [x] Marcadores en la línea de tiempo (por secuencia, en el `.mxproj`, con imán, Ctrl+M, Alt+←/→)
- [x] Detección de escenas (luz, color y composición con umbral adaptativo): divide el clip en sus
      planos
- [x] Texto a voz con Kokoro 82M (ONNX) y eSpeak NG (fonemas): 3 voces en español y 2 en inglés
- [x] Traducción de subtítulos español ↔ inglés con Opus-MT (Marian en ONNX + SentencePiece)
- [x] Separación de voz y música con HTDemucs v4 (ONNX): dos pistas nuevas alineadas con el clip
- [x] ONNX Runtime 1.31 (CPU, MIT) descargado y verificado por CMake; eSpeak NG compilado con un
      *overlay port* de vcpkg

### Correcciones que salieron de las pruebas

- El ggml de vcpkg se compila sin AVX (`GGML_NATIVE=OFF`, todo apagado): la transcripción tardaba
  34 s para 10 s de audio. El *overlay port* compila todas las variantes y carga la mejor.
- Las marcas de tiempo por token de Whisper se desviaban hasta 1 s (palabras dentro de silencios);
  DTW + el ajuste a la voz las dejan a menos de 0,15 s.
- Un subtítulo de 84 caracteres no siempre cabe en 2×42 cortando por palabras: el corte de
  subtítulos comprueba el reparto real en líneas.
- `TapHandler` no da los modificadores del clic (Mayús+clic en la transcripción): se usa `MouseArea`.
- RNNoise retrasa su salida 480 muestras: sin compensarlo, la voz filtrada iba 10 ms tarde.
- El flujo espectral por bins daba mucho más peso a la caja que al bombo y el tempo salía a la
  mitad (150 → 75 BPM); por bandas logarítmicas, y sumando los retardos vecinos, sale el correcto.
- `setFrameRate` solo convertía las pistas de la secuencia activa: los clips compuestos quedaban con
  los fotogramas de la velocidad anterior.

### Limitaciones conocidas

- La IA se ejecuta en la CPU. La GPU (DirectML o CUDA) queda pendiente de decidir: el paquete de
  DirectML incluye un redistribuible propietario de Microsoft.
- El inglés de Kokoro se fonetiza con eSpeak (el modelo original usa un léxico propio): suena algo
  menos natural que el español. La traducción usa búsqueda voraz (sin *beam search*).
- La detección de escenas no reconoce los fundidos lentos; la de ritmo supone un tempo constante.

## F5 — IA visual (local) ✅

- [x] GPU 100 % libre: whisper.cpp con Vulkan (cargador de Khronos, sin Vulkan SDK) y ONNX Runtime
      con el proveedor WebGPU (plugin oficial MIT; Dawn sobre Vulkan, sin `dxil.dll` propietario).
      Si un modelo no cabe en la GPU, la sesión se recarga en la CPU y repite (nunca falla por la GPU)
- [x] Mates de IA por clip: video en escala de grises cuya luma es la opacidad, decodificado al
      instante de la fuente y combinado con las máscaras (`.mxproj` v6); invertir o quitar
- [x] Quitar el fondo: MODNet (personas, en GPU: ~7 fps en 1080p, 3,7× la CPU) y BiRefNet lite
      (cualquier sujeto; en CPU porque supera el límite de búferes del proveedor WebGPU)
- [x] Seleccionar un objeto con un clic (SAM 2.1 tiny, en GPU) y seguirlo por el clip
- [x] Seguimiento de movimiento: correlación cruzada normalizada sobre luma y croma con plantilla
      gaussiana adaptable; anima la máscara y mueve con ella los clips adjuntos (títulos, imágenes)
- [x] Reencuadre automático a 9:16, 1:1 o 4:5: cambia el formato del proyecto, escala para cubrir
      y anima la posición siguiendo al sujeto (MODNet o, sin personas, lo que destaca del fondo)
- [x] Aumentar la resolución ×2/×4 (Real-ESRGAN general x4v3, por teselas, en GPU) y cámara lenta
      suave ×2/×4 (RIFE 4 con paso de tiempo, en GPU); el audio original se conserva separado

### Correcciones que salieron de las pruebas

- Dawn no encontraba `vulkan-1.dll` (lo busca junto a su DLL) y caía a Direct3D 12: se copia el
  cargador de Vulkan de vcpkg junto a los ejecutables.
- Cerrar un proceso con sesiones de ONNX Runtime vivas en el plugin WebGPU fallaba
  (`0xc0000409`): el entorno y las sesiones no se destruyen al salir.
- El rastreador solo con luma perdía objetos que se distinguen del fondo por el color.
- La prueba del escalado medía PSNR, que favorece el desenfoque; ahora mide la nitidez recuperada.
- `frameToRgba` no admite fotogramas RGBA de entrada (la referencia bilineal de una prueba era basura).

### Limitaciones conocidas

- BiRefNet en la CPU tarda ~10 s por fotograma (1024 × 1024): para video, mejor «Personas» o el
  clic con SAM 2. HTDemucs sigue en la CPU (crear su sesión en la GPU cuesta más de lo que ahorra).
- La primera ejecución en la GPU tarda unos segundos más (el controlador compila los sombreadores).
- SAM 2 se propaga desde el cabezal hasta el final del clip (no hacia atrás) guiándose por la
  máscara anterior; con oclusiones largas puede perder el objeto.
- El seguimiento usa una región rectangular sin escala ni giro; el reencuadre solo desplaza en
  horizontal (pensado para pasar de un video horizontal a vertical, cuadrado o 4:5).

## F6 — Pulido y ecosistema (en curso)

Orden acordado: instalador y actualizaciones → OpenTimelineIO → multicámara → el resto.

- [x] Instalador MSI (WiX 4+, con CPack) en español: menú Inicio y escritorio, asociación de los
      `.mxproj`, icono y datos de versión en el ejecutable, actualización mayor sobre la versión
      anterior; y ZIP portable. Las DLL se resuelven al instalar (OpenColorIO arrastraba siete que
      faltaban en la carpeta instalada) y la biblioteca de ejecución de MSVC va junto al programa
- [x] Sin componentes propietarios ni sobrantes: fuera `dxil.dll`/`dxcompiler.dll`, el compilador
      de sombreadores que ya trae Windows, OpenGL por software, `vc_redist.x64.exe` y los
      complementos de depuración de QML (407 → 349 MB instalados; MSI de 109 MB)
- [x] Avisos de terceros y texto de cada licencia en `licenses/`, con dónde obtener el código de
      los componentes copyleft
- [x] Actualizaciones: consulta la versión más reciente (API de versiones de GitHub) al iniciar,
      como mucho una vez al día y desactivable, o desde Ayuda; con el MSI instalado descarga el
      instalador, comprueba su SHA-256 (resumen que publica GitHub) y lo abre al cerrar la
      aplicación; si no, ofrece la página de la versión. Se puede omitir una versión
- [x] OpenTimelineIO: importar y exportar la línea de tiempo (`.otio`, Archivo o arrastrando el
      archivo) con lector y escritor propios en `core/Otio` (vcpkg no tiene OTIO y el formato es
      JSON). Viajan pistas (ocultas/silenciadas), clips, huecos, transiciones, velocidad
      (`LinearTimeWarp`), títulos (`GeneratorReference`), compuestos (pilas anidadas) y marcadores.
      El audio de los clips de video sale en pistas de audio aparte y al importar se vuelve a unir
      al clip de video que coincide. Admite `Clip.1`/`Clip.2`, medios con código de tiempo, rutas
      relativas, colecciones y medios que faltan (se conservan para reconectarlos). Verificado con
      la biblioteca oficial (OpenTimelineIO 0.18 lee lo que exportamos y un archivo suyo se importa
      en las pruebas). Los efectos de imagen no tienen equivalente en OTIO y no se exportan
- [x] Multicámara: se eligen varias grabaciones en la biblioteca (Ctrl+clic) y se crea un clip
      multicámara sincronizado por el audio (GCC-PHAT a 8 kHz, desfase con precisión de
      submuestra; las que no se pueden sincronizar se alinean al inicio y se avisa) o alineado al
      inicio. Es un compuesto cuya secuencia tiene un ángulo por pista de video (y los micrófonos
      aparte en pistas de audio); el clip guarda el ángulo que se ve y el audio que suena (el del
      ángulo o uno fijo). Vista «Multicámara» del visor: todos los ángulos en cuadrícula (la compone
      el propio reproductor), rotulados, con el activo resaltado; las teclas 1–9 o un clic cortan
      al ángulo en el cabezal, también reproduciendo. Se ajusta la sincronía abriendo el clip; al
      romperlo queda el ángulo y el audio elegidos; en OTIO se exporta lo que se ve y se oye.
      `.mxproj` v7
- [ ] Plantillas propias, plugins/scripting, OpenFX
- [ ] Linux y macOS

### Pendiente de comprobar

- MSI: la instalación real (accesos directos, asociación de `.mxproj`, entrada en «Aplicaciones
  instaladas») y la reinstalación sobre la misma versión se comprobaron en un equipo el 22-10-2026;
  faltan la desinstalación y la actualización entre dos versiones distintas.
- Abrir en DaVinci Resolve / Kdenlive un `.otio` exportado (y al revés) con proyectos reales.
- Actualizar FFmpeg cuando la rama 9.0 incorpore las correcciones de `arnndn` de la rama principal
  (5474b1e05a: lee entradas de las GRU sin inicializar; 279f225d98: modelos con capas que no
  encajan). Con el modelo que usamos no se observa efecto (salida idéntica en cientos de pasadas).

### Corregido: cierres y salida errática de la reducción de ruido

`arnndn` (FFmpeg) consume bloques de 480 muestras, pero al recibir el fin de flujo se le entrega
el último bloque incompleto y lo lee entero: lee fuera del búfer de entrada. Eso daba NaN o salida
distinta en cada ejecución y, cuando el búfer acababa en el borde de una página, el cierre del
proceso (≈ 1 de cada 10 pruebas; también podía ocurrir al reproducir o exportar el final de un clip
con reducción de ruido). Se localizó con una traza de la pila en `tests/main.cpp` (instrucción del
filtro paso alto de `rnnoise_channel` sobre la entrada) y el código de FFmpeg. `Denoiser::flush`
ya no envía el fin de flujo: empuja silencio hasta recuperar toda la salida retenida. Prueba nueva
(12 pasadas idénticas al bit y sin NaN con una longitud que no es múltiplo de 480): fallaba 15 de
15 veces con el código anterior; las pruebas `[denoise]` pasan 40 de 40.
- Publicar la primera versión en https://github.com/Gorgogito/movixstudio/releases con el MSI
  adjunto (nombre `MovixStudio-<versión>-win64.msi`), para que la búsqueda de actualizaciones la encuentre.
