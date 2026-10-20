#include "ai/Speech.h"

#include "media/AudioDecoder.h"

#include <ggml-backend.h>
#include <whisper.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace movix::ai {

namespace {

// whisper.cpp y ggml escriben mucho en stderr; solo se muestra con MOVIX_AI_VERBOSE.
void quietLog(ggml_log_level, const char* text, void*)
{
    static const bool verbose = std::getenv("MOVIX_AI_VERBOSE") != nullptr;
    if (verbose)
        std::fputs(text, stderr);
}

// Una vez por proceso: silencia los registros y carga los backends de ggml (las variantes de CPU
// son DLL junto al ejecutable; ggml elige la mejor que admite el procesador).
void initialize()
{
    static std::once_flag once;
    std::call_once(once, [] {
        whisper_log_set(quietLog, nullptr);
        ggml_backend_load_all();
    });
}

int threadCount(int requested)
{
    if (requested > 0)
        return requested;
    return std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
}

constexpr TimeUs centisecondsToUs(int64_t cs)
{
    return cs * 10'000;
}

// El último modelo cargado se conserva: transcribir varios medios seguidos no lo relee.
struct ContextCache {
    std::mutex mutex;
    std::string path;
    whisper_context* ctx = nullptr;
    ~ContextCache()
    {
        if (ctx)
            whisper_free(ctx);
    }
};
ContextCache& cache()
{
    static ContextCache c;
    return c;
}

// Cabezas de alineamiento según el modelo (por el nombre del archivo ggml).
whisper_alignment_heads_preset alignmentHeads(const std::string& modelPath)
{
    std::string name = modelPath.substr(modelPath.find_last_of("/\\") + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const bool en = name.find(".en") != std::string::npos;
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("large-v3-turbo"))
        return WHISPER_AHEADS_LARGE_V3_TURBO;
    if (has("large-v3"))
        return WHISPER_AHEADS_LARGE_V3;
    if (has("large-v2"))
        return WHISPER_AHEADS_LARGE_V2;
    if (has("large"))
        return WHISPER_AHEADS_LARGE_V1;
    if (has("medium"))
        return en ? WHISPER_AHEADS_MEDIUM_EN : WHISPER_AHEADS_MEDIUM;
    if (has("small"))
        return en ? WHISPER_AHEADS_SMALL_EN : WHISPER_AHEADS_SMALL;
    if (has("base"))
        return en ? WHISPER_AHEADS_BASE_EN : WHISPER_AHEADS_BASE;
    if (has("tiny"))
        return en ? WHISPER_AHEADS_TINY_EN : WHISPER_AHEADS_TINY;
    return WHISPER_AHEADS_N_TOP_MOST;
}

bool isPunctuation(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80 && (std::ispunct(c) || std::isspace(c)); });
}

// Ajusta las palabras a los tramos con voz: ninguna termina dentro de un silencio y la primera de
// cada tramo empieza con él (DTW marca el instante en que se emite el token, algo después del inicio).
void snapToSpeech(std::vector<core::TranscriptWord>& words, const std::vector<SpeechSpan>& speech)
{
    if (speech.empty())
        return;
    const TimeUs kReach = 400'000;
    size_t lastSpan = SIZE_MAX;
    for (core::TranscriptWord& w : words) {
        // Tramo que contiene el inicio o, si cae en un silencio, el siguiente si está cerca.
        size_t k = 0;
        while (k < speech.size() && speech[k].end <= w.start)
            ++k;
        if (k == speech.size()) {
            if (speech.back().end > w.start - kReach)
                k = speech.size() - 1;
            else
                continue;
        }
        const SpeechSpan& span = speech[k];
        if (w.start < span.start) {
            if (span.start - w.start > kReach)
                continue;
            w.start = span.start;
        } else if (k != lastSpan && w.start - span.start < kReach) {
            w.start = span.start;
        }
        w.end = std::clamp(w.end, w.start, span.end);
        lastSpan = k;
    }
}

