# movix_media: acceso a medios sobre FFmpeg (sondeo, decodificación de video/audio, conversión de color).
# No depende de la interfaz; se usa tanto desde la app como desde las pruebas.
add_library(movix_media STATIC
    Time.h
    FFmpegUtils.h FFmpegUtils.cpp
    MediaInfo.h MediaInfo.cpp
    VideoFrame.h
    ColorMatrix.h ColorMatrix.cpp
    VideoDecoder.h VideoDecoder.cpp
    AudioDecoder.h AudioDecoder.cpp
    Encoder.h Encoder.cpp
    FrameConvert.h FrameConvert.cpp
    ImageDecoder.h ImageDecoder.cpp
    AudioFilter.h AudioFilter.cpp
    TempoFilter.h TempoFilter.cpp
)

target_include_directories(movix_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(movix_media
    PUBLIC
        FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample FFmpeg::avfilter
)
