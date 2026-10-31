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
  lineal); los modos de fusión dentro de un compuesto suponen un fondo opaco. *(Luz lineal y
  fusiones en compuestos: resuelto en F6.)*
- Los puntos de las máscaras de forma libre no se animan (sí su posición, tamaño, giro y borde).
- Deshacer un clip compuesto exige velocidad 1× y sobrescribe lo que haya en su sitio. *(Resuelto
  en F6.)*
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
  DirectML incluye un redistribuible propietario de Microsoft. *(Resuelto en F5 sin componentes propietarios:
  Whisper con Vulkan y los modelos ONNX con WebGPU. Kokoro y Opus-MT siguen en la CPU porque en la
  GPU van más lentos; ver F6.)*
- El inglés de Kokoro se fonetiza con eSpeak (el modelo original usa un léxico propio): suena algo
  menos natural que el español. La traducción usa búsqueda voraz (sin *beam search*). *(Las dos
  cosas, resueltas en F6: léxico de Kokoro y búsqueda en haz.)*
- La detección de escenas no reconoce los fundidos lentos; la de ritmo supone un tempo constante.
  *(Fundidos y disolvencias, y tempo variable: resuelto en F6.)*

## F5 — IA visual (local) ✅

- [x] GPU 100 % libre: whisper.cpp con Vulkan (cargador de Khronos, sin Vulkan SDK) y ONNX Runtime
      con el proveedor WebGPU (plugin oficial MIT; Dawn sobre Vulkan, sin `dxil.dll` propietario).
      Si un modelo no cabe en la GPU, la sesión se recarga en la CPU y repite (nunca falla por la GPU)
- [x] Mates de IA por clip: video en escala de grises cuya luma es la opacidad, decodificado al
      instante de la fuente y combinado con las máscaras (`.mxproj` v6); invertir o quitar
- [x] Quitar el fondo: MODNet (personas, en GPU: ~7 fps en 1080p, 3,7× la CPU) y BiRefNet lite
      (cualquier sujeto; en CPU porque supera el límite de búferes del proveedor WebGPU; en GPU
      desde F6)
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
  *(Resuelto en F6: los dos van en la GPU.)*
- La primera ejecución en la GPU tarda unos segundos más (el controlador compila los sombreadores).
- SAM 2 se propaga desde el cabezal hasta el final del clip (no hacia atrás) guiándose por la
  máscara anterior; con oclusiones largas puede perder el objeto. *(Hacia atrás: resuelto en F6.)*
- El seguimiento usa una región rectangular sin escala ni giro; el reencuadre solo desplaza en
  horizontal (pensado para pasar de un video horizontal a vertical, cuadrado o 4:5).
  *(Escala y giro, y reencuadre en los dos ejes: resuelto en F6.)*

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
      `.mxproj` v7 *(Desde la ampliación de más abajo: cualquier duración y deriva corregida; en
      OTIO, todos los ángulos.)*
- [x] Plantillas propias: de título (estilo, posición, duración y fundidos; se guardan desde las
      propiedades de un título y aparecen en la pestaña Títulos) y de proyecto (ajustes, pistas,
      títulos, marcadores y medios; Archivo → Nuevo desde plantilla). En la carpeta de datos
- [x] Scripts en JavaScript (QJSEngine) con una API `movix` para consultar y editar el proyecto
      ([docs/SCRIPTS.md](SCRIPTS.md)): menú Scripts con cinco incluidos y los de la carpeta del
      usuario. Un script es un paso de deshacer y, si falla, no cambia nada; sin acceso a archivos
      ni red y con un límite de 10 s
