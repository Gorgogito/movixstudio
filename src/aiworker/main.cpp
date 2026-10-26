// movix-ai-worker: ejecuta los trabajos de IA fuera del proceso de la aplicación (un fallo o falta
// de memoria aquí no cierra el editor). Ver ai/WorkerProtocol.h.

#include "ai/Beats.h"
#include "ai/Onnx.h"
#include "ai/Scenes.h"
#include "ai/Separation.h"
#include "ai/Translate.h"
#include "ai/Tts.h"
#include "ai/Tracking.h"
#include "ai/VideoAi.h"
#include "ai/Vision.h"
#include "ai/Speech.h"
#include "ai/WorkerProtocol.h"
#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/log.h>
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace movix;

namespace {

std::mutex g_outMutex;
std::string g_exeDir;

// Carpeta que contiene espeak-ng-data (junto al ejecutable).
std::string espeakDataParent()
{
    return g_exeDir;
}

void send(const QJsonObject& event)
{
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard lock(g_outMutex);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

QJsonObject event(qint64 id, const char* name)
{
    return QJsonObject{{"id", id}, {"event", QLatin1String(name)}};
}

struct Queue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<QJsonObject> jobs;
    qint64 current = -1;
    std::atomic<qint64> cancelId{-1};
    bool closing = false;
};

void runJob(Queue& queue, const QJsonObject& job)
{
    const qint64 id = job["id"].toInteger();
    const QString type = job["type"].toString();
    const std::string mediaPath = job["media"].toString().toStdString();
    const std::string modelPath = job["model"].toString().toStdString();
    auto canceled = [&] { return queue.cancelId.load() == id; };
    auto fail = [&](const std::string& message) {
        QJsonObject e = event(id, "error");
        e["message"] = QString::fromStdString(message);
        e["canceled"] = canceled();
        send(e);
    };
    // Cada fase ocupa un tramo del progreso total; los eventos se limitan a un cambio de 0,5 %.
    double lastSent = -1.0;
    auto progressIn = [&](double from, double to, const char* stage) {
        return [&, from, to, stage](double f) {
            const double value = from + (to - from) * std::clamp(f, 0.0, 1.0);
            if (value - lastSent < 0.005 && value < 1.0)
                return;
            lastSent = value;
            QJsonObject e = event(id, "progress");
            e["value"] = value;
            e["stage"] = QLatin1String(stage);
            send(e);
        };
    };

    send(event(id, "started"));
    std::vector<float> pcm;
    std::string error;
    if (type == QLatin1String(ai::protocol::kTranscribe)) {
        if (!ai::loadSpeechAudio(mediaPath, pcm, &error, progressIn(0.0, 0.1, "decode"), canceled))
            return fail(error);
        ai::TranscribeOptions options;
        options.language = job["language"].toString(QStringLiteral("auto")).toStdString();
        options.translate = job["translate"].toBool();
        // Con el modelo de detección de voz, los bordes de las palabras se ajustan a la voz real.
        const std::string vadModel = job["vadModel"].toString().toStdString();
        if (!vadModel.empty()) {
            ai::VadOptions vad;
            vad.minSilenceMs = 200;
            vad.padMs = 60;
            if (!ai::detectSpeech(vadModel, pcm, vad, options.speech, &error))
                return fail(error);
        }
        core::Transcript transcript;
        if (!ai::transcribe(modelPath, pcm, options, transcript, &error, progressIn(0.1, 1.0, "transcribe"), canceled))
            return fail(error);
        transcript.model = job["modelName"].toString().toStdString();
        QJsonObject e = event(id, "result");
        e["transcript"] = core::transcriptToJson(transcript);
        send(e);
    } else if (type == QLatin1String(ai::protocol::kDetectSpeech)) {
        if (!ai::loadSpeechAudio(mediaPath, pcm, &error, progressIn(0.0, 0.5, "decode"), canceled))
            return fail(error);
        ai::VadOptions options;
        options.threshold = float(job["threshold"].toDouble(options.threshold));
        options.minSpeechMs = job["minSpeechMs"].toInt(options.minSpeechMs);
        options.minSilenceMs = job["minSilenceMs"].toInt(options.minSilenceMs);
        options.padMs = job["padMs"].toInt(options.padMs);
        std::vector<ai::SpeechSpan> spans;
        progressIn(0.5, 1.0, "detect")(0.0);
        if (!ai::detectSpeech(modelPath, pcm, options, spans, &error))
            return fail(error);
        if (canceled())
            return fail("cancelado");
        QJsonArray speech;
        for (const ai::SpeechSpan& s : spans)
            speech.append(QJsonArray{qint64(s.start), qint64(s.end)});
        QJsonObject e = event(id, "result");
        e["speech"] = speech;
        e["duration"] = qint64(double(pcm.size()) * kUsPerSecond / ai::kSpeechSampleRate);
        send(e);
    } else if (type == QLatin1String(ai::protocol::kDetectScenes)) {
        ai::SceneOptions options;
        options.threshold = float(job["threshold"].toDouble(options.threshold));
        options.minScene = job["minSceneUs"].toInteger(options.minScene);
        std::vector<TimeUs> cuts;
        if (!ai::detectScenes(mediaPath, job["from"].toInteger(), job["to"].toInteger(), options, cuts, &error,
                              progressIn(0.0, 1.0, "detect"), canceled))
            return fail(error);
        QJsonArray list;
        for (TimeUs c : cuts)
            list.append(qint64(c));
        QJsonObject e = event(id, "result");
        e["cuts"] = list;
        send(e);
    } else if (type == QLatin1String(ai::protocol::kDetectBeats)) {
        ai::BeatResult beats;
        if (!ai::detectBeats(mediaPath, job["from"].toInteger(), job["to"].toInteger(), beats, &error,
                             progressIn(0.0, 1.0, "detect"), canceled))
            return fail(error);
        QJsonArray list;
        for (TimeUs b : beats.beats)
            list.append(qint64(b));
        QJsonObject e = event(id, "result");
        e["bpm"] = beats.bpm;
        e["beats"] = list;
        send(e);
    } else if (type == QLatin1String(ai::protocol::kSynthesize)) {
        ai::TtsOptions options;
        options.voice = job["voice"].toString(QStringLiteral("ef_dora")).toStdString();
        options.speed = job["speed"].toDouble(1.0);
        std::vector<float> audio;
        if (!ai::synthesize(modelPath, espeakDataParent(), job["text"].toString().toStdString(), options, audio, &error,
                            progressIn(0.0, 0.95, "synthesize"), canceled))
            return fail(error);
        const std::string output = job["output"].toString().toStdString();
        if (!ai::writeWav(output, audio, ai::kTtsSampleRate, 1, &error))
            return fail(error);
        QJsonObject e = event(id, "result");
        e["output"] = QString::fromStdString(output);
        e["duration"] = qint64(double(audio.size()) * kUsPerSecond / ai::kTtsSampleRate);
        send(e);
    } else if (type == QLatin1String(ai::protocol::kTranslate)) {
        std::vector<std::string> texts, translated;
        for (const QJsonValue& v : job["texts"].toArray())
            texts.push_back(v.toString().toStdString());
        if (!ai::translate(modelPath, texts, translated, &error, progressIn(0.0, 1.0, "translate"), canceled))
            return fail(error);
        QJsonArray list;
        for (const std::string& t : translated)
            list.append(QString::fromStdString(t));
        QJsonObject e = event(id, "result");
        e["texts"] = list;
        send(e);
    } else if (type == QLatin1String(ai::protocol::kSeparate)) {
        ai::Stems stems;
        if (!ai::separateFile(modelPath, mediaPath, job["from"].toInteger(), job["to"].toInteger(), stems, &error,
                              progressIn(0.0, 0.95, "separate"), canceled))
            return fail(error);
        const std::string vocals = job["vocals"].toString().toStdString();
        const std::string accompaniment = job["accompaniment"].toString().toStdString();
        if (!ai::writeWav(vocals, stems.vocals, ai::kSeparationSampleRate, 2, &error)
            || !ai::writeWav(accompaniment, stems.accompaniment, ai::kSeparationSampleRate, 2, &error))
            return fail(error);
        QJsonObject e = event(id, "result");
        e["vocals"] = QString::fromStdString(vocals);
        e["accompaniment"] = QString::fromStdString(accompaniment);
        e["duration"] = qint64(double(stems.vocals.size() / 2) * kUsPerSecond / ai::kSeparationSampleRate);
        send(e);
    } else if (type == QLatin1String(ai::protocol::kRemoveBackground) || type == QLatin1String(ai::protocol::kSegmentObject)) {
        ai::MatteResult matte;
        const bool ok = type == QLatin1String(ai::protocol::kSegmentObject)
            ? ai::segmentObject(modelPath, mediaPath, job["from"].toInteger(), job["to"].toInteger(),
                                job["at"].toInteger(job["from"].toInteger()), job["x"].toDouble(0.5),
                                job["y"].toDouble(0.5), job["output"].toString().toStdString(), matte, &error,
                                progressIn(0.0, 1.0, "matte"), canceled)
            : ai::removeBackground(modelPath,
                                   job["kind"].toString() == QLatin1String("general") ? ai::MatteModel::General : ai::MatteModel::Portrait,
                                   mediaPath, job["from"].toInteger(), job["to"].toInteger(), job["output"].toString().toStdString(),
                                   matte, &error, progressIn(0.0, 1.0, "matte"), canceled);
        if (!ok)
            return fail(error);
        QJsonObject e = event(id, "result");
        e["output"] = QString::fromStdString(matte.output);
        e["start"] = qint64(matte.start);
        e["frames"] = matte.frames;
        send(e);
    } else if (type == QLatin1String(ai::protocol::kUpscale) || type == QLatin1String(ai::protocol::kInterpolate)) {
        ai::VideoResult video;
        const std::string output = job["output"].toString().toStdString();
        const bool ok = type == QLatin1String(ai::protocol::kUpscale)
            ? ai::upscaleVideo(modelPath, mediaPath, job["from"].toInteger(), job["to"].toInteger(), job["scale"].toInt(2), output,
                               video, &error, progressIn(0.0, 1.0, "video"), canceled)
            : ai::interpolateVideo(modelPath, mediaPath, job["from"].toInteger(), job["to"].toInteger(), job["factor"].toInt(2),
                                   output, video, &error, progressIn(0.0, 1.0, "video"), canceled);
        if (!ok)
            return fail(error);
        QJsonObject e = event(id, "result");
        e["output"] = QString::fromStdString(video.output);
        e["width"] = video.width;
        e["height"] = video.height;
        e["fps"] = video.fps;
        e["frames"] = video.frames;
        send(e);
    } else if (type == QLatin1String(ai::protocol::kTrack) || type == QLatin1String(ai::protocol::kSubjectCenters)) {
        std::vector<ai::TrackPoint> points;
        bool ok = false;
        if (type == QLatin1String(ai::protocol::kTrack)) {
            const QJsonArray r = job["region"].toArray();
            const ai::Region region{r.at(0).toDouble(0.5), r.at(1).toDouble(0.5), r.at(2).toDouble(0.2), r.at(3).toDouble(0.2),
                                    r.at(4).toDouble(0.0)};
            ok = ai::trackRegion(mediaPath, job["from"].toInteger(), job["to"].toInteger(), region, job["scaleRotation"].toBool(),
                                 points, &error, progressIn(0.0, 1.0, "track"), canceled);
        } else {
            ok = ai::subjectCenters(mediaPath, job["from"].toInteger(), job["to"].toInteger(), job["step"].toInteger(250'000),
                                    modelPath, points, &error, progressIn(0.0, 1.0, "detect"), canceled);
        }
        if (!ok)
            return fail(error);
        QJsonArray list;
        for (const ai::TrackPoint& t : points)
            list.append(QJsonArray{qint64(t.time), t.cx, t.cy, double(t.confidence), t.scale, t.angle});
        QJsonObject e = event(id, "result");
        e["points"] = list;
        send(e);
    } else {
        fail("Trabajo desconocido: " + type.toStdString());
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv); // rutas de la aplicación (sin bucle de eventos)
    av_log_set_level(AV_LOG_ERROR);   // sin los mensajes informativos de los codificadores
    g_exeDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdString();
    if (argc > 1 && std::string(argv[1]) == "--info") {
        std::printf("movix-ai-worker\n%s\nONNX Runtime GPU: %s\n", ai::systemInfo().c_str(),
                    ai::onnx::gpuAvailable() ? ai::onnx::gpuDescription().c_str() : "no disponible");
        return 0;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    Queue queue;
    std::thread worker([&] {
        for (;;) {
            QJsonObject job;
            {
                std::unique_lock lock(queue.mutex);
                queue.wake.wait(lock, [&] { return queue.closing || !queue.jobs.empty(); });
                if (queue.closing)
                    return;
                job = queue.jobs.front();
                queue.jobs.pop_front();
                queue.current = job["id"].toInteger();
            }
            runJob(queue, job);
            std::lock_guard lock(queue.mutex);
            queue.current = -1;
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if (request.isEmpty())
            continue;
        const qint64 id = request["id"].toInteger();
        if (request["type"].toString() == QLatin1String(movix::ai::protocol::kCancel)) {
            std::lock_guard lock(queue.mutex);
            const auto it = std::find_if(queue.jobs.begin(), queue.jobs.end(),
                                         [&](const QJsonObject& j) { return j["id"].toInteger() == id; });
            if (it != queue.jobs.end()) {
                queue.jobs.erase(it);
                QJsonObject e = event(id, "error");
                e["message"] = QStringLiteral("cancelado");
                e["canceled"] = true;
                send(e);
            } else if (queue.current == id) {
                queue.cancelId = id;
            }
            continue;
        }
        std::lock_guard lock(queue.mutex);
        queue.jobs.push_back(request);
        queue.wake.notify_one();
    }

    // stdin cerrado: la aplicación terminó o nos pide salir. Se abandona el trabajo en curso.
    {
        std::lock_guard lock(queue.mutex);
        queue.closing = true;
        queue.cancelId = queue.current;
        queue.wake.notify_one();
    }
    worker.join();
    return 0;
}

