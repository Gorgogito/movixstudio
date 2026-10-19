#include "core/Transcript.h"

#include "core/Timeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>

namespace movix::core {

namespace {

// Longitud visible en caracteres (UTF-8: no cuenta los bytes de continuación).
int visibleLength(const std::string& s)
{
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

bool endsSentence(const std::string& word)
{
    if (word.empty())
        return false;
    const char last = word.back();
    if (last == '.' || last == '?' || last == '!')
        return true;
    // "…" (U+2026) en UTF-8
    return word.size() >= 3 && word.compare(word.size() - 3, 3, "\xE2\x80\xA6") == 0;
}

std::string lowered(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return r;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string srtTime(TimeUs t)
{
    const long long ms = std::max<long long>(0, (t + 500) / 1000);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
    return buf;
}

// "HH:MM:SS,mmm" (también acepta '.' como separador decimal). kNoTime si no es válida.
TimeUs parseSrtTime(const std::string& s)
{
    int h = 0, m = 0, sec = 0, ms = 0;
    char sep = 0;
    if (std::sscanf(s.c_str(), " %d:%d:%d%c%d", &h, &m, &sec, &sep, &ms) != 5 || (sep != ',' && sep != '.'))
        return kNoTime;
    return ((TimeUs(h) * 60 + m) * 60 + sec) * kUsPerSecond + TimeUs(ms) * 1000;
}

} // namespace

std::vector<TimedWord> timelineWords(const Project& p)
{
    std::vector<TimedWord> words;
    for (const AudioSegment& seg : audioSegmentsIn(p, 0, p.durationTime())) {
        if (!seg.media || !seg.media->transcript || std::max(seg.gainStart, seg.gainEnd) < 0.05f)
            continue;
        const double speed = seg.speed > 0 ? seg.speed : 1.0;
        auto toTimeline = [&](TimeUs source) {
            return seg.timelineStart + TimeUs(std::llround(double(source - seg.sourceStart) / speed));
        };
        for (const TranscriptWord& w : seg.media->transcript->words) {
            const TimeUs mid = toTimeline((w.start + w.end) / 2);
            if (mid < seg.timelineStart || mid >= seg.timelineEnd)
                continue;
            TimedWord tw;
            tw.start = std::max(seg.timelineStart, toTimeline(w.start));
            tw.end = std::min(seg.timelineEnd, toTimeline(w.end));
            tw.text = w.text;
            tw.clipId = seg.clipId;
            words.push_back(std::move(tw));
        }
    }
    std::stable_sort(words.begin(), words.end(), [](const TimedWord& a, const TimedWord& b) { return a.start < b.start; });

    // El mismo audio por dos caminos (p. ej. el tramo prolongado de una transición) no se repite.
    std::vector<TimedWord> unique;
    for (TimedWord& w : words) {
        if (!unique.empty()) {
            const TimedWord& prev = unique.back();
            const TimeUs overlap = std::min(prev.end, w.end) - std::max(prev.start, w.start);
            if (overlap * 2 > std::max<TimeUs>(1, w.end - w.start) && lowered(prev.text) == lowered(w.text))
                continue;
        }
        unique.push_back(std::move(w));
    }
    return unique;
}

std::string balanceLines(const std::string& text, int maxCharsPerLine, int maxLines)
{
    const std::string t = trim(text);
    if (maxLines < 2 || visibleLength(t) <= maxCharsPerLine)
        return t;
    std::vector<std::string> words;
    std::istringstream in(t);
    for (std::string w; in >> w;)
        words.push_back(w);
    if (words.size() < 2)
        return t;

    // Corte en el espacio que deja las dos partes más parecidas (las líneas extra, si hiciera falta,
    // se reparten igual de forma recursiva sobre la segunda parte).
    const int total = visibleLength(t);
    size_t best = 1;
    int bestScore = INT32_MAX;
    int left = 0;
    for (size_t i = 1; i < words.size(); ++i) {
        left += visibleLength(words[i - 1]) + (i > 1 ? 1 : 0);
        const int right = total - left - 1;
        const int target = total / maxLines;
        const bool overflow = left > maxCharsPerLine || (maxLines == 2 && right > maxCharsPerLine);
        const int score = std::abs(left - target) + (overflow ? 1000 : 0);
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    std::string first, rest;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string& line = i < best ? first : rest;
        if (!line.empty())
            line += ' ';
        line += words[i];
    }
    return first + "\n" + balanceLines(rest, maxCharsPerLine, maxLines - 1);
}

// ¿Cabe el texto en las líneas permitidas sin superar el máximo en ninguna?
static bool fitsLines(const std::string& text, int maxCharsPerLine, int maxLines)
{
    std::istringstream in(balanceLines(text, maxCharsPerLine, maxLines));
    int lines = 0;
    for (std::string line; std::getline(in, line); ++lines)
        if (visibleLength(line) > maxCharsPerLine)
            return false;
    return lines <= std::max(1, maxLines);
}

std::vector<Cue> makeCues(const std::vector<TimedWord>& words, const CueOptions& o)
{
    std::vector<Cue> cues;

    std::string text;
    TimeUs start = 0, end = 0;
    bool open = false;
    auto flush = [&] {
        if (open && !text.empty())
            cues.push_back({start, end, balanceLines(text, o.maxCharsPerLine, o.maxLines)});
        text.clear();
        open = false;
    };

    for (size_t i = 0; i < words.size(); ++i) {
        const TimedWord& w = words[i];
        const std::string word = trim(w.text);
        if (word.empty())
            continue;
        if (open) {
            const bool gap = w.start - end > o.maxGap;
            const bool tooLong = !fitsLines(text + " " + word, o.maxCharsPerLine, o.maxLines);
            const bool tooSlow = w.end - start > o.maxDuration;
            if (gap || tooLong || tooSlow)
                flush();
        }
        if (!open) {
            start = w.start;
            open = true;
        }
        text += (text.empty() ? "" : " ") + word;
        end = std::max(end, w.end);
        if (o.breakAtSentences && endsSentence(word))
            flush();
    }
    flush();

    // Duración mínima de lectura sin pisar el siguiente; nunca solapados.
    for (size_t i = 0; i < cues.size(); ++i) {
        const TimeUs limit = i + 1 < cues.size() ? cues[i + 1].start : INT64_MAX;
        if (cues[i].end - cues[i].start < o.minDuration)
            cues[i].end = std::min(cues[i].start + o.minDuration, limit);
        cues[i].end = std::min(cues[i].end, limit);
    }
    return cues;
}

std::string toSrt(const std::vector<Cue>& cues)
{
    std::string out;
    int index = 1;
    for (const Cue& c : cues) {
        out += std::to_string(index++) + "\n";
        out += srtTime(c.start) + " --> " + srtTime(c.end) + "\n";
        out += c.text + "\n\n";
    }
    return out;
}

std::vector<Cue> parseSrt(const std::string& srt)
{
    std::string s = srt;
    if (s.rfind("\xEF\xBB\xBF", 0) == 0)
        s.erase(0, 3);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());

    std::vector<Cue> cues;
    std::istringstream in(s);
    std::string line;
    Cue current;
    bool inCue = false;
    auto finish = [&] {
        if (inCue) {
            current.text = trim(current.text);
            if (!current.text.empty() && current.end > current.start)
                cues.push_back(current);
        }
        current = Cue{};
        inCue = false;
    };
    while (std::getline(in, line)) {
        const auto arrow = line.find("-->");
        if (arrow != std::string::npos) {
            finish();
            const TimeUs a = parseSrtTime(line.substr(0, arrow));
            const TimeUs b = parseSrtTime(line.substr(arrow + 3));
            if (a != kNoTime && b != kNoTime) {
                current.start = a;
                current.end = b;
                inCue = true;
            }
            continue;
        }
        if (trim(line).empty()) {
            finish();
            continue;
        }
        if (inCue)
            current.text += (current.text.empty() ? "" : "\n") + trim(line);
        // Fuera de un bloque: número de secuencia u otra cosa; se ignora.
    }
    finish();
    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    return cues;
}

std::vector<FrameRange> silentRanges(const Project& p, const std::map<Id, std::vector<SpeechRange>>& speechByMedia,
                                     TimeUs minSilence, TimeUs padding)
{
    // Voz en tiempo de la línea de tiempo.
    std::vector<std::pair<TimeUs, TimeUs>> speech;
    const TimeUs total = p.durationTime();
    for (const AudioSegment& seg : audioSegmentsIn(p, 0, total)) {
        if (!seg.media || std::max(seg.gainStart, seg.gainEnd) < 0.05f)
            continue;
        const auto it = speechByMedia.find(seg.media->id);
        if (it == speechByMedia.end())
            continue;
        const double speed = seg.speed > 0 ? seg.speed : 1.0;
        for (const SpeechRange& r : it->second) {
            const TimeUs a = seg.timelineStart + TimeUs(std::llround(double(r.start - seg.sourceStart) / speed));
            const TimeUs b = seg.timelineStart + TimeUs(std::llround(double(r.end - seg.sourceStart) / speed));
            const TimeUs from = std::max(a, seg.timelineStart), to = std::min(b, seg.timelineEnd);
            if (to > from)
                speech.emplace_back(from, to);
        }
    }
    std::sort(speech.begin(), speech.end());

    // Complemento dentro de [0, final): silencios de al menos `minSilence`, acortados con el margen
    // (salvo hacia fuera de la línea de tiempo, al principio y al final).
    std::vector<FrameRange> silences;
    auto addSilence = [&](TimeUs a, TimeUs b, bool padStart, bool padEnd) {
        if (b - a < std::max<TimeUs>(minSilence, 1))
            return;
        if (padStart)
            a += padding;
        if (padEnd)
            b -= padding;
        // Fotogramas completamente dentro del silencio.
        const FramePos fa = p.settings.timeToFrame(a + p.settings.frameDuration() - 1);
        const FramePos fb = p.settings.timeToFrame(b);
        if (fb > fa)
            silences.push_back({fa, fb});
    };
    TimeUs cursor = 0;
    for (const auto& [a, b] : speech) {
        if (a > cursor)
            addSilence(cursor, a, cursor > 0, true);
        cursor = std::max(cursor, b);
    }
    if (total > cursor)
        addSilence(cursor, total, cursor > 0, false);
    return silences;
}

FrameRange wordsRange(const Project& p, const std::vector<TimedWord>& words)
{
    if (words.empty())
        return {};
    TimeUs a = words.front().start, b = words.front().end;
    for (const TimedWord& w : words) {
        a = std::min(a, w.start);
        b = std::max(b, w.end);
    }
    return {p.settings.timeToFrame(a), p.settings.timeToFrame(b + p.settings.frameDuration() - 1)};
}

TitleStyle subtitleStyle()
{
    TitleStyle s;
    s.text.clear();
    s.size = 0.05;
    s.bold = false;
    s.outline = 0.0;
    s.shadow = false;
    s.background = 0xB0000000;
    return s;
}

std::vector<Cue> cuesOfTrack(const Project& p, const Track& track)
{
    std::vector<Cue> cues;
    for (const Clip& c : track.clips)
        if (c.title && c.enabled)
            cues.push_back({p.settings.frameToTime(c.start), p.settings.frameToTime(c.end()), c.title->text});
    return cues;
}

} // namespace movix::core