- [x] OpenFX: anfitrión propio (`src/ofx`, sobre las cabeceras oficiales 1.4, BSD-3) para plugins
      de efectos de terceros en los clips con imagen. Busca en `OFX_PLUGIN_PATH`,
      `%CommonProgramFiles%\OFX\Plugins` y la carpeta del usuario; describe cada plugin una vez y
      crea una instancia por aparición del clip. Suites de propiedades, efecto, parámetros, memoria,
      hilos y mensajes; imágenes RGBA de 8 bits o en coma flotante, fotograma completo, de abajo
      arriba como pide OFX; un fallo dentro del plugin no cierra el editor. Pestaña Efectos: añadir,
      activar, reordenar y quitar, con controles generados (números, casillas, listas, colores,
      vectores). `.mxproj` v8. Verificado con un plugin propio hecho con la biblioteca de apoyo
      oficial y con cuatro ejemplos del repositorio de OpenFX (API en C y biblioteca de apoyo)
- [ ] Linux y macOS

### Mejoras pendientes de fases anteriores (orden pedido)

- [x] Velocidad variable (keyframes de velocidad, integrada fotograma a fotograma) y clips al revés
      (también combinados). Video hacia atrás por bloques de 1 s guardados (sin buscar en cada
      fotograma); audio a velocidad variable o al revés leído por ventanas y remuestreado (cambia
      el tono, como una cinta). OTIO: al revés como velocidad negativa; la variable, como su media
- [x] Interpolación de los keyframes desde la interfaz: clic derecho en el rombo del keyframe
      (también en los de color): lineal, suave o mantener; la forma del icono la indica
- [x] Animaciones de texto: de entrada y de salida (fundido, deslizar desde cada lado, zoom y
      máquina de escribir, que dibuja los caracteres en su sitio) con su duración
- [x] Atajos configurables: Editar → Atajos de teclado (pulsar la combinación nueva; quita el
      atajo a la acción que lo usaba y avisa; restablecer uno o todos). Se guardan en la
      configuración y los menús muestran los actuales
- [x] Selección por recuadro (arrastrar en una zona vacía; Ctrl o Mayús añade) y edición de varios
      clips a la vez en el inspector: activos, opacidad, escala, volumen, velocidad, al revés,
      fundidos y pegar el color, cada cambio en un solo paso de deshacer
- [x] Proxies automáticos: los medios de más de 1080p se copian a 540p (H.264, una clave cada
      medio segundo) en segundo plano; la vista previa los usa y la exportación, el original.
      Se reconocen en otros proyectos, se crean o borran a mano desde el menú del medio y se
      desactivan en Ver → Usar proxies en la vista previa
- [x] Exportación más rápida: conversión a NV12 en la GPU (sin bajar RGBA) y decodificación de
      copia cero con D3D11VA en el mismo dispositivo que compone. 1080p60 con NVENC: de ~115 a
      ~190 fps (decodificar 1,1 · componer 3,1 · codificar 4,0 ms); el límite es el codificador.
      La vista previa todavía baja los fotogramas a memoria *(resuelto: copia cero en la vista
      previa, más abajo)*
- [x] Forma animada de las máscaras de forma libre: keyframes de todos los puntos a la vez
      (fila «Forma» del panel de efectos); mover los puntos en otro instante crea su keyframe y
      añadir o quitar un punto lo hace en todos los keyframes
- [x] Composición en luz lineal (ajuste del proyecto): fusión, opacidad y fundidos mezclan luz;
      el lienzo RGBA16F guarda luz y la copia final vuelve a la señal del proyecto. La corrección
      de color sigue trabajando sobre la señal
- [x] Monitor HDR (Ver → Monitor HDR): ventana aparte, o a pantalla completa en la pantalla
      elegida, con la misma imagen que el visor. Con el HDR de Windows activo usa una cadena de
      intercambio scRGB en coma flotante: nits absolutos, compresión por encima del pico de la
      pantalla y proyectos SDR con su blanco SDR. Sin HDR muestra la conversión a SDR. Dibuja en
      su propio hilo y dispositivo D3D11; solo en Windows
