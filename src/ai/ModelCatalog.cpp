#include "ai/ModelCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace movix::ai {

namespace {

// Archivos de Hugging Face fijados a una revisión concreta: el contenido (y su hash) no cambia.
QString hf(const char* repo, const char* revision, const QString& path)
{
    return QStringLiteral("https://huggingface.co/%1/resolve/%2/%3").arg(QLatin1String(repo), QLatin1String(revision), path);
}

ModelFile file(const char* name, const QString& url, const char* sha256, std::int64_t size)
{
    return ModelFile{QLatin1String(name), url, QLatin1String(sha256), size};
}

ModelInfo whisper(const char* id, const char* name, const char* description, const char* fileName, const char* sha256, std::int64_t size)
{
    ModelInfo m;
    m.id = QLatin1String(id);
    m.task = ModelTask::Transcription;
    m.name = QLatin1String(name);
    m.description = QCoreApplication::translate("ModelCatalog", description);
    m.license = QStringLiteral("MIT");
    m.files = {file(fileName, hf("ggerganov/whisper.cpp", "main", QLatin1String(fileName)), sha256, size)};
    return m;
}

constexpr const char* kKokoroOnnx = "onnx-community/Kokoro-82M-v1.0-ONNX";
constexpr const char* kKokoroOnnxRev = "1939ad2a8e416c0acfeecc08a694d14ef25f2231";

constexpr const char* kMisakiData = "https://raw.githubusercontent.com/hexgrad/misaki/fba1236595f2d2bf21d414ba6e57d25256afada3/misaki/data/";

ModelFile kokoroVoice(const char* voice, const char* sha256)
{
    const QString name = QStringLiteral("%1.bin").arg(QLatin1String(voice));
    return ModelFile{name, hf(kKokoroOnnx, kKokoroOnnxRev, QStringLiteral("voices/") + name), QLatin1String(sha256), 522240};
}

// Los tokenizadores de SentencePiece de los dos sentidos son los mismos archivos intercambiados.
constexpr const char* kSpmSpanish = "e236ee6d866b635c0142114f8647f39831f9d92534aa2aad75c942f6a78ad0e3"; // 825924 bytes
constexpr const char* kSpmEnglish = "4dd547c24816a335e7b0b2e63376a8f1b3cbfc671eda5ab808dd44fdadaa8791"; // 801636 bytes

ModelInfo opusMt(const char* id, const char* name, const char* description, const char* repo, const char* revision,
                 const char* encoderSha, const char* decoderSha, bool fromSpanish)
{
    ModelInfo m;
    m.id = QLatin1String(id);
    m.task = ModelTask::Translation;
    m.name = QLatin1String(name);
    m.description = QCoreApplication::translate("ModelCatalog", description);
    m.license = QStringLiteral("Apache-2.0");
    m.folder = m.id;
    const ModelFile spanish = file("", {}, kSpmSpanish, 825924), english = file("", {}, kSpmEnglish, 801636);
    ModelFile source = fromSpanish ? spanish : english, target = fromSpanish ? english : spanish;
    source.name = QStringLiteral("source.spm");
    source.url = hf(repo, revision, source.name);
    target.name = QStringLiteral("target.spm");
    target.url = hf(repo, revision, target.name);
    m.files = {
        file("encoder.onnx", hf(repo, revision, QStringLiteral("onnx/encoder_model_q4f16.onnx")), encoderSha, 77900833),
        file("decoder.onnx", hf(repo, revision, QStringLiteral("onnx/decoder_model_q4f16.onnx")), decoderSha, 81759639),
        source,
        target,
        file("vocab.json", hf(repo, revision, QStringLiteral("vocab.json")),
             "b074b4cca0036ade5a39ea97faabd534e1015482c480fc2cb02c6481983eb163", 1720044),
    };
    return m;
}

} // namespace

std::int64_t ModelInfo::totalSize() const
{
    std::int64_t total = 0;
    for (const ModelFile& f : files)
        total += f.size;
    return total;
}

