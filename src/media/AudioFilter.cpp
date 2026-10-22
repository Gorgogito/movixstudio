#include "media/AudioFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace movix::media {

struct AudioFilter::Impl {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    AVFrame* frame = av_frame_alloc();
    int64_t pts = 0;

    ~Impl()
    {
        avfilter_graph_free(&graph);
        av_frame_free(&frame);
    }
};

AudioFilter::AudioFilter() : d(std::make_unique<Impl>()) {}

AudioFilter::~AudioFilter() = default;

bool AudioFilter::isValid() const
{
    return d->graph != nullptr;
}

bool AudioFilter::init(int sampleRate, int channels, const std::string& chain)
{
    m_rate = sampleRate;
    m_channels = channels;
    m_chain = chain;
    avfilter_graph_free(&d->graph);
    d->pts = 0;
    d->graph = avfilter_graph_alloc();
    if (!d->graph)
        return false;

    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    char layoutName[64] = {};
    av_channel_layout_describe(&layout, layoutName, sizeof(layoutName));
    av_channel_layout_uninit(&layout);
    const std::string srcArgs = "time_base=1/" + std::to_string(sampleRate) + ":sample_rate=" + std::to_string(sampleRate)
        + ":sample_fmt=flt:channel_layout=" + layoutName;
    bool ok = avfilter_graph_create_filter(&d->src, avfilter_get_by_name("abuffer"), "in", srcArgs.c_str(), nullptr, d->graph) >= 0
        && avfilter_graph_create_filter(&d->sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, d->graph) >= 0;
    if (ok) {
        // La salida vuelve al formato de la entrada (los filtros pueden pedir otra frecuencia).
        const std::string full = chain + ",aformat=sample_fmts=flt:sample_rates=" + std::to_string(sampleRate)
            + ":channel_layouts=" + layoutName;
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = d->src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = d->sink;
        ok = avfilter_graph_parse_ptr(d->graph, full.c_str(), &inputs, &outputs, nullptr) >= 0
            && avfilter_graph_config(d->graph, nullptr) >= 0;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    }
    if (!ok)
        avfilter_graph_free(&d->graph);
    return ok;
}

bool AudioFilter::reset()
{
    return init(m_rate, m_channels, m_chain);
}

bool AudioFilter::push(const float* samples, int frames)
{
    if (!d->graph)
        return false;
    if (frames <= 0)
        return av_buffersrc_add_frame(d->src, nullptr) >= 0; // fin del flujo: vaciar el filtro
    AVFrame* f = d->frame;
    av_frame_unref(f);
    f->format = AV_SAMPLE_FMT_FLT;
    f->sample_rate = m_rate;
    f->nb_samples = frames;
    av_channel_layout_default(&f->ch_layout, m_channels);
    if (av_frame_get_buffer(f, 0) < 0)
        return false;
    std::memcpy(f->data[0], samples, size_t(frames) * size_t(m_channels) * sizeof(float));
    f->pts = d->pts;
    d->pts += frames;
    return av_buffersrc_add_frame(d->src, f) >= 0;
}

int AudioFilter::pull(std::vector<float>& out)
{
    if (!d->graph)
        return 0;
    int total = 0;
    while (av_buffersink_get_frame(d->sink, d->frame) >= 0) {
        const auto* data = reinterpret_cast<const float*>(d->frame->data[0]);
        out.insert(out.end(), data, data + size_t(d->frame->nb_samples) * size_t(m_channels));
        total += d->frame->nb_samples;
        av_frame_unref(d->frame);
    }
    return total;
}

// --- Reducción de ruido ---

namespace {
std::mutex g_modelMutex;
std::string g_model;

// Escapa un valor de opción para una descripción de filtros (dos niveles, ver "Notes on
// filtergraph escaping" en la documentación de FFmpeg): el de las opciones del filtro y el del grafo.
std::string escapeOption(const std::string& value)
{
    std::string level1;
    for (char c : value) {
        const char ch = c == '\\' ? '/' : c; // rutas de Windows con barras normales
        if (ch == '\'' || ch == ':')
            level1 += '\\';
        level1 += ch;
    }
    std::string level2;
    for (char c : level1) {
        if (c == '\\' || c == '\'' || c == '[' || c == ']' || c == ',' || c == ';')
            level2 += '\\';
        level2 += c;
    }
    return level2;
}
} // namespace

void setDenoiseModel(const std::string& path)
{
    std::lock_guard lock(g_modelMutex);
    g_model = path;
}

std::string denoiseModel()
{
    std::lock_guard lock(g_modelMutex);
    return g_model;
}

bool Denoiser::init(int sampleRate, int channels, const std::string& model, double amount)
{
    m_model = model;
    m_amount = std::clamp(amount, 0.0, 1.0);
    char mix[32];
    std::snprintf(mix, sizeof mix, "%.3f", m_amount);
    // Escapado en dos niveles: la opción dentro del filtro y el filtro dentro de la cadena.
    const std::string chain = "arnndn=m=" + escapeOption(model) + ":mix=" + mix;
    // RNNoise trabaja con ventanas de 480 muestras a 48 kHz y retrasa la salida una ventana (10 ms).
    m_delay = int(std::lround(sampleRate * 0.010));
    const bool ok = m_filter.init(sampleRate, channels, chain);
    reset();
    return ok;
}

void Denoiser::reset()
{
    if (m_filter.isValid())
        m_filter.reset();
    m_in = 0;
    m_out = 0;
    m_toDrop = m_delay;
}

void Denoiser::collect(std::vector<float>& out)
{
    m_tmp.clear();
    m_filter.pull(m_tmp);
    const int channels = m_filter.channels();
    int frames = int(m_tmp.size() / size_t(channels));
    int skip = std::min(m_toDrop, frames);
    m_toDrop -= skip;
    // Nunca más salida que entrada.
    const int keep = int(std::min<int64_t>(frames - skip, m_in - m_out));
    if (keep <= 0)
        return;
    out.insert(out.end(), m_tmp.begin() + std::ptrdiff_t(skip) * channels, m_tmp.begin() + std::ptrdiff_t(skip + keep) * channels);
    m_out += keep;
}

void Denoiser::process(const float* in, int frames, std::vector<float>& out)
{
    if (frames <= 0)
        return;
    m_in += frames;
    if (!m_filter.isValid()) {
        out.insert(out.end(), in, in + size_t(frames) * size_t(m_filter.channels()));
        m_out += frames;
        return;
    }
    m_filter.push(in, frames);
    collect(out);
}

void Denoiser::flush(std::vector<float>& out)
{
    if (m_filter.isValid()) {
        // Sin señal de fin de flujo: con ella, arnndn recibe el último bloque incompleto y aun así
        // lee 480 muestras (lee fuera del búfer: basura o, si cae en el borde de una página, un
        // cierre). Se empuja silencio en bloques de 10 ms hasta recuperar toda la salida retenida.
        const int block = std::max(1, m_filter.sampleRate() / 100);
        const std::vector<float> silence(size_t(block) * size_t(m_filter.channels()), 0.0f);
        for (int i = 0; i < 100 && m_out < m_in; ++i) { // como mucho 1 s de relleno
            m_filter.push(silence.data(), block);
            collect(out);
        }
    }
    // Si el filtro devolvió menos (relleno final), se completa con silencio.
    if (m_out < m_in) {
        out.insert(out.end(), size_t(m_in - m_out) * size_t(m_filter.channels()), 0.0f);
        m_out = m_in;
    }
}

} // namespace movix::media
