#pragma once

#include "media/FFmpegUtils.h"
#include "media/VideoFrame.h"

#include <memory>
#include <string>

struct SwsContext;

namespace movix::media {

struct VideoDecoderOptions {
    bool hardware = true; // intentar D3D11VA (Windows) y, si falla, usar CPU
};

// Decodificador de video síncrono: un demuxer propio por instancia, salida NV12 de 8 bits.
// No es thread-safe; cada hilo de reproducción posee su propio decodificador.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, const VideoDecoderOptions& options = {}, std::string* error = nullptr);
    void close();
    bool isOpen() const { return m_codec != nullptr; }

    // Siguiente fotograma en orden de presentación; nullptr al llegar al final.
    std::shared_ptr<VideoFrame> decodeNext();

    // Búsqueda precisa: el próximo decodeNext() devuelve el fotograma que se muestra en `t`
    // (el último con pts <= t), o el último del archivo si `t` está más allá del final.
    bool seek(TimeUs t);

    bool isHardwareAccelerated() const;
    std::string decoderDescription() const; // p. ej. "h264 · D3D11VA" o "hevc · CPU (8 hilos)"
    TimeUs duration() const { return m_duration; }
    TimeUs frameDuration() const { return m_frameDuration; }
    double frameRate() const { return m_frameRate; }
    int width() const;
    int height() const;

private:
    enum class Receive { Frame, Eof, Error };
    Receive receiveRaw(AVFrame* out);  // decodifica sin convertir
    std::shared_ptr<VideoFrame> convert(AVFrame* frame, TimeUs pts);
    TimeUs framePts(const AVFrame* frame);
    TimeUs frameDur(const AVFrame* frame) const;

    FormatContextPtr m_format;
    CodecContextPtr m_codec;
    PacketPtr m_packet;
    FramePtr m_frame;
    FramePtr m_swFrame;
    FramePtr m_pending;          // fotograma ya decodificado por seek() pendiente de entregar
    SwsContext* m_sws = nullptr;
    AVBufferRef* m_hwDevice = nullptr;
    int m_hwPixFmt = -1;         // AVPixelFormat del frame hardware
    int m_streamIndex = -1;
    AVRational m_timeBase{1, 1};
    TimeUs m_origin = 0;
    TimeUs m_duration = 0;
    TimeUs m_frameDuration = 0;
    double m_frameRate = 0.0;
    double m_sampleAspect = 1.0;
    TimeUs m_lastPts = kNoTime;
    TimeUs m_pendingPts = 0;
    bool m_hasPending = false;
    bool m_flushing = false;     // ya se envió el paquete nulo al decodificador
    bool m_lastFrameWasHw = false;
};

} // namespace movix::media