const std::vector<ModelInfo>& modelCatalog()
{
    // Se construye al primer uso (las descripciones necesitan el traductor ya instalado).
    static const std::vector<ModelInfo> catalog = [] {
        std::vector<ModelInfo> list;
        list.push_back(whisper("whisper-tiny", "Whisper Tiny",
                               QT_TRANSLATE_NOOP("ModelCatalog", "El más rápido; precisión básica."), "ggml-tiny.bin",
                               "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21", 77691713));
        list.push_back(whisper("whisper-base", "Whisper Base",
                               QT_TRANSLATE_NOOP("ModelCatalog", "Rápido y ligero; buena precisión con voz clara."),
                               "ggml-base.bin", "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe", 147951465));
        list.push_back(whisper("whisper-small", "Whisper Small",
                               QT_TRANSLATE_NOOP("ModelCatalog", "Equilibrio entre velocidad y precisión."), "ggml-small.bin",
                               "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b", 487601967));
        list.push_back(whisper("whisper-large-v3-turbo-q5", "Whisper Large v3 Turbo (Q5)",
                               QT_TRANSLATE_NOOP("ModelCatalog", "La mejor precisión; más lento en CPU."),
                               "ggml-large-v3-turbo-q5_0.bin",
                               "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2", 574041195));

        ModelInfo vad;
        vad.id = QStringLiteral("silero-vad");
        vad.task = ModelTask::VoiceActivity;
        vad.name = QStringLiteral("Silero VAD v6.2");
        vad.description = QCoreApplication::translate("ModelCatalog", "Detección de voz (eliminar silencios).");
        vad.license = QStringLiteral("MIT");
        vad.files = {file("ggml-silero-v6.2.0.bin", hf("ggml-org/whisper-vad", "main", QStringLiteral("ggml-silero-v6.2.0.bin")),
                          "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987", 885098)};
        list.push_back(vad);

        ModelInfo rnn;
        rnn.id = QStringLiteral("rnnoise-speech");
        rnn.task = ModelTask::Denoise;
        rnn.name = QStringLiteral("RNNoise (voz)");
        rnn.description = QCoreApplication::translate("ModelCatalog", "Reducción de ruido de fondo en la voz.");
        rnn.license = QCoreApplication::translate("ModelCatalog", "sin derechos de autor");
        rnn.files = {file("rnnoise-somnolent-hogwash.rnnn",
                          QStringLiteral("https://raw.githubusercontent.com/GregorR/rnnoise-models/master/somnolent-hogwash-2018-09-01/sh.rnnn"),
                          "70bb6685eb0c2a1d18e2918dca3fbfbd39317010b1802eb1b6ea73a92f3fdec0", 297646)};
        list.push_back(rnn);

        ModelInfo kokoro;
        kokoro.id = QStringLiteral("kokoro-82m");
        kokoro.task = ModelTask::Speech;
        kokoro.name = QStringLiteral("Kokoro 82M");
        kokoro.description = QCoreApplication::translate("ModelCatalog", "Texto a voz natural en español e inglés (5 voces).");
        kokoro.license = QStringLiteral("Apache-2.0");
        kokoro.folder = kokoro.id;
        kokoro.files = {
            file("model.onnx", hf(kKokoroOnnx, kKokoroOnnxRev, QStringLiteral("onnx/model_quantized.onnx")),
                 "fbae9257e1e05ffc727e951ef9b9c98418e6d79f1c9b6b13bd59f5c9028a1478", 92361116),
            file("config.json", hf("hexgrad/Kokoro-82M", "f3ff3571791e39611d31c381e3a41a3af07b4987", QStringLiteral("config.json")),
                 "5abb01e2403b072bf03d04fde160443e209d7a0dad49a423be15196b9b43c17f", 2351),
            kokoroVoice("ef_dora", "f66ec66bd295acb18372e37008533a9a3228483ccd294e7538d5d9294ac9a532"),
            kokoroVoice("em_alex", "27809e9eafdcbcfff90a3016c697568676531de2a2c39cee29c96c7bd6b83e95"),
            kokoroVoice("em_santa", "ad43b774e1ca24d05c6161297d8aeb770ac3d29bb95daf516727af5f7d543683"),
            kokoroVoice("af_heart", "d583ccff3cdca2f7fae535cb998ac07e9fcb90f09737b9a41fa2734ec44a8f0b"),
            kokoroVoice("am_michael", "1d1f21dd8da39c30705cd4c75d039d265e9bc4a2a93ed09bc9e1b1225eb95ba1"),
            // Léxico inglés de misaki, el fonetizador con el que se entrenó Kokoro (Apache-2.0).
            file("us_gold.json", QLatin1String(kMisakiData) + QStringLiteral("us_gold.json"),
                 "dc414872a49a28ae6c141463d502fd945f3b2fde040484fdc47d00cc4612686f", 3000469),
            file("us_silver.json", QLatin1String(kMisakiData) + QStringLiteral("us_silver.json"),
                 "de8f67be911bb6c659187b4a65fd966b6a30e56350e0f790d763210b053ac475", 3099517),
        };
        list.push_back(kokoro);

        list.push_back(opusMt("opus-mt-es-en", "Opus-MT español → inglés",
                              QT_TRANSLATE_NOOP("ModelCatalog", "Traducción de español a inglés."), "Xenova/opus-mt-es-en",
                              "eadfd7c658a9d8929ac3b8e996b68a68e2c7d480",
                              "e0121c9448bbb91dce4e34c8e8f5eb64bcd14e4478bc67ead3c079a92ff2916a",
                              "9f581b5beb212f59027d427bb3c634a270349ec3985a9e52e9735e3ad1c30775", true));
        list.push_back(opusMt("opus-mt-en-es", "Opus-MT inglés → español",
                              QT_TRANSLATE_NOOP("ModelCatalog", "Traducción de inglés a español."), "Xenova/opus-mt-en-es",
                              "4b002a4c7edd54a7ced58877258b87f7efd3f892",
                              "2d41f51f690a3dc9d1b4ae36b570dc801256cf4249290042bc375a75b0d424ca",
                              "b3cc431f1aa3265f80570e8c29d9dd1aa508490d5d9eb8100e0d54d81adedcfc", false));

        ModelInfo demucs;
        demucs.id = QStringLiteral("htdemucs");
        demucs.task = ModelTask::Separation;
        demucs.name = QStringLiteral("HTDemucs v4");
        demucs.description = QCoreApplication::translate("ModelCatalog", "Separa la voz de la música (batería, bajo y resto).");
        demucs.license = QStringLiteral("MIT");
        demucs.folder = demucs.id;
        demucs.files = {file("htdemucs.onnx",
                             hf("smank/htdemucs-onnx", "fb1fd25aa6260c253d86bbb4aa26b73aebc79f10", QStringLiteral("htdemucs.onnx")),
                             "d2b401f322558cd57d67a752ed7be3fa55178a0626011eda8ac7bb74e17280c0", 304321552)};
        list.push_back(demucs);

        auto single = [&](const char* id, ModelTask task, const char* name, const char* description, const char* license,
                          const char* repo, const char* revision, const char* path, const char* sha256, std::int64_t size) {
            ModelInfo m;
            m.id = QLatin1String(id);
            m.task = task;
            m.name = QLatin1String(name);
            m.description = QCoreApplication::translate("ModelCatalog", description);
            m.license = QLatin1String(license);
            m.folder = m.id;
            m.files = {file("model.onnx", hf(repo, revision, QLatin1String(path)), sha256, size)};
            list.push_back(m);
        };
        single("modnet", ModelTask::Matting, "MODNet", QT_TRANSLATE_NOOP("ModelCatalog", "Recorta personas del fondo; rápido."),
               "Apache-2.0", "Xenova/modnet", "fa2fa546052fba4c08921230a26cc69a333fca12", "onnx/model.onnx",
               "07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9", 25888640);
        single("birefnet-lite", ModelTask::Matting, "BiRefNet lite",
               QT_TRANSLATE_NOOP("ModelCatalog", "Recorta cualquier sujeto del fondo; más preciso y más lento."), "MIT",
               "onnx-community/BiRefNet_lite-ONNX", "de15b22ba131738a16dff04aab8bdf8dc32e3ac1", "onnx/model_fp16.onnx",
               "d39b897ceb16ae654c1731f3dba0cf9b368d9cae74b5a57459b455cc8bfec402", 114538221);
        single("realesrgan-x4v3", ModelTask::Upscaling, "Real-ESRGAN general x4v3",
               QT_TRANSLATE_NOOP("ModelCatalog", "Aumenta la resolución del video (hasta ×4) recuperando detalle."), "BSD-3-Clause",
               "skillsafe-ai/realesr-general-x4v3", "042a40bc4c918349ad3e2e607a68ae509a4c27b5", "model.onnx",
               "a946f7a9397021b9b6b7e71df3d2821b04cc09ff244423b7ca79cb191ce4a00e", 4866428);
        single("rife-4", ModelTask::Interpolation, "RIFE 4",
               QT_TRANSLATE_NOOP("ModelCatalog", "Crea fotogramas intermedios para cámara lenta suave."), "MIT",
               "walterlow/RIFE_fp32_timestep", "ee09066f9822f8b28b8477a1b4cc30f19d607590", "RIFE_fp32_timestep.onnx",
               "4da60c1f20d42dba4f21503140940aa48a488585ead977532bf22e95a0319327", 21604567);

        ModelInfo sam;
        sam.id = QStringLiteral("sam2-tiny");
        sam.task = ModelTask::Segmentation;
        sam.name = QStringLiteral("SAM 2.1 tiny");
        sam.description = QCoreApplication::translate("ModelCatalog", "Selecciona y sigue un objeto con un clic.");
        sam.license = QStringLiteral("Apache-2.0");
        sam.folder = sam.id;
        constexpr const char* kSam = "onnx-community/sam2.1-hiera-tiny-ONNX";
        constexpr const char* kSamRev = "814a066640debee5a91e70aa401fb8e17e030503";
        // Los .onnx_data (pesos externos) deben conservar su nombre junto al .onnx.
        sam.files = {
            file("vision_encoder.onnx", hf(kSam, kSamRev, QStringLiteral("onnx/vision_encoder.onnx")),
                 "4f30aacd3aaefbca81a0b7fe4c1fc96345570ea0a6f80ced599493d1b3be2e8c", 354238),
            file("vision_encoder.onnx_data", hf(kSam, kSamRev, QStringLiteral("onnx/vision_encoder.onnx_data")),
                 "e83df9866a5afe68ea7f0f721f18f65137fc3acbf0da1c74e946d363e09c69cc", 134084864),
            file("prompt_encoder_mask_decoder.onnx", hf(kSam, kSamRev, QStringLiteral("onnx/prompt_encoder_mask_decoder.onnx")),
                 "874414704c5d686db7d206a35f6e15d26563d50c8c4468fccc6739bd7e491dcf", 213114),
            file("prompt_encoder_mask_decoder.onnx_data", hf(kSam, kSamRev, QStringLiteral("onnx/prompt_encoder_mask_decoder.onnx_data")),
                 "e9874d900dd4134ed60eab1e97910327c2419e0b2954485d8fd6e7f1a1470f47", 20958208),
        };
        list.push_back(sam);
        return list;
    }();
    return catalog;
}

