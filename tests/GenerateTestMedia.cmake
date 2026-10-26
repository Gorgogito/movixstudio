# Genera los medios sintéticos de prueba con el ejecutable de FFmpeg.
# Uso: cmake -DFFMPEG_EXE=<ffmpeg> -DOUT_DIR=<dir> -P GenerateTestMedia.cmake
#
# Todos los archivos tienen contenido conocido de antemano para que las pruebas puedan
# comprobar resultados exactos (número de fotogramas, timestamps, sincronía A/V).

file(MAKE_DIRECTORY "${OUT_DIR}")

function(make_media name)
    set(out "${OUT_DIR}/${name}")
    if(EXISTS "${out}")
        return()
    endif()
    message(STATUS "Generando ${name}")
    execute_process(
        COMMAND "${FFMPEG_EXE}" -hide_banner -loglevel error -y ${ARGN} "${out}.tmp${ext}"
        RESULT_VARIABLE res)
    if(NOT res EQUAL 0)
        file(REMOVE "${out}.tmp${ext}")
        message(FATAL_ERROR "FFmpeg falló generando ${name} (${res})")
    endif()
    file(RENAME "${out}.tmp${ext}" "${out}")
endfunction()

# 1) Sincronía A/V: 5 s a 720p30. Un fotograma blanco cada segundo exacto (n % 30 == 0)
#    y un pitido de 1 kHz de 100 ms que empieza en el mismo instante.
set(ext .mp4)
make_media(sync_720p30.mp4
    -f lavfi -i "color=c=black:s=1280x720:r=30:d=5,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='eq(mod(n\\,30)\\,0)',format=yuv420p"
    -f lavfi -i "aevalsrc='if(lt(mod(t\\,1)\\,0.1)\\,0.8*sin(2*PI*1000*t)\\,0)':s=48000:d=5"
    -c:v libx264 -preset veryfast -g 30 -bf 2 -c:a aac -b:a 128k -ac 2 -shortest)

# 2) Conteo y búsqueda: 3 s de 1080p60 H.264 con B-frames y GOP de 1 s (180 fotogramas).
make_media(count_1080p60.mp4
    -f lavfi -i "testsrc2=s=1920x1080:r=60:d=3,format=yuv420p"
    -c:v libx264 -preset veryfast -g 60 -bf 3)

# 3) HEVC 10 bits + FLAC a 44,1 kHz en Matroska (conversión de formato y remuestreo).
set(ext .mkv)
make_media(hevc10_360p.mkv
    -f lavfi -i "testsrc2=s=640x360:r=25:d=2,format=yuv420p10le"
    -f lavfi -i "sine=f=440:r=44100:d=2"
    -c:v libx265 -preset ultrafast -x265-params log-level=error -c:a flac -shortest)

