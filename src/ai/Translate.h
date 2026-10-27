#pragma once

#include "ai/Speech.h"

#include <string>
#include <vector>

// Traducción automática con Opus-MT (modelos Marian de Helsinki-NLP en ONNX): SentencePiece para
// trocear el texto, el codificador una vez y el decodificador paso a paso con búsqueda en haz (como
// la configuración de los modelos: 4 haces, sin penalizar la longitud más allá de la media).
namespace movix::ai {

inline constexpr int kTranslationBeams = 4;

// `modelFolder` contiene encoder.onnx, decoder.onnx, source.spm, target.spm y vocab.json. `beams`:
// hipótesis que se mantienen en cada paso (1 = búsqueda voraz).
bool translate(const std::string& modelFolder, const std::vector<std::string>& texts, std::vector<std::string>& out,
               std::string* error = nullptr, const ProgressFn& progress = {}, const CancelFn& cancel = {},
               int beams = kTranslationBeams);

// Log-probabilidad media por token (con el final) que da el modelo a `translation` como traducción de
// `text`: la medida con que se comparan las hipótesis.
bool translationScore(const std::string& modelFolder, const std::string& text, const std::string& translation, double& score,
                      std::string* error = nullptr);

} // namespace movix::ai