const ModelInfo* findModel(const QString& id)
{
    for (const ModelInfo& m : modelCatalog())
        if (m.id == id)
            return &m;
    return nullptr;
}

QString defaultTranscriptionModel()
{
    return QStringLiteral("whisper-base");
}

QString defaultVadModel()
{
    return QStringLiteral("silero-vad");
}

QString defaultDenoiseModel()
{
    return QStringLiteral("rnnoise-speech");
}

QString speechModel()
{
    return QStringLiteral("kokoro-82m");
}

QString separationModel()
{
    return QStringLiteral("htdemucs");
}

QString portraitMatteModel()
{
    return QStringLiteral("modnet");
}

QString generalMatteModel()
{
    return QStringLiteral("birefnet-lite");
}

QString upscaleModel()
{
    return QStringLiteral("realesrgan-x4v3");
}

QString interpolationModel()
{
    return QStringLiteral("rife-4");
}

QString segmentationModel()
{
    return QStringLiteral("sam2-tiny");
}

QString translationModel(const QString& from, const QString& to)
{
    const QString id = QStringLiteral("opus-mt-%1-%2").arg(from, to);
    return findModel(id) ? id : QString();
}

QString modelsDirectory()
{
    QString dir = qEnvironmentVariable("MOVIX_MODELS_DIR");
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/MovixStudio/models");
    QDir().mkpath(dir);
    return QDir::cleanPath(dir);
}

QString modelFolder(const ModelInfo& model)
{
    return model.folder.isEmpty() ? modelsDirectory() : modelsDirectory() + QLatin1Char('/') + model.folder;
}

QString modelFilePath(const ModelInfo& model, const QString& fileName)
{
    return modelFolder(model) + QLatin1Char('/') + fileName;
}

QString modelPath(const ModelInfo& model)
{
    return model.files.empty() ? QString() : modelFilePath(model, model.files.front().name);
}

bool isModelInstalled(const ModelInfo& model)
{
    for (const ModelFile& f : model.files) {
        const QFileInfo info(modelFilePath(model, f.name));
        if (!info.isFile() || info.size() != f.size)
            return false;
    }
    return !model.files.empty();
}

} // namespace movix::ai