- [x] BiRefNet y HTDemucs en la GPU. Al cargar un modelo para la GPU se reescriben, leyendo el
      protobuf directamente, los nodos que el proveedor WebGPU no admite o ejecuta muy despacio:
      Split de más salidas que búferes por sombreador → Slice, Sum → Add y ConvTranspose 1D de
      núcleo grande (la STFT inversa de HTDemucs) → MatMul y suma con solapamiento. Mismo
      resultado que en la CPU. BiRefNet pasa a media precisión (115 MB en vez de 224 MB; mate
      igual a ≤ 0,03/255 de media en fotos reales) para caber en 8 GB: ~1,1 s por fotograma
      1024² (CPU ~9,5 s). HTDemucs: ~0,5 s por ventana de 7,8 s (CPU ~3 s). Si un nodo aún falla
      en la GPU, solo ese nodo pasa a la CPU (se recuerda en `<modelo>.cpu-nodes`)
- [x] Seguimiento con escala y giro (IA → «Seguir el movimiento con escala y giro»): la plantilla
      se muestrea con una transformación de semejanza y se ajusta por descenso de coordenadas,
      anclada a la del primer fotograma para que el error no se acumule. La máscara crece y gira
      con el objeto, y los clips adjuntos conservan su separación, escalada y girada con él. En una
      tarjeta que avanza, crece ×1,8 y gira 69°: error ≤ 1,2 px, 0,6 % y 0,3°
- [x] SAM 2 hacia atrás: el clic recorta el objeto en todo el clip. Desde el fotograma del clic se
      segmenta hacia el principio por tramos de 1 s decodificados y recorridos al revés (las
      máscaras se guardan comprimidas a la resolución de SAM) y luego hacia el final
- [x] Escenas con disolvencias y fundidos: además de los cortes, se buscan tramos de 1/3 a 2,7 s
      en los que la imagen reducida pasa en línea recta de un plano al otro (una mezcla lo hace;
      el movimiento de cámara no). La disolvencia corta a mitad de la mezcla; el fundido a negro y
      la aparición, en el fotograma más uniforme del negro. Los cortes que caen dentro de una
      transición se descartan, y un fundido al principio o al final no separa escenas
- [x] Multicámara larga: las grabaciones de más de 10 minutos (o con desfases mayores) se
      sincronizan en dos pasos. Primero, uno grueso con la envolvente de inicios de sonido (cuatro
      bandas, 100 valores por segundo, cualquier desfase); después, uno fino a 8 kHz en tres
      puntos del solape, que mide la deriva entre los relojes de las grabadoras y la corrige con la
      velocidad del clip de ese ángulo. Cada archivo se decodifica una sola vez. Error: desfase
      ≤ 2 ms y deriva de ±5 ppm (20 min sintéticos, 100 ppm); con archivos de 13 min, desfase de
      11 min a 0,8 ms
- [x] Multicámara en OTIO: cada tramo es una pila con todos los ángulos en la que solo están
      activos el que se ve y el audio que suena, así que otros editores lo reproducen igual. Los
      metadatos guardan el ángulo y el audio, y al importar vuelve a ser una multicámara cuyos
      tramos comparten la secuencia. Comprobado con la biblioteca oficial (0.18.1)
- [x] Keyframes en los parámetros OpenFX: cada parámetro animable tiene su keyframe en el cabezal
      (con anterior/siguiente e interpolación, como el resto); en los de varios componentes
      (colores, vectores) los keyframes van en los mismos fotogramas, y las casillas y listas
      saltan de un valor a otro. El plugin lee el valor en cualquier instante, también entre
      fotogramas, y la suite de keyframes (número, instantes, índice, derivada e integral) responde
      con los del proyecto. Recortar el clip los mantiene en su instante. `.mxproj` v9 (los
      parámetros fijos siguen siendo números)
- [x] OpenFX en 10 bits y HDR: el video de más de 8 bits o con señal HDR pasa al plugin en coma
      flotante, convertido con la misma matriz que usa la GPU, y vuelve como RGBA de 16 bits en coma
      flotante que conserva su señal (PQ/HLG, Rec.2020). Con un efecto neutro, un clip HDR10 se ve
      igual que sin él (diferencia ≤ 0,0005); en 10 bits, el 60 % de los valores no caen en la
      rejilla de 8 bits. Si el plugin solo admite 8 bits, la imagen pasa por 8 bits pero no pierde
      la señal
