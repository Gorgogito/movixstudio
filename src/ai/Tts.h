#pragma once

#include "ai/Speech.h"

#include <string>
#include <vector>

// Texto a voz con Kokoro 82M (ONNX): el texto se pasa a fonemas IPA con eSpeak NG (en inglés, con el
// léxico de Kokoro), los fonemas a identificadores con el vocabulario del modelo, y el modelo genera
// la voz a 24 kHz con el estilo de la voz elegida.
namespace movix::ai {

inline constexpr int kTtsSampleRate = 24000;

struct Voice {
    std::string id;       // "ef_dora"
    std::string language; // "es", "en-us", "en-gb"
};

// Voces que instala el modelo de Kokoro (ver ModelCatalog).
std::vector<Voice> ttsVoices();

// Fonemas IPA (con acentos de intensidad) del texto, en el formato que espera Kokoro: la puntuación
// se conserva y los diptongos y africadas llevan sus símbolos propios. `espeakDataParent` es la
// carpeta que contiene espeak-ng-data. En inglés, si `lexiconFolder` tiene el léxico de Kokoro
// (us_gold.json y us_silver.json, ver EnglishG2P.h), las palabras salen de él y eSpeak solo
// fonetiza las que no están. Devuelve "" si no se pudo fonetizar.
std::string phonemize(const std::string& espeakDataParent, const std::string& language, const std::string& text,
                      std::string* error = nullptr, const std::string& lexiconFolder = {});

struct TtsOptions {
    std::string voice = "ef_dora";
    double speed = 1.0; // 0,5..2
};

// Sintetiza el texto (de cualquier longitud: se divide por frases) a mono 24 kHz.
bool synthesize(const std::string& modelFolder, const std::string& espeakDataParent, const std::string& text,
                const TtsOptions& options, std::vector<float>& audio, std::string* error = nullptr,
                const ProgressFn& progress = {}, const CancelFn& cancel = {});

// Escribe un WAV de 16 bits.
bool writeWav(const std::string& path, const std::vector<float>& samples, int sampleRate, int channels, std::string* error = nullptr);

} // namespace movix::ai
