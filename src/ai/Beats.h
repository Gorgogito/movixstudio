#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <string>
#include <vector>

// Detección de ritmo: envolvente de ataques (flujo espectral), tempo por autocorrelación ponderada
// hacia 120 BPM y pulsos por programación dinámica (D. P. W. Ellis, "Beat Tracking by Dynamic
// Programming", 2007). El tempo puede variar: se estima en ventanas de 8 s y se elige un recorrido
// suave entre ellas (Viterbi), que la programación dinámica sigue. Sin modelos: DSP puro.
namespace movix::ai {

inline constexpr int kBeatSampleRate = 22050;

struct BeatResult {
    double bpm = 0.0;          // tempo típico (la mediana si varía)
    std::vector<TimeUs> beats; // instantes de los pulsos
    bool variableTempo = false;
};

// `pcm`: mono a `sampleRate`. Devuelve false si no hay ritmo reconocible (silencio, voz...).
bool detectBeats(const std::vector<float>& pcm, int sampleRate, BeatResult& out);

// Decodifica el audio del archivo (mono, 22,05 kHz) en [from, to) y detecta su ritmo. Los tiempos
// son del medio.
bool detectBeats(const std::string& path, TimeUs from, TimeUs to, BeatResult& out, std::string* error = nullptr,
                 const ProgressFn& progress = {}, const CancelFn& cancel = {});

} // namespace movix::ai
