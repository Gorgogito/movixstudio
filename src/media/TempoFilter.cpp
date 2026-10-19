#include "media/TempoFilter.h"

#include <cstring>
#include <string>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace movix::media {

struct TempoFilter::Impl {
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

TempoFilter::TempoFilter() : d(std::make_unique<Impl>()) {}

TempoFilter::~TempoFilter() = default;

bool TempoFilter::init(int sampleRate, int channels, double speed)
{
    m_rate = sampleRate;
    m_channels = channels;
    m_speed = speed;
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
    if (avfilter_graph_create_filter(&d->src, avfilter_get_by_name("abuffer"), "in", srcArgs.c_str(), nullptr, d->graph) < 0)
        return false;
    if (avfilter_graph_create_filter(&d->sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, d->graph) < 0)
        return false;

    // atempo admite 0,5..100: por debajo de 0,5 se encadenan varios.
    std::string chain;
    double remaining = speed;
    while (remaining < 0.5) {
        chain += "atempo=0.5,";
        remaining /= 0.5;
    }
    chain += "atempo=" + std::to_string(remaining) + ",aformat=sample_fmts=flt:sample_rates=" + std::to_string(sampleRate)
        + ":channel_layouts=" + layoutName;

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = d->src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = d->sink;
    const int ret = avfilter_graph_parse_ptr(d->graph, chain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret >= 0 && avfilter_graph_config(d->graph, nullptr) >= 0;
}

void TempoFilter::reset()
{
    init(m_rate, m_channels, m_speed);
}

bool TempoFilter::push(const float* samples, int frames)
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

int TempoFilter::pull(std::vector<float>& out)
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

} // namespace movix::media
