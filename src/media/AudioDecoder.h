#pragma once

#include "media/FFmpegUtils.h"

#include <string>
#include <vector>

struct SwrContext;

namespace movix::media {

// Bloque de audio PCM float intercalado al formato de salida pedido.
struct AudioChunk {
    std::vector<float> samples; // frames * channels
    int frames = 0;
    TimeUs pts = 0;             // instante de la primera muestra
};

// Decodificador de audio síncrono con remuestreo a una frecuencia/canales fijos.
// La línea de tiempo de salida es continua: pts(n+1) = pts(n) + frames/sampleRate.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const std::string& path, int outSampleRate, int outChannels, std::string* error = nullptr);
    void close();
    bool isOpen() const { return m_codec != nullptr; }

    // Rellena `chunk` con el siguiente bloque; false al final del stream.
    bool read(AudioChunk& chunk);

    // Búsqueda exacta a la muestra: el siguiente read() empieza en `t`.
    bool seek(TimeUs t);

    int sampleRate() const { return m_outRate; }
    int channels() const { return m_outChannels; }
    TimeUs duration() const { return m_duration; }
    std::string decoderDescription() const;

private:
    bool resetResampler(const AVFrame* frame);
    bool decodeFrame();                       // deja un frame en m_frame; false en EOF
    void appendOutput(AudioChunk& chunk, const float* data, int frames);

    std::string m_path;
    FormatContextPtr m_format;
    CodecContextPtr m_codec;
    PacketPtr m_packet;
    FramePtr m_frame;
    SwrContext* m_swr = nullptr;
    int m_streamIndex = -1;
    AVRational m_timeBase{1, 1};
    TimeUs m_origin = 0;
    TimeUs m_duration = 0;
    int m_outRate = 48000;
    int m_outChannels = 2;
    TimeUs m_basePts = kNoTime;  // pts de la primera muestra de salida tras abrir/buscar
    int64_t m_samplesOut = 0;    // muestras de salida producidas desde m_basePts
    TimeUs m_trimUntil = kNoTime;// descartar salida anterior a este instante (tras seek)
    std::vector<float> m_scratch;
    int m_swrInRate = 0;
    int m_swrInFormat = -1;
    int m_swrInChannels = 0;
    bool m_flushing = false;
    bool m_drained = false;
};

} // namespace movix::media
