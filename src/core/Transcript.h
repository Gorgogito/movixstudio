#pragma once

#include "core/Project.h"

#include <map>
#include <string>
#include <vector>

// Transcripciones y subtítulos. Una transcripción pertenece a un medio (tiempos del medio); los
// subtítulos se obtienen llevando sus palabras a la línea de tiempo a través de los clips que lo usan.
namespace movix::core {

// Palabra situada en la línea de tiempo (o en cualquier eje de tiempo común).
struct TimedWord {
    TimeUs start = 0;
    TimeUs end = 0;
    std::string text;
    Id clipId = kNoId; // clip por el que suena (kNoId si no procede)
};

// Palabras que se oyen en la secuencia activa: las de los medios transcritos de los clips con
// audio audible (pistas sin silenciar, clips activos, audio no separado), ordenadas por tiempo.
// Una palabra entra si su punto medio cae dentro del clip; su final se limita al del clip.
std::vector<TimedWord> timelineWords(const Project& p);

struct Cue {
    TimeUs start = 0;
    TimeUs end = 0;
    std::string text; // las líneas separadas por '\n'
    bool operator==(const Cue&) const = default;
};

struct CueOptions {
    int maxCharsPerLine = 42;
    int maxLines = 2;
    TimeUs maxDuration = 6 * kUsPerSecond;
    TimeUs minDuration = 800'000;   // se alarga hasta aquí si no pisa el siguiente
    TimeUs maxGap = 700'000;        // una pausa mayor empieza un subtítulo nuevo
    bool breakAtSentences = true;   // cerrar el subtítulo tras . ? ! …
};

// Agrupa palabras en subtítulos legibles (por longitud, pausas, duración y frases) y reparte el
// texto en líneas equilibradas.
std::vector<Cue> makeCues(const std::vector<TimedWord>& words, const CueOptions& options = {});

// Reparte un texto en como mucho `maxLines` líneas de longitud parecida (sin partir palabras).
std::string balanceLines(const std::string& text, int maxCharsPerLine, int maxLines);

// SubRip (.srt). El análisis acepta BOM, CRLF y bloques sin número; ignora los mal formados.
std::string toSrt(const std::vector<Cue>& cues);
std::vector<Cue> parseSrt(const std::string& srt);

// Tramo [start, end) de la línea de tiempo en fotogramas.
struct FrameRange {
    FramePos start = 0;
    FramePos end = 0;
    bool operator==(const FrameRange&) const = default;
};

// Tramo con voz de un medio (tiempos del medio).
struct SpeechRange {
    TimeUs start = 0;
    TimeUs end = 0;
};

// Silencios de la línea de tiempo: donde ningún clip audible de un medio analizado tiene voz.
// `speechByMedia` contiene los tramos con voz de cada medio analizado (los demás, p. ej. música,
// no cuentan como voz). Solo cuentan los silencios de al menos `minSilence`, y se acortan `padding`
// por cada lado con voz (para no comerse el principio ni el final de las palabras).
std::vector<FrameRange> silentRanges(const Project& p, const std::map<Id, std::vector<SpeechRange>>& speechByMedia,
                                     TimeUs minSilence, TimeUs padding);

// Tramo de la línea de tiempo que ocupan unas palabras (del inicio de la primera al final de la
// última, ampliado a fotogramas completos).
FrameRange wordsRange(const Project& p, const std::vector<TimedWord>& words);

// Estilo por defecto de los subtítulos y posición vertical de su centro (fracción del lienzo).
TitleStyle subtitleStyle();
inline constexpr double kSubtitleOffsetY = 0.38;

// Subtítulos de una pista: los títulos que contiene, en tiempo de la línea de tiempo.
std::vector<Cue> cuesOfTrack(const Project& p, const Track& track);

} // namespace movix::core