- [x] Copia cero en la vista previa: el reproductor decodifica en el dispositivo Direct3D 11 de
      la ventana y los fotogramas no bajan a memoria. Componer 1080p60 pasa de ~240 a ~580 fps;
      reproduciendo, la aplicación usa ~30 % menos CPU. Lo que necesita los píxeles en la CPU
      (el cuentagotas de la clave) baja solo ese fotograma; con el monitor HDR abierto (otro
      dispositivo) y en los clips con efectos OpenFX se sigue usando la memoria.
      `MOVIX_NO_ZERO_COPY` la desactiva
- [x] Deshacer un clip compuesto a cualquier velocidad: cada clip recibe la velocidad (también
      variable) y la dirección con que lo mostraba el compuesto, y sus keyframes, fundidos y
      transiciones se recolocan con él; fotograma a fotograma se ve lo mismo que antes. Nada se
      sobrescribe: las pistas de video del compuesto siguen juntas y en orden sobre la suya (se
      intercala una pista nueva donde la de encima tiene algo en ese tramo) y el audio va a la
      primera pista libre
- [x] Fusiones dentro de un compuesto con fondo transparente: como en el W3C, donde no hay nada
      debajo la capa se ve tal cual (antes se mezclaba con negro)
- [x] Reencuadre en los dos ejes: se localizan el centro y el tamaño del sujeto, y el encuadre lo
      sigue en horizontal y en vertical; nuevo formato horizontal 16:9 (para videos verticales) y,
      opcional, acercarse (hasta ×2, con cambios lentos) cuando el sujeto ocupa poco
- [x] Pulsos con tempo variable: el tempo se estima en ventanas de 8 s cada segundo y se elige el
      recorrido más probable entre ellas (Viterbi, que castiga los saltos de octava); la
      programación dinámica sigue el periodo de cada instante. Con tempo constante se usa el
      global, como antes. Percusión sintética: accelerando de 90 a 150 BPM (79 de 79 pulsos a
      menos de 50 ms), ritardando de 140 a 100 (60 de 60) y salto de 100 a 128 (57 de 58)
- [x] Traducción con búsqueda en haz (4 hipótesis, como la configuración de Opus-MT): el
      decodificador evalúa las hipótesis en un mismo lote y se elige la de mayor log-probabilidad
      media. En frases de prueba, 4 de 6 traducciones más probables que con la voraz y ninguna
      peor («Hace un frío que pela»: «It's cold and cold» → «It's freezing»), casi sin más tiempo
- [x] OpenFX en clips compuestos: la secuencia se compone en la GPU (fuera de pantalla, sobre fondo
      transparente) y el plugin la recibe como imagen con su transparencia, en coma flotante y en la
      señal del proyecto si es HDR. Con un efecto neutro se ve igual que sin él (≤ 1/255; HDR10,
      idéntico). Lo que no se ve dentro del compuesto sigue sin verse aunque el plugin cambie su
      color. Al deshacer el compuesto sus efectos se pierden, como su color y sus máscaras
- [x] Generadores OpenFX: un plugin con contexto de generador crea un clip propio, como un título
      (pestaña Títulos → Generadores OpenFX: se arrastra a la línea de tiempo o con doble clic en el
      cabezal). Sus parámetros, con keyframes, van en las propiedades del clip, y admite efectos,
      color, máscaras y transformación como cualquier clip. En SDR Rec.709, en coma flotante si el
      plugin la admite; en el archivo (`.mxproj` v10) y en OTIO (`GeneratorReference`)
