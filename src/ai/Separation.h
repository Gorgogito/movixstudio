#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <string>
#include <vector>

// Separación de fuentes con HTDemucs v4 (ONNX): batería, bajo, resto y voz. El audio se procesa en
// ventanas de 7,8 s con un 25 % de solapamiento y fundido triangular (como apply_model de Demucs).
namespace movix::ai {

inline constexpr int kSeparationSampleRate = 44100;

struct Stems {
    // Estéreo intercalado a 44,1 kHz, misma longitud que la entrada.
    std::vector<float> vocals;
    std::vector<float> accompaniment; // batería + bajo + resto
};

// `stereo`: intercalado, 44,1 kHz.
bool separate(const std::string& modelPath, const std::vector<float>& stereo, Stems& out, std::string* error = nullptr,
              const ProgressFn& progress = {}, const CancelFn& cancel = {});

// Decodifica [from, to) del medio (estéreo 44,1 kHz) y lo separa.
bool separateFile(const std::string& modelPath, const std::string& mediaPath, TimeUs from, TimeUs to, Stems& out,
                  std::string* error = nullptr, const ProgressFn& progress = {}, const CancelFn& cancel = {});

} // namespace movix::ai
