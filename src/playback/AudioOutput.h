#pragma once

#include "media/Time.h"

#include <cstdint>
#include <memory>
#include <string>

namespace movix::playback {

// Salida de audio en tiempo real (miniaudio) con buffer circular y reloj de reproducción.
//
// El reloj indica la posición de lo que se está oyendo: avanza solo cuando el dispositivo
// consume muestras, así que es el reloj maestro para sincronizar el video. Si el buffer se
// vacía (decodificación lenta, búsqueda), el reloj se detiene en lugar de adelantarse.
class AudioOutput {
public:
    struct Config {
        int sampleRate = 48000;
        int channels = 2;
        bool nullBackend = false;  // dispositivo virtual sin sonido (pruebas, equipos sin audio)
        double bufferSeconds = 1.0;
    };

    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool start(const Config& config, std::string* error = nullptr);
    void stop();
    bool isRunning() const;
    int sampleRate() const;
    int channels() const;
    std::string deviceName() const;

    // --- Productor (hilo de decodificación de audio) ---
    // Cada reset() incrementa la generación; las escrituras de una generación anterior se rechazan.
    uint64_t generation() const;
    // Escribe hasta `frames` muestras cuya primera está en `pts`. Devuelve las escritas
    // (0 si el buffer está lleno) o -1 si la generación ya no es la actual.
    int write(const float* data, int frames, TimeUs pts, uint64_t generation);
    void setEndOfStream(uint64_t generation);

    // --- Control ---
    void reset(TimeUs position); // vacía el buffer; el reloj queda en `position` hasta que llegue audio
    void setPlaying(bool playing);
    void setVolume(float volume);

    // --- Reloj ---
    TimeUs clock() const;
    bool drained() const;        // fin del stream y buffer consumido
    TimeUs buffered() const;     // audio pendiente de reproducir

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace movix::playback