- [x] Transiciones OpenFX: los plugins con contexto de transición aparecen junto a las propias
      (pestaña Transiciones y propiedades del clip, con sus parámetros fijos). Cada lado del corte
      se compone en memoria con todo lo suyo y el plugin recibe las dos imágenes y el progreso; si
      falta el plugin queda una disolvencia. En OTIO viaja el plugin con sus parámetros. Cada plugin
      se describe en cada papel que admite (filtro, generador o transición), con sus parámetros de
      ese contexto. Verificado con plugins propios y con los ejemplos oficiales Noise, Rectangle
      (generadores) y Cross Fade (transición)
- [x] Interfaz de los plugins OpenFX sobre el visor (overlays): el botón ◎ del efecto (o del
      generador) la muestra sobre su capa. El plugin dibuja con OpenGL en un contexto propio fuera de
      pantalla (WGL, sin depender del visor Direct3D) y recibe el ratón en las coordenadas de su
      imagen; los parámetros que cambia pasan al proyecto como una edición (un paso de deshacer por
      arrastre, con keyframe si el parámetro está animado), y lo que no usa pasa a mover el clip.
      Verificado con un plugin propio y con el ejemplo oficial Overlay
- [x] IA en la GPU para el resto de modelos: medido y descartado. Whisper ya usa Vulkan y los
      modelos de imagen y HTDemucs, WebGPU; Silero VAD es diminuto. Kokoro y Opus-MT son modelos
      pequeños en los que el coste de cada llamada a la GPU pesa más que el cálculo: sus versiones
      de 8 bits usan operadores enteros que WebGPU no tiene (576 y 476 nodos a la CPU) y van más
      lentas que en la CPU (traducir 6 frases: 3,4 s en la CPU, 5,6 s en la GPU; Kokoro: ~0,5 s por
      frase en la CPU, 0,6–1 s en la GPU). En fp32, Kokoro acierta en la GPU pero tarda el doble que
      en la CPU; en fp16 da NaN. Opus-MT en q4f16 (`MatMulNBits`) empata en la GPU con la CPU
      (en haz, 2,4 s; la voraz, 2,9 s en la GPU y 2,2 s en la CPU), y en la CPU es un 35 % más
      rápido que el de 8 bits: queda como posible mejora de la CPU, no de la GPU
- [x] Opus-MT en q4f16 (pesos de 4 bits, `MatMulNBits`) en lugar de la versión de 8 bits, en la
      CPU: traducir es un 35–45 % más rápido en los dos sentidos (6 frases de español a inglés: 3,9 s
      → 2,3 s en haz; 5 de inglés a español: 2,8 s → 1,5 s). Las traducciones son equivalentes,
      con diferencias de estilo en ambas direcciones («Hey, how you doing?» por «¿Cómo estás?»,
      «sin fallar» por «sin falta», pero «If I'd known you'd come» donde la de 8 bits decía «If I
      knew»). Los archivos pesan más (78 + 82 MB por sentido en vez de 53 + 60): quien tuviera la
      versión anterior la verá como no instalada y tendrá que descargarla de nuevo
- [x] Keyframes en los parámetros de las transiciones OpenFX: cuentan desde el primer fotograma de
      la transición (en el inspector, con el cabezal dentro de ella), se recorren con ‹ › y viajan
      en el proyecto y en OpenTimelineIO como los de los efectos
- [x] Teclado en la interfaz de los plugins sobre el visor: al hacer clic en ella toma el foco y las
      teclas van al plugin (con su símbolo de OFX y su texto) antes que a los atajos de la
      aplicación; las que no usa siguen siendo atajos. Cada pulsación es un paso de deshacer
- [x] Voz en inglés de Kokoro con su léxico: adaptación a C++ del de misaki, el fonetizador con el
      que se entrenó el modelo (90 000 palabras con sus fonemas, reglas de -s, -ed e -ing, siglas
      letra a letra, «the», «to», «a» o «I» según el contexto). eSpeak solo fonetiza lo que no está
      (números, nombres poco comunes), con las mismas equivalencias que misaki. Corrige lo que
      eSpeak hacía mal con el modelo: «I» sin acento (Whisper no lo oía), «the» ante vocal, ɹ
      dobles («editor», «victory»), palabras pegadas («in the»). Sin el etiquetador gramatical de
      misaki: «read», «live» o «record» usan siempre su pronunciación habitual. El léxico (6 MB) se
      descarga con el modelo; quien ya lo tenga solo baja esos dos archivos. En 8 frases leídas
      por cada voz inglesa, Whisper entiende las 79 palabras (con eSpeak, 78)
