#pragma once

// Protocolo entre la aplicación y el proceso de IA (movix-ai-worker): un objeto JSON por línea,
// UTF-8, por stdin (peticiones) y stdout (eventos). El proceso termina al cerrarse su stdin.
//
// Peticiones:
//   {"id": 1, "type": "transcribe", "media": "<ruta>", "model": "<ruta>", "modelName": "whisper-base",
//    "language": "auto", "translate": false, "vadModel": "<ruta opcional: ajusta las palabras a la voz>"}
//   {"id": 2, "type": "detectSpeech", "media": "<ruta>", "model": "<ruta>", "threshold": 0.5,
//    "minSpeechMs": 250, "minSilenceMs": 300, "padMs": 100}
//   {"id": 3, "type": "detectScenes", "media": "<ruta>", "from": µs, "to": µs (0 = hasta el final),
//    "threshold": 0.18, "minSceneUs": 500000}
//   {"id": 4, "type": "detectBeats", "media": "<ruta>", "from": µs, "to": µs}
//   {"id": 5, "type": "synthesize", "model": "<carpeta de Kokoro>", "text": "...", "voice": "ef_dora",
//    "speed": 1.0, "output": "<ruta .wav>"}
//   {"id": 6, "type": "translate", "model": "<carpeta de Opus-MT>", "texts": ["...", ...]}
//   {"id": 7, "type": "separate", "model": "<ruta htdemucs.onnx>", "media": "<ruta>", "from": µs, "to": µs,
//    "vocals": "<ruta .wav>", "accompaniment": "<ruta .wav>"}
//   {"id": 8, "type": "removeBackground", "model": "<ruta .onnx>", "kind": "portrait" | "general",
//    "media": "<ruta>", "from": µs, "to": µs, "output": "<ruta .mp4 del mate>"}
//   {"id": 9, "type": "upscale", "model": "<ruta .onnx>", "media": "<ruta>", "from": µs, "to": µs, "scale": 2 | 4,
//    "output": "<ruta .mp4>"}
//   {"id": 10, "type": "interpolate", "model": "<ruta .onnx>", "media": "<ruta>", "from": µs, "to": µs,
//    "factor": 2..8, "output": "<ruta .mp4>"}
//   {"id": 11, "type": "track", "media": "<ruta>", "from": µs, "to": µs, "region": [cx, cy, w, h] (fracciones)}
//   {"id": 12, "type": "subjectCenters", "media": "<ruta>", "from": µs, "to": µs, "step": µs, "model": "<MODNet opcional>"}
//   {"id": 13, "type": "segmentObject", "model": "<carpeta de SAM 2>", "media": "<ruta>", "from": µs, "to": µs,
//    "x": 0..1, "y": 0..1, "output": "<ruta .mp4 del mate>"}  → como removeBackground
//   {"type": "cancel", "id": 1}
// Eventos:
//   {"id": 1, "event": "started"}
//   {"id": 1, "event": "progress", "value": 0.42, "stage": "decode" | "transcribe" | "detect"}
//   {"id": 1, "event": "result", ...}  transcribe → "transcript": {...} (ver core::transcriptToJson)
//                                      detectSpeech → "speech": [[inicio µs, fin µs], ...], "duration": µs
//                                      detectScenes → "cuts": [µs, ...] (inicio de cada escena nueva)
//                                      detectBeats → "bpm": 120.0, "beats": [µs, ...]
//                                      synthesize → "output": ruta, "duration": µs
//                                      translate → "texts": ["...", ...]
//                                      separate → "vocals": ruta, "accompaniment": ruta, "duration": µs
//                                      removeBackground → "output": ruta, "start": µs, "frames": n
//                                      upscale / interpolate → "output": ruta, "width", "height", "fps", "frames"
//                                      track / subjectCenters → "points": [[µs, cx, cy, confianza], ...]
//   {"id": 1, "event": "error", "message": "...", "canceled": false}
namespace movix::ai::protocol {

inline constexpr const char* kTranscribe = "transcribe";
inline constexpr const char* kDetectSpeech = "detectSpeech";
inline constexpr const char* kDetectScenes = "detectScenes";
inline constexpr const char* kDetectBeats = "detectBeats";
inline constexpr const char* kSynthesize = "synthesize";
inline constexpr const char* kTranslate = "translate";
inline constexpr const char* kSeparate = "separate";
inline constexpr const char* kRemoveBackground = "removeBackground";
inline constexpr const char* kUpscale = "upscale";
inline constexpr const char* kInterpolate = "interpolate";
inline constexpr const char* kTrack = "track";
inline constexpr const char* kSubjectCenters = "subjectCenters";
inline constexpr const char* kSegmentObject = "segmentObject";
inline constexpr const char* kCancel = "cancel";

} // namespace movix::ai::protocol