# 3b) HDR10: HEVC de 10 bits marcado como PQ (SMPTE ST 2084) y Rec.2020, gris uniforme.
set(ext .mkv)
make_media(hdr_pq.mkv
    -f lavfi -i "color=c=0x9C9C9C:s=320x180:r=30:d=1,format=yuv420p10le"
    -c:v libx265 -preset ultrafast
    -x265-params "log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc)

# 4) VP9 + Opus en WebM.
set(ext .webm)
make_media(vp9_opus.webm
    -f lavfi -i "testsrc2=s=640x360:r=30:d=2,format=yuv420p"
    -f lavfi -i "sine=f=660:r=48000:d=2"
    -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 500k -c:a libopus -shortest)

# 6) Imagen con transparencia: mitad izquierda verde opaca, mitad derecha transparente.
set(ext .png)
make_media(half_alpha.png
    -f lavfi -i "color=c=black:s=320x180,format=rgba,geq=r=0:g=255:b=0:a='if(lt(X,160),255,0)'"
    -frames:v 1 -update 1)

# 5) Solo audio: 3 s de tono a 44,1 kHz mono en WAV.
set(ext .wav)
make_media(tone_44k_mono.wav
    -f lavfi -i "sine=f=440:r=44100:d=3" -c:a pcm_s16le)

# 7) Voz con ruido: la voz de prueba (tests/data) con ruido blanco constante, 48 kHz mono.
set(ext .wav)
make_media(speech_noisy.wav
    -i "${CMAKE_CURRENT_LIST_DIR}/data/speech_es.flac"
    -f lavfi -i "anoisesrc=color=white:amplitude=0.03:r=48000:seed=7"
    -filter_complex "[0:a][1:a]amix=inputs=2:duration=first:normalize=0,aresample=48000"
    -ac 1 -c:a pcm_s16le)

# 8) Escenas: tres planos de 2 s (patrón en movimiento, barras, Mandelbrot en zoom) a 720p30.
#    Cortes exactos en 2 s y 4 s; dentro de cada plano hay movimiento que no es un corte.
set(ext .mp4)
make_media(scenes_720p30.mp4
    -f lavfi -i "testsrc2=s=1280x720:r=30:d=2"
    -f lavfi -i "smptehdbars=s=1280x720:r=30:d=2"
    -f lavfi -i "mandelbrot=s=1280x720:r=30,trim=duration=2"
    -filter_complex "[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=yuv420p"
    -c:v libx264 -preset veryfast -g 30)

# 8b) Escenas con transiciones graduales (grafo en data/escenas_fundidos.txt): testsrc2 → disolvencia
#     de 1 s (2–3 s, centro 2,5) → barras → fundido a negro y desde negro de 1,2 s (3,8–5 s; xfade
#     oscurece deprisa: negro hacia 4,05) → mandelbrot con zoom y paneo de 150 px/s (no es un cambio
#     de escena) → corte a 7,8 s →
#     barras rgb con fundido a negro (8,5–9,3) → 0,5 s de negro → testsrc2 apareciendo (9,8–10,6).
make_media(scenes_fades.mp4
    -f lavfi -i "testsrc2=s=640x360:r=30:d=3,format=yuv420p"
    -f lavfi -i "smptehdbars=s=640x360:r=30:d=3,format=yuv420p"
    -f lavfi -i "mandelbrot=s=1280x360:r=30,trim=duration=4,setpts=PTS-STARTPTS,crop=640:360:t*150:0,format=yuv420p"
    -f lavfi -i "rgbtestsrc=s=640x360:r=30:d=1.5,format=yuv420p"
    -f lavfi -i "color=c=black:s=640x360:r=30:d=0.5,format=yuv420p"
    -f lavfi -i "testsrc2=s=640x360:r=30:d=2,format=yuv420p"
    -/filter_complex "${CMAKE_CURRENT_LIST_DIR}/data/escenas_fundidos.txt"
    -c:v libx264 -preset veryfast -g 30)

# 9) Escenas con música: los tres planos de (8) con un bombo a 120 BPM (pulsos en 0,25 + 0,5·k s).
set(ext .mp4)
make_media(scenes_beats.mp4
    -f lavfi -i "testsrc2=s=1280x720:r=30:d=2"
    -f lavfi -i "smptehdbars=s=1280x720:r=30:d=2"
    -f lavfi -i "mandelbrot=s=1280x720:r=30,trim=duration=2"
    -f lavfi -i "aevalsrc='0.8*sin(2*PI*60*t)*exp(-mod(t-0.25\,0.5)*14)*gte(t\,0.25)':s=48000:d=6"
    -filter_complex "[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=yuv420p"
    -c:v libx264 -preset veryfast -g 30 -c:a aac -b:a 128k)

# 10) Sujeto para recortar: bola roja de radio 70 que oscila en horizontal (centro x = 320 + 100·sen 3t,
#     y = 180) sobre un fondo gris con textura suave. 1 s a 10 fps, 640×360.
set(ext .mp4)
make_media(subject_ball.mp4
    -f lavfi -i "color=c=gray:s=640x360:r=10:d=1,format=rgb24,geq=r='if(lt(hypot(X-320-100*sin(T*3),Y-180),70),230,90+20*sin(X/9))':g='if(lt(hypot(X-320-100*sin(T*3),Y-180),70),40,100+20*sin(Y/7))':b='if(lt(hypot(X-320-100*sin(T*3),Y-180),70),40,110)',format=yuv420p"
    -c:v libx264 -preset veryfast -crf 12)

# 10c) La misma bola durante 2,5 s (SAM 2 hacia atrás por varios tramos de un segundo).
make_media(subject_ball_long.mp4
    -f lavfi -i "color=c=gray:s=640x360:r=10:d=2.5,format=rgb24,geq=r='if(lt(hypot(X-320-100*sin(T*3),Y-180),70),230,90+20*sin(X/9))':g='if(lt(hypot(X-320-100*sin(T*3),Y-180),70),40,100+20*sin(Y/7))':b='if(lt(hypot(X-320-100*sin(T*3),Y-180),70),40,110)',format=yuv420p"
    -c:v libx264 -preset veryfast -crf 12)

# 10b) Seguimiento con escala y giro: tarjeta de 120 × 80 a cuadrantes y con franjas que avanza
#      (centro x = 200 + 120·t, y = 180), crece (escala 1 + 0,4·t) y gira 0,6·t rad en sentido horario
#      sobre el mismo fondo que la bola. 2 s a 15 fps, 640×360.
set(card_u "((X-200-120*T)*cos(0.6*T)+(Y-180)*sin(0.6*T))/(1+0.4*T)")
set(card_v "((Y-180)*cos(0.6*T)-(X-200-120*T)*sin(0.6*T))/(1+0.4*T)")
set(card_in "lt(abs(${card_u}),60)*lt(abs(${card_v}),40)")
make_media(subject_card.mp4
    -f lavfi -i "color=c=gray:s=640x360:r=15:d=2,format=rgb24,geq=r='if(${card_in},if(gt(${card_u},0),220,40),90+20*sin(X/9))':g='if(${card_in},if(gt(${card_v},0),200,60),100+20*sin(Y/7))':b='if(${card_in},80+120*gt(sin(${card_u}/5),0),110)',format=yuv420p"
    -c:v libx264 -preset veryfast -crf 12)

# 11) Multicámara: la misma voz grabada por dos cámaras y un micrófono aparte, con desfases conocidos
#     respecto de mc_cam1 (que empieza con la voz): mc_cam2 empieza 1,7 s después, más baja, filtrada
#     y con ruido; mc_mic empieza 0,6 s después, sin agudos.
set(ext .mp4)
make_media(mc_cam1.mp4
    -f lavfi -i "testsrc2=s=640x360:r=30:d=6"
    -i "${CMAKE_CURRENT_LIST_DIR}/data/speech_es.flac"
    -map 0:v -map 1:a -t 6 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -b:a 128k -ar 48000)
make_media(mc_cam2.mp4
    -f lavfi -i "smptehdbars=s=640x360:r=30:d=6"
    -ss 1.7 -i "${CMAKE_CURRENT_LIST_DIR}/data/speech_es.flac"
    -f lavfi -i "anoisesrc=color=pink:amplitude=0.02:r=48000:seed=3"
    -filter_complex "[1:a][2:a]amix=inputs=2:duration=first:normalize=0,volume=0.5,highpass=f=200"
    -map 0:v -t 6 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -b:a 128k -ar 48000)
set(ext .wav)
make_media(mc_mic.wav
    -ss 0.6 -i "${CMAKE_CURRENT_LIST_DIR}/data/speech_es.flac"
    -af "aresample=48000,lowpass=f=5000" -t 8 -c:a pcm_s16le)

# 11b) Multicámara larga: 13 min de ruido con envolvente cambiante (mc_long_ref) y otra grabación de
#      la misma escena que empieza 11 min después, dura 115 s y cuyo reloj adelanta 125 ppm (su
#      audio va 8001/8000 veces más rápido), más baja y sin agudos.
set(ext .flac)
make_media(mc_long_ref.flac
    -f lavfi -i "anoisesrc=d=780:c=white:r=8000:a=0.5:seed=7,volume='(0.15+0.85*abs(sin(2*PI*0.7*t)*sin(2*PI*0.23*t+1)))':eval=frame"
    -c:a flac)
make_media(mc_long_late.flac
    -ss 660 -t 115.1 -i "${OUT_DIR}/mc_long_ref.flac"
    -af "asetrate=8001,aresample=8000,lowpass=f=2500,volume=0.5" -t 115 -c:a flac)
