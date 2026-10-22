#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace movix::media {

struct VideoEncoderSettings {
    std::string codec = "libx264"; // nombre del codificador de FFmpeg
    int width = 1920;
    int height = 1080;
    int fpsNum = 30;
    int fpsDen = 1;
    int quality = 23;              // CRF/CQ/QP según el códec (menor = mejor)
    int64_t bitRate = 0;           // > 0: tasa de bits objetivo en lugar de calidad constante
    std::string preset;            // vacío = valor recomendado para el códec
    bool input16 = false;          // writeVideo recibe RGBA de 16 bits por canal (little endian)
    bool tenBit = false;           // codificar a 10 bits (falla si el códec no lo admite)
    int gop = 0;                   // > 0: fotogramas entre claves (si no, unos 2 s)
    // Señal de la imagen: SDR Rec.709, o HDR Rec.2020 con PQ (HDR10) o HLG.
    enum class Signal { SDR, PQ, HLG } signal = Signal::SDR;
};

struct AudioEncoderSettings {
    std::string codec = "aac";
    int sampleRate = 48000;
    int channels = 2;
    int64_t bitRate = 192'000;
};

struct EncoderSettings {
    std::string path;              // UTF-8
    std::string container;         // vacío = según la extensión
    bool hasVideo = true;
    VideoEncoderSettings video;
    bool hasAudio = true;
    AudioEncoderSettings audio;
};

struct EncoderInfo {
    std::string name;              // libx264, h264_nvenc...
    std::string description;
    bool hardware = false;
};

// Codificador y multiplexor: recibe fotogramas RGBA y audio float intercalado y escribe el archivo.
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool open(const EncoderSettings& settings, std::string* error = nullptr);
    // Siguiente fotograma de video (RGBA de 8 bits, o de 16 con input16; `stride` bytes por fila).
    bool writeVideo(const uint8_t* rgba, int stride);
    // Lo mismo desde NV12 ya convertido (BT.709, rango limitado; Y y luego UV intercalado, sin
    // relleno): sin conversión en la CPU. Solo si acceptsNv12().
    bool writeVideoNv12(const uint8_t* nv12);
    bool acceptsNv12() const;
    // Audio intercalado al formato de AudioEncoderSettings.
    bool writeAudio(const float* samples, int frames);
    // Vacía los codificadores y cierra el archivo.
    bool finish(std::string* error = nullptr);
    // Cierra sin terminar y borra el archivo parcial.
    void abort();

    int64_t videoFramesWritten() const;
    std::string lastError() const;

    // Codificadores de video que funcionan en este equipo (se prueban una vez y se recuerdan).
    static const std::vector<EncoderInfo>& availableVideoEncoders();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace movix::media