whisper_context* contextFor(const std::string& modelPath, bool useGpu, std::string* error)
{
    ContextCache& c = cache();
    if (c.ctx && c.path == modelPath)
        return c.ctx;
    if (c.ctx) {
        whisper_free(c.ctx);
        c.ctx = nullptr;
    }
    whisper_context_params params = whisper_context_default_params();
    params.use_gpu = useGpu;
    // Marcas por palabra con DTW sobre las cabezas de atención de alineamiento (mucho más precisas
    // que las de los tokens de tiempo). No es compatible con flash attention.
    params.flash_attn = false;
    params.dtw_token_timestamps = true;
    params.dtw_aheads_preset = alignmentHeads(modelPath);
    params.dtw_n_top = 2; // solo con WHISPER_AHEADS_N_TOP_MOST
    c.ctx = whisper_init_from_file_with_params(modelPath.c_str(), params);
    if (!c.ctx) {
        if (error)
            *error = "No se pudo cargar el modelo de transcripción: " + modelPath;
        return nullptr;
    }
    c.path = modelPath;
    return c.ctx;
}

struct Callbacks {
    const ProgressFn* progress = nullptr;
    const CancelFn* cancel = nullptr;
};

} // namespace

std::string systemInfo()
{
    initialize();
    std::string info = whisper_print_system_info();
    // Dispositivos que ggml puede usar (CPU y, si hay controlador, GPU por Vulkan).
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        info += std::string("\ndispositivo: ") + ggml_backend_dev_name(dev) + " - " + ggml_backend_dev_description(dev);
    }
    return info;
}

bool loadSpeechAudio(const std::string& path, std::vector<float>& out, std::string* error, const ProgressFn& progress,
                     const CancelFn& cancel)
{
    out.clear();
    media::AudioDecoder decoder;
    if (!decoder.open(path, kSpeechSampleRate, 1, error))
        return false;
    const TimeUs duration = decoder.duration();
    if (duration > 0)
        out.reserve(size_t(toSeconds(duration) * kSpeechSampleRate) + kSpeechSampleRate);
    media::AudioChunk chunk;
    int blocks = 0;
    while (decoder.read(chunk)) {
        out.insert(out.end(), chunk.samples.begin(), chunk.samples.begin() + chunk.frames);
        if (++blocks % 64 == 0) {
            if (cancel && cancel()) {
                if (error)
                    *error = "cancelado";
                return false;
            }
            if (progress && duration > 0)
                progress(std::clamp(double(out.size()) / kSpeechSampleRate / toSeconds(duration), 0.0, 1.0));
        }
    }
    if (out.empty()) {
        if (error)
            *error = "El archivo no tiene audio.";
        return false;
    }
    return true;
}

