#pragma once

#include "core/Project.h"
#include "media/Time.h"

#include <functional>
#include <string>
#include <vector>

// Reconocimiento de voz (whisper.cpp) y detección de voz (Silero VAD) sobre audio mono a 16 kHz.
// Funciones síncronas: las ejecuta el proceso de IA en su hilo de trabajo.
namespace movix::ai {

inline constexpr int kSpeechSampleRate = 16000;

using ProgressFn = std::function<void(double fraction)>; // 0..1
using CancelFn = std::function<bool()>;                  // true = abandonar

// Capacidades de la CPU/GPU que usa whisper.cpp (diagnóstico).
std::string systemInfo();

// Decodifica el audio del archivo a mono 16 kHz float. false si no tiene audio o falla (o se cancela).
bool loadSpeechAudio(const std::string& path, std::vector<float>& out, std::string* error = nullptr,
                     const ProgressFn& progress = {}, const CancelFn& cancel = {});

struct SpeechSpan {
    TimeUs start = 0;
    TimeUs end = 0;
};

struct TranscribeOptions {
    std::string language = "auto"; // ISO 639-1 o "auto"
    bool translate = false;        // traducir al inglés
    int threads = 0;               // 0 = automático
    bool useGpu = true;
    // Tramos con voz (Silero VAD) para ajustar los bordes de las palabras; vacío = sin ajuste.
    std::vector<SpeechSpan> speech;
};

// Transcribe con marcas de tiempo por palabra. Devuelve false (y `error`) si falla o se cancela.
bool transcribe(const std::string& modelPath, const std::vector<float>& pcm, const TranscribeOptions& options,
                core::Transcript& out, std::string* error = nullptr, const ProgressFn& progress = {},
                const CancelFn& cancel = {});

struct VadOptions {
    float threshold = 0.5f;
    int minSpeechMs = 250;
    int minSilenceMs = 300;
    int padMs = 100;
    int threads = 0;
};

// Tramos con voz, ordenados y sin solaparse.
bool detectSpeech(const std::string& modelPath, const std::vector<float>& pcm, const VadOptions& options,
                  std::vector<SpeechSpan>& out, std::string* error = nullptr);

} // namespace movix::ai