- [x] OpenTimelineIO con DaVinci Resolve 21.1 y Kdenlive 26.08 (30-10-2026), con medios reales:
      un montaje con corte, disolvencia, hueco, título, audio y marcador, abierto en los dos y vuelto
      a exportar por ellos. Resolve conserva todo menos el título (propio de MovixStudio: queda como
      clip sin medio); Kdenlive pierde además la disolvencia (su OTIO no tiene transiciones). Lo
      que exportan se importa entero en MovixStudio. Corregido en el camino: Kdenlive no encontraba
      los medios porque exportábamos URL `file:///` (ahora rutas absolutas, como escriben los dos) y
      mostraba los marcadores sin nombre (lee el comentario: ahora va en los dos campos); los
      efectos desactivados que Resolve pone en cada clip ya no dan aviso. Sus archivos quedan como
      pruebas (`tests/data/otio/resolve.otio` y `kdenlive.otio`)
- [x] OpenFX en la GPU: el anfitrión ofrece la *suite* de render por OpenGL de OFX 1.4. Los plugins
      que la admiten reciben sus imágenes como texturas en un contexto OpenGL propio de cada hilo y
      dibujan el resultado en la GPU (en 8 bits o en coma flotante, RGBA32F); si no hay OpenGL o el
      plugin falla con él, renderizan en la CPU. Verificado con un plugin propio escrito sobre la
      API en C: 1080p en 5,5 ms con OpenGL, contando subir la imagen y leer el resultado
- [x] Menos trabajo del anfitrión en cada efecto, para todos los plugins: el video de 8 bits pasa al
      plugin en 8 bits si lo admite (antes, en coma flotante siempre que pudiera, con dos
      conversiones por fotograma) y el plugin lee y escribe las imágenes del motor sin copias. Con
      un efecto en un clip 1080p, el anfitrión pasa de ~14 ms a ~4 ms por fotograma, más los ~4 ms
      de convertir NV12 a RGBA

### Limitaciones de OpenFX

- Sin los contextos de retoque, cambio de velocidad, lectura o escritura. La interfaz sobre el
  visor solo funciona en Windows (OpenGL por WGL).
- Los clips con efectos no usan la copia cero: la imagen baja a la memoria para el plugin y vuelve a
  subir al visor (Direct3D), también con los que renderizan con OpenGL. Entre dos efectos con
  OpenGL seguidos la imagen también pasa por la memoria. Las *suites* de CUDA, OpenCL y Metal de
  OFX 1.5 (las de muchos plugins comerciales) no están.

### Pendiente de comprobar

- MSI: la instalación real (accesos directos, asociación de `.mxproj`, entrada en «Aplicaciones
  instaladas») y la reinstalación sobre la misma versión se comprobaron en un equipo el 22-10-2026,
  y la actualización de la 0.1.0 a la 0.2.0 desde la propia aplicación el 26-10-2026 (aviso al
  iniciar, descarga verificada, cierre, instalador; una sola entrada instalada y los mismos 541
  archivos que el MSI, sin restos), y la desinstalación y reinstalación ese mismo día (quita la
  entrada, la carpeta, los accesos directos y la asociación; conserva los ajustes y los datos del
  usuario).
- Actualizar FFmpeg cuando la rama 9.0 incorpore las correcciones de `arnndn` de la rama principal
  (5474b1e05a: lee entradas de las GRU sin inicializar; 279f225d98: modelos con capas que no
  encajan). Con el modelo que usamos no se observa efecto (salida idéntica en cientos de pasadas).
  Comprobado de nuevo el 30-10-2026: la rama 9.0 aún no las tiene.

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