bool transcribe(const std::string& modelPath, const std::vector<float>& pcm, const TranscribeOptions& options,
                core::Transcript& out, std::string* error, const ProgressFn& progress, const CancelFn& cancel)
{
    initialize();
    std::lock_guard lock(cache().mutex);
    whisper_context* ctx = contextFor(modelPath, options.useGpu, error);
    if (!ctx)
        return false;

    Callbacks callbacks{&progress, &cancel};
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.n_threads = threadCount(options.threads);
    params.translate = options.translate;
    params.language = options.language.empty() ? "auto" : options.language.c_str();
    params.token_timestamps = true; // marcas por token → por palabra
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.suppress_nst = true;
    params.progress_callback = [](whisper_context*, whisper_state*, int percent, void* user) {
        auto* cb = static_cast<Callbacks*>(user);
        if (*cb->progress)
            (*cb->progress)(percent / 100.0);
    };
    params.progress_callback_user_data = &callbacks;
    params.abort_callback = [](void* user) {
        auto* cb = static_cast<Callbacks*>(user);
        return *cb->cancel && (*cb->cancel)();
    };
    params.abort_callback_user_data = &callbacks;

    if (whisper_full(ctx, params, pcm.data(), int(pcm.size())) != 0 || (cancel && cancel())) {
        if (error)
            *error = cancel && cancel() ? "cancelado" : "La transcripción falló.";
        return false;
    }

    out = core::Transcript{};
    out.language = whisper_lang_str(whisper_full_lang_id(ctx));
    const whisper_token eot = whisper_token_eot(ctx);

    // Los tokens que empiezan por espacio abren palabra; los demás (sufijos, puntuación, bytes de
    // un carácter UTF-8 partido) se pegan a la anterior. Cada palabra empieza en el instante DTW de
    // su primer token y termina donde empieza la siguiente (o, la última del segmento, poco después
    // de su último token).
    struct Pending {
        core::TranscriptWord word;
        TimeUs lastToken = 0;
        double probSum = 0.0;
        int tokens = 0;
    };
    const int segments = whisper_full_n_segments(ctx);
    for (int s = 0; s < segments; ++s) {
        const TimeUs segmentEnd = centisecondsToUs(whisper_full_get_segment_t1(ctx, s));
        std::vector<Pending> words;
        const int n = whisper_full_n_tokens(ctx, s);
        for (int i = 0; i < n; ++i) {
            const whisper_token_data data = whisper_full_get_token_data(ctx, s, i);
            if (data.id >= eot)
                continue; // tokens especiales y de tiempo
            const std::string text = whisper_full_get_token_text(ctx, s, i);
            if (text.empty())
                continue;
            const TimeUs at = centisecondsToUs(std::max<int64_t>(0, data.t_dtw));
            if (text.front() == ' ' || words.empty()) {
                Pending w;
                w.word.start = at;
                words.push_back(w);
            }
            Pending& w = words.back();
            w.word.text += text;
            if (!isPunctuation(text))
                w.lastToken = std::max(w.lastToken, at);
            w.probSum += data.p;
            ++w.tokens;
        }
        for (size_t i = 0; i < words.size(); ++i) {
            Pending& w = words[i];
            const auto b = w.word.text.find_first_not_of(' ');
            if (b == std::string::npos)
                continue;
            w.word.text.erase(0, b);
            const TimeUs natural = std::max(w.lastToken, w.word.start) + 250'000;
            if (i + 1 < words.size())
                w.word.end = std::min(words[i + 1].word.start, std::max(natural, w.word.start + 100'000));
            else
                w.word.end = std::min(natural, std::max(segmentEnd, w.word.start + 100'000));
            w.word.end = std::max(w.word.end, w.word.start);
            w.word.probability = float(std::round(w.probSum / w.tokens * 1000.0) / 1000.0);
            out.words.push_back(std::move(w.word));
        }
    }
    std::stable_sort(out.words.begin(), out.words.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    snapToSpeech(out.words, options.speech);
    return true;
}

bool detectSpeech(const std::string& modelPath, const std::vector<float>& pcm, const VadOptions& options,
                  std::vector<SpeechSpan>& out, std::string* error)
{
    initialize();
    out.clear();
    whisper_vad_context_params cparams = whisper_vad_default_context_params();
    cparams.n_threads = threadCount(options.threads);
    cparams.use_gpu = false; // el modelo es diminuto
    whisper_vad_context* vctx = whisper_vad_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!vctx) {
        if (error)
            *error = "No se pudo cargar el modelo de detección de voz: " + modelPath;
        return false;
    }
    whisper_vad_params params = whisper_vad_default_params();
    params.threshold = options.threshold;
    params.min_speech_duration_ms = options.minSpeechMs;
    params.min_silence_duration_ms = options.minSilenceMs;
    params.speech_pad_ms = options.padMs;
    whisper_vad_segments* segments = whisper_vad_segments_from_samples(vctx, params, pcm.data(), int(pcm.size()));
    if (!segments) {
        whisper_vad_free(vctx);
        if (error)
            *error = "La detección de voz falló.";
        return false;
    }
    const int n = whisper_vad_segments_n_segments(segments);
    for (int i = 0; i < n; ++i) {
        SpeechSpan span{centisecondsToUs(std::llround(whisper_vad_segments_get_segment_t0(segments, i))),
                        centisecondsToUs(std::llround(whisper_vad_segments_get_segment_t1(segments, i)))};
        if (!out.empty() && span.start <= out.back().end)
            out.back().end = std::max(out.back().end, span.end);
        else if (span.end > span.start)
            out.push_back(span);
    }
    whisper_vad_free_segments(segments);
    whisper_vad_free(vctx);
    return true;
}

} // namespace movix::ai
