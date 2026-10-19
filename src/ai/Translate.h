#pragma once

#include "ai/Speech.h"

#include <string>
#include <vector>

// Traducción automática con Opus-MT (modelos Marian de Helsinki-NLP en ONNX): SentencePiece para
// trocear el texto, el codificador una vez y el decodificador paso a paso (búsqueda voraz).
namespace movix::ai {

// `modelFolder` contiene encoder.onnx, decoder.onnx, source.spm, target.spm y vocab.json.
bool translate(const std::string& modelFolder, const std::vector<std::string>& texts, std::vector<std::string>& out,
               std::string* error = nullptr, const ProgressFn& progress = {}, const CancelFn& cancel = {});

} // namespace movix::ai
