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
